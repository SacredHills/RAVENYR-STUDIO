#include "SessionController.h"
#include "core/DrumModel.h"
#include "plugins/PluginManager.h"
#include "engine/fx/BuiltinPlugins.h"
#include "core/MidiClip.h"
#include "core/Automation.h"

namespace ff
{
using namespace timeline;

SessionController::SessionController (EngineHost& h, ProjectDocument& d, juce::PropertiesFile& s)
    : host (h), doc (d), settings (s)
{
    cache.onLoaded = [this] {
        triggerAsyncUpdate();
        if (onArrangementChanged) onArrangementChanged();
    };
    cache.onError = [this] (const juce::String& e) { if (onError) onError ("Audio file problem", e); };
    rigBank = std::make_unique<fx::RigBank> (host.engine);
    if (auto xml = settings.getXmlValue ("liveRig"))
    {
        auto t = juce::ValueTree::fromXml (*xml);
        t.setProperty (rig::ids::enabled, false, nullptr);   // live monitoring always starts off (no surprise feedback)
        rigBank->setLiveTree (t);
    }
    rigBank->onRigsChanged = [this] { triggerAsyncUpdate(); };
    rigBank->onLiveChanged = [this] {
        auto t = rigBank->liveTree().createCopy();
        t.setProperty (rig::ids::enabled, false, nullptr);
        if (auto xml = t.createXml()) settings.setValue ("liveRig", xml.get());
    };
    rebind();
    startTimerHz (30);
}

SessionController::~SessionController()
{
    stopTimer();
    root.removeListener (this);
    if (sessionActive)
    {
        host.engine.stop();
        for (auto& [id, r] : recorders)
        {
            diskWriter.remove (r);
            r->finish();
        }
    }
}

void SessionController::rebind()
{
    root.removeListener (this);
    root = doc.tree();
    root.addListener (this);
    rigBank->rebind (root);
    selectedTracks.clear();
    selectionAnchor = {};
    if (auto first = doc.tracks().getChild (0); first.isValid())
        setSelectedTracks ({ first });
    selectionChanged.sendChangeMessage();
    rebuildGraphNow();
}

void SessionController::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (ids::TRACK) && selectedTracks.contains (child))
    {
        selectedTracks.removeAllInstancesOf (child);
        if (selectionAnchor == child) selectionAnchor = selectedTracks.getFirst();
        selectionChanged.sendChangeMessage();
    }
    triggerAsyncUpdate();
}

// ------------------------------------------------------------------ selection

void SessionController::setSelectedTracks (const juce::Array<juce::ValueTree>& tracks)
{
    selectedTracks = tracks;
    if (! selectedTracks.contains (selectionAnchor))
        selectionAnchor = selectedTracks.getFirst();
    selectionChanged.sendChangeMessage();
}

void SessionController::selectTrackRange (const juce::ValueTree& from, const juce::ValueTree& to)
{
    auto tracks = doc.tracks();
    int a = tracks.indexOf (from), b = tracks.indexOf (to);
    if (a < 0) a = b;
    if (b < 0) return;
    juce::Array<juce::ValueTree> range;
    for (int i = juce::jmin (a, b); i <= juce::jmax (a, b); ++i)
        range.add (tracks.getChild (i));
    selectedTracks = range;
    selectionAnchor = from.isValid() ? from : to;
    selectionChanged.sendChangeMessage();
}

void SessionController::clickTrack (const juce::ValueTree& t, juce::ModifierKeys mods)
{
    if (! t.isValid()) return;
    if (mods.isShiftDown() && selectionAnchor.isValid())
    {
        const auto anchor = selectionAnchor;
        selectTrackRange (anchor, t);
        selectionAnchor = anchor;
        return;
    }
    if (mods.isCommandDown() || mods.isCtrlDown())
    {
        if (selectedTracks.contains (t)) selectedTracks.removeAllInstancesOf (t);
        else selectedTracks.add (t);
        selectionAnchor = t;
        selectionChanged.sendChangeMessage();
        return;
    }
    if (selectedTracks.size() == 1 && selectedTracks[0] == t) return;
    selectedTracks = { t };
    selectionAnchor = t;
    selectionChanged.sendChangeMessage();
}

juce::Array<juce::ValueTree> SessionController::tracksForEdit (const juce::ValueTree& clicked) const
{
    if (selectedTracks.contains (clicked) && selectedTracks.size() > 1)
        return selectedTracks;
    return { clicked };
}

juce::ValueTree SessionController::primaryTrack()
{
    for (auto t : doc.tracks())
        if (selectedTracks.contains (t))
            return t;
    return {};
}

void SessionController::moveTracks (juce::Array<juce::ValueTree> moving, int insertIndex)
{
    auto tracks = doc.tracks();
    for (int k = 0, n = moving.size(); k < n; ++k)   // a folder takes its tracks along
        if (isFolder (moving[k]))
            for (auto t : tracks)
                if (folderIdOf (t) == moving[k][ids::id].toString()) moving.addIfNotAlreadyThere (t);
    std::sort (moving.begin(), moving.end(), [&] (const juce::ValueTree& a, const juce::ValueTree& b) {
        return tracks.indexOf (a) < tracks.indexOf (b);
    });
    // desired order: the untouched tracks with the moved block inserted at the drop point
    juce::Array<juce::ValueTree> order;
    int insertAt = insertIndex;
    for (int i = 0; i < tracks.getNumChildren(); ++i)
    {
        auto t = tracks.getChild (i);
        if (moving.contains (t)) { if (i < insertIndex) --insertAt; continue; }
        order.add (t);
    }
    for (int i = 0; i < moving.size(); ++i)
        order.insert (insertAt + i, moving[i]);

    bool changed = false;
    for (int i = 0; i < order.size(); ++i)
        changed = changed || tracks.indexOf (order[i]) != i;
    if (! changed) return;

    doc.undo().beginNewTransaction (moving.size() > 1 ? "Move tracks" : "Move track");
    for (int i = 0; i < order.size(); ++i)
    {
        const int from = tracks.indexOf (order[i]);
        if (from != i) tracks.moveChild (from, i, &doc.undo());
    }
}

void SessionController::moveTrackVolume (const juce::ValueTree& clicked, double now, double delta)
{
    const auto store = [this] (juce::ValueTree t, double db) { doc.set (t, ids::gainDb, db <= -59.95 ? -96.0 : juce::jmin (6.0, db)); };
    for (auto t : linkedTracks (clicked))
    {
        const bool automated = automation::hasVolumeAutomation (t);
        if (automated) automation::offsetLane (t, automation::volumeParam, (float) delta, -60.0f, 6.0f, &doc.undo());
        if (t == clicked && ! automated) store (t, now);
        else if (std::abs (delta) > 1.0e-9) store (t, juce::jmax (-60.0, (double) t[ids::gainDb]) + delta);
    }
}

juce::Array<juce::ValueTree> SessionController::linkedTracks (const juce::ValueTree& clicked) const
{
    auto list = tracksForEdit (clicked);
    if (juce::ModifierKeys::currentModifiers.isAltDown()) return list;   // Alt: just this one
    juce::StringArray groups;
    for (auto& t : list)
        if (t[ids::group].toString().isNotEmpty()) groups.addIfNotAlreadyThere (t[ids::group].toString());
    if (groups.isEmpty()) return list;
    for (auto t : doc.tracks())
        if (groups.contains (t[ids::group].toString()) && ! list.contains (t)) list.add (t);
    return list;
}

juce::StringArray SessionController::groupNames() const
{
    juce::StringArray g;
    for (auto t : doc.tracks())
        if (t[ids::group].toString().isNotEmpty()) g.addIfNotAlreadyThere (t[ids::group].toString());
    return g;
}

juce::Colour SessionController::groupColour (const juce::String& group)
{
    const auto& p = trackPalette();
    return juce::Colour::fromString (p[(int) ((juce::uint32) group.hashCode() % (juce::uint32) p.size())]);
}

void SessionController::addGroupMenu (juce::PopupMenu& menu, const juce::ValueTree& track, int baseId) const
{
    juce::PopupMenu g;
    const auto current = track[ids::group].toString();
    const int n = tracksForEdit (track).size();
    g.addItem (baseId, n > 1 ? "New group from the " + juce::String (n) + " selected tracks..." : juce::String ("New group..."));
    const auto names = groupNames();
    if (! names.isEmpty()) g.addSeparator();
    for (int i = 0; i < names.size() && i < 50; ++i)
        g.addColouredItem (baseId + 10 + i, (n > 1 ? "Add selected to " : "Add to ") + names[i], groupColour (names[i]), true, current == names[i]);
    g.addSeparator();
    g.addItem (baseId + 1, "Remove from group", current.isNotEmpty());
    g.addItem (baseId + 2, "Select the group's tracks", current.isNotEmpty());
    menu.addSubMenu ("Group" + (current.isNotEmpty() ? "  (" + current + ")" : juce::String()), g);
}

bool SessionController::handleGroupMenu (int r, const juce::ValueTree& track, int baseId)
{
    if (r < baseId || r >= baseId + 60) return false;
    const auto targets = tracksForEdit (track);
    auto setGroup = [this, targets] (const juce::String& name) {
        doc.undo().beginNewTransaction (name.isEmpty() ? "Ungroup tracks" : "Group tracks");
        for (auto t : targets) t.setProperty (ids::group, name, &doc.undo());
    };
    if (r == baseId)
    {
        auto* w = new juce::AlertWindow ("New group", "Tracks in a group move together: volume, pan, mute, solo, record. "
                                                      "Hold Alt to change one of them on its own.", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "Group " + juce::String (groupNames().size() + 1));
        w->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w, setGroup] (int res) {
            const auto name = w->getTextEditorContents ("name").trim();
            if (res == 1 && name.isNotEmpty()) setGroup (name);
        }), true);
    }
    else if (r == baseId + 1) setGroup ({});
    else if (r == baseId + 2)
    {
        juce::Array<juce::ValueTree> members;
        for (auto t : doc.tracks()) if (t[ids::group] == track[ids::group]) members.add (t);
        setSelectedTracks (members);
    }
    else if (const auto names = groupNames(); r - baseId - 10 < names.size()) setGroup (names[r - baseId - 10]);
    return true;
}

