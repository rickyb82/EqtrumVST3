#pragma once

#include "PluginProcessor.h"
#include "PresetManager.h"
#include "SpectrumView.h"
#include "TrackListPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

// One resizable window for both modes. Controls are not attached to the APVTS directly: they
// act on a "target" that is the local processor (own track), a remote slot (HUB) or read-only.
class EqtrumAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit EqtrumAudioProcessorEditor (EqtrumAudioProcessor& processorToEdit);
    ~EqtrumAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    enum class TargetKind
    {
        none,
        local,
        remote,
        readOnly
    };

    enum class Field
    {
        on,
        freq,
        gain,
        q,
        slope,
        dynOn,
        threshold,
        range,
        attack,
        release,
        source
    };

    class SidechainLed : public juce::Component
    {
    public:
        void setState (bool connected, float levelLinear);
        void paint (juce::Graphics& g) override;

    private:
        bool isConnected = false;
        float heldLevel = 0.0f;
    };

    struct SliderBinding
    {
        juce::Slider* slider = nullptr;
        std::function<int()> index;
        std::shared_ptr<int> dragIndex;
    };

    struct ButtonBinding
    {
        juce::Button* button = nullptr;
        std::function<int()> index;
    };

    struct ComboBinding
    {
        juce::ComboBox* box = nullptr;
        std::function<int()> index;
    };

    void timerCallback() override;

    // ---- target abstraction ----
    bool isHub() const;
    TargetKind getTargetKind() const;
    eqtrum::Slot* findSlot (std::uint64_t owner) const;
    eqtrum::Slot* getTargetSlot() const;
    bool canEditTarget() const;
    float getNormalised (int paramIndex) const;
    float getPlain (int paramIndex) const;
    float toNormalised (int paramIndex, float plain) const;
    float getTargetDynOffset (int band) const;
    double getTargetSampleRate() const;
    juce::Colour getTargetColour() const;
    void beginEdit (int paramIndex);
    void performEdit (int paramIndex, float normalised);
    void endEdit (int paramIndex);
    void setNormalisedWithGesture (int paramIndex, float normalised);
    void setPlainWithGesture (int paramIndex, float plain);
    bool pushCommand (std::uint64_t owner, const eqtrum::RemoteCommand& command);
    int fieldIndex (int band, Field field) const;

    // ---- setup ----
    void setupSlider (juce::Slider& slider, juce::Label& label, const juce::String& text,
                      double minValue, double maxValue, double midPoint,
                      const juce::String& suffix, int decimals, std::function<int()> index);
    void setupToggle (juce::Button& button, std::function<int()> index);
    void setupChoice (juce::ComboBox& box, const juce::StringArray& items, std::function<int()> index);
    void setupLabel (juce::Label& label, const juce::String& text);
    void setupCallbacks();

    // ---- refresh (30 Hz) ----
    void refreshTrackRows();
    void refreshControls();
    void refreshSpectra();
    void refreshCurve();
    void refreshInfo();
    void refreshPresetBox();
    void selectBand (int band);
    void selectTrack (std::uint64_t owner);

    // ---- presets ----
    void applyPresetEntry (int entryIndex);
    void stepPreset (int delta);
    void saveCurrentAsPreset();
    void deleteSelectedPreset();
    void showStatus (const juce::String& text);

    EqtrumAudioProcessor& eqProcessor;
    const eqtrum::ParamTable& table;
    eqtrum::PresetManager presets;

    juce::TooltipWindow tooltipWindow { this, 700 };
    SpectrumView spectrumView;
    TrackListPanel trackList;

    juce::ComboBox modeBox;
    juce::TextButton linearButton { "Linear Phase" };
    juce::ComboBox firBox;
    juce::Label latencyLabel;
    juce::Label outGainLabel;
    juce::Slider outGainSlider;
    juce::ToggleButton preEqButton { "Pre-EQ" };
    juce::ToggleButton showScButton { "Spettro SC" };
    SidechainLed scLed;
    juce::Label scLabel;
    juce::ComboBox eqRangeBox;
    juce::ComboBox specRangeBox;
    juce::Label statusLabel;

    juce::TextButton prevPresetButton { "<" };
    juce::TextButton nextPresetButton { ">" };
    juce::ComboBox presetBox;
    juce::TextEditor presetNameEditor;
    juce::TextButton savePresetButton { "Save as" };
    juce::TextButton deletePresetButton { "Delete" };

    std::array<juce::TextButton, eqtrum::kNumBands> bandButtons;
    juce::ToggleButton bandOnButton { "On" };
    juce::Slider freqSlider, gainSlider, qSlider;
    juce::Label freqLabel, gainLabel, qLabel;
    juce::ComboBox slopeBox;
    juce::Label slopeLabel;
    juce::ToggleButton dynOnButton { "Dyn" };
    juce::Slider thresholdSlider, rangeSlider, attackSlider, releaseSlider;
    juce::Label thresholdLabel, rangeLabel, attackLabel, releaseLabel;
    juce::ComboBox sourceBox;
    juce::Label sourceLabel;
    juce::Label dynNotice;

    std::vector<SliderBinding> sliderBindings;
    std::vector<ButtonBinding> buttonBindings;
    std::vector<ComboBinding> comboBindings;

    int selectedBand = 4;
    std::uint64_t selectedOwner = 0;
    std::set<std::uint64_t> soloOwners;
    bool updatingControls = false;
    bool deleteArmed = false;
    int deleteArmTicks = 0;
    int statusTicks = 0;
    int lastPresetIndex = -1;
    std::vector<float> lastCurveKey;
    std::uint64_t lastCurveOwner = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqtrumAudioProcessorEditor)
};
