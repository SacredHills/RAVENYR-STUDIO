#pragma once
#include "../EngineHost.h"
#include "../ProjectDocument.h"
#include "../SessionController.h"
#include "../Widgets.h"

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
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();
    static constexpr int width = 104;
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
    juce::Slider fader;
    MeterView meterL, meterR;
    juce::Rectangle<int> masterBox, scale;
    juce::Label emptyLabel;
    juce::TextButton addTrackButton { "+ Track" };
    std::unique_ptr<InsertList> masterInserts;   // plugins on the master bus
    int masterRows = 0;
    int faderTop = 0, lastAutoTop = -1;
    juce::Viewport masterInsertView;

    /** The bar across all strips at the top of the faders: drag it down to shrink every fader (more room for
        plugins), up to make them longer. Double-click: back to automatic. */
    struct FaderDivider final : public juce::Component
    {
        explicit FaderDivider (MixerPage& p) : page (p) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); setRepaintsOnMouseActivity (true); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override { startTop = page.faderTop; }
        void mouseDrag (const juce::MouseEvent& e) override { page.setFaderTop (startTop + e.getDistanceFromDragStartY()); }
        void mouseDoubleClick (const juce::MouseEvent&) override { page.setFaderTop (-1); }
        MixerPage& page;
        int startTop = 0;
    } divider { *this };
    int autoFaderTop() const;
    void setFaderTop (int top);   // -1: automatic (just below the tallest plugin / send list)

    void rebuild();
    void timerCallback() override;
    void visibilityChanged() override { if (isShowing()) startTimerHz (30); else stopTimer(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override { if (c.hasType ("TRACK")) rebuild(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override { if (c.hasType ("TRACK")) rebuild(); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override { if (p == tracksNode) rebuild(); }
};
} // namespace ff
