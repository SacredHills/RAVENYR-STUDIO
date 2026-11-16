#include "ArrangeView.h"
#include "core/MidiClip.h"

namespace ff
{
using namespace timeline;
namespace pids = project::ids;

static double prop (const juce::ValueTree& v, const juce::Identifier& id) { return (double) v[id]; }

// =====================================================================================
// TrackHeader
// =====================================================================================

static void styleToggle (juce::TextButton& b, juce::Colour on, const juce::String& tip)
{
    b.setColour (juce::TextButton::buttonOnColourId, on);
    b.setColour (juce::TextButton::textColourOnId, palette::onAccent);
    b.setTooltip (tip);
    b.setWantsKeyboardFocus (false);
}

InsertStrip::InsertStrip (ArrangeView& v, juce::ValueTree t) : owner (v), track (t)
{
    track.addListener (this);
    setTooltip ("Plugins on this track: click to see them all (click one to open it, right-click to bypass, replace or remove it)");
}

InsertStrip::~InsertStrip() { track.removeListener (this); }

namespace
{
/** The dropdown of a track's plugins: every plugin in order, click to open, right-click for bypass / replace / remove,
    the dot switches bypass; "+ Add plugin" at the bottom. */
class PluginListPanel final : public juce::Component
{
public:
    PluginListPanel (ArrangeView& o, juce::ValueTree t, juce::Component& a) : owner (o), track (t), anchor (&a)
    {
        setSize (240, rows() * rowH + 8);
    }
    void paint (juce::Graphics& g) override
    {
        const int n = rig::numInserts (track), amp = ampRows();
        g.setFont (uiFont (12.5f, true));
        for (int row = 0; row < rows(); ++row)
        {
            auto r = rowRect (row);
            if (row == hover) { g.setColour (palette::panelHi.brighter (0.1f)); g.fillRoundedRectangle (r.toFloat(), 4.0f); }
            if (row == rows() - 1)
            {
                g.setColour (palette::textDim);
                g.drawText ("+ Add plugin", r.reduced (10, 0), juce::Justification::centredLeft);
                continue;
            }
            const int i = row - amp;
            const bool bypassed = i < 0 ? owner.session.ampBypassed (track) : (bool) rig::insertNode (track, i)[rig::ids::bypass];
            const auto name = i < 0 ? juce::String ("NAM Amp") : rig::insertNode (track, i)[rig::ids::name].toString();
            auto dot = r.removeFromLeft (26).toFloat().withSizeKeepingCentre (10.0f, 10.0f);
            g.setColour (bypassed ? palette::outline : palette::accent);
            g.fillEllipse (dot);
            g.setColour (bypassed ? palette::textDim.withAlpha (0.6f) : palette::text);
            g.drawText (juce::String (row + 1) + "  " + name + (bypassed ? "  (bypassed)" : ""), r.reduced (2, 0),
                        juce::Justification::centredLeft, true);
        }
        juce::ignoreUnused (n);
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = rowAt (e.y); if (h != hover) { hover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.y);
        if (row < 0) return;
        auto& session = owner.session;
        auto t = track;
        juce::Component::SafePointer<juce::Component> a (anchor);
        if (row == rows() - 1)
        {
            dismiss();
            if (a != nullptr) session.showAddInsertMenu (t, a.getComponent());
            return;
        }
        const int i = row - ampRows();
        if (! e.mods.isPopupMenu() && e.x < 28)   // the dot: bypass on / off, the list stays open
        {
            if (i < 0) session.toggleAmpBypass (t); else session.toggleInsertBypass (t, i);
            repaint();
            return;
        }
        dismiss();
        if (i < 0)
        {
            if (e.mods.isPopupMenu()) { if (a != nullptr) session.showAmpMenu (t, a.getComponent()); }
            else if (session.onOpenRig) session.onOpenRig (t);
            return;
        }
        if (e.mods.isPopupMenu()) { if (a != nullptr) session.showInsertMenu (t, i, a.getComponent()); }
        else session.openInsert (t, i);
    }
private:
    static constexpr int rowH = 26;
    ArrangeView& owner;
    juce::ValueTree track;
    juce::Component::SafePointer<juce::Component> anchor;
    int hover = -1;
    int ampRows() const { return owner.session.trackHasRig (track) ? 1 : 0; }
    int rows() const { return ampRows() + rig::numInserts (track) + 1; }
    juce::Rectangle<int> rowRect (int i) const { return { 4, 4 + i * rowH, getWidth() - 8, rowH }; }
    int rowAt (int y) const
    {
        const int i = (y - 4) / rowH;
        return y >= 4 && i < rows() ? i : -1;
    }
    void dismiss()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
    }
};
} // namespace

void InsertStrip::paint (juce::Graphics& g)
{
    const bool amp = owner.session.trackHasRig (track);
    const int n = rig::numInserts (track) + (amp ? 1 : 0);
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (hover != -2 ? palette::panelHi.brighter (0.12f) : palette::panelHi);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (palette::outline.brighter (0.2f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    // "FX 3" badge, then the plugin names, then the arrow
    auto area = getLocalBounds().reduced (6, 0);
    g.setFont (uiFont (10.5f, true));
    auto badge = area.removeFromLeft (n > 0 ? 34 : 0).toFloat().reduced (0.0f, 4.0f);
    if (n > 0)
    {
        g.setColour (palette::accent.withAlpha (0.85f));
        g.fillRoundedRectangle (badge, 3.0f);
        g.setColour (juce::Colours::black);
        g.drawText ("FX " + juce::String (n), badge, juce::Justification::centred);
        area.removeFromLeft (6);
    }
    auto arrow = area.removeFromRight (12).toFloat();
    juce::Path p;
    p.addTriangle (arrow.getCentreX() - 4.0f, arrow.getCentreY() - 2.0f, arrow.getCentreX() + 4.0f, arrow.getCentreY() - 2.0f,
                   arrow.getCentreX(), arrow.getCentreY() + 3.0f);
    g.setColour (palette::textDim);
    g.fillPath (p);
    juce::StringArray names;
    if (amp) names.add (juce::String ("NAM Amp") + (owner.session.ampBypassed (track) ? " (off)" : ""));
    for (int i = 0; i < rig::numInserts (track); ++i)
    {
        const auto node = rig::insertNode (track, i);
        names.add (node[rig::ids::name].toString() + ((bool) node[rig::ids::bypass] ? " (off)" : ""));
    }
    g.setFont (uiFont (11.5f, n > 0));
    g.setColour (n > 0 ? palette::text : palette::textDim);
    g.drawText (n > 0 ? names.joinIntoString (", ") : juce::String ("+ Plugin"), area, juce::Justification::centredLeft, true);
}

void InsertStrip::mouseMove (const juce::MouseEvent&)
{
    if (hover != 0) { hover = 0; repaint(); }
}

void InsertStrip::mouseDown (const juce::MouseEvent& e)
{
    if (rig::numInserts (track) == 0 && ! owner.session.trackHasRig (track) && ! e.mods.isPopupMenu())
    {
        owner.session.showAddInsertMenu (track, this);
        return;
    }
    auto panel = std::make_unique<PluginListPanel> (owner, track, *this);
    juce::CallOutBox::launchAsynchronously (std::move (panel), getScreenBounds(), nullptr);
}

TrackHeader::TrackHeader (ArrangeView& v, juce::ValueTree t, int i) : track (t), index (i), owner (v)
{
    inserts = std::make_unique<InsertStrip> (v, t);   // (not shown: plugins live in the mixer)
    name.setFont (uiFont (14.0f, true));
    name.setColour (juce::Label::textColourId, palette::text);
    name.setEditable (false, true, false);
    name.onTextChange = [this] {
        const auto n = name.getText().trim();
        if (n.isNotEmpty() && n != track[ids::name].toString())
        {
            owner.doc.undo().beginNewTransaction ("Rename track");
            owner.doc.set (track, ids::name, n);
        }
    };
    name.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (name);

    styleToggle (arm, palette::bad, "Arm for recording");
    styleToggle (mute, palette::accent, "Mute");
    styleToggle (solo, palette::warn, "Solo");
    // With several tracks selected, these buttons switch the whole selection.
    arm.onClick = [this] { owner.session.setOnTracks (track, ids::armed, ! (bool) track[ids::armed], false); };
    mon.onClick = [this] { owner.session.setOnTracks (track, ids::monitor, ! (bool) track[ids::monitor], false); };
    mute.onClick = [this] { owner.session.setOnTracks (track, ids::mute, ! (bool) track[ids::mute], false); };
    solo.onClick = [this] { owner.session.setOnTracks (track, ids::solo, ! (bool) track[ids::solo], false); };
    for (juce::Button* b : { (juce::Button*) &arm, (juce::Button*) &mon, (juce::Button*) &mute, (juce::Button*) &solo })
        addAndMakeVisible (b);

    input.setTooltip ("Recording / monitoring input");
    input.onChange = [this] {
        const int i2 = input.getSelectedItemIndex();
        if (updating || ! juce::isPositiveAndBelow (i2, inputValues.size())) return;
        owner.doc.undo().beginNewTransaction ("Track input");
        owner.doc.set (track, ids::inputChannel, inputValues[i2].first);
        owner.doc.set (track, ids::inputStereo, inputValues[i2].second);
    };
    addAndMakeVisible (input);
    route.setTooltip ("Where this bus / FX track plays into, and its sends");
    laneButton.onClick = [this] { owner.showLaneChooser (track, &laneButton); };
    addChildComponent (laneButton);
    route.onClick = [this] {
        if (! isInstrumentTrack (track)) { owner.session.showRoutingMenu (track, &route); return; }
        if (track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::desc].toString().isEmpty())
            owner.session.showInstrumentContextMenu (track, &route);
        else owner.session.openInstrument (track);
    };
    route.onRightClick = [this] {
        if (isInstrumentTrack (track)) owner.session.showInstrumentContextMenu (track, &route);   // open / change / remove
        else owner.session.showRoutingMenu (track, &route);
    };
    channels.onClick = [this] { owner.session.setTrackStereo (track, ! (bool) track[ids::inputStereo]); };
    addChildComponent (channels);
    addChildComponent (route);

    gain.setScrollWheelEnabled (false);   // the wheel scrolls the tracks over the header
    pan.setScrollWheelEnabled (false);
    gain.setSliderStyle (juce::Slider::LinearHorizontal);
    gain.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    gain.setRange (-60.0, 6.0, 0.1);
    gain.setSkewFactorFromMidPoint (-12.0);
    gain.setDoubleClickReturnValue (true, 0.0);
    gain.setPopupDisplayEnabled (true, true, nullptr);
    gain.setTextValueSuffix (" dB");
    gain.setTooltip ("Volume (double-click: 0 dB). With volume automation the fader follows it, and moving the fader "
                     "raises or lowers the whole automation.");
    gain.onDragStart = [this] { owner.doc.undo().beginNewTransaction ("Track volume"); gainPrev = gain.getValue(); };
    gain.onValueChange = [this] {
        if (updating) return;
        const double now = gain.getValue(), delta = now - gainPrev;
        gainPrev = now;
        owner.session.moveTrackVolume (track, now, delta);
    };
    addAndMakeVisible (gain);

    pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pan.setRange (-1.0, 1.0, 0.01);
    pan.getProperties().set ("pan", true);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setPopupDisplayEnabled (true, true, nullptr);
    pan.setTooltip ("Pan (double-click: centre)");
    pan.onDragStart = [this] { owner.doc.undo().beginNewTransaction ("Track pan"); beginGroupDrag (ids::pan); };
    pan.onValueChange = [this] {
        if (updating) return;
        const double before = (double) track[ids::pan];
        owner.doc.set (track, ids::pan, pan.getValue());
        applyGroupDelta (ids::pan, pan.getValue() - before, -1.0, 1.0);
    };
    addAndMakeVisible (pan);

    track.addListener (this);
    refreshInputs();
    refresh();
}

TrackHeader::~TrackHeader() { track.removeListener (this); }

void TrackHeader::refreshInputs()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    channels.setStereo ((bool) track[ids::inputStereo]);
    const auto names = owner.session.inputChoiceNames (inputValues, (bool) track[ids::inputStereo]);
    input.clear (juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i)
        input.addItem (names[i], i + 1);
    refresh();
}

void TrackHeader::refreshAutomation()
{
    const bool a = owner.automationMode();
    laneButton.setVisible (a);
    input.setVisible (! a && isAudioTrack (track));
    channels.setVisible (! a && isAudioTrack (track));   // the lane button takes their place
    route.setVisible (! a && ! isAudioTrack (track));
    if (a)
    {
        const auto prm = owner.laneParamFor (track);
        laneButton.setButtonText ("A: " + owner.laneLabel (track, prm));
        laneButton.setTooltip ("Automation lane shown for this track (click to pick volume, pan or a plugin parameter)");
        input.setVisible (false);
        route.setVisible (false);
    }
}

void TrackHeader::refresh()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    name.setText (track[ids::name].toString(), juce::dontSendNotification);
    arm.setToggleState ((bool) track[ids::armed], juce::dontSendNotification);
    const bool folder = isFolder (track);
    const bool audio = isAudioTrack (track), inst = isInstrumentTrack (track);
    if (folder)
    {
        arm.setVisible (false); mon.setVisible (false); input.setVisible (false); channels.setVisible (false);
        route.setVisible (false); laneButton.setVisible (false); gain.setVisible (false); pan.setVisible (false);
        mute.setVisible (false); solo.setVisible (false); inserts->setVisible (false);
        resized();
        repaint();
        return;
    }
    arm.setVisible (audio || inst);
    arm.setTooltip (inst ? "Play this instrument from your MIDI keyboard" : "Arm for recording");
    mon.setVisible (audio || inst);
    mon.setTooltip (inst ? "Monitor: play this instrument from your MIDI keyboard without arming it"
                         : "Monitor: hear what comes into this track's input (your guitar / mic) through the track and its plugins");
    input.setVisible (audio);
    channels.setVisible (audio);
    if (audio && channels.isStereo() != (bool) track[ids::inputStereo])
    {
        channels.setStereo ((bool) track[ids::inputStereo]);
        refreshInputs();   // the list shows single inputs for a mono track, pairs for a stereo one (calls back into refresh)
        return;
    }
    route.setVisible (! audio);
    if (inst) route.setButtonText ("SYNTH  " + owner.session.instrumentName (track));
    else route.setButtonText ((kindOf (track) == TrackKind::fx ? "FX  > " : "BUS  > ") + owner.session.outputName (track));
    route.setTooltip (inst ? "Click to open the instrument; right-click to change or remove it" : "Where this bus / FX track plays into, and its sends");
    refreshAutomation();
    mon.setToggleState ((bool) track[ids::monitor], juce::dontSendNotification);
    mute.setToggleState ((bool) track[ids::mute], juce::dontSendNotification);
    solo.setToggleState ((bool) track[ids::solo], juce::dontSendNotification);
    gain.setValue (automation::volumeAt (track, owner.playhead()), juce::dontSendNotification);
    gainPrev = gain.getValue();
    pan.setValue (prop (track, ids::pan), juce::dontSendNotification);
    const std::pair<int, bool> cur { (int) track[ids::inputChannel], (bool) track[ids::inputStereo] };
    int sel = 0;
    for (int i = 0; i < inputValues.size(); ++i)
        if (inputValues[i] == cur) sel = i;
    input.setSelectedItemIndex (sel, juce::dontSendNotification);
    repaint();
}

void TrackHeader::beginGroupDrag (const juce::Identifier&)
{
    groupStart.clear();
}

void TrackHeader::applyGroupDelta (const juce::Identifier& id, double delta, double lo, double hi)
{
    // Relative moves keep the balance between selected tracks (like dragging a grouped fader).
    if (std::abs (delta) < 1.0e-9) return;
    for (auto t : owner.session.linkedTracks (track))
        if (t != track)
            owner.doc.set (t, id, juce::jlimit (lo, hi, (double) t[id] + delta));
}

void TrackHeader::tickMeter()
{
    if (automation::hasVolumeAutomation (track) && ! gain.isMouseButtonDown())   // the fader shows the automated volume
    {
        const double v = automation::volumeAt (track, owner.playhead());
        if (std::abs (v - gain.getValue()) > 0.01) { gain.setValue (v, juce::dontSendNotification); gainPrev = gain.getValue(); }
    }
    auto& m = owner.host.engine.trackMeters[(size_t) juce::jlimit (0, maxTracks - 1, index)];
    m.uiTake();
    const float db = gainToDb (m.uiPeak);
    meterLevel = db > meterLevel ? db : juce::jmax (db, meterLevel - 1.6f);
    repaint (getLocalBounds().removeFromRight (8));
}

void TrackHeader::paint (juce::Graphics& g)
{
    const bool selected = owner.session.isTrackSelected (track);
    auto r = getLocalBounds();
    g.setColour (selected ? palette::panelHi : palette::panel);
    g.fillRect (r);
    g.setColour (palette::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.setColour (owner.trackColour (track));
    g.fillRect (r.removeFromLeft (5));
    if (const auto grp = track[ids::group].toString(); grp.isNotEmpty())   // track group stripe
    {
        g.setColour (SessionController::groupColour (grp));
        g.fillRect (r.removeFromLeft (3));
    }
    if (selected)
    {
        g.setColour (palette::accent.withAlpha (0.8f));
        g.drawRect (getLocalBounds().withTrimmedLeft (5), 1);
    }
    if ((bool) track[ids::armed])
    {
        g.setColour (palette::bad.withAlpha (0.12f));
        g.fillRect (r);
    }

    if (isFolder (track))   // a chevron: open / closed, and how many tracks are inside
    {
        const bool closed = (bool) track[ids::collapsed];
        juce::Path tri;
        const float cx = 17.0f, cy = getHeight() * 0.5f;
        if (closed) tri.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 4.0f, cy);
        else tri.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
        g.setColour (palette::text);
        g.fillPath (tri);
        int count = 0;
        for (auto t : owner.doc.tracks()) count += folderIdOf (t) == track[ids::id].toString() ? 1 : 0;
        g.setColour (palette::textDim);
        g.setFont (uiFont (11.0f, true));
        g.drawText (juce::String (count) + (count == 1 ? " track" : " tracks"), getLocalBounds().withTrimmedRight (14).reduced (6, 0), juce::Justification::centredRight);
        return;
    }
    if (const auto fid = folderIdOf (track); fid.isNotEmpty())   // inside a folder: a stripe in the folder's colour
    {
        g.setColour (owner.trackColour (owner.session.findTrack (fid)).withAlpha (0.8f));
        g.fillRect (getLocalBounds().removeFromLeft (5).translated (5, 0));
    }
    auto meter = getLocalBounds().removeFromRight (8).reduced (2, 6);
    g.setColour (palette::well);
    g.fillRect (meter);
    const float n = juce::jlimit (0.0f, 1.0f, (meterLevel + 60.0f) / 60.0f);
    g.setColour (meterLevel > -0.5f ? palette::bad : (meterLevel > -9.0f ? palette::warn : palette::good));
    g.fillRect (meter.withTop (meter.getBottom() - juce::roundToInt (n * meter.getHeight())));
}

void TrackHeader::resized()
{
    auto r = getLocalBounds().reduced (10, 6).withTrimmedRight (6);
    if (isFolder (track))
    {
        r = getLocalBounds().reduced (6, 3).withTrimmedRight (12);
        name.setBounds (r.withTrimmedLeft (22));
        return;
    }
    auto top = r.removeFromTop (24);
    const int bw = 24;
    solo.setBounds (top.removeFromRight (bw).reduced (1));
    mute.setBounds (top.removeFromRight (bw).reduced (1));
    mon.setBounds (top.removeFromRight (bw).reduced (1));
    arm.setBounds (top.removeFromRight (bw).reduced (1));
    name.setBounds (top.withTrimmedRight (4));
    r.removeFromTop (6);
    auto row = r.removeFromTop (24);
    pan.setBounds (row.removeFromRight (26));
    row.removeFromRight (4);
    const auto inputArea = row.removeFromLeft (row.getWidth() / 2 + 10);
    route.setBounds (inputArea);
    laneButton.setBounds (inputArea);
    auto ia = inputArea;
    channels.setBounds (ia.removeFromLeft (26).reduced (0, 1));
    ia.removeFromLeft (3);
    input.setBounds (isAudioTrack (track) ? ia : inputArea);
    row.removeFromLeft (4);
    gain.setBounds (row);
}

