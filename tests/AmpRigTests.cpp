#include "engine/fx/Saturator.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "core/RigModel.h"
#include "core/ProjectFile.h"
#include "core/Timeline.h"
#include "engine/fx/AmpRig.h"
#include "engine/fx/NamModel.h"
#include "engine/fx/RigBank.h"
#include "engine/fx/BuiltinPlugins.h"
#include "engine/AudioEngine.h"
#include "engine/Exporter.h"
#include "NAM/get_dsp.h"
#include "NAM/dsp.h"
#include <cmath>

using namespace ff;
namespace rg = ff::rig;
namespace tl = ff::timeline;

namespace
{
juce::File namData (const juce::String& name) { return juce::File (FF_TEST_DATA_DIR).getChildFile ("nam").getChildFile (name); }

juce::File tempFile (const juce::String& name)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ff_rig_tests");
    d.createDirectory();
    return d.getChildFile (name);
}

/** A NAM "Linear" model: y[n] = sum w[k] x[n-k]. weights = {1} is the identity. */
juce::File writeLinearModel (const juce::String& name, std::vector<float> weights, double sr = 48000.0)
{
    juce::String w;
    for (size_t i = 0; i < weights.size(); ++i) w << (i ? "," : "") << juce::String (weights[i], 6);
    auto f = tempFile (name);
    f.replaceWithText ("{\"version\":\"0.5.4\",\"architecture\":\"Linear\",\"config\":{\"receptive_field\":" + juce::String ((int) weights.size())
                       + ",\"bias\":false},\"weights\":[" + w + "],\"sample_rate\":" + juce::String (sr, 1)
                       + ",\"metadata\":{\"name\":\"Identity\",\"loudness\":-18.0,\"input_level_dbu\":6.0}}");
    return f;
}

/** Rig with every block switched off (output = mono sum of the input). */
void allOff (fx::AmpRig& r)
{
    for (int b = 0; b < rg::numBlocks; ++b) r.setParam (rg::switchFor (b), 0.0f);
}

float peakOf (const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX)
{
    float p = 0;
    for (size_t i = from; i < juce::jmin (to, v.size()); ++i) p = juce::jmax (p, std::abs (v[i]));
    return p;
}

bool allFinite (const std::vector<float>& v)
{
    for (float x : v) if (! std::isfinite (x)) return false;
    return true;
}

/** Runs `in` (mono) through the rig in blocks; returns left output. */
std::vector<float> runRig (fx::AmpRig& r, const std::vector<float>& in, int block, std::vector<float>* right = nullptr)
{
    std::vector<float> out (in.size()), outR (in.size());
    for (size_t pos = 0; pos < in.size(); pos += (size_t) block)
    {
        const int n = (int) juce::jmin ((size_t) block, in.size() - pos);
        std::copy (in.begin() + (long) pos, in.begin() + (long) pos + n, out.begin() + (long) pos);
        std::copy (in.begin() + (long) pos, in.begin() + (long) pos + n, outR.begin() + (long) pos);
        r.process (out.data() + pos, outR.data() + pos, n);
    }
    if (right) *right = outR;
    return out;
}

std::vector<float> sine (double freq, double sr, int n, float amp = 0.5f)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr);
    return v;
}

/** Best lag (0..maxLag) of b against a, and the residual (dB relative to a) at that lag. */
std::pair<int, double> bestLag (const std::vector<float>& a, const std::vector<float>& b, int from, int len, int maxLag)
{
    int best = 0;
    double bestErr = 1e300, ref = 0;
    for (int i = from; i < from + len; ++i) ref += (double) a[(size_t) i] * a[(size_t) i];
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double e = 0;
        for (int i = from; i < from + len; ++i)
        {
            const double d = b[(size_t) (i + lag)] - a[(size_t) i];
            e += d * d;
        }
        if (e < bestErr) { bestErr = e; best = lag; }
    }
    return { best, 10.0 * std::log10 (juce::jmax (1e-30, bestErr) / juce::jmax (1e-30, ref)) };
}
}

struct RigModelTests : juce::UnitTest
{
    RigModelTests() : juce::UnitTest ("Rig model", "fx") {}

    void runTest() override
    {
        beginTest ("defaults, clamping and switches");
        {
            auto r = rg::makeDefault();
            expect (rg::isEnabled (r));
            for (int p = 0; p < rg::numParams; ++p)
                expectEquals (rg::get (r, p), rg::info (p).def);
            rg::set (r, rg::bass, 40.0f);
            expectEquals (rg::get (r, rg::bass), 12.0f);
            rg::set (r, rg::delayOn, 0.7f);
            expectEquals (rg::get (r, rg::delayOn), 1.0f);
            expectEquals (rg::formatValue (rg::delayTime, 380.4f), juce::String ("380 ms"));
            expectEquals (rg::formatValue (rg::cabHighCut, 12000.0f), juce::String ("12.0 kHz"));
            expectEquals (rg::formatValue (rg::calLevel, 12.0f), juce::String ("12.0 dBu"));
            expect (rg::isToggle (rg::namCalibrate) && ! rg::isToggle (rg::calLevel));
            for (int b = 0; b < rg::numBlocks; ++b)
                expectEquals (rg::info (rg::switchFor (b)).block, b);
        }

        beginTest ("block order: parse, repair, move, pack");
        {
            auto r = rg::makeDefault();
            expect (rg::getOrder (r) == rg::defaultOrder());
            r.setProperty (rg::ids::order, "reverb,amp,bogus,amp,gate", nullptr);
            auto o = rg::getOrder (r);
            expectEquals (o[0], (int) rg::reverb);
            expectEquals (o[1], (int) rg::amp);
            expectEquals (o[2], (int) rg::gate);
            std::set<int> all (o.begin(), o.end());
            expectEquals ((int) all.size(), (int) rg::numBlocks);   // missing blocks appended, no duplicates
            rg::moveBlock (r, rg::amp, -1);
            expectEquals (rg::getOrder (r)[0], (int) rg::amp);
            rg::moveBlock (r, rg::amp, -1);                        // already first: no change
            expectEquals (rg::getOrder (r)[0], (int) rg::amp);
            expect (rg::unpackOrder (rg::packOrder (rg::getOrder (r))) == rg::getOrder (r));
        }

        beginTest ("presets round-trip and report missing files");
        {
            auto a = rg::makeDefault();
            rg::set (a, rg::driveOn, 1);
            rg::set (a, rg::delayTime, 512);
            a.setProperty (rg::ids::order, "amp,gate,comp,drive,eq,cab,chorus,delay,reverb", nullptr);
            a.setProperty (rg::ids::namFile, "/definitely/not/here.nam", nullptr);
            const auto json = juce::JSON::toString (rg::toPreset (a, "Mine"));
            auto b = rg::makeDefault();
            const auto missing = rg::applyPreset (b, juce::JSON::parse (json));
            expectEquals (missing.size(), 1);
            expectEquals (missing[0], juce::String ("here.nam"));
            expectEquals (rg::get (b, rg::driveOn), 1.0f);
            expectEquals (rg::get (b, rg::delayTime), 512.0f);
            expectEquals (rg::getOrder (b)[0], (int) rg::amp);
            expectEquals (b[rg::ids::namFile].toString(), juce::String());
            expectEquals (b[rg::ids::rigName].toString(), juce::String ("Mine"));

            const auto f = rg::factoryPresets();
            expect (f.size() >= 4);
            for (auto& p : f)
            {
                auto t = rg::makeDefault();
                t.setProperty (rg::ids::namFile, "/my/amp.nam", nullptr);
                expect (rg::applyPreset (t, p.json).isEmpty(), p.name);
                expectEquals (t[rg::ids::namFile].toString(), juce::String ("/my/amp.nam"));   // kept
            }
        }
    }
};

struct NamModelTests : juce::UnitTest
{
    NamModelTests() : juce::UnitTest ("NAM model", "fx") {}