void SessionController::setOnTracks (const juce::ValueTree& clicked, const juce::Identifier& id, const juce::var& value,
                                     bool undoable, const juce::String& undoName)
{
    if (undoable) doc.undo().beginNewTransaction (undoName);
    for (auto t : linkedTracks (clicked))
        doc.set (t, id, value, undoable);
}

void SessionController::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& prop)
{
    if (prop.toString() == "laneHeight" || prop == automation::showLane) return;   // view only
    if (node.hasType (ids::TRACK) || node.hasType (ids::CLIP) || node.hasType (ids::SEND) || node.hasType (rig::ids::RIG)
        || node.hasType (automation::LANE) || node.hasType (midiclip::MIDI)
        || (node.hasType (rig::ids::PLUGIN) && prop == rig::ids::sidechain))
        triggerAsyncUpdate();
}

void SessionController::addTrackRig (juce::ValueTree track)
{
    if (! track.isValid() || ! isAudioTrack (track)) return;   // NAM amps belong on audio tracks
    auto r = rig::find (track);
    if (! rig::isEnabled (r))
    {
        doc.undo().beginNewTransaction ("Add NAM amp");
        if (! r.isValid())
        {
            auto fresh = rig::makeDefault();
            rig::set (fresh, rig::calLevel, (float) settings.getDoubleValue ("namInterfaceDbu", 12.0));   // the user's interface
            track.appendChild (fresh, &doc.undo());
        }
        else r.setProperty (rig::ids::enabled, true, &doc.undo());
    }
    if (onOpenRig) onOpenRig (track);
}

void SessionController::removeTrackRig (juce::ValueTree track)
{
    auto r = rig::find (track);
    if (! r.isValid()) return;
    doc.undo().beginNewTransaction ("Remove NAM amp");
    track.removeChild (r, &doc.undo());
}

void SessionController::toggleAmpBypass (juce::ValueTree track)
{
    auto r = rig::find (track);
    if (! r.isValid()) return;
    doc.undo().beginNewTransaction ("NAM amp on / off");
    r.setProperty (rig::ids::bypass, ! (bool) r[rig::ids::bypass], &doc.undo());
}

bool SessionController::ampBypassed (const juce::ValueTree& track) const
{
    return (bool) rig::find (track)[rig::ids::bypass];
}

void SessionController::showAmpMenu (juce::ValueTree track, juce::Component* target)
{
    if (! trackHasRig (track)) return;
    juce::PopupMenu m;
    m.addSectionHeader ("NAM Amp (Neural Amp Modeler)");
    m.addItem (1, "Open amp window");
    m.addItem (2, "Bypass", true, ampBypassed (track));
    m.addItem (3, "Remove");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track] (int r) {
        if (r == 1 && onOpenRig) onOpenRig (track);
        else if (r == 2) toggleAmpBypass (track);
        else if (r == 3) removeTrackRig (track);
    });
}

void SessionController::showAddInsertMenu (juce::ValueTree track, juce::Component* target)
{
    if (plugins == nullptr || ! track.isValid()) return;
    if (rig::numInserts (track) >= rig::maxInserts)
    {
        if (onError) onError ("Plugins", "This track already has the maximum of " + juce::String (rig::maxInserts) + " plugins.");
        return;
    }
    juce::PopupMenu m;
    if (! plugins->clipboard.empty() && plugins->clipboard.descXml.isNotEmpty())
    {
        m.addItem (-3, "Paste plugin: " + plugins->clipboard.name);
        m.addSeparator();
    }
    plugins->addToMenu (m, 1);
    m.addSeparator();
    m.addItem (-2, "Scan for plugins / manage...");
    juce::Component::SafePointer<juce::Component> safeTarget (target);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track, safeTarget] (int r) {
        if (r == 0) return;
        if (r == -2) { plugins->showManager(); return; }
        if (r == -3)
        {
            const auto c = plugins->clipboard;
            doc.undo().beginNewTransaction ("Paste plugin " + c.name);
            const int at = rig::addInsert (track, c.descXml, c.name, &doc.undo(), c.state);
            if (at >= 0) showNewInsert (track, at, c.descXml);
            return;
        }
        juce::String name;
        const auto desc = plugins->descriptionForMenuResult (r, 1, name);
        if (desc.isEmpty()) return;
        doc.undo().beginNewTransaction ("Add plugin " + name);
        const int slot = rig::addInsert (track, desc, name, &doc.undo());
        if (slot < 0) return;
        showNewInsert (track, slot, desc);
    });
}

void SessionController::showNewInsert (juce::ValueTree track, int slot, const juce::String& descXml)
{
    // a built-in plugin expands in the Mixer page's rack when that is on screen; everything else (and a built-in added where there is
    // no rack) opens its own window once it has loaded
    if (ff::fx::builtin::hasRackPanelForDescription (descXml) && onBuiltInInsertAdded && onBuiltInInsertAdded (track, slot)) return;
    openInsertWhenLoaded (track, slot, 40);
}

void SessionController::openInsertWhenLoaded (juce::ValueTree track, int slot, int triesLeft)
{
    const auto st = rigBank->insertStatus (track, slot);
    if (st.loaded) { openInsert (track, slot); return; }
    if (st.error.isNotEmpty()) { if (onError) onError ("Plugin", st.name + " could not be loaded: " + st.error); return; }
    if (triesLeft > 0)
        juce::Timer::callAfterDelay (250, [this, track, slot, triesLeft] { openInsertWhenLoaded (track, slot, triesLeft - 1); });
}

static void addMuteSoloImpl (SessionController* self, PluginManager::BypassControl& bc, juce::ValueTree track)
{
    auto setOnTracks = [self] (const juce::ValueTree& t, const juce::Identifier& id, const juce::var& v, bool u) { self->setOnTracks (t, id, v, u); };
    bc.isMuted = [track] { return (bool) track[ids::mute]; };
    bc.isSoloed = [track] { return (bool) track[ids::solo]; };
    bc.toggleMute = [setOnTracks, track] { setOnTracks (track, ids::mute, ! (bool) track[ids::mute], false); };
    bc.toggleSolo = [setOnTracks, track] { setOnTracks (track, ids::solo, ! (bool) track[ids::solo], false); };
}

namespace
{
/** A plugin's state going from one block to another; undo / redo set it back on the live plugin. */
struct PluginStateAction final : public juce::UndoableAction
{
    PluginStateAction (std::weak_ptr<juce::AudioPluginInstance> p, juce::ValueTree n, juce::MemoryBlock b, juce::MemoryBlock a)
        : plugin (std::move (p)), node (n), before (std::move (b)), after (std::move (a)) {}
    bool perform() override { const bool fresh = first; first = false; return apply (after, ! fresh); }   // the first run is the edit itself
    bool undo() override { return apply (before, true); }
    int getSizeInUnits() override { return (int) (before.getSize() + after.getSize()) / 64 + 1; }
    bool apply (const juce::MemoryBlock& m, bool bump)
    {
        if (bump)
            if (auto p = plugin.lock()) p->setStateInformation (m.getData(), (int) m.getSize());
        if (node.isValid()) node.setProperty ("rev", (int) node.getProperty ("rev", 0) + 1, nullptr);   // marks the project changed
        return true;
    }
    std::weak_ptr<juce::AudioPluginInstance> plugin;
    juce::ValueTree node;
    juce::MemoryBlock before, after;
    bool first = true;
};
}

void SessionController::recordPluginEdit (juce::ValueTree track, int slot, std::shared_ptr<juce::AudioPluginInstance> plugin,
                                          const juce::MemoryBlock& before, const juce::MemoryBlock& after)
{
    if (plugin == nullptr) return;
    const auto now = juce::Time::getMillisecondCounter();
    if (lastPluginEdited != plugin.get() || now - lastPluginEditMs > 500)
        doc.undo().beginNewTransaction ("Edit " + rig::insertNode (track, slot)[rig::ids::name].toString());
    lastPluginEdited = plugin.get();
    lastPluginEditMs = now;
    doc.undo().perform (new PluginStateAction (plugin, rig::insertNode (track, slot), before, after));
}

void SessionController::openInsert (juce::ValueTree track, int slot)
{
    if (plugins == nullptr) return;
    if (auto p = rigBank->insertPlugin (track, slot))
    {
        PluginManager::BypassControl bc;
        bc.isBypassed = [this, track, slot] { return (bool) rig::insertNode (track, slot)[rig::ids::bypass]; };
        bc.toggle = [this, track, slot] { toggleInsertBypass (track, slot); };
        if (track[ids::id].toString() != project::masterId) addMuteSoloImpl (this, bc, track);
        plugins->showEditor (p, rig::insertNode (track, slot)[rig::ids::name].toString() + "  (" + track[ids::name].toString() + ")", bc);
    }
}

void SessionController::toggleInsertBypass (juce::ValueTree track, int slot)
{
    auto n = rig::insertNode (track, slot);
    if (! n.isValid()) return;
    doc.undo().beginNewTransaction ((bool) n[rig::ids::bypass] ? "Enable plugin" : "Bypass plugin");
    n.setProperty (rig::ids::bypass, ! (bool) n[rig::ids::bypass], &doc.undo());
}

juce::ValueTree SessionController::trackOrMaster (const juce::String& id)
{
    if (id == project::masterId) return project::masterNode (doc.tree());
    return findTrack (id);
}