void TrackHeader::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (onBottomEdge (e.y) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    resizingTrack = ! e.mods.isPopupMenu() && onBottomEdge (e.y);
    if (resizingTrack)   // drag the bottom edge: this track gets taller / shorter (double-click: back to normal)
    {
        resizeStartH = getHeight();
        if (e.getNumberOfClicks() == 2) track.setProperty (ArrangeView::laneHeightId, 0, nullptr);
        return;
    }
    if (isFolder (track) && ! e.mods.isPopupMenu() && e.x < 30)
    {
        owner.session.toggleFolder (track);
        return;
    }
    if (e.mods.isPopupMenu())
    {
        if (! owner.session.isTrackSelected (track))
            owner.session.clickTrack (track, {});
        showMenu();
        return;
    }
    // a plain click on one of several selected tracks keeps them all, so they can be dragged as a block (a click without a drag narrows it to this one on release)
    pendingCollapse = ! e.mods.isShiftDown() && ! e.mods.isCommandDown() && ! e.mods.isCtrlDown()
                      && owner.session.selectedTracks.size() > 1 && owner.session.isTrackSelected (track);
    if (! pendingCollapse) owner.session.clickTrack (track, e.mods);
    owner.clearSelection();   // picking tracks leaves no clip selected (Delete would remove the clips instead of the tracks)
    owner.repaintAll();
    if (e.getNumberOfClicks() == 2)
        name.showEditor();
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (resizingTrack)
    {
        const int h = juce::jlimit (ArrangeGeometry::minTrackHeight, ArrangeGeometry::maxTrackHeight, resizeStartH + e.getDistanceFromDragStartY());
        // with several tracks selected (Studio One style) they all take the new height
        auto targets = owner.session.isTrackSelected (track) ? owner.session.selectedTracks : juce::Array<juce::ValueTree> { track };
        for (auto t : targets) t.setProperty (ArrangeView::laneHeightId, h, nullptr);
        return;
    }
    // Drag a header up/down to move the track (and any other selected tracks) to a new position.
    if (e.mods.isPopupMenu() || std::abs (e.getDistanceFromDragStartY()) < 5) return;
    draggingTrack = true;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    const int y = e.getEventRelativeTo (getParentComponent()).y;
    // drop before the track under the mouse, or after it when in its lower half
    const int cy = y + owner.geo.scrollY, hit = owner.geo.indexAt (cy);
    const int idx = juce::jlimit (0, owner.numTracks(), cy > owner.geo.topOf (hit) + owner.geo.heightOf (hit) / 2 ? hit + 1 : hit);
    owner.setTrackDropIndex (idx);
}

void TrackHeader::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    owner.geo.scrollY = juce::jmax (0, owner.geo.scrollY - juce::roundToInt (w.deltaY * 200.0f));
    owner.updateScrollBars();
    owner.repaintAll();
}

void TrackHeader::mouseUp (const juce::MouseEvent&)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    if (resizingTrack) { resizingTrack = false; return; }
    if (! draggingTrack)
    {
        if (pendingCollapse) { pendingCollapse = false; owner.session.clickTrack (track, {}); owner.repaintAll(); }
        return;
    }
    pendingCollapse = false;
    draggingTrack = false;
    const int idx = owner.trackDropIndex();
    owner.setTrackDropIndex (-1);
    if (idx >= 0)
        owner.moveTracks (owner.session.linkedTracks (track), idx);   // a group moves as one block
}

void TrackHeader::showMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Rename...");
    juce::PopupMenu colours;
    for (int i = 0; i < trackPalette().size(); ++i)
        colours.addColouredItem (100 + i, "Colour " + juce::String (i + 1), juce::Colour::fromString (trackPalette()[i]));
    m.addSubMenu ("Colour", colours);
    owner.session.addGroupMenu (m, track, 700);
    if (! isFolder (track)) owner.session.addFolderMenu (m, track, 900);
    else m.addItem (30, (bool) track[ids::collapsed] ? "Open folder" : "Collapse folder");
    m.addSeparator();
    if (! isAudioTrack (track)) {}
    else if (owner.session.trackHasRig (track))
    {
        m.addItem (10, "Open NAM amp...");
        m.addItem (11, "Remove NAM amp");
    }
    else m.addItem (10, "Add NAM amp...");
    m.addItem (12, "Add plugin...", owner.session.plugins != nullptr && rig::numInserts (track) < rig::maxInserts);
    m.addItem (13, "Output and sends  (now: " + owner.session.outputName (track) + ")...");
    if (isAudioTrack (track))
        m.addItem (18, "Stereo track (records an input pair)", true, (bool) track[ids::inputStereo]);
    if (isInstrumentTrack (track))
    {
        const bool hasInstrument = track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::desc].toString().isNotEmpty();
        m.addItem (14, hasInstrument ? "Change instrument  (now: " + owner.session.instrumentName (track) + ")..." : juce::String ("Choose instrument..."));
        m.addItem (15, "Open instrument", hasInstrument);
        m.addItem (19, "Remove instrument", hasInstrument);
        m.addItem (16, "New MIDI clip at playhead");
        m.addItem (17, "Export MIDI file...");
    }
    for (int i = 0; i < rig::numInserts (track); ++i)
        m.addItem (200 + i, "Plugin: " + rig::insertNode (track, i)[rig::ids::name].toString()
                                + ((bool) rig::insertNode (track, i)[rig::ids::bypass] ? "  (bypassed)" : "") + "...");
    m.addSeparator();
    m.addItem (2, "Move up", index > 0);
    m.addItem (3, "Move down", index < owner.numTracks() - 1);
    const int n = owner.session.tracksForEdit (track).size();
    m.addItem (4, n > 1 ? "Duplicate " + juce::String (n) + " tracks" : juce::String ("Duplicate track"));
    m.addSeparator();
    m.addItem (6, "Select all tracks");
    m.addItem (5, n > 1 ? "Delete " + juce::String (n) + " tracks" : juce::String ("Delete track"));
    juce::Component::SafePointer<TrackHeader> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        auto& o = safe->owner;
        auto t = safe->track;
        auto tracks = o.doc.tracks();
        const int idx = tracks.indexOf (t);
        if (o.session.handleGroupMenu (r, t, 700) || o.session.handleFolderMenu (r, t, 900)) return;
        if (r == 30) { o.session.toggleFolder (t); return; }
        if (r == 1) safe->name.showEditor();
        else if (r == 10) o.session.addTrackRig (t);
        else if (r == 11) o.session.removeTrackRig (t);
        else if (r == 12) o.session.showAddInsertMenu (t, safe.getComponent());
        else if (r == 13) o.session.showRoutingMenu (t, safe.getComponent());
        else if (r == 14) o.session.showInstrumentMenu (t, safe.getComponent());
        else if (r == 15) o.session.openInstrument (t);
        else if (r == 18) o.session.setTrackStereo (t, ! (bool) t[ids::inputStereo]);
        else if (r == 19) o.session.removeInstrument (t);
        else if (r == 16) o.newMidiClip (t, o.playhead());
        else if (r == 17) o.exportMidi (t);
        else if (r >= 200 && r < 200 + rig::maxInserts) o.session.showInsertMenu (t, r - 200, safe.getComponent());
        else if (r >= 100) { o.doc.undo().beginNewTransaction ("Track colour"); o.doc.set (t, ids::colour, trackPalette()[r - 100]); }
        else if (r == 2) { o.doc.undo().beginNewTransaction ("Move track"); tracks.moveChild (idx, idx - 1, &o.doc.undo()); }
        else if (r == 3) { o.doc.undo().beginNewTransaction ("Move track"); tracks.moveChild (idx, idx + 1, &o.doc.undo()); }
        else if (r == 4) { for (auto x : o.session.tracksForEdit (t)) o.session.duplicateTrack (x); }
        else if (r == 5) o.session.deleteTracks (o.session.tracksForEdit (t));
        else if (r == 6)
        {
            juce::Array<juce::ValueTree> all;
            for (auto x : o.doc.tracks()) all.add (x);
            o.session.setSelectedTracks (all);
        }
    });
}

// =====================================================================================
// Ruler
// =====================================================================================

juce::ValueTree Ruler::markerAt (float x) const
{
    for (auto m : owner.doc.markers())
        if (std::abs (owner.geo.xFor (prop (m, ids::time)) - x) < 6.0f)
            return m;
    return {};
}

