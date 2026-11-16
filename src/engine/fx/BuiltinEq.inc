// The EQ: eight bands (bell, shelves, low / high cut with 6..96 dB/oct slopes, notch, band pass), each of which can
// be static, Dynamic (the band's gain moves with the level in that band) or Spectral (the dynamics work frequency by
// frequency inside the band, so only the frequencies that poke out get turned down or up).
// Included by BuiltinPlugins.cpp inside its anonymous namespace: uses Base, col and the helpers there.

constexpr int eqBandCount = 8;
enum EqType { eqBell, eqLowShelf, eqHighShelf, eqLowCut, eqHighCut, eqNotch, eqBandPass };
enum EqDyn { eqDynOff, eqDynDynamic, eqDynSpectral };
const juce::StringArray eqTypeNames { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
const juce::StringArray eqSlopeNames { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" };
constexpr int eqSlopeDb[] { 6, 12, 18, 24, 36, 48, 72, 96 };
const juce::StringArray eqDynNames { "Off", "Dynamic", "Spectral" };
const juce::Colour eqBandColours[eqBandCount] { juce::Colour (0xfff0a030), juce::Colour (0xff46c38a), juce::Colour (0xff4c9bf5),
                                                juce::Colour (0xffe5484d), juce::Colour (0xffb57cf0), juce::Colour (0xff3cc8c8),
                                                juce::Colour (0xfff07cb4), juce::Colour (0xffc8c83c) };
constexpr float eqGainRange = 30.0f;

inline bool eqHasGain (int type) { return type == eqBell || type == eqLowShelf || type == eqHighShelf; }
inline bool eqIsCut (int type) { return type == eqLowCut || type == eqHighCut; }

juce::String eqId (int band, const char* what) { return "b" + juce::String (band + 1) + what; }

/** One band's settings. All floats (choices as their index) so a whole set compares with memcmp. */
struct EqBand
{
    float on = 0, type = 0, freq = 1000, gain = 0, q = 1, slope = 1, dyn = 0, range = -6, thr = -24, use = 0, atk = 10, rel = 120;   // use: the band exists (on: not bypassed)
    int typeI() const { return juce::roundToInt (type); }
    int dynI() const { return juce::roundToInt (dyn); }
    bool isUsed() const { return use > 0.5f; }
    bool isOn() const { return isUsed() && on > 0.5f; }
    bool dynamic() const { return isOn() && dynI() == eqDynDynamic && eqHasGain (typeI()); }
    bool spectral() const { return isOn() && dynI() == eqDynSpectral && ! eqIsCut (typeI()); }
};
struct EqAll
{
    std::array<EqBand, eqBandCount> b {};
    float attack = 10, release = 120, out = 0;
    bool operator== (const EqAll& o) const { return std::memcmp (this, &o, sizeof (*this)) == 0; }
    bool anySpectral() const
    {
        for (auto& x : b) if (x.spectral()) return true;
        return false;
    }
};

/** The filter sections of one band (a cut needs up to eight). */
struct EqBandFilter
{
    std::array<Biquad, 8> sec;
    int n = 0;
    void design (const EqBand& b, double sr, float gainDb) noexcept
    {
        const double f = juce::jlimit (10.0, sr * 0.49, (double) b.freq), q = juce::jlimit (0.1, 30.0, (double) b.q);
        n = 1;
        switch (b.typeI())
        {
            case eqBell:      sec[0].set (Biquad::peak, sr, f, q, gainDb); break;
            case eqLowShelf:  sec[0].set (Biquad::lowShelf, sr, f, juce::jlimit (0.25, 4.0, q), gainDb); break;
            case eqHighShelf: sec[0].set (Biquad::highShelf, sr, f, juce::jlimit (0.25, 4.0, q), gainDb); break;
            case eqNotch:     sec[0].set (Biquad::notch, sr, f, q); break;
            case eqBandPass:  sec[0].set (Biquad::bandPass, sr, f, q); break;
            case eqLowCut:
            case eqHighCut:
            {
                // Butterworth of order slope / 6: one first-order section if odd, then second-order sections; the Q
                // knob adds resonance to the sharpest section
                const bool hp = b.typeI() == eqLowCut;
                const int order = eqSlopeDb[juce::jlimit (0, 7, juce::roundToInt (b.slope))] / 6;
                n = 0;
                if (order % 2 == 1) sec[(size_t) n++].setFirstOrder (hp, sr, f);
                for (int k = 1; k <= order / 2; ++k)
                {
                    double qk = 1.0 / (2.0 * std::sin ((2 * k - 1) * juce::MathConstants<double>::pi / (2.0 * order)));
                    if (k == 1) qk *= q / 0.7071;
                    sec[(size_t) n++].set (hp ? Biquad::highPass : Biquad::lowPass, sr, f, qk);
                }
                break;
            }
            default: sec[0].setIdentity(); break;
        }
    }
    void process (float* d, int len) noexcept
    {
        for (int s = 0; s < n; ++s) sec[(size_t) s].process (d, len);
    }
    double magnitude (double freq, double sr) const noexcept
    {
        double m = 1.0;
        for (int s = 0; s < n; ++s) m *= sec[(size_t) s].magnitude (freq, sr);
        return m;
    }
};

/** How strongly a band's dynamics act at `f` (1 at its centre), for spectral mode. */
inline float eqSpectralWeight (const EqBand& b, double f)
{
    const double f0 = juce::jmax (10.0, (double) b.freq);
    switch (b.typeI())
    {
        case eqLowShelf:  return (float) (1.0 / (1.0 + std::pow (f / f0, 4.0)));
        case eqHighShelf: return (float) (1.0 / (1.0 + std::pow (f0 / juce::jmax (1.0, f), 4.0)));
        default:
        {
            const double x = juce::jmax (0.1, (double) b.q) * (f / f0 - f0 / juce::jmax (1.0, f));
            return (float) (1.0 / (1.0 + x * x));
        }
    }
}

/** dB change for a level `over` dB above the threshold: a 4:1-like move toward `range`, never past it. */
inline float eqDynamicDelta (float over, float range)
{
    if (over <= 0.0f) return 0.0f;
    const float d = juce::jmin (std::abs (range), over * 0.75f);
    return range < 0.0f ? -d : d;
}

/** Short-time Fourier processor for spectral dynamics (2048-point frames, 75 % overlap, latency = frame size). */
struct EqSpectral
{
    static constexpr int order = 11, size = 1 << order, hop = size / 4, bins = size / 2 + 1, displayPoints = 160;
    juce::dsp::FFT fft { order };
    std::vector<float> window = std::vector<float> ((size_t) size), frame[2], inRing[2], outRing[2];
    std::vector<float> env = std::vector<float> ((size_t) bins, -120.0f), gainDb = std::vector<float> ((size_t) bins, 0.0f);
    std::vector<float> weights = std::vector<float> ((size_t) (bins * eqBandCount), 0.0f);
    std::vector<float> binAtk = std::vector<float> ((size_t) bins, 0.5f), binRel = std::vector<float> ((size_t) bins, 0.1f);   // per-frame coefficients
    std::array<std::atomic<float>, displayPoints> display {};
    float norm = 1.0f;
    int pos = 0, hopCount = 0;

    EqSpectral()
    {
        float sum = 0.0f;
        for (int i = 0; i < size; ++i)
        {
            window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) size));
            sum += window[(size_t) i];
        }
        norm = 2.0f / sum;   // a full-scale sine reads 0 dB in its bin
        for (int c = 0; c < 2; ++c)
        {
            frame[c].assign ((size_t) size * 2, 0.0f);
            inRing[c].assign ((size_t) size, 0.0f);
            outRing[c].assign ((size_t) size, 0.0f);
        }
        for (auto& d : display) d.store (0.0f);
    }
    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (inRing[c].begin(), inRing[c].end(), 0.0f);
            std::fill (outRing[c].begin(), outRing[c].end(), 0.0f);
        }
        std::fill (env.begin(), env.end(), -120.0f);
        std::fill (gainDb.begin(), gainDb.end(), 0.0f);
        for (auto& d : display) d.store (0.0f);
        pos = hopCount = 0;
    }
    void setWeights (const EqAll& s, double sr) noexcept
    {
        for (int b = 0; b < eqBandCount; ++b)
            for (int k = 0; k < bins; ++k)
                weights[(size_t) (b * bins + k)] = s.b[(size_t) b].spectral() ? eqSpectralWeight (s.b[(size_t) b], (double) k * sr / size) : 0.0f;
        // each frequency follows the attack / release of the band that acts most there
        const float frameSec = (float) hop / (float) sr;
        for (int k = 0; k < bins; ++k)
        {
            int best = -1;
            float bw = 0.0f;
            for (int b = 0; b < eqBandCount; ++b)
                if (weights[(size_t) (b * bins + k)] > bw) { bw = weights[(size_t) (b * bins + k)]; best = b; }
            const float a = best >= 0 ? s.b[(size_t) best].atk : 10.0f, r = best >= 0 ? s.b[(size_t) best].rel : 120.0f;
            binAtk[(size_t) k] = 1.0f - std::exp (-frameSec / juce::jmax (0.0005f, a * 0.001f));
            binRel[(size_t) k] = 1.0f - std::exp (-frameSec / juce::jmax (0.005f, r * 0.001f));
        }
    }

    void process (float* L, float* R, int n, const EqAll& s, double sr) noexcept
    {
        float* io[2] = { L, R };
        const int chans = R != nullptr ? 2 : 1;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < chans; ++c)
            {
                const float x = io[c][i];
                io[c][i] = outRing[c][(size_t) pos];
                outRing[c][(size_t) pos] = 0.0f;
                inRing[c][(size_t) pos] = x;
            }
            if (++pos == size) pos = 0;
            if (++hopCount == hop) { hopCount = 0; processFrame (chans, s, sr); }
        }
    }

