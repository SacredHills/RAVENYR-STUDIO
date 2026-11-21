#include "RigEditor.h"
#include "engine/fx/NamModel.h"
#include "../../tone3000/Tone3000.h"

namespace ff
{
using namespace ff::rig;
namespace tlids = ff::timeline::ids;

namespace
{
    const char* blockHelpText (int b)
    {
        switch (b)
        {
            case gate:   return "Mutes hiss and hum between notes. Raise the threshold until the noise disappears while you are not playing.";
            case comp:   return "Evens out picking dynamics and adds sustain. Mostly for clean and funk sounds, before the amp.";
            case drive:  return "A mid-focused overdrive pedal. With low drive and high level it tightens a high-gain amp.";
            case amp:    return "Load a Neural Amp Modeler capture (.nam) of a real amp or pedal, or use the built-in amp.";
            case eq:     return "Tone controls after the amp: bass 120 Hz, mid 650 Hz, treble 2.8 kHz, presence 5.5 kHz.";
            case cab:    return "A speaker cabinet. Load an impulse response (.wav) for a real cabinet; most NAM amp captures need one.";
            case chorus: return "Stereo chorus. From here on the signal is stereo.";
            case delay:  return "Echo with darker repeats as the tone is lowered. The right side repeats 11 ms later for width.";
            case reverb: return "Room / hall reverb.";
            case plug1: case plug2: case plug3: case plug4:
                return "A plugin in the chain (stereo). Drag its tile to move it; the lamp bypasses it. Settings made in its window are saved with the project.";
            default:     return "";
        }
    }

    juce::String shortPath (const juce::File& f, const juce::File& base)
    {
        return f.isAChildOf (base) ? f.getRelativePathFrom (base) : f.getFileName();
    }
}

// ================================================================ ParamKnob

ParamKnob::ParamKnob()
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (slider);
    for (auto* l : { &name, &value })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    }
    styleLabel (name, 12.5f, palette::text, true);
    styleLabel (value, 12.0f, palette::textDim);
    slider.onDragStart = [this] { if (undo != nullptr) undo->beginNewTransaction ("Change " + juce::String (info (p).label)); };
    slider.onValueChange = [this] {
        if (updating || ! tree.isValid()) return;
        set (tree, p, (float) slider.getValue(), undo);
        value.setText (formatValue (p, (float) slider.getValue()), juce::dontSendNotification);
    };
}

void ParamKnob::bind (juce::ValueTree t, int param, juce::UndoManager* u)
{
    tree = t;
    p = param;
    undo = u;
    const auto& pi = info (p);
    const juce::ScopedValueSetter<bool> svs (updating, true);
    slider.setRange (pi.min, pi.max, 0.0);
    if (std::abs (pi.centre - (pi.min + pi.max) * 0.5f) > 1.0e-3f)
        slider.setSkewFactorFromMidPoint (pi.centre);
    else
        slider.setSkewFactor (1.0);
    slider.setDoubleClickReturnValue (true, pi.def);
    name.setText (pi.label, juce::dontSendNotification);
    refresh();
}

void ParamKnob::refresh()
{
    if (! tree.isValid()) return;
    const juce::ScopedValueSetter<bool> svs (updating, true);
    const float v = get (tree, p);
    slider.setValue (v, juce::dontSendNotification);
    value.setText (formatValue (p, v), juce::dontSendNotification);
}

void ParamKnob::resized()
{
    auto r = getLocalBounds();
    name.setBounds (r.removeFromTop (16));
    value.setBounds (r.removeFromBottom (16));
    slider.setBounds (r.reduced (4, 0));
}

// ================================================================ ChainView

std::vector<int> ChainView::visible() const
{
    std::vector<int> v;
    if (! tree.isValid()) return v;
    for (int b : getOrder (tree))
        if (b == eq) continue;   // folded into the amp
        else if (! isPluginBlock (b) || pluginNode (tree, pluginSlotOf (b)).isValid()) v.push_back (b);
    return v;
}

juce::Rectangle<float> ChainView::tileRect (int index) const
{
    const int n = juce::jmax (1, numTiles());
    const float gap = 12.0f;
    const float w = ((float) getWidth() - gap * (float) (n - 1)) / (float) n;
    return { (float) index * (w + gap), 0.0f, w, (float) getHeight() };
}

int ChainView::indexAt (float x) const
{
    const int n = numTiles();
    for (int i = 0; i < n; ++i)
        if (x < tileRect (i).getRight() + 6.0f) return i;
    return juce::jmax (0, n - 1);
}

void ChainView::paint (juce::Graphics& g)
{
    if (! tree.isValid()) return;
    const auto vis = visible();
    for (int i = 0; i < (int) vis.size(); ++i)
    {
        const int b = vis[(size_t) i];
        auto r = tileRect (i);
        if (i > 0)
        {
            g.setColour (palette::outline);
            const float y = r.getCentreY();
            g.drawArrow ({ r.getX() - 11.0f, y, r.getX() - 1.0f, y }, 1.5f, 6.0f, 5.0f);
        }
        const bool on = get (tree, switchFor (b)) >= 0.5f;
        const bool sel = b == selected;
        g.setColour (b == dragBlock ? palette::panelHi.brighter (0.1f) : (sel ? palette::panelHi : palette::panel));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (sel ? palette::accent : (isPluginBlock (b) ? palette::info.withAlpha (0.6f) : palette::outline));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, sel ? 2.0f : 1.0f);

        auto lamp = lampRect (i);
        g.setColour (on ? palette::good : palette::well);
        g.fillEllipse (lamp);
        g.setColour (palette::outline);
        g.drawEllipse (lamp, 1.0f);

        auto text = r.reduced (8.0f, 6.0f);
        g.setColour (on ? palette::text : palette::textDim);
        g.setFont (uiFont (13.5f, true));
        g.drawText (blockLabel (tree, b), text.removeFromTop (20.0f).withTrimmedRight (16.0f), juce::Justification::centredLeft, true);
        g.setFont (uiFont (11.5f));
        g.setColour (palette::textDim);
        juce::String sub;
        if (b == amp) sub = ampText;
        else if (b == cab) sub = cabText;
        else if (isPluginBlock (b)) sub = pluginText[(size_t) pluginSlotOf (b)];
        else if (b == gate) sub = formatValue (gateThresh, get (tree, gateThresh));
        else if (b == drive) sub = "Drive " + formatValue (driveGain, get (tree, driveGain));
        else if (b == delay) sub = formatValue (delayTime, get (tree, delayTime));
        else if (b == reverb) sub = "Mix " + formatValue (reverbMix, get (tree, reverbMix));
        else if (b == chorus) sub = formatValue (chorusRate, get (tree, chorusRate));
        else if (b == comp) sub = formatValue (compRatio, get (tree, compRatio));
        else if (b == eq) sub = "4-band";
        g.drawFittedText (sub, text.toNearestInt(), juce::Justification::topLeft, 3, 0.9f);
    }
    if (canAddPlugin)
    {
        auto r = tileRect ((int) vis.size());
        g.setColour (palette::outline);
        const float dashes[] = { 4.0f, 4.0f };
        juce::Path p;
        p.addRoundedRectangle (r.reduced (1.0f), 6.0f);
        juce::Path dashed;
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dashes, 2);
        g.fillPath (dashed);
        g.setColour (palette::textDim);
        g.setFont (uiFont (13.0f, true));
        g.drawFittedText ("+ Plugin\n(" + pluginFormatsLabel() + ")", r.toNearestInt(), juce::Justification::centred, 2);
    }
    if (dragBlock >= 0 && dropIndex >= 0)
    {
        auto r = tileRect (dropIndex);
        g.setColour (palette::accent);
        g.fillRect (juce::Rectangle<float> (r.getX() - 7.0f, 0.0f, 3.0f, (float) getHeight()));
    }
}

