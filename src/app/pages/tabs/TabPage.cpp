#include "TabPage.h"
#include "core/MusicTheory.h"
#include "engine/TabSynth.h"
#include "core/GuitarPro.h"
#include "core/MusicXml.h"
#include "ScorePdf.h"
#include "core/DrumModel.h"

namespace ff
{
namespace tids = tab::ids;

static const char* sigNames[] = { "2/4", "3/4", "4/4", "5/4", "6/4", "7/4", "2/2", "3/8", "5/8", "6/8", "7/8", "9/8", "12/8" };

TabPage::TabPage (ProjectDocument& d, EngineHost& h, juce::PropertiesFile& s)
    : doc (d), host (h), settings (s), editor (d, h)
{
    viewport.setViewedComponent (&editor, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    editor.onStateChanged = [this] { refreshPalette(); ensureCursorVisible(); };

    trackCombo.onChange = [this] { if (! updating) editor.setTrackIndex (trackCombo.getSelectedItemIndex()); };
    trackCombo.setTooltip ("Track to view and edit (Ctrl+Up/Down)");
    addTrackButton.onClick = [this] { showAddTrackDialog(); };
    tuningButton.onClick = [this] { showTuningDialog(); };
    trackMenuButton.onClick = [this] { showTrackMenu(); };

    tempoLabel.setText ("Tempo", juce::dontSendNotification);
    speedLabel.setText ("Speed", juce::dontSendNotification);
    for (auto* l : { &tempoLabel, &speedLabel }) styleLabel (*l, 12.0f, palette::textDim, true);  // shown only when there is room
    tempoSlider.setSliderStyle (juce::Slider::LinearBarVertical);
    tempoSlider.setRange (20.0, 400.0, 1.0);
    tempoSlider.setTextValueSuffix (" BPM");
    tempoSlider.setColour (juce::Slider::trackColourId, palette::well);
    tempoSlider.onDragStart = [this] { doc.undo().beginNewTransaction ("Tab tempo"); };
    tempoSlider.onValueChange = [this] { if (! updating) doc.set (score, tids::tempo, tempoSlider.getValue()); };
    tempoSlider.setTooltip ("Score tempo (drag or double-click to type)");

    for (int pct : { 25, 40, 50, 60, 70, 75, 80, 90, 100, 110, 125, 150 })
        speedCombo.addItem (juce::String (pct) + "%", pct);
    speedCombo.setSelectedId (100, juce::dontSendNotification);
    speedCombo.setTooltip ("Practice speed: slows playback without changing pitch");
    speedCombo.onChange = [this] {
        const double newSpeed = speedCombo.getSelectedId() / 100.0;
        if (active && host.engine.isPlaying())
            host.engine.locateSeconds (host.engine.getPositionSeconds() * speed / newSpeed);
        speed = newSpeed;
        pushEngineOverrides();
        rebuildSequence();
    };

    playButton.onClick = [this] { togglePlay(); };
    playButton.setTooltip ("Play from the cursor, or loop the selection (Space also plays/stops)");
    loopButton.setClickingTogglesState (true);
    loopButton.setTooltip ("Loop the selected beats (or the cursor's bar)");
    loopButton.onClick = [this] { if (loopButton.getToggleState()) loopSongButton.setToggleState (false, juce::dontSendNotification); pushEngineOverrides(); };
    loopSongButton.setClickingTogglesState (true);
    loopSongButton.setTooltip ("Loop the whole song: playback starts at the cursor and wraps from the last bar to the first");
    loopSongButton.onClick = [this] { if (loopSongButton.getToggleState()) loopButton.setToggleState (false, juce::dontSendNotification); pushEngineOverrides(); };
    clickButton.setClickingTogglesState (true);
    clickButton.setToggleState (settings.getBoolValue ("tabClick", false), juce::dontSendNotification);
    clickButton.onClick = [this] { settings.setValue ("tabClick", clickButton.getToggleState()); pushEngineOverrides(); };
    countButton.setClickingTogglesState (true);
    countButton.setToggleState (settings.getBoolValue ("tabCountIn", false), juce::dontSendNotification);
    countButton.onClick = [this] { settings.setValue ("tabCountIn", countButton.getToggleState()); };
    importButton.setTooltip ("Import Guitar Pro (.gp3 .gp4 .gp5 .gpx .gp), MusicXML (.musicxml .xml .mxl), MIDI or ASCII tab");
    importButton.onClick = [this] { importDialog(); };
    exportButton.setTooltip ("Export MIDI, MusicXML or PDF, or print");
    exportButton.onClick = [this] { exportDialog(); };
    notationButton.setClickingTogglesState (true);
    notationButton.setTooltip ("Show standard notation above the tab");
    notationButton.onClick = [this] { doc.set (score, tids::notation, notationButton.getToggleState(), false); editor.grabKeyboardFocus(); };
    drumsButton.setClickingTogglesState (true);
    drumsButton.setToggleState (settings.getBoolValue ("tabDrums", false), juce::dontSendNotification);
    drumsButton.setTooltip ("Play the MIDI Studio drum song along with the tab (bar 1 of the drum song = bar 1 of the score)");
    drumsButton.onClick = [this] { settings.setValue ("tabDrums", drumsButton.getToggleState()); rebuildSequence(); editor.grabKeyboardFocus(); };
    midiButton.setClickingTogglesState (true);
    midiButton.setTooltip ("Enter notes from a MIDI keyboard or MIDI guitar: play a note or chord to write it at the cursor");
    midiButton.onClick = [this] { setMidiInput (midiButton.getToggleState()); editor.grabKeyboardFocus(); };

    for (auto* c : std::initializer_list<juce::Component*> { &trackCombo, &addTrackButton, &tuningButton, &trackMenuButton, &tempoSlider,
                                                             &speedCombo, &playButton, &loopButton, &loopSongButton, &clickButton, &countButton,
                                                             &importButton, &exportButton, &notationButton, &midiButton, &drumsButton })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }

    // palette
    const std::pair<int, const char*> durs[] = { { 1, "1" }, { 2, "1/2" }, { 4, "1/4" }, { 8, "1/8" }, { 16, "1/16" }, { 32, "1/32" }, { 64, "1/64" } };
    for (auto [dur, name] : durs)
    {
        auto* b = durButtons.add (new NoteValueButton (dur, name));
        b->setTooltip ("Note value (+ / - keys)");
        b->onClick = [this, dur = dur] { editor.setDuration (dur); editor.grabKeyboardFocus(); };
    }
    dotButton.setTooltip ("Dotted (.)");
    tripletButton.setTooltip ("Triplet (T)");
    restButton.setTooltip ("Rest (R)");
    dotButton.onClick = [this] { editor.toggleDots(); editor.grabKeyboardFocus(); };
    tripletButton.onClick = [this] { editor.toggleTuplet(); editor.grabKeyboardFocus(); };
    restButton.onClick = [this] { editor.toggleRest(); editor.grabKeyboardFocus(); };

    techs = {
        { "Hammer", "Hammer-on / pull-off to the next note", "H", [] (TabEditor& e) { e.toggleNoteFlag (tids::hammer, "Hammer-on"); } },
        { "Slide", "Slide: to next / in from below / out downwards", "S", [] (TabEditor& e) { e.cycleSlide(); } },
        { "Bend", "Bend: 1/2, full, 1 1/2, 2", "B", [] (TabEditor& e) { e.cycleBend(); } },
        { "Vibrato", "Vibrato", "V", [] (TabEditor& e) { e.toggleNoteFlag (tids::vibrato, "Vibrato"); } },
        { "Palm mute", "Palm mute", "P", [] (TabEditor& e) { e.toggleNoteFlag (tids::palmMute, "Palm mute"); } },
        { "Dead note", "Dead note (x)", "X", [] (TabEditor& e) { e.toggleNoteFlag (tids::dead, "Dead note"); } },
        { "Ghost note", "Ghost note ( )", "G", [] (TabEditor& e) { e.toggleNoteFlag (tids::ghost, "Ghost note"); } },
        { "Natural harmonic", "Natural harmonic", "N", [] (TabEditor& e) { e.cycleHarmonic (tab::harmNatural); } },
        { "Artificial harmonic", "Artificial harmonic", "A", [] (TabEditor& e) { e.cycleHarmonic (tab::harmArtificial); } },
        { "Let ring", "Let ring", "L", [] (TabEditor& e) { e.toggleNoteFlag (tids::letRing, "Let ring"); } },
        { "Tapping", "Tapping", "Shift+T", [] (TabEditor& e) { e.toggleNoteFlag (tids::tap, "Tap"); } },
        { "Tie", "Tie to the previous note", "Z", [] (TabEditor& e) { e.toggleNoteFlag (tids::tie, "Tie"); } },
        { "Accent", "Accent (>)", ">", [] (TabEditor& e) { e.toggleNoteFlag (tids::accent, "Accent"); } },
        { "Downstroke", "Downstroke", "D", [] (TabEditor& e) { e.setStroke (tab::strokeDown); } },
        { "Upstroke", "Upstroke", "U", [] (TabEditor& e) { e.setStroke (tab::strokeUp); } },
        { "Tremolo picking", "Tremolo picking", "W", [] (TabEditor& e) { e.toggleBeatFlag (tids::tremolo, "Tremolo picking"); } },
    };
    for (int i = 0; i < numVisibleTechs; ++i)
    {
        auto* b = techButtons.add (new juce::TextButton (techs[(size_t) i].label));
        b->setTooltip (techs[(size_t) i].name + " (" + techs[(size_t) i].key + ")");
        b->setColour (juce::TextButton::buttonOnColourId, palette::accent);
        b->setColour (juce::TextButton::textColourOnId, juce::Colour (0xff15171a));
        b->onClick = [this, fn = techs[(size_t) i].fn] { fn (editor); editor.grabKeyboardFocus(); };
    }
    techMenuButton.setTooltip ("Every technique (dead / ghost notes, harmonics, let ring, tapping, ties, strokes...)");
    techMenuButton.onClick = [this] { showTechMenu(); };
    techMenuButton.setWantsKeyboardFocus (false);
    addAndMakeVisible (techMenuButton);

    transDownButton.setTooltip ("Transpose selection down a semitone");
    transUpButton.setTooltip ("Transpose selection up a semitone");
    transDownButton.onClick = [this] { editor.transpose (-1); editor.grabKeyboardFocus(); };
    transUpButton.onClick = [this] { editor.transpose (1); editor.grabKeyboardFocus(); };
    for (int i = 0; i < (int) std::size (sigNames); ++i)
        sigCombo.addItem (sigNames[i], i + 1);
    sigCombo.setTooltip ("Time signature of the cursor's bar");
    sigCombo.onChange = [this] {
        if (updating || sigCombo.getSelectedId() <= 0) return;
        const auto t = juce::StringArray::fromTokens (sigCombo.getText(), "/", "");
        auto meas = tab::measures (score).getChild (editor.cursor.bar);
        doc.undo().beginNewTransaction ("Time signature");
        doc.set (meas, tids::tsNum, t[0].getIntValue());
        doc.set (meas, tids::tsDen, t[1].getIntValue());
        editor.grabKeyboardFocus();
    };

    for (auto* b : durButtons) { b->setClickingTogglesState (false); b->setWantsKeyboardFocus (false); addAndMakeVisible (b); }
    for (auto* b : techButtons) { b->setWantsKeyboardFocus (false); addAndMakeVisible (b); }
    barButton.setTooltip ("Insert or delete bars; tempo change, key signature and section marker of the cursor's bar");
    barButton.onClick = [this] { showBarMenu(); };
    textButton.setTooltip ("Chord name, lyric and text of the cursor's beat (or double-click a beat)");
    textButton.onClick = [this] { showBeatTextDialog(); };
    editor.onDoubleClick = [this] { showBeatTextDialog(); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &dotButton, &tripletButton, &restButton, &transDownButton, &transUpButton, &sigCombo,
                                                                       &barButton, &textButton })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }
    styleLabel (status, 12.0f);
    addAndMakeVisible (status);

    rebind();
    startTimerHz (30);
}

