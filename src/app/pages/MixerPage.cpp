#include "MixerPage.h"
#include "core/Automation.h"
#include "core/Timeline.h"
#include "core/ProjectFile.h"
#include "core/MidiClip.h"

namespace ff
{
using namespace timeline;

static void setupFader (juce::Slider& f)
{
    f.setSliderStyle (juce::Slider::LinearVertical);
    f.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
    f.setRange (-60.0, 6.0, 0.1);
    f.setSkewFactorFromMidPoint (-12.0);
    f.setDoubleClickReturnValue (true, 0.0);
    f.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; };
    f.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-0123456789.").getDoubleValue(); };
}

// ------------------------------------------------------------------ strip

ChannelStrip::ChannelStrip (ProjectDocument& d, EngineHost& h, SessionController& sc, juce::ValueTree t, int s)
    : doc (d), host (h), session (sc), track (t), slot (s)
{
    setupFader (fader);
    fader.onDragStart = [this] { doc.undo().beginNewTransaction ("Track volume"); faderPrev = fader.getValue(); };
    fader.onValueChange = [this] {
        if (updating) return;
        const double now = fader.getValue(), delta = now - faderPrev;
        faderPrev = now;
        session.moveTrackVolume (track, now, delta);   // grouped / multi-selection; shifts volume automation
    };
    fader.setTooltip ("Volume. With volume automation the fader follows it, and moving the fader raises or lowers the whole automation.");
    addAndMakeVisible (fader);

    pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pan.setRange (-1.0, 1.0, 0.01);
    pan.getProperties().set ("pan", true);
    pan.textFromValueFunction = [] (double v) { return std::abs (v) < 0.005 ? juce::String ("C") : juce::String (juce::roundToInt (std::abs (v) * 100)) + (v < 0 ? " L" : " R"); };
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setPopupDisplayEnabled (true, true, nullptr);
    pan.onDragStart = [this] { doc.undo().beginNewTransaction ("Track pan"); };
    pan.onValueChange = [this] {
        if (updating) return;
        const double delta = pan.getValue() - (double) track[ids::pan];
        doc.set (track, ids::pan, pan.getValue());
        for (auto other : session.linkedTracks (track))
            if (other != track)
                doc.set (other, ids::pan, juce::jlimit (-1.0, 1.0, (double) other[ids::pan] + delta));
    };
    addAndMakeVisible (pan);

    auto setup = [this] (juce::TextButton& b, juce::Colour c, const juce::Identifier& id) {
        b.setColour (juce::TextButton::buttonOnColourId, c);
        b.setWantsKeyboardFocus (false);
        b.onClick = [this, &id] { session.setOnTracks (track, id, ! (bool) track[id], false); };
        addAndMakeVisible (b);
    };
    setup (mute, palette::accent, ids::mute);
    setup (solo, palette::warn, ids::solo);
    setup (arm, palette::bad, ids::armed);
    mon.onClick = [this] { session.setOnTracks (track, ids::monitor, ! (bool) track[ids::monitor], false); };
    addAndMakeVisible (mon);

    ampButton.setColour (juce::TextButton::buttonOnColourId, palette::accent);
    ampButton.setWantsKeyboardFocus (false);
    ampButton.setTooltip ("Neural Amp Modeler amp on this track: click to add / open it");
    ampButton.onClick = [this] {
        if (! isInstrumentTrack (track)) { session.addTrackRig (track); return; }
        if (track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::desc].toString().isEmpty())
            session.showInstrumentContextMenu (track, &ampButton);
        else session.openInstrument (track);
    };
    ampButton.onRightClick = [this] {
        if (isInstrumentTrack (track)) session.showInstrumentContextMenu (track, &ampButton);   // open / change / remove
    };
    addAndMakeVisible (ampButton);
    ampPower.setTooltip ("Switch the NAM amp on / off");
    ampPower.onClick = [this] {
        auto r = rig::find (track);
        if (! r.isValid()) return;
        doc.undo().beginNewTransaction ("NAM amp on / off");
        r.setProperty (rig::ids::bypass, ! (bool) r[rig::ids::bypass], &doc.undo());
    };
    addChildComponent (ampPower);

    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (uiFont (12.5f, true));
    nameLabel.setColour (juce::Label::textColourId, palette::text);
    nameLabel.setInterceptsMouseClicks (false, false);   // clicks select the strip; double-click renames
    nameLabel.setTooltip ("Double-click to rename");
    nameLabel.onTextChange = [this] {
        const auto n = nameLabel.getText().trim();
        if (n.isEmpty() || n == track[ids::name].toString()) { refresh(); return; }
        doc.undo().beginNewTransaction ("Rename track");
        doc.set (track, ids::name, n);
    };
    addAndMakeVisible (nameLabel);
    routeList.setTooltip ("Sends (drag a send up / down for its level, click for options) and where this track plays into "
                          "(click to route it to a bus, or add sends)");
    addAndMakeVisible (routeList);
    insertList.setTooltip ("Plugins on this track (after its NAM amp, before the fader). Click a plugin to open it, right-click for more.");
    insertView.setViewedComponent (&insertList, false);
    insertView.setScrollBarsShown (true, false);
    insertView.setScrollBarThickness (6);
    addAndMakeVisible (insertView);
    session.rigs().statusChanged.addChangeListener (this);
    track.addListener (this);
    refresh();
}

