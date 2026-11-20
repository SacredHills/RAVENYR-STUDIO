#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "../../ProjectDocument.h"
#include "../../EngineHost.h"
#include "../../SessionController.h"
#include "../arrange/PianoRoll.h"

namespace ff
{
/** MIDI Studio, Keys part: piano, synth and bass. Each keys track is one MIDI clip, edited in the piano roll. Plays the
    project (so it can be heard with everything else), looping the clip. */
class KeysPage : public juce::Component, private juce::ValueTree::Listener, private juce::AsyncUpdater, private juce::Timer
{
public:
    KeysPage (ProjectDocument&, EngineHost&, SessionController&, juce::PropertiesFile&);
    ~KeysPage() override;
    void rebind();
    void setActive (bool, bool keepPlaying = false);
    bool endBackground();   // stops background playback and restores the project's loop settings; true if it was playing
    void togglePlay();
    void goToStart();
    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int tabsReserve = 156;   // room at the left of the first row for the page's mode tabs

private:
    ProjectDocument& doc;
    EngineHost& host;
    SessionController& session;
    juce::PropertiesFile& settings;
    juce::ValueTree root, track, clip;
    std::vector<juce::ValueTree> trackList;
    std::unique_ptr<PianoRoll> roll;
    bool active = false, lingering = false, updating = false;

    juce::ComboBox trackCombo, barsCombo;
    juce::TextButton newTrackButton { "+ Track" }, deleteTrackButton { "Delete track" }, instrumentButton { "Instrument..." },
                     backButton { "|<" }, loopButton { "Loop" };
    juce::Label lengthLabel { {}, "Length" }, hint;

    void refresh();
    void handleAsyncUpdate() override { refresh(); }
    void timerCallback() override { if (! host.engine.isPlaying()) endBackground(); }
    void valueTreePropertyChanged (juce::ValueTree& n, const juce::Identifier&) override { if (! n.hasType (midiclip::MIDI)) triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }

    juce::ValueTree makeTrack();
    juce::ValueTree firstClip (const juce::ValueTree& t) const;
    juce::ValueTree makeClip (int bars);
    double barQuarters() const;
    void startPlayback();
    void message (const juce::String& title, const juce::String& text);
};
} // namespace ff