TabPage::~TabPage()
{
    setMidiInput (false);
    root.removeListener (this);
}

void TabPage::rebind()
{
    root.removeListener (this);
    root = doc.tree();
    score = tab::getOrCreateScore (root);
    root.addListener (this);
    editor.setScore (score);
    refreshTracks();
    rebuildSequence();
}

void TabPage::structureChanged()
{
    refreshTracks();
    editor.layoutChanged();
    triggerAsyncUpdate();
}

void TabPage::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& id)
{
    if (! node.isAChildOf (score) && node != score)
        return;
    if (node == score && id == tids::tempo)
        pushEngineOverrides();
    if (node.hasType (tids::TABTRACK)) refreshTracks();
    editor.layoutChanged();
    refreshPalette();
    triggerAsyncUpdate();
}

void TabPage::refreshTracks()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    trackCombo.clear (juce::dontSendNotification);
    int i = 1;
    for (auto t : tab::tracks (score))
        trackCombo.addItem (juce::String (i) + ". " + t[tids::name].toString() + "  (" + juce::String (tab::numStrings (t)) + " strings)", i), ++i;
    trackCombo.setSelectedItemIndex (editor.getTrackIndex(), juce::dontSendNotification);
    tempoSlider.setValue ((double) score.getProperty (tids::tempo, 120.0), juce::dontSendNotification);
    notationButton.setToggleState ((bool) score.getProperty (tids::notation, true), juce::dontSendNotification);
    const bool hasTrack = tab::tracks (score).getNumChildren() > 0;
    for (auto* c : std::initializer_list<juce::Component*> { &tuningButton, &trackMenuButton, &playButton, &exportButton })
        c->setEnabled (hasTrack);
    refreshPalette();
}

