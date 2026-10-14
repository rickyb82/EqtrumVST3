#include "LinearPhaseBuilder.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace eqtrum
{
LinearPhaseBuilder::LinearPhaseBuilder (const ParamTable& t, const std::vector<std::atomic<float>*>& rawParams)
    : juce::Thread ("Eqtrum linear phase"),
      table (t),
      raw (rawParams)
{
}

LinearPhaseBuilder::~LinearPhaseBuilder()
{
    stopBuilder();
}

void LinearPhaseBuilder::startBuilder()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);
}

void LinearPhaseBuilder::stopBuilder()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        notify();
        stopThread (3000);
    }
}

void LinearPhaseBuilder::setSampleRate (double newSampleRate) noexcept
{
    if (std::isfinite (newSampleRate) && newSampleRate > 1000.0)
        sampleRate.store (newSampleRate, std::memory_order_release);
}

void LinearPhaseBuilder::setActive (bool shouldBeActive) noexcept
{
    active.store (shouldBeActive, std::memory_order_release);
}

void LinearPhaseBuilder::invalidate() noexcept
{
    forceRebuild.store (true, std::memory_order_release);
}

bool LinearPhaseBuilder::deliverTo (juce::dsp::Convolution& convolution)
{
    const juce::SpinLock::ScopedTryLockType lock (pendingLock);

    if (! lock.isLocked() || ! hasPending)
        return false;

    // Moves the buffer (no allocation); JUCE builds the new engine on its own background thread.
    convolution.loadImpulseResponse (std::move (pendingIr), pendingSampleRate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);
    hasPending = false;
    return true;
}

void LinearPhaseBuilder::buildNow (double rate, int length, juce::AudioBuffer<float>& out) const
{
    std::vector<float> values;
    takeSnapshot (values);
    buildImpulse (values, rate, length, out);
}

float LinearPhaseBuilder::snapshotValue (const std::vector<float>& values, int index, float fallback) const noexcept
{
    if (index < 0 || index >= static_cast<int> (values.size()))
        return fallback;

    const float v = values[static_cast<size_t> (index)];
    return std::isfinite (v) ? v : fallback;
}

void LinearPhaseBuilder::takeSnapshot (std::vector<float>& values) const
{
    values.resize (raw.size());

    for (size_t i = 0; i < raw.size(); ++i)
    {
        // output gain is applied outside the convolution, it must not trigger rebuilds
        if (static_cast<int> (i) == table.outGain || raw[i] == nullptr)
            values[i] = 0.0f;
        else
            values[i] = raw[i]->load (std::memory_order_relaxed);
    }
}

BandSpec LinearPhaseBuilder::bandSpecFromSnapshot (const std::vector<float>& values, int band) const
{
    const auto& idx = table.bands[static_cast<size_t> (band)];
    BandSpec spec;
    spec.type = getBandType (band);
    spec.on = snapshotValue (values, idx.on, 0.0f) > 0.5f;
    spec.freq = juce::jlimit (20.0f, 20000.0f, snapshotValue (values, idx.freq, 1000.0f));
    spec.gainDb = bandHasGain (band) ? juce::jlimit (-18.0f, 18.0f, snapshotValue (values, idx.gain, 0.0f)) : 0.0f;
    spec.q = juce::jlimit (0.1f, 18.0f, snapshotValue (values, idx.q, 0.707f));
    spec.slopeBlend = (bandHasSlope (band) && snapshotValue (values, idx.slope, 0.0f) > 0.5f) ? 1.0 : 0.0;
    return spec;
}

