#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

EqtrumAudioProcessor::EqtrumAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      table (eqtrum::getParamTable()),
      apvts (*this, nullptr, "EqtrumState", eqtrum::createParameterLayout()),
      instanceId (eqtrum::InstanceRegistry::get().createInstanceId()),
      eq (table, rawParams),
      irBuilder (table, rawParams),
      analyzer (slotIndex, instanceId)
{
    for (const auto& id : table.ids)
    {
        paramList.push_back (apvts.getParameter (id));
        auto* raw = apvts.getRawParameterValue (id);
        rawParams.push_back (raw != nullptr ? raw : &dummyParam);
    }

    trackColour = eqtrum::getPaletteColour (0);

    for (auto& line : bypassDelay)
        line.assign (static_cast<size_t> (kBypassDelaySize), 0.0f);

    analyzer.startAnalysis();
    irBuilder.startBuilder();
    startTimerHz (30);
}

EqtrumAudioProcessor::~EqtrumAudioProcessor()
{
    stopTimer();
    analyzer.stopAnalysis();
    irBuilder.stopBuilder();

    const int index = slotIndex.exchange (-1);
    eqtrum::InstanceRegistry::get().release (index, instanceId);
}

//==============================================================================
const juce::String EqtrumAudioProcessor::getName() const   { return JucePlugin_Name; }
bool EqtrumAudioProcessor::acceptsMidi() const              { return false; }
bool EqtrumAudioProcessor::producesMidi() const             { return false; }
bool EqtrumAudioProcessor::isMidiEffect() const             { return false; }
double EqtrumAudioProcessor::getTailLengthSeconds() const   { return 0.4; }   // longest linear FIR at 44.1 kHz
int EqtrumAudioProcessor::getNumPrograms()                  { return 1; }
int EqtrumAudioProcessor::getCurrentProgram()               { return 0; }
void EqtrumAudioProcessor::setCurrentProgram (int index)    { juce::ignoreUnused (index); }
const juce::String EqtrumAudioProcessor::getProgramName (int index) { juce::ignoreUnused (index); return {}; }
void EqtrumAudioProcessor::changeProgramName (int index, const juce::String& newName) { juce::ignoreUnused (index, newName); }
bool EqtrumAudioProcessor::hasEditor() const                { return true; }

juce::AudioProcessorEditor* EqtrumAudioProcessor::createEditor()
{
    return new EqtrumAudioProcessorEditor (*this);
}

//==============================================================================
float EqtrumAudioProcessor::readRaw (int index, float fallback) const noexcept
{
    if (index < 0 || index >= static_cast<int> (rawParams.size()))
        return fallback;

    const float v = rawParams[static_cast<size_t> (index)]->load (std::memory_order_relaxed);
    return std::isfinite (v) ? v : fallback;
}

int EqtrumAudioProcessor::computeDesiredPath() const noexcept
{
    if (mode.load (std::memory_order_acquire) == modeHub)
        return pathPassThrough;

    return readRaw (table.phase, 0.0f) > 0.5f ? pathLinear : pathMinimum;
}

int EqtrumAudioProcessor::computeDesiredFirLength() const noexcept
{
    return eqtrum::firLengthForChoice (juce::roundToInt (readRaw (table.firLength, 1.0f)));
}

int EqtrumAudioProcessor::computeDesiredLatency() const noexcept
{
    return computeDesiredPath() == pathLinear ? computeDesiredFirLength() / 2 : 0;
}

void EqtrumAudioProcessor::applyLatency (int samples)
{
    reportedLatency.store (samples, std::memory_order_release);
    setLatencySamples (samples);
}

void EqtrumAudioProcessor::updateLatency()
{
    const int desired = computeDesiredLatency();

    if (desired != getLatencySamples())
        applyLatency (desired);
}

//==============================================================================
bool EqtrumAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto sidechain = layouts.getChannelSet (true, 1);

        if (! sidechain.isDisabled()
            && sidechain != juce::AudioChannelSet::stereo()
            && sidechain != juce::AudioChannelSet::mono())
            return false;
    }

    return true;
}

void EqtrumAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate = (std::isfinite (sampleRate) && sampleRate > 1000.0) ? sampleRate : 44100.0;
    sampleRateForUi.store (currentRate, std::memory_order_relaxed);

    const int maxBlock = std::max (1, samplesPerBlock);
    convBlockSize = juce::jlimit (64, 2048, juce::nextPowerOfTwo (maxBlock));

    eq.prepare (currentRate);

    activePath = pendingPath = computeDesiredPath();
    activeFirLength = pendingFirLength = computeDesiredFirLength();

    // Linear phase: build the IR synchronously and load it *before* prepare(), so that it is
    // active from the very first process() call (JUCE's documented pattern). This also makes
    // offline renders (export/freeze, faster than real time) correct from sample 0.
    if (activePath == pathLinear)
    {
        juce::AudioBuffer<float> ir;
        irBuilder.buildNow (currentRate, activeFirLength, ir);
        convolution.loadImpulseResponse (std::move (ir), currentRate,
                                         juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no,
                                         juce::dsp::Convolution::Normalise::no);
    }

    const juce::dsp::ProcessSpec spec { currentRate, static_cast<juce::uint32> (convBlockSize), 2 };
    convolution.prepare (spec);
    convolution.reset();

    outputGain.reset (currentRate, 0.03);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, readRaw (table.outGain, 0.0f))));
    fadeStep = static_cast<float> (1.0 / (0.01 * currentRate));   // 10 ms fades

    analyzer.setSampleRate (currentRate);
    irBuilder.setSampleRate (currentRate);
    irBuilder.setActive (activePath == pathLinear);

    // Filters and convolution start from a clean state with the right IR: no fade needed.
    fadeGain = 1.0f;
    fade = Fade::running;
    irWaitSamples = 0;
    irHoldSamples = -1;
    bypassedLastBlock = false;

    applyLatency (computeDesiredLatency());
}

void EqtrumAudioProcessor::releaseResources()
{
    convolution.reset();
    eq.reset();
}

//==============================================================================
void EqtrumAudioProcessor::sanitise (float* data, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        if (! std::isfinite (data[i]))
            data[i] = 0.0f;
}

bool EqtrumAudioProcessor::sanitiseAndCheck (float* data, int numSamples) noexcept
{
    bool foundBad = false;

    for (int i = 0; i < numSamples; ++i)
    {
        if (! std::isfinite (data[i]))
        {
            data[i] = 0.0f;
            foundBad = true;
        }
        else if (std::abs (data[i]) > 64.0f)   // +36 dBFS: something blew up, clamp hard
        {
            data[i] = data[i] > 0.0f ? 64.0f : -64.0f;
        }
    }

    return foundBad;
}

void EqtrumAudioProcessor::processLinear (juce::AudioBuffer<float>& mainBuffer, int numSamples) noexcept
{
    juce::dsp::AudioBlock<float> block (mainBuffer);

    // The convolution was prepared for at most convBlockSize samples per call.
    for (int start = 0; start < numSamples; start += convBlockSize)
    {
        const int length = std::min (convBlockSize, numSamples - start);
        auto sub = block.getSubBlock (static_cast<size_t> (start), static_cast<size_t> (length));
        juce::dsp::ProcessContextReplacing<float> context (sub);
        convolution.process (context);
    }
}

void EqtrumAudioProcessor::applyOutputGain (float* left, float* right, int numSamples) noexcept
{
    if (! outputGain.isSmoothing())
    {
        const float g = outputGain.getTargetValue();

        if (std::abs (g - 1.0f) < 1.0e-6f)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            left[i] *= g;
            right[i] *= g;
        }

        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = outputGain.getNextValue();
        left[i] *= g;
        right[i] *= g;
    }
}

void EqtrumAudioProcessor::switchToPendingPath() noexcept
{
    activePath = pendingPath;
    activeFirLength = pendingFirLength;
    eq.reset();
    outputGain.setCurrentAndTargetValue (outputGain.getTargetValue());

    if (activePath == pathLinear)
    {
        convolution.reset();
        fade = Fade::waitingForIr;
        irWaitSamples = 0;
        irHoldSamples = -1;
        irWaitStartMs = juce::Time::getMillisecondCounter();
    }
    else
    {
        fade = Fade::fadingIn;
    }
}

