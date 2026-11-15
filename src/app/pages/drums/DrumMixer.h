#pragma once
#include "../../ProjectDocument.h"
#include "../../EngineHost.h"
#include "../../StudioLookAndFeel.h"
#include "core/DrumModel.h"

namespace ff
{
/** Drum Studio mixer: a strip per kit piece with volume fader, pan, mute / solo and a meter. Settings live in the
    project's drum kit (DRUMS/KIT) and apply to pattern / song playback, the tab play-along and Render to Studio. */
class DrumMixer : public juce::Component, private juce::Timer
{
public:
    DrumMixer (ProjectDocument& d, EngineHost& h) : doc (d), host (h)
    {
        viewport.setViewedComponent (&holder, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);
        for (int p = 0; p < drums::numPieces; ++p)
            holder.addAndMakeVisible (strips.add (new Strip (*this, p)));
    }

    void setDrums (juce::ValueTree d) { drumsTree = d; refresh(); }
    void refresh() { for (auto* s : strips) s->refresh(); }

    void visibilityChanged() override { if (isShowing()) startTimerHz (30); else stopTimer(); }
    void paint (juce::Graphics& g) override { g.fillAll (palette::bg); }
    void resized() override
    {
        viewport.setBounds (getLocalBounds().reduced (8, 4));
        const int h = juce::jmax (260, viewport.getHeight() - viewport.getScrollBarThickness());
        holder.setSize (juce::jmax (viewport.getWidth(), strips.size() * stripWidth), h);
        for (int i = 0; i < strips.size(); ++i) strips[i]->setBounds (i * stripWidth, 0, stripWidth, h);
    }

private:
    static constexpr int stripWidth = 64;
    ProjectDocument& doc;
    EngineHost& host;
    juce::ValueTree drumsTree;
    juce::Viewport viewport;
    juce::Component holder;

    struct Strip : public juce::Component
    {
        Strip (DrumMixer& o, int p) : owner (o), piece (p)
        {
            fader.setSliderStyle (juce::Slider::LinearVertical);
            fader.setSliderSnapsToMousePosition (false);
            fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 18);
            fader.setRange (-60.0, 6.0, 0.1);
            fader.setSkewFactorFromMidPoint (-12.0);
            fader.setDoubleClickReturnValue (true, 0.0);
            fader.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf") : juce::String (v, 1); };
            fader.onDragStart = [this] { owner.doc.undo().beginNewTransaction ("Drum volume"); };
            fader.onValueChange = [this] { if (! updating) set ("gain", fader.getValue()); };
            addAndMakeVisible (fader);
            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.getProperties().set ("pan", true);
            pan.setDoubleClickReturnValue (true, (double) drums::pieceInfo (piece).pan);
            pan.setPopupDisplayEnabled (true, true, nullptr);
            pan.textFromValueFunction = [] (double v) { return std::abs (v) < 0.005 ? juce::String ("C") : juce::String (juce::roundToInt (std::abs (v) * 100)) + (v < 0 ? " L" : " R"); };
            pan.onDragStart = [this] { owner.doc.undo().beginNewTransaction ("Drum pan"); };
            pan.onValueChange = [this] { if (! updating) set ("pan", pan.getValue()); };
            addAndMakeVisible (pan);
            for (auto* b : { &mute, &solo })
            {
                b->setClickingTogglesState (false);
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (*b);
            }
            mute.setColour (juce::TextButton::buttonOnColourId, palette::accent);
            solo.setColour (juce::TextButton::buttonOnColourId, palette::warn);
            mute.onClick = [this] { owner.doc.undo().beginNewTransaction ("Mute drum"); set ("mute", ! mute.getToggleState()); };
            solo.onClick = [this] { owner.doc.undo().beginNewTransaction ("Solo drum"); set ("solo", ! solo.getToggleState()); };
            setTooltip (juce::String (drums::pieceInfo (piece).name) + ": click the name to hear it");
        }
        void set (const juce::String& what, const juce::var& v)
        {
            if (owner.drumsTree.isValid()) drums::setPieceMix (owner.drumsTree, piece, what, v, &owner.doc.undo());
        }
        void refresh()
        {
            if (! owner.drumsTree.isValid()) return;
            const juce::ScopedValueSetter<bool> svs (updating, true);
            const auto m = drums::pieceMix (owner.drumsTree, piece);
            fader.setValue (m.gainDb, juce::dontSendNotification);
            pan.setValue (m.pan, juce::dontSendNotification);
            mute.setToggleState (m.mute, juce::dontSendNotification);
            solo.setToggleState (m.solo, juce::dontSendNotification);
        }
        void setTooltip (const juce::String& t) { tip = t; }
        void tick()
        {
            const float pk = owner.host.engine.drums().piecePeak[(size_t) piece].exchange (0.0f);
            const float db = juce::Decibels::gainToDecibels (pk, -100.0f);
            level = db > level ? db : juce::jmax (db, level - 1.6f);
            repaint (meterArea());
        }
        juce::Rectangle<int> meterArea() const { return fader.getBounds().withTrimmedBottom (20).removeFromRight (8).translated (6, 0); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.y < 34) owner.host.engine.previewDrum (piece, 0.8f);   // audition
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (2, 0);
            g.setColour (palette::panel);
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
            g.setColour (palette::text);
            g.setFont (uiFont (11.0f, true));
            g.drawFittedText (drums::pieceInfo (piece).name, r.removeFromTop (34).reduced (3, 2), juce::Justification::centred, 2);
            auto m = meterArea();
            g.setColour (palette::well);
            g.fillRect (m);
            const float n = juce::jlimit (0.0f, 1.0f, (level + 60.0f) / 60.0f);
            g.setColour (level > -0.5f ? palette::bad : (level > -9.0f ? palette::warn : palette::good));
            g.fillRect (m.withTop (m.getBottom() - juce::roundToInt (n * m.getHeight())));
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (4, 0).withTrimmedTop (34);
            pan.setBounds (r.removeFromTop (36).withSizeKeepingCentre (34, 34));
            r.removeFromTop (4);
            auto b = r.removeFromTop (22);
            mute.setBounds (b.removeFromLeft (b.getWidth() / 2).reduced (1, 0));
            solo.setBounds (b.reduced (1, 0));
            r.removeFromTop (6);
            fader.setBounds (r.withTrimmedBottom (4).withTrimmedRight (8));
        }
        DrumMixer& owner;
        int piece;
        juce::Slider fader, pan;
        juce::TextButton mute { "M" }, solo { "S" };
        juce::String tip;
        float level = -100.0f;
        bool updating = false;
    };
    juce::OwnedArray<Strip> strips;

    void timerCallback() override { for (auto* s : strips) s->tick(); }
};
} // namespace ff
