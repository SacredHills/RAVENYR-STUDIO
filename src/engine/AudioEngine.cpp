#include "AudioEngine.h"
#include "core/DrumModel.h"
#include <cmath>

namespace ff
{
void LevelMeter::update (const float* data, int n, float gain) noexcept
{
    if (data == nullptr || n <= 0)
        return;
    float pk = 0.0f;
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (data[i]) * gain;
        pk = juce::jmax (pk, a);
        sum += (double) a * a;
    }
    if (pk > peak.load (std::memory_order_relaxed))
        peak.store (pk, std::memory_order_relaxed);
    rms.store ((float) std::sqrt (sum / n), std::memory_order_relaxed);
    if (pk >= 0.999f)
    {
        clipped.store (true, std::memory_order_relaxed);
        clipCount.fetch_add (1, std::memory_order_relaxed);
    }
}

AudioEngine::AudioEngine()
{
    // Sized once, never resized: up to ~1 s at 192 kHz, so the analysis thread can never
    // observe a reallocation while the audio thread is pushing.
    analysis.prepare (1 << 18);
    retired.prepare (1024);
    retiredTabs.prepare (256);
    retiredClickMaps.prepare (64);
    liveMidiIn.prepare (1024);
    pendingSamples.prepare (128);
    retiredSamples.prepare (256);
}

AudioEngine::~AudioEngine()
{
    workers.stop();
    delete pendingGraph.exchange (nullptr);
    delete pendingTab.exchange (nullptr);
    delete pendingClicks.exchange (nullptr);
    delete clickMap;
    collectGarbage();
    delete graph;
    delete tabSeq;
    SampleSwap sw;
    while (pendingSamples.pop (&sw, 1) == 1) delete sw.buffer;
    for (auto* b : drumSamples) delete b;
}

bool AudioEngine::setDrumSample (int piece, std::unique_ptr<juce::AudioBuffer<float>> sample)
{
    if (! juce::isPositiveAndBelow (piece, DrumSynth::maxPieces)) return true;
    SampleSwap sw { piece, sample.get() };
    if (pendingSamples.push (&sw, 1) != 1) return false;
    sample.release();
    return true;
}

void AudioEngine::setPrepareHook (std::function<void (double, int)> hook)
{
    const juce::ScopedLock sl (hookLock);
    prepareHook = std::move (hook);
}

void AudioEngine::waitForAudioCallback()
{
    const auto start = callbackCounter.load();
    for (int i = 0; i < 200 && deviceRunning.load() && callbackCounter.load() - start < 2; ++i)
        juce::Thread::sleep (2);
}

void AudioEngine::submitClickMap (std::unique_ptr<ClickList> clicks)
{
    if (clicks != nullptr && clicks->empty()) clicks.reset();
    // nullptr is a message too (back to the constant click): an empty list stands for it
    delete pendingClicks.exchange (clicks != nullptr ? clicks.release() : new ClickList());
}

void AudioEngine::setDrumInstrument (std::shared_ptr<fx::InstrumentSlot> slot)
{
    if (slot.get() == drumInstOwner.get()) return;
    drumInst.store (nullptr);
    waitForAudioCallback();
    drumInstOwner = std::move (slot);
    drumInst.store (drumInstOwner.get());
}

void AudioEngine::submitTabSequence (std::unique_ptr<TabSequence> seq)
{
    delete pendingTab.exchange (seq.release());
}

