#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "../../ProjectDocument.h"
#include "../../EngineHost.h"
#include "../../SessionController.h"
#include "../../Widgets.h"
#include "core/Timeline.h"
#include "core/MidiClip.h"
#include "core/Automation.h"
#include "PianoRoll.h"
#include "../../ToolWindow.h"
#include <map>

namespace ff
{
class ArrangeView;

/** Shared view geometry for the arrange window (ruler, lanes, headers). */
struct ArrangeGeometry
{
    double pixelsPerSecond = 60.0;
    double scrollX = 0.0;        // seconds at the left edge of the lanes
    int scrollY = 0;             // pixels
    int trackHeight = 96;        // default; each track can be taller or shorter (TRACK laneHeight, drag a header's bottom edge)
    std::vector<int> heights;    // per track, refreshed by the view
    static constexpr int minTrackHeight = 66, maxTrackHeight = 480;
    int heightOf (int i) const { return i >= 0 && i < (int) heights.size() ? heights[(size_t) i] : trackHeight; }
    int topOf (int i) const      // content y (before scrolling)
    {
        int y = 0;
        for (int k = 0; k < i; ++k) y += heightOf (k);
        return y;
    }
    int indexAt (int contentY) const
    {
        int y = 0;
        for (int k = 0; k < (int) heights.size(); ++k)
        {
            if (contentY < y + heights[(size_t) k]) return k;
            y += heights[(size_t) k];
        }
        return (int) heights.size() + juce::jmax (0, contentY - y) / trackHeight;
    }
    int totalHeight() const { return topOf ((int) heights.size()); }
    double snapBeats = 1.0;      // grid in quarter notes; 0 = off
    TempoMap tempoMap;           // tempo changes of the project (the constant tempo `bpm` when there are none)
    double barQuarters() const { return tsNum * 4.0 / tsDen; }
    double beatQuarters() const { return 4.0 / tsDen; }
    double barSecAt (int bar) const { return tempoMap.secondsAtQuarter (bar * barQuarters()); }
    double beatSecAt (int bar, int k) const { return tempoMap.secondsAtQuarter (bar * barQuarters() + k * beatQuarters()); }
    int barAt (double t) const { return (int) std::floor (tempoMap.quarterAtSeconds (juce::jmax (0.0, t)) / barQuarters() + 1.0e-9); }
    double bpm = 120.0;
    int tsNum = 4, tsDen = 4;

