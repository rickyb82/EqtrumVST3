#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
const juce::Colour windowBackground (0xff0d0f12);
const juce::Colour textColour (0xffd1d5db);
const juce::Colour warningColour (0xffffa726);
} // namespace

//==============================================================================
void EqtrumAudioProcessorEditor::SidechainLed::setState (bool connected, float levelLinear)
{
    const float level = std::isfinite (levelLinear) ? levelLinear : 0.0f;
    const float newHeld = std::max (level, heldLevel * 0.85f);

    if (connected != isConnected || std::abs (newHeld - heldLevel) > 1.0e-4f)
    {
        isConnected = connected;
        heldLevel = newHeld;
        repaint();
    }
}

void EqtrumAudioProcessorEditor::SidechainLed::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    juce::Colour c (0xff3f4450);

    if (isConnected)
        c = heldLevel > 0.001f ? juce::Colour (0xff22c55e) : juce::Colour (0xff14532d);

    g.setColour (c);
    g.fillEllipse (r);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (r, 1.0f);
}

//==============================================================================
EqtrumAudioProcessorEditor::EqtrumAudioProcessorEditor (EqtrumAudioProcessor& processorToEdit)
    : AudioProcessorEditor (&processorToEdit),
      eqProcessor (processorToEdit),
      table (processorToEdit.getTable()),
      presets (processorToEdit.getTable(), processorToEdit.getParameterList())
{
    selectedOwner = isHub() ? 0 : eqProcessor.getInstanceId();

    addAndMakeVisible (spectrumView);
    addAndMakeVisible (trackList);

    // ---- top bar ----
    modeBox.addItem ("TRACK", 1);
    modeBox.addItem ("HUB", 2);
    modeBox.setSelectedId (isHub() ? 2 : 1, juce::dontSendNotification);
    modeBox.setTooltip ("TRACK: EQ su questa traccia. HUB: solo visualizzazione e controllo remoto");
    addAndMakeVisible (modeBox);

    setupToggle (linearButton, [this] { return table.phase; });
    linearButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2563eb));
    setupChoice (firBox, { "FIR 4096", "FIR 8192", "FIR 16384" }, [this] { return table.firLength; });

    setupLabel (latencyLabel, "Latenza: 0.0 ms");
    setupSlider (outGainSlider, outGainLabel, "Out", -24.0, 24.0, 0.0, " dB", 1, [this] { return table.outGain; });
    outGainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    outGainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);

    preEqButton.setToggleState (eqProcessor.getAnalyzerPreEq(), juce::dontSendNotification);
    preEqButton.setTooltip ("Analisi pre-EQ (acceso) o post-EQ (spento)");
    addAndMakeVisible (preEqButton);

    showScButton.setTooltip ("Mostra lo spettro del sidechain della traccia selezionata");
    addAndMakeVisible (showScButton);
    addAndMakeVisible (scLed);
    setupLabel (scLabel, "SC assente");

    eqRangeBox.addItemList ({ "EQ +/-6 dB", "EQ +/-12 dB", "EQ +/-18 dB", "EQ +/-24 dB", "EQ +/-30 dB" }, 1);
    eqRangeBox.setSelectedId (3, juce::dontSendNotification);
    addAndMakeVisible (eqRangeBox);
    specRangeBox.addItemList ({ "Spettro -60 dB", "Spettro -90 dB", "Spettro -120 dB" }, 1);
    specRangeBox.setSelectedId (2, juce::dontSendNotification);
    addAndMakeVisible (specRangeBox);

    setupLabel (statusLabel, {});
    statusLabel.setColour (juce::Label::textColourId, warningColour);

    // ---- presets ----
    addAndMakeVisible (prevPresetButton);
    addAndMakeVisible (nextPresetButton);
    addAndMakeVisible (presetBox);
    presetNameEditor.setTextToShowWhenEmpty ("Nome nuovo preset", juce::Colours::grey);
    addAndMakeVisible (presetNameEditor);
    addAndMakeVisible (savePresetButton);
    addAndMakeVisible (deletePresetButton);

    // ---- band panel ----
    for (int b = 0; b < eqtrum::kNumBands; ++b)
    {
        auto& button = bandButtons[static_cast<size_t> (b)];
        button.setButtonText (eqtrum::getBandShortName (b));
        button.setClickingTogglesState (true);
        button.setRadioGroupId (4242);
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff374151));
        button.onClick = [this, b] { selectBand (b); };
        addAndMakeVisible (button);
    }

    setupToggle (bandOnButton, [this] { return fieldIndex (selectedBand, Field::on); });
    setupSlider (freqSlider, freqLabel, "Freq", 20.0, 20000.0, 1000.0, " Hz", 0, [this] { return fieldIndex (selectedBand, Field::freq); });
    setupSlider (gainSlider, gainLabel, "Gain", -18.0, 18.0, 0.0, " dB", 1, [this] { return fieldIndex (selectedBand, Field::gain); });
    setupSlider (qSlider, qLabel, "Q", 0.1, 18.0, 1.0, "", 2, [this] { return fieldIndex (selectedBand, Field::q); });
    setupChoice (slopeBox, { "12 dB/oct", "24 dB/oct" }, [this] { return fieldIndex (selectedBand, Field::slope); });
    setupLabel (slopeLabel, "Slope");

    setupToggle (dynOnButton, [this] { return fieldIndex (selectedBand, Field::dynOn); });
    setupSlider (thresholdSlider, thresholdLabel, "Threshold", -60.0, 0.0, 0.0, " dB", 1, [this] { return fieldIndex (selectedBand, Field::threshold); });
    setupSlider (rangeSlider, rangeLabel, "Range", -12.0, 12.0, 0.0, " dB", 1, [this] { return fieldIndex (selectedBand, Field::range); });
    setupSlider (attackSlider, attackLabel, "Attack", 0.1, 200.0, 10.0, " ms", 1, [this] { return fieldIndex (selectedBand, Field::attack); });
    setupSlider (releaseSlider, releaseLabel, "Release", 5.0, 2000.0, 150.0, " ms", 0, [this] { return fieldIndex (selectedBand, Field::release); });
    setupChoice (sourceBox, { "Internal", "Sidechain" }, [this] { return fieldIndex (selectedBand, Field::source); });
    setupLabel (sourceLabel, "Source");

    setupLabel (dynNotice, {});
    dynNotice.setColour (juce::Label::textColourId, warningColour);
    dynNotice.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    setupCallbacks();
    refreshPresetBox();
    selectBand (4);

    setResizable (true, true);
    setResizeLimits (960, 600, 2560, 1600);
    setSize (1100, 680);

    startTimerHz (30);
}

