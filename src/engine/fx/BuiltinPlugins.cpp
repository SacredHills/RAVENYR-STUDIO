#include "ScrollSafeSlider.h"
#include "BuiltinPlugins.h"
#include "Biquad.h"
#include "AmpRig.h"
#include "NamModel.h"
#include "RigBank.h"
#include "core/RigModel.h"
#include "engine/DrumSynth.h"
#include "core/DrumModel.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <cstring>

namespace ff::fx::builtin
{
namespace
{
namespace col
{
    // the app's gothic palette (see StudioLookAndFeel.h)
    const juce::Colour bg { 0xff0a0a0a }, panel { 0xff141414 }, well { 0xff050505 }, outline { 0xff353535 }, text { 0xffeeeeee },
        dim { 0xff8a8a8a }, accent { 0xfff2f2f2 }, graph { 0xffc32f3d }, good { 0xff4fae80 }, info { 0xff8c97d8 }, bad { 0xffff4655 };
}

using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
using APVTS = juce::AudioProcessorValueTreeState;

std::unique_ptr<juce::AudioParameterFloat> param (const char* id, const char* name, float lo, float hi, float def,
                                                  const char* unit, float centre = 0.0f, float step = 0.0f)
{
    juce::NormalisableRange<float> r (lo, hi, step);
    if (centre > lo && centre < hi) r.setSkewForCentre (centre);
    const juce::String u (unit);
    auto toText = [u] (float v, int) {
        if (u == "Hz") return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
        if (u == "ms") return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (v, v < 10.0f ? 1 : 0) + " ms";
        if (u == "s") return juce::String (v, 2) + " s";
        if (u == "dB") return juce::String (v, 1) + " dB";
        if (u == "%") return juce::String (juce::roundToInt (v)) + " %";
        if (u == ":1") return juce::String (v, 1) + ":1";
        return juce::String (v, 2) + (u.isEmpty() ? juce::String() : " " + u);
    };
    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def,
                                                       juce::AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction (toText));
}

// =====================================================================================================  base

class Base : public juce::AudioPluginInstance
{
public:
    static BusesProperties busesFor (const Info& i)
    {
        if (i.instrument) return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo());
        return BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo()).withOutput ("Output", juce::AudioChannelSet::stereo());
    }
    Base (const Info& i, Layout layout)
        : juce::AudioPluginInstance (busesFor (i)),
          info (i), state (*this, nullptr, "STATE", std::move (layout)) {}

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = describe (info); }
    const juce::String getName() const override { return info.name; }
    void releaseResources() override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return info.instrument; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& d) override
    {
        if (auto x = state.copyState().createXml()) copyXmlToBinary (*x, d);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (auto x = getXmlFromBinary (data, size); x != nullptr && x->hasTagName (state.state.getType()))
            state.replaceState (juce::ValueTree::fromXml (*x));
    }
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
        if (info.instrument) return in.isDisabled() && out == juce::AudioChannelSet::stereo();
        return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
    }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples();
        const int ch = juce::jmin (b.getNumChannels(), getTotalNumOutputChannels());
        if (n == 0 || ch == 0) return;
        float* L = b.getWritePointer (0);
        float* R = ch > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = getTotalNumInputChannels(); i < ch; ++i) b.clear (i, 0, n);
        process (L, R, n);
    }

    /** R is nullptr when mono. */
    virtual void process (float* L, float* R, int n) noexcept = 0;
    /** Optional picture above the knobs (EQ curve, gain reduction...). */
    virtual std::unique_ptr<juce::Component> createDisplay() { return nullptr; }
    virtual int displayHeight() const { return 0; }

    /** A parameter in its own units. Read from the parameter itself, so a host's setValue (automation) is seen at
        once (the value-tree state's copy only follows setValueNotifyingHost). */
    float p (const char* id) const noexcept
    {
        auto* rp = state.getParameter (id);
        return rp != nullptr ? rp->convertFrom0to1 (rp->getValue()) : 0.0f;
    }
    double sampleRate() const noexcept { return juce::jmax (8000.0, getSampleRate()); }

    const Info& info;
    APVTS state;
};

// =====================================================================================================  editor

class Editor final : public juce::AudioProcessorEditor
{
public:
    explicit Editor (Base& b) : juce::AudioProcessorEditor (b), proc (b)
    {
        display = b.createDisplay();
        if (display != nullptr) addAndMakeVisible (*display);
        title.setText (b.info.name, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, col::text);
        addAndMakeVisible (title);
        sub.setText (b.info.description, juce::dontSendNotification);
        sub.setFont (juce::Font (juce::FontOptions (12.0f)));
        sub.setColour (juce::Label::textColourId, col::dim);
        addAndMakeVisible (sub);

        for (auto* p : b.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
            if (rp == nullptr) continue;
            auto k = std::make_unique<Knob>();
            k->label.setText (rp->getName (40), juce::dontSendNotification);
            k->label.setJustificationType (juce::Justification::centred);
            k->label.setFont (juce::Font (juce::FontOptions (11.5f)));
            k->label.setColour (juce::Label::textColourId, col::dim);
            if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (rp))
            {
                k->combo.addItemList (cp->choices, 1);
                k->comboAttach = std::make_unique<juce::ComboBoxParameterAttachment> (*cp, k->combo);
                addAndMakeVisible (k->combo);
                addAndMakeVisible (k->label);
            }
            else if (auto* bp = dynamic_cast<juce::AudioParameterBool*> (rp))
            {
                k->toggle.setButtonText (rp->getName (40));
                k->toggle.setClickingTogglesState (true);
                k->toggle.setColour (juce::TextButton::buttonOnColourId, col::accent);
                k->buttonAttach = std::make_unique<juce::ButtonParameterAttachment> (*bp, k->toggle);
                addAndMakeVisible (k->toggle);
            }
            else
            {
                k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 18);
                k->slider.setColour (juce::Slider::rotarySliderFillColourId, col::accent);
                k->attach = std::make_unique<juce::SliderParameterAttachment> (*rp, k->slider);
                k->slider.setDoubleClickReturnValue (true, rp->convertFrom0to1 (rp->getDefaultValue()));
                addAndMakeVisible (k->slider);
                addAndMakeVisible (k->label);
            }
            knobs.push_back (std::move (k));
        }
        const int cols = juce::jmin (7, (int) knobs.size());
        const int rows = ((int) knobs.size() + cols - 1) / juce::jmax (1, cols);
        setSize (juce::jmax (520, cols * 88 + 24), 56 + b.displayHeight() + (b.displayHeight() > 0 ? 10 : 0) + rows * 104 + 12);
    }

    void paint (juce::Graphics& g) override { g.fillAll (col::bg); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        auto top = r.removeFromTop (40);
        title.setBounds (top.removeFromTop (22));
        sub.setBounds (top);
        if (display != nullptr)
        {
            display->setBounds (r.removeFromTop (proc.displayHeight()));
            r.removeFromTop (10);
        }
        const int cols = juce::jmin (7, (int) knobs.size());
        const int w = r.getWidth() / juce::jmax (1, cols);
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            const int c = (int) i % cols, row = (int) i / cols;
            auto cell = juce::Rectangle<int> (r.getX() + c * w, r.getY() + row * 104, w, 100).reduced (4, 0);
            auto& k = *knobs[i];
            if (k.toggle.isVisible()) { k.toggle.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 28)); continue; }
            if (k.combo.isVisible())
            {
                k.label.setBounds (cell.removeFromTop (16));
                k.combo.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 26));
                continue;
            }
            k.label.setBounds (cell.removeFromTop (16));
            k.slider.setBounds (cell);
        }
    }

private:
    struct Knob
    {
        ScrollSafeSlider slider;
        juce::Label label;
        juce::TextButton toggle;
        juce::ComboBox combo;
        std::unique_ptr<juce::ComboBoxParameterAttachment> comboAttach;
        std::unique_ptr<juce::SliderParameterAttachment> attach;
        std::unique_ptr<juce::ButtonParameterAttachment> buttonAttach;
    };
    Base& proc;
    juce::Label title, sub;
    std::unique_ptr<juce::Component> display;
    std::vector<std::unique_ptr<Knob>> knobs;
};

juce::AudioProcessorEditor* Base::createEditor() { return new Editor (*this); }

/** Sets a parameter from the UI in its own units, as one gesture step. */
void setParam (APVTS& s, const char* id, float value)
{
    if (auto* p = s.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (value));
}
void beginGesture (APVTS& s, const char* id) { if (auto* p = s.getParameter (id)) p->beginChangeGesture(); }
void endGesture (APVTS& s, const char* id) { if (auto* p = s.getParameter (id)) p->endChangeGesture(); }

// =====================================================================================================  EQ

#include "BuiltinEq.inc"
#include "BuiltinNam.inc"

// =====================================================================================================  compressor