void SessionController::dropInsert (const juce::String& fromTrackId, int fromSlot, juce::ValueTree to, int toSlot)
{
    auto from = trackOrMaster (fromTrackId);
    const auto node = rig::insertNode (from, fromSlot);
    if (! node.isValid() || ! to.isValid()) return;
    if (from == to)
    {
        int at = fromSlot, target = juce::jlimit (0, rig::numInserts (to) - 1, toSlot > fromSlot ? toSlot - 1 : toSlot);
        if (at == target) return;
        doc.undo().beginNewTransaction ("Move plugin");
        while (at != target) { rig::moveInsert (to, at, target > at ? 1 : -1, &doc.undo()); at += target > at ? 1 : -1; }
        return;
    }
    if (rig::numInserts (to) >= rig::maxInserts)
    {
        if (onError) onError ("Plugins", "That track already has " + juce::String (rig::maxInserts) + " plugins.");
        return;
    }
    juce::String state = node[rig::ids::state].toString();
    if (auto p = rigBank->insertPlugin (from, fromSlot)) state = fx::capturePluginState (*p);   // its settings right now
    doc.undo().beginNewTransaction ("Copy plugin " + node[rig::ids::name].toString());
    int at = rig::addInsert (to, node[rig::ids::desc].toString(), node[rig::ids::name].toString(), &doc.undo(), state,
                             (bool) node[rig::ids::bypass]);
    while (at > toSlot && at > 0) { rig::moveInsert (to, at, -1, &doc.undo()); --at; }
}

void SessionController::dropAmp (const juce::String& fromTrackId, juce::ValueTree to, bool move)
{
    auto from = trackOrMaster (fromTrackId);
    auto src = rig::find (from);
    if (! src.isValid() || ! to.isValid() || from == to) return;
    if (! isAudioTrack (to))
    {
        if (onError) onError ("NAM amp", "A NAM amp can only go on an audio track.");
        return;
    }
    doc.undo().beginNewTransaction (move ? "Move NAM amp" : "Copy NAM amp");
    auto copy = src.createCopy();
    copy.setProperty (rig::ids::enabled, true, nullptr);
    if (auto old = rig::find (to); old.isValid()) to.removeChild (old, &doc.undo());
    to.appendChild (copy, &doc.undo());
    if (move) from.removeChild (src, &doc.undo());
}

void SessionController::showInsertMenu (juce::ValueTree track, int slot, juce::Component* target)
{
    const auto node = rig::insertNode (track, slot);
    if (! node.isValid()) return;
    const auto st = rigBank->insertStatus (track, slot);
    // the plugin list is a submenu (its own item ids); the actions use ids well above it
    enum { openId = 200001, bypassId, upId, downId, removeId, sidechainNone, sidechainBase = 300000 };
    juce::PopupMenu m, replace, side;
    m.addSectionHeader (node[rig::ids::name].toString() + (st.error.isNotEmpty() ? "  (not loaded)" : juce::String()));
    m.addItem (openId, "Open plugin window", st.loaded);
    m.addItem (bypassId, "Bypass", true, (bool) node[rig::ids::bypass]);
    if (plugins != nullptr)
    {
        plugins->addToMenu (replace, 1);
        m.addSubMenu ("Replace with", replace);
    }
    {   // side-chain: the plugin's side-chain input listens to another track (its sound after its plugins, before its fader)
        const auto current = node[rig::ids::sidechain].toString();
        side.addItem (sidechainNone, "None", true, current.isEmpty());
        side.addSeparator();
        int k = 0;
        for (auto t : doc.tracks())
        {
            if (t.hasType (ids::TRACK) && t[ids::id] != track[ids::id])
                side.addItem (sidechainBase + k, t[ids::name].toString(), true, current == t[ids::id].toString());
            ++k;
        }
        juce::String sideName = "Side-chain from";
        if (current.isNotEmpty())
            for (auto t : doc.tracks())
                if (t[ids::id].toString() == current) sideName += ": " + t[ids::name].toString();
        m.addSubMenu (sideName, side);
    }
    m.addItem (removeId, "Remove");
    m.addSeparator();
    m.addItem (upId, "Move up", slot > 0);
    m.addItem (downId, "Move down", slot < rig::numInserts (track) - 1);
    if (st.error.isNotEmpty()) { m.addSeparator(); m.addItem (-1, st.error, false); }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track, slot] (int r) {
        auto n = rig::insertNode (track, slot);
        if (! n.isValid() || r == 0) return;
        if (r == openId) openInsert (track, slot);
        else if (r == bypassId) { doc.undo().beginNewTransaction ("Bypass plugin"); n.setProperty (rig::ids::bypass, ! (bool) n[rig::ids::bypass], &doc.undo()); }
        else if (r == upId || r == downId) { doc.undo().beginNewTransaction ("Move plugin"); rig::moveInsert (track, slot, r == upId ? -1 : 1, &doc.undo()); }
        else if (r == sidechainNone) { doc.undo().beginNewTransaction ("Side-chain off"); n.removeProperty (rig::ids::sidechain, &doc.undo()); }
        else if (r >= sidechainBase && r < sidechainBase + 10000)   // (the plugin list's own ids are far above this range)
        {
            auto src = doc.tracks().getChild (r - sidechainBase);
            if (! src.isValid()) return;
            doc.undo().beginNewTransaction ("Side-chain source");
            n.setProperty (rig::ids::sidechain, src[ids::id].toString(), &doc.undo());
        }
        else if (r == removeId) { doc.undo().beginNewTransaction ("Remove plugin"); rig::removeInsert (track, slot, &doc.undo()); }
        else if (plugins != nullptr)
        {
            juce::String name;
            const auto desc = plugins->descriptionForMenuResult (r, 1, name);
            if (desc.isEmpty()) return;
            doc.undo().beginNewTransaction ("Replace plugin");
            rig::replaceInsert (track, slot, desc, name, &doc.undo());
        }
    });
}

bool SessionController::trackHasRig (const juce::ValueTree& track) const
{
    return rig::isEnabled (rig::find (track));
}

void SessionController::rebuildGraphNow()
{
    cancelPendingUpdate();
    GraphBuildOptions opt;
    opt.sampleRate = host.engine.getSampleRate() > 0 ? host.engine.getSampleRate() : 48000.0;
    opt.recorders = &recorders;
    opt.midiRecorders = &midiRecorders;
    opt.rigFor = [this] (const juce::ValueTree& t) -> std::shared_ptr<fx::AmpRig> {
        if ((bool) rig::find (t)[rig::ids::bypass]) return nullptr;   // amp switched off from the mixer
        return rigBank->rigForTrack (t[ids::id].toString());
    };
    opt.insertsFor = [this] (const juce::ValueTree& t) { return rigBank->insertsForTrack (t[ids::id].toString()); };
    opt.masterInserts = rigBank->insertsForTrack (project::masterId);
    opt.masterNode = project::masterNode (doc.tree());
    opt.instrumentFor = [this] (const juce::ValueTree& t) { return rigBank->instrumentForTrack (t[ids::id].toString()); };
    host.engine.setDrumInstrument (rigBank->instrumentForTrack (fx::RigBank::drumKey));
    host.engine.submitGraph (buildPlaybackGraph (doc.tracks(), doc.mediaBundle(), cache, opt));
}

juce::File SessionController::fileFor (const juce::ValueTree& clip) const
{
    return project::Bundle::fromPortablePath (doc.mediaBundle(), clip[ids::file].toString());
}

std::shared_ptr<const CachedAudio> SessionController::audioFor (const juce::ValueTree& clip)
{
    const auto f = fileFor (clip);
    return f == juce::File() ? nullptr : cache.get (f);
}

// ------------------------------------------------------------------ tracks

juce::ValueTree SessionController::addTrack (const juce::String& nameIn, int insertIndex, TrackKind kind)
{
    auto tracks = doc.tracks();
    if (tracks.getNumChildren() >= maxTracks)
    {
        if (onError) onError ("Track limit", "This version supports up to " + juce::String (maxTracks) + " tracks.");
        return {};
    }
    int sameKind = 1;
    for (auto x : tracks) sameKind += kindOf (x) == kind ? 1 : 0;
    const auto name = nameIn.isNotEmpty() ? nameIn
                    : (kind == TrackKind::bus ? "Bus " : kind == TrackKind::fx ? "FX " : kind == TrackKind::instrument ? "Instrument " : "Track ")
                      + juce::String (sameKind);
    auto t = kind == TrackKind::audio ? createTrack (name, tracks.getNumChildren()) : createBusTrack (name, tracks.getNumChildren(), kind);
    if (kind == TrackKind::instrument)   // starts with the built-in synth
    {
        juce::ValueTree in (midiclip::INSTRUMENT);
        in.setProperty (rig::ids::desc, fx::builtin::descriptionXml ("fretforge:synth"), nullptr);
        in.setProperty (rig::ids::name, "Basic Synth", nullptr);
        t.appendChild (in, nullptr);
    }
    if (kind == TrackKind::audio)
    {
        const auto inputs = host.activeInputChannelNames();
        t.setProperty (ids::inputChannel, inputs.isEmpty() ? -1 : 0, nullptr);
    }
    else t.setProperty (ids::inputChannel, -1, nullptr);
    if (! batchingTracks)
        doc.undo().beginNewTransaction (kind == TrackKind::bus ? "Add bus" : kind == TrackKind::fx ? "Add FX track"
                                        : kind == TrackKind::instrument ? "Add instrument track" : "Add track");
    tracks.addChild (t, insertIndex, &doc.undo());
    setSelectedTracks ({ t });
    return t;
}

// ------------------------------------------------------------------ folders

