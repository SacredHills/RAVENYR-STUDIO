#include "PianoRoll.h"
#include "core/Timeline.h"
#include "core/ProjectFile.h"
#include <cmath>

namespace ff
{
namespace tl = timeline;

namespace
{
const char* gridNames[] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8 triplet", "1/16 triplet", "Off" };
const double gridQuarters[] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0, 0.0 };
const char* lengthNames[] = { "Grid", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
const double lengthQuarters[] = { 0.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
bool isBlackKey (int pitch) { const int k = pitch % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
}

PianoRoll::PianoRoll (ProjectDocument& d, EngineHost& h, juce::ValueTree c) : doc (d), host (h), clip (c)
{
    refreshMap();
    setWantsKeyboardFocus (true);
    for (int i = 0; i < (int) std::size (gridNames); ++i) gridBox.addItem (gridNames[i], i + 1);
    gridBox.setSelectedId (5, juce::dontSendNotification);   // 1/16
    gridBox.onChange = [this] { flashTicks = 0; updateInfo(); repaint(); };
    for (int i = 0; i < (int) std::size (lengthNames); ++i) lengthBox.addItem (lengthNames[i], i + 1);
    lengthBox.setSelectedId (1, juce::dontSendNotification);
    gridLabel.setText ("Grid", juce::dontSendNotification);
    lengthLabel.setText ("Length", juce::dontSendNotification);
    lengthBox.setTooltip ("Length of the notes you add (Draw tool or double-click). Grid: one grid step.");
    gridBox.setTooltip ("Grid: where notes snap when you add, move or resize them (hold Alt to place freely), and what Quantize moves notes onto");
    for (auto* l : { &gridLabel, &lengthLabel, &info })
    {
        l->setColour (juce::Label::textColourId, palette::textDim);
        l->setFont (uiFont (12.0f));
        addAndMakeVisible (*l);
    }
    drawButton.setClickingTogglesState (true);
    drawButton.setColour (juce::TextButton::buttonOnColourId, palette::accent);
    drawButton.setTooltip ("Draw: click to add notes and drag to set their length (D). Off: drag to select, double-click to add.");
    quantizeButton.setTooltip ("Move the selected notes (or all of them) onto the grid (Q)");
    quantizeButton.onClick = [this] { quantize(); };
    humanizeButton.setTooltip ("Humanize: loosen the timing, velocity and length of the selected notes (or all of them) a little");
    humanizeButton.onClick = [this] { showHumanize(); };
    zoomIn.onClick = [this] { pps = juce::jmin (4000.0, pps * 1.4); repaint(); };
    zoomOut.onClick = [this] { pps = juce::jmax (10.0, pps / 1.4); repaint(); };
    fitButton.onClick = [this] { fit(); };
    followButton.setClickingTogglesState (true);
    followButton.setToggleState (followOn(), juce::dontSendNotification);
    followButton.setColour (juce::TextButton::buttonOnColourId, palette::accent);
    followButton.setTooltip ("Follow: the editor scrolls to the play position, whether you play or click to move it in the arranger");
    followButton.onClick = [this] { followOn() = followButton.getToggleState(); if (followOn()) lastPlayhead = -1.0; };
    for (auto* c2 : std::initializer_list<juce::Component*> { &drawButton, &quantizeButton, &humanizeButton, &zoomIn, &zoomOut, &fitButton, &followButton, &gridBox, &lengthBox })
    {
        c2->setWantsKeyboardFocus (false);
        addAndMakeVisible (c2);
    }
    for (auto* sb : { &hbar, &vbar })
    {
        sb->setAutoHide (false);
        sb->addListener (this);
        sb->setWantsKeyboardFocus (false);
        addAndMakeVisible (sb);
    }
    clip.addListener (this);
    load();
    startTimerHz (30);
}

PianoRoll::~PianoRoll()
{
    stopAudition();
    clip.removeListener (this);
}

// ------------------------------------------------------------------ geometry

juce::Rectangle<int> PianoRoll::gridArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarH + rulerH).withTrimmedLeft (keysW).withTrimmedBottom (velH() + sbW).withTrimmedRight (sbW);
}
juce::Rectangle<int> PianoRoll::velArea() const
{
    return getLocalBounds().withTrimmedBottom (sbW).removeFromBottom (velH()).withTrimmedLeft (keysW).withTrimmedRight (sbW).withTrimmedTop (4);
}
juce::Rectangle<int> PianoRoll::velHandle() const
{
    const auto v = velArea();
    return { keysW, v.getY() - 6, v.getWidth() + 0, 9 };
}
juce::Rectangle<int> PianoRoll::keysArea() const
{
    return getLocalBounds().withTrimmedTop (toolbarH + rulerH).withTrimmedBottom (velH() + sbW).removeFromLeft (keysW);
}
juce::Rectangle<int> PianoRoll::rulerArea() const
{
    return { keysW, toolbarH, getWidth() - keysW - sbW, rulerH };
}

void PianoRoll::updateScrollBars()
{
    const auto g = gridArea();
    if (g.getWidth() <= 0 || g.getHeight() <= 0) return;
    const double view = g.getWidth() / pps;
    double end = clipEnd();
    for (auto& n : notes) end = juce::jmax (end, absStart (n) + n.length);
    end += 4.0 * quarterSec() * juce::jmax (1, doc.tsNum());   // (roughly four bars of room)
    const double lo = juce::jmin (0.0, scrollT), hi = juce::jmax (end, scrollT + view);
    hbar.setRangeLimits (lo, hi, juce::dontSendNotification);
    hbar.setCurrentRange (scrollT, view, juce::dontSendNotification);
    const double rows = g.getHeight() / (double) keyH;
    vbar.setRangeLimits (0.0, 128.0, juce::dontSendNotification);
    vbar.setCurrentRange (127.0 - topPitch, rows, juce::dontSendNotification);
}

void PianoRoll::scrollBarMoved (juce::ScrollBar* sb, double start)
{
    if (sb == &hbar) scrollT = start;
    else topPitch = (float) (127.0 - start);
    repaint();
}

void PianoRoll::flash (const juce::String& message)
{
    info.setText (message, juce::dontSendNotification);
    info.setColour (juce::Label::textColourId, palette::accent);
    flashTicks = 75;   // 2.5 s at 30 Hz
}
float PianoRoll::xFor (double t) const { return (float) gridArea().getX() + (float) ((t - scrollT) * pps); }
double PianoRoll::timeFor (float x) const { return scrollT + (x - (float) gridArea().getX()) / pps; }
float PianoRoll::yFor (int pitch) const { return (float) gridArea().getY() + (topPitch - (float) pitch) * keyH; }
int PianoRoll::pitchFor (float y) const { return juce::jlimit (0, 127, (int) std::floor (topPitch - (y - (float) gridArea().getY()) / keyH) + 1); }
double PianoRoll::clipStart() const { return (double) clip[tl::ids::start]; }
double PianoRoll::clipEnd() const { return clipStart() + (double) clip[tl::ids::length]; }
double PianoRoll::offset() const { return (double) clip[tl::ids::offset]; }
double PianoRoll::quarterSec() const { return 60.0 / juce::jmax (20.0, tmap.bpmAtSeconds (scrollT)); }   // at the left edge of the view
double PianoRoll::gridQuarter() const { return gridQuarters[juce::jlimit (0, (int) std::size (gridQuarters) - 1, gridBox.getSelectedId() - 1)]; }
double PianoRoll::gridSec() const { return gridQuarter() * quarterSec(); }
// The grid is on the beats of the tempo track, so snapping works in quarter notes and converts back to seconds.
double PianoRoll::snap (double t, bool on) const
{
    const double g = gridQuarter();
    if (! on || g <= 0.0) return t;
    return tmap.secondsAtQuarter (std::round (tmap.quarterAtSeconds (t) / g) * g);
}
double PianoRoll::snapDown (double t, bool on) const
{
    const double g = gridQuarter();
    if (! on || g <= 0.0) return t;
    return tmap.secondsAtQuarter (std::floor (tmap.quarterAtSeconds (t) / g + 1.0e-9) * g);
}
double PianoRoll::newNoteLength (double atSec) const
{
    double q = lengthQuarters[juce::jlimit (0, (int) std::size (lengthQuarters) - 1, lengthBox.getSelectedId() - 1)];
    if (q <= 0.0) q = gridQuarter();
    if (q <= 0.0) q = 0.25;
    const double q0 = tmap.quarterAtSeconds (atSec);
    return tmap.secondsAtQuarter (q0 + q) - tmap.secondsAtQuarter (q0);
}

juce::Rectangle<float> PianoRoll::noteRect (const midiclip::Note& n) const
{
    const float x0 = xFor (absStart (n)), x1 = xFor (absStart (n) + n.length);
    return { x0, yFor (n.pitch) + 1.0f, juce::jmax (3.0f, x1 - x0 - 1.0f), keyH - 2.0f };
}

int PianoRoll::noteAt (juce::Point<float> p, bool& edge, bool grabEnds) const
{
    const auto edgeOf = [] (const juce::Rectangle<float>& r, juce::Point<float> q) { return q.x > r.getRight() - juce::jmin (6.0f, r.getWidth() * 0.25f); };
    // 1. the note really under the cursor
    for (int i = (int) notes.size(); --i >= 0;)
    {
        const auto r = noteRect (notes[(size_t) i]);
        if (r.contains (p)) { edge = edgeOf (r, p); return i; }
    }
    if (! grabEnds) { edge = false; return -1; }
    // 2. just outside one: a few pixels past its end still grab that note's end (so the edge of a note that touches the
    //    next one is the first note's, not the neighbour's), and a pixel above / below
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto r = noteRect (notes[(size_t) i]);
        if (r.withWidth (r.getWidth() + 4.0f).expanded (0.0f, 1.0f).contains (p)) { edge = p.x > r.getRight() - juce::jmin (6.0f, r.getWidth() * 0.25f); return i; }
    }
    edge = false;
    return -1;
}

