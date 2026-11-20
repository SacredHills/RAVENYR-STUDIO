// =====================================================================================================  NAM amp + cabinet
// Included by BuiltinPlugins.cpp (inside its anonymous namespace, after Base).
//
// A plugin like the other built-ins: its knobs are parameters and the files it uses (the NAM capture and up to two
// cabinet impulse responses) are properties of its state, so they are saved and restored with the project exactly
// like any plugin's settings. The sound comes from an AmpRig running only its gate, amp, EQ and cabinet blocks.

namespace namamp
{
constexpr int numFiles = 3;   // 0 = NAM capture, 1 = cabinet, 2 = second cabinet
inline const char* fileProp (int k) { return k == 0 ? "namFile" : k == 1 ? "irFile" : "irFile2"; }

const std::array<int, 19>& exposed()
{
    using namespace ff::rig;
    static const std::array<int, 19> p { { inGain, gateOn, gateThresh, gateRelease, namNormalize, namCalibrate, calLevel, ampGain, ampLevel,
                                           bass, mid, treble, presence, cabLowCut, cabHighCut, cabBlend, cabPhase1, cabPhase2, outGain } };
    return p;
}

Layout makeLayout()
{
    Layout l;
    for (int p : exposed())
    {
        const auto& i = ff::rig::info (p);
        juce::String label (i.label);
        if (p == ff::rig::gateOn) label = "Noise gate";
        if (p == ff::rig::gateThresh) label = "Gate threshold";
        if (p == ff::rig::gateRelease) label = "Gate release";
        if (p == ff::rig::namNormalize) label = "Normalise";
        if (p == ff::rig::namCalibrate) label = "Calibrate";
        if (p == ff::rig::ampGain) label = "Gain (no capture)";
        if (p == ff::rig::ampLevel) label = "Amp level";
        if (p == ff::rig::cabLowCut) label = "Cab low cut";
        if (p == ff::rig::cabHighCut) label = "Cab high cut";
        if (p == ff::rig::cabPhase1) label = "Cab 1 phase";
        if (p == ff::rig::cabPhase2) label = "Cab 2 phase";
        if (ff::rig::isToggle (p)) l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { i.id, 1 }, label, i.def >= 0.5f));
        else l.add (param (i.id, label.toRawUTF8(), i.min, i.max, i.def, i.unit, i.centre));
    }
    return l;
}
} // namespace namamp

class NamEditor;

class NamAmp final : public Base, private juce::AsyncUpdater, public juce::ChangeBroadcaster
{
public:
    explicit NamAmp (const Info& i) : Base (i, namamp::makeLayout()), rig (&queue)
    {
        using namespace ff::rig;
        for (int b : { comp, drive, chorus, delay, reverb, plug1, plug2, plug3, plug4 }) rig.setParam (switchFor (b), 0.0f);
        for (int b : { amp, eq, cab }) rig.setParam (switchFor (b), 1.0f);
        for (size_t k = 0; k < namamp::exposed().size(); ++k) cached[k] = state.getParameter (info_ (namamp::exposed()[k]).id);
    }

    ~NamAmp() override
    {
        cancelPendingUpdate();
        pool.removeAllJobs (true, 10000);
    }

    // ---- files (message thread) ----
    struct FileStatus { juce::String path, name, error; bool loading = false, loaded = false; };

    /** Points the plugin at a file (an empty path clears it). Saved with the project. */
    void setFile (int kind, const juce::File& f) { setFilePath (kind, f == juce::File() ? juce::String() : f.getFullPathName()); }
    void setFilePath (int kind, const juce::String& path)
    {
        state.state.setProperty (namamp::fileProp (kind), path, nullptr);
        refreshFiles();
    }
    FileStatus fileStatus (int kind) const
    {
        const juce::ScopedLock sl (mx);
        return slots[(size_t) kind].status;
    }