juce::ValueTree SessionController::newFolder (juce::Array<juce::ValueTree> members, int insertIndex)
{
    auto tracks = doc.tracks();
    if (tracks.getNumChildren() >= maxTracks)
    {
        if (onError) onError ("Track limit", "This version supports up to " + juce::String (maxTracks) + " tracks.");
        return {};
    }
    members.removeIf ([] (const juce::ValueTree& t) { return isFolder (t); });
    int count = 1;
    for (auto x : tracks) count += isFolder (x) ? 1 : 0;
    auto f = createBusTrack ("Folder " + juce::String (count), tracks.getNumChildren(), TrackKind::bus);
    f.setProperty (ids::folder, true, nullptr);
    int at = insertIndex < 0 ? tracks.getNumChildren() : juce::jmin (insertIndex, tracks.getNumChildren());
    for (auto& m : members) at = juce::jmin (at, tracks.indexOf (m));
    if (at < 0) at = tracks.getNumChildren();
    doc.undo().beginNewTransaction (members.isEmpty() ? "New folder" : "Folder from tracks");
    tracks.addChild (f, at, &doc.undo());
    for (auto& m : members) m.setProperty (ids::inFolder, f[ids::id].toString(), &doc.undo());
    if (! members.isEmpty()) moveTracks (members, tracks.indexOf (f) + 1);
    setSelectedTracks ({ f });
    return f;
}

void SessionController::setFolder (juce::Array<juce::ValueTree> list, const juce::ValueTree& folder)
{
    list.removeIf ([] (const juce::ValueTree& t) { return isFolder (t); });
    if (list.isEmpty()) return;
    doc.undo().beginNewTransaction (folder.isValid() ? "Move to folder" : "Take out of folder");
    for (auto& t : list) t.setProperty (ids::inFolder, folder.isValid() ? folder[ids::id].toString() : juce::String(), &doc.undo());
    if (folder.isValid()) moveTracks (list, doc.tracks().indexOf (folder) + 1);
}

void SessionController::toggleFolder (const juce::ValueTree& folderIn)
{
    auto folder = folderIn;
    if (! folder.isValid()) return;
    doc.undo().beginNewTransaction ("Folder");
    folder.setProperty (ids::collapsed, ! (bool) folder[ids::collapsed], &doc.undo());
}

bool SessionController::isHiddenByFolder (const juce::ValueTree& track) const
{
    const auto fid = folderIdOf (track);
    if (fid.isEmpty() || isFolder (track)) return false;
    const auto f = timeline::findTrack (doc.tracks(), fid);
    return f.isValid() && (bool) f[ids::collapsed];
}

void SessionController::addFolderMenu (juce::PopupMenu& menu, const juce::ValueTree& track, int baseId) const
{
    juce::PopupMenu m;
    const int n = tracksForEdit (track).size();
    m.addItem (baseId, n > 1 ? "New folder from the " + juce::String (n) + " selected tracks" : juce::String ("New folder from this track"));
    int i = 0;
    bool any = false;
    for (auto t : doc.tracks())
        if (isFolder (t) && i < 50)
        {
            if (! any) m.addSeparator();
            any = true;
            m.addItem (baseId + 10 + i, (n > 1 ? "Move selected to " : "Move to ") + t[ids::name].toString(), true, folderIdOf (track) == t[ids::id].toString());
            ++i;
        }
    m.addSeparator();
    m.addItem (baseId + 1, "Take out of folder", folderIdOf (track).isNotEmpty());
    menu.addSubMenu ("Folder", m);
}

bool SessionController::handleFolderMenu (int r, const juce::ValueTree& track, int baseId)
{
    if (r < baseId || r >= baseId + 60) return false;
    const auto targets = tracksForEdit (track);
    if (r == baseId) newFolder (targets);
    else if (r == baseId + 1) setFolder (targets, {});
    else
    {
        int i = 0;
        for (auto t : doc.tracks())
            if (isFolder (t) && i++ == r - baseId - 10) { setFolder (targets, t); break; }
    }
    return true;
}

void SessionController::showAddTrackMenu (juce::Component* target, int insertIndex)
{
    juce::PopupMenu m;
    m.addItem (1, "Audio track");
    m.addItem (2, "Bus track (group tracks: route them to it)");
    m.addItem (3, "FX track (shared effect: send tracks to it)");
    m.addItem (4, "Instrument track (MIDI with a synth / VST instrument)");
    m.addItem (5, "Folder (collapse tracks to save space)");
    m.addSeparator();
    m.addItem (6, "Multiple tracks...");
    juce::Component::SafePointer<juce::Component> safe (target);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, insertIndex] (int r) {
        if (r == 1) addTrack ({}, insertIndex);
        else if (r == 2) addTrack ({}, insertIndex, TrackKind::bus);
        else if (r == 3) addTrack ({}, insertIndex, TrackKind::fx);
        else if (r == 4) addTrack ({}, insertIndex, TrackKind::instrument);
        else if (r == 5) newFolder ({}, insertIndex);
        else if (r == 6) showAddMultipleTracksDialog (insertIndex);
    });
}

void SessionController::showAddMultipleTracksDialog (int insertIndex)
{
    auto* w = new juce::AlertWindow ("Add multiple tracks", "How many tracks, and what kind?", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("count", "4", "Number of tracks (1 - 64)");
    w->addComboBox ("kind", { "Audio", "Instrument (MIDI with a synth / VST instrument)" }, "Type");
    w->addComboBox ("chans", { "Mono", "Stereo" }, "Channels (audio tracks)");
    w->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, insertIndex] (int r) {
        std::unique_ptr<juce::AlertWindow> keep (w);
        if (r != 1) return;
        const int n = juce::jlimit (1, 64, w->getTextEditorContents ("count").getIntValue());
        const bool instrument = w->getComboBoxComponent ("kind")->getSelectedItemIndex() == 1;
        const bool stereo = w->getComboBoxComponent ("chans")->getSelectedItemIndex() == 1 && ! instrument;
        const juce::ScopedValueSetter<bool> batch (batchingTracks, true);
        doc.undo().beginNewTransaction ("Add " + juce::String (n) + (instrument ? " instrument tracks" : " tracks"));
        juce::Array<juce::ValueTree> added;
        for (int i = 0; i < n; ++i)
        {
            auto t = addTrack ({}, insertIndex < 0 ? -1 : insertIndex + i, instrument ? TrackKind::instrument : TrackKind::audio);
            if (! t.isValid()) break;
            if (stereo) setTrackStereo (t, true);
            added.add (t);
        }
        if (! added.isEmpty()) setSelectedTracks (added);
    }), false);
}

juce::String SessionController::outputName (const juce::ValueTree& track) const
{
    return routeName (outputOf (track));
}

juce::String SessionController::routeName (const juce::String& id) const
{
    if (timeline::isHardwareRoute (id)) return timeline::hardwareRouteName (timeline::hardwareChannelOf (id));
    if (id.isEmpty()) return "Master";
    const auto t = timeline::findTrack (doc.tracks(), id);
    return t.isValid() ? t[ids::name].toString() : juce::String ("Master");
}

juce::Array<std::pair<int, juce::String>> SessionController::interfaceOutputPairs() const
{
    juce::Array<int> channels;
    const auto names = host.activeOutputChannelNames (channels);
    juce::Array<std::pair<int, juce::String>> pairs;
    for (int i = 0; i < channels.size(); ++i)
    {
        const int ch = channels[i];
        if (ch < 2 || ch % 2 != 0) continue;   // pairs from Out 3+4 on (the master plays on 1+2)
        const bool right = channels.contains (ch + 1);
        juce::String label = right ? timeline::hardwareRouteName (ch) : "Out " + juce::String (ch + 1);
        const auto devName = names[i] + (right ? " / " + names[channels.indexOf (ch + 1)] : juce::String());
        if (devName.isNotEmpty() && ! devName.containsIgnoreCase ("Output " + juce::String (ch + 1))) label << "  (" << devName << ")";
        pairs.add ({ ch, label });
    }
    return pairs;
}

void SessionController::setOutput (juce::ValueTree track, const juce::String& busId)
{
    const auto bus = timeline::findTrack (doc.tracks(), busId);
    if (busId.isNotEmpty() && ! timeline::isHardwareRoute (busId) && ! canRouteTo (doc.tracks(), track, bus))
    {
        if (onError) onError ("Routing", "\"" + track[ids::name].toString() + "\" cannot play into \"" + bus[ids::name].toString()
                                         + "\": that would feed the sound back into itself.");
        return;
    }
    doc.undo().beginNewTransaction ("Track output");
    track.setProperty (ids::output, busId, &doc.undo());
}