void Ruler::paint (juce::Graphics& g)
{
    const auto& geo = owner.geo;
    g.fillAll (palette::panel);
    auto r = getLocalBounds();
    auto markersArea = r.removeFromBottom (markerLane);
    g.setColour (palette::well);
    g.fillRect (markersArea);

    // loop range
    auto t = owner.doc.transport();
    const float lx0 = geo.xFor (prop (t, pids::loopStart)), lx1 = geo.xFor (prop (t, pids::loopEnd));
    const bool loopOn = (bool) t[pids::loopEnabled];
    {
        auto bar = juce::Rectangle<float> (lx0, 2.0f, juce::jmax (4.0f, lx1 - lx0), (float) loopLane - 3.0f);
        g.setColour ((loopOn ? palette::accent : palette::textDim).withAlpha (loopOn ? 0.75f : 0.35f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (loopOn ? palette::accent.brighter (0.4f) : palette::textDim);
        g.fillRect (bar.withWidth (3.0f));                              // drag handles
        g.fillRect (bar.withX (bar.getRight() - 3.0f).withWidth (3.0f));
    }
    // export range (green): what File > Export renders when "Export range" is chosen
    {
        const double es = prop (t, exportStartId), ee = prop (t, exportEndId);
        auto lane = juce::Rectangle<float> (0.0f, (float) loopLane, (float) getWidth(), (float) exportLane);
        if (ee > es)
        {
            const float ex0 = geo.xFor (es), ex1 = geo.xFor (ee);
            auto bar = juce::Rectangle<float> (ex0, lane.getY() + 1.0f, juce::jmax (4.0f, ex1 - ex0), lane.getHeight() - 2.0f);
            g.setColour (palette::good.withAlpha (0.6f));
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (palette::good.brighter (0.3f));
            g.fillRect (bar.withWidth (3.0f));
            g.fillRect (bar.withX (bar.getRight() - 3.0f).withWidth (3.0f));
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.setFont (uiFont (9.5f, true));
            if (bar.getWidth() > 50.0f) g.drawText ("EXPORT", bar.reduced (5.0f, 0.0f), juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (palette::textDim.withAlpha (0.35f));
            g.setFont (uiFont (9.5f));
            g.drawText ("drag here to set an export range", lane.reduced (6.0f, 0.0f), juce::Justification::centredLeft);
        }
    }
    if ((bool) t[pids::punchEnabled])
    {
        g.setColour (palette::bad);
        g.fillRect (juce::Rectangle<float> (lx0, 2.0f, 2.0f, (float) r.getHeight()));
        g.fillRect (juce::Rectangle<float> (lx1 - 2.0f, 2.0f, 2.0f, (float) r.getHeight()));
    }

    // bars and beats
    const int firstBar = juce::jmax (0, geo.barAt (geo.scrollX));
    const int lastBar = geo.barAt (geo.timeFor ((float) getWidth())) + 1;
    const double pxPerBar = (geo.barSecAt (firstBar + 1) - geo.barSecAt (firstBar)) * geo.pixelsPerSecond;
    int labelEvery = 1;
    while (pxPerBar * labelEvery < 50.0) labelEvery *= 2;
    g.setFont (uiFont (11.0f));
    for (int b = firstBar; b <= lastBar; ++b)
    {
        const double beat = geo.beatSecAt (b, 1) - geo.barSecAt (b);
        const float x = geo.xFor (geo.barSecAt (b));
        g.setColour (palette::textDim);
        g.drawVerticalLine ((int) x, (float) r.getBottom() - 14.0f, (float) r.getBottom());
        if (b % labelEvery == 0)
            g.drawText (juce::String (b + 1), (int) x + 3, r.getY() + numbersTop, 40, 14, juce::Justification::left);
        if (beat * geo.pixelsPerSecond > 8.0)
            for (int k = 1; k < geo.tsNum; ++k)
            {
                const float bx = geo.xFor (geo.beatSecAt (b, k));
                g.setColour (palette::outline);
                g.drawVerticalLine ((int) bx, (float) r.getBottom() - 6.0f, (float) r.getBottom());
            }
    }

    // tempo track (like Studio One's): the tempo as an envelope, points joined by lines
    if (tempoArea().getHeight() > 20)
    {
        const auto lane = tempoArea().toFloat();
        g.setColour (palette::well.withAlpha (0.7f));
        g.fillRect (lane);
        g.setColour (palette::outline);
        g.drawHorizontalLine ((int) lane.getY(), 0.0f, (float) getWidth());
        double lo, hi;
        tempoRange (lo, hi);
        g.setFont (uiFont (9.5f));
        g.setColour (palette::textDim);
        g.drawText (juce::String ((int) std::round (hi)), 4, (int) lane.getY() + 1, 40, 11, juce::Justification::left);
        g.drawText (juce::String ((int) std::round (lo)), 4, (int) lane.getBottom() - 12, 40, 11, juce::Justification::left);
        juce::Path curve;
        const auto& map = geo.tempoMap;
        {
            // base tempo, then the points in order: ramps are sampled, a jump (points on top of each other) is a straight vertical line
            std::vector<TempoMap::Point> ps;
            for (auto c : owner.doc.transport())
                if (c.hasType (pids::TEMPO)) ps.push_back ({ (double) c[pids::beat], (double) c[pids::bpm] });
            std::stable_sort (ps.begin(), ps.end(), [] (const auto& a1, const auto& b1) { return a1.quarter < b1.quarter; });
            curve.startNewSubPath (geo.xFor (0.0), tempoY ((float) map.baseBpm(), lo, hi));
            double prevSec = 0.0;
            for (const auto& pt : ps)
            {
                const double sec = map.secondsAtQuarter (pt.quarter);
                if (sec > prevSec + 1.0e-9)
                {
                    const float x0 = juce::jmax (geo.xFor (prevSec), -4.0f), x1 = juce::jmin (geo.xFor (sec), (float) getWidth() + 4.0f);
                    for (float px = x0 + 2.0f; px < x1 - 1.0f; px += 2.0f)
                        curve.lineTo (px, tempoY ((float) map.bpmAtSeconds (geo.timeFor (px)), lo, hi));
                }
                curve.lineTo (geo.xFor (sec), tempoY ((float) pt.bpm, lo, hi));
                prevSec = juce::jmax (prevSec, sec);
            }
            curve.lineTo ((float) getWidth() + 4.0f, curve.getCurrentPosition().y);
        }
        g.setColour (palette::warn.withAlpha (0.9f));
        g.strokePath (curve, juce::PathStrokeType (1.6f));
        const auto dot = [&] (double sec, double bpm, bool base) {
            const float x = geo.xFor (sec), y = tempoY ((float) bpm, lo, hi);
            if (x < -8.0f || x > (float) getWidth() + 8.0f) return;
            g.setColour (base ? palette::bg : palette::warn);
            g.fillEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f);
            g.setColour (palette::warn);
            g.drawEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f, 1.4f);
            g.setColour (palette::text);
            g.setFont (uiFont (10.0f, true));
            g.drawText (juce::String (bpm, std::abs (bpm - std::round (bpm)) < 0.005 ? 0 : 1), (int) x + 7, (int) y - 7, 46, 14, juce::Justification::left);
        };
        dot (0.0, map.baseBpm(), true);
        for (auto c : owner.doc.transport())
            if (c.hasType (pids::TEMPO)) dot (geo.tempoMap.secondsAtQuarter ((double) c[pids::beat]), (double) c[pids::bpm], false);
        if (map.isConstant())
        {
            g.setColour (palette::textDim.withAlpha (0.5f));
            g.setFont (uiFont (10.0f));
            g.drawText ("double-click to add a tempo point", lane.withTrimmedLeft (90.0f), juce::Justification::centredLeft);
        }
    }

    // markers
    g.setFont (uiFont (11.0f, true));
    for (auto m : owner.doc.markers())
    {
        const float x = geo.xFor (prop (m, ids::time));
        if (x < -100.0f || x > (float) getWidth()) continue;
        juce::Path p;
        p.addTriangle (x - 5.0f, (float) markersArea.getY() + 2.0f, x + 5.0f, (float) markersArea.getY() + 2.0f, x, (float) markersArea.getY() + 10.0f);
        g.setColour (palette::info);
        g.fillPath (p);
        g.drawText (m[ids::name].toString(), (int) x + 7, markersArea.getY(), 140, markersArea.getHeight(), juce::Justification::centredLeft);
    }

    // playhead
    const float px = geo.xFor (owner.playhead());
    g.setColour (palette::accent);
    juce::Path ph;
    ph.addTriangle (px - 6.0f, (float) r.getBottom() - 10.0f, px + 6.0f, (float) r.getBottom() - 10.0f, px, (float) r.getBottom());
    g.fillPath (ph);
    g.setColour (palette::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void Ruler::tempoRange (double& lo, double& hi) const
{
    if (tempoDrag.isValid() || tempoDragBase) { lo = dragLo; hi = dragHi; return; }
    lo = hi = owner.doc.tempo();
    for (auto c : owner.doc.transport())
        if (c.hasType (pids::TEMPO)) { lo = juce::jmin (lo, (double) c[pids::bpm]); hi = juce::jmax (hi, (double) c[pids::bpm]); }
    lo = juce::jmax (10.0, std::floor ((lo - 20.0) / 10.0) * 10.0);
    hi = std::ceil ((hi + 20.0) / 10.0) * 10.0;
    if (hi - lo < 60.0) hi = lo + 60.0;
    if (hi > 500.0) { hi = 500.0; lo = juce::jmin (lo, 440.0); }
}

float Ruler::tempoY (float bpm, double lo, double hi) const
{
    const auto lane = tempoArea();
    return (float) lane.getBottom() - 8.0f - (float) ((bpm - lo) / (hi - lo)) * (float) (lane.getHeight() - 16);
}

double Ruler::tempoBpmAt (float y, double lo, double hi) const
{
    const auto lane = tempoArea();
    return juce::jlimit (juce::jmax (10.0, lo), juce::jmin (500.0, hi), lo + (double) ((float) lane.getBottom() - 8.0f - y) / (double) (lane.getHeight() - 16) * (hi - lo));
}

juce::ValueTree Ruler::tempoAt (juce::Point<int> p, bool& isBase) const
{
    isBase = false;
    double lo, hi;
    tempoRange (lo, hi);
    const auto dist = [&] (double sec, double bpm) {
        const float dx = std::abs (owner.geo.xFor (sec) - (float) p.x), dy = std::abs (tempoY ((float) bpm, lo, hi) - (float) p.y);
        return dx < 8.0f && dy < 9.0f ? dx * dx + dy * dy : 1.0e9f;
    };
    const auto near = [&] (double sec, double bpm) { return dist (sec, bpm) < 1.0e8f; };
    juce::ValueTree best;
    float bestD = 1.0e8f;
    for (auto c : owner.doc.transport())   // stacked points: the one nearest the pointer
        if (c.hasType (pids::TEMPO))
            if (const float d = dist (owner.geo.tempoMap.secondsAtQuarter ((double) c[pids::beat]), (double) c[pids::bpm]); d < bestD) { bestD = d; best = c; }
    if (best.isValid()) return best;
    if (near (0.0, owner.doc.tempo())) isBase = true;
    return {};
}

void Ruler::editTempo (juce::ValueTree point, double quarter, bool isBase)
{
    const double current = isBase ? owner.doc.tempo() : point.isValid() ? (double) point[pids::bpm] : owner.geo.tempoMap.bpmAtQuarter (quarter);
    owner.renameDialog ("Tempo (bpm)", juce::String (current, 2), [this, point, quarter, isBase] (juce::String text) mutable {
        const double bpm = juce::jlimit (10.0, 500.0, text.getDoubleValue());
        if (text.getDoubleValue() < 10.0) return;
        if (isBase) owner.doc.setTempo (bpm);
        else if (point.isValid()) { owner.doc.undo().beginNewTransaction ("Tempo"); owner.doc.set (point, pids::bpm, bpm); }
        else owner.doc.addTempoPoint (quarter, bpm);
        repaint();
    });
}

/** Where the dragged point goes (quarter notes). The dot is kept under the pointer (where it was grabbed): since a point's own
    time depends on its tempo, its position is solved with the point included, using the current height. Other points are
    sticky in what you see: near one, the dragged point lands on exactly its position (stacking them is how an instant
    tempo change is made) and stays there until the pointer is clearly away. */
double Ruler::dragQuarterFor (float x, double bpm, bool snapOn) const
{
    const float want = x - tempoGrabDx;
    const auto timeAt = [&] (double q) {
        auto pts = tempoDragOthers;
        pts.push_back ({ q, bpm });
        TempoMap m;
        m.set (owner.doc.tempo(), std::move (pts));
        return m.secondsAtQuarter (q);
    };
    if (snapOn)
        for (const auto& o : tempoDragOthers)
        {
            const bool locked = std::abs ((double) tempoDrag[pids::beat] - o.quarter) < 1.0e-9;
            if (std::abs (owner.geo.xFor (timeAt (o.quarter)) - want) < (locked ? 40.0f : 12.0f)) return o.quarter;
        }
    const double t = owner.geo.timeFor (want);
    double lo = 0.001, hi = 4096.0;
    for (int i = 0; i < 48; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (timeAt (mid) < t ? lo : hi) = mid;
    }
    const double q = 0.5 * (lo + hi), g = owner.geo.snapBeats;
    return juce::jmax (0.001, snapOn && g > 0.0 ? std::round (q / g) * g : std::round (q * 1000.0) / 1000.0);
}

/** Where a new point goes so that it appears under the click: it is solved like a dragged point (with every existing point as
    the others), so it lands on the grid line or exactly on top of a point near the click. */
double Ruler::newPointQuarter (juce::Point<int> p, bool snapOn, double bpm)
{
    double lo, hi;
    tempoRange (lo, hi);
    if (bpm <= 0.0) bpm = std::round (tempoBpmAt ((float) p.y, lo, hi));
    tempoDragOthers.clear();
    for (auto c : owner.doc.transport())
        if (c.hasType (pids::TEMPO)) tempoDragOthers.push_back ({ (double) c[pids::beat], (double) c[pids::bpm] });
    tempoGrabDx = 0.0f;
    tempoDrag = {};
    const double q = dragQuarterFor ((float) p.x, bpm, snapOn);
    tempoDragOthers.clear();
    return q;
}

/** The beat-grid position (quarter notes) under x: on the snap grid, or free with Alt / snap off. */
double Ruler::tempoQuarterAt (float x, const TempoMap& map, bool snapOn) const
{
    const double q = map.quarterAtSeconds (owner.geo.timeFor (x));
    const double g = owner.geo.snapBeats;
    return juce::jmax (0.001, snapOn && g > 0.0 ? std::round (q / g) * g : std::round (q * 1000.0) / 1000.0);
}

void Ruler::mouseDown (const juce::MouseEvent& e)
{
    dragStart = -1.0;
    if (tempoArea().getHeight() > 20 && tempoArea().contains (e.getPosition()))
    {
        bool isBase = false;
        auto ch = tempoAt (e.getPosition(), isBase);
        const double at = newPointQuarter (e.getPosition(), ! e.mods.isAltDown());
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            if (ch.isValid() || isBase)
            {
                m.addItem (1, "Set tempo...");
                if (ch.isValid()) m.addItem (2, "Delete this point");
            }
            else m.addItem (3, "Add a tempo point here...");
            juce::Component::SafePointer<Ruler> safe (this);
            m.showMenuAsync ({}, [safe, ch, isBase, at] (int r) mutable {
                if (safe == nullptr || r == 0) return;
                if (r == 1) safe->editTempo (ch, ch.isValid() ? (double) ch[pids::beat] : 0.0, isBase);
                else if (r == 2) safe->owner.doc.removeTempoPoint (ch);
                else if (r == 3) safe->editTempo ({}, at, false);   // `at` is a quarter position
            });
            return;
        }
        tempoDrag = {};
        tempoDragBase = false;
        if (ch.isValid() || isBase)
        {
            // the pointer is read against the tempo map without this point, so a dragged point does not chase itself
            std::vector<TempoMap::Point> pts;
            for (auto c : owner.doc.transport())
                if (c.hasType (pids::TEMPO) && c != ch) pts.push_back ({ (double) c[pids::beat], (double) c[pids::bpm] });
            tempoDragOthers = pts;
            tempoDragMap.set (owner.doc.tempo(), std::move (pts));
            tempoRange (dragLo, dragHi);          // the scale stays put while dragging
            tempoDrag = ch;
            tempoDragBase = isBase;
            tempoGrabDx = ch.isValid() ? (float) e.x - owner.geo.xFor (owner.geo.tempoMap.secondsAtQuarter ((double) ch[pids::beat])) : 0.0f;
            owner.doc.undo().beginNewTransaction ("Move tempo point");
        }
        return;
    }
    if (e.y >= getHeight() - markerLane)
    {
        auto m = markerAt ((float) e.x);
        if (! m.isValid())
            return;
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem (1, "Rename marker...");
            menu.addItem (2, "Delete marker");
            juce::Component::SafePointer<Ruler> safe (this);
            menu.showMenuAsync ({}, [safe, m] (int r) mutable {
                if (safe == nullptr) return;
                auto& o = safe->owner;
                if (r == 1)
                    o.renameDialog ("Rename marker", m[ids::name].toString(), [&o, m] (juce::String n) mutable {
                        o.doc.undo().beginNewTransaction ("Rename marker");
                        o.doc.set (m, ids::name, n);
                    });
                if (r == 2) { o.doc.undo().beginNewTransaction ("Delete marker"); o.doc.markers().removeChild (m, &o.doc.undo()); }
            });
            return;
        }
        owner.locate (prop (m, ids::time));
        return;
    }
    loopDrag = LoopDrag::none;
    scrubbing = zooming = false;
    auto tr = owner.doc.transport();
    if (e.y < numbersTop)
    {
        exportDrag = e.y >= loopLane;
        if (exportDrag && e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            const bool has = prop (tr, endId (true)) > prop (tr, startId (true));
            m.addItem (1, "Export this range...", has);
            m.addItem (2, "Set export range to the loop");
            m.addItem (3, "Clear export range  (double-click)", has);
            juce::Component::SafePointer<Ruler> safe (this);
            m.showMenuAsync ({}, [safe, tr] (int r) mutable {
                if (safe == nullptr) return;
                auto& o = safe->owner;
                if (r == 1 && o.onExportRequested) o.onExportRequested();
                if (r == 2) { o.doc.undo().beginNewTransaction ("Export range"); o.doc.set (tr, exportStartId, prop (tr, pids::loopStart)); o.doc.set (tr, exportEndId, prop (tr, pids::loopEnd)); }
                if (r == 3) { o.doc.undo().beginNewTransaction ("Clear export range"); o.doc.set (tr, exportStartId, 0.0); o.doc.set (tr, exportEndId, 0.0); }
                safe->repaint();
            });
            return;
        }
        loopDrag = loopHit ((float) e.x, exportDrag);
        loopA0 = prop (tr, startId (exportDrag));
        loopB0 = prop (tr, endId (exportDrag));
        dragStart = owner.geo.snap (owner.geo.timeFor ((float) e.x), ! e.mods.isAltDown());
        owner.doc.undo().beginNewTransaction (exportDrag ? "Export range" : "Loop range");
        return;
    }
    // the bar numbers: click to move the playhead; drag sideways to scrub, down to zoom in, up to zoom out
    scrubbing = true;
    scrubMoved = false;
    zoomPps0 = owner.geo.pixelsPerSecond;
    zoomAnchor = owner.geo.timeFor ((float) e.x);
    playheadBefore = owner.playhead();
    const double clicked = owner.geo.snap (owner.geo.timeFor ((float) e.x), ! e.mods.isAltDown());
    if (owner.host.engine.isPlaying())
    {
        // while playing, a click only jumps on release: a drag that turns into a zoom must not move the playhead at all
        pendingLocate = true;
        pendingLocateTime = clicked;
    }
    else
        owner.locate (clicked);
}

juce::Identifier Ruler::startId (bool exportRange) const { return exportRange ? exportStartId : pids::loopStart; }
juce::Identifier Ruler::endId (bool exportRange) const { return exportRange ? exportEndId : pids::loopEnd; }

Ruler::LoopDrag Ruler::loopHit (float x, bool exportRange) const
{
    auto tr = owner.doc.transport();
    if (prop (tr, endId (exportRange)) <= prop (tr, startId (exportRange))) return LoopDrag::draw;
    const float a = owner.geo.xFor (prop (tr, startId (exportRange))), b = owner.geo.xFor (prop (tr, endId (exportRange)));
    const float edge = juce::jmin (7.0f, (b - a) * 0.3f);
    if (std::abs (x - a) <= edge) return LoopDrag::left;
    if (std::abs (x - b) <= edge) return LoopDrag::right;
    if (x > a && x < b) return LoopDrag::move;
    return LoopDrag::draw;
}

void Ruler::mouseMove (const juce::MouseEvent& e)
{
    if (tempoArea().getHeight() > 20 && tempoArea().contains (e.getPosition())) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    if (e.y >= numbersTop)
    {
        setMouseCursor (e.y < getHeight() - markerLane ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        return;
    }
    const auto h = loopHit ((float) e.x, e.y >= loopLane);
    setMouseCursor (h == LoopDrag::left || h == LoopDrag::right ? juce::MouseCursor::LeftRightResizeCursor
                    : h == LoopDrag::move ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void Ruler::mouseDrag (const juce::MouseEvent& e)
{
    if (tempoDrag.isValid() || tempoDragBase)
    {
        const double bpm = juce::jlimit (10.0, 500.0, e.mods.isAltDown() ? tempoBpmAt ((float) e.y, dragLo, dragHi) : std::round (tempoBpmAt ((float) e.y, dragLo, dragHi)));
        if (tempoDragBase) { if (std::abs (bpm - owner.doc.tempo()) > 1.0e-9) owner.doc.set (owner.doc.transport(), pids::bpm, bpm); return; }
        const double q = dragQuarterFor ((float) e.x, bpm, ! e.mods.isAltDown());
        if (std::abs (q - (double) tempoDrag[pids::beat]) > 1.0e-9) owner.doc.set (tempoDrag, pids::beat, q);
        if (std::abs (bpm - (double) tempoDrag[pids::bpm]) > 1.0e-9) owner.doc.set (tempoDrag, pids::bpm, bpm);
        return;
    }
    const bool snapOn = ! e.mods.isAltDown();
    // a press on the bar numbers is a click, a scrub or a zoom: nothing happens (no seeking!) until the pointer has moved
    // a few pixels, then the dominant direction decides
    if (scrubbing && ! zooming && ! scrubMoved)
    {
        const int dx = std::abs (e.getDistanceFromDragStartX()), dy = std::abs (e.getDistanceFromDragStartY());
        if (juce::jmax (dx, dy) <= 5) return;
        if (dy <= dx) scrubMoved = true;
    }
    if (scrubbing && ! zooming && ! scrubMoved)
    {
        zooming = true;   // a mostly vertical drag zooms (down: in, up: out); sideways while zooming scrolls
        if (pendingLocate) pendingLocate = false;       // playing: it never moved
        else owner.locate (playheadBefore);              // stopped: zooming leaves the playhead where it was
    }
    if (zooming)
    {
        const double pps = zoomPps0 * std::exp (e.getDistanceFromDragStartY() * 0.012);
        owner.zoomAround (pps, zoomAnchor, (float) e.x);
        return;
    }
    if (scrubbing)
    {
        pendingLocate = false;
        owner.locate (owner.geo.snap (owner.geo.timeFor ((float) e.x), snapOn));
        return;
    }
    if (loopDrag == LoopDrag::none || std::abs (e.getDistanceFromDragStartX()) < 3) return;
    auto tr = owner.doc.transport();
    const double minLen = juce::jmax (0.05, owner.geo.gridSec() > 0.0 ? owner.geo.gridSec() : 0.05);
    double a = loopA0, b = loopB0;
    const double t = owner.geo.snap (owner.geo.timeFor ((float) e.x), snapOn);
    switch (loopDrag)
    {
        case LoopDrag::move:
        {
            const double d = owner.geo.snap (loopA0 + (t - dragStart), snapOn) - loopA0;
            a = juce::jmax (0.0, loopA0 + d);
            b = a + (loopB0 - loopA0);
            break;
        }
        case LoopDrag::left: a = juce::jlimit (0.0, loopB0 - minLen, t); break;
        case LoopDrag::right: b = juce::jmax (loopA0 + minLen, t); break;
        case LoopDrag::draw: a = juce::jmin (dragStart, t); b = juce::jmax (dragStart, t); if (b - a < minLen) return; break;
        case LoopDrag::none: return;
    }
    owner.doc.set (tr, startId (exportDrag), a);
    owner.doc.set (tr, endId (exportDrag), b);
    repaint();
}

void Ruler::mouseUp (const juce::MouseEvent&)
{
    if (tempoDrag.isValid() || tempoDragBase) { tempoDrag = {}; tempoDragBase = false; repaint(); return; }
    dragStart = -1.0;
    loopDrag = LoopDrag::none;
    if (pendingLocate && scrubbing && ! zooming && ! scrubMoved) owner.locate (pendingLocateTime);
    pendingLocate = false;
    scrubbing = zooming = scrubMoved = false;
}

void Ruler::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (tempoArea().getHeight() > 20 && tempoArea().contains (e.getPosition()))
    {
        bool isBase = false;
        auto ch = tempoAt (e.getPosition(), isBase);
        if (ch.isValid() || isBase) { editTempo (ch, ch.isValid() ? (double) ch[pids::beat] : 0.0, isBase); return; }
        // a new point where you clicked (on the beat grid), at the height you clicked
        double lo, hi;
        tempoRange (lo, hi);
        const double bpm = std::round (tempoBpmAt ((float) e.y, lo, hi));
        owner.doc.addTempoPoint (newPointQuarter (e.getPosition(), ! e.mods.isAltDown(), bpm), bpm);
        return;
    }
    if (e.y >= loopLane && e.y < numbersTop && loopHit ((float) e.x, true) != LoopDrag::draw)   // double-click the export bar: clear it
    {
        auto tr = owner.doc.transport();
        owner.doc.undo().beginNewTransaction ("Clear export range");
        owner.doc.set (tr, exportStartId, 0.0);
        owner.doc.set (tr, exportEndId, 0.0);
        repaint();
        return;
    }
    if (e.y < loopLane && loopHit ((float) e.x) != LoopDrag::draw)   // double-click the loop bar: loop on / off
    {
        auto tr = owner.doc.transport();
        owner.doc.undo().beginNewTransaction ("Loop on / off");
        owner.doc.set (tr, pids::loopEnabled, ! (bool) tr[pids::loopEnabled]);
        return;
    }
    if (e.y >= getHeight() - markerLane && ! markerAt ((float) e.x).isValid())
    {
        owner.doc.undo().beginNewTransaction ("Add marker");
        owner.doc.markers().appendChild (createMarker ("Marker " + juce::String (owner.doc.markers().getNumChildren() + 1),
                                                       owner.geo.snap (owner.geo.timeFor ((float) e.x))), &owner.doc.undo());
    }
}

// =====================================================================================
// TimelineCanvas
// =====================================================================================

const std::vector<midiclip::Note>& TimelineCanvas::cachedNotes (const juce::ValueTree& clip)
{
    const auto text = clip.getChildWithName (midiclip::MIDI)[midiclip::notes].toString();
    auto& e = noteCache[clip[ids::id].toString()];
    if (e.first != text) { e.first = text; e.second = midiclip::notesOf (clip); }
    return e.second;
}

void TimelineCanvas::drawClip (juce::Graphics& g, const juce::ValueTree& clip, juce::Rectangle<float> r, juce::Colour colour,
                               bool selected)
{
    // Zoomed far in, a clip is many thousands of pixels wide. Shapes that big (rounded corners, outlines) draw
    // with glitches, so only the visible part (plus a margin) is drawn; notes and waveforms use real positions.
    r = r.withLeft (juce::jmax (r.getX(), -24.0f)).withRight (juce::jmin (r.getRight(), (float) getWidth() + 24.0f));
    if (r.getWidth() < 2.0f) return;
    const bool muted = (bool) clip[ids::muted];
    const float alpha = muted ? 0.45f : 1.0f;
    g.setColour (palette::bg.interpolatedWith (colour, selected ? 0.5f : 0.28f).withAlpha (alpha));
    g.fillRoundedRectangle (r, 4.0f);
    // outline last: a dark edge between neighbouring clips (so slices show), white when selected
    const auto outline = [&g, r, colour, selected] {
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        if (selected)
        {
            g.setColour (palette::text);
            g.drawRoundedRectangle (r.reduced (1.5f), 3.5f, 2.0f);
        }
        else
        {
            g.setColour (colour.brighter (0.2f).withAlpha (0.6f));
            g.drawRoundedRectangle (r.reduced (1.5f), 3.5f, 1.0f);
        }
    };
    auto header = r.withHeight (15.0f);
    g.setColour ((selected ? colour.brighter (0.35f) : colour).withAlpha (0.85f * alpha));
    g.fillRoundedRectangle (header, 4.0f);
    g.fillRect (header.withTrimmedTop (8.0f));

    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.setFont (uiFont (11.0f, true));
    g.drawText ((muted ? "[muted] " : "") + clip[ids::name].toString(), header.reduced (5.0f, 0.0f).toNearestInt(),
                juce::Justification::centredLeft, true);

    const auto& geo = owner.geo;
    const double start = prop (clip, ids::start), len = prop (clip, ids::length), offset = prop (clip, ids::offset);
    auto wave = r.withTrimmedTop (16.0f).reduced (0.0f, 2.0f);
    if (midiclip::isMidiClip (clip))   // MIDI: a mini piano roll
    {
        const auto& notes = cachedNotes (clip);
        int lo = 0, hi = 0;
        if (midiclip::pitchRange (notes, lo, hi) && wave.getHeight() > 4.0f)
        {
            lo = juce::jmin (lo, hi - 11);   // at least an octave tall
            const float rowH = wave.getHeight() / (float) (hi - lo + 1);
            g.setColour ((selected ? juce::Colours::white : colour.brighter (0.5f)).withAlpha (0.95f * alpha));
            for (const auto& n : notes)
            {
                const double t0 = start + (n.start - offset), t1 = t0 + n.length;
                if (t1 <= start || t0 >= start + len) continue;
                const float x0 = juce::jmax (r.getX(), geo.xFor (juce::jmax (t0, start)));
                const float x1 = juce::jmin (r.getRight(), geo.xFor (juce::jmin (t1, start + len)));
                if (x1 < 0.0f || x0 > (float) getWidth()) continue;
                const float y = wave.getBottom() - (float) (n.pitch - lo + 1) * rowH;
                g.fillRect (x0, y, juce::jmax (1.5f, x1 - x0 - 1.0f), juce::jmax (1.5f, rowH - 0.5f));
            }
        }
        else
        {
            g.setColour (palette::textDim);
            g.setFont (uiFont (11.0f));
            g.drawText ("Empty MIDI clip", wave.toNearestInt(), juce::Justification::centred, true);
        }
        outline();
        return;
    }
    auto audio = owner.session.audioFor (clip);
    if (audio == nullptr)
    {
        g.setColour (palette::textDim);
        g.setFont (uiFont (11.0f));
        const bool missing = ! owner.session.fileFor (clip).existsAsFile();
        g.drawText (missing ? "Missing file: " + clip[ids::file].toString() : juce::String ("Loading..."), wave.toNearestInt(),
                    juce::Justification::centred, true);
    }
    else if (wave.getHeight() > 6.0f)
    {
        const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
        const float clipGain = juce::Decibels::decibelsToGain ((float) prop (clip, ids::gainDb));
        const int x0 = juce::jmax ((int) r.getX(), 0), x1 = juce::jmin ((int) r.getRight(), getWidth());
        const double sr = audio->sampleRate;
        const int ch = audio->numChannels();
        const auto blocks = (juce::int64) audio->peaks.size() / (ch * 2);
        g.setColour (colour.brighter (0.4f).withAlpha (0.9f * alpha));
        for (int x = x0; x < x1; ++x)
        {
            const double s0 = (offset + (geo.timeFor ((float) x) - start)) * sr;
            const double s1 = (offset + (geo.timeFor ((float) x + 1.0f) - start)) * sr;
            if (s1 <= 0 || s0 >= audio->length()) continue;
            float lo = 0.0f, hi = 0.0f;
            if (s1 - s0 < CachedAudio::peakBlock)
            {
                const int a = juce::jlimit (0, (int) audio->length() - 1, (int) s0), b = juce::jlimit (a + 1, (int) audio->length(), (int) s1 + 1);
                for (int c = 0; c < ch; ++c)
                {
                    auto mm = juce::FloatVectorOperations::findMinAndMax (audio->buffer.getReadPointer (c, a), b - a);
                    lo = juce::jmin (lo, mm.getStart()); hi = juce::jmax (hi, mm.getEnd());
                }
            }
            else
            {
                const auto b0 = juce::jlimit ((juce::int64) 0, blocks - 1, (juce::int64) (s0 / CachedAudio::peakBlock));
                const auto b1 = juce::jlimit (b0 + 1, blocks, (juce::int64) (s1 / CachedAudio::peakBlock) + 1);
                for (auto b = b0; b < b1; ++b)
                    for (int c = 0; c < ch; ++c)
                    {
                        lo = juce::jmin (lo, audio->peaks[(size_t) ((b * ch + c) * 2)]);
                        hi = juce::jmax (hi, audio->peaks[(size_t) ((b * ch + c) * 2 + 1)]);
                    }
            }
            const double t = geo.timeFor ((float) x);
            const float fade = (float) (fadeGain (t, start, start + prop (clip, ids::fadeIn), true)
                                        * fadeGain (t, start + len - prop (clip, ids::fadeOut), start + len, false));
            const float k = clipGain * fade * half;
            g.drawVerticalLine (x, mid - juce::jmin (half, hi * k), mid - juce::jmax (-half, lo * k) + 1.0f);
        }
    }

    // fades
    const float fiW = (float) (prop (clip, ids::fadeIn) * geo.pixelsPerSecond);
    const float foW = (float) (prop (clip, ids::fadeOut) * geo.pixelsPerSecond);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    if (fiW > 1.0f)
    {
        juce::Path p;
        p.addTriangle (r.getX(), r.getY() + 15.0f, r.getX() + fiW, r.getY() + 15.0f, r.getX(), r.getBottom());
        g.fillPath (p);
    }
    if (foW > 1.0f)
    {
        juce::Path p;
        p.addTriangle (r.getRight(), r.getY() + 15.0f, r.getRight() - foW, r.getY() + 15.0f, r.getRight(), r.getBottom());
        g.fillPath (p);
    }
    if (selected || r.getWidth() > 30.0f)
    {
        g.setColour (palette::text.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> (r.getX() + fiW - 3.0f, r.getY() + 1.0f, 6.0f, 6.0f));
        g.fillRect (juce::Rectangle<float> (r.getRight() - foW - 3.0f, r.getY() + 1.0f, 6.0f, 6.0f));
    }

    outline();
}

void TimelineCanvas::paint (juce::Graphics& g)
{
    const auto& geo = owner.geo;
    g.fillAll (palette::bg);
    const int n = owner.numTracks();
    // lanes
    for (int i = 0; i < n; ++i)
    {
        const int th = geo.heightOf (i), y = geo.topOf (i) - geo.scrollY;
        if (th <= 0 || y > getHeight() || y + th < 0) continue;
        auto t = owner.trackAtIndex (i);
        if (isFolder (t))   // a folder row is just a band
        {
            g.setColour (owner.trackColour (t).withAlpha (0.16f));
            g.fillRect (0, y, getWidth(), th);
            g.setColour (palette::outline.withAlpha (0.6f));
            g.drawHorizontalLine (y + th - 1, 0.0f, (float) getWidth());
            continue;
        }
        g.setColour (owner.session.isTrackSelected (t) ? palette::panel.brighter (0.09f) : (i % 2 ? palette::bg : palette::bg.brighter (0.02f)));
        g.fillRect (0, y, getWidth(), th);
        g.setColour (palette::outline.withAlpha (0.6f));
        g.drawHorizontalLine (y + th - 1, 0.0f, (float) getWidth());
    }

    // grid
    const int firstBar = juce::jmax (0, geo.barAt (geo.scrollX));
    const int lastBar = geo.barAt (geo.timeFor ((float) getWidth())) + 1;
    for (int b = firstBar; b <= lastBar; ++b)
    {
        const double beat = geo.beatSecAt (b, 1) - geo.barSecAt (b);
        g.setColour (palette::outline.withAlpha (0.7f));
        g.drawVerticalLine ((int) geo.xFor (geo.barSecAt (b)), 0.0f, (float) getHeight());
        if (beat * geo.pixelsPerSecond > 14.0)
        {
            g.setColour (palette::outline.withAlpha (0.25f));
            for (int k = 1; k < geo.tsNum; ++k)
                g.drawVerticalLine ((int) geo.xFor (geo.beatSecAt (b, k)), 0.0f, (float) getHeight());
        }
    }

    // loop region
    auto tr = owner.doc.transport();
    if ((bool) tr[pids::loopEnabled])
    {
        const float a = juce::jmax (-10.0f, geo.xFor (prop (tr, pids::loopStart))), b = juce::jmin ((float) getWidth() + 10.0f, geo.xFor (prop (tr, pids::loopEnd)));
        g.setColour (palette::accent.withAlpha (0.06f));
        if (b > a) g.fillRect (juce::Rectangle<float> (a, 0.0f, b - a, (float) getHeight()));
    }

    // clips (child order = layering: later children drawn on top)
    for (int i = 0; i < n; ++i)
    {
        const int th = geo.heightOf (i), y = geo.topOf (i) - geo.scrollY;
        if (th <= 0 || y > getHeight() || y + th < 0) continue;
        auto t = owner.trackAtIndex (i);
        const auto colour = owner.trackColour (t);
        for (auto c : t)
        {
            if (! c.hasType (ids::CLIP)) continue;
            const float x0 = geo.xFor (prop (c, ids::start));
            const float x1 = geo.xFor (prop (c, ids::start) + prop (c, ids::length));
            if (x1 < 0.0f || x0 > (float) getWidth()) continue;
            drawClip (g, c, { x0, (float) y + 3.0f, juce::jmax (2.0f, x1 - x0), (float) th - 7.0f }, colour, owner.selection.contains (c));
        }
    }

    if (owner.automationMode()) drawAutomation (g);

    if (n == 0)
    {
        g.setColour (palette::textDim);
        g.setFont (uiFont (15.0f));
        g.drawFittedText ("No tracks yet.\nClick \"+ Track\" (Ctrl+T), or drag audio files here.", getLocalBounds(),
                          juce::Justification::centred, 3);
    }

    // recording in progress
    auto& e = owner.host.engine;
    if (owner.session.isRecordingSession() && e.isRecordingActive() && ! e.isCountingIn())
    {
        const double ph = owner.playhead();
        double from = owner.session.recordingStartSec();
        if (ph < from) from = (bool) tr[pids::loopEnabled] ? prop (tr, pids::loopStart) : 0.0;
        if ((bool) tr[pids::punchEnabled]) from = juce::jmax (from, prop (tr, pids::loopStart));
        for (auto& id : owner.session.recordingTrackIds())
        {
            const int i = owner.doc.tracks().indexOf (owner.session.findTrack (id));
            if (i < 0 || ph <= from) continue;
            const float x0 = geo.xFor (from), x1 = geo.xFor (ph);
            juce::Rectangle<float> r (x0, (float) (geo.topOf (i) - geo.scrollY) + 3.0f, x1 - x0, (float) geo.heightOf (i) - 7.0f);
            g.setColour (palette::bad.withAlpha (0.22f));
            g.fillRoundedRectangle (r, 4.0f);
            // the take's waveform as it is written (each recorded stretch where it lands on the timeline)
            if (auto rec = owner.session.recorderFor (id); rec != nullptr && rec->sampleRate > 0.0)
            {
                const auto wave = r.withTrimmedTop (12.0f).reduced (0.0f, 2.0f);
                const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
                const double rate = rec->sampleRate;
                const auto written = rec->samplesWritten();
                std::vector<float> pk;
                g.setColour (palette::bad.brighter (0.3f).withAlpha (0.9f));
                for (int s = 0; s < rec->getNumSegments(); ++s)
                {
                    const auto& seg = rec->segment (s);
                    const auto off = seg.fileOffset.load();
                    auto len = seg.length.load();
                    if (s == rec->getNumSegments() - 1 || len <= 0) len = juce::jmax ((juce::int64) 0, written - off);
                    const int n = rec->copyPeaks (off, len, pk);
                    if (n == 0) continue;
                    const double t0 = (double) seg.timelineStart.load() / rate;
                    const double secPerPeak = TrackRecorder::peakBlock / rate;
                    const float sx = geo.xFor (t0), ex = geo.xFor (t0 + n * secPerPeak);
                    for (float x = juce::jmax (sx, r.getX()); x < juce::jmin (ex, r.getRight()); x += 1.0f)
                    {
                        const int a = juce::jlimit (0, n - 1, (int) ((x - sx) / (ex - sx) * (float) n));
                        const int b = juce::jlimit (a + 1, n, (int) ((x + 1.0f - sx) / (ex - sx) * (float) n));
                        float m = 0.0f;
                        for (int k = a; k < b; ++k) m = juce::jmax (m, pk[(size_t) k]);
                        const float h = juce::jmin (1.0f, m) * half;
                        g.drawVerticalLine ((int) x, mid - h, mid + h + 1.0f);
                    }
                }
            }
            g.setColour (palette::bad);
            g.drawRoundedRectangle (r, 4.0f, 1.5f);
            g.setFont (uiFont (10.5f, true));
            g.drawText ("REC", r.reduced (5.0f, 1.0f).removeFromTop (11.0f), juce::Justification::topLeft);
        }
    }

    // markers
    for (auto m : owner.doc.markers())
    {
        const float x = geo.xFor (prop (m, ids::time));
        g.setColour (palette::info.withAlpha (0.5f));
        for (float y = 0; y < (float) getHeight(); y += 8.0f)
            g.drawVerticalLine ((int) x, y, y + 4.0f);
    }

    // drop indicator
    if (dropX >= 0)
    {
        g.setColour (palette::accent);
        g.drawVerticalLine ((int) geo.xFor (geo.snap (geo.timeFor ((float) dropX))), 0.0f, (float) getHeight());
    }

    // marquee
    if (mode == Mode::marquee && ! marqueeRect.isEmpty())
    {
        g.setColour (palette::accent.withAlpha (0.12f));
        g.fillRect (marqueeRect);
        g.setColour (palette::accent);
        g.drawRect (marqueeRect, 1);
    }

    // slice tool: where the cut goes
    if (owner.sliceMode() && sliceX >= 0.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        const float dashes[] = { 4.0f, 3.0f };
        g.drawDashedLine ({ sliceX, 0.0f, sliceX, (float) getHeight() }, dashes, 2, 1.5f);
    }

    // playhead
    g.setColour (e.isRecordingActive() ? palette::bad : palette::accent);
    g.drawVerticalLine ((int) geo.xFor (owner.playhead()), 0.0f, (float) getHeight());
}

TimelineCanvas::Mode TimelineCanvas::modeAt (const juce::ValueTree& clip, juce::Point<float> p) const
{
    const auto& geo = owner.geo;
    const float x0 = geo.xFor (prop (clip, ids::start));
    const float x1 = geo.xFor (prop (clip, ids::start) + prop (clip, ids::length));
    const int i = owner.doc.tracks().indexOf (clip.getParent());
    const float top = (float) (geo.topOf (i) - geo.scrollY) + 3.0f;
    const float fiX = x0 + (float) (prop (clip, ids::fadeIn) * geo.pixelsPerSecond);
    const float foX = x1 - (float) (prop (clip, ids::fadeOut) * geo.pixelsPerSecond);
    if (p.y < top + 10.0f && std::abs (p.x - fiX) < 6.0f) return Mode::fadeIn;
    if (p.y < top + 10.0f && std::abs (p.x - foX) < 6.0f) return Mode::fadeOut;
    const float edge = juce::jmin (6.0f, (x1 - x0) * 0.25f);
    if (p.x < x0 + edge) return Mode::trimLeft;
    if (p.x > x1 - edge) return Mode::trimRight;
    return Mode::move;
}

static juce::ValueTree clipAt (ArrangeView& v, juce::Point<float> p)
{
    const int i = v.trackIndexAtY ((int) p.y);
    auto t = v.trackAtIndex (i);
    if (! t.isValid()) return {};
    const double time = v.geo.timeFor (p.x);
    for (int c = t.getNumChildren(); --c >= 0;)
    {
        auto clip = t.getChild (c);
        if (clip.hasType (ids::CLIP) && time >= prop (clip, ids::start) && time <= prop (clip, ids::start) + prop (clip, ids::length))
            return clip;
    }
    return {};
}

void TimelineCanvas::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (owner.automationMode() && owner.trackIndexAtY (e.y) < owner.numTracks()) { mode = Mode::none; autoMouseDown (e); return; }
    mode = Mode::none;
    dragged = false;
    const auto p = e.position;
    auto clip = clipAt (owner, p);
    const int ti = owner.trackIndexAtY ((int) p.y);
    if (auto t = owner.trackAtIndex (ti); t.isValid() && clip.isValid() && ! owner.session.isTrackSelected (t))
        owner.session.clickTrack (t, e.mods.isShiftDown() || e.mods.isCommandDown() ? juce::ModifierKeys (juce::ModifierKeys::commandModifier) : juce::ModifierKeys());

    if (e.mods.isPopupMenu())
    {
        if (clip.isValid())
        {
            if (! owner.selection.contains (clip)) owner.select (clip, false);
            showClipMenu (clip);
        }
        else if (ti >= owner.numTracks())
        {
            owner.session.showAddTrackMenu (this);   // empty space below the tracks: add a track of any kind
        }
        else
        {
            showLaneMenu (ti, owner.geo.timeFor (p.x));
        }
        owner.repaintAll();
        return;
    }

    if (owner.sliceMode())
    {
        // slice tool: cut the clip under the mouse (and the other selected clips under that time) at the click
        if (clip.isValid())
        {
            const double t = owner.geo.snap (owner.geo.timeFor (p.x), ! e.mods.isAltDown());
            juce::Array<juce::ValueTree> targets { clip };
            if (owner.selection.contains (clip))
                for (auto& c : owner.selection)
                    if (c != clip && t > prop (c, ids::start) + 0.001 && t < clipEnd (c) - 0.001) targets.add (c);
            if (! e.mods.isAltDown())   // grouped tracks are cut at the same time
                for (auto tr : owner.session.linkedTracks (clip.getParent()))
                    for (auto c : tr)
                        if (c.hasType (ids::CLIP) && t > prop (c, ids::start) + 0.001 && t < clipEnd (c) - 0.001) targets.addIfNotAlreadyThere (c);
            if (t > prop (clip, ids::start) + 0.001 && t < clipEnd (clip) - 0.001) owner.splitClipsAt (targets, t);
        }
        mode = Mode::none;
        return;
    }

    if (! clip.isValid())
    {
        // Empty space: a click selects the track and clears the clip selection; a drag draws a selection rectangle.
        mode = Mode::marquee;
        marqueeRect = {};
        marqueeStartTime = owner.geo.timeFor ((float) e.x);
        marqueeStartY = e.y + owner.geo.scrollY;
        juce::Desktop::getInstance().beginDragAutoRepeat (35);   // keeps scrolling while the mouse rests past the edge
        return;
    }

    if (e.mods.isShiftDown() || e.mods.isCommandDown())
    {
        owner.select (clip, true);
        owner.repaintAll();
        if (! owner.selection.contains (clip)) return;
    }
    else if (! owner.selection.contains (clip))
    {
        owner.select (clip, false);
    }

    mode = modeAt (clip, p);
    anchor = clip;
    trackDelta = 0;
    originals.clear();
    auto& sel = owner.selection;
    if (mode == Mode::move)   // clips on grouped tracks come along
        for (int i = 0, n = sel.size(); i < n; ++i)
            for (auto& p : owner.groupPartners (sel[i])) sel.addIfNotAlreadyThere (p);
    juce::Array<juce::ValueTree> edited { clip };
    edited.addArray (owner.groupPartners (clip));   // trims and fades also apply to the grouped twins
    for (auto& c : sel)                              // ... and to every other selected clip (drag one edge, they all follow)
        if (sel.contains (clip)) edited.addIfNotAlreadyThere (c);
    for (auto& c : (mode == Mode::move ? sel : edited))
        originals.push_back ({ c, prop (c, ids::start), prop (c, ids::length), prop (c, ids::offset), prop (c, ids::fadeIn),
                               prop (c, ids::fadeOut), owner.doc.tracks().indexOf (c.getParent()) });
    const char* names[] = { "", "Move clip", "Trim clip", "Trim clip", "Fade in", "Fade out", "" };
    owner.doc.undo().beginNewTransaction (names[(int) mode]);
    owner.repaintAll();
}

void TimelineCanvas::mouseDrag (const juce::MouseEvent& e)
{
    if (owner.automationMode() && (autoLane.isValid() || autoPending)) { autoMouseDrag (e); return; }
    if (mode == Mode::marquee)
    {
        if (! dragged && e.getDistanceFromDragStart() < 4) return;
        dragged = true;
        auto& geo = owner.geo;
        // dragging past an edge scrolls the view (the farther out, the faster)
        {
            const int w = getWidth(), h = getHeight();
            const int dy = e.y < 0 ? e.y : (e.y > h ? e.y - h : 0), dx = e.x < 0 ? e.x : (e.x > w ? e.x - w : 0);
            if (dx != 0 || dy != 0)
            {
                geo.scrollY = juce::jmax (0, geo.scrollY + juce::jlimit (-60, 60, dy / 2 + (dy > 0 ? 6 : dy < 0 ? -6 : 0)));
                geo.scrollX = juce::jmax (0.0, geo.scrollX + juce::jlimit (-60, 60, dx / 2 + (dx > 0 ? 6 : dx < 0 ? -6 : 0)) / geo.pixelsPerSecond);
                owner.updateScrollBars();
            }
        }
        const int sx = juce::roundToInt (geo.xFor (marqueeStartTime)), sy = marqueeStartY - geo.scrollY;
        const int cx = juce::jlimit (0, getWidth(), e.x), cy = juce::jlimit (0, getHeight(), e.y);
        marqueeRect = juce::Rectangle<int>::leftTopRightBottom (juce::jmin (sx, cx), juce::jmin (sy, cy), juce::jmax (sx, cx), juce::jmax (sy, cy));
        const double t0 = geo.timeFor ((float) marqueeRect.getX()), t1 = geo.timeFor ((float) marqueeRect.getRight());
        const int first = owner.trackIndexAtY (marqueeRect.getY());
        const int last = juce::jmin (owner.numTracks() - 1, owner.trackIndexAtY (marqueeRect.getBottom()));
        if (! e.mods.isShiftDown()) owner.clearSelection();
        juce::Array<juce::ValueTree> tracksHit;
        for (int i = first; i <= last; ++i)
        {
            auto t = owner.trackAtIndex (i);
            if (geo.heightOf (i) <= 0 || isFolder (t)) continue;   // collapsed away / a folder row
            tracksHit.add (t);
            for (auto c : t)
                if (c.hasType (ids::CLIP) && prop (c, ids::start) < t1 && clipEnd (c) > t0)
                    owner.selection.addIfNotAlreadyThere (c);
        }
        if (! tracksHit.isEmpty())
            owner.session.setSelectedTracks (tracksHit);
        owner.repaintAll();
        return;
    }
    if (mode == Mode::none || originals.empty()) return;
    if (! dragged && e.getDistanceFromDragStart() < 3) return;
    dragged = true;
    auto& geo = owner.geo;
    auto& doc = owner.doc;
    const bool snapOn = ! e.mods.isAltDown();
    const double dt = geo.timeFor ((float) e.x) - geo.timeFor ((float) e.getMouseDownX());
    const double minLen = 0.01;

    auto findOrig = [this] (const juce::ValueTree& c) -> Orig* {
        for (auto& o : originals) if (o.clip == c) return &o;
        return nullptr;
    };
    auto* ao = findOrig (anchor);
    if (ao == nullptr) return;
    auto audio = owner.session.audioFor (anchor);

    switch (mode)
    {
        case Mode::move:
        {
            const double newStart = geo.snap (ao->start + dt, snapOn);
            const double delta = newStart - ao->start;
            const int dTrack = owner.trackIndexAtY (e.y) - owner.trackIndexAtY (e.getMouseDownY());
            bool tracksOk = true;
            for (auto& o : originals)
                tracksOk = tracksOk && juce::isPositiveAndBelow (o.track + dTrack, owner.numTracks())
                           && acceptsClip (owner.trackAtIndex (o.track + dTrack), o.clip);   // audio / MIDI clips on their own kind of track
            for (auto& o : originals)
            {
                doc.set (o.clip, ids::start, juce::jmax (0.0, o.start + delta));
                if (tracksOk && dTrack != trackDelta)
                {
                    auto target = owner.trackAtIndex (o.track + dTrack);
                    auto parent = o.clip.getParent();
                    if (target.isValid() && parent != target)
                    {
                        const bool wasSelected = owner.selection.contains (o.clip);   // removing it drops it from the selection
                        parent.removeChild (o.clip, &doc.undo());
                        target.appendChild (o.clip, &doc.undo());
                        if (wasSelected) owner.selection.addIfNotAlreadyThere (o.clip);
                    }
                }
            }
            if (tracksOk) trackDelta = dTrack;
            break;
        }
        case Mode::trimLeft:
        {
            // the anchor follows the mouse (snapped); grouped clips move their edge by the same amount
            const double shift = geo.snap (ao->start + dt, snapOn) - ao->start;
            for (auto& o : originals)
            {
                const double end = o.start + o.length;
                const double ns = juce::jlimit (juce::jmax (0.0, o.start - o.offset), end - minLen, o.start + shift);
                const double nl = end - ns;
                doc.set (o.clip, ids::start, ns);
                doc.set (o.clip, ids::offset, o.offset + (ns - o.start));
                doc.set (o.clip, ids::length, nl);
                doc.set (o.clip, ids::fadeIn, juce::jmin (o.fadeIn, nl * 0.5));
                doc.set (o.clip, ids::fadeOut, juce::jmin (o.fadeOut, nl * 0.5));
            }
            break;
        }
        case Mode::trimRight:
        {
            const double shift = geo.snap (ao->start + ao->length + dt, snapOn) - (ao->start + ao->length);
            for (auto& o : originals)
            {
                const auto a = o.clip == anchor ? audio : owner.session.audioFor (o.clip);
                const double len = a != nullptr ? a->lengthSeconds() : 1.0e9;
                const double ne = juce::jlimit (o.start + minLen, o.start + (len - o.offset), o.start + o.length + shift);
                const double nl = ne - o.start;
                doc.set (o.clip, ids::length, nl);
                doc.set (o.clip, ids::fadeIn, juce::jmin (o.fadeIn, nl * 0.5));
                doc.set (o.clip, ids::fadeOut, juce::jmin (o.fadeOut, nl * 0.5));
            }
            break;
        }
        case Mode::fadeIn:
            for (auto& o : originals)
                doc.set (o.clip, ids::fadeIn, juce::jlimit (0.0, o.length - o.fadeOut, o.fadeIn + dt));
            break;
        case Mode::fadeOut:
            for (auto& o : originals)
                doc.set (o.clip, ids::fadeOut, juce::jlimit (0.0, o.length - o.fadeIn, o.fadeOut - dt));
            break;
        case Mode::none:
        case Mode::marquee: break;
    }
    owner.repaintAll();
}

void TimelineCanvas::mouseUp (const juce::MouseEvent& e)
{
    if (autoPending) { autoPending = false; repaint(); return; }
    if (autoLane.isValid()) { autoLane = {}; autoPoint = -1; autoTrack = -1; repaint(); return; }
    autoSuppressDrag = false;
    if (mode == Mode::marquee)
    {
        if (! dragged)
        {
            owner.clearSelection();
            if (auto t = owner.trackAtIndex (owner.trackIndexAtY (e.getMouseDownY())); t.isValid())
                owner.session.clickTrack (t, e.mods);
            // the playhead only moves from the ruler (bar numbers) at the top
        }
        mode = Mode::none;
        marqueeRect = {};
        owner.repaintAll();
        return;
    }
    if (mode == Mode::move && dragged)
    {
        // a moved clip goes on top of whatever it now overlaps
        for (auto& o : originals)
        {
            auto parent = o.clip.getParent();
            const int idx = parent.indexOf (o.clip);
            if (idx >= 0 && idx != parent.getNumChildren() - 1)
                parent.moveChild (idx, parent.getNumChildren() - 1, &owner.doc.undo());
        }
    }
    mode = Mode::none;
    originals.clear();
}

bool TimelineCanvas::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && owner.sliceMode()) { owner.setSliceMode (false); return true; }
    return false;
}