void ChainView::mouseDown (const juce::MouseEvent& e)
{
    if (! tree.isValid()) return;
    const auto vis = visible();
    const int i = indexAt ((float) e.x);
    dragBlock = -1;
    dropIndex = -1;
    if (i >= (int) vis.size()) { if (canAddPlugin && onAddPlugin) onAddPlugin(); return; }
    const int b = vis[(size_t) i];
    if (e.mods.isPopupMenu()) { if (onContext) onContext (b); return; }
    if (lampRect (i).expanded (4.0f).contains (e.position)) { if (onToggle) onToggle (b); return; }
    if (onSelect) onSelect (b);
}

void ChainView::mouseDrag (const juce::MouseEvent& e)
{
    if (! tree.isValid() || std::abs (e.getDistanceFromDragStartX()) < 8) return;
    const auto vis = visible();
    const int from = indexAt ((float) e.getMouseDownX());
    if (from >= (int) vis.size()) return;
    dragBlock = vis[(size_t) from];
    dropIndex = juce::jmin (indexAt ((float) e.x), (int) vis.size() - 1);
    repaint();
}

void ChainView::mouseUp (const juce::MouseEvent&)
{
    if (dragBlock >= 0 && dropIndex >= 0 && onMove)
    {
        // a position among the shown tiles -> a position in the full order (hidden empty slots stay where they are)
        const auto vis = visible();
        const auto order = getOrder (tree);
        const int targetBlock = vis[(size_t) juce::jlimit (0, (int) vis.size() - 1, dropIndex)];
        int full = 0;
        for (int i = 0; i < numBlocks; ++i) if (order[(size_t) i] == targetBlock) full = i;
        onMove (dragBlock, full);
    }
    dragBlock = dropIndex = -1;
    repaint();
}

// ================================================================ RigMeter

void RigMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (palette::well);
    g.fillRoundedRectangle (r, 2.0f);
    const float n = juce::jlimit (0.0f, 1.0f, (level + 60.0f) / 60.0f);
    g.setColour (level > -1.0f ? palette::bad : (level > -9.0f ? palette::warn : palette::good));
    g.fillRoundedRectangle (r.withWidth (r.getWidth() * n), 2.0f);
}

// ================================================================ RigEditor

