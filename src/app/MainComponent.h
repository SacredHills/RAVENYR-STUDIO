#pragma once
#include "LoadingOverlay.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include "StudioLookAndFeel.h"
#include "EngineHost.h"
#include "ProjectDocument.h"
#include "TransportBar.h"
#include "SessionController.h"
#include "ExportDialog.h"
#include "pages/StudioPage.h"
#include "pages/TunerPanel.h"
#include "pages/MetronomePanel.h"
#include "ToolWindow.h"
#include "tone3000/Tone3000Panel.h"
#include "plugins/PluginManager.h"
#include "pages/MixerPage.h"
#include "pages/SettingsPage.h"
#include "pages/MilestonePage.h"
#include "pages/training/TrainingPage.h"
#include "pages/tabs/TabPage.h"
#include "pages/midistudio/MidiStudioPage.h"
#include "pages/fx/RigEditor.h"

namespace ff
{
/** Row of section buttons above the pages (Settings lives next to the CPU readout instead). */
class SectionNav : public juce::Component, public juce::SettableTooltipClient
{
public:
    juce::StringArray names;
    juce::Array<bool> implemented;
    int selected = 0;
    int hidden = -1;   // a section shown elsewhere
    std::function<void (int)> onSelect;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool hitTest (int x, int y) override { return fillBackground || indexAt ({ x, y }) >= 0; }   // transparent: the tabs underneath stay clickable
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    static constexpr int barHeight = 30;   // same as the Arrange / Project tab bar
    bool fillBackground = true;
private:
    int hover = -1;
    juce::Rectangle<int> buttonBounds (int i) const;
    int indexAt (juce::Point<int>) const;
};

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      private juce::Timer
{
public:
    // audio / MIDI files dropped anywhere on the window (the arrange view takes them when dropped on it)
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    enum Commands
    {
        cmdNew = 0x2000, cmdOpen, cmdSave, cmdSaveAs, cmdReveal, cmdProject, cmdUndo, cmdRedo,
        cmdPlayStop, cmdReturnToZero, cmdBackToPlayStart, cmdToggleClick, cmdTapTempo, cmdPracticeMetronome,
        cmdShortcuts, cmdAbout, cmdTunerWindow, cmdMetronomeWindow, cmdPlugins,
        cmdRecord, cmdLoop, cmdPunch, cmdAddTrack, cmdImport, cmdExport, cmdSplit, cmdDuplicate, cmdCombineMidi, cmdDelete,
        cmdCopy, cmdCut, cmdPaste, cmdSelectAll, cmdAddMarker, cmdZoomIn, cmdZoomOut, cmdZoomFit,
        cmdSectionBase = 0x2100 // + section index
    };

    MainComponent (juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

    /** Runs startup prompts (crash recovery, first-run, device errors). */
    void runStartupChecks();
    /** Asks about unsaved changes, then calls `quit` if the user agrees. */
    void requestQuit (std::function<void()> quit);
    void openFromCommandLine (const juce::File&);

    juce::ApplicationCommandManager commandManager;
    std::function<void (const juce::String&)> onTitleChanged;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return juce::JUCEApplication::getInstance(); }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int, int) override;

private:
    juce::uint32 lastNumpad0Ms = 0;
    juce::PropertiesFile& settings;
    StudioLookAndFeel lnf;
    EngineHost host;
    std::unique_ptr<PluginManager> pluginManager;
    juce::Rectangle<int> logoArea;   // before the session: rigs load plugins through it
    std::unique_ptr<ProjectDocument> doc;
    std::unique_ptr<SessionController> session;

    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<TransportBar> transport;
    SectionNav nav;
    std::unique_ptr<StudioPage> studioPage;
    std::unique_ptr<TunerPanel> tunerPanel;
    std::unique_ptr<MetronomePanel> metronomePanel;
    std::unique_ptr<ToolWindow> tunerWindow, metronomeWindow, projectWindow;
    struct RigWindow { std::unique_ptr<RigEditor> editor; std::unique_ptr<ToolWindow> window; };
    std::map<juce::String, RigWindow> rigWindows;   // by track id
    void openRigWindow (juce::ValueTree track);
    std::unique_ptr<t3k::Client> t3kClient;
    std::unique_ptr<Tone3000Panel> t3kPanel;
    std::unique_ptr<ToolWindow> t3kWindow;
    void openTone3000 (RigEditor&, bool cab);
    void openTone3000 (bool cab, const juce::String& targetName, std::function<void (const juce::File&, bool isIr)> load);
    void openTuner();
    void openProjectSheet();
    void openMetronome();
    std::unique_ptr<MixerPage> mixerPage;
    std::unique_ptr<SettingsPage> settingsPage;
    std::unique_ptr<TabPage> tabPage;
    std::unique_ptr<MidiStudioPage> drumPage;
    std::unique_ptr<TrainingPage> trainingPage;
    std::unique_ptr<RigEditor> effectsPage;
    juce::OwnedArray<MilestonePage> milestonePages;
    juce::Array<juce::Component*> pages;
    juce::Label statusLeft, statusRight;
    LoadingOverlay loadingOverlay;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };
    int currentSection = 0;
    static constexpr int numSections = 7, drumSection = 2, tabsSection = 3, trainingSection = 4, effectsSection = 5, settingsSection = 6;

    void showSection (int index);
    void layoutPages();
    void togglePlayForSection();
    void returnToStartForSection();
    juce::Rectangle<int> pagesArea;
    void updateTitleAndStatus();
    void timerCallback() override;

    void withChangesHandled (std::function<void()> continuation);
    void saveThen (std::function<void()> continuation);
    void saveAsThen (std::function<void()> continuation);
    void openProjectDialog();
    void openProject (const juce::File& bundleOrDocument);
    void showError (const juce::String& title, const juce::String& message);
    void showShortcutEditor();
};
} // namespace ff