EqtrumAudioProcessorEditor::~EqtrumAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void EqtrumAudioProcessorEditor::setupLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::FontOptions (12.0f));
    label.setColour (juce::Label::textColourId, textColour);
    addAndMakeVisible (label);
}

void EqtrumAudioProcessorEditor::setupSlider (juce::Slider& slider, juce::Label& label, const juce::String& text,
                                              double minValue, double maxValue, double midPoint,
                                              const juce::String& suffix, int decimals, std::function<int()> index)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
    slider.setRange (minValue, maxValue, 0.0);

    if (midPoint > minValue && midPoint < maxValue)
        slider.setSkewFactorFromMidPoint (midPoint);

    slider.setTextValueSuffix (suffix);
    slider.setNumDecimalPlacesToDisplay (decimals);
    addAndMakeVisible (slider);

    setupLabel (label, text);
    label.setJustificationType (juce::Justification::centred);

    auto dragIndex = std::make_shared<int> (-1);

    slider.onDragStart = [this, dragIndex, index]
    {
        *dragIndex = index();
        beginEdit (*dragIndex);
    };

    slider.onDragEnd = [this, dragIndex]
    {
        endEdit (*dragIndex);
        *dragIndex = -1;
    };

    juce::Slider* sliderPtr = &slider;

    slider.onValueChange = [this, sliderPtr, dragIndex, index]
    {
        if (updatingControls)
            return;

        const int idx = *dragIndex >= 0 ? *dragIndex : index();

        if (idx < 0)
            return;

        const float normalised = toNormalised (idx, static_cast<float> (sliderPtr->getValue()));

        if (*dragIndex >= 0)
            performEdit (idx, normalised);
        else
            setNormalisedWithGesture (idx, normalised);
    };

    sliderBindings.push_back ({ sliderPtr, std::move (index), dragIndex });
}

void EqtrumAudioProcessorEditor::setupToggle (juce::Button& button, std::function<int()> index)
{
    button.setClickingTogglesState (true);
    juce::Button* buttonPtr = &button;

    button.onClick = [this, buttonPtr, index]
    {
        if (updatingControls)
            return;

        const int idx = index();

        if (idx >= 0)
            setPlainWithGesture (idx, buttonPtr->getToggleState() ? 1.0f : 0.0f);
    };

    addAndMakeVisible (button);
    buttonBindings.push_back ({ buttonPtr, std::move (index) });
}

void EqtrumAudioProcessorEditor::setupChoice (juce::ComboBox& box, const juce::StringArray& items, std::function<int()> index)
{
    box.addItemList (items, 1);
    juce::ComboBox* boxPtr = &box;

    box.onChange = [this, boxPtr, index]
    {
        if (updatingControls)
            return;

        const int idx = index();
        const int selected = boxPtr->getSelectedItemIndex();

        if (idx >= 0 && selected >= 0)
            setPlainWithGesture (idx, static_cast<float> (selected));
    };

    addAndMakeVisible (box);
    comboBindings.push_back ({ boxPtr, std::move (index) });
}

