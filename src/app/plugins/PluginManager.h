#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "engine/fx/PluginHost.h"
#include <map>
#include <set>
#include <deque>

namespace ff
{
/** Command-line id that turns an instance of the app into a plugin-scanning child process. */
inline constexpr const char* pluginScannerProcessId = "fretforge-plugin-scanner";
/** The formats this build hosts, e.g. "VST3 / LV2" or "VST3 / AU / LV2". */
inline juce::String pluginFormatsLabel()
{
    juce::StringArray f { "VST3" };
   #if JUCE_PLUGINHOST_VST
    f.add ("VST");
   #endif
   #if JUCE_MAC
    f.add ("AU");
   #endif
   #if JUCE_PLUGINHOST_LV2
    f.add ("LV2");
   #endif
    return f.joinIntoString (" / ");
}

/** Runs in the child process: scans one plugin file per request and answers with its descriptions. A plugin that
    crashes while being scanned only takes this child process down; the main app marks it as failed and goes on. */
class PluginScannerWorker;
/** Returns a new worker (free it with destroyPluginScannerWorker) if this process was started as the scanner. */
PluginScannerWorker* startPluginScannerWorker (const juce::String& commandLine);
void destroyPluginScannerWorker (PluginScannerWorker*);
struct PluginScannerWorkerDeleter { void operator() (PluginScannerWorker* w) const { destroyPluginScannerWorker (w); } };

/** VST3 (and Audio Unit on macOS) hosting for the app: formats, the list of known plugins (scanned out of process,
    saved in the settings), instance creation for the rigs, plugin editor windows and the plugin manager window. */
class PluginManager : public fx::PluginHost, private juce::ChangeListener
{
public:
    explicit PluginManager (juce::PropertiesFile&);
    ~PluginManager() override;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    static constexpr int builtinMenuBase = 9000;
    static constexpr int instrumentMenu = -100;   // pass as firstId to list instruments instead of effects
    /** Menu of RAVENYR's own effects, then the known plugins (by manufacturer). */
    void addToMenu (juce::PopupMenu& menu, int firstId) const;
    /** The scanned plugins for a menu, one entry per distinct plugin (the same plugin installed twice is listed
        once). `display` has names made distinct where different plugins share one; originals[i] is what to load. */
    void menuTypes (bool instruments, juce::Array<juce::PluginDescription>& display, juce::Array<juce::PluginDescription>& originals) const;
    /** The plugin chosen from that menu, as description XML (empty if none). */
    juce::String descriptionForMenuResult (int result, int firstId, juce::String& name) const;

    void showManager();
    /** The plugin settings (scan folders, scan, the plugin list, disabled plugins) as a panel, for the Settings tab. */
    std::unique_ptr<juce::Component> createManagerPanel();
    /** Set by the main window: showManager() opens Settings > Plugins instead of a separate window. */
    std::function<void()> onShowManager;

    /** Copied plugin settings (the Copy / Paste buttons in every plugin window). */
    struct Clipboard { juce::String identifier, descXml, name, state; bool empty() const { return state.isEmpty() && descXml.isEmpty(); } };
    Clipboard clipboard;
    void copySettings (juce::AudioPluginInstance&);
    /** Pastes into the plugin if it is the same kind; returns false (and says why) otherwise. */
    bool pasteSettings (juce::AudioPluginInstance&, juce::String& error);
    /** If the last run crashed or hung inside a plugin, tells the user and offers to disable that plugin. */
    void reportPreviousCrash (juce::Component* parent);
    /** Removes a plugin from the menus and refuses to load it until it is taken off the blacklist. */
    void disablePlugin (const juce::String& fileOrIdentifier);
    /** Called when plugins may load again (a held-back plugin was kept, or the blacklist changed). */
    std::function<void()> onPluginsReenabled;
    /** Optional bypass switch shown in the plugin window (for track / master inserts). */
    struct BypassControl
    {
        std::function<bool()> isBypassed;
        std::function<void()> toggle;
        // optional: the track's mute / solo, shown beside Bypass
        std::function<bool()> isMuted, isSoloed;
        std::function<void()> toggleMute, toggleSolo;
    };
    void showEditor (std::shared_ptr<juce::AudioPluginInstance>, const juce::String& title, BypassControl bypass = {});

    // ---- presets (the Presets button in every plugin window) ----
    /** Where a plugin's own saved presets live: Documents/<app>/Presets/<plugin name>. */
    static juce::File presetFolder (const juce::AudioPluginInstance&);
    /** Saves the plugin's current settings as a .rvnpreset file. Returns an error message, empty on success. */
    static juce::String savePreset (juce::AudioPluginInstance&, const juce::File&);
    /** Loads a .rvnpreset (or older .ffpreset) file saved from the same plugin. Returns an error message, empty on success. */
    static juce::String loadPreset (juce::AudioPluginInstance&, const juce::File&);
    void showPresetMenu (juce::Component& anchor, juce::AudioPluginInstance&, std::function<void (const juce::String&)> status);

    // ---- undo: changes made in a plugin window are steps in the project's undo history ----
    std::function<juce::UndoManager*()> getUndoManager;
    /** Turns changes still settling in open plugin windows into undo steps now (before an undo / redo). */
    void commitPendingChanges();
    /** Records a settings change of `plugin` (e.g. a preset load) as one undo step. */
    void recordChange (std::shared_ptr<juce::AudioPluginInstance>, const juce::String& before, const juce::String& after, const juce::String& name);
    void closeEditor (juce::AudioPluginInstance*);
    void closeAllEditors();

    // fx::PluginHost
    void createAsync (const juce::String& descriptionXml, double sampleRate, int blockSize,
                      std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> done) override;
    /** Called right before a queued plugin starts loading (the UI shows what is loading, then repaints at once). */
    std::function<void (const juce::String& pluginName)> onLoadingPlugin;
    std::unique_ptr<juce::AudioPluginInstance> createBlocking (const juce::String& descriptionXml, double sampleRate, int blockSize,
                                                               juce::String& error) override;

private:
    juce::PropertiesFile& settings;
    juce::File deadMansPedal, crashReportFile;
    fx::PluginCrashReport previousCrash;
    int lastBlacklistSize = 0;
    juce::String heldBack;   // the plugin from the last crash, refused until the user decides
    std::unique_ptr<juce::DocumentWindow> managerWindow;
    struct EditorWindow;
    std::map<juce::AudioPluginInstance*, std::unique_ptr<EditorWindow>> editors;
    std::set<juce::String> pinnedWindows;   // titles of the plugin windows that are pinned on top
    std::unique_ptr<juce::FileChooser> presetChooser;
    std::function<void (juce::AudioPluginInstance*, const juce::String&)> restoredCallback();
    std::shared_ptr<juce::AudioPluginInstance> sharedFor (juce::AudioPluginInstance*);
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    // plugins load one at a time with the message loop breathing in between: a project with many plugins used to
    // load them back to back and the window could neither repaint nor react until the last one was done
    struct PendingCreate
    {
        juce::PluginDescription desc;
        double sampleRate = 48000.0;
        int blockSize = 512;
        std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> done;
    };
    std::deque<PendingCreate> createQueue;
    bool creating = false;
    void pumpCreateQueue();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
};
} // namespace ff