void TabPage::showTechMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Techniques for the note at the cursor");
    const char* groups[] = { "Legato and bends", nullptr, nullptr, nullptr, "Muting and feel", nullptr, nullptr, "Harmonics", nullptr,
                             "Sustain", "Two hands", "Ties and accents", nullptr, "Picking", nullptr, nullptr };
    for (int i = 0; i < (int) techs.size(); ++i)
    {
        if (i < (int) std::size (groups) && groups[i] != nullptr && i > 0) m.addSeparator();
        m.addItem (i + 1, techs[(size_t) i].label + "        " + techs[(size_t) i].key, true, techActive[(size_t) i]);
    }
    juce::Component::SafePointer<TabPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&techMenuButton), [safe] (int r) {
        if (safe == nullptr || r <= 0) return;
        safe->techs[(size_t) (r - 1)].fn (safe->editor);
        safe->editor.grabKeyboardFocus();
    });
}

void TabPage::refreshPalette()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    auto bar = tab::bar (editor.track(), editor.cursor.bar);
    auto beat = bar.getChild (editor.cursor.beat);
    const int dur = beat.isValid() ? (int) beat[tids::dur] : editor.entryDur;
    const int durs[] = { 1, 2, 4, 8, 16, 32, 64 };
    for (int i = 0; i < durButtons.size(); ++i)
        durButtons[i]->setToggleState (durs[i] == dur, juce::dontSendNotification);
    dotButton.setToggleState (beat.isValid() ? (int) beat[tids::dots] > 0 : editor.entryDots > 0, juce::dontSendNotification);
    tripletButton.setToggleState (beat.isValid() ? (int) beat[tids::tuplet] > 0 : editor.entryTuplet > 0, juce::dontSendNotification);
    restButton.setToggleState (beat.isValid() && (bool) beat[tids::rest], juce::dontSendNotification);

    auto note = tab::noteOnString (beat, editor.cursorString);
    const bool noteFlags[] = { (bool) note[tids::hammer], (int) note[tids::slide] != 0, (double) note[tids::bend] > 0.0, (bool) note[tids::vibrato],
                           (bool) note[tids::palmMute], (bool) note[tids::dead], (bool) note[tids::ghost], (int) note[tids::harmonic] == 1,
                           (int) note[tids::harmonic] == 2, (bool) note[tids::letRing], (bool) note[tids::tap], (bool) note[tids::tie],
                           (bool) note[tids::accent], (int) beat[tids::stroke] == 1, (int) beat[tids::stroke] == 2, (bool) beat[tids::tremolo] };
    for (int i = 0; i < techButtons.size(); ++i)
        techButtons[i]->setToggleState (noteFlags[i], juce::dontSendNotification);
    for (int i = 0; i < (int) std::size (noteFlags) && i < (int) techActive.size(); ++i) techActive[(size_t) i] = noteFlags[i];

    auto meas = tab::measures (score).getChild (editor.cursor.bar);
    const auto sig = juce::String ((int) meas.getProperty (tids::tsNum, 4)) + "/" + juce::String ((int) meas.getProperty (tids::tsDen, 4));
    sigCombo.setSelectedId (0, juce::dontSendNotification);
    for (int i = 0; i < sigCombo.getNumItems(); ++i)
        if (sigCombo.getItemText (i) == sig) sigCombo.setSelectedItemIndex (i, juce::dontSendNotification);
    if (sigCombo.getSelectedId() == 0) sigCombo.setText (sig, juce::dontSendNotification);

    if (editor.track().isValid())
    {
        const double content = tab::barContentQ (bar), len = tab::measureLengthQ (meas);
        juce::String st;
        st << "Bar " << editor.cursor.bar + 1 << " / " << tab::numMeasures (score) << dotSep() << "Beat " << editor.cursor.beat + 1
           << dotSep() << "String " << tab::numStrings (editor.track()) - editor.cursorString << dotSep()
           << "Bar holds " << juce::String (content, 2) << " of " << juce::String (len, 2) << " beats (quarters)";
        if (editor.hasSelection()) st << dotSep() << editor.selectedBeats().size() << " beats selected";
        status.setText (st, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, content > len + 1e-6 ? palette::bad : palette::textDim);
    }
    else
        status.setText ({}, juce::dontSendNotification);
}

