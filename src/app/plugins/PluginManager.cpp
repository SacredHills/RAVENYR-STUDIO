#include "PluginManager.h"
#include "../ToolWindow.h"
#include "../StudioLookAndFeel.h"
#include "../GlobalKeys.h"
#include "../HardExit.h"
#include "engine/fx/BuiltinPlugins.h"
#include <condition_variable>
#include <queue>

namespace ff
{
// ===================================================================== child process side

class PluginScannerWorker final : private juce::ChildProcessWorker, private juce::AsyncUpdater
{
public:
    PluginScannerWorker() { juce::addDefaultFormatsToManager (formats); }
    using juce::ChildProcessWorker::initialiseFromCommandLine;

private:
    juce::AudioPluginFormatManager formats;
    std::mutex mutex;
    std::queue<juce::MemoryBlock> pending;

    void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override
    {
        if (mb.isEmpty()) return;
        const std::lock_guard<std::mutex> lock (mutex);
        if (auto results = scan (mb); ! results.isEmpty()) send (results);
        else { pending.push (mb); triggerAsyncUpdate(); }   // formats that need the message thread
    }

    // The app closed, finished scanning or gave up on a plugin that hangs: end this scanner process at once. (A quit
    // message would wait for the message thread, which a hung plugin never gives back: the scanner would stay in the
    // task manager for ever.)
    void handleConnectionLost() override { hardExit (0); }

    void handleAsyncUpdate() override
    {
        for (;;)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (pending.empty()) return;
            send (scan (pending.front()));
            pending.pop();
        }
    }

    juce::OwnedArray<juce::PluginDescription> scan (const juce::MemoryBlock& block)
    {
        juce::MemoryInputStream in (block, false);
        const auto formatName = in.readString();
        const auto identifier = in.readString();
        juce::PluginDescription pd;
        pd.fileOrIdentifier = identifier;
        juce::OwnedArray<juce::PluginDescription> results;
        for (auto* f : formats.getFormats())
            if (f->getName() == formatName
                && (juce::MessageManager::getInstance()->isThisTheMessageThread() || f->requiresUnblockedMessageThreadDuringCreation (pd)))
                f->findAllTypesForFile (results, identifier);
        return results;
    }

    void send (const juce::OwnedArray<juce::PluginDescription>& results)
    {
        juce::XmlElement xml ("LIST");
        for (auto* d : results) xml.addChildElement (d->createXml().release());
        const auto s = xml.toString();
        sendMessageToCoordinator ({ s.toRawUTF8(), s.getNumBytesAsUTF8() });
    }
};

PluginScannerWorker* startPluginScannerWorker (const juce::String& commandLine)
{
    auto w = std::make_unique<PluginScannerWorker>();
    if (w->initialiseFromCommandLine (commandLine, pluginScannerProcessId)) return w.release();
    return nullptr;
}

void destroyPluginScannerWorker (PluginScannerWorker* w) { delete w; }

// ===================================================================== app side: out-of-process scanning

namespace
{
class ScannerProcess final : private juce::ChildProcessCoordinator
{
public:
    ScannerProcess()
    {
        launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), pluginScannerProcessId, 0, 0);
    }
    enum class State { timeout, gotResult, lost };
    std::pair<State, std::unique_ptr<juce::XmlElement>> response()
    {
        std::unique_lock<std::mutex> lock (mutex);
        if (! cv.wait_for (lock, std::chrono::milliseconds (50), [this] { return got || lost; })) return { State::timeout, nullptr };
        const auto st = lost ? State::lost : State::gotResult;
        got = lost = false;
        return { st, std::move (xml) };
    }
    using juce::ChildProcessCoordinator::sendMessageToWorker;
private:
    void handleMessageFromWorker (const juce::MemoryBlock& mb) override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        xml = juce::parseXML (mb.toString());
        got = true;
        cv.notify_one();
    }
    void handleConnectionLost() override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        lost = true;
        cv.notify_one();
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::unique_ptr<juce::XmlElement> xml;
    bool got = false, lost = false;
};

class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    bool findPluginTypesFor (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        if (process == nullptr) process = std::make_unique<ScannerProcess>();
        juce::MemoryBlock block;
        {
            juce::MemoryOutputStream out (block, true);
            out.writeString (format.getName());
            out.writeString (fileOrIdentifier);
        }
        if (! process->sendMessageToWorker (block)) { process = nullptr; return false; }
        const auto started = juce::Time::getMillisecondCounter();
        for (;;)
        {
            if (shouldExit()) return true;
            auto [state, xml] = process->response();
            if (state == ScannerProcess::State::timeout)
            {
                if (juce::Time::getMillisecondCounter() - started > 60000) { process = nullptr; return false; }   // hung plugin
                continue;
            }
            if (xml != nullptr)
                for (auto* item : xml->getChildIterator())
                {
                    auto d = std::make_unique<juce::PluginDescription>();
                    if (d->loadFromXml (*item)) result.add (std::move (d));
                }
            if (state == ScannerProcess::State::lost) { process = nullptr; return false; }   // it crashed: mark as failed
            return true;
        }
    }
    void scanFinished() override { process = nullptr; }
private:
    std::unique_ptr<ScannerProcess> process;
};
}

// ===================================================================== undo

namespace
{
/** One change of a plugin's settings, undone / redone by restoring its whole state. */
struct PluginStateAction final : public juce::UndoableAction
{
    PluginStateAction (std::weak_ptr<juce::AudioPluginInstance> p, juce::String b, juce::String a, std::function<void (juce::AudioPluginInstance*, const juce::String&)> restored)
        : plugin (std::move (p)), before (std::move (b)), after (std::move (a)), onRestored (std::move (restored)) {}
    bool perform() override
    {
        if (first) { first = false; return true; }   // already happened in the plugin
        return apply (after);
    }
    bool undo() override { return apply (before); }
    int getSizeInUnits() override { return (int) (before.length() + after.length()) / 64 + 1; }
    bool apply (const juce::String& st)
    {
        if (auto p = plugin.lock())
        {
            if (onRestored) onRestored (p.get(), st);   // the window stops listening while the state is put back
            fx::restorePluginState (*p, st);
        }
        return true;
    }
    std::weak_ptr<juce::AudioPluginInstance> plugin;
    juce::String before, after;
    std::function<void (juce::AudioPluginInstance*, const juce::String&)> onRestored;
    bool first = true;
};
constexpr int maxUndoStateChars = 4 * 1024 * 1024;   // plugins with huge states (samplers) are not tracked
} // namespace

