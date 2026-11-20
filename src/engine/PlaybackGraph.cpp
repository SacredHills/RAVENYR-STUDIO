#include "PlaybackGraph.h"
#include "core/Timeline.h"
#include "core/ProjectFile.h"
#include "core/MidiClip.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace ff
{
using namespace timeline;

int trackLatency (const PlayTrack& t) noexcept
{
    int lat = (t.rig != nullptr ? t.rig->totalLatency() : 0) + (t.inserts != nullptr ? t.inserts->latency() : 0);
    if (t.instrument != nullptr)
        if (auto* p = t.instrument->activePlugin()) lat += p->getLatencySamples();
    return lat;
}

namespace
{
void addMidiRange (PlayTrack& t, juce::int64 from, int n, int offset) noexcept
{
    auto& ev = t.midiEvents;
    auto it = std::lower_bound (ev.begin(), ev.end(), from, [] (const PlayTrack::MidiEvent& e, juce::int64 p) { return e.pos < p; });
    for (; it != ev.end() && it->pos < from + n; ++it)
    {
        const juce::uint8 bytes[3] = { it->b0, it->b1, it->b2 };
        t.midiBuffer.addEvent (bytes, 3, offset + (int) (it->pos - from));
    }
}

void allNotesOff (juce::MidiBuffer& b, int offset = 0) noexcept
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        b.addEvent (juce::MidiMessage::controllerEvent (ch, 64, 0), offset);
        b.addEvent (juce::MidiMessage::allNotesOff (ch), offset);
    }
}

/** Starting (or jumping) to `at`: notes that began a little before and are still held sound from here, so a note right
    at the start point is never lost to rounding and starting inside a long note plays it. Also the pedal state. */
void chaseNotes (PlayTrack& t, juce::int64 at, int offset) noexcept
{
    bool pedal[16] = {};
    for (const auto& e : t.midiEvents)
    {
        if (e.pos >= at) break;
        if ((e.b0 & 0xF0) == 0xB0 && e.b1 == 64) pedal[e.b0 & 0x0F] = e.b2 >= 64;
        if ((e.b0 & 0xF0) == 0x90 && e.end > at)
        {
            const juce::uint8 bytes[3] = { e.b0, e.b1, e.b2 };
            t.midiBuffer.addEvent (bytes, 3, offset);
        }
    }
    for (int ch = 0; ch < 16; ++ch)
        if (pedal[ch]) t.midiBuffer.addEvent (juce::MidiMessage::controllerEvent (ch + 1, 64, 127), offset);
}
}

void prepareTrackBlock (PlayTrack& t, const BlockTiming& bt, int n) noexcept
{
    // ---- automation ----
    if (! t.lanes.empty() && bt.rolling)
    {
        float db = t.baseGainDb, pan = t.basePan;
        bool gains = false;
        for (auto& l : t.lanes)
        {
            const float v = automation::valueAt (l.points, bt.timeSec, -1000.0f);
            if (v <= -999.0f) continue;
            if (l.target == PlayTrack::Lane::volume) { db = v; gains = true; }
            else if (l.target == PlayTrack::Lane::pan) { pan = v; gains = true; }
            else if (t.inserts != nullptr && std::abs (v - l.last) > 1.0e-4f)
                if (auto* p = t.inserts->activePlugin (l.slot))
                {
                    const auto& params = p->getParameters();
                    if (juce::isPositiveAndBelow (l.param, params.size())) params[l.param]->setValue (juce::jlimit (0.0f, 1.0f, v));
                    l.last = v;
                }
        }
        if (gains) panGains (pan, juce::Decibels::decibelsToGain (db, -60.0f), t.gainL, t.gainR);
    }

    // ---- MIDI ----
    if (t.instrument == nullptr) return;
    t.midiBuffer.clear();
    if (! bt.rolling)
    {
        if (t.midiWasRolling) allNotesOff (t.midiBuffer);   // stopped: let go of every note
        t.midiWasRolling = false;
        t.midiNextPos = -1;
        return;
    }
    const bool jumped = ! t.midiWasRolling || bt.from != t.midiNextPos;   // started, sought or looped
    if (t.midiWasRolling && jumped) allNotesOff (t.midiBuffer);
    t.midiWasRolling = true;
    if (bt.looping && bt.loopEnd > bt.loopStart && bt.from >= bt.loopEnd)
    {
        const juce::int64 f = bt.loopStart + (bt.from - bt.loopEnd) % (bt.loopEnd - bt.loopStart);
        if (jumped) chaseNotes (t, f, 0);
        addMidiRange (t, f, n, 0);
        t.midiNextPos = f + n;
    }
    else if (bt.looping && bt.loopEnd > bt.loopStart && bt.from + n > bt.loopEnd)
    {
        const int first = (int) (bt.loopEnd - bt.from);
        if (jumped) chaseNotes (t, bt.from, 0);
        addMidiRange (t, bt.from, first, 0);
        allNotesOff (t.midiBuffer, first);   // wrap: notes still held at the loop end stop there
        chaseNotes (t, bt.loopStart, first);
        addMidiRange (t, bt.loopStart, n - first, first);
        t.midiNextPos = bt.loopStart + (n - first);
    }
    else
    {
        if (jumped) chaseNotes (t, bt.from, 0);
        addMidiRange (t, bt.from, n, 0);
        t.midiNextPos = bt.from + n;
    }
}