    float xFor (double t) const { return (float) ((t - scrollX) * pixelsPerSecond); }
    double timeFor (float x) const { return scrollX + x / pixelsPerSecond; }
    double quarterSec() const { return 60.0 / bpm; }
    double barSec() const { return tsNum * 4.0 / tsDen * quarterSec(); }
    double gridSec() const { return snapBeats * quarterSec(); }
    double snap (double t, bool enabled = true) const
    {
        if (! enabled || snapBeats <= 0.0) return juce::jmax (0.0, t);
        if (! tempoMap.isConstant())   // the grid follows the tempo changes
            return juce::jmax (0.0, tempoMap.secondsAtQuarter (std::round (tempoMap.quarterAtSeconds (juce::jmax (0.0, t)) / snapBeats) * snapBeats));
        const double g = gridSec();
        return juce::jmax (0.0, std::round (t / g) * g);
    }
};

/** The track's plugins under its input, as one dropdown: click it to see every plugin (click one to open it, right-click
    to bypass / replace / remove, + to add). With no plugins it is a "+ Plugin" button. */
class InsertStrip : public juce::Component, public juce::SettableTooltipClient, private juce::ValueTree::Listener
{
public:
    InsertStrip (ArrangeView&, juce::ValueTree track);
    ~InsertStrip() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -2; repaint(); }
private:
    ArrangeView& owner;
    juce::ValueTree track;
    int hover = -2;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { repaint(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { repaint(); }
};

class TrackHeader : public juce::Component, private juce::ValueTree::Listener
{
public:
    TrackHeader (ArrangeView&, juce::ValueTree track, int index);
    ~TrackHeader() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void tickMeter();
    void refreshInputs();
    void refreshAutomation();
    juce::ValueTree track;
    int index;

private:
    ArrangeView& owner;
    juce::Label name;
    juce::TextButton arm { "R" }, mute { "M" }, solo { "S" };
    MonitorButton mon;
    juce::ComboBox input;
    ChannelModeButton channels;           // mono / stereo (audio tracks)
    ContextButton route;                  // bus / FX: output and sends; instrument track: the instrument (right-click: change / remove)
    juce::TextButton laneButton;          // automation mode, which lane the track shows
    juce::Slider gain, pan;
    std::unique_ptr<InsertStrip> inserts;
    juce::Array<std::pair<int, bool>> inputValues;
    std::vector<std::pair<juce::ValueTree, double>> groupStart; // other selected tracks' values at drag start
    void beginGroupDrag (const juce::Identifier& id);
    void applyGroupDelta (const juce::Identifier& id, double delta, double lo, double hi);
    float meterLevel = -100.0f;
    bool updating = false, draggingTrack = false, resizingTrack = false, pendingCollapse = false;
    int resizeStartH = 0;
    bool onBottomEdge (int y) const { return y >= getHeight() - 5; }
    void mouseMove (const juce::MouseEvent&) override;
    double gainPrev = 0.0;
    void refresh();
    void showMenu();
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { refresh(); }
    // the instrument is a child node: choosing or removing one changes the name shown on the header
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override { if (c.hasType (midiclip::INSTRUMENT)) refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override { if (c.hasType (midiclip::INSTRUMENT)) refresh(); }
};

class TimelineCanvas : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit TimelineCanvas (ArrangeView& v) : owner (v) { setWantsKeyboardFocus (true); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override { dropX = x; dropY = y; repaint(); }
    void fileDragMove (const juce::StringArray&, int x, int y) override { dropX = x; dropY = y; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropX = -1; repaint(); }

private:
    ArrangeView& owner;
    enum class Mode { none, move, trimLeft, trimRight, fadeIn, fadeOut, marquee };
    juce::Rectangle<int> marqueeRect;
    double marqueeStartTime = 0.0;   // where the rectangle started, in content coordinates (it stays put while the view scrolls)
    int marqueeStartY = 0;
    float sliceX = -1.0f;   // slice tool: where the cut would go (hover)
    void mouseExit (const juce::MouseEvent&) override { sliceX = -1.0f; repaint(); }
    bool keyPressed (const juce::KeyPress& k) override;
    struct Orig { juce::ValueTree clip; double start, length, offset, fadeIn, fadeOut; int track; };
    Mode mode = Mode::none;
    std::vector<Orig> originals;
    juce::ValueTree anchor;
    int trackDelta = 0;
    bool dragged = false;
    int dropX = -1, dropY = -1;

    Mode modeAt (const juce::ValueTree& clip, juce::Point<float> p) const;
    const std::vector<midiclip::Note>& cachedNotes (const juce::ValueTree& clip);
    std::map<juce::String, std::pair<juce::String, std::vector<midiclip::Note>>> noteCache;
    void drawClip (juce::Graphics&, const juce::ValueTree& clip, juce::Rectangle<float> r, juce::Colour colour, bool selected);
    void showClipMenu (juce::ValueTree clip);
    void showLaneMenu (int trackIndex, double time);

    // automation mode: points of the lane each track shows
    juce::ValueTree autoLane;
    int autoTrack = -1, autoPoint = -1;
    std::vector<automation::Point> autoPts;
    // a new point needs click-and-hold (autoHoldMs) without moving, so stray clicks add nothing; a double-click adds one
    // at once. Dragging (existing or just-made points) starts after a few pixels and keeps the grab offset, so a point
    // does not jump when it is clicked.
    static constexpr int autoHoldMs = 350;
    bool autoPending = false, autoMoved = false;
    int autoPendingTrack = -1, autoPendingToken = 0;
    juce::Point<float> autoPendingPos, autoGrabOffset;
    bool autoPendingAlt = false;
    bool autoMouseDown (const juce::MouseEvent&);
    void autoMouseDrag (const juce::MouseEvent&);
    void addAutoPoint (int trackIndex, juce::Point<float> pos, bool noSnap);
    juce::Rectangle<float> laneRect (int trackIndex) const;
    juce::Point<float> pointPos (const juce::String& param, const automation::Point&, juce::Rectangle<float> row) const;
    int pointAt (juce::Point<float>, const std::vector<automation::Point>&, const juce::String& param, juce::Rectangle<float> row) const;
    void drawAutomation (juce::Graphics&);
};

class Ruler : public juce::Component
{
public:
    explicit Ruler (ArrangeView& v) : owner (v) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    static constexpr int markerLane = 18;
    static constexpr int baseHeight = 58;  // the ruler without the tempo track
    static constexpr int loopLane = 14;   // the loop bar's strip at the top
    static constexpr int exportLane = 12; // below it: the export range (what File > Export renders)
    static constexpr int numbersTop = loopLane + exportLane;
    bool isZooming() const noexcept { return zooming; }
    void mouseMove (const juce::MouseEvent&) override;
    static inline const juce::Identifier exportStartId { "exportStart" }, exportEndId { "exportEnd" };   // TRANSPORT
private:
    ArrangeView& owner;
    double dragStart = -1.0;
    juce::ValueTree markerAt (float x) const;
    // tempo track: an envelope in the lane that appears above the markers when the ruler is taller than its base height
    juce::Rectangle<int> tempoArea() const { const int h = juce::jmax (0, getHeight() - baseHeight); return { 0, getHeight() - markerLane - h, getWidth(), h }; }
    juce::ValueTree tempoAt (juce::Point<int> p, bool& isBase) const;
    void tempoRange (double& lo, double& hi) const;
    float tempoY (float bpm, double lo, double hi) const;
    double tempoBpmAt (float y, double lo, double hi) const;
    void editTempo (juce::ValueTree point, double quarter, bool isBase);
    double tempoQuarterAt (float x, const TempoMap& map, bool snapOn) const;
    juce::ValueTree tempoDrag;
    bool tempoDragBase = false;
    double dragLo = 60.0, dragHi = 180.0;
    TempoMap tempoDragMap;
    std::vector<TempoMap::Point> tempoDragOthers;   // the other points while one is dragged
    double dragQuarterFor (float x, double bpm, bool snapOn) const;
    double newPointQuarter (juce::Point<int> p, bool snapOn, double bpm = 0.0);
    float tempoGrabDx = 0.0f;   // where the pointer grabbed the dot, relative to its centre
    // loop bar (like Studio One): drag the bar to move it, its ends to resize it, the empty strip to draw a new one
    enum class LoopDrag { none, move, left, right, draw } loopDrag = LoopDrag::none;
    double loopA0 = 0.0, loopB0 = 0.0;
    bool scrubbing = false, zooming = false, exportDrag = false, pendingLocate = false, scrubMoved = false;
    double pendingLocateTime = 0.0;
    double zoomPps0 = 0.0, zoomAnchor = 0.0, playheadBefore = 0.0;
    LoopDrag loopHit (float x, bool exportRange = false) const;
    juce::Identifier startId (bool exportRange) const;
    juce::Identifier endId (bool exportRange) const;
};

/** The master bus's automation lane, pinned under the tracks (toolbar: "Master"): its volume and the parameters of its plugins.
    Double-click to add a point, drag a point to move it, double-click one to type its value, right-click for more. */
class MasterLane : public juce::Component
{
public:
    explicit MasterLane (ArrangeView& o) : owner (o) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragLane = {}; dragPoint = -1; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent& e) override;
private:
    ArrangeView& owner;
    juce::ValueTree dragLane;
    std::vector<automation::Point> dragPts;
    int dragPoint = -1;
    bool moved = false;
    juce::Point<float> grabOffset;
    juce::Rectangle<float> row() const { return getLocalBounds().toFloat().reduced (0.0f, 12.0f); }
    juce::Point<float> posOf (const juce::String& prm, const automation::Point& p) const;
    int pointAt (juce::Point<float> at, const std::vector<automation::Point>& pts, const juce::String& prm) const;
};

/** The left side of the master lane: its name, the parameter shown, and its value at the play position. */
class MasterHeader : public juce::Component
{
public:
    explicit MasterHeader (ArrangeView&);
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::TextButton laneButton;
private:
    ArrangeView& owner;
};

/** The Studio arrange window: toolbar, ruler, track headers and the clip timeline. */
class ArrangeView : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer, private juce::ValueTree::Listener,
                    private juce::ScrollBar::Listener, private juce::ChangeListener
{
public:
    ArrangeView (ProjectDocument&, EngineHost&, SessionController&, juce::PropertiesFile&);
    ~ArrangeView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void rebind();
    void deviceChanged();

    // ---- editing commands ----
    void addTrack();
    void importAudioDialog();
    void deleteSelection();
    void splitAtPlayhead();
    void duplicateSelection();
    void copySelection (bool cut);
    void paste();
    void selectAll();
    void addMarkerAtPlayhead();
    void zoom (double factor, float aroundX = -1.0f);
    void zoomToFit();
    /** Zoom to `pps` pixels per second keeping `time` under canvas x `x` (the ruler's drag-to-zoom). */
    void zoomAround (double pps, double time, float x);
    bool hasSelection() const { return ! selection.isEmpty(); }
    /** True when two or more selected MIDI clips share a track. */
    bool canCombineMidi() const;
    /** Merges the selected MIDI clips of each track into one clip (one undo step). */
    void combineMidiClips();
    /** Reorders: moves `tracks` as a block so they start at `insertIndex` (0..numTracks). */
    void moveTracks (juce::Array<juce::ValueTree> tracks, int insertIndex);
    void setTrackDropIndex (int index);
    int trackDropIndex() const { return dropIndex; }
    void paintOverChildren (juce::Graphics&) override;
    // files dragged in from the system's file browser: dropping anywhere on the page imports them
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    /** Imports audio / MIDI files at canvas position (x, y); a position outside the lanes means a new track at x (or 0). */
    void importDropped (const juce::StringArray& files, int canvasX, int canvasY);

    // ---- shared with children ----
    ProjectDocument& doc;
    EngineHost& host;
    SessionController& session;
    juce::PropertiesFile& settings;
    ArrangeGeometry geo;
    juce::Array<juce::ValueTree> selection;

    juce::ValueTree trackAtIndex (int i) { return doc.tracks().getChild (i); }
    int trackIndexAtY (int canvasY) const;
    void refreshHeights();
    static inline const juce::Identifier laneHeightId { "laneHeight" };   // TRACK: height in the arrange view (0 = default)
    int numTracks() { return doc.tracks().getNumChildren(); }
    juce::Colour trackColour (const juce::ValueTree& t) const;
    void select (const juce::ValueTree& clip, bool add);
    /** Clips on the other tracks of a clip's track group lined up with it (same start and end) (they are edited with it; Alt: none). */
    juce::Array<juce::ValueTree> groupPartners (const juce::ValueTree& clip) const;
    void clearSelection() { selection.clear(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void locate (double seconds) { host.engine.locateSeconds (seconds); }
    double playhead() const { return host.engine.getPositionSeconds(); }
    void updateScrollBars();
    void repaintAll();
    void showError (const juce::String& title, const juce::String& message);
    std::function<void()> onExportRequested;   // the export bar's "Export this range..."
    void renameDialog (const juce::String& title, const juce::String& current, std::function<void (juce::String)> onDone);
    juce::Rectangle<int> lanesArea() const { return canvas.getBounds(); }

private:
    Ruler ruler { *this };
    TimelineCanvas canvas { *this };
    /** The column of track headers; its empty space below the last track offers "add track" on right-click. */
    struct HeaderHolder : public juce::Component
    {
        std::function<void (const juce::MouseEvent&)> onEmptyClick;
        void mouseDown (const juce::MouseEvent& e) override { if (onEmptyClick) onEmptyClick (e); }
    } headerHolder;
    juce::OwnedArray<TrackHeader> headers;
    juce::ScrollBar hScroll { false }, vScroll { true };
    ContextButton addTrackButton { "+ Track" };
    juce::TextButton importButton { "Import" }, zoomInButton { "+" }, zoomOutButton { "-" },
        fitButton { "Fit" }, followButton { "Follow" }, sliceButton { "Slice" }, autoButton { "Automation" }, tempoButton { "Tempo Track" }, masterButton { "Master" };
public:
    /** Automation mode: tracks show an automation lane (volume, pan, plugin parameters) over their clips. */
    bool automationMode() const { return autoButton.getToggleState(); }
    juce::String laneParamFor (const juce::ValueTree& track) const;
    juce::String laneLabel (const juce::ValueTree& track, const juce::String& param) const;
    void showLaneChooser (juce::ValueTree track, juce::Component* target);
    /** Opens (or brings forward) the MIDI editor of a MIDI clip. */
    void openMidiEditor (juce::ValueTree clip);
    /** A new empty one-bar MIDI clip on an instrument track, at the bar containing `time`; opens it in the MIDI editor. */
    void newMidiClip (juce::ValueTree track, double time);
    /** Save a MIDI clip (or all of a track's MIDI clips, at their song positions) as a .mid file. */
    void exportMidi (juce::ValueTree clipOrTrack);
    juce::AudioProcessorParameter* laneParameter (const juce::ValueTree& track, const juce::String& param) const;
    juce::String laneValueText (const juce::ValueTree& track, const juce::String& param, float value) const;
    bool laneValueFromText (const juce::ValueTree& track, const juce::String& param, const juce::String& text, float& value) const;
    bool sliceMode() const { return sliceButton.getToggleState(); }
    void setSliceMode (bool on) { sliceButton.setToggleState (on, juce::sendNotificationSync); }
    /** Splits the clips at time t (seconds). */
    void splitClipsAt (const juce::Array<juce::ValueTree>& clips, double t);
private:
    juce::ComboBox snapCombo;
    juce::Label snapLabel;
    juce::ValueTree root;
    std::unique_ptr<juce::FileChooser> chooser;
    // The MIDI editor docks at the bottom of the arranger (drag the bar above it to resize, like Studio One).
    struct DockBar : juce::Component
    {
        explicit DockBar (ArrangeView& o) : owner (o) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
        void paint (juce::Graphics& g) override
        {
            g.setColour (palette::outline);
            g.fillRect (getLocalBounds());
            g.setColour (palette::textDim.withAlpha (0.6f));
            g.fillRoundedRectangle ((float) getWidth() * 0.5f - 18.0f, (float) getHeight() * 0.5f - 1.0f, 36.0f, 2.0f, 1.0f);
        }
        void mouseDown (const juce::MouseEvent&) override { startHeight = owner.dockHeight; }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            owner.dockHeight = juce::jlimit (160, juce::jmax (160, owner.getHeight() - 200), startHeight - e.getDistanceFromDragStartY());
            owner.resized();
        }
        void mouseUp (const juce::MouseEvent&) override { owner.settings.setValue ("midiDockHeight", owner.dockHeight); }
        ArrangeView& owner;
        int startHeight = 0;
    };
    struct DockTitle : juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.setColour (palette::panel);
            g.fillRect (getLocalBounds());
            g.setColour (palette::text);
            g.setFont (uiFont (12.0f, true));
            g.drawText (text, getLocalBounds().reduced (12, 0).withTrimmedRight (30), juce::Justification::centredLeft);
        }
        juce::String text;
    };
    std::unique_ptr<PianoRoll> dockedEditor;
    juce::String dockedClipId;
    DockBar dockBar { *this };
    DockTitle dockTitle;
    juce::TextButton dockClose { "X" };
    int dockHeight = 320;
    void closeMidiEditor();
    struct ClipboardItem { juce::ValueTree clip; int trackOffset; double timeOffset; };
    std::vector<ClipboardItem> clipboard;
    bool rebuildPending = false;
    int dropIndex = -1;
    int lastPlayheadX = -2000000;
    double lastScrollX = -1.0, lastPps = -1.0;

    void rebuildHeaders();
    void layoutHeaders();
    void syncGeometry();
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override { rebuildHeaders(); }

    static constexpr int headerWidth = 250;
    static constexpr int tempoTrackHeight = 84;
    int rulerHeight() const { return Ruler::baseHeight + (tempoTrackShown ? tempoTrackHeight : 0); }
    bool tempoTrackShown = false;
    // the master lane (automation of the main output): under the tracks, toggled by the Master button
    static constexpr int masterLaneHeight = 104;
    bool masterShown = false;
    MasterLane masterLane { *this };
    MasterHeader masterHeader { *this };
    friend class MasterLane;
    friend class MasterHeader;
    static constexpr int toolbarHeight = 38;
};
} // namespace ff
