#include <juce_core/juce_core.h>
#include "core/KeysGen.h"
#include <set>

using namespace ff;

class KeysGenTests final : public juce::UnitTest
{
public:
    KeysGenTests() : juce::UnitTest ("Keys generator", "Core") {}

    void runTest() override
    {
        beginTest ("every style, scale and progression makes valid notes inside the bars");
        {
            for (int style = 0; style < keys::numStyles; ++style)
                for (int scale = 0; scale < keys::numScales; ++scale)
                    for (int prog = 0; prog < keys::numProgressions(); ++prog)
                    {
                        keys::Options o;
                        o.style = style; o.scale = scale; o.progression = prog; o.bars = 8; o.key = (style + scale + prog) % 12;
                        const auto notes = keys::generate (o);
                        expect (! notes.empty(), "no notes for style " + juce::String (style));
                        const double total = o.bars * keys::barQuarters (o);
                        for (auto& n : notes)
                        {
                            expect (n.pitch >= 0 && n.pitch <= 127 && n.velocity >= 1 && n.velocity <= 127);
                            expect (n.startQ >= 0.0 && n.startQ < total && n.lengthQ > 0.0, "note inside the pattern");
                        }
                    }
        }

        beginTest ("same seed repeats, another seed changes a riff; bass is lower than chords");
        {
            keys::Options a;
            a.style = keys::riff; a.seed = 5;
            auto b = a;
            auto c = a; c.seed = 6;
            const auto na = keys::generate (a), nb = keys::generate (b), nc = keys::generate (c);
            expect (na.size() == nb.size());
            bool same = na.size() == nb.size();
            for (size_t i = 0; same && i < na.size(); ++i) same = na[i].pitch == nb[i].pitch && na[i].startQ == nb[i].startQ;
            expect (same, "deterministic");
            bool differs = na.size() != nc.size();
            for (size_t i = 0; ! differs && i < na.size(); ++i) differs = na[i].pitch != nc[i].pitch || na[i].startQ != nc[i].startQ;
            expect (differs, "another seed gives another riff");

            keys::Options chord, bass;
            bass.style = keys::bassRoots;
            int lowChord = 127, lowBass = 127;
            for (auto& n : keys::generate (chord)) lowChord = std::min (lowChord, n.pitch);
            for (auto& n : keys::generate (bass)) lowBass = std::min (lowBass, n.pitch);
            expect (lowBass < lowChord - 12, "bass plays well below the chords");
        }

        beginTest ("a block chord is a triad of the scale on the first beat of each bar");
        {
            keys::Options o;   // C major, I V vi IV
            o.bars = 4;
            const auto notes = keys::generate (o);
            expect ((int) notes.size() == 12);
            std::set<int> first;
            for (auto& n : notes) if (n.startQ == 0.0) first.insert (n.pitch);
            expect (first == std::set<int> { 60, 64, 67 }, "C major triad in the first bar");
            std::set<int> second;
            for (auto& n : notes) if (n.startQ == 4.0) second.insert (n.pitch);
            expect (second == std::set<int> { 67, 71, 74 }, "G major triad in the second bar");
        }
    }
};

static KeysGenTests keysGenTests;
