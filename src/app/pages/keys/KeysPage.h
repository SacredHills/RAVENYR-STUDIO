#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "../../ProjectDocument.h"
#include "../../EngineHost.h"
#include "../../SessionController.h"
#include "../arrange/PianoRoll.h"
#include "core/KeysGen.h"

namespace ff
{
/** MIDI Studio, Keys part: piano, synth and bass. A MIDI clip on an instrument track, edited in the piano roll, with a
    generator (chords, arpeggios, bass lines, riffs in a key / scale / progression) that fills the clip. Plays the project
    (so it can be heard with everything else), looping the clip. */
class KeysPage : public juce::Component, private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    KeysPage (ProjectDocument&, EngineHost&, SessionController&, juce::PropertiesFile&);
    ~KeysPage() override;
    void rebind();
    void setActive (bool);
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
    std::vector<juce::ValueTree> trackList, clipList;
    std::unique_ptr<PianoRoll> roll;
    bool active = false, updating = false;
    int seedCounter = 1;

    juce::ComboBox trackCombo, clipCombo, styleCombo, keyCombo, scaleCombo, progCombo, genCombo, octaveCombo, barsCombo;
    juce::TextButton newTrackButton { "+ Track" }, instrumentButton { "Instrument..." }, backButton { "|<" }, loopButton { "Loop" },
                     generateButton { "Generate" }, newClipButton { "New clip" }, deleteClipButton { "Delete clip" };
    juce::Label hint;

    void refresh();
    void handleAsyncUpdate() override { refresh(); }
    void valueTreePropertyChanged (juce::ValueTree& n, const juce::Identifier&) override { if (! n.hasType (midiclip::MIDI)) triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }

    void applyStyleDefaults();
    juce::ValueTree makeTrack();
    juce::ValueTree makeClip (int bars);
    int tsNum() const;
    int tsDen() const;
    void generate();
    void startPlayback();
    void message (const juce::String& title, const juce::String& text);
};
} // namespace ff
