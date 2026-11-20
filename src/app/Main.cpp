#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "AppInfo.h"
#include "plugins/PluginManager.h"
#include "HardExit.h"

namespace ff
{
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (juce::PropertiesFile& settings)
        : DocumentWindow (FF_DISPLAY_NAME, palette::bg, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        auto* mc = new MainComponent (settings);
        content = mc;
        setContentOwned (mc, true);
        mc->onTitleChanged = [this] (const juce::String& t) { setName (t); };
        setResizable (true, true);
        setResizeLimits (1200, 720, 10000, 10000);

        if (auto state = settings.getValue ("windowState"); state.isNotEmpty())
            restoreWindowStateFromString (state);
        else
            centreWithSize (getWidth(), getHeight());
        setVisible (true);

        juce::Component::SafePointer<MainComponent> safe (mc);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->runStartupChecks(); });
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    MainComponent* content = nullptr;
};

class Application : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return FF_DISPLAY_NAME; }
    const juce::String getApplicationVersion() override { return FF_VERSION_STRING; }
    // the plugin scanner runs as a second copy of the app
    bool moreThanOneInstanceAllowed() override { return getCommandLineParameters().contains (pluginScannerProcessId); }

    void initialise (const juce::String& commandLine) override
    {
        if (auto* w = startPluginScannerWorker (commandLine))
        {
            scanner.reset (w);
            return;
        }

        migrateFromOldName();   // before anything creates the new folders (on Windows the log goes into the app data folder)
        logger.reset (juce::FileLogger::createDateStampedLogger (FF_APP_NAME, "session", ".log",
                                                                 juce::String (FF_APP_NAME) + " " + FF_VERSION_STRING + " starting on "
                                                                     + juce::SystemStats::getOperatingSystemName()));
        juce::Logger::setCurrentLogger (logger.get());

        juce::PropertiesFile::Options o;
        o.applicationName = FF_APP_NAME;
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.folderName = FF_APP_NAME;
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = 2000;
        properties.setStorageParameters (o);

        window = std::make_unique<MainWindow> (*properties.getUserSettings());
        openArgument (commandLine);
    }

    /** The app used to be called FretForge Studio: carry its settings, plugin list, rigs, presets and downloads over
        once (copies; the old folders are left alone). */
    static void migrateFromOldName()
    {
        const auto copyOnce = [] (const juce::File& from, const juce::File& to) {
            if (from.isDirectory() && ! to.exists()) from.copyDirectoryTo (to);
            else if (from.existsAsFile() && ! to.exists()) { to.getParentDirectory().createDirectory(); from.copyFileTo (to); }
        };
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        auto optionsFor = [] (const juce::String& name) {
            juce::PropertiesFile::Options o;
            o.applicationName = name;
            o.filenameSuffix = ".settings";
            o.osxLibrarySubFolder = "Application Support";
            o.folderName = name;
            return o;
        };
        const auto oldSettings = optionsFor ("FretForge Studio").getDefaultFile(), newSettings = optionsFor (FF_APP_NAME).getDefaultFile();
        copyOnce (appData.getChildFile ("FretForge Studio"), appData.getChildFile (FF_APP_NAME));   // plugin list, recovery, drum kit
        copyOnce (oldSettings, newSettings);
        copyOnce (appData.getChildFile ("FretForge").getChildFile ("Rigs"), appData.getChildFile (FF_APP_NAME).getChildFile ("Rigs"));
        copyOnce (docs.getChildFile ("FretForge Studio").getChildFile ("Presets"), docs.getChildFile (FF_APP_NAME).getChildFile ("Presets"));
        copyOnce (docs.getChildFile ("FretForge").getChildFile ("TONE3000"), docs.getChildFile (FF_APP_NAME).getChildFile ("TONE3000"));
    }

    void shutdown() override
    {
        if (scanner != nullptr) { scanner.reset(); hardExit (0); }
        if (window != nullptr)
            properties.getUserSettings()->setValue ("windowState", window->getWindowStateAsString());
        properties.saveIfNeeded();
        // The project is saved (or the user chose not to) and the settings are written. Closing plugins and the audio
        // device comes next; if one of them never returns, the app must still go away rather than sit in the task
        // manager with no window.
        juce::Logger::writeToLog ("Shutting down");
        armShutdownWatchdog (12);
        // every window goes at once (plugin editors and tool windows too, so nothing is left flashing on its own)
        // the main window goes last: hiding it first hands focus to one of the other windows, which then flashes up
        // for a moment before it is hidden too
        for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;)
            if (auto* c = juce::Desktop::getInstance().getComponent (i))
                if (c != window.get()) c->setVisible (false);
        if (window != nullptr) window->setVisible (false);
        window.reset();
        properties.closeFiles();
        juce::Logger::writeToLog ("Clean shutdown");
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
        // skip static destructors and library unloading: plugins and drivers that hang there kept the process alive
        hardExit (0);
    }

    void systemRequestedQuit() override
    {
        if (scanner != nullptr) { quit(); return; }
        if (window != nullptr && window->content != nullptr)
            window->content->requestQuit ([] { juce::JUCEApplication::getInstance()->quit(); });
        else
            quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override { openArgument (commandLine); }

private:
    juce::ApplicationProperties properties;
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<PluginScannerWorker, PluginScannerWorkerDeleter> scanner;

    void openArgument (const juce::String& commandLine)
    {
        const auto arg = commandLine.unquoted().trim();
        if (arg.isNotEmpty() && juce::File::isAbsolutePath (arg) && window != nullptr && window->content != nullptr)
            window->content->openFromCommandLine (juce::File (arg));
    }
};
} // namespace ff

START_JUCE_APPLICATION (ff::Application)
