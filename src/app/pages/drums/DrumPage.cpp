#include "DrumPage.h"
#include <climits>
#include <unordered_set>
#include "engine/TabSynth.h"
#include "engine/DrumSynth.h"
#include "core/Timeline.h"
#include "core/ProjectFile.h"
#include "core/GuitarPro.h"
#include <cmath>

namespace ff
{
namespace dids = drums::ids;

namespace
{
juce::Colour laneColour (int lane)
{
    using namespace drums;
    if (lane <= kick2) return juce::Colour (0xffe8833a);
    if (lane <= clap) return juce::Colour (0xffe05d5d);
    if (lane <= hhOpen) return juce::Colour (0xffe6c84a);
    if (lane <= tomFloor) return juce::Colour (0xff6cc070);
    return juce::Colour (0xff5ca8e6);
}

const char* sigNames[] = { "4/4", "3/4", "2/4", "5/4", "6/4", "7/4", "6/8", "7/8", "9/8", "12/8", "5/8" };
const std::pair<int, const char*> grids[] = { { 2, "1/8" }, { 3, "1/8 triplets" }, { 4, "1/16" }, { 6, "1/16 triplets" }, { 8, "1/32" } };
} // namespace

// ================================================================== grid

DrumGrid::DrumGrid (ProjectDocument& d, EngineHost& h) : doc (d), host (h) {}

void DrumGrid::setPattern (juce::ValueTree p)
{
    if (p != pat) { selected.clear(); selKeys.clear(); }
    pat = p;
    updateSize (getParentComponent() != nullptr ? getParentComponent()->getWidth() : getWidth());
    repaint();
}

float DrumGrid::xForQ (double q) const
{
    return pat.isValid() ? (float) nameWidth + cellW * (float) (q * drums::ppq / drums::stepTicks (pat)) : 0.0f;
}

void DrumGrid::setPlayheadQ (double q)
{
    if (std::abs (q - playQ) < 1.0e-4) return;
    // repaint only the strips around the old and new playhead: long patterns make this component very wide
    const auto strip = [this] (double at) { return juce::Rectangle<int> ((int) xForQ (at) - 3, 0, 6, getHeight()); };
    if (playQ >= 0.0) repaint (strip (playQ));
    playQ = q;
    if (playQ >= 0.0) repaint (strip (playQ));
}

int DrumGrid::steps() const
{
    return pat.isValid() ? juce::jmax (1, drums::lengthTicks (pat) / drums::stepTicks (pat)) : 16;
}

void DrumGrid::updateSize (int visibleWidth)
{
    const int n = steps();
    cellW = juce::jlimit (12.0f, 44.0f, (float) (visibleWidth - nameWidth - 10) / (float) n);
    setSize (juce::jmax (visibleWidth, nameWidth + (int) std::ceil (cellW * (float) n) + 10), velTop() + velLaneHeight + 8);
}

juce::ValueTree DrumGrid::hitInCell (int lane, int step) const
{
    const int st = drums::stepTicks (pat);
    for (auto h : pat)
        if ((int) h[dids::lane] == lane && (int) h[dids::tick] >= step * st && (int) h[dids::tick] < (step + 1) * st)
            return h;
    return {};
}

void DrumGrid::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    const int lanes = drums::numPieces;
    const float gridRight = (float) nameWidth + cellW * (float) steps();
    for (int l = 0; l < lanes; ++l)
    {
        const int y = headerHeight + l * laneHeight;
        g.setColour (l % 2 ? palette::panel.withAlpha (0.55f) : palette::panel.withAlpha (0.25f));
        g.fillRect (0.0f, (float) y, gridRight, (float) laneHeight);
    }
    auto drawNames = [&] {
        // lane names stay visible at the left edge however far the grid is scrolled
        const int x0 = juce::jmax (0, -getX());
        g.setColour (palette::bg);
        g.fillRect (x0, 0, nameWidth, getHeight());
        for (int l = 0; l < lanes; ++l)
        {
            const int y = headerHeight + l * laneHeight;
            g.setColour (l % 2 ? palette::panel.withAlpha (0.55f) : palette::panel.withAlpha (0.25f));
            g.fillRect (x0, y, nameWidth, laneHeight);
            g.setColour (laneColour (l));
            g.fillRect (x0 + 2, y + 4, 3, laneHeight - 8);
            const bool sampled = laneHasSample && laneHasSample (l);
            g.setColour (sampled ? palette::accent : palette::text);
            g.setFont (uiFont (12.0f, sampled));
            g.drawText (juce::String (drums::pieceInfo (l).name) + (sampled ? " *" : ""), x0 + 10, y, nameWidth - 14, laneHeight,
                        juce::Justification::centredLeft);
        }
        g.setColour (palette::textDim);
        g.setFont (uiFont (11.0f));
        g.drawText ("Velocity", x0 + 10, velTop() - 2, nameWidth - 14, 16, juce::Justification::centredLeft);
    };
    if (! pat.isValid())
    {
        g.setColour (palette::textDim);
        g.setFont (uiFont (15.0f));
        g.drawFittedText ("No patterns yet. Click \"New\" for an empty pattern or \"Generate as new\" for a groove.",
                          getLocalBounds().withTrimmedLeft (nameWidth).withHeight (220), juce::Justification::centred, 2);
        drawNames();
        return;
    }

    const int st = drums::stepTicks (pat);
    const int beatTicks = drums::ppq * 4 / juce::jmax (1, (int) pat[dids::tsDen]);
    const int barTicks = drums::barTicks (pat);
    const int n = steps();
    const float bottom = (float) (headerHeight + lanes * laneHeight);
    const auto clip = g.getClipBounds();
    const int firstStep = juce::jmax (0, (int) std::floor ((clip.getX() - nameWidth) / cellW) - 1);
    const int lastStep = juce::jmin (n, (int) std::ceil ((clip.getRight() - nameWidth) / cellW) + 1);
    for (int s = firstStep; s <= lastStep; ++s)
    {
        const int tick = s * st;
        const float x = (float) nameWidth + cellW * (float) s;
        const bool bar = tick % barTicks == 0, beat = tick % beatTicks == 0;
        g.setColour (bar ? palette::textDim : beat ? palette::outline.brighter (0.4f) : palette::outline);
        g.drawLine (x, (float) headerHeight - (bar ? 6.0f : 0.0f), x, bottom, bar ? 1.6f : 1.0f);
        if (bar && s < n)
        {
            g.setColour (palette::text);
            g.setFont (uiFont (11.0f, true));
            g.drawText (juce::String (tick / barTicks + 1), (int) x + 3, 2, 30, 14, juce::Justification::left);
        }
        else if (beat && s < n)
        {
            g.setColour (palette::textDim);
            g.setFont (uiFont (10.0f));
            g.drawText (juce::String ((tick % barTicks) / beatTicks + 1), (int) x + 3, 3, 20, 13, juce::Justification::left);
        }
    }

    for (auto h : pat)
    {
        const int lane = (int) h[dids::lane];
        if (! juce::isPositiveAndBelow (lane, lanes)) continue;
        const bool isSel = ! selKeys.empty() && selKeys.count (lane * 10000000 + (int) h[dids::tick]) > 0;
        const float vel = juce::jlimit (1, 127, (int) h[dids::vel]) / 127.0f;
        const float x = (float) nameWidth + cellW * (float) (int) h[dids::tick] / (float) st;
        if (x + cellW < (float) clip.getX() || x > (float) clip.getRight()) continue;
        const float y = (float) (headerHeight + lane * laneHeight);
        auto cell = juce::Rectangle<float> (x + 1.5f, y + 2.0f, juce::jmax (4.0f, cellW - 3.0f), (float) laneHeight - 4.0f);
        g.setColour (laneColour (lane).withAlpha (0.25f));
        g.fillRoundedRectangle (cell, 3.0f);
        auto level = cell.withTrimmedTop (cell.getHeight() * (1.0f - vel));
        g.setColour (laneColour (lane).withAlpha (0.55f + 0.45f * vel));
        g.fillRoundedRectangle (level, 3.0f);
        if (vel < 0.4f)
        {   // ghost note marker
            g.setColour (palette::bg);
            g.drawRoundedRectangle (cell.reduced (2.0f), 2.0f, 1.0f);
        }
        if (isSel)
        {
            g.setColour (palette::text);
            g.drawRoundedRectangle (cell.expanded (1.0f), 3.5f, 1.6f);
        }
    }
    {
        // velocity lane under the grid
        const int vt = velTop();
        g.setColour (palette::well);
        g.fillRect (0, vt - 4, (int) gridRight, velLaneHeight + 8);
        g.setColour (palette::outline);
        g.drawHorizontalLine (vt - 4, 0.0f, gridRight);
        for (auto h : pat)
        {
            const int lane = (int) h[dids::lane];
            if (! juce::isPositiveAndBelow (lane, lanes)) continue;
            const float bx = (float) nameWidth + cellW * (float) (int) h[dids::tick] / (float) st + cellW * 0.5f;
            if (bx < (float) clip.getX() - 8.0f || bx > (float) clip.getRight() + 8.0f) continue;
            const float v = juce::jlimit (1, 127, (int) h[dids::vel]) / 127.0f;
            const float bh = v * (float) velLaneHeight;
            const bool isSel = ! selKeys.empty() && selKeys.count (lane * 10000000 + (int) h[dids::tick]) > 0;
            g.setColour (laneColour (lane).withAlpha (isSel ? 1.0f : 0.8f));
            g.fillRect (bx - 2.0f, (float) vt + (float) velLaneHeight - bh, 4.0f, bh);
            g.fillEllipse (bx - 4.0f, (float) vt + (float) velLaneHeight - bh - 4.0f, 8.0f, 8.0f);
            if (isSel) { g.setColour (palette::text); g.drawEllipse (bx - 4.5f, (float) vt + (float) velLaneHeight - bh - 4.5f, 9.0f, 9.0f, 1.2f); }
        }
    }
    if (! marquee.isEmpty())
    {
        g.setColour (palette::accent.withAlpha (0.14f));
        g.fillRect (marquee);
        g.setColour (palette::accent.withAlpha (0.8f));
        g.drawRect (marquee, 1);
    }

