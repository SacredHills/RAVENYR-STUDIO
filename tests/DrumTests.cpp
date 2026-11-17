#include "engine/fx/InstrumentSlot.h"
#include "engine/fx/BuiltinPlugins.h"
#include "engine/fx/PluginHost.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include "core/DrumModel.h"
#include "core/TabModel.h"
#include "engine/DrumSynth.h"
#include "engine/TabSynth.h"
#include "engine/AudioEngine.h"
#include <cmath>
#include <set>

using namespace ff;
namespace d = ff::drums;

namespace
{
std::set<std::pair<int, int>> hitSet (const juce::ValueTree& p)
{
    std::set<std::pair<int, int>> s;
    for (auto h : p) s.insert ({ (int) h[d::ids::lane], (int) h[d::ids::tick] });
    return s;
}
int countLane (const juce::ValueTree& p, int lane)
{
    int n = 0;
    for (auto h : p) n += (int) h[d::ids::lane] == lane ? 1 : 0;
    return n;
}
float peak (const juce::AudioBuffer<float>& b, int from, int to)
{
    float m = 0.0f;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = juce::jmax (0, from); i < juce::jmin (b.getNumSamples(), to); ++i) m = juce::jmax (m, std::abs (b.getSample (ch, i)));
    return m;
}
double energy (const juce::AudioBuffer<float>& b, int from, int to)
{
    double e = 0.0;
    for (int i = juce::jmax (0, from); i < juce::jmin (b.getNumSamples(), to); ++i) e += b.getSample (0, i) * b.getSample (0, i);
    return e;
}
} // namespace

struct DrumTests : juce::UnitTest
{
    DrumTests() : juce::UnitTest ("Drum studio", "core") {}