float applyMasterLanes (PlaybackGraph& g, double timeSec, float* pan) noexcept
{
    float gain = 1.0f;
    if (pan != nullptr) *pan = 0.0f;
    for (auto& l : g.masterLanes)
    {
        const float v = automation::valueAt (l.points, timeSec, -1000.0f);
        if (v <= -999.0f) continue;
        if (l.target == PlayTrack::Lane::volume) gain = juce::Decibels::decibelsToGain (v, -60.0f);
        else if (l.target == PlayTrack::Lane::pan) { if (pan != nullptr) *pan = juce::jlimit (-1.0f, 1.0f, v); }
        else if (l.target == PlayTrack::Lane::plugin && g.masterInserts != nullptr && std::abs (v - l.last) > 1.0e-4f)
            if (auto* p = g.masterInserts->activePlugin (l.slot))
            {
                const auto& params = p->getParameters();
                if (juce::isPositiveAndBelow (l.param, params.size())) params[l.param]->setValue (juce::jlimit (0.0f, 1.0f, v));
                l.last = v;
            }
    }
    return gain;
}

void panGains (float pan, float gain, float& left, float& right) noexcept
{
    const float theta = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    left = gain * juce::MathConstants<float>::sqrt2 * std::cos (theta);
    right = gain * juce::MathConstants<float>::sqrt2 * std::sin (theta);
}

