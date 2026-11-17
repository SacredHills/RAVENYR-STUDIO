#include "KeysPage.h"
#include <algorithm>
#include "core/MidiClip.h"
#include "engine/fx/BuiltinPlugins.h"

namespace ff
{
namespace tl = timeline;

KeysPage::KeysPage (ProjectDocument& d, EngineHost& h, SessionController& s, juce::PropertiesFile& st)
    : doc (d), host (h), session (s), settings (st)
{
    root = doc.tree();
    root.addListener (this);

    for (const char* n : { "Piano", "Synth", "Bass" }) styleCombo.addItem (n, styleCombo.getNumItems() + 1);
    styleCombo.setSelectedId (settings.getIntValue ("keysStyle", 1), juce::dontSendNotification);
    styleCombo.setTooltip ("Piano, Synth or Bass: sets sensible defaults (range and pattern) and names new tracks");
    styleCombo.onChange = [this] { settings.setValue ("keysStyle", styleCombo.getSelectedId()); applyStyleDefaults(); };
    for (const char* n : { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }) keyCombo.addItem (n, keyCombo.getNumItems() + 1);
    keyCombo.setSelectedId (settings.getIntValue ("keysKey", 1), juce::dontSendNotification);
    keyCombo.onChange = [this] { settings.setValue ("keysKey", keyCombo.getSelectedId()); };
    for (int i = 0; i < keys::numScales; ++i) scaleCombo.addItem (keys::scaleName (i), i + 1);
    scaleCombo.setSelectedId (settings.getIntValue ("keysScale", 1), juce::dontSendNotification);
    scaleCombo.onChange = [this] { settings.setValue ("keysScale", scaleCombo.getSelectedId()); };
    for (int i = 0; i < keys::numProgressions(); ++i) progCombo.addItem (keys::progressionName (i), i + 1);
    progCombo.setSelectedId (settings.getIntValue ("keysProg", 1), juce::dontSendNotification);
    progCombo.setTooltip ("Chord progression: one chord per bar, repeated");
    progCombo.onChange = [this] { settings.setValue ("keysProg", progCombo.getSelectedId()); };
    for (int i = 0; i < keys::numStyles; ++i) genCombo.addItem (keys::styleName (i), i + 1);
    genCombo.setSelectedId (settings.getIntValue ("keysGen", 1), juce::dontSendNotification);
    genCombo.onChange = [this] { settings.setValue ("keysGen", genCombo.getSelectedId()); };
    for (int o = 1; o <= 7; ++o) octaveCombo.addItem ("Octave " + juce::String (o), o);
    octaveCombo.setSelectedId (settings.getIntValue ("keysOctave", 4), juce::dontSendNotification);
    octaveCombo.setTooltip ("Where the pattern sits (C4 = middle C; bass patterns play two octaves lower)");
    octaveCombo.onChange = [this] { settings.setValue ("keysOctave", octaveCombo.getSelectedId()); };
    for (int b : { 1, 2, 4, 8, 12, 16 }) barsCombo.addItem (juce::String (b) + (b == 1 ? " bar" : " bars"), b);
    barsCombo.setSelectedId (settings.getIntValue ("keysBars", 4), juce::dontSendNotification);
    barsCombo.onChange = [this] { settings.setValue ("keysBars", barsCombo.getSelectedId()); };

    trackCombo.setTooltip ("The instrument track being edited");
    trackCombo.onChange = [this] {
        if (updating || trackCombo.getSelectedItemIndex() < 0) return;
        track = trackList[(size_t) trackCombo.getSelectedItemIndex()];
        clip = {};
        refresh();
    };
    clipCombo.setTooltip ("The MIDI clip on that track");
    clipCombo.onChange = [this] {
        if (updating || clipCombo.getSelectedItemIndex() < 0) return;
        clip = clipList[(size_t) clipCombo.getSelectedItemIndex()];
        refresh();
    };
    newTrackButton.setTooltip ("A new instrument track with the built-in synth (load a real instrument with Instrument...)");
    newTrackButton.onClick = [this] {
        doc.undo().beginNewTransaction ("New keys track");
        track = makeTrack();
        clip = {};
        refresh();
    };
    instrumentButton.setTooltip ("Choose the instrument (VST3 / LV2 / built-in synth) that plays this part");
    instrumentButton.onClick = [this] { if (track.isValid()) session.showInstrumentContextMenu (track, &instrumentButton); };
    backButton.setTooltip ("Back to the start of the clip (Home)");
    backButton.onClick = [this] { goToStart(); };
    loopButton.setClickingTogglesState (true);
    loopButton.setToggleState (true, juce::dontSendNotification);
    loopButton.setTooltip ("Loop the clip while playing");
    loopButton.onClick = [this] { host.engine.loopEnabled = loopButton.getToggleState(); };
    generateButton.setTooltip ("Fill the clip with a new pattern in the chosen key, scale and progression (one undo step)");
    generateButton.onClick = [this] { generate(); };
    newClipButton.setTooltip ("A new empty clip after the last one on this track");
    newClipButton.onClick = [this] {
        if (! track.isValid()) { doc.undo().beginNewTransaction ("New keys track"); track = makeTrack(); }
        doc.undo().beginNewTransaction ("New MIDI clip");
        clip = makeClip (barsCombo.getSelectedId());
        refresh();
    };
    deleteClipButton.onClick = [this] {
        if (! clip.isValid()) return;
        doc.undo().beginNewTransaction ("Delete MIDI clip");
        clip.getParent().removeChild (clip, &doc.undo());
        clip = {};
        refresh();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &trackCombo, &clipCombo, &styleCombo, &keyCombo, &scaleCombo, &progCombo, &genCombo,
                                                             &octaveCombo, &barsCombo, &newTrackButton, &instrumentButton, &backButton, &loopButton,
                                                             &generateButton, &newClipButton, &deleteClipButton, &hint })
        addAndMakeVisible (c);
    hint.setJustificationType (juce::Justification::centred);
    hint.setColour (juce::Label::textColourId, palette::textDim);
    hint.setFont (uiFont (15.0f));
    refresh();
}

