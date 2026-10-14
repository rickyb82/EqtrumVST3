#pragma once

#include "EqMath.h"
#include "Params.h"

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <vector>

namespace eqtrum
{
// Builds the linear-phase impulse response on its own low-priority thread, only when the
// parameters change (rate limited). The finished IR is handed to the audio thread through a
// SpinLock that the audio thread only try-locks, and the audio thread passes it to
// juce::dsp::Convolution::loadImpulseResponse (which JUCE documents as wait-free and requires
// to be called from the same thread as process()).
class LinearPhaseBuilder : private juce::Thread
{
public:
    static constexpr juce::uint32 kMinRebuildIntervalMs = 100;

    LinearPhaseBuilder (const ParamTable& table, const std::vector<std::atomic<float>*>& rawParams);
    ~LinearPhaseBuilder() override;

    void startBuilder();
    void stopBuilder();
    void setSampleRate (double newSampleRate) noexcept;
    void setActive (bool shouldBeActive) noexcept;
    void invalidate() noexcept;

    // Audio thread: hands a pending IR to the convolution. Never blocks.
    bool deliverTo (juce::dsp::Convolution& convolution);

    // Synchronous build from the current parameter values (prepareToPlay only, may allocate).
    void buildNow (double rate, int length, juce::AudioBuffer<float>& out) const;

private:
    void run() override;
    void takeSnapshot (std::vector<float>& values) const;
    BandSpec bandSpecFromSnapshot (const std::vector<float>& values, int band) const;
    void buildImpulse (const std::vector<float>& values, double rate, int length, juce::AudioBuffer<float>& out) const;
    float snapshotValue (const std::vector<float>& values, int index, float fallback) const noexcept;

    const ParamTable& table;
    const std::vector<std::atomic<float>*>& raw;

    std::atomic<double> sampleRate { 0.0 };
    std::atomic<bool> active { false };
    std::atomic<bool> forceRebuild { false };

    juce::SpinLock pendingLock;
    juce::AudioBuffer<float> pendingIr;
    double pendingSampleRate = 44100.0;
    bool hasPending = false;
};
} // namespace eqtrum