std::unique_ptr<PlaybackGraph> buildPlaybackGraph (const juce::ValueTree& tracks, const juce::File& mediaBundle,
                                                   AudioCache& cache, const GraphBuildOptions& opt)
{
    auto g = std::make_unique<PlaybackGraph>();
    g->sampleRate = opt.sampleRate;
    g->masterInserts = opt.masterInserts;
    if (opt.masterNode.isValid())
        for (const auto& lane : automation::activeLanes (opt.masterNode))
        {
            PlayTrack::Lane l;
            const auto prm = lane[automation::param].toString();
            if (prm == automation::volumeParam) l.target = PlayTrack::Lane::volume;
            else if (prm == automation::panParam) l.target = PlayTrack::Lane::pan;
            else if (automation::parsePluginParam (prm, l.slot, l.param)) l.target = PlayTrack::Lane::plugin;
            else continue;
            l.points = automation::pointsOf (lane);
            if (! l.points.empty()) g->masterLanes.push_back (std::move (l));
        }
    const double sr = opt.sampleRate;

    bool anySolo = false;
    for (const auto& t : tracks)
        anySolo = anySolo || (t.hasType (ids::TRACK) && (bool) t[ids::solo]);
    std::vector<juce::ValueTree> trees;

    int slot = 0;
    for (const auto& t : tracks)
    {
        if (! t.hasType (ids::TRACK) || slot >= maxTracks)
            continue;
        PlayTrack pt;
        pt.meterSlot = slot++;
        const float gain = juce::Decibels::decibelsToGain ((float) (double) t[ids::gainDb], -96.0f);
        panGains ((float) (double) t[ids::pan], gain, pt.gainL, pt.gainR);
        pt.kind = kindOf (t);
        pt.key = t[ids::id].toString().hashCode64();
        // buses and FX returns stay audible when other tracks are soloed (the soloed track plays through them)
        pt.audible = ! (bool) t[ids::mute];   // solo is applied below, once the routing is known
        pt.monitor = (bool) t[ids::monitor];
        pt.armed = (bool) t[ids::armed];
        pt.inputLeft = (int) t[ids::inputChannel];
        pt.monoPlayback = kindOf (t) == TrackKind::audio && ! (bool) t[ids::inputStereo];
        pt.inputRight = (bool) t[ids::inputStereo] ? pt.inputLeft + 1 : -1;

        if (opt.recorders != nullptr)
            if (auto it = opt.recorders->find (t[ids::id].toString()); it != opt.recorders->end())
                pt.recorder = it->second;
        if (opt.midiRecorders != nullptr)
            if (auto it = opt.midiRecorders->find (t[ids::id].toString()); it != opt.midiRecorders->end())
                pt.midiRecorder = it->second;
        if (opt.rigFor && pt.kind == TrackKind::audio) pt.rig = opt.rigFor (t);   // NAM amps live on audio tracks
        if (opt.insertsFor) pt.inserts = opt.insertsFor (t);
        pt.baseGainDb = (float) (double) t[ids::gainDb];
        pt.basePan = (float) (double) t[ids::pan];

        // automation lanes with points
        for (const auto& lane : automation::activeLanes (t))
        {
            PlayTrack::Lane l;
            const auto prm = lane[automation::param].toString();
            if (prm == automation::volumeParam) l.target = PlayTrack::Lane::volume;
            else if (prm == automation::panParam) l.target = PlayTrack::Lane::pan;
            else if (automation::parsePluginParam (prm, l.slot, l.param)) l.target = PlayTrack::Lane::plugin;
            else continue;
            l.points = automation::pointsOf (lane);
            if (! l.points.empty()) pt.lanes.push_back (std::move (l));
        }

        // instrument tracks: the instrument and the notes of the MIDI clips
        if (pt.kind == TrackKind::instrument)
        {
            if (opt.instrumentFor) pt.instrument = opt.instrumentFor (t);
            auto toS = [sr] (double sec) { return (juce::int64) std::llround (sec * sr); };
            for (const auto& c : t)
            {
                if (! midiclip::isMidiClip (c) || (bool) c[ids::muted]) continue;
                const double cs = (double) c[ids::start], len = (double) c[ids::length], off = (double) c[ids::offset];
                const double ce = cs + len;
                const int ch = juce::jlimit (1, 16, (int) c.getProperty (midiclip::channel, 1));
                for (const auto& nt : midiclip::notesOf (c))
                {
                    const double on = cs + (nt.start - off);
                    if (on < cs - 1.0e-9 || on >= ce) continue;
                    const double offT = juce::jmin (ce, on + nt.length);
                    const auto vel = (juce::uint8) juce::jlimit (1, 127, nt.velocity);
                    const auto offS = juce::jmax (toS (on) + 1, toS (offT));
                    pt.midiEvents.push_back ({ toS (on), (juce::uint8) (0x90 | (ch - 1)), (juce::uint8) nt.pitch, vel, offS });
                    pt.midiEvents.push_back ({ offS, (juce::uint8) (0x80 | (ch - 1)), (juce::uint8) nt.pitch, 0 });
                }
                bool down = false;
                for (const auto& pe : midiclip::pedalOf (c))
                {
                    const double at = cs + (pe.time - off);
                    if (at < cs || at >= ce) continue;
                    pt.midiEvents.push_back ({ toS (at), (juce::uint8) (0xB0 | (ch - 1)), 64, (juce::uint8) (pe.down ? 127 : 0) });
                    down = pe.down;
                }
                if (down) pt.midiEvents.push_back ({ toS (ce), (juce::uint8) (0xB0 | (ch - 1)), 64, 0 });
            }
            // note-offs before note-ons at the same sample (repeated notes)
            std::stable_sort (pt.midiEvents.begin(), pt.midiEvents.end(), [] (const PlayTrack::MidiEvent& a, const PlayTrack::MidiEvent& b) {
                if (a.pos != b.pos) return a.pos < b.pos;
                return (a.b0 & 0xF0) == 0x80 && (b.b0 & 0xF0) != 0x80;
            });
            pt.midiBuffer.ensureSize (16384);
        }

        // gather clips in layer order with their audio
        std::vector<ClipSpan> spans;
        std::vector<std::shared_ptr<const CachedAudio>> audio;
        for (const auto& c : t)
        {
            if (! c.hasType (ids::CLIP) || midiclip::isMidiClip (c))
                continue;
            const auto f = project::Bundle::fromPortablePath (mediaBundle, c[ids::file].toString());
            std::shared_ptr<const CachedAudio> a;
            if (f != juce::File())
                a = opt.waitForAudio ? cache.loadSync (f) : cache.get (f);
            if (a == nullptr)
            {
                if (! (bool) c[ids::muted]) ++g->missingClips;
                continue;
            }
            ClipSpan s;
            s.start = (double) c[ids::start];
            s.length = (double) c[ids::length];
            s.offset = (double) c[ids::offset];
            s.fadeIn = (double) c[ids::fadeIn];
            s.fadeOut = (double) c[ids::fadeOut];
            s.gain = juce::Decibels::decibelsToGain ((double) c[ids::gainDb], -96.0);
            s.muted = (bool) c[ids::muted];
            s.index = (int) audio.size();
            spans.push_back (s);
            audio.push_back (a);
        }

        for (const auto& p : visiblePieces (spans))
        {
            PlayPiece pp;
            pp.audio = audio[(size_t) p.clip];
            auto toS = [sr] (double sec) { return (juce::int64) std::llround (sec * sr); };
            pp.start = toS (p.start);
            pp.end = toS (p.end);
            if (pp.end <= pp.start)
                continue;
            pp.step = pp.audio->sampleRate / sr;
            pp.srcAtStart = p.srcAtStart * pp.audio->sampleRate;
            pp.gain = (float) p.gain;
            pp.fadeInStart = toS (p.fadeInStart);
            pp.fadeInEnd = toS (p.fadeInEnd);
            pp.fadeOutStart = toS (p.fadeOutStart);
            pp.fadeOutEnd = toS (p.fadeOutEnd);
            pt.pieces.push_back (std::move (pp));
        }
        g->tracks.push_back (std::move (pt));
        trees.push_back (t);
    }

    // ---- routing: outputs and sends by index, then processing levels (loops are cut) ----
    std::map<juce::String, int> byId;
    for (int i = 0; i < (int) trees.size(); ++i) byId[trees[(size_t) i][ids::id].toString()] = i;
    auto indexOf = [&] (const juce::String& id) { auto it = byId.find (id); return it == byId.end() ? -1 : it->second; };
    for (int i = 0; i < (int) trees.size(); ++i)
    {
        auto& pt = g->tracks[(size_t) i];
        const auto outId = outputOf (trees[(size_t) i]);
        const int o = indexOf (outId);
        pt.output = (o >= 0 && o != i && ! isSourceKind (g->tracks[(size_t) o].kind)) ? o : -1;
        pt.hwOutput = hardwareChannelOf (outId);
        for (const auto& c : trees[(size_t) i])
            if (c.hasType (ids::SEND) && (int) (pt.sends.size() + pt.hwSends.size()) < maxSends)
            {
                if (isHardwareRoute (c[ids::target].toString()))
                {
                    pt.hwSends.push_back ({ hardwareChannelOf (c[ids::target].toString()),
                                            juce::Decibels::decibelsToGain ((float) (double) c[ids::level], -96.0f), (bool) c[ids::pre] });
                    continue;
                }
                const int target = indexOf (c[ids::target].toString());
                if (target < 0 || target == i || (isSourceKind (g->tracks[(size_t) target].kind) && g->tracks[(size_t) target].kind != TrackKind::instrument)) continue;
                if (g->tracks[(size_t) target].kind == TrackKind::instrument) g->tracks[(size_t) target].instInput = true;
                pt.sends.push_back ({ target, juce::Decibels::decibelsToGain ((float) (double) c[ids::level], -96.0f), (bool) c[ids::pre] });
            }
    }
    // instrument outputs: bus tracks fed by an extra output of an instrument track
    for (int i = 0; i < (int) trees.size(); ++i)
    {
        const auto src = trees[(size_t) i][ids::instrSource].toString();
        if (src.isEmpty() || isSourceKind (g->tracks[(size_t) i].kind)) continue;
        const int s = indexOf (src);
        if (s < 0 || s == i || g->tracks[(size_t) s].kind != TrackKind::instrument) continue;
        g->tracks[(size_t) s].auxOuts.push_back ({ i, juce::jmax (1, (int) trees[(size_t) i][ids::instrBus]) });
    }
    // side-chains: a plugin's side-chain source is another track's signal (after its plugins, before its fader)
    auto sidechainsOf = [&] (const std::shared_ptr<fx::InsertChain>& chain, int self) {
        std::vector<std::pair<int, int>> out;
        if (chain == nullptr) return out;
        for (int s = 0; s < fx::InsertChain::maxSlots; ++s)
        {
            if (chain->sidechainId[(size_t) s].isEmpty()) continue;
            const int src = indexOf (chain->sidechainId[(size_t) s]);
            if (src >= 0 && src != self) out.push_back ({ s, src });
        }
        return out;
    };
    for (int i = 0; i < (int) trees.size(); ++i) g->tracks[(size_t) i].sidechains = sidechainsOf (g->tracks[(size_t) i].inserts, i);
    g->masterSidechains = sidechainsOf (g->masterInserts, -1);
    // level = longest chain of sources; an edge that would close a loop is dropped
    const int n = (int) g->tracks.size();
    std::vector<int> state ((size_t) n, 0);   // 0 new, 1 visiting, 2 done
    std::vector<std::vector<int>> sources ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        auto& pt = g->tracks[(size_t) i];
        if (pt.output >= 0) sources[(size_t) pt.output].push_back (i);
        for (auto& sd : pt.sends) sources[(size_t) sd.target].push_back (i);
        for (auto& ax : pt.auxOuts) sources[(size_t) ax.target].push_back (i);
        for (auto& sc : pt.sidechains) sources[(size_t) i].push_back (sc.second);
    }
    std::function<int (int)> levelOf = [&] (int i) -> int {
        if (state[(size_t) i] == 2) return g->tracks[(size_t) i].level;
        state[(size_t) i] = 1;
        int lv = isSourceKind (g->tracks[(size_t) i].kind) ? 0 : 1;
        for (int src : sources[(size_t) i])
        {
            if (state[(size_t) src] == 1)   // loop: cut this route
            {
                auto& s = g->tracks[(size_t) src];
                if (s.output == i) s.output = -1;
                s.sends.erase (std::remove_if (s.sends.begin(), s.sends.end(), [i] (const PlayTrack::Send& x) { return x.target == i; }), s.sends.end());
                s.auxOuts.erase (std::remove_if (s.auxOuts.begin(), s.auxOuts.end(), [i] (const PlayTrack::AuxOut& x) { return x.target == i; }), s.auxOuts.end());
                auto& self = g->tracks[(size_t) i];   // a side-chain loop: the plugin goes without its side-chain
                self.sidechains.erase (std::remove_if (self.sidechains.begin(), self.sidechains.end(), [src] (const std::pair<int, int>& x) { return x.second == src; }), self.sidechains.end());
                continue;
            }
            lv = juce::jmax (lv, levelOf (src) + 1);
        }
        state[(size_t) i] = 2;
        g->tracks[(size_t) i].level = lv;
        return lv;
    };
    for (int i = 0; i < n; ++i) levelOf (i);

    // ---- solo: the soloed tracks, everything they play through (buses, FX returns) and everything feeding them run;
    //      tracks that only feed a soloed bus / FX return are heard through it, not on their own ----
    if (anySolo)
    {
        auto edges = [&] (int i) {
            std::vector<int> e;
            if (g->tracks[(size_t) i].output >= 0) e.push_back (g->tracks[(size_t) i].output);
            for (auto& sd : g->tracks[(size_t) i].sends) e.push_back (sd.target);
            for (auto& ax : g->tracks[(size_t) i].auxOuts) e.push_back (ax.target);
            for (int k = 0; k < n; ++k)   // a track feeding another's side-chain runs with it (heard only through it)
                for (auto& sc : g->tracks[(size_t) k].sidechains)
                    if (sc.second == i) e.push_back (k);
            return e;
        };
        std::vector<char> soloed ((size_t) n, 0), down ((size_t) n, 0), up ((size_t) n, 0);
        std::vector<int> stack;
        for (int i = 0; i < n; ++i)
            if ((bool) trees[(size_t) i][ids::solo]) { soloed[(size_t) i] = down[(size_t) i] = up[(size_t) i] = 1; stack.push_back (i); }
        for (auto work = stack; ! work.empty();)
        {
            const int i = work.back(); work.pop_back();
            for (int d : edges (i)) if (! down[(size_t) d]) { down[(size_t) d] = 1; work.push_back (d); }
        }
        for (auto work = stack; ! work.empty();)
        {
            const int i = work.back(); work.pop_back();
            for (int k = 0; k < n; ++k)
                if (! up[(size_t) k])
                    for (int d : edges (k)) if (d == i) { up[(size_t) k] = 1; work.push_back (k); break; }
        }
        for (int i = 0; i < n; ++i)
        {
            auto& pt = g->tracks[(size_t) i];
            const bool inPath = down[(size_t) i] || up[(size_t) i];
            if (! inPath) { pt.audible = false; continue; }
            auto onPath = [&] (int d) { return down[(size_t) d] || up[(size_t) d]; };
            pt.sends.erase (std::remove_if (pt.sends.begin(), pt.sends.end(), [&] (const PlayTrack::Send& x) { return ! onPath (x.target); }),
                            pt.sends.end());
            if (! down[(size_t) i]) pt.hwSends.clear();   // only soloed tracks (and what they play through) are heard on interface outputs
            if (pt.output >= 0) pt.outputMuted = ! onPath (pt.output);
            else pt.outputMuted = ! down[(size_t) i];   // only soloed tracks and what they play through reach the master
        }
    }

    g->order.resize ((size_t) n);
    for (int i = 0; i < n; ++i) g->order[(size_t) i] = i;
    std::stable_sort (g->order.begin(), g->order.end(), [&] (int a, int b) { return g->tracks[(size_t) a].level < g->tracks[(size_t) b].level; });
    return g;
}