    {
        // play-from marker (set by clicking the bar numbers)
        const float sx = (float) nameWidth + cellW * (float) (startQ * drums::ppq / st);
        g.setColour (palette::info.withAlpha (0.8f));
        juce::Path tri;
        tri.addTriangle (sx - 6.0f, 2.0f, sx + 6.0f, 2.0f, sx, (float) headerHeight - 4.0f);
        g.fillPath (tri);
        g.setColour (palette::info.withAlpha (0.35f));
        g.drawLine (sx, (float) headerHeight, sx, bottom, 1.0f);
    }
    if (playQ >= 0.0)
    {
        const float x = (float) nameWidth + cellW * (float) (playQ * drums::ppq / st);
        g.setColour (palette::accent);
        g.drawLine (x, 0.0f, x, bottom, 2.0f);
    }
    drawNames();
}

void DrumGrid::rebuildSelKeys()
{
    selKeys.clear();
    for (auto& h : selected)
        if (h.getParent() == pat) selKeys.insert ((int) h[dids::lane] * 10000000 + (int) h[dids::tick]);
}

void DrumGrid::selectInRect (juce::Rectangle<int> r, bool add)
{
    if (! add) selected.clear();
    const int st = drums::stepTicks (pat);
    for (auto h : pat)
    {
        const int lane = (int) h[dids::lane];
        const float x = (float) nameWidth + cellW * (float) (int) h[dids::tick] / (float) st;
        const juce::Rectangle<float> cell (x, (float) (headerHeight + lane * laneHeight), cellW, (float) laneHeight);
        if (cell.intersects (r.toFloat()) && ! selected.contains (h)) selected.add (h);
    }
    rebuildSelKeys();
}

void DrumGrid::snapshotOrigins()
{
    origins.clear();
    for (auto& h : selected)
        if (h.getParent() == pat) origins.push_back ({ h, (int) h[dids::tick], (int) h[dids::lane] });
}

void DrumGrid::moveSelected (int dTicks, int dLanes)
{
    if (origins.empty() || ! pat.isValid()) return;
    int minT = INT_MAX, maxT = 0, minL = INT_MAX, maxL = 0;
    for (auto& o : origins) { minT = juce::jmin (minT, o.tick); maxT = juce::jmax (maxT, o.tick); minL = juce::jmin (minL, o.lane); maxL = juce::jmax (maxL, o.lane); }
    const int lastTick = drums::lengthTicks (pat) - drums::stepTicks (pat);
    dTicks = juce::jlimit (-minT, juce::jmax (-minT, lastTick - maxT), dTicks);
    dLanes = juce::jlimit (-minL, (int) drums::numPieces - 1 - maxL, dLanes);
    for (auto& o : origins)
    {
        if ((int) o.hit[dids::tick] != o.tick + dTicks) o.hit.setProperty (dids::tick, o.tick + dTicks, &doc.undo());
        if ((int) o.hit[dids::lane] != o.lane + dLanes) o.hit.setProperty (dids::lane, o.lane + dLanes, &doc.undo());
    }
    rebuildSelKeys();
    repaint();
}

void DrumGrid::dropOverlaps()
{
    // a hit moved onto a hit that was not part of the move replaces it
    std::vector<juce::ValueTree> doomed;
    for (auto h : pat)
        if (! selected.contains (h) && selKeys.count ((int) h[dids::lane] * 10000000 + (int) h[dids::tick]) > 0)
            doomed.push_back (h);
    for (auto& h : doomed) pat.removeChild (h, &doc.undo());
}

bool DrumGrid::keyPressed (const juce::KeyPress& k)
{
    if (! pat.isValid()) return false;
    const auto m = k.getModifiers();
    if ((m.isCommandDown() || m.isCtrlDown()) && k.getKeyCode() == 'A')
    {
        selected.clear();
        for (auto h : pat) selected.add (h);
        rebuildSelKeys();
        repaint();
        return true;
    }
    if (selected.isEmpty()) return false;
    if (k == juce::KeyPress::escapeKey) { clearSelection(); selKeys.clear(); return true; }
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
    {
        doc.undo().beginNewTransaction ("Delete drum hits");
        for (auto& h : selected) if (h.getParent() == pat) pat.removeChild (h, &doc.undo());
        selected.clear();
        selKeys.clear();
        repaint();
        return true;
    }
    int dt = 0, dl = 0;
    const int st = drums::stepTicks (pat);
    if (k == juce::KeyPress::leftKey) dt = -st;
    else if (k == juce::KeyPress::rightKey) dt = st;
    else if (k == juce::KeyPress::upKey) dl = -1;
    else if (k == juce::KeyPress::downKey) dl = 1;
    else return false;
    doc.undo().beginNewTransaction ("Move drum hits");
    snapshotOrigins();
    moveSelected (dt, dl);
    dropOverlaps();
    rebuildSelKeys();
    return true;
}

void DrumGrid::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::none;
    marquee = {};
    moved = false;
    grabKeyboardFocus();
    const int nameEdge = juce::jmax (0, -getX()) + nameWidth;
    if (e.y < headerHeight && e.x >= nameEdge && pat.isValid())
    {
        // bar-number strip: set the play position (snapped to the grid; Alt = no snap)
        const double st = drums::stepTicks (pat);
        double ticks = juce::jmax (0.0, ((double) e.x - nameWidth) / cellW * st);
        if (! e.mods.isAltDown()) ticks = std::round (ticks / st) * st;
        ticks = juce::jmin (ticks, (double) drums::lengthTicks (pat) - 1.0);
        if (onSeek) onSeek (ticks / drums::ppq);
        drag = Drag::seek;
        return;
    }
    if (pat.isValid() && e.y >= velTop() - 4 && e.x >= nameEdge)
    {
        // velocity lane: drag a bar to set its hit's velocity (every selected hit follows)
        juce::ValueTree best;
        int bestDist = 1 << 30;
        const int st = drums::stepTicks (pat);
        for (auto h : pat)
        {
            const int bx = nameWidth + (int) (cellW * (float) (int) h[dids::tick] / (float) st + cellW * 0.5f);
            if (std::abs (e.x - bx) > juce::jmax (5, (int) (cellW * 0.5f))) continue;
            const int top = velTop() + velLaneHeight - (int) (juce::jlimit (1, 127, (int) h[dids::vel]) / 127.0f * velLaneHeight);
            const int d = std::abs (e.y - top);
            if (d < bestDist) { bestDist = d; best = h; }
        }
        if (! best.isValid()) return;
        if (selKeys.count ((int) best[dids::lane] * 10000000 + (int) best[dids::tick]) == 0)
        {
            if (! e.mods.isShiftDown()) selected.clear();
            selected.add (best);
            rebuildSelKeys();
        }
        doc.undo().beginNewTransaction ("Drum velocity");
        velOrigins.clear();
        for (auto& h : selected) if (h.getParent() == pat) velOrigins.push_back ({ h, (int) h[dids::vel], 0 });
        dragHit = best;
        dragStartVel = (int) best[dids::vel];
        drag = Drag::velocity;
        beginDragAutoRepeat (25);
        mouseDrag (e);
        return;
    }
    const int lane = laneAt (e.y);
    if (! juce::isPositiveAndBelow (lane, (int) drums::numPieces) || e.y < headerHeight) return;
    if (e.x < nameEdge)
    {
        if (e.mods.isPopupMenu()) { if (onLaneMenu) onLaneMenu (lane); }
        else host.engine.previewDrum (lane, 0.9f);
        return;
    }
    if (! pat.isValid()) return;
    const int step = stepAt (e.x);
    if (! juce::isPositiveAndBelow (step, steps())) return;
    auto hit = hitInCell (lane, step);

    if (e.mods.isPopupMenu())
    {
        if (! hit.isValid()) return;
        juce::PopupMenu m;
        m.addItem (1, "Ghost (40)");
        m.addItem (2, "Soft (70)");
        m.addItem (3, "Normal (100)");
        m.addItem (4, "Accent (127)");
        m.addSeparator();
        m.addItem (5, "Delete");
        juce::Component::SafePointer<DrumGrid> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, hit] (int r) mutable {
            if (safe == nullptr || r == 0) return;
            safe->doc.undo().beginNewTransaction ("Drum velocity");
            if (r == 5) hit.getParent().removeChild (hit, &safe->doc.undo());
            else hit.setProperty (dids::vel, r == 1 ? 40 : r == 2 ? 70 : r == 3 ? 100 : 127, &safe->doc.undo());
        });
        return;
    }

    beginDragAutoRepeat (25);
    const bool hitSelected = hit.isValid() && selKeys.count (lane * 10000000 + (int) hit[dids::tick]) > 0;
    if (e.getNumberOfClicks() >= 2)
    {
        // double-click: add a hit on an empty cell (Shift = ghost, Alt = accent), delete the one under the pointer
        if (hit.isValid())
        {
            doc.undo().beginNewTransaction ("Delete drum hit");
            selected.removeFirstMatchingValue (hit);
            hit.getParent().removeChild (hit, &doc.undo());
        }
        else
        {
            paintVel = e.mods.isShiftDown() ? 40 : e.mods.isAltDown() ? 127 : 100;
            doc.undo().beginNewTransaction ("Add drum hit");
            drums::setHit (pat, lane, step * drums::stepTicks (pat), paintVel, &doc.undo());
            host.engine.previewDrum (lane, paintVel / 127.0f);
        }
        rebuildSelKeys();
        repaint();
        return;
    }
    if (hit.isValid())
    {
        if (e.mods.isShiftDown())   // Shift-click toggles a hit in the selection
        {
            if (hitSelected) selected.removeFirstMatchingValue (hit); else selected.add (hit);
            rebuildSelKeys();
            repaint();
            return;
        }
        if (! hitSelected) { selected.clear(); selected.add (hit); rebuildSelKeys(); }
        host.engine.previewDrum (lane, juce::jlimit (1, 127, (int) hit[dids::vel]) / 127.0f);
        doc.undo().beginNewTransaction ("Move drum hits");
        snapshotOrigins();
        dragHit = hit;
        drag = Drag::move;
        repaint();
        return;
    }
    // empty space: a selection box (Shift adds to the selection)
    marqueeBase = e.mods.isShiftDown() ? selected : juce::Array<juce::ValueTree>();
    selected = marqueeBase;
    rebuildSelKeys();
    marquee = { e.getPosition(), e.getPosition() };
    drag = Drag::marquee;
    repaint();
}

void DrumGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 3) moved = true;
    if (drag == Drag::seek && pat.isValid())
    {
        const double st = drums::stepTicks (pat);
        double ticks = juce::jlimit (0.0, (double) drums::lengthTicks (pat) - 1.0, ((double) e.x - nameWidth) / cellW * st);
        if (! e.mods.isAltDown()) ticks = std::round (ticks / st) * st;
        if (onSeek) onSeek (ticks / drums::ppq);
        return;
    }
    if (drag == Drag::marquee)
    {
        if (auto* vp = findParentComponentOfClass<juce::Viewport>()) vp->autoScroll (e.getEventRelativeTo (vp).x, e.getEventRelativeTo (vp).y, 30, 14);
        marquee = juce::Rectangle<int>::leftTopRightBottom (juce::jmin (e.getMouseDownX(), e.x), juce::jmin (e.getMouseDownY(), e.y),
                                                             juce::jmax (e.getMouseDownX(), e.x), juce::jmax (e.getMouseDownY(), e.y));
        selected = marqueeBase;
        selectInRect (marquee, true);
        repaint();
        return;
    }
    if (drag == Drag::move)
    {
        if (auto* vp = findParentComponentOfClass<juce::Viewport>()) vp->autoScroll (e.getEventRelativeTo (vp).x, e.getEventRelativeTo (vp).y, 30, 14);
        if (! moved) return;
        const int st = drums::stepTicks (pat);
        moveSelected ((int) std::lround (e.getDistanceFromDragStartX() / cellW) * st, (int) std::lround ((double) e.getDistanceFromDragStartY() / laneHeight));
        return;
    }
    if (drag == Drag::velocity && dragHit.isValid())
    {
        const int v = juce::jlimit (1, 127, (int) std::lround ((double) (velTop() + velLaneHeight - e.y) / velLaneHeight * 127.0));
        const int delta = v - dragStartVel;
        for (auto& o : velOrigins)
        {
            const int nv = o.hit == dragHit ? v : juce::jlimit (1, 127, o.tick + delta);   // tick holds the original velocity here
            if ((int) o.hit[dids::vel] != nv) o.hit.setProperty (dids::vel, nv, &doc.undo());
        }
        repaint();
    }
}

void DrumGrid::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::marquee) { marquee = {}; repaint(); }
    if (drag == Drag::move && moved) { dropOverlaps(); rebuildSelKeys(); repaint(); }
    drag = Drag::none;
    dragHit = {};
}

// ================================================================== song strip

SongStrip::SongStrip (ProjectDocument& d) : doc (d) {}

void SongStrip::paint (juce::Graphics& g)
{
    g.fillAll (palette::panel);
    blocks.clear();
    auto songTree = drums::song (drums);
    if (! songTree.isValid() || songTree.getNumChildren() == 0)
    {
        g.setColour (palette::textDim);
        g.setFont (uiFont (13.0f));
        g.drawText ("Song arrangement is empty: select a pattern and click \"+ Add pattern\". Sections play left to right.",
                    getLocalBounds().reduced (10, 0), juce::Justification::centredLeft);
        return;
    }
    double totalQ = 0.0;
    std::vector<double> lens;
    for (auto s : songTree)
    {
        auto p = drums::patternById (drums, s[dids::pattern].toString());
        const double q = p.isValid() ? drums::lengthTicks (p) / (double) drums::ppq * (int) s.getProperty (dids::repeats, 1) : 4.0;
        lens.push_back (q);
        totalQ += q;
    }
    const float w = (float) getWidth() - 12.0f;
    const float minW = 70.0f;
    float scale = w / (float) juce::jmax (1.0, totalQ);
    float x = 6.0f;
    int i = 0;
    for (auto s : songTree)
    {
        auto p = drums::patternById (drums, s[dids::pattern].toString());
        const float bw = juce::jmax (minW, (float) lens[(size_t) i] * scale);
        auto r = juce::Rectangle<float> (x, 6.0f, bw - 4.0f, (float) getHeight() - 12.0f);
        blocks.push_back (r);
        g.setColour (i == selected ? palette::accent.withAlpha (0.45f) : palette::well);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (i == selected ? palette::accent : palette::outline.brighter (0.3f));
        g.drawRoundedRectangle (r, 4.0f, 1.2f);
        g.setColour (p.isValid() ? palette::text : palette::bad);
        g.setFont (uiFont (12.0f, true));
        g.drawText (p.isValid() ? p[dids::name].toString() : juce::String ("(missing)"), r.reduced (6.0f, 4.0f).removeFromTop (16.0f),
                    juce::Justification::left);
        g.setColour (palette::textDim);
        g.setFont (uiFont (11.0f));
        g.drawText ("x" + juce::String ((int) s.getProperty (dids::repeats, 1)), r.reduced (6.0f, 4.0f).removeFromBottom (14.0f),
                    juce::Justification::left);
        x += bw;
        ++i;
    }
    if (playQ >= 0.0)
    {
        // playhead: find the block and the fraction inside it
        double acc = 0.0;
        for (size_t k = 0; k < lens.size() && k < blocks.size(); ++k)
        {
            if (playQ < acc + lens[k])
            {
                const float px = blocks[k].getX() + blocks[k].getWidth() * (float) ((playQ - acc) / lens[k]);
                g.setColour (palette::accent);
                g.drawLine (px, 2.0f, px, (float) getHeight() - 2.0f, 2.0f);
                break;
            }
            acc += lens[k];
        }
    }
}

void SongStrip::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < (int) blocks.size(); ++i)
        if (blocks[(size_t) i].contains (e.position))
        {
            selected = i;
            if (onSelect) onSelect (i);
            repaint();
            return;
        }
}

// ================================================================== page

