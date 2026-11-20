#include "Tone3000Panel.h"

namespace ff
{
using t3k::Client;

Tone3000Panel::Tone3000Panel (t3k::Client& c) : client (c)
{
    styleLabel (title, 18.0f, palette::text, true);
    title.setText ("TONE3000", juce::dontSendNotification);
    for (auto* l : { &title, &account, &keyLabel, &targetLabel, &status, &toneTitle, &toneInfo, &attribution })
        addAndMakeVisible (l);
    styleLabel (account, 13.0f, palette::text);
    styleLabel (keyLabel, 12.5f);
    keyLabel.setText ("Publishable key", juce::dontSendNotification);
    styleLabel (targetLabel, 13.0f, palette::accent, true);
    targetLabel.setJustificationType (juce::Justification::centredRight);
    styleLabel (status, 12.5f);
    styleLabel (toneTitle, 16.0f, palette::text, true);
    styleLabel (toneInfo, 12.5f);
    toneInfo.setJustificationType (juce::Justification::topLeft);
    styleLabel (attribution, 11.5f);
    attribution.setText ("Tones and captures are made and shared by their creators on TONE3000 (tone3000.com); each tone has its own licence. "
                         "Downloads are saved in " + Client::libraryFolder().getFullPathName(), juce::dontSendNotification);

    keyEditor.setTextToShowWhenEmpty ("t3k_pub_...", palette::textDim);
    keyEditor.setText (client.clientId(), false);
    keyEditor.onReturnKey = [this] {
        client.setClientId (keyEditor.getText());
        keyEditor.setText (client.clientId(), false);
        if (auto p = t3k::Client::keyProblem (client.clientId()); p.isNotEmpty() && client.clientId().isNotEmpty()) setStatus (p, true);
        refreshAccount();
    };
    keyEditor.onFocusLost = keyEditor.onReturnKey;
    keyEditor.setTooltip ("Your TONE3000 publishable API key (client ID). Create one in your TONE3000 account settings under API keys.");
    addAndMakeVisible (keyEditor);
    keyHelpButton.onClick = [] { juce::URL ("https://www.tone3000.com/api").launchInDefaultBrowser(); };
    addAndMakeVisible (keyHelpButton);

    signButton.onClick = [this] {
        if (client.isConnected()) { client.disconnect(); return; }
        client.setClientId (keyEditor.getText());
        keyEditor.setText (client.clientId(), false);
        setStatus ("Sign in in your browser, then come back here. If the page says \"Unknown client_id\", TONE3000 does not know this key: "
                   "copy the publishable key again from your TONE3000 account settings > API keys (and check that it is active).");
        client.signIn ([safe = juce::Component::SafePointer<Tone3000Panel> (this)] (juce::String err) {
            if (safe == nullptr) return;
            if (err.isNotEmpty() && err != "cancelled") safe->setStatus (err, true);
            else safe->setStatus (err == "cancelled" ? "Sign-in cancelled." : "Signed in.");
            safe->refreshAccount();
            safe->reload();
        });
    };
    browseButton.setTooltip ("Opens TONE3000's catalogue in your browser (search, filters, previews). Pick a tone there and it appears here.");
    browseButton.onClick = [this] {
        client.setClientId (keyEditor.getText());
        keyEditor.setText (client.clientId(), false);
        setStatus ("Pick a tone on the TONE3000 page in your browser. If it says \"Unknown client_id\", copy the publishable key again "
                   "from your TONE3000 account settings > API keys.");
        client.selectTone (format(), cabMode ? "cab" : "amp", [safe = juce::Component::SafePointer<Tone3000Panel> (this)] (int toneId, juce::String err) {
            if (safe == nullptr) return;
            safe->refreshAccount();
            if (err.isNotEmpty()) { safe->setStatus (err == "cancelled" ? "Nothing picked." : err, err != "cancelled"); return; }
            if (toneId <= 0) { safe->setStatus ("No tone was picked."); return; }
            safe->setStatus ("Loading the tone...");
            safe->client.getTone (toneId, [safe] (t3k::Tone t, juce::String e) {
                if (safe == nullptr) return;
                if (e.isNotEmpty()) { safe->setStatus (e, true); return; }
                safe->toneList.deselectAllRows();
                safe->showTone (t);
            });
        });
    };
    for (auto* b : { &signButton, &browseButton, &refreshButton, &prevButton, &nextButton, &downloadButton, &openPageButton })
        addAndMakeVisible (b);

    feedCombo.addItem ("My favourites", 1);
    feedCombo.addItem ("My downloads", 2);
    feedCombo.addItem ("My tones", 3);
    feedCombo.addItem ("Trending", 4);
    feedCombo.addItem ("Latest", 5);
    feedCombo.addItem ("Search", 6);
    feedCombo.setSelectedId (1, juce::dontSendNotification);
    feedCombo.onChange = [this] { page = 1; searchBox.setVisible (feedCombo.getSelectedId() == 6); resized(); reload(); };
    addAndMakeVisible (feedCombo);
    kindCombo.addItem ("Amps and cabs", 1);
    kindCombo.addItem ("Amp models only", 2);
    kindCombo.addItem ("Cabs only", 3);
    kindCombo.setSelectedId (2, juce::dontSendNotification);
    kindCombo.setTooltip ("Filter the list: amp captures (.nam) or cabinet impulse responses");
    kindCombo.onChange = [this] { if (feedCombo.getSelectedId() - 1 == (int) Client::Feed::trending) reload(); else applyFilter(); };
    addAndMakeVisible (kindCombo);
    searchBox.setTextToShowWhenEmpty ("Search and press Enter", palette::textDim);
    searchBox.onReturnKey = [this] { page = 1; reload(); };
    addChildComponent (searchBox);
    refreshButton.onClick = [this] { reload(); };
    prevButton.onClick = [this] { if (page > 1) { --page; reload(); } };
    nextButton.onClick = [this] { if (page < totalPages) { ++page; reload(); } };
    downloadButton.onClick = [this] { downloadRow (modelList.getSelectedRow()); };
    openPageButton.onClick = [this] { if (currentTone.url.isNotEmpty()) juce::URL (currentTone.url).launchInDefaultBrowser(); };

    for (auto* l : { &toneList, &modelList })
    {
        l->setRowHeight (l == &toneList ? 40 : 26);
        l->setColour (juce::ListBox::backgroundColourId, palette::well);
        addAndMakeVisible (l);
    }
    client.addChangeListener (this);
    refreshAccount();
    showTone ({});
}

Tone3000Panel::~Tone3000Panel()
{
    client.removeChangeListener (this);
}

void Tone3000Panel::setMode (bool cab, const juce::String& targetName, std::function<void (const juce::File&, bool isIr)> load)
{
    const bool changed = cab != cabMode;
    cabMode = cab;
    onLoad = std::move (load);
    targetLabel.setText ((cab ? "Cabinet IRs for " : "Amp captures for ") + targetName, juce::dontSendNotification);
    if (changed) { page = 1; showTone ({}); kindCombo.setSelectedId (cab ? 3 : 2, juce::dontSendNotification); }
    reload();
}

void Tone3000Panel::refreshAccount()
{
    const bool connected = client.isConnected();
    account.setText (connected ? "Signed in" + (client.userName().isNotEmpty() ? " as " + client.userName() : juce::String())
                               : (client.clientId().isEmpty() ? "Enter your TONE3000 publishable key, then sign in" : "Not signed in"),
                     juce::dontSendNotification);
    signButton.setButtonText (connected ? "Sign out" : "Sign in");
    for (auto* c : std::initializer_list<juce::Component*> { &feedCombo, &refreshButton, &prevButton, &nextButton, &downloadButton })
        c->setEnabled (connected);
}

void Tone3000Panel::setStatus (const juce::String& s, bool error)
{
    status.setText (s, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, error ? palette::bad : palette::textDim);
}

void Tone3000Panel::reload()
{
    if (! client.isConnected()) { tones.clear(); shownTones.clear(); toneList.updateContent(); toneList.repaint(); return; }
    const auto feed = (Client::Feed) (feedCombo.getSelectedId() - 1);
    if (feed == Client::Feed::search && searchBox.getText().trim().isEmpty()) { setStatus ("Type something to search for."); return; }
    const int req = ++requestId;
    setStatus ("Loading...");
    // trending has only 10 tones of every kind: ask for cabinets when browsing IRs
    const juce::String gear = feed != Client::Feed::trending ? juce::String() : kindCombo.getSelectedId() == 3 ? "cab" : kindCombo.getSelectedId() == 2 ? "amp" : "";
    client.listTones (feed, searchBox.getText().trim(), page, gear, [safe = juce::Component::SafePointer<Tone3000Panel> (this), req] (t3k::TonePage p, juce::String err) {
        if (safe == nullptr || req != safe->requestId) return;
        if (err.isNotEmpty()) { safe->setStatus (err, true); return; }
        safe->tones = p.tones;
        safe->page = p.page;
        safe->totalPages = p.totalPages;
        safe->prevButton.setEnabled (p.page > 1);
        safe->nextButton.setEnabled (p.page < p.totalPages);
        safe->applyFilter();
    });
}

void Tone3000Panel::applyFilter()
{
    const int kind = kindCombo.getSelectedId();   // 1 all, 2 amps, 3 cabs
    shownTones.clear();
    int unloadable = 0;
    for (auto& t : tones)
    {
        if (! loadable (t)) { ++unloadable; continue; }   // NAM and IR tones (each loads into its own block)
        const bool isCab = t.format == "ir" || t.gear == "cab";
        // "Amp models only" means amp heads: full rigs (amp + cab captures) and pedals are not amp models for the amp block
        const bool ampHead = t.gear.isEmpty() || t.gear == "amp";
        if ((kind == 2 && (isCab || ! ampHead)) || (kind == 3 && ! isCab)) continue;
        shownTones.add (t);
    }
    toneList.updateContent();
    toneList.repaint();
    setStatus (juce::String (shownTones.size()) + " tones"
               + (totalPages > 1 ? "  -  page " + juce::String (page) + " of " + juce::String (totalPages) : juce::String())
               + (unloadable > 0 ? "  (" + juce::String (unloadable) + " in formats RAVENYR can't load hidden)" : juce::String()));
}

void Tone3000Panel::selectTone (int row)
{
    if (juce::isPositiveAndBelow (row, shownTones.size())) showTone (shownTones[row]);
}

void Tone3000Panel::showTone (const t3k::Tone& t)
{
    currentTone = t;
    models.clear();
    modelList.updateContent();
    modelList.repaint();
    openPageButton.setVisible (t.id > 0 && t.url.isNotEmpty());
    downloadButton.setVisible (t.id > 0);
    if (t.id <= 0)
    {
        toneTitle.setText ({}, juce::dontSendNotification);
        toneInfo.setText ("Pick a tone on the left, or use \"Browse TONE3000...\" to find one in TONE3000's catalogue.", juce::dontSendNotification);
        return;
    }
    toneTitle.setText (t.title, juce::dontSendNotification);
    juce::StringArray parts;
    if (t.creator.isNotEmpty()) parts.add ("by " + t.creator);
    if (t.gear.isNotEmpty()) parts.add (t.gear);
    if (! t.makes.isEmpty()) parts.add (t.makes.joinIntoString (", "));
    if (t.license.isNotEmpty()) parts.add ("licence: " + t.license);
    toneInfo.setText (parts.joinIntoString ("  -  "), juce::dontSendNotification);
    const int req = ++requestId;
    client.listModels (t.id, [safe = juce::Component::SafePointer<Tone3000Panel> (this), req] (juce::Array<t3k::Model> m, juce::String err) {
        if (safe == nullptr || req != safe->requestId) return;
        if (err.isNotEmpty()) { safe->setStatus (err, true); return; }
        safe->models = m;
        safe->modelList.updateContent();
        safe->modelList.repaint();
        if (! m.isEmpty()) safe->modelList.selectRow (0);
        const bool ir = safe->currentTone.format == "ir";
        safe->setStatus (juce::String (m.size()) + (ir ? " IRs (they load into the Cabinet block)" : " captures (they load into the Amp block)")
                         + ". Double-click one (or select it and click Download & load).");
    });
}

void Tone3000Panel::downloadRow (int row)
{
    if (! juce::isPositiveAndBelow (row, models.size()) || currentTone.id <= 0) return;
    const auto m = models[row];
    setStatus ("Downloading " + m.name + "...");
    const bool ir = currentTone.format == "ir" || (currentTone.format.isEmpty() && cabMode);
    client.download (currentTone, m, [safe = juce::Component::SafePointer<Tone3000Panel> (this), name = m.name, ir] (juce::File f, juce::String err) {
        if (safe == nullptr) return;
        if (err.isNotEmpty()) { safe->setStatus (err, true); return; }
        safe->setStatus ("Loaded " + name + (ir ? " into the Cabinet" : " into the Amp") + "  (" + f.getFullPathName() + ")");
        if (safe->onLoad) safe->onLoad (f, ir);
    });
}

void Tone3000Panel::ToneListModel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, o.shownTones.size())) return;
    const auto& t = o.shownTones.getReference (row);
    if (selected) { g.setColour (palette::accentDim); g.fillRect (0, 0, w, h); }
    g.setColour (palette::text);
    g.setFont (uiFont (13.5f, true));
    g.drawText (t.title, 10, 3, w - 20, 18, juce::Justification::centredLeft, true);
    g.setColour (palette::textDim);
    g.setFont (uiFont (11.5f));
    juce::String sub = t.format == "ir" ? "IR" : "NAM";
    if (t.creator.isNotEmpty()) sub << "  -  " << t.creator;
    if (t.gear.isNotEmpty()) sub << "  -  " << t.gear;
    sub << "  -  " << t.countLabel();
    if (t.downloadsCount > 0) sub << "  -  " << t.downloadsCount << " downloads";
    g.drawText (sub, 10, 21, w - 20, 16, juce::Justification::centredLeft, true);
}

