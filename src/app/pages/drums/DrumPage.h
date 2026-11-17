#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <unordered_set>
#include "../../ProjectDocument.h"
#include "../../EngineHost.h"
#include "../../SessionController.h"
#include "../../Widgets.h"
#include "DrumMixer.h"
#include "core/DrumModel.h"

namespace ff
{
/** Step grid for one drum pattern: a lane per kit piece, a column per grid step.
    Click adds a hit (Shift = ghost, Alt = accent), click a hit to remove it, drag a hit up/down to change
    its velocity, drag across empty cells to paint, right-click for velocity presets, click a lane name to hear it. */
class DrumGrid : public juce::Component
{
public:
    DrumGrid (ProjectDocument&, EngineHost&);
    void setPattern (juce::ValueTree p);
    juce::ValueTree pattern() const { return pat; }
    void setPlayheadQ (double q);
    float xForQ (double q) const;
    void updateSize (int visibleWidth);
    std::function<void (int lane)> onLaneMenu;       // right-click on a lane name
    std::function<void (double q)> onSeek;           // click on the bar-number strip at the top
    void setStartQ (double q) { startQ = q; repaint(); }
    void clearSelection() { if (! selected.isEmpty()) { selected.clear(); repaint(); } }
    int numSelected() const { return selected.size(); }
    bool keyPressed (const juce::KeyPress&) override;
    double getStartQ() const { return startQ; }
    std::function<bool (int lane)> laneHasSample;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr int laneHeight = 22, nameWidth = 112, headerHeight = 24, velLaneHeight = 84;
    static int velTop() { return headerHeight + (int) drums::numPieces * laneHeight + 12; }

private:
    ProjectDocument& doc;
    EngineHost& host;
    juce::ValueTree pat;
    double playQ = -1.0, startQ = 0.0;
    float cellW = 24.0f;

    enum class Drag { none, velocity, paint, erase, seek, marquee, move } drag = Drag::none;
    juce::Array<juce::ValueTree> selected;
    struct Origin { juce::ValueTree hit; int tick, lane; };
    std::vector<Origin> origins, velOrigins;   // velOrigins: `tick` holds the hit's starting velocity
    juce::Rectangle<int> marquee;
    juce::Array<juce::ValueTree> marqueeBase;   // the selection before a Shift-marquee
    bool isSelected (const juce::ValueTree& h) const { return selected.contains (h); }
    std::unordered_set<int> selKeys;   // lane * 10000000 + tick of every selected hit (for painting)
    void rebuildSelKeys();
    void snapshotOrigins();
    void selectInRect (juce::Rectangle<int>, bool add);
    void moveSelected (int dTicks, int dLanes);
    void dropOverlaps();
    juce::ValueTree dragHit;
    int dragStartVel = 100, dragLane = -1, paintVel = 100;
    bool moved = false;

    int steps() const;
    int laneAt (int y) const { return (y - headerHeight) / laneHeight; }
    int stepAt (int x) const { return (int) std::floor (((float) x - (float) nameWidth) / cellW); }
    juce::ValueTree hitInCell (int lane, int step) const;
};

/** Arrangement of patterns into a song: one block per section (pattern x repeats). */
class SongStrip : public juce::Component
{
public:
    explicit SongStrip (ProjectDocument&);
    void setDrums (juce::ValueTree d) { drums = d; repaint(); }
    int selected = -1;
    double playQ = -1.0;
    std::function<void (int)> onSelect;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
private:
    ProjectDocument& doc;
    juce::ValueTree drums;
    std::vector<juce::Rectangle<float>> blocks;
};

/** Drum Studio section: patterns, generator, humanize, song arrangement, playback and export. */
class DrumPage : public juce::Component, private juce::ValueTree::Listener, private juce::AsyncUpdater, private juce::Timer
{
public:
    DrumPage (ProjectDocument&, EngineHost&, SessionController&, juce::PropertiesFile&);
    ~DrumPage() override;
    void rebind();
    void setActive (bool);
    void togglePlay();
    /** Back to the start of the pattern (or song); keeps playing if it was playing. */
    void goToStart();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ProjectDocument& doc;
    EngineHost& host;
    SessionController& session;
    juce::PropertiesFile& settings;
    juce::ValueTree root, drums;
    bool active = false, updating = false, songMode = false;
    juce::uint32 seed = 1;
    bool seekSet = false;   // the user clicked a play position in the grid's bar numbers

