#pragma once
#include "PluginHost.h"
#include "core/RigModel.h"
#include <array>
#include <atomic>

namespace ff::fx
{
/** A track's insert plugins (after its amp rig, before the fader), processed in stereo in a user-defined order.

    Plugins live in fixed physical slots; the play order is a separate packed array, so reordering never reloads a
    plugin. Threading follows AmpRig: setPlugin / setOrder / bypass from the message thread, prepare() only while no
    audio callback is processing this chain, process() real-time safe. */
class InsertChain
{
public:
    static constexpr int maxSlots = rig::maxInserts;

    void prepare (double sampleRate, int maxBlock);
    double getPreparedRate() const noexcept { return preparedRate.load(); }
    bool isPreparedFor (double rate) const noexcept { return rate > 0.0 && std::abs (preparedRate.load() - rate) < 0.01; }

    /** Returns the previous plugin of the physical slot (free it after the next audio callback). */
    std::shared_ptr<juce::AudioPluginInstance> setPlugin (int physicalSlot, std::shared_ptr<juce::AudioPluginInstance>);
    std::shared_ptr<juce::AudioPluginInstance> getPlugin (int physicalSlot) const;
    /** The plugin running in a physical slot (audio thread; automation), nullptr if none. */
    juce::AudioPluginInstance* activePlugin (int physicalSlot) const noexcept
    {
        return juce::isPositiveAndBelow (physicalSlot, maxSlots) ? slots[(size_t) physicalSlot].active.load() : nullptr;
    }
    /** Play order as physical slots; `count` entries are used. */
    void setOrder (const std::array<int, maxSlots>& order, int count) noexcept;
    std::array<std::atomic<bool>, maxSlots> bypass {};
    /** Per physical slot: share of real time the plugin takes to process (1.0 = all of one block's duration), smoothed.
        Written by the audio thread, read by the UI (CPU breakdown). */
    std::array<std::atomic<float>, maxSlots> load {};
    void clearLoad() noexcept { for (auto& l : load) l.store (0.0f, std::memory_order_relaxed); }   // the chain is not running

    /** scLeft / scRight (optional): per physical slot, the audio for that plugin's side-chain input (nullptr: none). */
    /** inputOffset: song position of the chain's input relative to the engine position (see PlayHeadOffset); each
        plugin is told the position of the audio it receives (minus the latency of the plugins before it). */
    void process (float* left, float* right, int numSamples, const float* const* scLeft = nullptr, const float* const* scRight = nullptr,
                  juce::int64 inputOffset = 0) noexcept;
    /** Which track feeds a plugin's side-chain, by physical slot ("" = none). Message thread only (read when a graph is built). */
    std::array<juce::String, maxSlots> sidechainId;
    /** Sum of the latencies of the plugins in the order that are not bypassed. Audio thread. */
    int latency() const noexcept;

private:
    struct Slot
    {
        std::shared_ptr<juce::AudioPluginInstance> owner;
        std::atomic<juce::AudioPluginInstance*> active { nullptr };
    };
    std::array<Slot, maxSlots> slots;
    // play order, published with a sequence lock (odd = being written) so the audio thread always sees a whole order
    std::array<std::atomic<int>, maxSlots> orderSlots {};
    std::atomic<int> orderCount { 0 };
    std::atomic<juce::uint32> orderSeq { 0 };
    int readOrder (std::array<int, maxSlots>& out) const noexcept;   // returns the count
    std::atomic<double> preparedRate { 0.0 };
    double sr = 48000.0;
    int maxBlock = 0;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
};
} // namespace ff::fx
