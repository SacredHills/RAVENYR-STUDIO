#include "ExportDialog.h"
#include "core/ProjectFile.h"

namespace ff
{
namespace pids = project::ids;

namespace
{
    class ExportJob : public juce::ThreadWithProgressWindow
    {
    public:
        ExportJob (juce::ValueTree t, juce::File media, AudioCache& c, ExportSettings s, std::function<void (ExportResult)> done)
            : juce::ThreadWithProgressWindow ("Exporting audio...", true, true, -1),   // -1: never kill the thread, wait for it to notice the cancel
              tracks (t), mediaBundle (media), cache (c),
              settings (std::move (s)), onDone (std::move (done)) {}

        void run() override
        {
            result = exportAudio (tracks, mediaBundle, cache, settings, [this] (double p) {
                setProgress (p);
                return ! threadShouldExit();
            });
        }

        void threadComplete (bool userPressedCancel) override
        {
            if (userPressedCancel)
                result.result = juce::Result::fail ("Export cancelled.");
            if (onDone) onDone (result);
            delete this;
        }

    private:
        juce::ValueTree tracks;
        juce::File mediaBundle;
        AudioCache& cache;
        ExportSettings settings;
        std::function<void (ExportResult)> onDone;
        ExportResult result;
    };
}

ExportDialog::ExportDialog (ProjectDocument& d, SessionController& s, EngineHost& h, juce::PropertiesFile& p)
    : doc (d), session (s), host (h), settings (p)
{
    auto addLabel = [this] (const juce::String& t) {
        auto* l = labels.add (new juce::Label ({}, t));
        styleLabel (*l, 13.0f, palette::textDim, true);
        addAndMakeVisible (l);
    };
    addLabel ("Export");
    addLabel ("Format");
    addLabel ("Bit depth");
    addLabel ("Sample rate");
    addLabel ("Range");

    mode.addItem ("Full mix (one stereo file)", 1);
    mode.addItem ("Stems (one file per track)", 2);
    format.addItem ("WAV", 1);
    format.addItem ("FLAC (lossless, smaller)", 2);
    bits.addItem ("16-bit (dithered)", 16);
    bits.addItem ("24-bit", 24);
    bits.addItem ("32-bit float (WAV only)", 32);
    const double projectRate = host.engine.getSampleRate() > 0 ? host.engine.getSampleRate() : 48000.0;
    rate.addItem ("Session rate (" + juce::String (projectRate / 1000.0, 1) + " kHz)", 1);
    rate.addItem ("44.1 kHz", 44100);
    rate.addItem ("48 kHz", 48000);
    rate.addItem ("88.2 kHz", 88200);
    rate.addItem ("96 kHz", 96000);
    range.addItem ("Whole song (start to last clip)", 1);
    range.addItem ("Loop range on the ruler", 2);
    range.addItem ("Export range on the ruler (green bar)", 3);

    mode.setSelectedId (settings.getIntValue ("exportMode", 1), juce::dontSendNotification);
    format.setSelectedId (settings.getIntValue ("exportFormat", 1), juce::dontSendNotification);
    bits.setSelectedId (settings.getIntValue ("exportBits", 24), juce::dontSendNotification);
    rate.setSelectedId (settings.getIntValue ("exportRate", 1), juce::dontSendNotification);
    {
        auto t = doc.transport();
        const bool hasRange = (double) t.getProperty ("exportEnd", 0.0) > (double) t.getProperty ("exportStart", 0.0);
        range.setItemEnabled (3, hasRange);
        range.setSelectedId (hasRange ? 3 : 1, juce::dontSendNotification);   // a drawn export range is what you want to export
    }
    normalize.setToggleState (settings.getBoolValue ("exportNormalize", false), juce::dontSendNotification);

    for (auto* c : { &mode, &format, &bits, &rate, &range })
    {
        c->onChange = [this] { rememberChoices(); updateSummary(); };
        addAndMakeVisible (c);
    }
    normalize.onClick = [this] { rememberChoices(); updateSummary(); };
    addAndMakeVisible (normalize);

    styleLabel (summary, 13.0f, palette::text);
    summary.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (summary);

    exportButton.setColour (juce::TextButton::buttonColourId, palette::accent);
    exportButton.setColour (juce::TextButton::textColourOffId, palette::onAccent);
    exportButton.onClick = [this] { startExport(); };
    cancelButton.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };
    addAndMakeVisible (exportButton);
    addAndMakeVisible (cancelButton);
    setSize (520, 380);
    updateSummary();
}

ExportSettings ExportDialog::currentSettings() const
{
    ExportSettings s;
    s.stems = mode.getSelectedId() == 2;
    s.format = format.getSelectedId() == 2 ? ExportSettings::Format::flac : ExportSettings::Format::wav;
    s.bitDepth = bits.getSelectedId();
    if (s.format == ExportSettings::Format::flac && s.bitDepth == 32) s.bitDepth = 24;
    s.sampleRate = rate.getSelectedId() == 1 ? (host.engine.getSampleRate() > 0 ? host.engine.getSampleRate() : 48000.0)
                                             : (double) rate.getSelectedId();
    s.normalize = normalize.getToggleState();
    s.tempoMap = doc.tempoMap();
    s.tsNum = doc.tsNum();
    s.tsDen = doc.tsDen();
    auto t = doc.transport();
    if (range.getSelectedId() == 3)
    {
        s.startSec = (double) t.getProperty ("exportStart", 0.0);
        s.endSec = (double) t.getProperty ("exportEnd", 0.0);
    }
    else if (range.getSelectedId() == 2)
    {
        s.startSec = (double) t[pids::loopStart];
        s.endSec = (double) t[pids::loopEnd];
    }
    else
    {
        s.startSec = 0.0;
        s.endSec = timeline::songEnd (doc.tracks());
    }
    s.baseName = juce::File::createLegalFileName (doc.tree()[pids::name].toString());
    return s;
}

