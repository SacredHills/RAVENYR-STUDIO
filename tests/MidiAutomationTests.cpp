#include <juce_audio_formats/juce_audio_formats.h>
#include "core/ProjectFile.h"
#include "core/RigModel.h"
#include "core/Timeline.h"
#include "core/MidiClip.h"
#include "core/Automation.h"
#include "engine/AudioCache.h"
#include "engine/GraphMixer.h"
#include "engine/Exporter.h"
#include "engine/Recorder.h"
#include "engine/fx/BuiltinPlugins.h"
#include "engine/fx/InsertChain.h"
#include "engine/fx/PluginHost.h"
#include <cmath>

using namespace ff;
namespace tl = ff::timeline;
namespace mc = ff::midiclip;
namespace au = ff::automation;

namespace
{
constexpr double sr = 48000.0;

/** Plays the arrangement from 0 like the engine does (no inputs). */
struct PlaySource final : GraphMixer::Source
{
    juce::int64 pos = 0;
    float constant = 0.0f;   // audio tracks play this level
    double masterTime() const noexcept override { return (double) pos / sr; }
    bool fill (int, PlayTrack& t, float* L, float* R, int n) noexcept override
    {
        BlockTiming bt;
        bt.from = pos + readAhead (t);
        bt.rolling = true;
        bt.timeSec = (double) pos / sr;
        prepareTrackBlock (t, bt, n);
        if (! t.audible) return false;
        if (t.kind == tl::TrackKind::audio)
            for (int i = 0; i < n; ++i) { L[i] += constant; R[i] += constant; }
        return true;
    }
};

/** Renders `seconds` of the graph; returns the left channel. */
std::vector<float> render (PlaybackGraph& g, double seconds, float constant = 0.0f, double startAt = 0.0)
{
    const auto start = (juce::int64) (startAt * sr);
    GraphMixer m;
    m.prepare (256, sr);
    PlaySource src;
    src.constant = constant;
    std::vector<float> out;
    std::vector<float> L (256), R (256);
    for (juce::int64 p = 0; p < (juce::int64) (seconds * sr); p += 256)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        src.pos = start + p;
        m.mix (g, src, L.data(), R.data(), 256);
        out.insert (out.end(), L.begin(), L.end());
    }
    return out;
}

float peak (const std::vector<float>& x, double from, double to)
{
    float p = 0.0f;
    for (auto i = (size_t) (from * sr); i < (size_t) (to * sr) && i < x.size(); ++i) p = juce::jmax (p, std::abs (x[i]));
    return p;
}

std::shared_ptr<fx::InstrumentSlot> builtinInstrument (const char* id)
{
    auto p = fx::builtin::create (id);
    fx::preparePlugin (*p, sr, 512);
    auto slot = std::make_shared<fx::InstrumentSlot>();
    slot->prepare (sr, 512);
    slot->setPlugin (std::shared_ptr<juce::AudioPluginInstance> (p.release()));
    return slot;
}
}

class MidiAutomationTests final : public juce::UnitTest
{
public:
    MidiAutomationTests() : juce::UnitTest ("MIDI, instruments and automation", "Engine") {}

