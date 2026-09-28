#include "heap.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "memory.h"

GuestHeap g_guestHeap;

namespace {

constexpr uint32_t kSmallBase = 0x00010000;
constexpr uint32_t kSmallEnd = 0x40000000;
constexpr uint32_t kLargeBase = 0x40000000;
constexpr uint32_t kLargeEnd = 0x7FE00000;
constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
constexpr uint32_t kGpuRegisterSize = 0x00010000;
constexpr uint32_t kKernelStackBase = 0x70000000;
constexpr uint32_t kKernelStackEnd = 0x7F000000;
constexpr uint32_t kUserBase = 0x88000000;
constexpr uint32_t kUserEnd = 0x9FF00000;
constexpr uint32_t kPhysicalBase = 0xA0000000;
constexpr uint32_t kPhysicalEnd = 0xBFFF0000;

static_assert(uint64_t(kUserBase) >=
              PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2,
              "guest user heap overlaps XenonRecomp indirect dispatch table");

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t RangeAlloc(std::map<uint32_t, uint32_t>& freeMap,
                    std::unordered_map<uint32_t, uint32_t>& usedMap,
                    uint32_t need, uint32_t alignment, bool topDown)
{
    auto carve = [&](std::map<uint32_t, uint32_t>::iterator it) -> uint32_t {
        const uint32_t block = it->first;
        const uint32_t blockSize = it->second;
        uint32_t address = AlignUp(block, alignment);

        if (topDown && blockSize >= need)
        {
            const uint32_t high = (block + blockSize - need) & ~(alignment - 1);
            if (high >= address)
                address = high;
        }

        if (uint64_t(address) + need > uint64_t(block) + blockSize)
            return 0;

        freeMap.erase(it);
        if (address > block)
            freeMap.emplace(block, address - block);
        if (address + need < block + blockSize)
            freeMap.emplace(address + need, block + blockSize - address - need);
        usedMap.emplace(address, need);
        return address;
    };

    if (topDown)
    {
        for (auto it = freeMap.rbegin(); it != freeMap.rend(); ++it)
        {
            auto forward = std::prev(it.base());
            if (const uint32_t address = carve(forward))
                return address;
        }
    }
    else
    {
        for (auto it = freeMap.begin(); it != freeMap.end(); ++it)
            if (const uint32_t address = carve(it))
                return address;
    }
    return 0;
}

bool RangeFree(std::map<uint32_t, uint32_t>& freeMap,
               std::unordered_map<uint32_t, uint32_t>& usedMap,
               uint32_t address)
{
    auto used = usedMap.find(address);
    if (used == usedMap.end())
        return false;

    uint32_t size = used->second;
    usedMap.erase(used);

    auto next = freeMap.lower_bound(address);
    if (next != freeMap.end() && address + size == next->first)
    {
        size += next->second;
        next = freeMap.erase(next);
    }
    if (next != freeMap.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == address)
        {
            address = previous->first;
            size += previous->second;
            freeMap.erase(previous);
        }
    }
    freeMap.emplace(address, size);
    return true;
}

uint32_t NormalizePhysicalGuest(uint32_t guest)
{
    if (guest >= 0xC0000000u)
        return 0xA0000000u | (guest & 0x1FFFFFFFu);
    return guest;
}

} // namespace

void GuestHeap::Init()
{
    std::lock_guard lock(mutex_);
    if (initialized_)
        return;

    user_.free.emplace(kUserBase, kUserEnd - kUserBase);
    physical_.free.emplace(kPhysicalBase, kPhysicalEnd - kPhysicalBase);
    smallVirtual_.free.emplace(kSmallBase, kSmallEnd - kSmallBase);

    // The Xenos MMIO aperture is hardware-owned and must never be returned as
    // ordinary virtual memory.
    largeVirtual_.free.emplace(kLargeBase, kKernelStackBase - kLargeBase);
    kernelStack_.free.emplace(kKernelStackBase, kKernelStackEnd - kKernelStackBase);
    largeVirtual_.free.emplace(kKernelStackEnd, kGpuRegisterBase - kKernelStackEnd);
    largeVirtual_.free.emplace(kGpuRegisterBase + kGpuRegisterSize,
                               kLargeEnd - kGpuRegisterBase - kGpuRegisterSize);
    initialized_ = true;
}

void* GuestHeap::Alloc(size_t size, size_t alignment)
{
    const uint32_t align = std::max<uint32_t>(16, static_cast<uint32_t>(alignment));
    const uint32_t need = AlignUp(static_cast<uint32_t>(std::max<size_t>(size, 1)), 16);
    std::lock_guard lock(mutex_);
    const uint32_t address = RangeAlloc(user_.free, user_.used, need, align, false);
    if (!address)
        return nullptr;
    std::memset(g_guestMemory.Translate(address), 0, need);
    return g_guestMemory.Translate(address);
}

void* GuestHeap::AllocVirtual(size_t size, bool largePages)
{
    const uint32_t need = AlignUp(
        static_cast<uint32_t>(std::max<size_t>(size, 1)), 0x10000);
    std::lock_guard lock(mutex_);
    Arena& preferred = largePages ? largeVirtual_ : smallVirtual_;
    Arena& fallback = largePages ? smallVirtual_ : largeVirtual_;
    uint32_t address = RangeAlloc(preferred.free, preferred.used, need, 0x10000, false);
    if (!address)
        address = RangeAlloc(fallback.free, fallback.used, need, 0x10000, false);
    if (!address)
        return nullptr;
    std::memset(g_guestMemory.Translate(address), 0, need);
    return g_guestMemory.Translate(address);
}