class Compressor final : public Base
{
public:
    explicit Compressor (const Info& i) : Base (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (param ("threshold", "Threshold", -60.0f, 0.0f, -18.0f, "dB"));
        l.add (param ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", 4.0f));
        l.add (param ("attack", "Attack", 0.1f, 100.0f, 10.0f, "ms", 10.0f));
        l.add (param ("release", "Release", 10.0f, 1000.0f, 120.0f, "ms", 150.0f));
        l.add (param ("knee", "Knee", 0.0f, 12.0f, 6.0f, "dB"));
        l.add (param ("makeup", "Makeup", 0.0f, 24.0f, 0.0f, "dB"));
        l.add (param ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%"));
        return l;
    }
    void prepareToPlay (double, int) override { env = 0.0f; grDb = 0.0f; }
    float gainComputer (float levelDb) const noexcept
    {
        const float t = p ("threshold"), ratio = p ("ratio"), knee = p ("knee");
        const float over = levelDb - t;
        if (knee > 0.0f && std::abs (over) <= knee * 0.5f)
            return (1.0f / ratio - 1.0f) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
        return over > 0.0f ? over * (1.0f / ratio - 1.0f) : 0.0f;
    }
    void process (float* L, float* R, int n) noexcept override
    {
        const double sr = sampleRate();
        const float atk = std::exp (-1.0f / (float) (sr * p ("attack") * 0.001));
        const float rel = std::exp (-1.0f / (float) (sr * p ("release") * 0.001));
        const float makeup = juce::Decibels::decibelsToGain (p ("makeup")), mix = p ("mix") * 0.01f;
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float x = juce::jmax (std::abs (L[i]), R != nullptr ? std::abs (R[i]) : 0.0f);
            const float level = juce::Decibels::gainToDecibels (x, -120.0f);
            const float target = -gainComputer (level);   // reduction in dB, >= 0
            env = target > env ? atk * env + (1.0f - atk) * target : rel * env + (1.0f - rel) * target;
            const float g = juce::Decibels::decibelsToGain (-env) * makeup;
            const float gm = mix * g + (1.0f - mix);
            L[i] *= gm;
            if (R != nullptr) R[i] *= gm;
            maxGr = juce::jmax (maxGr, env);
        }
        grDb.store (juce::jmax (maxGr, grDb.load (std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    std::unique_ptr<juce::Component> createDisplay() override;
    int displayHeight() const override { return 150; }
    std::atomic<float> grDb { 0.0f };   // largest gain reduction since the display took it
private:
    float env = 0.0f;
};

/** Transfer curve and a gain-reduction meter. */
class CompDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit CompDisplay (Compressor& c) : comp (c) { startTimerHz (30); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (r, 6.0f);
        auto curveArea = r.removeFromLeft (r.getHeight()).reduced (10.0f);
        g.setColour (col::outline);
        g.drawRect (curveArea);
        g.drawLine (curveArea.getX(), curveArea.getBottom(), curveArea.getRight(), curveArea.getY(), 1.0f);
        juce::Path p;
        for (int i = 0; i <= 60; ++i)
        {
            const float in = -60.0f + (float) i;
            const float out = in + comp.gainComputer (in);
            const float x = curveArea.getX() + (in + 60.0f) / 60.0f * curveArea.getWidth();
            const float y = curveArea.getBottom() - juce::jlimit (0.0f, 1.0f, (out + 60.0f) / 60.0f) * curveArea.getHeight();
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (col::graph);
        g.strokePath (p, juce::PathStrokeType (2.0f));
        auto meter = r.reduced (16.0f, 18.0f);
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("Gain reduction  " + juce::String (shownGr, 1) + " dB", meter.removeFromTop (18.0f), juce::Justification::centredLeft);
        meter.removeFromTop (6.0f);
        auto bar = meter.removeFromTop (16.0f);
        g.setColour (col::panel);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (col::bad.withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, shownGr / 24.0f)), 3.0f);
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (int db : { 0, 6, 12, 18, 24 })
            g.drawText (juce::String (db), juce::Rectangle<float> (bar.getX() + bar.getWidth() * (float) db / 24.0f - 10.0f, bar.getBottom() + 2.0f, 20.0f, 12.0f),
                        juce::Justification::centred);
    }
private:
    Compressor& comp;
    float shownGr = 0.0f;
    void timerCallback() override
    {
        const float gr = comp.grDb.exchange (0.0f);
        shownGr = gr > shownGr ? gr : juce::jmax (gr, shownGr - 1.0f);
        repaint();
    }
};

std::unique_ptr<juce::Component> Compressor::createDisplay() { return std::make_unique<CompDisplay> (*this); }

// =====================================================================================================  instruments

/** Base for the built-in instruments: splits each block at the MIDI events so they land on their sample. */
class InstrumentBase : public Base
{
public:
    using Base::Base;
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples();
        b.clear();
        if (b.getNumChannels() == 0 || n == 0) return;
        float* L = b.getWritePointer (0);
        float* R = b.getNumChannels() > 1 ? b.getWritePointer (1) : nullptr;
        int pos = 0;
        for (const auto meta : midi)
        {
            const int at = juce::jlimit (0, n, meta.samplePosition);
            if (at > pos) { render (L + pos, R != nullptr ? R + pos : nullptr, at - pos); pos = at; }
            handle (meta.getMessage());
        }
        if (pos < n) render (L + pos, R != nullptr ? R + pos : nullptr, n - pos);
    }
    void process (float*, float*, int) noexcept override {}
    virtual void handle (const juce::MidiMessage&) noexcept = 0;
    virtual void render (float* L, float* R, int n) noexcept = 0;
};

/** Polyphonic subtractive synth: two detuned oscillators, a resonant low-pass with its own envelope, ADSR. */
class BasicSynth final : public InstrumentBase
{
public:
    static constexpr const char* waves[] = { "Saw", "Square", "Triangle", "Sine", "Pluck" };
    explicit BasicSynth (const Info& i) : InstrumentBase (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "wave", 1 }, "Wave", juce::StringArray (waves, 5), 0));
        l.add (param ("detune", "Detune", 0.0f, 50.0f, 8.0f, "ct"));
        l.add (param ("cutoff", "Cutoff", 80.0f, 18000.0f, 3500.0f, "Hz", 2000.0f));
        l.add (param ("reso", "Resonance", 0.0f, 100.0f, 20.0f, "%"));
        l.add (param ("envAmt", "Filter env", 0.0f, 100.0f, 35.0f, "%"));
        l.add (param ("attack", "Attack", 0.5f, 2000.0f, 5.0f, "ms", 50.0f));
        l.add (param ("decay", "Decay", 5.0f, 4000.0f, 400.0f, "ms", 400.0f));
        l.add (param ("sustain", "Sustain", 0.0f, 100.0f, 70.0f, "%"));
        l.add (param ("release", "Release", 5.0f, 5000.0f, 300.0f, "ms", 400.0f));
        l.add (param ("volume", "Volume", -36.0f, 6.0f, -8.0f, "dB"));
        return l;
    }
    void prepareToPlay (double, int) override { for (auto& v : voices) v = {}; pedal = false; bend = 0.0f; }
    void handle (const juce::MidiMessage& m) noexcept override
    {
        if (m.isNoteOn()) noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff()) noteOff (m.getNoteNumber());
        else if (m.isSustainPedalOn()) pedal = true;
        else if (m.isSustainPedalOff()) { pedal = false; for (auto& v : voices) if (v.held && ! v.keyDown) v.released (time); }
        else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) { v.keyDown = false; v.held = false; if (v.active) v.released (time); }
        else if (m.isPitchWheel()) bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
    }
    void render (float* L, float* R, int n) noexcept override
    {
        const double sr = sampleRate();
        const int wave = juce::roundToInt (p ("wave"));
        const float det = p ("detune"), cutoff = p ("cutoff"), reso = p ("reso") * 0.01f, envAmt = p ("envAmt") * 0.01f;
        const float a = p ("attack") * 0.001f, d = p ("decay") * 0.001f, su = p ("sustain") * 0.01f, rel = p ("release") * 0.001f;
        const float vol = juce::Decibels::decibelsToGain (p ("volume"));
        const float k = 2.0f - 1.9f * reso;   // SVF damping
        for (auto& v : voices)
        {
            if (! v.active) continue;
            const double f0 = 440.0 * std::pow (2.0, (v.note - 69 + bend) / 12.0);
            const double inc1 = f0 * std::pow (2.0, det / 1200.0) / sr, inc2 = f0 * std::pow (2.0, -det / 1200.0) / sr;
            for (int i = 0; i < n; ++i)
            {
                // envelope (linear attack, exponential decay / release)
                const double t = v.t;
                float env;
                if (! v.releasing) env = t < a ? (float) (t / a) : su + (1.0f - su) * std::exp (-(float) (t - a) / juce::jmax (0.001f, d * 0.3f));
                else env = v.relFrom * std::exp (-(float) (t - v.relAt) / juce::jmax (0.001f, rel * 0.3f));
                if (wave == 4 && ! v.releasing) env = std::exp (-(float) t / juce::jmax (0.01f, d * 0.4f));   // pluck: always decays
                v.lastEnv = env;
                v.t += 1.0 / sr;
                if (v.releasing && env < 1.0e-4f) { v.active = false; break; }
                const float o = osc (wave, v.ph1, inc1) + osc (wave, v.ph2, inc2);
                v.ph1 += inc1; if (v.ph1 >= 1.0) v.ph1 -= 1.0;
                v.ph2 += inc2; if (v.ph2 >= 1.0) v.ph2 -= 1.0;
                // filter: cutoff follows the envelope
                const float fc = juce::jlimit (30.0f, (float) (sr * 0.45), cutoff * (1.0f + envAmt * 4.0f * env));
                const float g = (float) std::tan (juce::MathConstants<double>::pi * fc / sr);
                const float hp = (o * 0.5f - (k + g) * v.s1 - v.s2) / (1.0f + g * (k + g));
                const float bp = g * hp + v.s1; v.s1 = g * hp + bp;
                const float lp = g * bp + v.s2; v.s2 = g * bp + lp;
                const float out = lp * env * v.vel * vol;
                L[i] += out;
                if (R != nullptr) R[i] += out;
            }
        }
        time += n / sr;
    }
private:
    struct Voice
    {
        bool active = false, keyDown = false, held = false, releasing = false;
        int note = 0;
        float vel = 0.0f, lastEnv = 0.0f, relFrom = 0.0f, s1 = 0.0f, s2 = 0.0f;
        double t = 0.0, relAt = 0.0, ph1 = 0.0, ph2 = 0.25, started = 0.0;
        void released (double) { if (! releasing) { releasing = true; relAt = t; relFrom = lastEnv; } held = false; }
    };
    std::array<Voice, 16> voices;
    bool pedal = false;
    float bend = 0.0f;
    double time = 0.0;

    static float polyBlep (double t, double dt) noexcept
    {
        if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }
    static float osc (int wave, double ph, double dt) noexcept
    {
        switch (wave)
        {
            case 1: { float s = ph < 0.5 ? 1.0f : -1.0f; s += polyBlep (ph, dt); s -= polyBlep (std::fmod (ph + 0.5, 1.0), dt); return s * 0.7f; }
            case 2: return (float) (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph);
            case 3: return (float) std::sin (juce::MathConstants<double>::twoPi * ph);
            default: return (float) (2.0 * ph - 1.0) - polyBlep (ph, dt);   // saw (and pluck)
        }
    }
    void noteOn (int note, float vel) noexcept
    {
        Voice* v = nullptr;
        for (auto& x : voices) if (x.active && x.note == note) { v = &x; break; }   // retrigger the same key
        if (v == nullptr) for (auto& x : voices) if (! x.active) { v = &x; break; }
        if (v == nullptr)   // steal the oldest
        {
            v = &voices[0];
            for (auto& x : voices) if (x.started < v->started) v = &x;
        }
        *v = {};
        v->active = v->keyDown = true;
        v->note = note;
        v->vel = 0.25f + 0.75f * vel;
        v->started = time;
    }
    void noteOff (int note) noexcept
    {
        for (auto& v : voices)
            if (v.active && v.note == note && v.keyDown)
            {
                v.keyDown = false;
                if (pedal) v.held = true; else v.released (time);
            }
    }
};

/** General MIDI drum kit (any channel) played by RAVENYR's drum sounds. */
class DrumKit final : public InstrumentBase
{
public:
    explicit DrumKit (const Info& i) : InstrumentBase (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (param ("volume", "Volume", -36.0f, 6.0f, -3.0f, "dB"));
        return l;
    }
    void prepareToPlay (double, int) override { drums.prepare (sampleRate()); }
    void handle (const juce::MidiMessage& m) noexcept override
    {
        if (m.isNoteOn())
        {
            int piece = drums::pieceForGmNote (m.getNoteNumber());
            if (piece < 0) piece = fallback (m.getNoteNumber());
            if (piece >= 0) drums.trigger (piece, m.getFloatVelocity());
        }
        else if (m.isAllSoundOff()) drums.allNotesOff();
    }
    void render (float* L, float* R, int n) noexcept override
    {
        float* r = R != nullptr ? R : L;
        if (R == nullptr) { tmp.resize (std::max (tmp.size(), (size_t) n)); std::fill (tmp.begin(), tmp.begin() + n, 0.0f); r = tmp.data(); }
        drums.volume = juce::Decibels::decibelsToGain (p ("volume"));
        drums.render (L, r, n);
        if (R == nullptr) for (int i = 0; i < n; ++i) L[i] = 0.5f * (L[i] + r[i]);
    }
private:
    DrumSynth drums;
    std::vector<float> tmp;
    static int fallback (int note) noexcept
    {
        if (note == 35 || note == 36) return drums::kick;
        if (note == 37) return drums::sideStick;
        if (note == 38 || note == 40) return drums::snare;
        if (note == 39) return drums::clap;
        if (note == 42 || note == 44) return note == 42 ? drums::hhClosed : drums::hhPedal;
        if (note == 46) return drums::hhOpen;
        if (note == 41 || note == 43) return drums::tomFloor;
        if (note == 45 || note == 47) return drums::tomLow;
        if (note == 48 || note == 50) return drums::tomHigh;
        if (note == 49 || note == 57 || note == 52) return note == 52 ? drums::china : drums::crash;
        if (note == 51 || note == 59) return drums::ride;
        if (note == 53) return drums::rideBell;
        if (note == 55) return drums::splash;
        if (note == 56) return drums::cowbell;
        if (note >= 60) return drums::cowbell;   // percussion
        return -1;
    }
};

// =====================================================================================================  limiter

/** Look-ahead brick-wall limiter: the gain each sample needs is held over the look-ahead window and smoothed with
    a box filter of the same length, so the gain is already down when a peak leaves the delay; a final clip at the
    ceiling catches anything left (there is nothing left in practice). */
