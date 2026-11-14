#pragma once
#include "ProjectDocument.h"
#include "EngineHost.h"
#include "engine/AudioCache.h"
#include "engine/Recorder.h"
#include "engine/Exporter.h"
#include "engine/fx/RigBank.h"
#include "core/Timeline.h"

namespace ff
{
class PluginManager;
/** Glue between the project document and the real-time engine: rebuilds playback
    snapshots when the arrangement changes, owns the audio cache, runs recording
    sessions (recorders, disk writer, turning segments into clips) and imports media. */
class SessionController : private juce::ValueTree::Listener, private juce::AsyncUpdater, private juce::Timer
{
public:
    SessionController (EngineHost&, ProjectDocument&, juce::PropertiesFile&);
    ~SessionController() override;

    AudioCache cache;
    /** Amp rigs (track inserts and the live rig). */
    fx::RigBank& rigs() { return *rigBank; }

    void rebind();                     // project replaced
    void deviceChanged() { triggerAsyncUpdate(); }
    void rebuildGraphNow();

    juce::File fileFor (const juce::ValueTree& clip) const;
    std::shared_ptr<const CachedAudio> audioFor (const juce::ValueTree& clip);

    // ---- tracks ----
    juce::ValueTree addTrack (const juce::String& name = {}, int insertIndex = -1,
                              timeline::TrackKind kind = timeline::TrackKind::audio);
    /** Menu: audio / bus / FX track (added after `after`, or at the end). */
    void showAddTrackMenu (juce::Component* target, int insertIndex = -1);

    // ---- routing (outputs and sends) ----
    void setOutput (juce::ValueTree track, const juce::String& busId);
    /** Output and sends menu for a track: pick its bus, add sends, or make a new bus / FX track for it. */
    void showRoutingMenu (juce::ValueTree track, juce::Component* target);
    /** Level / pre-post / remove for one send. */
    void showSendMenu (juce::ValueTree track, juce::ValueTree send, juce::Component* target);
    /** "Master" or the bus name. */
    juce::String outputName (const juce::ValueTree& track) const;
    /** Display name of a route target: a track's name, an interface output ("Out 3+4"), or "Master" for "". */
    juce::String routeName (const juce::String& targetId) const;
    /** The interface output pairs that can take a track (left channel numbers, from Out 3+4 on) with their labels. */
    juce::Array<std::pair<int, juce::String>> interfaceOutputPairs() const;

    void deleteTrack (juce::ValueTree track);
    /** Deletes all of them as one undo step. */
    void deleteTracks (const juce::Array<juce::ValueTree>& tracks);
    juce::ValueTree duplicateTrack (juce::ValueTree track);
    juce::ValueTree findTrack (const juce::String& id);
    juce::StringArray inputChoiceNames (juce::Array<std::pair<int, bool>>& values) const; // (channel, stereo)

    // ---- track selection (shared by arrange view and mixer) ----
    juce::Array<juce::ValueTree> selectedTracks;
    juce::ChangeBroadcaster selectionChanged;
    /** The clips the last recording made: the arrange view keeps them selected (highlighted) afterwards. */
    juce::Array<juce::ValueTree> justRecorded;
    juce::ChangeBroadcaster recordingFinished;
    /** Plain click selects only `t`; Shift extends from the anchor; Ctrl/Cmd toggles. */
    void clickTrack (const juce::ValueTree& t, juce::ModifierKeys mods);
    void selectTrackRange (const juce::ValueTree& from, const juce::ValueTree& to);
    void setSelectedTracks (const juce::Array<juce::ValueTree>& tracks);
    bool isTrackSelected (const juce::ValueTree& t) const { return selectedTracks.contains (t); }
    /** The track an action on `clicked` should affect: the whole selection if `clicked` is in it. */
    juce::Array<juce::ValueTree> tracksForEdit (const juce::ValueTree& clicked) const;
    /** First selected track in track order (target for import/paste/add). */
    juce::ValueTree primaryTrack();
    juce::ValueTree selectionAnchor;
    /** tracksForEdit plus every track in the same group(s) (hold Alt to edit one track of a group on its own). */
    juce::Array<juce::ValueTree> linkedTracks (const juce::ValueTree& clicked) const;
    /** A volume fader moved to `now` (by `delta` dB since its last value). Moves the linked tracks by the same amount; on a
        track with volume automation the whole automation shifts too, so the change is always heard. */
    void moveTrackVolume (const juce::ValueTree& clicked, double now, double delta);
    juce::StringArray groupNames() const;
    static juce::Colour groupColour (const juce::String& group);
    /** "Group" submenu for a track's menu (ids from baseId) and its handler (returns true if it handled `result`). */
    void addGroupMenu (juce::PopupMenu& menu, const juce::ValueTree& track, int baseId) const;
    bool handleGroupMenu (int result, const juce::ValueTree& track, int baseId);
    /** Sets a property on every track in linkedTracks(clicked). */
    void setOnTracks (const juce::ValueTree& clicked, const juce::Identifier& id, const juce::var& value, bool undoable,
                      const juce::String& undoName = {});

    // ---- amp rigs (NAM) ----
    /** Gives the track an enabled amp rig (undoable) and asks the app to open its window. */
    void addTrackRig (juce::ValueTree track);
    void removeTrackRig (juce::ValueTree track);
    bool trackHasRig (const juce::ValueTree& track) const;
    /** The NAM amp shows up as the first plugin of an audio track: open / on-off / remove. */
    void showAmpMenu (juce::ValueTree track, juce::Component* target);
    void toggleAmpBypass (juce::ValueTree track);
    /** Drag of a NAM amp onto another audio track: copies the whole rig (model, cabinets, pedals, settings, plugins); move = also removes it from the source. */
    void dropAmp (const juce::String& fromTrackId, juce::ValueTree to, bool move);
    bool ampBypassed (const juce::ValueTree& track) const;
    std::function<void (juce::ValueTree track)> onOpenRig;   // set by the app: shows the track's NAM window