void EqtrumAudioProcessorEditor::setupCallbacks()
{
    modeBox.onChange = [this]
    {
        const int newMode = modeBox.getSelectedId() == 2 ? EqtrumAudioProcessor::modeHub : EqtrumAudioProcessor::modeTrack;
        eqProcessor.setMode (newMode);
        selectedOwner = newMode == EqtrumAudioProcessor::modeTrack ? eqProcessor.getInstanceId() : 0;
        soloOwners.clear();
        lastCurveKey.clear();
    };

    preEqButton.onClick = [this] { eqProcessor.setAnalyzerPreEq (preEqButton.getToggleState()); };

    eqRangeBox.onChange = [this]
    {
        const float ranges[] = { 6.0f, 12.0f, 18.0f, 24.0f, 30.0f };
        const int i = juce::jlimit (0, 4, eqRangeBox.getSelectedItemIndex());
        spectrumView.setEqRange (ranges[i]);
    };

    specRangeBox.onChange = [this]
    {
        const float floors[] = { -60.0f, -90.0f, -120.0f };
        const int i = juce::jlimit (0, 2, specRangeBox.getSelectedItemIndex());
        spectrumView.setSpectrumFloor (floors[i]);
    };

    prevPresetButton.onClick = [this] { stepPreset (-1); };
    nextPresetButton.onClick = [this] { stepPreset (1); };
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedItemIndex();

        if (idx >= 0)
            applyPresetEntry (idx);
    };
    savePresetButton.onClick = [this] { saveCurrentAsPreset(); };
    deletePresetButton.onClick = [this] { deleteSelectedPreset(); };

    // ---- graph interaction ----
    spectrumView.onNodeSelected = [this] (int band) { selectBand (band); };

    spectrumView.onNodeDragStart = [this] (int band)
    {
        const int onIdx = fieldIndex (band, Field::on);

        if (onIdx >= 0 && getPlain (onIdx) < 0.5f)
            setPlainWithGesture (onIdx, 1.0f);   // dragging a disabled node enables it

        beginEdit (fieldIndex (band, Field::freq));
        beginEdit (fieldIndex (band, Field::gain));
    };

    spectrumView.onNodeDrag = [this] (int band, float freqHz, float gainDb)
    {
        const int freqIdx = fieldIndex (band, Field::freq);
        const int gainIdx = fieldIndex (band, Field::gain);

        if (freqIdx >= 0)
            performEdit (freqIdx, toNormalised (freqIdx, freqHz));

        if (gainIdx >= 0)
            performEdit (gainIdx, toNormalised (gainIdx, gainDb));
    };

    spectrumView.onNodeDragEnd = [this] (int band)
    {
        endEdit (fieldIndex (band, Field::freq));
        endEdit (fieldIndex (band, Field::gain));
    };

    spectrumView.onNodeDoubleClick = [this] (int band)
    {
        const int idx = fieldIndex (band, Field::on);

        if (idx >= 0)
            setPlainWithGesture (idx, getPlain (idx) > 0.5f ? 0.0f : 1.0f);
    };

    spectrumView.onNodeWheel = [this] (int band, float delta)
    {
        const int idx = fieldIndex (band, Field::q);

        if (idx < 0 || ! std::isfinite (delta))
            return;

        selectBand (band);
        const float q = getPlain (idx) * std::pow (2.0f, juce::jlimit (-1.0f, 1.0f, delta) * 2.0f);
        setPlainWithGesture (idx, juce::jlimit (0.1f, 18.0f, q));
    };

    // ---- track list ----
    trackList.onSelect = [this] (std::uint64_t owner) { selectTrack (owner); };

    trackList.onRename = [this] (std::uint64_t owner, const juce::String& name)
    {
        if (owner == eqProcessor.getInstanceId())
        {
            eqProcessor.setTrackNameManual (name);
            return;
        }

        if (! isHub())
            return;

        eqtrum::RemoteCommand command;
        command.type = eqtrum::RemoteCommand::Type::setName;
        name.copyToUTF8 (command.text.data(), command.text.size() - 1);
        pushCommand (owner, command);
    };

    trackList.onColour = [this] (std::uint64_t owner, juce::Colour colour)
    {
        if (owner == eqProcessor.getInstanceId())
        {
            eqProcessor.setTrackColourManual (colour);
            return;
        }

        if (! isHub())
            return;

        eqtrum::RemoteCommand command;
        command.type = eqtrum::RemoteCommand::Type::setColour;
        command.colour = colour.getARGB();
        pushCommand (owner, command);
    };

    trackList.onVisible = [this] (std::uint64_t owner, bool visible)
    {
        if (owner == eqProcessor.getInstanceId())
            eqProcessor.setTrackVisible (visible);

        if (auto* slot = findSlot (owner))
            slot->visible.store (visible, std::memory_order_relaxed);
    };

    trackList.onSolo = [this] (std::uint64_t owner, bool solo)
    {
        if (solo)
            soloOwners.insert (owner);
        else
            soloOwners.erase (owner);
    };
}

//==============================================================================
bool EqtrumAudioProcessorEditor::isHub() const
{
    return eqProcessor.getMode() == EqtrumAudioProcessor::modeHub;
}

eqtrum::Slot* EqtrumAudioProcessorEditor::findSlot (std::uint64_t owner) const
{
    if (owner == 0)
        return nullptr;

    auto& registry = eqtrum::InstanceRegistry::get();

    for (int i = 0; i < eqtrum::InstanceRegistry::kMaxSlots; ++i)
        if (auto* slot = registry.getSlot (i))
            if (slot->owner.load (std::memory_order_acquire) == owner)
                return slot;

    return nullptr;
}

eqtrum::Slot* EqtrumAudioProcessorEditor::getTargetSlot() const
{
    return findSlot (selectedOwner);
}

EqtrumAudioProcessorEditor::TargetKind EqtrumAudioProcessorEditor::getTargetKind() const
{
    if (! isHub())
    {
        if (selectedOwner == 0 || selectedOwner == eqProcessor.getInstanceId())
            return TargetKind::local;

        return getTargetSlot() != nullptr ? TargetKind::readOnly : TargetKind::local;
    }

    return getTargetSlot() != nullptr ? TargetKind::remote : TargetKind::none;
}

bool EqtrumAudioProcessorEditor::canEditTarget() const
{
    const auto kind = getTargetKind();
    return kind == TargetKind::local || kind == TargetKind::remote;
}

int EqtrumAudioProcessorEditor::fieldIndex (int band, Field field) const
{
    if (band < 0 || band >= eqtrum::kNumBands)
        return -1;

    const auto& b = table.bands[static_cast<size_t> (band)];

    switch (field)
    {
        case Field::on:        return b.on;
        case Field::freq:      return b.freq;
        case Field::gain:      return b.gain;
        case Field::q:         return b.q;
        case Field::slope:     return b.slope;
        case Field::dynOn:     return b.dynOn;
        case Field::threshold: return b.threshold;
        case Field::range:     return b.range;
        case Field::attack:    return b.attack;
        case Field::release:   return b.release;
        case Field::source:    return b.source;
    }

    return -1;
}

