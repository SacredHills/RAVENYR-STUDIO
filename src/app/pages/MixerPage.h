#pragma once
#include "../EngineHost.h"
#include "../ProjectDocument.h"
#include "../SessionController.h"
#include "../Widgets.h"
#include "engine/fx/BuiltinPlugins.h"

namespace ff
{
/** A strip's insert plugins: one row per plugin (click: plugin window, right-click: bypass / move / replace / remove)
    and a "+ Plugin" row. */
class InsertList : public juce::Component, public juce::SettableTooltipClient, public juce::DragAndDropTarget
{
public:
    InsertList (SessionController& s, juce::ValueTree t) : session (s), track (t) {}
    int rowsWanted() const;
    static constexpr int rowHeight = 20;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    // drag a plugin onto another track (copies it with its settings) or up / down in this list (moves it)
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails& d) override { if (d.description.toString().startsWith ("ffamp|")) return; dropRow = juce::jlimit (0, rig::numInserts (track), (d.localPosition.y + rowHeight / 2) / rowHeight - ampRows()); repaint(); }
    void itemDragExit (const SourceDetails&) override { dropRow = -1; repaint(); }
    void itemDropped (const SourceDetails&) override;
    juce::ValueTree getTrack() const { return track; }
private:
    SessionController& session;
    juce::ValueTree track;
    bool dragging = false;
    int dropRow = -1;
    bool onBypassButton (int x) const { return x > getWidth() - 18; }
    int ampRows() const;   // 1 when the track has a NAM amp (shown first)
};

/** The rack on top of a mixer strip: one slot per plugin on the track (plus its NAM amp). Slots are collapsed rows
    (name, bypass LED); a built-in plugin expands in place into its compact panel, one at a time, filling the rack.
    Other plugins open their own window (the arrow). The last row adds a plugin. */
class RackView : public juce::Component, public juce::DragAndDropTarget, private juce::ValueTree::Listener,
                 private juce::ChangeListener, private juce::AsyncUpdater
{
public:
    RackView (SessionController& s, juce::ValueTree t);
    ~RackView() override;
    static constexpr int slotH = 26;
    void resized() override;
    void paint (juce::Graphics&) override;
    void tick();   // the mixer's timer: follows plugins being added / removed / loading
    juce::ValueTree getTrack() const { return track; }
    juce::ValueTree getExpandedNode() const { return expandedNode; }
    void restoreExpanded (juce::ValueTree n) { expandedNode = n; signature = {}; }   // after the mixer rebuilds its strips
    void expand (int slot) { expandedNode = rig::insertNode (track, slot); signature = {}; tick(); }
    bool isInterestedInDragSource (const SourceDetails& d) override;
    void itemDragMove (const SourceDetails& d) override;
    void itemDragExit (const SourceDetails&) override { dropRow = -1; content.repaint(); }
    void itemDropped (const SourceDetails& d) override;

private:
    // refresh at once when this track's plugins change or finish loading (the mixer's timer is only a backstop)
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { tick(); }
    struct Row { int kind, index, y; };   // kind: 0 NAM amp, 1 plugin, 2 "+ Add plugin"
    struct Content final : public juce::Component
    {
        explicit Content (RackView& o) : owner (o) {}
        void paint (juce::Graphics& g) override { owner.paintContent (g); }
        void mouseDown (const juce::MouseEvent& e) override { owner.dragging = false; (void) e; }
        void mouseDrag (const juce::MouseEvent& e) override { owner.contentDrag (e); }
        void mouseUp (const juce::MouseEvent& e) override { owner.contentUp (e); }
        RackView& owner;
    };
    SessionController& session;
    juce::ValueTree track;
    juce::Viewport vp;
    Content content { *this };
    std::vector<Row> rows;
    juce::Rectangle<int> panelArea;
    std::shared_ptr<juce::AudioPluginInstance> panelPlugin;   // declared before the panel: outlives it
    std::unique_ptr<juce::Component> panel;
    int expanded = -1, dropRow = -1;   // expanded: index of expandedNode (the slot follows its plugin when slots move)
    juce::ValueTree expandedNode;
    bool knownReady = false;
    std::vector<juce::ValueTree> knownInserts;   // to spot newly added built-ins and expand them
    bool dragging = false;
    juce::String signature;
    void rebuild();
    juce::String makeSignature() const;
    bool isBuiltIn (int index) const;
    const Row* rowAt (int y) const;
    void paintContent (juce::Graphics&);
    void contentDrag (const juce::MouseEvent&);
    void contentUp (const juce::MouseEvent&);
    void drawRowHeader (juce::Graphics&, const Row&, int w);
};

/** A strip's sends (drag up / down for the level, click for options) and its output (click to route). */
class RouteList : public juce::Component, public juce::SettableTooltipClient
{
public:
    RouteList (SessionController& s, ProjectDocument& d, juce::ValueTree t) : session (s), doc (d), track (t) {}
    int rowsWanted() const;
    static constexpr int rowHeight = 20;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    SessionController& session;
    ProjectDocument& doc;
    juce::ValueTree track;
    juce::ValueTree dragSend;
    double dragStartLevel = 0.0;
    bool dragged = false;
    juce::ValueTree sendAt (int row) const;
};