KeysPage::~KeysPage()
{
    root.removeListener (this);
}

void KeysPage::rebind()
{
    root.removeListener (this);
    root = doc.tree();
    root.addListener (this);
    track = {};
    clip = {};
    roll.reset();
    refresh();
}

int KeysPage::tsNum() const { return juce::jmax (1, (int) const_cast<ProjectDocument&> (doc).transport().getProperty (project::ids::tsNum, 4)); }
int KeysPage::tsDen() const { return juce::jmax (1, (int) const_cast<ProjectDocument&> (doc).transport().getProperty (project::ids::tsDen, 4)); }

void KeysPage::applyStyleDefaults()
{
    switch (styleCombo.getSelectedId())
    {
        case 1: genCombo.setSelectedId (keys::blockChords + 1); octaveCombo.setSelectedId (4); break;   // piano
        case 2: genCombo.setSelectedId (keys::arpUp + 1); octaveCombo.setSelectedId (4); break;         // synth
        default: genCombo.setSelectedId (keys::bassWalking + 1); octaveCombo.setSelectedId (4); break;  // bass (plays two octaves lower)
    }
}

void KeysPage::setActive (bool on)
{
    if (active == on) return;
    active = on;
    auto& e = host.engine;
    if (e.isPlaying()) e.stop();
    if (on) refresh();
    else doc.refreshEngine();   // the project's own loop and click settings again
}

void KeysPage::togglePlay()
{
    if (host.engine.isPlaying()) { host.engine.stop(); return; }
    startPlayback();
}

void KeysPage::startPlayback()
{
    if (! clip.isValid()) { message ("Play", "There is no clip yet. Click Generate, or New clip, first."); return; }
    auto& e = host.engine;
    const double s = (double) clip[tl::ids::start], len = juce::jmax (0.25, (double) clip[tl::ids::length]);
    e.stop();
    e.punchEnabled = false;
    e.loopStartSec = s;
    e.loopEndSec = s + len;
    e.loopEnabled = loopButton.getToggleState();
    e.locateSeconds (s);
    e.play();
}

void KeysPage::goToStart()
{
    host.engine.locateSeconds (clip.isValid() ? (double) clip[tl::ids::start] : 0.0);
}

