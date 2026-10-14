#include "Params.h"

#include <cmath>

namespace eqtrum
{
namespace
{
enum class Kind
{
    floating,
    boolean,
    choice
};

struct Spec
{
    juce::String id;
    juce::String name;
    juce::String unit;
    Kind kind = Kind::floating;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    float step = 0.0f;
    bool logarithmic = false;
    juce::StringArray choices;
    int defaultChoice = 0;
};

Spec makeFloat (const juce::String& id, const juce::String& name, float minValue, float maxValue,
                float defaultValue, float step, bool logarithmic, const juce::String& unit)
{
    Spec s;
    s.id = id;
    s.name = name;
    s.unit = unit;
    s.kind = Kind::floating;
    s.minValue = minValue;
    s.maxValue = maxValue;
    s.defaultValue = defaultValue;
    s.step = step;
    s.logarithmic = logarithmic;
    return s;
}

Spec makeBool (const juce::String& id, const juce::String& name, bool defaultValue)
{
    Spec s;
    s.id = id;
    s.name = name;
    s.kind = Kind::boolean;
    s.defaultValue = defaultValue ? 1.0f : 0.0f;
    return s;
}

Spec makeChoice (const juce::String& id, const juce::String& name, const juce::StringArray& choices, int defaultChoice)
{
    Spec s;
    s.id = id;
    s.name = name;
    s.kind = Kind::choice;
    s.choices = choices;
    s.defaultChoice = defaultChoice;
    return s;
}

juce::NormalisableRange<float> makeLogRange (float minValue, float maxValue)
{
    return juce::NormalisableRange<float> (
        minValue, maxValue,
        [] (float start, float end, float normalised)
        {
            const float n = std::isfinite (normalised) ? juce::jlimit (0.0f, 1.0f, normalised) : 0.0f;
            return start * std::pow (end / start, n);
        },
        [] (float start, float end, float value)
        {
            const float v = std::isfinite (value) ? juce::jlimit (start, end, value) : start;
            return juce::jlimit (0.0f, 1.0f, std::log (v / start) / std::log (end / start));
        },
        [] (float start, float end, float value)
        {
            return std::isfinite (value) ? juce::jlimit (start, end, value) : start;
        });
}

const float defaultFrequencies[kNumBands] = { 30.0f, 18000.0f, 100.0f, 8000.0f,
                                              120.0f, 300.0f, 800.0f, 2000.0f, 4500.0f, 9000.0f };

std::vector<Spec> buildSpecs()
{
    std::vector<Spec> specs;
    specs.push_back (makeFloat ("outGain", "Output Gain", -24.0f, 24.0f, 0.0f, 0.01f, false, "dB"));
    specs.push_back (makeChoice ("phase", "Phase", { "Minimum", "Linear" }, 0));
    specs.push_back (makeChoice ("firLen", "Linear FIR Length", { "4096", "8192", "16384" }, 1));

    for (int b = 0; b < kNumBands; ++b)
    {
        const juce::String prefix = getBandShortName (b) + " ";
        const bool isBell = getBandType (b) == BandType::bell;

        // Shelves and bells start enabled at 0 dB (transparent), HP/LP start disabled.
        specs.push_back (makeBool (makeBandParamId (b, "on"), prefix + "On", bandHasGain (b)));
        specs.push_back (makeFloat (makeBandParamId (b, "freq"), prefix + "Freq", 20.0f, 20000.0f,
                                    defaultFrequencies[b], 0.0f, true, "Hz"));

        if (bandHasGain (b))
            specs.push_back (makeFloat (makeBandParamId (b, "gain"), prefix + "Gain", -18.0f, 18.0f, 0.0f, 0.01f, false, "dB"));

        specs.push_back (makeFloat (makeBandParamId (b, "q"), prefix + "Q", 0.1f, 18.0f,
                                    isBell ? 1.0f : 0.707f, 0.0f, true, ""));

        if (bandHasSlope (b))
            specs.push_back (makeChoice (makeBandParamId (b, "slope"), prefix + "Slope", { "12 dB/oct", "24 dB/oct" }, 0));

        if (bandHasDynamics (b))
        {
            specs.push_back (makeBool (makeBandParamId (b, "dynOn"), prefix + "Dyn On", false));
            specs.push_back (makeFloat (makeBandParamId (b, "thr"), prefix + "Dyn Threshold", -60.0f, 0.0f, -24.0f, 0.1f, false, "dB"));
            specs.push_back (makeFloat (makeBandParamId (b, "range"), prefix + "Dyn Range", -12.0f, 12.0f, -6.0f, 0.1f, false, "dB"));
            specs.push_back (makeFloat (makeBandParamId (b, "att"), prefix + "Dyn Attack", 0.1f, 200.0f, 10.0f, 0.0f, true, "ms"));
            specs.push_back (makeFloat (makeBandParamId (b, "rel"), prefix + "Dyn Release", 5.0f, 2000.0f, 150.0f, 0.0f, true, "ms"));
            specs.push_back (makeChoice (makeBandParamId (b, "src"), prefix + "Dyn Source", { "Internal", "Sidechain" }, 0));
        }
    }

    return specs;
}
} // namespace

BandType getBandType (int band) noexcept
{
    switch (band)
    {
        case 0:  return BandType::highPass;
        case 1:  return BandType::lowPass;
        case 2:  return BandType::lowShelf;
        case 3:  return BandType::highShelf;
        default: return BandType::bell;
    }
}

bool bandHasGain (int band) noexcept      { return band >= 2 && band < kNumBands; }
bool bandHasSlope (int band) noexcept     { return band == 0 || band == 1; }
bool bandHasDynamics (int band) noexcept  { return band >= 2 && band < kNumBands; }

juce::String getBandShortName (int band)
{
    switch (band)
    {
        case 0:  return "HP";
        case 1:  return "LP";
        case 2:  return "LS";
        case 3:  return "HS";
        default: break;
    }

    if (band >= 4 && band < kNumBands)
        return "Bell " + juce::String (band - 3);

    return "?";
}

int firLengthForChoice (int choiceIndex) noexcept
{
    switch (choiceIndex)
    {
        case 0:  return 4096;
        case 2:  return 16384;
        default: return 8192;
    }
}

juce::String makeBandParamId (int band, const char* suffix)
{
    return "b" + juce::String (band) + "_" + juce::String (suffix);
}

int ParamTable::indexOf (const juce::String& id) const noexcept
{
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id)
            return static_cast<int> (i);