ChannelStrip::~ChannelStrip()
{
    session.rigs().statusChanged.removeChangeListener (this);
    track.removeListener (this);
}

int InsertList::ampRows() const { return session.trackHasRig (track) ? 1 : 0; }

int InsertList::rowsWanted() const
{
    const int n = rig::numInserts (track);
    return ampRows() + n + (n < rig::maxInserts && session.plugins != nullptr ? 1 : 0);
}

void InsertList::paint (juce::Graphics& g)
{
    const int n = rig::numInserts (track), amp = ampRows();
    if (amp > 0)   // the NAM amp: always the first plugin
    {
        auto r = juce::Rectangle<int> (0, 0, getWidth(), rowHeight - 2).toFloat();
        const bool bypassed = session.ampBypassed (track);
        g.setColour (palette::accent.withAlpha (bypassed ? 0.08f : 0.25f));
        g.fillRoundedRectangle (r, 3.0f);
        auto lamp = r.removeFromRight (18.0f).withSizeKeepingCentre (10.0f, 10.0f);
        g.setColour (bypassed ? palette::well : palette::good);
        g.fillEllipse (lamp);
        g.setColour (bypassed ? palette::textDim : palette::good.brighter (0.3f));
        g.drawEllipse (lamp, 1.0f);
        g.setColour (bypassed ? palette::textDim : palette::text);
        g.setFont (uiFont (11.0f, ! bypassed));
        g.drawText ("NAM Amp", r.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
    }
    for (int row = amp; row < rowsWanted(); ++row)
    {
        const int i = row - amp;
        auto r = juce::Rectangle<int> (0, row * rowHeight, getWidth(), rowHeight - 2).toFloat();
        if (i < n)
        {
            const auto node = rig::insertNode (track, i);
            const auto st = session.rigs().insertStatus (track, i);
            const bool bypassed = (bool) node[rig::ids::bypass];
            g.setColour (st.error.isNotEmpty() ? palette::bad.withAlpha (0.25f) : palette::info.withAlpha (bypassed ? 0.08f : 0.22f));
            g.fillRoundedRectangle (r, 3.0f);
            // on / off button
            auto lamp = r.removeFromRight (18.0f).withSizeKeepingCentre (10.0f, 10.0f);
            g.setColour (bypassed ? palette::well : palette::good);
            g.fillEllipse (lamp);
            g.setColour (bypassed ? palette::textDim : palette::good.brighter (0.3f));
            g.drawEllipse (lamp, 1.0f);
            g.setColour (bypassed ? palette::textDim : palette::text);
            g.setFont (uiFont (11.0f, ! bypassed));
            g.drawText ((st.loading ? "... " : juce::String()) + node[rig::ids::name].toString(), r.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (palette::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (palette::textDim);
            g.setFont (uiFont (11.0f));
            g.drawText ("+ Plugin", r, juce::Justification::centred);
        }
    }
    if (dropRow >= 0)
    {
        g.setColour (palette::accent);
        g.fillRect (0, juce::jmax (0, (dropRow + amp) * rowHeight - 2), getWidth(), 2);
    }
}

void InsertList::mouseDown (const juce::MouseEvent&) { dragging = false; }

void InsertList::mouseDrag (const juce::MouseEvent& e)
{
    const int row = e.getMouseDownY() / rowHeight - ampRows();
    const bool ampRow = row < 0 && ampRows() > 0 && e.getMouseDownY() >= 0;   // the NAM amp: drag it onto another track to copy it
    if (dragging || (row < 0 && ! ampRow) || row >= rig::numInserts (track) || e.getDistanceFromDragStart() < 5) return;
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragging = true;
        const auto rowArea = juce::Rectangle<int> (0, (row + ampRows()) * rowHeight, getWidth(), rowHeight - 2);
        const juce::Point<int> offset (-e.getMouseDownX(), -(e.getMouseDownY() - rowArea.getY()));
        c->startDragging ((ampRow ? "ffamp|" + track[ids::id].toString() : "ffinsert|" + track[ids::id].toString() + "|" + juce::String (row)), this,
                          juce::ScaledImage (createComponentSnapshot (rowArea, true, 2.0f), 2.0), false, &offset);
    }
}

void InsertList::mouseUp (const juce::MouseEvent& e)
{
    if (dragging || ! e.mouseWasClicked()) return;
    const int amp = ampRows(), row = e.y / rowHeight - amp, n = rig::numInserts (track);
    if (row < 0)
    {
        if (e.mods.isPopupMenu()) session.showAmpMenu (track, this);
        else if (onBypassButton (e.x)) session.toggleAmpBypass (track);
        else if (session.onOpenRig) session.onOpenRig (track);
        return;
    }
    if (row < n)
    {
        if (e.mods.isPopupMenu()) session.showInsertMenu (track, row, this);
        else if (onBypassButton (e.x)) session.toggleInsertBypass (track, row);
        else session.openInsert (track, row);
    }
    else if (row + amp < rowsWanted()) session.showAddInsertMenu (track, this);
}

bool InsertList::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString().startsWith ("ffinsert|") || d.description.toString().startsWith ("ffamp|");
}

void InsertList::itemDropped (const SourceDetails& d)
{
    const auto parts = juce::StringArray::fromTokens (d.description.toString(), "|", "");
    if (parts[0] == "ffamp")
    {
        dropRow = -1;
        repaint();
        session.dropAmp (parts[1], track, juce::ModifierKeys::currentModifiers.isShiftDown());   // Shift: move instead of copy
        return;
    }
    const int to = dropRow < 0 ? rig::numInserts (track) : dropRow;
    dropRow = -1;
    repaint();
    if (parts.size() < 3) return;
    session.dropInsert (parts[1], parts[2].getIntValue(), track, to);
}

// ------------------------------------------------------------------ sends and output

juce::ValueTree RouteList::sendAt (int row) const
{
    int k = 0;
    for (auto c : track)
        if (c.hasType (ids::SEND) && k++ == row) return c;
    return {};
}

int RouteList::rowsWanted() const
{
    int n = 1;
    for (auto c : track) n += c.hasType (ids::SEND) ? 1 : 0;
    return n;
}

void RouteList::paint (juce::Graphics& g)
{
    const int n = rowsWanted();
    for (int i = 0; i < n; ++i)
    {
        auto r = juce::Rectangle<int> (0, i * rowHeight, getWidth(), rowHeight - 2).toFloat();
        if (i < n - 1)
        {
            const auto s = sendAt (i);
            const auto dest = findTrack (doc.tracks(), s[ids::target].toString());
            g.setColour (palette::good.withAlpha (0.16f));
            g.fillRoundedRectangle (r, 3.0f);
            // level bar: -36..+6 dB
            const float x = juce::jlimit (0.0f, 1.0f, (float) (((double) s[ids::level] + 36.0) / 42.0));
            g.setColour (palette::good.withAlpha (0.35f));
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * x), 3.0f);
            g.setColour (palette::text);
            g.setFont (uiFont (10.5f, true));
            g.drawText (timeline::isHardwareRoute (s[ids::target].toString()) ? session.routeName (s[ids::target].toString())
                                                                             : dest.isValid() ? dest[ids::name].toString() : juce::String ("?"),
                        r.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
            g.setFont (uiFont (10.0f));
            g.drawText (juce::String ((double) s[ids::level], 1) + ((bool) s[ids::pre] ? "p" : ""), r.reduced (4.0f, 0.0f), juce::Justification::centredRight, false);
        }
        else
        {
            g.setColour (palette::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (outputOf (track).isEmpty() ? palette::textDim : palette::warn);
            g.setFont (uiFont (10.5f));
            g.drawText ("> " + session.outputName (track), r.reduced (4.0f, 0.0f), juce::Justification::centred, true);
        }
    }
}

void RouteList::mouseDown (const juce::MouseEvent& e)
{
    dragged = false;
    dragSend = {};
    const int row = e.y / rowHeight;
    if (row < rowsWanted() - 1 && ! e.mods.isPopupMenu())
    {
        dragSend = sendAt (row);
        dragStartLevel = (double) dragSend[ids::level];
    }
}

void RouteList::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragSend.isValid() || e.getDistanceFromDragStart() < 3) return;
    if (! dragged) { doc.undo().beginNewTransaction ("Send level"); dragged = true; }
    const double v = juce::jlimit (-60.0, 6.0, dragStartLevel - e.getDistanceFromDragStartY() * 0.25 + e.getDistanceFromDragStartX() * 0.25);
    dragSend.setProperty (ids::level, std::round (v * 10.0) / 10.0, &doc.undo());
    repaint();
}