    void runTest() override
    {
        beginTest ("metadata");
        {
            fx::NamInfo info;
            juce::String err;
            expect (fx::readNamInfo (namData ("wavenet.nam"), info, err), err);
            expectEquals (info.architecture, juce::String ("WaveNet"));
            expectEquals (info.gearMake, juce::String ("Darkglass Electronics"));
            expectEquals (info.sampleRate, 48000.0);
            expect (info.hasLoudness);
            expect (info.displayName().contains ("Microtubes"));

            expect (! fx::readNamInfo (tempFile ("missing.nam"), info, err));
            auto junk = tempFile ("junk.nam");
            junk.replaceWithText ("{ not json");
            expect (! fx::readNamInfo (junk, info, err));
            expect (err.contains ("not a valid"));
            auto notNam = tempFile ("other.nam");
            notNam.replaceWithText ("{\"hello\": 1}");
            expect (fx::NamModel::load (notNam, err) == nullptr);
        }

        beginTest ("our wrapper is bit-identical to NAM itself at the model's rate");
        for (auto name : { "wavenet.nam", "lstm.nam", "wavenet_a1_standard.nam" })
        {
            juce::String err;
            auto m = fx::NamModel::load (namData (name), err);
            expect (m != nullptr, err);
            if (m == nullptr) continue;
            m->prepare (48000.0, 256);
            expect (! m->isResampling());

            auto ref = nam::get_dsp (std::filesystem::path (namData (name).getFullPathName().toStdString()));
            ref->Reset (48000.0, 4096);

            auto in = sine (220.0, 48000.0, 4800, 0.3f);
            std::vector<float> ours = in, theirs (in.size());
            for (size_t p = 0; p < in.size(); p += 256)
            {
                const int n = (int) juce::jmin ((size_t) 256, in.size() - p);
                m->process (ours.data() + p, n);
                float* ip[1] = { in.data() + p };
                float* op[1] = { theirs.data() + p };
                ref->process (ip, op, n);
            }
            expect (ours == theirs, name);
            expect (allFinite (ours) && peakOf (ours, 2400) > 1.0e-4f, name);
        }

        beginTest ("resampling: identity model at 44.1 and 96 kHz");
        for (double devRate : { 44100.0, 96000.0, 32000.0 })
        {
            juce::String err;
            auto m = fx::NamModel::load (writeLinearModel ("identity.nam", { 1.0f }), err);
            expect (m != nullptr, err);
            m->prepare (devRate, 512);
            expect (m->isResampling());
            const int n = (int) devRate;   // 1 s
            // band-limited noise: unlike a sine, it has only one alignment that matches
            std::vector<float> in ((size_t) n);
            juce::Random rnd (7);
            fx::Biquad lp1, lp2;
            lp1.set (fx::Biquad::lowPass, devRate, 3000.0, 0.541);
            lp2.set (fx::Biquad::lowPass, devRate, 3000.0, 1.307);
            for (auto& x : in) x = lp2.process (lp1.process (rnd.nextFloat() - 0.5f));
            auto out = in;
            for (int p = 0; p < n; p += 333)                     // odd block sizes on purpose
                m->process (out.data() + p, juce::jmin (333, n - p));
            const auto [lag, errDb] = bestLag (in, out, n / 4, n / 2, 200);
            juce::ignoreUnused (errDb);
            expect (std::abs (lag - m->latencySamples()) <= 1, "latency: measured " + juce::String (lag) + ", reported " + juce::String (m->latencySamples()));

            // quality: a 440 Hz tone comes out as the same tone (no pitch change, nothing added)
            m->reset();
            auto tone = sine (440.0, devRate, n, 0.5f);
            for (int p = 0; p < n; p += 333) m->process (tone.data() + p, juce::jmin (333, n - p));
            double ss = 0, sc = 0, s2 = 0, c2 = 0, sx = 0, tot = 0;
            for (int i = n / 4; i < n * 3 / 4; ++i)
            {
                const double ph = 2.0 * juce::MathConstants<double>::pi * 440.0 * i / devRate;
                const double x = tone[(size_t) i];
                ss += x * std::sin (ph); sc += x * std::cos (ph);
                s2 += std::sin (ph) * std::sin (ph); c2 += std::cos (ph) * std::cos (ph);
                juce::ignoreUnused (sx);
                tot += x * x;
            }
            const double a = ss / s2, b = sc / c2;
            const double fund = a * a * s2 + b * b * c2;
            const double residualDb = 10.0 * std::log10 (juce::jmax (1e-30, tot - fund) / tot);
            const double amp = std::sqrt (a * a + b * b);
            logMessage ("  " + juce::String (devRate) + " Hz: latency " + juce::String (lag) + " samples, 440 Hz amplitude "
                        + juce::String (amp, 4) + ", residual " + juce::String (residualDb, 1) + " dB");
            expectWithinAbsoluteError (amp, 0.5, 0.005);
            expect (residualDb < -50.0, "resampling adds too much: " + juce::String (residualDb, 1) + " dB");
        }

        beginTest ("normalisation gain from loudness");
        {
            juce::String err;
            auto m = fx::NamModel::load (namData ("wavenet.nam"), err);
            expectWithinAbsoluteError (juce::Decibels::gainToDecibels (m->normalisationGain()), (float) (-14.0 - m->info().loudness), 0.01f);
        }
    }
};

struct AmpRigTests : juce::UnitTest
{
    AmpRigTests() : juce::UnitTest ("Amp rig", "fx") {}

    void runTest() override
    {
        const double sr = 48000.0;

        beginTest ("all blocks off: output is the input (mono sum)");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            auto in = sine (330.0, sr, 4800);
            auto out = runRig (r, in, 256);
            float maxDiff = 0;
            for (size_t i = 0; i < in.size(); ++i) maxDiff = juce::jmax (maxDiff, std::abs (out[i] - in[i]));
            expectLessThan (maxDiff, 1.0e-6f);
        }

