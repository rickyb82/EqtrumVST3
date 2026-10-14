#include "EqEngine.h"

#include <algorithm>
#include <cmath>

namespace eqtrum
{
EqEngine::EqEngine (const ParamTable& t, const std::vector<std::atomic<float>*>& rawParams)
    : table (t), raw (rawParams)
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& band = bands[static_cast<size_t> (b)];
        band.type = getBandType (b);
        band.hasGain = bandHasGain (b);
        band.hasSlope = bandHasSlope (b);
        band.hasDynamics = bandHasDynamics (b);
    }

    for (auto& v : dynOffsetForUi)
        v.store (0.0f, std::memory_order_relaxed);
}

float EqEngine::readParam (int index, float fallback) const noexcept
{
    if (index < 0 || index >= static_cast<int> (raw.size()))
        return fallback;

    const auto* p = raw[static_cast<size_t> (index)];

    if (p == nullptr)
        return fallback;

    const float v = p->load (std::memory_order_relaxed);
    return std::isfinite (v) ? v : fallback;
}

void EqEngine::prepare (double newSampleRate)
{
    sampleRate = (std::isfinite (newSampleRate) && newSampleRate > 1000.0) ? newSampleRate : 44100.0;
    updateTargets();

    for (auto& band : bands)
    {
        band.freqSmoothed.reset (sampleRate, 0.03);
        band.qSmoothed.reset (sampleRate, 0.03);
        band.gainSmoothed.reset (sampleRate, 0.03);
        band.mixSmoothed.reset (sampleRate, 0.02);
        band.slopeSmoothed.reset (sampleRate, 0.03);
        band.dynSmoothed.reset (sampleRate, 0.01);
    }

    reset();
}

void EqEngine::resetBandStates (Band& band) noexcept
{
    for (auto& f : band.stage1)
        f.reset();

    for (auto& f : band.stage2)
        f.reset();

    band.detector.reset();
    band.envelope = 0.0f;
}

void EqEngine::reset() noexcept
{
    for (auto& band : bands)
    {
        band.freqSmoothed.setCurrentAndTargetValue (band.freq);
        band.qSmoothed.setCurrentAndTargetValue (band.q);
        band.gainSmoothed.setCurrentAndTargetValue (band.gain);
        band.mixSmoothed.setCurrentAndTargetValue (band.on ? 1.0f : 0.0f);
        band.slopeSmoothed.setCurrentAndTargetValue (band.slope24 ? 1.0f : 0.0f);
        band.dynSmoothed.setCurrentAndTargetValue (0.0f);
        resetBandStates (band);
        band.statesInUse = false;
    }

    for (auto& v : dynOffsetForUi)
        v.store (0.0f, std::memory_order_relaxed);
}

void EqEngine::updateTargets() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& band = bands[static_cast<size_t> (b)];
        const auto& idx = table.bands[static_cast<size_t> (b)];

        band.on = readParam (idx.on, 0.0f) > 0.5f;
        band.freq = juce::jlimit (20.0f, 20000.0f, readParam (idx.freq, 1000.0f));
        band.gain = band.hasGain ? juce::jlimit (-18.0f, 18.0f, readParam (idx.gain, 0.0f)) : 0.0f;
        band.q = juce::jlimit (0.1f, 18.0f, readParam (idx.q, 0.707f));
        band.slope24 = band.hasSlope && readParam (idx.slope, 0.0f) > 0.5f;

        if (band.hasDynamics)
        {
            band.dynOn = readParam (idx.dynOn, 0.0f) > 0.5f;
            band.threshold = juce::jlimit (-60.0f, 0.0f, readParam (idx.threshold, -24.0f));
            band.range = juce::jlimit (-12.0f, 12.0f, readParam (idx.range, -6.0f));
            band.attackMs = juce::jlimit (0.1f, 200.0f, readParam (idx.attack, 10.0f));
            band.releaseMs = juce::jlimit (5.0f, 2000.0f, readParam (idx.release, 150.0f));
            band.useSidechain = readParam (idx.source, 0.0f) > 0.5f;
        }

        band.freqSmoothed.setTargetValue (band.freq);
        band.qSmoothed.setTargetValue (band.q);
        band.gainSmoothed.setTargetValue (band.gain);
        band.mixSmoothed.setTargetValue (band.on ? 1.0f : 0.0f);
        band.slopeSmoothed.setTargetValue (band.slope24 ? 1.0f : 0.0f);
    }
}

void EqEngine::process (float* left, float* right, const float* scLeft, const float* scRight,
                        int numSamples, bool dynamicsEnabled) noexcept
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    for (int start = 0; start < numSamples; start += kSubBlock)
    {
        const int n = std::min (kSubBlock, numSamples - start);
        processSubBlock (left + start, right + start,
                         scLeft != nullptr ? scLeft + start : nullptr,
                         scRight != nullptr ? scRight + start : nullptr,
                         n, dynamicsEnabled);
    }

    for (size_t b = 0; b < bands.size(); ++b)
        dynOffsetForUi[b].store (bands[b].dynSmoothed.getCurrentValue(), std::memory_order_relaxed);
}