void RouteList::mouseUp (const juce::MouseEvent& e)
{
    if (dragged || ! e.mouseWasClicked()) return;
    const int row = e.y / rowHeight;
    if (row < rowsWanted() - 1) session.showSendMenu (track, sendAt (row), this);
    else if (row < rowsWanted()) session.showRoutingMenu (track, this);
}

void ChannelStrip::refresh()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    fader.setValue (juce::jmax (-60.0, (double) automation::volumeAt (track, host.engine.getPositionSeconds())), juce::dontSendNotification);
    faderPrev = fader.getValue();
    pan.setValue ((double) track[ids::pan], juce::dontSendNotification);
    mute.setToggleState ((bool) track[ids::mute], juce::dontSendNotification);
    solo.setToggleState ((bool) track[ids::solo], juce::dontSendNotification);
    arm.setToggleState ((bool) track[ids::armed], juce::dontSendNotification);
    ampButton.setToggleState (session.trackHasRig (track), juce::dontSendNotification);
    arm.setVisible (isSourceTrack (track));
    mon.setVisible (isSourceTrack (track));
    mon.setToggleState ((bool) track[ids::monitor], juce::dontSendNotification);
    ampButton.setVisible (isInstrumentTrack (track));
    if (isInstrumentTrack (track))
    {
        ampButton.setButtonText (session.instrumentName (track));
        ampButton.setTooltip ("Instrument: click to open it, right-click to change or remove it");
    }
    ampPower.setVisible (false);
    ampPower.setToggleState (! (bool) rig::find (track)[rig::ids::bypass], juce::dontSendNotification);
    routeList.repaint();
    if (! nameLabel.isBeingEdited()) nameLabel.setText (track[ids::name].toString(), juce::dontSendNotification);
    repaint();
}