uint32_t GuestHeap::ReserveVirtualAt(uint32_t base, size_t size)
{
    if (base < kLargeBase || base >= kLargeEnd || (base & 0xFFFF))
        return 0;
    const uint32_t need = AlignUp(static_cast<uint32_t>(std::max<size_t>(size, 1)), 0x10000);
    if (uint64_t(base) + need > kLargeEnd)
        return 0;

    std::lock_guard lock(mutex_);
    auto it = largeVirtual_.free.upper_bound(base);
    if (it == largeVirtual_.free.begin())
        return 0;
    --it;
    const uint32_t block = it->first;
    const uint32_t blockSize = it->second;
    if (base < block || uint64_t(base) + need > uint64_t(block) + blockSize)
        return 0;

    largeVirtual_.free.erase(it);
    if (base > block)
        largeVirtual_.free.emplace(block, base - block);
    if (base + need < block + blockSize)
        largeVirtual_.free.emplace(base + need, block + blockSize - base - need);
    largeVirtual_.used.emplace(base, need);
    std::memset(g_guestMemory.Translate(base), 0, need);
    return base;
}

void* GuestHeap::AllocPhysical(size_t size, size_t alignment, bool topDown)
{
    const uint32_t align = static_cast<uint32_t>(alignment ? alignment : 0x1000);
    const uint32_t granularity = size >= 0x1000 || align > 0x1000 ? 0x1000 : 16;
    const uint32_t need = AlignUp(
        static_cast<uint32_t>(std::max<size_t>(size, 1)), granularity);

    std::lock_guard lock(mutex_);
    const uint32_t address = RangeAlloc(physical_.free, physical_.used, need,
                                        std::max<uint32_t>(16, align), topDown);
    if (!address)
        return nullptr;
    std::memset(g_guestMemory.Translate(address), 0, need);
    return g_guestMemory.Translate(address);
}

void* GuestHeap::AllocKernelStack(size_t size, size_t alignment)
{
    const uint32_t align = std::max<uint32_t>(0x1000, static_cast<uint32_t>(alignment));
    const uint32_t need = AlignUp(static_cast<uint32_t>(std::max<size_t>(size, 1)), 0x1000);
    std::lock_guard lock(mutex_);
    const uint32_t address =
        RangeAlloc(kernelStack_.free, kernelStack_.used, need, align, false);
    if (!address)
        return nullptr;
    std::memset(g_guestMemory.Translate(address), 0, need);
    return g_guestMemory.Translate(address);
}

void GuestHeap::Free(void* ptr)
{
    if (!ptr)
        return;
    uint32_t guest = g_guestMemory.MapVirtual(ptr);
    guest = NormalizePhysicalGuest(guest);

    std::lock_guard lock(mutex_);
    if (guest >= kPhysicalBase && guest < kPhysicalEnd)
        RangeFree(physical_.free, physical_.used, guest);
    else if (guest >= kUserBase && guest < kUserEnd)
        RangeFree(user_.free, user_.used, guest);
    else if (guest >= kSmallBase && guest < kSmallEnd)
        RangeFree(smallVirtual_.free, smallVirtual_.used, guest);
    else if (guest >= kKernelStackBase && guest < kKernelStackEnd)
        RangeFree(kernelStack_.free, kernelStack_.used, guest);
    else if (guest >= kLargeBase && guest < kLargeEnd)
        RangeFree(largeVirtual_.free, largeVirtual_.used, guest);
}

size_t GuestHeap::Size(void* ptr)
{
    if (!ptr)
        return 0;
    uint32_t guest = NormalizePhysicalGuest(g_guestMemory.MapVirtual(ptr));
    std::lock_guard lock(mutex_);

    const auto lookup = [&](const Arena& arena) -> size_t {
        auto it = arena.used.find(guest);
        return it == arena.used.end() ? 0 : it->second;
    };
    if (guest >= kPhysicalBase && guest < kPhysicalEnd) return lookup(physical_);
    if (guest >= kUserBase && guest < kUserEnd) return lookup(user_);
    if (guest >= kSmallBase && guest < kSmallEnd) return lookup(smallVirtual_);
    if (guest >= kKernelStackBase && guest < kKernelStackEnd) return lookup(kernelStack_);
    if (guest >= kLargeBase && guest < kLargeEnd) return lookup(largeVirtual_);
    return 0;
}

bool GuestHeap::QueryRegion(uint32_t address, uint32_t& regionBase, uint32_t& regionSize)
{
    address = NormalizePhysicalGuest(address);
    std::lock_guard lock(mutex_);
    for (const Arena* arena : {&smallVirtual_, &largeVirtual_, &kernelStack_, &user_, &physical_})
    {
        for (const auto& [base, size] : arena->used)
        {
            if (address >= base && uint64_t(address) < uint64_t(base) + size)
            {
                regionBase = base;
                regionSize = size;
                return true;
            }
        }
    }
    return false;
}