DrumPage::DrumPage (ProjectDocument& d, EngineHost& h, SessionController& s, juce::PropertiesFile& st)
    : doc (d), host (h), session (s), settings (st), grid (d, h), strip (d)
{
    grid.onLaneMenu = [this] (int lane) { showLaneMenu (lane); };
    grid.onSeek = [this] (double q) { seekTo (q); };
    grid.laneHasSample = [this] (int lane) { return effectiveSample (lane) != juce::File(); };
    viewport.setViewedComponent (&grid, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    addAndMakeVisible (strip);

    patternCombo.setTooltip ("Pattern to edit");
    patternCombo.onChange = [this] {
        if (updating || patternCombo.getSelectedItemIndex() < 0) return;
        selectPattern (drums::patterns (drums).getChild (patternCombo.getSelectedItemIndex())[dids::id].toString());
    };
    newButton.setTooltip ("New empty pattern (meter and length of the current one)");
    newButton.onClick = [this] {
        auto cur = current();
        doc.undo().beginNewTransaction ("New drum pattern");
        auto p = drums::addPattern (drums, "Pattern " + juce::String (drums::patterns (drums).getNumChildren() + 1),
                                    cur.isValid() ? (int) cur[dids::tsNum] : 4, cur.isValid() ? (int) cur[dids::tsDen] : 4,
                                    cur.isValid() ? (int) cur[dids::bars] : 1, &doc.undo());
        selectPattern (p[dids::id].toString());
    };
    dupButton.onClick = [this] {
        auto cur = current();
        if (! cur.isValid()) return;
        auto c = cur.createCopy();
        c.setProperty (dids::id, timeline::newId(), nullptr);
        c.setProperty (dids::name, cur[dids::name].toString() + " copy", nullptr);
        doc.undo().beginNewTransaction ("Duplicate drum pattern");
        drums::patterns (drums).addChild (c, drums::patterns (drums).indexOf (cur) + 1, &doc.undo());
        selectPattern (c[dids::id].toString());
    };
    renameButton.onClick = [this] {
        auto cur = current();
        if (! cur.isValid()) return;
        auto* w = new juce::AlertWindow ("Rename pattern", {}, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", cur[dids::name].toString());
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<DrumPage> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, cur] (int r) {
            const auto n = w->getTextEditorContents ("name").trim();
            if (safe == nullptr || r != 1 || n.isEmpty()) return;
            safe->doc.undo().beginNewTransaction ("Rename pattern");
            safe->doc.set (cur, dids::name, n);
        }), true);
    };
    kitButton.setTooltip ("Drum sounds: load your own samples (one per drum or a whole folder) and set a default kit");
    kitButton.onClick = [this] { showKitMenu(); };
    mixerButton.setTooltip ("Volume, pan, mute and solo for each drum");
    mixerButton.setClickingTogglesState (true);
    mixerButton.setColour (juce::TextButton::buttonOnColourId, palette::accent);
    mixerButton.onClick = [this] { mixer.setVisible (mixerButton.getToggleState()); viewport.setVisible (! mixerButton.getToggleState()); };
    addAndMakeVisible (mixerButton);
    addChildComponent (mixer);
    instrumentButton.setTooltip ("Play the drums through a plugin instrument (a drum VST3 / LV2) instead of the built-in kit");
    instrumentButton.onClick = [this] { session.showDrumInstrumentMenu (&instrumentButton); };
    addAndMakeVisible (instrumentButton);
    addAndMakeVisible (kitButton);
    deleteButton.setTooltip ("Delete this pattern, unused patterns, all patterns, or clear the song");
    deleteButton.onClick = [this] {
        juce::PopupMenu m;
        m.addItem (1, "Delete this pattern", current().isValid());
        m.addItem (2, "Delete patterns not used in the song", drums::patterns (drums).getNumChildren() > 0);
        m.addItem (3, "Delete all patterns (and clear the song)", drums::patterns (drums).getNumChildren() > 0);
        m.addSeparator();
        m.addItem (4, "Clear the song arrangement", drums::song (drums).getNumChildren() > 0);
        juce::Component::SafePointer<DrumPage> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&deleteButton), [safe] (int r) {
            if (safe == nullptr || r == 0) return;
            auto& um = safe->doc.undo();
            auto pats = drums::patterns (safe->drums);
            auto songTree = drums::song (safe->drums);
            if (r == 1)
            {
                auto cur = safe->current();
                um.beginNewTransaction ("Delete drum pattern");
                const auto id = cur[dids::id].toString();
                for (int i = songTree.getNumChildren(); --i >= 0;)
                    if (songTree.getChild (i)[dids::pattern].toString() == id) songTree.removeChild (i, &um);
                const int idx = pats.indexOf (cur);
                pats.removeChild (cur, &um);
                auto next = pats.getChild (juce::jmax (0, idx - 1));
                safe->selectPattern (next.isValid() ? next[dids::id].toString() : juce::String());
            }
            else if (r == 2)
            {
                juce::StringArray used;
                for (auto sec : songTree) used.add (sec[dids::pattern].toString());
                um.beginNewTransaction ("Delete unused drum patterns");
                for (int i = pats.getNumChildren(); --i >= 0;)
                    if (! used.contains (pats.getChild (i)[dids::id].toString())) pats.removeChild (i, &um);
                safe->selectPattern (pats.getChild (0)[dids::id].toString());
            }
            else if (r == 3)
            {
                um.beginNewTransaction ("Delete all drum patterns");
                songTree.removeAllChildren (&um);
                pats.removeAllChildren (&um);
                safe->selectPattern ({});
            }
            else if (r == 4)
            {
                um.beginNewTransaction ("Clear drum song");
                songTree.removeAllChildren (&um);
            }
            safe->strip.selected = -1;
            safe->refresh();
        });
    };

    for (int i = 0; i < (int) std::size (sigNames); ++i) sigCombo.addItem (sigNames[i], i + 1);
    sigCombo.setTooltip ("Time signature of the pattern");
    sigCombo.onChange = [this] {
        auto cur = current();
        if (updating || ! cur.isValid()) return;
        const auto t = juce::StringArray::fromTokens (sigCombo.getText(), "/", "");
        doc.undo().beginNewTransaction ("Pattern meter");
        doc.set (cur, dids::tsNum, t[0].getIntValue());
        doc.set (cur, dids::tsDen, t[1].getIntValue());
        drums::setBars (cur, (int) cur[dids::bars], &doc.undo());  // drop hits past the new end
    };
    for (int b : { 1, 2, 4, 8, 16 }) barsCombo.addItem (juce::String (b) + (b == 1 ? " bar" : " bars"), b);
    barsCombo.setTooltip ("Pattern length");
    barsCombo.onChange = [this] {
        auto cur = current();
        if (updating || ! cur.isValid()) return;
        doc.undo().beginNewTransaction ("Pattern length");
        drums::setBars (cur, barsCombo.getSelectedId(), &doc.undo());
    };
    for (auto& [n, name] : grids) gridCombo.addItem (name, n);
    gridCombo.setTooltip ("Editing grid (hits keep their exact position when you change it)");
    gridCombo.onChange = [this] { if (! updating && current().isValid()) doc.set (current(), dids::grid, gridCombo.getSelectedId(), false); };

    auto setupSlider = [] (juce::Slider& sl, double lo, double hi, double step, const juce::String& suffix) {
        sl.setSliderStyle (juce::Slider::LinearBar);
        sl.setRange (lo, hi, step);
        sl.setTextValueSuffix (suffix);
        sl.setColour (juce::Slider::trackColourId, palette::well);
    };
    setupSlider (swingSlider, 0.0, 50.0, 1.0, "% swing");
    swingSlider.setTooltip ("Swing: delays off-beat 8ths (or 16ths on a 16th grid)");
    swingSlider.onValueChange = [this] { if (! updating && current().isValid()) doc.set (current(), dids::swing, swingSlider.getValue() / 100.0, false); };
    setupSlider (tempoSlider, 20.0, 400.0, 1.0, " BPM");
    tempoSlider.setTooltip ("Project tempo (shared with the Editor)");
    tempoSlider.onDragStart = [this] { doc.undo().beginNewTransaction ("Tempo"); };
    tempoSlider.onValueChange = [this] { if (! updating) doc.setTempo (tempoSlider.getValue()); };

    backButton.setTooltip ("Back to the start of the pattern (Home)");
    backButton.onClick = [this] { goToStart(); };
    patternModeButton.setRadioGroupId (7101);
    songModeButton.setRadioGroupId (7101);
    patternModeButton.setClickingTogglesState (true);
    songModeButton.setClickingTogglesState (true);
    patternModeButton.setToggleState (! songMode, juce::dontSendNotification);
    songModeButton.setToggleState (songMode, juce::dontSendNotification);
    patternModeButton.setTooltip ("The play button plays the selected pattern");
    songModeButton.setTooltip ("The play button plays the whole song arrangement");
    patternModeButton.onClick = [this] { if (patternModeButton.getToggleState()) { songMode = false; if (host.engine.isPlaying()) host.engine.stop(); rebuildSequence(); pushEngine(); } };
    songModeButton.onClick = [this] { if (songModeButton.getToggleState()) { songMode = true; if (host.engine.isPlaying()) host.engine.stop(); rebuildSequence(); pushEngine(); } };
    loopButton.setClickingTogglesState (true);
    loopButton.setToggleState (true, juce::dontSendNotification);
    loopButton.setTooltip ("Loop the pattern / song");
    loopButton.onClick = [this] { pushEngine(); };
    clickButton.setClickingTogglesState (true);
    clickButton.setToggleState (settings.getBoolValue ("drumClick", false), juce::dontSendNotification);
    clickButton.onClick = [this] { settings.setValue ("drumClick", clickButton.getToggleState()); pushEngine(); };

    for (int g = 0; g < drums::numGenres; ++g) genreCombo.addItem (drums::genreName (g), g + 1);
    genreCombo.setSelectedId (settings.getIntValue ("drumGenre", 1), juce::dontSendNotification);
    genreCombo.onChange = [this] { settings.setValue ("drumGenre", genreCombo.getSelectedId()); };
    genreCombo.setTooltip ("Style for the generator");
    for (auto* b : { &halfButton, &fillButton, &crashButton }) b->setClickingTogglesState (true);
    fillButton.setToggleState (true, juce::dontSendNotification);
    crashButton.setToggleState (true, juce::dontSendNotification);
    halfButton.setTooltip ("Half-time feel (backbeat on 3)");
    fillButton.setTooltip ("Put a fill at the end of the last bar");
    crashButton.setTooltip ("Crash cymbal on the first beat");
    setupSlider (complexitySlider, 0.0, 100.0, 1.0, "% busy");
    complexitySlider.setValue (50.0, juce::dontSendNotification);
    complexitySlider.setTooltip ("Complexity: syncopation, ghost notes, open hats, kick density, blast variations");
    generateButton.setTooltip ("Replace the current pattern with a generated groove (each click gives a new variation; undoable)");
    generateButton.onClick = [this] { generate (false); };
    generateNewButton.setTooltip ("Generate a groove into a new pattern");
    generateNewButton.onClick = [this] { generate (true); };

    setupSlider (humanTimeSlider, 0.0, 30.0, 0.5, " ms");
    humanTimeSlider.setTooltip ("Humanize timing: random push/pull of each hit (same every playback)");
    humanTimeSlider.onValueChange = [this] { if (! updating) doc.set (drums, dids::humanizeMs, humanTimeSlider.getValue(), false); };
    setupSlider (humanVelSlider, 0.0, 50.0, 1.0, "% vel");
    humanVelSlider.setTooltip ("Humanize velocity: random variation of each hit's strength");
    humanVelSlider.onValueChange = [this] { if (! updating) doc.set (drums, dids::humanizeVel, humanVelSlider.getValue() / 100.0, false); };
    setupSlider (volumeSlider, 0.0, 150.0, 1.0, "%");
    volumeSlider.setTooltip ("Drum kit volume");
    volumeSlider.onValueChange = [this] { if (! updating) doc.set (drums, dids::volume, volumeSlider.getValue() / 100.0, false); };

    importButton.setTooltip ("Import drums from a MIDI or Guitar Pro file as one pattern containing the whole song");
    importButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Import drums (MIDI or Guitar Pro)", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                       "*.mid;*.midi;*.gp3;*.gp4;*.gp5;*.gpx;*.gp");
        juce::Component::SafePointer<DrumPage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto file = fc.getResult();
            safe->doc.undo().beginNewTransaction ("Import drums");
            drums::MidiImport r;
            if (tab::gp::isGuitarProFile (file))
            {
                tab::gp::Song song;
                auto read = tab::gp::readFile (file, song);
                if (read.failed()) { safe->message ("Import failed", read.getErrorMessage()); return; }
                juce::StringArray warnings;
                auto bars = tab::gp::drumBars (song, warnings);
                bool any = false;
                for (auto& b : bars) any = any || ! b.hits.empty();
                if (! any) { safe->message ("Import failed", file.getFileName() + " has no drum track with notes."); return; }
                r = drums::importAsOnePattern (safe->drums, bars, (song.title.isNotEmpty() ? song.title : file.getFileNameWithoutExtension()) + " drums",
                                               false, &safe->doc.undo());
                warnings.addArray (r.warnings);
                r.warnings = warnings;
                r.tempo = song.tempo;
                bool tempoChanges = false;
                for (auto& mb : song.masterBars) tempoChanges = tempoChanges || mb.tempo > 0.0;
                if (tempoChanges)
                    r.warnings.add ("The song changes tempo; MIDI Studio plays it at " + juce::String (song.tempo, 1)
                                    + " BPM. Play it from Guitar Tabs (Drums on) to follow the tempo changes.");
            }
            else
                r = drums::importMidi (safe->drums, file, &safe->doc.undo());
            if (r.result.failed()) { safe->message ("Import failed", r.result.getErrorMessage()); return; }
            if (r.tempo > 0.0) safe->doc.setTempo (r.tempo);
            safe->selectPattern (r.firstPatternId);
            juce::String msg;
            msg << "Imported the whole file as one pattern: " << r.bars << " bars, " << r.hits << " hits.";
            if (r.tempo > 0.0) msg << "\nTempo set to " << juce::String (r.tempo, 1) << " BPM.";
            msg << "\nPress \"Play pattern\" to hear it; scroll the grid sideways to see all of it.";
            if (! r.warnings.isEmpty()) msg << "\n\n" << r.warnings.joinIntoString ("\n");
            safe->message ("Import complete", msg);
        });
    };
    exportButton.setTooltip ("Export the current pattern or the whole song as MIDI: to the Editor (a new drum track you can edit "
                             "in the piano roll) or to a General MIDI drum file");
    exportButton.onClick = [this] {
        juce::PopupMenu m;
        const bool hasPattern = current().isValid(), hasSong = drums::song (drums).getNumChildren() > 0;
        m.addSectionHeader ("To the Editor (new MIDI drum track)");
        m.addItem (3, "Current pattern", hasPattern);
        m.addItem (4, "Whole song", hasSong);
        m.addSectionHeader ("To a MIDI file");
        m.addItem (1, "Current pattern...", hasPattern);
        m.addItem (2, "Whole song...", hasSong);
        juce::Component::SafePointer<DrumPage> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [safe] (int r) {
            if (safe == nullptr || r == 0) return;
            if (r == 3) { safe->midiToEditor (safe->current()); return; }
            if (r == 4) { safe->midiToEditor ({}); return; }
            const auto pat = r == 1 ? safe->current() : juce::ValueTree();
            const auto name = juce::File::createLegalFileName (r == 1 ? pat[dids::name].toString() : juce::String ("Drum song")) + ".mid";
            safe->chooser = std::make_unique<juce::FileChooser> ("Export MIDI drums",
                juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (name), "*.mid");
            safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                            | juce::FileBrowserComponent::warnAboutOverwriting,
                                        [safe, pat] (const juce::FileChooser& fc) {
                                            if (safe == nullptr || fc.getResult() == juce::File()) return;
                                            const auto f = fc.getResult().withFileExtension (".mid");
                                            auto res = drums::exportMidi (safe->drums, pat, safe->doc.tempo(), f);
                                            safe->message (res.wasOk() ? "Export complete" : "Export failed", res.wasOk() ? "Wrote " + f.getFullPathName() : res.getErrorMessage());
                                        });
        });
    };
    renderButton.setTooltip ("Render the song (or the current pattern) to audio and add it as a new track in the Editor, starting at bar 1");
    renderButton.onClick = [this] { renderToStudio(); };

    addSectionButton.setTooltip ("Append the current pattern to the song (after the selected section)");
    addSectionButton.onClick = [this] {
        auto cur = current();
        if (! cur.isValid()) return;
        doc.undo().beginNewTransaction ("Add song section");
        const int at = strip.selected >= 0 ? strip.selected + 1 : -1;
        drums::addSection (drums, cur[dids::id].toString(), 1, at, &doc.undo());
        strip.selected = at >= 0 ? at : drums::song (drums).getNumChildren() - 1;
    };
    removeSectionButton.onClick = [this] {
        auto songTree = drums::song (drums);
        if (! juce::isPositiveAndBelow (strip.selected, songTree.getNumChildren())) return;
        doc.undo().beginNewTransaction ("Remove song section");
        songTree.removeChild (strip.selected, &doc.undo());
        strip.selected = juce::jmin (strip.selected, songTree.getNumChildren() - 1);
    };
    auto move = [this] (int dir) {
        auto songTree = drums::song (drums);
        const int to = strip.selected + dir;
        if (! juce::isPositiveAndBelow (strip.selected, songTree.getNumChildren()) || ! juce::isPositiveAndBelow (to, songTree.getNumChildren())) return;
        doc.undo().beginNewTransaction ("Move song section");
        songTree.moveChild (strip.selected, to, &doc.undo());
        strip.selected = to;
    };
    leftButton.onClick = [move] { move (-1); };
    rightButton.onClick = [move] { move (1); };
    auto repeats = [this] (int delta) {
        auto sec = drums::song (drums).getChild (strip.selected);
        if (! sec.isValid()) return;
        doc.undo().beginNewTransaction ("Section repeats");
        doc.set (sec, dids::repeats, juce::jlimit (1, 64, (int) sec.getProperty (dids::repeats, 1) + delta));
    };
    lessButton.onClick = [repeats] { repeats (-1); };
    moreButton.onClick = [repeats] { repeats (1); };
    lessButton.setTooltip ("Fewer repeats of the selected section");
    moreButton.setTooltip ("More repeats of the selected section");
    strip.onSelect = [this] (int i) {
        auto sec = drums::song (drums).getChild (i);
        if (sec.isValid()) selectPattern (sec[dids::pattern].toString());
    };

    for (auto* l : { &swingLabel, &tempoLabel, &complexityLabel, &humanLabel, &volumeLabel, &songLabel })
        styleLabel (*l, 12.0f, palette::textDim, true);
    humanLabel.setText ("Humanize", juce::dontSendNotification);
    volumeLabel.setText ("Kit", juce::dontSendNotification);
    styleLabel (status, 12.0f);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &patternCombo, &newButton, &dupButton, &renameButton, &deleteButton, &sigCombo, &barsCombo, &gridCombo, &swingSlider,
             &tempoSlider, &backButton, &patternModeButton, &songModeButton, &loopButton, &clickButton, &genreCombo, &halfButton, &complexitySlider,
             &fillButton, &crashButton, &generateButton, &generateNewButton, &humanLabel, &humanTimeSlider, &humanVelSlider,
             &volumeLabel, &volumeSlider, &importButton, &exportButton, &renderButton, &addSectionButton, &removeSectionButton,
             &leftButton, &rightButton, &lessButton, &moreButton, &songLabel, &status })
        addAndMakeVisible (c);

    rebind();
    startTimerHz (30);
}