// ===================================================================== editor windows

/** Copy / Paste bar above a plugin's own editor. */
struct PluginManager::EditorWindow final : public juce::DocumentWindow
{
    struct Content final : public juce::Component, private juce::ComponentListener, private juce::Timer
    {
        Content (PluginManager& m, std::shared_ptr<juce::AudioPluginInstance> sp, juce::AudioProcessorEditor& e, BypassControl bp)
            : manager (m), plugin (*sp), weakPlugin (sp), editor (e), bypassControl (std::move (bp))
        {
            undoButton.setTooltip ("Undo (Ctrl+Z): changes in plugin windows are part of the project's undo history");
            redoButton.setTooltip ("Redo (Ctrl+Y / Ctrl+Shift+Z)");
            undoButton.onClick = [this] { manager.commitPendingChanges(); if (auto* um = undoManager()) um->undo(); };
            redoButton.onClick = [this] { manager.commitPendingChanges(); if (auto* um = undoManager()) um->redo(); };
            addAndMakeVisible (undoButton);
            addAndMakeVisible (redoButton);
            baseline = fx::capturePluginState (plugin);
            tracker.owner = this;
            plugin.addListener (&tracker);
            startTimerHz (10);
            presets.setTooltip ("Presets: load one, save the current settings as your own preset, or load a preset file");
            presets.onClick = [this] {
                juce::Component::SafePointer<Content> safe (this);
                manager.showPresetMenu (presets, plugin, [safe] (const juce::String& msg) {
                    if (safe != nullptr) safe->status.setText (msg, juce::dontSendNotification);
                });
            };
            addAndMakeVisible (presets);
            bypass.setClickingTogglesState (false);
            bypass.setColour (juce::TextButton::buttonOnColourId, palette::warn);
            bypass.setTooltip ("Bypass: the sound passes through untouched while this is lit");
            bypass.onClick = [this] { if (bypassControl.toggle) bypassControl.toggle(); syncBypass(); };
            if (bypassControl.isBypassed) { addAndMakeVisible (bypass); syncBypass(); }
            mute.setClickingTogglesState (false);
            mute.setColour (juce::TextButton::buttonOnColourId, palette::accent);
            mute.setTooltip ("Mute this track");
            mute.onClick = [this] { if (bypassControl.toggleMute) bypassControl.toggleMute(); syncBypass(); };
            solo.setClickingTogglesState (false);
            solo.setColour (juce::TextButton::buttonOnColourId, palette::warn);
            solo.setTooltip ("Solo this track");
            solo.onClick = [this] { if (bypassControl.toggleSolo) bypassControl.toggleSolo(); syncBypass(); };
            if (bypassControl.isMuted) addAndMakeVisible (mute);
            if (bypassControl.isSoloed) addAndMakeVisible (solo);
            pin.setClickingTogglesState (true);
            pin.setColour (juce::TextButton::buttonOnColourId, palette::warn);
            pin.setTooltip ("Pin: keep this plugin window on top of everything (and open) until you unpin it");
            pin.onClick = [this] { if (onPin) onPin (pin.getToggleState()); };
            for (auto* b : { &copy, &paste, &presets, &bypass, &mute, &solo, &undoButton, &redoButton, &pin })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (*b);
            }
            copy.setTooltip ("Copy this plugin's settings (paste them into the same plugin on another track, or use "
                             "\"Paste plugin\" on a track's + Plugin menu)");
            paste.setTooltip ("Paste copied settings of the same plugin");
            copy.onClick = [this] {
                manager.copySettings (plugin);
                status.setText ("Settings copied", juce::dontSendNotification);
                refresh();
            };
            paste.onClick = [this] {
                juce::String err;
                const auto before = fx::capturePluginState (plugin);
                const bool ok = manager.pasteSettings (plugin, err);
                if (ok) manager.recordChange (weakPlugin.lock(), before, fx::capturePluginState (plugin), "Paste plugin settings");
                status.setText (ok ? juce::String ("Settings pasted") : err, juce::dontSendNotification);
            };
            status.setColour (juce::Label::textColourId, palette::textDim);
            status.setFont (uiFont (12.0f));
            addAndMakeVisible (status);
            addAndMakeVisible (editor);
            editor.addComponentListener (this);
            refresh();
            fit();
        }
        ~Content() override
        {
            commitPending();
            fx::markPluginStateChanged (plugin);   // whatever was changed in the window gets saved
            plugin.removeListener (&tracker);
            editor.removeComponentListener (this);
        }

