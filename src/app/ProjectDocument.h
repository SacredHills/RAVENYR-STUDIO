#pragma once
#include "core/TempoMap.h"
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "core/ProjectFile.h"
#include "EngineHost.h"

namespace ff
{
/** The open project: ValueTree + UndoManager + bundle location, autosave, recent
    files and crash-recovery session lock. Pushes musical settings to the engine. */
class ProjectDocument : private juce::ValueTree::Listener, private juce::Timer
{
public:
    ProjectDocument (EngineHost&, juce::PropertiesFile& settings);
    ~ProjectDocument() override;

    juce::ValueTree& tree() noexcept { return root; }
    juce::UndoManager& undo() noexcept { return undoManager; }
    juce::ValueTree transport() { return root.getChildWithName (project::ids::TRANSPORT); }
    juce::ValueTree metronome() { return root.getChildWithName (project::ids::METRONOME); }

    bool hasLocation() const { return bundle != juce::File(); }
    juce::File getBundle() const { return bundle; }
    /** Folder media is read from / recorded into: the bundle, or a scratch bundle while untitled. */
    juce::File mediaBundle() const;
    juce::ValueTree tracks() { return root.getChildWithName (project::ids::TRACKS); }
    juce::ValueTree markers() { return root.getChildWithName (project::ids::MARKERS); }
    bool isDirty() const { return dirty; }
    /** View state saved with the project (last page, open rack modules): set without marking the project changed. */
    void setUiProperty (juce::ValueTree node, const juce::Identifier& id, const juce::var& v)
    {
        if (node.isValid() && node[id] != v) { silentUi = true; node.setProperty (id, v, nullptr); silentUi = false; }
    }
    juce::String displayName() const;

    /** Re-sends tempo, meter, metronome and loop settings to the engine (after a temporary override). */
    void refreshEngine() { pushAllToEngine(); }

    void newUntitled();
    juce::Result openBundle (const juce::File& bundleDir);
    juce::Result saveTo (const juce::File& bundleDir);   // Save As (also used for first save)
    juce::Result save();                                  // requires hasLocation()

    // ---- typed accessors (undoable edits) ----
    double tempo() { return (double) transport()[project::ids::bpm]; }
    int tsNum() { return (int) transport()[project::ids::tsNum]; }
    int tsDen() { return (int) transport()[project::ids::tsDen]; }
    void setTempo (double bpm);
    /** The tempo track: tempo points (TEMPO {beat (quarter notes), bpm} children of the transport) on top of the base tempo. */
    TempoMap tempoMap();
    /** Adds a tempo point at `quarter` (quarter notes from the start; at 0 it is the project tempo). A second point at the
        same quarter makes an instant change. */
    void addTempoPoint (double quarter, double bpm);
    void removeTempoPoint (juce::ValueTree point);
    void setTimeSignature (int num, int den);
    /** Generic undoable property edit. */
    void set (juce::ValueTree node, const juce::Identifier& id, const juce::var& value, bool undoable = true);

    // ---- recent files ----
    juce::RecentlyOpenedFilesList recent;

    // ---- autosave & recovery ----
    void setAutosaveIntervalSeconds (int seconds);
    int getAutosaveIntervalSeconds() const { return autosaveSeconds; }
    juce::Time lastAutosave() const { return lastAutosaveTime; }

    struct Recovery { juce::File bundle; bool untitled = false; };
    /** Call once at startup: finds an autosave left behind by a crashed session. */
    std::optional<Recovery> findCrashRecovery();
    juce::Result recoverFrom (const Recovery&);
    /** Starts writing the session lock (call after findCrashRecovery()). */
    void beginSession() { sessionStarted = true; writeSessionLock(); }
    /** Clean shutdown: removes the lock and the untitled recovery autosave. */
    void endSession();

    std::function<void()> onStateChanged;  // title/dirty/location changed
    std::function<void()> onTreeReplaced;  // whole project replaced (pages must rebind)

    static juce::File appDataDir();

private:
    EngineHost& host;
    juce::PropertiesFile& settings;
    juce::ValueTree root;
    juce::UndoManager undoManager { 0, 60 };
    juce::File bundle;
    bool dirty = false;
    bool silentUi = false;
    bool autosaveDirty = false;
    int autosaveSeconds = 120;
    juce::Time lastAutosaveTime;
    bool sessionStarted = false;
    // autosave runs on a background thread from a snapshot of the tree, so big projects never pause the UI
    juce::ThreadPool autosaver { juce::ThreadPoolOptions().withThreadName ("Autosave").withNumberOfThreads (1) };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::atomic<bool> autosaveRunning { false };
    void waitForAutosave() { autosaver.removeAllJobs (false, 10000); }
    void writeSessionLock();
    void wipeUntitledMedia();
    juce::Result copyMediaTo (const juce::File& newBundle);

    void replaceTree (juce::ValueTree newTree, const juce::File& newBundle, bool markDirty);
    void pushAllToEngine();
    /** The tempo track changed: MIDI clips are musical, so they are re-timed from the old tempo map to the new one. */
    void tempoEdited();
    void retimeMidi (const TempoMap& from, const TempoMap& to);
    TempoMap lastMap;
    bool haveLastMap = false;
    void markDirty();
    void timerCallback() override;
    juce::File untitledRecoveryBundle() const;
    juce::File sessionLockFile() const;
    void addToRecent (const juce::File&);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree&) override { if (p.getType() == project::ids::TRANSPORT) tempoEdited(); markDirty(); }
    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree&, int) override { if (p.getType() == project::ids::TRANSPORT) tempoEdited(); markDirty(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { markDirty(); }
};
} // namespace ff