float EqtrumAudioProcessorEditor::getNormalised (int paramIndex) const
{
    const auto& params = eqProcessor.getParameterList();

    if (paramIndex < 0 || paramIndex >= static_cast<int> (params.size()))
        return 0.0f;

    const auto* p = params[static_cast<size_t> (paramIndex)];

    if (p == nullptr)
        return 0.0f;

    switch (getTargetKind())
    {
        case TargetKind::local:
            return p->getValue();

        case TargetKind::remote:
        case TargetKind::readOnly:
            if (auto* slot = getTargetSlot())
                if (paramIndex < eqtrum::kMaxParams)
                    return juce::jlimit (0.0f, 1.0f, slot->params[static_cast<size_t> (paramIndex)].load (std::memory_order_relaxed));
            return p->getDefaultValue();

        case TargetKind::none:
            break;
    }

    return p->getDefaultValue();
}

float EqtrumAudioProcessorEditor::getPlain (int paramIndex) const
{
    const auto& params = eqProcessor.getParameterList();

    if (paramIndex < 0 || paramIndex >= static_cast<int> (params.size()))
        return 0.0f;

    const auto* p = params[static_cast<size_t> (paramIndex)];
    return p != nullptr ? p->convertFrom0to1 (getNormalised (paramIndex)) : 0.0f;
}

float EqtrumAudioProcessorEditor::toNormalised (int paramIndex, float plain) const
{
    const auto& params = eqProcessor.getParameterList();

    if (paramIndex < 0 || paramIndex >= static_cast<int> (params.size()) || ! std::isfinite (plain))
        return 0.0f;

    const auto* p = params[static_cast<size_t> (paramIndex)];
    return p != nullptr ? juce::jlimit (0.0f, 1.0f, p->convertTo0to1 (plain)) : 0.0f;
}

float EqtrumAudioProcessorEditor::getTargetDynOffset (int band) const
{
    if (band < 0 || band >= eqtrum::kNumBands)
        return 0.0f;

    if (getTargetKind() == TargetKind::local)
        return eqProcessor.getDynamicOffsetDb (band);

    if (auto* slot = getTargetSlot())
        return slot->dynOffsetDb[static_cast<size_t> (band)].load (std::memory_order_relaxed);

    return 0.0f;
}

double EqtrumAudioProcessorEditor::getTargetSampleRate() const
{
    double rate = 48000.0;

    if (getTargetKind() == TargetKind::local)
        rate = eqProcessor.getEffectiveSampleRate();
    else if (auto* slot = getTargetSlot())
        rate = slot->sampleRate.load (std::memory_order_relaxed);

    return (std::isfinite (rate) && rate > 1000.0) ? rate : 48000.0;
}

juce::Colour EqtrumAudioProcessorEditor::getTargetColour() const
{
    if (getTargetKind() == TargetKind::local)
        return eqProcessor.getTrackColour();

    if (auto* slot = getTargetSlot())
        return juce::Colour (slot->colourArgb.load (std::memory_order_relaxed));

    return juce::Colours::white;
}

bool EqtrumAudioProcessorEditor::pushCommand (std::uint64_t owner, const eqtrum::RemoteCommand& command)
{
    if (auto* slot = findSlot (owner))
    {
        if (slot->commands.push (command))
            return true;

        showStatus ("Coda comandi piena: riprova");
    }

    return false;
}

void EqtrumAudioProcessorEditor::beginEdit (int paramIndex)
{
    if (getTargetKind() != TargetKind::local)
        return;

    const auto& params = eqProcessor.getParameterList();

    if (paramIndex >= 0 && paramIndex < static_cast<int> (params.size()))
        if (auto* p = params[static_cast<size_t> (paramIndex)])
            p->beginChangeGesture();
}

void EqtrumAudioProcessorEditor::performEdit (int paramIndex, float normalised)
{
    const auto kind = getTargetKind();
    const auto& params = eqProcessor.getParameterList();

    if (paramIndex < 0 || paramIndex >= static_cast<int> (params.size()) || ! std::isfinite (normalised))
        return;

    const float v = juce::jlimit (0.0f, 1.0f, normalised);

    if (kind == TargetKind::local)
    {
        if (auto* p = params[static_cast<size_t> (paramIndex)])
            p->setValueNotifyingHost (v);
    }
    else if (kind == TargetKind::remote)
    {
        eqtrum::RemoteCommand command;
        command.type = eqtrum::RemoteCommand::Type::setParam;
        command.paramIndex = paramIndex;
        command.value = v;
        pushCommand (selectedOwner, command);
    }
}

void EqtrumAudioProcessorEditor::endEdit (int paramIndex)
{
    if (getTargetKind() != TargetKind::local)
        return;

    const auto& params = eqProcessor.getParameterList();

    if (paramIndex >= 0 && paramIndex < static_cast<int> (params.size()))
        if (auto* p = params[static_cast<size_t> (paramIndex)])
            p->endChangeGesture();
}

void EqtrumAudioProcessorEditor::setNormalisedWithGesture (int paramIndex, float normalised)
{
    const auto kind = getTargetKind();

    if (kind == TargetKind::local)
        eqProcessor.applyNormalisedValue (paramIndex, normalised);
    else if (kind == TargetKind::remote)
        performEdit (paramIndex, normalised);   // the TRACK instance wraps it in a full gesture
}

void EqtrumAudioProcessorEditor::setPlainWithGesture (int paramIndex, float plain)
{
    setNormalisedWithGesture (paramIndex, toNormalised (paramIndex, plain));
}

//==============================================================================
void EqtrumAudioProcessorEditor::selectBand (int band)
{
    selectedBand = juce::jlimit (0, eqtrum::kNumBands - 1, band);

    for (int b = 0; b < eqtrum::kNumBands; ++b)
        bandButtons[static_cast<size_t> (b)].setToggleState (b == selectedBand, juce::dontSendNotification);
}