void TimelineCanvas::mouseMove (const juce::MouseEvent& e)
{
    if (owner.automationMode())
    {
        int ht = -1, hp = -1;
        const int ti = owner.trackIndexAtY (e.y);
        if (auto t = owner.trackAtIndex (ti); t.isValid())
        {
            const auto prm = owner.laneParamFor (t);
            hp = pointAt (e.position, automation::pointsOf (automation::lane (t, prm, false, nullptr)), prm, laneRect (ti));
            if (hp >= 0) ht = ti;
        }
        if (ht != autoHoverTrack || hp != autoHoverPoint) { autoHoverTrack = ht; autoHoverPoint = hp; repaint(); }
        return;
    }
    auto clip = clipAt (owner, e.position);
    if (owner.sliceMode())
    {
        sliceX = clip.isValid() ? owner.geo.xFor (owner.geo.snap (owner.geo.timeFor (e.position.x), ! e.mods.isAltDown())) : -1.0f;
        setMouseCursor (juce::MouseCursor::IBeamCursor);
        repaint();
        return;
    }
    if (sliceX >= 0.0f) { sliceX = -1.0f; repaint(); }
    auto newCursor = juce::MouseCursor::NormalCursor;
    if (clip.isValid())
    {
        switch (modeAt (clip, e.position))
        {
            case Mode::trimLeft:
            case Mode::trimRight: newCursor = juce::MouseCursor::LeftRightResizeCursor; break;
            case Mode::fadeIn:
            case Mode::fadeOut: newCursor = juce::MouseCursor::CrosshairCursor; break;
            case Mode::move: newCursor = juce::MouseCursor::DraggingHandCursor; break;
            case Mode::none:
            case Mode::marquee: break;
        }
    }
    setMouseCursor (newCursor);
}