        // ---- undo tracking ----
        struct Tracker final : public juce::AudioProcessorListener
        {
            Content* owner = nullptr;
            std::atomic<bool> dirty { false }, gestureEnded { false };
            std::atomic<int> gestures { 0 };
            std::atomic<juce::uint32> lastChange { 0 }, quietUntil { 0 };
            void touched()
            {
                const auto now = juce::Time::getMillisecondCounter();
                if (now < quietUntil.load()) return;   // the state is being put back by undo / redo / paste
                dirty = true;
                lastChange = now;
            }
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { touched(); }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
            {
                if (d.parameterInfoChanged || d.programChanged || d.nonParameterStateChanged) touched();
            }
            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++gestures; }
            void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override
            {
                if (--gestures <= 0) { gestures = 0; gestureEnded = true; }
            }
        } tracker;
        juce::String baseline;   // the settings as of the last undo step

        juce::UndoManager* undoManager() const { return manager.getUndoManager ? manager.getUndoManager() : nullptr; }
        /** Settings were put back (undo / redo / paste / preset): they are the new starting point. */
        void restored (const juce::String& st)
        {
            tracker.quietUntil = juce::Time::getMillisecondCounter() + 400;
            tracker.dirty = false;
            baseline = st;
        }
        void commitPending()
        {
            if (! tracker.dirty.exchange (false)) return;
            tracker.gestureEnded = false;
            const auto now = fx::capturePluginState (plugin);
            if (now == baseline) return;
            if (auto* um = undoManager(); um != nullptr && now.length() < maxUndoStateChars && baseline.length() < maxUndoStateChars)
            {
                um->beginNewTransaction ("Change " + plugin.getName());
                um->perform (new PluginStateAction (weakPlugin, baseline, now, manager.restoredCallback()));
            }
            baseline = now;
        }
        void pollUndo()
        {
            if (! tracker.dirty.load() || tracker.gestures.load() > 0) return;
            const auto quiet = juce::Time::getMillisecondCounter() - tracker.lastChange.load();
            if (tracker.gestureEnded.load() || quiet > 600) commitPending();   // a finished drag, or a click / wheel that has settled
        }
        void refresh()
        {
            const auto& c = manager.clipboard;
            paste.setEnabled (! c.empty() && c.identifier == plugin.getPluginDescription().createIdentifierString());
        }
        // The window is exactly as wide as the plugin; the bar above it shrinks its buttons for narrow plugins. Only a
        // plugin narrower than the smallest bar gets a margin, and is then centred.
        void fit() { setSize (juce::jmax (editor.getWidth(), minBarWidth), editor.getHeight() + bar); }
        void syncBypass()
        {
            if (bypassControl.isMuted) mute.setToggleState (bypassControl.isMuted(), juce::dontSendNotification);
            if (bypassControl.isSoloed) solo.setToggleState (bypassControl.isSoloed(), juce::dontSendNotification);
            if (! bypassControl.isBypassed) return;
            const bool on = bypassControl.isBypassed();
            bypass.setToggleState (on, juce::dontSendNotification);
            bypass.setButtonText (on ? "Bypassed" : "Bypass");
        }
        void timerCallback() override
        {
            syncBypass();
            pollUndo();
            if (++ticks % 50 == 0) fx::markPluginStateChanged (plugin);   // some plugins never tell the host about changes
        }
        int ticks = 0;
        void componentMovedOrResized (juce::Component&, bool, bool wasResized) override { if (wasResized && ! resizing) fit(); }
        void resized() override
        {
            auto r = getLocalBounds();
            auto top = r.removeFromTop (bar).reduced (6, 4);
            // full-size buttons when there is room, compact ones (arrows, narrower buttons, no status text) otherwise
            const bool hasBypass = bypass.isVisible();
            const int extra = (mute.isVisible() ? 36 : 0) + (solo.isVisible() ? 36 : 0);
            const bool compact = getWidth() < (hasBypass ? 440 : 350) + extra;
            undoButton.setButtonText (compact ? juce::String::fromUTF8 ("\xe2\x86\x90") : juce::String ("Undo"));
            redoButton.setButtonText (compact ? juce::String::fromUTF8 ("\xe2\x86\x92") : juce::String ("Redo"));
            const int gap = compact ? 3 : 10;
            undoButton.setBounds (top.removeFromLeft (compact ? 26 : 52));
            top.removeFromLeft (2);
            redoButton.setBounds (top.removeFromLeft (compact ? 26 : 52));
            top.removeFromLeft (gap);
            presets.setBounds (top.removeFromLeft (compact ? 58 : 74));
            top.removeFromLeft (compact ? 3 : 4);
            if (hasBypass) { bypass.setBounds (top.removeFromLeft (compact ? 66 : 78)); top.removeFromLeft (compact ? 3 : 4); }
            if (mute.isVisible()) { mute.setBounds (top.removeFromLeft (compact ? 26 : 34)); top.removeFromLeft (2); }
            if (solo.isVisible()) { solo.setBounds (top.removeFromLeft (compact ? 26 : 34)); top.removeFromLeft (2); }
            top.removeFromLeft (gap);
            copy.setBounds (top.removeFromLeft (compact ? 44 : 56));
            top.removeFromLeft (compact ? 3 : 4);
            paste.setBounds (top.removeFromLeft (compact ? 46 : 56));
            top.removeFromLeft (gap);
            pin.setBounds (top.removeFromRight (compact ? 38 : 46));
            top.removeFromRight (compact ? 3 : 6);
            status.setBounds (top);
            const juce::ScopedValueSetter<bool> svs (resizing, true);
            if (editor.isResizable() && editor.getWidth() >= minBarWidth)
            {
                editor.setBounds (r);
            }
            else editor.setTopLeftPosition (r.getX() + juce::jmax (0, (r.getWidth() - editor.getWidth()) / 2), r.getY());
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (palette::bg);
            g.setColour (palette::panel);
            g.fillRect (getLocalBounds().removeFromTop (bar));
        }
        void visibilityChanged() override { refresh(); }
        static constexpr int bar = 30, minBarWidth = 300;
        PluginManager& manager;
        juce::AudioPluginInstance& plugin;
        juce::AudioProcessorEditor& editor;
        juce::TextButton copy { "Copy" }, paste { "Paste" }, presets { "Presets" }, bypass { "Bypass" }, mute { "M" }, solo { "S" }, undoButton { "Undo" }, redoButton { "Redo" }, pin { "Pin" };
        std::function<void (bool)> onPin;   // the window stays on top of everything while pinned
        std::weak_ptr<juce::AudioPluginInstance> weakPlugin;
        BypassControl bypassControl;
        juce::Label status;
        bool resizing = false;
    };

    EditorWindow (PluginManager& m, std::shared_ptr<juce::AudioPluginInstance> p, const juce::String& title, std::function<void()> closed,
                  BypassControl bypass)
        : juce::DocumentWindow (title, palette::bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          manager (&m), plugin (std::move (p)), onClosed (std::move (closed))
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* ed = nullptr;
        {
            fx::PluginActivity activity (*plugin, "opening its window");
            ed = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;
        }
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*plugin);
        editor.reset (ed);
        content = std::make_unique<Content> (m, plugin, *ed, std::move (bypass));
        setContentNonOwned (content.get(), true);
        setResizable (ed->isResizable(), false);
        content->onPin = [this, &m, title] (bool on) {
            setAlwaysOnTop (on);
            if (on) { m.pinnedWindows.insert (title); toFront (true); } else m.pinnedWindows.erase (title);
        };
        if (m.pinnedWindows.count (title) != 0)   // pinned earlier: pinned again when the window is reopened
        {
            content->pin.setToggleState (true, juce::dontSendNotification);
            setAlwaysOnTop (true);
        }
        floating = std::make_unique<FloatAboveApp> (*this);
        centreWithSize (getWidth(), getHeight());
        if (ed->isResizable())
        {
            // never smaller than the plugin allows (its own minimum, or the size it opened at); larger is fine
            int minW = ed->getWidth(), minH = ed->getHeight();
            if (auto* c = ed->getConstrainer())
            {
                minW = juce::jmax (c->getMinimumWidth(), 1);
                minH = juce::jmax (c->getMinimumHeight(), 1);
            }
            const int extraW = getWidth() - content->getWidth(), extraH = getHeight() - content->getHeight();
            setResizeLimits (juce::jmax (minW, Content::minBarWidth) + extraW, minH + Content::bar + extraH, 10000, 10000);
            // the size it had when it was last closed
            const auto saved = juce::StringArray::fromTokens (m.settings.getValue (sizeKey()), ",", {});
            if (saved.size() == 2 && saved[0].getIntValue() > 100 && saved[1].getIntValue() > 100)
            {
                setSize (saved[0].getIntValue(), saved[1].getIntValue());
                centreWithSize (getWidth(), getHeight());
            }
        }
        setVisible (true);
    }
    juce::String sizeKey() const { return "pluginWinSize:" + plugin->getName(); }
    void rememberSize()
    {
        if (editor != nullptr && editor->isResizable() && manager != nullptr)
            manager->settings.setValue (sizeKey(), juce::String (getWidth()) + "," + juce::String (getHeight()));
    }
    ~EditorWindow() override
    {
        rememberSize();
        clearContentComponent();
        content.reset();
        if (editor != nullptr) plugin->editorBeingDeleted (editor.get());
        editor.reset();
    }
    void closeButtonPressed() override { rememberSize(); if (floating != nullptr) floating->aboutToHide(); if (onClosed) juce::MessageManager::callAsync (onClosed); }
    bool keyPressed (const juce::KeyPress& k) override { return globalkeys::forward (k) || juce::DocumentWindow::keyPressed (k); }
    void activeWindowStatusChanged() override { if (content != nullptr) content->refresh(); }

    PluginManager* manager = nullptr;
    std::shared_ptr<juce::AudioPluginInstance> plugin;   // keeps the plugin alive while its editor is open
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<Content> content;
    std::unique_ptr<FloatAboveApp> floating;
    std::function<void()> onClosed;
};

