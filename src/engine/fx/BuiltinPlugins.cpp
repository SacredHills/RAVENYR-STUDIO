#include "BuiltinPlugins.h"
#include "Biquad.h"
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
        juce::Slider slider;
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

/** The compact version of a built-in's editor that lives inside a mixer rack slot: preset menu and A/B, the plugin's
    picture (EQ curve, gain reduction...) and its knobs. Sized by whoever hosts it. */
class RackPanel final : public juce::Component
{
public:
    explicit RackPanel (Base& b) : proc (b), eq (dynamic_cast<Eq*> (&b))
    {
        const auto& presets = factoryPresets (juce::String (b.info.id));
        for (size_t i = 0; i < presets.size(); ++i)
            presetBox.addItem ((presets[i].category.isNotEmpty() ? presets[i].category + ": " : juce::String()) + presets[i].name, (int) i + 1);
        presetBox.setTextWhenNothingSelected ("Preset");
        presetBox.onChange = [this] {
            const auto& ps = factoryPresets (juce::String (proc.info.id));
            const int i = presetBox.getSelectedId() - 1;
            if (i >= 0 && i < (int) ps.size()) applyPreset (proc, ps[(size_t) i]);
        };
        if (presets.empty()) presetBox.setVisible (false); else addAndMakeVisible (presetBox);
        for (auto* t : { &abA, &abB })
        {
            t->setClickingTogglesState (false);
            t->setColour (juce::TextButton::buttonOnColourId, col::accent);
            t->setColour (juce::TextButton::textColourOnId, col::bg);
            addAndMakeVisible (*t);
        }
        abA.setToggleState (true, juce::dontSendNotification);
        abA.onClick = [this] { switchTo (0); };
        abB.onClick = [this] { switchTo (1); };

        if (eq != nullptr)
        {
            auto g = std::make_unique<EqGraph> (*eq);
            graph = g.get();
            graph->onSelect = [this] (int band) { buildKnobs (band); resized(); };
            display = std::move (g);
        }
        else display = b.createDisplay();
        if (display != nullptr) addAndMakeVisible (*display);
        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (5);
        addAndMakeVisible (viewport);
        buildKnobs (0);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (22);
        abB.setBounds (top.removeFromRight (24));
        abA.setBounds (top.removeFromRight (24));
        top.removeFromRight (3);
        presetBox.setBounds (top);
        r.removeFromTop (4);
        if (display != nullptr)
        {
            const int dh = eq != nullptr ? juce::jlimit (70, 170, r.getHeight() - 100)
                                         : juce::jmin (juce::jmin (proc.displayHeight(), 110), juce::jmax (40, r.getHeight() - 90));
            display->setBounds (r.removeFromTop (dh));
            r.removeFromTop (4);
        }
        viewport.setBounds (r);
        const int cols = 3, cellH = 76;
        const int rows = ((int) knobs.size() + cols - 1) / cols;
        const bool scrolls = rows * cellH > r.getHeight();
        const int w = r.getWidth() - (scrolls ? viewport.getScrollBarThickness() + 1 : 0);
        grid.setSize (w, rows * cellH);
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            auto cell = juce::Rectangle<int> (((int) i % cols) * (w / cols), ((int) i / cols) * cellH, w / cols, cellH).reduced (1, 1);
            auto& k = *knobs[i];
            if (k.toggle.isVisible()) { k.toggle.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 24)); continue; }
            k.label.setBounds (cell.removeFromTop (13));
            if (k.combo.isVisible()) k.combo.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 22));
            else k.slider.setBounds (cell);
        }
    }

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        juce::TextButton toggle;
        juce::ComboBox combo;
        std::unique_ptr<juce::ComboBoxParameterAttachment> comboAttach;
        std::unique_ptr<juce::SliderParameterAttachment> attach;
        std::unique_ptr<juce::ButtonParameterAttachment> buttonAttach;
    };

    void addKnob (juce::RangedAudioParameter* rp)
    {
        if (rp == nullptr) return;
        auto k = std::make_unique<Knob>();
        k->label.setText (rp->getName (14), juce::dontSendNotification);
        k->label.setJustificationType (juce::Justification::centred);
        k->label.setFont (juce::Font (juce::FontOptions (10.5f)));
        k->label.setColour (juce::Label::textColourId, col::dim);
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
            k->toggle.setColour (juce::TextButton::buttonOnColourId, col::accent);
            k->toggle.setColour (juce::TextButton::textColourOnId, col::bg);
            k->buttonAttach = std::make_unique<juce::ButtonParameterAttachment> (*bp, k->toggle);
            grid.addAndMakeVisible (k->toggle);
        }
        else
        {
            k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 13);
            k->slider.setColour (juce::Slider::rotarySliderFillColourId, col::accent);
            k->attach = std::make_unique<juce::SliderParameterAttachment> (*rp, k->slider);
            k->slider.setDoubleClickReturnValue (true, rp->convertFrom0to1 (rp->getDefaultValue()));
            grid.addAndMakeVisible (k->slider);
            grid.addAndMakeVisible (k->label);
        }
        knobs.push_back (std::move (k));
    }

    void buildKnobs (int band)
    {
        knobs.clear();
        grid.removeAllChildren();
        if (eq != nullptr)   // the selected band's main controls and the output level
        {
            band = juce::jlimit (0, eqBandCount - 1, band);
            for (const char* what : { "freq", "gain", "q" })
                addKnob (dynamic_cast<juce::RangedAudioParameter*> (eq->state.getParameter (eqId (band, what))));
            addKnob (dynamic_cast<juce::RangedAudioParameter*> (eq->state.getParameter ("out")));
        }
        else
            for (auto* p : proc.getParameters())
                addKnob (dynamic_cast<juce::RangedAudioParameter*> (p));
    }

    void switchTo (int which)
    {
        if (which == current) return;
        proc.getStateInformation (ab[(size_t) current]);
        if (ab[(size_t) which].getSize() == 0) ab[(size_t) which] = ab[(size_t) current];   // first visit: B starts as a copy of A
        proc.setStateInformation (ab[(size_t) which].getData(), (int) ab[(size_t) which].getSize());
        current = which;
        abA.setToggleState (which == 0, juce::dontSendNotification);
        abB.setToggleState (which == 1, juce::dontSendNotification);
    }

    Base& proc;
    Eq* eq = nullptr;
    EqGraph* graph = nullptr;
    juce::ComboBox presetBox;
    juce::TextButton abA { "A" }, abB { "B" };
    std::unique_ptr<juce::Component> display;
    juce::Viewport viewport;
    juce::Component grid;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::array<juce::MemoryBlock, 2> ab;
    int current = 0;
};
} // namespace

