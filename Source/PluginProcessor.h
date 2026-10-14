#pragma once

#include "EqEngine.h"
#include "InstanceRegistry.h"
#include "LinearPhaseBuilder.h"
#include "Params.h"
#include "SpectrumAnalyzer.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class EqtrumAudioProcessor : public juce::AudioProcessor,
                             private juce::Timer
{
public:
    enum Mode
    {
        modeTrack = 0,
        modeHub = 1
    };

    EqtrumAudioProcessor();
    ~EqtrumAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void updateTrackProperties (const TrackProperties& properties) override;

    //==============================================================================
    // Editor API (message thread)
    const eqtrum::ParamTable& getTable() const noexcept { return table; }
    const std::vector<juce::RangedAudioParameter*>& getParameterList() const noexcept { return paramList; }
    std::uint64_t getInstanceId() const noexcept { return instanceId; }
    int getSlotIndex() const noexcept { return slotIndex.load (std::memory_order_acquire); }

    int getMode() const noexcept { return mode.load (std::memory_order_acquire); }
    void setMode (int newMode);
    bool getAnalyzerPreEq() const noexcept { return analyzerPreEq.load (std::memory_order_acquire); }
    void setAnalyzerPreEq (bool shouldTapPreEq);

    juce::String getTrackName() const;
    void setTrackNameManual (const juce::String& newName);
    juce::Colour getTrackColour() const;
    void setTrackColourManual (juce::Colour newColour);
    void setTrackVisible (bool shouldBeVisible);

    float getSidechainLevel() const noexcept { return sidechainLevel.load (std::memory_order_relaxed); }
    bool isSidechainConnected() const noexcept { return sidechainConnected.load (std::memory_order_relaxed); }
    float getDynamicOffsetDb (int band) const noexcept { return eq.getDynamicOffsetDb (band); }
    double getEffectiveSampleRate() const noexcept { return sampleRateForUi.load (std::memory_order_relaxed); }
    const eqtrum::SpectrumSeqlock& getLocalSpectrum() const noexcept { return analyzer.getLocalSpectrum(); }

    void applyNormalisedValue (int paramIndex, float normalised);   // full host gesture

private:
    enum Path
    {
        pathPassThrough = 0,
        pathMinimum = 1,
        pathLinear = 2
    };

    enum class Fade
    {
        running,
        fadingOut,
        waitingForIr,
        fadingIn
    };

    void timerCallback() override;
    float readRaw (int index, float fallback) const noexcept;
    int computeDesiredPath() const noexcept;
    int computeDesiredFirLength() const noexcept;
    int computeDesiredLatency() const noexcept;
    void updateLatency();
    void applyLatency (int samples);
    void processLinear (juce::AudioBuffer<float>& mainBuffer, int numSamples) noexcept;
    void applyOutputGain (float* left, float* right, int numSamples) noexcept;
    void applyFadeAndTransitions (float* left, float* right, int numSamples) noexcept;
    void switchToPendingPath() noexcept;
    static void sanitise (float* data, int numSamples) noexcept;
    static bool sanitiseAndCheck (float* data, int numSamples) noexcept;

    void ensureSlot();
    void publishToSlot (eqtrum::Slot& slot);
    void drainCommands (eqtrum::Slot& slot);
    juce::String getEffectiveName (int index) const;
    juce::Colour getEffectiveColour (int index) const;

    //==============================================================================
    const eqtrum::ParamTable& table;
    juce::AudioProcessorValueTreeState apvts;
    std::vector<juce::RangedAudioParameter*> paramList;
    std::vector<std::atomic<float>*> rawParams;
    std::atomic<float> dummyParam { 0.0f };

    const std::uint64_t instanceId;
    std::atomic<int> slotIndex { -1 };
    std::atomic<int> mode { modeTrack };
    std::atomic<bool> analyzerPreEq { false };
    std::atomic<float> sidechainLevel { 0.0f };
    std::atomic<bool> sidechainConnected { false };
    std::atomic<double> sampleRateForUi { 44100.0 };

    eqtrum::EqEngine eq;
    juce::dsp::Convolution convolution;
    eqtrum::LinearPhaseBuilder irBuilder;
    eqtrum::SpectrumAnalyzer analyzer;
    juce::SmoothedValue<float> outputGain;

    // audio-thread state
    double currentRate = 44100.0;
    int convBlockSize = 512;
    int activePath = pathMinimum;
    int activeFirLength = 8192;
    int pendingPath = pathMinimum;
    int pendingFirLength = 8192;
    Fade fade = Fade::fadingIn;
    float fadeGain = 0.0f;
    float fadeStep = 0.001f;
    int irWaitSamples = 0;
    int irHoldSamples = -1;
    juce::uint32 irWaitStartMs = 0;

    // host bypass: delay line keeps the reported latency (max 8192 samples)
    static constexpr int kBypassDelaySize = 16384;   // power of two
    std::atomic<int> reportedLatency { 0 };
    std::array<std::vector<float>, 2> bypassDelay;
    int bypassWritePos = 0;
    bool bypassedLastBlock = false;

    // track metadata (message thread, guarded for get/setStateInformation)
    juce::CriticalSection metaLock;
    juce::String trackName;
    bool nameCustom = false;
    juce::Colour trackColour;
    bool colourCustom = false;
    bool trackVisible = true;
    bool visibilityPending = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqtrumAudioProcessor)
};