        beginTest ("output gain");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::outGain, -6.0206f);
            r.reset();
            auto out = runRig (r, sine (330.0, sr, 9600), 512);
            expectWithinAbsoluteError (peakOf (out, 4800), 0.25f, 0.002f);
        }

        beginTest ("noise gate closes on hiss and opens on playing");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::gateOn, 1);
            r.setParam (rg::gateThresh, -50.0f);
            juce::Random rnd (1);
            std::vector<float> hiss (24000);
            for (auto& x : hiss) x = (rnd.nextFloat() - 0.5f) * 0.002f;   // about -60 dBFS
            auto out = runRig (r, hiss, 256);
            expectLessThan (peakOf (out, 12000), 1.0e-5f);
            expect (! r.gateOpen.load());
            auto loud = runRig (r, sine (200.0, sr, 9600, 0.3f), 256);
            expectGreaterThan (peakOf (loud, 4800), 0.29f);
            expect (r.gateOpen.load());
        }

        beginTest ("built-in amp, drive, eq, cab: bounded, finite, louder harmonics with more gain");
        {
            auto thd = [&] (float gain) {
                fx::AmpRig r;
                r.prepare (sr, 512);
                allOff (r);
                r.setParam (rg::ampOn, 1);
                r.setParam (rg::ampGain, gain);
                auto out = runRig (r, sine (200.0, sr, 24000, 0.3f), 512);
                expect (allFinite (out));
                expectLessThan (peakOf (out), 4.0f);
                // energy left after removing the 200 Hz fundamental (least squares)
                double ss = 0, sc = 0, s2 = 0, c2 = 0, tot = 0;
                for (int i = 12000; i < 24000; ++i)
                {
                    const double ph = 2.0 * juce::MathConstants<double>::pi * 200.0 * i / sr;
                    ss += out[(size_t) i] * std::sin (ph); sc += out[(size_t) i] * std::cos (ph);
                    s2 += std::sin (ph) * std::sin (ph); c2 += std::cos (ph) * std::cos (ph);
                    tot += (double) out[(size_t) i] * out[(size_t) i];
                }
                const double fund = ss * ss / s2 + sc * sc / c2;
                return (tot - fund) / juce::jmax (1e-12, tot);
            };
            const double clean = thd (0.05f), dirty = thd (0.95f);
            logMessage ("  harmonic energy: gain 5% " + juce::String (clean, 4) + ", gain 95% " + juce::String (dirty, 4));
            expectGreaterThan (dirty, clean * 5.0);

            fx::AmpRig r;
            r.prepare (sr, 512);
            for (int b = 0; b < rg::numBlocks; ++b) r.setParam (rg::switchFor (b), 1.0f);   // everything on
            r.setParam (rg::driveGain, 1.0f);
            r.setParam (rg::bass, 12.0f);
            r.setParam (rg::treble, 12.0f);
            std::vector<float> right;
            auto out = runRig (r, sine (110.0, sr, 48000, 0.8f), 480, &right);
            expect (allFinite (out) && allFinite (right));
            expectLessThan (juce::jmax (peakOf (out), peakOf (right)), 8.0f);
            expectGreaterThan (peakOf (out, 24000), 0.01f);
        }

        beginTest ("delay repeats at the set time");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::delayOn, 1);
            r.setParam (rg::delayTime, 250.0f);
            r.setParam (rg::delayFeedback, 0.0f);
            r.setParam (rg::delayMix, 0.5f);
            r.setParam (rg::delayTone, 1.0f);
            std::vector<float> in (24000, 0.0f);
            in[100] = 1.0f;
            auto out = runRig (r, in, 256);
            int at = 0;
            float best = 0;
            for (int i = 200; i < 24000; ++i) if (std::abs (out[(size_t) i]) > best) { best = std::abs (out[(size_t) i]); at = i; }
            expect (std::abs (at - (100 + 12000)) <= 2, "echo at " + juce::String (at));
            expectGreaterThan (best, 0.2f);
        }

        beginTest ("block order is honoured (reverb before the amp runs mono afterwards)");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::reverbOn, 1);
            r.setParam (rg::ampOn, 1);
            auto order = rg::defaultOrder();
            std::swap (order[3], order[8]);   // reverb where the amp was, amp last
            r.order = rg::packOrder (order);
            std::vector<float> right;
            auto out = runRig (r, sine (200.0, sr, 9600, 0.3f), 512, &right);
            float maxDiff = 0;
            for (size_t i = 0; i < out.size(); ++i) maxDiff = juce::jmax (maxDiff, std::abs (out[i] - right[i]));
            expectLessThan (maxDiff, 1.0e-6f);    // the amp (mono) came last: both sides identical
            expectGreaterThan (peakOf (out, 4800), 0.01f);
        }

        beginTest ("reverb adds no volume at zero mix; mono delay stays centred");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            auto dry = runRig (r, sine (200.0, sr, 9600, 0.3f), 512);
            r.setParam (rg::reverbOn, 1);
            r.setParam (rg::reverbMix, 0.0f);
            auto wet = runRig (r, sine (200.0, sr, 9600, 0.3f), 512);
            expectWithinAbsoluteError (peakOf (wet, 4800), peakOf (dry, 4800), 0.01f);
            r.setParam (rg::reverbOn, 0);
            r.setParam (rg::delayOn, 1);
            r.setParam (rg::delayStereo, 0);
            std::vector<float> right;
            auto out = runRig (r, sine (200.0, sr, 9600, 0.3f), 512, &right);
            float maxDiff = 0;
            for (size_t i = 0; i < out.size(); ++i) maxDiff = juce::jmax (maxDiff, std::abs (out[i] - right[i]));
            expectLessThan (maxDiff, 1.0e-6f);
            r.setParam (rg::delayStereo, 1);
            out = runRig (r, sine (200.0, sr, 9600, 0.3f), 512, &right);
            maxDiff = 0;
            for (size_t i = 0; i < out.size(); ++i) maxDiff = juce::jmax (maxDiff, std::abs (out[i] - right[i]));
            expectGreaterThan (maxDiff, 1.0e-4f);   // stereo repeats: the sides differ
        }

        beginTest ("cabinet phase flips the polarity");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::cabOn, 1);
            auto a = runRig (r, sine (200.0, sr, 9600, 0.3f), 512);
            r.setParam (rg::cabPhase1, 1);
            auto b = runRig (r, sine (200.0, sr, 9600, 0.3f), 512);
            float maxSum = 0;
            for (size_t i = 4800; i < a.size(); ++i) maxSum = juce::jmax (maxSum, std::abs (a[i] + b[i]));
            expectLessThan (maxSum, 1.0e-4f);
            expectGreaterThan (peakOf (a, 4800), 0.01f);
        }

        beginTest ("NAM model hand-over, normalisation and garbage collection");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::ampOn, 1);
            r.setParam (rg::namNormalize, 0);
            juce::String err;
            auto m = fx::NamModel::load (writeLinearModel ("half.nam", { 0.5f }), err);
            m->prepare (sr, 512);
            r.setModel (std::move (m));
            expect (! r.modelActive.load());          // not picked up before the next block
            auto out = runRig (r, sine (330.0, sr, 4800), 256);
            expect (r.modelActive.load());
            expectWithinAbsoluteError (peakOf (out, 2400), 0.25f, 0.002f);

            r.setParam (rg::namNormalize, 1);        // loudness -18 dB in the file, target -14: +4 dB
            out = runRig (r, sine (330.0, sr, 4800), 256);
            expectWithinAbsoluteError (peakOf (out, 2400), 0.25f * 1.5849625f, 0.004f);

            auto m2 = fx::NamModel::load (writeLinearModel ("quarter.nam", { 0.25f }), err);
            m2->prepare (sr, 512);
            r.setModel (std::move (m2));
            r.setParam (rg::namNormalize, 0);
            out = runRig (r, sine (330.0, sr, 9600), 256);
            expectWithinAbsoluteError (peakOf (out, 4800), 0.125f, 0.002f);
            r.collectGarbage();                       // frees the first model on this thread

            auto wrongRate = fx::NamModel::load (writeLinearModel ("x.nam", { 1.0f }), err);
            wrongRate->prepare (44100.0, 512);
            r.setModel (std::move (wrongRate));
            out = runRig (r, sine (330.0, sr, 4800), 256);
            expect (r.modelNeedsPrepare.load());
            expect (! r.modelActive.load());          // falls back to the built-in amp, never runs a mis-prepared model
            expect (allFinite (out));

            // input calibration: interface 0 dBFS = 12 dBu, capture made at 6 dBu -> +6 dB into the model
            auto m3 = fx::NamModel::load (writeLinearModel ("cal.nam", { 1.0f }), err);
            expect (m3->info().hasInputLevel);
            expectWithinAbsoluteError (m3->info().inputLevelDbu, 6.0, 1e-9);
            m3->prepare (sr, 512);
            r.setModel (std::move (m3));
            r.setParam (rg::namNormalize, 0);
            r.setParam (rg::namCalibrate, 1);
            r.setParam (rg::calLevel, 12.0f);
            out = runRig (r, sine (330.0, sr, 9600, 0.25f), 256);
            expectWithinAbsoluteError (peakOf (out, 4800), 0.25f * 1.99526f, 0.003f);
            r.setParam (rg::namCalibrate, 0);
            out = runRig (r, sine (330.0, sr, 9600, 0.25f), 256);
            expectWithinAbsoluteError (peakOf (out, 4800), 0.25f, 0.003f);
            r.collectGarbage();

            r.clearModel();
            runRig (r, sine (330.0, sr, 512), 256);
            expect (! r.modelActive.load() && ! r.modelNeedsPrepare.load());
        }

        beginTest ("cabinet impulse response");
        {
            fx::AmpRig r;
            r.prepare (sr, 512);
            allOff (r);
            r.setParam (rg::cabOn, 1);
            r.setParam (rg::cabLowCut, 20.0f);
            r.setParam (rg::cabHighCut, 20000.0f);
            juce::AudioBuffer<float> ir (1, 64);
            ir.clear();
            ir.setSample (0, 0, 0.05f);   // a little direct sound so trimming keeps the start
            ir.setSample (0, 40, 1.0f);
            r.setImpulseResponse (std::move (ir), sr);
            r.finishLoadingOffline();
            r.reset();
            std::vector<float> in (4800, 0.0f);
            in[1000] = 1.0f;
            auto out = runRig (r, in, 256);
            int at = 0;
            float best = 0;
            for (int i = 0; i < 4800; ++i) if (std::abs (out[(size_t) i]) > best) { best = std::abs (out[(size_t) i]); at = i; }
            expectEquals (at, 1040);
            expect (r.irActive.load());
        }

        beginTest ("cabinet loaded into a running / waiting rig with a shared queue is applied");
        {
            // The way the app does it: rigs share one message queue, the IR is handed over while the rig is idle or
            // running, the device may re-prepare the rig, and the cabinet must still end up in the signal.
            for (int scenario = 0; scenario < 4; ++scenario)
            {
                juce::dsp::ConvolutionMessageQueue queue { 64 };
                fx::AmpRig other (&queue);   // a second rig on the same queue
                other.prepare (sr, 512);
                fx::AmpRig r (&queue);
                r.prepare (sr, 512);
                allOff (r);
                r.setParam (rg::cabOn, 1);
                r.setParam (rg::cabLowCut, 20.0f);
                r.setParam (rg::cabHighCut, 20000.0f);
                juce::AudioBuffer<float> ir (1, 64);
                ir.clear();
                ir.setSample (0, 0, 0.05f);
                ir.setSample (0, 40, 1.0f);
                std::vector<float> in (512, 0.0f);
                in[100] = 1.0f;
                std::vector<float> silence (512, 0.0f);
                auto pump = [&] (int blocks) {   // idle blocks, like the audio callback while the user waits
                    for (int b = 0; b < blocks; ++b) { runRig (r, silence, 512); juce::Thread::sleep (4); }
                };
                if (scenario == 1) pump (5);                  // running before the IR arrives
                r.setImpulseResponse (std::move (ir), sr);
                if (scenario == 2) r.prepare (sr, 512);       // the device re-prepares the rig after the load
                if (scenario == 3) { juce::Thread::sleep (300); }   // nobody processes it for a while (live rig switched off)
                bool found = false;
                for (int attempt = 0; attempt < 100 && ! found; ++attempt)
                {
                    pump (1);
                    auto out = runRig (r, in, 512);
                    int at = 0;
                    float best = 0;
                    for (int i = 0; i < 512; ++i) if (std::abs (out[(size_t) i]) > best) { best = std::abs (out[(size_t) i]); at = i; }
                    found = at == 140 && best > 0.01f;
                    pump (2);
                }
                expect (found, "scenario " + juce::String (scenario) + ": the cabinet is in the signal");
            }
        }

        beginTest ("cabinets still arrive while other rigs are being created on the message thread");
        {
            // Opening a project creates and prepares track rigs on the message thread while the audio thread already
            // runs the others, all on one shared convolution queue: no cabinet may get lost.
            for (int round = 0; round < 6; ++round)
            {
                juce::dsp::ConvolutionMessageQueue queue { 64 };
                std::vector<std::unique_ptr<fx::AmpRig>> rigs;
                std::atomic<bool> stop { false };
                juce::CriticalSection rigLock;
                auto makeCabRig = [&] (int k) {
                    auto r = std::make_unique<fx::AmpRig> (&queue);
                    r->prepare (sr, 512);
                    allOff (*r);
                    r->setParam (rg::cabOn, 1);
                    r->setParam (rg::cabLowCut, 20.0f);
                    r->setParam (rg::cabHighCut, 20000.0f);
                    juce::AudioBuffer<float> ir (1, 128);
                    ir.clear();
                    ir.setSample (0, 0, 0.05f);
                    ir.setSample (0, 30 + 10 * k, 1.0f);
                    r->setImpulseResponse (std::move (ir), sr);
                    return r;
                };
                struct Audio : juce::Thread
                {
                    Audio (std::vector<std::unique_ptr<fx::AmpRig>>& r, juce::CriticalSection& l, std::atomic<bool>& s) : Thread ("audio"), rigs (r), lock (l), stop (s) {}
                    void run() override
                    {
                        std::vector<float> a (512, 0.0f), b (512, 0.0f);
                        while (! stop.load())
                        {
                            { const juce::ScopedLock sl (lock); for (auto& r : rigs) { r->process (a.data(), b.data(), 512); } }
                            juce::Thread::sleep (2);
                        }
                    }
                    std::vector<std::unique_ptr<fx::AmpRig>>& rigs; juce::CriticalSection& lock; std::atomic<bool>& stop;
                } audio (rigs, rigLock, stop);
                audio.startThread();
                for (int k = 0; k < 8; ++k)
                {
                    auto r = makeCabRig (k);
                    { const juce::ScopedLock sl (rigLock); rigs.push_back (std::move (r)); }
                    juce::Thread::sleep (3);
                }
                juce::Thread::sleep (300);
                stop = true;
                audio.stopThread (2000);
                for (int k = 0; k < 8; ++k)
                {
                    auto& r = *rigs[(size_t) k];
                    std::vector<float> in (512, 0.0f);
                    in[100] = 1.0f;
                    bool found = false;
                    for (int attempt = 0; attempt < 100 && ! found; ++attempt)
                    {
                        auto out = runRig (r, in, 512);
                        int at = 0; float best = 0;
                        for (int i = 0; i < 512; ++i) if (std::abs (out[(size_t) i]) > best) { best = std::abs (out[(size_t) i]); at = i; }
                        found = at == 130 + 10 * k && best > 0.01f;
                        juce::Thread::sleep (5);
                    }
                    expect (found, "round " + juce::String (round) + ", rig " + juce::String (k) + ": cabinet in the signal");
                }
            }
        }

        beginTest ("NAM Amp plugin: capture and cabinet come back with the saved state");
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ff_namamp_test");
            dir.createDirectory();
            auto irFile = dir.getChildFile ("cab.wav");
            irFile.deleteFile();
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (irFile), sr, 1, 24, {}, 0));
                juce::AudioBuffer<float> ir (1, 256);
                ir.clear();
                ir.setSample (0, 0, 0.5f);
                ir.setSample (0, 20, -0.6f);
                ir.setSample (0, 90, 0.3f);
                w->writeFromAudioSampleBuffer (ir, 0, 256);
            }
            auto render = [&] (juce::AudioPluginInstance& p) {
                p.setRateAndBufferSizeDetails (sr, 256);
                p.prepareToPlay (sr, 256);
                std::vector<float> out;
                const auto in = sine (196.0, sr, 256 * 24, 0.3f);
                for (int b = 0; b < 24; ++b)
                {
                    juce::AudioBuffer<float> buf (2, 256);
                    for (int i = 0; i < 256; ++i) buf.setSample (0, i, in[(size_t) (b * 256 + i)]), buf.setSample (1, i, in[(size_t) (b * 256 + i)]);
                    juce::MidiBuffer midi;
                    p.processBlock (buf, midi);
                    for (int i = 0; i < 256; ++i) out.push_back (buf.getSample (0, i));
                }
                return out;
            };
            auto maxDiff = [] (const std::vector<float>& a, const std::vector<float>& b) {
                float d = 0;
                for (size_t i = 0; i < a.size(); ++i) d = juce::jmax (d, std::abs (a[i] - b[i]));
                return d;
            };
            auto a = fx::builtin::create ("fretforge:nam");
            expect (a != nullptr);
            if (a != nullptr)
            {
                expect (fx::builtin::setNamAmpFile (*a, 0, namData ("wavenet.nam")));
                expect (fx::builtin::setNamAmpFile (*a, 1, irFile));
                const auto outA = render (*a);
                juce::MemoryBlock saved;
                a->getStateInformation (saved);

                auto b = fx::builtin::create ("fretforge:nam");
                b->setStateInformation (saved.getData(), (int) saved.getSize());   // a project opening
                const auto outB = render (*b);
                expect (maxDiff (outA, outB) < 1.0e-4f, "restored plugin sounds like the saved one: " + juce::String (maxDiff (outA, outB)));

                if (const auto snap = juce::SystemStats::getEnvironmentVariable ("FF_SNAPSHOT", {}); snap.isNotEmpty())
                {
                    if (const auto logoPath = juce::SystemStats::getEnvironmentVariable ("FF_LOGO", {}); logoPath.isNotEmpty())
                        fx::builtin::setBrandLogo (juce::ImageFileFormat::loadFrom (juce::File (logoPath)));
                    std::unique_ptr<juce::AudioProcessorEditor> ed (b->createEditorIfNeeded());
                    ed->setBounds (0, 0, ed->getWidth(), ed->getHeight());
                    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.5f);
                    juce::File (snap).deleteFile();
                    juce::FileOutputStream os { juce::File (snap) };
                    juce::PNGImageFormat().writeImageToStream (img, os);
                }
                auto c = fx::builtin::create ("fretforge:nam");
                fx::builtin::setNamAmpFile (*c, 0, namData ("wavenet.nam"));
                const auto outC = render (*c);
                expect (maxDiff (outA, outC) > 1.0e-3f, "the cabinet is in the signal: " + juce::String (maxDiff (outA, outC)));
            }
            dir.deleteRecursively();
        }

        beginTest ("engine: track insert and live rig");
        {
            AudioEngine e;
            e.prepare (sr, 256);
            auto graph = std::make_unique<PlaybackGraph>();
            PlayTrack t;
            t.monitor = true;
            t.inputLeft = 0;
            t.rig = std::make_shared<fx::AmpRig>();
            t.rig->prepare (sr, 256);
            allOff (*t.rig);
            t.rig->setParam (rg::outGain, -6.0206f);
            t.rig->reset();
            graph->tracks.push_back (t);
            e.submitGraph (std::move (graph));

            std::vector<float> in (256), l (256), r (256);
            auto run = [&] (int blocks) {
                float pk = 0;
                for (int b = 0; b < blocks; ++b)
                {
                    for (int i = 0; i < 256; ++i) in[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (b * 256 + i) / sr);
                    const float* ins[1] = { in.data() };
                    float* outs[2] = { l.data(), r.data() };
                    e.process (ins, 1, outs, 2, 256);
                    if (b >= blocks / 2) pk = juce::jmax (pk, peakOf (l));
                }
                return pk;
            };
            expectWithinAbsoluteError (run (40), 0.25f, 0.003f);     // monitored through the rig at -6 dB

            fx::AmpRig live;
            live.prepare (sr, 256);
            allOff (live);
            e.liveRig = &live;
            e.liveRigOn = true;
            e.liveInput = 0;
            expectWithinAbsoluteError (run (40), 0.75f, 0.005f);     // + live rig at 0 dB (same phase)
            e.liveRigOn = false;
            expectWithinAbsoluteError (run (40), 0.25f, 0.003f);
            e.liveRig = nullptr;
            e.collectGarbage();
        }

        beginTest ("export renders the track rig offline");
        {
            auto dir = tempFile ("exp.ffproj");
            dir.deleteRecursively();
            project::Bundle::createStructure (dir);
            auto wav = dir.getChildFile ("Audio/s.wav");
            {
                juce::AudioBuffer<float> b (1, 48000);
                for (int i = 0; i < 48000; ++i) b.setSample (0, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * i / sr));
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::OutputStream> os (wav.createOutputStream().release());
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), sr, 1, 32, {}, 0));
                os.release();
                w->writeFromAudioSampleBuffer (b, 0, 48000);
            }
            auto projectTree = project::createDefault ("T");
            auto tracks = projectTree.getChildWithName (project::ids::TRACKS);
            auto t = tl::createTrack ("Gtr", 0);
            tracks.appendChild (t, nullptr);
            auto c = tl::createClip ("Audio/s.wav", "s", 0.0, 1.0, 0.0);
            t.appendChild (c, nullptr);
            auto rigTree = rg::getOrCreate (t);
            rigTree.setProperty (rg::ids::enabled, true, nullptr);
            for (int b = 0; b < rg::numBlocks; ++b) rg::set (rigTree, rg::switchFor (b), 0.0f);
            rg::set (rigTree, rg::outGain, -6.0206f);
            rigTree.setProperty (rg::ids::namFile, "/nope/missing.nam", nullptr);

            AudioCache cache;
            ExportSettings s;
            s.sampleRate = sr;
            s.bitDepth = 32;
            s.startSec = 0.0;
            s.endSec = 1.0;
            s.destination = tempFile ("rigexport.wav");
            auto res = exportAudio (tracks, dir, cache, s);
            expect (res.result.wasOk(), res.result.getErrorMessage());
            expect (res.warnings.joinIntoString (" ").contains ("missing.nam"), "missing model not reported");
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (s.destination));
            expect (rd != nullptr);
            if (rd != nullptr)
            {
                juce::AudioBuffer<float> b (2, 48000);
                rd->read (&b, 0, 48000, 0, true, true);
                expectWithinAbsoluteError (b.getMagnitude (0, 24000, 24000), 0.25f, 0.003f);
            }
            dir.deleteRecursively();
        }

        beginTest ("plugin delay compensation: a resampled NAM model stays in time (export and live playback)");
        {
            const double rate = 44100.0;   // identity model trained at 48 kHz -> resampling latency
            auto dir = tempFile ("pdc.ffproj");
            dir.deleteRecursively();
            project::Bundle::createStructure (dir);
            auto wav = dir.getChildFile ("Audio/click.wav");
            {
                juce::AudioBuffer<float> b (1, 44100);
                b.clear();
                b.setSample (0, 22050, 1.0f);
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::OutputStream> os (wav.createOutputStream().release());
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), rate, 1, 32, {}, 0));
                os.release();
                w->writeFromAudioSampleBuffer (b, 0, 44100);
            }
            auto projectTree = project::createDefault ("T");
            auto tracks = projectTree.getChildWithName (project::ids::TRACKS);
            auto t = tl::createTrack ("Gtr", 0);
            tracks.appendChild (t, nullptr);
            auto c = tl::createClip ("Audio/click.wav", "c", 0.0, 1.0, 0.0);
            c.setProperty (tl::ids::fadeIn, 0.0, nullptr);
            c.setProperty (tl::ids::fadeOut, 0.0, nullptr);
            t.appendChild (c, nullptr);
            auto rigTree = rg::getOrCreate (t);
            rigTree.setProperty (rg::ids::enabled, true, nullptr);
            for (int b = 0; b < rg::numBlocks; ++b) rg::set (rigTree, rg::switchFor (b), 0.0f);
            rg::set (rigTree, rg::ampOn, 1.0f);
            rg::set (rigTree, rg::namNormalize, 0.0f);
            rigTree.setProperty (rg::ids::namFile, writeLinearModel ("pdc-identity.nam", { 1.0f }).getFullPathName(), nullptr);

            auto peakIndex = [] (const float* x, int n) {
                int at = 0; float best = 0;
                for (int i = 0; i < n; ++i) if (std::abs (x[i]) > best) { best = std::abs (x[i]); at = i; }
                return at;
            };

            AudioCache cache;
            ExportSettings s;
            s.sampleRate = rate;
            s.bitDepth = 32;
            s.endSec = 1.0;
            s.destination = tempFile ("pdc.wav");
            auto res = exportAudio (tracks, dir, cache, s);
            expect (res.result.wasOk(), res.result.getErrorMessage());
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (s.destination));
            juce::AudioBuffer<float> out (2, 44100);
            rd->read (&out, 0, 44100, 0, true, true);
            expect (std::abs (peakIndex (out.getReadPointer (0), 44100) - 22050) <= 1, "export: click at "
                    + juce::String (peakIndex (out.getReadPointer (0), 44100)));

            // live playback through the engine
            AudioEngine e;
            e.prepare (rate, 256);
            auto liveRig = fx::makeOfflineRig (rigTree, rate, 256, res.warnings);
            expectGreaterThan (liveRig->totalLatency(), 10);
            GraphBuildOptions o;
            o.sampleRate = rate;
            o.waitForAudio = true;
            o.rigFor = [&] (const juce::ValueTree&) { return liveRig; };
            e.submitGraph (buildPlaybackGraph (tracks, dir, cache, o));
            e.play();
            std::vector<float> captured, l (256), r (256);
            for (int b = 0; b < 44100 / 256; ++b)
            {
                float* outs[2] = { l.data(), r.data() };
                e.process (nullptr, 0, outs, 2, 256);
                captured.insert (captured.end(), l.begin(), l.end());
            }
            e.stop();
            const int at = peakIndex (captured.data(), (int) captured.size());
            logMessage ("  rig latency " + juce::String (liveRig->totalLatency()) + " samples; click at " + juce::String (at) + " (clip has it at 22050)");
            expect (std::abs (at - 22050) <= 1, "live: click at " + juce::String (at));
            e.collectGarbage();
            dir.deleteRecursively();
        }

        beginTest ("rig bank follows the project tree");
        {
            AudioEngine e;
            e.prepare (sr, 256);
            fx::RigBank bank (e);
            int rebuilds = 0;
            bank.onRigsChanged = [&] { ++rebuilds; };
            auto projectTree = project::createDefault ("T");
            auto tracks = projectTree.getChildWithName (project::ids::TRACKS);
            auto t = tl::createTrack ("Gtr", 0);
            tracks.appendChild (t, nullptr);
            bank.rebind (projectTree);
            const auto id = t[tl::ids::id].toString();
            expect (bank.rigForTrack (id) == nullptr);
            auto rigTree = rg::getOrCreate (t);              // created disabled
            expect (bank.rigForTrack (id) == nullptr);
            rigTree.setProperty (rg::ids::enabled, true, nullptr);
            auto rig = bank.rigForTrack (id);
            expect (rig != nullptr);
            expectEquals (rebuilds, 1);
            expect (rig->isPreparedFor (sr));
            rg::set (rigTree, rg::bass, 5.0f);
            expectEquals (rig->getParam (rg::bass), 5.0f);
            rigTree.setProperty (rg::ids::namFile, namData ("wavenet.nam").getFullPathName(), nullptr);
            expect (bank.statusFor (rigTree).modelLoading);
            e.prepare (44100.0, 128);                         // device restart: every rig follows
            expect (rig->isPreparedFor (44100.0));
            expect (bank.liveRig().isPreparedFor (44100.0));
            auto saved = rg::makeDefault();
            rg::setPlugin (saved, 1, "<PLUGIN name=\"P\"/>", "P");
            bank.setLiveTree (saved);                          // settings at start-up: plugins come along
            expect (rg::pluginNode (bank.liveTree(), 1).isValid());
            auto live = bank.liveTree();
            live.setProperty (rg::ids::enabled, true, nullptr);
            expect (e.liveRigOn.load());
            rigTree.setProperty (rg::ids::enabled, false, nullptr);
            expect (bank.rigForTrack (id) == nullptr);
            expectEquals (rebuilds, 2);
            tracks.removeChild (t, nullptr);
        }
    }
};