void PluginManager::copySettings (juce::AudioPluginInstance& p)
{
    const auto d = p.getPluginDescription();
    clipboard.identifier = d.createIdentifierString();
    clipboard.descXml = d.createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    clipboard.name = d.name;
    clipboard.state = fx::capturePluginState (p);
}

bool PluginManager::pasteSettings (juce::AudioPluginInstance& p, juce::String& error)
{
    if (clipboard.empty()) { error = "Nothing copied yet"; return false; }
    if (clipboard.identifier != p.getPluginDescription().createIdentifierString())
    {
        error = "Copied settings are for " + clipboard.name;
        return false;
    }
    fx::restorePluginState (p, clipboard.state);
    return true;
}

// ===================================================================== manager

PluginManager::PluginManager (juce::PropertiesFile& s) : settings (s)
{
    juce::addDefaultFormatsToManager (formats);
    formats.addFormat (std::make_unique<fx::builtin::Format>());
    if (auto xml = settings.getXmlValue ("knownPlugins")) known.recreateFromXml (*xml);
    known.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    known.addChangeListener (this);
    lastBlacklistSize = known.getBlacklistedFiles().size();
    deadMansPedal = settings.getFile().getSiblingFile ("PluginScanCrashGuard.txt");
    crashReportFile = settings.getFile().getSiblingFile ("LastCrash.txt");
    previousCrash = fx::takePreviousCrashReport (crashReportFile);
    if (previousCrash.pluginName.isNotEmpty())   // don't let it crash again before the user has decided
        heldBack = previousCrash.fileOrIdentifier.isNotEmpty() ? previousCrash.fileOrIdentifier : previousCrash.pluginName;
    fx::installPluginCrashReporter (crashReportFile);
    fx::setPluginHost (this);
}

void PluginManager::reportPreviousCrash (juce::Component* parent)
{
    if (! previousCrash.exists) return;
    const auto r = previousCrash;
    previousCrash = {};
    const auto saved = crashReportFile.getSiblingFile ("LastCrash-previous.txt");
    if (r.pluginName.isEmpty())
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("RAVENYR closed unexpectedly")
                                          .withMessage ("RAVENYR closed unexpectedly last time. A crash report was saved to:\n" + saved.getFullPathName())
                                          .withButton ("OK").withAssociatedComponent (parent), nullptr);
        return;
    }
    const auto what = r.hang ? "stopped responding while \"" + r.pluginName + "\" was " + r.activity
                             : "closed unexpectedly while \"" + r.pluginName + "\" was " + r.activity;
    std::weak_ptr<bool> token = alive;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("Plugin problem")
                                      .withMessage ("Last time RAVENYR " + what + ".\n\nDisable this plugin? Tracks and rigs that use it keep "
                                                    "their settings but leave it out until you enable it again (Plugins > Options).\n\n"
                                                    "Report: " + saved.getFullPathName())
                                      .withButton ("Disable plugin").withButton ("Keep it").withAssociatedComponent (parent),
                                  [this, token, r] (int result) {
        if (token.expired()) return;
        if (result == 1) disablePlugin (heldBack);
        heldBack.clear();
        if (result != 1 && onPluginsReenabled) onPluginsReenabled();
    });
}

void PluginManager::disablePlugin (const juce::String& fileOrIdentifier)
{
    for (auto& d : known.getTypes())
        if (d.fileOrIdentifier == fileOrIdentifier || d.name == fileOrIdentifier)
        {
            known.removeType (d);
            known.addToBlacklist (d.fileOrIdentifier);
            return;
        }
    known.addToBlacklist (fileOrIdentifier);
}

PluginManager::~PluginManager()
{
    fx::setPluginHost (nullptr);
    fx::shutdownPluginCrashReporter();
    *alive = false;
    closeAllEditors();
    managerWindow.reset();
    known.removeChangeListener (this);
}