juce::Rectangle<float> TimelineCanvas::laneRect (int i) const
{
    const auto& geo = owner.geo;
    return { 0.0f, (float) (geo.topOf (i) - geo.scrollY) + 6.0f, (float) getWidth(), (float) geo.heightOf (i) - 13.0f };
}

juce::Point<float> TimelineCanvas::pointPos (const juce::String& prm, const automation::Point& p, juce::Rectangle<float> row) const
{
    return { owner.geo.xFor (p.time), row.getBottom() - automation::toNormal (prm, p.value) * row.getHeight() };
}

int TimelineCanvas::pointAt (juce::Point<float> at, const std::vector<automation::Point>& pts, const juce::String& prm, juce::Rectangle<float> row) const
{
    for (int k = 0; k < (int) pts.size(); ++k)
        if (pointPos (prm, pts[(size_t) k], row).getDistanceFrom (at) < 7.0f) return k;
    return -1;
}

void TimelineCanvas::drawAutomation (juce::Graphics& g)
{
    const int n = owner.numTracks();
    if (autoPending)   // holding on an empty spot: a ring shows where the point will go
    {
        g.setColour (palette::text.withAlpha (0.8f));
        g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (autoPendingPos), 1.5f);
    }
    for (int i = 0; i < n; ++i)
    {
        const auto row = laneRect (i);
        if (row.getBottom() < 0.0f || row.getY() > (float) getHeight()) continue;
        auto t = owner.trackAtIndex (i);
        const auto prm = owner.laneParamFor (t);
        // dim the clips: the lane is what is being edited
        g.setColour (palette::bg.withAlpha (0.55f));
        g.fillRect (row.expanded (0.0f, 3.0f));
        const auto colour = palette::warn;
        const auto pts = automation::pointsOf (automation::lane (t, prm, false, nullptr));
        g.setFont (uiFont (10.5f, true));
        g.setColour (colour.withAlpha (0.8f));
        g.drawText (owner.laneLabel (t, prm), row.reduced (6.0f, 2.0f).toNearestInt(), juce::Justification::topLeft, true);
        if (pts.empty())
        {
            const float y = row.getBottom() - automation::toNormal (prm, automation::defaultValue (prm, t)) * row.getHeight();
            const float dashes[] = { 5.0f, 4.0f };
            g.setColour (colour.withAlpha (0.45f));
            g.drawDashedLine ({ 0.0f, y, (float) getWidth(), y }, dashes, 2, 1.5f);
            continue;
        }
        juce::Path p;
        const auto first = pointPos (prm, pts.front(), row);
        p.startNewSubPath (0.0f, first.y);
        for (auto& pt : pts) p.lineTo (pointPos (prm, pt, row));
        p.lineTo ((float) getWidth(), pointPos (prm, pts.back(), row).y);
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (2.0f));
        for (int k = 0; k < (int) pts.size(); ++k)
        {
            const auto c = pointPos (prm, pts[(size_t) k], row);
            if (c.x < -6.0f || c.x > (float) getWidth() + 6.0f) continue;
            const bool hot = (i == autoTrack && k == autoPoint) || (i == autoHoverTrack && k == autoHoverPoint);
            g.setColour (hot ? juce::Colours::white : colour);
            g.fillEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f);
            if (hot)
            {
                g.setFont (uiFont (11.0f, true));
                g.drawText (owner.laneValueText (t, prm, pts[(size_t) k].value), juce::Rectangle<float> (c.x + 8.0f, c.y - 16.0f, 120.0f, 14.0f),
                            juce::Justification::centredLeft, false);
            }
        }
    }
}

namespace
{
struct LaneClipboard { juce::String param; std::vector<automation::Point> points; };
LaneClipboard& laneClipboard() { static LaneClipboard c; return c; }

/** Writes copied points into a track's lane; values are converted through their 0..1 height, so a volume lane
    pasted onto a pan lane (or another plugin parameter) keeps its shape. */
void pasteLane (ArrangeView& o, juce::ValueTree track, const juce::String& prm, const LaneClipboard& c)
{
    auto l = automation::lane (track, prm, true, &o.doc.undo());
    auto pts = c.points;
    if (c.param != prm)
        for (auto& p : pts) p.value = automation::fromNormal (prm, automation::toNormal (c.param, p.value));
    automation::setPoints (l, pts, &o.doc.undo());
}
}

bool TimelineCanvas::autoMouseDown (const juce::MouseEvent& e)
{
    const int ti = owner.trackIndexAtY (e.y);
    auto t = owner.trackAtIndex (ti);
    autoTrack = -1;
    autoPoint = -1;
    if (! t.isValid()) return true;
    const auto prm = owner.laneParamFor (t);
    const auto row = laneRect (ti);
    auto existing = automation::lane (t, prm, false, nullptr);
    auto pts = automation::pointsOf (existing);
    const int hit = pointAt (e.position, pts, prm, row);
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem (1, "Delete point", hit >= 0);
        m.addItem (2, "Clear this lane", ! pts.empty());
        m.addSeparator();
        m.addItem (3, "Copy this lane", ! pts.empty());
        m.addItem (4, "Paste onto this lane" + (laneClipboard().points.empty() ? juce::String() : "  (" + juce::String ((int) laneClipboard().points.size()) + " points)"),
                   ! laneClipboard().points.empty());
        const int others = juce::jmax (0, owner.session.selectedTracks.size() - (owner.session.isTrackSelected (t) ? 1 : 0));
        m.addItem (5, "Copy this lane to the " + juce::String (others) + " other selected track" + (others == 1 ? "" : "s"), ! pts.empty() && others > 0);
        juce::Component::SafePointer<TimelineCanvas> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, t, prm, hit] (int r) {
            if (safe == nullptr || r == 0) return;
            if (r >= 3)
            {
                auto& o = safe->owner;
                auto src = automation::pointsOf (automation::lane (t, prm, false, nullptr));
                if (r == 3) laneClipboard() = { prm, src };
                else if (r == 4 && ! laneClipboard().points.empty())
                {
                    o.doc.undo().beginNewTransaction ("Paste automation");
                    pasteLane (o, t, prm, laneClipboard());
                }
                else if (r == 5 && ! src.empty())
                {
                    o.doc.undo().beginNewTransaction ("Copy automation to tracks");
                    const LaneClipboard c { prm, src };
                    for (auto dst : o.session.selectedTracks)
                        if (dst != t && ! isFolder (dst)) pasteLane (o, dst, prm == automation::volumeParam || prm == automation::panParam ? prm : o.laneParamFor (dst), c);
                }
                safe->repaint();
                return;
            }
            auto l = automation::lane (t, prm, false, nullptr);
            if (! l.isValid()) return;
            auto p2 = automation::pointsOf (l);
            safe->owner.doc.undo().beginNewTransaction ("Automation");
            if (r == 1 && juce::isPositiveAndBelow (hit, (int) p2.size())) p2.erase (p2.begin() + hit);
            if (r == 2) p2.clear();
            automation::setPoints (l, p2, &safe->owner.doc.undo());
            safe->repaint();
        });
        return true;
    }
    autoMoved = false;
    autoSuppressDrag = false;
    if (hit >= 0)
    {
        owner.doc.undo().beginNewTransaction ("Move automation point");
        autoLane = automation::lane (t, prm, true, &owner.doc.undo());
        autoTrack = ti;
        autoPoint = hit;
        autoPts = automation::pointsOf (autoLane);
        autoGrabOffset = pointPos (prm, autoPts[(size_t) hit], row) - e.position;
        repaint();
        return true;
    }
    // empty spot: the point appears once the button has been held for a moment
    autoPending = true;
    autoPendingTrack = ti;
    autoPendingPos = e.position;
    autoPendingAlt = e.mods.isAltDown();
    const int token = ++autoPendingToken;
    juce::Component::SafePointer<TimelineCanvas> safe (this);
    juce::Timer::callAfterDelay (autoHoldMs, [safe, token] {
        if (safe == nullptr || ! safe->autoPending || safe->autoPendingToken != token) return;
        if (! juce::ModifierKeys::currentModifiers.isLeftButtonDown()) { safe->autoPending = false; safe->repaint(); return; }
        safe->autoPending = false;
        safe->addAutoPoint (safe->autoPendingTrack, safe->autoPendingPos, safe->autoPendingAlt);
        safe->autoGrabOffset = {};
        safe->autoMoved = false;   // it stays where it appeared until the mouse is dragged a few pixels
    });
    repaint();
    return true;
}

void TimelineCanvas::addAutoPoint (int ti, juce::Point<float> pos, bool noSnap, bool onLine)
{
    auto t = owner.trackAtIndex (ti);
    if (! t.isValid()) return;
    const auto prm = owner.laneParamFor (t);
    const auto row = laneRect (ti);
    owner.doc.undo().beginNewTransaction ("Add automation point");
    autoLane = automation::lane (t, prm, true, &owner.doc.undo());
    autoTrack = ti;
    auto pts = automation::pointsOf (autoLane);
    automation::Point np;
    np.time = owner.geo.snap (owner.geo.timeFor (pos.x), ! noSnap);
    np.value = automation::fromNormal (prm, juce::jlimit (0.0f, 1.0f, (row.getBottom() - pos.y) / row.getHeight()));
    if (onLine)   // a double-click: the point goes ON the line as it is now, so nothing moves until it is dragged
        np.value = automation::valueAt (pts, np.time, automation::defaultValue (prm, t));
    if (pts.empty())   // the first point: hold the current value up to here
    {
        const float v0 = automation::defaultValue (prm, t);
        if (np.time > 0.001) pts.push_back ({ 0.0, v0 });
    }
    pts.push_back (np);
    std::stable_sort (pts.begin(), pts.end(), [] (const automation::Point& a, const automation::Point& b) { return a.time < b.time; });
    for (int k = 0; k < (int) pts.size(); ++k)
        if (pts[(size_t) k].time == np.time && pts[(size_t) k].value == np.value) autoPoint = k;
    automation::setPoints (autoLane, pts, &owner.doc.undo());
    autoPts = automation::pointsOf (autoLane);
    repaint();
}

void TimelineCanvas::autoMouseDrag (const juce::MouseEvent& e)
{
    if (autoPending)
    {
        if (e.getDistanceFromDragStart() > 4) { autoPending = false; repaint(); }   // moved before the hold: no point
        return;
    }
    if (autoSuppressDrag || ! autoLane.isValid() || ! juce::isPositiveAndBelow (autoPoint, (int) autoPts.size())) return;
    if (! autoMoved && e.getDistanceFromDragStart() < 4) return;   // a click on a point does not nudge it
    autoMoved = true;
    const auto t = owner.trackAtIndex (autoTrack);
    const auto prm = owner.laneParamFor (t);
    const auto row = laneRect (autoTrack);
    const auto pos = e.position + autoGrabOffset;
    auto& p = autoPts[(size_t) autoPoint];
    const double lo = autoPoint > 0 ? autoPts[(size_t) autoPoint - 1].time : 0.0;
    const double hi = autoPoint + 1 < (int) autoPts.size() ? autoPts[(size_t) autoPoint + 1].time : 1.0e9;
    p.time = juce::jlimit (lo, hi, owner.geo.snap (owner.geo.timeFor (pos.x), ! e.mods.isAltDown()));
    p.value = automation::fromNormal (prm, juce::jlimit (0.0f, 1.0f, (row.getBottom() - pos.y) / row.getHeight()));
    automation::setPoints (autoLane, autoPts, &owner.doc.undo());
    repaint();
}

void TimelineCanvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! owner.automationMode() && ! owner.sliceMode())
        if (auto c = clipAt (owner, e.position); midiclip::isMidiClip (c)) { owner.openMidiEditor (c); return; }
    if (owner.automationMode())
    {
        autoSuppressDrag = true;   // the second press of a double-click never drags a point
        autoPending = false;
        autoMoved = false;
        const int ti = owner.trackIndexAtY (e.y);
        auto t = owner.trackAtIndex (ti);
        if (! t.isValid()) return;
        const auto prm = owner.laneParamFor (t);
        auto l = automation::lane (t, prm, false, nullptr);
        auto pts = automation::pointsOf (l);
        const int hit = pointAt (e.position, pts, prm, laneRect (ti));
        if (hit < 0)
        {
            autoPending = false;
            addAutoPoint (ti, e.position, true, true);   // at the clicked time, on the line as it is (no snap, no jump)
            autoHoverTrack = autoTrack;            // and its value shows
            autoHoverPoint = autoPoint;
            autoLane = {};
            autoPoint = -1;
            autoTrack = -1;
            return;
        }
        // type in the point's value
        auto* w = new juce::AlertWindow ("Automation point", owner.laneLabel (t, prm) + " at " + juce::String (pts[(size_t) hit].time, 2)
                                             + " s.  Type the value (" + (prm == automation::volumeParam ? juce::String ("dB, e.g. -6")
                                                                          : prm == automation::panParam ? juce::String ("e.g. C, 30 L, 100 R")
                                                                          : juce::String ("as the plugin shows it")) + ").",
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("v", owner.laneValueText (t, prm, pts[(size_t) hit].value));
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Delete point", 2);
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<TimelineCanvas> safe (this);
        const double when = pts[(size_t) hit].time;
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, t, prm, when] (int r) {
            if (safe == nullptr || r == 0) return;
            auto lane = automation::lane (t, prm, false, nullptr);
            auto p2 = automation::pointsOf (lane);
            int k = -1;
            for (int i = 0; i < (int) p2.size(); ++i) if (std::abs (p2[(size_t) i].time - when) < 1.0e-6) k = i;
            if (k < 0) return;
            auto& o = safe->owner;
            if (r == 2)
            {
                o.doc.undo().beginNewTransaction ("Delete automation point");
                p2.erase (p2.begin() + k);
            }
            else
            {
                float v = 0.0f;
                if (! o.laneValueFromText (t, prm, w->getTextEditorContents ("v"), v)) return;
                o.doc.undo().beginNewTransaction ("Automation value");
                p2[(size_t) k].value = v;
            }
            automation::setPoints (lane, p2, &o.doc.undo());
            safe->repaint();
        }), true);
        return;
    }
    const int ti = owner.trackIndexAtY (e.y);
    if (ti >= owner.numTracks()) { owner.addTrack(); return; }
    if (auto t = owner.trackAtIndex (ti); isInstrumentTrack (t) && ! owner.sliceMode() && ! clipAt (owner, e.position).isValid())
        owner.newMidiClip (t, owner.geo.timeFor ((float) e.x));
}