void AudioEngine::renderTab (float* const* bus, int numBus, int offset, int len) noexcept
{
    const double sr = sampleRate.load (std::memory_order_relaxed);
    if (numBus == 0 || sr <= 0.0)
        return;
    float* L = scratchL.data();
    float* R = scratchR.data();
    juce::FloatVectorOperations::clear (L, len);
    juce::FloatVectorOperations::clear (R, len);

    if (drumInstPanic)   // after a jump or stop: let go of every drum note, before this block's hits (the first one may be at the very start)
    {
        drumInstPanic = false;
        drumOffN = 0;
        drumEvN = 0;
        for (int n = 0; n < 128; ++n) if (drums::pieceForGmNote (n) >= 0) drumMidiAdd (0, 0x89, (juce::uint8) n, 0);
    }

    if (tabNeedsSeek && tabSeq != nullptr)
    {
        const double t = (double) pos / sr - 1.0e-9;
        const auto& notes = tabSeq->notes;
        tabNext = (size_t) (std::lower_bound (notes.begin(), notes.end(), t,
                                              [] (const SynthNote& n, double v) { return n.startSec < v; }) - notes.begin());
        const auto& hits = tabSeq->drumHits;
        drumNext = (size_t) (std::lower_bound (hits.begin(), hits.end(), t,
                                               [] (const DrumHit& h, double v) { return h.sec < v; }) - hits.begin());
        tabNeedsSeek = false;
    }

    if (tabNeedsSeekClicks && tabSeq != nullptr)
    {
        const double t = (double) pos / sr - 1.0e-9;
        const auto& c = tabSeq->clicks;
        tabNextClick = (size_t) (std::lower_bound (c.begin(), c.end(), t,
                                                   [] (const TabSequence::Click& k, double v) { return k.sec < v; }) - c.begin());
        tabNeedsSeekClicks = false;
    }

    // score clicks (follow meter and tempo changes) — rendered straight into the bus
    if (playing && tabSeq != nullptr && clickWhilePlaying.load())
    {
        float* out[2] = { bus[0] + offset, numBus > 1 ? bus[1] + offset : nullptr };
        const double t1 = (double) (pos + len) / sr;
        const auto& c = tabSeq->clicks;
        int ccur = 0;
        while (tabNextClick < c.size() && c[tabNextClick].sec < t1)
        {
            const int off = juce::jlimit (ccur, len, (int) (std::llround (c[tabNextClick].sec * sr) - pos));
            float* seg[2] = { out[0] + ccur, out[1] != nullptr ? out[1] + ccur : nullptr };
            metronome.renderTriggered (seg, numBus, off - ccur);
            ccur = off;
            metronome.triggerClick (c[tabNextClick].accent);
            ++tabNextClick;
        }
        float* seg[2] = { out[0] + ccur, out[1] != nullptr ? out[1] + ccur : nullptr };
        metronome.renderTriggered (seg, numBus, len - ccur);
    }
    else if (playing && tabSeq != nullptr)
    {
        const double t1 = (double) (pos + len) / sr;
        while (tabNextClick < tabSeq->clicks.size() && tabSeq->clicks[tabNextClick].sec < t1) ++tabNextClick;
    }

    int cursor = 0;
    if (playing && tabSeq != nullptr)
    {
        const double t1 = (double) (pos + len) / sr;
        const auto& notes = tabSeq->notes;
        while (tabNext < notes.size() && notes[tabNext].startSec < t1)
        {
            const int off = juce::jlimit (cursor, len, (int) (std::llround (notes[tabNext].startSec * sr) - pos));
            synth.render (L + cursor, R + cursor, off - cursor);
            cursor = off;
            synth.start (notes[tabNext]);
            ++tabNext;
        }
    }
    synth.render (L + cursor, R + cursor, len - cursor);

    // drums: take over new samples (the old buffer goes back to the message thread to be freed)
    {
        SampleSwap sw;
        while (pendingSamples.freeSpace() >= 0 && retiredSamples.freeSpace() > 0 && pendingSamples.pop (&sw, 1) == 1)
        {
            auto* old = drumSamples[(size_t) sw.piece];
            drumSamples[(size_t) sw.piece] = sw.buffer;
            drumSynth.setSample (sw.piece, sw.buffer);
            if (old != nullptr) retiredSamples.push (&old, 1);
        }
    }
    auto* inst = drumInst.load();
    const bool useInst = inst != nullptr && inst->activePlugin() != nullptr && ! inst->bypassed.load();
    if (const int pv = drumPreview.exchange (-1); pv >= 0)
    {
        if (useInst) drumMidiAdd (0, 0x99, (juce::uint8) drums::pieceInfo (pv & 0xff).gmNote, (juce::uint8) ((pv >> 8) & 0x7f));
        else drumSynth.trigger (pv & 0xff, (float) ((pv >> 8) & 0x7f) / 127.0f);
    }
    cursor = 0;
    if (tabSeq != nullptr) drumSynth.volume = tabSeq->drumVolume;
    if (playing && tabSeq != nullptr)
    {
        const double t1 = (double) (pos + len) / sr;
        const auto& hits = tabSeq->drumHits;
        while (drumNext < hits.size() && hits[drumNext].sec < t1)
        {
            const int off = juce::jlimit (cursor, len, (int) (std::llround (hits[drumNext].sec * sr) - pos));
            if (useInst)
            {
                const auto note = (juce::uint8) juce::jlimit (0, 127, drums::pieceInfo (hits[drumNext].piece).gmNote);
                drumMidiAdd (juce::jmin (off, len - 1), 0x99, note, (juce::uint8) juce::jlimit (1, 127, (int) (hits[drumNext].velocity * 127.0f)));
                if (drumOffN < (int) drumOffs.size()) drumOffs[(size_t) drumOffN++] = { pos + off + (juce::int64) (0.12 * sr), note };
            }
            else
            {
                drumSynth.render (L + cursor, R + cursor, off - cursor);
                cursor = off;
                drumSynth.trigger (hits[drumNext].piece, hits[drumNext].velocity);
            }
            ++drumNext;
        }
    }
    drumSynth.render (L + cursor, R + cursor, len - cursor);

    // the Drum Studio's plugin instrument: this block's hits (and the note-offs that fall due) as MIDI
    if (inst != nullptr)
    {
        if (! useInst) { drumEvN = 0; drumOffN = 0; }
        else
        {
            for (int i = 0; i < drumOffN;)
                if (drumOffs[(size_t) i].at < pos + len)
                {
                    drumMidiAdd ((int) juce::jlimit ((juce::int64) 0, (juce::int64) (len - 1), drumOffs[(size_t) i].at - pos), 0x89, drumOffs[(size_t) i].note, 0);
                    drumOffs[(size_t) i] = drumOffs[(size_t) --drumOffN];
                }
                else ++i;
            const float vol = tabSeq != nullptr ? tabSeq->drumVolume : 1.0f;
            const int chunk = 512;
            for (int s = 0; s < len; s += chunk)
            {
                const int n = juce::jmin (chunk, len - s);
                drumMidi.clear();
                for (int e = 0; e < drumEvN; ++e)
                {
                    const auto& ev = drumEv[(size_t) e];
                    if (ev.pos >= s && ev.pos < s + n)
                    {
                        const juce::uint8 data[3] = { ev.b0, ev.b1, ev.b2 };
                        drumMidi.addEvent (data, 3, ev.pos - s);
                    }
                }
                float* il = instL.data();
                float* ir = instR.data();
                juce::FloatVectorOperations::clear (il, n);
                juce::FloatVectorOperations::clear (ir, n);
                inst->render (il, ir, n, drumMidi);
                juce::FloatVectorOperations::addWithMultiply (L + s, il, vol, n);
                juce::FloatVectorOperations::addWithMultiply (R + s, ir, vol, n);
            }
            drumEvN = 0;
        }
    }

    juce::FloatVectorOperations::add (bus[0] + offset, L, len);
    juce::FloatVectorOperations::add ((numBus > 1 ? bus[1] : bus[0]) + offset, R, len);
}