DrumPage::~DrumPage()
{
    root.removeListener (this);
}

void DrumPage::rebind()
{
    root.removeListener (this);
    root = doc.tree();
    drums = drums::getOrCreate (root);
    root.addListener (this);
    strip.setDrums (drums);
    strip.selected = -1;
    refresh();
    rebuildSequence();
    reloadKit (true);
    mixer.setDrums (drums);
    pushMix();
}

void DrumPage::pushMix()
{
    float g[drums::numPieces], p[drums::numPieces];
    drums::effectiveMix (drums, g, p);
    auto& synth = host.engine.drums();
    for (int i = 0; i < drums::numPieces; ++i) { synth.pieceGain[(size_t) i] = g[i]; synth.piecePan[(size_t) i] = p[i]; }
}

juce::ValueTree DrumPage::current() const
{
    auto p = drums::patternById (drums, drums[dids::selected].toString());
    return p.isValid() ? p : drums::patterns (drums).getChild (0);
}

double DrumPage::songOffsetQ() const
{
    const auto id = current()[dids::id].toString();
    double acc = 0.0, first = -1.0;
    int i = 0;
    for (auto s : drums::song (drums))
    {
        auto p = drums::patternById (drums, s[dids::pattern].toString());
        if (p.isValid())
        {
            if (s[dids::pattern].toString() == id)
            {
                if (i == strip.selected) return acc;
                if (first < 0.0) first = acc;
            }
            acc += drums::lengthTicks (p) / (double) drums::ppq * juce::jlimit (1, 64, (int) s.getProperty (dids::repeats, 1));
        }
        ++i;
    }
    return juce::jmax (0.0, first);
}

void DrumPage::seekTo (double q)
{
    grid.setStartQ (q);
    seekSet = true;
    auto& e = host.engine;
    if (! e.isPlaying()) return;
    const double spq = 60.0 / juce::jmax (10.0, doc.tempo());
    e.locateSeconds ((songMode ? songOffsetQ() + q : q) * spq);   // jump while playing
}

