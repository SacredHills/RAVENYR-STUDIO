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

    for (int b : { 1, 2, 4, 8, 12, 16, 32 }) barsCombo.addItem (juce::String (b) + (b == 1 ? " bar" : " bars"), b);
    barsCombo.setTooltip ("How long the clip is (and what loops)");
    barsCombo.onChange = [this] {
        if (updating || ! clip.isValid() || barsCombo.getSelectedId() <= 0) return;
        const auto map = doc.tempoMap();
        const double s0 = (double) clip[tl::ids::start], q0 = map.quarterAtSeconds (s0);
        doc.undo().beginNewTransaction ("Clip length");
        clip.setProperty (tl::ids::length, map.secondsAtQuarter (q0 + barsCombo.getSelectedId() * barQuarters()) - s0, &doc.undo());
        if (roll != nullptr) roll->fitView();
    };
    trackCombo.setTooltip ("The keys track being edited");
    trackCombo.onChange = [this] {
        if (updating || trackCombo.getSelectedItemIndex() < 0) return;
        track = trackList[(size_t) trackCombo.getSelectedItemIndex()];
        clip = {};
        refresh();
    };
    newTrackButton.setTooltip ("A new instrument track with the built-in synth (load a real instrument with Instrument...)");
    newTrackButton.onClick = [this] {
        doc.undo().beginNewTransaction ("New keys track");
        track = makeTrack();
        clip = {};
        refresh();
    };
    deleteTrackButton.setTooltip ("Delete this track and its notes (undo brings it back)");
    deleteTrackButton.onClick = [this] {
        if (! track.isValid()) return;
        if (host.engine.isPlaying()) host.engine.stop();
        auto t = track;
        track = {};
        clip = {};
        roll.reset();
        session.deleteTrack (t);   // one undo step
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

    for (auto* c : std::initializer_list<juce::Component*> { &trackCombo, &newTrackButton, &deleteTrackButton, &instrumentButton, &backButton,
                                                             &loopButton, &barsCombo, &lengthLabel, &hint })
        addAndMakeVisible (c);
    lengthLabel.setColour (juce::Label::textColourId, palette::textDim);
    lengthLabel.setJustificationType (juce::Justification::centredRight);
    hint.setJustificationType (juce::Justification::centred);
    hint.setColour (juce::Label::textColourId, palette::textDim);
    hint.setFont (uiFont (15.0f));
    refresh();
}

KeysPage::~KeysPage()
{
    stopTimer();
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

double KeysPage::barQuarters() const
{
    auto tr = const_cast<ProjectDocument&> (doc).transport();
    const int num = juce::jmax (1, (int) tr.getProperty (project::ids::tsNum, 4)), den = juce::jmax (1, (int) tr.getProperty (project::ids::tsDen, 4));
    return num * 4.0 / den;
}

void KeysPage::setActive (bool on, bool keepPlaying)
{
    if (active == on) return;
    active = on;
    auto& e = host.engine;
    if (on)
    {
        // playback started in the editor or the mixer carries on; the page just shows it
        if (lingering) { lingering = false; stopTimer(); }
        refresh();
    }
    else
    {
        if (keepPlaying && e.isPlaying()) { lingering = true; startTimerHz (20); return; }
        if (e.isPlaying()) e.stop();
        doc.refreshEngine();   // the project's own loop and click settings again
    }
}

bool KeysPage::endBackground()
{
    if (! lingering) return false;
    lingering = false;
    stopTimer();
    const bool wasPlaying = host.engine.isPlaying();
    if (wasPlaying) host.engine.stop();
    doc.refreshEngine();
    return wasPlaying;
}

void KeysPage::togglePlay()
{
    if (host.engine.isPlaying()) { host.engine.stop(); return; }
    startPlayback();
}

void KeysPage::startPlayback()
{
    if (! clip.isValid()) { message ("Play", "There is no keys track yet. Click + Track first."); return; }
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
    int same = 0;
    for (auto t : doc.tracks()) if (t[tl::ids::name].toString().startsWith ("Keys")) ++same;
    auto t = session.addTrack (same > 0 ? "Keys " + juce::String (same + 1) : juce::String ("Keys"), -1, tl::TrackKind::instrument);
    session.setInstrument (t, fx::builtin::descriptionXml ("fretforge:synth"), "Basic Synth");
    track = t;
    makeClip (4);
    return t;
}

juce::ValueTree KeysPage::firstClip (const juce::ValueTree& t) const
{
    juce::ValueTree best;
    for (auto c : t)
        if (midiclip::isMidiClip (c) && (! best.isValid() || (double) c[tl::ids::start] < (double) best[tl::ids::start])) best = c;
    return best;
}

juce::ValueTree KeysPage::makeClip (int bars)
{
    const auto map = doc.tempoMap();
    auto c = midiclip::createMidiClip ("Keys", 0.0, {});
    c.setProperty (tl::ids::length, map.secondsAtQuarter (bars * barQuarters()), nullptr);
    track.appendChild (c, &doc.undo());
    return c;
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

    // a track is one clip: tracks without one (made in the editor) get it here
    if (track.isValid() && ! firstClip (track).isValid())
    {
        doc.undo().beginNewTransaction ("New MIDI clip");
        makeClip (4);
    }
    clip = track.isValid() ? firstClip (track) : juce::ValueTree();

    const bool hasClip = clip.isValid();
    deleteTrackButton.setEnabled (track.isValid());
    instrumentButton.setEnabled (track.isValid());
    barsCombo.setEnabled (hasClip);
    if (hasClip)
    {
        const auto map = doc.tempoMap();
        const double s0 = (double) clip[tl::ids::start];
        const double bars = (map.quarterAtSeconds (s0 + (double) clip[tl::ids::length]) - map.quarterAtSeconds (s0)) / barQuarters();
        barsCombo.setSelectedId (juce::roundToInt (bars), juce::dontSendNotification);   // none selected for a length that is not on the list
    }
    hint.setVisible (! hasClip);
    hint.setText ("No keys track yet. Click + Track, then draw notes in the piano roll.", juce::dontSendNotification);
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
    g.fillRect (getLocalBounds().removeFromTop (42));
    g.setColour (palette::outline);
    g.drawHorizontalLine (42, 0.0f, (float) getWidth());
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
    barsCombo.setBounds (row0.removeFromRight (84));
    lengthLabel.setBounds (row0.removeFromRight (56));
    row0.removeFromLeft (tabsReserve);
    trackCombo.setBounds (row0.removeFromLeft (140)); row0.removeFromLeft (4);
    newTrackButton.setBounds (row0.removeFromLeft (70)); row0.removeFromLeft (4);
    deleteTrackButton.setBounds (row0.removeFromLeft (88)); row0.removeFromLeft (4);
    instrumentButton.setBounds (row0.removeFromLeft (108));

    hint.setBounds (r);
    if (roll != nullptr) roll->setBounds (r);
}
} // namespace ff