    void runTest() override
    {
        beginTest ("model: patterns, hits, bars, GM mapping");
        juce::ValueTree project ("PROJECT");
        auto drums = d::getOrCreate (project);
        auto p = d::addPattern (drums, "Beat", 4, 4, 1, nullptr);
        expectEquals (d::barTicks (p), 3840);
        expectEquals (d::stepTicks (p), 240);
        d::setHit (p, d::kick, 0, 110, nullptr);
        d::setHit (p, d::snare, 960, 100, nullptr);
        d::setHit (p, d::snare, 960, 64, nullptr);          // same spot: velocity changes, no duplicate
        expectEquals (p.getNumChildren(), 2);
        expectEquals ((int) d::hitAt (p, d::snare, 960)[d::ids::vel], 64);
        d::setHit (p, d::hhClosed, 3840 + 480, 90, nullptr);
        d::setBars (p, 2, nullptr);
        d::setHit (p, d::hhClosed, 3840 + 480, 90, nullptr);
        expectEquals (p.getNumChildren(), 3);
        d::setBars (p, 1, nullptr);
        expectEquals (p.getNumChildren(), 2, "hits past the end are dropped");
        for (int i = 0; i < d::numPieces; ++i)
            expectEquals (d::pieceForGmNote (d::pieceInfo (i).gmNote), i);
        expectEquals (d::pieceForGmNote (40), (int) d::snare);
        expectEquals (d::pieceForGmNote (20), -1);

        beginTest ("compile: repeats, swing, song sections, humanize");
        {
            auto c = d::compile (drums, p, 3);
            expectEquals ((int) c.events.size(), 6);
            expectEquals (c.lengthQ, 12.0);
            expectEquals ((int) c.bars.size(), 3);
            expectEquals (c.events[2].q, 4.0);
            expectEquals (d::swungTick (480, 0.5, 2), 480 + 240);  // 8th swing
            expectEquals (d::swungTick (240, 0.5, 4), 240 + 120);  // 16th swing
            expectEquals (d::swungTick (960, 0.5, 4), 960);        // on the beat: unchanged
            auto p2 = d::addPattern (drums, "Fill", 3, 4, 1, nullptr);
            d::setHit (p2, d::tomFloor, 0, 100, nullptr);
            d::addSection (drums, p[d::ids::id].toString(), 2, -1, nullptr);
            d::addSection (drums, p2[d::ids::id].toString(), 1, -1, nullptr);
            d::addSection (drums, "missing", 4, -1, nullptr);   // ignored
            auto songC = d::compile (drums, {}, 1);
            expectEquals (songC.lengthQ, 4.0 * 2 + 3.0);
            expectEquals ((int) songC.events.size(), 5);
            expectEquals (songC.events.back().piece, (int) d::tomFloor);
            expectEquals (songC.events.back().q, 8.0);
            expectEquals (songC.bars[2].num, 3);

            drums.setProperty (d::ids::humanizeMs, 10.0, nullptr);
            drums.setProperty (d::ids::humanizeVel, 0.2, nullptr);
            auto h1 = d::compile (drums, p, 4), h2 = d::compile (drums, p, 4);
            bool varied = false;
            for (size_t i = 0; i < h1.events.size(); ++i)
            {
                expectEquals (h1.events[i].offsetSec, h2.events[i].offsetSec);  // deterministic
                expect (std::abs (h1.events[i].offsetSec) <= 0.010 + 1e-9);
                varied = varied || std::abs (h1.events[i].offsetSec) > 1e-4;
            }
            expect (varied, "humanize moves hits");
            drums.setProperty (d::ids::humanizeMs, 0.0, nullptr);
            drums.setProperty (d::ids::humanizeVel, 0.0, nullptr);
        }

        beginTest ("generator: every genre, deterministic, genre rules");
        for (int g = 0; g < d::numGenres; ++g)
        {
            d::GenOptions o;
            o.genre = g;
            o.bars = 2;
            o.seed = 7;
            auto a = d::generate (o), b = d::generate (o);
            expect (hitSet (a) == hitSet (b), juce::String ("deterministic: ") + d::genreName (g));
            expect (a.getNumChildren() >= 8, juce::String ("has hits: ") + d::genreName (g));
            expect (countLane (a, d::kick) > 0, juce::String ("kick: ") + d::genreName (g));
            for (auto h : a)
            {
                expect ((int) h[d::ids::tick] < d::lengthTicks (a));
                expect ((int) h[d::ids::vel] >= 1 && (int) h[d::ids::vel] <= 127);
            }
            o.seed = 8;
            o.complexity = 0.9;
            juce::ignoreUnused (d::generate (o));
        }
        {
            d::GenOptions o;
            o.genre = d::genreRock;
            o.fill = false;
            o.crashOnOne = false;
            o.complexity = 0.0;
            o.bars = 1;
            auto rock = d::generate (o);
            expect (d::hitAt (rock, d::snare, 960).isValid() && d::hitAt (rock, d::snare, 2880).isValid(), "backbeat on 2 and 4");
            expect (d::hitAt (rock, d::kick, 0).isValid() && d::hitAt (rock, d::kick, 1920).isValid(), "kick on 1 and 3");
            expectEquals (countLane (rock, d::hhClosed), 8, "8th hats");
            o.halfTime = true;
            auto half = d::generate (o);
            expect (d::hitAt (half, d::snare, 1920).isValid() && ! d::hitAt (half, d::snare, 960).isValid(), "half-time snare on 3");

            o = {};
            o.genre = d::genreBlast; o.complexity = 0.9; o.fill = false; o.crashOnOne = false; o.bars = 1;
            auto blast = d::generate (o);
            expectEquals (countLane (blast, d::kick), 16, "bomb blast: 16th kicks");
            expectEquals (countLane (blast, d::snare), 8, "bomb blast: 8th snares");

            o.genre = d::genreDBeat; o.complexity = 0.0;
            auto db = d::generate (o);
            for (int t : { 0, 1440, 1920 }) expect (d::hitAt (db, d::kick, t).isValid(), "d-beat kick " + juce::String (t));
            for (int t : { 960, 2880 }) expect (d::hitAt (db, d::snare, t).isValid(), "d-beat snare " + juce::String (t));

            o.genre = d::genreDjent; o.complexity = 0.0; o.bars = 3;
            auto dj = d::generate (o);
            for (auto h : dj)
                if ((int) h[d::ids::lane] == d::kick) expectEquals (((int) h[d::ids::tick] / 240) % 3, 0, "3-over-4 kick grouping");

            o = {};
            o.genre = d::genreMetal; o.bars = 2; o.fill = true; o.crashOnOne = true;
            auto withFill = d::generate (o);
            expect (d::hitAt (withFill, d::crash, 0).isValid(), "crash on 1");
            int toms = 0;
            for (auto h : withFill)
                if ((int) h[d::ids::tick] >= 3840 + 2880 && (int) h[d::ids::lane] >= d::tomHigh && (int) h[d::ids::lane] <= d::tomFloor) ++toms;
            expect (toms >= 2, "fill uses toms in the last beat");

            o.genre = d::genreJazz;
            expectEquals ((int) d::generate (o)[d::ids::grid], 3, "triplet grid for swing");
            o.tsNum = 6; o.tsDen = 8; o.genre = d::genreRock; o.bars = 1;
            auto six = d::generate (o);
            expectEquals (d::barTicks (six), 2880);
        }

        beginTest ("MIDI export / import round trip");
        {
            d::GenOptions o;
            o.genre = d::genreFunk;
            o.bars = 2;
            auto g = d::generate (o);
            juce::ValueTree proj2 ("PROJECT");
            auto dr = d::getOrCreate (proj2);
            d::patterns (dr).appendChild (g, nullptr);
            auto f = juce::File::createTempFile ("mid");
            expect (d::exportMidi (dr, g, 100.0, f).wasOk());
            auto r = d::importMidi (dr, f, nullptr);
            expect (r.result.wasOk(), r.result.getErrorMessage());
            expectEquals (r.tempo, 100.0);
            auto back = d::patternById (dr, r.firstPatternId);
            expect (back.isValid());
            expectEquals ((int) back[d::ids::bars], 2);
            std::set<std::tuple<int, int, int>> a, b;
            for (auto h : g) a.insert ({ (int) h[d::ids::lane], (int) h[d::ids::tick], (int) h[d::ids::vel] });
            for (auto h : back) b.insert ({ (int) h[d::ids::lane], (int) h[d::ids::tick], (int) h[d::ids::vel] });
            expect (a == b, "hits survive the round trip");
            f.deleteFile();
        }

        beginTest ("MIDI import: the whole file as one pattern (long files, meter changes)");
        {
            juce::ValueTree src ("PROJECT");
            auto sd = d::getOrCreate (src);
            auto mk = [&] (int genre, int bars, int num, int den, bool fill) {
                d::GenOptions o;
                o.genre = genre; o.bars = bars; o.tsNum = num; o.tsDen = den; o.fill = fill; o.crashOnOne = false;
                auto p = d::generate (o);
                d::patterns (sd).appendChild (p, nullptr);
                return p[d::ids::id].toString();
            };
            const auto a = mk (d::genreRock, 1, 4, 4, false);
            const auto b = mk (d::genreMetal, 2, 4, 4, true);
            const auto c = mk (d::genreRock, 1, 3, 4, false);
            d::addSection (sd, a, 8, -1, nullptr);
            d::addSection (sd, b, 30, -1, nullptr);
            d::addSection (sd, c, 4, -1, nullptr);
            d::addSection (sd, a, 8, -1, nullptr);
            const auto original = d::compile (sd, {}, 1);
            expectEquals ((int) original.bars.size(), 80);
            auto f = juce::File::createTempFile ("mid");
            expect (d::exportMidi (sd, {}, 140.0, f).wasOk());

            juce::ValueTree dst ("PROJECT");
            auto dd = d::getOrCreate (dst);
            auto r = d::importMidi (dd, f, nullptr);
            expect (r.result.wasOk(), r.result.getErrorMessage());
            expectEquals (r.patterns, 1, "the whole file is one pattern");
            expectEquals (r.sections, 1, "added to the empty song");
            expectEquals (r.tempo, 140.0);
            auto whole = d::patternById (dd, r.firstPatternId);
            expect (whole.isValid());
            expectEquals ((int) whole[d::ids::bars], 79, "76 bars of 4/4 + 4 of 3/4 = 79 bars of 4/4");
            expect (r.warnings.joinIntoString (" ").contains ("time signature"));
            const auto imported = d::compile (dd, whole, 1);
            expectEquals ((int) imported.events.size(), (int) original.events.size());
            std::multiset<std::pair<long long, int>> x, y;
            for (auto& e : original.events) x.insert ({ std::llround (e.q * 960), e.piece });
            for (auto& e : imported.events) y.insert ({ std::llround (e.q * 960), e.piece });
            expect (x == y, "every hit is in the same place");
            f.deleteFile();

            // notes outside the kit are reported, the rest imports
            juce::MidiMessageSequence seq;
            for (int i = 0; i < 8; ++i)
            {
                seq.addEvent (juce::MidiMessage::noteOn (10, i % 2 ? 38 : 36, (juce::uint8) 100), i * 480.0);
                seq.addEvent (juce::MidiMessage::noteOn (10, 70, (juce::uint8) 100), i * 480.0);
            }
            juce::MidiFile mf;
            mf.setTicksPerQuarterNote (480);
            mf.addTrack (seq);
            auto g = juce::File::createTempFile ("mid");
            { juce::FileOutputStream os (g); mf.writeTo (os); }
            juce::ValueTree p3 ("PROJECT");
            auto d3 = d::getOrCreate (p3);
            auto r3 = d::importMidi (d3, g, nullptr);
            expect (r3.result.wasOk());
            expectEquals (r3.hits, 8);
            expect (r3.warnings.joinIntoString (" ").contains ("70"), "unmapped note numbers are listed");
            g.deleteFile();
        }

        beginTest ("play-along: drum song follows the tab score's tempo changes");
        {
            juce::ValueTree pr ("PROJECT");
            auto sc = ff::tab::getOrCreateScore (pr);
            sc.setProperty (ff::tab::ids::tempo, 120.0, nullptr);
            ff::tab::measures (sc).getChild (1).setProperty (ff::tab::ids::tempo, 60.0, nullptr);
            auto dr = d::getOrCreate (pr);
            auto pat = d::addPattern (dr, "K", 4, 4, 1, nullptr);
            d::setHit (pat, d::kick, 0, 100, nullptr);
            d::addSection (dr, pat[d::ids::id].toString(), 3, -1, nullptr);
            TabSequence seq;
            addDrumSong (seq, dr, ff::tab::tempoMap (sc, 1.0));
            expectEquals ((int) seq.drumHits.size(), 3);
            expectWithinAbsoluteError (seq.drumHits[1].sec, 2.0, 1e-9);
            expectWithinAbsoluteError (seq.drumHits[2].sec, 6.0, 1e-9);
            TabSequence slow;
            addDrumSong (slow, dr, ff::tab::tempoMap (sc, 0.5));
            expectWithinAbsoluteError (slow.drumHits[2].sec, 12.0, 1e-9);

            // a tab longer than the drum song: the song repeats to the end of the tab, cut at the tab's end
            TabSequence longer;
            const auto map = ff::tab::tempoMap (sc, 1.0);
            longer.lengthSec = map.secondsAt (4.0 * 7.5);   // 7.5 bars of tab
            addDrumSong (longer, dr, map);
            expectEquals ((int) longer.drumHits.size(), 8);  // hits on bars 1..8 (the 9th would start after the tab ends)
            for (int i = 1; i < (int) longer.drumHits.size(); ++i)
                expect (longer.drumHits[(size_t) i].sec > longer.drumHits[(size_t) i - 1].sec);

            // no song arranged: the selected pattern loops
            juce::ValueTree pr2 ("PROJECT");
            auto sc2 = ff::tab::getOrCreateScore (pr2);
            auto dr2 = d::getOrCreate (pr2);
            auto pat2 = d::addPattern (dr2, "P", 4, 4, 1, nullptr);
            d::setHit (pat2, d::snare, 0, 100, nullptr);
            dr2.setProperty (d::ids::selected, pat2[d::ids::id].toString(), nullptr);
            TabSequence s2;
            const auto map2 = ff::tab::tempoMap (sc2, 1.0);
            s2.lengthSec = map2.secondsAt (16.0);
            addDrumSong (s2, dr2, map2);
            expectEquals ((int) s2.drumHits.size(), 4);
        }

        beginTest ("synth: every piece sounds, stays bounded, hats choke");
        {
            constexpr double sr = 48000.0;
            for (int piece = 0; piece < d::numPieces; ++piece)
            {
                juce::AudioBuffer<float> buf;
                DrumSynth::renderOffline ({ { 0.01, piece, 1.0f } }, sr, 1.0, 1.0f, buf);
                const float pk = peak (buf, 0, buf.getNumSamples());
                expect (pk > 0.05f && pk < 1.6f, juce::String (d::pieceInfo (piece).name) + " peak " + juce::String (pk));
                expect (peak (buf, 0, (int) (0.009 * sr)) == 0.0f, "silent before the hit");
                bool finite = true;
                for (int i = 0; i < buf.getNumSamples(); ++i) finite = finite && std::isfinite (buf.getSample (0, i));
                expect (finite);
            }
            // a kick is low, a closed hat is bright: count zero crossings in the first 30 ms
            auto zc = [&] (int piece) {
                juce::AudioBuffer<float> b;
                DrumSynth::renderOffline ({ { 0.0, piece, 1.0f } }, sr, 0.05, 1.0f, b);
                int n = 0;
                for (int i = 1; i < (int) (0.03 * sr); ++i) n += (b.getSample (0, i - 1) < 0) != (b.getSample (0, i) < 0) ? 1 : 0;
                return n;
            };
            expect (zc (d::kick) < 20, "kick is low: " + juce::String (zc (d::kick)));
            expect (zc (d::hhClosed) > 200, "hat is bright: " + juce::String (zc (d::hhClosed)));
            juce::AudioBuffer<float> open, choked;
            DrumSynth::renderOffline ({ { 0.0, d::hhOpen, 1.0f } }, sr, 1.0, 1.0f, open);
            DrumSynth::renderOffline ({ { 0.0, d::hhOpen, 1.0f }, { 0.2, d::hhPedal, 0.01f } }, sr, 1.0, 1.0f, choked);
            expect (energy (choked, (int) (0.3 * sr), (int) (0.6 * sr)) < 0.01 * energy (open, (int) (0.3 * sr), (int) (0.6 * sr)),
                    "pedal hat chokes the open hat");
            // softer hits are quieter
            juce::AudioBuffer<float> soft, loud;
            DrumSynth::renderOffline ({ { 0.0, d::snare, 0.3f } }, sr, 0.3, 1.0f, soft);
            DrumSynth::renderOffline ({ { 0.0, d::snare, 1.0f } }, sr, 0.3, 1.0f, loud);
            expect (peak (soft, 0, soft.getNumSamples()) < 0.3f * peak (loud, 0, loud.getNumSamples()));
        }

        beginTest ("sample kits: name matching, sample playback, engine hand-over");
        {
            const std::pair<const char*, int> names[] = {
                { "Kick_01.wav", d::kick }, { "BD hard.wav", d::kick }, { "Snare Center.wav", d::snare }, { "Rimshot.wav", d::snare }, { "Rim Click.wav", d::sideStick },
                { "HiHat Closed.wav", d::hhClosed }, { "hh_open_2.wav", d::hhOpen }, { "Hat Pedal.flac", d::hhPedal },
                { "Tom 1.wav", d::tomHigh }, { "tom-low.wav", d::tomLow }, { "Floor Tom.wav", d::tomFloor }, { "Crash 18.wav", d::crash },
                { "crash2.wav", d::crash2 }, { "Ride Bell.wav", d::rideBell }, { "ride.wav", d::ride }, { "China.wav", d::china },
                { "splash 10in.wav", d::splash }, { "Cowbell.wav", d::cowbell }, { "Clap.wav", d::clap }, { "Side Stick.wav", d::sideStick },
                { "vocals.wav", -1 }, { "Tom Mid.wav", d::tomMid } };
            for (auto& [n, piece] : names) expectEquals (d::pieceForSampleName (n), piece, n);

            juce::ValueTree pr ("PROJECT");
            auto dr = d::getOrCreate (pr);
            d::setSamplePath (dr, d::snare, "Audio/DrumKit/snare.wav", nullptr);
            expectEquals (d::samplePath (dr, d::snare), juce::String ("Audio/DrumKit/snare.wav"));
            expect (d::samplePath (dr, d::kick).isEmpty());
            d::setSamplePath (dr, d::snare, {}, nullptr);
            expect (d::samplePath (dr, d::snare).isEmpty());

            // a sample plays back as recorded (velocity scales it), stereo kept
            constexpr double sr = 48000.0;
            juce::AudioBuffer<float> smp (2, 4800);
            for (int i = 0; i < 4800; ++i) { smp.setSample (0, i, 0.5f * std::sin (i * 0.05f)); smp.setSample (1, i, 0.25f); }
            DrumSynth::SampleSet set {};
            set[(size_t) d::snare] = &smp;
            juce::AudioBuffer<float> out;
            DrumSynth::renderOffline ({ { 0.5, d::snare, 1.0f } }, sr, 1.0, 1.0f, out, &set);
            const int at = 24000;
            expectEquals (out.getSample (0, at - 1), 0.0f);
            expectWithinAbsoluteError (out.getSample (1, at + 100), 0.25f * 0.9f, 1e-5f);
            expectWithinAbsoluteError (out.getSample (0, at + 100), 0.5f * std::sin (100 * 0.05f) * 0.9f, 1e-5f);
            expectEquals (out.getSample (0, at + 4900), 0.0f, "ends with the sample");

            AudioEngine e;
            e.prepare (sr, 256);
            e.tabMode = true;
            auto b = std::make_unique<juce::AudioBuffer<float>> (1, 1000);
            b->clear();
            b->setSample (0, 10, 0.8f);
            expect (e.setDrumSample (d::kick, std::move (b)));
            auto seq = std::make_unique<TabSequence>();
            seq->drumHits.push_back ({ 0.0, d::kick, 1.0f });
            seq->lengthSec = 1.0;
            e.submitTabSequence (std::move (seq));
            std::vector<float> l (256), r (256);
            float* outs[2] = { l.data(), r.data() };
            e.play();
            e.process (nullptr, 0, outs, 2, 256);
            float pk = 0.0f;
            int pkAt = -1;
            for (int i = 0; i < 256; ++i) if (std::abs (l[(size_t) i]) > pk) { pk = std::abs (l[(size_t) i]); pkAt = i; }
            expectEquals (pkAt, 10, "the sample is what plays");
            expect (e.setDrumSample (d::kick, nullptr));   // back to the built-in kick; old buffer retired
            e.process (nullptr, 0, outs, 2, 256);
            e.collectGarbage();
        }

        beginTest ("per-drum volume, pan, mute and solo");
        {
            juce::ValueTree d (drums::ids::DRUMS);
            drums::setPieceMix (d, drums::snare, "gain", -6.0206, nullptr);
            drums::setPieceMix (d, drums::kick, "pan", -1.0, nullptr);
            float g[drums::numPieces], p[drums::numPieces];
            drums::effectiveMix (d, g, p);
            expectWithinAbsoluteError (g[drums::snare], 0.5f, 1.0e-3f);
            expectWithinAbsoluteError (p[drums::kick], -1.0f, 1.0e-6f);
            drums::setPieceMix (d, drums::kick, "solo", true, nullptr);
            drums::effectiveMix (d, g, p);
            expect (g[drums::kick] > 0.9f && g[drums::snare] == 0.0f, "solo silences the others");
            drums::setPieceMix (d, drums::kick, "mute", true, nullptr);
            drums::effectiveMix (d, g, p);
            expectEquals (g[drums::kick], 0.0f);

            DrumSynth::Mix mix;
            for (int i = 0; i < DrumSynth::maxPieces; ++i) { mix.gain[(size_t) i] = 1.0f; mix.pan[(size_t) i] = 0.0f; }
            mix.pan[drums::kick] = -1.0f;
            juce::AudioBuffer<float> out;
            DrumSynth::renderOffline ({ { 0.0, drums::kick, 1.0f } }, 48000.0, 0.5, 0.8f, out, nullptr, &mix);
            expect (out.getMagnitude (0, 0, 24000) > 0.05f && out.getMagnitude (1, 0, 24000) < 1.0e-4f, "hard-left kick");
            mix.gain[drums::kick] = 0.0f;
            DrumSynth::renderOffline ({ { 0.0, drums::kick, 1.0f } }, 48000.0, 0.5, 0.8f, out, nullptr, &mix);
            expect (out.getMagnitude (0, 24000) < 1.0e-6f, "zero volume is silent");
        }

        beginTest ("engine plays drum sequences on time");
        {
            juce::ValueTree proj3 ("PROJECT");
            auto dr = d::getOrCreate (proj3);
            auto pat = d::addPattern (dr, "Q", 4, 4, 1, nullptr);
            for (int b = 0; b < 4; ++b) d::setHit (pat, d::sideStick, b * 960, 127, nullptr);
            auto seq = buildDrumSequence (dr, pat, 120.0);
            expectEquals ((int) seq->drumHits.size(), 4);
            expectWithinAbsoluteError (seq->drumHits[3].sec, 1.5, 1e-9);
            expectEquals ((int) seq->clicks.size(), 4);
            expectWithinAbsoluteError (seq->lengthSec, 2.0, 1e-9);

            constexpr double sr = 48000.0;
            AudioEngine e;
            e.prepare (sr, 512);
            e.tabMode = true;
            e.submitTabSequence (std::move (seq));
            std::vector<float> l (512), r (512), all;
            float* outs[2] = { l.data(), r.data() };
            e.play();
            for (int i = 0; i < (int) (2.0 * sr / 512); ++i)
            {
                e.process (nullptr, 0, outs, 2, 512);
                all.insert (all.end(), l.begin(), l.end());
            }
            std::vector<int> onsets;
            for (int i = 0; i < (int) all.size(); ++i)
            {
                if (std::abs (all[(size_t) i]) <= 1.0e-3f) continue;
                bool quietBefore = true;   // an onset follows at least 10 ms below the threshold
                for (int k = juce::jmax (0, i - 480); k < i && quietBefore; ++k) quietBefore = std::abs (all[(size_t) k]) <= 1.0e-3f;
                if (quietBefore && (onsets.empty() || i - onsets.back() > 4800)) onsets.push_back (i);
            }
            expectEquals ((int) onsets.size(), 4);
            for (int i = 0; i < juce::jmin (4, (int) onsets.size()); ++i)
                expect (std::abs (onsets[(size_t) i] - i * 24000) <= 3, "hit " + juce::String (i) + " at " + juce::String (onsets[(size_t) i]));
            e.collectGarbage();
        }

        beginTest ("a plugin instrument plays the drum hits as MIDI, on time, instead of the built-in kit");
        {
            juce::ValueTree proj4 ("PROJECT");
            auto dr = d::getOrCreate (proj4);
            auto pat = d::addPattern (dr, "Q", 4, 4, 1, nullptr);
            for (int b = 0; b < 4; ++b) d::setHit (pat, d::sideStick, b * 960, 127, nullptr);
            constexpr double sr = 48000.0;
            auto run = [&] (bool withInstrument)
            {
                AudioEngine e;
                e.prepare (sr, 512);
                e.tabMode = true;
                std::shared_ptr<fx::InstrumentSlot> slot;
                if (withInstrument)
                {
                    auto p = fx::builtin::create ("fretforge:synth");
                    fx::preparePlugin (*p, sr, 512);
                    slot = std::make_shared<fx::InstrumentSlot>();
                    slot->prepare (sr, 512);
                    slot->setPlugin (std::shared_ptr<juce::AudioPluginInstance> (p.release()));
                    e.setDrumInstrument (slot);
                }
                e.submitTabSequence (buildDrumSequence (dr, pat, 120.0));
                std::vector<float> l (512), r (512), all;
                float* outs[2] = { l.data(), r.data() };
                e.returnToZero();   // play from the start: a jump in the same block as the first hit must not eat it
                e.play();
                for (int i = 0; i < (int) (2.0 * sr / 512); ++i)
                {
                    e.process (nullptr, 0, outs, 2, 512);
                    all.insert (all.end(), l.begin(), l.end());
                }
                e.setDrumInstrument (nullptr);
                e.collectGarbage();
                return all;
            };
            const auto builtIn = run (false), plugin = run (true);
            // the synth rings on between hits, so look for new energy in the 10 ms after each hit instead of a silent gap before it
            auto peak = [&] (int from, int n) { float m = 0.0f; for (int i = from; i < from + n && i < (int) plugin.size(); ++i) m = juce::jmax (m, std::abs (plugin[(size_t) i])); return m; };
            for (int i = 0; i < 4; ++i)
                expect (peak (i * 24000, 2400) > 1.0e-3f, "plugin sound after hit " + juce::String (i));
            expect (peak (0, 20) <= 0.5f && peak (0, 2400) > 1.0e-3f, "the first hit sounds right away");
            expect (builtIn != plugin, "the sound comes from the plugin, not the built-in kit");
        }
    }
};

