#pragma once
#include "core/Timeline.h"
#include <juce_data_structures/juce_data_structures.h>
#include "AudioCache.h"
#include "Recorder.h"
#include "fx/AmpRig.h"
#include "fx/Saturator.h"
#include "fx/InsertChain.h"
#include "fx/InstrumentSlot.h"
#include "core/Automation.h"
#include <map>
#include <array>

namespace ff
{
/** One audible region of a clip, in engine samples. */
struct PlayPiece
{
    std::shared_ptr<const CachedAudio> audio;
    juce::int64 start = 0, end = 0;          // timeline samples [start, end)
    double srcAtStart = 0.0, step = 1.0;     // source sample position at `start`, source samples per engine sample
    float gain = 1.0f;
    juce::int64 fadeInStart = 0, fadeInEnd = 0, fadeOutStart = 0, fadeOutEnd = 0;
};

struct PlayTrack
{
    std::vector<PlayPiece> pieces;           // sorted by start
    float gainL = 1.0f, gainR = 1.0f;        // fader + pan
    bool audible = true;                     // after mute / solo
    bool monitor = false, armed = false;
    bool monoPlayback = false;               // mono audio track: stereo clips are summed to one channel
    int inputLeft = -1, inputRight = -1;     // indices into the device's active input channels
    int meterSlot = 0;
    std::shared_ptr<TrackRecorder> recorder; // only while recording
    std::shared_ptr<MidiRecorder> midiRecorder;   // instrument tracks, only while recording
    std::shared_ptr<fx::AmpRig> rig;         // amp rig insert (Effects & Amps), applied before fader / pan
    std::shared_ptr<fx::InsertChain> inserts; // plugin inserts, after the rig
    std::vector<std::pair<int, int>> sidechains;   // (insert physical slot, index of the track feeding its side-chain)
    std::array<const float*, fx::InsertChain::maxSlots> scL {}, scR {};   // audio thread: this block's side-chain audio per slot

    // routing
    timeline::TrackKind kind = timeline::TrackKind::audio;
    juce::int64 key = 0;                      // hash of the track id: carries audio-thread state into the next snapshot
    int output = -1;                          // index of the bus track this one plays into, -1 = master
    struct Send { int target = -1; float gain = 0.0f; bool pre = false; };
    std::vector<Send> sends;                  // into FX / bus tracks
    struct AuxOut { int target = -1; int bus = 1; };
    std::vector<AuxOut> auxOuts;              // instrument tracks: extra output buses that play into bus tracks (before the fader)
    bool instInput = false;                   // instrument tracks: other tracks send into the instrument's audio input
    const float *instInL = nullptr, *instInR = nullptr;   // audio thread: this block's input for the instrument
    int hwOutput = -1;                        // >= 0: plays to this device output channel (and the next) instead of the master
    std::vector<Send> hwSends;                // sends to interface outputs (target = device output channel of the left side)
    int level = 0;                            // processing stage: a track's sources all have lower levels
    bool outputMuted = false;                 // solo: the track feeds a soloed bus / FX return but is not heard directly

    // instrument tracks
    std::shared_ptr<fx::InstrumentSlot> instrument;
    struct MidiEvent { juce::int64 pos; juce::uint8 b0, b1, b2; juce::int64 end = 0; };   // end: a note-on's note-off position
    std::vector<MidiEvent> midiEvents;        // from the MIDI clips, timeline samples, sorted
    juce::MidiBuffer midiBuffer;              // this block's events (space reserved at build)

    // automation
    struct Lane
    {
        enum Target { volume, pan, plugin } target = volume;
        int slot = -1, param = -1;            // plugin: insert physical slot and parameter index
        std::vector<automation::Point> points;
        float last = -1.0f;                   // audio thread: last value sent to the plugin
    };
    std::vector<Lane> lanes;
    float baseGainDb = 0.0f, basePan = 0.0f;  // fader / pan without automation