int PianoRoll::trackIndex() const { return doc.tracks().indexOf (clip.getParent()); }
int PianoRoll::numSelected() const { int n = 0; for (auto s : selected) n += s ? 1 : 0; return n; }

// ------------------------------------------------------------------ data

void PianoRoll::load()
{
    notes = midiclip::notesOf (clip);
    pedal = midiclip::pedalOf (clip);
    selected.assign (notes.size(), 0);
    updateInfo();
    repaint();
}

void PianoRoll::beginEdit (const juce::String& name)
{
    doc.undo().beginNewTransaction (name);
    changed = false;
}

void PianoRoll::commit (const juce::String& undoName)
{
    if (undoName.isNotEmpty()) doc.undo().beginNewTransaction (undoName);
    const juce::ScopedValueSetter<bool> svs (writing, true);
    midiclip::setNotes (clip, notes, pedal, &doc.undo());
    // grow the clip to hold notes past its end
    double end = clipEnd();
    for (auto& n : notes) end = juce::jmax (end, absStart (n) + n.length);
    if (end > clipEnd() + 1.0e-6) clip.setProperty (tl::ids::length, end - clipStart(), &doc.undo());
    changed = true;
    updateInfo();
    repaint();
}

void PianoRoll::updateInfo()
{
    if (flashTicks > 0) return;
    info.setColour (juce::Label::textColourId, palette::textDim);
    const int s = numSelected();
    juce::String t = juce::String ((int) notes.size()) + " notes";
    if (s > 0)
    {
        t << ", " << s << " selected";
        if (s == 1)
            for (size_t i = 0; i < notes.size(); ++i)
                if (selected[i])
                    t << ":  " << juce::MidiMessage::getMidiNoteName (notes[i].pitch, true, true, 4) << "  velocity " << notes[i].velocity;
    }
    info.setText (t, juce::dontSendNotification);
}

void PianoRoll::audition (int pitch, int velocity)
{
    stopAudition();
    keyboardNote = pitch;
    host.engine.pushLiveMidi (juce::MidiMessage::noteOn (1, pitch, (juce::uint8) juce::jlimit (1, 127, velocity)), trackIndex());
}

void PianoRoll::stopAudition()
{
    if (keyboardNote >= 0) host.engine.pushLiveMidi (juce::MidiMessage::noteOff (1, keyboardNote), trackIndex());
    keyboardNote = -1;
}