void EqtrumAudioProcessorEditor::selectTrack (std::uint64_t owner)
{
    if (owner != selectedOwner)
    {
        selectedOwner = owner;
        lastCurveKey.clear();
    }
}

void EqtrumAudioProcessorEditor::showStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusTicks = 120;   // ~4 s
}

//==============================================================================
void EqtrumAudioProcessorEditor::timerCallback()
{
    refreshTrackRows();
    refreshControls();
    refreshSpectra();
    refreshCurve();
    refreshInfo();

    if (spectrumView.consumeRepaintRequest())
        spectrumView.repaint();

    if (statusTicks > 0 && --statusTicks == 0)
        statusLabel.setText ({}, juce::dontSendNotification);

    if (deleteArmed && --deleteArmTicks <= 0)
    {
        deleteArmed = false;
        deletePresetButton.setButtonText ("Delete");
    }
}

void EqtrumAudioProcessorEditor::refreshTrackRows()
{
    auto& registry = eqtrum::InstanceRegistry::get();
    const bool hub = isHub();
    const auto self = eqProcessor.getInstanceId();

    std::vector<TrackListPanel::RowInfo> rows;
    bool selectedFound = false;

    for (int i = 0; i < eqtrum::InstanceRegistry::kMaxSlots; ++i)
    {
        auto* slot = registry.getSlot (i);

        if (slot == nullptr)
            continue;

        const auto owner = slot->owner.load (std::memory_order_acquire);

        if (owner == 0)
            continue;

        TrackListPanel::RowInfo row;
        row.owner = owner;
        row.name = slot->getName();
        row.colour = juce::Colour (slot->colourArgb.load (std::memory_order_relaxed));
        row.visible = slot->visible.load (std::memory_order_relaxed);
        row.solo = soloOwners.count (owner) > 0;
        row.isSelf = owner == self;
        row.editable = hub || row.isSelf;
        row.selected = owner == selectedOwner;
        selectedFound = selectedFound || row.selected;
        rows.push_back (row);
    }

    if (! selectedFound)
    {
        const auto fallback = hub ? (rows.empty() ? std::uint64_t (0) : rows.front().owner) : self;

        if (fallback != selectedOwner)
        {
            selectedOwner = fallback;
            lastCurveKey.clear();
        }

        for (auto& row : rows)
            row.selected = row.owner == selectedOwner;
    }

    for (auto it = soloOwners.begin(); it != soloOwners.end();)
    {
        const auto owner = *it;
        const bool exists = std::any_of (rows.begin(), rows.end(), [owner] (const TrackListPanel::RowInfo& r) { return r.owner == owner; });
        it = exists ? std::next (it) : soloOwners.erase (it);
    }

    trackList.setRows (rows);
}

void EqtrumAudioProcessorEditor::refreshControls()
{
    const auto kind = getTargetKind();
    const bool hasTarget = kind != TargetKind::none;
    const bool editable = kind == TargetKind::local || kind == TargetKind::remote;
    const bool targetLinear = hasTarget && getPlain (table.phase) > 0.5f;
    const bool hasGain = eqtrum::bandHasGain (selectedBand);
    const bool hasSlope = eqtrum::bandHasSlope (selectedBand);
    const bool hasDynamics = eqtrum::bandHasDynamics (selectedBand);

    updatingControls = true;

    for (auto& b : sliderBindings)
    {
        const int idx = b.index();
        const bool dragging = b.dragIndex != nullptr && *b.dragIndex >= 0;

        if (idx < 0 || ! hasTarget || dragging || b.slider->isMouseButtonDown())
            continue;

        const double v = getPlain (idx);

        if (std::abs (b.slider->getValue() - v) > 1.0e-4 * std::max (1.0, std::abs (v)))
            b.slider->setValue (v, juce::dontSendNotification);
    }

    for (auto& b : buttonBindings)
    {
        const int idx = b.index();

        if (idx >= 0 && hasTarget)
            b.button->setToggleState (getPlain (idx) > 0.5f, juce::dontSendNotification);
    }

    for (auto& b : comboBindings)
    {
        const int idx = b.index();

        if (idx < 0 || ! hasTarget)
            continue;

        const int selected = juce::roundToInt (getPlain (idx));

        if (b.box->getSelectedItemIndex() != selected)
            b.box->setSelectedItemIndex (selected, juce::dontSendNotification);
    }

    updatingControls = false;

    linearButton.setEnabled (editable);
    firBox.setEnabled (editable);
    outGainSlider.setEnabled (editable);
    bandOnButton.setEnabled (editable);
    freqSlider.setEnabled (editable);
    qSlider.setEnabled (editable);
    gainSlider.setEnabled (editable && hasGain);
    slopeBox.setVisible (hasSlope);
    slopeLabel.setVisible (hasSlope);
    slopeBox.setEnabled (editable);

    const bool dynamicsEditable = editable && hasDynamics && ! targetLinear;

    for (juce::Component* c : { static_cast<juce::Component*> (&dynOnButton), static_cast<juce::Component*> (&thresholdSlider),
                                static_cast<juce::Component*> (&rangeSlider), static_cast<juce::Component*> (&attackSlider),
                                static_cast<juce::Component*> (&releaseSlider), static_cast<juce::Component*> (&sourceBox),
                                static_cast<juce::Component*> (&thresholdLabel), static_cast<juce::Component*> (&rangeLabel),
                                static_cast<juce::Component*> (&attackLabel), static_cast<juce::Component*> (&releaseLabel),
                                static_cast<juce::Component*> (&sourceLabel) })
    {
        c->setVisible (hasDynamics);
        c->setEnabled (dynamicsEditable);
    }

    juce::String notice;

    if (kind == TargetKind::none)
        notice = "Nessuna traccia selezionata";
    else if (kind == TargetKind::readOnly)
        notice = "Sola lettura: modifica questa traccia dal suo plugin o da un HUB";
    else if (targetLinear && hasDynamics)
        notice = "LINEAR PHASE: bande dinamiche DISATTIVATE";
    else if (! hasDynamics)
        notice = "Dinamica disponibile solo su shelf e bell";

    if (dynNotice.getText() != notice)
        dynNotice.setText (notice, juce::dontSendNotification);

    presetBox.setEnabled (editable);
    prevPresetButton.setEnabled (editable);
    nextPresetButton.setEnabled (editable);
    savePresetButton.setEnabled (hasTarget);
    preEqButton.setEnabled (! isHub());

    const int modeId = isHub() ? 2 : 1;

    if (modeBox.getSelectedId() != modeId)
        modeBox.setSelectedId (modeId, juce::dontSendNotification);
}