void EqtrumAudioProcessor::applyFadeAndTransitions (float* left, float* right, int numSamples) noexcept
{
    if (fade == Fade::running)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        if (fade == Fade::fadingOut)
            fadeGain = std::max (0.0f, fadeGain - fadeStep);
        else if (fade == Fade::fadingIn)
            fadeGain = std::min (1.0f, fadeGain + fadeStep);
        else
            fadeGain = 0.0f;   // waiting for the linear-phase IR: muted

        left[i] *= fadeGain;
        right[i] *= fadeGain;
    }

    switch (fade)
    {
        case Fade::fadingOut:
            if (fadeGain <= 0.0f)
                switchToPendingPath();
            break;

        case Fade::fadingIn:
            if (fadeGain >= 1.0f)
                fade = Fade::running;
            break;

        case Fade::waitingForIr:
        {
            irWaitSamples += numSamples;

            // The new IR is active once the convolution reports its length; then wait for N/2
            // samples of history plus JUCE's internal 50 ms engine crossfade before fading in.
            if (irHoldSamples < 0 && convolution.getCurrentIRSize() == activeFirLength)
                irHoldSamples = activeFirLength / 2 + juce::roundToInt (0.06 * currentRate);

            if (irHoldSamples >= 0)
            {
                irHoldSamples -= numSamples;

                if (irHoldSamples <= 0)
                    fade = Fade::fadingIn;
            }

            // Safety net, never stay muted: at least 3 s of audio AND 1.5 s of wall-clock time
            // (offline renders run faster than real time while the IR is built asynchronously).
            if (fade == Fade::waitingForIr && irWaitSamples > juce::roundToInt (3.0 * currentRate)
                && juce::Time::getMillisecondCounter() - irWaitStartMs > 1500u)
                fade = Fade::fadingIn;

            break;
        }

        case Fade::running:
            break;
    }
}

void EqtrumAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0)
        return;

    auto mainBuffer = getBusBuffer (buffer, false, 0);

    if (mainBuffer.getNumChannels() < 2)
    {
        for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
            sanitise (mainBuffer.getWritePointer (ch), numSamples);

        return;
    }

    if (bypassedLastBlock)
    {
        // coming back from host bypass: stay muted for this block, then restart the
        // active path cleanly (states reset, linear phase waits for a full history)
        bypassedLastBlock = false;
        fadeGain = 0.0f;
        fade = Fade::fadingOut;
    }

    float* left = mainBuffer.getWritePointer (0);
    float* right = mainBuffer.getWritePointer (1);
    sanitise (left, numSamples);
    sanitise (right, numSamples);

    // ---- optional sidechain bus ----
    const float* scLeft = nullptr;
    const float* scRight = nullptr;

    if (getBusCount (true) > 1)
    {
        auto* scBus = getBus (true, 1);

        if (scBus != nullptr && scBus->isEnabled())
        {
            auto scBuffer = getBusBuffer (buffer, true, 1);

            if (scBuffer.getNumChannels() > 0)
            {
                scLeft = scBuffer.getReadPointer (0);
                scRight = scBuffer.getNumChannels() > 1 ? scBuffer.getReadPointer (1) : scLeft;
            }
        }
    }

    float scPeak = 0.0f;

    if (scLeft != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::max (std::abs (scLeft[i]), std::abs (scRight[i]));

            if (std::isfinite (a))
                scPeak = std::max (scPeak, a);
        }
    }

    sidechainLevel.store (scPeak, std::memory_order_relaxed);
    sidechainConnected.store (scLeft != nullptr, std::memory_order_relaxed);

    // ---- path selection (TRACK minimum / TRACK linear / HUB pass-through) ----
    pendingPath = computeDesiredPath();
    pendingFirLength = computeDesiredFirLength();

    if (pendingPath == pathLinear || activePath == pathLinear)
        irBuilder.deliverTo (convolution);

    const bool tapPreEq = analyzerPreEq.load (std::memory_order_relaxed) || activePath == pathPassThrough;

    if (tapPreEq)
        analyzer.pushMain (left, right, numSamples);

    if (scLeft != nullptr)
        analyzer.pushSidechain (scLeft, scRight, numSamples);

    const bool mismatch = pendingPath != activePath
                       || (activePath == pathLinear && pendingFirLength != activeFirLength);

    if (mismatch && fade != Fade::fadingOut)
        fade = Fade::fadingOut;

    eq.updateTargets();
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 24.0f, readRaw (table.outGain, 0.0f))));

    switch (activePath)
    {
        case pathMinimum:
            eq.process (left, right, scLeft, scRight, numSamples, true);
            applyOutputGain (left, right, numSamples);
            break;

        case pathLinear:
            processLinear (mainBuffer, numSamples);   // dynamics are disabled in Linear mode
            applyOutputGain (left, right, numSamples);
            break;

        default:
            outputGain.setCurrentAndTargetValue (outputGain.getTargetValue());   // HUB: audio untouched
            break;
    }

    applyFadeAndTransitions (left, right, numSamples);

    const bool badLeft = sanitiseAndCheck (left, numSamples);
    const bool badRight = sanitiseAndCheck (right, numSamples);

    if (badLeft || badRight)
    {
        eq.reset();
        convolution.reset();
    }

    if (! tapPreEq)
        analyzer.pushMain (left, right, numSamples);
}

void EqtrumAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0)
        return;

    auto mainBuffer = getBusBuffer (buffer, false, 0);
    const int numChannels = std::min (2, mainBuffer.getNumChannels());

    for (int ch = 0; ch < numChannels; ++ch)
        sanitise (mainBuffer.getWritePointer (ch), numSamples);

    if (numChannels == 2)
        analyzer.pushMain (mainBuffer.getReadPointer (0), mainBuffer.getReadPointer (1), numSamples);

    if (! bypassedLastBlock)
        for (auto& line : bypassDelay)
            std::fill (line.begin(), line.end(), 0.0f);   // no stale audio from an earlier bypass

    // Dry signal delayed by the reported latency, so the host's delay compensation stays valid.
    constexpr int mask = kBypassDelaySize - 1;
    const int delay = juce::jlimit (0, kBypassDelaySize - 1, reportedLatency.load (std::memory_order_acquire));
    int endPos = bypassWritePos;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& line = bypassDelay[static_cast<size_t> (ch)];
        float* data = mainBuffer.getWritePointer (ch);
        int w = bypassWritePos;

        for (int i = 0; i < numSamples; ++i)
        {
            line[static_cast<size_t> (w)] = data[i];
            data[i] = line[static_cast<size_t> ((w - delay + kBypassDelaySize) & mask)];
            w = (w + 1) & mask;
        }

        endPos = w;
    }

    bypassWritePos = endPos;
    bypassedLastBlock = true;
}

//==============================================================================
void EqtrumAudioProcessor::timerCallback()
{
    auto& registry = eqtrum::InstanceRegistry::get();
    registry.reapStale();
    ensureSlot();

    const int index = slotIndex.load (std::memory_order_acquire);
    auto* slot = registry.getSlot (index);

    if (slot != nullptr && registry.isOwnedBy (index, instanceId))
    {
        drainCommands (*slot);
        publishToSlot (*slot);
    }

    irBuilder.setActive (computeDesiredPath() == pathLinear);
    updateLatency();
}

void EqtrumAudioProcessor::ensureSlot()
{
    auto& registry = eqtrum::InstanceRegistry::get();
    int index = slotIndex.load (std::memory_order_acquire);

    if (mode.load (std::memory_order_acquire) == modeTrack)
    {
        if (index < 0 || ! registry.isOwnedBy (index, instanceId))
        {
            index = registry.acquire (instanceId);
            slotIndex.store (index, std::memory_order_release);

            if (index >= 0)
            {
                const juce::ScopedLock lock (metaLock);
                visibilityPending = true;
            }
        }
    }
    else if (index >= 0)
    {
        slotIndex.store (-1, std::memory_order_release);
        registry.release (index, instanceId);
    }
}