RigEditor::RigEditor (ProjectDocument& d, EngineHost& h, SessionController& s, juce::PropertiesFile& st, const juce::String& trackId)
    : doc (d), host (h), session (s), settings (st), targetTrackId (trackId), trackMode (trackId.isNotEmpty())
{
    for (auto* c : { &inputCombo, &presetCombo })
        addAndMakeVisible (c);
    styleLabel (titleLabel, 16.0f, palette::text, true);
    addAndMakeVisible (titleLabel);
    inputCombo.setTooltip ("Input the live rig plays");
    inputCombo.onChange = [this] {
        if (targetTrackId.isEmpty() && target.isValid())
            target.setProperty (ids::input, inputCombo.getSelectedId() - 2, nullptr);
    };
    presetCombo.setTextWhenNothingSelected ("Presets");
    presetCombo.onChange = [this] { applyPresetId (presetCombo.getSelectedId()); };

    onButton.setClickingTogglesState (false);
    onButton.onClick = [this] {
        if (! target.isValid()) return;
        setOnTarget (ids::enabled, ! rig::isEnabled (target), targetTrackId.isEmpty() ? "" : "Amp rig on/off");
    };
    monitorButton.setButtonText ("Monitor input");
    monitorButton.setTooltip ("Hear the track's input through its rig (same as the track's monitor button in the Editor)");
    monitorButton.onClick = [this] {
        auto t = session.findTrack (targetTrackId);
        if (t.isValid()) doc.set (t, tlids::monitor, ! (bool) t[tlids::monitor]);
    };
    addRigButton.onClick = [this] {
        auto t = session.findTrack (targetTrackId);
        if (! t.isValid()) return;
        doc.undo().beginNewTransaction ("Add amp rig");
        auto r = rig::find (t);
        if (! r.isValid()) { r = makeDefault(); t.appendChild (r, &doc.undo()); }
        else r.setProperty (ids::enabled, true, &doc.undo());
        selectTarget (targetTrackId);
    };
    savePresetButton.onClick = [this] { savePreset(); };
    deletePresetButton.onClick = [this] { deletePreset(); };
    resetButton.setTooltip ("All controls back to their defaults (keeps the model and IR)");
    resetButton.onClick = [this] {
        if (! target.isValid()) return;
        if (auto* u = um()) u->beginNewTransaction ("Reset rig");
        for (int p = 0; p < numParams; ++p) set (target, p, info (p).def, um());
        setOrder (target, defaultOrder(), um());
        target.setProperty (ids::rigName, juce::String(), um());
    };
    copyButton.setTooltip ("Copy this whole amp rig (model, cabinet, pedals, settings, plugins) so you can paste it onto another track");
    copyButton.onClick = [this] { copyRig(); };
    pasteButton.setTooltip ("Paste a copied amp rig over this one");
    pasteButton.onClick = [this] { pasteRig(); };
    for (auto* b : { &onButton, &monitorButton, &addRigButton, &savePresetButton, &deletePresetButton, &resetButton, &copyButton, &pasteButton })
        addAndMakeVisible (b);
    delayStereoButton.setTooltip ("Off: the repeats stay centred. On: the right repeats come a little later, which sounds wider");
    delayStereoButton.onClick = [this] {
        if (! target.isValid()) return;
        if (auto* u = um()) u->beginNewTransaction ("Delay stereo");
        set (target, delayStereo, get (target, delayStereo) >= 0.5f ? 0.0f : 1.0f, um());
    };
    addChildComponent (delayStereoButton);
    chain.onContext = [this] (int b) { blockMenu (b); };
    loadIr2Button.setTooltip ("Load a second cabinet and blend the two with the Cab blend knob");
    loadIr2Button.onClick = [this] {
        const auto start = juce::File (settings.getValue ("irFolder", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName()));
        chooser = std::make_unique<juce::FileChooser> ("Load the second cabinet impulse response", start, "*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) setIrFile (fc.getResult(), true); });
    };
    clearIr2Button.onClick = [this] { setIrFile ({}, true); };
    for (auto [btn, param] : { std::pair<juce::TextButton*, int> { &phase1Button, cabPhase1 }, { &phase2Button, cabPhase2 } })
    {
        btn->setTooltip ("Flip this cabinet's polarity. When two cabs sound thin or hollow together, flipping one brings the low end back");
        btn->onClick = [this, param = param] {
            if (! target.isValid()) return;
            if (auto* u = um()) u->beginNewTransaction ("Cabinet phase");
            set (target, param, get (target, param) >= 0.5f ? 0.0f : 1.0f, um());
        };
        addChildComponent (btn);
    }
    for (auto* l : { &cab1Label, &cab2Label }) { styleLabel (*l, 13.0f, palette::text, true); addChildComponent (l); }
    models.extraFolder = irs.extraFolder = t3k::Client::libraryFolder();
    models.rescan();
    irs.rescan();
    addChildComponent (loadIr2Button);
    addChildComponent (clearIr2Button);

    for (auto* l : { &targetHint, &statusLabel, &inLabel, &outLabel, &blockTitle, &blockHelp, &modelLabel, &modelInfo, &irLabel })
        addAndMakeVisible (l);
    styleLabel (targetHint, 12.5f);
    styleLabel (statusLabel, 12.5f);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    styleLabel (inLabel, 11.5f);
    styleLabel (outLabel, 11.5f);
    inLabel.setText ("IN", juce::dontSendNotification);
    outLabel.setText ("OUT", juce::dontSendNotification);
    styleLabel (blockTitle, 17.0f, palette::text, true);
    styleLabel (blockHelp, 12.5f);
    styleLabel (modelLabel, 15.0f, palette::accent, true);
    styleLabel (modelInfo, 12.5f);
    modelInfo.setJustificationType (juce::Justification::topLeft);
    styleLabel (irLabel, 13.0f, palette::text);
    irLabel.setJustificationType (juce::Justification::topLeft);

    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);
    addAndMakeVisible (inKnob);
    addAndMakeVisible (outKnob);

    addAndMakeVisible (chain);
    chain.onSelect = [this] (int b) { selectBlock (b); };
    chain.onToggle = [this] (int b) {
        if (! target.isValid()) return;
        if (auto* u = um()) u->beginNewTransaction (juce::String (blockName (b)) + " on/off");
        set (target, switchFor (b), get (target, switchFor (b)) >= 0.5f ? 0.0f : 1.0f, um());
        selectBlock (b);
    };
    chain.onMove = [this] (int b, int newIndex) {
        if (! target.isValid()) return;
        auto o = getOrder (target);
        std::vector<int> v (o.begin(), o.end());
        v.erase (std::find (v.begin(), v.end(), b));
        v.insert (v.begin() + juce::jlimit (0, (int) v.size(), newIndex), b);
        std::copy (v.begin(), v.end(), o.begin());
        if (auto* u = um()) u->beginNewTransaction ("Move " + juce::String (blockName (b)));
        setOrder (target, o, um());
    };

    blockOnButton.onClick = [this] { if (chain.onToggle) chain.onToggle (chain.selected); };
    moveLeftButton.setTooltip ("Move this block earlier in the chain");
    moveRightButton.setTooltip ("Move this block later in the chain");
    // move past hidden (empty) plugin slots so every click visibly moves the block
    auto stepBlock = [this] (int dir) {
        if (! target.isValid()) return;
        if (auto* u = um()) u->beginNewTransaction ("Move block");
        for (int guard = 0; guard < numBlocks; ++guard)
        {
            const auto before = getOrder (target);
            moveBlock (target, chain.selected, dir, um());
            const auto after = getOrder (target);
            if (after == before) break;
            int pos = 0;
            for (int i = 0; i < numBlocks; ++i) if (after[(size_t) i] == chain.selected) pos = i;
            const int neighbour = after[(size_t) juce::jlimit (0, numBlocks - 1, pos - dir)];
            if (neighbour != eq && (! isPluginBlock (neighbour) || pluginNode (target, pluginSlotOf (neighbour)).isValid())) break;
        }
    };
    moveLeftButton.onClick = [stepBlock] { stepBlock (-1); };
    moveRightButton.onClick = [stepBlock] { stepBlock (+1); };

    chain.onAddPlugin = [this] { if (target.isValid()) showAddPluginMenu (freePluginSlot (target)); };
    pluginOpenButton.onClick = [this] {
        if (plugins == nullptr || ! isPluginBlock (chain.selected)) return;
        plugins->showEditor (session.rigs().pluginFor (target, pluginSlotOf (chain.selected)),
                             blockLabel (target, chain.selected) + "  (" + titleLabel.getText() + ")");
    };
    pluginReplaceButton.onClick = [this] { if (isPluginBlock (chain.selected)) showAddPluginMenu (pluginSlotOf (chain.selected)); };
    pluginRemoveButton.onClick = [this] {
        if (! target.isValid() || ! isPluginBlock (chain.selected)) return;
        if (auto* u = um()) u->beginNewTransaction ("Remove plugin");
        removePlugin (target, pluginSlotOf (chain.selected), um());
        selectBlock (amp);
    };
    pluginManagerButton.onClick = [this] { if (plugins != nullptr) plugins->showManager(); };
    styleLabel (pluginInfo, 13.0f, palette::text);
    pluginInfo.setJustificationType (juce::Justification::topLeft);
    addChildComponent (pluginInfo);
    for (auto* b : { &pluginOpenButton, &pluginReplaceButton, &pluginRemoveButton, &pluginManagerButton })
        addChildComponent (b);
    for (auto* b : { &blockOnButton, &moveLeftButton, &moveRightButton, &loadModelButton, &clearModelButton,
                     &normaliseButton, &calibrateButton, &loadIrButton, &clearIrButton })
        addChildComponent (b);

    loadModelButton.onClick = [this] {
        const auto start = juce::File (settings.getValue ("namFolder", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName()));
        chooser = std::make_unique<juce::FileChooser> ("Load a Neural Amp Modeler model", start, "*.nam");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) setModelFile (fc.getResult()); });
    };
    clearModelButton.onClick = [this] { setModelFile ({}); };
    calibrateButton.setTooltip ("Matches your interface's input level to the level the capture was recorded at, so the amp's gain "
                                "responds like the real one (needs a capture that stores its input level)");
    calibrateButton.onClick = [this] {
        if (! target.isValid()) return;
        if (auto* u = um()) u->beginNewTransaction ("Calibrate input");
        set (target, namCalibrate, get (target, namCalibrate) >= 0.5f ? 0.0f : 1.0f, um());
    };
    normaliseButton.setTooltip ("Levels every model to the same loudness (uses the loudness stored in the .nam file)");
    normaliseButton.onClick = [this] {
        if (! target.isValid()) return;
        if (auto* u = um()) u->beginNewTransaction ("Normalise loudness");
        set (target, namNormalize, get (target, namNormalize) >= 0.5f ? 0.0f : 1.0f, um());
    };
    loadIrButton.onClick = [this] {
        const auto start = juce::File (settings.getValue ("irFolder", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName()));
        chooser = std::make_unique<juce::FileChooser> ("Load a cabinet impulse response", start, "*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) setIrFile (fc.getResult()); });
    };
    clearIrButton.onClick = [this] { setIrFile ({}); };

    models.onContext = [this] (const juce::File& f) { fileMenu (f, false); };
    irs.onContext = [this] (const juce::File& f) { fileMenu (f, true); };
    models.onPick = [this] (const juce::File& f) { setModelFile (f); };
    irs.onPick = [this] (const juce::File& f) { setIrFile (f); };
    for (auto* b : { &models, &irs })
    {
        b->extraButton.setButtonText ("TONE3000...");
        b->extraButton.setTooltip ("Browse and download from TONE3000");
        addChildComponent (b);
    }
    models.extraButton.onClick = [this] { if (onBrowseTone3000) onBrowseTone3000 (false); };
    irs.extraButton.onClick = [this] { if (onBrowseTone3000) onBrowseTone3000 (true); };

    session.rigs().onStatusChanged = [this] { refreshStatus(); };
    rebuildPresets();
    rebind();
    startTimerHz (30);
}