void PluginManager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = known.createXml()) settings.setValue ("knownPlugins", xml.get());
    const int blacklisted = known.getBlacklistedFiles().size();
    if (blacklisted < lastBlacklistSize && onPluginsReenabled) onPluginsReenabled();   // a plugin was re-enabled
    lastBlacklistSize = blacklisted;
}

void PluginManager::menuTypes (bool instruments, juce::Array<juce::PluginDescription>& display,
                               juce::Array<juce::PluginDescription>& originals) const
{
    display.clear();
    originals.clear();
    // one entry per distinct plugin: the same plugin found in two folders (or scanned twice) is listed once
    juce::StringArray seen;
    for (auto& d : known.getTypes())
    {
        // instruments that also take audio in (drum replacers / triggers such as Thruma) work as inserts too
        const bool fits = d.isInstrument == instruments || (! instruments && d.isInstrument && d.numInputChannels > 0);
        if (! fits) continue;
        const auto key = d.name.trim().toLowerCase() + "|" + d.pluginFormatName + "|" + d.manufacturerName.trim().toLowerCase() + "|"
                         + d.version + "|" + juce::String (d.uniqueId) + "|" + juce::String (d.numInputChannels) + "/" + juce::String (d.numOutputChannels);
        if (seen.contains (key)) continue;
        seen.add (key);
        originals.add (d);
    }
    // plugins that share a name but are different get what differs added to the name, so they can be told apart
    display = originals;
    for (int i = 0; i < display.size(); ++i)
    {
        juce::Array<int> same;
        for (int k = 0; k < originals.size(); ++k)
            if (originals[k].name.trim().equalsIgnoreCase (originals[i].name.trim())) same.add (k);
        if (same.size() < 2) continue;
        auto differs = [&] (auto field) {
            for (int k : same) if (field (originals[k]) != field (originals[i])) return true;
            return false;
        };
        juce::StringArray extra;
        if (differs ([] (const juce::PluginDescription& d) { return d.pluginFormatName; })) extra.add (originals[i].pluginFormatName);
        if (differs ([] (const juce::PluginDescription& d) { return d.version; }) && originals[i].version.isNotEmpty()) extra.add ("v" + originals[i].version);
        if (differs ([] (const juce::PluginDescription& d) { return juce::String (d.numInputChannels) + "/" + juce::String (d.numOutputChannels); }))
            extra.add (juce::String (originals[i].numInputChannels) + " in / " + juce::String (originals[i].numOutputChannels) + " out");
        if (extra.isEmpty() && differs ([] (const juce::PluginDescription& d) { return d.manufacturerName; }) && originals[i].manufacturerName.isNotEmpty())
            extra.add (originals[i].manufacturerName);
        if (extra.isEmpty())   // nothing visible differs (another id): show where each one is installed
            extra.add (juce::File::isAbsolutePath (originals[i].fileOrIdentifier)
                           ? juce::File (originals[i].fileOrIdentifier).getParentDirectory().getFileName() : originals[i].fileOrIdentifier);
        display.getReference (i).name = originals[i].name + "  (" + extra.joinIntoString (", ") + ")";
    }
}

void PluginManager::addToMenu (juce::PopupMenu& menu, int firstId) const
{
    const bool instruments = firstId == instrumentMenu;
    menu.addSectionHeader ("RAVENYR");
    int i = 0;
    for (auto& b : fx::builtin::all())
    {
        if (b.instrument == instruments) menu.addItem (builtinMenuBase + i, b.name);
        ++i;
    }
    juce::Array<juce::PluginDescription> list, originals;
    menuTypes (instruments, list, originals);
    if (list.isEmpty())
    {
        if (instruments) menu.addItem (-1, "(scan your plugin folders to list your VST / AU instruments)", false);
        return;
    }
    menu.addSectionHeader (instruments ? "Your instruments" : "Your plugins");
    juce::KnownPluginList::addToMenu (menu, list, juce::KnownPluginList::sortByManufacturer);
}

juce::String PluginManager::descriptionForMenuResult (int result, int firstId, juce::String& name) const
{
    const bool instruments = firstId == instrumentMenu;
    if (const auto& b = fx::builtin::all(); result >= builtinMenuBase && result < builtinMenuBase + (int) b.size())
    {
        name = b[(size_t) (result - builtinMenuBase)].name;
        return fx::builtin::descriptionXml (b[(size_t) (result - builtinMenuBase)].id);
    }
    juce::Array<juce::PluginDescription> shown, effects;
    menuTypes (instruments, shown, effects);   // the same list the menu was built from
    const int idx = juce::KnownPluginList::getIndexChosenByMenu (shown, result);
    if (! juce::isPositiveAndBelow (idx, effects.size())) return {};
    name = effects[idx].name;
    return effects[idx].createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
}