void ChannelStrip::tick()
{
    if (automation::hasVolumeAutomation (track) && ! fader.isMouseButtonDown())
    {
        const double v = juce::jmax (-60.0, (double) automation::volumeAt (track, host.engine.getPositionSeconds()));
        if (std::abs (v - fader.getValue()) > 0.01) { const juce::ScopedValueSetter<bool> s (updating, true); fader.setValue (v); faderPrev = v; }
    }
    auto& m = host.engine.trackMeters[(size_t) juce::jlimit (0, maxTracks - 1, slot)];
    m.uiTake();
    const float db = gainToDb (m.uiPeak);
    level = db > level ? db : juce::jmax (db, level - 1.6f);
    if (db >= hold) { hold = db; holdTicks = 45; }
    else if (--holdTicks <= 0) hold = juce::jmax (-100.0f, hold - 1.5f);
    if (const auto c = m.clipCount.load(); c != clipSeen) { clipSeen = c; clipTicks = 90; }
    else if (clipTicks > 0) --clipTicks;
    repaint (meterArea());
}

juce::Rectangle<int> ChannelStrip::meterArea() const
{
    // the meter runs exactly along the fader's travel, so its dB marks line up with the fader's
    const int top = fader.getY() + juce::roundToInt (fader.getPositionOfValue (6.0)) - 7;
    const int bottom = fader.getY() + juce::roundToInt (fader.getPositionOfValue (-60.0));
    return { getWidth() - 3 - 16, top, 10, juce::jmax (10, bottom - top) };
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (! session.isTrackSelected (track)) session.clickTrack (track, {});
        showMenu();
        return;
    }
    session.clickTrack (track, e.mods); // click the strip (name area / background) to select; Shift/Ctrl extend
}

void ChannelStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (trackDragging || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 6) return;
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        trackDragging = true;
        if (! session.isTrackSelected (track)) session.clickTrack (track, {});
        c->startDragging ("fftrack|" + track[ids::id].toString(), this,
                          juce::ScaledImage (createComponentSnapshot (getLocalBounds(), true, 1.0f), 1.0), false);
    }
}

void ChannelStrip::itemDragMove (const SourceDetails& d)
{
    if (! d.description.toString().startsWith ("fftrack|")) return;
    const int side = d.localPosition.x < getWidth() / 2 ? -1 : 1;
    if (side != dropSide) { dropSide = side; repaint(); }
}

void ChannelStrip::itemDropped (const SourceDetails& d)
{
    const auto desc = d.description.toString();
    if (! desc.startsWith ("fftrack|")) { insertList.itemDropped (d); return; }
    const int side = d.localPosition.x < getWidth() / 2 ? -1 : 1;
    dropSide = 0;
    repaint();
    auto tracks = doc.tracks();
    juce::ValueTree dragged;
    for (auto t : tracks) if (t[ids::id].toString() == desc.fromFirstOccurrenceOf ("|", false, false)) dragged = t;
    if (! dragged.isValid() || dragged == track) return;
    session.moveTracks (session.linkedTracks (dragged), tracks.indexOf (track) + (side > 0 ? 1 : 0));
}

void ChannelStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < 34) nameLabel.showEditor();
}