    // ---- plugin state ----
    void setStateInformation (const void* data, int size) override
    {
        Base::setStateInformation (data, size);
        refreshFiles();
    }

    void prepareToPlay (double sr, int block) override
    {
        const int blk = juce::jmax (block, 512);
        rate = sr;
        maxBlock = blk;
        {
            const juce::ScopedLock sl (installLock);
            rig.prepare (sr, blk);
        }
        rightScratch.assign ((size_t) blk, 0.0f);
        // the capture and cabinets of the saved state are in place before the first audio block
        for (int waited = 0; waited < 5000 && pool.getNumJobs() > 0; waited += 2) juce::Thread::sleep (2);
        installReady();
        {
            const juce::ScopedLock sl (installLock);
            rig.finishLoadingOffline();
        }
    }

    void process (float* L, float* R, int n) noexcept override
    {
        for (size_t k = 0; k < namamp::exposed().size(); ++k)
            if (auto* rp = cached[k]) rig.setParam (namamp::exposed()[k], rp->convertFrom0to1 (rp->getValue()));
        if (R != nullptr) { rig.process (L, R, n); return; }
        for (int done = 0; done < n;)
        {
            const int len = juce::jmin ((int) rightScratch.size(), n - done);
            if (len <= 0) return;
            std::copy (L + done, L + done + len, rightScratch.begin());
            rig.process (L + done, rightScratch.data(), len);
            done += len;
        }
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool modelRunning() const noexcept { return rig.modelActive.load(); }
    float inputPeak() noexcept { return rig.inPeak.exchange (0.0f); }
    float outputPeak() noexcept { return rig.outPeak.exchange (0.0f); }

private:
    friend class NamEditor;

    struct Result
    {
        int kind = 0, gen = 0;
        bool ok = false;
        juce::String error;
        std::unique_ptr<ff::fx::NamModel> model;
        juce::AudioBuffer<float> ir;
        double irRate = 0.0;
    };
    struct Slot { FileStatus status; int gen = 0; };

    static const ff::rig::ParamInfo& info_ (int p) { return ff::rig::info (p); }

    void refreshFiles()
    {
        for (int k = 0; k < namamp::numFiles; ++k)
        {
            const auto path = state.state.getProperty (namamp::fileProp (k)).toString();
            int gen = 0;
            {
                const juce::ScopedLock sl (mx);
                auto& s = slots[(size_t) k];
                if (s.status.path == path) continue;
                s.status = {};
                s.status.path = path;
                s.status.name = path.isEmpty() ? juce::String() : juce::File (path).getFileNameWithoutExtension();
                s.status.loading = path.isNotEmpty();
                gen = ++s.gen;
            }
            if (path.isEmpty())
            {
                const juce::ScopedLock sl (installLock);
                if (k == 0) { rig.clearModel(); setLatencySamples (0); }
                else if (k == 1) rig.clearImpulseResponse();
                else rig.clearImpulseResponse2();
                rig.collectGarbage();
                sendChangeMessage();
                continue;
            }
            pool.addJob ([this, k, path, gen] { loadJob (k, path, gen); });
        }
        sendChangeMessage();
    }

    void loadJob (int k, const juce::String& path, int gen)
    {
        Result r;
        r.kind = k;
        r.gen = gen;
        if (k == 0)
        {
            r.model = ff::fx::NamModel::load (juce::File (path), r.error);
            if (r.model != nullptr) { r.model->prepare (rate.load(), maxBlock.load()); r.ok = true; }
        }
        else r.ok = ff::fx::loadImpulseFile (juce::File (path), r.ir, r.irRate, r.error);
        {
            const juce::ScopedLock sl (mx);
            ready.push_back (std::move (r));
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override { installReady(); }

    /** Hands finished loads to the rig (any thread, one at a time). */
    void installReady()
    {
        const juce::ScopedLock il (installLock);
        std::vector<Result> take;
        {
            const juce::ScopedLock sl (mx);
            take.swap (ready);
        }
        for (auto& r : take)
        {
            {
                const juce::ScopedLock sl (mx);
                auto& s = slots[(size_t) r.kind];
                if (r.gen != s.gen) continue;   // superseded by a newer request
                s.status.loading = false;
                s.status.loaded = r.ok;
                s.status.error = r.ok ? juce::String() : r.error;
            }
            if (r.kind == 0)
            {
                if (r.ok)
                {
                    if (! r.model->isPreparedFor (rate.load(), maxBlock.load())) r.model->prepare (rate.load(), maxBlock.load());
                    setLatencySamples (r.model->latencySamples());
                    rig.setModel (std::move (r.model));
                }
                else { rig.clearModel(); setLatencySamples (0); }
            }
            else if (r.ok)
            {
                if (r.kind == 1) rig.setImpulseResponse (std::move (r.ir), r.irRate);
                else rig.setImpulseResponse2 (std::move (r.ir), r.irRate);
            }
            else
            {
                if (r.kind == 1) rig.clearImpulseResponse(); else rig.clearImpulseResponse2();
            }
        }
        rig.collectGarbage();
        if (! take.empty()) sendChangeMessage();
    }

    juce::dsp::ConvolutionMessageQueue queue { 64 };
    ff::fx::AmpRig rig;
    std::array<juce::RangedAudioParameter*, 19> cached {};
    std::vector<float> rightScratch;
    std::atomic<double> rate { 48000.0 };
    std::atomic<int> maxBlock { 512 };

    mutable juce::CriticalSection mx, installLock;
    std::array<Slot, namamp::numFiles> slots;
    std::vector<Result> ready;
    juce::ThreadPool pool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (NamAmp)
};

// ---------------------------------------------------------------------------------------------------  the amp head
namespace ampui
{
// black tolex, black faceplate with white lettering, red jewel lights: a high-gain head
const juce::Colour tolex { 0xff0b0b0b }, plateHi { 0xff1d1d1d }, plateLo { 0xff0a0a0a }, ink { 0xfff0f0f0 }, inkDim { 0xff9c9c9c },
    amber { 0xffff3b30 }, lcdBg { 0xff0e0404 }, trim { 0xffd6d6d6 }, stripe { 0xffc8102e };

class Lnf final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override
    {
        const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (1.0f);
        const float r = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
        const auto c = area.getCentre();
        // scale ticks 0..10
        for (int i = 0; i <= 10; ++i)
        {
            const float a = start + (end - start) * (float) i / 10.0f;
            const float major = (i % 5 == 0) ? 1.0f : 0.0f;
            const float r0 = r * (0.86f - 0.05f * major), r1 = r * 0.99f;
            g.setColour (ink.withAlpha (0.9f));
            g.drawLine (c.x + std::sin (a) * r0, c.y - std::cos (a) * r0, c.x + std::sin (a) * r1, c.y - std::cos (a) * r1, 1.0f + major * 0.8f);
        }
        const float skirt = r * 0.70f, body = r * 0.56f;
        // soft shadow, skirt, ridges, body
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillEllipse (c.x - skirt + 1.5f, c.y - skirt + 3.0f, skirt * 2.0f, skirt * 2.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3a3a), c.x - skirt * 0.5f, c.y - skirt * 0.6f, juce::Colour (0xff070707), c.x + skirt, c.y + skirt, true));
        g.fillEllipse (c.x - skirt, c.y - skirt, skirt * 2.0f, skirt * 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        for (int i = 0; i < 40; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / 40.0f;
            g.drawLine (c.x + std::sin (a) * body, c.y - std::cos (a) * body, c.x + std::sin (a) * skirt, c.y - std::cos (a) * skirt, 1.0f);
        }
        g.setColour (juce::Colour (0xff5c5c5c));
        g.drawEllipse (c.x - skirt, c.y - skirt, skirt * 2.0f, skirt * 2.0f, 1.2f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4a4a), c.x - body * 0.6f, c.y - body * 0.7f, juce::Colour (0xff141414), c.x + body * 0.8f, c.y + body, true));
        g.fillEllipse (c.x - body, c.y - body, body * 2.0f, body * 2.0f);
        g.setColour (juce::Colour (0xff000000).withAlpha (0.7f));
        g.drawEllipse (c.x - body, c.y - body, body * 2.0f, body * 2.0f, 1.0f);
        // pointer
        const float a = start + (end - start) * pos;
        juce::Path ptr;
        ptr.addRoundedRectangle (-1.6f, -skirt + 2.0f, 3.2f, skirt * 0.62f, 1.4f);
        g.setColour (juce::Colour (0xfff4f0e4));
        g.fillPath (ptr, juce::AffineTransform::rotation (a).translated (c.x, c.y));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (down ? 0xff0a0a0a : hover ? 0xff3a3a3a : 0xff2a2a2a), 0.0f, r.getY(), juce::Colour (0xff101010), 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (b.isEnabled() ? trim : trim.withAlpha (0.3f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::Font (juce::FontOptions (10.5f, juce::Font::bold)); }
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setColour (b.isEnabled() ? trim : trim.withAlpha (0.3f));
        g.setFont (getTextButtonFont (b, 0));
        g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds(), juce::Justification::centred);
    }
};