void EqtrumAudioProcessorEditor::refreshSpectra()
{
    auto& registry = eqtrum::InstanceRegistry::get();
    const bool hub = isHub();
    int visibleTracks = 0;
    int totalTracks = 0;

    spectrumView.beginTraceUpdate();

    for (int i = 0; i < eqtrum::InstanceRegistry::kMaxSlots; ++i)
    {
        auto* slot = registry.getSlot (i);

        if (slot == nullptr)
            continue;

        const auto owner = slot->owner.load (std::memory_order_acquire);

        if (owner == 0)
            continue;

        ++totalTracks;

        if (! slot->visible.load (std::memory_order_relaxed))
            continue;

        if (! soloOwners.empty() && soloOwners.count (owner) == 0)
            continue;

        ++visibleTracks;
        spectrumView.updateTrace (owner, juce::Colour (slot->colourArgb.load (std::memory_order_relaxed)),
                                  owner == selectedOwner, slot->spectrum);
    }

    spectrumView.endTraceUpdate();
    spectrumView.updateSumSpectrum (hub ? &eqProcessor.getLocalSpectrum() : nullptr);

    const eqtrum::SpectrumSeqlock* sidechain = nullptr;

    if (showScButton.getToggleState())
        if (auto* slot = getTargetSlot())
            if (slot->sidechainConnected.load (std::memory_order_relaxed))
                sidechain = &slot->sidechainSpectrum;

    spectrumView.updateSidechainSpectrum (sidechain, selectedOwner);

    if (hub && totalTracks == 0)
        spectrumView.setMessage ("Nessuna istanza TRACK attiva in questo progetto");
    else if (totalTracks > 0 && visibleTracks == 0 && ! hub)
        spectrumView.setMessage ("Tutte le tracce sono nascoste");
    else
        spectrumView.setMessage ({});
}

void EqtrumAudioProcessorEditor::refreshCurve()
{
    const auto kind = getTargetKind();
    std::array<SpectrumView::Node, eqtrum::kNumBands> nodes {};

    if (kind == TargetKind::none)
    {
        spectrumView.clearEqCurve();
        spectrumView.setNodes (nodes, -1, false);
        lastCurveKey.clear();
        return;
    }

    const int numParams = table.numParams();
    const bool linear = getPlain (table.phase) > 0.5f;
    std::array<float, eqtrum::kNumBands> dyn {};

    std::vector<float> key;
    key.reserve (static_cast<size_t> (numParams + eqtrum::kNumBands + 4));

    for (int i = 0; i < numParams; ++i)
        key.push_back (getNormalised (i));

    for (int b = 0; b < eqtrum::kNumBands; ++b)
    {
        dyn[static_cast<size_t> (b)] = linear ? 0.0f : getTargetDynOffset (b);
        key.push_back (std::round (dyn[static_cast<size_t> (b)] * 10.0f));
    }

    const double rate = getTargetSampleRate();
    const auto colour = getTargetColour();
    key.push_back (static_cast<float> (rate));
    key.push_back (static_cast<float> (selectedBand));
    key.push_back (static_cast<float> (static_cast<int> (kind)));
    key.push_back (static_cast<float> ((colour.getARGB() >> 16) & 0xffffu));
    key.push_back (static_cast<float> (colour.getARGB() & 0xffffu));

    if (key == lastCurveKey && selectedOwner == lastCurveOwner)
        return;

    lastCurveKey = key;
    lastCurveOwner = selectedOwner;

    std::array<eqtrum::BandCoeffs, eqtrum::kNumBands> coeffs;

    for (int b = 0; b < eqtrum::kNumBands; ++b)
    {
        const auto bi = static_cast<size_t> (b);
        const bool hasGain = eqtrum::bandHasGain (b);
        const bool dynOn = eqtrum::bandHasDynamics (b) && getPlain (fieldIndex (b, Field::dynOn)) > 0.5f;
        const float staticGain = hasGain ? getPlain (fieldIndex (b, Field::gain)) : 0.0f;

        eqtrum::BandSpec spec;
        spec.type = eqtrum::getBandType (b);
        spec.on = getPlain (fieldIndex (b, Field::on)) > 0.5f;
        spec.freq = getPlain (fieldIndex (b, Field::freq));
        spec.gainDb = static_cast<double> (staticGain) + (dynOn ? static_cast<double> (dyn[bi]) : 0.0);
        spec.q = getPlain (fieldIndex (b, Field::q));
        spec.slopeBlend = (eqtrum::bandHasSlope (b) && getPlain (fieldIndex (b, Field::slope)) > 0.5f) ? 1.0 : 0.0;
        coeffs[bi] = eqtrum::eqmath::designBand (spec, rate);

        nodes[bi].on = spec.on;
        nodes[bi].hasGain = hasGain;
        nodes[bi].freq = static_cast<float> (spec.freq);
        nodes[bi].gainDb = staticGain;
        nodes[bi].dynOffsetDb = dynOn ? dyn[bi] : 0.0f;
    }

    std::vector<float> curve (static_cast<size_t> (SpectrumView::kCurvePoints));
    constexpr double twoPi = 6.283185307179586476925;

    for (int p = 0; p < SpectrumView::kCurvePoints; ++p)
    {
        const double f = SpectrumView::curvePointFrequency (p);
        const double omega = std::min (twoPi * f / rate, 3.13);
        double magnitude = 1.0;

        for (const auto& c : coeffs)
            magnitude *= eqtrum::eqmath::bandMagnitude (c, omega);

        curve[static_cast<size_t> (p)] = static_cast<float> (20.0 * std::log10 (std::max (magnitude, 1.0e-6)));
    }

    const bool editable = kind == TargetKind::local || kind == TargetKind::remote;
    spectrumView.setEqCurve (curve, colour.brighter (0.35f), editable);
    spectrumView.setNodes (nodes, selectedBand, true);
}