struct AmpRigPerf : juce::UnitTest
{
    AmpRigPerf() : juce::UnitTest ("Amp rig CPU", "perf") {}

    void runTest() override
    {
        beginTest ("standard WaveNet capture + full chain, real-time factor");
        juce::String err;
        auto m = fx::NamModel::load (namData ("wavenet_a1_standard.nam"), err);
        expect (m != nullptr, err);
        for (double rate : { 48000.0, 44100.0 })
        {
            fx::AmpRig r;
            r.prepare (rate, 256);
            auto mm = fx::NamModel::load (namData ("wavenet_a1_standard.nam"), err);
            mm->prepare (rate, 256);
            r.setModel (std::move (mm));
            for (int b = 0; b < rg::numBlocks; ++b) r.setParam (rg::switchFor (b), 1.0f);
            auto in = sine (110.0, rate, (int) rate * 3, 0.3f);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            auto out = runRig (r, in, 256);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (r.modelActive.load());
            expect (allFinite (out));
            logMessage ("  " + juce::String (rate) + " Hz: 3 s of audio in " + juce::String (ms, 1) + " ms ("
                        + juce::String (3000.0 / ms, 1) + "x real time)");
            // shared CI machines vary a lot: only enforce a margin when asked to (FF_PERF=1)
            if (juce::SystemStats::getEnvironmentVariable ("FF_PERF", {}).isNotEmpty())
                expectGreaterThan (3000.0 / ms, 2.0);
        }
    }
};