void AudioEngine::setTimeSignature (int num, int den) noexcept
{
    metronome.beatsPerBar = juce::jlimit (1, MetronomeDsp::maxBeats, num);
    metronome.beatUnit = juce::jlimit (1, 32, den);
}

void AudioEngine::setRecording (bool shouldRecord, juce::int64 countInSamples) noexcept
{
    if (shouldRecord && ! playRequested.load())
    {
        lastPlayStart = positionSamples.load();
        countInRequest = juce::jmax ((juce::int64) 0, countInSamples);
        recordRequested = true;
        playRequested = true;
    }
    else
    {
        recordRequested = shouldRecord;
    }
}

void AudioEngine::submitGraph (std::unique_ptr<PlaybackGraph> g)
{
    delete pendingGraph.exchange (g.release()); // a snapshot never seen by the audio thread
}

void AudioEngine::collectGarbage()
{
    PlaybackGraph* old[64];
    int n;
    while ((n = retired.pop (old, 64)) > 0)
        for (int i = 0; i < n; ++i)
            delete old[i];
    juce::AudioBuffer<float>* oldSamples[32];
    while ((n = retiredSamples.pop (oldSamples, 32)) > 0)
        for (int i = 0; i < n; ++i)
            delete oldSamples[i];
    ClickList* oldClicks[32];
    while ((n = retiredClickMaps.pop (oldClicks, 32)) > 0)
        for (int i = 0; i < n; ++i)
            delete oldClicks[i];
    TabSequence* oldTabs[32];
    while ((n = retiredTabs.pop (oldTabs, 32)) > 0)
        for (int i = 0; i < n; ++i)
            delete oldTabs[i];
}

void AudioEngine::dropGraphWhileStopped()
{
    if (deviceRunning.load()) return;
    delete pendingGraph.exchange (nullptr);
    collectGarbage();
    liveGraph.store (nullptr);
    delete graph;
    graph = nullptr;
}

