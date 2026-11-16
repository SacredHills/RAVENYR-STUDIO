#pragma once
#include "AmpRig.h"
#include "InsertChain.h"
#include "InstrumentSlot.h"
#include <map>
#include <mutex>
#include <atomic>
#include <functional>

namespace ff { class AudioEngine; }

namespace ff::fx
{
/** Reads an impulse response (any audio file JUCE can read) as mono, at most `maxSeconds` long. */
bool loadImpulseFile (const juce::File&, juce::AudioBuffer<float>& out, double& sampleRate, juce::String& error,
                      double maxSeconds = 3.0);

/** Builds a ready-to-run rig for offline rendering: model and IR are loaded synchronously.
    Problems (missing files, bad model) are added to `warnings`; the rig then uses its built-in amp / cab. */
std::shared_ptr<AmpRig> makeOfflineRig (const juce::ValueTree& rigTree, double sampleRate, int maxBlock,
                                        juce::StringArray& warnings);
/** The same for a track's inserts (nullptr if the track has none). */
std::shared_ptr<InsertChain> makeOfflineInserts (const juce::ValueTree& track, double sampleRate, int maxBlock,
                                                 juce::StringArray& warnings);

/** The instrument of an instrument track for offline rendering (nullptr if none or it failed; then a warning). */
std::shared_ptr<InstrumentSlot> makeOfflineInstrument (const juce::ValueTree& track, double sampleRate, int maxBlock,
                                                       juce::StringArray& warnings);

/** Message-thread owner of the live rigs: one per track whose RIG is enabled, plus the live practice rig.

    Keeps the rigs in step with the project tree (parameters immediately, model / IR files loaded on a
    background thread and handed over without blocking the audio thread), re-prepares every rig when the audio
    device restarts and frees replaced models. */
class RigBank : private juce::ValueTree::Listener, private juce::Timer
{
public:
    explicit RigBank (AudioEngine&);
    ~RigBank() override;

    /** Project replaced. */
    void rebind (juce::ValueTree projectRoot);

    /** Rig for a track id (nullptr if the track has no enabled rig). */
    std::shared_ptr<AmpRig> rigForTrack (const juce::String& trackId) const;

    // ---- the live practice rig (input -> rig -> output, independent of tracks and transport) ----
    juce::ValueTree liveTree() const { return live; }
    /** Replaces the live rig's settings (from the app settings at start-up). */
    void setLiveTree (juce::ValueTree t);
    AmpRig& liveRig() { return *liveEntry.rig; }

    struct Status
    {
        juce::String modelPath, irPath, irPath2;
        NamInfo modelInfo;
        bool modelLoaded = false, modelLoading = false, irLoaded = false, irLoading = false;
        juce::String modelError, irError;
        juce::String irName, irName2, irError2;
        bool irLoaded2 = false, irLoading2 = false;
        double irSeconds = 0.0;
        struct Plugin { juce::String desc, name, error; bool loading = false, loaded = false; int latency = 0; };
        std::array<Plugin, rig::numPluginSlots> plugins;
    };
    /** Waits for plugins still being prepared on worker threads and hands them over (message thread). The running app
        does this by itself; tests call it instead of running a message loop. */
    void finishLoads();
    /** What is still loading (plugins, instruments, amp models, impulse responses), as short descriptions for a status line. */
    juce::StringArray loadingNames() const;
    /** Load status for a RIG node (a track's or the live one). */
    Status statusFor (const juce::ValueTree& rigTree) const;
    /** The running rig for a RIG node (nullptr if none). */
    AmpRig* rigFor (const juce::ValueTree& rigTree) const;

    /** The running plugin in a slot of a RIG node (for its editor window), nullptr if none. */
    std::shared_ptr<juce::AudioPluginInstance> pluginFor (const juce::ValueTree& rigTree, int slot) const;
    // ---- track inserts ----
    /** Insert chain for a track id (nullptr if the track has no inserts). */
    std::shared_ptr<InsertChain> insertsForTrack (const juce::String& trackId) const;
    /** Load status / running plugin of a track's insert slot (by its slot number in the INSERTS list). */
    Status::Plugin insertStatus (const juce::ValueTree& track, int slot) const;
    std::shared_ptr<juce::AudioPluginInstance> insertPlugin (const juce::ValueTree& track, int slot) const;