void TimelineCanvas::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto& geo = owner.geo;
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        owner.zoom (w.deltaY > 0 ? 1.25 : 0.8, (float) e.x);
    else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        geo.scrollX = juce::jmax (0.0, geo.scrollX - d * 300.0 / geo.pixelsPerSecond);
    }
    else
        geo.scrollY = juce::jmax (0, geo.scrollY - juce::roundToInt (w.deltaY * 200.0f));
    owner.updateScrollBars();
    owner.repaintAll();
}

bool TimelineCanvas::isInterestedInFileDrag (const juce::StringArray& files)
{
    return owner.isInterestedInFileDrag (files);
}

void TimelineCanvas::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropX = -1;
    owner.importDropped (files, x, y);
}

static bool isImportable (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;mid;midi"))
            return true;
    return false;
}

bool ArrangeView::isInterestedInFileDrag (const juce::StringArray& files) { return isImportable (files); }

void ArrangeView::filesDropped (const juce::StringArray& files, int x, int y)
{
    const auto p = canvas.getLocalPoint (this, juce::Point<int> (x, y));
    importDropped (files, juce::jmax (0, p.x), p.y);
}

void ArrangeView::importDropped (const juce::StringArray& files, int canvasX, int canvasY)
{
    juce::Array<juce::File> list;
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;mid;midi"))
            list.add (juce::File (f));
    if (list.isEmpty()) return;
    juce::ValueTree track;
    if (canvasY >= 0 && canvasY < canvas.getHeight()) track = trackAtIndex (trackIndexAtY (canvasY));
    const auto r = session.importFiles (list, track, geo.snap (geo.timeFor ((float) canvasX)));
    if (r.failed())
        showError ("Import problem", r.getErrorMessage());
    repaintAll();
}

void TimelineCanvas::showClipMenu (juce::ValueTree clip)
{
    const double ph = owner.playhead();
    const bool canSplit = ph > prop (clip, ids::start) && ph < clipEnd (clip);
    juce::PopupMenu m;
    m.addItem (1, "Split at playhead (S)", canSplit);
    m.addItem (2, "Duplicate (Ctrl+D)");
    m.addItem (3, "Delete (Del)");
    m.addSeparator();
    m.addItem (4, (bool) clip[ids::muted] ? "Unmute clip" : "Mute clip");
    m.addItem (5, "Normalize clip gain");
    m.addItem (6, "Reset clip gain", ! juce::exactlyEqual (prop (clip, ids::gainDb), 0.0));
    m.addItem (7, "Rename...");
    if (midiclip::isMidiClip (clip))
    {
        m.addItem (8, "Edit MIDI...  (double-click)");
        m.addItem (9, "Export as MIDI file...");
        m.addItem (10, "Combine MIDI clips  (Ctrl+J)", owner.canCombineMidi());
    }

    // takes: other clips on this track overlapping this one
    juce::PopupMenu takes;
    juce::Array<juce::ValueTree> overlapping;
    auto parent = clip.getParent();
    for (auto c : parent)
        if (c.hasType (ids::CLIP) && prop (c, ids::start) < clipEnd (clip) && clipEnd (c) > prop (clip, ids::start))
            overlapping.add (c);
    for (int i = 0; i < overlapping.size(); ++i)
        takes.addItem (100 + i, overlapping[i][ids::name].toString(), true, overlapping[i] == parent.getChild (parent.getNumChildren() - 1)
                                                                             || overlapping[i] == clip);
    m.addSubMenu ("Takes (bring to top)", takes, overlapping.size() > 1);

    juce::Component::SafePointer<TimelineCanvas> safe (this);
    m.showMenuAsync ({}, [safe, clip, overlapping] (int r) mutable {
        if (safe == nullptr || r == 0) return;
        auto& o = safe->owner;
        auto& um = o.doc.undo();
        if (r == 1) o.splitAtPlayhead();
        else if (r == 2) o.duplicateSelection();
        else if (r == 3) o.deleteSelection();
        else if (r == 4) { um.beginNewTransaction ("Mute clip"); o.doc.set (clip, ids::muted, ! (bool) clip[ids::muted]); }
        else if (r == 8) o.openMidiEditor (clip);
        else if (r == 9) o.exportMidi (clip);
        else if (r == 10) o.combineMidiClips();
        else if (r == 5)
        {
            if (auto a = o.session.audioFor (clip))
            {
                const float pk = a->peakInRange (prop (clip, ids::offset), prop (clip, ids::offset) + prop (clip, ids::length));
                if (pk > 0.0f)
                {
                    um.beginNewTransaction ("Normalize clip");
                    o.doc.set (clip, ids::gainDb, juce::jlimit (-60.0, 24.0, -1.0 - (double) juce::Decibels::gainToDecibels (pk)));
                }
            }
        }
        else if (r == 6) { um.beginNewTransaction ("Reset clip gain"); o.doc.set (clip, ids::gainDb, 0.0); }
        else if (r == 7)
            o.renameDialog ("Rename clip", clip[ids::name].toString(), [&o, clip] (juce::String n) mutable {
                o.doc.undo().beginNewTransaction ("Rename clip");
                o.doc.set (clip, ids::name, n);
            });
        else if (r >= 100 && r - 100 < overlapping.size())
        {
            auto take = overlapping[r - 100];
            auto parent2 = take.getParent();
            um.beginNewTransaction ("Choose take");
            o.doc.set (take, ids::muted, false);
            parent2.moveChild (parent2.indexOf (take), parent2.getNumChildren() - 1, &um);
        }
        o.repaintAll();
    });
}

void TimelineCanvas::showLaneMenu (int trackIndex, double time)
{
    juce::PopupMenu m;
    m.addItem (1, "Paste at playhead (Ctrl+V)", true);
    m.addItem (2, "Import audio here...");
    m.addItem (3, "Add marker here");
    const auto laneTrack = owner.trackAtIndex (trackIndex);
    if (isInstrumentTrack (laneTrack)) m.addItem (7, "New MIDI clip here  (double-click)");
    m.addSeparator();
    m.addItem (4, "Add audio track");
    m.addItem (8, "Add instrument track");
    m.addItem (5, "Add bus track");
    m.addItem (6, "Add FX track");
    juce::Component::SafePointer<TimelineCanvas> safe (this);
    m.showMenuAsync ({}, [safe, trackIndex, time] (int r) {
        if (safe == nullptr) return;
        auto& o = safe->owner;
        if (r == 1) o.paste();
        else if (r == 2) { o.locate (o.geo.snap (time)); o.importAudioDialog(); }
        else if (r == 3)
        {
            o.doc.undo().beginNewTransaction ("Add marker");
            o.doc.markers().appendChild (createMarker ("Marker " + juce::String (o.doc.markers().getNumChildren() + 1), o.geo.snap (time)),
                                         &o.doc.undo());
        }
        else if (r == 4) o.addTrack();
        else if (r == 5) o.session.addTrack ({}, -1, TrackKind::bus);
        else if (r == 6) o.session.addTrack ({}, -1, TrackKind::fx);
        else if (r == 8) o.session.addTrack ({}, -1, TrackKind::instrument);
        else if (r == 7) o.newMidiClip (o.trackAtIndex (trackIndex), time);
    });
}

// =====================================================================================
// ArrangeView
// =====================================================================================

ArrangeView::ArrangeView (ProjectDocument& d, EngineHost& h, SessionController& s, juce::PropertiesFile& p)
    : doc (d), host (h), session (s), settings (p)
{
    addTrackButton.setTooltip ("Add a track: audio (Ctrl+T), instrument, bus or FX. You can also right-click the empty space below the tracks.");
    importButton.setTooltip ("Import WAV/AIFF/FLAC/OGG/MP3 onto the selected track at the playhead, or a MIDI file as instrument tracks (Ctrl+I)");
    zoomInButton.setTooltip ("Zoom in (Ctrl + mouse wheel)");
    zoomOutButton.setTooltip ("Zoom out");
    fitButton.setTooltip ("Zoom to fit the song");
    addTrackButton.onRightClick = [this] { session.showAddTrackMenu (&addTrackButton); };
    headerHolder.onEmptyClick = [this] (const juce::MouseEvent& e) {
        if (e.mods.isPopupMenu()) session.showAddTrackMenu (&headerHolder);   // right-click below the track names
        else if (e.getNumberOfClicks() == 2) addTrack();
    };
    addTrackButton.onClick = [this] {
        session.showAddTrackMenu (&addTrackButton, session.primaryTrack().isValid() ? doc.tracks().indexOf (session.primaryTrack()) + 1 : -1);
    };
    importButton.onClick = [this] { importAudioDialog(); };
    zoomInButton.onClick = [this] { zoom (1.5); };
    zoomOutButton.onClick = [this] { zoom (1.0 / 1.5); };
    fitButton.onClick = [this] { zoomToFit(); };
    for (juce::Button* b : { (juce::Button*) &addTrackButton, (juce::Button*) &importButton, (juce::Button*) &zoomInButton,
                             (juce::Button*) &zoomOutButton, (juce::Button*) &fitButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    snapLabel.setText ("Snap", juce::dontSendNotification);
    styleLabel (snapLabel, 12.0f, palette::textDim, true);
    addAndMakeVisible (snapLabel);

    followButton.setClickingTogglesState (true);
    followButton.setToggleState (settings.getBoolValue ("studioFollow", true), juce::dontSendNotification);
    followButton.setTooltip ("Follow: the view scrolls along with the playhead while playing");
    followButton.onClick = [this] { settings.setValue ("studioFollow", followButton.getToggleState()); };
    sliceButton.setClickingTogglesState (true);
    sliceButton.setTooltip ("Slice: click a clip to cut it there (snaps to the grid; hold Alt for no snap). "
                            "Click Slice again (or press Esc) to go back to normal editing.");
    sliceButton.onClick = [this] { canvas.setMouseCursor (sliceMode() ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor); repaintAll(); };
    autoButton.setClickingTogglesState (true);
    autoButton.setTooltip ("Automation: draw volume, pan and plugin changes over time. Click a track's lane to add a point, "
                           "drag points, double-click one to type its value, right-click one to remove it. Pick the lane at the left of each track.");
    autoButton.onClick = [this] {
        if (automationMode() && sliceMode()) setSliceMode (false);
        for (auto* h : headers) h->refreshAutomation();
        repaintAll();
    };
    tempoButton.setClickingTogglesState (true);
    tempoButton.setTooltip ("Tempo Track: show the tempo as an envelope above the tracks. Double-click it to add a point, drag a point to change "
                            "the tempo (up / down) or move it (left / right); points joined by lines make tempo ramps. "
                            "The click, bar lines and snap grid follow it.");
    tempoButton.onClick = [this] {
        tempoTrackShown = tempoButton.getToggleState();
        doc.set (doc.transport(), juce::Identifier ("tempoTrack"), tempoTrackShown, false);
        resized();
        repaintAll();
    };
    styleToggle (allMute, palette::accent, "Mute every track (click again to unmute them all)");
    styleToggle (allSolo, palette::warn, "Solo every track (click again to clear all solos)");
    allMute.onClick = [this] {
        bool any = false;
        for (auto t : doc.tracks()) any = any || (bool) t[ids::mute];
        doc.undo().beginNewTransaction (any ? "Unmute all" : "Mute all");
        for (auto t : doc.tracks()) doc.set (t, ids::mute, ! any, true);
        refreshAllButtons();
    };
    allSolo.onClick = [this] {
        bool any = false;
        for (auto t : doc.tracks()) any = any || (bool) t[ids::solo];
        doc.undo().beginNewTransaction (any ? "Clear solo" : "Solo all");
        for (auto t : doc.tracks()) doc.set (t, ids::solo, ! any, true);
        refreshAllButtons();
    };
    addAndMakeVisible (allMute);
    addAndMakeVisible (allSolo);
    masterButton.setClickingTogglesState (true);
    masterButton.setTooltip ("Master: show the main output's automation lane under the tracks: its volume (a fade-out, say) and the "
                             "parameters of the plugins on the master bus. Double-click to add a point, drag to move it. Plays and exports.");
    masterButton.onClick = [this] {
        masterShown = masterButton.getToggleState();
        doc.set (doc.transport(), juce::Identifier ("masterLane"), masterShown, false);
        resized();
        repaintAll();
    };
    addChildComponent (masterLane);
    addChildComponent (masterHeader);
    for (auto* b : { &followButton, &sliceButton, &autoButton, &tempoButton, &masterButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    const std::pair<int, const char*> snaps[] = { { 1, "Off" }, { 2, "Bar" }, { 3, "1/2" }, { 4, "1/4 (beat)" }, { 5, "1/8" }, { 6, "1/16" } };
    for (auto& [id, n] : snaps) snapCombo.addItem (n, id);
    snapCombo.setSelectedId (settings.getIntValue ("arrangeSnap", 4), juce::dontSendNotification);
    snapCombo.onChange = [this] { settings.setValue ("arrangeSnap", snapCombo.getSelectedId()); syncGeometry(); };
    snapCombo.setTooltip ("Grid snapping (hold Alt while dragging to bypass)");
    addAndMakeVisible (snapCombo);

    geo.pixelsPerSecond = settings.getDoubleValue ("arrangeZoom", 60.0);

    addAndMakeVisible (ruler);
    addAndMakeVisible (canvas);
    addAndMakeVisible (headerHolder);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);
    vScroll.setAutoHide (false);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);

    dockHeight = settings.getIntValue ("midiDockHeight", 320);
    mixerHeight = settings.getIntValue ("miniMixerHeight", 340);
    mixerButton.setClickingTogglesState (true);
    mixerButton.setColour (juce::TextButton::buttonOnColourId, palette::accent);
    mixerButton.setTooltip ("Pop the mixer up under the tracks: faders, pan, mute / solo and plugins");
    mixerButton.onClick = [this] { setMixerShown (mixerButton.getToggleState()); };
    addAndMakeVisible (mixerButton);
    dockClose.setTooltip ("Close the MIDI editor");
    dockClose.onClick = [this] { closeMidiEditor(); };
    session.selectionChanged.addChangeListener (this);
    session.recordingFinished.addChangeListener (this);
    session.onArrangementChanged = [this] { repaintAll(); };   // a waveform finished loading
    rebind();
    startTimerHz (30);
}

ArrangeView::~ArrangeView()
{
    session.onArrangementChanged = nullptr;
    session.selectionChanged.removeChangeListener (this);
    session.recordingFinished.removeChangeListener (this);
    root.removeListener (this);
    settings.setValue ("arrangeZoom", geo.pixelsPerSecond);
}

void ArrangeView::rebind()
{
    root.removeListener (this);
    root = doc.tree();
    root.addListener (this);
    selection.clear();
    syncGeometry();
    rebuildHeaders();
}

void ArrangeView::deviceChanged()
{
    for (auto* h : headers)
        h->refreshInputs();
}

void ArrangeView::syncGeometry()
{
    geo.bpm = doc.tempo();
    geo.tempoMap = doc.tempoMap();
    if (const bool shown = (bool) doc.transport()["tempoTrack"]; shown != tempoTrackShown)
    {
        tempoTrackShown = shown;
        tempoButton.setToggleState (shown, juce::dontSendNotification);
        resized();
    }
    if (const bool shown = (bool) doc.transport()["masterLane"]; shown != masterShown)
    {
        masterShown = shown;
        masterButton.setToggleState (shown, juce::dontSendNotification);
        resized();
    }
    geo.tsNum = doc.tsNum();
    geo.tsDen = doc.tsDen();
    const double beatQ = 4.0 / geo.tsDen;
    switch (snapCombo.getSelectedId())
    {
        case 1: geo.snapBeats = 0.0; break;
        case 2: geo.snapBeats = geo.tsNum * beatQ; break;
        case 3: geo.snapBeats = 2.0; break;
        case 5: geo.snapBeats = 0.5; break;
        case 6: geo.snapBeats = 0.25; break;
        default: geo.snapBeats = 1.0; break;
    }
    repaintAll();
}

juce::Colour ArrangeView::trackColour (const juce::ValueTree& t) const
{
    return juce::Colour::fromString (t[ids::colour].toString().isNotEmpty() ? t[ids::colour].toString() : juce::String ("ff808080"));
}

void ArrangeView::refreshHeights()
{
    auto tracks = doc.tracks();
    geo.heights.resize ((size_t) tracks.getNumChildren());
    for (int i = 0; i < tracks.getNumChildren(); ++i)
    {
        auto tr = tracks.getChild (i);
        const int h = (int) tr[laneHeightId];
        geo.heights[(size_t) i] = isFolder (tr) ? folderHeight
                                : session.isHiddenByFolder (tr) ? 0   // inside a collapsed folder
                                : h > 0 ? juce::jlimit (ArrangeGeometry::minTrackHeight, ArrangeGeometry::maxTrackHeight, h) : geo.trackHeight;
    }
}

int ArrangeView::trackIndexAtY (int y) const
{
    return geo.indexAt (y + geo.scrollY);
}

void ArrangeView::select (const juce::ValueTree& clip, bool add)
{
    if (! add) selection.clear();
    const auto partners = groupPartners (clip);
    if (add && selection.contains (clip))
    {
        selection.removeFirstMatchingValue (clip);
        for (auto& p : partners) selection.removeFirstMatchingValue (p);
    }
    else
    {
        selection.addIfNotAlreadyThere (clip);
        for (auto& p : partners) selection.addIfNotAlreadyThere (p);   // grouped tracks edit together
    }
}

juce::Array<juce::ValueTree> ArrangeView::groupPartners (const juce::ValueTree& clip) const
{
    juce::Array<juce::ValueTree> out;
    const auto track = clip.getParent();
    const auto grp = track[ids::group].toString();
    if (grp.isEmpty() || juce::ModifierKeys::currentModifiers.isAltDown()) return out;   // Alt: just this clip
    const double s = prop (clip, ids::start), e = clipEnd (clip);
    const bool midi = midiclip::isMidiClip (clip);
    for (auto t : doc.tracks())
        if (t != track && t[ids::group].toString() == grp)
            for (auto c : t)
                if (c.hasType (ids::CLIP)
                    && ((std::abs (prop (c, ids::start) - s) < 0.002 && std::abs (clipEnd (c) - e) < 0.002)
                        || ((midi || midiclip::isMidiClip (c)) && prop (c, ids::start) < e - 0.001 && clipEnd (c) > s + 0.001)))   // a MIDI part and another clip (or other MIDI parts) of a different length still line up by start
                    out.add (c);   // like Studio One: only the clips lined up with this one (same start and end)
    return out;
}

void ArrangeView::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &session.recordingFinished)   // keep what was just recorded highlighted
    {
        selection.clear();
        for (auto& c : session.justRecorded)
            if (c.getParent().isValid()) selection.addIfNotAlreadyThere (c);
    }
    if (source == &session.selectionChanged && session.selectionCameFromMixer() && ! session.selectedTracks.isEmpty())   // picked in a mixer: show it here
    {
        const int i = doc.tracks().indexOf (session.selectedTracks.getFirst());
        if (i >= 0 && i < numTracks())
        {
            const int top = geo.topOf (i), h = geo.heightOf (i), view = canvas.getHeight();
            if (top < geo.scrollY) geo.scrollY = top;
            else if (top + h > geo.scrollY + view && view > 0) geo.scrollY = juce::jmax (0, top + h - view);
            updateScrollBars();
            layoutHeaders();
        }
    }
    for (auto* h : headers) h->repaint();
    canvas.repaint();
}