private:
    void processFrame (int chans, const EqAll& s, double sr) noexcept
    {
        for (int c = 0; c < chans; ++c)
        {
            auto& f = frame[c];
            for (int j = 0; j < size; ++j) f[(size_t) j] = inRing[c][(size_t) ((pos + j) % size)] * window[(size_t) j];
            std::fill (f.begin() + size, f.end(), 0.0f);
            fft.performRealOnlyForwardTransform (f.data(), true);
        }
        for (int k = 0; k < bins; ++k)
        {
            float mag2 = 0.0f;
            for (int c = 0; c < chans; ++c)
            {
                const float re = frame[c][(size_t) (2 * k)], im = frame[c][(size_t) (2 * k + 1)];
                mag2 = juce::jmax (mag2, re * re + im * im);
            }
            const float lvl = 10.0f * std::log10 (mag2 * norm * norm + 1.0e-12f);
            auto& e = env[(size_t) k];
            e += (lvl - e) * (lvl > e ? binAtk[(size_t) k] : binRel[(size_t) k]);
            float g = 0.0f;
            for (int b = 0; b < eqBandCount; ++b)
                if (const float w = weights[(size_t) (b * bins + k)]; w > 0.001f)
                    g += w * eqDynamicDelta (e - s.b[(size_t) b].thr, s.b[(size_t) b].range);
            g = juce::jlimit (-eqGainRange, eqGainRange, g);
            gainDb[(size_t) k] = g;
            const float lin = std::pow (10.0f, g / 20.0f);
            for (int c = 0; c < chans; ++c)
            {
                frame[c][(size_t) (2 * k)] *= lin;
                frame[c][(size_t) (2 * k + 1)] *= lin;
            }
        }
        const float scale = 0.5f;   // sqrt-Hann in and out at 75 % overlap sums to 2
        for (int c = 0; c < chans; ++c)
        {
            auto& f = frame[c];
            fft.performRealOnlyInverseTransform (f.data());
            for (int j = 0; j < size; ++j) outRing[c][(size_t) ((pos + j) % size)] += f[(size_t) j] * window[(size_t) j] * scale;
        }
        for (int d = 0; d < displayPoints; ++d)   // for the display: log-spaced 20 Hz .. 20 kHz
        {
            const double fq = 20.0 * std::pow (1000.0, (double) d / (displayPoints - 1));
            const int k = juce::jlimit (0, bins - 1, (int) std::round (fq / sr * size));
            display[(size_t) d].store (gainDb[(size_t) k], std::memory_order_relaxed);
        }
    }
};

class Eq final : public Base, private juce::AudioProcessorParameter::Listener, private juce::AsyncUpdater
{
public:
    explicit Eq (const Info& i) : Base (i, layout())
    {
        for (int b = 0; b < eqBandCount; ++b)
        {
            const char* what[] { "on", "type", "freq", "gain", "q", "slope", "dyn", "range", "thr", "use", "atk", "rel" };
            for (int k = 0; k < 12; ++k) bandParams[(size_t) b][(size_t) k] = state.getParameter (eqId (b, what[k]));
            bandParams[(size_t) b][6]->addListener (this);
            bandParams[(size_t) b][1]->addListener (this);
            bandParams[(size_t) b][0]->addListener (this);
            bandParams[(size_t) b][9]->addListener (this);
        }
        attackP = state.getParameter ("attack");
        releaseP = state.getParameter ("release");
        outP = state.getParameter ("out");
        for (auto& d : dynNow) d.store (0.0f);
        updateLatency();
    }
    ~Eq() override
    {
        cancelPendingUpdate();
        for (auto& bp : bandParams) for (int k : { 0, 1, 6, 9 }) bp[(size_t) k]->removeListener (this);
    }