void EqtrumAudioProcessorEditor::refreshInfo()
{
    const auto kind = getTargetKind();
    int latencySamples = 0;
    bool scConnected = false;
    float scLevel = 0.0f;

    if (kind == TargetKind::local)
    {
        latencySamples = eqProcessor.getLatencySamples();
        scConnected = eqProcessor.isSidechainConnected();
        scLevel = eqProcessor.getSidechainLevel();
    }
    else if (auto* slot = getTargetSlot())
    {
        latencySamples = slot->latencySamples.load (std::memory_order_relaxed);
        scConnected = slot->sidechainConnected.load (std::memory_order_relaxed);
        scLevel = slot->sidechainLevel.load (std::memory_order_relaxed);
    }

    const double rate = getTargetSampleRate();
    const double ms = 1000.0 * static_cast<double> (latencySamples) / rate;
    const juce::String latencyText = isHub() && kind == TargetKind::none
                                         ? juce::String ("HUB: latenza 0 ms")
                                         : "Latenza: " + juce::String (ms, 1) + " ms (" + juce::String (latencySamples) + " camp.)";

    if (latencyLabel.getText() != latencyText)
        latencyLabel.setText (latencyText, juce::dontSendNotification);

    scLed.setState (scConnected, scLevel);
    const juce::String scText = ! scConnected ? "SC assente" : (scLevel > 0.001f ? "SC segnale" : "SC silenzio");

    if (scLabel.getText() != scText)
        scLabel.setText (scText, juce::dontSendNotification);
}

//==============================================================================
void EqtrumAudioProcessorEditor::refreshPresetBox()
{
    presetBox.clear (juce::dontSendNotification);
    const auto& entries = presets.getEntries();

    for (size_t i = 0; i < entries.size(); ++i)
        presetBox.addItem ((entries[i].isFactory ? "Factory: " : "") + entries[i].name, static_cast<int> (i) + 1);

    if (lastPresetIndex >= 0 && lastPresetIndex < static_cast<int> (entries.size()))
        presetBox.setSelectedItemIndex (lastPresetIndex, juce::dontSendNotification);
}

void EqtrumAudioProcessorEditor::applyPresetEntry (int entryIndex)
{
    const auto kind = getTargetKind();

    if (kind != TargetKind::local && kind != TargetKind::remote)
    {
        showStatus ("Traccia non modificabile da qui");
        return;
    }

    std::vector<float> values;

    if (! presets.getPresetValues (entryIndex, values))
    {
        showStatus ("Preset illeggibile");
        return;
    }

    if (kind == TargetKind::local)
    {
        for (size_t i = 0; i < values.size(); ++i)
            eqProcessor.applyNormalisedValue (static_cast<int> (i), values[i]);
    }
    else
    {
        auto* slot = getTargetSlot();

        if (slot == nullptr)
            return;

        if (slot->commands.getFreeSpace() < static_cast<int> (values.size()))
        {
            showStatus ("Coda comandi piena: riprova tra un attimo");
            return;
        }

        for (size_t i = 0; i < values.size(); ++i)
        {
            eqtrum::RemoteCommand command;
            command.type = eqtrum::RemoteCommand::Type::setParam;
            command.paramIndex = static_cast<std::int32_t> (i);
            command.value = values[i];
            slot->commands.push (command);
        }
    }

    lastPresetIndex = entryIndex;
    const auto& entries = presets.getEntries();

    if (entryIndex >= 0 && entryIndex < static_cast<int> (entries.size()))
        showStatus ("Preset: " + entries[static_cast<size_t> (entryIndex)].name);
}

void EqtrumAudioProcessorEditor::stepPreset (int delta)
{
    const int count = static_cast<int> (presets.getEntries().size());

    if (count <= 0)
        return;

    const int current = presetBox.getSelectedItemIndex();
    const int next = current < 0 ? 0 : ((current + delta) % count + count) % count;
    presetBox.setSelectedItemIndex (next, juce::dontSendNotification);
    applyPresetEntry (next);
}

void EqtrumAudioProcessorEditor::saveCurrentAsPreset()
{
    const auto name = presetNameEditor.getText().trim();

    if (name.isEmpty())
    {
        showStatus ("Scrivi il nome del preset nel campo accanto");
        return;
    }

    if (getTargetKind() == TargetKind::none)
        return;

    std::vector<float> values (static_cast<size_t> (table.numParams()));

    for (int i = 0; i < table.numParams(); ++i)
        values[static_cast<size_t> (i)] = getNormalised (i);

    juce::String error;

    if (! presets.saveUserPreset (name, values, error))
    {
        showStatus (error);
        return;
    }

    lastPresetIndex = presets.indexOfName (juce::File::createLegalFileName (name));
    refreshPresetBox();
    showStatus ("Salvato: " + name);
}

