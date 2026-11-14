#include "Timeline.h"

namespace ff::timeline
{
juce::String newId() { return juce::Uuid().toDashedString(); }

const juce::StringArray& trackPalette()
{
    static const juce::StringArray p { "ffe0803a", "ff4c9bf5", "ff46c38a", "ffd65db1", "ffe8c547",
                                       "ff8f7cf0", "ff3fc1c9", "ffe5484d", "ff9bc53d", "ffc98a55" };
    return p;
}

juce::ValueTree createTrack (const juce::String& name, int paletteIndex)
{
    juce::ValueTree t (ids::TRACK);
    t.setProperty (ids::id, newId(), nullptr);
    t.setProperty (ids::name, name, nullptr);
    t.setProperty (ids::colour, trackPalette()[paletteIndex % trackPalette().size()], nullptr);
    t.setProperty (ids::gainDb, 0.0, nullptr);
    t.setProperty (ids::pan, 0.0, nullptr);
    t.setProperty (ids::mute, false, nullptr);
    t.setProperty (ids::solo, false, nullptr);
    t.setProperty (ids::armed, false, nullptr);
    t.setProperty (ids::monitor, false, nullptr);
    t.setProperty (ids::inputChannel, 0, nullptr);
    t.setProperty (ids::inputStereo, false, nullptr);
    return t;
}

TrackKind kindOf (const juce::ValueTree& t)
{
    const auto k = t[ids::kind].toString();
    if (k == "bus") return TrackKind::bus;
    if (k == "fx") return TrackKind::fx;
    if (k == "instrument") return TrackKind::instrument;
    return TrackKind::audio;
}

juce::String kindName (TrackKind k)
{
    return k == TrackKind::bus ? "bus" : k == TrackKind::fx ? "fx" : k == TrackKind::instrument ? "instrument" : "audio";
}

juce::ValueTree createBusTrack (const juce::String& name, int paletteIndex, TrackKind kind)
{
    auto t = createTrack (name, paletteIndex);
    t.setProperty (ids::kind, kindName (kind), nullptr);
    return t;
}

bool acceptsClip (const juce::ValueTree& track, const juce::ValueTree& clip)
{
    const bool midiClip = (bool) clip["midi"];
    return midiClip ? isInstrumentTrack (track) : isAudioTrack (track);
}

juce::ValueTree findTrack (const juce::ValueTree& tracks, const juce::String& id)
{
    if (id.isEmpty()) return {};
    for (const auto& t : tracks)
        if (t.hasType (ids::TRACK) && t[ids::id].toString() == id) return t;
    return {};
}

juce::String outputOf (const juce::ValueTree& t)
{
    return t[ids::output].toString();
}

namespace
{
    /** Every track `t` feeds (its output and its sends). */
    juce::StringArray destinationsOf (const juce::ValueTree& t)
    {
        juce::StringArray d;
        if (outputOf (t).isNotEmpty()) d.add (outputOf (t));
        for (const auto& c : t)
            if (c.hasType (ids::SEND) && c[ids::target].toString().isNotEmpty()) d.addIfNotAlreadyThere (c[ids::target].toString());
        return d;
    }
}

bool canRouteTo (const juce::ValueTree& tracks, const juce::ValueTree& from, const juce::ValueTree& to, bool toInstrumentInput)
{
    if (! from.isValid() || ! to.isValid() || from == to) return false;
    if (isSourceTrack (to) && ! (toInstrumentInput && isInstrumentTrack (to))) return false;
    // would `to` reach `from`? (then from -> to closes a loop)
    juce::StringArray seen, stack { to[ids::id].toString() };
    const auto fromId = from[ids::id].toString();
    while (! stack.isEmpty())
    {
        const auto id = stack[stack.size() - 1];
        stack.remove (stack.size() - 1);
        if (id == fromId) return false;
        if (seen.contains (id)) continue;
        seen.add (id);
        stack.addArray (destinationsOf (findTrack (tracks, id)));
    }
    return true;
}

juce::ValueTree addSend (juce::ValueTree track, const juce::String& targetId, double levelDb, bool preFader, juce::UndoManager* um)
{
    juce::ValueTree s (ids::SEND);
    s.setProperty (ids::id, newId(), nullptr);
    s.setProperty (ids::target, targetId, nullptr);
    s.setProperty (ids::level, levelDb, nullptr);
    s.setProperty (ids::pre, preFader, nullptr);
    track.appendChild (s, um);
    return s;
}

void removeRoutesTo (juce::ValueTree tracks, const juce::String& id, juce::UndoManager* um)
{
    for (auto t : tracks)
    {
        if (! t.hasType (ids::TRACK)) continue;
        if (outputOf (t) == id) t.setProperty (ids::output, juce::String(), um);
        for (int i = t.getNumChildren(); --i >= 0;)
            if (t.getChild (i).hasType (ids::SEND) && t.getChild (i)[ids::target].toString() == id) t.removeChild (i, um);
    }
}

juce::ValueTree createClip (const juce::String& file, const juce::String& name, double start, double length, double offset)
{
    juce::ValueTree c (ids::CLIP);
    c.setProperty (ids::id, newId(), nullptr);
    c.setProperty (ids::name, name, nullptr);
    c.setProperty (ids::file, file, nullptr);
    c.setProperty (ids::start, start, nullptr);
    c.setProperty (ids::length, length, nullptr);
    c.setProperty (ids::offset, offset, nullptr);
    c.setProperty (ids::gainDb, 0.0, nullptr);
    c.setProperty (ids::fadeIn, juce::jmin (defaultFade, length * 0.25), nullptr);
    c.setProperty (ids::fadeOut, juce::jmin (defaultFade, length * 0.25), nullptr);
    c.setProperty (ids::muted, false, nullptr);
    return c;
}

juce::ValueTree createMarker (const juce::String& name, double time)
{
    juce::ValueTree m (ids::MARKER);
    m.setProperty (ids::id, newId(), nullptr);
    m.setProperty (ids::name, name, nullptr);
    m.setProperty (ids::time, time, nullptr);
    return m;
}

double clipEnd (const juce::ValueTree& clip)
{
    return (double) clip[ids::start] + (double) clip[ids::length];
}

double songEnd (const juce::ValueTree& tracks)
{
    double end = 0.0;
    for (const auto& t : tracks)
        for (const auto& c : t)
            if (c.hasType (ids::CLIP) && ! (bool) c[ids::muted])
                end = juce::jmax (end, clipEnd (c));
    return end;
}

namespace
{
    struct Cover { double start, end, leftFade, rightFade; };