// =====================================================================================================  registry

const std::vector<Info>& all()
{
    static const std::vector<Info> list {
        { "fretforge:eq", "EQ", "EQ", "Eight-band EQ: bells, shelves, notch, band pass and low / high cuts up to 96 dB/oct. Any band can be dynamic, or spectral (dynamics frequency by frequency)." },
        { "fretforge:comp", "Compressor", "Dynamics", "Smooth compressor with soft knee and parallel mix." },
        { "fretforge:delay", "Delay", "Delay", "Stereo or ping-pong delay with filtered repeats." },
        { "fretforge:chorus", "Chorus", "Modulation", "Stereo chorus." },
        { "fretforge:reverb", "Reverb", "Reverb", "Room to hall reverb. On an FX track, set Mix to 100 %." },
        { "fretforge:limiter", "Limiter", "Dynamics", "Look-ahead brick-wall limiter: nothing gets past the ceiling. Put it last on the master." },
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
    if (s == "fretforge:comp") return std::make_unique<Compressor> (*i);
    if (s == "fretforge:delay") return std::make_unique<Delay> (*i);
    if (s == "fretforge:chorus") return std::make_unique<Chorus> (*i);
    if (s == "fretforge:reverb") return std::make_unique<Reverb> (*i);
    if (s == "fretforge:limiter") return std::make_unique<Limiter> (*i);
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
struct PB { int type; float freq, gain = 0.0f, q = 1.0f; int slope = 1, dyn = 0; float thr = -24.0f, range = -6.0f, atk = 10.0f, rel = 120.0f; };
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

std::unique_ptr<juce::Component> createRackPanel (juce::AudioPluginInstance& inst)
{
    if (auto* b = dynamic_cast<Base*> (&inst); b != nullptr && ! b->info.instrument) return std::make_unique<RackPanel> (*b);
    return nullptr;
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