    static Layout layout()
    {
        Layout l;
        const float defFreq[eqBandCount] { 60, 150, 400, 1000, 2500, 5000, 9000, 15000 };
        for (int b = 0; b < eqBandCount; ++b)
        {
            const auto name = "Band " + juce::String (b + 1) + " ";
            auto fp = [&] (const char* what, const juce::String& nm, float lo, float hi, float def, const juce::String& unit, float centre) {
                juce::NormalisableRange<float> r (lo, hi);
                if (centre > lo && centre < hi) r.setSkewForCentre (centre);
                auto toText = [unit] (float v, int) {
                    if (unit == "Hz") return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
                    if (unit == "dB") return juce::String (v, 1) + " dB";
                    return juce::String (v, 2);
                };
                l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { eqId (b, what), 1 }, name + nm, r, def,
                                                                    juce::AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction (toText)));
            };
            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { eqId (b, "on"), 1 }, name + "on", false));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { eqId (b, "type"), 1 }, name + "type", eqTypeNames, 0));
            fp ("freq", "freq", 10.0f, 30000.0f, defFreq[b], "Hz", 1000.0f);
            fp ("gain", "gain", -eqGainRange, eqGainRange, 0.0f, "dB", 0.0f);
            fp ("q", "Q", 0.1f, 30.0f, 1.0f, "", 1.5f);
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { eqId (b, "slope"), 1 }, name + "slope", eqSlopeNames, 1));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { eqId (b, "dyn"), 1 }, name + "dynamics", eqDynNames, 0));
            fp ("range", "dyn range", -eqGainRange, eqGainRange, -6.0f, "dB", 0.0f);
            fp ("thr", "threshold", -80.0f, 0.0f, -24.0f, "dB", -24.0f);
        }
        l.add (param ("attack", "Dyn attack", 0.5f, 200.0f, 10.0f, "ms", 15.0f));
        l.add (param ("release", "Dyn release", 5.0f, 2000.0f, 120.0f, "ms", 150.0f));
        l.add (param ("out", "Output", -18.0f, 18.0f, 0.0f, "dB"));
        // added later: at the end so automation (by parameter index) of the parameters above keeps working
        for (int b = 0; b < eqBandCount; ++b)
            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { eqId (b, "use"), 1 }, "Band " + juce::String (b + 1) + " in use", false));
        for (int b = 0; b < eqBandCount; ++b)   // dynamics timing per band (the global attack / release above are only read from old projects)
        {
            const auto name = "Band " + juce::String (b + 1) + " ";
            juce::NormalisableRange<float> ra (0.5f, 200.0f), rr (5.0f, 2000.0f);
            ra.setSkewForCentre (15.0f);
            rr.setSkewForCentre (150.0f);
            auto ms = [] (float v, int) { return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (v, v < 10.0f ? 1 : 0) + " ms"; };
            l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { eqId (b, "atk"), 1 }, name + "attack", ra, 10.0f,
                                                                juce::AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (ms)));
            l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { eqId (b, "rel"), 1 }, name + "release", rr, 120.0f,
                                                                juce::AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (ms)));
        }
        return l;
    }

    static float value (juce::RangedAudioParameter* p) noexcept { return p->convertFrom0to1 (p->getValue()); }
    EqAll settings() const noexcept
    {
        EqAll s;
        for (int b = 0; b < eqBandCount; ++b)
        {
            auto& p = bandParams[(size_t) b];
            auto& x = s.b[(size_t) b];
            x.on = value (p[0]); x.type = value (p[1]); x.freq = value (p[2]); x.gain = value (p[3]); x.q = value (p[4]);
            x.slope = value (p[5]); x.dyn = value (p[6]); x.range = value (p[7]); x.thr = value (p[8]); x.use = value (p[9]); x.atk = value (p[10]); x.rel = value (p[11]);
        }
        s.attack = value (attackP);
        s.release = value (releaseP);
        s.out = value (outP);
        return s;
    }
    juce::RangedAudioParameter* bandParam (int band, int k) const { return bandParams[(size_t) band][(size_t) k]; }

    void prepareToPlay (double, int block) override
    {
        for (auto& v : listenIn) v.assign ((size_t) juce::jmax (block, 512) * 2, 0.0f);
        for (auto& c : listenFilters) for (auto& f : c) f.reset();
        listenDesigned = -2;
        for (auto& c : filters) for (auto& f : c) for (auto& s : f.sec) s.reset();
        for (auto& d : detectors) d.reset();
        std::fill (envelopes.begin(), envelopes.end(), 0.0f);
        std::fill (applied.begin(), applied.end(), 0.0f);
        for (auto& d : dynNow) d.store (0.0f);
        last.out = -1000.0f;   // force a redesign
        spectral.reset();
        spectralWasOn = false;
        outGain.reset (sampleRate(), 0.03);
        outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (value (outP)));
        updateLatency();
    }
    double getTailLengthSeconds() const override { return 0.0; }

    void setStateInformation (const void* data, int size) override
    {
        auto x = getXmlFromBinary (data, size);
        if (x == nullptr || ! x->hasTagName (state.state.getType())) return;
        auto tree = juce::ValueTree::fromXml (*x);
        if (tree.getChildWithProperty ("id", "m1G").isValid()) tree = migrateOldState (tree);
        if (! tree.getChildWithProperty ("id", "b1atk").isValid())   // saved with one attack / release for all bands
        {
            const auto g = [&tree] (const char* id, float def) {
                auto c = tree.getChildWithProperty ("id", id);
                return c.isValid() ? (float) c["value"] : def;
            };
            const float a = g ("attack", 10.0f), r = g ("release", 120.0f);
            for (int b = 0; b < eqBandCount; ++b)
                for (auto [w, v] : { std::pair<const char*, float> { "atk", a }, { "rel", r } })
                {
                    auto c = tree.getChildWithProperty ("id", eqId (b, w));
                    if (! c.isValid()) { c = juce::ValueTree ("PARAM"); c.setProperty ("id", eqId (b, w), nullptr); tree.appendChild (c, nullptr); }
                    c.setProperty ("value", v, nullptr);
                }
        }
        else if (! tree.getChildWithProperty ("id", "b1use").isValid())   // saved before bands could be bypassed: on = in use
            for (int b = 0; b < eqBandCount; ++b)
            {
                juce::ValueTree c ("PARAM");
                c.setProperty ("id", eqId (b, "use"), nullptr);
                c.setProperty ("value", tree.getChildWithProperty ("id", eqId (b, "on"))["value"], nullptr);
                tree.appendChild (c, nullptr);
            }
        state.replaceState (tree);
        updateLatency();
    }

    void process (float* L, float* R, int n) noexcept override
    {
        const auto s = settings();
        const double sr = sampleRate();
        // listening to a band: keep the input (the band region is taken from what comes in)
        const int listen = listenBand.load (std::memory_order_relaxed);
        const bool listening = juce::isPositiveAndBelow (listen, eqBandCount) && s.b[(size_t) listen].isUsed()
                               && (size_t) n <= listenIn[0].size();
        if (listening)
        {
            std::copy (L, L + n, listenIn[0].begin());
            std::copy (R != nullptr ? R : L, (R != nullptr ? R : L) + n, listenIn[1].begin());
        }
        if (! (s == last))
        {
            for (int b = 0; b < eqBandCount; ++b)
            {
                const auto& band = s.b[(size_t) b];
                active[(size_t) b] = band.isOn() && (! eqHasGain (band.typeI()) || std::abs (band.gain) > 0.01f || band.dynamic());
                for (auto& c : filters) c[(size_t) b].design (band, sr, band.gain + (band.dynamic() ? applied[(size_t) b] : 0.0f));
                if (band.dynamic())
                {
                    const int t = band.typeI();
                    detectors[(size_t) b].set (t == eqLowShelf ? Biquad::lowPass : t == eqHighShelf ? Biquad::highPass : Biquad::bandPass,
                                               sr, band.freq, t == eqBell ? juce::jmax (0.3f, band.q) : 0.7071f);
                }
                else
                {
                    applied[(size_t) b] = 0.0f;
                    dynNow[(size_t) b].store (0.0f, std::memory_order_relaxed);
                }
            }
            spectral.setWeights (s, sr);
            last = s;
        }
        float* chans[2] = { L, R };
        const int nch = R != nullptr ? 2 : 1;
        constexpr int sub = 32;
        for (int off = 0; off < n; off += sub)
        {
            const int len = juce::jmin (sub, n - off);
            for (int b = 0; b < eqBandCount; ++b)
            {
                const auto& band = s.b[(size_t) b];
                if (! band.dynamic()) continue;
                // level in the band (before the EQ), RMS-like with the attack / release times
                auto& det = detectors[(size_t) b];
                float e = envelopes[(size_t) b];
                const float atk = std::exp (-1.0f / (float) (juce::jmax (0.0005f, band.atk * 0.001f) * sr));
                const float rel = std::exp (-1.0f / (float) (juce::jmax (0.005f, band.rel * 0.001f) * sr));
                for (int i = 0; i < len; ++i)
                {
                    const float x = R != nullptr ? 0.5f * (L[off + i] + R[off + i]) : L[off + i];
                    const float y = det.process (x);
                    const float p2 = y * y * 2.0f;   // a sine's peak level
                    e = p2 > e ? atk * e + (1.0f - atk) * p2 : rel * e + (1.0f - rel) * p2;
                }
                envelopes[(size_t) b] = e;
                const float delta = eqDynamicDelta (10.0f * std::log10 (e + 1.0e-12f) - band.thr, band.range);
                if (std::abs (delta - applied[(size_t) b]) > 0.05f)
                {
                    applied[(size_t) b] = delta;
                    for (auto& c : filters) c[(size_t) b].design (band, sr, band.gain + delta);
                }
                dynNow[(size_t) b].store (applied[(size_t) b], std::memory_order_relaxed);
            }
            for (int c = 0; c < nch; ++c)
                for (int b = 0; b < eqBandCount; ++b)
                    if (active[(size_t) b]) filters[(size_t) c][(size_t) b].process (chans[c] + off, len);
        }

        if (listening)   // output only the part of the sound the band works on (what it adds or takes away)
        {
            const auto& band = s.b[(size_t) listen];
            const int key = listen * 1000 + band.typeI();
            if (key != listenDesigned || std::abs (band.freq - listenFreq) > 0.01f || std::abs (band.q - listenQ) > 0.001f)
            {
                const int t = band.typeI();
                const auto type = t == eqLowShelf || t == eqHighCut ? Biquad::lowPass       // a high cut takes away the highs:
                                : t == eqHighShelf || t == eqLowCut ? Biquad::highPass      //  hear them (and the lows of a low cut)
                                : Biquad::bandPass;
                const auto ty = t == eqHighCut ? Biquad::highPass : t == eqLowCut ? Biquad::lowPass : type;
                const double q = ty == Biquad::bandPass ? juce::jmax (0.3, (double) band.q) : 0.7071;
                for (auto& c : listenFilters)
                    for (auto& f : c) f.set (ty, sr, band.freq, q);
                listenDesigned = key;
                listenFreq = band.freq;
                listenQ = band.q;
            }
            float* outs[2] = { L, R };
            for (int c = 0; c < nch; ++c)
            {
                std::copy (listenIn[(size_t) c].begin(), listenIn[(size_t) c].begin() + n, outs[c]);
                for (auto& f : listenFilters[(size_t) c]) f.process (outs[c], n);
            }
        }

        const bool specOn = spectralOn.load (std::memory_order_acquire);
        if (specOn != spectralWasOn) { spectral.reset(); spectralWasOn = specOn; }
        if (specOn) spectral.process (L, R, n, s, sr);

        outGain.setTargetValue (juce::Decibels::decibelsToGain (s.out));
        for (int i = 0; i < n; ++i)
        {
            const float g = outGain.getNextValue();
            L[i] *= g;
            if (R != nullptr) R[i] *= g;
        }
        if (analyserWanted.load (std::memory_order_relaxed) > 0)   // feed the spectrum display (only while it is open)
        {
            int s1, n1, s2, n2;
            scope.prepareToWrite (n, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i) scopeBuf[(size_t) (s1 + i)] = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
            for (int i = 0; i < n2; ++i) scopeBuf[(size_t) (s2 + i)] = R != nullptr ? 0.5f * (L[n1 + i] + R[n1 + i]) : L[n1 + i];
            scope.finishedWrite (n1 + n2);
        }
    }

    juce::AudioProcessorEditor* createEditor() override;

    // shared with the editor
    static constexpr int scopeSize = 16384;
    juce::AbstractFifo scope { scopeSize };
    std::vector<float> scopeBuf = std::vector<float> ((size_t) scopeSize, 0.0f);
    std::atomic<int> analyserWanted { 0 };   // how many graphs are open (rack and plugin window can both be)
    std::array<std::atomic<float>, eqBandCount> dynNow {};   // live dynamic gain change per band (dB)
    EqSpectral spectral;
    std::atomic<bool> spectralOn { false };
    std::atomic<int> listenBand { -1 };   // band being listened to (the editor's Listen button / Alt-drag), -1: none