// ------------------------------------------------------------------ editing

void PianoRoll::quantize()
{
    if (notes.empty()) { flash ("No notes to quantize"); return; }
    if (gridSec() <= 0.0) { flash ("Grid is Off: pick a grid (e.g. 1/16) to quantize to"); return; }
    const bool any = numSelected() > 0;
    int moved = 0, count = 0;
    for (size_t i = 0; i < notes.size(); ++i)
        if (! any || selected[i])
        {
            auto& n = notes[i];
            ++count;
            const double a = snap (absStart (n), true);
            const double s = a - clipStart() + offset();
            if (std::abs (s - n.start) > 1.0e-5) { n.start = s; ++moved; }
        }
    const auto gridName = gridBox.getText();
    if (moved == 0)
    {
        flash (juce::String (any ? "The selected notes are" : "All notes are") + " already on the " + gridName + " grid");
        return;
    }
    commit ("Quantize notes");
    flash ("Quantized " + juce::String (moved) + " of " + juce::String (count) + " notes to " + gridName);
}

namespace
{
    struct HumanizeSettings { double timingMs = 12.0, velocity = 10.0, lengthPct = 5.0; };
    HumanizeSettings& humanizeSettings() { static HumanizeSettings h; return h; }

    class HumanizePanel final : public juce::Component
    {
    public:
        explicit HumanizePanel (std::function<void()> applyFn) : onApply (std::move (applyFn))
        {
            auto& h = humanizeSettings();
            setup (timing, "Timing (± ms)", 0.0, 60.0, 1.0, h.timingMs);
            setup (vel, "Velocity (± steps)", 0.0, 40.0, 1.0, h.velocity);
            setup (len, "Length (± %)", 0.0, 40.0, 1.0, h.lengthPct);
            timing.onValueChange = [this] { humanizeSettings().timingMs = timing.getValue(); };
            vel.onValueChange = [this] { humanizeSettings().velocity = vel.getValue(); };
            len.onValueChange = [this] { humanizeSettings().lengthPct = len.getValue(); };
            apply.onClick = [this] { if (onApply) onApply(); };
            addAndMakeVisible (apply);
            setSize (260, 190);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 8);
            int i = 0;
            for (auto* s : { &timing, &vel, &len }) { labels[i++]->setBounds (r.removeFromTop (14)); s->setBounds (r.removeFromTop (24)); r.removeFromTop (6); }
            apply.setBounds (r.removeFromTop (26));
        }
    private:
        void setup (juce::Slider& s, const juce::String& name, double lo, double hi, double step, double v)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
            s.setRange (lo, hi, step);
            s.setValue (v, juce::dontSendNotification);
            addAndMakeVisible (s);
            auto* l = labels.add (new juce::Label ({}, name));
            l->setFont (uiFont (11.0f));
            l->setColour (juce::Label::textColourId, palette::textDim);
            l->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (l);
        }
        void paintOverChildren (juce::Graphics&) override {}
        juce::Slider timing, vel, len;
        juce::TextButton apply { "Humanize" };
        juce::OwnedArray<juce::Label> labels;
        std::function<void()> onApply;
    };
}

void PianoRoll::showHumanize()
{
    if (notes.empty()) { flash ("No notes to humanize"); return; }
    auto panel = std::make_unique<HumanizePanel> ([this] {
        const auto h = humanizeSettings();
        const bool any = numSelected() > 0;
        static juce::Random rng;
        auto jitter = [&] (double amt) { return (rng.nextDouble() * 2.0 - 1.0) * amt; };
        int count = 0;
        for (size_t i = 0; i < notes.size(); ++i)
            if (! any || selected[i])
            {
                auto& n = notes[i];
                n.start = juce::jmax (0.0, n.start + jitter (h.timingMs / 1000.0));
                n.velocity = juce::jlimit (1, 127, n.velocity + juce::roundToInt (jitter (h.velocity)));
                n.length = juce::jmax (0.01, n.length * (1.0 + jitter (h.lengthPct / 100.0)));
                ++count;
            }
        commit ("Humanize notes");
        flash ("Humanized " + juce::String (count) + " notes");
    });
    juce::CallOutBox::launchAsynchronously (std::move (panel), humanizeButton.getScreenBounds(), nullptr);
}

namespace
{
    std::vector<midiclip::Note> noteClipboard;   // shared by all MIDI editors; starts relative to the first note
}

void PianoRoll::copySelected()
{
    if (numSelected() == 0) { flash ("Select notes to copy"); return; }
    double first = 1.0e12;
    for (size_t i = 0; i < notes.size(); ++i) if (selected[i]) first = juce::jmin (first, absStart (notes[i]));
    noteClipboard.clear();
    for (size_t i = 0; i < notes.size(); ++i)
        if (selected[i])
        {
            auto n = notes[i];
            n.start = absStart (n) - first;
            noteClipboard.push_back (n);
        }
    flash ("Copied " + juce::String ((int) noteClipboard.size()) + (noteClipboard.size() == 1 ? " note" : " notes"));
}

void PianoRoll::pasteAt (double t)
{
    if (noteClipboard.empty()) { flash ("Nothing to paste: copy some notes first (Ctrl+C)"); return; }
    const double base = juce::jmax (clipStart(), t);
    selected.assign (notes.size(), 0);
    for (auto n : noteClipboard)
    {
        n.start = base + n.start - clipStart() + offset();
        notes.push_back (n);
        selected.push_back (1);
    }
    commit ("Paste notes");
    flash ("Pasted " + juce::String ((int) noteClipboard.size()) + (noteClipboard.size() == 1 ? " note" : " notes"));
}

void PianoRoll::duplicateSelected()
{
    if (numSelected() == 0) { flash ("Select notes to duplicate"); return; }
    const auto saved = noteClipboard;
    double first = 1.0e12, last = 0.0;
    for (size_t i = 0; i < notes.size(); ++i)
        if (selected[i]) { first = juce::jmin (first, absStart (notes[i])); last = juce::jmax (last, absStart (notes[i]) + notes[i].length); }
    copySelected();
    const double gq = gridQuarter(), fq = tmap.quarterAtSeconds (first), lq = tmap.quarterAtSeconds (last);
    const double spanQ = gq > 0.0 ? std::ceil ((lq - fq) / gq - 1.0e-6) * gq : lq - fq;
    pasteAt (tmap.secondsAtQuarter (fq + juce::jmax (spanQ, gq > 0.0 ? gq : 0.01)));
    noteClipboard = saved;   // duplicating leaves the clipboard alone
    flashTicks = 0;
    updateInfo();
}