RigEditor::~RigEditor()
{
    session.rigs().onStatusChanged = nullptr;
    root.removeListener (this);
    liveTree.removeListener (this);
}

void RigEditor::rebind()
{
    root.removeListener (this);
    root = doc.tree();
    root.addListener (this);
    liveTree.removeListener (this);
    liveTree = session.rigs().liveTree();
    liveTree.addListener (this);
    if (trackMode && ! session.findTrack (targetTrackId).isValid())
    {
        target = {};
        if (onTrackGone) juce::MessageManager::callAsync ([cb = onTrackGone] { cb(); });
        return;
    }
    selectTarget (targetTrackId);
}

void RigEditor::setActive (bool a)
{
    active = a;
    if (a) selectTarget (targetTrackId);
}

juce::String RigEditor::windowTitle() const
{
    if (! trackMode) return "Live rig";
    auto t = session.findTrack (targetTrackId);
    return "NAM amp - " + (t.isValid() ? t[tlids::name].toString() : juce::String ("track"));
}

void RigEditor::rebuildInputs()
{
    if (trackMode)
    {
        auto t = session.findTrack (targetTrackId);
        const int idx = doc.tracks().indexOf (t);
        titleLabel.setText ("Track " + juce::String (idx + 1) + ": " + t[tlids::name].toString(), juce::dontSendNotification);
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setName (windowTitle());
    }
    else titleLabel.setText ("Live input (practice rig)", juce::dontSendNotification);

    inputCombo.clear (juce::dontSendNotification);
    inputCombo.addItem ("Tuner input", 1);
    const auto names = host.activeInputChannelNames();
    for (int c = 0; c < names.size(); ++c) inputCombo.addItem ("In " + juce::String (c + 1) + ": " + names[c], c + 2);
    inputCombo.setSelectedId ((int) session.rigs().liveTree().getProperty (ids::input, -1) + 2, juce::dontSendNotification);
}

void RigEditor::selectTarget (const juce::String& trackId)
{
    targetTrackId = trackId;
    if (trackId.isEmpty()) target = session.rigs().liveTree();
    else
    {
        auto r = rig::find (session.findTrack (trackId));
        target = rig::isEnabled (r) ? r : juce::ValueTree();
    }
    rebuildInputs();
    chain.setRig (target);
    inKnob.bind (target, inGain, um());
    outKnob.bind (target, outGain, um());
    selectBlock (chain.selected);
    refreshAll();
}

void RigEditor::selectBlock (int b)
{
    chain.selected = b;
    knobs.clear();
    if (target.isValid())
        for (int p = 0; p < numParams; ++p)
        {
            const auto& pi = info (p);
            if ((pi.block != b && ! (b == amp && pi.block == eq)) || pi.isSwitch || p == namNormalize) continue;
            if (p == delayStereo || p == cabPhase1 || p == cabPhase2) continue;   // buttons
            if (p == cabBlend && target[ids::irFile2].toString().isEmpty()) continue;
            if (p == ampGain && ! target[ids::namFile].toString().isEmpty()) continue;   // built-in amp only
            if (p == calLevel && (target[ids::namFile].toString().isEmpty() || get (target, namCalibrate) < 0.5f)) continue;
            if (p == namCalibrate) continue;
            auto* k = knobs.add (new ParamKnob());
            k->bind (target, p, um());
            addAndMakeVisible (k);
        }
    refreshAll();
}