juce::ValueTree KeysPage::makeTrack()
{
    static const char* names[] = { "Piano", "Synth", "Bass" };
    const auto name = juce::String (names[juce::jlimit (0, 2, styleCombo.getSelectedId() - 1)]);
    int same = 0;
    for (auto t : doc.tracks()) if (t[tl::ids::name].toString().startsWith (name)) ++same;
    auto t = session.addTrack (same > 0 ? name + " " + juce::String (same + 1) : name, -1, tl::TrackKind::instrument);
    session.setInstrument (t, fx::builtin::descriptionXml ("fretforge:synth"), "Basic Synth");
    return t;
}

juce::ValueTree KeysPage::makeClip (int bars)
{
    double start = 0.0;
    for (auto c : track) if (midiclip::isMidiClip (c)) start = juce::jmax (start, (double) c[tl::ids::start] + (double) c[tl::ids::length]);
    const auto map = doc.tempoMap();
    const double q0 = map.quarterAtSeconds (start);
    keys::Options bo;
    bo.tsNum = tsNum();
    bo.tsDen = tsDen();
    const double barQ = keys::barQuarters (bo);
    auto c = midiclip::createMidiClip ("Clip " + juce::String (clipList.size() + 1), start, {});
    c.setProperty (tl::ids::length, map.secondsAtQuarter (q0 + bars * barQ) - start, nullptr);
    track.appendChild (c, &doc.undo());
    return c;
}

void KeysPage::generate()
{
    doc.undo().beginNewTransaction ("Generate keys");
    if (! track.isValid()) track = makeTrack();
    const int bars = barsCombo.getSelectedId();
    if (! clip.isValid()) clip = makeClip (bars);
    keys::Options o;
    o.key = keyCombo.getSelectedId() - 1;
    o.scale = scaleCombo.getSelectedId() - 1;
    o.progression = progCombo.getSelectedId() - 1;
    o.style = genCombo.getSelectedId() - 1;
    o.octave = octaveCombo.getSelectedId();
    o.bars = bars;
    o.tsNum = tsNum();
    o.tsDen = tsDen();
    o.seed = seedCounter++ + (int) (juce::Time::getMillisecondCounter() & 0xffff);
    const auto gen = keys::generate (o);
    const auto map = doc.tempoMap();
    const double s0 = (double) clip[tl::ids::start], q0 = map.quarterAtSeconds (s0);
    std::vector<midiclip::Note> notes;
    for (auto& n : gen)
    {
        midiclip::Note m;
        m.start = map.secondsAtQuarter (q0 + n.startQ) - s0;
        m.length = map.secondsAtQuarter (q0 + n.startQ + n.lengthQ) - s0 - m.start;
        m.pitch = n.pitch;
        m.velocity = n.velocity;
        notes.push_back (m);
    }
    midiclip::setNotes (clip, notes, {}, &doc.undo());
    clip.setProperty (tl::ids::length, map.secondsAtQuarter (q0 + bars * keys::barQuarters (o)) - s0, &doc.undo());
    clip.setProperty (tl::ids::offset, 0.0, &doc.undo());
    refresh();
    if (roll != nullptr) roll->fitView();
}