void TabPage::ensureCursorVisible()
{
    auto r = editor.cursorBounds();
    if (r.isEmpty()) return;
    const int top = viewport.getViewPositionY(), h = viewport.getViewHeight();
    if (r.getY() < top) viewport.setViewPosition (0, r.getY());
    else if (r.getBottom() > top + h) viewport.setViewPosition (0, r.getBottom() - h);
}

// ------------------------------------------------------------------ engine

void TabPage::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive) return;
    active = shouldBeActive;
    if (active)
    {
        if (host.engine.isPlaying()) host.engine.stop();
        host.engine.tabMode = true;
        rebuildSequence();
        pushEngineOverrides();
        editor.grabKeyboardFocus();
    }
    else
    {
        restoreEngine();
    }
}

void TabPage::pushEngineOverrides()
{
    if (! active) return;
    auto& e = host.engine;
    const double tempo = tab::measureTempo (score, editor.cursor.bar) * speed;
    e.setTempo (tempo);
    auto m0 = tab::measures (score).getChild (editor.cursor.bar);
    e.setTimeSignature ((int) m0.getProperty (tids::tsNum, 4), (int) m0.getProperty (tids::tsDen, 4));
    e.clickWhilePlaying = clickButton.getToggleState();
    e.punchEnabled = false;
    if (loopSongButton.getToggleState())
    {
        const auto map = tab::tempoMap (score, speed);
        e.loopStartSec = 0.0;
        e.loopEndSec = map.secondsAt (tab::measureStarts (score).back());
        e.loopEnabled = true;
    }
    else if (loopButton.getToggleState())
    {
        const auto [a, b] = editor.selectionSpanQ();
        const auto map = tab::tempoMap (score, speed);
        e.loopStartSec = map.secondsAt (a);
        e.loopEndSec = map.secondsAt (b);
        e.loopEnabled = true;
    }
    else
        e.loopEnabled = false;
}

void TabPage::restoreEngine()
{
    auto& e = host.engine;
    if (e.isPlaying()) e.stop();
    e.tabMode = false;
    doc.refreshEngine(); // project tempo, meter, click and loop settings again
}

void TabPage::rebuildSequence()
{
    playMap = tab::tempoMap (score, speed);
    auto seq = buildTabSequence (score, speed);
    if (drumsButton.getToggleState())
        addDrumSong (*seq, drums::find (root), playMap);
    host.engine.submitTabSequence (std::move (seq));
}

void TabPage::togglePlay()
{
    auto& e = host.engine;
    if (e.isPlaying()) { e.stop(); return; }
    rebuildSequence();
    pushEngineOverrides();
    const double tempo = tab::measureTempo (score, editor.cursor.bar) * speed;
    const double startQ = loopButton.getToggleState() ? editor.selectionSpanQ().first : editor.cursorQ();
    e.locateSeconds (playMap.secondsAt (startQ));
    juce::int64 countIn = 0;
    if (countButton.getToggleState() && e.getSampleRate() > 0)
    {
        auto meas = tab::measures (score).getChild (editor.cursor.bar);
        countIn = (juce::int64) ((int) meas.getProperty (tids::tsNum, 4)
                                 * MetronomeDsp::beatLengthSamples (e.getSampleRate(), tempo, (int) meas.getProperty (tids::tsDen, 4)));
    }
    e.playWithCountIn (countIn);
    editor.grabKeyboardFocus();
}

void TabPage::timerCallback()
{
    if (! active) return;
    auto& e = host.engine;
    const bool playing = e.isPlaying();
    playButton.setButtonText (playing ? "Stop" : "Play tab");
    playButton.setToggleState (playing, juce::dontSendNotification);
    if (playing && ! e.isCountingIn())
    {
        const double q = playMap.quarterAt (e.getPositionSeconds());
        editor.setPlayheadQ (q);
        // follow the playhead: keep the system being played in view
        const auto r = editor.barBounds (editor.barAtQ (q));
        const int top = viewport.getViewPositionY(), h = viewport.getViewHeight();
        if (! r.isEmpty() && (r.getY() < top || r.getBottom() > top + h))
            viewport.setViewPosition (viewport.getViewPositionX(), juce::jmax (0, r.getY() - 8));
    }
    else
        editor.setPlayheadQ (-1.0);
}