namespace
{
/** The plugin manager window's content: the folders to scan per format (edited in a normal window, so the folder
    chooser is never opened from behind a modal dialog), and the list of known plugins. */
class ManagerContent final : public juce::Component, private juce::ListBoxModel
{
public:
    ManagerContent (juce::AudioPluginFormatManager& fm, juce::KnownPluginList& kl, const juce::File& pedal, juce::PropertiesFile& props)
        : formats (fm), settings (props), list (fm, kl, pedal, &props, true)
    {
        for (auto* f : formats.getFormats())
            if (f->canScanForPlugins() && f->getDefaultLocationsToSearch().getNumPaths() > 0)
                pathFormats.add (f);

        title.setText ("Plugin folders", juce::dontSendNotification);
        title.setFont (uiFont (14.0f, true));
        addAndMakeVisible (title);
        for (int i = 0; i < pathFormats.size(); ++i) formatBox.addItem (pathFormats[i]->getName(), i + 1);
        formatBox.onChange = [this] { loadPath(); };
        addAndMakeVisible (formatBox);
        folders.setModel (this);
        folders.setRowHeight (22);
        folders.setColour (juce::ListBox::backgroundColourId, palette::well);
        addAndMakeVisible (folders);

        addFolder.onClick = [this] { chooseFolder(); };
        removeFolder.onClick = [this] {
            const int r = folders.getSelectedRow();
            if (r < 0 || r >= path.getNumPaths()) return;
            path.remove (r);
            savePath();
        };
        defaults.onClick = [this] {
            if (auto* f = currentFormat()) { path = f->getDefaultLocationsToSearch(); savePath(); }
        };
        scanButton.onClick = [this] { scanCurrent(); };
        for (auto* b : { &addFolder, &removeFolder, &defaults, &scanButton }) addAndMakeVisible (*b);
        scanButton.setColour (juce::TextButton::buttonColourId, palette::accentDim);

        hint.setText ("Pick a format, add the folders your plugins are installed in, then Scan. "
                      "Plugins that crash while scanning are skipped and marked as failed.", juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, palette::textDim);
        hint.setFont (uiFont (12.0f));
        addAndMakeVisible (hint);

        list.setOptionsButtonText ("Options...");
        list.getOptionsButton().onClick = [this] { showOptions(); };
        addAndMakeVisible (list);

        if (pathFormats.isEmpty()) { formatBox.setEnabled (false); addFolder.setEnabled (false); scanButton.setEnabled (false); }
        else formatBox.setSelectedId (1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (26);
        title.setBounds (top.removeFromLeft (120));
        formatBox.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (8);
        scanButton.setBounds (top.removeFromRight (90));
        top.removeFromRight (6);
        defaults.setBounds (top.removeFromRight (80));
        top.removeFromRight (6);
        removeFolder.setBounds (top.removeFromRight (80));
        top.removeFromRight (6);
        addFolder.setBounds (top.removeFromRight (100));
        r.removeFromTop (6);
        folders.setBounds (r.removeFromTop (110));
        r.removeFromTop (4);
        hint.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override { g.fillAll (palette::bg); }

private:
    juce::AudioPluginFormatManager& formats;
    juce::PropertiesFile& settings;
    juce::PluginListComponent list;
    juce::Array<juce::AudioPluginFormat*> pathFormats;
    juce::FileSearchPath path;
    juce::Label title, hint;
    juce::ComboBox formatBox;
    juce::ListBox folders;
    juce::TextButton addFolder { "Add folder..." }, removeFolder { "Remove" }, defaults { "Defaults" }, scanButton { "Scan" };
    std::unique_ptr<juce::FileChooser> chooser;

    juce::AudioPluginFormat* currentFormat() const { return pathFormats[formatBox.getSelectedId() - 1]; }

    void loadPath()
    {
        if (auto* f = currentFormat()) path = juce::PluginListComponent::getLastSearchPath (settings, *f);
        folders.updateContent();
        folders.repaint();
    }
    void savePath()
    {
        if (auto* f = currentFormat()) juce::PluginListComponent::setLastSearchPath (settings, *f, path);
        settings.saveIfNeeded();
        loadPath();
    }
    void chooseFolder()
    {
        auto start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        for (int i = 0; i < path.getNumPaths(); ++i)
            if (path[i].isDirectory()) { start = path[i]; break; }
        chooser = std::make_unique<juce::FileChooser> ("Add a plugin folder", start);
        juce::Component::SafePointer<ManagerContent> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr) return;
            const auto dir = fc.getResult();
            if (dir == juce::File() || ! dir.isDirectory()) return;
            safe->path.add (dir);
            safe->path.removeRedundantPaths();
            safe->savePath();
        });
    }
    void scanCurrent()
    {
        auto* f = currentFormat();
        if (f == nullptr || list.isScanning()) return;
        auto files = f->searchPathsForPlugins (path, true, false);
        if (files.isEmpty())
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Scan").withMessage ("No " + f->getName()
                                              + " plugins were found in these folders.").withButton ("OK").withAssociatedComponent (this), nullptr);
            return;
        }
        list.scanFor (*f, files);
    }
    void showOptions()
    {
        juce::PopupMenu menu;
        const auto original = list.createOptionsMenu();
        for (juce::PopupMenu::MenuItemIterator it (original); it.next();)
        {
            auto& item = it.getItem();
            if (item.text.startsWith ("Scan for new")) continue;   // scanning with folders is done above
            if (item.isSeparator) menu.addSeparator(); else menu.addItem (item);
        }
        for (auto* f : formats.getFormats())   // formats without folders (e.g. Audio Units) scan the system list
            if (f->canScanForPlugins() && ! pathFormats.contains (f))
                menu.addItem ("Scan for " + f->getName() + " plugins", [this, f] { if (! list.isScanning()) list.scanFor (*f); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (list.getOptionsButton()).withDeletionCheck (*this));
    }

    int getNumRows() override { return path.getNumPaths(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected) g.fillAll (palette::accentDim);
        g.setColour (path[row].isDirectory() ? palette::text : palette::bad);
        g.setFont (uiFont (13.0f));
        g.drawText (path[row].getFullPathName() + (path[row].isDirectory() ? juce::String() : "   (missing)"),
                    8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    void deleteKeyPressed (int) override { removeFolder.triggerClick(); }
};
}

std::unique_ptr<juce::Component> PluginManager::createManagerPanel()
{
    return std::make_unique<ManagerContent> (formats, known, deadMansPedal, settings);
}

void PluginManager::showManager()
{
    if (onShowManager) { onShowManager(); return; }
    if (managerWindow == nullptr)
    {
        struct Window : juce::DocumentWindow
        {
            Window (juce::Component* c)
                : juce::DocumentWindow ("Plugins (" + juce::String (pluginFormatsLabel()) + ")", palette::bg,
                                        juce::DocumentWindow::closeButton)
            {
                setUsingNativeTitleBar (true);
                setContentOwned (c, true);
                setResizable (true, false);
                setResizeLimits (560, 420, 4000, 3000);
                centreWithSize (860, 640);
            }
            void closeButtonPressed() override { setVisible (false); }
        };
        managerWindow = std::make_unique<Window> (new ManagerContent (formats, known, deadMansPedal, settings));
    }
    managerWindow->setVisible (true);
    managerWindow->toFront (true);
}

void PluginManager::showEditor (std::shared_ptr<juce::AudioPluginInstance> p, const juce::String& title, BypassControl bypass)
{
    if (p == nullptr) return;
    auto* key = p.get();
    if (auto it = editors.find (key); it != editors.end()) { it->second->setVisible (true); it->second->toFront (true); return; }
    // opening another plugin closes the plugin windows that are open, except the pinned ones
    std::vector<juce::AudioPluginInstance*> others;
    for (auto& [other, w] : editors)
        if (pinnedWindows.count (w->getName()) == 0) others.push_back (other);
    for (auto* other : others) closeEditor (other);
    std::weak_ptr<bool> token = alive;
    editors[key] = std::make_unique<EditorWindow> (*this, p, title, [this, token, key] { if (! token.expired()) closeEditor (key); },
                                                   std::move (bypass));
}

std::shared_ptr<juce::AudioPluginInstance> PluginManager::sharedFor (juce::AudioPluginInstance* p)
{
    if (auto it = editors.find (p); it != editors.end()) return it->second->plugin;
    return {};
}

void PluginManager::commitPendingChanges()
{
    for (auto& [p, w] : editors)
        if (w->content != nullptr) w->content->commitPending();
}

std::function<void (juce::AudioPluginInstance*, const juce::String&)> PluginManager::restoredCallback()
{
    std::weak_ptr<bool> token = alive;
    return [this, token] (juce::AudioPluginInstance* p, const juce::String& st) {
        if (token.expired()) return;
        if (auto it = editors.find (p); it != editors.end() && it->second->content != nullptr) it->second->content->restored (st);
    };
}

void PluginManager::recordChange (std::shared_ptr<juce::AudioPluginInstance> p, const juce::String& before, const juce::String& after,
                                  const juce::String& name)
{
    if (p == nullptr || before == after) return;
    if (auto it = editors.find (p.get()); it != editors.end() && it->second->content != nullptr) it->second->content->restored (after);
    auto* um = getUndoManager ? getUndoManager() : nullptr;
    if (um == nullptr || before.length() >= maxUndoStateChars || after.length() >= maxUndoStateChars) return;
    um->beginNewTransaction (name);
    um->perform (new PluginStateAction (p, before, after, restoredCallback()));
}

// ===================================================================== presets

juce::File PluginManager::presetFolder (const juce::AudioPluginInstance& p)
{
    const auto app = juce::JUCEApplicationBase::getInstance() != nullptr ? juce::JUCEApplicationBase::getInstance()->getApplicationName()
                                                                         : juce::String ("RAVENYR");
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile (app).getChildFile ("Presets").getChildFile (juce::File::createLegalFileName (p.getPluginDescription().name));
}

juce::String PluginManager::savePreset (juce::AudioPluginInstance& p, const juce::File& f)
{
    const auto d = p.getPluginDescription();
    juce::XmlElement x ("RavenyrPreset");
    x.setAttribute ("version", 1);
    x.setAttribute ("name", f.getFileNameWithoutExtension());
    x.setAttribute ("pluginName", d.name);
    x.setAttribute ("pluginId", d.createIdentifierString());
    x.setAttribute ("format", d.pluginFormatName);
    x.createNewChildElement ("State")->addTextElement (fx::capturePluginState (p));
    f.getParentDirectory().createDirectory();
    if (! x.writeTo (f)) return "Could not write " + f.getFullPathName();
    return {};
}

juce::String PluginManager::loadPreset (juce::AudioPluginInstance& p, const juce::File& f)
{
    const auto x = juce::XmlDocument::parse (f);
    if (x == nullptr || ! (x->hasTagName ("RavenyrPreset") || x->hasTagName ("FretForgePreset"))) return f.getFileName() + " is not a preset file";
    const auto d = p.getPluginDescription();
    if (x->getStringAttribute ("pluginId") != d.createIdentifierString() && x->getStringAttribute ("pluginName") != d.name)
        return "That preset is for " + x->getStringAttribute ("pluginName");
    const auto* st = x->getChildByName ("State");
    if (st == nullptr) return f.getFileName() + " has no settings in it";
    fx::restorePluginState (p, st->getAllSubText().trim());
    return {};
}

void PluginManager::showPresetMenu (juce::Component& anchor, juce::AudioPluginInstance& plugin, std::function<void (const juce::String&)> status)
{
    const auto builtinId = plugin.getPluginDescription().pluginFormatName == fx::builtin::formatName
                               ? plugin.getPluginDescription().fileOrIdentifier : juce::String();
    const auto& factory = fx::builtin::factoryPresets (builtinId);
    const auto folder = presetFolder (plugin);
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.rvnpreset;*.ffpreset");
    files.sort();

    juce::PopupMenu m;
    if (! factory.empty())
    {
        m.addSectionHeader ("Factory presets");
        std::map<juce::String, juce::PopupMenu> cats;
        juce::StringArray order;
        for (int i = 0; i < (int) factory.size(); ++i)
        {
            const auto& pr = factory[(size_t) i];
            if (pr.category.isEmpty()) { m.addItem (1000 + i, pr.name); continue; }
            if (! order.contains (pr.category)) order.add (pr.category);
            cats[pr.category].addItem (1000 + i, pr.name);
        }
        for (auto& c : order) m.addSubMenu (c, cats[c]);
    }
    m.addSectionHeader ("My presets");
    if (files.isEmpty()) m.addItem (-1, "(none saved yet)", false);
    for (int i = 0; i < files.size(); ++i) m.addItem (2000 + i, files[i].getFileNameWithoutExtension());
    m.addSeparator();
    m.addItem (1, "Save preset...");
    m.addItem (2, "Load preset file...");
    m.addItem (3, "Show preset folder");

    juce::Component::SafePointer<juce::Component> safeAnchor (&anchor);
    auto* pl = &plugin;
    std::weak_ptr<bool> token = alive;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor),
                     [this, token, safeAnchor, pl, factory, files, folder, status] (int r) {
        if (token.expired() || safeAnchor == nullptr || r == 0) return;
        if (r >= 2000 && r - 2000 < files.size())
        {
            const auto before = fx::capturePluginState (*pl);
            const auto err = loadPreset (*pl, files[r - 2000]);
            if (err.isEmpty()) recordChange (sharedFor (pl), before, fx::capturePluginState (*pl), "Load preset");
            status (err.isEmpty() ? "Loaded " + files[r - 2000].getFileNameWithoutExtension() : err);
        }
        else if (r >= 1000 && r - 1000 < (int) factory.size())
        {
            const auto before = fx::capturePluginState (*pl);
            fx::builtin::applyPreset (*pl, factory[(size_t) (r - 1000)]);
            recordChange (sharedFor (pl), before, fx::capturePluginState (*pl), "Load preset");
            status ("Preset: " + factory[(size_t) (r - 1000)].name);
        }
        else if (r == 1)
        {
            auto* w = new juce::AlertWindow ("Save preset", "Name for this preset of " + pl->getPluginDescription().name + ":",
                                             juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", "My preset");
            w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->setAlwaysOnTop (true);
            w->enterModalState (true, juce::ModalCallbackFunction::create ([w, pl, folder, status, token] (int res) {
                const auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
                if (res != 1 || name.isEmpty() || token.expired()) return;
                const auto f = folder.getChildFile (name + ".rvnpreset");
                const auto err = savePreset (*pl, f);
                status (err.isEmpty() ? "Saved preset " + name : err);
            }), true);
        }
        else if (r == 2)
        {
            presetChooser = std::make_unique<juce::FileChooser> ("Load preset", folder.exists() ? folder : folder.getParentDirectory(), "*.rvnpreset;*.ffpreset");
            presetChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                        [this, pl, status, token] (const juce::FileChooser& fc) {
                if (token.expired() || fc.getResult() == juce::File()) return;
                const auto before = fx::capturePluginState (*pl);
                const auto err = loadPreset (*pl, fc.getResult());
                if (err.isEmpty()) recordChange (sharedFor (pl), before, fx::capturePluginState (*pl), "Load preset");
                status (err.isEmpty() ? "Loaded " + fc.getResult().getFileNameWithoutExtension() : err);
            });
        }
        else if (r == 3)
        {
            folder.createDirectory();
            folder.startAsProcess();
        }
    });
}