void EqEngine::processSubBlock (float* left, float* right, const float* scLeft, const float* scRight,
                                int n, bool dynamicsEnabled) noexcept
{
    float internalSource[kSubBlock];
    float sidechainSource[kSubBlock];
    float mixRamp[kSubBlock];
    float slopeRamp[kSubBlock];

    // Detector sources are taken before this sub-block is equalised (pre-EQ signal).
    for (int i = 0; i < n; ++i)
    {
        const float s = 0.5f * (left[i] + right[i]);
        internalSource[i] = std::isfinite (s) ? s : 0.0f;
    }

    const bool haveSidechain = scLeft != nullptr;

    if (haveSidechain)
    {
        const float* r = scRight != nullptr ? scRight : scLeft;

        for (int i = 0; i < n; ++i)
        {
            const float s = 0.5f * (scLeft[i] + r[i]);
            sidechainSource[i] = std::isfinite (s) ? s : 0.0f;
        }
    }

    const auto fs = static_cast<float> (sampleRate);

    for (auto& band : bands)
    {
        const float freq = band.freqSmoothed.skip (n);
        const float q = band.qSmoothed.skip (n);
        const float gain = band.gainSmoothed.skip (n);

        // Fully disabled band: skip the filters and keep their state cleared,
        // the mix ramp hides the restart when the band is enabled again.
        if (! band.mixSmoothed.isSmoothing() && band.mixSmoothed.getCurrentValue() <= 0.0f)
        {
            if (band.statesInUse)
            {
                resetBandStates (band);
                band.statesInUse = false;
            }

            band.dynSmoothed.setCurrentAndTargetValue (0.0f);
            band.slopeSmoothed.skip (n);
            continue;
        }

        band.statesInUse = true;

        // ---- dynamics: band-pass detector, peak envelope, smoothstep gain offset ----
        float offsetTarget = 0.0f;

        if (dynamicsEnabled && band.hasDynamics && band.dynOn && band.on)
        {
            const float* src = (band.useSidechain && haveSidechain) ? sidechainSource : internalSource;
            band.detector.c = eqmath::bandPass (sampleRate, freq, q);

            const float attackCoef = std::exp (-1.0f / (std::max (0.05f, band.attackMs) * 0.001f * fs));
            const float releaseCoef = std::exp (-1.0f / (std::max (1.0f, band.releaseMs) * 0.001f * fs));
            float env = band.envelope;

            for (int i = 0; i < n; ++i)
            {
                const float x = std::abs (band.detector.process (src[i]));
                const float coef = x > env ? attackCoef : releaseCoef;
                env = coef * env + (1.0f - coef) * x;
            }

            if (! std::isfinite (env) || ! band.detector.isFinite())
            {
                env = 0.0f;
                band.detector.reset();
            }

            band.envelope = env;

            const float levelDb = 20.0f * std::log10 (std::max (env, 1.0e-6f));
            const float t = juce::jlimit (0.0f, 1.0f, (levelDb - band.threshold) / 6.0f);
            offsetTarget = band.range * t * t * (3.0f - 2.0f * t);
        }
        else
        {
            band.envelope = 0.0f;
        }

        band.dynSmoothed.setTargetValue (offsetTarget);
        const float dynOffset = band.dynSmoothed.skip (n);

        // ---- per-sample ramps (band on/off and HP/LP slope crossfade) ----
        for (int i = 0; i < n; ++i)
            mixRamp[i] = band.mixSmoothed.getNextValue();

        const float slopeAtStart = band.slopeSmoothed.getCurrentValue();
        const bool useStage2 = band.hasSlope && (band.slopeSmoothed.isSmoothing() || slopeAtStart > 0.0f);

        for (int i = 0; i < n; ++i)
            slopeRamp[i] = band.slopeSmoothed.getNextValue();

        // ---- coefficients for this 32-sample sub-block ----
        BandSpec spec;
        spec.type = band.type;
        spec.on = true;
        spec.freq = freq;
        spec.gainDb = static_cast<double> (gain) + static_cast<double> (dynOffset);
        spec.q = q;
        spec.slopeBlend = band.hasSlope ? slopeAtStart : 0.0f;
        const auto coeffs = eqmath::designBand (spec, sampleRate);

        for (size_t ch = 0; ch < 2; ++ch)
        {
            float* x = ch == 0 ? left : right;
            auto& s1 = band.stage1[ch];
            auto& s2 = band.stage2[ch];
            s1.c = coeffs.stage1;
            s2.c = coeffs.stage2;

            if (useStage2)
            {
                for (int i = 0; i < n; ++i)
                {
                    float y = s1.process (x[i]);
                    const float y2 = s2.process (y);
                    y += slopeRamp[i] * (y2 - y);
                    x[i] += mixRamp[i] * (y - x[i]);
                }
            }
            else
            {
                s2.reset();

                for (int i = 0; i < n; ++i)
                {
                    const float y = s1.process (x[i]);
                    x[i] += mixRamp[i] * (y - x[i]);
                }
            }

            if (! s1.isFinite() || ! s2.isFinite())
            {
                s1.reset();
                s2.reset();
            }
        }
    }
}

float EqEngine::getDynamicOffsetDb (int band) const noexcept
{
    if (band < 0 || band >= kNumBands)
        return 0.0f;

    return dynOffsetForUi[static_cast<size_t> (band)].load (std::memory_order_relaxed);
}
} // namespace eqtrum