void PianoRoll::deleteSelected()
{
    if (numSelected() == 0) return;
    std::vector<midiclip::Note> keep;
    for (size_t i = 0; i < notes.size(); ++i) if (! selected[i]) keep.push_back (notes[i]);
    notes = std::move (keep);
    selected.assign (notes.size(), 0);
    commit ("Delete notes");
}

void PianoRoll::transpose (int semis)
{
    if (numSelected() == 0) return;
    for (size_t i = 0; i < notes.size(); ++i) if (selected[i]) notes[i].pitch = juce::jlimit (0, 127, notes[i].pitch + semis);
    commit ("Transpose notes");
}

void PianoRoll::nudge (int steps)
{
    const double g = gridQuarter() > 0.0 ? gridQuarter() : 0.25;
    if (numSelected() == 0) return;
    for (size_t i = 0; i < notes.size(); ++i)
        if (selected[i])
        {
            const double q = tmap.quarterAtSeconds (absStart (notes[i])) + steps * g;
            notes[i].start = juce::jmax (offset() - clipStart() + 0.0, tmap.secondsAtQuarter (juce::jmax (0.0, q)) - clipStart() + offset());
        }
    commit ("Move notes");
}

void PianoRoll::fit()
{
    const auto g = gridArea();
    double a = clipStart(), b = clipEnd();
    int lo = 60, hi = 72;
    if (midiclip::pitchRange (notes, lo, hi)) {}
    pps = juce::jlimit (10.0, 4000.0, (g.getWidth() - 20.0) / juce::jmax (0.25, b - a));
    scrollT = a - 10.0 / pps;
    const int rows = juce::jmax (12, hi - lo + 5);
    keyH = juce::jlimit (6.0f, 24.0f, (float) g.getHeight() / (float) rows);
    topPitch = (float) juce::jmin (127, hi + 2 + juce::jmax (0, ((int) (g.getHeight() / keyH) - rows) / 2));
    repaint();
}

// ------------------------------------------------------------------ painting