    // ---- instruments of instrument tracks (TRACK/INSTRUMENT { desc, name, state }) ----
    std::shared_ptr<InstrumentSlot> instrumentForTrack (const juce::String& trackId) const;
    Status::Plugin instrumentStatus (const juce::ValueTree& track) const;
    std::shared_ptr<juce::AudioPluginInstance> instrumentPlugin (const juce::ValueTree& track) const;

    /** Copies every plugin's current state into its PLUGIN node (before saving / exporting). With onlyChanged, only
        plugins that reported a change are asked (the periodic capture: asking every plugin can stall the app). */
    void captureAllPluginStates (bool onlyChanged = false);
    /** Tries again to load every plugin that failed to load (e.g. after it was re-enabled). */
    void retryFailedPlugins();

    std::function<void (juce::AudioPluginInstance*)> onPluginRemoved;   // close its editor window before it is deleted
    std::function<void()> onRigsChanged;    // a track rig was created or removed (rebuild the playback graph)
    std::function<void()> onStatusChanged;  // a model / IR finished loading or failed
    juce::ChangeBroadcaster statusChanged;  // the same, for any number of listeners (mixer strips)
    std::function<void()> onLiveChanged;    // the live rig's settings changed (persist them)

private:
    struct Entry
    {
        std::shared_ptr<AmpRig> rig;
        juce::ValueTree tree;
        Status status;
        int generation = 0;
        std::array<int, rig::numPluginSlots> pluginGeneration {};
    };

    struct InsertEntry
    {
        std::shared_ptr<InsertChain> chain;
        juce::ValueTree track;
        std::array<juce::ValueTree, InsertChain::maxSlots> nodes;   // PLUGIN node held by each physical slot
        std::array<Status::Plugin, InsertChain::maxSlots> status;
        std::array<int, InsertChain::maxSlots> generation {};
    };
    std::map<juce::String, InsertEntry> inserts;
    struct InstrumentEntry
    {
        std::shared_ptr<InstrumentSlot> slot;
        juce::ValueTree node;   // the INSTRUMENT node
        juce::String loadedDesc;
        int loadedAux = 0;
        Status::Plugin status;
        int generation = 0;
    };
    std::map<juce::String, InstrumentEntry> instruments;
    void loadInstrument (const juce::String& key);
    void syncInstruments (bool& changed);
    void syncInserts (InsertEntry&);
    void loadInsert (InsertEntry&, int physical);
    void releaseInsertPlugins (InsertEntry&);
    int physicalOf (const InsertEntry&, const juce::ValueTree& node) const;

    AudioEngine& engine;
    juce::ValueTree root, live;
    std::map<juce::String, Entry> tracks;
    Entry liveEntry;
    int generationCounter = 0;
    juce::dsp::ConvolutionMessageQueue convQueue { 64 };
    /** Loading a plugin's saved settings (samples!) and preparing it can take seconds: done on worker threads while the
        message thread keeps the window alive, then `then` runs on the message thread with the ready plugin. */
    juce::ThreadPool pluginPrep { juce::ThreadPoolOptions{}.withThreadName ("FF plugin prep").withNumberOfThreads (2) };
    std::mutex readyMutex;
    std::vector<std::function<void()>> readyQueue;   // finished preparations, run on the message thread
    std::atomic<int> prepJobs { 0 };
    void drainReady();
    void prepareAsync (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String state, double rate, int block, bool sidechain, int aux,
                       std::function<void (std::unique_ptr<juce::AudioPluginInstance>)> then);
    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withThreadName ("FF rig loader").withNumberOfThreads (1) };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    mutable juce::CriticalSection lock;   // serialises rig creation / model hand-over with prepareAll()

    void prepareAll (double sr, int block);
    void syncAll();
    void syncEntry (Entry&);
    void requestModel (Entry&, bool force);
    void requestIr (Entry&, bool second);
    void requestPlugin (Entry&, int slot);
    void releasePlugin (std::shared_ptr<juce::AudioPluginInstance>);
    /** Takes every plugin out of every rig / chain / instrument and frees them after one audio callback. */
    void releaseAllPlugins();
    void releaseEntryPlugins (Entry&);
    Entry* entryByKey (const juce::String& key);
    juce::String keyOf (const Entry&) const;
    int ticks = 0;
    void notifyStatus() { if (onStatusChanged) onStatusChanged(); statusChanged.sendChangeMessage(); }
    Entry* entryFor (const juce::ValueTree& rigTree);
    const Entry* entryFor (const juce::ValueTree& rigTree) const;
    std::shared_ptr<AmpRig> makeRig();

    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeRedirected (juce::ValueTree&) override { syncAll(); }
};
} // namespace ff::fx