void DrumPage::selectPattern (const juce::String& id)
{
    if (id != drums[dids::selected].toString()) { grid.setStartQ (0.0); seekSet = false; }
    drums.setProperty (dids::selected, id, nullptr);   // view state: not an undoable edit
    refresh();
    if (! host.engine.isPlaying() || ! songMode) rebuildSequence();
}

void DrumPage::refresh()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    auto cur = current();
    patternCombo.clear (juce::dontSendNotification);
    int i = 1;
    for (auto p : drums::patterns (drums))
        patternCombo.addItem (juce::String (i) + ". " + p[dids::name].toString(), i), ++i;
    patternCombo.setSelectedItemIndex (drums::patterns (drums).indexOf (cur), juce::dontSendNotification);
    const bool has = cur.isValid();
    for (auto* c : std::initializer_list<juce::Component*> { &dupButton, &renameButton, &deleteButton, &sigCombo, &barsCombo, &gridCombo,
                                                             &swingSlider, &generateButton, &addSectionButton })
        c->setEnabled (has);
    if (has)
    {
        const auto sig = juce::String ((int) cur[dids::tsNum]) + "/" + juce::String ((int) cur[dids::tsDen]);
        sigCombo.setSelectedId (0, juce::dontSendNotification);
        for (int k = 0; k < sigCombo.getNumItems(); ++k)
            if (sigCombo.getItemText (k) == sig) sigCombo.setSelectedItemIndex (k, juce::dontSendNotification);
        if (sigCombo.getSelectedId() == 0) sigCombo.setText (sig, juce::dontSendNotification);
        barsCombo.setSelectedId ((int) cur[dids::bars], juce::dontSendNotification);
        if (barsCombo.getSelectedId() == 0) barsCombo.setText (juce::String ((int) cur[dids::bars]) + " bars", juce::dontSendNotification);
        gridCombo.setSelectedId ((int) cur.getProperty (dids::grid, 4), juce::dontSendNotification);
        swingSlider.setValue ((double) cur.getProperty (dids::swing, 0.0) * 100.0, juce::dontSendNotification);
    }
    tempoSlider.setValue (doc.tempo(), juce::dontSendNotification);
    humanTimeSlider.setValue ((double) drums.getProperty (dids::humanizeMs, 0.0), juce::dontSendNotification);
    humanVelSlider.setValue ((double) drums.getProperty (dids::humanizeVel, 0.0) * 100.0, juce::dontSendNotification);
    volumeSlider.setValue ((double) drums.getProperty (dids::volume, 0.8) * 100.0, juce::dontSendNotification);

    if (grid.pattern() != cur) grid.setPattern (cur);
    else { grid.updateSize (viewport.getMaximumVisibleWidth()); grid.repaint(); }

    const auto songC = drums::compile (drums, {}, 1);
    const double secs = songC.lengthQ * 60.0 / juce::jmax (10.0, doc.tempo());
    songLabel.setText ("Song: " + juce::String (drums::song (drums).getNumChildren()) + " sections, " + juce::String ((int) songC.bars.size())
                           + " bars, " + juce::String ((int) secs / 60) + ":" + juce::String ((int) secs % 60).paddedLeft ('0', 2),
                       juce::dontSendNotification);
    strip.repaint();
    status.setText (has ? juce::String (cur.getNumChildren()) + " hits" + dotSep() + "double-click: add / delete" + dotSep() + "Shift / Alt double-click: ghost / accent"
                              + dotSep() + "drag a velocity bar: velocity" + dotSep() + "right-click: presets"
                              + dotSep() + "drag empty space: box-select, drag hits to move" + dotSep() + "click the bar numbers to set where playback starts" + dotSep() + "click a name to hear it, right-click to load a sample" + dotSep() + "Space: play / stop"
                        : juce::String(), juce::dontSendNotification);
}

void DrumPage::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& id)
{
    if (id == dids::selected) return;
    if (node.hasType (dids::KIT)) { reloadKit (false); pushMix(); mixer.refresh(); grid.repaint(); return; }
    if (node == drums || node.isAChildOf (drums) || node == doc.transport())
        triggerAsyncUpdate();
    juce::ignoreUnused (id);
}

// ------------------------------------------------------------------ engine

void DrumPage::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive) return;
    active = shouldBeActive;
    auto& e = host.engine;
    if (active)
    {
        if (e.isPlaying()) e.stop();
        e.tabMode = true;
        rebuildSequence();
        pushEngine();
        grid.updateSize (viewport.getMaximumVisibleWidth());
    }
    else
    {
        if (e.isPlaying()) e.stop();
        e.tabMode = false;
        doc.refreshEngine();
    }
}

void DrumPage::rebuildSequence()
{
    if (! active) return;
    auto seq = buildDrumSequence (drums, songMode ? juce::ValueTree() : current(), doc.tempo());
    host.engine.submitTabSequence (std::move (seq));
    pushEngine();
}

void DrumPage::pushEngine()
{
    if (! active) return;
    auto& e = host.engine;
    e.punchEnabled = false;
    e.clickWhilePlaying = clickButton.getToggleState();
    const double spq = 60.0 / juce::jmax (10.0, doc.tempo());
    const auto pat = songMode ? juce::ValueTree() : current();
    const double lenQ = drums::compile (drums, pat, 1).lengthQ;
    e.setTempo (doc.tempo());
    if (pat.isValid()) e.setTimeSignature ((int) pat[dids::tsNum], (int) pat[dids::tsDen]);
    e.loopStartSec = 0.0;
    e.loopEndSec = lenQ * spq;
    e.loopEnabled = loopButton.getToggleState() && lenQ > 0.0;
}

void DrumPage::togglePlay()
{
    if (host.engine.isPlaying()) { host.engine.stop(); return; }
    startPlayback();
}

void DrumPage::startPlayback()
{
    if (songMode)
    {
        if (drums::song (drums).getNumChildren() == 0) { message ("Play song", "Add patterns to the song arrangement first, or switch to Pattern."); return; }
    }
    else if (! current().isValid()) return;
    const double spq = 60.0 / juce::jmax (10.0, doc.tempo());
    host.engine.stop();
    rebuildSequence();
    pushEngine();
    host.engine.locateSeconds (songMode ? (seekSet ? (songOffsetQ() + grid.getStartQ()) * spq : 0.0) : grid.getStartQ() * spq);
    host.engine.play();
}

void DrumPage::goToStart()
{
    grid.setStartQ (0.0);
    seekSet = false;
    host.engine.locateSeconds (0.0);
}

void DrumPage::timerCallback()
{
    // samples are resampled to the device rate: reload when it changes (and retry hand-overs that did not fit)
    if (host.engine.getSampleRate() > 0.0 && (std::abs (host.engine.getSampleRate() - loadedRate) > 0.5 || kitDirty))
        reloadKit (std::abs (host.engine.getSampleRate() - loadedRate) > 0.5);
    if (! active) return;
    auto& e = host.engine;
    const bool playing = e.isPlaying();
    if (! playing)
    {
        grid.setPlayheadQ (-1.0);
        if (strip.playQ >= 0.0) { strip.playQ = -1.0; strip.repaint(); }
        return;
    }
    const double q = e.getPositionSeconds() * juce::jmax (10.0, doc.tempo()) / 60.0;
    auto follow = [this] (double localQ) {
        // keep the playhead in view on long patterns
        const int x = (int) grid.xForQ (localQ);
        const int left = viewport.getViewPositionX(), w = viewport.getViewWidth();
        if (x < left + DrumGrid::nameWidth || x > left + w - 40)
            viewport.setViewPosition (juce::jmax (0, x - DrumGrid::nameWidth - 20), viewport.getViewPositionY());
    };
    if (! songMode)
    {
        const double len = current().isValid() ? drums::lengthTicks (current()) / (double) drums::ppq : 0.0;
        const double local = len > 0.0 ? std::fmod (q, len) : q;
        grid.setPlayheadQ (local);
        follow (local);
        return;
    }
    strip.playQ = q;
    strip.repaint();
    // show the playhead in the grid when the section under it is the pattern being edited
    double acc = 0.0;
    double local = -1.0;
    for (auto s : drums::song (drums))
    {
        auto p = drums::patternById (drums, s[dids::pattern].toString());
        if (! p.isValid()) continue;
        const double len = drums::lengthTicks (p) / (double) drums::ppq;
        const int reps = (int) s.getProperty (dids::repeats, 1);
        if (q < acc + len * reps)
        {
            if (p == current()) local = std::fmod (q - acc, len);
            break;
        }
        acc += len * reps;
    }
    grid.setPlayheadQ (local);
    if (local >= 0.0) follow (local);
}

// ------------------------------------------------------------------ generator & output

void DrumPage::generate (bool asNew)
{
    auto cur = current();
    drums::GenOptions o;
    o.genre = juce::jmax (0, genreCombo.getSelectedId() - 1);
    o.halfTime = halfButton.getToggleState();
    o.complexity = complexitySlider.getValue() / 100.0;
    o.fill = fillButton.getToggleState();
    o.crashOnOne = crashButton.getToggleState();
    o.bars = cur.isValid() ? (int) cur[dids::bars] : 2;
    o.tsNum = cur.isValid() ? (int) cur[dids::tsNum] : 4;
    o.tsDen = cur.isValid() ? (int) cur[dids::tsDen] : 4;
    o.seed = seed++;
    auto g = drums::generate (o);
    if (asNew || ! cur.isValid())
    {
        doc.undo().beginNewTransaction ("Generate drum pattern");
        g.setProperty (dids::name, g[dids::name].toString() + " " + juce::String (drums::patterns (drums).getNumChildren() + 1), nullptr);
        drums::patterns (drums).appendChild (g, &doc.undo());
        selectPattern (g[dids::id].toString());
    }
    else
    {
        doc.undo().beginNewTransaction ("Generate drums");
        drums::replaceHits (cur, g, &doc.undo());
        doc.set (cur, dids::grid, (int) g[dids::grid]);
        doc.set (cur, dids::swing, (double) g[dids::swing]);
    }
    status.setText (juce::String ("Generated ") + drums::genreName (o.genre) + ", variation " + juce::String ((int) o.seed)
                        + " - click Generate again for another one", juce::dontSendNotification);
}

