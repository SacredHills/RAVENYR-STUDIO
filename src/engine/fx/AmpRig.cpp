#include "AmpRig.h"
#include <cmath>

namespace ff::fx
{
using namespace ff::rig;

namespace
{
    inline float dbToGain (float db) noexcept { return db <= -95.0f ? 0.0f : std::pow (10.0f, db * 0.05f); }

    inline float peakOf (const float* x, int n) noexcept
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i) p = juce::jmax (p, std::abs (x[i]));
        return p;
    }

    inline void storeMax (std::atomic<float>& a, float v) noexcept
    {
        if (v > a.load (std::memory_order_relaxed)) a.store (v, std::memory_order_relaxed);
    }

    bool isStereoBlock (int b) noexcept { return b == chorus || b == delay || b == reverb || isPluginBlock (b); }
}

AmpRig::AmpRig (juce::dsp::ConvolutionMessageQueue* queue)
{
    for (int p = 0; p < numParams; ++p) params[(size_t) p].store (info (p).def);
    retiredModels.prepare (32);
    using C = juce::dsp::Convolution;
    convolution = queue != nullptr ? std::make_unique<C> (C::Latency { 0 }, *queue) : std::make_unique<C>();
    convolution2 = queue != nullptr ? std::make_unique<C> (C::Latency { 0 }, *queue) : std::make_unique<C>();
}

AmpRig::~AmpRig()
{
    delete pendingModel.exchange (nullptr);
    delete model;
    collectGarbage();
}

void AmpRig::setParamsFrom (const juce::ValueTree& t)
{
    for (int p = 0; p < numParams; ++p) params[(size_t) p].store (get (t, p));
    order.store (packOrder (getOrder (t)));
}

void AmpRig::setModel (std::unique_ptr<NamModel> m)
{
    if (m == nullptr) { clearModel(); return; }
    clearRequested = false;
    delete pendingModel.exchange (m.release());   // a model never seen by the audio thread
}

void AmpRig::clearModel()
{
    delete pendingModel.exchange (nullptr);
    clearRequested = true;
}

void AmpRig::setImpulseResponse (juce::AudioBuffer<float>&& ir, double irRate)
{
    using C = juce::dsp::Convolution;
    convolution->loadImpulseResponse (std::move (ir), irRate, C::Stereo::no, C::Trim::yes, C::Normalise::yes);
    irActive = true;
}

void AmpRig::setImpulseResponse2 (juce::AudioBuffer<float>&& ir, double irRate)
{
    using C = juce::dsp::Convolution;
    convolution2->loadImpulseResponse (std::move (ir), irRate, C::Stereo::no, C::Trim::yes, C::Normalise::yes);
    irActive2 = true;
}

void AmpRig::finishLoadingOffline()
{
    if (maxBlock > 0)
        { convolution->prepare ({ sr, (juce::uint32) maxBlock, 1 }); convolution2->prepare ({ sr, (juce::uint32) maxBlock, 1 }); }   // installs the most recently loaded IRs synchronously
}

void AmpRig::collectGarbage()
{
    NamModel* old[16];
    int n;
    while ((n = retiredModels.pop (old, 16)) > 0)
        for (int i = 0; i < n; ++i) delete old[i];
}

void AmpRig::takeOverModel() noexcept
{
    auto retire = [this] (NamModel* m) {
        if (m != nullptr && retiredModels.push (&m, 1) == 0) {}   // FIFO full: leak rather than free here
    };
    if (clearRequested.exchange (false))
    {
        retire (model);
        model = nullptr;
    }
    if (auto* m = pendingModel.exchange (nullptr))
    {
        retire (model);
        model = m;
        model->reset();
        normG.setCurrentAndTargetValue (getParam (namNormalize) >= 0.5f ? model->normalisationGain() : 1.0f);
        calG.setCurrentAndTargetValue (getParam (namCalibrate) >= 0.5f ? model->calibrationGain (getParam (calLevel)) : 1.0f);
    }
    const bool ready = model != nullptr && model->isPreparedFor (sr, maxBlock);
    modelNeedsPrepare.store (model != nullptr && ! ready, std::memory_order_relaxed);
    modelActive.store (ready, std::memory_order_relaxed);
    latency.store (ready ? model->latencySamples() : 0, std::memory_order_relaxed);
}