static DrumTests drumTests;

// Listening aid: FF_DRUM_DEMO=/path/demo.wav ff_tests renders a few generated grooves.
static struct DrumDemo
{
    DrumDemo()
    {
        const auto path = juce::SystemStats::getEnvironmentVariable ("FF_DRUM_DEMO", {});
        if (path.isEmpty()) return;
        juce::ScopedJuceInitialiser_GUI init;
        std::vector<DrumHit> hits;
        double t = 0.0;
        const std::pair<int, double> styles[] = { { d::genreRock, 110 }, { d::genreFunk, 100 }, { d::genreMetal, 140 },
                                                   { d::genreDBeat, 170 }, { d::genreBlackMetal, 190 }, { d::genreDjent, 120 } };
        juce::ValueTree project ("PROJECT");
        auto dr = d::getOrCreate (project);
        for (auto [g, bpm] : styles)
        {
            d::GenOptions o;
            o.genre = g;
            o.bars = 2;
            o.complexity = 0.6;
            auto p = d::generate (o);
            const auto c = d::compile (dr, p, 2);
            for (auto& e : c.events) hits.push_back ({ t + e.q * 60.0 / bpm, e.piece, e.velocity });
            t += c.lengthQ * 60.0 / bpm + 0.5;
        }
        juce::AudioBuffer<float> buf;
        DrumSynth::renderOffline (hits, 44100.0, t + 1.0, 0.8f, buf);
        juce::File f (path);
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
        if (auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16)))
            w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }
} drumDemo;
