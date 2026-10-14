#pragma once

#include "Params.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace eqtrum
{
// Single-writer seqlock around a 512-bin spectrum (dB values). Elements are relaxed atomics,
// so a concurrent read is never a data race; the sequence counter detects torn frames.
class SpectrumSeqlock
{
public:
    SpectrumSeqlock() noexcept;

    void write (const float* source) noexcept;                 // single writer (analysis thread)
    bool read (float* destination) const noexcept;             // any reader, returns false if torn
    std::uint32_t getVersion() const noexcept { return sequence.load (std::memory_order_acquire); }
    void fill (float value) noexcept;

private:
    std::atomic<std::uint32_t> sequence { 0 };
    std::array<std::atomic<float>, kNumSpectrumBins> bins;
};

struct RemoteCommand
{
    enum class Type : std::int32_t
    {
        setParam = 0,
        setName = 1,
        setColour = 2
    };

    Type type = Type::setParam;
    std::int32_t paramIndex = -1;          // index into ParamTable::ids (same binary in every instance)
    float value = 0.0f;                    // normalised 0..1
    std::uint32_t colour = 0;              // ARGB for setColour
    std::array<char, 48> text {};          // UTF-8, zero terminated, for setName
};

// Lock-free single-producer / single-consumer ring (producer: HUB editor, consumer: TRACK timer).
// Both normally run on the host's message thread; the atomics keep it correct across threads too.
class CommandQueue
{
public:
    static constexpr std::uint32_t capacity = 512;   // power of two

    bool push (const RemoteCommand& command) noexcept;
    bool pop (RemoteCommand& command) noexcept;
    int getFreeSpace() const noexcept;
    void clear() noexcept;   // consumer side

private:
    std::array<RemoteCommand, capacity> buffer {};
    std::atomic<std::uint32_t> writeIndex { 0 };
    std::atomic<std::uint32_t> readIndex { 0 };
};

struct Slot
{
    Slot() noexcept;

    std::atomic<std::uint64_t> owner { 0 };              // 0 = free, else unique instance id
    std::atomic<std::uint32_t> heartbeatMs { 0 };        // juce::Time::getMillisecondCounter()
    std::atomic<std::uint32_t> colourArgb { 0xff808080u };
    std::atomic<bool> visible { true };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> latencySamples { 0 };
    std::atomic<float> sidechainLevel { 0.0f };          // linear peak of the last block
    std::atomic<bool> sidechainConnected { false };
    std::atomic<std::uint32_t> paramVersion { 0 };
    std::array<std::atomic<float>, kMaxParams> params;   // normalised parameter mirror
    std::array<std::atomic<float>, kNumBands> dynOffsetDb;
    SpectrumSeqlock spectrum;
    SpectrumSeqlock sidechainSpectrum;
    CommandQueue commands;

    void setName (const juce::String& newName);
    juce::String getName() const;

private:
    mutable juce::SpinLock nameLock;
    std::array<char, 64> name {};
};

// In-process registry shared by every Eqtrum instance loaded from the same binary.
class InstanceRegistry
{
public:
    static constexpr int kMaxSlots = 64;
    static constexpr std::uint32_t kStaleTimeoutMs = 2000;

    static InstanceRegistry& get();

    std::uint64_t createInstanceId() noexcept;
    int acquire (std::uint64_t ownerId) noexcept;              // message thread
    void release (int index, std::uint64_t ownerId) noexcept;  // message thread
    void reapStale() noexcept;                                 // message thread
    bool isOwnedBy (int index, std::uint64_t ownerId) const noexcept;
    Slot* getSlot (int index) noexcept;
    const Slot* getSlot (int index) const noexcept;

private:
    InstanceRegistry() = default;

    std::array<Slot, kMaxSlots> slots;
    std::atomic<std::uint64_t> nextId { 1 };
};

juce::Colour getPaletteColour (int index);
int getPaletteSize() noexcept;
} // namespace eqtrum