// ------------------------------------------------------------------ dialogs

void TabPage::message (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle (title).withMessage (text).withButton ("OK").withAssociatedComponent (this),
                                  nullptr);
}

void TabPage::showAddTrackDialog()
{
    auto* w = new juce::AlertWindow ("Add tab track", "Choose the instrument and tuning.", juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", "Guitar " + juce::String (tab::tracks (score).getNumChildren() + 1), "Name");
    juce::StringArray names;
    for (auto& t : theory::tuningPresets()) names.add (t.category + ":  " + t.name);
    w->addComboBox ("tuning", names, "Instrument & tuning");
    w->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TabPage> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r) {
        if (safe == nullptr || r != 1) return;
        const auto& preset = theory::tuningPresets()[(size_t) juce::jmax (0, w->getComboBoxComponent ("tuning")->getSelectedItemIndex())];
        auto name = w->getTextEditorContents ("name").trim();
        if (name.isEmpty()) name = preset.name;
        safe->doc.undo().beginNewTransaction ("Add tab track");
        tab::addTrack (safe->score, name, preset.strings, preset.category.startsWith ("Bass") ? "bass" : "guitar", &safe->doc.undo());
        safe->refreshTracks();
        safe->editor.setTrackIndex (tab::tracks (safe->score).getNumChildren() - 1);
        safe->refreshTracks();
    }), true);
}

void TabPage::showTuningDialog()
{
    auto t = editor.track();
    if (! t.isValid()) return;
    const auto current = tab::trackTuning (t);
    auto* w = new juce::AlertWindow ("Tuning: " + t[tids::name].toString(),
                                     "Pick a preset or type the open strings from lowest to highest (e.g. D2 A2 D3 G3 B3 E4).",
                                     juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray presetNames { "(custom)" };
    std::vector<const theory::Tuning*> matching;
    for (auto& p : theory::tuningPresets())
        if (p.strings.size() == current.size()) { presetNames.add (p.name); matching.push_back (&p); }
    w->addComboBox ("preset", presetNames, "Preset");
    juce::StringArray noteNames;
    for (int n : current) noteNames.add (theory::midiNoteName (n));
    w->addTextEditor ("notes", noteNames.joinIntoString (" "), "Strings");
    juce::StringArray capos { "No capo" };
    for (int i = 1; i <= 12; ++i) capos.add ("Capo " + juce::String (i));
    w->addComboBox ("capo", capos, "Capo");
    w->getComboBoxComponent ("capo")->setSelectedItemIndex ((int) t[tids::capo]);
    w->getComboBoxComponent ("preset")->setSelectedItemIndex (0);
    auto* presetBox = w->getComboBoxComponent ("preset");
    presetBox->onChange = [w, presetBox, matching] {
        const int i = presetBox->getSelectedItemIndex() - 1;
        if (! juce::isPositiveAndBelow (i, (int) matching.size())) return;
        juce::StringArray nn;
        for (int n : matching[(size_t) i]->strings) nn.add (theory::midiNoteName (n));
        if (auto* ed = w->getTextEditor ("notes")) ed->setText (nn.joinIntoString (" "));
    };
    w->addButton ("Apply", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TabPage> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, t] (int r) mutable {
        if (safe == nullptr || r != 1) return;
        std::vector<int> notes;
        for (auto& tok : juce::StringArray::fromTokens (w->getTextEditorContents ("notes"), " ,", ""))
        {
            const int n = theory::parseNoteName (tok);
            if (n < 0) { safe->message ("Tuning", "\"" + tok + "\" is not a note name like E2 or F#1."); return; }
            notes.push_back (n);
        }
        if (notes.size() != tab::trackTuning (t).size())
        {
            safe->message ("Tuning", "Enter exactly " + juce::String ((int) tab::trackTuning (t).size()) + " notes (one per string).");
            return;
        }
        std::sort (notes.begin(), notes.end());
        safe->doc.undo().beginNewTransaction ("Change tuning");
        safe->doc.set (t, tids::tuning, tab::tuningToString (notes));
        safe->doc.set (t, tids::capo, w->getComboBoxComponent ("capo")->getSelectedItemIndex());
    }), true);
}

void TabPage::showTrackMenu()
{
    auto t = editor.track();
    if (! t.isValid()) return;
    juce::PopupMenu m;
    m.addItem (1, "Rename...");
    m.addItem (2, "Mute", true, (bool) t[tids::mute]);
    m.addItem (3, "Solo", true, (bool) t[tids::solo]);
    juce::PopupMenu vol;
    for (int pct : { 100, 80, 60, 40, 20 })
        vol.addItem (100 + pct, juce::String (pct) + "%", true, std::abs ((double) t[tids::volume] * 100.0 - pct) < 1.0);
    m.addSubMenu ("Volume", vol);
    m.addSeparator();
    m.addItem (4, "Delete track");
    juce::Component::SafePointer<TabPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&trackMenuButton), [safe, t] (int r) mutable {
        if (safe == nullptr || r == 0) return;
        auto& um = safe->doc.undo();
        if (r == 1)
        {
            auto* w = new juce::AlertWindow ("Rename track", {}, juce::MessageBoxIconType::NoIcon, safe.getComponent());
            w->addTextEditor ("name", t[tids::name].toString());
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, t] (int res) mutable {
                const auto n = w->getTextEditorContents ("name").trim();
                if (safe != nullptr && res == 1 && n.isNotEmpty())
                {
                    safe->doc.undo().beginNewTransaction ("Rename track");
                    safe->doc.set (t, tids::name, n);
                }
            }), true);
        }
        else if (r == 2) safe->doc.set (t, tids::mute, ! (bool) t[tids::mute], false);
        else if (r == 3) safe->doc.set (t, tids::solo, ! (bool) t[tids::solo], false);
        else if (r >= 100) safe->doc.set (t, tids::volume, (r - 100) / 100.0, false);
        else if (r == 4)
        {
            um.beginNewTransaction ("Delete tab track");
            tab::tracks (safe->score).removeChild (t, &um);
            safe->editor.setTrackIndex (safe->editor.getTrackIndex());
        }
    });
}