void ExportDialog::updateSummary()
{
    const auto s = currentSettings();
    const double len = s.endSec - s.startSec;
    juce::String txt;
    if (len <= 0.0)
        txt = "Nothing to export yet: the song has no audio clips.";
    else
    {
        const int mins = (int) (len / 60.0);
        txt << "Length: " << mins << ":" << juce::String (len - mins * 60.0, 1).paddedLeft ('0', 4) << "\n";
        txt << (s.stems ? "One " : "One stereo ") << (s.format == ExportSettings::Format::flac ? "FLAC" : "WAV") << " file"
            << (s.stems ? " per audible track" : "") << ", " << s.bitDepth << "-bit, " << juce::String (s.sampleRate / 1000.0, 1) << " kHz.";
        if (format.getSelectedId() == 2 && bits.getSelectedId() == 32)
            txt << "\nFLAC is limited to 24-bit; 24-bit will be used.";
        txt << "\nMuted tracks are skipped; solo is respected. The main output fader is not applied.";
    }
    summary.setText (txt, juce::dontSendNotification);
    exportButton.setEnabled (len > 0.0);
}

/** The export choices (stems or full mix, format, depth, rate, normalise) are kept as soon as they change, so they are
    still there next time even if the dialog is cancelled. */
void ExportDialog::rememberChoices()
{
    settings.setValue ("exportMode", mode.getSelectedId());
    settings.setValue ("exportFormat", format.getSelectedId());
    settings.setValue ("exportBits", bits.getSelectedId());
    settings.setValue ("exportRate", rate.getSelectedId());
    settings.setValue ("exportNormalize", normalize.getToggleState());
    settings.saveIfNeeded();
}

void ExportDialog::startExport()
{
    auto s = currentSettings();
    rememberChoices();

    juce::File startDir = doc.hasLocation() ? doc.getBundle().getParentDirectory() : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (const juce::File last (settings.getValue (s.stems ? "exportStemsDir" : "exportMixDir")); last.isDirectory()) startDir = last;   // where the last export went
    juce::Component::SafePointer<ExportDialog> safe (this);
    if (s.stems)
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the stems", startDir);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe, s] (const juce::FileChooser& fc) mutable {
                                  if (safe == nullptr || fc.getResult() == juce::File()) return;
                                  s.destination = fc.getResult();
                                  safe->runExport (s);
                              });
    }
    else
    {
        chooser = std::make_unique<juce::FileChooser> ("Export mix as", startDir.getChildFile (s.baseName + s.extension()), "*" + s.extension());
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, s] (const juce::FileChooser& fc) mutable {
                                  if (safe == nullptr || fc.getResult() == juce::File()) return;
                                  s.destination = fc.getResult().withFileExtension (s.extension());
                                  safe->runExport (s);
                              });
    }
}

void ExportDialog::runExport (ExportSettings s)
{
    settings.setValue (s.stems ? "exportStemsDir" : "exportMixDir", (s.stems ? s.destination : s.destination.getParentDirectory()).getFullPathName());
    settings.saveIfNeeded();
    session.rigs().captureAllPluginStates();   // the export uses every plugin's current settings
    s.master = project::masterNode (doc.tree()).createCopy();   // master plugins (the tracks are copied without their parent)
    auto* job = new ExportJob (doc.tracks().createCopy(), doc.mediaBundle(), session.cache, s, [] (ExportResult r) {
        juce::String msg;
        if (r.result.failed())
            msg = r.result.getErrorMessage();
        else
        {
            msg << "Wrote " << r.files.size() << (r.files.size() == 1 ? " file" : " files") << ":\n";
            for (auto& f : r.files) msg << f.getFullPathName() << "\n";
            juce::Logger::writeToLog ("Export finished: " + juce::String (r.files.size()) + " file(s)");
        }
        if (! r.warnings.isEmpty())
            msg << "\nNotes:\n" << r.warnings.joinIntoString ("\n");
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (r.result.failed() ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon)
                                          .withTitle (r.result.failed() ? "Export failed" : "Export complete")
                                          .withMessage (msg)
                                          .withButton ("OK"),
                                      nullptr);
    });
    job->launchThread();
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
        w->exitModalState (1);
}

void ExportDialog::paint (juce::Graphics& g)
{
    g.fillAll (palette::panel);
}

void ExportDialog::resized()
{
    auto r = getLocalBounds().reduced (20);
    auto row = [&r] { auto x = r.removeFromTop (30); r.removeFromTop (8); return x; };
    juce::ComboBox* combos[] = { &mode, &format, &bits, &rate, &range };
    for (int i = 0; i < 5; ++i)
    {
        auto x = row();
        labels[i]->setBounds (x.removeFromLeft (110));
        combos[i]->setBounds (x);
    }
    normalize.setBounds (row().withTrimmedLeft (106));
    auto buttons = r.removeFromBottom (32);
    cancelButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    exportButton.setBounds (buttons.removeFromRight (120));
    summary.setBounds (r.reduced (0, 4));
}

void ExportDialog::show (juce::Component* parent, ProjectDocument& d, SessionController& s, EngineHost& h, juce::PropertiesFile& p)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new ExportDialog (d, s, h, p));
    o.dialogTitle = "Export audio";
    o.dialogBackgroundColour = palette::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = parent;
    o.launchAsync();
}
} // namespace ff