/** A chrome toggle switch with a pilot LED, bound to a boolean parameter. */
class Switch final : public juce::Button
{
public:
    Switch() : juce::Button ({}) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics& g, bool, bool) override
    {
        const auto r = getLocalBounds().toFloat();
        const float cx = r.getCentreX();
        const bool on = getToggleState();
        // LED
        g.setColour (on ? amber : juce::Colour (0xff3c1512));
        g.fillEllipse (cx - 4.0f, r.getY() + 3.0f, 8.0f, 8.0f);
        if (on) { g.setColour (amber.withAlpha (0.3f)); g.fillEllipse (cx - 8.0f, r.getY() - 1.0f, 16.0f, 16.0f); }
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (cx - 4.0f, r.getY() + 3.0f, 8.0f, 8.0f, 1.0f);
        // housing and lever
        const auto house = juce::Rectangle<float> (cx - 10.0f, r.getY() + 17.0f, 20.0f, r.getHeight() - 19.0f);
        g.setColour (juce::Colour (0xff0b0b0b));
        g.fillRoundedRectangle (house, 4.0f);
        g.setColour (juce::Colour (0xff8a8a8a));
        g.drawRoundedRectangle (house, 4.0f, 1.2f);
        const float ly = on ? house.getY() + 2.0f : house.getBottom() - 18.0f;
        const auto lever = juce::Rectangle<float> (cx - 5.5f, ly, 11.0f, 16.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff3f3f3), lever.getX(), 0.0f, juce::Colour (0xff8a8a8a), lever.getRight(), 0.0f, false));
        g.fillRoundedRectangle (lever, 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (lever, 5.0f, 1.0f);
    }
};