void AmpRig::prepare (double rate, int block)
{
    sr = rate;
    maxBlock = juce::jmax (16, block);
    mono.assign ((size_t) maxBlock, 0.0f);
    right2.assign ((size_t) maxBlock, 0.0f);
    tmp.assign ((size_t) maxBlock, 0.0f);

    for (auto* s : { &inG, &outG, &compG, &driveG, &ampG, &normG, &calG })
        s->reset (rate, 0.03);

    const juce::dsp::ProcessSpec mono1 { rate, (juce::uint32) maxBlock, 1 };
    const juce::dsp::ProcessSpec stereo { rate, (juce::uint32) maxBlock, 2 };
    compressor.prepare (mono1);
    compressor.setAttack (8.0f);
    compressor.setRelease (120.0f);
    driveOs.initProcessing ((size_t) maxBlock);
    ampOs.initProcessing ((size_t) maxBlock);
    convolution->prepare (mono1);
    convolution2->prepare (mono1);
    chorusFx.prepare (stereo);
    chorusFx.setCentreDelay (7.0f);
    chorusFx.setFeedback (0.0f);
    reverbFx.prepare (stereo);

    const int dlSize = (int) std::ceil (rate * 2.2) + 8;
    dlL.assign ((size_t) dlSize, 0.0f);
    dlR.assign ((size_t) dlSize, 0.0f);
    dlTime.reset (rate, 0.25);
    dlTime.setCurrentAndTargetValue (getParam (delayTime));

    // fixed filters
    const double os = rate * 2.0;
    driveHp.set (Biquad::highPass, os, 180.0, 0.707);
    ampHp.set (Biquad::highPass, os, 110.0, 0.707);
    ampMid.set (Biquad::peak, os, 750.0, 0.8, 4.0);
    ampLp.set (Biquad::lowPass, os, 9000.0, 0.707);
    spkHp.set (Biquad::highPass, rate, 75.0, 0.9);
    spkPeak.set (Biquad::peak, rate, 2400.0, 1.4, 4.0);
    spkLp1.set (Biquad::lowPass, rate, 4800.0, 0.541);
    spkLp2.set (Biquad::lowPass, rate, 4800.0, 1.307);
    lastDriveTone = lastLowCut = lastHighCut = -1.0f;
    lastEq = { { 99, 99, 99, 99 } };

    // models (the current one and any waiting one) follow the device
    if (model != nullptr) model->prepare (rate, maxBlock);
    if (auto* p = pendingModel.load()) p->prepare (rate, maxBlock);

    reset();
    pluginBuffer.setSize (8, maxBlock);
    pluginMidi.ensureSize (256);
    for (auto& ps : pluginSlots)
        if (ps.owner != nullptr) preparePlugin (*ps.owner, rate, maxBlock);
    preparedRate = rate;
}

void AmpRig::reset() noexcept
{
    for (int b = 0; b < numBlocks; ++b) resetBlock (b);
    wasOn = {};
    inG.setCurrentAndTargetValue (dbToGain (getParam (inGain)));
    outG.setCurrentAndTargetValue (dbToGain (getParam (outGain)));
}