void AudioEngine::prepare (double sr, int maximumBlockSize)
{
    sampleRate = sr;
    blockSize = maximumBlockSize;
    metronome.prepare (sr);
    synth.prepare (sr);
    drumSynth.prepare (sr);
    instL.assign (512, 0.0f);
    instR.assign (512, 0.0f);
    drumMidi.ensureSize (4096);
    tabNeedsSeek = tabNeedsSeekClicks = mapNeedsSeek = true;
    smoothedGain.reset (sr, 0.02);
    smoothedGain.setCurrentAndTargetValue (masterGain.load());
    const int scratch = juce::jmax (maximumBlockSize, 4096);
    scratchL.assign ((size_t) scratch, 0.0f);
    scratchR.assign ((size_t) scratch, 0.0f);
    silence.assign ((size_t) scratch, 0.0f);
    liveL.assign ((size_t) scratch, 0.0f);
    liveR.assign ((size_t) scratch, 0.0f);
    mixer.prepare (scratch, sr);
    mixer.workers = &workers;
    if (workers.size() != threadCount.load() || std::abs (workersRate - sr) > 0.5 || workersBlock != maximumBlockSize)
    {
        workers.start (juce::jmax (0, threadCount.load()), sr, maximumBlockSize);
        workersRate = sr;
        workersBlock = maximumBlockSize;
    }
    playing = false;
    practiceRunning = false;
    playingState = false;
    countingIn = false;
    countInRemaining = 0;

    const juce::ScopedLock sl (hookLock);
    if (prepareHook) prepareHook (sr, maximumBlockSize);
}

double AudioEngine::beatsAt (juce::int64 samplePos) const noexcept
{
    const double sr = sampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0)
        return 0.0;
    return (double) samplePos / sr * metronome.bpm.load() / 60.0 * metronome.beatUnit.load() / 4.0;
}

void AudioEngine::closeRecordSegments() noexcept
{
    if (graph != nullptr)
        for (auto& t : graph->tracks)
            if (t.recorder != nullptr)
                t.recorder->closeSegment();
}

struct AudioEngine::Segment final : GraphMixer::Source
{
    AudioEngine& e;
    const float* const* inputs;
    int numInputs, offset;
    bool rolling, recordingNow;

    Segment (AudioEngine& en, const float* const* in, int nIn, int off, bool roll, bool rec)
        : e (en), inputs (in), numInputs (nIn), offset (off), rolling (roll), recordingNow (rec) {}

    const float* input (int ch) const noexcept
    {
        return (ch >= 0 && ch < numInputs && inputs[ch] != nullptr) ? inputs[ch] + offset : nullptr;
    }

    double masterTime() const noexcept override
    {
        const double sr = e.sampleRate.load (std::memory_order_relaxed);
        return rolling && sr > 0 ? (double) e.pos / sr : -1.0;
    }

