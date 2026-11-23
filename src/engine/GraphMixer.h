#pragma once
#include "PlaybackGraph.h"
#include "TrackWorkers.h"

namespace ff
{
/** Mixes a PlaybackGraph: every track's signal goes through its rig and plugins (in parallel on the worker threads),
    then its sends, fader and pan, into the bus it plays into or the master. Bus and FX tracks are processed after
    the tracks that feed them. Used by live playback and by export. Real-time safe after prepare(). */
class GraphMixer
{
public:
    /** Fills the pre-effects signal of each track. */
    struct Source
    {
        virtual ~Source() = default;
        /** Audio tracks: add the track's clips / input into L and R (they arrive cleared). Bus and FX tracks: L and R
            already hold what was routed to them. Return false to leave the track out (muted / nothing to do). */
        virtual bool fill (int index, PlayTrack&, float* L, float* R, int n) noexcept = 0;
        /** The track's final signal after fader and pan (meters). */
        virtual void post (int, const PlayTrack&, const float*, const float*, int) noexcept {}
        /** Timeline position (seconds) of this block for the master bus's automation; negative: not rolling (no automation). */
        virtual double masterTime() const noexcept { return -1.0; }
        /** How much the Drive knob changed the level of this block, in dB (<= 0), for its meter; index -1 is the master. */
        virtual void driveActivity (int, float) noexcept {}
    };

    void prepare (int maxBlock, double sampleRate);
    int maxBlock() const noexcept { return blockSize; }
    TrackWorkers* workers = nullptr;   // optional
    bool allowIdle = true;             // skip rigs and plugins on tracks that have been silent for a while

    /** Mixes the graph into outL / outR, which must start out silent (the master plugins process the result).
        n <= maxBlock(). hwOut (optional): the audio device's output channels by channel number (nullptr where a channel
        is off); tracks and sends routed to an interface output are added there. A track whose interface output is not
        available plays through the master instead (so export, which has none, still contains it); a send to one is
        left out. */
    void mix (PlaybackGraph&, Source&, float* outL, float* outR, int n, float* const* hwOut = nullptr, int numHw = 0) noexcept;

    /** Updates every track's downstreamLatency (call before Source::fill reads the clips). */
    static void updateLatencies (PlaybackGraph&) noexcept;

private:
    int blockSize = 0;
    double sr = 48000.0;
    std::vector<std::vector<float>> bufL, bufR;
    std::vector<std::vector<float>> instL, instR;   // audio sent into an instrument track's input
    std::vector<float> tmpL, tmpR;
    std::vector<char> active;
    std::vector<char> scFlag;                          // tracks whose signal feeds some plugin's side-chain
    std::vector<std::vector<float>> scBufL, scBufR;    // those tracks' signal before the fader, for the plugins that read it
    std::vector<int> stage;   // tracks of the level being processed

    struct JobContext { GraphMixer* self; PlaybackGraph* graph; int n; };
    static void processJob (void* ctx, int i) noexcept;
    void processTrack (PlayTrack&, float* L, float* R, int n) noexcept;
};
} // namespace ff