void ChannelStrip::showMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Rename...");
    session.addGroupMenu (m, track, 700);
    m.addSeparator();
    if (! isAudioTrack (track)) {}
    else if (session.trackHasRig (track)) { m.addItem (10, "Open NAM amp..."); m.addItem (11, "Remove NAM amp"); }
    else m.addItem (10, "Add NAM amp...");
    m.addItem (12, "Add plugin...", session.plugins != nullptr && rig::numInserts (track) < rig::maxInserts);
    m.addItem (13, "Output and sends...");
    if (isAudioTrack (track)) m.addItem (18, "Stereo track (records an input pair)", true, (bool) track[ids::inputStereo]);
    if (isInstrumentTrack (track))
    {
        const bool hasInstrument = track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::desc].toString().isNotEmpty();
        m.addItem (14, hasInstrument ? "Change instrument  (now: " + session.instrumentName (track) + ")..." : juce::String ("Choose instrument..."));
        m.addItem (15, "Open instrument", hasInstrument);
        m.addItem (19, "Remove instrument", hasInstrument);
    }
    m.addSeparator();
    m.addItem (4, "Duplicate track");
    m.addItem (5, "Delete track");
    juce::Component::SafePointer<ChannelStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        auto t = safe->track;
        auto& sc = safe->session;
        if (sc.handleGroupMenu (r, t, 700)) return;
        if (r == 1) safe->nameLabel.showEditor();
        else if (r == 10) sc.addTrackRig (t);
        else if (r == 12) sc.showAddInsertMenu (t, safe.getComponent());
        else if (r == 13) sc.showRoutingMenu (t, safe.getComponent());
        else if (r == 14) sc.showInstrumentMenu (t, safe.getComponent());
        else if (r == 15) sc.openInstrument (t);
        else if (r == 18) sc.setTrackStereo (t, ! (bool) t[ids::inputStereo]);
        else if (r == 19) sc.removeInstrument (t);
        else if (r == 11) sc.removeTrackRig (t);
        else if (r == 4) sc.duplicateTrack (t);
        else if (r == 5) sc.deleteTrack (t);   // destroys this strip
    });
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (3, 0);
    const bool selected = session.isTrackSelected (track);
    g.setColour (selected ? palette::panelHi : palette::panel);
    g.fillRoundedRectangle (r.toFloat(), 5.0f);
    if (selected)
    {
        g.setColour (palette::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);
    }
    if (dropSide != 0)
    {
        g.setColour (palette::accent);
        g.fillRect (dropSide < 0 ? 0 : getWidth() - 3, 0, 3, getHeight());
    }
    const auto colour = juce::Colour::fromString (track[ids::colour].toString());
    g.setColour (colour);
    g.fillRoundedRectangle (r.removeFromTop (6).toFloat(), 3.0f);

    drawLevelMeter (g, meterArea(), level, hold, clipTicks > 0, true);
    g.setColour (palette::textDim);
    g.setFont (uiFont (10.0f));
    g.drawText ("PAN", getLocalBounds().withTrimmedTop (30).withHeight (14), juce::Justification::centred);
    if (const auto grp = track[ids::group].toString(); grp.isNotEmpty())
    {
        const auto gc = SessionController::groupColour (grp);
        g.setColour (gc);
        g.fillRect (getLocalBounds().reduced (3, 0).withTrimmedTop (8).removeFromLeft (3));
        g.setFont (uiFont (9.5f, true));
        g.drawText (grp, getLocalBounds().reduced (6, 0).withTrimmedTop (30).withHeight (14).removeFromRight (34),
                    juce::Justification::centredRight, true);
    }
    if (! isAudioTrack (track))
    {
        g.setColour (kindOf (track) == TrackKind::fx ? palette::good : palette::info);
        g.setFont (uiFont (9.5f, true));
        g.drawText (kindOf (track) == TrackKind::fx ? "FX" : "BUS", getLocalBounds().reduced (8, 0).withTrimmedTop (30).withHeight (14),
                    juce::Justification::centredLeft);
    }
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (8, 0).withTrimmedTop (44);
    pan.setBounds (r.removeFromTop (40).withSizeKeepingCentre (40, 40));
    r.removeFromTop (6);
    auto buttons = r.removeFromTop (24);
    const int w = buttons.getWidth() / (isSourceTrack (track) ? 4 : 2);
    if (isSourceTrack (track)) arm.setBounds (buttons.removeFromLeft (w).reduced (1));
    if (isSourceTrack (track)) mon.setBounds (buttons.removeFromLeft (w).reduced (1));
    mute.setBounds (buttons.removeFromLeft (w).reduced (1));
    solo.setBounds (buttons.reduced (1));
    if (isInstrumentTrack (track))   // the instrument (an audio track's NAM amp is in its plugin list)
    {
        auto row = r.removeFromTop (30).reduced (1, 4);
        ampButton.setBounds (row);
    }
    else r.removeFromTop (6);
    // plugins take the room left above the sends and the fader (which starts where the mixer's divider is); they scroll if
    // they do not fit
    const int insRows = insertList.rowsWanted(), routesH = routeList.rowsWanted() * RouteList::rowHeight;
    const int roomBottom = juce::jmax (r.getY(), (alignTop > 0 ? alignTop : getHeight()) - 6 - routesH - (insRows > 0 ? 6 : 0));
    const int insH = juce::jmin (insRows * InsertList::rowHeight, roomBottom - r.getY());
    insertView.setBounds (r.removeFromTop (juce::jmax (0, insH)));
    const bool scrolls = insRows * InsertList::rowHeight > insH;
    insertList.setSize (insertView.getWidth() - (scrolls ? insertView.getScrollBarThickness() + 1 : 0), insRows * InsertList::rowHeight);
    r.removeFromTop (insRows > 0 ? 6 : 0);
    routeList.setBounds (r.removeFromTop (routesH));
    r.removeFromTop (6);
    nameLabel.setBounds (getLocalBounds().reduced (5, 0).withTrimmedTop (6).withHeight (24));
    r.removeFromBottom (6);
    r.setTop (juce::jmax (r.getY(), alignTop));   // every strip's fader starts at the same height
    fader.setBounds (r.withTrimmedRight (16));
}