static RigModelTests rigModelTests;
static NamModelTests namModelTests;
static AmpRigTests ampRigTests;
static AmpRigPerf ampRigPerf;

// ---------------------------------------------------------------------------------------------- plugins

namespace
{
/** Plugin host for tests: creates instances synchronously (tests run on the message thread). */
struct TestPluginHost : fx::PluginHost
{
    juce::AudioPluginFormatManager formats;
    TestPluginHost() { juce::addDefaultFormatsToManager (formats); }
    void createAsync (const juce::String& xml, double sr, int bs,
                      std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> done) override
    {
        juce::String err;
        auto p = createBlocking (xml, sr, bs, err);
        done (std::move (p), err);
    }
    std::unique_ptr<juce::AudioPluginInstance> createBlocking (const juce::String& xml, double sr, int bs, juce::String& err) override
    {
        juce::PluginDescription d;
        if (auto x = juce::parseXML (xml)) d.loadFromXml (*x);
        auto inst = formats.createPluginInstance (d, sr, bs, err);
        fx::registerPluginInstance (inst.get(), d.name, d.fileOrIdentifier);   // like the app's plugin manager
        return inst;
    }
};
}

struct PluginSlotTests : juce::UnitTest
{
    PluginSlotTests() : juce::UnitTest ("Plugin slots", "fx") {}