    return -1;
}

const ParamTable& getParamTable()
{
    static const ParamTable table = []
    {
        ParamTable t;

        for (const auto& spec : buildSpecs())
            t.ids.push_back (spec.id);

        t.outGain = t.indexOf ("outGain");
        t.phase = t.indexOf ("phase");
        t.firLength = t.indexOf ("firLen");

        for (int b = 0; b < kNumBands; ++b)
        {
            auto& bi = t.bands[static_cast<size_t> (b)];
            bi.on = t.indexOf (makeBandParamId (b, "on"));
            bi.freq = t.indexOf (makeBandParamId (b, "freq"));
            bi.gain = t.indexOf (makeBandParamId (b, "gain"));
            bi.q = t.indexOf (makeBandParamId (b, "q"));
            bi.slope = t.indexOf (makeBandParamId (b, "slope"));
            bi.dynOn = t.indexOf (makeBandParamId (b, "dynOn"));
            bi.threshold = t.indexOf (makeBandParamId (b, "thr"));
            bi.range = t.indexOf (makeBandParamId (b, "range"));
            bi.attack = t.indexOf (makeBandParamId (b, "att"));
            bi.release = t.indexOf (makeBandParamId (b, "rel"));
            bi.source = t.indexOf (makeBandParamId (b, "src"));
        }

        return t;
    }();

    return table;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& spec : buildSpecs())
    {
        const juce::ParameterID pid { spec.id, 1 };

        switch (spec.kind)
        {
            case Kind::floating:
            {
                auto range = spec.logarithmic ? makeLogRange (spec.minValue, spec.maxValue)
                                              : juce::NormalisableRange<float> (spec.minValue, spec.maxValue, spec.step);
                layout.add (std::make_unique<juce::AudioParameterFloat> (pid, spec.name, range, spec.defaultValue,
                                                                         juce::AudioParameterFloatAttributes().withLabel (spec.unit)));
                break;
            }

            case Kind::boolean:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, spec.name, spec.defaultValue > 0.5f));
                break;

            case Kind::choice:
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, spec.name, spec.choices, spec.defaultChoice));
                break;
        }
    }

    return layout;
}
} // namespace eqtrum