class Limiter final : public Base
{
public:
    explicit Limiter (const Info& i) : Base (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (param ("gain", "Input gain", 0.0f, 24.0f, 0.0f, "dB"));
        l.add (param ("ceiling", "Ceiling", -12.0f, 0.0f, -1.0f, "dB"));
        l.add (param ("release", "Release", 1.0f, 1000.0f, 80.0f, "ms", 100.0f));
        return l;
    }
    void prepareToPlay (double, int) override
    {
        look = juce::jmax (1, (int) std::round (sampleRate() * 0.0015));
        setLatencySamples (look);
        for (auto& d : delay) d.assign ((size_t) look + 1, 0.0f);
        held.assign ((size_t) look + 1, 1.0f);
        dqVal.assign ((size_t) look + 2, 1.0f);
        dqIdx.assign ((size_t) look + 2, 0);
        dqHead = dqTail = 0;
        box.assign ((size_t) look, 1.0f);
        boxSum = (double) look;
        pos = 0; t = 0; env = 1.0f;
        inGain.reset (sampleRate(), 0.05);
        inGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p ("gain")));
    }
    void process (float* L, float* R, int n) noexcept override
    {
        if (delay[0].empty()) return;
        const float ceiling = juce::Decibels::decibelsToGain (p ("ceiling"));
        const float rel = 1.0f - std::exp (-1.0f / (float) (sampleRate() * p ("release") * 0.001));
        inGain.setTargetValue (juce::Decibels::decibelsToGain (p ("gain")));
        const int len = look + 1, qcap = (int) dqVal.size();
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float g0 = inGain.getNextValue();
            const float xl = L[i] * g0, xr = R != nullptr ? R[i] * g0 : xl;
            const float pk = juce::jmax (std::abs (xl), std::abs (xr));
            const float need = pk > ceiling ? ceiling / pk : 1.0f;
            // sliding minimum of `need` over the last look + 1 samples (monotonic queue)
            while (dqHead != dqTail && dqVal[(size_t) ((dqTail - 1 + qcap) % qcap)] >= need) dqTail = (dqTail - 1 + qcap) % qcap;
            dqVal[(size_t) dqTail] = need;
            dqIdx[(size_t) dqTail] = t;
            dqTail = (dqTail + 1) % qcap;
            while (dqIdx[(size_t) dqHead] <= t - len) dqHead = (dqHead + 1) % qcap;
            const float hold = dqVal[(size_t) dqHead];
            // box filter over `look` samples
            const int bi = (int) (t % look);
            boxSum += (double) hold - (double) box[(size_t) bi];
            box[(size_t) bi] = hold;
            const float target = (float) (boxSum / look);
            env = target < env ? target : env + rel * (target - env);
            // delay the audio by `look`
            const float dl = delay[0][(size_t) pos], dr = delay[1][(size_t) pos];
            delay[0][(size_t) pos] = xl;
            delay[1][(size_t) pos] = xr;
            pos = (pos + 1) % look;
            L[i] = juce::jlimit (-ceiling, ceiling, dl * env);
            if (R != nullptr) R[i] = juce::jlimit (-ceiling, ceiling, dr * env);
            maxGr = juce::jmax (maxGr, -juce::Decibels::gainToDecibels (env, -60.0f));
            ++t;
        }
        grDb.store (juce::jmax (maxGr, grDb.load (std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    std::unique_ptr<juce::Component> createDisplay() override;
    int displayHeight() const override { return 70; }
    std::atomic<float> grDb { 0.0f };
private:
    int look = 1, pos = 0, dqHead = 0, dqTail = 0;
    juce::int64 t = 0;
    std::array<std::vector<float>, 2> delay;
    std::vector<float> held, dqVal, box;
    std::vector<juce::int64> dqIdx;
    double boxSum = 0.0;
    float env = 1.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inGain;
};

class GrMeter final : public juce::Component, private juce::Timer
{
public:
    explicit GrMeter (std::atomic<float>& g) : gr (g) { startTimerHz (30); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (r, 6.0f);
        auto m = r.reduced (16.0f, 10.0f);
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("Gain reduction  " + juce::String (shown, 1) + " dB", m.removeFromTop (18.0f), juce::Justification::centredLeft);
        m.removeFromTop (6.0f);
        auto bar = m.removeFromTop (14.0f);
        g.setColour (col::panel);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (col::bad.withAlpha (0.85f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, shown / 12.0f)), 3.0f);
    }
private:
    std::atomic<float>& gr;
    float shown = 0.0f;
    void timerCallback() override
    {
        const float v = gr.exchange (0.0f);
        shown = v > shown ? v : juce::jmax (v, shown - 0.5f);
        repaint();
    }
};

std::unique_ptr<juce::Component> Limiter::createDisplay() { return std::make_unique<GrMeter> (grDb); }

// =====================================================================================================  delay

class Delay final : public Base
{
public:
    explicit Delay (const Info& i) : Base (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (param ("time", "Time", 1.0f, 2000.0f, 380.0f, "ms", 400.0f));
        l.add (param ("feedback", "Feedback", 0.0f, 95.0f, 35.0f, "%"));
        l.add (param ("mix", "Mix", 0.0f, 100.0f, 30.0f, "%"));
        l.add (param ("lowCut", "Low cut", 20.0f, 1000.0f, 80.0f, "Hz", 150.0f));
        l.add (param ("highCut", "High cut", 1000.0f, 20000.0f, 6000.0f, "Hz", 5000.0f));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pingpong", 1 }, "Ping-pong", false));
        return l;
    }
    void prepareToPlay (double, int) override
    {
        const int len = (int) (sampleRate() * 2.1) + 4;
        for (auto& b : buf) b.assign ((size_t) len, 0.0f);
        write = 0;
        time.reset (sampleRate(), 0.25);
        time.setCurrentAndTargetValue (p ("time"));
        for (auto& f : lo) f.reset();
        for (auto& f : hi) f.reset();
    }
    double getTailLengthSeconds() const override { return 6.0; }
    void process (float* L, float* R, int n) noexcept override
    {
        if (buf[0].empty()) return;
        const double sr = sampleRate();
        const int len = (int) buf[0].size();
        const float fb = p ("feedback") * 0.01f, mix = p ("mix") * 0.01f;
        const bool ping = p ("pingpong") >= 0.5f;
        for (int c = 0; c < 2; ++c)
        {
            lo[(size_t) c].set (Biquad::highPass, sr, p ("lowCut"), 0.707);
            hi[(size_t) c].set (Biquad::lowPass, sr, p ("highCut"), 0.707);
        }
        time.setTargetValue (p ("time"));
        for (int i = 0; i < n; ++i)
        {
            const double d = juce::jlimit (1.0, (double) len - 3.0, (double) time.getNextValue() * 0.001 * sr);
            double rp = (double) write - d;
            if (rp < 0) rp += len;
            const int i0 = (int) rp;
            const float fr = (float) (rp - i0);
            const int i1 = (i0 + 1) % len;
            const float dl = buf[0][(size_t) i0] + fr * (buf[0][(size_t) i1] - buf[0][(size_t) i0]);
            const float dr = buf[1][(size_t) i0] + fr * (buf[1][(size_t) i1] - buf[1][(size_t) i0]);
            const float inL = L[i], inR = R != nullptr ? R[i] : L[i];
            const float wl = hi[0].process (lo[0].process (dl)) * fb;
            const float wr = hi[1].process (lo[1].process (dr)) * fb;
            if (ping)
            {
                buf[0][(size_t) write] = 0.5f * (inL + inR) + wr;   // left repeats feed the right line and back
                buf[1][(size_t) write] = wl;
            }
            else
            {
                buf[0][(size_t) write] = inL + wl;
                buf[1][(size_t) write] = inR + wr;
            }
            write = (write + 1) % len;
            L[i] = inL * (1.0f - mix) + dl * mix;
            if (R != nullptr) R[i] = inR * (1.0f - mix) + dr * mix;
            else L[i] = inL * (1.0f - mix) + 0.5f * (dl + dr) * mix;
        }
    }
private:
    std::array<std::vector<float>, 2> buf;
    int write = 0;
    juce::SmoothedValue<float> time;
    std::array<Biquad, 2> lo, hi;
};

// =====================================================================================================  chorus

/** Classic stereo chorus: one modulated delay per side, LFOs 90 degrees apart, smooth (Hermite) interpolation. */
class Chorus final : public Base
{
public:
    explicit Chorus (const Info& i) : Base (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (param ("rate", "Rate", 0.05f, 5.0f, 0.6f, "Hz", 1.0f));
        l.add (param ("depth", "Depth", 0.0f, 100.0f, 40.0f, "%"));
        l.add (param ("delay", "Delay", 3.0f, 25.0f, 8.0f, "ms", 10.0f));
        l.add (param ("feedback", "Feedback", 0.0f, 70.0f, 0.0f, "%"));
        l.add (param ("width", "Width", 0.0f, 100.0f, 100.0f, "%"));
        l.add (param ("mix", "Mix", 0.0f, 100.0f, 50.0f, "%"));
        return l;
    }
    void prepareToPlay (double, int) override
    {
        const int len = (int) (sampleRate() * 0.06) + 8;
        for (auto& b : buf) b.assign ((size_t) len, 0.0f);
        write = 0;
        phase = 0.0;
        fbL = fbR = 0.0f;
        for (auto* v : { &delayMs, &depthMs }) v->reset (sampleRate(), 0.1);
        delayMs.setCurrentAndTargetValue (p ("delay"));
        depthMs.setCurrentAndTargetValue (depthFor (p ("delay"), p ("depth")));
    }
    void process (float* L, float* R, int n) noexcept override
    {
        if (buf[0].empty()) return;
        const double sr = sampleRate();
        const int len = (int) buf[0].size();
        const float fb = p ("feedback") * 0.01f, mix = p ("mix") * 0.01f, width = p ("width") * 0.01f;
        const double inc = juce::MathConstants<double>::twoPi * p ("rate") / sr;
        delayMs.setTargetValue (p ("delay"));
        depthMs.setTargetValue (depthFor (p ("delay"), p ("depth")));
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R != nullptr ? R[i] : L[i];
            const float base = delayMs.getNextValue(), depth = depthMs.getNextValue();
            const float modL = (float) std::sin (phase), modR = (float) std::sin (phase + juce::MathConstants<double>::halfPi * width * 2.0);
            phase += inc;
            if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            buf[0][(size_t) write] = inL + fbL * fb;
            buf[1][(size_t) write] = inR + fbR * fb;
            const float wl = read (buf[0], len, (base + depth * modL) * 0.001 * sr);
            const float wr = read (buf[1], len, (base + depth * modR) * 0.001 * sr);
            fbL = wl; fbR = wr;
            write = (write + 1) % len;
            L[i] = inL * (1.0f - 0.5f * mix) + wl * mix * 0.7f;
            if (R != nullptr) R[i] = inR * (1.0f - 0.5f * mix) + wr * mix * 0.7f;
            else L[i] = inL * (1.0f - 0.5f * mix) + 0.35f * (wl + wr) * mix;
        }
    }
private:
    std::array<std::vector<float>, 2> buf;
    int write = 0;
    double phase = 0.0;
    float fbL = 0.0f, fbR = 0.0f;
    juce::SmoothedValue<float> delayMs, depthMs;

    static float depthFor (float delay, float depthPercent) { return juce::jmin (delay - 1.0f, 6.0f) * depthPercent * 0.01f; }
    float read (const std::vector<float>& b, int len, double delaySamples) const noexcept
    {
        double rp = (double) write - delaySamples;
        while (rp < 0) rp += len;
        const int i1 = (int) rp;
        const float f = (float) (rp - i1);
        auto at = [&] (int k) { return b[(size_t) ((k % len + len) % len)]; };
        const float xm1 = at (i1 - 1), x0 = at (i1), x1 = at (i1 + 1), x2 = at (i1 + 2);
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

// =====================================================================================================  reverb

/** Feedback delay network: pre-delay, input diffusion, 8 damped, gently modulated lines mixed by a Householder
    matrix. The type sets the space: line lengths, diffusion, brightness, modulation, and for Spring a chirping
    dispersion chain. */
class Reverb final : public Base
{
public:
    static constexpr const char* typeNames[] = { "Room", "Hall", "Plate", "Chamber", "Spring", "Ambience", "Cathedral" };
    struct Type { float size, decay, diffusion, damping, mod, spread; bool spring; };
    static constexpr Type types[] = {
        { 0.55f, 0.8f, 0.60f, 0.10f, 1.0f, 1.0f, false },   // room
        { 1.25f, 1.2f, 0.70f, 0.05f, 6.0f, 1.0f, false },   // hall
        { 0.85f, 1.0f, 0.78f, -0.25f, 3.0f, 1.0f, false },  // plate: dense and bright
        { 0.80f, 1.0f, 0.65f, 0.15f, 2.0f, 0.8f, false },   // chamber
        { 0.40f, 1.0f, 0.50f, 0.20f, 0.5f, 0.6f, true },    // spring
        { 0.35f, 0.35f, 0.55f, 0.10f, 1.0f, 1.0f, false },  // ambience: short and close
        { 1.90f, 1.6f, 0.72f, 0.20f, 8.0f, 1.0f, false },   // cathedral
    };
    static constexpr int numTypes = (int) std::size (typeNames);

    explicit Reverb (const Info& i) : Base (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "type", 1 }, "Type",
                                                              juce::StringArray (typeNames, numTypes), 1));
        l.add (param ("size", "Size", 0.0f, 100.0f, 50.0f, "%"));
        l.add (param ("decay", "Decay", 0.2f, 12.0f, 2.2f, "s", 2.0f));
        l.add (param ("damping", "Damping", 0.0f, 100.0f, 40.0f, "%"));
        l.add (param ("predelay", "Pre-delay", 0.0f, 200.0f, 15.0f, "ms", 40.0f));
        l.add (param ("lowCut", "Low cut", 20.0f, 800.0f, 120.0f, "Hz", 150.0f));
        l.add (param ("width", "Width", 0.0f, 100.0f, 100.0f, "%"));
        l.add (param ("mix", "Mix", 0.0f, 100.0f, 25.0f, "%"));
        return l;
    }
    double getTailLengthSeconds() const override { return 18.0; }
    void prepareToPlay (double, int) override
    {
        const double sr = sampleRate();
        for (auto& l : lines) l.assign ((size_t) (sr * 0.45) + 16, 0.0f);
        for (size_t k = 0; k < diff.size(); ++k) diff[k].assign ((size_t) (sr * diffMs[k] * 0.001) + 2, 0.0f);
        pre.assign ((size_t) (sr * 0.21) + 2, 0.0f);
        std::fill (pos.begin(), pos.end(), 0);
        std::fill (dpos.begin(), dpos.end(), 0);
        std::fill (lp.begin(), lp.end(), 0.0f);
        for (auto& c : chirp) c = {};
        prePos = 0;
        lfo = 0.0;
        hp.reset();
        springLp.reset();
        lastType = -1;
    }
    void process (float* L, float* R, int n) noexcept override
    {
        if (lines[0].empty()) return;
        const double sr = sampleRate();
        const int typeIndex = juce::jlimit (0, numTypes - 1, juce::roundToInt (p ("type")));
        const auto& ty = types[typeIndex];
        if (typeIndex != lastType)   // a different space: start the tail fresh
        {
            for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
            std::fill (lp.begin(), lp.end(), 0.0f);
            lastType = typeIndex;
        }
        const float size = ty.size * (0.5f + 1.5f * p ("size") * 0.01f);
        const float decay = juce::jmax (0.1f, p ("decay") * ty.decay), mix = p ("mix") * 0.01f, width = p ("width") * 0.01f * ty.spread;
        const float damp = juce::jlimit (0.0f, 0.97f, 0.05f + 0.9f * p ("damping") * 0.01f + ty.damping);
        const float dg = ty.diffusion;
        const int preLen = (int) pre.size();
        const int preD = juce::jlimit (0, preLen - 1, (int) (p ("predelay") * 0.001 * sr));
        hp.set (Biquad::highPass, sr, ty.spring ? juce::jmax (300.0f, p ("lowCut")) : p ("lowCut"), 0.707);
        springLp.set (Biquad::lowPass, sr, 4200.0, 0.8);
        const double modDepth = ty.mod * sr / 48000.0;
        const double lfoInc = juce::MathConstants<double>::twoPi * 0.7 / sr;
        std::array<double, numLines> len {};
        std::array<float, numLines> g {};
        for (int k = 0; k < numLines; ++k)
        {
            const int maxLen = (int) lines[(size_t) k].size() - 4 - (int) modDepth;
            len[(size_t) k] = juce::jlimit (8.0, (double) maxLen, lineMs[(size_t) k] * size * 0.001 * sr);
            g[(size_t) k] = std::pow (10.0f, -3.0f * (float) len[(size_t) k] / (decay * (float) sr));
        }
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R != nullptr ? R[i] : L[i];
            pre[(size_t) prePos] = 0.5f * (inL + inR);
            int rp = prePos - preD;
            if (rp < 0) rp += preLen;
            float x = hp.process (pre[(size_t) rp]);
            prePos = (prePos + 1) % preLen;
            for (size_t k = 0; k < diff.size(); ++k)   // input diffusion
            {
                auto& d = diff[k];
                const float dv = d[(size_t) dpos[k]];
                const float y = -dg * x + dv;
                d[(size_t) dpos[k]] = x + dg * y;
                dpos[k] = (dpos[k] + 1) % (int) d.size();
                x = y;
            }
            lfo += lfoInc;
            if (lfo > juce::MathConstants<double>::twoPi) lfo -= juce::MathConstants<double>::twoPi;
            std::array<float, numLines> out {};
            float sum = 0.0f;
            for (int k = 0; k < numLines; ++k)
            {
                auto& line = lines[(size_t) k];
                const int size_ = (int) line.size();
                const double m = modDepth * std::sin (lfo + k * 0.785398);   // each line drifts a little: no metallic ring
                double r = pos[(size_t) k] - (len[(size_t) k] + m);
                while (r < 0) r += size_;
                const int r0 = (int) r;
                const float f = (float) (r - r0);
                const float v = line[(size_t) r0] + f * (line[(size_t) ((r0 + 1) % size_)] - line[(size_t) r0]);
                auto& st = lp[(size_t) k];
                st += (1.0f - damp) * (v - st);
                out[(size_t) k] = st;
                sum += st;
            }
            sum *= 2.0f / (float) numLines;
            float wl = 0.0f, wr = 0.0f;
            for (int k = 0; k < numLines; ++k)
            {
                auto& line = lines[(size_t) k];
                line[(size_t) pos[(size_t) k]] = (out[(size_t) k] - sum) * g[(size_t) k] + x * ((k & 1) ? -0.35f : 0.35f);
                pos[(size_t) k] = (pos[(size_t) k] + 1) % (int) line.size();
                if (k & 1) wr += out[(size_t) k]; else wl += out[(size_t) k];
            }
            wl *= 0.5f; wr *= 0.5f;
            if (ty.spring)   // dispersion: a chain of first-order all-passes makes the springy "drip"
            {
                for (auto& c : chirp)
                {
                    const float yl = -0.62f * wl + c.xl + 0.62f * c.yl;
                    c.xl = wl; c.yl = yl; wl = yl;
                    const float yr = -0.62f * wr + c.xr + 0.62f * c.yr;
                    c.xr = wr; c.yr = yr; wr = yr;
                }
                wl = springLp.process (wl);
                wr = 0.8f * wr + 0.2f * wl;
            }
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            wl = mid + side;
            wr = mid - side;
            L[i] = inL * (1.0f - mix) + wl * mix;
            if (R != nullptr) R[i] = inR * (1.0f - mix) + wr * mix;
            else L[i] = inL * (1.0f - mix) + mid * mix;
        }
        for (auto& st : lp) if (std::abs (st) < 1.0e-20f) st = 0.0f;
    }