bool TabPage::canImport (const juce::File& f)
{
    return f.hasFileExtension ("gp3;gp4;gp5;gpx;gp;musicxml;mxl;xml;mid;midi;txt;tab");
}

void TabPage::importDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a score", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.gp3;*.gp4;*.gp5;*.gpx;*.gp;*.musicxml;*.mxl;*.xml;*.mid;*.midi;*.txt;*.tab");
    juce::Component::SafePointer<TabPage> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
        if (safe != nullptr && fc.getResult() != juce::File()) safe->importFile (fc.getResult());
    });
}

void TabPage::importFile (const juce::File& f)
{
    const bool wholeScore = f.hasFileExtension ("gp3;gp4;gp5;gpx;gp;musicxml;mxl;xml");
    bool hasNotes = false;
    for (auto tr : tab::tracks (score))
        for (auto b : tr)
            for (auto beat : b)
                hasNotes = hasNotes || beat.getNumChildren() > 0;
    if (! wholeScore || ! hasNotes)
    {
        applyImport (f, wholeScore);
        return;
    }
    juce::Component::SafePointer<TabPage> safe (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withTitle ("Import " + f.getFileName())
                                      .withMessage ("This project already has a tab score. Replace it with the file's score, or add the file's "
                                                    "tracks to the current score? (Both can be undone.)")
                                      .withButton ("Replace score")
                                      .withButton ("Add tracks")
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [safe, f] (int r) {
                                      if (safe != nullptr && (r == 1 || r == 2)) safe->applyImport (f, r == 1);
                                  });
}

void TabPage::applyImport (const juce::File& f, bool replace)
{
    if (host.engine.isPlaying()) host.engine.stop();
    doc.undo().beginNewTransaction ("Import " + f.getFileName());
    tab::ImportResult r;
    const int before = tab::tracks (score).getNumChildren();
    if (f.hasFileExtension ("gp3;gp4;gp5;gpx;gp"))
        r = tab::gp::importFile (score, f, replace, &doc.undo());
    else if (f.hasFileExtension ("musicxml;mxl;xml"))
        r = tab::musicxml::importFile (score, f, replace, &doc.undo());
    else if (f.hasFileExtension ("mid;midi"))
        r = tab::importMidi (score, f, &doc.undo());
    else if (f.getSize() > 4 * 1024 * 1024)
        r.result = juce::Result::fail ("That text file is too large to be a tab.");
    else
        r = tab::importAsciiTab (score, f.loadFileAsString(), f.getFileNameWithoutExtension(), &doc.undo());
    if (r.result.failed())
    {
        doc.undo().undoCurrentTransactionOnly();
        message ("Import failed", f.getFileName() + ":\n" + r.result.getErrorMessage());
        return;
    }
    refreshTracks();
    editor.setTrackIndex (replace ? 0 : before);
    editor.layoutChanged();
    refreshTracks();
    rebuildSequence();
    pushEngineOverrides();
    juce::String msg = (replace ? "Imported " : "Added ") + juce::String (r.tracksAdded) + (r.tracksAdded == 1 ? " track" : " tracks")
                       + " and " + juce::String (tab::numMeasures (score)) + " bars.";
    if (! r.warnings.isEmpty()) msg << "\n\n" << r.warnings.joinIntoString ("\n");
    message ("Import complete", msg);
}

void TabPage::exportDialog()
{
    juce::PopupMenu m;
    m.addItem (1, "MIDI file (.mid)...");
    m.addItem (2, "MusicXML (.musicxml)...");
    m.addItem (3, "PDF (notation + tab)...");
    m.addSeparator();
    m.addItem (4, "Print (opens the PDF in your viewer)...");
    juce::Component::SafePointer<TabPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [safe] (int r) {
        if (safe != nullptr && r > 0) safe->exportAs (r);
    });
}

void TabPage::exportAs (int kind)
{
    if (! tab::tracks (score).getNumChildren()) { message ("Nothing to export", "Add a tab track first."); return; }
    auto baseName = score[tids::title].toString().trim();
    if (baseName.isEmpty() || baseName == "Untitled tab") baseName = doc.tree()["name"].toString();
    baseName = juce::File::createLegalFileName (baseName.isEmpty() ? juce::String ("Score") : baseName);

    if (kind == 4)
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (baseName + " (print).pdf");
        ScorePdf::Options opt;
        auto r = ScorePdf::write (doc, host, score, f, opt);
        if (r.failed()) { message ("Print failed", r.getErrorMessage()); return; }
        if (! f.startAsProcess())
            message ("Print", "The PDF was written to\n" + f.getFullPathName() + "\nOpen it and print from your PDF viewer.");
        return;
    }
    const char* ext = kind == 1 ? ".mid" : kind == 2 ? ".musicxml" : ".pdf";
    const auto start = doc.hasLocation() ? doc.getBundle().getParentDirectory() : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> (kind == 1 ? "Export MIDI" : kind == 2 ? "Export MusicXML" : "Export PDF",
                                                   start.getChildFile (baseName + ext), juce::String ("*") + ext);
    juce::Component::SafePointer<TabPage> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, kind, ext] (const juce::FileChooser& fc) {
                              if (safe == nullptr || fc.getResult() == juce::File()) return;
                              const auto f = fc.getResult().withFileExtension (ext);
                              juce::Result r = juce::Result::ok();
                              if (kind == 1) r = tab::exportMidi (safe->score, f);
                              else if (kind == 2) r = tab::musicxml::exportFile (safe->score, f);
                              else
                              {
                                  ScorePdf::Options opt;
                                  r = ScorePdf::write (safe->doc, safe->host, safe->score, f, opt);
                              }
                              safe->message (r.wasOk() ? "Export complete" : "Export failed", r.wasOk() ? "Wrote " + f.getFullPathName() : r.getErrorMessage());
                          });
}