int MixerPage::autoFaderTop() const
{
    int top = 0;
    for (auto* st : strips) top = juce::jmax (top, st->wantedFaderTop());
    return top;
}

void MixerPage::setFaderTop (int top)
{
    settings.setValue ("mixerFaderTop", top < 0 ? -1 : top);
    resized();
}

void MixerPage::FaderDivider::paint (juce::Graphics& g)
{
    const bool hot = isMouseOverOrDragging();
    auto r = getLocalBounds().toFloat().reduced (0.0f, 2.5f);
    g.setColour ((hot ? palette::accent : palette::outline.brighter (0.3f)).withAlpha (hot ? 0.9f : 0.7f));
    g.fillRoundedRectangle (r, 1.5f);
    g.setColour (palette::bg);
    for (float x = 40.0f; x < (float) getWidth(); x += (float) ChannelStrip::width)   // a grip in every strip
        for (int k = -1; k <= 1; ++k)
            g.fillEllipse (x + (float) k * 6.0f + 10.0f, r.getCentreY() - 1.0f, 2.0f, 2.0f);
}

int ChannelStrip::wantedFaderTop() const
{
    const int ins = insertList.rowsWanted(), routes = routeList.rowsWanted();
    return 44 + 40 + 6 + 24 + (isInstrumentTrack (track) ? 30 : 6) + ins * InsertList::rowHeight + (ins > 0 ? 6 : 0)
           + routes * RouteList::rowHeight + 6;
}

// ------------------------------------------------------------------ page

MixerPage::MixerPage (EngineHost& h, ProjectDocument& d, SessionController& sc, juce::PropertiesFile& s)
    : host (h), doc (d), session (sc), settings (s), meterL (h.engine.outputMeterL), meterR (h.engine.outputMeterR)
{
    setupFader (fader);
    fader.onValueChange = [this] {
        const double db = fader.getValue();
        host.engine.masterGain = db <= -59.95 ? 0.0f : juce::Decibels::decibelsToGain ((float) db);
        settings.setValue ("mainOutDb", db);
    };
    fader.setTooltip ("Main output level (monitoring level; not part of exports)");
    fader.setValue (settings.getDoubleValue ("mainOutDb", 0.0), juce::sendNotificationSync);
    addAndMakeVisible (fader);
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);

    emptyLabel.setText ("No tracks yet. Click \"+ Track\" (or add tracks in the Editor) and they appear here as channel strips.", juce::dontSendNotification);
    addTrackButton.setTooltip ("Add an audio, bus or FX track");
    addTrackButton.onClick = [this] { session.showAddTrackMenu (&addTrackButton); };
    addAndMakeVisible (addTrackButton);
    styleLabel (emptyLabel, 14.0f);
    emptyLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (emptyLabel);
    session.selectionChanged.addChangeListener (this);
    rebind();
}

MixerPage::~MixerPage()
{
    session.selectionChanged.removeChangeListener (this);
    tracksNode.removeListener (this);
}

void MixerPage::rebind()
{
    tracksNode.removeListener (this);
    tracksNode = doc.tracks();
    tracksNode.addListener (this);
    masterInserts = std::make_unique<InsertList> (session, project::masterNode (doc.tree()));
    masterInserts->setTooltip ("Plugins on the master bus (they process the whole mix, and are in exports). "
                               "Click a plugin to open it, right-click for more.");
    masterInsertView.setViewedComponent (masterInserts.get(), false);
    masterInsertView.setScrollBarsShown (true, false);
    masterInsertView.setScrollBarThickness (6);
    addAndMakeVisible (masterInsertView);
    rebuild();
}