void RigEditor::refreshAll()
{
    const bool live = targetTrackId.isEmpty();
    const bool has = target.isValid();
    const auto status = session.rigs().statusFor (target);

    onButton.setVisible (has);
    onButton.setButtonText (live ? (rig::isEnabled (target) ? "Live monitoring ON" : "Live monitoring off")
                                 : (rig::isEnabled (target) ? "Rig ON" : "Rig off"));
    onButton.setToggleState (has && rig::isEnabled (target), juce::dontSendNotification);
    inputCombo.setVisible (live);
    auto track = session.findTrack (targetTrackId);
    monitorButton.setVisible (! live && has);
    monitorButton.setToggleState (track.isValid() && (bool) track[tlids::monitor], juce::dontSendNotification);
    addRigButton.setVisible (! live && ! has);
    for (auto* c : std::initializer_list<juce::Component*> { &presetCombo, &savePresetButton, &deletePresetButton, &resetButton, &copyButton, &pasteButton, &chain,
                                                             &inKnob, &outKnob, &inMeter, &outMeter, &inLabel, &outLabel, &blockTitle, &blockHelp })
        c->setVisible (has);

    targetHint.setText (live ? "Live rig: your guitar input -> rig -> speakers, whatever the transport is doing. Use headphones if you play through a microphone."
                             : (has ? "Track insert: shapes this track's recorded audio and its live monitoring. Recordings stay dry, so you can change the amp later."
                                    : "This track has no amp rig. Add one to play and record through an amp, or to re-amp what is already recorded."),
                        juce::dontSendNotification);

    // chain subtitles
    if (status.modelLoading) chain.ampText = "Loading model...";
    else if (status.modelLoaded) chain.ampText = "NAM: " + status.modelInfo.displayName();
    else if (status.modelError.isNotEmpty()) chain.ampText = "Model failed - built-in amp";
    else chain.ampText = "Built-in amp";
    if (status.irLoading) chain.cabText = "Loading IR...";
    else if (status.irLoaded) chain.cabText = "IR: " + status.irName;
    else if (status.irError.isNotEmpty()) chain.cabText = "IR failed - built-in";
    else chain.cabText = "Built-in speaker";
    chain.repaint();

    // selected block
    const int b = chain.selected;
    blockTitle.setText (has ? blockLabel (target, b) : juce::String (blockName (b)), juce::dontSendNotification);
    blockHelp.setText (blockHelpText (b), juce::dontSendNotification);
    const bool on = has && get (target, switchFor (b)) >= 0.5f;
    blockOnButton.setButtonText (on ? "On" : "Off");
    blockOnButton.setToggleState (on, juce::dontSendNotification);
    for (auto* c : { &blockOnButton, &moveLeftButton, &moveRightButton }) c->setVisible (has);

    const bool isAmp = has && b == amp, isCab = has && b == cab, isPlug = has && isPluginBlock (b);
    for (auto* c : std::initializer_list<juce::Component*> { &pluginOpenButton, &pluginReplaceButton, &pluginRemoveButton, &pluginInfo })
        c->setVisible (isPlug);
    pluginManagerButton.setVisible (isPlug && plugins != nullptr);   // only on a plugin's own page
    chain.canAddPlugin = has && plugins != nullptr && freePluginSlot (target) >= 0;
    for (int sl = 0; sl < numPluginSlots; ++sl)
    {
        const auto& ps = status.plugins[(size_t) sl];
        chain.pluginText[(size_t) sl] = ps.loading ? "Loading..." : (ps.error.isNotEmpty() ? "Not loaded" : (ps.loaded ? "Plugin" : ""));
    }
    if (isPlug)
    {
        const int sl = pluginSlotOf (b);
        const auto& ps = status.plugins[(size_t) sl];
        juce::String text;
        juce::PluginDescription d;
        if (auto xml = juce::parseXML (pluginNode (target, sl)[ids::desc].toString())) d.loadFromXml (*xml);
        text << d.name << (d.manufacturerName.isNotEmpty() ? "  by " + d.manufacturerName : juce::String())
             << (d.pluginFormatName.isNotEmpty() ? "  (" + d.pluginFormatName + (d.version.isNotEmpty() ? " " + d.version : juce::String()) + ")" : juce::String()) << "\n";
        if (ps.loading) text << "Loading...";
        else if (ps.error.isNotEmpty()) text << "Could not load it: " << ps.error << "\nThe slot passes the sound through unchanged.";
        else if (ps.loaded)
        {
            text << "Running";
            if (ps.latency > 0) text << ", latency " << ps.latency << " samples (compensated on playback)";
        }
        text << "\n" << d.fileOrIdentifier;
        pluginInfo.setText (text, juce::dontSendNotification);
        pluginInfo.setColour (juce::Label::textColourId, ps.error.isNotEmpty() ? palette::bad : palette::text);
        pluginOpenButton.setEnabled (ps.loaded && plugins != nullptr);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &loadModelButton, &clearModelButton, &normaliseButton, &calibrateButton, &modelLabel,
                                                             &modelInfo, &models })
        c->setVisible (isAmp);
    for (auto* c : std::initializer_list<juce::Component*> { &loadIrButton, &clearIrButton, &loadIr2Button, &clearIr2Button, &phase1Button, &phase2Button, &cab1Label, &cab2Label, &irLabel, &irs })
        c->setVisible (isCab);
    phase1Button.setToggleState (has && get (target, cabPhase1) >= 0.5f, juce::dontSendNotification);
    phase2Button.setToggleState (has && get (target, cabPhase2) >= 0.5f, juce::dontSendNotification);
    delayStereoButton.setVisible (has && b == delay);
    delayStereoButton.setToggleState (has && get (target, delayStereo) >= 0.5f, juce::dontSendNotification);
    for (auto* b : { &models, &irs })
        if (b->extraButton.isVisible() != (onBrowseTone3000 != nullptr)) { b->extraButton.setVisible (onBrowseTone3000 != nullptr); b->resized(); }
    normaliseButton.setToggleState (has && get (target, namNormalize) >= 0.5f, juce::dontSendNotification);
    calibrateButton.setToggleState (has && get (target, namCalibrate) >= 0.5f, juce::dontSendNotification);
    if (has)
    {
        models.setCurrent (juce::File (target[ids::namFile].toString().isEmpty() ? juce::String() : target[ids::namFile].toString()));
        irs.setCurrent (juce::File (target[ids::irFile].toString().isEmpty() ? juce::String() : target[ids::irFile].toString()));
    }

    if (isAmp)
    {
        juce::String text;
        if (status.modelLoading) modelLabel.setText ("Loading " + juce::File (status.modelPath).getFileName() + "...", juce::dontSendNotification);
        else if (status.modelLoaded) modelLabel.setText (status.modelInfo.displayName(), juce::dontSendNotification);
        else modelLabel.setText ("Built-in amp", juce::dontSendNotification);
        if (status.modelLoaded)
        {
            const auto& mi = status.modelInfo;
            juce::StringArray parts;
            if (mi.gearType.isNotEmpty()) parts.add (mi.gearType.substring (0, 1).toUpperCase() + mi.gearType.substring (1));
            if (mi.toneType.isNotEmpty()) parts.add (mi.toneType);
            parts.add (mi.architecture + (mi.modeledBy.isNotEmpty() ? ", captured by " + mi.modeledBy : juce::String()));
            text << parts.joinIntoString (" - ") << "\n";
            const double dev = host.engine.getSampleRate();
            text << "Trained at " << juce::String (mi.sampleRate / 1000.0, 1) << " kHz";
            if (dev > 0 && std::abs (dev - mi.sampleRate) > 0.5) text << ", resampled from " << juce::String (dev / 1000.0, 1) << " kHz";
            text << (mi.hasLoudness ? ", loudness " + juce::String (mi.loudness, 1) + " dB" : juce::String (", no loudness stored (normalising has no effect)"));
            text << "\n";
            if (mi.hasInputLevel)
            {
                text << "Captured with 0 dBFS = " << juce::String (mi.inputLevelDbu, 1) << " dBu at the input";
                if (get (target, namCalibrate) >= 0.5f)
                    text << "; calibrated: " << (get (target, calLevel) - mi.inputLevelDbu >= 0 ? "+" : "")
                         << juce::String (get (target, calLevel) - mi.inputLevelDbu, 1) << " dB into the model";
            }
            else text << "No input level stored in this capture: calibration has no effect";
            text << "\n" << status.modelPath;
        }
        else if (status.modelError.isNotEmpty())
            text << "Could not load the model: " << status.modelError << "\nThe built-in amp is used instead.";
        else
            text << "Two-stage overdriven preamp. For the sound of a real amp, load a NAM capture (.nam): thousands are free on TONE3000 (tone3000.com).";
        modelInfo.setText (text, juce::dontSendNotification);
        modelInfo.setColour (juce::Label::textColourId, status.modelError.isNotEmpty() ? palette::bad : palette::textDim);
        clearModelButton.setEnabled (status.modelPath.isNotEmpty());
        calibrateButton.setEnabled (status.modelLoaded);
    }
    if (isCab)
    {
        juce::String text;
        if (status.irLoading) text = "Loading " + juce::File (status.irPath).getFileName() + "...";
        else if (status.irLoaded) text = "Impulse response: " + status.irName + "  (" + juce::String (status.irSeconds * 1000.0, 0) + " ms)\n" + status.irPath;
        else if (status.irError.isNotEmpty()) text = "Could not load the IR: " + status.irError + "\nThe built-in speaker filter is used instead.";
        else text = "Built-in speaker filter (a basic 4x12-style curve). Load a cabinet impulse response (.wav) for a realistic cabinet.";
        irLabel.setText (text, juce::dontSendNotification);
        irLabel.setColour (juce::Label::textColourId, status.irError.isNotEmpty() ? palette::bad : palette::text);
        clearIrButton.setEnabled (status.irPath.isNotEmpty());
        clearIr2Button.setEnabled (status.irPath2.isNotEmpty());
        loadIr2Button.setEnabled (status.irPath.isNotEmpty());
        phase1Button.setEnabled (true);
        phase2Button.setEnabled (status.irPath2.isNotEmpty());
        cab1Label.setText ("Cab 1: " + (status.irLoading ? juce::String ("loading...") : status.irLoaded ? status.irName : juce::String ("built-in speaker")), juce::dontSendNotification);
        cab2Label.setText ("Cab 2: " + (status.irPath2.isEmpty() ? juce::String ("none - load one to blend") : status.irLoading2 ? juce::String ("loading...")
                                                                  : status.irError2.isNotEmpty() ? "could not load (" + status.irError2 + ")" : status.irName2), juce::dontSendNotification);
    }
    for (auto* k : knobs) k->refresh();
    inKnob.refresh();
    outKnob.refresh();
    resized();
    repaint();
}