void DrumPage::renderToStudio()
{
    const bool useSong = drums::song (drums).getNumChildren() > 0;
    const auto pat = useSong ? juce::ValueTree() : current();
    if (! useSong && ! pat.isValid()) { message ("Render", "Create a pattern first."); return; }
    const double spq = 60.0 / juce::jmax (10.0, doc.tempo());
    const auto c = drums::compile (drums, pat, 1);
    if (c.events.empty()) { message ("Render", "There are no drum hits to render."); return; }
    std::vector<DrumHit> hits;
    for (auto& e : c.events) hits.push_back ({ juce::jmax (0.0, e.q * spq + e.offsetSec), e.piece, e.velocity });
    std::stable_sort (hits.begin(), hits.end(), [] (const DrumHit& a, const DrumHit& b) { return a.sec < b.sec; });
    const double sr = host.engine.getSampleRate() > 0 ? host.engine.getSampleRate() : 48000.0;
    std::vector<std::unique_ptr<juce::AudioBuffer<float>>> owned;
    DrumSynth::SampleSet set {};
    for (int p = 0; p < drums::numPieces; ++p)
        if (auto f = effectiveSample (p); f != juce::File())
            if (auto b = loadSampleFile (f, sr))
            {
                set[(size_t) p] = b.get();
                owned.push_back (std::move (b));
            }
    juce::AudioBuffer<float> buf;
    const auto mix = host.engine.drums().currentMix();
    DrumSynth::renderOffline (hits, sr, c.lengthQ * spq + 3.0, (float) (double) drums.getProperty (dids::volume, 0.8), buf, &set, &mix);

    const auto name = useSong ? juce::String ("Drums") : "Drums - " + pat[dids::name].toString();
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("RavenyrRender");
    dir.createDirectory();
    auto file = dir.getChildFile (juce::File::createLegalFileName (name) + ".wav");
    file.deleteFile();
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (file.createOutputStream());
        if (os == nullptr) { message ("Render failed", "Could not write a temporary file."); return; }
        auto opts = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24);
        auto writer = wav.createWriterFor (os, opts);
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples()))
        { message ("Render failed", "Could not write the WAV file."); return; }
    }
    auto r = session.importFiles ({ file }, {}, 0.0);
    file.deleteFile();
    if (r.failed()) { message ("Render failed", r.getErrorMessage()); return; }
    message ("Rendered to the Editor", "Added the track \"" + name + "\" (" + juce::String (c.lengthQ * spq, 1)
                                           + " s at " + juce::String (doc.tempo(), 1) + " BPM) to the Editor, starting at bar 1.");
}

void DrumPage::midiToEditor (const juce::ValueTree& pattern)
{
    const auto name = pattern.isValid() ? "Drums - " + pattern[dids::name].toString() : juce::String ("Drums");
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("RavenyrRender");
    dir.createDirectory();
    const auto file = dir.getChildFile (juce::File::createLegalFileName (name) + ".mid");
    if (auto r = drums::exportMidi (drums, pattern, doc.tempo(), file); r.failed()) { message ("Export MIDI to the Editor", r.getErrorMessage()); return; }
    const int before = doc.tracks().getNumChildren();
    const auto r = session.importMidi (file, 0.0);   // a drum instrument track with one MIDI clip, in one undo step
    file.deleteFile();
    if (r.failed()) { message ("Export MIDI to the Editor", r.getErrorMessage()); return; }
    for (int i = before; i < doc.tracks().getNumChildren(); ++i)   // named after the pattern, not the temporary file
    {
        auto t = doc.tracks().getChild (i);
        t.setProperty (timeline::ids::name, name, nullptr);
        for (auto c : t)
            if (c.hasType (timeline::ids::CLIP)) c.setProperty (timeline::ids::name, name, nullptr);
    }
    const auto c = drums::compile (drums, pattern, 1);
    message ("MIDI added to the Editor", "Added the track \"" + name + "\" (" + juce::String ((int) c.bars.size()) + (c.bars.size() == 1 ? " bar" : " bars")
                                             + ") to the Editor, starting at bar 1. It plays the built-in Drum Kit; double-click the "
                                               "clip to edit the notes, or right-click the instrument to use another drum plugin.");
}

// ------------------------------------------------------------------ sample kit

std::unique_ptr<juce::AudioBuffer<float>> DrumPage::loadSampleFile (const juce::File& file, double sampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0) return nullptr;
    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    const int len = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 10.0));
    juce::AudioBuffer<float> raw (channels, len);
    reader->read (&raw, 0, len, 0, true, channels > 1);
    if (std::abs (reader->sampleRate - sampleRate) < 0.5) return std::make_unique<juce::AudioBuffer<float>> (std::move (raw));
    const double ratio = reader->sampleRate / sampleRate;
    const int outLen = (int) std::ceil (len / ratio);
    auto out = std::make_unique<juce::AudioBuffer<float>> (channels, outLen);
    for (int ch = 0; ch < channels; ++ch)
    {
        juce::LagrangeInterpolator interp;
        interp.process (ratio, raw.getReadPointer (ch), out->getWritePointer (ch), outLen, len, 0);
    }
    return out;
}

void DrumPage::reloadKit (bool force)
{
    const double sr = host.engine.getSampleRate();
    if (sr <= 0.0) { kitDirty = true; return; }
    if (force) for (auto& p : loadedPaths) p = "\x01";   // invalidate
    kitDirty = false;
    juce::StringArray missing;
    for (int p = 0; p < drums::numPieces; ++p)
    {
        const auto file = effectiveSample (p);
        const auto rel = file == juce::File() ? juce::String() : file.getFullPathName();
        if (rel == loadedPaths[(size_t) p]) continue;
        std::unique_ptr<juce::AudioBuffer<float>> buf;
        if (rel.isNotEmpty())
        {
            buf = loadSampleFile (file, sr);
            if (buf == nullptr) missing.add (juce::String (drums::pieceInfo (p).name) + ": " + file.getFileName());
        }
        if (! host.engine.setDrumSample (p, std::move (buf))) { kitDirty = true; continue; }  // queue full: retry
        loadedPaths[(size_t) p] = rel;
    }
    loadedRate = sr;
    if (! missing.isEmpty())
        status.setText ("Missing drum samples (built-in sound used): " + missing.joinIntoString ("; "), juce::dontSendNotification);
}

juce::File DrumPage::effectiveSample (int piece) const
{
    const auto rel = drums::samplePath (drums, piece);
    if (rel.isNotEmpty()) return project::Bundle::fromPortablePath (doc.mediaBundle(), rel);
    const auto def = settings.getValue (defaultKitKey (piece));
    return def.isNotEmpty() && juce::File::isAbsolutePath (def) ? juce::File (def) : juce::File();
}

void DrumPage::addKitItems (juce::PopupMenu& m, int lane) const
{
    if (lane >= 0)
    {
        const auto name = juce::String (drums::pieceInfo (lane).name);
        const auto f = effectiveSample (lane);
        const bool own = drums::samplePath (drums, lane).isNotEmpty();
        m.addSectionHeader (name + (f == juce::File() ? juce::String (" - built-in sound")
                                                      : " - sample: " + f.getFileName() + (own ? juce::String() : juce::String (" (default kit)"))));
        m.addItem (1, "Load sample for " + name + "...");
        m.addItem (2, "Use the default sound for " + name, own);
        m.addSeparator();
    }
    else
    {
        juce::PopupMenu pieces;
        for (int p = 0; p < drums::numPieces; ++p)
        {
            const auto f = effectiveSample (p);
            pieces.addItem (100 + p, juce::String (drums::pieceInfo (p).name) + (f == juce::File() ? juce::String ("   (built-in)") : "   = " + f.getFileName()));
        }
        m.addSectionHeader ("Drum kit");
        m.addSubMenu ("Load a sample for one drum", pieces);
    }
    m.addItem (3, "Load kit from folder (matches file names)...");
    m.addItem (4, "Reset this project's kit");
    m.addSeparator();
    m.addItem (5, "Use this kit as the default for all projects");
    bool hasDefault = false;
    for (int p = 0; p < drums::numPieces; ++p) hasDefault = hasDefault || settings.getValue (defaultKitKey (p)).isNotEmpty();
    m.addItem (6, "Clear the default kit (back to built-in sounds)", hasDefault);
}

void DrumPage::showLaneMenu (int lane)
{
    juce::PopupMenu m;
    addKitItems (m, lane);
    juce::Component::SafePointer<DrumPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, lane] (int r) { if (safe != nullptr) safe->handleKitMenu (r, lane); });
}

void DrumPage::showKitMenu()
{
    juce::PopupMenu m;
    addKitItems (m, -1);
    juce::Component::SafePointer<DrumPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitButton), [safe] (int r) { if (safe != nullptr) safe->handleKitMenu (r, -1); });
}