    void runTest() override
    {
        AudioCache cache;
        const juce::File noBundle;

        beginTest ("MIDI clip notes round-trip");
        {
            std::vector<mc::Note> notes { { 0.0, 0.5, 60, 100 }, { 0.5, 0.25, 64, 80 }, { 1.0, 1.0, 67, 127 } };
            auto c = mc::createMidiClip ("Riff", 2.0, notes, { { 0.2, true }, { 1.8, false } });
            expect (mc::isMidiClip (c));
            expectWithinAbsoluteError ((double) c[tl::ids::length], 2.0, 1.0e-9);
            const auto back = mc::notesOf (c);
            expectEquals ((int) back.size(), 3);
            expectEquals (back[1].pitch, 64);
            expectEquals (back[2].velocity, 127);
            expectWithinAbsoluteError (back[1].start, 0.5, 1.0e-5);
            expectEquals ((int) mc::pedalOf (c).size(), 2);
            juce::ValueTree track ("TRACK");
            track.setProperty (tl::ids::kind, "instrument", nullptr);
            expect (tl::acceptsClip (track, c));
            expect (! tl::acceptsClip (tl::createTrack ("a", 0), c));
        }

        beginTest ("instrument tracks, MIDI clips and automation survive save / load");
        {
            auto root = project::createDefault ("T");
            auto tracks = root.getChildWithName (project::ids::TRACKS);
            auto t = tl::createBusTrack ("Keys", 0, tl::TrackKind::instrument);
            juce::ValueTree in (mc::INSTRUMENT);
            in.setProperty (rig::ids::desc, "<x/>", nullptr);
            in.setProperty (rig::ids::name, "Synth", nullptr);
            t.appendChild (in, nullptr);
            t.appendChild (mc::createMidiClip ("c", 1.0, { { 0.0, 0.5, 60, 100 } }), nullptr);
            au::setPoints (au::lane (t, au::volumeParam, true, nullptr), { { 0.0, -3.0f }, { 2.0, -12.0f } }, nullptr);
            tracks.appendChild (t, nullptr);
            const auto loaded = project::parse (project::serialise (root));
            expect (loaded.result.wasOk(), loaded.result.getErrorMessage());
            const auto t2 = loaded.tree.getChildWithName (project::ids::TRACKS).getChild (0);
            expect (tl::isInstrumentTrack (t2));
            expectEquals (t2.getChildWithName (mc::INSTRUMENT)[rig::ids::name].toString(), juce::String ("Synth"));
            const auto clip = t2.getChildWithName (tl::ids::CLIP);
            expect (mc::isMidiClip (clip), "the MIDI clip is kept");
            expectEquals ((int) mc::notesOf (clip).size(), 1);
            expectEquals ((int) au::pointsOf (au::lane (t2, au::volumeParam, false, nullptr)).size(), 2);
        }

        beginTest ("MIDI file import: tracks, channels, tempo");
        {
            juce::MidiFile mf;
            mf.setTicksPerQuarterNote (480);
            juce::MidiMessageSequence conductor, piano, drums;
            conductor.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0);   // 120 BPM
            conductor.addEvent (juce::MidiMessage::timeSignatureMetaEvent (3, 4), 0);
            piano.addEvent (juce::MidiMessage::textMetaEvent (3, "Piano"), 0);
            piano.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            piano.addEvent (juce::MidiMessage::noteOff (1, 60), 480);
            piano.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90), 480);
            piano.addEvent (juce::MidiMessage::noteOff (1, 64), 960);
            drums.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 110), 0);
            drums.addEvent (juce::MidiMessage::noteOff (10, 36), 120);
            mf.addTrack (conductor);
            mf.addTrack (piano);
            mf.addTrack (drums);
            auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ff_test.mid");
            f.deleteFile();
            {
                juce::FileOutputStream os (f);
                mf.writeTo (os);
            }
            const auto r = mc::readMidiFile (f);
            expect (r.error.isEmpty(), r.error);
            expectEquals ((int) r.tracks.size(), 2);
            expectWithinAbsoluteError (r.firstTempo, 120.0, 0.01);
            expectEquals (r.tsNum, 3);
            if (r.tracks.size() == 2)
            {
                expectEquals (r.tracks[0].name, juce::String ("Piano"));
                expectEquals ((int) r.tracks[0].notes.size(), 2);
                expectWithinAbsoluteError (r.tracks[0].notes[1].start, 0.5, 1.0e-6);   // beat 2 at 120 BPM
                expectWithinAbsoluteError (r.tracks[0].notes[1].length, 0.5, 1.0e-6);
                expectEquals (r.tracks[1].channel, 10);
            }
            f.deleteFile();
        }

        beginTest ("MIDI file export round-trips, and a clip exports only what it plays");
        {
            auto clip = mc::createMidiClip ("c", 4.0, { { 0.25, 0.5, 60, 100 }, { 1.0, 0.75, 67, 80 }, { 3.0, 0.5, 72, 90 } },
                                            { { 1.0, true }, { 1.5, false } });
            clip.setProperty (tl::ids::offset, 0.5, nullptr);   // trimmed: the first note is cut off
            clip.setProperty (tl::ids::length, 2.0, nullptr);   // ... and the last
            std::vector<mc::Note> ns;
            std::vector<mc::Pedal> ps;
            mc::clipContent (clip, ns, ps);
            expectEquals ((int) ns.size(), 1);
            if (ns.size() == 1) expectWithinAbsoluteError (ns[0].start, 0.5, 1.0e-9);
            expectEquals ((int) ps.size(), 2);

            auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ff_export_test.mid");
            const std::vector<mc::Note> src { { 0.0, 0.5, 60, 100 }, { 0.75, 0.25, 64, 70 }, { 1.5, 1.0, 67, 127 } };
            const auto err = mc::writeMidiFile (f, "Keys", src, { { 0.0, true }, { 2.0, false } }, 90.0, 3, 4, 1);
            expect (err.isEmpty(), err);
            const auto r = mc::readMidiFile (f);
            expect (r.error.isEmpty(), r.error);
            expectWithinAbsoluteError (r.firstTempo, 90.0, 0.01);
            expectEquals (r.tsNum, 3);
            expectEquals ((int) r.tracks.size(), 1);
            if (r.tracks.size() == 1)
            {
                expectEquals (r.tracks[0].name, juce::String ("Keys"));
                expectEquals ((int) r.tracks[0].notes.size(), 3);
                for (size_t i = 0; i < juce::jmin ((size_t) 3, r.tracks[0].notes.size()); ++i)
                {
                    expectWithinAbsoluteError (r.tracks[0].notes[i].start, src[i].start, 1.0e-3);
                    expectWithinAbsoluteError (r.tracks[0].notes[i].length, src[i].length, 1.0e-3);
                    expectEquals (r.tracks[0].notes[i].pitch, src[i].pitch);
                    expectEquals (r.tracks[0].notes[i].velocity, src[i].velocity);
                }
                expectEquals ((int) r.tracks[0].pedal.size(), 2);
            }
            f.deleteFile();
        }

        beginTest ("MIDI recording turns key presses into notes");
        {
            MidiRecorder rec;
            const juce::uint8 on60[] { 0x90, 60, 100 }, off60[] { 0x80, 60, 0 }, on64[] { 0x90, 64, 80 }, off64z[] { 0x90, 64, 0 },
                              ped[] { 0xB0, 64, 127 }, on67[] { 0x90, 67, 90 };
            rec.push (48000, on60, 3);
            rec.push (52000, ped, 3);
            rec.push (72000, on64, 3);
            rec.push (96000, off60, 3);
            rec.push (100000, off64z, 3);   // note-on with velocity 0 = note-off
            rec.push (120000, on67, 3);     // still held when recording stops
            rec.drain();
            expectEquals ((int) rec.getEvents().size(), 6);
            std::vector<MidiRecorder::Note> ns;
            std::vector<MidiRecorder::Pedal> ps;
            MidiRecorder::toNotes (rec.getEvents(), 48000.0, 0.1, 3.0, ns, ps);
            expectEquals ((int) ns.size(), 3);
            if (ns.size() == 3)
            {
                expectEquals (ns[0].pitch, 60);
                expectWithinAbsoluteError (ns[0].start, 0.9, 1.0e-9);   // shifted back by the latency
                expectWithinAbsoluteError (ns[0].length, 1.0, 1.0e-9);
                expectEquals (ns[0].velocity, 100);
                expectEquals (ns[1].pitch, 64);
                expectWithinAbsoluteError (ns[1].length, 28000.0 / 48000.0, 1.0e-9);
                expectEquals (ns[2].pitch, 67);
                expectWithinAbsoluteError (ns[2].start + ns[2].length, 3.0, 1.0e-9);   // held notes end where recording stopped
            }
            expectEquals ((int) ps.size(), 1);
        }

        beginTest ("a rebuilt graph carries on: held notes are not played again");
        {
            juce::ValueTree tracks ("TRACKS");
            auto t = tl::createBusTrack ("Keys", 0, tl::TrackKind::instrument);
            tracks.appendChild (t, nullptr);
            t.appendChild (mc::createMidiClip ("c", 0.0, { { 0.0, 2.0, 60, 120 } }), nullptr);
            GraphBuildOptions o;
            o.sampleRate = sr;
            o.instrumentFor = [&] (const juce::ValueTree&) { return builtinInstrument ("fretforge:synth"); };
            auto g1 = buildPlaybackGraph (tracks, noBundle, cache, o);
            auto g2 = buildPlaybackGraph (tracks, noBundle, cache, o);
            auto& a = g1->tracks[0];
            a.midiWasRolling = true;
            a.midiNextPos = 24000;
            a.prevGainL = 0.5f;
            g2->tracks[0].takeStateFrom (a);
            expect (g2->tracks[0].key == a.key && a.key != 0, "tracks are matched by id");
            expect (g2->tracks[0].midiWasRolling && g2->tracks[0].midiNextPos == 24000 && juce::exactlyEqual (g2->tracks[0].prevGainL, 0.5f));
            BlockTiming bt;
            bt.from = 24000;
            bt.rolling = true;
            prepareTrackBlock (g2->tracks[0], bt, 256);
            expect (g2->tracks[0].midiBuffer.isEmpty(), "no re-triggered note");
        }

        beginTest ("instrument track plays its MIDI clip (synth and GM drums), in time");
        {
            for (auto* id : { "fretforge:synth", "fretforge:drumkit" })
            {
                juce::ValueTree tracks ("TRACKS");
                auto t = tl::createBusTrack ("Keys", 0, tl::TrackKind::instrument);
                tracks.appendChild (t, nullptr);
                const bool drums = juce::String (id).contains ("drum");
                t.appendChild (mc::createMidiClip ("c", 0.0, { { 0.0, 0.1, drums ? 36 : 48, 120 }, { 0.25, 0.3, drums ? 38 : 57, 120 } }), nullptr);
                GraphBuildOptions o;
                o.sampleRate = sr;
                o.instrumentFor = [&] (const juce::ValueTree&) { return builtinInstrument (id); };
                auto g = buildPlaybackGraph (tracks, noBundle, cache, o);
                expectEquals ((int) g->tracks[0].midiEvents.size(), 4, id);
                const auto out = render (*g, 2.0);
                expect (peak (out, 0.0, 0.05) > 0.02f, juce::String (id) + ": the very first note (at 0) sounds");
                expect (peak (out, 0.25, 0.4) > 0.02f, juce::String (id) + " sounds: " + juce::String (peak (out, 0.25, 0.4)));
                expect (peak (out, 1.8, 2.0) < peak (out, 0.25, 0.4) * 0.05f, "and stops after the note");
            }
        }

        beginTest ("starting inside a held note plays it (note chase)");
        {
            juce::ValueTree tracks ("TRACKS");
            auto t = tl::createBusTrack ("Keys", 0, tl::TrackKind::instrument);
            tracks.appendChild (t, nullptr);
            t.appendChild (mc::createMidiClip ("c", 0.0, { { 0.0, 1.5, 60, 120 } }), nullptr);
            GraphBuildOptions o;
            o.sampleRate = sr;
            o.instrumentFor = [&] (const juce::ValueTree&) { return builtinInstrument ("fretforge:synth"); };
            auto g = buildPlaybackGraph (tracks, noBundle, cache, o);
            const auto out = render (*g, 1.0, 0.0f, 0.4);
            expect (peak (out, 0.0, 0.05) > 0.02f, "the held note sounds from the start point");
            auto g2 = buildPlaybackGraph (tracks, noBundle, cache, o);
            const auto late = render (*g2, 0.5, 0.0f, 1.6);
            expect (peak (late, 0.1, 0.5) < 0.001f, "a note that has ended is not chased");
        }

        beginTest ("volume, pan and plugin automation");
        {
            juce::ValueTree tracks ("TRACKS");
            auto t = tl::createTrack ("Gtr", 0);
            tracks.appendChild (t, nullptr);
            au::setPoints (au::lane (t, au::volumeParam, true, nullptr), { { 0.0, 0.0f }, { 1.0, -6.0206f }, { 2.0, -60.0f } }, nullptr);
            auto g = buildPlaybackGraph (tracks, noBundle, cache, { sr });
            const auto out = render (*g, 2.5, 0.5f);
            expectWithinAbsoluteError (out[(size_t) (0.01 * sr)], 0.5f, 0.01f);
            expectWithinAbsoluteError (out[(size_t) (1.0 * sr) + 256], 0.25f, 0.01f);
            expect (std::abs (out[(size_t) (2.2 * sr)]) < 1.0e-4f, "-60 dB is silence");

            au::setPoints (au::lane (t, au::volumeParam, false, nullptr), {}, nullptr);
            au::setPoints (au::lane (t, au::panParam, true, nullptr), { { 0.0, -1.0f } }, nullptr);
            g = buildPlaybackGraph (tracks, noBundle, cache, { sr });
            {
                GraphMixer m;
                m.prepare (256, sr);
                PlaySource src;
                src.constant = 0.5f;
                std::vector<float> L (256, 0.0f), R (256, 0.0f);
                m.mix (*g, src, L.data(), R.data(), 256);
                expect (std::abs (R[255]) < 1.0e-4f && L[255] > 0.6f, "hard left");
            }

            // a plugin parameter: the EQ's output gain, by its insert slot and parameter index
            au::setPoints (au::lane (t, au::panParam, false, nullptr), {}, nullptr);
            auto eq = std::shared_ptr<juce::AudioPluginInstance> (fx::builtin::create ("fretforge:eq").release());
            fx::preparePlugin (*eq, sr, 512);
            int outIndex = -1;
            for (int k = 0; k < eq->getParameters().size(); ++k)
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (eq->getParameters()[k]); rp != nullptr && rp->getParameterID() == "out") outIndex = k;
            auto* outParam = dynamic_cast<juce::RangedAudioParameter*> (eq->getParameters()[outIndex]);
            const float plus6 = outParam->convertTo0to1 (6.0206f);
            au::setPoints (au::lane (t, au::pluginParam (0, outIndex), true, nullptr), { { 0.0, plus6 } }, nullptr);
            auto chain = std::make_shared<fx::InsertChain>();
            chain->prepare (sr, 512);
            chain->setPlugin (0, eq);
            std::array<int, fx::InsertChain::maxSlots> order {};
            chain->setOrder (order, 1);
            GraphBuildOptions o;
            o.sampleRate = sr;
            o.insertsFor = [chain] (const juce::ValueTree&) { return chain; };
            g = buildPlaybackGraph (tracks, noBundle, cache, o);
            const auto o2 = render (*g, 0.5, 0.25f);
            expectWithinAbsoluteError (o2[(size_t) (0.4 * sr)], 0.5f, 0.01f);   // +6 dB from the automation
        }

        beginTest ("master pan automation");
        {
            juce::ValueTree tracks ("TRACKS");
            auto t = tl::createTrack ("Gtr", 0);
            tracks.appendChild (t, nullptr);
            juce::ValueTree master (project::ids::MASTER);
            au::setPoints (au::lane (master, au::panParam, true, nullptr), { { 0.0, 1.0f } }, nullptr);   // hard right
            GraphBuildOptions o;
            o.sampleRate = sr;
            o.masterNode = master;
            auto g = buildPlaybackGraph (tracks, noBundle, cache, o);
            GraphMixer m;
            m.prepare (256, sr);
            PlaySource src;
            src.constant = 0.5f;
            std::vector<float> L (256, 0.0f), R (256, 0.0f);
            for (int b = 0; b < 4; ++b)
            {
                std::fill (L.begin(), L.end(), 0.0f);
                std::fill (R.begin(), R.end(), 0.0f);
                src.pos = b * 256;
                m.mix (*g, src, L.data(), R.data(), 256);
            }
            expect (std::abs (L[255]) < 1.0e-4f && R[255] > 0.6f, "master pan hard right");
        }

        beginTest ("export renders instrument tracks");
        {
            struct BuiltinHost final : fx::PluginHost
            {
                void createAsync (const juce::String& d, double r, int b, std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> done) override
                { juce::String e; done (createBlocking (d, r, b, e), e); }
                std::unique_ptr<juce::AudioPluginInstance> createBlocking (const juce::String& d, double r, int b, juce::String&) override
                {
                    juce::PluginDescription pd;
                    if (auto x = juce::parseXML (d)) pd.loadFromXml (*x);
                    auto p = fx::builtin::create (pd.fileOrIdentifier);
                    if (p != nullptr) p->setRateAndBufferSizeDetails (r, b);
                    return p;
                }
            } host;
            fx::setPluginHost (&host);
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ff_midi_test.ffproj");
            dir.deleteRecursively();
            project::Bundle::createStructure (dir);
            auto root = project::createDefault ("T");
            auto tracks = root.getChildWithName (project::ids::TRACKS);
            auto t = tl::createBusTrack ("Keys", 0, tl::TrackKind::instrument);
            juce::ValueTree in (mc::INSTRUMENT);
            in.setProperty (rig::ids::desc, fx::builtin::descriptionXml ("fretforge:synth"), nullptr);
            in.setProperty (rig::ids::name, "Basic Synth", nullptr);
            t.appendChild (in, nullptr);
            t.appendChild (mc::createMidiClip ("c", 0.0, { { 0.1, 0.4, 60, 120 } }), nullptr);
            tracks.appendChild (t, nullptr);
            ExportSettings s;
            s.sampleRate = sr;
            s.bitDepth = 32;
            s.endSec = 1.0;
            s.destination = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ff_midi_export.wav");
            auto res = exportAudio (tracks.createCopy(), dir, cache, s);
            fx::setPluginHost (nullptr);
            expect (res.result.wasOk(), res.result.getErrorMessage());
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (s.destination));
            expect (rd != nullptr);
            if (rd != nullptr)
            {
                juce::AudioBuffer<float> b (2, 48000);
                rd->read (&b, 0, 48000, 0, true, true);
                expect (b.getMagnitude (0, 0, 4000) < 1.0e-6f && b.getMagnitude (0, 6000, 12000) > 0.02f, "the note is in the export");
            }
            dir.deleteRecursively();
        }
    }
};

static MidiAutomationTests midiAutomationTests;