/** One mixer channel strip bound to a TRACK node; meter reads the engine's track meter. */
class ChannelStrip : public juce::Component, private juce::ValueTree::Listener, private juce::ChangeListener,
                     public juce::DragAndDropTarget
{
public:
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return d.description.toString().startsWith ("fftrack|") || insertList.isInterestedInDragSource (d);
    }
    void itemDropped (const SourceDetails& d) override;
    void itemDragMove (const SourceDetails& d) override;
    void itemDragExit (const SourceDetails&) override { if (dropSide != 0) { dropSide = 0; repaint(); } }
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { trackDragging = false; }
    ChannelStrip (ProjectDocument&, EngineHost&, SessionController&, juce::ValueTree track, int slot);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    ~ChannelStrip() override;
    void rename() { nameLabel.showEditor(); }
    juce::ValueTree getTrack() const { return track; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();
    static constexpr int width = 168;   // wide enough for a rack slot (rack 168 x 588 = 2:7)
    int wantedFaderTop() const;
    int dropSide = 0;   // -1 / +1: a dragged track will land before / after this strip
    int alignTop = 0;   // set by the mixer: the lowest fader top of all strips

private:
    ProjectDocument& doc;
    EngineHost& host;
    SessionController& session;
    juce::ValueTree track;
    int slot;
    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" }, arm { "R" };
    ContextButton ampButton { "NAM AMP" };   // instrument tracks: the instrument (right-click: change / remove)
    MonitorButton mon;
    PowerButton ampPower;
    juce::Label nameLabel;
    InsertList insertList { session, track };
    juce::Viewport insertView;   // the plugin list scrolls when there are more plugins than room
    RouteList routeList { session, doc, track };
    void showMenu();
    bool trackDragging = false;
    float level = -100.0f, hold = -100.0f;
    int holdTicks = 0, clipTicks = 0;
    juce::uint32 clipSeen = 0;
    juce::Rectangle<int> meterArea() const;
    bool updating = false;
    double faderPrev = 0.0;
    void refresh();
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { refresh(); resized(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { refresh(); resized(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { refresh(); resized(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { insertList.repaint(); }
};

/** Mixer: a channel strip per track plus the main output. */
class MixerPage : public juce::Component, public juce::DragAndDropContainer, private juce::Timer, private juce::ValueTree::Listener, private juce::ChangeListener
{
public:
    MixerPage (EngineHost&, ProjectDocument&, SessionController&, juce::PropertiesFile&);
    ~MixerPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void rebind();

private:
    EngineHost& host;
    ProjectDocument& doc;
    SessionController& session;
    juce::PropertiesFile& settings;
    juce::ValueTree tracksNode;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { for (auto* s : strips) s->repaint(); }
    juce::Viewport viewport;
    juce::Component stripHolder;
    juce::OwnedArray<ChannelStrip> strips;
    juce::OwnedArray<RackView> racks;   // one on top of each strip
    int rackH = 588;
    static constexpr int rackBarH = 12;
    int stripTop() const { return rackH + rackBarH; }
    /** The bar under all the racks: drag it to make every rack shorter or taller. Double-click: default height. */
    struct RackDivider final : public juce::Component
    {
        explicit RackDivider (MixerPage& p) : page (p) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); setRepaintsOnMouseActivity (true); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override { startH = page.rackH; }
        void mouseDrag (const juce::MouseEvent& e) override { page.setRackHeight (startH + e.getDistanceFromDragStartY()); }
        void mouseDoubleClick (const juce::MouseEvent&) override { page.setRackHeight (588); }
        MixerPage& page;
        int startH = 0;
    } rackDivider { *this };
    void setRackHeight (int h);
    juce::Slider fader;
    MeterView meterL, meterR;
    juce::Rectangle<int> masterBox, scale;
    juce::Label emptyLabel;
    juce::TextButton addTrackButton { "+ Track" };
    std::unique_ptr<InsertList> masterInserts;   // plugins on the master bus
    int masterRows = 0;
    int faderTop = 0, lastAutoTop = -1;
    static int widthOf (const juce::ValueTree& t);   // a strip's own width (drag its right edge)
    /** The handle on a strip's right edge: drag to widen or narrow that one strip; double-click for the standard width. */
    struct WidthGrip final : public juce::Component
    {
        WidthGrip (MixerPage& p, juce::ValueTree t) : page (p), track (t) { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); setRepaintsOnMouseActivity (true); }
        void paint (juce::Graphics& g) override { if (isMouseOverOrDragging()) { g.setColour (palette::accent.withAlpha (0.55f)); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f, 0.0f), 1.5f); } }
        void mouseDown (const juce::MouseEvent&) override { startW = MixerPage::widthOf (track); }
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        MixerPage& page;
        juce::ValueTree track;
        int startW = 0;
    };
    juce::OwnedArray<WidthGrip> grips;
    juce::Viewport masterInsertView;

    int autoFaderTop() const;

    void rebuild();
    void timerCallback() override;
    void visibilityChanged() override { if (isShowing()) startTimerHz (30); else stopTimer(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override { if (c.hasType ("TRACK")) rebuild(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override { if (c.hasType ("TRACK")) rebuild(); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override { if (p == tracksNode) rebuild(); }
};
} // namespace ff