    bool fill (int, PlayTrack& t, float* sL, float* sR, int len) noexcept override
    {
        auto& meter = e.trackMeters[(size_t) juce::jlimit (0, timeline::maxTracks - 1, t.meterSlot)];
        if (! t.lanes.empty() || t.instrument != nullptr)   // automation, and the MIDI of instrument tracks
        {
            BlockTiming bt;
            bt.from = e.pos + readAhead (t);
            bt.rolling = rolling;
            bt.looping = e.segLooping;
            bt.loopStart = e.segLoopStart;
            bt.loopEnd = e.segLoopEnd;
            const double sr = e.sampleRate.load (std::memory_order_relaxed);
            bt.timeSec = sr > 0 ? (double) e.pos / sr : 0.0;
            prepareTrackBlock (t, bt, len);
        }
        if (t.kind == timeline::TrackKind::instrument)
        {
            if (t.instrument != nullptr && offset == 0)   // MIDI keyboard (armed tracks) and the MIDI editor's preview
                for (int i = 0; i < e.liveMidiCount; ++i)
                {
                    const auto& lm = e.liveMidiBlock[(size_t) i];
                    if (lm.target == t.meterSlot || (lm.target < 0 && (t.armed || t.monitor)))
                        t.midiBuffer.addEvent (lm.b, lm.size, 0);
                    if (lm.target < 0 && t.armed && rolling && recordingNow && t.midiRecorder != nullptr)
                        t.midiRecorder->push (e.pos, lm.b, lm.size);
                }
            if (t.midiRecorder != nullptr && rolling && recordingNow)
                t.midiRecorder->lastPos.store (e.pos + len, std::memory_order_relaxed);
            if (! t.audible) meter.rms.store (0.0f, std::memory_order_relaxed);
            return t.audible;
        }
        if (t.kind != timeline::TrackKind::audio)
        {
            if (! t.audible) meter.rms.store (0.0f, std::memory_order_relaxed);
            return t.audible;
        }
        if (rolling)
        {
            // plugin delay compensation: read the clips ahead by the track's latency (wrapping at the loop end)
            const juce::int64 from = e.pos + readAhead (t);
            if (e.segLooping && from >= e.segLoopEnd)
                renderTrackClips (t, e.segLoopStart + (from - e.segLoopEnd) % juce::jmax ((juce::int64) 1, e.segLoopEnd - e.segLoopStart), len, sL, sR);
            else if (e.segLooping && from + len > e.segLoopEnd)
            {
                const int first = (int) (e.segLoopEnd - from);
                renderTrackClips (t, from, first, sL, sR);
                renderTrackClips (t, e.segLoopStart, len - first, sL + first, sR + first);
            }
            else
                renderTrackClips (t, from, len, sL, sR);
        }

        const float* inL = input (t.inputLeft);
        const float* inR = t.inputRight >= 0 ? input (t.inputRight) : inL;
        if (t.monitor && inL != nullptr)
        {
            juce::FloatVectorOperations::add (sL, inL, len);
            juce::FloatVectorOperations::add (sR, inR != nullptr ? inR : inL, len);
        }
        if (t.armed && ! t.monitor && inL != nullptr)   // an armed track's meter shows its input even when it is not heard
        {
            meter.update (inL, len);
            if (inR != nullptr && inR != inL) meter.update (inR, len);
        }
        if (recordingNow && t.recorder != nullptr && inL != nullptr)
            t.recorder->push (inL, t.recorder->numChannels > 1 ? inR : nullptr, len, e.pos);

        if (! t.audible)
        {
            meter.rms.store (0.0f, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void post (int, const PlayTrack& t, const float* L, const float* R, int len) noexcept override
    {
        auto& meter = e.trackMeters[(size_t) juce::jlimit (0, timeline::maxTracks - 1, t.meterSlot)];
        meter.update (L, len);
        meter.update (R, len);
    }

    void driveActivity (int slot, float db) noexcept override
    {
        auto& m = e.driveDb[(size_t) juce::jlimit (0, timeline::maxTracks - 1, slot)];
        m.store (db, std::memory_order_relaxed);
    }
};

void AudioEngine::publishTransport (bool rolling, bool recordingNow) noexcept
{
    const double sr = sampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0) return;
    const int den = juce::jlimit (1, 32, metronome.beatUnit.load());
    const double unitQ = 4.0 / (double) den;   // quarters per metronome beat
    double constBpm = metronome.rampEnabled.load() ? metronome.effectiveBpm.load() : metronome.bpm.load();
    // quarter position and tempo at a timeline sample: from the project's beat times when it has tempo changes
    auto at = [&] (juce::int64 samples, double& bpm) -> double {
        const double t = (double) samples / sr;
        bpm = constBpm;
        if (clickMap != nullptr && clickMap->size() >= 2 && t >= (*clickMap)[0].sec)
        {
            const auto& cm = *clickMap;
            auto it = std::upper_bound (cm.begin(), cm.end(), t, [] (double v, const TabSequence::Click& k) { return v < k.sec; });
            if (it != cm.end())
            {
                const size_t k = (size_t) (it - cm.begin()) - 1;
                const double span = cm[k + 1].sec - cm[k].sec;
                if (span > 1.0e-9)
                {
                    bpm = 60.0 * unitQ / span;
                    return ((double) k + (t - cm[k].sec) / span) * unitQ;
                }
            }
            else   // after the last beat: keep going at the last interval's tempo
            {
                const size_t k = cm.size() - 1;
                const double span = cm[k].sec - cm[k - 1].sec;
                if (span > 1.0e-9)
                {
                    bpm = 60.0 * unitQ / span;
                    return ((double) k + (t - cm[k].sec) / span) * unitQ;
                }
            }
        }
        return t * constBpm / 60.0;
    };
    fx::TransportPlayHead::Info i;
    double ignored = 0.0;
    i.playing = rolling;
    i.recording = recordingNow;
    i.looping = segLooping;
    i.samples = pos;
    i.sampleRate = sr;
    i.ppq = at (i.samples, i.bpm);
    i.tsNum = metronome.beatsPerBar.load();
    i.tsDen = den;
    i.loopStartPpq = at (segLoopStart, ignored);
    i.loopEndPpq = at (segLoopEnd, ignored);
    fx::transportPlayHead().update (i);
}

void AudioEngine::renderSegment (const float* const* inputs, int numInputs, float* const* bus, int numBus,
                                 int offset, int len, bool rolling, bool recordingNow) noexcept
{
    if (graph == nullptr || numBus == 0)
        return;
    float* sL = scratchL.data();
    float* sR = scratchR.data();
    juce::FloatVectorOperations::clear (sL, len);
    juce::FloatVectorOperations::clear (sR, len);
    publishTransport (rolling, recordingNow);
    Segment seg (*this, inputs, numInputs, offset, rolling, recordingNow);
    for (int ch = 0; ch < numHwOut; ++ch)
        hwSeg[(size_t) ch] = hwOut[(size_t) ch] != nullptr ? hwOut[(size_t) ch] + offset : nullptr;
    mixer.mix (*graph, seg, sL, sR, len, hwSeg.data(), numHwOut);
    juce::FloatVectorOperations::add (bus[0] + offset, sL, len);
    if (numBus > 1)
        juce::FloatVectorOperations::add (bus[1] + offset, sR, len);
    else
        juce::FloatVectorOperations::add (bus[0] + offset, sR, len);
}

void AudioEngine::process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                           int numSamples) noexcept
{
    const double sr = sampleRate.load (std::memory_order_relaxed);

    // ---- pick up a new arrangement snapshot ----
    if (auto* g = pendingGraph.exchange (nullptr))
    {
        // a rebuilt snapshot (e.g. while a fader moves) continues seamlessly: no re-triggered notes, no gain jumps
        if (graph != nullptr)
            for (auto& nt : g->tracks)
                for (const auto& ot : graph->tracks)
                    if (ot.key == nt.key && ot.instrument == nt.instrument) { nt.takeStateFrom (ot); break; }
        if (graph != nullptr && retired.push (&graph, 1) == 0)
        {
            // FIFO full (message thread stalled): leaking is safer than freeing here.
        }
        graph = g;
        liveGraph.store (g, std::memory_order_release);
    }
    if (auto* cm = pendingClicks.exchange (nullptr))
    {
        if (clickMap != nullptr && retiredClickMaps.push (&clickMap, 1) == 0) {}
        if (cm->empty()) { delete cm; clickMap = nullptr; } else clickMap = cm;
        mapNeedsSeek = true;
    }
    if (auto* t = pendingTab.exchange (nullptr))
    {
        if (tabSeq != nullptr && retiredTabs.push (&tabSeq, 1) == 0) {}
        tabSeq = t;
        tabNeedsSeek = tabNeedsSeekClicks = mapNeedsSeek = true;
    }
    const bool tab = tabMode.load();
    liveMidiCount = liveMidiIn.pop (liveMidiBlock.data(), (int) liveMidiBlock.size());

    // ---- transport commands ----
    const auto loc = locateRequest.exchange (-1);
    if (loc >= 0)
    {
        pos = loc;
        closeRecordSegments();
        tabNeedsSeek = tabNeedsSeekClicks = mapNeedsSeek = true;
        synth.allNotesOff();
        drumSynth.allNotesOff(); drumInstPanic = true;
        if (playing && countInRemaining == 0)
            metronome.resetAtBeat (beatsAt (pos));
    }

    const bool wantPlay = playRequested.load();
    if (wantPlay && ! playing)
    {
        playing = true;
        tabNeedsSeek = tabNeedsSeekClicks = mapNeedsSeek = true;
        countInRemaining = countInRequest.exchange (0);
        if (countInRemaining > 0)
            metronome.reset();
        else
            metronome.resetAtBeat (beatsAt (pos));
    }
    else if (! wantPlay && playing)
    {
        playing = false;
        countInRemaining = 0;
        synth.allNotesOff();
        drumSynth.allNotesOff(); drumInstPanic = true;
    }

    const bool practice = practiceMetronome.load();
    if (practice && ! practiceRunning && ! playing)
        metronome.reset();
    practiceRunning = practice;

    activeInputs.store (numInputs, std::memory_order_relaxed);
    activeOutputs.store (numOutputs, std::memory_order_relaxed);

    // ---- analysis tap & input meter ----
    const int ach = analysisInputChannel.load (std::memory_order_relaxed);
    if (ach >= 0 && ach < numInputs && inputs[ach] != nullptr)
    {
        inputMeter.update (inputs[ach], numSamples);
        analysis.push (inputs[ach], numSamples);
    }
    else
    {
        inputMeter.rms.store (0.0f, std::memory_order_relaxed);
    }

    // ---- output bus ----
    for (int i = 0; i < numOutputs; ++i)
        if (outputs[i] != nullptr)
            juce::FloatVectorOperations::clear (outputs[i], numSamples);

    // interface outputs by device channel (tracks / sends routed to "Out 3+4" etc.)
    numHwOut = 0;
    for (int ch = 0; ch < timeline::maxHardwareChannels; ++ch)
    {
        const int slot = haveOutputSlots ? outputSlot[(size_t) ch] : ch;
        hwOut[(size_t) ch] = juce::isPositiveAndBelow (slot, numOutputs) ? outputs[slot] : nullptr;
        if (hwOut[(size_t) ch] != nullptr) numHwOut = ch + 1;
    }

    float* bus[2] = { nullptr, nullptr };
    int numBus = 0;
    for (int i = 0; i < numOutputs && numBus < 2; ++i)
        if (outputs[i] != nullptr)
            bus[numBus++] = outputs[i];

    const bool recordWanted = recordRequested.load();
    const int scratchSize = (int) scratchL.size();

    int done = 0;
    while (done < numSamples)
    {
        int len = juce::jmin (numSamples - done, scratchSize);
        float* clickOut[2] = { numBus > 0 ? bus[0] + done : nullptr, numBus > 1 ? bus[1] + done : nullptr };

        if (playing && countInRemaining > 0)
        {
            len = (int) juce::jmin ((juce::int64) len, countInRemaining);
            renderSegment (inputs, numInputs, bus, numBus, done, len, false, false); // live monitoring only
            meterMix (bus, numBus, done, len);
            if (numBus > 0)
                metronome.process (clickOut, numBus, len);
            countInRemaining -= len;
            if (countInRemaining == 0)
                metronome.resetAtBeat (beatsAt (pos));
            done += len;
            continue;
        }

        const auto loopS = (juce::int64) (loopStartSec.load() * sr);
        const auto loopE = (juce::int64) (loopEndSec.load() * sr);
        const bool rangeValid = loopE > loopS + 64;
        const bool looping = playing && loopEnabled.load() && rangeValid && pos < loopE;
        const bool punch = punchEnabled.load() && rangeValid;
        if (looping)
            len = (int) juce::jmin ((juce::int64) len, loopE - pos);
        if (playing && punch)
        {
            if (pos < loopS && pos + len > loopS) len = (int) (loopS - pos);
            else if (pos < loopE && pos + len > loopE) len = (int) (loopE - pos);
        }

        const bool inPunch = ! punch || (pos >= loopS && pos < loopE);
        const bool recordingNow = playing && recordWanted && inPunch;
        if (wasRecording && ! recordingNow)
            closeRecordSegments();
        wasRecording = recordingNow;

        segLoopStart = loopS;
        segLoopEnd = loopE;
        segLooping = looping;
        renderSegment (inputs, numInputs, bus, numBus, done, len, playing && ! tab, recordingNow && ! tab);
        if (tab)
            renderTab (bus, numBus, done, len);
        meterMix (bus, numBus, done, len);   // the mix (after the master plugins), before the click is added

        if (clickMap != nullptr && playing && ! tab)
        {
            // the project has tempo changes: the click follows the beat times from the tempo map
            if (mapNeedsSeek)
            {
                const double t = (double) pos / sr - 1.0e-9;
                mapNextClick = (size_t) (std::lower_bound (clickMap->begin(), clickMap->end(), t,
                                                           [] (const TabSequence::Click& k, double v) { return k.sec < v; }) - clickMap->begin());
                mapNeedsSeek = false;
            }
            const double t1 = (double) (pos + len) / sr;
            int ccur = 0;
            const bool audible = numBus > 0 && clickWhilePlaying.load();
            while (mapNextClick < clickMap->size() && (*clickMap)[mapNextClick].sec < t1)
            {
                const auto& ck = (*clickMap)[mapNextClick];
                if (audible)
                {
                    const int off = juce::jlimit (ccur, len, (int) (std::llround (ck.sec * sr) - pos));
                    float* seg[2] = { clickOut[0] + ccur, clickOut[1] != nullptr ? clickOut[1] + ccur : nullptr };
                    metronome.renderTriggered (seg, numBus, off - ccur);
                    ccur = off;
                    metronome.triggerClick (ck.accent);
                }
                ++mapNextClick;
            }
            if (audible)
            {
                float* seg[2] = { clickOut[0] + ccur, clickOut[1] != nullptr ? clickOut[1] + ccur : nullptr };
                metronome.renderTriggered (seg, numBus, len - ccur);
            }
        }
        else if (numBus > 0 && ((playing && clickWhilePlaying.load() && ! tab) || (! playing && practice)))
            metronome.process (clickOut, numBus, len);

        if (playing)
        {
            pos += len;
            if (looping && pos >= loopE)
            {
                pos = loopS;
                closeRecordSegments();
                metronome.resetAtBeat (beatsAt (pos));
                tabNeedsSeek = tabNeedsSeekClicks = mapNeedsSeek = true;
                synth.allNotesOff();
        drumSynth.allNotesOff(); drumInstPanic = true;
            }
        }
        done += len;
    }

    // ---- live amp rig ----
    if (auto* lr = liveRig.load (std::memory_order_acquire); lr != nullptr && liveRigOn.load (std::memory_order_relaxed) && numBus > 0)
    {
        const int li = liveInput.load (std::memory_order_relaxed);
        const int ch = li >= 0 ? li : ach;
        if (ch >= 0 && ch < numInputs && inputs[ch] != nullptr)
        {
            for (int done = 0; done < numSamples;)
            {
                const int len = juce::jmin (numSamples - done, (int) liveL.size());
                std::copy (inputs[ch] + done, inputs[ch] + done + len, liveL.data());
                std::copy (inputs[ch] + done, inputs[ch] + done + len, liveR.data());
                lr->process (liveL.data(), liveR.data(), len);
                juce::FloatVectorOperations::add (bus[0] + done, liveL.data(), len);
                juce::FloatVectorOperations::add ((numBus > 1 ? bus[1] : bus[0]) + done, liveR.data(), len);
                done += len;
            }
        }
    }

    recordAck.store (playing && recordWanted, std::memory_order_relaxed);
    countingIn.store (playing && countInRemaining > 0, std::memory_order_relaxed);
    playingState.store (playing, std::memory_order_relaxed);

    // ---- master gain & meters ----
    smoothedGain.setTargetValue (juce::jlimit (0.0f, 2.0f, masterGain.load (std::memory_order_relaxed)));
    if (smoothedGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = smoothedGain.getNextValue();
            for (int c = 0; c < numBus; ++c)
                bus[c][i] *= g;
        }
    }
    else
    {
        const float g = smoothedGain.getTargetValue();
        for (int c = 0; c < numBus; ++c)
            juce::FloatVectorOperations::multiply (bus[c], g, numSamples);
    }