    void runTest() override
    {
        beginTest ("model: slots, order, labels, presets");
        {
            auto r = rg::makeDefault();
            const auto o = rg::getOrder (r);   // an old order string: plugin blocks are appended
            expectEquals (o[9], (int) rg::plug1);
            expectEquals (o[12], (int) rg::plug4);
            expect (rg::unpackOrder (rg::packOrder (o)) == o);
            expectEquals (rg::freePluginSlot (r), 0);
            rg::setPlugin (r, 0, "<PLUGIN name=\"X\"/>", "Big Verb");
            rg::setPlugin (r, 2, "<PLUGIN name=\"Y\"/>", "Tape");
            expectEquals (rg::freePluginSlot (r), 1);
            expectEquals (rg::blockLabel (r, rg::plug1), juce::String ("Big Verb"));
            expectEquals (rg::blockLabel (r, rg::plug2), juce::String ("Plugin 2"));
            expect (rg::isToggle (rg::plug3On));
            rg::pluginNode (r, 0).setProperty (rg::ids::state, "c3RhdGU=", nullptr);
            const auto json = juce::JSON::toString (rg::toPreset (r, "P"));
            auto b = rg::makeDefault();
            rg::setPlugin (b, 3, "<PLUGIN/>", "Old");
            rg::applyPreset (b, juce::JSON::parse (json));
            expect (! rg::pluginNode (b, 3).isValid(), "the preset's plugins replace the rig's");
            expectEquals (rg::pluginNode (b, 0)[rg::ids::state].toString(), juce::String ("c3RhdGU="));
            expectEquals (rg::pluginNode (b, 2)[rg::ids::name].toString(), juce::String ("Tape"));
            for (auto& f : rg::factoryPresets())   // built-in presets keep the plugins
            {
                rg::applyPreset (b, f.json);
                expect (rg::pluginNode (b, 0).isValid());
            }
            rg::removePlugin (b, 0);
            expect (! rg::pluginNode (b, 0).isValid());
        }

        beginTest ("track inserts: add, move, remove, replace");
        {
            juce::ValueTree t ("TRACK");
            expectEquals (rg::numInserts (t), 0);
            expectEquals (rg::addInsert (t, "<A/>", "A"), 0);
            expectEquals (rg::addInsert (t, "<B/>", "B"), 1);
            expectEquals (rg::addInsert (t, "<C/>", "C"), 2);
            rg::moveInsert (t, 2, -1);
            expectEquals (rg::insertNode (t, 1)[rg::ids::name].toString(), juce::String ("C"));
            rg::removeInsert (t, 0);
            expectEquals (rg::numInserts (t), 2);
            expectEquals (rg::insertNode (t, 0)[rg::ids::name].toString(), juce::String ("C"));
            expectEquals (rg::insertNode (t, 1)[rg::ids::name].toString(), juce::String ("B"));
            rg::replaceInsert (t, 0, "<D/>", "D");
            expectEquals (rg::insertNode (t, 0)[rg::ids::name].toString(), juce::String ("D"));
            for (int i = 0; i < rg::maxInserts; ++i) rg::addInsert (t, "<E/>", "E");   // the extra ones are refused
            expectEquals (rg::numInserts (t), rg::maxInserts);
            expect (rg::maxInserts >= 64, "plenty of plugins per track");
            juce::StringArray w;
            expect (fx::makeOfflineInserts (t, 48000.0, 512, w) != nullptr);
            expect (w.size() == rg::maxInserts, "every insert reported without a host");
            juce::ValueTree empty ("TRACK");
            expect (fx::makeOfflineInserts (empty, 48000.0, 512, w) == nullptr);
        }

        beginTest ("EQ: Metal Chug Control preset, and auto release lets go faster between hits");
        {
            auto eqA = fx::builtin::create ("fretforge:eq");
            const fx::builtin::Preset* chug = nullptr;
            for (auto& p : fx::builtin::factoryPresets ("fretforge:eq")) if (p.name == "Metal Chug Control") chug = &p;
            expect (chug != nullptr, "the preset exists");
            if (chug != nullptr)
            {
                expect (fx::builtin::applyPreset (*eqA, *chug));
                const auto get = [&] (const char* id) { 
                                                        for (auto* q : eqA->getParameters())
                                                            if (auto* w = dynamic_cast<juce::RangedAudioParameter*> (q); w != nullptr && w->paramID == id)
                                                                return w->convertFrom0to1 (w->getValue());
                                                        return -999.0f; };
                expectWithinAbsoluteError (get ("b1freq"), 90.0f, 0.5f);
                expectWithinAbsoluteError (get ("b3freq"), 10000.0f, 5.0f);
                expectWithinAbsoluteError (get ("b2rel"), 25.0f, 0.5f);

                if (const auto snap = juce::SystemStats::getEnvironmentVariable ("FF_EQ_SNAPSHOT", {}); snap.isNotEmpty())
                {
                    std::unique_ptr<juce::AudioProcessorEditor> ed (eqA->createEditorIfNeeded());
                    ed->setBounds (0, 0, ed->getWidth(), ed->getHeight());
                    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.2f);
                    juce::File (snap).deleteFile();
                    juce::FileOutputStream os { juce::File (snap) };
                    juce::PNGImageFormat().writeImageToStream (img, os);
                }
            }
        }