void SessionController::showRoutingMenu (juce::ValueTree track, juce::Component* target)
{
    auto tracks = doc.tracks();
    // with several tracks selected (and this one among them) every choice applies to all of them
    juce::Array<juce::ValueTree> group;
    if (selectedTracks.size() > 1 && selectedTracks.contains (track))
        for (auto& t : selectedTracks)
            if (t.hasType (ids::TRACK) && ! isFolder (t)) group.add (t);
    if (group.isEmpty()) group.add (track);
    const bool many = group.size() > 1;
    const auto sendsTo = [] (const juce::ValueTree& t, const juce::String& id) {
        for (auto c : t) if (c.hasType (ids::SEND) && c[ids::target].toString() == id) return true;
        return false;
    };
    const auto allSendTo = [&] (const juce::String& id) { for (auto& t : group) if (! sendsTo (t, id)) return false; return true; };
    juce::PopupMenu out, sends;
    out.addItem (1, "Master", true, outputOf (track).isEmpty() || (! timeline::isHardwareRoute (outputOf (track)) && ! timeline::findTrack (tracks, outputOf (track)).isValid()));
    int i = 0;
    juce::Array<juce::ValueTree> targets;
    for (auto t : tracks)
    {
        if (t.hasType (ids::TRACK) && ! isFolder (t) && ! group.contains (t) && (! isSourceTrack (t) || isInstrumentTrack (t)))
        {
            targets.add (t);
            const bool instr = isInstrumentTrack (t);   // an instrument takes sends into its audio input (not an output)
            bool ok = true;
            for (auto& g : group) ok = ok && canRouteTo (tracks, g, t, instr);
            if (! instr)
                out.addItem (100 + i, t[ids::name].toString() + (kindOf (t) == TrackKind::fx ? "  (FX)" : ""), ok, outputOf (track) == t[ids::id].toString());
            sends.addItem (300 + i, "Send to " + t[ids::name].toString() + (instr ? "  (instrument input)" : ""), ok && ! allSendTo (t[ids::id].toString()));
            ++i;
        }
    }
    // interface outputs (Out 3+4...): straight to the audio interface, not through the master
    const auto hw = interfaceOutputPairs();
    if (! hw.isEmpty())
    {
        out.addSectionHeader ("Interface outputs");
        sends.addSectionHeader ("Interface outputs");
        for (int k = 0; k < hw.size(); ++k)
        {
            const auto id = timeline::hardwareRoute (hw[k].first);
            out.addItem (700 + k, hw[k].second, true, outputOf (track) == id);
            sends.addItem (800 + k, "Send to " + hw[k].second, ! allSendTo (id));
        }
    }
    else
    {
        out.addItem (-1, "(Interface outputs: the audio device has only 2 outputs switched on)", false);
    }
    out.addSeparator();
    out.addItem (2, many ? "New bus for these " + juce::String (group.size()) + " tracks" : juce::String ("New bus for this track"));
    sends.addSeparator();
    sends.addItem (3, many ? "New FX track with a send from these " + juce::String (group.size()) + " tracks" : juce::String ("New FX track with a send from this track"));
    juce::PopupMenu m;
    m.addSectionHeader (many ? juce::String (group.size()) + " selected tracks" : track[ids::name].toString() + "  ->  " + outputName (track));
    m.addSubMenu ("Output", out);
    m.addSubMenu ("Add send", sends);
    int sendCount = 0;
    for (auto c : track) sendCount += c.hasType (ids::SEND) ? 1 : 0;
    if (sendCount > 0)
    {
        m.addSeparator();
        int k = 0;
        for (auto c : track)
            if (c.hasType (ids::SEND))
            {
                const auto tid = c[ids::target].toString();
                const auto dest = timeline::findTrack (tracks, tid);
                m.addItem (500 + k++, "Send to " + (timeline::isHardwareRoute (tid) ? routeName (tid) : dest.isValid() ? dest[ids::name].toString() : juce::String ("(missing)"))
                                          + ":  " + juce::String ((double) c[ids::level], 1) + " dB" + ((bool) c[ids::pre] ? " pre" : "") + "...");
            }
    }
    juce::Component::SafePointer<juce::Component> safe (target);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track, group, targets, safe, hw] (int r) {
        if (r == 0) return;
        auto tracks2 = doc.tracks();
        const auto outputAll = [this, group] (const juce::String& busId) {
            doc.undo().beginNewTransaction (group.size() > 1 ? "Tracks output" : "Track output");
            const auto bus = timeline::findTrack (doc.tracks(), busId);
            for (auto t : group)
            {
                if (busId.isNotEmpty() && ! timeline::isHardwareRoute (busId) && ! canRouteTo (doc.tracks(), t, bus)) continue;   // the menu greys these out
                t.setProperty (ids::output, busId, &doc.undo());
            }
        };
        const auto sendAll = [this, group] (const juce::String& id, double db) {
            doc.undo().beginNewTransaction ("Add send");
            for (auto t : group)
            {
                bool has = false;
                for (auto c : t) has = has || (c.hasType (ids::SEND) && c[ids::target].toString() == id);
                if (! has) addSend (t, id, db, false, &doc.undo());
            }
        };
        if (r >= 700 && r < 800 && r - 700 < hw.size()) { outputAll (timeline::hardwareRoute (hw[r - 700].first)); return; }
        if (r >= 800 && r < 900 && r - 800 < hw.size()) { sendAll (timeline::hardwareRoute (hw[r - 800].first), 0.0); return; }
        if (r == 1) outputAll ({});
        else if (r >= 100 && r < 300) outputAll (targets[r - 100][ids::id].toString());
        else if (r >= 300 && r < 500) sendAll (targets[r - 300][ids::id].toString(), -6.0);
        else if (r == 2 || r == 3)
        {
            const auto kind = r == 2 ? TrackKind::bus : TrackKind::fx;
            auto bus = addTrack ({}, -1, kind);
            if (! bus.isValid()) return;
            for (auto t : group)
            {
                if (kind == TrackKind::bus) t.setProperty (ids::output, bus[ids::id], &doc.undo());
                else addSend (t, bus[ids::id].toString(), -6.0, false, &doc.undo());
            }
            setSelectedTracks (group);
        }
        else if (r >= 500 && r < 700)
        {
            int k = 0;
            for (auto c : track)
                if (c.hasType (ids::SEND) && k++ == r - 500) { showSendMenu (track, c, safe.getComponent()); break; }
        }
    });
}

void SessionController::showSendMenu (juce::ValueTree track, juce::ValueTree send, juce::Component* target)
{
    if (! send.isValid()) return;
    const auto tid = send[ids::target].toString();
    const auto dest = timeline::findTrack (doc.tracks(), tid);
    juce::PopupMenu m;
    m.addSectionHeader ("Send to " + (timeline::isHardwareRoute (tid) ? routeName (tid) : dest.isValid() ? dest[ids::name].toString() : juce::String ("(missing track)")));
    static constexpr double levels[] = { 6.0, 3.0, 0.0, -3.0, -6.0, -9.0, -12.0, -18.0, -24.0, -36.0 };
    juce::PopupMenu lv;
    for (int i = 0; i < (int) std::size (levels); ++i)
        lv.addItem (10 + i, juce::String (levels[i], 1) + " dB", true, std::abs ((double) send[ids::level] - levels[i]) < 0.05);
    m.addSubMenu ("Level (" + juce::String ((double) send[ids::level], 1) + " dB; drag the send to fine-tune)", lv);
    m.addItem (2, "Pre-fader (ignores the track's fader)", true, (bool) send[ids::pre]);
    m.addSeparator();
    m.addItem (3, "Remove send");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track, send] (int r) {
        auto s = send;
        if (! s.getParent().isValid()) return;
        if (r >= 10 && r < 30) { doc.undo().beginNewTransaction ("Send level"); s.setProperty (ids::level, levels[r - 10], &doc.undo()); }
        else if (r == 2) { doc.undo().beginNewTransaction ("Send pre/post"); s.setProperty (ids::pre, ! (bool) s[ids::pre], &doc.undo()); }
        else if (r == 3) { doc.undo().beginNewTransaction ("Remove send"); auto t = track; t.removeChild (s, &doc.undo()); }
    });
}

void SessionController::deleteTrack (juce::ValueTree track)
{
    if (sessionActive && recorders.count (track[ids::id].toString()) > 0)
    {
        if (onError) onError ("Track is recording", "Stop recording before deleting this track.");
        return;
    }
    doc.undo().beginNewTransaction ("Delete track");
    removeRoutesTo (doc.tracks(), track[ids::id].toString(), &doc.undo());   // tracks that played into it go to the master
    doc.tracks().removeChild (track, &doc.undo());
}

void SessionController::deleteTracks (const juce::Array<juce::ValueTree>& tracks)
{
    juce::Array<juce::ValueTree> list;
    for (auto& t : tracks)
        if (t.isValid() && t.getParent() == doc.tracks()) list.addIfNotAlreadyThere (t);
    if (list.isEmpty()) return;
    juce::StringArray busy;
    for (auto& t : list)
        if (sessionActive && recorders.count (t[ids::id].toString()) > 0) busy.add (t[ids::name].toString());
    if (busy.size() > 0 && onError)
        onError ("Track is recording", "Stop recording before deleting: " + busy.joinIntoString (", "));
    doc.undo().beginNewTransaction (list.size() > 1 ? "Delete " + juce::String (list.size()) + " tracks" : juce::String ("Delete track"));
    for (auto& t : list)
    {
        if (sessionActive && recorders.count (t[ids::id].toString()) > 0) continue;
        if (isFolder (t))   // its tracks stay, outside any folder
            for (auto x : doc.tracks())
                if (folderIdOf (x) == t[ids::id].toString()) x.removeProperty (ids::inFolder, &doc.undo());
        removeRoutesTo (doc.tracks(), t[ids::id].toString(), &doc.undo());
        doc.tracks().removeChild (t, &doc.undo());
    }
}

juce::ValueTree SessionController::duplicateTrack (juce::ValueTree track)
{
    auto copy = track.createCopy();
    copy.setProperty (ids::id, newId(), nullptr);
    copy.setProperty (ids::name, track[ids::name].toString() + " copy", nullptr);
    copy.setProperty (ids::armed, false, nullptr);
    for (auto c : copy)
        c.setProperty (ids::id, newId(), nullptr);
    doc.undo().beginNewTransaction ("Duplicate track");
    doc.tracks().addChild (copy, doc.tracks().indexOf (track) + 1, &doc.undo());
    return copy;
}

juce::ValueTree SessionController::findTrack (const juce::String& id)
{
    for (auto t : doc.tracks())
        if (t[ids::id].toString() == id)
            return t;
    return {};
}

juce::StringArray SessionController::inputChoiceNames (juce::Array<std::pair<int, bool>>& values) const
{
    juce::StringArray names;
    values.clear();
    names.add ("No input");
    values.add ({ -1, false });
    const auto ch = host.activeInputChannelNames();
    for (int i = 0; i < ch.size(); ++i)
    {
        names.add ("In " + juce::String (i + 1) + "  (" + ch[i] + ")");
        values.add ({ i, false });
    }
    for (int i = 0; i + 1 < ch.size(); i += 2)
    {
        names.add ("In " + juce::String (i + 1) + "+" + juce::String (i + 2) + "  stereo");
        values.add ({ i, true });
    }
    return names;
}

