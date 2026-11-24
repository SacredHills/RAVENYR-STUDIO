#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "core/MetronomeDsp.h"
#include "core/SpscFifo.h"
#include "core/Timeline.h"
#include "PlaybackGraph.h"
#include "GraphMixer.h"
#include "TrackWorkers.h"
#include "TabSynth.h"
#include "fx/AmpRig.h"
#include "fx/InstrumentSlot.h"
#include <array>
#include <cstring>
#include <atomic>

namespace ff
{
/** Lock-free level meter. Audio thread accumulates peaks; UI reads and decays. */
struct LevelMeter
{
    std::atomic<float> peak { 0.0f };   // max |x| since the UI last took it
    std::atomic<float> rms { 0.0f };    // RMS of the latest block
    std::atomic<bool> clipped { false }; // sticky until cleared by the UI
    std::atomic<juce::uint32> clipCount { 0 };   // +1 for every block that clipped (meters light their clip LED for a while)

    void update (const float* data, int n, float gain = 1.0f) noexcept;
    void clear() noexcept { peak = 0.0f; rms = 0.0f; }
    float takePeak() noexcept { return peak.exchange (0.0f); }

    /** Message-thread only: one owner calls uiTake() once per frame, then any number of
        views read uiPeak. */
    void uiTake() noexcept { uiPeak = takePeak(); }
    float uiPeak = 0.0f;
};

/** The real-time audio engine. Owns no device; it is driven either by an
    AudioDeviceManager (as an AudioIODeviceCallback) or offline by tests via process().

    Real-time contract: process() performs no locking, allocation, IO or logging.
    The arrangement arrives as immutable PlaybackGraph snapshots (submitGraph); retired
    snapshots travel back through a lock-free FIFO and are freed by collectGarbage(). */
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    // ---- transport (any thread) ----
    void play() noexcept { if (! playRequested.load()) lastPlayStart = positionSamples.load(); playRequested = true; }
    /** Where the transport was when playback / recording last started (the "go back to where you were" key). */
    juce::int64 getLastPlayStart() const noexcept { return lastPlayStart.load(); }
    void stop() noexcept { playRequested = false; recordRequested = false; }
    void togglePlay() noexcept { if (playRequested.load()) stop(); else play(); }
    void returnToZero() noexcept { locate (0); }
    void locate (juce::int64 samples) noexcept { locateRequest = juce::jmax ((juce::int64) 0, samples); }
    void locateSeconds (double s) noexcept { locate ((juce::int64) std::llround (juce::jmax (0.0, s) * juce::jmax (1.0, sampleRate.load()))); }
    bool isPlaying() const noexcept { return playingState.load(); }
    bool isCountingIn() const noexcept { return countingIn.load(); }
    juce::int64 getPositionSamples() const noexcept { return positionSamples.load(); }
    double getPositionSeconds() const noexcept { const double sr = sampleRate.load(); return sr > 0 ? (double) positionSamples.load() / sr : 0.0; }
    double getPositionPpq() const noexcept { return positionPpq.load(); }
    double getSampleRate() const noexcept { return sampleRate.load(); }
    int getBlockSize() const noexcept { return blockSize.load(); }

    /** Project tempo (quarter-note BPM) and time signature. */
    void setTempo (double bpm) noexcept { metronome.bpm = juce::jlimit (10.0, 500.0, bpm); }
    void setTimeSignature (int num, int den) noexcept;

    // ---- loop / punch (seconds) ----
    std::atomic<bool> loopEnabled { false }, punchEnabled { false };
    std::atomic<double> loopStartSec { 0.0 }, loopEndSec { 8.0 };

    // ---- recording ----
    /** Starts/stops recording on armed tracks that have a recorder in the current graph.
        If the transport is stopped, starting also starts playback after an optional count-in. */
    void setRecording (bool shouldRecord, juce::int64 countInSamples = 0) noexcept;
    bool isRecordRequested() const noexcept { return recordRequested.load(); }
    /** True while the audio thread is writing (or about to write) recorded audio. */
    bool isRecordingActive() const noexcept { return recordAck.load(); }

    // ---- metronome ----
    MetronomeDsp metronome;
    std::atomic<bool> clickWhilePlaying { false }; // DAW click
    std::atomic<bool> practiceMetronome { false };   // free-running metronome when transport is stopped

    // ---- mix / IO ----
    std::atomic<float> masterGain { 1.0f };          // linear, 0..2
    std::atomic<int> analysisInputChannel { 0 };     // input feeding tuner & input meter
    LevelMeter inputMeter, outputMeterL, outputMeterR;
    std::array<LevelMeter, timeline::maxTracks> trackMeters;
    std::array<std::atomic<float>, timeline::maxTracks> driveDb {};   // per strip: how much its Drive knob changed the level this block (dB, <= 0)
    std::atomic<int> activeInputs { 0 }, activeOutputs { 0 };