private:
    std::array<std::array<juce::RangedAudioParameter*, 12>, eqBandCount> bandParams {};
    juce::RangedAudioParameter *attackP = nullptr, *releaseP = nullptr, *outP = nullptr;
    std::array<std::array<EqBandFilter, eqBandCount>, 2> filters;
    std::array<Biquad, eqBandCount> detectors;
    std::array<float, eqBandCount> envelopes {}, applied {};
    std::array<bool, eqBandCount> active {};
    std::array<std::vector<float>, 2> listenIn;
    std::array<std::array<Biquad, 2>, 2> listenFilters;   // per channel, two sections: a steeper slice
    int listenDesigned = -2;
    float listenFreq = 0.0f, listenQ = 0.0f;
    EqAll last {};
    bool spectralWasOn = false;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain;

    /** Spectral mode adds a frame of latency; reported (and switched) on the message thread. */
    void updateLatency()
    {
        const bool want = settings().anySpectral();
        spectralOn.store (want, std::memory_order_release);
        setLatencySamples (want ? EqSpectral::size : 0);
    }
    void parameterValueChanged (int, float) override { triggerAsyncUpdate(); }
    void parameterGestureChanged (int, bool) override {}
    void handleAsyncUpdate() override { updateLatency(); }

    /** Projects saved with the earlier six-band EQ keep their sound. */
    juce::ValueTree migrateOldState (const juce::ValueTree& old)
    {
        auto get = [&old] (const char* id, float def) {
            auto c = old.getChildWithProperty ("id", id);
            return c.isValid() ? (float) c["value"] : def;
        };
        auto tree = state.copyState().createCopy();
        auto set = [&tree] (const juce::String& id, float v) {
            auto c = tree.getChildWithProperty ("id", id);
            if (c.isValid()) c.setProperty ("value", v, nullptr);
        };
        auto band = [&] (int b, bool on, int type, float f, float g, float q) {
            set (eqId (b, "on"), on ? 1.0f : 0.0f);
            set (eqId (b, "use"), on ? 1.0f : 0.0f);
            set (eqId (b, "type"), (float) type);
            set (eqId (b, "freq"), f);
            set (eqId (b, "gain"), g);
            set (eqId (b, "q"), q);
        };
        const float hp = get ("hp", 10.0f), lp = get ("lp", 22000.0f);
        band (0, hp > 10.5f, eqLowCut, hp, 0.0f, 0.7071f);
        band (1, std::abs (get ("lowG", 0.0f)) > 0.01f, eqLowShelf, get ("lowF", 100.0f), get ("lowG", 0.0f), 0.7071f);
        band (2, std::abs (get ("m1G", 0.0f)) > 0.01f, eqBell, get ("m1F", 500.0f), get ("m1G", 0.0f), get ("m1Q", 1.0f));
        band (3, std::abs (get ("m2G", 0.0f)) > 0.01f, eqBell, get ("m2F", 2500.0f), get ("m2G", 0.0f), get ("m2Q", 1.0f));
        band (4, std::abs (get ("highG", 0.0f)) > 0.01f, eqHighShelf, get ("highF", 8000.0f), get ("highG", 0.0f), 0.7071f);
        band (5, lp < 21999.0f, eqHighCut, lp, 0.0f, 0.7071f);
        set ("out", get ("out", 0.0f));
        return tree;
    }
};

