#include "InstanceRegistry.h"

#include <cstring>

namespace eqtrum
{
//==============================================================================
SpectrumSeqlock::SpectrumSeqlock() noexcept
{
    for (auto& b : bins)
        b.store (-140.0f, std::memory_order_relaxed);
}

void SpectrumSeqlock::write (const float* source) noexcept
{
    if (source == nullptr)
        return;

    const auto s = sequence.load (std::memory_order_relaxed);
    sequence.store (s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);

    for (size_t i = 0; i < bins.size(); ++i)
        bins[i].store (source[i], std::memory_order_relaxed);

    sequence.store (s + 2, std::memory_order_release);
}

bool SpectrumSeqlock::read (float* destination) const noexcept
{
    if (destination == nullptr)
        return false;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto s1 = sequence.load (std::memory_order_acquire);

        if ((s1 & 1u) != 0)
            continue;

        for (size_t i = 0; i < bins.size(); ++i)
            destination[i] = bins[i].load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire);

        if (sequence.load (std::memory_order_relaxed) == s1)
            return true;
    }

    return false;
}

void SpectrumSeqlock::fill (float value) noexcept
{
    std::array<float, kNumSpectrumBins> tmp;
    tmp.fill (value);
    write (tmp.data());
}

//==============================================================================
bool CommandQueue::push (const RemoteCommand& command) noexcept
{
    const auto w = writeIndex.load (std::memory_order_relaxed);
    const auto r = readIndex.load (std::memory_order_acquire);

    if (w - r >= capacity)
        return false;

    buffer[w % capacity] = command;
    writeIndex.store (w + 1, std::memory_order_release);
    return true;
}

bool CommandQueue::pop (RemoteCommand& command) noexcept
{
    const auto r = readIndex.load (std::memory_order_relaxed);
    const auto w = writeIndex.load (std::memory_order_acquire);

    if (r == w)
        return false;

    command = buffer[r % capacity];
    readIndex.store (r + 1, std::memory_order_release);
    return true;
}

int CommandQueue::getFreeSpace() const noexcept
{
    const auto w = writeIndex.load (std::memory_order_acquire);
    const auto r = readIndex.load (std::memory_order_acquire);
    const auto used = w - r;
    return used >= capacity ? 0 : static_cast<int> (capacity - used);
}

void CommandQueue::clear() noexcept
{
    readIndex.store (writeIndex.load (std::memory_order_acquire), std::memory_order_release);
}

//==============================================================================
Slot::Slot() noexcept
{
    for (auto& p : params)
        p.store (0.0f, std::memory_order_relaxed);

    for (auto& d : dynOffsetDb)
        d.store (0.0f, std::memory_order_relaxed);
}

void Slot::setName (const juce::String& newName)
{
    std::array<char, 64> tmp {};
    newName.copyToUTF8 (tmp.data(), tmp.size() - 1);

    const juce::SpinLock::ScopedLockType lock (nameLock);
    name = tmp;
}

juce::String Slot::getName() const
{
    std::array<char, 64> tmp {};

    {
        const juce::SpinLock::ScopedLockType lock (nameLock);
        tmp = name;
    }

    tmp.back() = 0;
    return juce::String::fromUTF8 (tmp.data());
}

//==============================================================================
InstanceRegistry& InstanceRegistry::get()
{
    static InstanceRegistry instance;
    return instance;
}

std::uint64_t InstanceRegistry::createInstanceId() noexcept
{
    return nextId.fetch_add (1, std::memory_order_relaxed);
}

int InstanceRegistry::acquire (std::uint64_t ownerId) noexcept
{
    if (ownerId == 0)
        return -1;

    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& slot = slots[static_cast<size_t> (i)];
        std::uint64_t expected = 0;

        if (slot.owner.compare_exchange_strong (expected, ownerId, std::memory_order_acq_rel))
        {
            slot.heartbeatMs.store (juce::Time::getMillisecondCounter(), std::memory_order_release);
            slot.commands.clear();
            slot.visible.store (true, std::memory_order_relaxed);
            slot.latencySamples.store (0, std::memory_order_relaxed);
            slot.sidechainLevel.store (0.0f, std::memory_order_relaxed);
            slot.sidechainConnected.store (false, std::memory_order_relaxed);
            slot.spectrum.fill (-140.0f);
            slot.sidechainSpectrum.fill (-140.0f);
            slot.paramVersion.fetch_add (1, std::memory_order_release);
            return i;
        }
    }

    return -1;
}

void InstanceRegistry::release (int index, std::uint64_t ownerId) noexcept
{
    if (auto* slot = getSlot (index))
    {
        auto expected = ownerId;
        slot->owner.compare_exchange_strong (expected, 0, std::memory_order_acq_rel);
    }
}

void InstanceRegistry::reapStale() noexcept
{
    const auto now = juce::Time::getMillisecondCounter();

    for (auto& slot : slots)
    {
        auto owner = slot.owner.load (std::memory_order_acquire);

        if (owner == 0)
            continue;

        // unsigned subtraction is correct across the 32-bit counter wrap
        const auto age = now - slot.heartbeatMs.load (std::memory_order_acquire);

        if (age > kStaleTimeoutMs && age < 0x80000000u)
            slot.owner.compare_exchange_strong (owner, 0, std::memory_order_acq_rel);
    }
}

bool InstanceRegistry::isOwnedBy (int index, std::uint64_t ownerId) const noexcept
{
    if (const auto* slot = getSlot (index))
        return ownerId != 0 && slot->owner.load (std::memory_order_acquire) == ownerId;

    return false;
}

Slot* InstanceRegistry::getSlot (int index) noexcept
{
    if (index < 0 || index >= kMaxSlots)
        return nullptr;

    return &slots[static_cast<size_t> (index)];
}

const Slot* InstanceRegistry::getSlot (int index) const noexcept
{
    if (index < 0 || index >= kMaxSlots)
        return nullptr;

    return &slots[static_cast<size_t> (index)];
}

//==============================================================================
namespace
{
const std::uint32_t paletteArgb[] = {
    0xff4fc3f7u, 0xffff8a65u, 0xffaed581u, 0xffba68c8u,
    0xffffd54fu, 0xff4db6acu, 0xfff06292u, 0xff7986cbu,
    0xffa1887fu, 0xff81c784u, 0xffe57373u, 0xff64b5f6u,
    0xffdce775u, 0xff9575cdu, 0xffffb74du, 0xff4dd0e1u
};
} // namespace

int getPaletteSize() noexcept
{
    return static_cast<int> (sizeof (paletteArgb) / sizeof (paletteArgb[0]));
}

juce::Colour getPaletteColour (int index)
{
    const int size = getPaletteSize();
    const int i = ((index % size) + size) % size;
    return juce::Colour (paletteArgb[i]);
}
} // namespace eqtrum