    DrumGrid grid;
    juce::Viewport viewport;
    SongStrip strip;

    // row 1: pattern
    juce::ComboBox patternCombo, sigCombo, barsCombo, gridCombo;
    juce::TextButton newButton { "New" }, dupButton { "Duplicate" }, renameButton { "Rename" }, deleteButton { "Delete" };
    juce::Slider swingSlider, tempoSlider;
    juce::Label swingLabel, tempoLabel;
    juce::TextButton backButton { "|<" }, patternModeButton { "Pattern" }, songModeButton { "Song" }, loopButton { "Loop" }, clickButton { "Click" };
    void startPlayback();
    // row 2: generator & output
    juce::ComboBox genreCombo;
    juce::TextButton halfButton { "Half-time" }, fillButton { "Fill" }, crashButton { "Crash on 1" };
    juce::Slider complexitySlider, humanTimeSlider, humanVelSlider, volumeSlider;
    juce::Label complexityLabel, humanLabel, volumeLabel;
    juce::TextButton generateButton { "Generate" }, generateNewButton { "Generate as new" };
    juce::TextButton kitButton { "Kit..." }, instrumentButton { "Instrument..." }, mixerButton { "Mixer" };
    DrumMixer mixer { doc, host };
    void pushMix();
    juce::TextButton importButton { "Import..." }, exportButton { "Export MIDI" }, renderButton { "Render to Editor" };
    // song
    juce::TextButton addSectionButton { "+ Add pattern" }, removeSectionButton { "Remove" }, leftButton { "<" }, rightButton { ">" },
                     lessButton { "x-" }, moreButton { "x+" };
    juce::Label songLabel, status;
    std::unique_ptr<juce::FileChooser> chooser;

    std::array<juce::String, drums::numPieces> loadedPaths;
    double loadedRate = 0.0;
    bool kitDirty = true;
    void reloadKit (bool force);
    void showLaneMenu (int lane);
    void showKitMenu();
    void handleKitMenu (int result, int lane);
    void addKitItems (juce::PopupMenu&, int lane) const;
    /** The sample a piece plays: the project's own, else the default kit's (from the settings), else none. */
    juce::File effectiveSample (int piece) const;
    juce::String defaultKitKey (int piece) const { return "defaultDrumKit" + juce::String (piece); }
    void assignSample (int piece, const juce::File& file);
    void loadKitFolder();
    juce::File kitFolder() const { return doc.mediaBundle().getChildFile ("Audio").getChildFile ("DrumKit"); }
public:
    /** Reads an audio file into memory at `sampleRate` (resampled if needed, max 2 channels, max 10 s). */
    static std::unique_ptr<juce::AudioBuffer<float>> loadSampleFile (const juce::File&, double sampleRate);
private:

    juce::ValueTree current() const;
    void refresh();
    void selectPattern (const juce::String& id);
    void rebuildSequence();
    void pushEngine();
    void generate (bool asNew);
    void seekTo (double patternQ);
    double songOffsetQ() const;   // where the edited pattern starts in the song (selected section, else first use)
    void renderToStudio();
    /** Adds the pattern (or the whole song when `pattern` is invalid) to the Editor as a MIDI clip on a new drum
        instrument track, starting at bar 1, where it can be edited in the piano roll. */
    void midiToEditor (const juce::ValueTree& pattern);
    void message (const juce::String& title, const juce::String& text);

    void handleAsyncUpdate() override { refresh(); rebuildSequence(); }
    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree&) override { if (p.isAChildOf (root) || p == root) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree&, int) override { if (p.isAChildOf (root) || p == root) triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
};
} // namespace ff