juce::String EqtrumAudioProcessor::getEffectiveName (int index) const
{
    const juce::ScopedLock lock (metaLock);

    if (nameCustom && trackName.isNotEmpty())
        return trackName;

    return index >= 0 ? "Track " + juce::String (index + 1) : juce::String ("Track");
}

juce::Colour EqtrumAudioProcessor::getEffectiveColour (int index) const
{
    const juce::ScopedLock lock (metaLock);

    if (colourCustom)
        return trackColour;

    return eqtrum::getPaletteColour (std::max (0, index));
}

void EqtrumAudioProcessor::publishToSlot (eqtrum::Slot& slot)
{
    const int count = std::min (static_cast<int> (paramList.size()), eqtrum::kMaxParams);
    bool changed = false;

    for (int i = 0; i < count; ++i)
    {
        const auto* p = paramList[static_cast<size_t> (i)];
        const float v = p != nullptr ? p->getValue() : 0.0f;
        auto& mirror = slot.params[static_cast<size_t> (i)];

        if (mirror.load (std::memory_order_relaxed) != v)
        {
            mirror.store (v, std::memory_order_relaxed);
            changed = true;
        }
    }

    if (changed)
        slot.paramVersion.fetch_add (1, std::memory_order_release);

    slot.sampleRate.store (sampleRateForUi.load (std::memory_order_relaxed), std::memory_order_relaxed);
    slot.latencySamples.store (getLatencySamples(), std::memory_order_relaxed);
    slot.sidechainLevel.store (sidechainLevel.load (std::memory_order_relaxed), std::memory_order_relaxed);
    slot.sidechainConnected.store (sidechainConnected.load (std::memory_order_relaxed), std::memory_order_relaxed);

    for (int b = 0; b < eqtrum::kNumBands; ++b)
        slot.dynOffsetDb[static_cast<size_t> (b)].store (eq.getDynamicOffsetDb (b), std::memory_order_relaxed);

    const int index = slotIndex.load (std::memory_order_acquire);
    slot.setName (getEffectiveName (index));
    slot.colourArgb.store (getEffectiveColour (index).getARGB(), std::memory_order_relaxed);

    const juce::ScopedLock lock (metaLock);

    if (visibilityPending)
    {
        slot.visible.store (trackVisible, std::memory_order_relaxed);
        visibilityPending = false;
    }
    else
    {
        trackVisible = slot.visible.load (std::memory_order_relaxed);   // may be toggled by any viewer
    }
}

void EqtrumAudioProcessor::drainCommands (eqtrum::Slot& slot)
{
    eqtrum::RemoteCommand command;
    int processed = 0;

    while (processed < static_cast<int> (eqtrum::CommandQueue::capacity) && slot.commands.pop (command))
    {
        ++processed;

        switch (command.type)
        {
            case eqtrum::RemoteCommand::Type::setParam:
                applyNormalisedValue (command.paramIndex, command.value);
                break;

            case eqtrum::RemoteCommand::Type::setName:
                command.text.back() = 0;
                setTrackNameManual (juce::String::fromUTF8 (command.text.data()));
                break;

            case eqtrum::RemoteCommand::Type::setColour:
                setTrackColourManual (juce::Colour (command.colour));
                break;
        }
    }
}

void EqtrumAudioProcessor::applyNormalisedValue (int paramIndex, float normalised)
{
    if (paramIndex < 0 || paramIndex >= static_cast<int> (paramList.size()) || ! std::isfinite (normalised))
        return;

    if (auto* p = paramList[static_cast<size_t> (paramIndex)])
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalised));
        p->endChangeGesture();
    }
}

//==============================================================================
void EqtrumAudioProcessor::setMode (int newMode)
{
    mode.store (juce::jlimit (0, 1, newMode), std::memory_order_release);
    ensureSlot();
    irBuilder.setActive (computeDesiredPath() == pathLinear);
    updateLatency();
}

void EqtrumAudioProcessor::setAnalyzerPreEq (bool shouldTapPreEq)
{
    analyzerPreEq.store (shouldTapPreEq, std::memory_order_release);
}

