#include "GraphMixer.h"
#include "core/Timeline.h"
#include "ext/HostApi.h"

namespace ff
{
namespace
{
constexpr float silenceThreshold = 1.0e-5f;   // -100 dBFS

float peakOf (const float* L, const float* R, int n) noexcept
{
    const auto a = juce::FloatVectorOperations::findMinAndMax (L, n);
    const auto b = juce::FloatVectorOperations::findMinAndMax (R, n);
    return juce::jmax (std::abs (a.getStart()), std::abs (a.getEnd()), std::abs (b.getStart()), std::abs (b.getEnd()));
}

void addScaled (float* dst, const float* src, float g, int n) noexcept
{
    if (g == 0.0f) return;
    juce::FloatVectorOperations::addWithMultiply (dst, src, g, n);
}
}

void GraphMixer::prepare (int maxBlock_, double sampleRate)
{
    blockSize = juce::jmax (1, maxBlock_);
    sr = sampleRate > 0 ? sampleRate : 48000.0;
    bufL.assign ((size_t) timeline::maxTracks, std::vector<float> ((size_t) blockSize, 0.0f));
    bufR.assign ((size_t) timeline::maxTracks, std::vector<float> ((size_t) blockSize, 0.0f));
    instL.assign ((size_t) timeline::maxTracks, std::vector<float> ((size_t) blockSize, 0.0f));
    instR.assign ((size_t) timeline::maxTracks, std::vector<float> ((size_t) blockSize, 0.0f));
    tmpL.assign ((size_t) blockSize, 0.0f);
    tmpR.assign ((size_t) blockSize, 0.0f);
    active.assign ((size_t) timeline::maxTracks, 0);
    scFlag.assign ((size_t) timeline::maxTracks, 0);
    scBufL.assign ((size_t) timeline::maxTracks, std::vector<float> ((size_t) blockSize, 0.0f));
    scBufR.assign ((size_t) timeline::maxTracks, std::vector<float> ((size_t) blockSize, 0.0f));
    stage.reserve ((size_t) timeline::maxTracks);
}

void GraphMixer::updateLatencies (PlaybackGraph& g) noexcept
{
    // buses come after their sources in `order`, so walk it backwards
    const bool hasOrder = g.order.size() == g.tracks.size();
    for (int k = (int) g.tracks.size(); --k >= 0;)
    {
        auto& t = g.tracks[(size_t) (hasOrder ? g.order[(size_t) k] : k)];
        t.downstreamLatency = g.masterInserts != nullptr ? g.masterInserts->latency() : 0;
        if (juce::isPositiveAndBelow (t.output, (int) g.tracks.size()))
        {
            const auto& bus = g.tracks[(size_t) t.output];
            t.downstreamLatency = trackLatency (bus) + bus.downstreamLatency;   // the bus's own includes the master
        }
    }
}

/** A track that is not running (muted, asleep): its CPU readings drop to zero instead of showing the last value. */
static void clearLoads (PlayTrack& t) noexcept
{
    if (t.rig != nullptr) t.rig->cpuLoad.store (0.0f, std::memory_order_relaxed);
    if (t.inserts != nullptr) t.inserts->clearLoad();
    if (t.instrument != nullptr) t.instrument->cpuLoad.store (0.0f, std::memory_order_relaxed);
}

static bool preDrive (const PlayTrack& t) noexcept   // this strip's Drive runs before its plugins
{
    return t.drivePre && t.instrument == nullptr && (t.rig != nullptr || t.inserts != nullptr);
}

void GraphMixer::processTrack (PlayTrack& t, float* L, float* R, int n) noexcept
{
    // plugins are told the song position of the audio they receive: playback reads this track's clips (and MIDI)
    // readAhead() early for delay compensation
    const fx::CurrentTrackScope trackScope (t.key);
    const juce::int64 ahead = readAhead (t);
    if (t.instrument != nullptr)   // instrument tracks: the synth plays its MIDI, then the plugins (never idle)
    {
        {
            const fx::PlayHeadOffset at (ahead);
            t.instrument->render (L, R, n, t.midiBuffer, t.instInL, t.instInR);
        }
        if (t.inserts != nullptr)
        {
            const auto* ip = t.instrument->activePlugin();
            t.inserts->process (L, R, n, t.scL.data(), t.scR.data(), ahead - (ip != nullptr ? juce::jmax (0, ip->getLatencySamples()) : 0));
        }
        return;
    }
    if (t.rig == nullptr && t.inserts == nullptr) return;
    auto& idle = t.idle;
    if (hostapi::wakeRequested (t.key))   // a plugin plays audio of its own (host API): keep it running
    {
        if (idle.sleeping) idle = {};
        idle.quietOut = 0;
    }
    if (peakOf (L, R, n) > silenceThreshold) { idle.silentIn = 0; idle.sleeping = false; }
    else idle.silentIn += n;
    if (allowIdle && idle.sleeping)
    {
        juce::FloatVectorOperations::clear (L, n);
        juce::FloatVectorOperations::clear (R, n);
        clearLoads (t);
        return;
    }
    if (t.rig != nullptr)
    {
        const fx::PlayHeadOffset at (ahead);
        t.rig->process (L, R, n);
    }
    if (t.inserts != nullptr) t.inserts->process (L, R, n, t.scL.data(), t.scR.data(), ahead - (t.rig != nullptr ? t.rig->totalLatency() : 0));
    if (peakOf (L, R, n) > silenceThreshold) idle.quietOut = 0;
    else idle.quietOut += n;
    // nothing coming in and the tails (delay, reverb...) have died away: stop running the effects until sound arrives
    if (idle.silentIn > (juce::int64) (0.25 * sr) && idle.quietOut > (juce::int64) (3.0 * sr))
        idle.sleeping = true;
}

void GraphMixer::processJob (void* ctx, int i) noexcept
{
    auto& c = *static_cast<JobContext*> (ctx);
    auto& self = *c.self;
    const int ti = self.stage[(size_t) i];
    self.processTrack (c.graph->tracks[(size_t) ti], self.bufL[(size_t) ti].data(), self.bufR[(size_t) ti].data(), c.n);
}

void GraphMixer::mix (PlaybackGraph& g, Source& source, float* outL, float* outR, int n, float* const* hwOut, int numHw) noexcept
{
    // an interface output pair: its left channel and its right (the left again if the right is off)
    auto hwPair = [hwOut, numHw] (int ch, float*& l, float*& r) {
        l = r = nullptr;
        if (hwOut == nullptr || ! juce::isPositiveAndBelow (ch, numHw) || hwOut[ch] == nullptr) return false;
        l = hwOut[ch];
        r = ch + 1 < numHw && hwOut[ch + 1] != nullptr ? hwOut[ch + 1] : hwOut[ch];
        return true;
    };
    const int numTracks = juce::jmin ((int) g.tracks.size(), (int) bufL.size());
    if (numTracks == 0 || n <= 0 || n > blockSize) return;
    updateLatencies (g);

    for (int i = 0; i < numTracks; ++i)
        if (g.tracks[(size_t) i].instInput)
        {
            g.tracks[(size_t) i].instInL = instL[(size_t) i].data();
            g.tracks[(size_t) i].instInR = instR[(size_t) i].data();
            juce::FloatVectorOperations::clear (instL[(size_t) i].data(), n);
            juce::FloatVectorOperations::clear (instR[(size_t) i].data(), n);
        }
        else { g.tracks[(size_t) i].instInL = g.tracks[(size_t) i].instInR = nullptr; }
    for (int i = 0; i < numTracks; ++i)
        if (! timeline::isSourceKind (g.tracks[(size_t) i].kind))
        {
            juce::FloatVectorOperations::clear (bufL[(size_t) i].data(), n);
            juce::FloatVectorOperations::clear (bufR[(size_t) i].data(), n);
        }

    // side-chains: which tracks are read by some plugin
    std::fill (scFlag.begin(), scFlag.end(), (char) 0);
    for (auto& t : g.tracks)
        for (auto& sc : t.sidechains)
            if (juce::isPositiveAndBelow (sc.second, numTracks)) scFlag[(size_t) sc.second] = 1;
    for (auto& sc : g.masterSidechains)
        if (juce::isPositiveAndBelow (sc.second, numTracks)) scFlag[(size_t) sc.second] = 1;
    // a graph built by hand (tests) may have no order: then it is the track order
    const bool hasOrder = g.order.size() == g.tracks.size();
    auto orderAt = [&] (int k) { return hasOrder ? g.order[(size_t) k] : k; };
    const int numOrder = numTracks;
    for (int p = 0; p < numOrder;)
    {
        const int level = g.tracks[(size_t) orderAt (p)].level;
        int end = p;
        while (end < numOrder && g.tracks[(size_t) orderAt (end)].level == level) ++end;

        // 1. sources (serial: clips, inputs, recording)
        stage.clear();
        for (int k = p; k < end; ++k)
        {
            const int ti = orderAt (k);
            if (ti >= numTracks) continue;
            auto& t = g.tracks[(size_t) ti];
            float* L = bufL[(size_t) ti].data();
            float* R = bufR[(size_t) ti].data();
            if (timeline::isSourceKind (t.kind))
            {
                juce::FloatVectorOperations::clear (L, n);
                juce::FloatVectorOperations::clear (R, n);
            }
            active[(size_t) ti] = (source.fill (ti, t, L, R, n) || t.instInput) ? 1 : 0;
            if (! active[(size_t) ti]) clearLoads (t);
            if (active[(size_t) ti] && (t.rig != nullptr || t.inserts != nullptr || t.instrument != nullptr)) stage.push_back (ti);
        }

        // 2. effects (parallel); first point each plugin's side-chain at the signal of the track that feeds it
        for (int k = p; k < end; ++k)
        {
            auto& t = g.tracks[(size_t) orderAt (k)];
            t.scL.fill (nullptr);
            t.scR.fill (nullptr);
            for (auto& sc : t.sidechains)
                if (juce::isPositiveAndBelow (sc.second, numTracks))
                {
                    t.scL[(size_t) sc.first] = scBufL[(size_t) sc.second].data();
                    t.scR[(size_t) sc.first] = scBufR[(size_t) sc.second].data();
                }
        }
        // Drive set to "pre": before the plugins (instruments make their sound inside, so they stay post)
        for (int i : stage)
        {
            auto& t = g.tracks[(size_t) i];
            if (preDrive (t))
                source.driveActivity (t.meterSlot, t.saturator.process (bufL[(size_t) i].data(), bufR[(size_t) i].data(), n, t.drive, t.driveOs, sr));
        }
        JobContext ctx { this, &g, n };
        if (workers != nullptr) workers->run ((int) stage.size(), &GraphMixer::processJob, &ctx);
        else for (int i = 0; i < (int) stage.size(); ++i) processJob (&ctx, i);

        // keep the signal of the tracks that feed a side-chain (after their plugins, before fader / pan)
        for (int k = p; k < end; ++k)
        {
            const int ti = orderAt (k);
            if (ti >= numTracks || ! scFlag[(size_t) ti]) continue;
            if (active[(size_t) ti])
            {
                std::copy (bufL[(size_t) ti].data(), bufL[(size_t) ti].data() + n, scBufL[(size_t) ti].data());
                std::copy (bufR[(size_t) ti].data(), bufR[(size_t) ti].data() + n, scBufR[(size_t) ti].data());
            }
            else
            {
                juce::FloatVectorOperations::clear (scBufL[(size_t) ti].data(), n);
                juce::FloatVectorOperations::clear (scBufR[(size_t) ti].data(), n);
            }
        }

        // 3. sends, fader / pan, output (serial)
        for (int k = p; k < end; ++k)
        {
            const int ti = orderAt (k);
            if (ti >= numTracks || ! active[(size_t) ti]) continue;
            auto& t = g.tracks[(size_t) ti];
            float* L = bufL[(size_t) ti].data();
            float* R = bufR[(size_t) ti].data();
            for (const auto& ax : t.auxOuts)   // the instrument's extra outputs go to their tracks (before the fader)
            {
                const float *al, *ar;
                if (! juce::isPositiveAndBelow (ax.target, numTracks) || t.instrument == nullptr || ! t.instrument->auxOutput (ax.bus, al, ar)) continue;
                juce::FloatVectorOperations::add (bufL[(size_t) ax.target].data(), al, n);
                juce::FloatVectorOperations::add (bufR[(size_t) ax.target].data(), ar, n);
            }
            // the strip's Drive knob: saturation after the plugins, before the sends and the fader
            if (! preDrive (t))
                source.driveActivity (t.meterSlot, t.saturator.process (L, R, n, t.drive, t.driveOs, sr));
            for (const auto& s : t.sends)
            {
                if (! juce::isPositiveAndBelow (s.target, numTracks)) continue;
                auto& dstL = g.tracks[(size_t) s.target].instInput ? instL : bufL;   // an instrument takes it as audio input
                auto& dstR = g.tracks[(size_t) s.target].instInput ? instR : bufR;
                addScaled (dstL[(size_t) s.target].data(), L, s.gain * (s.pre ? 1.0f : t.gainL), n);
                addScaled (dstR[(size_t) s.target].data(), R, s.gain * (s.pre ? 1.0f : t.gainR), n);
            }
            for (const auto& s : t.hwSends)
            {
                float *hl, *hr;
                if (! hwPair (s.target, hl, hr)) continue;
                addScaled (hl, L, s.gain * (s.pre ? 1.0f : t.gainL), n);
                addScaled (hr, R, s.gain * (s.pre ? 1.0f : t.gainR), n);
            }
            // fader / pan (ramped when automation moved them since the last block)
            auto applyGain = [n] (float* x, float from, float to) {
                if (from < 0.0f || std::abs (from - to) < 1.0e-6f) { juce::FloatVectorOperations::multiply (x, to, n); return; }
                const float step = (to - from) / (float) n;
                for (int i = 0; i < n; ++i) x[i] *= from + step * (float) (i + 1);
            };
            applyGain (L, t.prevGainL, t.gainL);
            applyGain (R, t.prevGainR, t.gainR);
            t.prevGainL = t.gainL;
            t.prevGainR = t.gainR;
            source.post (ti, t, L, R, n);
            if (t.outputMuted) continue;
            float* dL = outL;
            float* dR = outR;
            if (juce::isPositiveAndBelow (t.output, numTracks)) { dL = bufL[(size_t) t.output].data(); dR = bufR[(size_t) t.output].data(); }
            else if (float *hl, *hr; t.hwOutput >= 0 && hwPair (t.hwOutput, hl, hr)) { dL = hl; dR = hr; }
            juce::FloatVectorOperations::add (dL, L, n);
            juce::FloatVectorOperations::add (dR, R, n);
        }
        p = end;
    }
    float autoGain = 1.0f, autoPan = 0.0f;   // master automation: plugin parameters now, the volume after the plugins
    if (! g.masterLanes.empty())
    {
        const double mt = source.masterTime();
        if (mt >= 0.0) autoGain = applyMasterLanes (g, mt, &autoPan);
    }
    if (g.masterInserts != nullptr)   // outL / outR started out silent, so this is the whole mix
    {
        g.masterScL.fill (nullptr);
        g.masterScR.fill (nullptr);
        for (auto& sc : g.masterSidechains)
            if (juce::isPositiveAndBelow (sc.second, numTracks))
            {
                g.masterScL[(size_t) sc.first] = scBufL[(size_t) sc.second].data();
                g.masterScR[(size_t) sc.first] = scBufR[(size_t) sc.second].data();
            }
        // every track is read ahead by the master plugins' latency too, so the master's input is that far ahead
        g.masterInserts->process (outL, outR, n, g.masterScL.data(), g.masterScR.data(), g.masterInserts->latency());
    }
    if (! g.masterLanes.empty())
    {
        const float from = g.masterPrevGain < 0.0f ? autoGain : g.masterPrevGain;   // ramped, so moving automation does not click
        const float panFrom = g.masterPrevPan < -1.0f ? autoPan : g.masterPrevPan;
        if (std::abs (from - autoGain) < 1.0e-6f && std::abs (panFrom - autoPan) < 1.0e-6f)
        {
            float gl, gr;
            panGains (autoPan, autoGain, gl, gr);
            if (std::abs (gl - 1.0f) > 1.0e-6f) juce::FloatVectorOperations::multiply (outL, gl, n);
            if (std::abs (gr - 1.0f) > 1.0e-6f) juce::FloatVectorOperations::multiply (outR, gr, n);
        }
        else
        {
            const float step = (autoGain - from) / (float) n, panStep = (autoPan - panFrom) / (float) n;
            for (int i = 0; i < n; ++i)
            {
                float gl, gr;
                panGains (panFrom + panStep * (float) (i + 1), from + step * (float) (i + 1), gl, gr);
                outL[i] *= gl;
                outR[i] *= gr;
            }
        }
        g.masterPrevPan = autoPan;
        g.masterPrevGain = autoGain;
    }
}
} // namespace ff