void TabPage::showBarMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Insert bar after");
    m.addItem (2, "Insert bar before");
    m.addItem (3, "Delete bar", tab::numMeasures (score) > 1);
    m.addSeparator();
    m.addItem (4, "Tempo, key and marker...");
    juce::Component::SafePointer<TabPage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&barButton), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        if (r == 1) safe->editor.insertMeasure (true);
        else if (r == 2) safe->editor.insertMeasure (false);
        else if (r == 3) safe->editor.deleteMeasure();
        else if (r == 4) { safe->showBarDialog(); return; }
        safe->editor.grabKeyboardFocus();
    });
}

void TabPage::showBarDialog()
{
    auto meas = tab::measures (score).getChild (editor.cursor.bar);
    if (! meas.isValid()) return;
    const int bar = editor.cursor.bar;
    auto* w = new juce::AlertWindow ("Bar " + juce::String (bar + 1), "Leave the tempo empty for no change at this bar.",
                                     juce::MessageBoxIconType::NoIcon, this);
    const double t = (double) meas[tids::tempo];
    w->addTextEditor ("tempo", t > 0.0 ? juce::String (t, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") : juce::String(),
                      bar == 0 ? "Tempo (bar 1 uses the score tempo)" : "Tempo change (BPM)");
    juce::StringArray keys;
    const char* names[] = { "Cb major / Ab minor (7 flats)", "Gb / Eb m (6 flats)", "Db / Bb m (5 flats)", "Ab / F m (4 flats)", "Eb / C m (3 flats)",
                            "Bb / G m (2 flats)", "F / D m (1 flat)", "C major / A minor", "G / E m (1 sharp)", "D / B m (2 sharps)",
                            "A / F# m (3 sharps)", "E / C# m (4 sharps)", "B / G# m (5 sharps)", "F# / D# m (6 sharps)", "C# / A# m (7 sharps)" };
    for (auto* n : names) keys.add (n);
    w->addComboBox ("key", keys, "Key signature from this bar");
    w->getComboBoxComponent ("key")->setSelectedItemIndex (tab::measureKey (score, bar) + 7);
    w->addTextEditor ("marker", meas[tids::marker].toString(), "Section marker (e.g. Verse, Chorus)");
    w->addButton ("Apply", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TabPage> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, meas, bar] (int r) mutable {
        if (safe == nullptr || r != 1) return;
        auto& d = safe->doc;
        d.undo().beginNewTransaction ("Bar settings");
        const auto tempoText = w->getTextEditorContents ("tempo").trim();
        if (bar == 0 && tempoText.getDoubleValue() >= 10.0) d.set (safe->score, tids::tempo, juce::jlimit (10.0, 999.0, tempoText.getDoubleValue()));
        else if (tempoText.getDoubleValue() >= 10.0) d.set (meas, tids::tempo, juce::jlimit (10.0, 999.0, tempoText.getDoubleValue()));
        else if (meas.hasProperty (tids::tempo)) meas.removeProperty (tids::tempo, &d.undo());
        const int key = w->getComboBoxComponent ("key")->getSelectedItemIndex() - 7;
        const int before = bar > 0 ? tab::measureKey (safe->score, bar - 1) : 0;
        if (key != before || bar == 0) d.set (meas, tids::key, key);
        else if (meas.hasProperty (tids::key)) meas.removeProperty (tids::key, &d.undo());
        d.set (meas, tids::marker, w->getTextEditorContents ("marker").trim());
        safe->pushEngineOverrides();
    }), true);
}

void TabPage::showBeatTextDialog()
{
    auto beat = tab::bar (editor.track(), editor.cursor.bar).getChild (editor.cursor.beat);
    if (! beat.isValid()) { message ("Text", "Move the cursor onto a beat first."); return; }
    auto* w = new juce::AlertWindow ("Beat text", "Bar " + juce::String (editor.cursor.bar + 1) + ", beat " + juce::String (editor.cursor.beat + 1)
                                                   + ". End a syllable with \"-\" when the word continues.",
                                     juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("chord", beat[tids::chord].toString(), "Chord name (shown with a diagram of the beat's notes)");
    w->addTextEditor ("lyric", beat[tids::lyric].toString(), "Lyric");
    w->addTextEditor ("text", beat[tids::text].toString(), "Text / instruction");
    w->addButton ("Apply", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TabPage> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, beat] (int r) mutable {
        if (safe == nullptr || r != 1) return;
        auto& d = safe->doc;
        d.undo().beginNewTransaction ("Beat text");
        const auto chord = w->getTextEditorContents ("chord").trim();
        if (chord != beat[tids::chord].toString())
        {
            d.set (beat, tids::chord, chord);
            if (beat.hasProperty (tids::chordFrets)) beat.removeProperty (tids::chordFrets, &d.undo()); // diagram follows the notes again
        }
        d.set (beat, tids::lyric, w->getTextEditorContents ("lyric").trim());
        d.set (beat, tids::text, w->getTextEditorContents ("text").trim());
        safe->editor.grabKeyboardFocus();
    }), true);
}