void ArrangeView::rebuildHeaders()
{
    headers.clear();
    auto tracks = doc.tracks();
    for (int i = 0; i < tracks.getNumChildren(); ++i)
    {
        auto* h = headers.add (new TrackHeader (*this, tracks.getChild (i), i));
        headerHolder.addAndMakeVisible (h);
    }
    selection.removeIf ([] (const juce::ValueTree& c) { return ! c.getParent().isValid() || ! c.getParent().getParent().isValid(); });
    layoutHeaders();
    updateScrollBars();
    repaint();
}

void ArrangeView::layoutHeaders()
{
    refreshHeights();
    for (auto* h : headers)
    {
        h->setVisible (geo.heightOf (h->index) > 0);
        h->setBounds (0, geo.topOf (h->index) - geo.scrollY, headerHolder.getWidth(), geo.heightOf (h->index));
    }
}

void ArrangeView::updateScrollBars()
{
    const double viewSec = canvas.getWidth() / geo.pixelsPerSecond;
    const double total = juce::jmax (songEnd (doc.tracks()) + 30.0, geo.scrollX + viewSec, 60.0);
    hScroll.setRangeLimits (0.0, total, juce::dontSendNotification);
    hScroll.setCurrentRange (geo.scrollX, viewSec, juce::dontSendNotification);
    refreshHeights();
    const int contentH = geo.totalHeight() + 80;
    vScroll.setRangeLimits (0.0, juce::jmax (contentH, canvas.getHeight()), juce::dontSendNotification);
    geo.scrollY = juce::jlimit (0, juce::jmax (0, contentH - canvas.getHeight()), geo.scrollY);
    vScroll.setCurrentRange (geo.scrollY, canvas.getHeight(), juce::dontSendNotification);
    layoutHeaders();
}

void ArrangeView::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hScroll) geo.scrollX = start;
    else geo.scrollY = (int) start;
    layoutHeaders();
    repaintAll();
}

void ArrangeView::repaintAll()
{
    canvas.repaint();
    ruler.repaint();
    if (masterShown) { masterLane.repaint(); masterHeader.repaint(); }
}

void ArrangeView::setMixerShown (bool on)
{
    if (on && miniMixer == nullptr)
    {
        miniMixer = std::make_unique<MiniMixer> (doc, host, session, settings);
        addAndMakeVisible (*miniMixer);
        addAndMakeVisible (mixerBar);
    }
    else if (! on)
    {
        miniMixer.reset();
        mixerBar.setVisible (false);
    }
    mixerButton.setToggleState (on, juce::dontSendNotification);
    resized();
}

void ArrangeView::refreshAllButtons()
{
    bool m = false, so = false;
    for (auto t : doc.tracks()) { m = m || (bool) t[ids::mute]; so = so || (bool) t[ids::solo]; }
    allMute.setToggleState (m, juce::dontSendNotification);
    allSolo.setToggleState (so, juce::dontSendNotification);
}

void ArrangeView::timerCallback()
{
    refreshAllButtons();
    for (auto* h : headers)
        h->tickMeter();

    if (host.engine.isPlaying() && followButton.getToggleState() && ! ruler.isZooming())
    {
        const double ph = playhead();
        const double viewSec = canvas.getWidth() / geo.pixelsPerSecond;
        if (ph > geo.scrollX + viewSec * 0.92 || ph < geo.scrollX)
        {
            geo.scrollX = juce::jmax (0.0, ph - viewSec * 0.08);
            updateScrollBars();
        }
    }
    repaint (0, toolbarHeight, headerWidth, Ruler::baseHeight + tempoTrackHeight + 4);   // the bpm at the playhead (and a closed lane's label gone)

    // Repainting every clip and waveform 30 times a second is heavy (macOS with a Retina screen above all), so only
    // repaint what moves: everything while recording (the take grows), the playhead's old and new position while
    // playing, and nothing while the playhead stands still.
    const int x = (int) geo.xFor (playhead());
    const bool viewMoved = ! juce::exactlyEqual (geo.scrollX, lastScrollX) || ! juce::exactlyEqual (geo.pixelsPerSecond, lastPps);   // follow scrolled, or a zoom
    lastScrollX = geo.scrollX;
    lastPps = geo.pixelsPerSecond;
    if (host.engine.isRecordingActive() || host.engine.isCountingIn() || viewMoved)
        repaintAll();
    else if (x != lastPlayheadX || host.engine.isPlaying())
    {
        const int h = canvas.getHeight();
        if (lastPlayheadX != x && lastPlayheadX > -1000000) canvas.repaint (lastPlayheadX - 2, 0, 5, h);
        canvas.repaint (x - 2, 0, 5, h);
        ruler.repaint();
        if (masterShown) { masterLane.repaint(); masterHeader.repaint(); }
    }
    lastPlayheadX = x;
}

void ArrangeView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent.hasType (pids::TRACKS) || child.hasType (pids::TRACKS)) rebuildHeaders();
    else { if (parent.hasType (pids::TRANSPORT)) syncGeometry(); repaintAll(); }
}

void ArrangeView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    selection.removeFirstMatchingValue (child);
    if (parent.hasType (pids::TRACKS)) rebuildHeaders();
    else { if (parent.hasType (pids::TRANSPORT)) syncGeometry(); repaintAll(); }
}

void ArrangeView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent.hasType (pids::TRACKS)) rebuildHeaders();
    else repaintAll();
}

void ArrangeView::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& prop)
{
    if (node.hasType (pids::TRANSPORT) || node.hasType (pids::TEMPO)) { syncGeometry(); repaintAll(); }
    else if (prop == laneHeightId || prop == ids::collapsed || prop == ids::inFolder) { layoutHeaders(); updateScrollBars(); repaintAll(); }
    else repaintAll();
}

void ArrangeView::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    auto tb = getLocalBounds().removeFromTop (toolbarHeight);
    g.setColour (palette::panel);
    g.fillRect (tb);
    g.setColour (palette::outline);
    g.drawHorizontalLine (tb.getBottom() - 1, 0.0f, (float) getWidth());
    auto corner = juce::Rectangle<int> (0, toolbarHeight, headerWidth, rulerHeight());
    g.setColour (palette::panel);
    g.fillRect (corner);
    g.setColour (palette::textDim);
    g.setFont (uiFont (11.0f, true));
    g.drawText (juce::String (numTracks()) + (numTracks() == 1 ? " TRACK" : " TRACKS"), corner.withHeight (Ruler::baseHeight).reduced (12, 0).withTrimmedRight (60), juce::Justification::centredLeft);
    if (tempoTrackShown)
    {
        auto lane = corner.withTrimmedTop (Ruler::baseHeight - Ruler::markerLane).withHeight (tempoTrackHeight);
        g.setColour (palette::warn);
        g.setFont (uiFont (11.0f, true));
        g.drawText ("TEMPO TRACK", juce::Rectangle<int> (lane.getX() + 12, lane.getY() + 8, lane.getWidth() - 24, 14), juce::Justification::centredLeft);
        g.setColour (palette::text);
        g.setFont (uiFont (18.0f, true));
        const double bpm = geo.tempoMap.bpmAtSeconds (host.engine.getPositionSeconds());
        g.drawText (juce::String (bpm, 1) + " bpm", lane.reduced (12, 6).withTrimmedTop (16), juce::Justification::centredLeft);
    }
    g.setColour (palette::outline);
    g.drawVerticalLine (headerWidth - 1, (float) toolbarHeight, (float) getHeight());
}

void ArrangeView::resized()
{
    auto r = getLocalBounds();
    auto tb = r.removeFromTop (toolbarHeight).reduced (8, 5);
    addTrackButton.setBounds (tb.removeFromLeft (80));
    tb.removeFromLeft (6);
    importButton.setBounds (tb.removeFromLeft (70));
    tb.removeFromLeft (20);
    snapLabel.setBounds (tb.removeFromLeft (40));
    snapCombo.setBounds (tb.removeFromLeft (110));
    tb.removeFromLeft (20);
    sliceButton.setBounds (tb.removeFromLeft (64));
    tb.removeFromLeft (6);
    autoButton.setBounds (tb.removeFromLeft (96));
    tb.removeFromLeft (6);
    followButton.setBounds (tb.removeFromLeft (70));
    tb.removeFromLeft (6);
    tempoButton.setBounds (tb.removeFromLeft (104));
    tb.removeFromLeft (6);
    masterButton.setBounds (tb.removeFromLeft (70));
    tb.removeFromLeft (6);
    mixerButton.setBounds (tb.removeFromLeft (70));
    {
        auto c = juce::Rectangle<int> (0, toolbarHeight, headerWidth, Ruler::baseHeight).reduced (10, 0).removeFromRight (56).withSizeKeepingCentre (56, 24);
        allMute.setBounds (c.removeFromLeft (26));
        allSolo.setBounds (c.removeFromRight (26));
    }
    fitButton.setBounds (tb.removeFromRight (44));
    tb.removeFromRight (4);
    zoomInButton.setBounds (tb.removeFromRight (30));
    tb.removeFromRight (4);
    zoomOutButton.setBounds (tb.removeFromRight (30));

    if (miniMixer != nullptr)
    {
        mixerHeight = juce::jlimit (200, juce::jmax (200, getHeight() - 200), mixerHeight);
        miniMixer->setBounds (r.removeFromBottom (mixerHeight));
        mixerBar.setBounds (r.removeFromBottom (6));
    }
    if (dockedEditor != nullptr)
    {
        dockHeight = juce::jlimit (160, juce::jmax (160, getHeight() - 200), dockHeight);
        dockedEditor->setBounds (r.removeFromBottom (dockHeight));
        auto title = r.removeFromBottom (24);
        dockClose.setBounds (title.removeFromRight (28).reduced (3));
        dockTitle.setBounds (title.withTrimmedRight (0).withX (title.getX()));
        dockTitle.setBounds (dockTitle.getBounds().withWidth (title.getWidth()));
        dockBar.setBounds (r.removeFromBottom (6));
    }
    auto rulerRow = r.removeFromTop (rulerHeight());
    rulerRow.removeFromLeft (headerWidth);
    ruler.setBounds (rulerRow.withTrimmedRight (12));
    auto bottom = r.removeFromBottom (12);
    hScroll.setBounds (bottom.withTrimmedLeft (headerWidth).withTrimmedRight (12));
    masterLane.setVisible (masterShown);
    masterHeader.setVisible (masterShown);
    if (masterShown)
    {
        auto mrow = r.removeFromBottom (masterLaneHeight);
        masterHeader.setBounds (mrow.removeFromLeft (headerWidth));
        masterLane.setBounds (mrow.withTrimmedRight (12));
    }
    vScroll.setBounds (r.removeFromRight (12));
    headerHolder.setBounds (r.removeFromLeft (headerWidth));
    canvas.setBounds (r);
    updateScrollBars();
}

// ------------------------------------------------------------------ commands

void ArrangeView::showError (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (title).withMessage (message).withButton ("OK").withAssociatedComponent (this),
                                  nullptr);
}

void ArrangeView::renameDialog (const juce::String& title, const juce::String& current, std::function<void (juce::String)> onDone)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", current);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, onDone] (int r) {
        const auto n = w->getTextEditorContents ("name").trim();
        if (r == 1 && n.isNotEmpty()) onDone (n);
    }), true);
}

void ArrangeView::addTrack()
{
    const auto primary = session.primaryTrack();
    session.addTrack ({}, primary.isValid() ? doc.tracks().indexOf (primary) + 1 : -1);
}

juce::String ArrangeView::laneParamFor (const juce::ValueTree& track) const
{
    const auto p = track[automation::showLane].toString();
    return p.isNotEmpty() ? p : automation::volumeParam;
}

juce::String ArrangeView::laneLabel (const juce::ValueTree& track, const juce::String& prm) const
{
    if (prm == automation::volumeParam) return "Volume";
    if (prm == automation::panParam) return "Pan";
    int slot = -1, index = -1;
    if (! automation::parsePluginParam (prm, slot, index)) return prm;
    for (int i = 0; i < rig::numInserts (track); ++i)
    {
        const auto node = rig::insertNode (track, i);
        if ((int) node[rig::ids::slot] != slot) continue;
        juce::String pname = "parameter " + juce::String (index + 1);
        if (auto p = session.rigs().insertPlugin (track, i))
            if (juce::isPositiveAndBelow (index, p->getParameters().size())) pname = p->getParameters()[index]->getName (40);
        return node[rig::ids::name].toString() + ": " + pname;
    }
    return "Removed plugin";
}

void ArrangeView::openMidiEditor (juce::ValueTree clip)
{
    if (! midiclip::isMidiClip (clip)) return;
    const auto id = clip[ids::id].toString();
    if (dockedEditor == nullptr || dockedClipId != id)
    {
        dockedEditor = std::make_unique<PianoRoll> (doc, host, clip);
        dockedClipId = id;
        dockTitle.text = "MIDI editor  -  " + clip.getParent()[ids::name].toString() + " / " + clip[ids::name].toString();
        dockedEditor->onClipGone = [safe = juce::Component::SafePointer<ArrangeView> (this), id] {
            juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr && safe->dockedClipId == id) safe->closeMidiEditor(); });
        };
        addAndMakeVisible (*dockedEditor);
        addAndMakeVisible (dockBar);
        addAndMakeVisible (dockTitle);
        addAndMakeVisible (dockClose);
        resized();
    }
    dockedEditor->grabKeyboardFocus();
}

void ArrangeView::closeMidiEditor()
{
    dockedEditor.reset();
    dockedClipId = {};
    dockBar.setVisible (false);
    dockTitle.setVisible (false);
    dockClose.setVisible (false);
    resized();
}

juce::AudioProcessorParameter* ArrangeView::laneParameter (const juce::ValueTree& track, const juce::String& prm) const
{
    int slot = -1, index = -1;
    if (! automation::parsePluginParam (prm, slot, index)) return nullptr;
    for (int i = 0; i < rig::numInserts (track); ++i)
        if ((int) rig::insertNode (track, i)[rig::ids::slot] == slot)
            if (auto p = session.rigs().insertPlugin (track, i))
                return juce::isPositiveAndBelow (index, p->getParameters().size()) ? p->getParameters()[index] : nullptr;
    return nullptr;
}

juce::String ArrangeView::laneValueText (const juce::ValueTree& track, const juce::String& prm, float v) const
{
    if (auto* p = laneParameter (track, prm))
    {
        const auto text = p->getText (v, 32).trim();
        const auto label = p->getLabel().trim();
        return label.isNotEmpty() && ! text.endsWith (label) ? text + " " + label : text;
    }
    return automation::valueText (prm, v);
}

bool ArrangeView::laneValueFromText (const juce::ValueTree& track, const juce::String& prm, const juce::String& textIn, float& out) const
{
    const auto text = textIn.trim().toLowerCase();
    if (text.isEmpty()) return false;
    if (prm == automation::volumeParam)
    {
        if (text.contains ("inf")) { out = -60.0f; return true; }
        out = juce::jlimit (-60.0f, 6.0f, text.retainCharacters ("-+.0123456789").getFloatValue());
        return true;
    }
    if (prm == automation::panParam)
    {
        if (text == "c" || text == "center" || text == "centre") { out = 0.0f; return true; }
        float amount = text.retainCharacters ("-+.0123456789").getFloatValue();
        if (text.containsChar ('l')) amount = -std::abs (amount);
        else if (text.containsChar ('r')) amount = std::abs (amount);
        if (text.retainCharacters ("0123456789").isEmpty()) amount = text.containsChar ('l') ? -100.0f : 100.0f;   // just "L" / "R"
        out = juce::jlimit (-1.0f, 1.0f, std::abs (amount) > 1.0f ? amount / 100.0f : amount);
        return true;
    }
    if (auto* p = laneParameter (track, prm))
    {
        out = juce::jlimit (0.0f, 1.0f, p->getValueForText (textIn.trim()));
        return true;
    }
    out = juce::jlimit (0.0f, 1.0f, text.retainCharacters ("-+.0123456789").getFloatValue() / 100.0f);   // percent
    return true;
}

void ArrangeView::showLaneChooser (juce::ValueTree track, juce::Component* target)
{
    juce::PopupMenu m;
    const auto cur = laneParamFor (track);
    auto has = [&track] (const juce::String& prm) { return automation::lane (track, prm, false, nullptr)[automation::points].toString().isNotEmpty(); };
    juce::StringArray params;
    auto add = [&] (juce::PopupMenu& menu, const juce::String& prm, const juce::String& label) {
        params.add (prm);
        menu.addItem (params.size(), label + (has (prm) ? "   *" : ""), true, prm == cur);
    };
    m.addSectionHeader ("Automation lane  (* = has points)");
    add (m, automation::volumeParam, "Volume");
    if (! track.hasType (pids::MASTER)) add (m, automation::panParam, "Pan");
    for (int i = 0; i < rig::numInserts (track); ++i)
    {
        const auto node = rig::insertNode (track, i);
        juce::PopupMenu sub;
        if (auto p = session.rigs().insertPlugin (track, i))
        {
            const auto& ps = p->getParameters();
            for (int k = 0; k < ps.size() && k < 400; ++k)
                if (ps[k]->isAutomatable()) add (sub, automation::pluginParam ((int) node[rig::ids::slot], k), ps[k]->getName (60));
        }
        else sub.addItem (-1, "(the plugin is not loaded)", false);
        m.addSubMenu (node[rig::ids::name].toString(), sub);
    }
    if (rig::numInserts (track) == 0) m.addItem (-1, "(add plugins to the track to automate their parameters)", false);
    m.addSeparator();
    m.addItem (-2, "Clear this lane", has (cur));
    juce::Component::SafePointer<ArrangeView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [safe, track, params, cur] (int r) {
        if (safe == nullptr) return;
        auto t = track;
        if (r == -2)
        {
            safe->doc.undo().beginNewTransaction ("Clear automation");
            if (auto l = automation::lane (t, cur, false, nullptr); l.isValid()) automation::setPoints (l, {}, &safe->doc.undo());
        }
        else if (r > 0 && r <= params.size()) t.setProperty (automation::showLane, params[r - 1], nullptr);
        for (auto* h : safe->headers) h->refreshAutomation();
        safe->repaintAll();
    });
}

// ------------------------------------------------------------------ master lane

namespace
{
juce::ValueTree masterOf (ArrangeView& v) { return project::masterNode (v.doc.tree()); }
}

juce::Point<float> MasterLane::posOf (const juce::String& prm, const automation::Point& p) const
{
    const auto r = row();
    return { owner.geo.xFor (p.time), r.getBottom() - automation::toNormal (prm, p.value) * r.getHeight() };
}

int MasterLane::pointAt (juce::Point<float> at, const std::vector<automation::Point>& pts, const juce::String& prm) const
{
    for (int k = 0; k < (int) pts.size(); ++k)
        if (posOf (prm, pts[(size_t) k]).getDistanceFrom (at) < 7.0f) return k;
    return -1;
}

void MasterLane::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg.brighter (0.04f));
    g.setColour (palette::text.withAlpha (0.15f));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    auto m = masterOf (owner);
    if (! m.isValid()) return;
    const auto prm = owner.laneParamFor (m);
    const auto r = row();
    const auto colour = palette::warn;
    const auto pts = automation::pointsOf (automation::lane (m, prm, false, nullptr));
    if (pts.empty())
    {
        const float y = r.getBottom() - automation::toNormal (prm, automation::defaultValue (prm, m)) * r.getHeight();
        const float dashes[] = { 5.0f, 4.0f };
        g.setColour (colour.withAlpha (0.45f));
        g.drawDashedLine ({ 0.0f, y, (float) getWidth(), y }, dashes, 2, 1.5f);
        g.setFont (uiFont (11.0f, false));
        g.setColour (palette::text.withAlpha (0.5f));
        g.drawText ("Double-click to add a point", getLocalBounds().reduced (8, 0), juce::Justification::centred, false);
        return;
    }
    juce::Path p;
    p.startNewSubPath (0.0f, posOf (prm, pts.front()).y);
    for (auto& pt : pts) p.lineTo (posOf (prm, pt));
    p.lineTo ((float) getWidth(), posOf (prm, pts.back()).y);
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (2.0f));
    for (int k = 0; k < (int) pts.size(); ++k)
    {
        const auto c = posOf (prm, pts[(size_t) k]);
        if (c.x < -6.0f || c.x > (float) getWidth() + 6.0f) continue;
        const bool hot = k == dragPoint;
        g.setColour (hot ? juce::Colours::white : colour);
        g.fillEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f);
        if (hot)
        {
            g.setFont (uiFont (11.0f, true));
            g.drawText (owner.laneValueText (m, prm, pts[(size_t) k].value), juce::Rectangle<float> (c.x + 8.0f, c.y - 16.0f, 120.0f, 14.0f),
                        juce::Justification::centredLeft, false);
        }
    }
}