void RigEditor::refreshStatus()
{
    refreshAll();
}

void RigEditor::setOnTarget (const juce::Identifier& id, const juce::var& v, const juce::String& undoName)
{
    if (! target.isValid()) return;
    if (auto* u = um(); u != nullptr && undoName.isNotEmpty()) u->beginNewTransaction (undoName);
    target.setProperty (id, v, um());
}

void RigEditor::setModelFile (const juce::File& f)
{
    if (! target.isValid()) return;
    setOnTarget (ids::namFile, f == juce::File() ? juce::String() : f.getFullPathName(), f == juce::File() ? "Use built-in amp" : "Load amp model");
    if (f != juce::File())
    {
        if (settings.getValue ("namFolder").isEmpty()) settings.setValue ("namFolder", f.getParentDirectory().getFullPathName());
        set (target, ampOn, 1.0f, um());
    }
    selectBlock (amp);
}

void RigEditor::setIrFile (const juce::File& f, bool second)
{
    if (! target.isValid()) return;
    const auto& id = second ? ids::irFile2 : ids::irFile;
    setOnTarget (id, f == juce::File() ? juce::String() : f.getFullPathName(),
                 f == juce::File() ? (second ? "Remove second cabinet" : "Use built-in cabinet") : (second ? "Load second cabinet" : "Load cabinet IR"));
    if (f != juce::File())
    {
        if (settings.getValue ("irFolder").isEmpty()) settings.setValue ("irFolder", f.getParentDirectory().getFullPathName());   // never move the user's folder away
        set (target, cabOn, 1.0f, um());
        if (second && target[ids::irFile].toString().isEmpty()) setOnTarget (ids::irFile, f.getFullPathName(), {});   // the blend needs a first cabinet
    }
    refreshAll();
}

void RigEditor::copyRig()
{
    if (! target.isValid()) return;
    juce::SystemClipboard::copyTextToClipboard (juce::JSON::toString (toPreset (target, "Copied rig"), false));
}

void RigEditor::pasteRig()
{
    if (! target.isValid()) return;
    const auto v = juce::JSON::parse (juce::SystemClipboard::getTextFromClipboard());
    if (! v.isObject() || v["format"].toString() != "RAVENYR rig")
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Paste rig", "Copy an amp rig first (the Copy button), then paste it here.");
        return;
    }
    if (auto* u = um()) u->beginNewTransaction ("Paste amp rig");
    const auto missing = applyPreset (target, v, um());
    set (target, ampOn, 1.0f, um());
    if (! missing.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Paste rig",
                                                "These files were not found on this computer, so they were left out:\n" + missing.joinIntoString ("\n"));
}

/** Right click on a model / cabinet file in the lists. */
void RigEditor::fileMenu (const juce::File& f, bool cab)
{
    juce::PopupMenu m;
    m.addItem (1, cab ? "Use as the cabinet" : "Use as the amp model");
    if (cab) m.addItem (2, "Blend in as the 2nd cabinet");
    m.addSeparator();
    m.addItem (3, "Show in folder");
    m.addItem (4, "Delete this file...");
    juce::Component::SafePointer<RigEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, f, cab] (int r) {
        if (safe == nullptr || r == 0) return;
        if (r == 1) { if (cab) safe->setIrFile (f); else safe->setModelFile (f); }
        else if (r == 2) safe->setIrFile (f, true);
        else if (r == 3) f.revealToUser();
        else if (r == 4)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle ("Delete file")
                                              .withMessage ("Move " + f.getFileName() + " to the trash?")
                                              .withButton ("Delete").withButton ("Cancel"),
                                          [safe, f, cab] (int choice) {
                                              if (choice != 1 || safe == nullptr) return;
                                              const auto path = f.getFullPathName();
                                              if (! f.moveToTrash()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Delete file", "Could not delete " + f.getFileName() + "."); return; }
                                              if (safe->target.isValid())
                                              {
                                                  if (! cab && safe->target[ids::namFile].toString() == path) safe->setModelFile ({});
                                                  if (cab && safe->target[ids::irFile].toString() == path) safe->setIrFile ({});
                                                  if (cab && safe->target[ids::irFile2].toString() == path) safe->setIrFile ({}, true);
                                              }
                                              safe->models.rescan();
                                              safe->irs.rescan();
                                          });
        }
    });
}

/** Right click on a chain tile: take the model / cabinet out, switch the block on or off. */
void RigEditor::blockMenu (int b)
{
    if (! target.isValid()) return;
    selectBlock (b);
    juce::PopupMenu m;
    const bool on = get (target, switchFor (b)) >= 0.5f;
    m.addItem (1, on ? "Switch off" : "Switch on");
    if (b == amp) m.addItem (2, "Remove the model (use the built-in amp)", target[ids::namFile].toString().isNotEmpty());
    if (b == cab)
    {
        m.addItem (3, "Remove the cabinet (use the built-in speaker)", target[ids::irFile].toString().isNotEmpty());
        m.addItem (4, "Remove the 2nd cabinet", target[ids::irFile2].toString().isNotEmpty());
    }
    if (isPluginBlock (b)) m.addItem (5, "Remove the plugin");
    juce::Component::SafePointer<RigEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, b] (int r) {
        if (safe == nullptr || r == 0) return;
        if (r == 1 && safe->chain.onToggle) safe->chain.onToggle (b);
        else if (r == 2) safe->setModelFile ({});
        else if (r == 3) safe->setIrFile ({});
        else if (r == 4) safe->setIrFile ({}, true);
        else if (r == 5) safe->pluginRemoveButton.onClick();
    });
}

// ---------------------------------------------------------------- presets

juce::File RigEditor::presetFolder() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("RAVENYR").getChildFile ("Rigs");
}