/** An amber display for what is loaded in a slot. */
class Lcd final : public juce::Component
{
public:
    void set (const juce::String& t, bool err, bool dim) { if (t != text || err != isErr || dim != isDim) { text = t; isErr = err; isDim = dim; repaint(); } }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (lcdBg);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colour (0xff4a1612));
        g.drawRoundedRectangle (r, 3.0f, 1.2f);
        g.setColour (isErr ? juce::Colour (0xffff6a5a) : isDim ? amber.withAlpha (0.45f) : amber);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        g.drawFittedText (text.toUpperCase(), getLocalBounds().reduced (8, 2), juce::Justification::centredLeft, 1, 0.8f);
    }
private:
    juce::String text; bool isErr = false, isDim = false;
};
} // namespace ampui

/** The plugin window, laid out like an amp head: faceplate with the controls, grille with the capture and cabinet slots. */
class NamEditor final : public juce::AudioProcessorEditor, private juce::ChangeListener, private juce::Timer
{
public:
    explicit NamEditor (NamAmp& p) : juce::AudioProcessorEditor (p), amp (p)
    {
        using namespace ff::rig;
        setLookAndFeel (&lnf);
        auto big = [this] (int param, const char* name) { addKnob (param, name, true); };
        auto small = [this] (int param, const char* name) { addKnob (param, name, false); };
        big (inGain, "Input");           big (gateThresh, "Gate");   big (bass, "Bass");        big (mid, "Mid");
        big (treble, "Treble");          big (presence, "Presence"); big (ampLevel, "Volume");  big (outGain, "Master");
        addSwitch (gateOn, "Gate on");   small (gateRelease, "Release");
        addSwitch (namNormalize, "Normalise"); addSwitch (namCalibrate, "Calibrate"); small (calLevel, "Interface"); small (ampGain, "No-capture gain");
        small (cabLowCut, "Low cut");    small (cabHighCut, "High cut"); small (cabBlend, "Blend");
        addSwitch (cabPhase1, "Cab 1 phase"); addSwitch (cabPhase2, "Cab 2 phase");

        static const char* titles[3] = { "Amp capture", "Cabinet", "Cabinet 2" };
        for (int k = 0; k < 3; ++k)
        {
            auto& row = rows[(size_t) k];
            row.title.setText (juce::String (titles[k]).toUpperCase(), juce::dontSendNotification);
            row.title.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            row.title.setColour (juce::Label::textColourId, ampui::trim);
            row.load.setButtonText ("Load");
            row.browse.setButtonText ("TONE3000");
            row.clear.setButtonText ("Clear");
            row.load.onClick = [this, k] { chooseFile (k); };
            row.browse.onClick = [this, k] { browseTone3000 (k); };
            row.clear.onClick = [this, k] { amp.setFilePath (k, {}); };
            for (auto* c : std::initializer_list<juce::Component*> { &row.title, &row.lcd, &row.load, &row.browse, &row.clear })
            {
                c->setLookAndFeel (&lnf);
                addAndMakeVisible (c);
            }
        }
        amp.addChangeListener (this);
        setSize (880, 468);
        refresh();
        startTimerHz (4);
    }