void LinearPhaseBuilder::buildImpulse (const std::vector<float>& values, double rate, int length,
                                       juce::AudioBuffer<float>& out) const
{
    const int order = juce::roundToInt (std::log2 (static_cast<double> (length)));
    juce::dsp::FFT fft (order);
    const int n = fft.getSize();
    const int half = n / 2;

    out.setSize (1, n, false, true, false);

    std::array<BandCoeffs, kNumBands> coeffs;

    for (int b = 0; b < kNumBands; ++b)
        coeffs[static_cast<size_t> (b)] = eqmath::designBand (bandSpecFromSnapshot (values, b), rate);

    // 1) total magnitude response on the FFT grid (static gains only, dynamics are off in Linear)
    std::vector<std::complex<float>> spectrum (static_cast<size_t> (n));
    std::vector<std::complex<float>> timeData (static_cast<size_t> (n));
    constexpr double twoPi = 6.283185307179586476925;

    for (int k = 0; k <= half; ++k)
    {
        const double omega = twoPi * static_cast<double> (k) / static_cast<double> (n);
        double magnitude = 1.0;

        for (const auto& c : coeffs)
            magnitude *= eqmath::bandMagnitude (c, omega);

        if (! std::isfinite (magnitude))
            magnitude = 1.0;

        magnitude = juce::jlimit (0.0, 1000.0, magnitude);
        const std::complex<float> value (static_cast<float> (magnitude), 0.0f);
        spectrum[static_cast<size_t> (k)] = value;

        if (k > 0 && k < half)
            spectrum[static_cast<size_t> (n - k)] = value;   // Hermitian (real, even) -> zero phase
    }

    // 2) inverse FFT -> real, even, zero-phase impulse response
    fft.perform (spectrum.data(), timeData.data(), true);

    // Calibrate the inverse-FFT scaling with a flat spectrum (unit impulse expected).
    std::vector<std::complex<float>> flat (static_cast<size_t> (n), std::complex<float> (1.0f, 0.0f));
    std::vector<std::complex<float>> flatOut (static_cast<size_t> (n));
    fft.perform (flat.data(), flatOut.data(), true);
    const float peak = flatOut[0].real();
    const float scale = (std::isfinite (peak) && std::abs (peak) > 1.0e-12f) ? 1.0f / peak : 1.0f;

    // 3) shift by N/2 (circular) and apply a Blackman window centred on N/2
    auto* dest = out.getWritePointer (0);

    for (int i = 0; i < n; ++i)
    {
        const int src = (i + half) % n;
        const double phase = twoPi * static_cast<double> (i) / static_cast<double> (n);
        const double w = 0.42 - 0.5 * std::cos (phase) + 0.08 * std::cos (2.0 * phase);
        const float s = timeData[static_cast<size_t> (src)].real() * scale * static_cast<float> (w);
        dest[i] = std::isfinite (s) ? s : 0.0f;
    }
}

void LinearPhaseBuilder::run()
{
    std::vector<float> current;
    std::vector<float> last;
    double lastRate = 0.0;
    int lastLength = 0;
    bool haveLast = false;
    juce::uint32 lastBuildMs = 0;

    while (! threadShouldExit())
    {
        wait (25);

        if (threadShouldExit())
            break;

        if (forceRebuild.exchange (false))
            haveLast = false;

        if (! active.load (std::memory_order_acquire))
        {
            haveLast = false;
            continue;
        }

        const double rate = sampleRate.load (std::memory_order_acquire);

        if (rate <= 1000.0)
            continue;

        takeSnapshot (current);
        const int length = firLengthForChoice (juce::roundToInt (snapshotValue (current, table.firLength, 1.0f)));

        const bool changed = ! haveLast || current != last
                          || std::abs (rate - lastRate) > 0.5 || length != lastLength;

        if (! changed)
            continue;

        const auto now = juce::Time::getMillisecondCounter();

        if (haveLast && now - lastBuildMs < kMinRebuildIntervalMs)
            continue;   // rate limit: the latest values are picked up on a later pass

        juce::AudioBuffer<float> ir (1, length);
        buildImpulse (current, rate, length, ir);

        {
            const juce::SpinLock::ScopedLockType lock (pendingLock);
            pendingIr = std::move (ir);   // frees an undelivered older IR here, never on the audio thread
            pendingSampleRate = rate;
            hasPending = true;
        }

        last = current;
        lastRate = rate;
        lastLength = length;
        haveLast = true;
        lastBuildMs = now;
    }
}
} // namespace eqtrum
