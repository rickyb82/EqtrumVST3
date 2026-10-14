#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <vector>

namespace eqtrum
{
// Band layout: 0 = HP, 1 = LP, 2 = Low shelf, 3 = High shelf, 4..9 = Bell 1..6
constexpr int kNumBands = 10;
constexpr int kNumSpectrumBins = 512;
constexpr int kMaxParams = 128;      // capacity of the parameter mirror in a registry slot
constexpr int kStateVersion = 1;

enum class BandType
{
    highPass,
    lowPass,
    lowShelf,
    highShelf,
    bell
};

BandType getBandType (int band) noexcept;
bool bandHasGain (int band) noexcept;      // shelves + bells
bool bandHasSlope (int band) noexcept;     // HP + LP
bool bandHasDynamics (int band) noexcept;  // shelves + bells (8 bands)
juce::String getBandShortName (int band);
int firLengthForChoice (int choiceIndex) noexcept;   // 0 -> 4096, 1 -> 8192, 2 -> 16384
juce::String makeBandParamId (int band, const char* suffix);   // e.g. "b3_freq"

struct BandParamIndices
{
    int on = -1, freq = -1, gain = -1, q = -1, slope = -1;
    int dynOn = -1, threshold = -1, range = -1, attack = -1, release = -1, source = -1;
};

// Stable list of parameter IDs. The index of an ID in this table is identical in every
// instance of the same binary, so it is used as the parameter key in remote commands.
struct ParamTable
{
    std::vector<juce::String> ids;
    int outGain = -1;
    int phase = -1;
    int firLength = -1;
    std::array<BandParamIndices, kNumBands> bands;

    int indexOf (const juce::String& id) const noexcept;
    int numParams() const noexcept { return static_cast<int> (ids.size()); }
};

const ParamTable& getParamTable();
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
} // namespace eqtrum
