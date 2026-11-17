#pragma once
#include "PluginHost.h"
#include <atomic>

namespace ff::fx
{
/** The instrument (synth / sampler plugin) of an instrument track. Threading like InsertChain: setPlugin from the
    message thread (free the returned plugin after the next audio callback), prepare() only while no audio callback
    is processing it, render() real-time safe. */
class InstrumentSlot
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        block = juce::jmax (1, maxBlock);
        scratch.setSize (32, block, false, true, false);
        preparedRate = sampleRate;
    }
    double getPreparedRate() const noexcept { return preparedRate.load(); }
    /** Bypassed: the instrument makes no sound (notes held at that moment are released). */
    std::atomic<bool> bypassed { false };
    /** Share of real time the instrument takes to render (1.0 = one block's duration), smoothed. Audio thread writes. */
    std::atomic<float> cpuLoad { 0.0f };
    /** Extra stereo output buses switched on (message thread; takes effect when the plugin is prepared). */
    std::atomic<int> auxOutputs { 0 };

    /** The sound of extra output bus `bus` (1 = the first extra one) of the block just rendered: false if it has none.
        Audio thread, after render(), valid until the next render(). */
    bool auxOutput (int bus, const float*& l, const float*& r) const noexcept
    {
        auto* p = active.load();
        if (! auxValid || p == nullptr || bus < 1 || bus >= p->getBusCount (false)) return false;
        auto* b = p->getBus (false, bus);
        if (b == nullptr || ! b->isEnabled() || b->getNumberOfChannels() < 1) return false;
        const int first = p->getChannelIndexInProcessBlockBuffer (false, bus, 0);
        const int second = b->getNumberOfChannels() > 1 ? first + 1 : first;
        if (second >= scratch.getNumChannels()) return false;
        l = scratch.getReadPointer (first);
        r = scratch.getReadPointer (second);
        return true;
    }

    std::shared_ptr<juce::AudioPluginInstance> setPlugin (std::shared_ptr<juce::AudioPluginInstance> p)
    {
        active.store (p.get());
        std::swap (owner, p);
        return p;
    }
    std::shared_ptr<juce::AudioPluginInstance> getPlugin() const { return owner; }
    juce::AudioPluginInstance* activePlugin() const noexcept { return active.load(); }

    /** Adds the instrument's sound for `midi` into L / R (n samples, n <= the prepared block). */
    void render (float* L, float* R, int n, juce::MidiBuffer& midi, const float* inL = nullptr, const float* inR = nullptr) noexcept
    {
        auto* p = active.load();
        auxValid = false;
        if (p == nullptr || n > scratch.getNumSamples()) return;
        if (bypassed.load())
        {
            if (! wasBypassed)   // let go of what is sounding, once, and drop the sound it makes
            {
                wasBypassed = true;
                const int c = juce::jmax (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels(), 1);
                if (c <= scratch.getNumChannels())
                {
                    juce::MidiBuffer off;
                    for (int chn = 1; chn <= 16; ++chn) { off.addEvent (juce::MidiMessage::allNotesOff (chn), 0); off.addEvent (juce::MidiMessage::allSoundOff (chn), 0); }
                    juce::AudioBuffer<float> junk (scratch.getArrayOfWritePointers(), c, n);
                    junk.clear();
                    const juce::ScopedLock sl (p->getCallbackLock());
                    if (! p->isSuspended()) p->processBlock (junk, off);
                }
            }
            midi.clear();
            cpuLoad.store (0.0f, std::memory_order_relaxed);
            return;
        }
        wasBypassed = false;
        const int ch = juce::jmax (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels(), 1);
        if (ch > scratch.getNumChannels()) return;
        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), ch, n);
        view.clear();
        if (inL != nullptr && p->getTotalNumInputChannels() > 0)   // audio sent into the instrument (samplers, vocoders, ...)
        {
            juce::FloatVectorOperations::copy (view.getWritePointer (0), inL, n);
            if (p->getTotalNumInputChannels() > 1 && inR != nullptr) juce::FloatVectorOperations::copy (view.getWritePointer (1), inR, n);
        }
        {
            const juce::ScopedLock sl (p->getCallbackLock());
            if (p->isSuspended()) return;
            const auto t0 = juce::Time::getHighResolutionTicks();
            p->processBlock (view, midi);
            const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            cpuLoad.store (cpuLoad.load (std::memory_order_relaxed) * 0.9f + (float) (secs / juce::jmax (1.0e-6, n / sr)) * 0.1f,
                           std::memory_order_relaxed);
            auxValid = true;
        }
        const int outs = p->getTotalNumOutputChannels();
        if (outs <= 0) return;
        juce::FloatVectorOperations::add (L, view.getReadPointer (0), n);
        juce::FloatVectorOperations::add (R, view.getReadPointer (outs > 1 ? 1 : 0), n);
    }

private:
    std::shared_ptr<juce::AudioPluginInstance> owner;
    std::atomic<juce::AudioPluginInstance*> active { nullptr };
    juce::AudioBuffer<float> scratch;
    std::atomic<double> preparedRate { 0.0 };
    double sr = 48000.0;
    int block = 512;
    bool wasBypassed = false;   // audio thread only
    bool auxValid = false;      // audio thread: the scratch buffer holds this block's output
};
} // namespace ff::fx
