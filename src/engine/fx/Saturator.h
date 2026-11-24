#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

namespace ff::fx
{
/** Linear-phase half-band filter for 2x oversampling (31 taps, Blackman window, unity gain at DC). Fixed size, no allocation. */
class HalfBand
{
public:
    HalfBand()
    {
        double sum = 0.5;
        for (int k = 1; k <= 15; k += 2)
        {
            const double n = (double) k + 15.0, w = 0.42 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * n / 30.0)
                                                  + 0.08 * std::cos (4.0 * juce::MathConstants<double>::pi * n / 30.0);
            h[(size_t) k] = (float) (std::sin (juce::MathConstants<double>::halfPi * k) / (juce::MathConstants<double>::pi * k) * w);
            sum += 2.0 * (double) h[(size_t) k];
        }
        center = (float) (0.5 / sum);
        for (auto& v : h) v = (float) ((double) v / sum);
    }

    void reset() noexcept { z.fill (0.0f); }

    /** Pushes one sample at the high rate and returns the filtered value (delayed by 15 high-rate samples). */
    float run (float x) noexcept
    {
        for (int i = 0; i < 30; ++i) z[(size_t) i] = z[(size_t) i + 1];
        z[30] = x;
        float y = center * z[15];
        for (int k = 1; k <= 15; k += 2) y += h[(size_t) k] * (z[(size_t) (15 - k)] + z[(size_t) (15 + k)]);
        return y;
    }

    void up (const float* in, float* out, int n) noexcept     // n samples in, 2n out
    {
        for (int i = 0; i < n; ++i) { out[2 * i] = 2.0f * run (in[i]); out[2 * i + 1] = 2.0f * run (0.0f); }
    }

    void down (const float* in, float* out, int n) noexcept   // 2n samples in, n out
    {
        for (int i = 0; i < n; ++i) { run (in[2 * i]); out[i] = run (in[2 * i + 1]); }
    }

private:
    std::array<float, 16> h {};
    float center = 0.5f;
    std::array<float, 31> z {};
};

/** The Drive knob of a mixer strip: a tape-style saturator with a console feel.
    - The signal is split at about 700 Hz and the two halves are clipped separately, the lows much harder than the highs:
      like tape and transformers, the low end saturates first and the top stays clean.
    - A small head-bump lifts the lows going in; the highs soften a little as Drive goes up.
    - A slow, gentle compression (the tape "squash") takes a few dB off hot peaks.
    - A bias makes the clipping slightly asymmetric (even harmonics: warmth); a DC blocker removes what it leaves.
    It is an approximation built from those behaviours, not a copy of any particular console.
    Runs at 1x, 2x or 4x oversampling (own half-band filters, so nothing is allocated on the audio thread) to keep the
    harmonics from folding back. The oversampling filters delay the signal a little (about 0.3-0.5 ms) while Drive is on; with
    Drive off the strip is not touched and not delayed. Drive coming on or going off is crossfaded over ~1.3 ms, so the small
    shift does not click. Real-time safe; Drive is ramped across the block, so moving the knob does not click. */
struct Saturator
{
    float dcIn[2] {}, dcOut[2] {};          // DC blocker
    float prevDrive = 0.0f;
    int lastOs = 0;                          // 0, 1, 2 = off, 2x, 4x (a change clears the filters)
    bool active = false;                     // the wet path has been running up to the previous block