void MixerPage::rebuild()
{
    strips.clear();
    int i = 0;
    for (auto t : tracksNode)
    {
        if (! t.hasType (ids::TRACK) || i >= maxTracks) continue;
        auto* s = strips.add (new ChannelStrip (doc, host, session, t, i++));
        stripHolder.addAndMakeVisible (s);
    }
    stripHolder.addChildComponent (divider);
    emptyLabel.setVisible (strips.isEmpty());
    resized();
}

void MixerPage::timerCallback()
{
    meterL.tick();
    meterR.tick();
    {
        const int top = autoFaderTop();
        if (top != lastAutoTop) { lastAutoTop = top; resized(); }   // a strip gained or lost a plugin / send row
    }
    if (masterInserts != nullptr)
    {
        if (masterInserts->rowsWanted() != masterRows) resized();
        masterInserts->repaint();
    }
    for (auto* s : strips)
        s->tick();
}

void MixerPage::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    drawSectionPanel (g, masterBox, "Main out");
    g.setFont (uiFont (10.0f));
    for (int db : { 6, 0, -6, -12, -18, -24, -36, -60 })   // at the fader's own positions
    {
        const int y = fader.getY() + juce::roundToInt (fader.getPositionOfValue ((double) db));
        g.setColour (db == 0 ? palette::text : palette::textDim);
        g.drawText (db > 0 ? "+" + juce::String (db) : juce::String (db), scale.getX(), y - 6, scale.getWidth(), 12, juce::Justification::centredRight);
    }
}

void MixerPage::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto top = r.removeFromTop (30);
    addTrackButton.setBounds (top.removeFromLeft (100));
    r.removeFromTop (8);
    masterBox = r.removeFromRight (150);
    r.removeFromRight (12);
    viewport.setBounds (r);
    emptyLabel.setBounds (r);
    stripHolder.setSize (juce::jmax (r.getWidth(), strips.size() * ChannelStrip::width), r.getHeight() - viewport.getScrollBarThickness());
    const int stored = settings.getIntValue ("mixerFaderTop", -1);
    const int lowest = juce::jmax (150, stripHolder.getHeight() - 140);   // a fader is always at least ~140 px long
    faderTop = juce::jlimit (150, lowest, stored >= 0 ? stored : autoFaderTop());
    for (auto* st : strips) st->alignTop = faderTop;
    for (int i = 0; i < strips.size(); ++i)
    {
        strips[i]->setBounds (i * ChannelStrip::width, 0, ChannelStrip::width, stripHolder.getHeight());
        strips[i]->resized();
    }
    divider.setBounds (0, faderTop - 9, strips.size() * ChannelStrip::width, 8);
    divider.setVisible (! strips.isEmpty());
    divider.toFront (false);

    auto s = masterBox.reduced (12).withTrimmedTop (24).withTrimmedBottom (8);
    if (masterInserts != nullptr)
    {
        masterRows = masterInserts->rowsWanted();
        const int want = masterRows * InsertList::rowHeight, room = juce::jmax (InsertList::rowHeight, s.getHeight() - 220);
        masterInsertView.setBounds (s.removeFromTop (juce::jmin (want, room)));
        masterInserts->setSize (masterInsertView.getWidth() - (want > room ? masterInsertView.getScrollBarThickness() + 1 : 0), want);
        s.removeFromTop (8);
    }
    auto meters = s.removeFromRight (34);
    scale = s.removeFromRight (28);
    fader.setBounds (s);
    // meters along the fader's travel (+6 at the top, -60 at the bottom), 7 px above for the clip LED
    const int mTop = fader.getY() + juce::roundToInt (fader.getPositionOfValue (6.0)) - 7;
    const int mBottom = fader.getY() + juce::roundToInt (fader.getPositionOfValue (-60.0));
    meterL.setBounds (meters.getX(), mTop, 15, mBottom - mTop);
    meterR.setBounds (meters.getX() + 19, mTop, 15, mBottom - mTop);
}
} // namespace ff
