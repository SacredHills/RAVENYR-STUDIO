#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "StudioLookAndFeel.h"
#include "engine/AudioEngine.h"
#include <cmath>

namespace ff
{
inline float gainToDb (float g) { return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f; }

/** Vertical or horizontal meter fed from a real engine LevelMeter. Shows peak with
    decay, peak-hold line and a clip LED (click to reset). Call tick() at ~30-60 Hz. */
/** A play / stop button in a page's own colour (Drum Studio, Tabs), so it is not mistaken for the main transport.
    Shows a play triangle, or a stop square while its toggle state is on. */
class PagePlayButton : public juce::TextButton
{
public:
    PagePlayButton (const juce::String& text, juce::Colour c) : juce::TextButton (text), colour (c) { setWantsKeyboardFocus (false); }
    juce::Colour colour;
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        auto c = on ? colour : colour.withMultipliedBrightness (0.3f);
        if (over) c = c.brighter (0.15f);
        if (down) c = c.darker (0.2f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (colour.brighter (0.3f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        auto icon = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.3f);
        g.setColour (c.getPerceivedBrightness() > 0.6f ? palette::onAccent : juce::Colours::white);
        if (on) g.fillRect (icon.reduced (1.0f));
        else
        {
            juce::Path p;
            p.addTriangle (icon.getX(), icon.getY(), icon.getX(), icon.getBottom(), icon.getRight(), icon.getCentreY());
            g.fillPath (p);
        }
        g.setFont (uiFont (13.0f, true));
        g.drawText (getButtonText(), r.withTrimmedRight (4.0f), juce::Justification::centred, true);
    }
};

/** Note value button: draws the note (whole ... 64th) and its fraction; lit when it is the current value. */
class NoteValueButton : public juce::Button
{
public:
    NoteValueButton (int denominator, const juce::String& label) : juce::Button (label), den (denominator) { setWantsKeyboardFocus (false); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? palette::accent : (over ? palette::panel.brighter (0.15f) : palette::panel));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? palette::accent.brighter (0.3f) : palette::outline);
        g.drawRoundedRectangle (r, 4.0f, on ? 1.5f : 1.0f);
        if (down) { g.setColour (juce::Colours::black.withAlpha (0.15f)); g.fillRoundedRectangle (r, 4.0f); }
        const auto ink = on ? juce::Colour (0xff15171a) : palette::text;
        g.setColour (ink);
        // the note: head, stem, flags
        auto glyph = r.removeFromLeft (r.getHeight() * 0.8f).reduced (2.0f, 3.0f);
        const float hw = 8.0f, hh = 5.6f;
        const juce::Rectangle<float> head (glyph.getX() + 1.0f, glyph.getBottom() - hh - 1.0f, hw, hh);
        juce::Path h;
        h.addEllipse (head);
        h.applyTransform (juce::AffineTransform::rotation (-0.35f, head.getCentreX(), head.getCentreY()));
        if (den <= 2) g.strokePath (h, juce::PathStrokeType (1.4f)); else g.fillPath (h);
        if (den >= 2)
        {
            const float sx = head.getRight() - 0.6f, top = glyph.getY() + 1.0f;
            g.drawLine (sx, head.getCentreY() - 1.0f, sx, top, 1.3f);
            int flags = den >= 64 ? 4 : den >= 32 ? 3 : den >= 16 ? 2 : den >= 8 ? 1 : 0;
            for (int f = 0; f < flags; ++f)
            {
                juce::Path fl;
                const float y = top + f * 3.6f;
                fl.startNewSubPath (sx, y);
                fl.quadraticTo (sx + 5.5f, y + 2.5f, sx + 4.5f, y + 7.0f);
                g.strokePath (fl, juce::PathStrokeType (1.3f));
            }
        }
        g.setFont (uiFont (12.0f, true));
        g.drawText (getButtonText(), r.withTrimmedRight (3.0f), juce::Justification::centred, false);
    }
private:
    int den;
};

/** Small round on / off button (lit green when on). */
class PowerButton : public juce::Button
{
public:
    PowerButton() : juce::Button ("power") { setClickingTogglesState (false); setWantsKeyboardFocus (false); }
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre (12.0f, 12.0f);
        const bool on = getToggleState();
        g.setColour (on ? palette::good : palette::well);
        g.fillEllipse (r);
        g.setColour (on ? palette::good.brighter (0.3f) : (over ? palette::text : palette::textDim));
        g.drawEllipse (r, 1.2f);
    }
};