void DrumPage::handleKitMenu (int r, int lane)
{
    if (r == 0) return;
    if (r >= 100) { lane = r - 100; r = 1; }
    juce::Component::SafePointer<DrumPage> safe (this);
    if (r == 1 && lane >= 0)
    {
        chooser = std::make_unique<juce::FileChooser> ("Sample for " + juce::String (drums::pieceInfo (lane).name),
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, lane] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            safe->doc.undo().beginNewTransaction ("Drum sample");
            safe->assignSample (lane, fc.getResult());
        });
    }
    else if (r == 2 && lane >= 0)
    {
        doc.undo().beginNewTransaction ("Default drum sound");
        drums::setSamplePath (drums, lane, {}, &doc.undo());
    }
    else if (r == 3) loadKitFolder();
    else if (r == 4)
    {
        doc.undo().beginNewTransaction ("Reset drum kit");
        for (int p = 0; p < drums::numPieces; ++p) drums::setSamplePath (drums, p, {}, &doc.undo());
    }
    else if (r == 5)
    {
        // copy the samples somewhere that outlives the project
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("RAVENYR").getChildFile ("Default drum kit");
        dir.createDirectory();
        int n = 0;
        for (int p = 0; p < drums::numPieces; ++p)
        {
            const auto f = effectiveSample (p);
            if (f == juce::File() || ! f.existsAsFile()) { settings.removeValue (defaultKitKey (p)); continue; }
            auto dst = dir.getChildFile (f.getFileName());
            if (! f.isAChildOf (dir)) { dst.deleteFile(); f.copyFileTo (dst); }
            settings.setValue (defaultKitKey (p), dst.getFullPathName());
            ++n;
        }
        settings.saveIfNeeded();
        kitDirty = true;
        message ("Drum kit", n == 0 ? juce::String ("This kit only uses built-in sounds, so the default kit is now the built-in one.")
                                    : "New projects (and drums without their own sample) now use these " + juce::String (n) + " samples.");
    }
    else if (r == 6)
    {
        for (int p = 0; p < drums::numPieces; ++p) settings.removeValue (defaultKitKey (p));
        settings.saveIfNeeded();
        kitDirty = true;
    }
    grid.repaint();
}

void DrumPage::assignSample (int piece, const juce::File& file)
{
    auto probe = loadSampleFile (file, 44100.0);
    if (probe == nullptr) { message ("Drum sample", "Could not read " + file.getFileName() + " as audio."); return; }
    project::Bundle::createStructure (doc.mediaBundle());
    auto dir = kitFolder();
    dir.createDirectory();
    auto dst = dir.getChildFile (juce::File::createLegalFileName (juce::String (drums::pieceInfo (piece).name) + " - " + file.getFileName()));
    if (dst != file && ! file.copyFileTo (dst)) { message ("Drum sample", "Could not copy " + file.getFileName() + " into the project."); return; }
    drums::setSamplePath (drums, piece, project::Bundle::toPortablePath (doc.mediaBundle(), dst), &doc.undo());
}

void DrumPage::loadKitFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a folder of drum samples", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    juce::Component::SafePointer<DrumPage> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc) {
        if (safe == nullptr || ! fc.getResult().isDirectory()) return;
        std::array<juce::File, drums::numPieces> chosen;
        juce::StringArray unknown;
        for (const auto& entry : juce::RangedDirectoryIterator (fc.getResult(), true, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3"))
        {
            const int piece = drums::pieceForSampleName (entry.getFile().getFileName());
            if (piece < 0) { unknown.add (entry.getFile().getFileName()); continue; }
            if (chosen[(size_t) piece] == juce::File()) chosen[(size_t) piece] = entry.getFile();
        }
        safe->doc.undo().beginNewTransaction ("Load drum kit");
        juce::StringArray mapped;
        for (int p = 0; p < drums::numPieces; ++p)
            if (chosen[(size_t) p] != juce::File())
            {
                safe->assignSample (p, chosen[(size_t) p]);
                mapped.add (juce::String (drums::pieceInfo (p).name) + " = " + chosen[(size_t) p].getFileName());
            }
        juce::String msg = mapped.isEmpty() ? juce::String ("No file names matched a kit piece (use names like kick, snare, hihat open, tom 1, ride bell...).")
                                            : "Loaded " + juce::String (mapped.size()) + " sounds:\n" + mapped.joinIntoString ("\n");
        if (! unknown.isEmpty())
            msg << "\n\nNot recognised (" << unknown.size() << "): " << unknown.joinIntoString (", ").substring (0, 400)
                << "\nRight-click a drum name to assign a file by hand.";
        safe->message ("Drum kit", msg);
    });
}

void DrumPage::message (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle (title).withMessage (text).withButton ("OK").withAssociatedComponent (this),
                                  nullptr);
}

// ------------------------------------------------------------------ layout

void DrumPage::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    g.setColour (palette::panel);
    g.fillRect (getLocalBounds().removeFromTop (126));
    g.setColour (palette::outline);
    g.drawHorizontalLine (42, 0.0f, (float) getWidth());
    g.drawHorizontalLine (84, 0.0f, (float) getWidth());
    g.drawHorizontalLine (126, 0.0f, (float) getWidth());
}

void DrumPage::resized()
{
    auto r = getLocalBounds();
    // row 1: instrument on the left, the transport-style controls in the middle of the screen, import / export on the right
    auto row0 = r.removeFromTop (42).reduced (8, 7);
    {
        const int groupW = 34 + 10 + 70 + 2 + 58 + 10 + 50 + 4 + 54;
        auto grp = juce::Rectangle<int> (groupW, row0.getHeight()).withCentre ({ getWidth() / 2, row0.getCentreY() });
        backButton.setBounds (grp.removeFromLeft (34)); grp.removeFromLeft (10);
        patternModeButton.setBounds (grp.removeFromLeft (70)); grp.removeFromLeft (2);
        songModeButton.setBounds (grp.removeFromLeft (58)); grp.removeFromLeft (10);
        loopButton.setBounds (grp.removeFromLeft (50)); grp.removeFromLeft (4);
        clickButton.setBounds (grp.removeFromLeft (54));
    }
    row0.removeFromLeft (156);   // room for the Drums | Keys tabs (MidiStudioPage)
    instrumentButton.setBounds (row0.removeFromLeft (108)); row0.removeFromLeft (4);
    mixerButton.setBounds (row0.removeFromLeft (64));
    renderButton.setBounds (row0.removeFromRight (116)); row0.removeFromRight (4);
    exportButton.setBounds (row0.removeFromRight (100)); row0.removeFromRight (2);
    importButton.setBounds (row0.removeFromRight (98));

    // row 2: the generator and humanize
    auto row2 = r.removeFromTop (42).reduced (8, 7);
    genreCombo.setBounds (row2.removeFromLeft (150)); row2.removeFromLeft (4);
    halfButton.setBounds (row2.removeFromLeft (70)); row2.removeFromLeft (2);
    complexitySlider.setBounds (row2.removeFromLeft (92)); row2.removeFromLeft (2);
    fillButton.setBounds (row2.removeFromLeft (40)); row2.removeFromLeft (2);
    crashButton.setBounds (row2.removeFromLeft (76)); row2.removeFromLeft (4);
    generateButton.setBounds (row2.removeFromLeft (76)); row2.removeFromLeft (2);
    generateNewButton.setBounds (row2.removeFromLeft (112)); row2.removeFromLeft (12);
    kitButton.setBounds (row2.removeFromRight (56)); row2.removeFromRight (10);
    humanLabel.setBounds (row2.removeFromLeft (64));
    humanTimeSlider.setBounds (row2.removeFromLeft (juce::jmax (60, juce::jmin (84, row2.getWidth() / 3)))); row2.removeFromLeft (2);
    humanVelSlider.setBounds (row2.removeFromLeft (juce::jmax (60, juce::jmin (84, row2.getWidth() / 2)))); row2.removeFromLeft (6);
    volumeLabel.setBounds (row2.removeFromLeft (24));
    volumeSlider.setBounds (row2.removeFromLeft (juce::jmax (50, juce::jmin (80, row2.getWidth()))));

    // row 3: the pattern
    auto row1 = r.removeFromTop (42).reduced (8, 7);
    patternCombo.setBounds (row1.removeFromLeft (190)); row1.removeFromLeft (4);
    newButton.setBounds (row1.removeFromLeft (48)); row1.removeFromLeft (2);
    dupButton.setBounds (row1.removeFromLeft (72)); row1.removeFromLeft (2);
    renameButton.setBounds (row1.removeFromLeft (62)); row1.removeFromLeft (2);
    deleteButton.setBounds (row1.removeFromLeft (56)); row1.removeFromLeft (10);
    sigCombo.setBounds (row1.removeFromLeft (64)); row1.removeFromLeft (4);
    barsCombo.setBounds (row1.removeFromLeft (76)); row1.removeFromLeft (4);
    gridCombo.setBounds (row1.removeFromLeft (110)); row1.removeFromLeft (4);
    swingSlider.setBounds (row1.removeFromLeft (92)); row1.removeFromLeft (10);
    tempoSlider.setBounds (row1.removeFromLeft (88));

    status.setBounds (r.removeFromBottom (24).reduced (10, 0));
    auto songArea = r.removeFromBottom (96).reduced (8, 4);
    auto songButtons = songArea.removeFromTop (28);
    songLabel.setBounds (songButtons.removeFromLeft (260));
    for (auto* b : { &addSectionButton, &removeSectionButton, &leftButton, &rightButton, &lessButton, &moreButton })
    {
        b->setBounds (songButtons.removeFromLeft (b == &addSectionButton ? 104 : b == &removeSectionButton ? 70 : 32));
        songButtons.removeFromLeft (3);
    }
    strip.setBounds (songArea);
    viewport.setBounds (r.reduced (0, 2));
    mixer.setBounds (r.reduced (0, 2));
    grid.updateSize (viewport.getMaximumVisibleWidth());
}
} // namespace ff