    // ---- track insert plugins ----
    PluginManager* plugins = nullptr;                        // set by the app
    /** Menu of known plugins; the chosen one is added after the track's last insert. */
    void showAddInsertMenu (juce::ValueTree track, juce::Component* target);
    /** Open / bypass / move / replace / remove for one insert slot. */
    void showInsertMenu (juce::ValueTree track, int slot, juce::Component* target);
    // folders (view-only containers that collapse)
    juce::ValueTree newFolder (juce::Array<juce::ValueTree> members, int insertIndex = -1);
    void setFolder (juce::Array<juce::ValueTree> tracks, const juce::ValueTree& folder);   // an invalid folder takes them out
    void toggleFolder (const juce::ValueTree& folder);
    bool isHiddenByFolder (const juce::ValueTree& track) const;
    void addFolderMenu (juce::PopupMenu&, const juce::ValueTree& track, int baseId) const;
    bool handleFolderMenu (int r, const juce::ValueTree& track, int baseId);
    void moveTracks (juce::Array<juce::ValueTree> moving, int insertIndex);   // reorder (the moved block lands before insertIndex)
    void openInsert (juce::ValueTree track, int slot);
    void toggleInsertBypass (juce::ValueTree track, int slot);
    /** A plugin dragged from (track id, slot) onto `to` at position `toSlot`: moved within a track, copied (with its
        current settings) onto another. */
    void dropInsert (const juce::String& fromTrackId, int fromSlot, juce::ValueTree to, int toSlot);
    /** A track by id, or the master bus node for project::masterId. */
    juce::ValueTree trackOrMaster (const juce::String& id);
    void openInsertWhenLoaded (juce::ValueTree track, int slot, int triesLeft);

    // ---- media ----
    /** Copies files into the project's Audio folder and places clips on `track` from `atSec`. */
    juce::Result importFiles (const juce::Array<juce::File>& files, juce::ValueTree track, double atSec);
    /** A MIDI file: one instrument track per MIDI track (drums get the Drum Kit, the rest the Basic Synth); the
        project takes the file's tempo if it has no clips yet. */
    juce::Result importMidi (const juce::File& file, double atSec, juce::ValueTree onto = {});   // `onto`: an instrument track that takes the first part

    // ---- instrument tracks ----
    /** Menu of instruments (built-in, then your VST / AU instruments) for an instrument track. */
    void showInstrumentMenu (juce::ValueTree track, juce::Component* target);
    /** Right-click menu of a track's instrument: open it, change it (the plugin list), or remove it. */
    void showInstrumentContextMenu (juce::ValueTree track, juce::Component* target);
    void removeInstrument (juce::ValueTree track);
    void toggleInstrumentBypass (juce::ValueTree track);
    /** Gives every extra output of an instrument (a multi-output drum sampler, say) its own bus track. */
    void instrumentOutputsToTracks (juce::ValueTree track);
    /** Takes those tracks away again (the instrument goes back to its one stereo output). */
    void removeInstrumentOutputTracks (juce::ValueTree track);
    /** Mono / stereo audio track: a stereo track records and monitors an input pair, a mono one a single input. */
    void setTrackStereo (juce::ValueTree track, bool stereo);
    /** The input choices of a track for its mode: single inputs for a mono track, pairs for a stereo one. */
    juce::StringArray inputChoiceNames (juce::Array<std::pair<int, bool>>& values, bool stereo) const;
    void setInstrument (juce::ValueTree track, const juce::String& descXml, const juce::String& name, const juce::String& state = {});
    void openInstrument (juce::ValueTree track);
    juce::String instrumentName (const juce::ValueTree& track) const;

    // ---- recording ----
    void toggleRecord();
    bool isRecordingSession() const { return sessionActive; }
    double recordingStartSec() const { return recordStartSec; }
    juce::StringArray recordingTrackIds() const;
    /** The audio recorder of a track while it records (for drawing the take as it comes in), or nullptr. */
    std::shared_ptr<TrackRecorder> recorderFor (const juce::String& trackId) const
    {
        auto it = recorders.find (trackId);
        return it != recorders.end() ? it->second : nullptr;
    }

    std::function<void (const juce::String& title, const juce::String& message)> onError;
    std::function<void()> onArrangementChanged; // repaint request (audio loaded, recording state)

private:
    EngineHost& host;
    ProjectDocument& doc;
    juce::PropertiesFile& settings;
    juce::ValueTree root;

    std::unique_ptr<fx::RigBank> rigBank;
    DiskWriter diskWriter;
    std::map<juce::String, std::shared_ptr<TrackRecorder>> recorders;
    std::map<juce::String, std::shared_ptr<MidiRecorder>> midiRecorders;
    juce::int64 midiLatencySamples = 0;
    bool sessionActive = false;
    double recordStartSec = 0.0;
    juce::int64 latencySamples = 0;
    double recordRate = 48000.0;

    void finishRecording();
    void handleAsyncUpdate() override { rebuildGraphNow(); }
    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void valueTreeRedirected (juce::ValueTree&) override { triggerAsyncUpdate(); }
};
} // namespace ff