    /** Mono audio from the analysis input channel for tuner / analysis threads. */
    SpscFifo<float>& analysisFifo() noexcept { return analysis; }

    // ---- arrangement ----
    /** Message thread: hands a new snapshot to the audio thread. */
    void submitGraph (std::unique_ptr<PlaybackGraph> graph);
    /** Message thread: the newest arrangement snapshot (submitted or playing), nullptr if none. Snapshots are only
        freed on the message thread, so it stays valid until the caller returns to the message loop. */
    const PlaybackGraph* latestGraph() const noexcept
    {
        if (auto* p = pendingGraph.load (std::memory_order_acquire)) return p;
        return liveGraph.load (std::memory_order_acquire);
    }
    /** Message thread: frees snapshots the audio thread has finished with. */
    void collectGarbage();
    /** Frees the playback snapshot (and the plugins its rigs hold) while no device is running (shutdown). */
    void dropGraphWhileStopped();

    // ---- tablature playback ----
    /** When true the engine plays the tab sequence (guitar synth) instead of the arrangement. */
    std::atomic<bool> tabMode { false };
    /** Hands a drum sample (at the engine's sample rate) to the audio thread for a kit piece; nullptr restores the
        built-in sound. Message thread only. Returns false if the hand-over queue is full (try again later). */
    bool setDrumSample (int piece, std::unique_ptr<juce::AudioBuffer<float>> sample);
    /** Auditions a drum piece on the next audio block (score mode only). Lock-free. */
    /** Per-piece drum mix (volume / pan) and meters; any thread. */
    DrumSynth& drums() noexcept { return drumSynth; }
    /** The plugin instrument that plays the Drum Studio's hits instead of the built-in kit (nullptr = built-in kit).
        Message thread; waits for the audio thread to let go of the old one. */
    void setDrumInstrument (std::shared_ptr<fx::InstrumentSlot> slot);
    void previewDrum (int piece, float velocity) noexcept { drumPreview.store ((piece & 0xff) | (juce::jlimit (1, 127, (int) (velocity * 127.0f)) << 8)); }
    void submitTabSequence (std::unique_ptr<TabSequence> seq);
    /** The DAW click when the project has tempo changes: the beat times (seconds) and which are bar starts. An empty list (or
        nullptr) returns to the plain constant-tempo click. */
    void submitClickMap (std::unique_ptr<std::vector<TabSequence::Click>> clicks);
    /** Starts playback after `countInSamples` of metronome count-in. */
    void playWithCountIn (juce::int64 countInSamples) noexcept { countInRequest = juce::jmax ((juce::int64) 0, countInSamples); playRequested = true; }

    // ---- live amp rig (Effects & Amps: input -> rig -> output, whatever the transport does) ----
    std::atomic<fx::AmpRig*> liveRig { nullptr };     // owned by the RigBank, which outlives its use here
    std::atomic<bool> liveRigOn { false };
    std::atomic<int> liveInput { -1 };                 // input channel, -1 = the analysis (tuner) input
    /** Called from prepare() (device start, no audio callback running) so rig owners can re-prepare. */
    void setPrepareHook (std::function<void (double, int)> hook);
    /** Message thread: returns once the audio thread has finished at least one callback after this call
        (or at once if no device is running), so a pointer it read before can no longer be in use. */
    void waitForAudioCallback();

    /** Helper threads that process tracks' rigs and plugins in parallel (applied at the next prepare). */
    std::atomic<int> threadCount { TrackWorkers::defaultThreadCount() };

    // ---- driving ----
    void prepare (double sampleRate, int maximumBlockSize);
    void process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept;

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs,
                                           int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

    std::atomic<juce::uint32> deviceErrorCounter { 0 };
    std::atomic<juce::uint32> callbackCounter { 0 };
    std::atomic<bool> deviceRunning { false };

private:
    std::atomic<juce::int64> lastPlayStart { 0 };
    std::atomic<bool> playRequested { false }, recordRequested { false }, recordAck { false };
    std::atomic<juce::int64> locateRequest { -1 }, countInRequest { 0 };
    std::atomic<bool> playingState { false }, countingIn { false };
    std::atomic<juce::int64> positionSamples { 0 };
    std::atomic<double> positionPpq { 0.0 };
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<int> blockSize { 0 };

    // audio-thread state
    bool playing = false, practiceRunning = false, wasRecording = false;
    juce::int64 pos = 0, countInRemaining = 0;
    juce::SmoothedValue<float> smoothedGain;
    std::vector<float> scratchL, scratchR, silence, liveL, liveR;
    juce::CriticalSection hookLock;
    std::function<void (double, int)> prepareHook;
    PlaybackGraph* graph = nullptr;