    ~NamEditor() override
    {
        amp.removeChangeListener (this);
        setLookAndFeel (nullptr);
        for (auto& row : rows)
            for (auto* c : std::initializer_list<juce::Component*> { &row.title, &row.lcd, &row.load, &row.browse, &row.clear }) c->setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        if (cache.getWidth() != getWidth() || cache.getHeight() != getHeight()) buildCache();
        g.drawImageAt (cache, 0, 0);
    }

    void resized() override
    {
        constexpr int bigCell = 100, smallCell = 74;
        const auto plate = facePlate();
        // big knobs, one row
        const int bigW = (plate.getWidth() - 24) / 8;
        for (size_t i = 0; i < bigKnobs.size(); ++i)
        {
            auto cell = juce::Rectangle<int> (plate.getX() + 12 + (int) i * bigW, plate.getY() + 50, bigW, bigCell + 20);
            place (*bigKnobs[i], cell, 66);
        }
        // small controls, one row
        const int sw = (plate.getWidth() - 24) / (int) smallItems.size();
        for (size_t i = 0; i < smallItems.size(); ++i)
        {
            auto cell = juce::Rectangle<int> (plate.getX() + 12 + (int) i * sw, plate.getY() + 172, sw, 84);
            auto& it = *smallItems[i];
            if (it.sw != nullptr)
            {
                it.name.setBounds (cell.removeFromTop (14));
                it.sw->setBounds (cell.withSizeKeepingCentre (30, 46).translated (0, 2));
            }
            else place (it, cell, 40);
        }
        // slots
        auto grille = grillePanel().reduced (22, 18);
        const int rowH = grille.getHeight() / 3;
        for (auto& row : rows)
        {
            auto line = grille.removeFromTop (rowH).reduced (0, 4);
            row.title.setBounds (line.removeFromLeft (110));
            row.clear.setBounds (line.removeFromRight (56));
            line.removeFromRight (6);
            row.browse.setBounds (line.removeFromRight (84));
            line.removeFromRight (6);
            row.load.setBounds (line.removeFromRight (56));
            line.removeFromRight (10);
            row.lcd.setBounds (line);
        }
    }

private:
    struct Item
    {
        ampui::Lnf* dummy = nullptr;
        juce::Label name, value;
        ScrollSafeSlider slider;
        std::unique_ptr<ampui::Switch> sw;
        std::unique_ptr<juce::SliderParameterAttachment> attach;
        std::unique_ptr<juce::ButtonParameterAttachment> buttonAttach;
    };
    struct Row { juce::Label title; ampui::Lcd lcd; juce::TextButton load, browse, clear; };