void MasterLane::mouseMove (const juce::MouseEvent& e)
{
    auto m = masterOf (owner);
    if (! m.isValid()) return;
    const auto prm = owner.laneParamFor (m);
    const auto pts = automation::pointsOf (automation::lane (m, prm, false, nullptr));
    setMouseCursor (pointAt (e.position, pts, prm) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void MasterLane::mouseDown (const juce::MouseEvent& e)
{
    auto m = masterOf (owner);
    if (! m.isValid()) return;
    const auto prm = owner.laneParamFor (m);
    auto pts = automation::pointsOf (automation::lane (m, prm, false, nullptr));
    const int hit = pointAt (e.position, pts, prm);
    moved = false;
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Delete point", hit >= 0);
        menu.addItem (2, "Clear this lane", ! pts.empty());
        juce::Component::SafePointer<MasterLane> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, m, prm, hit] (int r) {
            if (safe == nullptr || r == 0) return;
            auto l = automation::lane (m, prm, false, nullptr);
            if (! l.isValid()) return;
            auto p2 = automation::pointsOf (l);
            safe->owner.doc.undo().beginNewTransaction ("Master automation");
            if (r == 1 && juce::isPositiveAndBelow (hit, (int) p2.size())) p2.erase (p2.begin() + hit);
            if (r == 2) p2.clear();
            automation::setPoints (l, p2, &safe->owner.doc.undo());
            safe->repaint();
        });
        return;
    }
    if (hit < 0) return;
    owner.doc.undo().beginNewTransaction ("Move master automation point");
    dragLane = automation::lane (m, prm, true, &owner.doc.undo());
    dragPts = automation::pointsOf (dragLane);
    dragPoint = hit;
    grabOffset = posOf (prm, dragPts[(size_t) hit]) - e.position;
    repaint();
}

void MasterLane::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragLane.isValid() || ! juce::isPositiveAndBelow (dragPoint, (int) dragPts.size())) return;
    if (! moved && e.getDistanceFromDragStart() < 4) return;
    moved = true;
    auto m = masterOf (owner);
    const auto prm = owner.laneParamFor (m);
    const auto r = row();
    const auto pos = e.position + grabOffset;
    auto& p = dragPts[(size_t) dragPoint];
    const double lo = dragPoint > 0 ? dragPts[(size_t) dragPoint - 1].time : 0.0;
    const double hi = dragPoint + 1 < (int) dragPts.size() ? dragPts[(size_t) dragPoint + 1].time : 1.0e9;
    p.time = juce::jlimit (lo, hi, owner.geo.snap (owner.geo.timeFor (pos.x), ! e.mods.isAltDown()));
    p.value = automation::fromNormal (prm, juce::jlimit (0.0f, 1.0f, (r.getBottom() - pos.y) / r.getHeight()));
    automation::setPoints (dragLane, dragPts, &owner.doc.undo());
    repaint();
}

void MasterLane::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto m = masterOf (owner);
    if (! m.isValid()) return;
    const auto prm = owner.laneParamFor (m);
    auto l = automation::lane (m, prm, false, nullptr);
    auto pts = automation::pointsOf (l);
    const int hit = pointAt (e.position, pts, prm);
    if (hit < 0)
    {
        const auto r = row();
        owner.doc.undo().beginNewTransaction ("Add master automation point");
        auto lane = automation::lane (m, prm, true, &owner.doc.undo());
        auto p2 = automation::pointsOf (lane);
        automation::Point np;
        np.time = owner.geo.snap (owner.geo.timeFor (e.position.x), ! e.mods.isAltDown());
        np.value = automation::fromNormal (prm, juce::jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight()));
        if (p2.empty() && np.time > 0.001) p2.push_back ({ 0.0, automation::defaultValue (prm, m) });
        p2.push_back (np);
        std::stable_sort (p2.begin(), p2.end(), [] (const automation::Point& a, const automation::Point& b) { return a.time < b.time; });
        automation::setPoints (lane, p2, &owner.doc.undo());
        dragLane = {};
        dragPoint = -1;
        repaint();
        return;
    }
    auto* w = new juce::AlertWindow ("Master automation point", owner.laneLabel (m, prm) + " at " + juce::String (pts[(size_t) hit].time, 2)
                                         + " s.  Type the value (" + (prm == automation::volumeParam ? juce::String ("dB, e.g. -6, or -inf for silence")
                                                                      : juce::String ("as the plugin shows it")) + ").",
                                     juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("v", owner.laneValueText (m, prm, pts[(size_t) hit].value));
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Delete point", 2);
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MasterLane> safe (this);
    const double when = pts[(size_t) hit].time;
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, m, prm, when] (int res) {
        if (safe == nullptr || res == 0) return;
        auto lane = automation::lane (m, prm, false, nullptr);
        auto p2 = automation::pointsOf (lane);
        int k = -1;
        for (int i = 0; i < (int) p2.size(); ++i) if (std::abs (p2[(size_t) i].time - when) < 1.0e-6) k = i;
        if (k < 0) return;
        auto& o = safe->owner;
        if (res == 2)
        {
            o.doc.undo().beginNewTransaction ("Delete master automation point");
            p2.erase (p2.begin() + k);
        }
        else
        {
            float v = 0.0f;
            if (! o.laneValueFromText (m, prm, w->getTextEditorContents ("v"), v)) return;
            o.doc.undo().beginNewTransaction ("Master automation value");
            p2[(size_t) k].value = v;
        }
        automation::setPoints (lane, p2, &o.doc.undo());
        safe->repaint();
    }), true);
}

MasterHeader::MasterHeader (ArrangeView& o) : owner (o)
{
    addAndMakeVisible (laneButton);
    laneButton.setTooltip ("Which master parameter the lane below shows: the master volume, or a parameter of a plugin on the master bus.");
    laneButton.onClick = [this] { owner.showLaneChooser (masterOf (owner), &laneButton); };
}

void MasterHeader::resized()
{
    laneButton.setBounds (getLocalBounds().reduced (8, 0).withTrimmedTop (30).withHeight (24));
}

void MasterHeader::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg.brighter (0.08f));
    g.setColour (palette::text.withAlpha (0.15f));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    auto m = masterOf (owner);
    if (! m.isValid()) return;
    const auto prm = owner.laneParamFor (m);
    laneButton.setButtonText ("A: " + owner.laneLabel (m, prm));
    g.setColour (palette::text);
    g.setFont (uiFont (13.0f, true));
    g.drawText ("MASTER", 10, 6, 120, 20, juce::Justification::centredLeft, false);
    const auto pts = automation::pointsOf (automation::lane (m, prm, false, nullptr));
    g.setFont (uiFont (11.0f, false));
    g.setColour (palette::text.withAlpha (0.7f));
    g.drawText (pts.empty() ? juce::String ("no automation") : juce::String (pts.size()) + " points",
                10, 62, getWidth() - 20, 16, juce::Justification::centredLeft, false);
}


void ArrangeView::newMidiClip (juce::ValueTree track, double time)
{
    if (! isInstrumentTrack (track)) return;
    const int barIndex = geo.barAt (time);
    const double start = geo.barSecAt (barIndex);
    const double bar = geo.barSecAt (barIndex + 1) - start;
    int n = 1;
    for (auto c : track) n += midiclip::isMidiClip (c) ? 1 : 0;
    auto clip = midiclip::createMidiClip (track[ids::name].toString() + " " + juce::String (n), start, {});
    clip.setProperty (ids::length, bar, nullptr);
    doc.undo().beginNewTransaction ("New MIDI clip");
    track.appendChild (clip, &doc.undo());
    session.setSelectedTracks ({ track });
    openMidiEditor (clip);
}

void ArrangeView::exportMidi (juce::ValueTree item)
{
    const bool isClip = item.hasType (ids::CLIP);
    auto track = isClip ? item.getParent() : item;
    std::vector<midiclip::Note> notes;
    std::vector<midiclip::Pedal> pedal;
    if (isClip) midiclip::clipContent (item, notes, pedal);
    else
        for (auto c : track)
            if (midiclip::isMidiClip (c) && ! (bool) c[ids::muted])
            {
                std::vector<midiclip::Note> cn;
                std::vector<midiclip::Pedal> cp;
                midiclip::clipContent (c, cn, cp);
                const double at = prop (c, ids::start);
                for (auto& x : cn) { x.start += at; notes.push_back (x); }
                for (auto& x : cp) { x.time += at; pedal.push_back (x); }
            }
    if (notes.empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export MIDI",
                                                isClip ? "This clip has no notes yet." : "This track has no MIDI notes yet.");
        return;
    }
    const auto name = (isClip ? item : track)[ids::name].toString();
    const auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                           .getChildFile (juce::File::createLegalFileName (name.isNotEmpty() ? name : juce::String ("MIDI")) + ".mid");
    chooser = std::make_unique<juce::FileChooser> ("Export MIDI file", start, "*.mid");
    juce::Component::SafePointer<ArrangeView> safe (this);
    const auto trackName = track[ids::name].toString();
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, notes, pedal, trackName] (const juce::FileChooser& fc) {
                              if (safe == nullptr || fc.getResult() == juce::File()) return;
                              auto f = fc.getResult().withFileExtension ("mid");
                              const auto err = midiclip::writeMidiFile (f, trackName, notes, pedal, safe->doc.tempo(), safe->doc.tsNum(),
                                                                        safe->doc.tsDen());
                              if (err.isNotEmpty())
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export MIDI", err);
                          });
}

void ArrangeView::importAudioDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.mid;*.midi");
    juce::Component::SafePointer<ArrangeView> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe] (const juce::FileChooser& fc) {
                              if (safe == nullptr || fc.getResults().isEmpty()) return;
                              const auto r = safe->session.importFiles (fc.getResults(), safe->session.primaryTrack(), safe->playhead());
                              if (r.failed()) safe->showError ("Import problem", r.getErrorMessage());
                          });
}

void ArrangeView::deleteSelection()
{
    if (selection.isEmpty()) return;
    doc.undo().beginNewTransaction ("Delete clips");
    const auto doomed = selection;   // a copy: removing a clip also drops it from `selection` (valueTreeChildRemoved)
    for (auto c : doomed)
        if (auto p = c.getParent(); p.isValid())
            p.removeChild (c, &doc.undo());
    selection.clear();
    repaintAll();
}

void ArrangeView::splitAtPlayhead()
{
    const double t = playhead();
    juce::Array<juce::ValueTree> targets;
    for (auto& c : selection)
        if (t > prop (c, ids::start) + 0.001 && t < clipEnd (c) - 0.001) targets.add (c);
    if (targets.isEmpty())
        for (auto sel : session.selectedTracks)
            for (auto tr : session.linkedTracks (sel))
                for (auto c : tr)
                    if (c.hasType (ids::CLIP) && t > prop (c, ids::start) + 0.001 && t < clipEnd (c) - 0.001) targets.addIfNotAlreadyThere (c);
    for (int i = 0; i < targets.size(); ++i)   // grouped tracks are cut too
        for (auto& p : groupPartners (targets[i]))
            if (t > prop (p, ids::start) + 0.001 && t < clipEnd (p) - 0.001) targets.addIfNotAlreadyThere (p);
    splitClipsAt (targets, t);
}

void ArrangeView::splitClipsAt (const juce::Array<juce::ValueTree>& targets, double t)
{
    if (targets.isEmpty()) return;

    doc.undo().beginNewTransaction ("Split");
    for (auto c : targets)
    {
        const double s = prop (c, ids::start), len = prop (c, ids::length);
        auto right = c.createCopy();
        right.setProperty (ids::id, newId(), nullptr);
        right.setProperty (ids::start, t, nullptr);
        right.setProperty (ids::offset, prop (c, ids::offset) + (t - s), nullptr);
        right.setProperty (ids::length, s + len - t, nullptr);
        right.setProperty (ids::fadeIn, juce::jmin (defaultFade, (s + len - t) * 0.5), nullptr);
        right.setProperty (ids::fadeOut, juce::jmin (prop (c, ids::fadeOut), (s + len - t) * 0.5), nullptr);
        doc.set (c, ids::length, t - s);
        doc.set (c, ids::fadeOut, juce::jmin (defaultFade, (t - s) * 0.5));
        doc.set (c, ids::fadeIn, juce::jmin (prop (c, ids::fadeIn), (t - s) * 0.5));
        auto parent = c.getParent();
        parent.addChild (right, parent.indexOf (c) + 1, &doc.undo());
    }
    selection.clear();   // a click after a split picks just that piece (and its grouped twins)
    repaintAll();
}

void ArrangeView::duplicateSelection()
{
    if (selection.isEmpty()) return;
    double a = 1.0e12, b = 0.0;
    for (auto& c : selection) { a = juce::jmin (a, prop (c, ids::start)); b = juce::jmax (b, clipEnd (c)); }
    const double shift = b - a;
    doc.undo().beginNewTransaction ("Duplicate");
    juce::Array<juce::ValueTree> created;
    for (auto& c : selection)
    {
        auto copy = c.createCopy();
        copy.setProperty (ids::id, newId(), nullptr);
        copy.setProperty (ids::start, prop (c, ids::start) + shift, nullptr);
        c.getParent().appendChild (copy, &doc.undo());
        created.add (copy);
    }
    selection = created;
    repaintAll();
}

namespace
{
std::map<juce::String, juce::Array<juce::ValueTree>> midiClipsByTrack (const juce::Array<juce::ValueTree>& selection)
{
    std::map<juce::String, juce::Array<juce::ValueTree>> groups;
    for (auto& c : selection)
        if (c.getParent().isValid() && midiclip::isMidiClip (c))
            groups[c.getParent()[ids::id].toString()].add (c);
    return groups;
}
}

bool ArrangeView::canCombineMidi() const
{
    for (auto& [id, list] : midiClipsByTrack (selection))
        if (list.size() > 1) return true;
    return false;
}

void ArrangeView::combineMidiClips()
{
    juce::Array<juce::ValueTree> created;
    bool began = false;
    for (auto& [id, list] : midiClipsByTrack (selection))
    {
        if (list.size() < 2) continue;
        auto sorted = list;
        std::stable_sort (sorted.begin(), sorted.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) { return prop (a, ids::start) < prop (b, ids::start); });
        const double start = prop (sorted.getFirst(), ids::start);
        double end = start;
        std::vector<midiclip::Note> notes;
        std::vector<midiclip::Pedal> pedal;
        for (auto& c : sorted)
        {
            std::vector<midiclip::Note> n;
            std::vector<midiclip::Pedal> p;
            midiclip::clipContent (c, n, p);
            const double shift = prop (c, ids::start) - start;
            for (auto& x : n) { x.start += shift; notes.push_back (x); }
            for (auto& x : p) { x.time += shift; pedal.push_back (x); }
            end = juce::jmax (end, clipEnd (c));
        }
        std::stable_sort (notes.begin(), notes.end(), [] (const midiclip::Note& a, const midiclip::Note& b) { return a.start < b.start; });
        std::stable_sort (pedal.begin(), pedal.end(), [] (const midiclip::Pedal& a, const midiclip::Pedal& b) { return a.time < b.time; });
        auto merged = midiclip::createMidiClip (sorted.getFirst()[ids::name].toString(), start, notes, pedal);
        merged.setProperty (ids::length, juce::jmax (0.25, end - start), nullptr);   // keeps the gaps and the tail of the last clip
        if (sorted.getFirst().hasProperty (midiclip::channel)) merged.setProperty (midiclip::channel, sorted.getFirst()[midiclip::channel], nullptr);
        if (! began) { doc.undo().beginNewTransaction ("Combine MIDI clips"); began = true; }
        auto track = sorted.getFirst().getParent();
        for (auto& c : sorted) track.removeChild (c, &doc.undo());
        track.appendChild (merged, &doc.undo());
        created.add (merged);
    }
    if (created.isEmpty()) return;
    selection = created;
    repaintAll();
}

void ArrangeView::copySelection (bool cut)
{
    if (selection.isEmpty()) return;
    clipboard.clear();
    double a = 1.0e12;
    int firstTrack = 1 << 20;
    for (auto& c : selection)
    {
        a = juce::jmin (a, prop (c, ids::start));
        firstTrack = juce::jmin (firstTrack, doc.tracks().indexOf (c.getParent()));
    }
    for (auto& c : selection)
        clipboard.push_back ({ c.createCopy(), doc.tracks().indexOf (c.getParent()) - firstTrack, prop (c, ids::start) - a });
    if (cut)
    {
        doc.undo().beginNewTransaction ("Cut");
        const auto doomed = selection;   // a copy: see deleteSelection
        for (auto c : doomed)
            if (c.getParent().isValid()) c.getParent().removeChild (c, &doc.undo());
        selection.clear();
    }
    repaintAll();
}

void ArrangeView::paste()
{
    if (clipboard.empty()) return;
    const auto primary = session.primaryTrack();
    int base = primary.isValid() ? doc.tracks().indexOf (primary) : 0;
    if (numTracks() == 0) { session.addTrack(); base = 0; }
    doc.undo().beginNewTransaction ("Paste");
    selection.clear();
    const double at = playhead();
    for (auto& item : clipboard)
    {
        auto t = trackAtIndex (juce::jlimit (0, numTracks() - 1, base + item.trackOffset));
        if (! acceptsClip (t, item.clip)) continue;   // audio / MIDI clips only on their own kind of track
        auto c = item.clip.createCopy();
        c.setProperty (ids::id, newId(), nullptr);
        c.setProperty (ids::start, at + item.timeOffset, nullptr);
        t.appendChild (c, &doc.undo());
        selection.add (c);
    }
    repaintAll();
}

void ArrangeView::selectAll()
{
    selection.clear();
    for (auto t : doc.tracks())
        for (auto c : t)
            if (c.hasType (ids::CLIP)) selection.add (c);
    repaintAll();
}

void ArrangeView::setTrackDropIndex (int idx)
{
    if (dropIndex == idx) return;
    dropIndex = idx;
    repaint();
}

void ArrangeView::paintOverChildren (juce::Graphics& g)
{
    if (dropIndex < 0) return;
    const int y = headerHolder.getY() + geo.topOf (dropIndex) - geo.scrollY;
    g.setColour (palette::accent);
    g.fillRect (0, y - 2, getWidth(), 3);
}

void ArrangeView::moveTracks (juce::Array<juce::ValueTree> moving, int insertIndex)
{
    session.moveTracks (std::move (moving), insertIndex);
}

void ArrangeView::addMarkerAtPlayhead()
{
    doc.undo().beginNewTransaction ("Add marker");
    doc.markers().appendChild (createMarker ("Marker " + juce::String (doc.markers().getNumChildren() + 1), playhead()), &doc.undo());
}

void ArrangeView::zoom (double factor, float aroundX)
{
    const float x = aroundX >= 0.0f ? aroundX : geo.xFor (playhead());
    const double tAt = geo.timeFor (x);
    geo.pixelsPerSecond = juce::jlimit (2.0, 2000.0, geo.pixelsPerSecond * factor);
    geo.scrollX = juce::jmax (0.0, tAt - x / geo.pixelsPerSecond);
    updateScrollBars();
    repaintAll();
}

void ArrangeView::zoomAround (double pps, double time, float x)
{
    geo.pixelsPerSecond = juce::jlimit (2.0, 2000.0, pps);
    geo.scrollX = juce::jmax (0.0, time - x / geo.pixelsPerSecond);
    updateScrollBars();
    repaintAll();
}

void ArrangeView::zoomToFit()
{
    const double end = juce::jmax (songEnd (doc.tracks()), geo.barSec() * 4.0);
    geo.pixelsPerSecond = juce::jlimit (2.0, 2000.0, (canvas.getWidth() - 40) / end);
    geo.scrollX = 0.0;
    updateScrollBars();
    repaintAll();
}
} // namespace ff