// ------------------------------------------------------------------------------------------------- display

/** The EQ graph: spectrum, each band's curve and the total, dynamic / spectral activity, a node per band.
    Double-click empty space: new band. Drag a node: frequency / gain. Mouse wheel: Q (Shift + wheel: dynamic range).
    Double-click a node: remove it. Right-click a node: type, slope, dynamics. */
class EqGraph final : public juce::Component, private juce::Timer
{
public:
    explicit EqGraph (Eq& e) : eq (e)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }
    ~EqGraph() override { if (counted) --eq.analyserWanted; }

    std::function<void (int)> onSelect;
    std::function<void()> onListenChanged;
    int selected = -1;
    bool altListening = false;
    bool compact = false;   // the slim rack version: no axis labels, readout, spectrum or hint text
    void select (int b)
    {
        selected = b;
        if (onSelect) onSelect (b);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (float f : { 30.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 1000.0f, 2000.0f, 3000.0f, 5000.0f, 10000.0f, 20000.0f })
        {
            const float x = xFor (f);
            g.setColour (col::outline.withAlpha (f == 100.0f || f == 1000.0f || f == 10000.0f ? 0.9f : 0.45f));
            g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());
            g.setColour (col::dim);
            if (! compact)
                g.drawText (f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f)),
                            juce::Rectangle<float> (x + 2, r.getBottom() - 14, 30, 12), juce::Justification::centredLeft);
        }
        for (float db : { -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f, 24.0f, 30.0f })
        {
            const float y = yFor (db);
            g.setColour (col::outline.withAlpha (db == 0.0f ? 1.0f : 0.4f));
            g.drawHorizontalLine (juce::roundToInt (y), r.getX(), r.getRight());
            g.setColour (col::dim);
            if (! compact)
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (r.getRight() - 28, y - 12, 26, 12),
                            juce::Justification::centredRight);
        }
        paintSpectrum (g);

        const auto s = eq.settings();
        const double sr = juce::jmax (44100.0, eq.getSampleRate());
        std::array<EqBandFilter, eqBandCount> stat, live;
        for (int b = 0; b < eqBandCount; ++b)
        {
            const auto& band = s.b[(size_t) b];
            stat[(size_t) b].design (band, sr, band.gain);
            live[(size_t) b].design (band, sr, band.gain + (band.dynamic() ? eq.dynNow[(size_t) b].load() : 0.0f));
        }

        // the selected band: its own curve, and for dynamics the range it can move through
        if (juce::isPositiveAndBelow (selected, eqBandCount) && s.b[(size_t) selected].isOn())
        {
            const auto& band = s.b[(size_t) selected];
            const auto c = eqBandColours[selected];
            if (band.dynamic() || band.spectral())
            {
                EqBandFilter reach;
                reach.design (band, sr, band.gain + band.range);
                juce::Path area;
                for (int px = 0; px <= getWidth(); px += 2)
                {
                    const float y = yFor (dbOf (stat[(size_t) selected], px, sr));
                    if (px == 0) area.startNewSubPath ((float) px, y); else area.lineTo ((float) px, y);
                }
                for (int px = getWidth() / 2 * 2; px >= 0; px -= 2) area.lineTo ((float) px, yFor (dbOf (reach, px, sr)));
                area.closeSubPath();
                g.setColour (c.withAlpha (0.16f));
                g.fillPath (area);
            }
            g.setColour (c.withAlpha (0.8f));
            g.strokePath (curveOf ([&] (int px) { return dbOf (live[(size_t) selected], px, sr); }), juce::PathStrokeType (1.2f));
        }

        // spectral dynamics at work: how much each frequency is being moved right now
        if (eq.spectralOn.load())
        {
            juce::Path sp;
            const int pts = EqSpectral::displayPoints;
            for (int d = 0; d < pts; ++d)
            {
                const float x = xFor (20.0f * std::pow (1000.0f, (float) d / (float) (pts - 1)));
                const float y = yFor (juce::jlimit (-24.0f, 24.0f, eq.spectral.display[(size_t) d].load()));
                if (d == 0) sp.startNewSubPath (x, y); else sp.lineTo (x, y);
            }
            juce::Path fill (sp);
            fill.lineTo (xFor (20000.0f), yFor (0.0f));
            fill.lineTo (xFor (20.0f), yFor (0.0f));
            fill.closeSubPath();
            g.setColour (col::bad.withAlpha (0.25f));
            g.fillPath (fill);
            g.setColour (col::bad.withAlpha (0.7f));
            g.strokePath (sp, juce::PathStrokeType (1.0f));
        }

        // total
        const float outDb = s.out;
        auto total = curveOf ([&] (int px) {
            double db = outDb;
            for (int b = 0; b < eqBandCount; ++b)
                if (s.b[(size_t) b].isOn()) db += dbOf (live[(size_t) b], px, sr);
            return (float) db;
        });
        juce::Path fill (total);
        fill.lineTo ((float) getWidth(), yFor (0.0f));
        fill.lineTo (0.0f, yFor (0.0f));
        fill.closeSubPath();
        g.setColour (col::graph.withAlpha (0.10f));
        g.fillPath (fill);
        g.setColour (col::text);
        g.strokePath (total, juce::PathStrokeType (2.0f));

        // nodes
        for (int b = 0; b < eqBandCount; ++b)
        {
            const auto& band = s.b[(size_t) b];
            if (! band.isUsed()) continue;
            const auto p = nodePos (b, s);
            const auto c = eqBandColours[b];
            if (! band.isOn())   // bypassed: a hollow point, still there to grab and switch back on
            {
                const bool hotB = b == selected || b == hover || b == dragging;
                g.setColour (col::well);
                g.fillEllipse (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f);
                g.setColour (c.withAlpha (hotB ? 0.9f : 0.5f));
                g.drawEllipse (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f, 1.5f);
                if (b == selected) { g.setColour (juce::Colours::white.withAlpha (0.7f)); g.drawEllipse (p.x - 9.0f, p.y - 9.0f, 18.0f, 18.0f, 1.0f); }
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (juce::String (b + 1), juce::Rectangle<float> (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f), juce::Justification::centred);
                continue;
            }
            const bool hot = b == selected || b == hover || b == dragging;
            if (band.dynamic() || band.spectral())   // a ring marks a dynamic band
            {
                g.setColour (c.withAlpha (0.9f));
                g.drawEllipse (p.x - 10.0f, p.y - 10.0f, 20.0f, 20.0f, 1.5f);
                if (band.dynamic())
                {
                    g.setColour (c.withAlpha (0.6f));
                    const float ly = yFor (band.gain + eq.dynNow[(size_t) b].load());
                    g.drawLine (p.x, p.y, p.x, ly, 2.0f);
                }
            }
            g.setColour (c.withAlpha (hot ? 1.0f : 0.85f));
            g.fillEllipse (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f);
            if (b == selected) { g.setColour (juce::Colours::white); g.drawEllipse (p.x - 8.0f, p.y - 8.0f, 16.0f, 16.0f, 1.5f); }
            g.setColour (col::bg);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f), juce::Justification::centred);
        }

        // readout for the node under the mouse
        const int info = dragging >= 0 ? dragging : hover;
        if (! compact && info >= 0 && s.b[(size_t) info].isUsed())
        {
            const auto& band = s.b[(size_t) info];
            juce::String t = eqTypeNames[band.typeI()] + "  " + freqText (band.freq);
            if (eqHasGain (band.typeI())) t << "  " << (band.gain > 0 ? "+" : "") << juce::String (band.gain, 1) << " dB";
            if (eqIsCut (band.typeI())) t << "  " << eqSlopeNames[juce::roundToInt (band.slope)];
            else t << "  Q " << juce::String (band.q, 2);
            if (band.dynamic() || band.spectral()) t << "  (" << eqDynNames[band.dynI()] << ")";
            if (! band.isOn()) t << "  (bypassed)";
            const auto p = nodePos (info, s);
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            auto box = juce::Rectangle<float> (p.x + 12.0f, p.y - 30.0f, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), t) + 14.0f, 20.0f)
                           .constrainedWithin (r.reduced (4.0f));
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (eqBandColours[info]);
            g.drawText (t, box, juce::Justification::centred);
        }
        if (! compact && ! anyOn (s))
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawText ("Double-click to add a band", r.withTrimmedBottom (r.getHeight() * 0.3f), juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { const int h = nodeAt (e.position); if (h != hover) { hover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int b = nodeAt (e.position);
        if (e.mods.isPopupMenu())
        {
            if (b >= 0) { select (b); showMenu (b); }
            return;
        }
        select (b);
        dragging = b;
        if (b < 0) return;
        if (e.mods.isAltDown()) { eq.listenBand = b; altListening = true; }   // Alt-drag: hear the band while moving it
        for (int k : { 2, 3 }) eq.bandParam (b, k)->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const auto s = eq.settings();
        const auto& band = s.b[(size_t) dragging];
        float f = freqFor (e.position.x);
        if (e.mods.isShiftDown()) f = band.freq;          // Shift: gain only
        set (dragging, 2, f);
        if (eqHasGain (band.typeI()) && ! e.mods.isCommandDown())   // Ctrl: frequency only
            set (dragging, 3, juce::jlimit (-eqGainRange, eqGainRange, dbFor (e.position.y)));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) for (int k : { 2, 3 }) eq.bandParam (dragging, k)->endChangeGesture();
        dragging = -1;
        if (altListening) { altListening = false; eq.listenBand = -1; if (onListenChanged) onListenChanged(); }
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = nodeAt (e.position);
        if (b >= 0) { set (b, 9, 0.0f); select (-1); return; }   // remove the band
        const auto s = eq.settings();
        int free = -1;
        for (int k = 0; k < eqBandCount && free < 0; ++k) if (! s.b[(size_t) k].isUsed()) free = k;
        if (free < 0) return;
        const float f = freqFor (e.position.x);
        const int type = f < 35.0f ? eqLowCut : f > 15000.0f ? eqHighCut : eqBell;
        set (free, 1, (float) type);
        set (free, 2, f);
        set (free, 3, type == eqBell ? juce::jlimit (-eqGainRange, eqGainRange, dbFor (e.position.y)) : 0.0f);
        set (free, 4, type == eqBell ? 1.0f : 0.7071f);
        set (free, 6, 0.0f);
        set (free, 0, 1.0f);
        set (free, 9, 1.0f);
        select (free);
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        int b = nodeAt (e.position);
        if (b < 0) b = selected;
        if (b < 0) return;
        const auto s = eq.settings();
        const auto& band = s.b[(size_t) b];
        const float step = w.deltaY > 0 ? 1.0f : -1.0f;
        if (e.mods.isShiftDown()) set (b, 7, juce::jlimit (-eqGainRange, eqGainRange, band.range + step));
        else if (eqIsCut (band.typeI())) set (b, 5, (float) juce::jlimit (0, 7, juce::roundToInt (band.slope) + (int) step));
        else set (b, 4, juce::jlimit (0.1f, 30.0f, band.q * (step > 0 ? 1.12f : 1.0f / 1.12f)));
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && selected >= 0)
        {
            set (selected, 9, 0.0f);
            select (-1);
            return true;
        }
        return false;
    }