void AmpRig::resetBlock (int b) noexcept
{
    switch (b)
    {
        case gate: gateEnv = 0.0f; gateGain = 0.0f; gateIsOpen = false; break;
        case comp: compressor.reset(); compG.setCurrentAndTargetValue (dbToGain (getParam (compLevel))); break;
        case drive: driveOs.reset(); driveHp.reset(); driveToneLp.reset(); driveG.setCurrentAndTargetValue (dbToGain (getParam (driveLevel))); break;
        case amp: eqBass.reset(); eqMid.reset(); eqTreble.reset(); eqPresence.reset(); ampOs.reset(); ampHp.reset(); ampMid.reset(); ampLp.reset(); if (model != nullptr) model->reset();
                  ampG.setCurrentAndTargetValue (dbToGain (getParam (ampLevel))); break;
        case eq: eqBass.reset(); eqMid.reset(); eqTreble.reset(); eqPresence.reset(); break;
        case cab: convolution->reset(); convolution2->reset(); for (auto* f : { &cabHp1, &cabHp2, &cabLp1, &cabLp2, &spkHp, &spkPeak, &spkLp1, &spkLp2 }) f->reset(); break;
        case chorus: chorusFx.reset(); break;
        case delay: std::fill (dlL.begin(), dlL.end(), 0.0f); std::fill (dlR.begin(), dlR.end(), 0.0f); dlLpL = dlLpR = 0;
                    dlTime.setCurrentAndTargetValue (getParam (delayTime)); break;
        case reverb: reverbFx.reset(); break;
        default: break;
    }
}

void AmpRig::process (float* L, float* R, int n) noexcept
{
    takeOverModel();
    if (! isPreparedFor (sr) || maxBlock == 0) return;
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (int done = 0; done < n;)
    {
        const int len = juce::jmin (maxBlock, n - done);
        processChunk (L + done, R + done, len);
        done += len;
    }
    const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const float load = (float) (secs / juce::jmax (1.0e-6, n / sr));
    cpuLoad.store (cpuLoad.load (std::memory_order_relaxed) * 0.9f + load * 0.1f, std::memory_order_relaxed);
}

void AmpRig::processChunk (float* L, float* R, int n) noexcept
{
    float* m = mono.data();
    for (int i = 0; i < n; ++i) m[i] = 0.5f * (L[i] + R[i]);

    inG.setTargetValue (dbToGain (getParam (inGain)));
    inG.applyGain (m, n);
    storeMax (inPeak, peakOf (m, n));

    float* r = right2.data();
    bool stereo = false;
    const auto ord = unpackOrder (order.load (std::memory_order_relaxed));
    for (int b : ord)
    {
        const bool on = getParam (switchFor (b)) >= 0.5f;
        if (! on) { wasOn[(size_t) b] = false; if (b == gate) gateOpen.store (true, std::memory_order_relaxed); continue; }
        if (! wasOn[(size_t) b]) { resetBlock (b); wasOn[(size_t) b] = true; }

        if (isStereoBlock (b) && ! stereo) { std::copy (m, m + n, r); stereo = true; }
        else if (! isStereoBlock (b) && stereo) { for (int i = 0; i < n; ++i) m[i] = 0.5f * (m[i] + r[i]); stereo = false; }

        switch (b)
        {
            case gate: runGate (m, n); break;
            case comp:
            {
                compressor.setThreshold (getParam (compThresh));
                compressor.setRatio (getParam (compRatio));
                float* ch[1] = { m };
                juce::dsp::AudioBlock<float> blk (ch, 1, (size_t) n);
                compressor.process (juce::dsp::ProcessContextReplacing<float> (blk));
                compG.setTargetValue (dbToGain (getParam (compLevel)));
                compG.applyGain (m, n);
                break;
            }
            case drive: runDrive (m, n); break;
            case amp: runAmp (m, n); runEq (m, n); break;   // the amp's own tone controls
            case eq: break;                                 // (the old separate EQ block: folded into the amp)
            case cab: runCab (m, n); break;
            case chorus:
            {
                chorusFx.setRate (getParam (chorusRate));
                chorusFx.setDepth (getParam (chorusDepth) * 0.6f);
                chorusFx.setMix (getParam (chorusMix) * 0.5f);
                float* ch[2] = { m, r };
                juce::dsp::AudioBlock<float> blk (ch, 2, (size_t) n);
                chorusFx.process (juce::dsp::ProcessContextReplacing<float> (blk));
                break;
            }
            case delay: runDelay (m, r, n); break;
            case plug1: case plug2: case plug3: case plug4: runPlugin (pluginSlotOf (b), m, r, n); break;
            case reverb:
            {
                juce::dsp::Reverb::Parameters p;
                p.roomSize = 0.3f + 0.69f * getParam (reverbSize);
                p.damping = getParam (reverbDamp);
                p.wetLevel = getParam (reverbMix) * 0.2f;   // juce::Reverb scales the wet level by 3 internally
                p.dryLevel = 0.5f * (1.0f - getParam (reverbMix) * 0.4f);   // ... and the dry level by 2: 0.5 is unity, so switching the reverb on adds no volume
                p.width = 1.0f;
                reverbFx.setParameters (p);
                float* ch[2] = { m, r };
                juce::dsp::AudioBlock<float> blk (ch, 2, (size_t) n);
                reverbFx.process (juce::dsp::ProcessContextReplacing<float> (blk));
                break;
            }
            default: break;
        }
    }
    if (! stereo) std::copy (m, m + n, r);

    outG.setTargetValue (dbToGain (getParam (outGain)));
    for (int i = 0; i < n; ++i)
    {
        const float g = outG.getNextValue();
        L[i] = m[i] * g;
        R[i] = r[i] * g;
    }
    storeMax (outPeak, juce::jmax (peakOf (L, n), peakOf (R, n)));
}