juce::StringArray SessionController::inputChoiceNames (juce::Array<std::pair<int, bool>>& values, bool stereo) const
{
    juce::Array<std::pair<int, bool>> all;
    const auto allNames = inputChoiceNames (all);
    juce::StringArray names;
    values.clear();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].first < 0 || all[i].second == stereo) { names.add (allNames[i]); values.add (all[i]); }
    return names;
}

void SessionController::setTrackStereo (juce::ValueTree track, bool stereo)
{
    if (! isAudioTrack (track) || (bool) track[ids::inputStereo] == stereo) return;
    if (! batchingTracks) doc.undo().beginNewTransaction (stereo ? "Stereo track" : "Mono track");
    const int numInputs = host.activeInputChannelNames().size();
    for (auto t : tracksForEdit (track))
    {
        if (! isAudioTrack (t)) continue;
        int ch = (int) t[ids::inputChannel];
        if (stereo && ch >= 0)
        {
            ch -= ch % 2;                                   // the pair this input belongs to (In 2 -> In 1+2)
            if (ch + 1 >= numInputs) ch = numInputs >= 2 ? 0 : -1;   // no such pair on this interface
            t.setProperty (ids::inputChannel, ch, &doc.undo());
        }
        t.setProperty (ids::inputStereo, stereo, &doc.undo());
    }
}

void SessionController::toggleInstrumentBypass (juce::ValueTree track)
{
    auto in = track.getChildWithName (midiclip::INSTRUMENT);
    if (! in.isValid()) return;
    doc.undo().beginNewTransaction ((bool) in[rig::ids::bypass] ? "Enable instrument" : "Bypass instrument");
    in.setProperty (rig::ids::bypass, ! (bool) in[rig::ids::bypass], &doc.undo());
}

void SessionController::removeInstrument (juce::ValueTree track)
{
    auto in = track.getChildWithName (midiclip::INSTRUMENT);
    if (! in.isValid()) return;
    doc.undo().beginNewTransaction ("Remove instrument");
    track.removeChild (in, &doc.undo());
}

void SessionController::instrumentOutputsToTracks (juce::ValueTree track)
{
    auto in = track.getChildWithName (midiclip::INSTRUMENT);
    auto plugin = rigBank->instrumentPlugin (track);
    if (! in.isValid() || plugin == nullptr)
    {
        if (onError) onError ("Instrument outputs", "Wait until the instrument has finished loading, then try again.");
        return;
    }
    const int buses = plugin->getBusCount (false);
    if (buses < 2)
    {
        if (onError) onError ("Instrument outputs", in[rig::ids::name].toString() + " has only one stereo output, so there is nothing to split off.");
        return;
    }
    const int extra = juce::jmin (buses - 1, 15);
    auto tracks = doc.tracks();
    if (tracks.getNumChildren() + extra > maxTracks)
    {
        if (onError) onError ("Instrument outputs", "That would be more than " + juce::String (maxTracks) + " tracks.");
        return;
    }
    rigBank->captureAllPluginStates();   // the instrument is reloaded with its outputs on: keep its settings
    const auto id = track[ids::id].toString();
    doc.undo().beginNewTransaction ("Instrument outputs to tracks");
    // existing output tracks of this instrument are replaced
    for (int i = tracks.getNumChildren(); --i >= 0;)
        if (tracks.getChild (i)[ids::instrSource].toString() == id) tracks.removeChild (i, &doc.undo());
    int at = tracks.indexOf (track) + 1;
    for (int b = 1; b <= extra; ++b)
    {
        auto name = plugin->getBus (false, b) != nullptr ? plugin->getBus (false, b)->getName() : juce::String ("Output " + juce::String (b + 1));
        auto t = createBusTrack (track[ids::name].toString() + " - " + name, tracks.indexOf (track), TrackKind::bus);
        t.setProperty (ids::colour, track[ids::colour], nullptr);
        t.setProperty (ids::instrSource, id, nullptr);
        t.setProperty (ids::instrBus, b, nullptr);
        tracks.addChild (t, at++, &doc.undo());
    }
    in.setProperty (rig::ids::auxOutputs, extra, &doc.undo());
}

void SessionController::removeInstrumentOutputTracks (juce::ValueTree track)
{
    auto tracks = doc.tracks();
    const auto id = track[ids::id].toString();
    doc.undo().beginNewTransaction ("Remove instrument output tracks");
    for (int i = tracks.getNumChildren(); --i >= 0;)
        if (tracks.getChild (i)[ids::instrSource].toString() == id) tracks.removeChild (i, &doc.undo());
    if (auto in = track.getChildWithName (midiclip::INSTRUMENT); in.isValid() && (int) in[rig::ids::auxOutputs] != 0)
    {
        rigBank->captureAllPluginStates();
        in.setProperty (rig::ids::auxOutputs, 0, &doc.undo());
    }
}

void SessionController::showInstrumentContextMenu (juce::ValueTree track, juce::Component* target)
{
    if (plugins == nullptr || ! isInstrumentTrack (track)) return;
    const bool has = track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::desc].toString().isNotEmpty();
    juce::PopupMenu m, change;
    plugins->addToMenu (change, PluginManager::instrumentMenu);
    if (has)
    {
        m.addSectionHeader (instrumentName (track));
        m.addItem (-10, "Open");
        m.addItem (-12, "Bypass", true, (bool) track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::bypass]);
        m.addSubMenu ("Change instrument", change);
        m.addItem (-13, "Outputs to tracks", true, (int) track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::auxOutputs] > 0);
        if ((int) track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::auxOutputs] > 0) m.addItem (-14, "Remove output tracks");
        m.addItem (-11, "Remove instrument");
    }
    else
    {
        m.addSectionHeader ("Choose an instrument");
        plugins->addToMenu (m, PluginManager::instrumentMenu);
    }
    m.addSeparator();
    m.addItem (-2, "Scan for plugins / manage...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track] (int r) {
        if (r == 0) return;
        if (r == -2) { plugins->showManager(); return; }
        if (r == -10) { openInstrument (track); return; }
        if (r == -11) { removeInstrument (track); return; }
        if (r == -12) { toggleInstrumentBypass (track); return; }
        if (r == -13) { instrumentOutputsToTracks (track); return; }
        if (r == -14) { removeInstrumentOutputTracks (track); return; }
        juce::String name;
        const auto desc = plugins->descriptionForMenuResult (r, PluginManager::instrumentMenu, name);
        if (desc.isNotEmpty()) { setInstrument (track, desc, name); openInstrumentWhenLoaded (track, desc); }
    });
}

// ------------------------------------------------------------------ import

juce::String SessionController::instrumentName (const juce::ValueTree& track) const
{
    const auto in = track.getChildWithName (midiclip::INSTRUMENT);
    if ((bool) in[rig::ids::bypass] && in[rig::ids::desc].toString().isNotEmpty()) return in[rig::ids::name].toString() + " (off)";
    const auto n = track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::name].toString();
    return n.isNotEmpty() ? n : juce::String ("No instrument");
}

void SessionController::setInstrument (juce::ValueTree track, const juce::String& desc, const juce::String& name, const juce::String& state)
{
    auto in = track.getChildWithName (midiclip::INSTRUMENT);
    doc.undo().beginNewTransaction ("Instrument " + name);
    if (! in.isValid())
    {
        in = juce::ValueTree (midiclip::INSTRUMENT);
        track.appendChild (in, &doc.undo());
    }
    in.setProperty (rig::ids::state, state, &doc.undo());
    in.setProperty (rig::ids::name, name, &doc.undo());
    in.setProperty (rig::ids::desc, desc, &doc.undo());
}

void SessionController::showInstrumentMenu (juce::ValueTree track, juce::Component* target)
{
    if (plugins == nullptr) return;
    juce::PopupMenu m;
    plugins->addToMenu (m, PluginManager::instrumentMenu);
    m.addSeparator();
    m.addItem (-2, "Scan for plugins / manage...");
    juce::Component::SafePointer<juce::Component> safe (target);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this, track] (int r) {
        if (r == 0) return;
        if (r == -2) { plugins->showManager(); return; }
        juce::String name;
        const auto desc = plugins->descriptionForMenuResult (r, PluginManager::instrumentMenu, name);
        if (desc.isNotEmpty()) { setInstrument (track, desc, name); openInstrumentWhenLoaded (track, desc); }
    });
}

void SessionController::openInstrument (juce::ValueTree track)
{
    if (plugins == nullptr) return;
    if (auto p = rigBank->instrumentPlugin (track))
    {
        PluginManager::BypassControl bc;
        bc.isBypassed = [this, track] { return (bool) track.getChildWithName (midiclip::INSTRUMENT)[rig::ids::bypass]; };
        bc.toggle = [this, track] { toggleInstrumentBypass (track); };
        addMuteSoloImpl (this, bc, track);
        plugins->showEditor (p, track[ids::name].toString() + " - " + instrumentName (track), bc);
    }
    else
    {
        const auto st = rigBank->instrumentStatus (track);
        if (onError) onError ("Instrument", st.error.isNotEmpty() ? st.error : juce::String ("The instrument is still loading."));
    }
}

/** A plugin instrument that was just chosen opens its window as soon as it has loaded. */
void SessionController::openInstrumentWhenLoaded (juce::ValueTree track, const juce::String& desc, int triesLeft)
{
    if (! track.isValid() || track.getParent() == juce::ValueTree()) return;   // the track went away
    const auto st = rigBank->instrumentStatus (track);
    if (st.desc == desc && st.error.isNotEmpty()) { if (onError) onError ("Instrument", st.error); return; }
    if (st.desc == desc && st.loaded && rigBank->instrumentPlugin (track) != nullptr) { openInstrument (track); return; }
    if (triesLeft <= 0) return;
    std::weak_ptr<bool> alive = life;
    juce::Timer::callAfterDelay (150, [this, alive, track, desc, triesLeft] {
        if (alive.lock() == nullptr) return;
        openInstrumentWhenLoaded (track, desc, triesLeft - 1);
    });
}

