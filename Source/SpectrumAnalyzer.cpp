#include "SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace eqtrum
{
SpectrumAnalyzer::Stream::Stream()
    : fifoData (static_cast<size_t> (kFifoSize), 0.0f),
      frame (static_cast<size_t> (kFftSize), 0.0f)
{
    smoothed.fill (kFloorDb);
}

SpectrumAnalyzer::SpectrumAnalyzer (std::atomic<int>& slotIndexToPublish, std::uint64_t ownerId)
    : juce::Thread ("Eqtrum analysis"),
      slotIndex (slotIndexToPublish),
      instanceId (ownerId),
      fftBuffer (static_cast<size_t> (kFftSize * 2), 0.0f)
{
}

SpectrumAnalyzer::~SpectrumAnalyzer()
{
    stopAnalysis();
}

void SpectrumAnalyzer::startAnalysis()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);
}

void SpectrumAnalyzer::stopAnalysis()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        notify();
        stopThread (2000);
    }
}

void SpectrumAnalyzer::setSampleRate (double newSampleRate) noexcept
{
    if (std::isfinite (newSampleRate) && newSampleRate > 1000.0)
        sampleRate.store (newSampleRate, std::memory_order_release);
}

void SpectrumAnalyzer::pushMain (const float* left, const float* right, int numSamples) noexcept
{
    push (mainStream, left, right, numSamples);
}

void SpectrumAnalyzer::pushSidechain (const float* left, const float* right, int numSamples) noexcept
{
    push (sidechainStream, left, right, numSamples);
}

void SpectrumAnalyzer::push (Stream& stream, const float* left, const float* right, int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
        return;

    const float* r = right != nullptr ? right : left;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    stream.fifo.prepareToWrite (numSamples, start1, size1, start2, size2);   // drops samples if full

    for (int i = 0; i < size1; ++i)
        stream.fifoData[static_cast<size_t> (start1 + i)] = 0.5f * (left[i] + r[i]);

    for (int i = 0; i < size2; ++i)
        stream.fifoData[static_cast<size_t> (start2 + i)] = 0.5f * (left[size1 + i] + r[size1 + i]);

    stream.fifo.finishedWrite (size1 + size2);
}

void SpectrumAnalyzer::resetStream (Stream& stream)
{
    std::fill (stream.frame.begin(), stream.frame.end(), 0.0f);
    stream.frameFill = 0;
    stream.smoothed.fill (kFloorDb);
}

void SpectrumAnalyzer::configureForSampleRate (double rate)
{
    configuredRate = rate;

    // overlap >= 50 %: hop = min (N/2, fs/30) so that ~30 updates/s are reached at 44.1/48 kHz too
    hopSize = juce::jlimit (256, kFftSize / 2, static_cast<int> (std::lround (rate / 30.0)));
    const double frameSeconds = static_cast<double> (hopSize) / rate;
    attackCoef = static_cast<float> (std::exp (-frameSeconds / 0.025));
    releaseCoef = static_cast<float> (std::exp (-frameSeconds / 0.300));

    const double binHz = rate / static_cast<double> (kFftSize);
    const int maxBin = kFftSize / 2;
    const double ratio = std::log (1000.0);   // 20 Hz .. 20 kHz

    for (int i = 0; i < kNumSpectrumBins; ++i)
    {
        const double denom = static_cast<double> (kNumSpectrumBins - 1);
        const double centre = 20.0 * std::exp (ratio * static_cast<double> (i) / denom);
        const double lo = 20.0 * std::exp (ratio * (static_cast<double> (i) - 0.5) / denom);
        const double hi = 20.0 * std::exp (ratio * (static_cast<double> (i) + 0.5) / denom);

        const auto idx = static_cast<size_t> (i);
        binLow[idx] = juce::jlimit (0, maxBin, static_cast<int> (std::ceil (lo / binHz)));
        binHigh[idx] = juce::jlimit (0, maxBin, static_cast<int> (std::floor (hi / binHz)));
        binPosition[idx] = static_cast<float> (juce::jlimit (0.0, static_cast<double> (maxBin - 1), centre / binHz));
    }

    resetStream (mainStream);
    resetStream (sidechainStream);
}