std::shared_ptr<juce::AudioPluginInstance> AmpRig::setPlugin (int slot, std::shared_ptr<juce::AudioPluginInstance> p)
{
    if (! juce::isPositiveAndBelow (slot, rig::numPluginSlots)) return p;
    auto& ps = pluginSlots[(size_t) slot];
    auto old = std::move (ps.owner);
    ps.owner = std::move (p);
    ps.active.store (ps.owner.get(), std::memory_order_release);
    return old;
}

int AmpRig::totalLatency() const noexcept
{
    int l = latency.load (std::memory_order_relaxed);
    for (int sl = 0; sl < rig::numPluginSlots; ++sl)
        if (auto* p = pluginSlots[(size_t) sl].active.load (std::memory_order_acquire))
            if (getParam (switchFor (plug1 + sl)) >= 0.5f) l += juce::jmax (0, p->getLatencySamples());
    return l;
}

std::shared_ptr<juce::AudioPluginInstance> AmpRig::getPlugin (int slot) const
{
    return juce::isPositiveAndBelow (slot, rig::numPluginSlots) ? pluginSlots[(size_t) slot].owner : nullptr;
}

void AmpRig::runPlugin (int slot, float* L, float* R, int n) noexcept
{
    if (auto* p = pluginSlots[(size_t) slot].active.load (std::memory_order_acquire))
        runPluginStereo (*p, pluginBuffer, pluginMidi, L, R, n);
}

void AmpRig::runGate (float* x, int n) noexcept
{
    const float open = dbToGain (getParam (gateThresh));
    const float close = open * 0.5f;                    // 6 dB hysteresis
    const float envRel = std::exp (-1.0f / (float) (0.01 * sr));
    const float rel = std::exp (-1.0f / (float) (getParam (gateRelease) * 0.001 * sr));
    const float att = std::exp (-1.0f / (float) (0.0005 * sr));
    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (x[i]);
        gateEnv = a > gateEnv ? a : gateEnv * envRel;
        if (! gateIsOpen && gateEnv > open) gateIsOpen = true;
        else if (gateIsOpen && gateEnv < close) gateIsOpen = false;
        const float target = gateIsOpen ? 1.0f : 0.0f;
        const float c = target > gateGain ? att : rel;
        gateGain = target + (gateGain - target) * c;
        x[i] *= gateGain;
    }
    if (gateGain < 1.0e-6f) gateGain = 0.0f;
    gateOpen.store (gateIsOpen, std::memory_order_relaxed);
}