/** A text button whose right-click opens something else (a context menu) instead of clicking. (A plain button
    clicks on any mouse button, and by the time its onClick runs the right button is no longer down.) */
class ContextButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    std::function<void()> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        rightDown = e.mods.isPopupMenu() && onRightClick != nullptr;
        if (rightDown) onRightClick(); else juce::TextButton::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! rightDown) juce::TextButton::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (rightDown) { rightDown = false; return; }
        juce::TextButton::mouseUp (e);
    }
private:
    bool rightDown = false;
};

/** Mono / stereo switch of an audio track: one ring for mono, two linked rings for stereo. */
class ChannelModeButton : public juce::Button
{
public:
    ChannelModeButton() : juce::Button ("channels")
    {
        setClickingTogglesState (false);
        setWantsKeyboardFocus (false);
    }
    void setStereo (bool s)
    {
        if (stereo != s) { stereo = s; repaint(); }
        setTooltip (stereo ? "Stereo track: records and monitors an input pair (left + right). Click to make it mono."
                           : "Mono track: records and monitors one input. Click to make it stereo (an input pair).");
    }
    bool isStereo() const { return stereo; }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (over ? palette::panelHi.brighter (0.15f) : palette::panelHi);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (palette::outline.brighter (0.25f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        const float d = juce::jmin (10.0f, r.getHeight() - 8.0f);
        const auto c = r.getCentre() + juce::Point<float> (down ? 0.5f : 0.0f, down ? 0.5f : 0.0f);
        g.setColour (over ? palette::text : palette::text.withAlpha (0.8f));
        if (stereo)
        {
            g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (c.translated (-d * 0.32f, 0.0f)), 1.4f);
            g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (c.translated (d * 0.32f, 0.0f)), 1.4f);
        }
        else g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (c), 1.4f);
    }
private:
    bool stereo = false;
};

