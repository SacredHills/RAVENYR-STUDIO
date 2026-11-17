#include "InsertChain.h"

namespace ff::fx
{
void InsertChain::prepare (double rate, int block)
{
    sr = rate;
    maxBlock = juce::jmax (16, block);
    scratch.setSize (8, maxBlock);
    midi.ensureSize (256);
    for (auto& s : slots)
        if (s.owner != nullptr) preparePlugin (*s.owner, rate, maxBlock, true);
    preparedRate = rate;
}

std::shared_ptr<juce::AudioPluginInstance> InsertChain::setPlugin (int slot, std::shared_ptr<juce::AudioPluginInstance> p)
{
    if (! juce::isPositiveAndBelow (slot, maxSlots)) return p;
    auto& s = slots[(size_t) slot];
    auto old = std::move (s.owner);
    s.owner = std::move (p);
    s.active.store (s.owner.get(), std::memory_order_release);
    return old;
}

std::shared_ptr<juce::AudioPluginInstance> InsertChain::getPlugin (int slot) const
{
    return juce::isPositiveAndBelow (slot, maxSlots) ? slots[(size_t) slot].owner : nullptr;
}

void InsertChain::setOrder (const std::array<int, maxSlots>& o, int count) noexcept
{
    orderSeq.fetch_add (1, std::memory_order_acq_rel);   // odd: readers retry
    for (int i = 0; i < maxSlots; ++i) orderSlots[(size_t) i].store (o[(size_t) i], std::memory_order_relaxed);
    orderCount.store (juce::jlimit (0, maxSlots, count), std::memory_order_relaxed);
    orderSeq.fetch_add (1, std::memory_order_release);
}

int InsertChain::readOrder (std::array<int, maxSlots>& out) const noexcept
{
    for (;;)
    {
        const auto s0 = orderSeq.load (std::memory_order_acquire);
        if ((s0 & 1u) != 0) continue;   // a write is in progress (a few instructions long)
        const int count = orderCount.load (std::memory_order_relaxed);
        for (int i = 0; i < count; ++i) out[(size_t) i] = orderSlots[(size_t) i].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (orderSeq.load (std::memory_order_relaxed) == s0) return count;
    }
}

int InsertChain::latency() const noexcept
{
    std::array<int, maxSlots> o;
    const int count = readOrder (o);
    int l = 0;
    for (int i = 0; i < count; ++i)
    {
        const int slot = o[(size_t) i];
        if (slot < 0 || slot >= maxSlots || bypass[(size_t) slot].load (std::memory_order_relaxed)) continue;
        if (auto* p = slots[(size_t) slot].active.load (std::memory_order_acquire)) l += juce::jmax (0, p->getLatencySamples());
    }
    return l;
}

void InsertChain::process (float* L, float* R, int n, const float* const* scL, const float* const* scR, juce::int64 inputOffset) noexcept
{
    if (! isPreparedFor (sr) || maxBlock == 0) return;
    std::array<int, maxSlots> o;
    const int count = readOrder (o);
    std::array<juce::int64, maxSlots> spent {};   // ticks per slot over this call
    for (int done = 0; done < n;)
    {
        const int len = juce::jmin (maxBlock, n - done);
        juce::int64 offset = inputOffset + done;
        for (int i = 0; i < count; ++i)
        {
            const int slot = o[(size_t) i];
            if (slot < 0 || slot >= maxSlots || bypass[(size_t) slot].load (std::memory_order_relaxed)) continue;
            if (auto* p = slots[(size_t) slot].active.load (std::memory_order_acquire))
            {
                const PlayHeadOffset at (offset);
                offset -= juce::jmax (0, p->getLatencySamples());   // what follows hears this plugin's output, that much later
                const auto t0 = juce::Time::getHighResolutionTicks();
                runPluginStereo (*p, scratch, midi, L + done, R + done, len,
                                 scL != nullptr && scL[slot] != nullptr ? scL[slot] + done : nullptr,
                                 scR != nullptr && scR[slot] != nullptr ? scR[slot] + done : nullptr);
                spent[(size_t) slot] += juce::Time::getHighResolutionTicks() - t0;
            }
        }
        done += len;
    }
    const double blockSecs = juce::jmax (1.0e-6, n / sr);
    for (int i = 0; i < count; ++i)
    {
        const int slot = o[(size_t) i];
        if (slot < 0 || slot >= maxSlots) continue;
        const float now = (float) (juce::Time::highResolutionTicksToSeconds (spent[(size_t) slot]) / blockSecs);
        auto& l = load[(size_t) slot];
        l.store (l.load (std::memory_order_relaxed) * 0.9f + now * 0.1f, std::memory_order_relaxed);
    }
}
} // namespace ff::fx