// ---- the Drum Studio's instrument: a plugin that plays the pattern's hits instead of the built-in kit ----

juce::ValueTree SessionController::drumInstrumentNode() const
{
    return drums::find (doc.tree()).getChildWithName (midiclip::INSTRUMENT);
}

void SessionController::setDrumInstrument (const juce::String& desc, const juce::String& name)
{
    auto d = drums::getOrCreate (doc.tree());
    auto in = d.getChildWithName (midiclip::INSTRUMENT);
    doc.undo().beginNewTransaction ("Drum instrument " + name);
    if (! in.isValid())
    {
        in = juce::ValueTree (midiclip::INSTRUMENT);
        d.appendChild (in, &doc.undo());
    }
    in.setProperty (rig::ids::state, juce::String(), &doc.undo());
    in.setProperty (rig::ids::name, name, &doc.undo());
    in.setProperty (rig::ids::desc, desc, &doc.undo());
    in.setProperty (rig::ids::bypass, false, &doc.undo());
}

void SessionController::removeDrumInstrument()
{
    auto in = drumInstrumentNode();
    if (! in.isValid()) return;
    doc.undo().beginNewTransaction ("Use the built-in drum kit");
    in.getParent().removeChild (in, &doc.undo());
}

void SessionController::openDrumInstrument()
{
    if (plugins == nullptr) return;
    if (auto p = rigBank->drumInstrumentPlugin())
    {
        PluginManager::BypassControl bc;
        bc.isBypassed = [this] { return (bool) drumInstrumentNode()[rig::ids::bypass]; };
        bc.toggle = [this] {
            if (auto in = drumInstrumentNode(); in.isValid()) in.setProperty (rig::ids::bypass, ! (bool) in[rig::ids::bypass], &doc.undo());
        };
        plugins->showEditor (p, "MIDI Studio - " + drumInstrumentNode()[rig::ids::name].toString(), bc);
    }
    else
    {
        const auto st = rigBank->drumInstrumentStatus();
        if (onError) onError ("Drum instrument", st.error.isNotEmpty() ? st.error : juce::String ("The instrument is still loading."));
    }
}

void SessionController::openDrumInstrumentWhenLoaded (const juce::String& desc, int triesLeft)
{
    const auto st = rigBank->drumInstrumentStatus();
    if (st.desc == desc && st.error.isNotEmpty()) { if (onError) onError ("Drum instrument", st.error); return; }
    if (st.desc == desc && st.loaded && rigBank->drumInstrumentPlugin() != nullptr) { openDrumInstrument(); return; }
    if (triesLeft <= 0) return;
    std::weak_ptr<bool> alive = life;
    juce::Timer::callAfterDelay (150, [this, alive, desc, triesLeft] {
        if (alive.lock() == nullptr) return;
        openDrumInstrumentWhenLoaded (desc, triesLeft - 1);
    });
}

void SessionController::showDrumInstrumentMenu (juce::Component* target)
{
    if (plugins == nullptr) return;
    const auto in = drumInstrumentNode();
    const bool has = in[rig::ids::desc].toString().isNotEmpty();
    juce::PopupMenu m, change;
    plugins->addToMenu (change, PluginManager::instrumentMenu);
    if (has)
    {
        m.addSectionHeader (in[rig::ids::name].toString());
        m.addItem (-10, "Open");
        m.addItem (-12, "Bypass (play the built-in kit)", true, (bool) in[rig::ids::bypass]);
        m.addSubMenu ("Change instrument", change);
        m.addItem (-11, "Remove (use the built-in kit)");
    }
    else
    {
        m.addSectionHeader ("Play the drums through an instrument");
        plugins->addToMenu (m, PluginManager::instrumentMenu);
    }
    m.addSeparator();
    m.addItem (-2, "Scan for plugins / manage...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMousePosition(), [this] (int r) {
        if (r == 0) return;
        if (r == -2) { plugins->showManager(); return; }
        if (r == -10) { openDrumInstrument(); return; }
        if (r == -11) { removeDrumInstrument(); return; }
        if (r == -12)
        {
            if (auto n = drumInstrumentNode(); n.isValid()) n.setProperty (rig::ids::bypass, ! (bool) n[rig::ids::bypass], &doc.undo());
            return;
        }
        juce::String name;
        const auto desc = plugins->descriptionForMenuResult (r, PluginManager::instrumentMenu, name);
        if (desc.isNotEmpty()) { setDrumInstrument (desc, name); openDrumInstrumentWhenLoaded (desc); }
    });
}

juce::Result SessionController::importMidi (const juce::File& file, double atSec, juce::ValueTree onto)
{
    const auto mf = midiclip::readMidiFile (file);
    if (mf.error.isNotEmpty()) return juce::Result::fail (mf.error);
    auto tracks = doc.tracks();
    if (onto.isValid() && ! isInstrumentTrack (onto)) onto = {};
    if (tracks.getNumChildren() + (int) mf.tracks.size() - (onto.isValid() ? 1 : 0) > maxTracks)
        return juce::Result::fail ("That would be more than " + juce::String (maxTracks) + " tracks.");
    bool anyClips = false;
    for (auto t : tracks) for (auto c : t) anyClips = anyClips || c.hasType (ids::CLIP);
    doc.undo().beginNewTransaction ("Import MIDI " + file.getFileName());
    const auto projectMap = doc.tempoMap();
    const bool followTrack = ! projectMap.isConstant();   // a tempo track already exists: the notes go onto it
    if (! anyClips && mf.firstTempo > 20.0 && ! followTrack)   // an empty project follows the song's tempo, so the grid lines up
    {
        auto tr = doc.tree().getChildWithName (project::ids::TRANSPORT);
        tr.setProperty (project::ids::bpm, std::round (mf.firstTempo * 100.0) / 100.0, &doc.undo());
        if (mf.tsNum > 0 && mf.tsDen > 0)
        {
            tr.setProperty (project::ids::tsNum, mf.tsNum, &doc.undo());
            tr.setProperty (project::ids::tsDen, mf.tsDen, &doc.undo());
        }
    }
    juce::Array<juce::ValueTree> created;
    for (auto& it : mf.tracks)
    {
        const bool drums = it.channel == 10;
        const bool existing = onto.isValid();   // the first part goes onto the track it was dropped on
        auto t = existing ? onto : createBusTrack (it.name, tracks.getNumChildren(), TrackKind::instrument);
        if (! existing)
        {
            t.setProperty (ids::inputChannel, -1, nullptr);
            juce::ValueTree in (midiclip::INSTRUMENT);
            in.setProperty (rig::ids::desc, fx::builtin::descriptionXml (drums ? "fretforge:drumkit" : "fretforge:synth"), nullptr);
            in.setProperty (rig::ids::name, drums ? "Drum Kit" : "Basic Synth", nullptr);
            t.appendChild (in, nullptr);
        }
        auto notes = it.notes;
        auto pedal = it.pedal;
        if (followTrack)
        {
            // keep the music's beats: file seconds -> beats of the file -> seconds on the project's tempo track
            const double qa = projectMap.quarterAtSeconds (atSec);
            const auto warp = [&] (double fileSec) { return projectMap.secondsAtQuarter (qa + mf.quarterAtFileSeconds (fileSec)) - atSec; };
            for (auto& n : notes)
            {
                const double a = warp (n.start);
                n.length = juce::jmax (0.005, warp (n.start + n.length) - a);
                n.start = a;
            }
            for (auto& p : pedal) p.time = warp (p.time);
        }
        auto clip = midiclip::createMidiClip (it.name, atSec, notes, pedal);
        clip.setProperty (midiclip::channel, it.channel, nullptr);
        t.appendChild (clip, existing ? &doc.undo() : nullptr);
        if (! existing) tracks.appendChild (t, &doc.undo());
        created.add (t);
        onto = {};
    }
    setSelectedTracks (created);
    return juce::Result::ok();
}

juce::Result SessionController::importFiles (const juce::Array<juce::File>& filesIn, juce::ValueTree track, double atSec)
{
    // MIDI files become instrument tracks
    juce::Array<juce::File> files;
    for (auto& f : filesIn)
        if (f.hasFileExtension ("mid;midi;smf"))
        {
            if (auto r = importMidi (f, atSec, track); r.failed()) return r;
            else if (isInstrumentTrack (track)) track = {};   // that track took it
        }
        else files.add (f);
    if (files.isEmpty()) return juce::Result::ok();

    // One file goes onto the given track (a new one if none). Several files each get their own
    // track, named after the file, all starting at the same position - like dragging stems in.
    auto tracks = doc.tracks();
    const auto bundle = doc.mediaBundle();
    auto r = project::Bundle::createStructure (bundle);
    if (r.failed())
        return r;

    juce::StringArray problems;
    juce::Array<juce::ValueTree> created;
    doc.undo().beginNewTransaction (files.size() > 1 ? "Import " + juce::String (files.size()) + " files" : juce::String ("Import audio"));
    int insertAt = track.isValid() ? tracks.indexOf (track) + 1 : tracks.getNumChildren();
    bool useGivenTrack = track.isValid();

    for (const auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (cache.formats().createReaderFor (f));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
        {
            problems.add (f.getFileName() + ": not a supported audio file (WAV, AIFF, FLAC, OGG, MP3).");
            continue;
        }
        const double len = (double) reader->lengthInSamples / reader->sampleRate;
        const bool stereoFile = reader->numChannels > 1;
        reader.reset();

        auto dst = bundle.getChildFile ("Audio").getChildFile (f.getFileName());
        if (! f.isAChildOf (bundle))
        {
            dst = dst.getNonexistentSibling (false);
            if (! f.copyFileTo (dst))
            {
                problems.add (f.getFileName() + ": could not be copied into the project.");
                continue;
            }
        }
        else
            dst = f;

        juce::ValueTree target;
        if (useGivenTrack && ! isAudioTrack (track)) useGivenTrack = false;   // bus / FX tracks hold no clips
        if (useGivenTrack)
        {
            target = track;
            useGivenTrack = false;
        }
        else
        {
            if (tracks.getNumChildren() >= maxTracks)
            {
                problems.add (f.getFileName() + ": track limit reached (" + juce::String (maxTracks) + ").");
                continue;
            }
            target = createTrack (f.getFileNameWithoutExtension(), tracks.getNumChildren());
            const auto inputs = host.activeInputChannelNames();
            target.setProperty (ids::inputChannel, inputs.isEmpty() ? -1 : 0, nullptr);
            target.setProperty (ids::inputStereo, stereoFile && inputs.size() > 1, nullptr);
            tracks.addChild (target, juce::jmin (insertAt++, tracks.getNumChildren()), &doc.undo());
        }
        created.add (target);

        auto clip = createClip (project::Bundle::toPortablePath (bundle, dst), f.getFileNameWithoutExtension(), atSec, len, 0.0);
        target.appendChild (clip, &doc.undo());
        cache.get (dst);
        juce::Logger::writeToLog ("Imported " + f.getFullPathName() + " as " + dst.getFullPathName());
    }
    if (! created.isEmpty())
        setSelectedTracks (created);
    return problems.isEmpty() ? juce::Result::ok() : juce::Result::fail (problems.joinIntoString ("\n"));
}