    positionSamples.store (pos, std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double tempo = metronome.rampEnabled.load() ? metronome.effectiveBpm.load() : metronome.bpm.load();
        positionPpq.store ((double) pos / sr * tempo / 60.0, std::memory_order_relaxed);
    }
    callbackCounter.fetch_add (1, std::memory_order_release);
}

void AudioEngine::meterMix (float* const* bus, int numBus, int offset, int len) noexcept
{
    // OUT / MAIN OUT meters: what the master bus sends out (the master plugins, e.g. a limiter, included; the click and
    // the live amp page are not part of it), at the main output level
    const float g = juce::jlimit (0.0f, 2.0f, masterGain.load (std::memory_order_relaxed));
    if (numBus > 0) outputMeterL.update (bus[0] + offset, len, g);
    if (numBus > 1) outputMeterR.update (bus[1] + offset, len, g);
    else if (numBus > 0) outputMeterR.update (bus[0] + offset, len, g);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs,
                                                    int numOutputs, int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    process (inputs, numInputs, outputs, numOutputs, numSamples);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    const auto active = device->getActiveOutputChannels();
    int slot = 0;
    for (int ch = 0; ch < timeline::maxHardwareChannels; ++ch)
        outputSlot[(size_t) ch] = active[ch] ? slot++ : -1;
    haveOutputSlots = true;
    workers.setWorkgroup (device->getWorkgroup());
    deviceRunning = true;
}

void AudioEngine::audioDeviceStopped()
{
    deviceRunning = false;
    playing = false;
    playingState = false;
    recordAck = false;
    closeRecordSegments();
}

void AudioEngine::audioDeviceError (const juce::String&)
{
    deviceErrorCounter.fetch_add (1);
}
} // namespace ff