    juce::Rectangle<int> facePlate() const { return { 16, 16, getWidth() - 32, 274 }; }
    juce::Rectangle<int> grillePanel() const { return { 16, 302, getWidth() - 32, getHeight() - 318 }; }

    void styleLabel (juce::Label& l, float size, bool bold, juce::Colour c)
    {
        l.setFont (juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)));
        l.setColour (juce::Label::textColourId, c);
        l.setJustificationType (juce::Justification::centred);
        l.setMinimumHorizontalScale (0.6f);
        l.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    }

    void addKnob (int param, const char* name, bool big)
    {
        auto it = std::make_unique<Item>();
        auto* rp = amp.state.getParameter (ff::rig::info (param).id);
        styleLabel (it->name, big ? 11.5f : 10.0f, true, ampui::ink);
        it->name.setText (juce::String (name).toUpperCase(), juce::dontSendNotification);
        styleLabel (it->value, 10.5f, false, ampui::inkDim);
        it->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        it->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        it->slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        it->slider.setLookAndFeel (&lnf);
        auto* s = &it->slider;
        auto* v = &it->value;
        it->slider.onValueChange = [rp, s, v] { v->setText (rp->getText (rp->convertTo0to1 ((float) s->getValue()), 14), juce::dontSendNotification); };
        it->attach = std::make_unique<juce::SliderParameterAttachment> (*rp, it->slider);
        it->slider.setDoubleClickReturnValue (true, rp->convertFrom0to1 (rp->getDefaultValue()));
        it->slider.onValueChange();   // the readout for a value that starts where the slider already is
        addAndMakeVisible (it->slider);
        (big ? bigKnobs : smallItems).push_back (std::move (it));
    }

    void addSwitch (int param, const char* name)
    {
        auto it = std::make_unique<Item>();
        auto* bp = dynamic_cast<juce::AudioParameterBool*> (amp.state.getParameter (ff::rig::info (param).id));
        styleLabel (it->name, 10.0f, true, ampui::ink);
        it->name.setText (juce::String (name).toUpperCase(), juce::dontSendNotification);
        it->sw = std::make_unique<ampui::Switch>();
        if (bp != nullptr) it->buttonAttach = std::make_unique<juce::ButtonParameterAttachment> (*bp, *it->sw);
        addAndMakeVisible (*it->sw);
        smallItems.push_back (std::move (it));
    }

    void place (Item& it, juce::Rectangle<int> cell, int diameter)
    {
        it.name.setBounds (cell.removeFromTop (16));
        auto knob = cell.removeFromTop (diameter + 6);
        it.slider.setBounds (knob.withSizeKeepingCentre (diameter + 8, diameter + 6));
        it.value.setBounds (cell.removeFromTop (14));
    }

    void buildCache()
    {
        cache = juce::Image (juce::Image::ARGB, getWidth(), getHeight(), true);
        juce::Graphics g (cache);
        const auto all = getLocalBounds().toFloat();
        // black tolex with a fine grain
        g.setColour (ampui::tolex);
        g.fillRoundedRectangle (all, 10.0f);
        juce::Random rnd (7);
        for (int i = 0; i < getWidth() * getHeight() / 14; ++i)
        {
            g.setColour (juce::Colours::white.withAlpha (rnd.nextFloat() * 0.05f));
            g.fillRect (rnd.nextInt (getWidth()), rnd.nextInt (getHeight()), 1, 1);
        }
        g.setColour (juce::Colour (0xff2a2a2a));
        g.drawRoundedRectangle (all.reduced (0.5f), 10.0f, 1.0f);

        // brushed gold faceplate
        const auto f = facePlate().toFloat();
        g.setGradientFill (juce::ColourGradient (ampui::plateHi, 0.0f, f.getY(), ampui::plateLo, 0.0f, f.getBottom(), false));
        g.fillRoundedRectangle (f, 5.0f);
        for (int y = (int) f.getY() + 2; y < (int) f.getBottom() - 2; y += 2)
        {
            g.setColour ((y / 2) % 2 == 0 ? juce::Colours::white.withAlpha (0.035f) : juce::Colours::black.withAlpha (0.12f));
            g.drawHorizontalLine (y, f.getX() + 3.0f, f.getRight() - 3.0f);
        }
        g.setColour (juce::Colour (0xff4a4a4a));
        g.drawRoundedRectangle (f, 5.0f, 1.5f);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.drawRoundedRectangle (f.reduced (2.5f), 4.0f, 1.0f);
        for (auto pt : { juce::Point<float> (f.getX() + 12, f.getY() + 12), { f.getRight() - 12, f.getY() + 12 }, { f.getX() + 12, f.getBottom() - 12 }, { f.getRight() - 12, f.getBottom() - 12 } })
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff0f0f0), pt.x - 3, pt.y - 3, juce::Colour (0xff6a6a6a), pt.x + 4, pt.y + 4, true));
            g.fillEllipse (pt.x - 4.5f, pt.y - 4.5f, 9.0f, 9.0f);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.drawLine (pt.x - 3.0f, pt.y + 1.5f, pt.x + 3.0f, pt.y - 1.5f, 1.2f);
        }

        // badge and title
        g.setColour (ampui::ink);
        g.setFont (juce::Font (juce::FontOptions (28.0f, juce::Font::bold)));
        g.drawText ("RAVENYR", (int) f.getX() + 26, (int) f.getY() + 6, 240, 34, juce::Justification::centredLeft);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText ("NAM  AMP  +  CABINET", (int) f.getRight() - 330, (int) f.getY() + 10, 290, 24, juce::Justification::centredRight);
        g.setColour (ampui::stripe);
        g.fillRect (f.getX() + 20.0f, f.getY() + 43.0f, f.getWidth() - 40.0f, 3.0f);
        g.setColour (ampui::amber.withAlpha (0.35f));
        g.fillEllipse (f.getRight() - 26.0f, f.getY() + 12.0f, 18.0f, 18.0f);
        g.setColour (ampui::amber);
        g.fillEllipse (f.getRight() - 22.0f, f.getY() + 16.0f, 10.0f, 10.0f);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillEllipse (f.getRight() - 20.0f, f.getY() + 18.0f, 3.0f, 3.0f);

        // sections of the lower row
        auto bracket = [&] (int first, int last, const char* text) {
            const int sw = (facePlate().getWidth() - 24) / (int) smallItems.size();
            const float x0 = (float) (facePlate().getX() + 12 + first * sw + 6), x1 = (float) (facePlate().getX() + 12 + (last + 1) * sw - 6);
            const float y = (float) facePlate().getY() + 262.0f;
            g.setColour (ampui::ink.withAlpha (0.55f));
            g.drawHorizontalLine ((int) y, x0, x1);
            g.drawVerticalLine ((int) x0, y - 4.0f, y);
            g.drawVerticalLine ((int) x1, y - 4.0f, y);
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.setColour (ampui::ink);
            const auto label = juce::String (text);
            const int tw = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 14;
            g.setColour (juce::Colour (0xff111111));
            g.fillRect ((int) ((x0 + x1) * 0.5f) - tw / 2, (int) y - 6, tw, 12);
            g.setColour (ampui::ink);
            g.drawText (label, (int) ((x0 + x1) * 0.5f) - tw / 2, (int) y - 6, tw, 12, juce::Justification::centred);
        };
        g.setColour (ampui::ink.withAlpha (0.25f));
        g.drawHorizontalLine (facePlate().getY() + 160, f.getX() + 20.0f, f.getRight() - 20.0f);
        bracket (0, 1, "GATE");
        bracket (2, 5, "CAPTURE");
        bracket (6, 10, "CABINET");

        // grille cloth with gold piping
        const auto gr = grillePanel().toFloat();
        g.setColour (juce::Colour (0xff0a0908));
        g.fillRoundedRectangle (gr, 4.0f);
        g.saveState();
        g.reduceClipRegion (gr.reduced (3.0f).toNearestInt());
        for (float t = -gr.getHeight(); t < gr.getWidth() + gr.getHeight(); t += 5.0f)
        {
            g.setColour (juce::Colour (0xff1c1c1c));
            g.drawLine (gr.getX() + t, gr.getY(), gr.getX() + t + gr.getHeight(), gr.getBottom(), 1.0f);
            g.setColour (juce::Colour (0xff131313));
            g.drawLine (gr.getX() + t + gr.getHeight(), gr.getY(), gr.getX() + t, gr.getBottom(), 1.0f);
        }
        g.restoreState();
        g.setColour (ampui::trim);
        g.drawRoundedRectangle (gr.reduced (1.0f), 4.0f, 1.6f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (gr, 4.0f, 1.0f);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { refresh(); }

    void refresh()
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto s = amp.fileStatus (k);
            auto& r = rows[(size_t) k];
            if (s.path.isEmpty()) r.lcd.set (k == 0 ? "-- built-in amp --" : k == 1 ? "-- built-in speaker --" : "--", false, true);
            else if (s.error.isNotEmpty()) r.lcd.set (s.error, true, false);
            else r.lcd.set (s.name + (s.loading ? "  ..." : ""), false, s.loading);
            r.clear.setEnabled (s.path.isNotEmpty());
            r.browse.setVisible (static_cast<bool> (tone3000Browser()));
        }
    }

    void chooseFile (int k)
    {
        static juce::File folder = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        chooser = std::make_shared<juce::FileChooser> (k == 0 ? "Choose a NAM capture" : "Choose a cabinet impulse response", folder,
                                                       k == 0 ? "*.nam" : "*.wav;*.aif;*.aiff;*.flac");
        juce::Component::SafePointer<NamEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, k] (const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File() || safe == nullptr) return;
            folder = f.getParentDirectory();
            safe->amp.setFile (k, f);
        });
    }

    void browseTone3000 (int k)
    {
        if (! tone3000Browser()) return;
        juce::WeakReference<NamAmp> target (&amp);
        tone3000Browser() (k != 0, [target, k] (const juce::File& f, bool isIr) {
            if (target != nullptr) target->setFile (isIr ? (k == 0 ? 1 : k) : 0, f);   // an IR from the amp row goes to the first cabinet
        });
    }

    NamAmp& amp;
    ampui::Lnf lnf;
    std::vector<std::unique_ptr<Item>> bigKnobs, smallItems;
    std::array<Row, 3> rows;
    std::shared_ptr<juce::FileChooser> chooser;
    juce::Image cache;
};

juce::AudioProcessorEditor* NamAmp::createEditor() { return new NamEditor (*this); }