void PluginManager::closeEditor (juce::AudioPluginInstance* p)
{
    editors.erase (p);
}

void PluginManager::closeAllEditors()
{
    editors.clear();
}

void PluginManager::createAsync (const juce::String& descXml, double sr, int bs,
                                 std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> done)
{
    juce::PluginDescription d;
    auto xml = juce::parseXML (descXml);
    if (xml == nullptr || ! d.loadFromXml (*xml)) { done (nullptr, "The plugin description is not valid."); return; }
    if (d.pluginFormatName == "FretForge") d.pluginFormatName = fx::builtin::formatName;   // saved before the rename
    if (heldBack.isNotEmpty() && (d.fileOrIdentifier == heldBack || d.name == heldBack))
    {
        done (nullptr, "\"" + d.name + "\" was held back because it crashed or froze RAVENYR last time.");
        return;
    }
    if (known.getBlacklistedFiles().contains (d.fileOrIdentifier))
    {
        done (nullptr, "\"" + d.name + "\" is disabled because it crashed or froze RAVENYR. Re-enable it under Plugins > Options.");
        return;
    }
    createQueue.push_back ({ d, sr, bs, std::move (done) });
    pumpCreateQueue();
}

void PluginManager::pumpCreateQueue()
{
    if (creating || createQueue.empty()) return;
    creating = true;
    auto job = std::move (createQueue.front());
    createQueue.pop_front();
    if (onLoadingPlugin) onLoadingPlugin (job.desc.name);
    std::weak_ptr<bool> token = alive;
    fx::PluginActivity activity (job.desc.name, job.desc.fileOrIdentifier, "loading");
    formats.createPluginInstanceAsync (job.desc, job.sampleRate, job.blockSize,
                                       [this, token, done = std::move (job.done), name = job.desc.name, id = job.desc.fileOrIdentifier]
                                       (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err) {
        if (token.expired()) return;
        fx::registerPluginInstance (inst.get(), name, id);
        const auto message = inst == nullptr && err.isEmpty() ? "\"" + name + "\" could not be loaded (is it still installed?)" : err;
        done (std::move (inst), message);
        if (token.expired()) return;
        creating = false;
        // the next one after the window has had a chance to repaint and take input
        juce::Timer::callAfterDelay (15, [this, token] { if (! token.expired()) pumpCreateQueue(); });
    });
}

