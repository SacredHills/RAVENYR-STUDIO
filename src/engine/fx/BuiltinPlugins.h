#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <functional>

/** RAVENYR's own effects (EQ, compressor, delay, chorus, reverb), hosted like any other plugin: they show up
    first in every plugin menu, work as track inserts and in amp rigs, save their settings in the project and
    open in a plugin window. */
namespace ff::fx::builtin
{
inline constexpr const char* formatName = "RAVENYR";   // projects from before the rename say "FretForge"

struct Info
{
    const char* id;          // e.g. "fretforge:eq"
    const char* name;        // "EQ"
    const char* category;    // "EQ", "Dynamics", "Delay", "Modulation", "Reverb"
    const char* description;
    bool instrument = false;   // a synth: MIDI in, audio out
};
const std::vector<Info>& all();
const Info* find (const juce::String& id);

juce::PluginDescription describe (const Info&);
/** Single-line description XML, as stored in a project's PLUGIN node. */
juce::String descriptionXml (const juce::String& id);
std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& id);
/** Plugin state (base64, like a PLUGIN node's state) with some parameters set, e.g. {"mix", 100} for a reverb on an FX
    track. Values are in the parameter's own units. */
juce::String stateWith (const juce::String& id, const std::map<juce::String, float>& values);

/** A factory preset: parameter values in their own units; every parameter not listed goes back to its default. */
struct Preset
{
    juce::String name, category;
    std::map<juce::String, float> values;
};
const std::vector<Preset>& factoryPresets (const juce::String& id);
/** Applies a preset to a built-in plugin instance (false if it is not one). */
bool applyPreset (juce::AudioPluginInstance&, const Preset&);

/** Whether a built-in expands as a compact panel inside a mixer rack slot (the others open their own window). */
bool hasRackPanel (const juce::String& id);
bool hasRackPanelForDescription (const juce::String& descXml);
/** The compact editor shown inside a mixer rack slot (nullptr for plugins that are not effects of ours). Sized by the host;
    the plugin must outlive it. onEdit gets the plugin's state before and after each change (for undo). */
std::unique_ptr<juce::Component> createRackPanel (juce::AudioPluginInstance&,
                                                  std::function<void (const juce::MemoryBlock& before, const juce::MemoryBlock& after)> onEdit = {});

/** The NAM Amp's "TONE3000..." button: set by the app, which opens its browser and calls `load` with each downloaded
    file (isIr: a cabinet; otherwise a NAM capture). Empty: the button is hidden. */
using Tone3000Browser = std::function<void (bool cab, std::function<void (const juce::File&, bool isIr)> load)>;
Tone3000Browser& tone3000Browser();

/** NAM Amp: points the plugin at a file (kind 0 = NAM capture, 1 = cabinet, 2 = second cabinet; an empty file clears it).
    False if the instance is not a NAM Amp. */
bool setNamAmpFile (juce::AudioPluginInstance&, int kind, const juce::File&);

/** The EQ's band listen (the editor's Listen button): only that band's part of the sound is heard. -1: off. */
bool setEqListenBand (juce::AudioPluginInstance&, int band);

/** Plugin format for the AudioPluginFormatManager, so the built-ins are created like any other plugin. */
class Format final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return formatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& f) override { return f.startsWith ("fretforge:"); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& id) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription& d) override { return find (d.fileOrIdentifier) != nullptr; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
protected:
    void createPluginInstance (const juce::PluginDescription&, double sampleRate, int blockSize, PluginCreationCallback) override;
};
} // namespace ff::fx::builtin
