#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

namespace ff::fx
{
/** Tape-style drive for a mixer strip (the Drive knob). A level-dependent soft clip: quiet signal passes unchanged, louder
    peaks are rounded off, so turning it up adds density and low harmonics instead of just volume. A small bias makes the
    clipping slightly asymmetric (even harmonics: warmth); a high-pass takes out the DC that bias leaves.
    Real-time safe; the drive is ramped across the block, so moving the knob does not click. */
struct Saturator
{
    float dcInL = 0.0f, dcOutL = 0.0f, dcInR = 0.0f, dcOutR = 0.0f;   // DC blocker state
    float prevDrive = 0.0f;

    static float shape (float x) noexcept   // tanh, rational approximation (exact at 0, +-1 at +-3)
    {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    /** drive 0..1 (0 = off: the signal is untouched). Returns the change in level this block, in dB (<= 0), for the meter. */
    float process (float* L, float* R, int n, float drive) noexcept
    {
        if (drive <= 0.0001f && prevDrive <= 0.0001f) { prevDrive = 0.0f; return 0.0f; }
        const float from = prevDrive, to = drive, step = (to - from) / (float) juce::jmax (1, n);
        prevDrive = to;
        constexpr float r = 0.9993f;   // DC blocker, about 5 Hz at 48 kHz
        double inSum = 0.0, outSum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float d = from + step * (float) (i + 1);
            const float g = 1.0f + 9.0f * d * d;      // 0 .. +19 dB of gain into the clipper (more at the top of the knob)
            const float bias = 0.12f * d;
            const float norm = 1.0f / g, off = shape (bias) * norm;
            const float xl = L[i], xr = R[i];
            const float yl = shape (xl * g + bias) * norm - off, yr = shape (xr * g + bias) * norm - off;
            const float ol = yl - dcInL + r * dcOutL, orr = yr - dcInR + r * dcOutR;
            dcInL = yl; dcOutL = ol; dcInR = yr; dcOutR = orr;
            L[i] = ol; R[i] = orr;
            inSum += std::abs (xl) + std::abs (xr);
            outSum += std::abs (ol) + std::abs (orr);
        }
        if (inSum < 1.0e-6) return 0.0f;
        return juce::jmin (0.0f, 20.0f * std::log10 ((float) (outSum / inSum) + 1.0e-6f));
    }
};
} // namespace ff::fx