    std::atomic<PlaybackGraph*> pendingGraph { nullptr };
    std::atomic<PlaybackGraph*> liveGraph { nullptr };   // = graph, for latestGraph()
    std::atomic<TabSequence*> pendingTab { nullptr };
    TabSequence* tabSeq = nullptr;
    SpscFifo<TabSequence*> retiredTabs;
    using ClickList = std::vector<TabSequence::Click>;
    std::atomic<ClickList*> pendingClicks { nullptr };
    ClickList* clickMap = nullptr;
    SpscFifo<ClickList*> retiredClickMaps;
    size_t mapNextClick = 0;
    bool mapNeedsSeek = true;
    // live MIDI (keyboard) for armed instrument tracks
    struct LiveMidi { juce::uint8 b[3] {}; juce::uint8 size = 0; int target = -1; };
    SpscFifo<LiveMidi> liveMidiIn;
    std::array<LiveMidi, 256> liveMidiBlock;
    int liveMidiCount = 0;
public:
    /** A message from a MIDI keyboard: played by every armed / monitored instrument track, or only by the track
        whose meter slot (= index) is `targetTrack` (the MIDI editor auditioning notes). */
    void pushLiveMidi (const juce::MidiMessage& m, int targetTrack = -1) noexcept
    {
        if (m.getRawDataSize() > 3 || m.getRawDataSize() <= 0 || m.isSysEx()) return;
        LiveMidi lm;
        lm.size = (juce::uint8) m.getRawDataSize();
        lm.target = targetTrack;
        std::memcpy (lm.b, m.getRawData(), (size_t) lm.size);
        liveMidiIn.push (&lm, 1);
    }
private:
    GuitarSynth synth;
    DrumSynth drumSynth;
    std::atomic<int> drumPreview { -1 };
    std::shared_ptr<fx::InstrumentSlot> drumInstOwner;              // message thread
    std::atomic<fx::InstrumentSlot*> drumInst { nullptr };          // audio thread reads
    struct DrumMidiEv { int pos; juce::uint8 b0, b1, b2; };
    std::array<DrumMidiEv, 512> drumEv {};
    int drumEvN = 0;
    struct DrumOff { juce::int64 at; juce::uint8 note; };
    std::array<DrumOff, 128> drumOffs {};
    int drumOffN = 0;
    juce::MidiBuffer drumMidi;
    std::vector<float> instL, instR;
    bool drumInstPanic = false;
    void drumMidiAdd (int pos, juce::uint8 b0, juce::uint8 b1, juce::uint8 b2) noexcept
    {
        if (drumEvN < (int) drumEv.size()) drumEv[(size_t) drumEvN++] = { pos, b0, b1, b2 };
    }
    struct SampleSwap { int piece = 0; juce::AudioBuffer<float>* buffer = nullptr; };
    SpscFifo<SampleSwap> pendingSamples;
    SpscFifo<juce::AudioBuffer<float>*> retiredSamples;
    std::array<juce::AudioBuffer<float>*, DrumSynth::maxPieces> drumSamples {};
    size_t tabNext = 0, tabNextClick = 0, drumNext = 0;
    bool tabNeedsSeek = true, tabNeedsSeekClicks = true;
    void renderTab (float* const* bus, int numBus, int offset, int len) noexcept;
    SpscFifo<PlaybackGraph*> retired;
    SpscFifo<float> analysis;

    double beatsAt (juce::int64 samplePos) const noexcept;
    juce::int64 segLoopStart = 0, segLoopEnd = 0;   // loop range of the segment being rendered (latency read-ahead wraps)
    bool segLooping = false;
    void closeRecordSegments() noexcept;
    void meterMix (float* const* bus, int numBus, int offset, int len) noexcept;
    void renderSegment (const float* const* inputs, int numInputs, float* const* bus, int numBus,
                        int offset, int len, bool rolling, bool recordingNow) noexcept;
    /** Tells the plugins where the transport is (see fx::TransportPlayHead). */
    void publishTransport (bool rolling, bool recordingNow) noexcept;
    // interface outputs: device output channel -> index in the callback's (active-only) output array, set when the
    // device starts (-1 = channel off); without a device (tests) channel n is output n. hwOut: this callback's buffers.
    std::array<int, timeline::maxHardwareChannels> outputSlot {};
    bool haveOutputSlots = false;
    std::array<float*, timeline::maxHardwareChannels> hwOut {}, hwSeg {};
    int numHwOut = 0;
    GraphMixer mixer;
    TrackWorkers workers;
    double workersRate = 0.0;
    int workersBlock = -1;
    struct Segment;   // GraphMixer::Source for renderSegment
};
} // namespace ff