void RigEditor::showAddPluginMenu (int slot)
{
    if (plugins == nullptr || ! target.isValid()) return;
    if (slot < 0)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Plugins").withMessage ("All " + juce::String (numPluginSlots)
                                          + " plugin slots of this rig are in use.").withButton ("OK").withAssociatedComponent (this), nullptr);
        return;
    }
    juce::PopupMenu m;
    if (! plugins->clipboard.empty() && plugins->clipboard.descXml.isNotEmpty())
    {
        m.addItem (-3, "Paste plugin: " + plugins->clipboard.name);
        m.addSeparator();
    }
    plugins->addToMenu (m, 1, true);
    m.addSeparator();
    m.addItem (-2, "Scan for plugins / manage...");
    juce::Component::SafePointer<RigEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chain).withMousePosition(), [safe, slot] (int r) {
        if (safe == nullptr || r == 0) return;
        if (r == -2) { safe->plugins->showManager(); return; }
        juce::String name, state;
        auto desc = safe->plugins->descriptionForMenuResult (r, 1, name);
        if (r == -3) { desc = safe->plugins->clipboard.descXml; name = safe->plugins->clipboard.name; state = safe->plugins->clipboard.state; }
        if (desc.isEmpty() || ! safe->target.isValid()) return;
        if (auto* u = safe->um()) u->beginNewTransaction ("Add plugin " + name);
        const bool fresh = ! pluginNode (safe->target, slot).isValid();
        setPlugin (safe->target, slot, desc, name, safe->um());
        if (state.isNotEmpty()) pluginNode (safe->target, slot).setProperty (ids::state, state, safe->um());
        if (fresh)   // a new plugin goes at the end of the chain
        {
            auto o = getOrder (safe->target);
            std::vector<int> v (o.begin(), o.end());
            v.erase (std::find (v.begin(), v.end(), plug1 + slot));
            v.push_back (plug1 + slot);
            std::copy (v.begin(), v.end(), o.begin());
            setOrder (safe->target, o, safe->um());
        }
        safe->selectBlock (plug1 + slot);
    });
}

void RigEditor::rebuildPresets()
{
    presetCombo.clear (juce::dontSendNotification);
    presetCombo.addSectionHeading ("Built-in (no external files)");
    int id = 1;
    for (auto& p : factoryPresets()) presetCombo.addItem (p.name, id++);
    auto files = presetFolder().findChildFiles (juce::File::findFiles, false, "*.rvnrig;*.ffrig");
    files.sort();
    if (! files.isEmpty())
    {
        presetCombo.addSectionHeading ("My presets");
        for (int i = 0; i < files.size(); ++i) presetCombo.addItem (files[i].getFileNameWithoutExtension(), 100 + i);
    }
    const auto current = target.isValid() ? target[ids::rigName].toString() : juce::String();
    for (int i = 0; i < presetCombo.getNumItems(); ++i)
        if (presetCombo.getItemText (i) == current) presetCombo.setSelectedItemIndex (i, juce::dontSendNotification);
}

void RigEditor::applyPresetId (int id)
{
    if (! target.isValid() || id <= 0) return;
    juce::var json;
    if (id < 100)
    {
        const auto f = factoryPresets();
        if (id - 1 < (int) f.size()) json = f[(size_t) (id - 1)].json;
    }
    else
    {
        auto files = presetFolder().findChildFiles (juce::File::findFiles, false, "*.rvnrig;*.ffrig");
        files.sort();
        if (id - 100 < files.size()) json = juce::JSON::parse (files[id - 100]);
    }
    if (! json.isObject()) return;
    if (auto* u = um()) u->beginNewTransaction ("Load preset");
    const auto missing = applyPreset (target, json, um());
    if (missing.size() > 0)
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Preset loaded with missing files")
                                          .withMessage ("These files were not found, so the built-in amp / cabinet is used instead:\n" + missing.joinIntoString ("\n"))
                                          .withButton ("OK").withAssociatedComponent (this), nullptr);
    selectBlock (chain.selected);
}

void RigEditor::savePreset()
{
    if (! target.isValid()) return;
    auto* w = new juce::AlertWindow ("Save rig preset", "Saved in " + presetFolder().getFullPathName(), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", target[ids::rigName].toString().isNotEmpty() ? target[ids::rigName].toString() : "My rig", "Name");
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<RigEditor> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r) {
        if (r != 1 || safe == nullptr) return;
        const auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
        if (name.isEmpty()) return;
        auto folder = safe->presetFolder();
        folder.createDirectory();
        const auto f = folder.getChildFile (name + ".rvnrig");
        if (! f.replaceWithText (juce::JSON::toString (toPreset (safe->target, name))))
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Could not save").withMessage ("Writing " + f.getFullPathName() + " failed.")
                                              .withButton ("OK"), nullptr);
            return;
        }
        safe->target.setProperty (ids::rigName, name, safe->um());
        safe->rebuildPresets();
    }), true);
}

void RigEditor::deletePreset()
{
    const int id = presetCombo.getSelectedId();
    if (id < 100)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Delete preset").withMessage ("Select one of your own presets first (built-in presets cannot be deleted).")
                                          .withButton ("OK").withAssociatedComponent (this), nullptr);
        return;
    }
    auto files = presetFolder().findChildFiles (juce::File::findFiles, false, "*.rvnrig;*.ffrig");
    files.sort();
    if (id - 100 >= files.size()) return;
    const auto f = files[id - 100];
    juce::Component::SafePointer<RigEditor> safe (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Delete preset").withMessage ("Delete \"" + f.getFileNameWithoutExtension() + "\"?")
                                      .withButton ("Delete").withButton ("Cancel").withAssociatedComponent (this),
                                  [safe, f] (int r) {
                                      if (r != 1 || safe == nullptr) return;
                                      if (! f.moveToTrash()) f.deleteFile();
                                      safe->rebuildPresets();
                                  });
}

// ---------------------------------------------------------------- layout

void RigEditor::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
    if (target.isValid())
    {
        auto r = getLocalBounds().reduced (8);
        r.removeFromTop (34 + 6 + 22 + 8 + 116 + 10);
        g.setColour (palette::panel);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
    }
}

void RigEditor::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto row = r.removeFromTop (34);
    auto take = [&row] (juce::Component& c, int w, int gap = 6) { c.setBounds (row.removeFromLeft (w)); row.removeFromLeft (gap); };
    auto takeRight = [&row] (juce::Component& c, int w, int gap = 6) { c.setBounds (row.removeFromRight (w)); row.removeFromRight (gap); };
    take (titleLabel, 260);
    if (onButton.isVisible()) take (onButton, 160);
    if (inputCombo.isVisible()) take (inputCombo, 210);
    if (monitorButton.isVisible()) take (monitorButton, 120);
    if (resetButton.isVisible())
    {
        takeRight (resetButton, 64);
        takeRight (pasteButton, 54);
        takeRight (copyButton, 54);
        takeRight (deletePresetButton, 64);
        takeRight (savePresetButton, 110);
        takeRight (presetCombo, 200);
    }
    r.removeFromTop (6);
    auto hint = r.removeFromTop (22);
    statusLabel.setBounds (hint.removeFromRight (360));
    targetHint.setBounds (hint);
    r.removeFromTop (8);

    if (! target.isValid())
    {
        addRigButton.setBounds (r.removeFromTop (40).withSizeKeepingCentre (260, 36));
        return;
    }

    auto chainRow = r.removeFromTop (116);
    auto left = chainRow.removeFromLeft (84);
    auto right = chainRow.removeFromRight (84);
    inKnob.setBounds (left.removeFromTop (92));
    inLabel.setBounds (left.removeFromLeft (26));
    inMeter.setBounds (left.withSizeKeepingCentre (left.getWidth() - 4, 8));
    outKnob.setBounds (right.removeFromTop (92));
    outLabel.setBounds (right.removeFromLeft (30));
    outMeter.setBounds (right.withSizeKeepingCentre (right.getWidth() - 4, 8));
    chain.setBounds (chainRow.reduced (10, 0));
    r.removeFromTop (10);
    layoutDetail (r.reduced (14, 10));
}