    void addCover (std::vector<Cover>& covers, Cover c)
    {
        if (c.end <= c.start)
            return;
        std::vector<Cover> out;
        for (auto& x : covers)
        {
            if (x.end < c.start || x.start > c.end)
            {
                out.push_back (x);
                continue;
            }
            if (x.start < c.start) { c.start = x.start; c.leftFade = x.leftFade; }
            if (x.end > c.end) { c.end = x.end; c.rightFade = x.rightFade; }
        }
        out.push_back (c);
        std::sort (out.begin(), out.end(), [] (const Cover& a, const Cover& b) { return a.start < b.start; });
        covers.swap (out);
    }
}

std::vector<Piece> visiblePieces (const std::vector<ClipSpan>& clips)
{
    std::vector<Piece> pieces;
    std::vector<Cover> covers; // full-level regions of clips above the one being processed

    for (int i = (int) clips.size() - 1; i >= 0; --i)
    {
        const auto& c = clips[(size_t) i];
        if (c.muted || c.length <= 0.0)
            continue;
        const double s = c.start, e = c.start + c.length;
        const double fi = juce::jlimit (0.0, c.length, c.fadeIn);
        const double fo = juce::jlimit (0.0, c.length - fi, c.fadeOut);

        // subtract covered regions from [s, e)
        double cursor = s;
        double leftFadeForNext = -1.0; // fade length inherited from a cover ending at cursor
        auto emit = [&] (double a, double b, double inheritedIn, double inheritedOut) {
            if (b - a <= 1.0e-9)
                return;
            Piece p;
            p.clip = c.index;
            p.start = a;
            p.end = b;
            p.srcAtStart = c.offset + (a - s);
            p.gain = c.gain;
            if (inheritedIn >= 0.0) { p.fadeInStart = a; p.fadeInEnd = juce::jmin (b, a + inheritedIn); }
            else                    { p.fadeInStart = s; p.fadeInEnd = s + fi; }
            if (inheritedOut >= 0.0) { p.fadeOutStart = juce::jmax (a, b - inheritedOut); p.fadeOutEnd = b; }
            else                     { p.fadeOutStart = e - fo; p.fadeOutEnd = e; }
            pieces.push_back (p);
        };

        for (auto& cv : covers)
        {
            if (cv.end <= cursor || cv.start >= e)
                continue;
            if (cv.start > cursor)
                emit (cursor, juce::jmin (cv.start, e), leftFadeForNext, cv.start < e ? cv.leftFade : -1.0);
            cursor = juce::jmax (cursor, cv.end);
            leftFadeForNext = cv.rightFade;
            if (cursor >= e)
                break;
        }
        if (cursor < e)
            emit (cursor, e, leftFadeForNext, -1.0);

        addCover (covers, { s + fi, e - fo, fi, fo });
    }

    std::sort (pieces.begin(), pieces.end(), [] (const Piece& a, const Piece& b) { return a.start < b.start; });
    return pieces;
}
} // namespace ff::timeline