private:
    Eq& eq;
    int dragging = -1, hover = -1;

    void set (int band, int k, float v)
    {
        auto* p = eq.bandParam (band, k);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }
    void showMenu (int b)
    {
        const auto s = eq.settings();
        const auto& band = s.b[(size_t) b];
        juce::PopupMenu m, types, slopes, dyn;
        for (int t = 0; t < eqTypeNames.size(); ++t) types.addItem (100 + t, eqTypeNames[t], true, band.typeI() == t);
        for (int t = 0; t < eqSlopeNames.size(); ++t) slopes.addItem (200 + t, eqSlopeNames[t], true, juce::roundToInt (band.slope) == t);
        for (int t = 0; t < eqDynNames.size(); ++t)
            dyn.addItem (300 + t, eqDynNames[t], t == eqDynOff || (t == eqDynDynamic ? eqHasGain (band.typeI()) : ! eqIsCut (band.typeI())),
                         band.dynI() == t);
        m.addSubMenu ("Type", types);
        m.addSubMenu ("Slope", slopes, eqIsCut (band.typeI()));
        m.addSubMenu ("Dynamics", dyn);
        m.addSeparator();
        m.addItem (2, band.isOn() ? "Bypass band" : "Switch band on", true, ! band.isOn());
        m.addItem (1, "Delete band  (double-click)");
        juce::Component::SafePointer<EqGraph> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, b] (int r) {
            if (safe == nullptr || r == 0) return;
            if (r == 1) { safe->set (b, 9, 0.0f); safe->select (-1); }
            else if (r == 2) safe->set (b, 0, safe->eq.settings().b[(size_t) b].isOn() ? 0.0f : 1.0f);
            else if (r >= 300) safe->set (b, 6, (float) (r - 300));
            else if (r >= 200) safe->set (b, 5, (float) (r - 200));
            else if (r >= 100) safe->set (b, 1, (float) (r - 100));
            safe->select (b);
        });
    }

    static bool anyOn (const EqAll& s)
    {
        for (auto& b : s.b) if (b.isUsed()) return true;
        return false;
    }
    static juce::String freqText (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz"; }
    float dbOf (const EqBandFilter& f, int px, double sr) const
    {
        return (float) juce::Decibels::gainToDecibels (f.magnitude (freqFor ((float) px), sr), -60.0);
    }
    template <typename F> juce::Path curveOf (F&& dbAt) const
    {
        juce::Path p;
        for (int px = 0; px <= getWidth(); px += 2)
        {
            const float y = yFor (juce::jlimit (-40.0f, 40.0f, dbAt (px)));
            if (px == 0) p.startNewSubPath ((float) px, y); else p.lineTo ((float) px, y);
        }
        return p;
    }

    // ---- spectrum analyser ----
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> history = std::vector<float> ((size_t) fftSize, 0.0f), work = std::vector<float> ((size_t) fftSize * 2, 0.0f);
    std::vector<float> spectrum = std::vector<float> ((size_t) fftSize / 2, -120.0f);
    bool hasSpectrum = false;
    int quietTicks = 0;

    void paintSpectrum (juce::Graphics& g)
    {
        if (! hasSpectrum) return;
        juce::Path spec;
        const double sr0 = juce::jmax (8000.0, eq.getSampleRate());
        spec.startNewSubPath (0.0f, (float) getHeight());
        for (int px = 0; px <= getWidth(); px += 2)
        {
            const double f = freqFor ((float) px);
            const float bin = (float) (f / sr0 * fftSize);
            const int b0 = juce::jlimit (1, fftSize / 2 - 2, (int) bin);
            const float fr = juce::jlimit (0.0f, 1.0f, bin - (float) b0);
            const float db = spectrum[(size_t) b0] + fr * (spectrum[(size_t) b0 + 1] - spectrum[(size_t) b0]);
            spec.lineTo ((float) px, (float) getHeight() * (1.0f - juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f)));
        }
        spec.lineTo ((float) getWidth(), (float) getHeight());
        spec.closeSubPath();
        g.setColour (col::info.withAlpha (0.18f));
        g.fillPath (spec);
        g.setColour (col::info.withAlpha (0.4f));
        g.strokePath (spec, juce::PathStrokeType (1.0f));
    }
    void pullSamples()
    {
        int s1, n1, s2, n2;
        const int ready = eq.scope.getNumReady();
        if (ready == 0) { if (++quietTicks > 15) hasSpectrum = false; return; }
        quietTicks = 0;
        eq.scope.prepareToRead (ready, s1, n1, s2, n2);
        auto push = [this] (const float* d, int n) {
            if (n >= fftSize) { std::copy (d + n - fftSize, d + n, history.begin()); return; }
            std::move (history.begin() + n, history.end(), history.begin());
            std::copy (d, d + n, history.end() - n);
        };
        push (eq.scopeBuf.data() + s1, n1);
        if (n2 > 0) push (eq.scopeBuf.data() + s2, n2);
        eq.scope.finishedRead (n1 + n2);
        std::copy (history.begin(), history.end(), work.begin());
        std::fill (work.begin() + fftSize, work.end(), 0.0f);
        window.multiplyWithWindowingTable (work.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (work.data());
        const float norm = 4.0f / (float) fftSize;
        for (int b = 0; b < fftSize / 2; ++b)
        {
            const float db = juce::Decibels::gainToDecibels (work[(size_t) b] * norm, -120.0f);
            auto& v = spectrum[(size_t) b];
            v = db > v ? 0.6f * db + 0.4f * v : juce::jmax (db, v - 1.5f);
        }
        hasSpectrum = true;
    }
    /** Whether any of the graph can be seen: on screen, and not scrolled out of a viewport it sits in. */
    bool reallyVisible() const
    {
        if (! isShowing()) return false;
        auto r = getLocalBounds();
        for (const juce::Component* c = this; c->getParentComponent() != nullptr; c = c->getParentComponent())
        {
            auto* p = c->getParentComponent();
            r = p->getLocalArea (c, r).getIntersection (p->getLocalBounds());
            if (r.isEmpty()) return false;
        }
        return true;
    }
    bool counted = false;   // this graph is currently asking the EQ for its spectrum samples
    void timerCallback() override
    {
        // Many EQs can be open (one per track): only the ones in view analyse and draw
        const bool vis = reallyVisible();
        if (vis != counted) { counted = vis; if (vis) ++eq.analyserWanted; else --eq.analyserWanted; }
        if (! vis) return;
        pullSamples();
        repaint();
    }

    static constexpr float fLo = 20.0f, fHi = 20000.0f, dbRange = 32.0f;
    float xFor (float f) const { return (float) getWidth() * std::log (f / fLo) / std::log (fHi / fLo); }
    float freqFor (float x) const { return juce::jlimit (fLo, fHi, fLo * std::pow (fHi / fLo, x / (float) juce::jmax (1, getWidth()))); }
    float yFor (float db) const { return (float) getHeight() * 0.5f - db / dbRange * (float) getHeight() * 0.5f; }
    float dbFor (float y) const { return ((float) getHeight() * 0.5f - y) / ((float) getHeight() * 0.5f) * dbRange; }
    juce::Point<float> nodePos (int b, const EqAll& s) const
    {
        const auto& band = s.b[(size_t) b];
        const float x = xFor (juce::jlimit (fLo, fHi, band.freq));
        const float y = yFor (eqHasGain (band.typeI()) ? band.gain : 0.0f);
        return { x, juce::jlimit (8.0f, (float) getHeight() - 8.0f, y) };
    }
    int nodeAt (juce::Point<float> p) const
    {
        const auto s = eq.settings();
        int best = -1;
        float bestD = 13.0f;
        for (int b = 0; b < eqBandCount; ++b)
            if (s.b[(size_t) b].isUsed())
                if (const float d = nodePos (b, s).getDistanceFrom (p); d < bestD) { bestD = d; best = b; }
        return best;
    }
};