/** Input monitoring switch: a speaker in a button-shaped box, lit blue while you hear the input through the track. */
class MonitorButton : public juce::Button
{
public:
    MonitorButton() : juce::Button ("monitor")
    {
        setClickingTogglesState (false);
        setWantsKeyboardFocus (false);
        setTooltip ("Monitor: hear what comes into this track's input (your guitar / mic) through the track and its plugins");
    }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();
        g.setColour (on ? palette::info : (over ? palette::panelHi.brighter (0.15f) : palette::panelHi));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (on ? palette::info.brighter (0.3f) : palette::outline.brighter (0.25f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        // speaker: box + cone, and sound waves when on
        auto c = r.withSizeKeepingCentre (14.0f, 12.0f).translated (down ? 0.5f : 0.0f, down ? 0.5f : 0.0f);
        juce::Path p;
        p.startNewSubPath (c.getX(), c.getCentreY() - 2.5f);
        p.lineTo (c.getX() + 3.0f, c.getCentreY() - 2.5f);
        p.lineTo (c.getX() + 7.0f, c.getY());
        p.lineTo (c.getX() + 7.0f, c.getBottom());
        p.lineTo (c.getX() + 3.0f, c.getCentreY() + 2.5f);
        p.lineTo (c.getX(), c.getCentreY() + 2.5f);
        p.closeSubPath();
        const auto ink = on ? juce::Colours::black : (over ? palette::text : palette::textDim);
        g.setColour (ink);
        g.fillPath (p);
        for (int k = 0; k < 2; ++k)
        {
            juce::Path arc;
            const float rad = 3.0f + 3.0f * (float) k;
            arc.addCentredArc (c.getX() + 7.5f, c.getCentreY(), rad, rad, 0.0f, 0.7f, 2.45f, true);
            g.strokePath (arc, juce::PathStrokeType (1.3f));
        }
    }
};

/** Level meters and volume faders share one dB scale (-60 .. +6 dB, 0 dB about three quarters of the way up), so a
    meter next to a fader reads against the fader's own marks. */
inline float levelToProportion (float db)
{
    static const juce::NormalisableRange<float> range = [] {
        juce::NormalisableRange<float> r (-60.0f, 6.0f);
        r.setSkewForCentre (-12.0f);
        return r;
    }();
    return db <= -60.0f ? 0.0f : range.convertTo0to1 (juce::jlimit (-60.0f, 6.0f, db));
}

/** Draws a meter bar (green, then yellow from -12 dB, red from -3 dB), its peak-hold line and the clip LED. */
inline void drawLevelMeter (juce::Graphics& g, juce::Rectangle<int> r, float levelDb, float holdDb, bool clipped, bool vertical)
{
    auto led = vertical ? r.removeFromTop (5) : r.removeFromRight (5);
    r = vertical ? r.withTrimmedTop (2) : r.withTrimmedRight (2);
    g.setColour (clipped ? palette::bad : palette::well.brighter (0.08f));
    g.fillRect (led);
    g.setColour (palette::well);
    g.fillRect (r);
    const float n = levelToProportion (levelDb);
    auto filled = vertical ? r.withTop (r.getBottom() - juce::roundToInt (n * (float) r.getHeight()))
                           : r.withWidth (juce::roundToInt (n * (float) r.getWidth()));
    juce::ColourGradient grad = vertical
        ? juce::ColourGradient (palette::good, 0.0f, (float) r.getBottom(), palette::bad, 0.0f, (float) r.getY(), false)
        : juce::ColourGradient (palette::good, (float) r.getX(), 0.0f, palette::bad, (float) r.getRight(), 0.0f, false);
    grad.addColour (levelToProportion (-12.0f), palette::good);
    grad.addColour (levelToProportion (-6.0f), palette::warn);
    grad.addColour (levelToProportion (-3.0f), palette::warn);
    grad.addColour (levelToProportion (-0.5f), palette::bad);
    g.setGradientFill (grad);
    g.fillRect (filled);
    // a faint 0 dB mark
    g.setColour (palette::text.withAlpha (0.25f));
    const float z = levelToProportion (0.0f);
    if (vertical) g.fillRect (r.getX(), r.getBottom() - juce::roundToInt (z * (float) r.getHeight()), r.getWidth(), 1);
    else          g.fillRect (r.getX() + juce::roundToInt (z * (float) r.getWidth()), r.getY(), 1, r.getHeight());
    if (holdDb > -59.0f)
    {
        g.setColour (holdDb > -0.5f ? palette::bad : palette::text);
        const float h = levelToProportion (holdDb);
        if (vertical) g.fillRect (r.getX(), r.getBottom() - juce::roundToInt (h * (float) r.getHeight()), r.getWidth(), 1);
        else          g.fillRect (r.getX() + juce::roundToInt (h * (float) r.getWidth()), r.getY(), 1, r.getHeight());
    }
}

/** The meter above a strip's Drive knob: an analogue VU dial. The needle rests at the left. In drive mode `value` is the change
    in level the saturation makes (0 to -12 dB) and the needle swings right as it takes more off; in track mode `value` is the
    track's level in dBFS (-36 to 0) and the red zone is the top. */
inline void drawVuMeter (juce::Graphics& g, juce::Rectangle<int> r, float value, bool trackMode = false)
{
    const auto face = r.toFloat();
    g.setColour (juce::Colour (0xff0b0b0b));
    g.fillRoundedRectangle (face.expanded (1.5f), 4.0f);
    juce::ColourGradient grad (juce::Colour (0xffe6dfc0), face.getCentreX(), face.getY(), juce::Colour (0xffb8ae86), face.getCentreX(), face.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (face, 3.0f);

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (r);
    const float cx = face.getCentreX(), pivotY = face.getBottom() + face.getHeight() * 0.62f;
    const float radius = pivotY - face.getY() - 6.0f;
    const float a0 = juce::degreesToRadians (-52.0f), a1 = juce::degreesToRadians (52.0f);
    const auto point = [&] (float ang, float rad) { return juce::Point<float> (cx + rad * std::sin (ang), pivotY - rad * std::cos (ang)); };
    const auto ink = juce::Colour (0xff2b261a);
    const auto red = juce::Colour (0xffb0301f);
    const float redFrom = trackMode ? 0.83f : 0.75f;
    // the scale arc, its red end, then the ticks
    for (int pass = 0; pass < 2; ++pass)
    {
        juce::Path arc;
        arc.addCentredArc (cx, pivotY, radius, radius, 0.0f, pass == 0 ? a0 : a0 + (a1 - a0) * redFrom, pass == 0 ? a0 + (a1 - a0) * redFrom : a1, true);
        g.setColour (pass == 0 ? ink : red);
        g.strokePath (arc, juce::PathStrokeType (pass == 0 ? 1.0f : 2.4f));
    }
    g.setFont (uiFont (8.0f));
    for (int k = 0; k <= 12; ++k)
    {
        const float f = (float) k / 12.0f, ang = a0 + (a1 - a0) * f;
        const int db = trackMode ? -36 + 3 * k : k;
        const bool major = trackMode ? (k == 0 || k == 4 || k == 6 || k == 8 || k == 10 || k == 12) : k % 3 == 0;
        g.setColour (f >= redFrom - 0.001f ? red : ink);
        g.drawLine ({ point (ang, radius), point (ang, radius - (major ? 5.0f : 3.0f)) }, major ? 1.2f : 0.8f);
        if (major)
        {
            const auto p = point (ang, radius - 12.0f);
            g.drawText (juce::String (db), juce::Rectangle<float> (p.x - 9.0f, p.y - 5.0f, 18.0f, 10.0f), juce::Justification::centred);
        }
    }
    g.setColour (ink.withAlpha (0.7f));
    g.setFont (uiFont (8.0f).boldened());
    g.drawText (trackMode ? "dBFS" : "dB", juce::Rectangle<float> (face.getX() + 4.0f, face.getY() + 2.0f, 28.0f, 10.0f), juce::Justification::centredLeft);
    // the needle
    const float t = trackMode ? juce::jlimit (0.0f, 1.0f, (value + 36.0f) / 36.0f) : juce::jlimit (0.0f, 1.0f, -value / 12.0f);
    const float ang = a0 + (a1 - a0) * t;
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawLine ({ point (ang, 6.0f).translated (1.5f, 1.5f), point (ang, radius + 2.0f).translated (1.5f, 1.5f) }, 1.4f);   // its shadow
    g.setColour (juce::Colour (0xff15110a));
    g.drawLine ({ point (ang, 6.0f), point (ang, radius + 2.0f) }, 1.3f);
    g.setColour (juce::Colour (0xff15110a));
    g.fillEllipse (cx - 7.0f, face.getBottom() - 5.0f, 14.0f, 14.0f);   // the hub peeks over the bottom edge
}

/** Peak level of a LevelMeter with falling bar, peak hold and a clip LED that stays lit for 3 s after the last clip
    (click to clear it). */
class MeterView : public juce::Component, public juce::SettableTooltipClient
{
public:
    MeterView (LevelMeter& m, bool isVertical = true) : meter (m), vertical (isVertical) { clipSeen = m.clipCount.load(); }

    void tick()
    {
        const float pk = meter.uiPeak;
        const float db = gainToDb (pk);
        level = db > level ? db : juce::jmax (db, level - 1.6f); // ~50 dB/s at 30 Hz
        if (db >= hold) { hold = db; holdTicks = 45; }
        else if (--holdTicks <= 0) hold = juce::jmax (-60.0f, hold - 1.5f);
        if (const auto c = meter.clipCount.load(); c != clipSeen) { clipSeen = c; clipTicks = 90; }
        else if (clipTicks > 0) --clipTicks;
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override { clipTicks = 0; meter.clipped = false; hold = -60.0f; }

    void paint (juce::Graphics& g) override { drawLevelMeter (g, getLocalBounds(), level, hold, clipTicks > 0, vertical); }

    float levelDb() const { return level; }

private:
    LevelMeter& meter;
    bool vertical;
    float level = -120.0f, hold = -60.0f;
    int holdTicks = 0, clipTicks = 0;
    juce::uint32 clipSeen = 0;
};

/** Small label helper with consistent styling. */
inline void styleLabel (juce::Label& l, float size = 13.0f, juce::Colour c = palette::textDim, bool bold = false)
{
    l.setFont (uiFont (size, bold));
    l.setColour (juce::Label::textColourId, c);
}

/** A heading + body text card describing work that is not implemented yet. No controls. */
class NoticeCard : public juce::Component
{
public:
    NoticeCard (juce::String heading, juce::String body) : title (std::move (heading)), text (std::move (body)) {}
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (palette::panel);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (palette::accentDim);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);
        r.reduce (16, 12);
        g.setColour (palette::accent);
        g.setFont (uiFont (14.0f, true));
        g.drawText (title, r.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (palette::textDim);
        g.setFont (uiFont (13.5f));
        g.drawFittedText (text, r, juce::Justification::topLeft, 12, 1.0f);
    }
private:
    juce::String title, text;
};
} // namespace ff
