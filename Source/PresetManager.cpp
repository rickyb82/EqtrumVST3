#include "PresetManager.h"

#include <algorithm>
#include <cmath>

namespace eqtrum
{
PresetManager::PresetManager (const ParamTable& t, const std::vector<juce::RangedAudioParameter*>& parameters)
    : table (t), params (parameters), factory (makeFactoryPresets())
{
    refresh();
}

std::vector<PresetManager::FactoryPreset> PresetManager::makeFactoryPresets()
{
    // Bands: b0 HP, b1 LP, b2 low shelf, b3 high shelf, b4..b9 bell 1..6.
    // Values not listed keep their defaults (shelves/bells on at 0 dB, HP/LP off, Minimum phase).
    std::vector<FactoryPreset> p;
    p.push_back ({ "Flat", {} });
    p.push_back ({ "Low Cut 30 Hz", { { "b0_on", 1.0f }, { "b0_freq", 30.0f }, { "b0_q", 0.707f }, { "b0_slope", 1.0f } } });
    p.push_back ({ "Vocal Presence", { { "b0_on", 1.0f }, { "b0_freq", 80.0f }, { "b0_slope", 0.0f },
                                       { "b5_freq", 250.0f }, { "b5_gain", -2.0f }, { "b5_q", 1.2f },
                                       { "b7_freq", 3000.0f }, { "b7_gain", 3.0f }, { "b7_q", 1.0f },
                                       { "b3_freq", 10000.0f }, { "b3_gain", 2.0f }, { "b3_q", 0.7f } } });
    p.push_back ({ "Bass Tighten", { { "b0_on", 1.0f }, { "b0_freq", 35.0f }, { "b0_slope", 1.0f },
                                     { "b5_freq", 180.0f }, { "b5_gain", 0.0f }, { "b5_q", 1.4f },
                                     { "b5_dynOn", 1.0f }, { "b5_thr", -24.0f }, { "b5_range", -6.0f },
                                     { "b5_att", 10.0f }, { "b5_rel", 120.0f }, { "b5_src", 0.0f },
                                     { "b6_freq", 800.0f }, { "b6_gain", -2.0f }, { "b6_q", 1.0f } } });
    p.push_back ({ "Air Boost", { { "b3_freq", 10000.0f }, { "b3_gain", 4.0f }, { "b3_q", 0.7f },
                                  { "b9_freq", 16000.0f }, { "b9_gain", 1.5f }, { "b9_q", 0.8f } } });
    return p;
}

juce::File PresetManager::getPresetDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("SpectrumMixer")
               .getChildFile ("Presets");
}

void PresetManager::refresh()
{
    entries.clear();

    for (size_t i = 0; i < factory.size(); ++i)
    {
        Entry e;
        e.name = factory[i].name;
        e.isFactory = true;
        e.factoryIndex = static_cast<int> (i);
        entries.push_back (e);
    }

    auto dir = getPresetDirectory();

    if (! dir.isDirectory())
        dir.createDirectory();

    if (! dir.isDirectory())
        return;

    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.xml");
    std::vector<Entry> user;

    for (const auto& f : files)
    {
        Entry e;
        e.name = f.getFileNameWithoutExtension();
        e.file = f;
        user.push_back (e);
    }

    std::sort (user.begin(), user.end(), [] (const Entry& a, const Entry& b)
               { return a.name.compareNatural (b.name) < 0; });

    for (const auto& e : user)
        entries.push_back (e);
}

int PresetManager::indexOfName (const juce::String& name) const noexcept
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (! entries[i].isFactory && entries[i].name == name)
            return static_cast<int> (i);

    return -1;
}

void PresetManager::fillDefaults (std::vector<float>& normalisedOut) const
{
    normalisedOut.assign (params.size(), 0.0f);

    for (size_t i = 0; i < params.size(); ++i)
        if (params[i] != nullptr)
            normalisedOut[i] = params[i]->getDefaultValue();
}

void PresetManager::setPlain (std::vector<float>& normalisedOut, const juce::String& id, float plain) const
{
    const int index = table.indexOf (id);

    if (index < 0 || index >= static_cast<int> (params.size()) || ! std::isfinite (plain))
        return;

    if (auto* p = params[static_cast<size_t> (index)])
        normalisedOut[static_cast<size_t> (index)] = juce::jlimit (0.0f, 1.0f, p->convertTo0to1 (plain));
}

bool PresetManager::getPresetValues (int entryIndex, std::vector<float>& normalisedOut) const
{
    if (entryIndex < 0 || entryIndex >= static_cast<int> (entries.size()))
        return false;

    const auto& entry = entries[static_cast<size_t> (entryIndex)];
    fillDefaults (normalisedOut);

    if (entry.isFactory)
    {
        if (entry.factoryIndex < 0 || entry.factoryIndex >= static_cast<int> (factory.size()))
            return false;

        for (const auto& pv : factory[static_cast<size_t> (entry.factoryIndex)].plainValues)
            setPlain (normalisedOut, pv.first, pv.second);

        return true;
    }

    auto xml = juce::XmlDocument::parse (entry.file);

    if (xml == nullptr || ! xml->hasTagName ("EqtrumPreset"))
        return false;

    for (auto* child : xml->getChildWithTagNameIterator ("PARAM"))
    {
        const auto id = child->getStringAttribute ("id");
        const auto value = static_cast<float> (child->getDoubleAttribute ("value", std::nan ("")));
        setPlain (normalisedOut, id, value);
    }

    return true;
}

bool PresetManager::saveUserPreset (const juce::String& name, const std::vector<float>& normalised, juce::String& error)
{
    const auto cleanName = juce::File::createLegalFileName (name.trim());

    if (cleanName.isEmpty())
    {
        error = "Nome preset non valido";
        return false;
    }

    auto dir = getPresetDirectory();

    if (! dir.isDirectory())
    {
        const auto result = dir.createDirectory();

        if (result.failed())
        {
            error = "Impossibile creare la cartella: " + result.getErrorMessage();
            return false;
        }
    }

    juce::XmlElement xml ("EqtrumPreset");
    xml.setAttribute ("version", kStateVersion);
    xml.setAttribute ("name", cleanName);

    const size_t count = std::min (normalised.size(), params.size());

    for (size_t i = 0; i < count && i < table.ids.size(); ++i)
    {
        if (auto* p = params[i])
        {
            auto* child = xml.createNewChildElement ("PARAM");
            child->setAttribute ("id", table.ids[i]);
            child->setAttribute ("value", static_cast<double> (p->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised[i]))));
        }
    }

    const auto file = dir.getChildFile (cleanName + ".xml");

    if (! xml.writeTo (file))
    {
        error = "Scrittura fallita: " + file.getFullPathName();
        return false;
    }

    refresh();
    return true;
}

bool PresetManager::deleteUserPreset (int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= static_cast<int> (entries.size()))
        return false;

    const auto& entry = entries[static_cast<size_t> (entryIndex)];

    if (entry.isFactory || ! entry.file.existsAsFile())
        return false;

    const bool ok = entry.file.deleteFile();
    refresh();
    return ok;
}
} // namespace eqtrum