// ------------------------------------------------------------------------------------------------- editor

class EqEditor final : public juce::AudioProcessorEditor
{
public:
    ~EqEditor() override { eq.listenBand = -1; }
    explicit EqEditor (Eq& e) : juce::AudioProcessorEditor (e), eq (e), graph (e)
    {
        addAndMakeVisible (graph);
        graph.onSelect = [this] (int b) { showBand (b); };
        title.setText ("EQ", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, col::text);
        addAndMakeVisible (title);
        hint.setText ("Double-click: add band.  Drag: frequency / gain (Shift: gain only, Ctrl: frequency only).  Wheel: Q or slope "
                      "(Shift + wheel: dynamic range).  Right-click: type, slope, dynamics.  Double-click a band or Delete: remove.",
                      juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (11.5f)));
        hint.setColour (juce::Label::textColourId, col::dim);
        addAndMakeVisible (hint);

        bandLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        addAndMakeVisible (bandLabel);
        for (auto* b : { &typeBox, &slopeBox, &dynBox }) addAndMakeVisible (*b);
        typeBox.addItemList (eqTypeNames, 1);
        slopeBox.addItemList (eqSlopeNames, 1);
        dynBox.addItemList (eqDynNames, 1);
        typeBox.setTooltip ("Filter type");
        slopeBox.setTooltip ("How steeply the cut falls away (dB per octave)");
        dynBox.setTooltip ("Off: fixed EQ.  Dynamic: the band's gain moves with the level in the band (above the threshold it "
                           "moves toward Gain + Range).  Spectral: the same, but frequency by frequency inside the band, so only "
                           "the frequencies that poke out are moved (adds latency).");
        onButton.setClickingTogglesState (true);
        onButton.setColour (juce::TextButton::buttonOnColourId, col::accent);
        onButton.setTooltip ("Band on / bypassed");
        addAndMakeVisible (onButton);
        removeButton.onClick = [this] {
            if (graph.selected < 0) return;
            eq.bandParam (graph.selected, 9)->setValueNotifyingHost (0.0f);
            graph.select (-1);
        };
        addAndMakeVisible (removeButton);
        listenButton.setClickingTogglesState (false);
        listenButton.setColour (juce::TextButton::buttonOnColourId, col::info);
        listenButton.setTooltip ("Listen: hear only the part of the sound this band works on (what it boosts, cuts or removes). "
                                 "Tip: hold Alt while dragging a band to listen while you move it.");
        listenButton.onClick = [this] {
            const int b = graph.selected;
            eq.listenBand = eq.listenBand.load() == b ? -1 : b;
            syncListen();
        };
        addAndMakeVisible (listenButton);
        graph.onListenChanged = [this] { syncListen(); };

