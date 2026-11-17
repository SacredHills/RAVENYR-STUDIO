#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "ProjectDocument.h"
#include "EngineHost.h"
#include "Widgets.h"
#include "core/TapTempo.h"
#include "engine/fx/RigBank.h"

namespace ff
{
/** Button drawing a vector icon (play, stop, return-to-zero, metronome). */
class IconButton : public juce::Button
{
public:
    enum class Icon { play, stop, toStart, metronome, record, loop };
    IconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i) {}
    void setIcon (Icon i) { icon = i; repaint(); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    std::function<void()> onRightClick;   // when set, a right-click calls this instead of clicking the button
    void clicked (const juce::ModifierKeys& m) override
    {
        if (m.isPopupMenu() && onRightClick) { onRightClick(); return; }
        juce::Button::clicked (m);
    }
private:
    Icon icon;
};

/** Compact tuner readout for the transport bar (note + cents needle). Click opens the full tuner window. */
class TunerDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    TunerDisplay (EngineHost& h, juce::PropertiesFile* s) : host (h), settings (s) {}
    void update();
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent& e) override { if (onClick && e.mouseWasClicked()) onClick(); }
    std::function<void()> onClick;
private:
    EngineHost& host;
    juce::PropertiesFile* settings;
    juce::String note;
    double cents = 0.0, shownCents = 0.0;
    int holdTicks = 0;
};

/** Always-visible transport: locate/play, click, position, tempo, meter, tap,
    real-time meters, CPU and device status. */
class TransportBar : public juce::Component, private juce::Timer, private juce::ValueTree::Listener
{
public:
    TransportBar (EngineHost&, ProjectDocument&, juce::PropertiesFile* settings = nullptr);
    ~TransportBar() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    void rebind();              // after project replaced
    void tapTempo();
    void toggleClick();
    void toggleLoop();
    void togglePunch();
    std::function<void()> onRecord;
    std::function<void()> onOpenTuner, onOpenMetronome;
    /** What the audio engine's time is spent on (clicking the CPU readout shows it). Set by the owner of the rig bank. */
    std::function<std::vector<fx::RigBank::CpuRow>()> cpuRows;
    std::function<void()> onSettings;
    std::function<void()> onPlayStop, onReturnToStart;   // the page in front decides what play / return-to-start do (else the arrangement)
    void setSettingsActive (bool on) { settingsButton.setToggleState (on, juce::dontSendNotification); }
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    EngineHost& host;
    ProjectDocument& doc;
    juce::ValueTree transportNode, metroNode;

    IconButton rtzButton { "Return to start", IconButton::Icon::toStart };
    IconButton playButton { "Play / Stop", IconButton::Icon::play };
    IconButton clickButton { "Metronome click", IconButton::Icon::metronome };
    juce::TextButton clickSettingsButton { "..." };
    IconButton recordButton { "Record", IconButton::Icon::record };
    IconButton loopButton { "Loop", IconButton::Icon::loop };
    juce::TextButton punchButton { "PUNCH" }, countInButton { "COUNT-IN" };
    int recordBlink = 0;
    juce::Label barsLabel, timeLabel, bpmCaption, sigCaption;
    juce::Slider tempoSlider;
    juce::ComboBox sigNum, sigDen;
    juce::TextButton tapButton { "TAP" };
    MeterView inMeter, outL, outR;
    TunerDisplay tunerDisplay;

    juce::Label inCaption, outCaption, cpuLabel;
    juce::TextButton settingsButton { "Settings" };
    TapTempo tap;
    double lastTapMs = 0.0;
    juce::uint32 lastMidi = 0;
    int midiFlash = 0;
    bool updating = false;

    void timerCallback() override;
    void showCpuBreakdown();
    void refreshFromProject();
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { refreshFromProject(); }
};
} // namespace ff