private:
    static constexpr int numLines = 8;
    static constexpr std::array<float, numLines> lineMs { 29.7f, 37.1f, 41.1f, 43.7f, 53.0f, 59.9f, 67.7f, 73.1f };
    static constexpr std::array<float, 4> diffMs { 4.7f, 3.6f, 12.7f, 9.3f };
    std::array<std::vector<float>, numLines> lines;
    std::array<int, numLines> pos {};
    std::array<float, numLines> lp {};
    std::array<std::vector<float>, 4> diff;
    std::array<int, 4> dpos {};
    std::vector<float> pre;
    int prePos = 0, lastType = -1;
    double lfo = 0.0;
    struct ChirpStage { float xl = 0, yl = 0, xr = 0, yr = 0; };
    std::array<ChirpStage, 16> chirp {};
    Biquad hp, springLp;
};

// =====================================================================================================  console EQ

/** A four-band console EQ in the style of the classic E-series channel strip: high / high-mid / low-mid / low bands
    (the outer bands switch between shelf and bell), an 18 dB/oct high-pass and a 12 dB/oct low-pass. The curves are
    standard biquads for now; they can be voiced further later. */
class SslEq final : public Base
{
public:
    explicit SslEq (const Info& i) : Base (i, layout()) {}
    static Layout layout()
    {
        Layout l;
        auto sw = [&l] (const char* id, const char* name, bool def) {
            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def));
        };
        l.add (param ("hfGain", "HF Gain", -16.0f, 16.0f, 0.0f, "dB"));
        l.add (param ("hfFreq", "HF Freq", 1500.0f, 16000.0f, 8000.0f, "Hz", 5000.0f));
        sw ("hfBell", "HF Bell", false);
        l.add (param ("hmfGain", "HMF Gain", -16.0f, 16.0f, 0.0f, "dB"));
        l.add (param ("hmfFreq", "HMF Freq", 600.0f, 7000.0f, 3000.0f, "Hz", 2500.0f));
        l.add (param ("hmfQ", "HMF Q", 0.5f, 2.5f, 1.0f, ""));
        l.add (param ("lmfGain", "LMF Gain", -16.0f, 16.0f, 0.0f, "dB"));
        l.add (param ("lmfFreq", "LMF Freq", 200.0f, 2500.0f, 800.0f, "Hz", 700.0f));
        l.add (param ("lmfQ", "LMF Q", 0.5f, 2.5f, 1.0f, ""));
        l.add (param ("lfGain", "LF Gain", -16.0f, 16.0f, 0.0f, "dB"));
        l.add (param ("lfFreq", "LF Freq", 30.0f, 450.0f, 100.0f, "Hz", 120.0f));
        sw ("lfBell", "LF Bell", false);
        l.add (param ("hpf", "HPF Freq", 16.0f, 350.0f, 60.0f, "Hz", 60.0f));
        l.add (param ("lpf", "LPF Freq", 3000.0f, 22000.0f, 12000.0f, "Hz", 9000.0f));
        sw ("hpfOn", "HPF In", false);
        sw ("lpfOn", "LPF In", false);
        sw ("eqIn", "EQ In", true);
        return l;
    }
    void prepareToPlay (double, int) override { for (auto& st : stage) { st.l.reset(); st.r.reset(); st.on = false; } }
    void process (float* L, float* R, int n) noexcept override
    {
        if (p ("eqIn") < 0.5f) return;
        const double sr = sampleRate();
        const bool bellLf = p ("lfBell") > 0.5f, bellHf = p ("hfBell") > 0.5f;
        auto tune = [&] (Stage& st, bool on, auto&& setter) {
            if (! on) { if (st.on) { st.l.reset(); st.r.reset(); } st.on = false; return; }
            setter (st.l);
            st.r.b0 = st.l.b0; st.r.b1 = st.l.b1; st.r.b2 = st.l.b2; st.r.a1 = st.l.a1; st.r.a2 = st.l.a2;
            st.on = true;
        };
        const double hpf = p ("hpf"), lpf = p ("lpf");
        tune (stage[0], p ("hpfOn") > 0.5f, [&] (Biquad& b) { b.setFirstOrder (true, sr, hpf); });
        tune (stage[1], p ("hpfOn") > 0.5f, [&] (Biquad& b) { b.set (Biquad::highPass, sr, hpf, 1.0); });
        tune (stage[2], p ("lpfOn") > 0.5f, [&] (Biquad& b) { b.set (Biquad::lowPass, sr, lpf, 0.7071); });
        const double lfG = p ("lfGain"), lmfG = p ("lmfGain"), hmfG = p ("hmfGain"), hfG = p ("hfGain");
        tune (stage[3], std::abs (lfG) > 0.05, [&] (Biquad& b) { b.set (bellLf ? Biquad::peak : Biquad::lowShelf, sr, p ("lfFreq"), bellLf ? 0.9 : 0.7071, lfG); });
        tune (stage[4], std::abs (lmfG) > 0.05, [&] (Biquad& b) { b.set (Biquad::peak, sr, p ("lmfFreq"), p ("lmfQ"), lmfG); });
        tune (stage[5], std::abs (hmfG) > 0.05, [&] (Biquad& b) { b.set (Biquad::peak, sr, p ("hmfFreq"), p ("hmfQ"), hmfG); });
        tune (stage[6], std::abs (hfG) > 0.05, [&] (Biquad& b) { b.set (bellHf ? Biquad::peak : Biquad::highShelf, sr, p ("hfFreq"), bellHf ? 0.9 : 0.7071, hfG); });
        for (auto& st : stage)
        {
            if (! st.on) continue;
            st.l.process (L, n);
            if (R != nullptr) st.r.process (R, n);
        }
    }