        beginTest ("Drive: off is untouched, quiet stays quiet, loud is rounded off and bounded");
        {
            constexpr int n = 4800;
            std::vector<float> l (n), r (n);
            const auto fill = [&] (float amp) { for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = amp * std::sin (0.05f * (float) i); };
            fx::Saturator off;
            fill (0.8f);
            const auto ref = l;
            expectWithinAbsoluteError (off.process (l.data(), r.data(), n, 0.0f), 0.0f, 1.0e-6f);
            expect (l == ref, "drive 0 does not touch the signal");

            fx::Saturator quiet;
            fill (0.01f);
            for (int blk = 0; blk < n; blk += 480) quiet.process (l.data() + blk, r.data() + blk, 480, 1.0f);
            float peak = 0.0f;
            for (int i = n / 2; i < n; ++i) peak = juce::jmax (peak, std::abs (l[(size_t) i]));
            expect (peak > 0.04f && peak < 0.09f, "a quiet signal is lifted by about the drive gain");

            fx::Saturator loud;
            fill (0.9f);
            float red = 0.0f;
            for (int blk = 0; blk < n; blk += 480) red = loud.process (l.data() + blk, r.data() + blk, 480, 1.0f);
            float lp = 0.0f;
            for (int i = n / 2; i < n; ++i) lp = juce::jmax (lp, std::abs (l[(size_t) i]));
            expect (red < -1.0f, "a hot signal is reduced and the meter sees it");
            expect (lp < 0.9f && lp > 0.1f, "peaks are rounded off, not pushed up");
            expect (std::isfinite (lp));
        }