void PianoRoll::paint (juce::Graphics& g)
{
    refreshMap();
    updateScrollBars();
    g.fillAll (palette::bg);
    const auto grid = gridArea();
    const auto keys = keysArea();
    const auto vel = velArea();

    // toolbar background
    g.setColour (palette::panel);
    g.fillRect (getLocalBounds().removeFromTop (toolbarH));

    // grid rows
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (grid);
        // row fills on whole pixels (no seams), then the lines in a second pass so no row paints over its neighbour's
        // line: a line under every row, stronger between E / F and at each octave (under C)
        for (int p = 127; p >= 0; --p)
        {
            const float y = yFor (p);
            if (y > (float) grid.getBottom() || y + keyH < (float) grid.getY()) continue;
            const int y0 = juce::roundToInt (y), y1 = juce::roundToInt (y + keyH);
            g.setColour (isBlackKey (p) ? palette::bg.darker (0.45f) : palette::bg.brighter (0.07f));
            g.fillRect (grid.getX(), y0, grid.getWidth(), y1 - y0);
        }
        for (int p = 127; p >= 0; --p)
        {
            const float y = yFor (p);
            if (y > (float) grid.getBottom() || y + keyH < (float) grid.getY()) continue;
            const int k = p % 12;
            g.setColour (k == 0 ? juce::Colours::white.withAlpha (0.24f) : k == 5 ? juce::Colours::white.withAlpha (0.15f) : juce::Colours::white.withAlpha (0.08f));
            g.fillRect (grid.getX(), juce::roundToInt (y + keyH) - 1, grid.getWidth(), 1);
        }
        // outside the clip: darker
        const float cx0 = xFor (clipStart()), cx1 = xFor (clipEnd());
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> ((float) grid.getX(), (float) grid.getY(), cx0 - (float) grid.getX(), (float) grid.getHeight()));
        g.fillRect (juce::Rectangle<float> (cx1, (float) grid.getY(), (float) grid.getRight() - cx1, (float) grid.getHeight()));

        // grid: a line at every grid step (bars strongest, then beats, then the rest); alternate bars shaded
        const int num = juce::jmax (1, doc.tsNum()), den = juce::jmax (1, doc.tsDen());
        const double beatQ = 4.0 / den, barQ = beatQ * num;
        const double t0 = timeFor ((float) grid.getX()), t1 = timeFor ((float) grid.getRight());
        const double q0 = tmap.quarterAtSeconds (t0), q1 = tmap.quarterAtSeconds (t1);
        for (auto b = (juce::int64) std::floor (q0 / barQ); (double) b * barQ < q1; ++b)
            if ((b & 1) != 0)
            {
                const float xa = xFor (tmap.secondsAtQuarter ((double) b * barQ)), xb = xFor (tmap.secondsAtQuarter ((double) (b + 1) * barQ));
                g.setColour (juce::Colours::white.withAlpha (0.025f));
                g.fillRect (juce::Rectangle<float> (xa, (float) grid.getY(), xb - xa, (float) grid.getHeight()));
            }
        double step = gridQuarter() > 0.0 ? gridQuarter() : beatQ;
        const auto stepPx = [&] (double st) { return (tmap.secondsAtQuarter (q0 + st) - tmap.secondsAtQuarter (q0)) * pps; };
        while (stepPx (step) < 6.0 && step < barQ) step = step < beatQ ? beatQ : barQ;   // too dense to draw: coarser lines
        const auto onMultiple = [] (double q, double m) { return std::abs (std::remainder (q, m)) < 1.0e-6; };
        for (auto k = (juce::int64) std::floor (q0 / step); (double) k * step < q1; ++k)
        {
            const double q = (double) k * step;
            const bool isBar = onMultiple (q, barQ), isBeat = onMultiple (q, beatQ);
            g.setColour (juce::Colours::white.withAlpha (isBar ? 0.42f : isBeat ? 0.2f : 0.08f));
            g.drawVerticalLine ((int) xFor (tmap.secondsAtQuarter (q)), (float) grid.getY(), (float) grid.getBottom());
        }

        // notes
        const auto colour = palette::graph;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            const auto r = noteRect (n);
            if (r.getRight() < (float) grid.getX() || r.getX() > (float) grid.getRight()) continue;
            const bool inside = absStart (n) < clipEnd() && absStart (n) >= clipStart() - 1.0e-9;
            auto c = colour.withMultipliedBrightness (0.55f + 0.45f * (float) n.velocity / 127.0f);
            if (! inside) c = c.withAlpha (0.35f);
            g.setColour (selected[i] ? juce::Colours::white : c);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (selected[i] ? palette::text : c.darker (0.5f));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);
        }
        if (! marquee.isEmpty())
        {
            g.setColour (palette::info.withAlpha (0.15f));
            g.fillRect (marquee);
            g.setColour (palette::info);
            g.drawRect (marquee, 1.0f);
        }
        // playhead
        const float px = xFor (host.engine.getPositionSeconds());
        g.setColour (palette::accent);
        g.drawVerticalLine ((int) px, (float) grid.getY(), (float) grid.getBottom());
    }

    // ruler
    {
        auto ruler = juce::Rectangle<int> (grid.getX(), toolbarH, grid.getWidth(), rulerH);
        g.setColour (palette::well);
        g.fillRect (ruler);
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (ruler);
        const double barQ = 4.0 / juce::jmax (1, doc.tsDen()) * juce::jmax (1, doc.tsNum());
        g.setFont (uiFont (10.5f));
        const double t0 = timeFor ((float) grid.getX()), t1 = timeFor ((float) grid.getRight());
        const double q0 = tmap.quarterAtSeconds (t0);
        const double barPx = (tmap.secondsAtQuarter (q0 + barQ) - tmap.secondsAtQuarter (q0)) * pps;
        const int step = barPx < 40.0 ? (barPx < 12.0 ? 8 : 4) : 1;
        for (int b = juce::jmax (0, (int) std::floor (q0 / barQ)); tmap.secondsAtQuarter (b * barQ) < t1; ++b)
        {
            if (b % step != 0) continue;
            const float x = xFor (tmap.secondsAtQuarter (b * barQ));
            g.setColour (palette::textDim);
            g.drawVerticalLine ((int) x, (float) ruler.getY() + 8.0f, (float) ruler.getBottom());
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (x + 3.0f, (float) ruler.getY(), 40.0f, (float) rulerH), juce::Justification::centredLeft);
        }
        g.setColour (palette::accent.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> (xFor (clipStart()), (float) ruler.getBottom() - 3.0f, xFor (clipEnd()) - xFor (clipStart()), 3.0f));
        // playhead marker (click or drag in the ruler to move it)
        const float px = xFor (host.engine.getPositionSeconds());
        juce::Path tri;
        tri.addTriangle (px - 6.0f, (float) ruler.getY() + 6.0f, px + 6.0f, (float) ruler.getY() + 6.0f, px, (float) ruler.getBottom());
        g.setColour (palette::accent);
        g.fillPath (tri);
    }

    // keyboard
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (keys);
        // white keys first (each spans its row plus half of a neighbouring black key's), then the black keys over them
        g.setColour (juce::Colour (0xff15171a));
        g.fillRect (keys);
        for (int p = 127; p >= 0; --p)
        {
            if (isBlackKey (p)) continue;
            float top = yFor (p), bottom = top + keyH;
            if (p < 127 && isBlackKey (p + 1)) top -= keyH * 0.5f;
            if (p > 0 && isBlackKey (p - 1)) bottom += keyH * 0.5f;
            if (top > (float) keys.getBottom() || bottom < (float) keys.getY()) continue;
            auto r = juce::Rectangle<float> ((float) keys.getX(), std::round (top), (float) keys.getWidth() - 1.0f, std::round (bottom) - std::round (top) - 1.0f);
            g.setColour (p == keyboardNote ? palette::graph : juce::Colour (0xffe4e7eb));
            g.fillRect (r);
            // the line between this key and the one above, drawn explicitly so it never vanishes at any zoom
            g.setColour (juce::Colour (0xff8a9099));
            g.fillRect (keys.getX(), (int) std::round (top) - 1, keys.getWidth() - 1, 1);
            if (p % 12 == 0 && keyH >= 7.0f)
            {
                g.setColour (juce::Colour (0xff30343a));
                g.setFont (uiFont (juce::jmin (11.0f, keyH), true));
                g.drawText (juce::MidiMessage::getMidiNoteName (p, true, true, 4), r.reduced (3.0f, 0.0f).withTrimmedTop (r.getHeight() - keyH), juce::Justification::centredRight);
            }
        }
        for (int p = 127; p >= 0; --p)
        {
            if (! isBlackKey (p)) continue;
            const float y = yFor (p);
            if (y > (float) keys.getBottom() || y + keyH < (float) keys.getY()) continue;
            const auto r = juce::Rectangle<float> ((float) keys.getX(), y + 0.5f, (float) keys.getWidth() * 0.62f, keyH - 1.0f);
            g.setColour (p == keyboardNote ? palette::graph : juce::Colour (0xff1d2024));
            g.fillRoundedRectangle (r, 1.5f);
        }
        // the hovered key: highlight and its name (C4 = middle C)
        if (juce::isPositiveAndBelow (hoverPitch, 128))
        {
            const float y = yFor (hoverPitch);
            const auto row = juce::Rectangle<float> ((float) keys.getX(), y, (float) keys.getWidth() - 1.0f, keyH);
            g.setColour (juce::Colour (0x553c8cff));
            g.fillRect (isBlackKey (hoverPitch) ? row.withWidth ((float) keys.getWidth() * 0.62f) : row);
            const auto name = juce::MidiMessage::getMidiNoteName (hoverPitch, true, true, 4);
            g.setFont (uiFont (11.0f, true));
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name) + 10.0f;
            const auto badge = juce::Rectangle<float> ((float) keys.getRight() - w - 2.0f, y + keyH * 0.5f - 8.0f, w, 16.0f);
            g.setColour (palette::bg.withAlpha (0.92f));
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (palette::text);
            g.drawText (name, badge, juce::Justification::centred);
        }
    }

    // velocity lane
    g.setColour (palette::well);
    g.fillRect (vel);
    g.setColour (palette::outline);
    g.drawHorizontalLine (vel.getY(), (float) vel.getX(), (float) vel.getRight());
    g.setColour (palette::textDim);
    g.setFont (uiFont (10.0f));
    g.drawText ("VEL", juce::Rectangle<int> (0, vel.getY(), keysW, 16), juce::Justification::centred);
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (vel);
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const float x = xFor (absStart (notes[i]));
            if (x < (float) vel.getX() - 4.0f || x > (float) vel.getRight()) continue;
            const float h = (float) (vel.getHeight() - 6) * (float) notes[i].velocity / 127.0f;
            g.setColour (selected[i] ? juce::Colours::white : palette::graph.withAlpha (0.85f));
            g.fillRect (x, (float) vel.getBottom() - h, 3.0f, h);
            g.fillEllipse (x - 2.0f, (float) vel.getBottom() - h - 3.0f, 7.0f, 7.0f);
        }
    }

    // velocity value by the cursor while dragging
    if (velTip >= 0)
    {
        const juce::String t (velTip);
        g.setFont (uiFont (12.0f, true));
        auto r = juce::Rectangle<float> (velTipPos.x + 12.0f, velTipPos.y - 26.0f, 34.0f, 20.0f);
        r = r.constrainedWithin (getLocalBounds().toFloat().reduced (2.0f));
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (palette::accent);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (juce::Colours::white);
        g.drawText (t, r, juce::Justification::centred);
    }
}