    // audio-thread state
    struct Idle { juce::int64 silentIn = 0, quietOut = 0; bool sleeping = false; } idle;   // skip effects that have nothing to do
    int downstreamLatency = 0;                // latency of the buses this track plays through
    juce::int64 midiNextPos = -1;             // where the last rolling block ended (a jump silences held notes)
    bool midiWasRolling = false;
    float prevGainL = -1.0f, prevGainR = -1.0f;   // fader ramp across blocks (automation)
    float drive = 0.0f;                           // the strip's Drive knob, 0..1
    int driveOs = 1;                              // its oversampling: 0 off, 1 = 2x, 2 = 4x
    fx::Saturator saturator;                      // its state (kept across snapshots)
    /** Same track in the previous snapshot: keep playing where it left off (held notes, fader ramps). */
    void takeStateFrom (const PlayTrack& o) noexcept
    {
        idle = o.idle; midiNextPos = o.midiNextPos; midiWasRolling = o.midiWasRolling; prevGainL = o.prevGainL; prevGainR = o.prevGainR; saturator = o.saturator;
    }
    void resetState() noexcept { idle = {}; midiNextPos = -1; midiWasRolling = false; prevGainL = prevGainR = -1.0f; for (auto& l : lanes) l.last = -1.0f; }
};

/** Immutable snapshot of everything the audio thread needs to play the arrangement.
    Built on the message thread, swapped in atomically, destroyed on the message thread. */
struct PlaybackGraph
{
    std::vector<PlayTrack> tracks;
    std::vector<int> order;
    std::shared_ptr<fx::InsertChain> masterInserts;   // plugins on the master bus (after every track)                  // track indices sorted by level (sources before the buses they feed)
    std::vector<std::pair<int, int>> masterSidechains;   // like PlayTrack::sidechains, for the master plugins
    std::array<const float*, fx::InsertChain::maxSlots> masterScL {}, masterScR {};
    std::vector<PlayTrack::Lane> masterLanes;         // automation of the master bus: its volume and its plugins' parameters
    float masterPrevPan = -2.0f;                      // audio thread: the master pan automation of the last block (< -1: none yet)
    float masterDrive = 0.0f;
    int masterDriveOs = 1;                         // the master's Drive knob, 0..1
    fx::Saturator masterSaturator;
    float masterPrevGain = -1.0f;                     // audio thread: the master volume automation of the last block (-1: none yet)
    double sampleRate = 48000.0;
    int missingClips = 0; // clips whose audio is not loaded (yet)
};

struct GraphBuildOptions
{
    double sampleRate = 48000.0;
    bool waitForAudio = false;   // export: load synchronously instead of skipping unloaded clips
    const std::map<juce::String, std::shared_ptr<TrackRecorder>>* recorders = nullptr; // by track id
    const std::map<juce::String, std::shared_ptr<MidiRecorder>>* midiRecorders = nullptr; // by track id
    std::function<std::shared_ptr<fx::AmpRig> (const juce::ValueTree& track)> rigFor;    // amp rig per track (optional)
    std::function<std::shared_ptr<fx::InsertChain> (const juce::ValueTree& track)> insertsFor;
    std::shared_ptr<fx::InsertChain> masterInserts;
    juce::ValueTree masterNode;   // the master bus (its automation lanes)
    std::function<std::shared_ptr<fx::InstrumentSlot> (const juce::ValueTree& track)> instrumentFor;
};

/** Builds the graph from the TRACKS node. Clip files are resolved relative to mediaBundle. */
std::unique_ptr<PlaybackGraph> buildPlaybackGraph (const juce::ValueTree& tracks, const juce::File& mediaBundle,
                                                   AudioCache& cache, const GraphBuildOptions& options);

/** Adds one track's clips for timeline range [t0, t0+n) into L/R (does not clear). Real-time safe. */
void renderTrackClips (const PlayTrack& track, juce::int64 t0, int n, float* left, float* right) noexcept;

/** Moves the master bus's plugin parameters to their automation at `timeSec` and returns the master volume automation
    there as a linear gain (1 without a volume lane). Audio thread. */
float applyMasterLanes (PlaybackGraph&, double timeSec, float* pan = nullptr) noexcept;   // *pan: the master pan automation (-1..1, 0 without a pan lane)

/** Samples the track's rig and inserts delay its sound by (audio thread). Playback reads the track's clips that much
    earlier, so recorded audio stays in time with the other tracks (live monitoring cannot be compensated). */
int trackLatency (const PlayTrack&) noexcept;
/** How far ahead to read a track's clips so it lines up after its own effects and the buses it plays through. */
inline int readAhead (const PlayTrack& t) noexcept { return trackLatency (t) + t.downstreamLatency; }

/** Per block, before the track is mixed: collects the MIDI of [from, from + n) into midiBuffer (wrapping at the
    loop), silences held notes after a jump or a stop, and applies the automation at `timeSec` (fader / pan gains,
    plugin parameters). Real-time safe. */
struct BlockTiming
{
    juce::int64 from = 0;          // timeline samples (with read-ahead)
    bool rolling = false;
    bool looping = false;
    juce::int64 loopStart = 0, loopEnd = 0;
    double timeSec = 0.0;          // audible time, for automation
};
void prepareTrackBlock (PlayTrack&, const BlockTiming&, int n) noexcept;

/** Constant-power pan with unity at centre. */
void panGains (float pan, float gain, float& left, float& right) noexcept;
} // namespace ff