std::unique_ptr<juce::AudioPluginInstance> PluginManager::createBlocking (const juce::String& descXml, double sr, int bs, juce::String& error)
{
    juce::PluginDescription d;
    auto xml = juce::parseXML (descXml);
    if (xml == nullptr || ! d.loadFromXml (*xml)) { error = "The plugin description is not valid."; return nullptr; }
    if (d.pluginFormatName == "FretForge") d.pluginFormatName = fx::builtin::formatName;   // saved before the rename
    if (heldBack.isNotEmpty() && (d.fileOrIdentifier == heldBack || d.name == heldBack)) { error = "\"" + d.name + "\" was held back because it crashed last time."; return nullptr; }
    if (known.getBlacklistedFiles().contains (d.fileOrIdentifier)) { error = "\"" + d.name + "\" is disabled because it crashed or froze RAVENYR."; return nullptr; }
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        fx::PluginActivity activity (d.name, d.fileOrIdentifier, "loading");
        auto inst = formats.createPluginInstance (d, sr, bs, error);
        fx::registerPluginInstance (inst.get(), d.name, d.fileOrIdentifier);
        return inst;
    }

    struct Shared { juce::WaitableEvent done; std::unique_ptr<juce::AudioPluginInstance> inst; juce::String err; };
    auto shared = std::make_shared<Shared>();
    juce::MessageManager::callAsync ([this, d, sr, bs, shared, token = std::weak_ptr<bool> (alive)] {
        if (token.expired()) { shared->done.signal(); return; }
        fx::PluginActivity activity (d.name, d.fileOrIdentifier, "loading");
        formats.createPluginInstanceAsync (d, sr, bs, [shared, d] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err) {
            fx::registerPluginInstance (inst.get(), d.name, d.fileOrIdentifier);
            shared->inst = std::move (inst);
            shared->err = err;
            shared->done.signal();
        });
    });
    if (! shared->done.wait (60000)) { error = "Timed out loading \"" + d.name + "\"."; return nullptr; }
    error = shared->err;
    return std::move (shared->inst);
}
} // namespace ff