void KeysPage::refresh()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    trackList.clear();
    for (auto t : doc.tracks()) if (tl::isInstrumentTrack (t)) trackList.push_back (t);
    if (! track.isValid() || std::find (trackList.begin(), trackList.end(), track) == trackList.end())
        track = trackList.empty() ? juce::ValueTree() : trackList.front();
    trackCombo.clear (juce::dontSendNotification);
    int sel = -1;
    for (size_t i = 0; i < trackList.size(); ++i)
    {
        trackCombo.addItem (trackList[i][tl::ids::name].toString(), (int) i + 1);
        if (trackList[i] == track) sel = (int) i;
    }
    trackCombo.setSelectedItemIndex (sel, juce::dontSendNotification);

    clipList.clear();
    if (track.isValid())
        for (auto c : track) if (midiclip::isMidiClip (c)) clipList.push_back (c);
    std::sort (clipList.begin(), clipList.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) { return (double) a[tl::ids::start] < (double) b[tl::ids::start]; });
    if (! clip.isValid() || std::find (clipList.begin(), clipList.end(), clip) == clipList.end())
        clip = clipList.empty() ? juce::ValueTree() : clipList.front();
    clipCombo.clear (juce::dontSendNotification);
    sel = -1;
    for (size_t i = 0; i < clipList.size(); ++i)
    {
        clipCombo.addItem (clipList[i][tl::ids::name].toString() + "  (from " + juce::String ((double) clipList[i][tl::ids::start], 1) + " s)", (int) i + 1);
        if (clipList[i] == clip) sel = (int) i;
    }
    clipCombo.setSelectedItemIndex (sel, juce::dontSendNotification);

    const bool hasClip = clip.isValid();
    deleteClipButton.setEnabled (hasClip);
    instrumentButton.setEnabled (track.isValid());
    hint.setVisible (! hasClip);
    hint.setText (track.isValid() ? "No clip on this track yet. Click Generate for a pattern, or New clip for an empty one."
                                  : "No keys track yet. Pick Piano, Synth or Bass, then click Generate.", juce::dontSendNotification);
    if (hasClip && (roll == nullptr || roll->getClip() != clip))
    {
        roll = std::make_unique<PianoRoll> (doc, host, clip);
        roll->onClipGone = [safe = juce::Component::SafePointer<KeysPage> (this)] {
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->clip = {}; safe->roll.reset(); safe->refresh(); } });
        };
        addAndMakeVisible (*roll);
        resized();
    }
    else if (! hasClip && roll != nullptr)
        roll.reset();
}

void KeysPage::message (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle (title).withMessage (text).withButton ("OK").withAssociatedComponent (this), nullptr);
}

void KeysPage::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    g.setColour (palette::panel);
    g.fillRect (getLocalBounds().removeFromTop (126));
    g.setColour (palette::outline);
    g.drawHorizontalLine (42, 0.0f, (float) getWidth());
    g.drawHorizontalLine (84, 0.0f, (float) getWidth());
    g.drawHorizontalLine (126, 0.0f, (float) getWidth());
}

void KeysPage::resized()
{
    auto r = getLocalBounds();
    auto row0 = r.removeFromTop (42).reduced (8, 7);
    {
        const int groupW = 34 + 10 + 54;
        auto grp = juce::Rectangle<int> (groupW, row0.getHeight()).withCentre ({ getWidth() / 2, row0.getCentreY() });
        backButton.setBounds (grp.removeFromLeft (34)); grp.removeFromLeft (10);
        loopButton.setBounds (grp.removeFromLeft (54));
    }
    row0.removeFromLeft (tabsReserve);
    trackCombo.setBounds (row0.removeFromLeft (170)); row0.removeFromLeft (4);
    newTrackButton.setBounds (row0.removeFromLeft (70)); row0.removeFromLeft (4);
    instrumentButton.setBounds (row0.removeFromLeft (108));

    auto row1 = r.removeFromTop (42).reduced (8, 7);
    styleCombo.setBounds (row1.removeFromLeft (90)); row1.removeFromLeft (8);
    keyCombo.setBounds (row1.removeFromLeft (64)); row1.removeFromLeft (4);
    scaleCombo.setBounds (row1.removeFromLeft (150)); row1.removeFromLeft (4);
    progCombo.setBounds (row1.removeFromLeft (150)); row1.removeFromLeft (4);
    genCombo.setBounds (row1.removeFromLeft (160)); row1.removeFromLeft (4);
    octaveCombo.setBounds (row1.removeFromLeft (100)); row1.removeFromLeft (4);
    barsCombo.setBounds (row1.removeFromLeft (80)); row1.removeFromLeft (8);
    generateButton.setBounds (row1.removeFromLeft (90));

    auto row2 = r.removeFromTop (42).reduced (8, 7);
    clipCombo.setBounds (row2.removeFromLeft (230)); row2.removeFromLeft (4);
    newClipButton.setBounds (row2.removeFromLeft (84)); row2.removeFromLeft (2);
    deleteClipButton.setBounds (row2.removeFromLeft (96));

    hint.setBounds (r);
    if (roll != nullptr) roll->setBounds (r);
}
} // namespace ff