juce::String EqtrumAudioProcessor::getTrackName() const
{
    return getEffectiveName (slotIndex.load (std::memory_order_acquire));
}

void EqtrumAudioProcessor::setTrackNameManual (const juce::String& newName)
{
    const juce::ScopedLock lock (metaLock);
    trackName = newName.trim().substring (0, 40);
    nameCustom = trackName.isNotEmpty();
}

juce::Colour EqtrumAudioProcessor::getTrackColour() const
{
    return getEffectiveColour (slotIndex.load (std::memory_order_acquire));
}

void EqtrumAudioProcessor::setTrackColourManual (juce::Colour newColour)
{
    const juce::ScopedLock lock (metaLock);
    trackColour = newColour.withAlpha (1.0f);
    colourCustom = true;
}

void EqtrumAudioProcessor::setTrackVisible (bool shouldBeVisible)
{
    const juce::ScopedLock lock (metaLock);
    trackVisible = shouldBeVisible;
    visibilityPending = true;
}

void EqtrumAudioProcessor::updateTrackProperties (const TrackProperties& properties)
{
    // Ableton may or may not provide these: both are optional and simply override the defaults.
    const juce::ScopedLock lock (metaLock);

    if (properties.name.has_value() && properties.name->trim().isNotEmpty())
    {
        trackName = properties.name->trim().substring (0, 40);
        nameCustom = true;
    }

    if (properties.colour.has_value() && properties.colour->getARGB() != 0)
    {
        trackColour = properties.colour->withAlpha (1.0f);
        colourCustom = true;
    }
}

//==============================================================================
void EqtrumAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    if (xml == nullptr)
        return;

    xml->setAttribute ("eqtrumVersion", eqtrum::kStateVersion);
    xml->setAttribute ("mode", mode.load (std::memory_order_acquire));
    xml->setAttribute ("analyzerPre", analyzerPreEq.load (std::memory_order_acquire) ? 1 : 0);

    {
        const juce::ScopedLock lock (metaLock);
        xml->setAttribute ("nameCustom", nameCustom ? 1 : 0);
        xml->setAttribute ("trackName", nameCustom ? trackName : juce::String());
        xml->setAttribute ("colour", colourCustom ? trackColour.toString() : juce::String());
        xml->setAttribute ("visible", trackVisible ? 1 : 0);
    }

    copyXmlToBinary (*xml, destData);
}

void EqtrumAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;   // empty state: keep defaults

    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;   // corrupt or foreign state: keep current values

    // A newer version may contain more parameters: unknown IDs are dropped, known ones are loaded.
    auto tree = juce::ValueTree::fromXml (*xml);

    if (! tree.isValid())
        return;

    for (int i = tree.getNumChildren(); --i >= 0;)
    {
        auto child = tree.getChild (i);

        if (! child.hasType ("PARAM"))
        {
            tree.removeChild (i, nullptr);
            continue;
        }

        const auto id = child.getProperty ("id").toString();
        const auto value = static_cast<double> (child.getProperty ("value", 0.0));

        if (table.indexOf (id) < 0 || ! std::isfinite (value))
            tree.removeChild (i, nullptr);   // missing parameters fall back to their defaults
    }

    apvts.replaceState (tree);

    mode.store (juce::jlimit (0, 1, xml->getIntAttribute ("mode", modeTrack)), std::memory_order_release);
    analyzerPreEq.store (xml->getIntAttribute ("analyzerPre", 0) != 0, std::memory_order_release);

    const juce::ScopedLock lock (metaLock);
    const auto storedName = xml->getStringAttribute ("trackName").trim().substring (0, 40);
    nameCustom = xml->getIntAttribute ("nameCustom", 0) != 0 && storedName.isNotEmpty();
    trackName = nameCustom ? storedName : juce::String();

    const auto colourText = xml->getStringAttribute ("colour").trim();
    colourCustom = colourText.isNotEmpty();

    if (colourCustom)
        trackColour = juce::Colour::fromString (colourText).withAlpha (1.0f);

    trackVisible = xml->getIntAttribute ("visible", 1) != 0;
    visibilityPending = true;

    irBuilder.invalidate();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EqtrumAudioProcessor();
}