void SpectrumAnalyzer::drain (Stream& stream)
{
    int available = stream.fifo.getNumReady();

    while (available > 0)
    {
        const int needed = kFftSize - stream.frameFill;
        const int toRead = std::min (available, needed);
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        stream.fifo.prepareToRead (toRead, start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            stream.frame[static_cast<size_t> (stream.frameFill + i)] = stream.fifoData[static_cast<size_t> (start1 + i)];

        for (int i = 0; i < size2; ++i)
            stream.frame[static_cast<size_t> (stream.frameFill + size1 + i)] = stream.fifoData[static_cast<size_t> (start2 + i)];

        const int read = size1 + size2;
        stream.fifo.finishedRead (read);
        stream.frameFill += read;
        available -= read;

        if (read <= 0)
            break;

        if (stream.frameFill >= kFftSize)
        {
            analyseFrame (stream);
            std::copy (stream.frame.begin() + hopSize, stream.frame.end(), stream.frame.begin());
            stream.frameFill = kFftSize - hopSize;
        }
    }
}

void SpectrumAnalyzer::analyseFrame (Stream& stream)
{
    std::copy (stream.frame.begin(), stream.frame.end(), fftBuffer.begin());
    std::fill (fftBuffer.begin() + kFftSize, fftBuffer.end(), 0.0f);

    for (int i = 0; i < kFftSize; ++i)
        if (! std::isfinite (fftBuffer[static_cast<size_t> (i)]))
            fftBuffer[static_cast<size_t> (i)] = 0.0f;

    window.multiplyWithWindowingTable (fftBuffer.data(), static_cast<size_t> (kFftSize));
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    // Hann window (not normalised): a full-scale sine peaks at about N/4 -> 0 dBFS
    const float scale = 4.0f / static_cast<float> (kFftSize);
    const int maxBin = kFftSize / 2;

    for (size_t i = 0; i < static_cast<size_t> (kNumSpectrumBins); ++i)
    {
        float magnitude = 0.0f;

        if (binHigh[i] >= binLow[i])
        {
            for (int k = binLow[i]; k <= binHigh[i] && k <= maxBin; ++k)
                magnitude = std::max (magnitude, fftBuffer[static_cast<size_t> (k)]);
        }
        else
        {
            const float pos = binPosition[i];
            const int k0 = juce::jlimit (0, maxBin - 1, static_cast<int> (pos));
            const float frac = pos - static_cast<float> (k0);
            magnitude = fftBuffer[static_cast<size_t> (k0)] * (1.0f - frac)
                      + fftBuffer[static_cast<size_t> (k0 + 1)] * frac;
        }

        float db = 20.0f * std::log10 (std::max (magnitude * scale, 1.0e-9f));

        if (! std::isfinite (db))
            db = kFloorDb;

        db = juce::jlimit (kFloorDb, 24.0f, db);

        const float coef = db > stream.smoothed[i] ? attackCoef : releaseCoef;
        stream.smoothed[i] = coef * stream.smoothed[i] + (1.0f - coef) * db;
    }

    stream.lastFrameMs = juce::Time::getMillisecondCounter();
}

void SpectrumAnalyzer::decayIfStale (Stream& stream, juce::uint32 now)
{
    if (now - stream.lastFrameMs < 150u)
        return;

    for (auto& v : stream.smoothed)
        v = std::max (kFloorDb, v - 2.0f);
}

void SpectrumAnalyzer::publish()
{
    localMain.write (mainStream.smoothed.data());
    localSidechain.write (sidechainStream.smoothed.data());

    auto& registry = InstanceRegistry::get();
    const int index = slotIndex.load (std::memory_order_acquire);

    if (auto* slot = registry.getSlot (index))
    {
        if (slot->owner.load (std::memory_order_acquire) == instanceId)
        {
            slot->spectrum.write (mainStream.smoothed.data());
            slot->sidechainSpectrum.write (sidechainStream.smoothed.data());
        }
    }
}

void SpectrumAnalyzer::writeHeartbeat (juce::uint32 now)
{
    auto& registry = InstanceRegistry::get();
    const int index = slotIndex.load (std::memory_order_acquire);

    if (auto* slot = registry.getSlot (index))
        if (slot->owner.load (std::memory_order_acquire) == instanceId)
            slot->heartbeatMs.store (now, std::memory_order_release);
}

void SpectrumAnalyzer::run()
{
    juce::uint32 lastPublishMs = 0;

    while (! threadShouldExit())
    {
        const double rate = sampleRate.load (std::memory_order_acquire);

        if (std::abs (rate - configuredRate) > 0.5)
            configureForSampleRate (rate);

        drain (mainStream);
        drain (sidechainStream);

        const auto now = juce::Time::getMillisecondCounter();

        if (now - lastPublishMs >= 33u)
        {
            decayIfStale (mainStream, now);
            decayIfStale (sidechainStream, now);
            publish();
            lastPublishMs = now;
        }

        writeHeartbeat (now);
        wait (10);
    }
}
} // namespace eqtrum
