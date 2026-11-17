#include "KeysGen.h"
#include <algorithm>
#include <random>

namespace ff::keys
{
namespace
{
struct Prog { const char* name; std::vector<int> degrees; };
const std::vector<Prog>& progs()
{
    static const std::vector<Prog> p {
        { "I - V - vi - IV", { 0, 4, 5, 3 } },
        { "I - vi - IV - V", { 0, 5, 3, 4 } },
        { "vi - IV - I - V", { 5, 3, 0, 4 } },
        { "I - IV - V - IV", { 0, 3, 4, 3 } },
        { "ii - V - I - I", { 1, 4, 0, 0 } },
        { "i - VI - III - VII", { 0, 5, 2, 6 } },
        { "i - iv - v - i", { 0, 3, 4, 0 } },
        { "12-bar blues", { 0, 0, 0, 0, 3, 3, 0, 0, 4, 3, 0, 4 } },
        { "One chord (drone)", { 0 } },
    };
    return p;
}

/** Pitch of a scale degree (any integer, wrapping into other octaves) above `root`. */
int degreePitch (const std::vector<int>& steps, int root, int degree)
{
    const int n = (int) steps.size();
    int oct = degree / n, d = degree % n;
    if (d < 0) { d += n; --oct; }
    return root + steps[(size_t) d] + 12 * oct;
}

int clampPitch (int p) { while (p > 127) p -= 12; while (p < 0) p += 12; return p; }
}

const char* scaleName (int s)
{
    static const char* n[] = { "Major", "Minor", "Dorian", "Mixolydian", "Pentatonic major", "Pentatonic minor", "Blues" };
    return n[std::clamp (s, 0, (int) numScales - 1)];
}

const char* styleName (int s)
{
    static const char* n[] = { "Block chords", "Arpeggio up", "Arpeggio up / down", "Bass: roots", "Bass: walking", "Riff" };
    return n[std::clamp (s, 0, (int) numStyles - 1)];
}

const std::vector<int>& scaleSteps (int scale)
{
    static const std::vector<int> v[] = {
        { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 2, 4, 5, 7, 9, 10 },
        { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 }, { 0, 3, 5, 6, 7, 10 } };
    return v[std::clamp (scale, 0, (int) numScales - 1)];
}

int numProgressions() { return (int) progs().size(); }
const char* progressionName (int i) { return progs()[(size_t) std::clamp (i, 0, numProgressions() - 1)].name; }
const std::vector<int>& progression (int i) { return progs()[(size_t) std::clamp (i, 0, numProgressions() - 1)].degrees; }

double barQuarters (const Options& o) { return (double) std::max (1, o.tsNum) * 4.0 / (double) std::max (1, o.tsDen); }

std::vector<Note> generate (const Options& o)
{
    std::vector<Note> out;
    const auto& steps = scaleSteps (o.scale);
    const auto& prog = progression (o.progression);
    const double barQ = barQuarters (o);
    const int rootBase = (std::clamp (o.octave, 0, 8) + 1) * 12 + std::clamp (o.key, 0, 11);
    const bool bass = o.style == bassRoots || o.style == bassWalking;
    const int root = bass ? rootBase - 24 : rootBase;
    const int bars = std::clamp (o.bars, 1, 64);

    auto add = [&] (double start, double len, int pitch, int vel)
    {
        out.push_back ({ start, std::max (0.05, len), clampPitch (pitch), std::clamp (vel, 1, 127) });
    };

    for (int b = 0; b < bars; ++b)
    {
        std::mt19937 rng ((unsigned) (o.seed * 7919 + b * 104729 + o.style * 31));
        auto rnd = [&] (int n) { return (int) (rng() % (unsigned) std::max (1, n)); };
        const int d = prog[(size_t) b % prog.size()];
        const int nextD = prog[(size_t) (b + 1) % prog.size()];
        const double t0 = b * barQ;
        const int beats = (int) std::ceil (barQ);

        switch (o.style)
        {
            case blockChords:
                for (int k : { 0, 2, 4 }) add (t0, barQ * 0.96, degreePitch (steps, root, d + k), 88 + rnd (10));
                break;
            case arpUp:
            case arpUpDown:
            {
                const int tones[4] = { d, d + 2, d + 4, d + (int) steps.size() };
                const int order[8] = { 0, 1, 2, 3, 2, 1, 2, 1 };
                const int steps8 = (int) (barQ * 2.0);
                for (int i = 0; i < steps8; ++i)
                {
                    const int idx = o.style == arpUp ? i % 4 : order[i % 8];
                    add (t0 + i * 0.5, 0.45, degreePitch (steps, root, tones[idx]), (i % 2 == 0 ? 96 : 80) + rnd (8));
                }
                break;
            }
            case bassRoots:
            {
                const int r = degreePitch (steps, root, d);
                add (t0, 1.75, r, 100 + rnd (8));
                if (beats >= 3) add (t0 + 2.0, 1.5, r, 92 + rnd (8));
                if (beats >= 4) add (t0 + 3.5, 0.45, degreePitch (steps, root, d + 4) - 12 + 12, 80 + rnd (8));
                break;
            }
            case bassWalking:
            {
                const int walk[4] = { d, d + 2, d + 4, nextD > d ? nextD - 1 : nextD + 1 };
                for (int i = 0; i < beats; ++i)
                    add (t0 + i, 0.9, degreePitch (steps, root, walk[std::min (i, 3)]), i == 0 ? 104 : 88 + rnd (10));
                break;
            }
            case riff:
            {
                int deg = d + 4 + rnd (3);
                for (double t = 0.0; t < barQ - 1e-6;)
                {
                    const double len = rnd (4) == 0 ? 1.0 : 0.5;
                    if (rnd (6) != 0)   // an occasional rest
                    {
                        const bool last = t + len >= barQ - 1e-6;
                        const int target = last ? d + 2 * rnd (3) : deg;
                        add (t0 + t, len * 0.9, degreePitch (steps, rootBase, target), (std::fmod (t, 1.0) < 1e-6 ? 98 : 82) + rnd (10));
                        deg = target;
                    }
                    deg += rnd (5) - 2;
                    deg = std::clamp (deg, d - 2, d + 9);
                    t += len;
                }
                break;
            }
            default: break;
        }
    }
    return out;
}
} // namespace ff::keys