private:
    struct Stage { Biquad l, r; bool on = false; };
    std::array<Stage, 7> stage {};
};

// =====================================================================================================  rack panel

/** Gain-reduction meter, vertical (limiter) or a slim horizontal bar (compressor): no box around it. */
class GrBar final : public juce::Component, private juce::Timer
{
public:
    GrBar (std::atomic<float>& g, float maxDbIn, bool horizontalBar) : gr (g), maxDb (maxDbIn), horizontal (horizontalBar) { startTimerHz (30); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        if (horizontal)
        {
            g.setColour (col::dim);
            g.drawText ("GR", r.removeFromLeft (20.0f), juce::Justification::centredLeft);
            g.drawText (juce::String (shown, 1) + " dB", r.removeFromRight (46.0f), juce::Justification::centredRight);
            auto bar = r.withSizeKeepingCentre (r.getWidth(), 9.0f).reduced (2.0f, 0.0f);
            g.setColour (juce::Colour (0xff060606));
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (col::bad.withAlpha (0.9f));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, shown / maxDb)), 2.0f);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawRoundedRectangle (bar, 2.0f, 1.0f);
            return;
        }
        g.setColour (col::dim);
        g.drawText ("GR", r.removeFromTop (14.0f), juce::Justification::centred);
        auto valueRow = r.removeFromBottom (14.0f);
        g.drawText (juce::String (shown, 1), valueRow, juce::Justification::centred);
        auto track = r.reduced (juce::jmax (2.0f, (r.getWidth() - 10.0f) * 0.5f), 2.0f);
        g.setColour (juce::Colour (0xff060606));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (col::bad.withAlpha (0.9f));
        g.fillRoundedRectangle (track.withHeight (track.getHeight() * juce::jlimit (0.0f, 1.0f, shown / maxDb)), 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawRoundedRectangle (track, 2.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions (8.0f)));
        for (int db = 0; db <= (int) maxDb; db += 3)   // scale
        {
            const float y = track.getY() + track.getHeight() * (float) db / maxDb;
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawHorizontalLine ((int) y, track.getRight() + 1.0f, track.getRight() + 4.0f);
        }
    }
private:
    std::atomic<float>& gr;
    float maxDb;
    bool horizontal;
    float shown = 0.0f;
    void timerCallback() override
    {
        const float v = gr.exchange (0.0f);
        shown = v > shown ? v : juce::jmax (v, shown - 0.5f);
        repaint();
    }
};

/** Rack-mount look for the knobs: a machined knob with a pointer and a scale of ticks around it. A slider with a "cap"
    property gets a coloured knob top (the console EQ's colour-coded bands). */
struct RackLnF final : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s) override
    {
        auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
        const float d = juce::jmin (b.getWidth(), b.getHeight()), r = d * 0.5f;
        const auto c = b.getCentre();
        auto pt = [&] (float ang, float rad) { return juce::Point<float> (c.x + rad * std::sin (ang), c.y - rad * std::cos (ang)); };
        const auto ink = s.findColour (juce::Slider::rotarySliderFillColourId);
        const bool capped = s.getProperties().contains ("cap");
        const auto cap = capped ? juce::Colour ((juce::uint32) (int) s.getProperties()["cap"]) : juce::Colour (0xff3a3a3a);
        for (int i = 0; i <= 10; ++i)   // scale
        {
            const float a = start + (float) i / 10.0f * (end - start);
            g.setColour (juce::Colours::white.withAlpha (i % 5 == 0 ? 0.55f : 0.28f));
            g.drawLine (juce::Line<float> (pt (a, r * 0.84f), pt (a, r * (i % 5 == 0 ? 1.0f : 0.94f))), i % 5 == 0 ? 1.4f : 1.0f);
        }
        const float kr = r * 0.68f;
        g.setColour (juce::Colours::black.withAlpha (0.55f));   // drop shadow
        g.fillEllipse (c.x - kr, c.y - kr + 2.0f, kr * 2.0f, kr * 2.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a6a6a), c.x - kr * 0.5f, c.y - kr * 0.7f,
                                                 juce::Colour (0xff1c1c1c), c.x + kr * 0.6f, c.y + kr, true));
        g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
        const float tr = kr * 0.82f;   // the knob's top
        g.setGradientFill (juce::ColourGradient (cap.brighter (capped ? 0.25f : 0.0f), c.x, c.y - tr, cap.darker (capped ? 0.55f : 0.7f), c.x, c.y + tr, false));
        g.fillEllipse (c.x - tr, c.y - tr, tr * 2.0f, tr * 2.0f);
        g.setColour (juce::Colours::black);
        g.drawEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f, 1.0f);
        const float ang = start + pos * (end - start);
        g.setColour (capped ? juce::Colours::white : ink);
        g.drawLine (juce::Line<float> (pt (ang, tr * 0.25f), pt (ang, tr * 0.95f)), 2.4f);
        if (! capped)
        {
            juce::Path arc;   // value ring
            arc.addCentredArc (c.x, c.y, r * 0.8f, r * 0.8f, 0.0f, start, ang, true);
            g.setColour (ink.withAlpha (0.8f));
            g.strokePath (arc, juce::PathStrokeType (1.6f));
        }
    }
    juce::Font getLabelFont (juce::Label&) override { return juce::Font (juce::FontOptions (10.0f, juce::Font::bold)); }
};

/** The rack unit's faceplate: brushed metal, bevel and the engraved name. */
void paintRackFace (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour face, const juce::String& name, juce::Rectangle<int> titleArea)
{
    const auto r = bounds.toFloat();
    g.setGradientFill (juce::ColourGradient (face.brighter (0.12f), 0.0f, 0.0f, face.darker (0.35f), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.035f));   // brushed metal
    for (float y = 1.0f; y < r.getBottom(); y += 3.0f) g.drawHorizontalLine ((int) y, 0.0f, r.getRight());
    g.setColour (juce::Colours::white.withAlpha (0.22f));   // bevel
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.drawLine (1.0f, r.getBottom() - 0.5f, r.getRight() - 1.0f, r.getBottom() - 0.5f, 1.0f);
    const auto t = titleArea.toFloat();   // engraved name
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.drawText (name.toUpperCase(), t.translated (0.0f, -1.0f), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xffe6e6e6));
    g.drawText (name.toUpperCase(), t, juce::Justification::centredLeft);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawHorizontalLine (titleArea.getBottom() + 1, t.getX(), t.getRight());
}

/** The compact version of a built-in's editor that lives inside a mixer rack slot, styled as a rack unit: faceplate,
    engraved name, preset menu and A/B, and the plugin's knobs. It fills whatever size the rack gives it. Edits are
    reported (state before / after) so the host can undo them. */
class RackPanel final : public juce::Component, private juce::AudioProcessorParameter::Listener
{
public:
    using EditFn = std::function<void (const juce::MemoryBlock&, const juce::MemoryBlock&)>;

    RackPanel (Base& b, EditFn onEditFn) : proc (b), onEdit (std::move (onEditFn))
    {
        const auto id = juce::String (b.info.id);
        face = id == "fretforge:ssleq" ? juce::Colour (0xff2b2d30) : id == "fretforge:comp" ? juce::Colour (0xff2a211f)
             : id == "fretforge:limiter" ? juce::Colour (0xff2a1c22) : id == "fretforge:reverb" ? juce::Colour (0xff1b2926)
             : id == "fretforge:delay" ? juce::Colour (0xff232233) : id == "fretforge:chorus" ? juce::Colour (0xff29281c) : juce::Colour (0xff202020);
        ssl = dynamic_cast<SslEq*> (&b) != nullptr;
        if (auto* lim = dynamic_cast<Limiter*> (&b))   // knobs down the left, the meter up the right
        {
            sideMeter = std::make_unique<GrBar> (lim->grDb, 12.0f, false);
            addAndMakeVisible (*sideMeter);
        }
        else if (auto* comp = dynamic_cast<Compressor*> (&b))
        {
            topMeter = std::make_unique<GrBar> (comp->grDb, 12.0f, true);
            addAndMakeVisible (*topMeter);
        }
        const auto& presets = factoryPresets (id);
        for (size_t i = 0; i < presets.size(); ++i)
            presetBox.addItem ((presets[i].category.isNotEmpty() ? presets[i].category + ": " : juce::String()) + presets[i].name, (int) i + 1);
        presetBox.setTextWhenNothingSelected ("Preset");
        presetBox.onChange = [this] {
            const auto& ps = factoryPresets (juce::String (proc.info.id));
            const int i = presetBox.getSelectedId() - 1;
            if (i < 0 || i >= (int) ps.size()) return;
            juce::MemoryBlock before, after;
            proc.getStateInformation (before);
            applyPreset (proc, ps[(size_t) i]);
            proc.getStateInformation (after);
            report (before, after);
        };
        if (presets.empty()) presetBox.setVisible (false); else addAndMakeVisible (presetBox);
        for (auto* t : { &abA, &abB })
        {
            t->setClickingTogglesState (false);
            t->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd8d8d8));
            t->setColour (juce::TextButton::textColourOnId, col::bg);
            addAndMakeVisible (*t);
        }
        abA.setToggleState (true, juce::dontSendNotification);
        abA.onClick = [this] { switchTo (0); };
        abB.onClick = [this] { switchTo (1); };
        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (5);
        addAndMakeVisible (viewport);
        buildKnobs();
        for (auto* p : proc.getParameters()) p->addListener (this);
    }

    ~RackPanel() override
    {
        for (auto* p : proc.getParameters()) p->removeListener (this);
        knobs.clear();
    }

    void paint (juce::Graphics& g) override { paintRackFace (g, getLocalBounds(), face, proc.info.name, titleArea); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (7, 6);
        auto head = r.removeFromTop (20);
        titleArea = head;
        abB.setBounds (head.removeFromRight (22).reduced (0, 1));
        head.removeFromRight (2);
        abA.setBounds (head.removeFromRight (22).reduced (0, 1));
        titleArea.removeFromRight (50);
        r.removeFromTop (4);
        if (presetBox.isVisible()) { presetBox.setBounds (r.removeFromTop (22)); r.removeFromTop (5); }
        int cols = juce::jlimit (2, 6, r.getWidth() / 46);
        if (sideMeter != nullptr)
        {
            sideMeter->setBounds (r.removeFromRight (30));
            r.removeFromRight (4);
            cols = 1;
        }
        if (topMeter != nullptr)
        {
            topMeter->setBounds (r.removeFromTop (18));
            r.removeFromTop (4);
        }
        const int n = (int) knobs.size();
        const int rows = juce::jmax (1, (n + cols - 1) / cols);
        const int knobMin = 56, toggleH = 26;
        // a row of only switches is short; the rest share the height
        std::vector<bool> shortRow ((size_t) rows, true);
        for (int i = 0; i < n; ++i)
            if (! knobs[(size_t) i]->toggle.isVisible()) shortRow[(size_t) (i / cols)] = false;
        int tall = 0, shorts = 0;
        for (bool s : shortRow) (s ? shorts : tall)++;
        const int cellH = tall > 0 ? juce::jlimit (knobMin, 150, (r.getHeight() - shorts * toggleH) / tall) : toggleH;
        std::vector<int> rowY ((size_t) rows + 1, 0);
        for (int i = 0; i < rows; ++i) rowY[(size_t) i + 1] = rowY[(size_t) i] + (shortRow[(size_t) i] ? toggleH : cellH);
        viewport.setBounds (r);
        const bool scrolls = rowY.back() > r.getHeight();
        const int w = r.getWidth() - (scrolls ? viewport.getScrollBarThickness() + 1 : 0);
        grid.setSize (w, juce::jmax (r.getHeight(), rowY.back()));
        for (int i = 0; i < n; ++i)
        {
            const int row = i / cols;
            auto cell = juce::Rectangle<int> ((i % cols) * (w / cols), rowY[(size_t) row], w / cols, rowY[(size_t) row + 1] - rowY[(size_t) row]).reduced (1, 1);
            auto& k = *knobs[(size_t) i];
            if (k.toggle.isVisible()) { k.toggle.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 22)); continue; }
            k.label.setBounds (cell.removeFromTop (13));
            if (k.combo.isVisible()) { k.combo.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 22)); continue; }
            k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmax (40, cell.getWidth() - 4), 14);
            k.slider.setBounds (cell);
        }
    }

