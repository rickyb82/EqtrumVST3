#pragma once

#include "Params.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace eqtrum
{
// Factory presets (in code) + user presets (XML files). A preset stores every EQ, dynamics and
// sidechain parameter as plain values; track name, colour, mode and visibility are not stored.
class PresetManager
{
public:
    struct Entry
    {
        juce::String name;
        bool isFactory = false;
        int factoryIndex = -1;
        juce::File file;
    };

    PresetManager (const ParamTable& table, const std::vector<juce::RangedAudioParameter*>& parameters);

    static juce::File getPresetDirectory();

    void refresh();
    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    int indexOfName (const juce::String& name) const noexcept;

    // Fills one normalised value per parameter (ParamTable order). Missing values -> defaults.
    bool getPresetValues (int entryIndex, std::vector<float>& normalisedOut) const;
    bool saveUserPreset (const juce::String& name, const std::vector<float>& normalised, juce::String& error);
    bool deleteUserPreset (int entryIndex);

private:
    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> plainValues;
    };

    static std::vector<FactoryPreset> makeFactoryPresets();
    void fillDefaults (std::vector<float>& normalisedOut) const;
    void setPlain (std::vector<float>& normalisedOut, const juce::String& id, float plain) const;

    const ParamTable& table;
    const std::vector<juce::RangedAudioParameter*>& params;
    std::vector<FactoryPreset> factory;
    std::vector<Entry> entries;
};
} // namespace eqtrum
