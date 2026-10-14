#pragma once

#include "EqMath.h"
#include "Params.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

namespace eqtrum
{
// Minimum-phase EQ: cascaded biquads, per-band dynamics, coefficients refreshed every 32 samples.
// All methods except the constructor and prepare() are real-time safe (no allocation, no locks).
class EqEngine
{
public:
    static constexpr int kSubBlock = 32;

    EqEngine (const ParamTable& table, const std::vector<std::atomic<float>*>& rawParams);

    void prepare (double sampleRate);
    void reset() noexcept;            // clears filter states and snaps smoothers to their targets
    void updateTargets() noexcept;    // reads the parameter atomics, once per host block
    void process (float* left, float* right, const float* scLeft, const float* scRight,
                  int numSamples, bool dynamicsEnabled) noexcept;

    float getDynamicOffsetDb (int band) const noexcept;

private:
    struct Band
    {
        BandType type = BandType::bell;
        bool hasGain = false;
        bool hasSlope = false;
        bool hasDynamics = false;

        // targets read from parameters
        bool on = false;
        float freq = 1000.0f;
        float gain = 0.0f;
        float q = 0.707f;
        bool slope24 = false;
        bool dynOn = false;
        float threshold = -24.0f;
        float range = -6.0f;
        float attackMs = 10.0f;
        float releaseMs = 150.0f;
        bool useSidechain = false;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> freqSmoothed, qSmoothed;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainSmoothed, mixSmoothed, slopeSmoothed, dynSmoothed;

        std::array<Biquad, 2> stage1;
        std::array<Biquad, 2> stage2;
        Biquad detector;
        float envelope = 0.0f;
        bool statesInUse = false;
    };

    float readParam (int index, float fallback) const noexcept;
    void processSubBlock (float* left, float* right, const float* scLeft, const float* scRight,
                          int numSamples, bool dynamicsEnabled) noexcept;
    static void resetBandStates (Band& band) noexcept;

    const ParamTable& table;
    const std::vector<std::atomic<float>*>& raw;
    double sampleRate = 44100.0;
    std::array<Band, kNumBands> bands;
    std::array<std::atomic<float>, kNumBands> dynOffsetForUi;
};
} // namespace eqtrum