private:
    struct Knob
    {
        ScrollSafeSlider slider;
        juce::Label label;
        juce::TextButton toggle;
        juce::ComboBox combo;
        std::unique_ptr<juce::ComboBoxParameterAttachment> comboAttach;
        std::unique_ptr<juce::SliderParameterAttachment> attach;
        std::unique_ptr<juce::ButtonParameterAttachment> buttonAttach;
    };

    void addKnob (juce::RangedAudioParameter* rp, juce::Colour cap = juce::Colour())
    {
        if (rp == nullptr) return;
        auto k = std::make_unique<Knob>();
        k->label.setText (rp->getName (14).toUpperCase(), juce::dontSendNotification);
        k->label.setJustificationType (juce::Justification::centred);
        k->label.setColour (juce::Label::textColourId, juce::Colour (0xffc8c8c8));
        k->label.setLookAndFeel (&lnf);
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (rp))
        {
            k->combo.addItemList (cp->choices, 1);
            k->comboAttach = std::make_unique<juce::ComboBoxParameterAttachment> (*cp, k->combo);
            grid.addAndMakeVisible (k->combo);
            grid.addAndMakeVisible (k->label);
        }
        else if (auto* bp = dynamic_cast<juce::AudioParameterBool*> (rp))
        {
            k->toggle.setButtonText (rp->getName (14));
            k->toggle.setClickingTogglesState (true);
            k->toggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd8d8d8));
            k->toggle.setColour (juce::TextButton::textColourOnId, col::bg);
            k->buttonAttach = std::make_unique<juce::ButtonParameterAttachment> (*bp, k->toggle);
            grid.addAndMakeVisible (k->toggle);
        }
        else
        {
            k->slider.setLookAndFeel (&lnf);
            k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k->slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
            k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 14);
            k->slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffffb347));
            k->slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffffc46b));
            k->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff060606));
            k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a3a3a));
            if (! cap.isTransparent()) k->slider.getProperties().set ("cap", (int) cap.getARGB());
            k->attach = std::make_unique<juce::SliderParameterAttachment> (*rp, k->slider);
            k->slider.setDoubleClickReturnValue (true, rp->convertFrom0to1 (rp->getDefaultValue()));
            grid.addAndMakeVisible (k->slider);
            grid.addAndMakeVisible (k->label);
        }
        knobs.push_back (std::move (k));
    }

    void buildKnobs()
    {
        knobs.clear();
        grid.removeAllChildren();
        if (ssl)   // the strip's order: high, high-mid, low-mid, low, filters; each band in its own colour
        {
            struct Item { const char* id; juce::uint32 colour; };
            const Item items[] = { { "hfGain", 0xffd9423a }, { "hfFreq", 0xffd9423a }, { "hfBell", 0 },
                                   { "hmfGain", 0xff3fa66b }, { "hmfFreq", 0xff3fa66b }, { "hmfQ", 0xff3fa66b },
                                   { "lmfGain", 0xff3b82c4 }, { "lmfFreq", 0xff3b82c4 }, { "lmfQ", 0xff3b82c4 },
                                   { "lfGain", 0xff9a6a42 }, { "lfFreq", 0xff9a6a42 }, { "lfBell", 0 },
                                   { "hpf", 0xff8a8a8a }, { "lpf", 0xff8a8a8a }, { "eqIn", 0 },
                                   { "hpfOn", 0 }, { "lpfOn", 0 } };
            for (auto& it : items)
                addKnob (dynamic_cast<juce::RangedAudioParameter*> (proc.state.getParameter (it.id)), juce::Colour (it.colour));
        }
        else
            for (auto* p : proc.getParameters())
                addKnob (dynamic_cast<juce::RangedAudioParameter*> (p));
    }

    void report (const juce::MemoryBlock& before, const juce::MemoryBlock& after)
    {
        if (onEdit && before != after) onEdit (before, after);
    }

    void switchTo (int which)
    {
        if (which == current) return;
        juce::MemoryBlock before, after;
        proc.getStateInformation (before);
        ab[(size_t) current] = before;
        if (ab[(size_t) which].getSize() == 0) ab[(size_t) which] = before;   // first visit: B starts as a copy of A
        proc.setStateInformation (ab[(size_t) which].getData(), (int) ab[(size_t) which].getSize());
        proc.getStateInformation (after);
        current = which;
        abA.setToggleState (which == 0, juce::dontSendNotification);
        abB.setToggleState (which == 1, juce::dontSendNotification);
        report (before, after);
    }

    // every knob drag is a parameter gesture: the state at its start and end is one undo step
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override
    {
        if (starting)
        {
            if (gestureDepth++ == 0) proc.getStateInformation (gestureBefore);
        }
        else if (gestureDepth > 0 && --gestureDepth == 0)
        {
            juce::MemoryBlock after;
            proc.getStateInformation (after);
            report (gestureBefore, after);
        }
    }

    RackLnF lnf;   // outlives the knobs
    Base& proc;
    EditFn onEdit;
    bool ssl = false;
    juce::Colour face;
    juce::Rectangle<int> titleArea;
    juce::ComboBox presetBox;
    juce::TextButton abA { "A" }, abB { "B" };
    std::unique_ptr<GrBar> sideMeter, topMeter;
    juce::Viewport viewport;
    juce::Component grid;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::array<juce::MemoryBlock, 2> ab;
    juce::MemoryBlock gestureBefore;
    int current = 0, gestureDepth = 0;
};

/** The parametric EQ as a rack unit: a slim curve (drag the dots), eight band chips and one set of knobs for the
    selected band, so it is as narrow as the other modules. */
class EqRackPanel final : public juce::Component, private juce::AudioProcessorParameter::Listener, private juce::Timer
{
public:
    using EditFn = std::function<void (const juce::MemoryBlock&, const juce::MemoryBlock&)>;

    EqRackPanel (Eq& e, EditFn onEditFn) : eq (e), onEdit (std::move (onEditFn)), body (*this)
    {
        const auto& presets = factoryPresets ("fretforge:eq");
        for (size_t i = 0; i < presets.size(); ++i)
            presetBox.addItem ((presets[i].category.isNotEmpty() ? presets[i].category + ": " : juce::String()) + presets[i].name, (int) i + 1);
        presetBox.setTextWhenNothingSelected ("Preset");
        presetBox.onChange = [this] {
            const auto& ps = factoryPresets ("fretforge:eq");
            const int i = presetBox.getSelectedId() - 1;
            if (i < 0 || i >= (int) ps.size()) return;
            juce::MemoryBlock before, after;
            eq.getStateInformation (before);
            applyPreset (eq, ps[(size_t) i]);
            eq.getStateInformation (after);
            report (before, after);
        };
        addAndMakeVisible (presetBox);
        for (auto* t : { &abA, &abB })
        {
            t->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd8d8d8));
            t->setColour (juce::TextButton::textColourOnId, col::bg);
            addAndMakeVisible (*t);
        }
        abA.setToggleState (true, juce::dontSendNotification);
        abA.onClick = [this] { switchTo (0); };
        abB.onClick = [this] { switchTo (1); };

        graph = std::make_unique<EqGraph> (eq);
        graph->compact = true;
        graph->onSelect = [this] (int b) { if (b >= 0) selectBand (b); else repaintBody(); };
        addAndMakeVisible (*graph);
        addAndMakeVisible (chips);
        chips.owner = this;

        viewport.setViewedComponent (&body, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (5);
        addAndMakeVisible (viewport);
        selected = 0;
        body.build();
        graph->select (0);
        for (auto* p : eq.getParameters()) p->addListener (this);
        startTimerHz (10);
    }
    ~EqRackPanel() override
    {
        stopTimer();
        for (auto* p : eq.getParameters()) p->removeListener (this);
        body.releaseAttachments();
    }

    void paint (juce::Graphics& g) override { paintRackFace (g, getLocalBounds(), juce::Colour (0xff1c2530), "Parametric", titleArea); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (7, 6);
        auto head = r.removeFromTop (20);
        titleArea = head;
        abB.setBounds (head.removeFromRight (22).reduced (0, 1));
        head.removeFromRight (2);
        abA.setBounds (head.removeFromRight (22).reduced (0, 1));
        titleArea.removeFromRight (50);
        r.removeFromTop (4);
        presetBox.setBounds (r.removeFromTop (22));
        r.removeFromTop (5);
        graph->setBounds (r.removeFromTop (juce::jlimit (48, 84, r.getHeight() / 7)));
        r.removeFromTop (5);
        chips.setBounds (r.removeFromTop (22));
        r.removeFromTop (5);
        viewport.setBounds (r);
        const bool scrolls = body.minHeight() > r.getHeight();
        const int w = r.getWidth() - (scrolls ? viewport.getScrollBarThickness() + 1 : 0);
        body.setSize (w, juce::jmax (r.getHeight(), body.minHeight()));
    }

    void selectBand (int b)
    {
        if (b == selected && body.built) { repaintBody(); return; }
        selected = juce::jlimit (0, eqBandCount - 1, b);
        body.build();
        chips.repaint();
        if (graph->selected != selected) graph->select (selected);
        body.resized();
    }