void RigEditor::layoutDetail (juce::Rectangle<int> r)
{
    auto title = r.removeFromTop (30);
    blockTitle.setBounds (title.removeFromLeft (180));
    blockOnButton.setBounds (title.removeFromLeft (64).reduced (0, 2));
    title.removeFromLeft (12);
    moveLeftButton.setBounds (title.removeFromLeft (32).reduced (0, 2));
    title.removeFromLeft (4);
    moveRightButton.setBounds (title.removeFromLeft (32).reduced (0, 2));
    blockHelp.setBounds (r.removeFromTop (22));
    r.removeFromTop (8);

    const int b = chain.selected;
    if (b == amp)
    {
        models.setBounds (r.removeFromRight (juce::jmin (440, r.getWidth() / 2)));
        r.removeFromRight (20);

        auto buttons = r.removeFromTop (30);
        loadModelButton.setBounds (buttons.removeFromLeft (120));
        buttons.removeFromLeft (6);
        clearModelButton.setBounds (buttons.removeFromLeft (136));
        buttons.removeFromLeft (6);
        normaliseButton.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (6);
        calibrateButton.setBounds (buttons.removeFromLeft (124));
        r.removeFromTop (8);
        modelLabel.setBounds (r.removeFromTop (24));
        modelInfo.setBounds (r.removeFromTop (84));
        r.removeFromTop (6);
    }
    else if (b == cab)
    {
        irs.setBounds (r.removeFromRight (juce::jmin (440, r.getWidth() / 2)));
        r.removeFromRight (20);
        for (auto [label, load, phase, clear] : { std::tuple<juce::Label*, juce::TextButton*, juce::TextButton*, juce::TextButton*> { &cab1Label, &loadIrButton, &phase1Button, &clearIrButton },
                                                  { &cab2Label, &loadIr2Button, &phase2Button, &clearIr2Button } })
        {
            auto row = r.removeFromTop (30);
            clear->setBounds (row.removeFromRight (80));
            row.removeFromRight (6);
            phase->setBounds (row.removeFromRight (70));
            row.removeFromRight (6);
            load->setBounds (row.removeFromRight (80));
            row.removeFromRight (6);
            label->setBounds (row);
            r.removeFromTop (6);
        }
        r.removeFromTop (4);
        irLabel.setBounds (r.removeFromTop (46));
        r.removeFromTop (6);
    }
    if (isPluginBlock (b))
    {
        auto buttons = r.removeFromTop (30);
        pluginOpenButton.setBounds (buttons.removeFromLeft (160));
        buttons.removeFromLeft (6);
        pluginReplaceButton.setBounds (buttons.removeFromLeft (100));
        buttons.removeFromLeft (6);
        pluginRemoveButton.setBounds (buttons.removeFromLeft (90));
        r.removeFromTop (10);
        pluginInfo.setBounds (r.removeFromTop (80));
    }
    if (pluginManagerButton.isVisible())
        pluginManagerButton.setBounds (juce::Rectangle<int> (r.getRight() - 150, r.getBottom() - 30, 150, 30));
    if (b == delay)
    {
        delayStereoButton.setBounds (r.removeFromTop (30).removeFromLeft (150));
        r.removeFromTop (8);
    }
    auto knobRow = r.removeFromTop (110);
    for (auto* k : knobs)
    {
        k->setBounds (knobRow.removeFromLeft (96));
        knobRow.removeFromLeft (8);
    }
}

// ---------------------------------------------------------------- live updates

void RigEditor::timerCallback()
{
    if (! active || ! target.isValid()) return;
    if (auto* rigPtr = session.rigs().rigFor (target))
    {
        inMeter.push (rigPtr->inPeak.exchange (0.0f));
        outMeter.push (rigPtr->outPeak.exchange (0.0f));
        const bool running = targetTrackId.isEmpty() ? rig::isEnabled (target) : true;
        juce::String s;
        if (running)
        {
            s << "Rig CPU " << juce::String (rigPtr->cpuLoad.load() * 100.0f, 1) << " %";
            const double sr = host.engine.getSampleRate();
            if (const int lat = rigPtr->latency.load(); lat > 0 && sr > 0)
                s << "  -  resampling adds " << juce::String (lat * 1000.0 / sr, 2) << " ms";
            if (get (target, gateOn) >= 0.5f) s << "  -  gate " << (rigPtr->gateOpen.load() ? "open" : "closed");
        }
        statusLabel.setText (s, juce::dontSendNotification);
    }
}

void RigEditor::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& prop)
{
    if (node.hasType (ids::RIG))
    {
        if (node == target)
        {
            if (prop == ids::namFile || prop == ids::irFile2) { selectBlock (chain.selected); return; }   // the built-in amp's gain knob comes / goes
            if (prop == ids::rigName) rebuildPresets();
            if (prop == juce::Identifier (info (calLevel).id)) settings.setValue ("namInterfaceDbu", (double) get (target, calLevel));
            if (prop == juce::Identifier (info (namCalibrate).id)) { selectBlock (chain.selected); return; }
            if (prop == ids::input) rebuildInputs();
            refreshAll();
        }
        else if (prop == ids::enabled && node.getParent()[tlids::id].toString() == targetTrackId)
            selectTarget (targetTrackId);
        else if (prop == ids::enabled) rebuildInputs();
        return;
    }
    if (node.hasType (tlids::TRACK))
    {
        if (prop == tlids::name) rebuildInputs();
        if (prop == tlids::monitor && node[tlids::id].toString() == targetTrackId) refreshAll();
    }
}

void RigEditor::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (ids::PLUGIN)) { refreshAll(); return; }
    if (child.hasType (tlids::TRACK) || child.hasType (ids::RIG)) selectTarget (targetTrackId);
}

void RigEditor::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (ids::PLUGIN))
    {
        if (isPluginBlock (chain.selected) && ! pluginNode (target, pluginSlotOf (chain.selected)).isValid()) selectBlock (amp);
        else refreshAll();
        return;
    }
    if (trackMode && child.hasType (tlids::TRACK) && child[tlids::id].toString() == targetTrackId)
    {
        target = {};
        if (onTrackGone) juce::MessageManager::callAsync ([cb = onTrackGone] { cb(); });
        return;
    }
    if (child.hasType (tlids::TRACK) || child.hasType (ids::RIG)) selectTarget (targetTrackId);
}
} // namespace ff