void AmpRig::runDrive (float* x, int n) noexcept
{
    const float tone = getParam (driveTone);
    if (tone != lastDriveTone)
    {
        driveToneLp.set (Biquad::lowPass, sr * 2.0, 900.0 * std::pow (11.0, (double) tone), 0.707);
        lastDriveTone = tone;
    }
    const float g = 1.0f + 80.0f * std::pow (getParam (driveGain), 2.0f);
    const float comp = 1.0f / (0.7f + 0.3f * std::sqrt (g));
    float* ch[1] = { x };
    juce::dsp::AudioBlock<float> blk (ch, 1, (size_t) n);
    auto upBlk = driveOs.processSamplesUp (blk);
    float* u = upBlk.getChannelPointer (0);
    const int un = (int) upBlk.getNumSamples();
    for (int i = 0; i < un; ++i)
    {
        const float lo = u[i];
        const float hi = driveHp.process (lo);
        // soft clipping of the mid band, a little clean low end mixed back (keeps the bottom tight but present)
        const float y = std::tanh (hi * g) + 0.25f * (lo - hi);
        u[i] = driveToneLp.process (y);
    }
    driveOs.processSamplesDown (blk);
    driveG.setTargetValue (dbToGain (getParam (driveLevel)) * comp * 1.6f);
    driveG.applyGain (x, n);
}

void AmpRig::runAmp (float* x, int n) noexcept
{
    if (model != nullptr && modelActive.load (std::memory_order_relaxed))
    {
        calG.setTargetValue (getParam (namCalibrate) >= 0.5f ? model->calibrationGain (getParam (calLevel)) : 1.0f);
        calG.applyGain (x, n);
        model->process (x, n);
        normG.setTargetValue (getParam (namNormalize) >= 0.5f ? model->normalisationGain() : 1.0f);
        normG.applyGain (x, n);
    }
    else
    {
        // built-in amp: two soft-clipping triode-like stages with interstage filtering, 2x oversampled
        const float gain = getParam (ampGain);
        const float pre = std::pow (10.0f, (gain * 42.0f - 6.0f) / 20.0f);
        const float makeup = 0.9f / (0.6f + 0.4f * std::sqrt (pre));
        float* ch[1] = { x };
        juce::dsp::AudioBlock<float> blk (ch, 1, (size_t) n);
        auto upBlk = ampOs.processSamplesUp (blk);
        float* u = upBlk.getChannelPointer (0);
        const int un = (int) upBlk.getNumSamples();
        for (int i = 0; i < un; ++i)
        {
            float s = ampHp.process (u[i]) * pre;
            s = std::tanh (s + 0.15f) - 0.14888f;                 // asymmetric first stage (tanh(0.15))
            s = ampMid.process (s) * (1.0f + 6.0f * gain);
            s = std::tanh (s);
            u[i] = ampLp.process (s);
        }
        ampOs.processSamplesDown (blk);
        juce::FloatVectorOperations::multiply (x, makeup, n);
    }
    ampG.setTargetValue (dbToGain (getParam (ampLevel)));
    ampG.applyGain (x, n);
}

void AmpRig::runEq (float* x, int n) noexcept
{
    const std::array<float, 4> v { { getParam (bass), getParam (mid), getParam (treble), getParam (presence) } };
    if (v != lastEq)
    {
        eqBass.set (Biquad::lowShelf, sr, 120.0, 0.707, v[0]);
        eqMid.set (Biquad::peak, sr, 650.0, 0.7, v[1]);
        eqTreble.set (Biquad::highShelf, sr, 2800.0, 0.707, v[2]);
        eqPresence.set (Biquad::peak, sr, 5500.0, 0.9, v[3]);
        lastEq = v;
    }
    if (v[0] != 0.0f) eqBass.process (x, n);
    if (v[1] != 0.0f) eqMid.process (x, n);
    if (v[2] != 0.0f) eqTreble.process (x, n);
    if (v[3] != 0.0f) eqPresence.process (x, n);
}