private:
    friend struct Body;
    void repaintBody() { chips.repaint(); body.repaint(); }

    struct Chips final : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            const auto s = owner->eq.settings();
            const float w = (float) getWidth() / (float) eqBandCount;
            for (int b = 0; b < eqBandCount; ++b)
            {
                auto r = juce::Rectangle<float> ((float) b * w + 1.0f, 1.0f, w - 2.0f, (float) getHeight() - 2.0f);
                const auto& band = s.b[(size_t) b];
                const bool on = band.isOn();
                g.setColour (on ? eqBandColours[b] : (band.isUsed() ? eqBandColours[b].withAlpha (0.25f) : juce::Colour (0xff1b1b1d)));
                g.fillRoundedRectangle (r, 3.0f);
                if (b == owner->selected) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.5f); }
                g.setColour (on ? juce::Colours::black : juce::Colour (0xff7a7a7a));
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (juce::String (b + 1), r, juce::Justification::centred);
            }
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int b = juce::jlimit (0, eqBandCount - 1, e.x * eqBandCount / juce::jmax (1, getWidth()));
            if (! owner->eq.settings().b[(size_t) b].isUsed()) owner->addBand (b);
            owner->selectBand (b);
        }
        EqRackPanel* owner = nullptr;
    } chips;

    /** The selected band's controls. */
    struct Body final : public juce::Component
    {
        explicit Body (EqRackPanel& o) : owner (o) {}
        ~Body() override { releaseAttachments(); }
        bool built = false;

        struct K { ScrollSafeSlider s; juce::Label l; std::unique_ptr<juce::SliderParameterAttachment> a; };
        std::array<K, 7> k;   // freq, gain, q, thresh, range, speed, output
        juce::ComboBox typeBox, slopeBox, dynBox;
        juce::TextButton onBtn { "ON" }, listenBtn { "LISTEN" }, delBtn { "DELETE" };
        std::unique_ptr<juce::ComboBoxParameterAttachment> typeA, slopeA, dynA;
        std::unique_ptr<juce::ButtonParameterAttachment> onA;
        static constexpr int rowMin = 56;

        int minHeight() const { return 20 + 4 + 22 + 6 + rowMin + 4 + 22 + 6 + rowMin + 4 + rowMin + 4; }
        void releaseAttachments() { for (auto& x : k) x.a.reset(); typeA.reset(); slopeA.reset(); dynA.reset(); onA.reset(); }

        void build()
        {
            auto& eq = owner.eq;
            const int b = owner.selected;
            releaseAttachments();
            const auto c = eqBandColours[b];
            if (! built)
            {
                static const char* names[] = { "FREQ", "GAIN", "Q", "THRESH", "RANGE", "SPEED", "OUTPUT" };
                for (size_t i = 0; i < k.size(); ++i)
                {
                    auto& x = k[i];
                    x.s.setLookAndFeel (&owner.lnf);
                    x.l.setLookAndFeel (&owner.lnf);
                    x.s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                    x.s.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
                    x.s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 14);
                    x.s.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffffb347));
                    x.s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffffc46b));
                    x.s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff060606));
                    x.s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a3a3a));
                    x.l.setText (names[i], juce::dontSendNotification);
                    x.l.setJustificationType (juce::Justification::centred);
                    x.l.setColour (juce::Label::textColourId, juce::Colour (0xffc8c8c8));
                    addAndMakeVisible (x.s);
                    addAndMakeVisible (x.l);
                }
                for (auto* cb : { &typeBox, &slopeBox, &dynBox }) addAndMakeVisible (*cb);
                typeBox.addItemList (eqTypeNames, 1);
                slopeBox.addItemList (eqSlopeNames, 1);
                dynBox.addItemList (eqDynNames, 1);
                onBtn.setClickingTogglesState (true);
                onBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
                addAndMakeVisible (onBtn);
                listenBtn.setClickingTogglesState (true);
                listenBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd8d8d8));
                listenBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
                listenBtn.onClick = [this] { owner.eq.listenBand = listenBtn.getToggleState() ? owner.selected : -1; };
                addAndMakeVisible (listenBtn);
                delBtn.onClick = [this] { owner.removeBand (owner.selected); };
                addAndMakeVisible (delBtn);
                built = true;
            }
            onBtn.setColour (juce::TextButton::buttonOnColourId, c);
            auto bind = [&] (size_t i, juce::RangedAudioParameter* p) {
                k[i].s.getProperties().set ("cap", (int) c.getARGB());
                k[i].a = std::make_unique<juce::SliderParameterAttachment> (*p, k[i].s);
                k[i].s.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
            };
            bind (0, eq.bandParam (b, 2)); bind (1, eq.bandParam (b, 3)); bind (2, eq.bandParam (b, 4));
            bind (3, eq.bandParam (b, 8)); bind (4, eq.bandParam (b, 7)); bind (5, eq.bandParam (b, 10));
            k[6].s.getProperties().remove ("cap");
            k[6].a = std::make_unique<juce::SliderParameterAttachment> (*dynamic_cast<juce::RangedAudioParameter*> (eq.state.getParameter ("out")), k[6].s);
            typeA = std::make_unique<juce::ComboBoxParameterAttachment> (*dynamic_cast<juce::AudioParameterChoice*> (eq.bandParam (b, 1)), typeBox);
            slopeA = std::make_unique<juce::ComboBoxParameterAttachment> (*dynamic_cast<juce::AudioParameterChoice*> (eq.bandParam (b, 5)), slopeBox);
            dynA = std::make_unique<juce::ComboBoxParameterAttachment> (*dynamic_cast<juce::AudioParameterChoice*> (eq.bandParam (b, 6)), dynBox);
            onA = std::make_unique<juce::ButtonParameterAttachment> (*dynamic_cast<juce::AudioParameterBool*> (eq.bandParam (b, 0)), onBtn);
            listenBtn.setToggleState (eq.listenBand == b, juce::dontSendNotification);
            repaint();
        }

        void refreshEnabled()   // dim what the selected band does not use
        {
            const auto s = owner.eq.settings();
            const auto& band = s.b[(size_t) owner.selected];
            const bool gain = eqHasGain (band.typeI()), cut = eqIsCut (band.typeI());
            const bool dyn = band.dynI() != eqDynOff;
            k[1].s.setAlpha (gain ? 1.0f : 0.35f); k[1].l.setAlpha (gain ? 1.0f : 0.35f);
            k[2].s.setAlpha (cut ? 0.35f : 1.0f); k[2].l.setAlpha (cut ? 0.35f : 1.0f);
            for (size_t i : { (size_t) 3, (size_t) 4, (size_t) 5 }) { k[i].s.setAlpha (dyn ? 1.0f : 0.35f); k[i].l.setAlpha (dyn ? 1.0f : 0.35f); }
            slopeBox.setAlpha (cut ? 1.0f : 0.35f);
        }

        void paint (juce::Graphics& g) override
        {
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.setColour (eqBandColours[owner.selected]);
            g.drawText ("BAND " + juce::String (owner.selected + 1), 0, 0, getWidth() - 40, 20, juce::Justification::centredLeft);
        }

        void resized() override
        {
            if (! built) return;
            auto r = getLocalBounds();
            auto head = r.removeFromTop (20);
            onBtn.setBounds (head.removeFromRight (34).reduced (0, 1));
            r.removeFromTop (4);
            typeBox.setBounds (r.removeFromTop (22));
            r.removeFromTop (6);
            const int knobH = juce::jlimit (rowMin, 110, (r.getHeight() - 22 - 6 - 12) / 3);
            auto row = [&] (std::initializer_list<size_t> ks) {
                auto cell = r.removeFromTop (knobH);
                const int w = cell.getWidth() / 3;
                int x = cell.getX();
                for (size_t i : ks)
                {
                    auto c = juce::Rectangle<int> (x, cell.getY(), w, cell.getHeight()).reduced (1, 0);
                    k[i].l.setBounds (c.removeFromTop (13));
                    k[i].s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmax (40, c.getWidth() - 4), 14);
                    k[i].s.setBounds (c);
                    x += w;
                }
            };
            row ({ 0, 1, 2 });
            r.removeFromTop (4);
            auto combos = r.removeFromTop (22);
            slopeBox.setBounds (combos.removeFromLeft (combos.getWidth() / 2 - 2));
            combos.removeFromLeft (4);
            dynBox.setBounds (combos);
            r.removeFromTop (6);
            row ({ 3, 4, 5 });
            r.removeFromTop (4);
            auto foot = r.removeFromTop (knobH);
            {
                auto c = foot.removeFromLeft (foot.getWidth() / 3).reduced (1, 0);
                k[6].l.setBounds (c.removeFromTop (13));
                k[6].s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmax (40, c.getWidth() - 4), 14);
                k[6].s.setBounds (c);
            }
            foot.reduce (4, juce::jmax (0, (foot.getHeight() - 52) / 2));
            listenBtn.setBounds (foot.removeFromTop (24));
            foot.removeFromTop (4);
            delBtn.setBounds (foot.removeFromTop (24));
        }
        EqRackPanel& owner;
    };

    void addBand (int b)   // a chip for a band that does not exist yet: a bell at 1 kHz
    {
        juce::MemoryBlock before, after;
        eq.getStateInformation (before);
        auto set = [this, b] (int k, float v) { auto* p = eq.bandParam (b, k); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        set (1, (float) eqBell); set (2, 1000.0f); set (3, 0.0f); set (4, 1.0f); set (6, 0.0f); set (0, 1.0f); set (9, 1.0f);
        eq.getStateInformation (after);
        report (before, after);
    }
    void removeBand (int b)
    {
        juce::MemoryBlock before, after;
        eq.getStateInformation (before);
        auto* p = eq.bandParam (b, 9);
        p->setValueNotifyingHost (0.0f);
        eq.getStateInformation (after);
        report (before, after);
        chips.repaint();
    }
    void timerCallback() override
    {
        if (graph->selected >= 0 && graph->selected != selected) selectBand (graph->selected);
        body.refreshEnabled();
        chips.repaint();
        if (eq.listenBand != (body.listenBtn.getToggleState() ? selected : -1)) body.listenBtn.setToggleState (eq.listenBand == selected, juce::dontSendNotification);
    }
    void report (const juce::MemoryBlock& before, const juce::MemoryBlock& after) { if (onEdit && before != after) onEdit (before, after); }
    void switchTo (int which)
    {
        if (which == current) return;
        juce::MemoryBlock before, after;
        eq.getStateInformation (before);
        ab[(size_t) current] = before;
        if (ab[(size_t) which].getSize() == 0) ab[(size_t) which] = before;
        eq.setStateInformation (ab[(size_t) which].getData(), (int) ab[(size_t) which].getSize());
        eq.getStateInformation (after);
        current = which;
        abA.setToggleState (which == 0, juce::dontSendNotification);
        abB.setToggleState (which == 1, juce::dontSendNotification);
        report (before, after);
    }
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override
    {
        if (starting) { if (gestureDepth++ == 0) eq.getStateInformation (gestureBefore); }
        else if (gestureDepth > 0 && --gestureDepth == 0)
        {
            juce::MemoryBlock after;
            eq.getStateInformation (after);
            report (gestureBefore, after);
        }
    }

    RackLnF lnf;   // outlives the knobs
    Eq& eq;
    EditFn onEdit;
    Body body;
    std::unique_ptr<EqGraph> graph;
    juce::Rectangle<int> titleArea;
    juce::ComboBox presetBox;
    juce::TextButton abA { "A" }, abB { "B" };
    juce::Viewport viewport;
    std::array<juce::MemoryBlock, 2> ab;
    juce::MemoryBlock gestureBefore;
    int selected = 0, current = 0, gestureDepth = 0;
};
} // namespace

// =====================================================================================================  registry

const std::vector<Info>& all()
{
    static const std::vector<Info> list {
        { "fretforge:eq", "EQ", "EQ", "Eight-band EQ: bells, shelves, notch, band pass and low / high cuts up to 96 dB/oct. Any band can be dynamic, or spectral (dynamics frequency by frequency)." },
        { "fretforge:ssleq", "Console EQ", "EQ", "Four-band console EQ in the style of a classic E-series channel strip: high, high-mid, low-mid and low bands (the outer ones shelf or bell), 18 dB/oct high-pass and 12 dB/oct low-pass." },
        { "fretforge:comp", "Compressor", "Dynamics", "Smooth compressor with soft knee and parallel mix." },
        { "fretforge:delay", "Delay", "Delay", "Stereo or ping-pong delay with filtered repeats." },
        { "fretforge:chorus", "Chorus", "Modulation", "Stereo chorus." },
        { "fretforge:reverb", "Reverb", "Reverb", "Room to hall reverb. On an FX track, set Mix to 100 %." },
        { "fretforge:limiter", "Limiter", "Dynamics", "Look-ahead brick-wall limiter: nothing gets past the ceiling. Put it last on the master." },
        { "fretforge:nam", "NAM Amp", "Amp", "Neural Amp Modeler capture with up to two cabinet impulse responses: noise gate, amp, tone controls and cabinet in one plugin. Capture and cabinets are saved with the project." },
        { "fretforge:synth", "Basic Synth", "Synth", "Polyphonic synth: two detuned oscillators, resonant filter with envelope.", true },
        { "fretforge:drumkit", "Drum Kit", "Drums", "General MIDI drums (any channel) with RAVENYR's drum sounds.", true },
    };
    return list;
}

const Info* find (const juce::String& id)
{
    for (auto& i : all())
        if (id == i.id) return &i;
    return nullptr;
}

juce::PluginDescription describe (const Info& i)
{
    juce::PluginDescription d;
    d.name = i.name;
    d.descriptiveName = i.description;
    d.pluginFormatName = formatName;
    d.category = i.category;
    d.manufacturerName = "RAVENYR";
    d.version = "1.0";
    d.fileOrIdentifier = i.id;
    d.uniqueId = d.deprecatedUid = (int) juce::String (i.id).hashCode();
    d.isInstrument = i.instrument;
    d.numInputChannels = i.instrument ? 0 : 2;
    d.numOutputChannels = 2;
    return d;
}

juce::String descriptionXml (const juce::String& id)
{
    const auto* i = find (id);
    if (i == nullptr) return {};
    return describe (*i).createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
}

