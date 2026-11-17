#pragma once
#include "../drums/DrumPage.h"
#include "../keys/KeysPage.h"

namespace ff
{
/** MIDI Studio: a Drums tab and a Keys tab (piano / synth / bass). */
class MidiStudioPage : public juce::Component
{
public:
    MidiStudioPage (ProjectDocument& d, EngineHost& h, SessionController& s, juce::PropertiesFile& st)
        : settings (st), drums (d, h, s, st), keys (d, h, s, st)
    {
        addAndMakeVisible (drums);
        addChildComponent (keys);
        for (auto* b : { &drumsTab, &keysTab })
        {
            b->setRadioGroupId (7201);
            b->setClickingTogglesState (true);
            addAndMakeVisible (*b);
        }
        drumsTab.setTooltip ("Drum patterns and songs");
        keysTab.setTooltip ("Piano, synth and bass: piano-roll editor and generator");
        drumsTab.onClick = [this] { setMode (0); };
        keysTab.onClick = [this] { setMode (1); };
        mode = settings.getIntValue ("midiStudioMode", 0) == 1 ? 1 : 0;
        applyMode();
    }

    void rebind() { drums.rebind(); keys.rebind(); }
    void setActive (bool on)
    {
        pageActive = on;
        drums.setActive (on && mode == 0);
        keys.setActive (on && mode == 1);
    }
    void togglePlay() { if (mode == 0) drums.togglePlay(); else keys.togglePlay(); }
    void goToStart() { if (mode == 0) drums.goToStart(); else keys.goToStart(); }

    void resized() override
    {
        drums.setBounds (getLocalBounds());
        keys.setBounds (getLocalBounds());
        auto t = juce::Rectangle<int> (8, 7, 148, 28);
        drumsTab.setBounds (t.removeFromLeft (72)); t.removeFromLeft (4);
        keysTab.setBounds (t.removeFromLeft (72));
    }

private:
    juce::PropertiesFile& settings;
    DrumPage drums;
    KeysPage keys;
    juce::TextButton drumsTab { "Drums" }, keysTab { "Keys" };
    int mode = 0;
    bool pageActive = false;

    void setMode (int m)
    {
        if (m == mode) return;
        drums.setActive (false);
        keys.setActive (false);
        mode = m;
        settings.setValue ("midiStudioMode", mode);
        applyMode();
        drums.setActive (pageActive && mode == 0);
        keys.setActive (pageActive && mode == 1);
    }
    void applyMode()
    {
        drums.setVisible (mode == 0);
        keys.setVisible (mode == 1);
        drumsTab.setToggleState (mode == 0, juce::dontSendNotification);
        keysTab.setToggleState (mode == 1, juce::dontSendNotification);
        drumsTab.toFront (false);
        keysTab.toFront (false);
    }
};
} // namespace ff
