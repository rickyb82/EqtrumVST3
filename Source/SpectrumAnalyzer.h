#pragma once

#include "InstanceRegistry.h"
#include "Params.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace eqtrum
{
// One low-priority analysis thread per instance. The audio thread only writes into
// pre-allocated AbstractFifos; FFT, banding, smoothing and publishing happen here.
class SpectrumAnalyzer : private juce::Thread
{
public:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;   // 4096
    static constexpr int kFifoSize = 1 << 16;
    static constexpr float kFloorDb = -140.0f;

    SpectrumAnalyzer (std::atomic<int>& slotIndexToPublish, std::uint64_t ownerId);
    ~SpectrumAnalyzer() override;

    void startAnalysis();
    void stopAnalysis();
    void setSampleRate (double newSampleRate) noexcept;

    // audio thread, wait-free
    void pushMain (const float* left, const float* right, int numSamples) noexcept;
    void pushSidechain (const float* left, const float* right, int numSamples) noexcept;

    const SpectrumSeqlock& getLocalSpectrum() const noexcept           { return localMain; }
    const SpectrumSeqlock& getLocalSidechainSpectrum() const noexcept  { return localSidechain; }

private:
    struct Stream
    {
        Stream();

        juce::AbstractFifo fifo { kFifoSize };
        std::vector<float> fifoData;
        std::vector<float> frame;
        int frameFill = 0;
        std::array<float, kNumSpectrumBins> smoothed {};
        juce::uint32 lastFrameMs = 0;
    };

    void run() override;
    static void push (Stream& stream, const float* left, const float* right, int numSamples) noexcept;
    void drain (Stream& stream);
    void analyseFrame (Stream& stream);
    void configureForSampleRate (double rate);
    static void resetStream (Stream& stream);
    static void decayIfStale (Stream& stream, juce::uint32 now);
    void publish();
    void writeHeartbeat (juce::uint32 now);

    std::atomic<int>& slotIndex;
    const std::uint64_t instanceId;
    std::atomic<double> sampleRate { 44100.0 };

    Stream mainStream;
    Stream sidechainStream;
    SpectrumSeqlock localMain;
    SpectrumSeqlock localSidechain;

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { static_cast<size_t> (kFftSize),
                                                 juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> fftBuffer;
    std::array<int, kNumSpectrumBins> binLow {};
    std::array<int, kNumSpectrumBins> binHigh {};
    std::array<float, kNumSpectrumBins> binPosition {};

    double configuredRate = 0.0;
    int hopSize = 2048;
    float attackCoef = 0.5f;
    float releaseCoef = 0.9f;
};
} // namespace eqtrum