void AmpRig::runCab (float* x, int n) noexcept
{
    if (irActive.load (std::memory_order_relaxed))
    {
        float* ch[1] = { x };
        juce::dsp::AudioBlock<float> blk (ch, 1, (size_t) n);
        const bool blend = irActive2.load (std::memory_order_relaxed);
        float* t2 = tmp.data();
        if (blend) std::copy (x, x + n, t2);
        convolution->process (juce::dsp::ProcessContextReplacing<float> (blk));
        if (getParam (cabPhase1) >= 0.5f) juce::FloatVectorOperations::negate (x, x, n);
        if (blend)
        {
            float* ch2[1] = { t2 };
            juce::dsp::AudioBlock<float> blk2 (ch2, 1, (size_t) n);
            convolution2->process (juce::dsp::ProcessContextReplacing<float> (blk2));
            const float b = juce::jlimit (0.0f, 1.0f, getParam (cabBlend));   // equal power
            const float ga = std::cos (b * juce::MathConstants<float>::halfPi), gb = std::sin (b * juce::MathConstants<float>::halfPi);
            const float gb2 = getParam (cabPhase2) >= 0.5f ? -gb : gb;
            for (int i = 0; i < n; ++i) x[i] = x[i] * ga + t2[i] * gb2;
        }
    }
    else
    {
        spkHp.process (x, n);
        spkPeak.process (x, n);
        spkLp1.process (x, n);
        spkLp2.process (x, n);
        if (getParam (cabPhase1) >= 0.5f) juce::FloatVectorOperations::negate (x, x, n);
    }
    const float lc = getParam (cabLowCut), hc = getParam (cabHighCut);
    if (lc != lastLowCut)
    {
        cabHp1.set (Biquad::highPass, sr, lc, 0.541);
        cabHp2.set (Biquad::highPass, sr, lc, 1.307);
        lastLowCut = lc;
    }
    if (hc != lastHighCut)
    {
        cabLp1.set (Biquad::lowPass, sr, juce::jmin ((double) hc, sr * 0.45), 0.541);
        cabLp2.set (Biquad::lowPass, sr, juce::jmin ((double) hc, sr * 0.45), 1.307);
        lastHighCut = hc;
    }
    if (lc > 20.5f) { cabHp1.process (x, n); cabHp2.process (x, n); }
    if (hc < 19999.0f && hc < sr * 0.45) { cabLp1.process (x, n); cabLp2.process (x, n); }
}

void AmpRig::runDelay (float* L, float* R, int n) noexcept
{
    const int size = (int) dlL.size();
    if (size < 8) return;
    dlTime.setTargetValue (getParam (delayTime));
    const float fb = getParam (delayFeedback);
    const float mix = getParam (delayMix);
    const float cutoff = 1200.0f * std::pow (12.0f, getParam (delayTone));
    const float lpA = std::exp (-2.0f * juce::MathConstants<float>::pi * cutoff / (float) sr);
    const float spread = getParam (delayStereo) >= 0.5f ? (float) (0.011 * sr) : 0.0f;   // the right repeats come 11 ms later: width without ping-pong

    auto read = [&] (const std::vector<float>& buf, float delaySamples) noexcept {
        float pos = (float) dlWrite - delaySamples;
        while (pos < 0.0f) pos += (float) size;
        const int i0 = (int) pos;
        const float f = pos - (float) i0;
        const int i1 = (i0 + 1) % size;
        return buf[(size_t) (i0 % size)] * (1.0f - f) + buf[(size_t) i1] * f;
    };

    for (int i = 0; i < n; ++i)
    {
        const float d = juce::jlimit (2.0f, (float) size - 4.0f - spread, dlTime.getNextValue() * 0.001f * (float) sr);
        const float wl = read (dlL, d);
        const float wr = read (dlR, d + spread);
        dlLpL = wl + (dlLpL - wl) * lpA;
        dlLpR = wr + (dlLpR - wr) * lpA;
        const float inL = L[i], inR = R[i];
        dlL[(size_t) dlWrite] = inL + dlLpL * fb;
        dlR[(size_t) dlWrite] = inR + dlLpR * fb;
        L[i] = inL + mix * wl;
        R[i] = inR + mix * wr;
        if (++dlWrite >= size) dlWrite = 0;
    }
    if (std::abs (dlLpL) < 1.0e-20f) dlLpL = 0;
    if (std::abs (dlLpR) < 1.0e-20f) dlLpR = 0;
}
} // namespace ff::fx
