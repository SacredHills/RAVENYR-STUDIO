#pragma once
#include <juce_dsp/juce_dsp.h>
#include "core/RigModel.h"
#include "core/SpscFifo.h"
#include "NamModel.h"
#include "Biquad.h"
#include "PluginHost.h"
#include <array>
#include <atomic>

namespace ff::fx
{
/** One guitar rig: input -> noise gate, compressor, overdrive, amp (NAM capture or the built-in amp), EQ,
    cabinet (impulse response or a built-in speaker filter), chorus, delay, reverb -> output.
    The blocks run in a user-defined order; blocks up to the first chorus/delay/reverb run in mono.

    Threading:
      - parameters are atomics: write them from any thread (setParamsFrom on the message thread);
      - setModel / setImpulseResponse are wait-free hand-overs from the message thread; replaced models come
        back through collectGarbage() (message thread) so the audio thread never frees memory;
      - prepare() allocates: call it only while no audio callback is processing this rig, and never at the same
        time as setModel / setImpulseResponse (the owner serialises them);
      - process() is real-time safe. */
class AmpRig
{
public:
    explicit AmpRig (juce::dsp::ConvolutionMessageQueue* sharedQueue = nullptr);
    ~AmpRig();

    void prepare (double sampleRate, int maxBlock);
    bool isPreparedFor (double rate) const noexcept { return rate > 0.0 && std::abs (preparedRate.load() - rate) < 0.01; }
    double getPreparedRate() const noexcept { return preparedRate.load(); }

    // ---- parameters ----
    std::array<std::atomic<float>, rig::numParams> params;
    std::atomic<juce::uint64> order { rig::packOrder (rig::defaultOrder()) };
    void setParamsFrom (const juce::ValueTree& rigTree);
    void setParam (int p, float v) noexcept { params[(size_t) p].store (v); }
    float getParam (int p) const noexcept { return params[(size_t) p].load(); }

    // ---- model / impulse response (message thread) ----
    void setModel (std::unique_ptr<NamModel> model);
    void clearModel();
    /** Mono IR at its own sample rate (resampled, trimmed and normalised by the convolution engine). */
    void setImpulseResponse (juce::AudioBuffer<float>&& ir, double irSampleRate);
    void clearImpulseResponse() noexcept { irActive = false; }
    /** A second cabinet, blended with the first by the cabBlend parameter (only heard while the first is loaded too). */
    void setImpulseResponse2 (juce::AudioBuffer<float>&& ir, double irSampleRate);
    void clearImpulseResponse2() noexcept { irActive2 = false; }
    void collectGarbage();
    /** For offline rendering: finishes pending IR loads (call after setImpulseResponse, before processing). */
    void finishLoadingOffline();

    // ---- plugin slots (message thread) ----
    /** Puts a prepared plugin into a slot (nullptr empties it) and returns the previous one. The audio thread may still
        be inside the previous plugin until the next audio callback: the caller frees it only after that
        (AudioEngine::waitForAudioCallback), or at once if this rig is not playing. */
    std::shared_ptr<juce::AudioPluginInstance> setPlugin (int slot, std::shared_ptr<juce::AudioPluginInstance> plugin);
    std::shared_ptr<juce::AudioPluginInstance> getPlugin (int slot) const;

    // ---- audio thread ----
    /** Stereo in place. The rig's input is the mono sum of L and R. */
    void process (float* left, float* right, int numSamples) noexcept;
    void reset() noexcept;

    // ---- status (read by the UI) ----
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };   // since the UI last took them
    std::atomic<bool> gateOpen { false };
    std::atomic<float> cpuLoad { 0.0f };                     // fraction of real time spent in process()
    std::atomic<bool> modelActive { false };                 // a NAM model is processing
    std::atomic<bool> modelNeedsPrepare { false };           // a model arrived prepared for another device rate
    std::atomic<bool> irActive { false }, irActive2 { false };
    std::atomic<int> latency { 0 };                          // samples added by NAM resampling
    /** Everything the rig delays the sound by: NAM resampling + plugins that are switched on. Audio thread. */
    int totalLatency() const noexcept;

private:
    std::atomic<double> preparedRate { 0.0 };
    double sr = 48000.0;
    int maxBlock = 0;

    // model hand-over
    std::atomic<NamModel*> pendingModel { nullptr };
    std::atomic<bool> clearRequested { false };
    NamModel* model = nullptr;
    SpscFifo<NamModel*> retiredModels;
    void takeOverModel() noexcept;

    // block state
    std::array<bool, rig::numBlocks> wasOn {};
    juce::SmoothedValue<float> inG, outG, compG, driveG, ampG, normG, calG;
    std::vector<float> mono, right2, tmp;

    // gate
    float gateEnv = 0.0f, gateGain = 0.0f;
    bool gateIsOpen = false;
    // compressor
    juce::dsp::Compressor<float> compressor;
    // overdrive
    juce::dsp::Oversampling<float> driveOs { 1, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    Biquad driveHp, driveToneLp;
    float lastDriveTone = -1.0f;
    // built-in amp
    juce::dsp::Oversampling<float> ampOs { 1, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    Biquad ampHp, ampMid, ampLp;
    // eq
    Biquad eqBass, eqMid, eqTreble, eqPresence;
    std::array<float, 4> lastEq { { 99, 99, 99, 99 } };
    // cab
    std::unique_ptr<juce::dsp::Convolution> convolution, convolution2;
    Biquad cabHp1, cabHp2, cabLp1, cabLp2, spkHp, spkPeak, spkLp1, spkLp2;
    float lastLowCut = -1, lastHighCut = -1;
    // chorus / delay / reverb
    juce::dsp::Chorus<float> chorusFx;
    std::vector<float> dlL, dlR;
    int dlWrite = 0;
    juce::SmoothedValue<float> dlTime;
    float dlLpL = 0, dlLpR = 0;
    juce::dsp::Reverb reverbFx;
    // plugins
    struct PluginSlot
    {
        std::shared_ptr<juce::AudioPluginInstance> owner;            // message thread
        std::atomic<juce::AudioPluginInstance*> active { nullptr };  // audio thread
    };
    std::array<PluginSlot, rig::numPluginSlots> pluginSlots;
    juce::AudioBuffer<float> pluginBuffer;
    juce::MidiBuffer pluginMidi;
    void runPlugin (int slot, float* L, float* R, int n) noexcept;

    void processChunk (float* L, float* R, int n) noexcept;
    void runGate (float* x, int n) noexcept;
    void runDrive (float* x, int n) noexcept;
    void runAmp (float* x, int n) noexcept;
    void runEq (float* x, int n) noexcept;
    void runCab (float* x, int n) noexcept;
    void runDelay (float* L, float* R, int n) noexcept;
    void resetBlock (int block) noexcept;
};
} // namespace ff::fx
