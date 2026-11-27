#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "SessionController.h"
#include "Widgets.h"

namespace ff
{
/** Export options (mix or stems, WAV/FLAC, bit depth, rate, range, normalise). */
class ExportDialog : public juce::Component
{
public:
    ExportDialog (ProjectDocument&, SessionController&, EngineHost&, juce::PropertiesFile&);
    void paint (juce::Graphics&) override;
    void resized() override;
    static void show (juce::Component* parent, ProjectDocument&, SessionController&, EngineHost&, juce::PropertiesFile&);

private:
    ProjectDocument& doc;
    SessionController& session;
    EngineHost& host;
    juce::PropertiesFile& settings;
    juce::ComboBox mode, format, bits, rate, range;
    juce::ToggleButton normalize { "Normalise peaks to -1 dBFS" };
    juce::TextButton exportButton { "Export..." }, cancelButton { "Close" };
    juce::OwnedArray<juce::Label> labels;
    juce::Label summary;
    std::unique_ptr<juce::FileChooser> chooser;

    void updateSummary();
    ExportSettings currentSettings() const;
    void startExport();
    void rememberChoices();
    void runExport (ExportSettings);
};
} // namespace ff