void PianoRoll::resized()
{
    auto tb = getLocalBounds().removeFromTop (toolbarH).reduced (8, 5);
    drawButton.setBounds (tb.removeFromLeft (62)); tb.removeFromLeft (10);
    gridLabel.setBounds (tb.removeFromLeft (34));
    gridBox.setBounds (tb.removeFromLeft (110)); tb.removeFromLeft (10);
    lengthLabel.setBounds (tb.removeFromLeft (70));
    lengthBox.setBounds (tb.removeFromLeft (80)); tb.removeFromLeft (10);
    quantizeButton.setBounds (tb.removeFromLeft (80)); tb.removeFromLeft (6);
    humanizeButton.setBounds (tb.removeFromLeft (80)); tb.removeFromLeft (10);
    fitButton.setBounds (tb.removeFromRight (44)); tb.removeFromRight (4);
    followButton.setBounds (tb.removeFromRight (64)); tb.removeFromRight (4);
    zoomIn.setBounds (tb.removeFromRight (30)); tb.removeFromRight (2);
    zoomOut.setBounds (tb.removeFromRight (30)); tb.removeFromRight (10);
    info.setBounds (tb);
    const auto grid = gridArea();
    hbar.setBounds (keysW, getHeight() - sbW, getWidth() - keysW - sbW, sbW);
    vbar.setBounds (grid.getRight(), grid.getY(), sbW, grid.getHeight());
}