void Tone3000Panel::ModelListModel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, o.models.size())) return;
    const auto& m = o.models.getReference (row);
    if (selected) { g.setColour (palette::accentDim); g.fillRect (0, 0, w, h); }
    g.setColour (palette::text);
    g.setFont (uiFont (13.0f));
    g.drawText (m.name, 10, 0, w - 170, h, juce::Justification::centredLeft, true);
    g.setColour (palette::textDim);
    g.setFont (uiFont (11.5f));
    juce::String right;
    if (m.size.isNotEmpty()) right << m.size;
    if (m.architecture.isNotEmpty()) right << (right.isEmpty() ? "" : "  -  ") << "A" << m.architecture;
    g.drawText (right, w - 160, 0, 150, h, juce::Justification::centredRight, true);
}

void Tone3000Panel::paint (juce::Graphics& g)
{
    g.fillAll (palette::bg);
}

void Tone3000Panel::resized()
{
    auto r = getLocalBounds().reduced (14);
    auto top = r.removeFromTop (30);
    title.setBounds (top.removeFromLeft (120));
    signButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (8);
    keyHelpButton.setBounds (top.removeFromRight (96));
    top.removeFromRight (6);
    keyEditor.setBounds (top.removeFromRight (230).reduced (0, 2));
    keyLabel.setBounds (top.removeFromRight (104));
    account.setBounds (top);
    r.removeFromTop (8);

    auto row = r.removeFromTop (30);
    browseButton.setBounds (row.removeFromLeft (160));
    row.removeFromLeft (12);
    feedCombo.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (6);
    kindCombo.setBounds (row.removeFromLeft (140));
    row.removeFromLeft (6);
    if (searchBox.isVisible()) { searchBox.setBounds (row.removeFromLeft (220)); row.removeFromLeft (6); }
    refreshButton.setBounds (row.removeFromLeft (76));
    row.removeFromLeft (6);
    prevButton.setBounds (row.removeFromLeft (30));
    row.removeFromLeft (2);
    nextButton.setBounds (row.removeFromLeft (30));
    targetLabel.setBounds (row);
    r.removeFromTop (6);
    attribution.setBounds (r.removeFromBottom (32));
    status.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (4);

    toneList.setBounds (r.removeFromLeft (r.getWidth() * 45 / 100));
    r.removeFromLeft (14);
    toneTitle.setBounds (r.removeFromTop (24));
    toneInfo.setBounds (r.removeFromTop (38));
    auto buttons = r.removeFromBottom (32);
    downloadButton.setBounds (buttons.removeFromLeft (150));
    buttons.removeFromLeft (8);
    openPageButton.setBounds (buttons.removeFromLeft (170));
    r.removeFromBottom (6);
    modelList.setBounds (r);
}
} // namespace ff