        const char* names[] { "Frequency", "Gain", "Q", "Threshold", "Range", "Attack", "Release", "Output" };   // 0..6 band, 7 global
        for (int k = 0; k < 8; ++k)
        {
            auto& kn = knobs[(size_t) k];
            kn.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            kn.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 18);
            kn.slider.setColour (juce::Slider::rotarySliderFillColourId, k >= 3 && k < 7 ? col::info : col::accent);
            kn.label.setText (names[k], juce::dontSendNotification);
            kn.label.setJustificationType (juce::Justification::centred);
            kn.label.setFont (juce::Font (juce::FontOptions (11.5f)));
            kn.label.setColour (juce::Label::textColourId, col::dim);
            addAndMakeVisible (kn.slider);
            addAndMakeVisible (kn.label);
        }
        knobs[3].slider.setTooltip ("Level in the band where the dynamics start to act");
        knobs[4].slider.setTooltip ("How far the dynamics can move the gain: negative turns loud parts down, positive turns them up");
        knobs[5].slider.setTooltip ("How fast the dynamics react when the level rises (this band)");
        knobs[6].slider.setTooltip ("How fast the dynamics let go when the level falls (this band)");
        attachGlobal (7, eq.state.getParameter ("out"));
        setResizable (true, true);
        setResizeLimits (760, 520, 2400, 1400);
        setSize (980, 600);
        // start on the first band that is on
        const auto s = eq.settings();
        int first = -1;
        for (int b = 0; b < eqBandCount && first < 0; ++b) if (s.b[(size_t) b].isUsed()) first = b;
        graph.select (first);
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::panel);
        g.fillRoundedRectangle (bandPanel.toFloat(), 6.0f);
        g.fillRoundedRectangle (globalPanel.toFloat(), 6.0f);
        if (graph.selected >= 0)
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText ("Dynamics", dynLabelArea, juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("Select a band (click its point) to edit it, or double-click the graph to add one.", bandPanel,
                        juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        auto top = r.removeFromTop (40);
        title.setBounds (top.removeFromTop (22));
        hint.setBounds (top);
        auto bottom = r.removeFromBottom (128);
        r.removeFromBottom (10);
        graph.setBounds (r);
        globalPanel = bottom.removeFromRight (96);
        bottom.removeFromRight (10);
        bandPanel = bottom;

        auto bp = bandPanel.reduced (10, 8);
        auto left = bp.removeFromLeft (150);
        {
            auto lr = left.removeFromTop (24);
            listenButton.setBounds (lr.removeFromRight (64));
            bandLabel.setBounds (lr);
        }
        left.removeFromTop (4);
        auto row = left.removeFromTop (26);
        onButton.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (6);
        removeButton.setBounds (row);
        left.removeFromTop (6);
        typeBox.setBounds (left.removeFromTop (26));
        left.removeFromTop (6);
        slopeBox.setBounds (left.removeFromTop (26));
        bp.removeFromLeft (10);
        const int w = 74;
        for (int k = 0; k < 3; ++k) placeKnob (k, bp.removeFromLeft (w));
        bp.removeFromLeft (12);
        auto dynCol = bp.removeFromLeft (110);
        dynCol.removeFromTop (18);
        dynLabelArea = dynCol.removeFromTop (16);
        dynBox.setBounds (dynCol.removeFromTop (26));
        for (int k = 3; k < 7; ++k) placeKnob (k, bp.removeFromLeft (w));
        placeKnob (7, globalPanel.reduced (8, 8));
    }

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::SliderParameterAttachment> attach;
    };
    Eq& eq;
    EqGraph graph;
    juce::Label title, hint, bandLabel;
    juce::ComboBox typeBox, slopeBox, dynBox;
    juce::TextButton onButton { "On" }, removeButton { "Remove" }, listenButton { "Listen" };
    void syncListen()
    {
        listenButton.setToggleState (eq.listenBand.load() >= 0 && eq.listenBand.load() == graph.selected, juce::dontSendNotification);
    }
    std::array<Knob, 8> knobs;
    std::unique_ptr<juce::ComboBoxParameterAttachment> typeAttach, slopeAttach, dynAttach;
    std::unique_ptr<juce::ButtonParameterAttachment> onAttach;
    juce::Rectangle<int> bandPanel, globalPanel, dynLabelArea;

    void placeKnob (int k, juce::Rectangle<int> cell)
    {
        auto& kn = knobs[(size_t) k];
        kn.label.setBounds (cell.removeFromTop (16));
        kn.slider.setBounds (cell.reduced (2, 0));
    }
    void attachGlobal (int k, juce::RangedAudioParameter* p)
    {
        if (p == nullptr) return;
        knobs[(size_t) k].attach = std::make_unique<juce::SliderParameterAttachment> (*p, knobs[(size_t) k].slider);
        knobs[(size_t) k].slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    }
    void showBand (int b)
    {
        if (eq.listenBand.load() >= 0) eq.listenBand = b;   // listening follows the selected band
        typeAttach.reset();
        slopeAttach.reset();
        dynAttach.reset();
        onAttach.reset();
        for (int k = 0; k < 7; ++k) knobs[(size_t) k].attach.reset();
        const bool has = b >= 0;
        for (juce::Component* c : { (juce::Component*) &bandLabel, (juce::Component*) &typeBox, (juce::Component*) &slopeBox,
                                    (juce::Component*) &dynBox, (juce::Component*) &onButton, (juce::Component*) &removeButton,
                                    (juce::Component*) &listenButton })
            c->setVisible (has);
        for (int k = 0; k < 7; ++k) { knobs[(size_t) k].slider.setVisible (has); knobs[(size_t) k].label.setVisible (has); }
        if (has)
        {
            bandLabel.setText ("Band " + juce::String (b + 1), juce::dontSendNotification);
            bandLabel.setColour (juce::Label::textColourId, eqBandColours[b]);
            onAttach = std::make_unique<juce::ButtonParameterAttachment> (*eq.bandParam (b, 0), onButton);
            typeAttach = std::make_unique<juce::ComboBoxParameterAttachment> (*eq.bandParam (b, 1), typeBox);
            slopeAttach = std::make_unique<juce::ComboBoxParameterAttachment> (*eq.bandParam (b, 5), slopeBox);
            dynAttach = std::make_unique<juce::ComboBoxParameterAttachment> (*eq.bandParam (b, 6), dynBox);
            const int map[7] { 2, 3, 4, 8, 7, 10, 11 };
            for (int k = 0; k < 7; ++k)
            {
                auto* p = eq.bandParam (b, map[k]);
                knobs[(size_t) k].attach = std::make_unique<juce::SliderParameterAttachment> (*p, knobs[(size_t) k].slider);
                knobs[(size_t) k].slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
            }
        }
        refreshEnablement();
        repaint();
    }
    /** Controls that do nothing for the band's type / dynamics mode are dimmed. */
    void refreshEnablement()
    {
        const int b = graph.selected;
        if (b < 0) return;
        const auto band = eq.settings().b[(size_t) b];
        const int t = band.typeI();
        slopeBox.setEnabled (eqIsCut (t));
        knobs[1].slider.setEnabled (eqHasGain (t));
        knobs[1].label.setEnabled (eqHasGain (t));
        const bool dyn = band.dynamic() || band.spectral();
        for (int k = 3; k < 7; ++k) { knobs[(size_t) k].slider.setEnabled (dyn); knobs[(size_t) k].label.setEnabled (dyn); }
        dynBox.setEnabled (! eqIsCut (t));
    }
    void timerCallback() { refreshEnablement(); syncListen(); }
    struct Ticker final : public juce::Timer
    {
        EqEditor& e;
        explicit Ticker (EqEditor& ed) : e (ed) {}
        void timerCallback() override { e.timerCallback(); }
    } ticker { *this };
    void startTimerHz (int hz) { ticker.startTimerHz (hz); }
};

juce::AudioProcessorEditor* Eq::createEditor() { return new EqEditor (*this); }