void EqtrumAudioProcessorEditor::deleteSelectedPreset()
{
    const int idx = presetBox.getSelectedItemIndex();
    const auto& entries = presets.getEntries();

    if (idx < 0 || idx >= static_cast<int> (entries.size()) || entries[static_cast<size_t> (idx)].isFactory)
    {
        showStatus ("Seleziona un preset utente da eliminare");
        return;
    }

    if (! deleteArmed)
    {
        deleteArmed = true;
        deleteArmTicks = 90;
        deletePresetButton.setButtonText ("Conferma?");
        return;
    }

    deleteArmed = false;
    deletePresetButton.setButtonText ("Delete");
    const auto name = entries[static_cast<size_t> (idx)].name;

    if (presets.deleteUserPreset (idx))
    {
        lastPresetIndex = -1;
        refreshPresetBox();
        showStatus ("Eliminato: " + name);
    }
    else
    {
        showStatus ("Eliminazione non riuscita");
    }
}

//==============================================================================
void EqtrumAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (windowBackground);
}

void EqtrumAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (6);

    // row 1: mode, phase, latency, output, status
    auto row1 = area.removeFromTop (28);
    modeBox.setBounds (row1.removeFromLeft (100));
    row1.removeFromLeft (6);
    linearButton.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (6);
    firBox.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (6);
    latencyLabel.setBounds (row1.removeFromLeft (200));
    row1.removeFromLeft (6);
    outGainLabel.setBounds (row1.removeFromLeft (32));
    outGainSlider.setBounds (row1.removeFromLeft (220));
    row1.removeFromLeft (6);
    statusLabel.setBounds (row1);
    area.removeFromTop (4);

    // row 2: presets, analyzer options, sidechain indicator
    auto row2 = area.removeFromTop (28);
    prevPresetButton.setBounds (row2.removeFromLeft (28));
    row2.removeFromLeft (2);
    presetBox.setBounds (row2.removeFromLeft (230));
    row2.removeFromLeft (2);
    nextPresetButton.setBounds (row2.removeFromLeft (28));
    row2.removeFromLeft (8);
    presetNameEditor.setBounds (row2.removeFromLeft (160));
    row2.removeFromLeft (4);
    savePresetButton.setBounds (row2.removeFromLeft (76));
    row2.removeFromLeft (4);
    deletePresetButton.setBounds (row2.removeFromLeft (76));
    row2.removeFromLeft (12);
    preEqButton.setBounds (row2.removeFromLeft (80));
    showScButton.setBounds (row2.removeFromLeft (106));
    scLed.setBounds (row2.removeFromLeft (20).withSizeKeepingCentre (14, 14));
    scLabel.setBounds (row2.removeFromLeft (90));
    area.removeFromTop (6);

    // bottom: band panel
    auto bottom = area.removeFromBottom (156);
    area.removeFromBottom (6);

    // side list
    auto side = area.removeFromRight (230);
    area.removeFromRight (6);
    spectrumView.setBounds (area);

    auto ranges = side.removeFromTop (26);
    eqRangeBox.setBounds (ranges.removeFromLeft (ranges.getWidth() / 2).reduced (1, 0));
    specRangeBox.setBounds (ranges.reduced (1, 0));
    side.removeFromTop (4);
    trackList.setBounds (side);

    auto bandRow = bottom.removeFromTop (26);

    for (auto& b : bandButtons)
    {
        b.setBounds (bandRow.removeFromLeft (52));
        bandRow.removeFromLeft (2);
    }

    bandRow.removeFromLeft (10);
    dynNotice.setBounds (bandRow);
    bottom.removeFromTop (6);

    auto labelRow = bottom.removeFromTop (16);
    auto knobRow = bottom;

    auto column = [&labelRow, &knobRow] (int width)
    {
        return std::make_pair (labelRow.removeFromLeft (width), knobRow.removeFromLeft (width));
    };

    constexpr int knobWidth = 78;

    {
        auto c = column (60);
        bandOnButton.setBounds (c.second.withSizeKeepingCentre (56, 24));
    }
    {
        auto c = column (knobWidth);
        freqLabel.setBounds (c.first);
        freqSlider.setBounds (c.second);
    }
    {
        auto c = column (knobWidth);
        gainLabel.setBounds (c.first);
        gainSlider.setBounds (c.second);
    }
    {
        auto c = column (knobWidth);
        qLabel.setBounds (c.first);
        qSlider.setBounds (c.second);
    }
    {
        auto c = column (110);
        slopeLabel.setBounds (c.first);
        slopeBox.setBounds (c.second.withSizeKeepingCentre (100, 24));
    }

    column (16);

    {
        auto c = column (60);
        dynOnButton.setBounds (c.second.withSizeKeepingCentre (56, 24));
    }
    {
        auto c = column (knobWidth);
        thresholdLabel.setBounds (c.first);
        thresholdSlider.setBounds (c.second);
    }
    {
        auto c = column (knobWidth);
        rangeLabel.setBounds (c.first);
        rangeSlider.setBounds (c.second);
    }
    {
        auto c = column (knobWidth);
        attackLabel.setBounds (c.first);
        attackSlider.setBounds (c.second);
    }
    {
        auto c = column (knobWidth);
        releaseLabel.setBounds (c.first);
        releaseSlider.setBounds (c.second);
    }
    {
        auto c = column (110);
        sourceLabel.setBounds (c.first);
        sourceBox.setBounds (c.second.withSizeKeepingCentre (100, 24));
    }
}