        beginTest ("Drive: oversampling keeps harmonics from folding back, and turning it on does not click");
        {
            constexpr int n = 9600;
            const double sr = 48000.0;
            const auto alias = [&] (int os)   // level of the 3 kHz component (the third harmonic of 15 kHz folds there)
            {
                fx::Saturator sat;
                std::vector<float> l (n), r (n);
                for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = 0.8f * std::sin (2.0f * juce::MathConstants<float>::pi * 15000.0f * (float) i / 48000.0f);
                for (int blk = 0; blk < n; blk += 480) sat.process (l.data() + blk, r.data() + blk, 480, 1.0f, os, sr);
                double re = 0, im = 0;
                for (int i = n / 2; i < n; ++i)
                {
                    const double ph = 2.0 * juce::MathConstants<double>::pi * 3000.0 * i / sr;
                    re += l[(size_t) i] * std::cos (ph); im += l[(size_t) i] * std::sin (ph);
                }
                for (int i = n / 2; i < n; ++i) expect (std::isfinite (l[(size_t) i]));
                return std::sqrt (re * re + im * im) / (n / 2);
            };
            const double a0 = alias (0), a2 = alias (2);
            expect (a2 < a0 * 0.5, "4x oversampling folds back much less than none");

            fx::Saturator sat;   // a loud 100 Hz tone; Drive comes on in the middle (the first blocks are bypassed)
            std::vector<float> l (n), r (n);
            for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 100.0f * (float) i / 48000.0f);
            const auto ref = l;
            for (int blk = 0; blk < n; blk += 480) sat.process (l.data() + blk, r.data() + blk, 480, blk < 4800 ? 0.0f : 0.5f, 2, sr);
            float worst = 0.0f;
            for (int i = 4790; i < 5400; ++i) worst = juce::jmax (worst, std::abs (l[(size_t) i] - l[(size_t) i - 1]));
            float steady = 0.0f;   // the saturated tone's own steepest edge
            for (int i = 7000; i < n; ++i) steady = juce::jmax (steady, std::abs (l[(size_t) i] - l[(size_t) i - 1]));
            expect (worst < steady * 1.5f, "no click when Drive comes on");
        }

        beginTest ("offline rig without a plugin host reports the plugin");
        {
            auto r = rg::makeDefault();
            rg::setPlugin (r, 1, "<PLUGIN name=\"Missing\"/>", "Missing");
            juce::StringArray warnings;
            auto rig = fx::makeOfflineRig (r, 48000.0, 512, warnings);
            expect (warnings.joinIntoString (" ").contains ("Missing"));
            auto out = runRig (*rig, sine (200.0, 48000.0, 4800), 512);
            expect (allFinite (out));
        }

        if (const auto lv2 = juce::SystemStats::getEnvironmentVariable ("FF_TEST_LV2", {}); lv2.isNotEmpty())
        {
            beginTest ("LV2: scan, load and process (" + lv2 + ")");
            TestPluginHost host;
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto* f : host.formats.getFormats())
                if (f->getName() == "LV2")
                    for (auto& id : f->searchPathsForPlugins (juce::FileSearchPath ("/usr/lib/lv2"), true, false))
                        if (id.contains (lv2)) f->findAllTypesForFile (found, id);
            expect (! found.isEmpty(), "no LV2 plugin matching " + lv2);
            for (auto* d : found)
            {
                juce::String err;
                auto inst = host.formats.createPluginInstance (*d, 48000.0, 512, err);
                expect (inst != nullptr, d->name + ": " + err);
                if (inst == nullptr) continue;
                fx::preparePlugin (*inst, 48000.0, 512);
                juce::AudioBuffer<float> scratch (8, 512);
                juce::MidiBuffer midi;
                auto l = sine (300.0, 48000.0, 512, 0.3f), r2 = l;
                fx::runPluginStereo (*inst, scratch, midi, l.data(), r2.data(), 512);
                expect (allFinite (l) && allFinite (r2), d->name);
                logMessage ("  " + d->name + ": in " + juce::String (inst->getTotalNumInputChannels()) + " / out "
                            + juce::String (inst->getTotalNumOutputChannels()) + ", peak " + juce::String (peakOf (l), 3));
            }
        }

        const auto path = juce::SystemStats::getEnvironmentVariable ("FF_TEST_VST3", {});
        if (path.isEmpty())
        {
            logMessage ("  (set FF_TEST_VST3 to the FF Test Gain .vst3 bundle to test real plugin hosting)");
            return;
        }

        beginTest ("VST3: scan, load, process, bypass, state, offline");
        {
            TestPluginHost host;
            fx::setPluginHost (&host);
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto* f : host.formats.getFormats())
                if (f->getName() == "VST3") f->findAllTypesForFile (found, path);
            expectEquals (found.size(), 1);
            if (found.isEmpty()) { fx::setPluginHost (nullptr); return; }
            const auto desc = found[0]->createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

            juce::String err;
            auto inst = host.createBlocking (desc, 48000.0, 512, err);
            expect (inst != nullptr, err);
            fx::preparePlugin (*inst, 48000.0, 512);
            fx::AmpRig r;
            r.prepare (48000.0, 512);
            allOff (r);
            r.setParam (rg::plug1On, 1);
            std::shared_ptr<juce::AudioPluginInstance> shared (inst.release());
            r.setPlugin (0, shared);
            auto out = runRig (r, sine (300.0, 48000.0, 4800, 0.8f), 256);
            expectWithinAbsoluteError (peakOf (out, 2400), 0.4f, 0.003f);   // default gain 0.5

            shared->getParameters()[0]->setValueNotifyingHost (0.75f);   // normalised 0.75 of 0..2 -> gain 1.5
            out = runRig (r, sine (300.0, 48000.0, 4800, 0.4f), 256);
            expectWithinAbsoluteError (peakOf (out, 2400), 0.6f, 0.01f);
            const auto state = fx::capturePluginState (*shared);
            expect (state.isNotEmpty());

            r.setParam (rg::plug1On, 0);   // bypassed
            out = runRig (r, sine (300.0, 48000.0, 4800, 0.4f), 256);
            expectWithinAbsoluteError (peakOf (out, 2400), 0.4f, 0.003f);

            auto old = r.setPlugin (0, nullptr);
            expect (old == shared);

            // the project node carries the state into an offline (export) rig
            auto tree = rg::makeDefault();
            for (int b = 0; b < rg::numBlocks; ++b) rg::set (tree, rg::switchFor (b), 0.0f);
            rg::setPlugin (tree, 0, desc, "FF Test Gain");
            rg::pluginNode (tree, 0).setProperty (rg::ids::state, state, nullptr);
            juce::StringArray warnings;
            auto offline = fx::makeOfflineRig (tree, 48000.0, 512, warnings);
            expect (warnings.isEmpty(), warnings.joinIntoString ("; "));
            out = runRig (*offline, sine (300.0, 48000.0, 4800, 0.4f), 512);
            expectWithinAbsoluteError (peakOf (out, 2400), 0.6f, 0.01f);
            offline.reset();

            // track inserts: two gain plugins in a chain (0.5 x 1.5), bypass, order
            juce::ValueTree track ("TRACK");
            rg::addInsert (track, desc, "A");
            rg::addInsert (track, desc, "B");
            rg::insertNode (track, 1).setProperty (rg::ids::state, state, nullptr);   // gain 1.5
            warnings.clear();
            auto chain = fx::makeOfflineInserts (track, 48000.0, 512, warnings);
            expect (warnings.isEmpty(), warnings.joinIntoString ("; "));
            auto run = [&] (fx::InsertChain& c, float amp) {
                auto l = sine (300.0, 48000.0, 4800, amp), r2 = l;
                for (size_t p = 0; p < l.size(); p += 480) c.process (l.data() + p, r2.data() + p, 480);
                return peakOf (l, 2400);
            };
            expectWithinAbsoluteError (run (*chain, 0.4f), 0.4f * 0.5f * 1.5f, 0.01f);
            chain->bypass[0] = true;
            expectWithinAbsoluteError (run (*chain, 0.4f), 0.4f * 1.5f, 0.01f);
            chain->setOrder ({ { 0 } }, 1);   // only slot 0 (bypassed) left in the order
            expectWithinAbsoluteError (run (*chain, 0.4f), 0.4f, 0.003f);

            // many plugins on one track (more than the old limit of 8 / the old 14-entry order)
            juce::ValueTree many ("TRACK");
            for (int i = 0; i < 20; ++i)
            {
                rg::addInsert (many, desc, "G" + juce::String (i));
                if (i % 2 == 1) rg::insertNode (many, i).setProperty (rg::ids::state, state, nullptr);   // alternate 0.5 and 1.5
            }
            expectEquals (rg::numInserts (many), 20);
            warnings.clear();
            auto big = fx::makeOfflineInserts (many, 48000.0, 512, warnings);
            expect (big != nullptr && warnings.isEmpty(), warnings.joinIntoString ("; "));
            if (big != nullptr) expectWithinAbsoluteError (run (*big, 0.4f), 0.4f * std::pow (0.75f, 10.0f), 0.002f);

            // slot numbers with gaps (a damaged or hand-edited project): every plugin in the list still plays
            juce::ValueTree gappy ("TRACK");
            rg::addInsert (gappy, desc, "A");
            rg::addInsert (gappy, desc, "B");
            rg::findInserts (gappy).getChild (0).setProperty (rg::ids::slot, 2, nullptr);
            rg::findInserts (gappy).getChild (1).setProperty (rg::ids::slot, 5, nullptr);
            warnings.clear();
            auto gapChain = fx::makeOfflineInserts (gappy, 48000.0, 512, warnings);
            expect (gapChain != nullptr && warnings.isEmpty(), warnings.joinIntoString ("; "));
            if (gapChain != nullptr) expectWithinAbsoluteError (run (*gapChain, 0.4f), 0.4f * 0.25f, 0.005f);

            // live master bus plugins: they keep working when the audio device restarts (with the same or another
            // format) after they were loaded, also with gaps in their slot numbers
            {
                AudioEngine e;
                e.prepare (48000.0, 256);
                fx::RigBank bank (e);
                auto projectTree = project::createDefault ("T");
                auto master = project::masterNode (projectTree);
                rg::addInsert (master, desc, "M1");
                rg::addInsert (master, desc, "M2");
                rg::addInsert (master, desc, "M3");
                rg::findInserts (master).getChild (2).setProperty (rg::ids::slot, 7, nullptr);   // 0, 1, 7
                bank.rebind (projectTree);
                bank.finishLoads();   // plugins finish loading on worker threads and are handed over on the message thread
                auto live = bank.insertsForTrack (project::masterId);
                expect (live != nullptr);
                if (live != nullptr)
                {
                    expect (bank.insertStatus (master, 0).loaded && bank.insertStatus (master, 1).loaded);
                    expectWithinAbsoluteError (run (*live, 0.4f), 0.4f * 0.125f, 0.004f);   // three gains of 0.5
                    e.prepare (48000.0, 256);   // the device restarts with the same format (ASIO reset at start-up)
                    expectWithinAbsoluteError (run (*live, 0.4f), 0.4f * 0.125f, 0.004f);
                    e.prepare (44100.0, 512);   // ...and with another one
                    expect (bank.insertsForTrack (project::masterId) == live);
                    expectWithinAbsoluteError (run (*live, 0.4f), 0.4f * 0.125f, 0.004f);

                    // settings are only read back from plugins that reported a change
                    auto p0 = bank.insertPlugin (master, 0);
                    expect (p0 != nullptr);
                    if (p0 != nullptr)
                    {
                        bank.captureAllPluginStates (true);              // new plugins are captured once
                        const auto before = master.getChildWithName (rg::ids::INSERTS).getChild (0)[rg::ids::state].toString();
                        expect (before.isNotEmpty());
                        expect (! fx::pluginStateNeedsCapture (*p0), "nothing changed since the capture");
                        p0->getParameters()[0]->setValueNotifyingHost (0.75f);   // gain 1.5
                        run (*live, 0.4f);   // (the running engine processes a block; a plugin takes the new value into its state there)
                        expect (fx::pluginStateNeedsCapture (*p0));
                        fx::markPluginStateChanged (*p0);
                        bank.captureAllPluginStates (true);
                        expect (master.getChildWithName (rg::ids::INSERTS).getChild (0)[rg::ids::state].toString() != before);
                        expectWithinAbsoluteError (run (*live, 0.4f), 0.4f * 1.5f * 0.25f, 0.01f);
                    }
                }
                bank.rebind (project::createDefault ("Empty"));   // the project is replaced: its plugins go
                expect (bank.insertsForTrack (project::masterId) == nullptr);
            }
            fx::setPluginHost (nullptr);
        }
    }
};

static PluginSlotTests pluginSlotTests;
