#include "TransportBar.h"
#include "core/MusicTheory.h"

namespace ff
{
using namespace project;

namespace
{
/** Pop-up list of what is using the CPU: every amp rig, insert plugin and instrument, busiest first. */
class CpuBreakdownView final : public juce::Component, private juce::Timer
{
public:
    CpuBreakdownView (EngineHost& h, std::function<std::vector<fx::RigBank::CpuRow>()> rowsFn) : host (h), getRows (std::move (rowsFn))
    {
        setSize (480, 40 + 22 * (maxRows + 1) + 70);
        refresh();
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (palette::panel);
        auto r = getLocalBounds().reduced (14, 10);
        g.setColour (palette::text);
        g.setFont (uiFont (14.0f, true));
        g.drawText ("What is using the CPU", r.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (palette::textDim);
        g.setFont (uiFont (12.0f));
        const int xr = host.xruns();
        g.drawText ("Audio engine " + juce::String (juce::roundToInt (host.cpuUsage() * 100.0)) + "%" + (xr > 0 ? "   dropouts: " + juce::String (xr) : juce::String()),
                    r.removeFromTop (18), juce::Justification::centredLeft);
        r.removeFromTop (6);

        float listed = 0.0f;
        for (const auto& row : rows) listed += row.load;
        int shown = 0;
        for (const auto& row : rows)
        {
            if (shown++ >= maxRows) break;
            auto line = r.removeFromTop (22);
            auto pct = line.removeFromRight (52);
            auto bar = line.removeFromRight (110).reduced (0, 6);
            g.setColour (palette::well);
            g.fillRect (bar);
            g.setColour (row.load > 0.5f ? palette::bad : (row.load > 0.25f ? palette::warn : palette::info));
            g.fillRect (bar.withWidth (juce::jmax (1, juce::roundToInt ((float) bar.getWidth() * juce::jmin (1.0f, row.load)))));
            g.setColour (palette::text);
            g.setFont (uiFont (12.5f));
            g.drawText (juce::String::formatted ("%.1f%%", row.load * 100.0f), pct, juce::Justification::centredRight);
            g.drawFittedText (row.track + "  -  " + row.what, line.withTrimmedRight (8), juce::Justification::centredLeft, 1);
        }
        if (rows.empty())
        {
            g.setColour (palette::textDim);
            g.setFont (uiFont (12.5f));
            g.drawText ("No plugins or amp rigs are running.", r.removeFromTop (22), juce::Justification::centredLeft);
        }
        const auto total = (float) host.cpuUsage();
        r.setTop (getHeight() - 62);
        g.setColour (palette::outline);
        g.drawHorizontalLine (r.getY(), (float) r.getX(), (float) r.getRight());
        g.setColour (palette::textDim);
        g.setFont (uiFont (11.5f));
        g.drawFittedText ("Plugins and rigs listed: " + juce::String (juce::roundToInt (listed * 100.0f)) + "% of one audio block's time. "
                          "Everything else (mixing, recording, sample streaming, meters) is the rest of the engine's " + juce::String (juce::roundToInt (total * 100.0f))
                          + "%. Plugins on different cores run side by side, so the list can add up to more than the total.",
                          r.withTrimmedTop (4), juce::Justification::topLeft, 4);
    }

private:
    static constexpr int maxRows = 12;
    EngineHost& host;
    std::function<std::vector<fx::RigBank::CpuRow>()> getRows;
    std::vector<fx::RigBank::CpuRow> rows;
    void refresh() { rows = getRows ? getRows() : std::vector<fx::RigBank::CpuRow>(); }
    void timerCallback() override { refresh(); repaint(); }
};
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = getToggleState();
    auto base = on ? (icon == Icon::metronome || icon == Icon::loop ? palette::accent : (icon == Icon::record ? palette::bad : palette::accent))
                   : palette::panelHi;
    if (down) base = base.darker (0.2f);
    else if (highlighted) base = base.brighter (0.1f);
    g.setColour (base);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (palette::outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    const auto c = r.getCentre();
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.28f;
    g.setColour (on ? juce::Colours::black : palette::text);
    juce::Path p;
    switch (icon)
    {
        case Icon::play:    p.addTriangle (c.x - s * 0.8f, c.y - s, c.x - s * 0.8f, c.y + s, c.x + s, c.y); break;
        case Icon::stop:    p.addRectangle (c.x - s * 0.85f, c.y - s * 0.85f, s * 1.7f, s * 1.7f); break;
        case Icon::toStart:
            p.addRectangle (c.x - s, c.y - s, s * 0.35f, s * 2.0f);
            p.addTriangle (c.x + s, c.y - s, c.x + s, c.y + s, c.x - s * 0.5f, c.y);
            break;
        case Icon::metronome:
            p.addTriangle (c.x - s * 0.8f, c.y + s, c.x + s * 0.8f, c.y + s, c.x, c.y - s * 1.1f);
            g.fillPath (p);
            p.clear();
            g.setColour (on ? palette::accent : palette::panelHi);
            p.addLineSegment ({ c.x, c.y + s * 0.6f, c.x + s * 0.9f, c.y - s * 0.9f }, 2.0f);
            break;
        case Icon::record:
            g.setColour (on ? juce::Colours::white : palette::bad);
            p.addEllipse (c.x - s * 0.9f, c.y - s * 0.9f, s * 1.8f, s * 1.8f);
            break;
        case Icon::loop:
        {
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, s, s * 0.75f, 0.0f, 0.5f, juce::MathConstants<float>::twoPi - 0.3f, true);
            g.strokePath (arc, juce::PathStrokeType (2.0f));
            const float ax = c.x + s * std::sin (0.5f), ay = c.y - s * 0.75f * std::cos (0.5f);
            p.addTriangle (ax - 4.0f, ay - 4.0f, ax + 3.0f, ay, ax - 4.0f, ay + 4.0f);
            break;
        }
    }
    g.fillPath (p);
}

void TunerDisplay::update()
{
    const double f = host.tuner.frequency.load();
    if (f > 0.0)
    {
        const double a4 = settings != nullptr ? settings->getDoubleValue ("tunerA4", 440.0) : 440.0;
        const double midi = 69.0 + 12.0 * std::log2 (f / a4);
        const int n = juce::roundToInt (midi);
        note = theory::midiNoteName (n);
        cents = (midi - n) * 100.0;
        holdTicks = 20;
    }
    else if (holdTicks > 0 && --holdTicks == 0)
        note = {};
    shownCents += (cents - shownCents) * 0.35;
    repaint();
}

void TunerDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (palette::well);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (isMouseOver() ? palette::accentDim : palette::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    auto inner = r.reduced (6.0f, 3.0f);
    auto top = inner.removeFromTop (12.0f);
    g.setFont (uiFont (10.0f, true));
    g.setColour (palette::textDim);
    g.drawText ("TUNER", top, juce::Justification::centredLeft);
    const bool live = note.isNotEmpty();
    const bool inTune = live && std::abs (shownCents) < 3.0;
    if (live)
        g.drawText ((shownCents >= 0 ? "+" : "") + juce::String (juce::roundToInt (shownCents)) + " c", top, juce::Justification::centredRight);
    auto noteArea = inner.removeFromLeft (44.0f);
    g.setFont (monoFont (20.0f, true));
    g.setColour (inTune ? palette::good : (live ? palette::text : palette::textDim));
    g.drawText (live ? note : "--", noteArea, juce::Justification::centredLeft);
    // cents scale -50..+50
    auto bar = inner.reduced (2.0f, 6.0f);
    g.setColour (palette::panelHi);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (palette::good.withAlpha (0.35f));
    g.fillRect (bar.withSizeKeepingCentre (bar.getWidth() * 0.06f, bar.getHeight()));
    g.setColour (palette::textDim);
    g.fillRect (juce::Rectangle<float> (bar.getCentreX() - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
    if (live)
    {
        const float x = bar.getCentreX() + (float) juce::jlimit (-50.0, 50.0, shownCents) / 50.0f * bar.getWidth() * 0.5f;
        g.setColour (inTune ? palette::good : (std::abs (shownCents) < 15.0 ? palette::warn : palette::bad));
        g.fillRoundedRectangle (juce::Rectangle<float> (x - 2.0f, bar.getY() - 3.0f, 4.0f, bar.getHeight() + 6.0f), 1.5f);
    }
}

TransportBar::TransportBar (EngineHost& h, ProjectDocument& d, juce::PropertiesFile* settings)
    : host (h), doc (d), inMeter (h.engine.inputMeter, false), outL (h.engine.outputMeterL, false),
      outR (h.engine.outputMeterR, false), tunerDisplay (h, settings)
{
    rtzButton.onClick = [this] { if (onReturnToStart) onReturnToStart(); else host.engine.returnToZero(); };
    playButton.onClick = [this] { if (onPlayStop) onPlayStop(); else host.engine.togglePlay(); };
    clickButton.onClick = [this] { toggleClick(); };
    recordButton.onClick = [this] { if (onRecord) onRecord(); };
    loopButton.onClick = [this] { toggleLoop(); };
    punchButton.onClick = [this] { togglePunch(); };
    countInButton.onClick = [this] {
        const int bars = ((int) metroNode[ids::countInBars] + 1) % 3;
        doc.set (metroNode, ids::countInBars, bars, false);
    };
    rtzButton.setTooltip ("Return to start (Home)");
    playButton.setTooltip ("Play / Stop (Space)");
    recordButton.setTooltip ("Record on armed tracks (R). Press again to punch out.");
    loopButton.setTooltip ("Loop the range set on the ruler (L). Recording in a loop makes one take per pass.");
    punchButton.setTooltip ("Punch: record only inside the ruler range");
    countInButton.setTooltip ("Count-in bars before recording starts from stop (click to cycle Off / 1 / 2)");
    punchButton.setClickingTogglesState (false);
    punchButton.setColour (juce::TextButton::buttonOnColourId, palette::bad);
    countInButton.setColour (juce::TextButton::buttonOnColourId, palette::accent);
    for (juce::Component* b : std::initializer_list<juce::Component*> { &rtzButton, &playButton, &recordButton, &loopButton, &clickButton, &clickSettingsButton,
                                                                       &punchButton, &countInButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    for (auto* l : { &barsLabel, &timeLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setColour (juce::Label::backgroundColourId, palette::well);
        addAndMakeVisible (l);
    }
    barsLabel.setFont (monoFont (22.0f, true));
    barsLabel.setColour (juce::Label::textColourId, palette::accent);
    timeLabel.setFont (monoFont (14.0f));
    timeLabel.setColour (juce::Label::textColourId, palette::textDim);

    bpmCaption.setText ("TEMPO", juce::dontSendNotification);
    sigCaption.setText ("METER", juce::dontSendNotification);
    inCaption.setText ("IN", juce::dontSendNotification);
    outCaption.setText ("OUT", juce::dontSendNotification);
    for (auto* l : { &bpmCaption, &sigCaption, &inCaption, &outCaption, &cpuLabel })
    {
        styleLabel (*l, 11.0f, palette::textDim, true);
        addAndMakeVisible (l);
    }
    settingsButton.setClickingTogglesState (false);
    settingsButton.setTooltip ("Settings");
    settingsButton.onClick = [this] { if (onSettings) onSettings(); };
    addAndMakeVisible (settingsButton);
    cpuLabel.setJustificationType (juce::Justification::centredLeft);
    cpuLabel.setInterceptsMouseClicks (false, false);   // clicks go to the bar (mouseUp), which opens the CPU breakdown

    tempoSlider.setSliderStyle (juce::Slider::LinearBarVertical);
    tempoSlider.setRange (20.0, 400.0, 0.01);
    tempoSlider.setNumDecimalPlacesToDisplay (2);
    tempoSlider.setTextValueSuffix (" BPM");
    tempoSlider.setVelocityBasedMode (true);
    tempoSlider.setVelocityModeParameters (0.6, 1, 0.05, false);
    tempoSlider.setColour (juce::Slider::trackColourId, palette::well);
    tempoSlider.setTooltip ("Drag up/down or double-click to type a tempo (quarter-note BPM)");
    tempoSlider.onDragStart = [this] { doc.undo().beginNewTransaction ("Tempo"); };
    tempoSlider.onValueChange = [this] {
        if (! updating)
        {
            if (! tempoSlider.isMouseButtonDown()) doc.undo().beginNewTransaction ("Tempo");
            doc.setTempo (tempoSlider.getValue());
        }
    };
    addAndMakeVisible (tempoSlider);

    for (int i = 1; i <= 16; ++i) sigNum.addItem (juce::String (i), i);
    for (int d2 : { 2, 4, 8, 16 }) sigDen.addItem (juce::String (d2), d2);
    auto onSig = [this] {
        if (! updating && sigNum.getSelectedId() > 0 && sigDen.getSelectedId() > 0)
            doc.setTimeSignature (sigNum.getSelectedId(), sigDen.getSelectedId());
    };
    sigNum.onChange = onSig;
    sigDen.onChange = onSig;
    addAndMakeVisible (sigNum);
    addAndMakeVisible (sigDen);

    tapButton.setTooltip ("Tap tempo (T): tap in time with the music. The tempo is set from the average gap between your taps, from the third tap on; wait two seconds to start over");
    tapButton.onClick = [this] { tapTempo(); };
    addAndMakeVisible (tapButton);

    inMeter.setTooltip ("Input level of the analysis/tuner input. Click to reset the clip indicator.");
    for (auto* m : { &inMeter, &outL, &outR })
        addAndMakeVisible (m);
    tunerDisplay.setTooltip ("Tuner (input from Settings / the tuner window). Click to open the tuner.");
    tunerDisplay.onClick = [this] { if (onOpenTuner) onOpenTuner(); };
    tunerDisplay.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    addAndMakeVisible (tunerDisplay);
    host.tuner.active = true;   // the readout is always visible
    clickSettingsButton.onClick = [this] { if (onOpenMetronome) onOpenMetronome(); };
    clickSettingsButton.setTooltip ("Metronome settings: click sound, accent on/off, subdivision, tempo trainer, practice metronome.");
    clickButton.onRightClick = [this] { if (onOpenMetronome) onOpenMetronome(); };
    clickButton.setTooltip ("Metronome click while playing (C). Right-click: metronome window (practice metronome, trainer).");

    rebind();
    startTimerHz (30);
}

TransportBar::~TransportBar()
{
    transportNode.removeListener (this);
    metroNode.removeListener (this);
}

void TransportBar::rebind()
{
    transportNode.removeListener (this);
    metroNode.removeListener (this);
    transportNode = doc.transport();
    metroNode = doc.metronome();
    transportNode.addListener (this);
    metroNode.addListener (this);
    refreshFromProject();
}

void TransportBar::refreshFromProject()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    tempoSlider.setValue (doc.tempo(), juce::dontSendNotification);
    sigNum.setSelectedId (doc.tsNum(), juce::dontSendNotification);
    sigDen.setSelectedId (doc.tsDen(), juce::dontSendNotification);
    clickButton.setToggleState ((bool) metroNode[ids::enabled], juce::dontSendNotification);
    loopButton.setToggleState ((bool) transportNode[ids::loopEnabled], juce::dontSendNotification);
    punchButton.setToggleState ((bool) transportNode[ids::punchEnabled], juce::dontSendNotification);
    const int bars = (int) metroNode[ids::countInBars];
    countInButton.setButtonText (bars == 0 ? "NO COUNT" : (bars == 1 ? "COUNT 1 BAR" : "COUNT 2 BARS"));
    countInButton.setToggleState (bars > 0, juce::dontSendNotification);
}

void TransportBar::toggleLoop()
{
    doc.set (transportNode, ids::loopEnabled, ! (bool) transportNode[ids::loopEnabled], false);
}

void TransportBar::togglePunch()
{
    doc.set (transportNode, ids::punchEnabled, ! (bool) transportNode[ids::punchEnabled], false);
}

void TransportBar::tapTempo()
{
    // Tap in time with the music: the tempo is set from the average gap between your taps (from the third tap on).
    // A pause of two seconds starts over.
    const double bpm = tap.tap (juce::Time::getMillisecondCounterHiRes());
    tapButton.setButtonText (tap.numTaps() >= 3 ? juce::String (juce::roundToInt (bpm)) : "TAP");
    if (bpm > 0.0 && tap.numTaps() >= 3)
    {
        if (tap.numTaps() == 3) doc.undo().beginNewTransaction ("Tap tempo");
        doc.setTempo (bpm);
    }
    juce::Timer::callAfterDelay (2200, [safe = juce::Component::SafePointer<TransportBar> (this)] {
        if (safe != nullptr && juce::Time::getMillisecondCounterHiRes() - safe->lastTapMs > 2000.0) safe->tapButton.setButtonText ("TAP");
    });
    lastTapMs = juce::Time::getMillisecondCounterHiRes();
}

void TransportBar::toggleClick()
{
    doc.set (metroNode, ids::enabled, ! (bool) metroNode[ids::enabled], false);
}

void TransportBar::timerCallback()
{
    auto& e = host.engine;
    const bool playing = e.isPlaying();
    const bool recording = e.isRecordRequested();
    recordBlink = (recordBlink + 1) % 30;
    recordButton.setToggleState (recording && (! e.isCountingIn() || recordBlink < 15), juce::dontSendNotification);
    playButton.setToggleState (playing, juce::dontSendNotification);
    playButton.setIcon (playing ? IconButton::Icon::stop : IconButton::Icon::play);

    const double sr0 = e.getSampleRate();
    const auto tempoMap = doc.tempoMap();
    {
        // the tempo shown is the tempo at the play position (live while playing) once there is a tempo track
        const bool live = ! tempoMap.isConstant();
        bpmCaption.setText (live ? "TEMPO AT PLAYHEAD" : "TEMPO", juce::dontSendNotification);
        if (! tempoSlider.isMouseButtonDown() && ! tempoSlider.hasKeyboardFocus (true))
        {
            const double shown = live ? tempoMap.bpmAtSeconds (sr0 > 0 ? (double) e.getPositionSamples() / sr0 : 0.0) : doc.tempo();
            if (std::abs (tempoSlider.getValue() - shown) > 0.004)
            {
                const juce::ScopedValueSetter<bool> svs (updating, true);
                tempoSlider.setValue (shown, juce::dontSendNotification);
            }
        }
    }
    const double ppq = tempoMap.isConstant() ? e.getPositionPpq() : tempoMap.quarterAtSeconds (sr0 > 0 ? (double) e.getPositionSamples() / sr0 : 0.0);
    const int num = doc.tsNum(), den = doc.tsDen();
    const double qPerBar = num * 4.0 / den;
    const int bar = (int) std::floor (ppq / qPerBar) + 1;
    const double inBar = (ppq - (bar - 1) * qPerBar) * den / 4.0;
    const int beat = (int) std::floor (inBar) + 1;
    const int ticks = (int) ((inBar - std::floor (inBar)) * 960.0);
    barsLabel.setText (juce::String (bar).paddedLeft ('0', 3) + "." + juce::String (beat) + "." + juce::String (ticks).paddedLeft ('0', 3),
                       juce::dontSendNotification);

    const double sr = e.getSampleRate();
    const double secs = sr > 0 ? (double) e.getPositionSamples() / sr : 0.0;
    const int mins = (int) (secs / 60.0);
    timeLabel.setText (juce::String (mins).paddedLeft ('0', 2) + ":" + juce::String (secs - mins * 60.0, 3).paddedLeft ('0', 6),
                       juce::dontSendNotification);

    e.inputMeter.uiTake();
    e.outputMeterL.uiTake();
    e.outputMeterR.uiTake();
    inMeter.tick();
    outL.tick();
    outR.tick();
    tunerDisplay.update();

    const auto midi = host.midiActivity.load();
    if (midi != lastMidi) { lastMidi = midi; midiFlash = 6; }
    else if (midiFlash > 0) --midiFlash;

    const int xr = host.xruns();
    cpuLabel.setText ("CPU " + juce::String (juce::roundToInt (host.cpuUsage() * 100.0)) + "%"
                          + (xr > 0 ? "   XRUN " + juce::String (xr) : juce::String()),
                      juce::dontSendNotification);
    cpuLabel.setColour (juce::Label::textColourId, host.cpuUsage() > 0.8 || xr > 0 ? palette::bad : palette::textDim);
    repaint (getLocalBounds().removeFromRight (40));
}

void TransportBar::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (cpuRows && cpuLabel.getBounds().contains (e.getPosition()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void TransportBar::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasDraggedSinceMouseDown() && cpuRows && cpuLabel.getBounds().contains (e.getPosition()))
        showCpuBreakdown();
}

void TransportBar::showCpuBreakdown()
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<CpuBreakdownView> (host, cpuRows), cpuLabel.getScreenBounds(), nullptr);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.setColour (palette::panel);
    g.fillRect (getLocalBounds());
    g.setColour (palette::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // MIDI activity LED (top-right)
    auto led = getLocalBounds().removeFromRight (36).withSizeKeepingCentre (26, 14);
    g.setColour (midiFlash > 0 ? palette::info : palette::well);
    g.fillRoundedRectangle (led.toFloat(), 3.0f);
    g.setColour (midiFlash > 0 ? juce::Colours::black : palette::textDim);
    g.setFont (uiFont (9.5f, true));
    g.drawText ("MIDI", led, juce::Justification::centred);
}

void TransportBar::resized()
{
    auto full = getLocalBounds().reduced (10, 8);
    const int h = full.getHeight();
    // one group centred in the bar (the MIDI LED stays at the far right); the gaps shrink when the bar is narrow
    const int meterW = 116, cpuW = 84, tunerW = 116, posW = 150;
    const int fixed = 5 * h + 8 + 4 * 4 + 6 + 30 + 96 + posW + 120 + 6 + 46 + 124 + tunerW + meterW + cpuW;
    const int avail = getWidth() - 2 * 40;
    const int gap = juce::jlimit (4, 16, (avail - fixed) / 6);
    const int total = fixed + 6 * gap;
    const int x0 = juce::jmax (8, 40 + (avail - total) / 2);
    auto r = juce::Rectangle<int> (x0, full.getY(), juce::jmin (total, getWidth() - x0 - 36), h);

    rtzButton.setBounds (r.removeFromLeft (h));
    r.removeFromLeft (4);
    playButton.setBounds (r.removeFromLeft (h + 8));
    r.removeFromLeft (4);
    recordButton.setBounds (r.removeFromLeft (h));
    r.removeFromLeft (4);
    loopButton.setBounds (r.removeFromLeft (h));
    r.removeFromLeft (4);
    clickButton.setBounds (r.removeFromLeft (h));
    r.removeFromLeft (2);
    clickSettingsButton.setBounds (r.removeFromLeft (28).withSizeKeepingCentre (28, h * 2 / 3));
    r.removeFromLeft (6);
    auto small = r.removeFromLeft (96);
    punchButton.setBounds (small.removeFromTop (h / 2 - 1));
    countInButton.setBounds (small.removeFromBottom (h / 2 - 1));
    r.removeFromLeft (gap);

    auto pos = r.removeFromLeft (posW);
    barsLabel.setBounds (pos.removeFromTop (h * 3 / 5));
    timeLabel.setBounds (pos);
    r.removeFromLeft (gap);

    auto tempo = r.removeFromLeft (120);
    bpmCaption.setBounds (tempo.removeFromTop (14));
    tempoSlider.setBounds (tempo.reduced (0, 1));
    r.removeFromLeft (6);
    tapButton.setBounds (r.removeFromLeft (46).withTrimmedTop (14));
    r.removeFromLeft (gap);

    auto sig = r.removeFromLeft (124);
    sigCaption.setBounds (sig.removeFromTop (14));
    sigNum.setBounds (sig.removeFromLeft (60).reduced (0, 1));
    sigDen.setBounds (sig.removeFromRight (60).reduced (0, 1));
    r.removeFromLeft (gap);

    tunerDisplay.setBounds (r.removeFromLeft (tunerW));
    r.removeFromLeft (gap);

    auto meters = r.removeFromLeft (meterW);
    auto inRow = meters.removeFromTop (meters.getHeight() / 3);
    inCaption.setBounds (inRow.removeFromLeft (30));
    inMeter.setBounds (inRow.reduced (0, 2));
    meters.removeFromTop (2);
    auto outRow = meters;
    outCaption.setBounds (outRow.removeFromLeft (30));
    outL.setBounds (outRow.removeFromTop (outRow.getHeight() / 2).reduced (0, 1));
    outR.setBounds (outRow.reduced (0, 1));
    r.removeFromLeft (gap);
    auto cpuCol = r.removeFromLeft (cpuW);   // CPU readout with the Settings button under it
    cpuLabel.setBounds (cpuCol.removeFromTop (juce::jmax (14, cpuCol.getHeight() / 2)));
    settingsButton.setBounds (cpuCol.reduced (0, 1));
}
} // namespace ff