namespace
{
    // 4-point, 3rd-order Hermite interpolation
    inline float hermite (const float* d, juce::int64 len, double pos) noexcept
    {
        const auto i = (juce::int64) std::floor (pos);
        const float f = (float) (pos - (double) i);
        auto at = [d, len] (juce::int64 k) { return (k >= 0 && k < len) ? d[k] : 0.0f; };
        const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
}

void renderTrackClips (const PlayTrack& track, juce::int64 t0, int n, float* left, float* right) noexcept
{
    const juce::int64 t1 = t0 + n;
    for (const auto& p : track.pieces)
    {
        if (p.end <= t0 || p.start >= t1)
            continue;
        const auto& buf = p.audio->buffer;
        const auto len = (juce::int64) buf.getNumSamples();
        const float* srcL = buf.getReadPointer (0);
        const float* srcR = buf.getReadPointer (buf.getNumChannels() > 1 ? 1 : 0);

        const auto a = juce::jmax (t0, p.start), b = juce::jmin (t1, p.end);
        const bool integerStep = juce::exactlyEqual (p.step, 1.0) && juce::exactlyEqual (p.srcAtStart, std::floor (p.srcAtStart));
        const bool fades = a < p.fadeInEnd || b > p.fadeOutStart;

        for (auto t = a; t < b; ++t)
        {
            float g = p.gain;
            if (fades)
                g *= (float) (fadeGain ((double) t, (double) p.fadeInStart, (double) p.fadeInEnd, true)
                              * fadeGain ((double) t, (double) p.fadeOutStart, (double) p.fadeOutEnd, false));
            float l, r;
            if (integerStep)
            {
                const auto k = (juce::int64) p.srcAtStart + (t - p.start);
                if (k < 0 || k >= len) continue;
                l = srcL[k];
                r = srcR[k];
            }
            else
            {
                const double pos = p.srcAtStart + (double) (t - p.start) * p.step;
                if (pos < -1.0 || pos >= (double) len) continue;
                l = hermite (srcL, len, pos);
                r = hermite (srcR, len, pos);
            }
            if (track.monoPlayback) l = r = 0.5f * (l + r);
            left[t - t0] += l * g;
            right[t - t0] += r * g;
        }
    }
}
} // namespace ff