// ------------------------------------------------------------------ mouse

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    downPos = e.position;
    drag = Drag::none;
    changed = false;
    const bool snapOn = ! e.mods.isAltDown();
    flashTicks = 0;

    if (rulerArea().contains (e.getPosition()))   // set the play position
    {
        drag = Drag::scrub;
        host.engine.locateSeconds (snap (timeFor (e.position.x), snapOn));
        repaint();
        return;
    }
    if (keysArea().contains (e.getPosition()))
    {
        drag = Drag::keyboard;
        audition (pitchFor (e.position.y), 100);
        repaint();
        return;
    }
    if (velHandle().contains (e.getPosition()))   // drag the lane's top edge up or down to make it taller or shorter
    {
        drag = Drag::velSize;
        velDragStartH = velH();
        return;
    }
    if (velArea().contains (e.getPosition()))
    {
        // The bar under the cursor. Bars are 3 px wide with a dot on top; notes at the same time overlap, so ties go to a
        // selected note (dragging then changes the whole selection), then to the bar whose dot is nearest the cursor.
        int best = -1;
        float bestScore = 1.0e9f;
        const float volH = (float) (velArea().getHeight() - 6);
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const float dx = std::abs (xFor (absStart (notes[i])) + 1.5f - e.position.x);
            if (dx > 6.0f) continue;
            const float top = (float) velArea().getBottom() - volH * (float) notes[i].velocity / 127.0f;
            const float score = dx * 4.0f + std::abs (top - e.position.y) * 0.1f + (selected[i] ? 0.0f : 6.0f);
            if (score < bestScore) { bestScore = score; best = (int) i; }
        }
        if (best < 0) return;
        if (e.mods.isShiftDown() || e.mods.isCommandDown())   // shift-click bars to add them to / take them from the selection
        {
            selected[(size_t) best] = ! selected[(size_t) best];
            updateInfo();
            repaint();
            return;
        }
        anchor = best;
        if (! selected[(size_t) best]) { std::fill (selected.begin(), selected.end(), 0); selected[(size_t) best] = 1; }
        beginEdit ("Note velocity");
        origs.clear();
        for (auto& n : notes) origs.push_back ({ n.start, n.length, n.pitch, n.velocity });
        drag = Drag::velocity;
        mouseDrag (e);
        return;
    }
    if (! gridArea().contains (e.getPosition())) return;

    bool edge = false;
    const int hit = noteAt (e.position, edge);
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        if (hit >= 0 && ! selected[(size_t) hit]) { selected.assign (notes.size(), 0); selected[(size_t) hit] = 1; updateInfo(); repaint(); }
        const bool any = numSelected() > 0;
        m.addItem (4, "Cut  (Ctrl+X)", any);
        m.addItem (5, "Copy  (Ctrl+C)", any);
        m.addItem (6, "Paste here  (Ctrl+V pastes at the playhead)", ! noteClipboard.empty());
        m.addItem (7, "Duplicate  (Ctrl+D)", any);
        m.addItem (1, "Delete", any);
        m.addSeparator();
        m.addItem (2, "Quantize");
        m.addItem (3, "Select all");
        juce::Component::SafePointer<PianoRoll> safe (this);
        const double at = snap (timeFor (e.position.x), ! e.mods.isAltDown());
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, at] (int r) {
            if (safe == nullptr) return;
            if (r == 1) safe->deleteSelected();
            else if (r == 4) { safe->copySelected(); safe->deleteSelected(); }
            else if (r == 5) safe->copySelected();
            else if (r == 6) safe->pasteAt (at);
            else if (r == 7) safe->duplicateSelected();
            else if (r == 2) safe->quantize();
            else if (r == 3) { std::fill (safe->selected.begin(), safe->selected.end(), 1); safe->updateInfo(); safe->repaint(); }
        });
        return;
    }
    if (hit >= 0)
    {
        if (e.mods.isShiftDown() || e.mods.isCommandDown()) selected[(size_t) hit] = ! selected[(size_t) hit];
        else if (! selected[(size_t) hit]) { std::fill (selected.begin(), selected.end(), 0); selected[(size_t) hit] = 1; }
        anchor = hit;
        origs.clear();
        for (auto& n : notes) origs.push_back ({ n.start, n.length, n.pitch, n.velocity });
        drag = edge ? Drag::resize : Drag::move;
        beginEdit (edge ? "Note length" : "Move notes");
        if (! edge) audition (notes[(size_t) hit].pitch, notes[(size_t) hit].velocity);
        updateInfo();
        repaint();
        return;
    }
    if (drawButton.getToggleState())
    {
        // draw a note: click to add it, drag to set its length
        midiclip::Note n;
        const double t = snapDown (timeFor (e.position.x), snapOn);   // the cell that was clicked, not the nearest line
        n.start = t - clipStart() + offset();
        n.length = newNoteLength (t);
        n.pitch = pitchFor (e.position.y);
        n.velocity = 100;
        notes.push_back (n);
        selected.assign (notes.size(), 0);
        selected.back() = 1;
        anchor = (int) notes.size() - 1;
        origs.clear();
        for (auto& x : notes) origs.push_back ({ x.start, x.length, x.pitch, x.velocity });
        beginEdit ("Add note");
        commit();
        audition (n.pitch, n.velocity);
        drag = Drag::draw;
        return;
    }
    if (! e.mods.isShiftDown()) std::fill (selected.begin(), selected.end(), 0);
    drag = Drag::marquee;
    marquee = {};
    updateInfo();
    repaint();
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    const bool snapOn = ! e.mods.isAltDown();
    switch (drag)
    {
        case Drag::scrub:
            host.engine.locateSeconds (snap (timeFor (e.position.x), snapOn));
            repaint();
            break;
        case Drag::velSize:
            velHeight() = juce::jlimit (40, 600, velDragStartH - juce::roundToInt (e.position.y - downPos.y));
            resized();
            updateScrollBars();
            repaint();
            break;
        case Drag::keyboard:
        {
            const int p = pitchFor (e.position.y);
            if (p != keyboardNote) audition (p, 100);
            repaint();
            break;
        }
        case Drag::move:
        {
            if (e.getDistanceFromDragStart() < 3 || anchor < 0) return;
            const auto& a = origs[(size_t) anchor];
            const double aAbs = clipStart() + a.start - offset();
            const double dt = snap (aAbs + (e.position.x - downPos.x) / pps, snapOn) - aAbs;
            const int dp = pitchFor (e.position.y) - pitchFor (downPos.y);
            const int prevPitch = notes[(size_t) anchor].pitch;
            for (size_t i = 0; i < notes.size(); ++i)
                if (selected[i])
                {
                    notes[i].start = juce::jmax (offset() - clipStart(), origs[i].start + dt);
                    notes[i].pitch = juce::jlimit (0, 127, origs[i].pitch + dp);
                }
            if (notes[(size_t) anchor].pitch != prevPitch) audition (notes[(size_t) anchor].pitch, notes[(size_t) anchor].velocity);
            commit();
            break;
        }
        case Drag::resize:
        case Drag::draw:
        {
            if (anchor < 0) return;
            // a plain click on a note's end must not change it: snapping the end to the grid would otherwise stretch or
            // shrink the note up to the next grid line the moment the mouse moves a pixel
            if (drag == Drag::resize && e.getDistanceFromDragStart() < 4) return;
            const auto& a = origs[(size_t) anchor];
            const double aAbs = clipStart() + a.start - offset();
            const double minLen = juce::jmax (0.01, gridSec() > 0.0 ? gridSec() * 0.5 : 0.02);
            double newEnd = snap (aAbs + a.length + (e.position.x - downPos.x) / pps, snapOn);
            if (drag == Drag::draw) newEnd = juce::jmax (aAbs + newNoteLength (aAbs), snap (timeFor (e.position.x), snapOn));
            const double dl = (newEnd - aAbs) - a.length;
            for (size_t i = 0; i < notes.size(); ++i)
                if (selected[i]) notes[i].length = juce::jmax (minLen, origs[i].length + dl);
            commit();
            break;
        }
        case Drag::velocity:
        {
            const auto v = velArea();
            const int target = juce::jlimit (1, 127, juce::roundToInt (127.0f * ((float) v.getBottom() - e.position.y) / (float) (v.getHeight() - 6)));
            const int delta = target - origs[(size_t) anchor].velocity;
            for (size_t i = 0; i < notes.size(); ++i)
                if (selected[i]) notes[i].velocity = juce::jlimit (1, 127, origs[i].velocity + delta);
            velTip = notes[(size_t) anchor].velocity;
            velTipPos = e.position;
            commit();
            break;
        }
        case Drag::marquee:
        {
            marquee = juce::Rectangle<float> (downPos, e.position);
            for (size_t i = 0; i < notes.size(); ++i)
                if (noteRect (notes[i]).intersects (marquee)) selected[i] = 1;
                else if (! e.mods.isShiftDown()) selected[i] = 0;
            updateInfo();
            repaint();
            break;
        }
        case Drag::none: break;
    }
}

void PianoRoll::mouseUp (const juce::MouseEvent&)
{
    stopAudition();
    marquee = {};
    velTip = -1;
    drag = Drag::none;
    repaint();
}