// ------------------------------------------------------------------ MIDI keyboard entry

void TabPage::setMidiInput (bool enabled)
{
    auto& dm = host.deviceManager;
    if (enabled == midiEnabled) return;
    midiEnabled = enabled;
    if (enabled)
    {
        const auto devices = juce::MidiInput::getAvailableDevices();
        for (auto& dev : devices)
            dm.setMidiInputDeviceEnabled (dev.identifier, true);
        dm.addMidiInputDeviceCallback ({}, this);
        if (devices.isEmpty())
            message ("MIDI input", "No MIDI input devices were found. Connect a MIDI keyboard or MIDI guitar interface and turn MIDI In on again.");
        midiButton.setToggleState (! devices.isEmpty(), juce::dontSendNotification);
        if (devices.isEmpty()) { dm.removeMidiInputDeviceCallback ({}, this); midiEnabled = false; }
    }
    else
        dm.removeMidiInputDeviceCallback ({}, this);
}

void TabPage::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    if (! m.isNoteOn()) return;
    const int note = m.getNoteNumber();
    juce::Component::SafePointer<TabPage> safe (this);
    juce::MessageManager::callAsync ([safe, note] { if (safe != nullptr) safe->midiNoteOn (note); });
}

void TabPage::midiNoteOn (int note)
{
    if (! active || ! editor.track().isValid()) return;
    const auto now = juce::Time::getMillisecondCounter();
    const bool sameChord = ! midiChord.empty() && now - lastMidiMs < 90;  // notes struck together form one beat
    lastMidiMs = now;
    if (! sameChord) midiChord.clear();
    midiChord.push_back (note);
    editor.enterPitches (midiChord, ! sameChord);
}

// ------------------------------------------------------------------ layout

void TabPage::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    g.setColour (palette::panel);
    g.fillRect (getLocalBounds().removeFromTop (84));
    g.setColour (palette::outline);
    g.drawHorizontalLine (42, 0.0f, (float) getWidth());
    g.drawHorizontalLine (84, 0.0f, (float) getWidth());
}

void TabPage::resized()
{
    auto r = getLocalBounds();
    auto row1 = r.removeFromTop (42).reduced (8, 7);
    const bool roomy = row1.getWidth() >= 1460;
    tempoLabel.setVisible (roomy);
    speedLabel.setVisible (roomy);
    if (roomy && tempoLabel.getParentComponent() == nullptr) { addAndMakeVisible (tempoLabel); addAndMakeVisible (speedLabel); }
    auto take = [&row1] (juce::Component& c, int w, int gap) { c.setBounds (row1.removeFromLeft (w)); row1.removeFromLeft (gap); };
    take (trackCombo, roomy ? 220 : 150, 4);
    take (addTrackButton, 64, 2);
    take (tuningButton, 64, 2);
    take (trackMenuButton, 56, 8);
    if (roomy) take (tempoLabel, 44, 0);
    take (tempoSlider, 84, 6);
    if (roomy) take (speedLabel, 44, 0);
    take (speedCombo, 70, 8);
    take (playButton, 92, 3);
    take (loopButton, 46, 3);
    take (loopSongButton, 72, 3);
    take (clickButton, 46, 3);
    take (countButton, 62, 8);
    take (drumsButton, 54, 3);
    take (notationButton, 70, 3);
    take (midiButton, 60, 8);
    exportButton.setBounds (row1.removeFromRight (juce::jmax (60, juce::jmin (70, (row1.getWidth() - 4) / 2))));
    row1.removeFromRight (4);
    importButton.setBounds (row1.removeFromRight (juce::jmax (60, juce::jmin (70, row1.getWidth()))));

    auto row2 = r.removeFromTop (42).reduced (8, 8);
    for (auto* b : durButtons) { b->setBounds (row2.removeFromLeft (52)); row2.removeFromLeft (2); }
    row2.removeFromLeft (4);
    dotButton.setBounds (row2.removeFromLeft (26)); row2.removeFromLeft (2);
    tripletButton.setBounds (row2.removeFromLeft (26)); row2.removeFromLeft (2);
    restButton.setBounds (row2.removeFromLeft (44));
    row2.removeFromLeft (10);
    transUpButton.setBounds (row2.removeFromRight (30)); row2.removeFromRight (2);
    transDownButton.setBounds (row2.removeFromRight (30)); row2.removeFromRight (8);
    sigCombo.setBounds (row2.removeFromRight (66)); row2.removeFromRight (4);
    textButton.setBounds (row2.removeFromRight (50)); row2.removeFromRight (2);
    barButton.setBounds (row2.removeFromRight (48)); row2.removeFromRight (10);
    techMenuButton.setBounds (row2.removeFromRight (juce::jmin (124, row2.getWidth() / 4))); row2.removeFromRight (6);
    const int tw = juce::jmax (40, (row2.getWidth() - 2 * techButtons.size()) / juce::jmax (1, techButtons.size()));
    for (auto* b : techButtons) { b->setBounds (row2.removeFromLeft (juce::jmin (tw, 104))); row2.removeFromLeft (2); }

    status.setBounds (r.removeFromBottom (24).reduced (10, 0));
    viewport.setBounds (r);
    editor.setSize (r.getWidth() - viewport.getScrollBarThickness(), juce::jmax (editor.getHeight(), 100));
}
} // namespace ff