// ------------------------------------------------------------------ recording

juce::StringArray SessionController::recordingTrackIds() const
{
    juce::StringArray ids_;
    for (auto& [id, r] : recorders)
        ids_.add (id);
    for (auto& [id, r] : midiRecorders)
        ids_.add (id);
    return ids_;
}

void SessionController::toggleRecord()
{
    auto& e = host.engine;
    if (sessionActive)
    {
        if (e.isRecordRequested())
            e.setRecording (false); // punch out; playback continues, clips are created when the engine confirms
        return;
    }

    if (! host.hasDevice() || e.getSampleRate() <= 0.0)
    {
        if (onError) onError ("Cannot record", "No audio device is running. Choose your interface in Settings.");
        return;
    }

    juce::Array<juce::ValueTree> armed, armedMidi;
    for (auto t : doc.tracks())
        if ((bool) t[ids::armed])
        {
            if (isAudioTrack (t)) armed.add (t);
            else if (isInstrumentTrack (t)) armedMidi.add (t);
        }
    if (armed.isEmpty() && armedMidi.isEmpty())
    {
        if (onError) onError ("No track armed", "Arm at least one track for recording (the R button in its header): "
                                                "an audio track records its input, an instrument track records your MIDI keyboard.");
        return;
    }

    const auto inputs = host.activeInputChannelNames();
    const auto bundle = doc.mediaBundle();
    project::Bundle::createStructure (bundle);
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    recordRate = e.getSampleRate();

    std::map<juce::String, std::shared_ptr<TrackRecorder>> created;
    for (auto& t : armed)
    {
        const int inL = (int) t[ids::inputChannel];
        const bool stereo = (bool) t[ids::inputStereo];
        if (inL < 0 || inL >= inputs.size() || (stereo && inL + 1 >= inputs.size()))
        {
            if (onError) onError ("Input not available", "Track \"" + t[ids::name].toString()
                                                           + "\" has no valid input. Pick one in the track header.");
            return;
        }
        auto file = bundle.getChildFile ("Audio").getChildFile (juce::File::createLegalFileName (t[ids::name].toString())
                                                                 + " " + stamp + ".wav").getNonexistentSibling (false);
        juce::String err;
        auto rec = TrackRecorder::create (file, stereo ? 2 : 1, inL, stereo ? inL + 1 : -1, recordRate, 24, err);
        if (rec == nullptr)
        {
            for (auto& [id, r] : created) r->finish(), r->file.deleteFile();
            if (onError) onError ("Cannot record", err);
            return;
        }
        created[t[ids::id].toString()] = rec;
    }

    recorders = std::move (created);
    for (auto& [id, r] : recorders)
        diskWriter.add (r);
    midiRecorders.clear();
    for (auto& t : armedMidi)
        midiRecorders[t[ids::id].toString()] = std::make_shared<MidiRecorder>();
    rebuildGraphNow(); // the graph now carries the recorders

    if (auto* dev = host.deviceManager.getCurrentAudioDevice())
    {
        latencySamples = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
        // a key is played in time with what is heard: one output latency (plus the block it waits for) late
        midiLatencySamples = dev->getOutputLatencyInSamples() + dev->getCurrentBufferSizeSamples() / 2;
    }

    juce::int64 countIn = 0;
    if (! e.isPlaying())
    {
        const int bars = (int) doc.metronome()[project::ids::countInBars];
        countIn = (juce::int64) (bars * doc.tsNum() * MetronomeDsp::beatLengthSamples (recordRate, doc.tempo(), doc.tsDen()));
    }
    recordStartSec = e.getPositionSeconds();
    sessionActive = true;
    e.setRecording (true, countIn);
    juce::Logger::writeToLog ("Recording started on " + juce::String ((int) (recorders.size() + midiRecorders.size())) + " track(s), latency compensation "
                              + juce::String (latencySamples) + " samples");
    if (onArrangementChanged) onArrangementChanged();
}

void SessionController::timerCallback()
{
    host.engine.collectGarbage();
    if (! sessionActive)
        return;
    auto& e = host.engine;
    if (! e.isRecordRequested() && (! e.isRecordingActive() || ! host.hasDevice()))
        finishRecording();
    else
        for (auto& [id, m] : midiRecorders) m->drain();
    if (! sessionActive) return;
    if (diskWriter.diskErrorFlag.exchange (false))
        if (onError) onError ("Disk problem", "Writing the recording to disk failed. Check free space.");
}

void SessionController::finishRecording()
{
    sessionActive = false;
    auto finished = std::move (recorders);
    recorders.clear();
    auto finishedMidi = std::move (midiRecorders);
    midiRecorders.clear();
    rebuildGraphNow();

    juce::StringArray problems;
    int clipsMade = 0;
    justRecorded.clear();
    doc.undo().beginNewTransaction ("Record");
    for (auto& [trackId, rec] : finished)
    {
        diskWriter.remove (rec);
        auto res = rec->finish();
        if (res.failed())
            problems.add (res.getErrorMessage());

        auto track = findTrack (trackId);
        const auto written = rec->samplesWritten();
        if (! track.isValid() || written <= 0 || rec->getNumSegments() == 0)
        {
            rec->file.deleteFile();
            continue;
        }

        const auto group = newId();
        int take = 0;
        for (int s = 0; s < rec->getNumSegments(); ++s)
        {
            const auto& seg = rec->segment (s);
            const auto fileOffset = seg.fileOffset.load() + latencySamples;
            const auto len = juce::jmin (seg.length.load(), written - fileOffset);
            if (len < (juce::int64) (0.02 * recordRate))
                continue;
            auto clip = createClip (project::Bundle::toPortablePath (doc.mediaBundle(), rec->file),
                                    track[ids::name].toString() + " take " + juce::String (++take),
                                    (double) seg.timelineStart.load() / recordRate, (double) len / recordRate,
                                    (double) fileOffset / recordRate);
            clip.setProperty (ids::takeGroup, group, nullptr);
            track.appendChild (clip, &doc.undo());
            justRecorded.add (clip);
            ++clipsMade;
        }
        cache.invalidate (rec->file);
        cache.get (rec->file);
    }
    // MIDI takes: one clip per armed instrument track, from the bar before the first note to the bar after the last
    for (auto& [trackId, mr] : finishedMidi)
    {
        mr->drain();
        auto track = findTrack (trackId);
        if (! track.isValid() || mr->getEvents().empty()) continue;
        const double shift = (double) midiLatencySamples / recordRate;
        const double endSec = (double) juce::jmax ((juce::int64) 0, mr->lastPos.load()) / recordRate - shift;
        std::vector<MidiRecorder::Note> rec;
        std::vector<MidiRecorder::Pedal> recPedal;
        MidiRecorder::toNotes (mr->getEvents(), recordRate, shift, endSec, rec, recPedal);
        if (rec.empty()) continue;
        const double bar = 60.0 / juce::jmax (1.0, doc.tempo()) * 4.0 / juce::jmax (1, doc.tsDen()) * juce::jmax (1, doc.tsNum());
        double first = rec.front().start, last = 0.0;
        for (auto& n : rec) last = juce::jmax (last, n.start + n.length);
        const double clipStart = juce::jmax (0.0, std::floor (first / bar + 1.0e-6) * bar);
        const double clipEndT = juce::jmax (clipStart + bar, std::ceil (last / bar - 1.0e-6) * bar);
        std::vector<midiclip::Note> notes;
        for (auto& n : rec)
            notes.push_back ({ juce::jmax (0.0, n.start - clipStart), n.length, n.pitch, n.velocity });
        std::vector<midiclip::Pedal> pedal;
        for (auto& p : recPedal)
            if (p.time >= clipStart) pedal.push_back ({ p.time - clipStart, p.down });
        int take = 1;
        for (auto c : track) take += midiclip::isMidiClip (c) ? 1 : 0;
        auto clip = midiclip::createMidiClip (track[ids::name].toString() + " take " + juce::String (take), clipStart, notes, pedal);
        clip.setProperty (ids::length, clipEndT - clipStart, nullptr);
        track.appendChild (clip, &doc.undo());
        justRecorded.add (clip);
        ++clipsMade;
    }
    juce::Logger::writeToLog ("Recording finished: " + juce::String (clipsMade) + " clip(s)");
    if (! problems.isEmpty() && onError)
        onError ("Recording problem", problems.joinIntoString ("\n"));
    if (onArrangementChanged) onArrangementChanged();
    if (! justRecorded.isEmpty()) recordingFinished.sendChangeMessage();
}
} // namespace ff