void PianoRoll::mouseExit (const juce::MouseEvent&)
{
    if (hoverPitch >= 0) { hoverPitch = -1; repaint (keysArea()); }
}

void PianoRoll::mouseMove (const juce::MouseEvent& e)
{
    {
        const bool overRows = keysArea().contains (e.getPosition()) || gridArea().contains (e.getPosition());
        const int p = overRows ? juce::jlimit (0, 127, pitchFor (e.position.y)) : -1;
        if (p != hoverPitch) { hoverPitch = p; repaint (keysArea()); }
    }
    bool edge = false;
    const int hit = gridArea().contains (e.getPosition()) ? noteAt (e.position, edge) : -1;
    if (rulerArea().contains (e.getPosition())) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    if (velHandle().contains (e.getPosition())) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); return; }
    if (velArea().contains (e.getPosition())) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); return; }
    setMouseCursor (hit >= 0 ? (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor)
                             : drawButton.getToggleState() && gridArea().contains (e.getPosition()) ? juce::MouseCursor::CrosshairCursor
                                                                                                   : juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! gridArea().contains (e.getPosition()) || drawButton.getToggleState()) return;
    bool edge = false;
    const int hit = noteAt (e.position, edge, false);   // only a note really under the cursor is deleted
    if (hit >= 0)
    {
        notes.erase (notes.begin() + hit);
        selected.assign (notes.size(), 0);
        commit ("Delete note");
        return;
    }
    midiclip::Note n;
    n.start = snapDown (timeFor (e.position.x), ! e.mods.isAltDown()) - clipStart() + offset();
    n.length = newNoteLength (absStart (n));
    n.pitch = pitchFor (e.position.y);
    n.velocity = 100;
    notes.push_back (n);
    selected.assign (notes.size(), 0);
    selected.back() = 1;
    commit ("Add note");
    audition (n.pitch, n.velocity);
    juce::Timer::callAfterDelay (250, [safe = juce::Component::SafePointer<PianoRoll> (this)] { if (safe != nullptr) safe->stopAudition(); });
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown())   // zoom around the mouse
    {
        const double t = timeFor (e.position.x);
        pps = juce::jlimit (10.0, 4000.0, pps * (w.deltaY > 0 ? 1.2 : 1.0 / 1.2));
        scrollT = t - (e.position.x - (float) gridArea().getX()) / pps;
    }
    else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
        scrollT -= (std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY) * 300.0 / pps;
    else
        topPitch = juce::jlimit ((float) (gridArea().getHeight() / keyH) - 1.0f, 127.0f, topPitch + w.deltaY * 12.0f);
    repaint();
}

bool PianoRoll::keyPressed (const juce::KeyPress& k)
{
    const auto c = k.getKeyCode();
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    if (k.getModifiers().isCommandDown() && (c == 'A' || c == 'a')) { std::fill (selected.begin(), selected.end(), 1); updateInfo(); repaint(); return true; }
    if (k.getModifiers().isCommandDown() && (c == 'Z' || c == 'z'))
    {
        if (k.getModifiers().isShiftDown()) doc.undo().redo(); else doc.undo().undo();
        return true;
    }
    if (k.getModifiers().isCommandDown() && (c == 'Y' || c == 'y')) { doc.undo().redo(); return true; }
    if (k.getModifiers().isCommandDown() && (c == 'C' || c == 'c')) { copySelected(); return true; }
    if (k.getModifiers().isCommandDown() && (c == 'X' || c == 'x')) { if (numSelected() > 0) { copySelected(); deleteSelected(); } return true; }
    if (k.getModifiers().isCommandDown() && (c == 'V' || c == 'v')) { pasteAt (snap (host.engine.getPositionSeconds(), true)); return true; }
    if (k.getModifiers().isCommandDown() && (c == 'D' || c == 'd')) { duplicateSelected(); return true; }
    if (c == juce::KeyPress::upKey) { transpose (k.getModifiers().isShiftDown() ? 12 : 1); return true; }
    if (c == juce::KeyPress::downKey) { transpose (k.getModifiers().isShiftDown() ? -12 : -1); return true; }
    if (c == juce::KeyPress::leftKey) { nudge (-1); return true; }
    if (c == juce::KeyPress::rightKey) { nudge (1); return true; }
    if (c == 'Q' || c == 'q') { quantize(); return true; }
    if (c == 'D' || c == 'd') { drawButton.setToggleState (! drawButton.getToggleState(), juce::dontSendNotification); return true; }
    if (k == juce::KeyPress::spaceKey)
    {
        host.engine.togglePlay();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ updates

void PianoRoll::timerCallback()
{
    if (! fitted && isShowing() && gridArea().getWidth() > 200 && gridArea().getHeight() > 100) { fitted = true; fit(); }
    // gone when the clip is deleted, or its whole track is (the clip then still hangs off the removed track)
    if (! clip.getParent().isValid() || clip.getParent().getParent() != doc.tracks()) { stopTimer(); if (onClipGone) onClipGone(); return; }
    if (flashTicks > 0 && --flashTicks == 0) updateInfo();
    const double ph = host.engine.getPositionSeconds();
    if (ph != lastPlayhead)
    {
        lastPlayhead = ph;
        // Follow: bring the play position into view, however it moved (playing, or a click in the arranger)
        const double view = gridArea().getWidth() / pps;
        if (followOn() && drag == Drag::none && (ph > scrollT + view * 0.95 || ph < scrollT))
        {
            scrollT = juce::jmax (0.0, ph - view * 0.08);
            updateScrollBars();
        }
        repaint();
    }
}

void PianoRoll::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier&)
{
    if (node.hasType (project::ids::TRANSPORT) || node.hasType (project::ids::TEMPO)) refreshMap();
    if (writing) return;
    if (node == clip || node.hasType (midiclip::MIDI)) load();   // undo / redo, or edited elsewhere
}

void PianoRoll::valueTreeParentChanged (juce::ValueTree&)
{
    if (! clip.getParent().isValid() && onClipGone)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PianoRoll> (this)] { if (safe != nullptr && safe->onClipGone) safe->onClipGone(); });
}
} // namespace ff