std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& id)
{
    const auto* i = find (id);
    if (i == nullptr) return nullptr;
    const juce::String s (i->id);
    if (s == "fretforge:eq") return std::make_unique<Eq> (*i);
    if (s == "fretforge:ssleq") return std::make_unique<SslEq> (*i);
    if (s == "fretforge:comp") return std::make_unique<Compressor> (*i);
    if (s == "fretforge:delay") return std::make_unique<Delay> (*i);
    if (s == "fretforge:chorus") return std::make_unique<Chorus> (*i);
    if (s == "fretforge:reverb") return std::make_unique<Reverb> (*i);
    if (s == "fretforge:limiter") return std::make_unique<Limiter> (*i);
    if (s == "fretforge:nam") return std::make_unique<NamAmp> (*i);
    if (s == "fretforge:synth") return std::make_unique<BasicSynth> (*i);
    if (s == "fretforge:drumkit") return std::make_unique<DrumKit> (*i);
    return nullptr;
}

juce::String stateWith (const juce::String& id, const std::map<juce::String, float>& values)
{
    auto p = create (id);
    auto* b = dynamic_cast<Base*> (p.get());
    if (b == nullptr) return {};
    for (auto& [k, v] : values)
        if (auto* prm = b->state.getParameter (k)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    juce::MemoryBlock mb;
    b->getStateInformation (mb);
    return mb.toBase64Encoding();
}

namespace
{
/** One EQ band for a preset: type, frequency, gain, Q, slope index (cuts), dynamics (0 off, 1 dynamic, 2 spectral). */
struct PB { int type; float freq, gain = 0.0f, q = 1.0f; int slope = 1, dyn = 0; float thr = -24.0f, range = -6.0f, atk = 10.0f, rel = 120.0f, arel = 0.0f; };
Preset eqPreset (const char* category, const char* name, std::initializer_list<PB> bands, float out = 0.0f)
{
    Preset p { name, category, {} };
    int b = 0;
    for (auto& x : bands)
    {
        const auto id = [b] (const char* w) { return "b" + juce::String (b + 1) + w; };
        p.values[id ("use")] = 1.0f;
        p.values[id ("on")] = 1.0f;
        p.values[id ("type")] = (float) x.type;
        p.values[id ("freq")] = x.freq;
        p.values[id ("gain")] = x.gain;
        p.values[id ("q")] = x.q;
        p.values[id ("slope")] = (float) x.slope;
        p.values[id ("dyn")] = (float) x.dyn;
        p.values[id ("thr")] = x.thr;
        p.values[id ("range")] = x.range;
        p.values[id ("atk")] = x.atk;
        p.values[id ("rel")] = x.rel;
        p.values[id ("arel")] = x.arel;
        ++b;
    }
    p.values["out"] = out;
    return p;
}
} // namespace

const std::vector<Preset>& factoryPresets (const juce::String& id)
{
    static const std::vector<Preset> none;
    // types: 0 bell, 1 low shelf, 2 high shelf, 3 low cut, 4 high cut, 5 notch, 6 band pass
    // slopes: 0 = 6, 1 = 12, 2 = 18, 3 = 24, 4 = 36, 5 = 48, 6 = 72, 7 = 96 dB/oct
    static const std::vector<Preset> eq {
        eqPreset ("", "Flat (reset)", {}),
        eqPreset ("Cleanup", "Low cut 80 Hz", { { 3, 80.0f, 0.0f, 0.7071f, 3 } }),
        eqPreset ("Cleanup", "Remove 60 Hz hum", { { 5, 60.0f, 0.0f, 12.0f }, { 5, 120.0f, 0.0f, 12.0f }, { 5, 180.0f, 0.0f, 12.0f } }),
        eqPreset ("Cleanup", "Resonance control (spectral)",
                  { { 0, 2500.0f, 0.0f, 0.4f, 1, 2, -45.0f, -4.0f }, { 0, 220.0f, 0.0f, 0.6f, 1, 2, -35.0f, -3.0f } }),
        eqPreset ("Vocals", "Clarity", { { 3, 100.0f, 0.0f, 0.7071f, 2 }, { 0, 300.0f, -3.0f, 1.2f }, { 0, 3000.0f, 2.5f, 1.0f }, { 2, 10000.0f, 3.0f, 0.7071f } }),
        eqPreset ("Vocals", "De-esser (dynamic)", { { 3, 90.0f, 0.0f, 0.7071f, 1 }, { 0, 6500.0f, 0.0f, 3.0f, 1, 1, -32.0f, -8.0f, 1.0f, 60.0f } }),
        eqPreset ("Vocals", "Warm & smooth", { { 3, 80.0f, 0.0f, 0.7071f, 1 }, { 1, 200.0f, 1.5f, 0.7071f }, { 0, 3500.0f, 0.0f, 0.8f, 1, 2, -40.0f, -5.0f } }),
        eqPreset ("Guitar", "Tame harshness (spectral)", { { 3, 80.0f, 0.0f, 0.7071f, 1 }, { 0, 3000.0f, 0.0f, 0.7f, 1, 2, -40.0f, -6.0f } }),
        eqPreset ("Guitar", "Remove mud", { { 3, 90.0f, 0.0f, 0.7071f, 3 }, { 0, 250.0f, -3.5f, 1.4f }, { 0, 1800.0f, 1.5f, 1.0f } }),
        // Low-mid "woof" of palm-muted chugs (the 150-250 Hz chug zone): the cleanup cuts, and a dynamic cut that only acts while the chug
        // builds up, so open playing keeps its weight. A short release (25 ms) lets go between hits; Auto release is an alternative.
        eqPreset ("Guitar", "Metal Chug Control", { { 3, 90.0f, 0.0f, 0.7071f, 3 }, { 0, 190.0f, 0.0f, 0.8f, 1, 1, -23.0f, -5.5f, 15.0f, 25.0f },
                                                    { 4, 10000.0f, 0.0f, 0.7071f, 3 } }),
        eqPreset ("Guitar", "Fizz cut (high gain)", { { 3, 90.0f, 0.0f, 0.7071f, 3 }, { 4, 9000.0f, 0.0f, 0.7071f, 3 }, { 0, 5500.0f, -3.0f, 2.0f } }),
        eqPreset ("Guitar", "Acoustic sparkle", { { 3, 70.0f, 0.0f, 0.7071f, 1 }, { 0, 220.0f, -2.5f, 1.2f }, { 2, 8000.0f, 3.0f, 0.7071f } }),
        eqPreset ("Bass", "Tight low end", { { 3, 35.0f, 0.0f, 0.7071f, 3 }, { 0, 80.0f, 2.0f, 1.0f }, { 0, 250.0f, -3.0f, 1.5f }, { 1, 120.0f, 0.0f, 0.7071f, 1, 1, -20.0f, -4.0f } }),
        eqPreset ("Bass", "Growl", { { 3, 40.0f, 0.0f, 0.7071f, 3 }, { 0, 700.0f, 3.0f, 1.2f }, { 0, 2000.0f, 2.0f, 1.0f } }),
        eqPreset ("Drums", "Kick punch", { { 3, 30.0f, 0.0f, 0.7071f, 3 }, { 0, 60.0f, 4.0f, 1.2f }, { 0, 350.0f, -5.0f, 2.0f }, { 0, 4000.0f, 3.0f, 1.0f } }),
        eqPreset ("Drums", "Snare crack", { { 3, 100.0f, 0.0f, 0.7071f, 2 }, { 0, 200.0f, 2.0f, 1.0f }, { 0, 900.0f, -3.0f, 2.0f }, { 2, 5000.0f, 3.0f, 0.7071f } }),
        eqPreset ("Drums", "Overheads / cymbals", { { 3, 200.0f, 0.0f, 0.7071f, 3 }, { 0, 3500.0f, 0.0f, 0.7f, 1, 2, -38.0f, -4.0f }, { 2, 12000.0f, 2.0f, 0.7071f } }),
        eqPreset ("Drums", "Drum bus glue", { { 1, 80.0f, 1.5f, 0.7071f }, { 0, 400.0f, -2.0f, 0.8f }, { 2, 10000.0f, 2.0f, 0.7071f } }),
        eqPreset ("Master", "Gentle air & low cut", { { 3, 25.0f, 0.0f, 0.7071f, 5 }, { 2, 12000.0f, 1.5f, 0.7071f } }),
        eqPreset ("Master", "Smile curve", { { 3, 25.0f, 0.0f, 0.7071f, 5 }, { 1, 90.0f, 2.0f, 0.7071f }, { 0, 800.0f, -1.5f, 0.5f }, { 2, 10000.0f, 2.0f, 0.7071f } }),
        eqPreset ("Master", "Low end control (dynamic)", { { 3, 25.0f, 0.0f, 0.7071f, 5 }, { 1, 100.0f, 0.0f, 0.7071f, 1, 1, -18.0f, -3.0f, 30.0f, 250.0f } }),
        eqPreset ("Effects", "Telephone", { { 3, 400.0f, 0.0f, 0.7071f, 3 }, { 4, 3500.0f, 0.0f, 0.7071f, 3 }, { 0, 1500.0f, 4.0f, 1.0f } }),
        eqPreset ("Effects", "AM radio", { { 6, 1200.0f, 0.0f, 0.9f }, { 4, 5000.0f, 0.0f, 0.7071f, 5 } }, 6.0f),
    };
    return id == "fretforge:eq" ? eq : none;
}

juce::Image& brandLogoRef()
{
    static juce::Image img;
    return img;
}
void setBrandLogo (const juce::Image& logo) { brandLogoRef() = logo; }
juce::Image brandLogo() { return brandLogoRef(); }

Tone3000Browser& tone3000Browser()
{
    static Tone3000Browser b;
    return b;
}

bool hasRackPanel (const juce::String& id)
{
    return id == "fretforge:eq" || id == "fretforge:ssleq" || id == "fretforge:comp" || id == "fretforge:limiter" || id == "fretforge:reverb"
        || id == "fretforge:delay" || id == "fretforge:chorus";
}

bool hasRackPanelForDescription (const juce::String& descXml)
{
    return hasRackPanel (descXml.fromFirstOccurrenceOf (" file=\"", false, false).upToFirstOccurrenceOf ("\"", false, false));
}

std::unique_ptr<juce::Component> createRackPanel (juce::AudioPluginInstance& inst, std::function<void (const juce::MemoryBlock&, const juce::MemoryBlock&)> onEdit)
{
    if (auto* e = dynamic_cast<Eq*> (&inst)) return std::make_unique<EqRackPanel> (*e, std::move (onEdit));
    if (auto* b = dynamic_cast<Base*> (&inst); b != nullptr && hasRackPanel (b->info.id)) return std::make_unique<RackPanel> (*b, std::move (onEdit));
    return nullptr;
}

bool setNamAmpFile (juce::AudioPluginInstance& inst, int kind, const juce::File& f)
{
    auto* n = dynamic_cast<NamAmp*> (&inst);
    if (n == nullptr || kind < 0 || kind >= namamp::numFiles) return false;
    n->setFile (kind, f);
    return true;
}

bool setEqListenBand (juce::AudioPluginInstance& inst, int band)
{
    auto* e = dynamic_cast<Eq*> (&inst);
    if (e == nullptr) return false;
    e->listenBand = band;
    return true;
}

bool applyPreset (juce::AudioPluginInstance& inst, const Preset& preset)
{
    auto* b = dynamic_cast<Base*> (&inst);
    if (b == nullptr) return false;
    for (auto* prm : b->getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            const auto it = preset.values.find (rp->getParameterID());
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (it != preset.values.end() ? rp->convertTo0to1 (it->second) : rp->getDefaultValue());
            rp->endChangeGesture();
        }
    return true;
}

void Format::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& id)
{
    if (const auto* i = find (id)) results.add (new juce::PluginDescription (describe (*i)));
}

juce::String Format::getNameOfPluginFromIdentifier (const juce::String& id)
{
    const auto* i = find (id);
    return i != nullptr ? juce::String (i->name) : id;
}

juce::StringArray Format::searchPathsForPlugins (const juce::FileSearchPath&, bool, bool)
{
    juce::StringArray ids;
    for (auto& i : all()) ids.add (i.id);
    return ids;
}

void Format::createPluginInstance (const juce::PluginDescription& d, double sr, int bs, PluginCreationCallback cb)
{
    auto p = create (d.fileOrIdentifier);
    if (p == nullptr) { cb (nullptr, "Unknown built-in plugin \"" + d.name + "\"."); return; }
    p->setRateAndBufferSizeDetails (sr, bs);
    cb (std::move (p), {});
}
} // namespace ff::fx::builtin