    static float shape (float x) noexcept   // tanh, rational approximation (exact at 0, +-1 at +-3)
    {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    /** drive 0..1 (0 = off: the signal is untouched); os 0 / 1 / 2 = no oversampling / 2x / 4x.
        Returns the change in level this block, in dB (<= 0), for the meter. */
    float process (float* L, float* R, int n, float drive, int os = 1, double sampleRate = 48000.0) noexcept
    {
        os = juce::jlimit (0, 2, os);
        bool tail = false;   // Drive has just reached 0: one more wet block, faded out into the dry signal
        if (drive <= 0.0001f && prevDrive <= 0.0001f)
        {
            prevDrive = 0.0f;
            if (! active) { store (L, R, n); return 0.0f; }   // off: the signal is untouched
            tail = true;
        }
        const bool starting = ! active;
        if (starting || os != lastOs) prime (os);
        active = ! tail;
        lastOs = os;

        const float from = prevDrive, stepBase = (drive - from) / (float) juce::jmax (1, n);
        prevDrive = drive;
        const int factor = 1 << os;
        const double fs = sampleRate * factor;
        const auto coef = [&] (double hz) { return (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * hz / fs)); };
        const float aLow = coef (700.0), aBump = coef (90.0), aHf = coef (8000.0);
        const float attack = coef (150.0), release = coef (4.0);   // the squash: ~1 ms attack, ~40 ms release
        constexpr float r = 0.9993f;   // DC blocker, about 5 Hz at 48 kHz

        double inSum = 0.0, outSum = 0.0;
        for (int i = 0; i < n; ++i) inSum += std::abs (L[i]) + std::abs (R[i]);
        store (L, R, n);   // the dry input, for when Drive goes off again

        constexpr int chunk = 128, fadeLen = 64;
        float a[2][chunk * 4], b[2][chunk * 4], dry[2][chunk];
        float* io[2] = { L, R };
        for (int s = 0; s < n; s += chunk)
        {
            const int m = juce::jmin (chunk, n - s), mh = m * factor;
            for (int c = 0; c < 2; ++c)
            {
                std::copy (io[c] + s, io[c] + s + m, dry[c]);
                if (os == 0) std::copy (io[c] + s, io[c] + s + m, a[c]);
                else if (os == 1) up1[c].up (io[c] + s, a[c], m);
                else { up1[c].up (io[c] + s, b[c], m); up2[c].up (b[c], a[c], 2 * m); }
            }
            for (int i = 0; i < mh; ++i)
            {
                const float d = from + stepBase * ((float) s + (float) (i + 1) / (float) factor);
                const float g = 1.0f + 39.0f * std::pow (d, 1.5f);   // 0 .. +32 dB into the low band's clipper
                const float gHigh = 1.0f + (g - 1.0f) * 0.4f;        // the highs are driven less
                const float k = 1.0f / (1.0f + (g - 1.0f) * 0.14f), kHigh = 1.0f / (1.0f + (gHigh - 1.0f) * 0.14f);   // keeps the loudness up as it clips
                const float bias = 0.12f * d, off = shape (bias) * k;
                const float hfMix = 0.30f * d, squash = 0.12f * d, bump = 0.18f * d;
                for (int c = 0; c < 2; ++c)
                {
                    auto& t = tape[c];
                    const float x = a[c][i];
                    t.lp += aLow * (x - t.lp);
                    t.bump += aBump * (x - t.bump);
                    const float low = t.lp + t.bump * bump, high = x - t.lp;
                    float y = shape (low * g + bias) * k - off + shape (high * gHigh) * kHigh;
                    t.hf += aHf * (y - t.hf);
                    y += (t.hf - y) * hfMix;
                    const float mag = std::abs (y);
                    t.env += (mag > t.env ? attack : release) * (mag - t.env);
                    a[c][i] = y / (1.0f + squash * t.env);
                }
            }
            for (int c = 0; c < 2; ++c)
            {
                if (os == 0) std::copy (a[c], a[c] + m, io[c] + s);
                else if (os == 1) dn1[c].down (a[c], io[c] + s, m);
                else { dn2[c].down (a[c], b[c], 2 * m); dn1[c].down (b[c], io[c] + s, m); }
                for (int i = 0; i < m; ++i)   // the crossfade with the dry signal when Drive comes on / goes off, then the DC blocker
                {
                    const float x = (float) (s + i + 1) / (float) fadeLen;
                    const float w = starting ? juce::jmin (1.0f, x) : tail ? juce::jmax (0.0f, 1.0f - x) : 1.0f;
                    const float y = dry[c][i] * (1.0f - w) + io[c][s + i] * w, o = y - dcIn[c] + r * dcOut[c];
                    dcIn[c] = y; dcOut[c] = o;
                    io[c][s + i] = o;
                }
            }
        }
        for (int i = 0; i < n; ++i) outSum += std::abs (L[i]) + std::abs (R[i]);
        if (inSum < 1.0e-6) return 0.0f;
        // how far below the straight (unclipped) gain the block came out: the saturation at work, for the meter
        const float gEnd = 1.0f + 39.0f * std::pow (drive, 1.5f), straight = gEnd / (1.0f + (gEnd - 1.0f) * 0.14f);
        return juce::jmin (0.0f, 20.0f * std::log10 ((float) (outSum / inSum) / straight + 1.0e-6f));
    }

private:
    struct Tape { float lp = 0, bump = 0, hf = 0, env = 0; };
    Tape tape[2];
    HalfBand up1[2], up2[2], dn1[2], dn2[2];
    static constexpr int ringSize = 64;   // the last input samples, per channel
    float ring[2][ringSize] {};
    int ringPos = 0;                       // where the next input sample goes

    void store (const float* L, const float* R, int n) noexcept   // the block's input, without moving the position
    {
        int p = ringPos;
        for (int i = 0; i < n; ++i, ++p) { ring[0][p & (ringSize - 1)] = L[i]; ring[1][p & (ringSize - 1)] = R[i]; }
        ringPos = p & (ringSize - 1);
    }

    /** Fills the filters (and the tape state) with the recent past, so Drive coming on does not start from silence. */
    void prime (int os) noexcept
    {
        for (int c = 0; c < 2; ++c) { up1[c].reset(); up2[c].reset(); dn1[c].reset(); dn2[c].reset(); tape[c] = {}; }
        float past[2][32];
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 32; ++i) past[c][i] = ring[c][(ringPos - 32 + i) & (ringSize - 1)];
        for (int c = 0; c < 2; ++c)
        {
            tape[c].lp = tape[c].bump = tape[c].hf = past[c][31];
            if (os == 0) continue;
            float t1[64], t2[128], d1[64], d2[32];
            up1[c].up (past[c], t1, 32);
            if (os == 1) dn1[c].down (t1, d2, 32);
            else { up2[c].up (t1, t2, 64); dn2[c].down (t2, d1, 64); dn1[c].down (d1, d2, 32); }
        }
    }
};
} // namespace ff::fx
