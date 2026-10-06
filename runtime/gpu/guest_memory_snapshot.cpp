#include "guest_memory_snapshot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <list>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mojorecomp::gpu {
namespace {

constexpr uint32_t kTrackedGuestBase = 0xA0000000u;
constexpr uint64_t kTrackedGuestBytes = 0x20000000ull;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageBytes = 1u << kPageShift;
constexpr size_t kPageCount = size_t(kTrackedGuestBytes >> kPageShift);
constexpr size_t kMaxRangeCacheEntries = 8192;

struct CacheKey
{
    uint32_t address = 0;
    uint64_t bytes = 0;

    bool operator==(const CacheKey& other) const noexcept
    {
        return address == other.address && bytes == other.bytes;
    }
};

struct CacheKeyHash
{
    size_t operator()(const CacheKey& key) const noexcept
    {
        uint64_t value = (uint64_t(key.address) << 32) ^ key.bytes;
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdull;
        value ^= value >> 33;
        return static_cast<size_t>(value);
    }
};

struct CacheEntry
{
    std::vector<uint64_t> generations;
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint64_t identity = 0;
    std::list<CacheKey>::iterator lru;
};

std::mutex g_cacheMutex;
std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> g_rangeCache;
std::list<CacheKey> g_rangeCacheLru;
std::atomic<uintptr_t> g_guestBase{0};
std::array<std::atomic<uint64_t>, kPageCount> g_pageGeneration{};
// 0 = writable/dirty, 1 = read-only/tracked, 2 = protection transition,
// 3 = write-fault transition back to writable.
std::array<std::atomic<uint8_t>, kPageCount> g_pageState{};
std::array<std::atomic<uint8_t>, kPageCount> g_pageTouched{};
std::atomic<uint64_t> g_cacheHits{0};
std::atomic<uint64_t> g_cacheMisses{0};
std::atomic<uint64_t> g_missingReads{0};
std::atomic<uint64_t> g_nextIdentity{1};

#if defined(_WIN32)
PVOID g_vectoredHandler = nullptr;

bool SetAliasPageProtection(uintptr_t base, size_t firstPage, size_t lastPage,
                            DWORD protection) noexcept
{
    const SIZE_T span = SIZE_T(lastPage - firstPage + 1u) * kPageBytes;
    bool ok = true;
    for (uint32_t view : kGuestMemoryPhysicalViews)
    {
        DWORD oldProtection = 0;
        void* host = reinterpret_cast<void*>(
            base + view + (uintptr_t(firstPage) << kPageShift));
        ok = VirtualProtect(host, span, protection, &oldProtection) != FALSE && ok;
    }
    return ok;
}
#endif

thread_local const GuestMemorySnapshot* g_activeSnapshot = nullptr;
thread_local bool g_activeSnapshotReadFailed = false;

bool TrackedRange(uint32_t address, uint64_t bytes) noexcept
{
    if (!bytes || address < kTrackedGuestBase)
        return false;
    const uint64_t offset = uint64_t(address) - kTrackedGuestBase;
    return offset < kTrackedGuestBytes && bytes <= kTrackedGuestBytes - offset;
}

size_t FirstPage(uint32_t address) noexcept
{
    return size_t((uint64_t(address) - kTrackedGuestBase) >> kPageShift);
}

size_t LastPage(uint32_t address, uint64_t bytes) noexcept
{
    return size_t((uint64_t(address) - kTrackedGuestBase + bytes - 1u) >> kPageShift);
}

#if defined(_WIN32)
LONG CALLBACK SnapshotWriteFaultHandler(EXCEPTION_POINTERS* exceptionInfo)
{
    if (!exceptionInfo || !exceptionInfo->ExceptionRecord ||
        exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exceptionInfo->ExceptionRecord->NumberParameters < 2 ||
        exceptionInfo->ExceptionRecord->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t base = g_guestBase.load(std::memory_order_acquire);
    if (!base)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t fault = static_cast<uintptr_t>(
        exceptionInfo->ExceptionRecord->ExceptionInformation[1]);
    size_t page = kPageCount;
    for (uint32_t view : kGuestMemoryPhysicalViews)
    {
        const uintptr_t trackedBegin = base + view;
        const uintptr_t trackedEnd = trackedBegin + kTrackedGuestBytes;
        if (fault >= trackedBegin && fault < trackedEnd)
        {
            page = size_t((fault - trackedBegin) >> kPageShift);
            break;
        }
    }
    if (page >= kPageCount)
        return EXCEPTION_CONTINUE_SEARCH;

    if (!g_pageTouched[page].load(std::memory_order_acquire))
        return EXCEPTION_CONTINUE_SEARCH;

    g_pageState[page].store(3, std::memory_order_release);
    g_pageGeneration[page].fetch_add(1, std::memory_order_acq_rel);
    if (!SetAliasPageProtection(base, page, page, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    g_pageState[page].store(0, std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool EnsureTrackingBase(const uint8_t* guestBase)
{
    if (!guestBase)
        return false;
    const uintptr_t wanted = reinterpret_cast<uintptr_t>(guestBase);
    const uintptr_t current = g_guestBase.load(std::memory_order_acquire);
    if (current == wanted && g_vectoredHandler)
        return true;
    if (current && current != wanted)
        return false;
    if (!g_vectoredHandler)
    {
        g_vectoredHandler = AddVectoredExceptionHandler(1, SnapshotWriteFaultHandler);
        if (!g_vectoredHandler)
            return false;
    }
    g_guestBase.store(wanted, std::memory_order_release);
    return true;
}

bool ProtectTrackedPages(const uint8_t* guestBase, size_t firstPage, size_t lastPage)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(guestBase);
    size_t page = firstPage;
    while (page <= lastPage)
    {
        const uint8_t state = g_pageState[page].load(std::memory_order_acquire);
        if (state == 1)
        {
            ++page;
            continue;
        }
        if (state == 2 || state == 3)
        {
            std::this_thread::yield();
            continue;
        }

        size_t runBegin = page;
        size_t runEnd = page;
        uint8_t expected = 0;
        if (!g_pageState[page].compare_exchange_strong(
                expected, 2, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        g_pageTouched[page].store(1, std::memory_order_release);

        while (runEnd < lastPage)
        {
            expected = 0;
            if (!g_pageState[runEnd + 1].compare_exchange_strong(
                    expected, 2, std::memory_order_acq_rel, std::memory_order_acquire))
                break;
            ++runEnd;
            g_pageTouched[runEnd].store(1, std::memory_order_release);
        }

        if (!SetAliasPageProtection(base, runBegin, runEnd, PAGE_READONLY))
        {
            SetAliasPageProtection(base, runBegin, runEnd, PAGE_READWRITE);
            for (size_t i = runBegin; i <= runEnd; ++i)
                g_pageState[i].store(0, std::memory_order_release);
            return false;
        }

        for (size_t i = runBegin; i <= runEnd; ++i)
        {
            expected = 2;
            g_pageState[i].compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        page = runEnd + 1u;
    }

    for (size_t i = firstPage; i <= lastPage; ++i)
        if (g_pageState[i].load(std::memory_order_acquire) != 1)
            return ProtectTrackedPages(guestBase, firstPage, lastPage);
    return true;
}
#endif

std::vector<uint64_t> ReadGenerations(size_t firstPage, size_t lastPage)
{
    std::vector<uint64_t> generations;
    generations.reserve(lastPage - firstPage + 1u);
    for (size_t page = firstPage; page <= lastPage; ++page)
        generations.push_back(g_pageGeneration[page].load(std::memory_order_acquire));
    return generations;
}

bool GenerationsStable(const std::vector<uint64_t>& before,
                       size_t firstPage, size_t lastPage)
{
    if (before.size() != lastPage - firstPage + 1u)
        return false;
    for (size_t page = firstPage, i = 0; page <= lastPage; ++page, ++i)
    {
        if (g_pageState[page].load(std::memory_order_acquire) != 1 ||
            g_pageGeneration[page].load(std::memory_order_acquire) != before[i])
            return false;
    }
    return true;
}

void TrimCache()
{
    while (g_rangeCache.size() > kMaxRangeCacheEntries)
    {
        if (g_rangeCacheLru.empty())
            break;
        // Removing the cache's shared_ptr is safe even while an in-flight
        // snapshot still references the same immutable bytes: that snapshot
        // owns its own shared_ptr. Keep eviction strictly O(1) instead of
        // scanning the whole cache looking for an unreferenced entry.
        const CacheKey victim = g_rangeCacheLru.back();
        g_rangeCacheLru.pop_back();
        g_rangeCache.erase(victim);
    }
}

void TouchCacheEntry(CacheEntry& entry)
{
    if (entry.lru != g_rangeCacheLru.begin())
        g_rangeCacheLru.splice(g_rangeCacheLru.begin(), g_rangeCacheLru, entry.lru);
}

} // namespace

bool GuestMemorySnapshot::Capture(const uint8_t* guestBase, uint32_t address, uint64_t bytes)
{
    if (!guestBase || !bytes || bytes > std::numeric_limits<size_t>::max() ||
        uint64_t(address) + bytes > uint64_t(UINT32_MAX) + 1ull)
        return false;

    for (const Range& range : ranges_)
    {
        if (address >= range.address &&
            uint64_t(address) + bytes <= uint64_t(range.address) + range.bytes)
            return true;
    }

    std::shared_ptr<const std::vector<uint8_t>> captured;
    uint64_t identity = 0;
    if (TrackedRange(address, bytes))
    {
#if defined(_WIN32)
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (!EnsureTrackingBase(guestBase))
            return false;
        const size_t firstPage = FirstPage(address);
        const size_t lastPage = LastPage(address, bytes);
        const CacheKey key{address, bytes};

        for (;;)
        {
            auto cached = g_rangeCache.find(key);
            if (cached != g_rangeCache.end() &&
                GenerationsStable(cached->second.generations, firstPage, lastPage))
            {
                TouchCacheEntry(cached->second);
                captured = cached->second.data;
                identity = cached->second.identity;
                g_cacheHits.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            // A stable cached generation implies every page is still tracked
            // read-only, so the common cache-hit path above needs no separate
            // protection scan. Only misses/dirty ranges need to transition
            // pages back to the tracked state before copying.
            if (!ProtectTrackedPages(guestBase, firstPage, lastPage))
                return false;

            std::vector<uint64_t> generations = ReadGenerations(firstPage, lastPage);
            auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(bytes));
            std::memcpy(data->data(), guestBase + address, static_cast<size_t>(bytes));
            if (!GenerationsStable(generations, firstPage, lastPage))
                continue;

            if (cached != g_rangeCache.end())
            {
                cached->second.generations = std::move(generations);
                cached->second.data = data;
                cached->second.identity = g_nextIdentity.fetch_add(1, std::memory_order_relaxed);
                TouchCacheEntry(cached->second);
                identity = cached->second.identity;
            }
            else
            {
                g_rangeCacheLru.push_front(key);
                CacheEntry entry{};
                entry.generations = std::move(generations);
                entry.data = data;
                entry.identity = g_nextIdentity.fetch_add(1, std::memory_order_relaxed);
                entry.lru = g_rangeCacheLru.begin();
                identity = entry.identity;
                g_rangeCache.emplace(key, std::move(entry));
            }
            TrimCache();
            captured = std::move(data);
            g_cacheMisses.fetch_add(1, std::memory_order_relaxed);
            break;
        }
#else
        auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(bytes));
        std::memcpy(data->data(), guestBase + address, static_cast<size_t>(bytes));
        captured = std::move(data);
        identity = g_nextIdentity.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    else
    {
        auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(bytes));
        std::memcpy(data->data(), guestBase + address, static_cast<size_t>(bytes));
        captured = std::move(data);
        identity = g_nextIdentity.fetch_add(1, std::memory_order_relaxed);
    }

    ranges_.push_back({address, bytes, identity, std::move(captured)});
    byteSize_ += bytes;
    return true;
}

void GuestMemorySnapshot::Overlay(uint32_t address, const void* data, uint64_t bytes)
{
    if (!data || !bytes || uint64_t(address) + bytes > uint64_t(UINT32_MAX) + 1ull)
        return;

    const uint64_t overlayBegin = address;
    const uint64_t overlayEnd = overlayBegin + bytes;
    const auto* source = static_cast<const uint8_t*>(data);
    for (Range& range : ranges_)
    {
        const uint64_t rangeBegin = range.address;
        const uint64_t rangeEnd = rangeBegin + range.bytes;
        const uint64_t copyBegin = std::max(overlayBegin, rangeBegin);
        const uint64_t copyEnd = std::min(overlayEnd, rangeEnd);
        if (copyBegin >= copyEnd || !range.data)
            continue;

        auto patched = std::make_shared<std::vector<uint8_t>>(*range.data);
        std::memcpy(patched->data() + static_cast<size_t>(copyBegin - rangeBegin),
                    source + static_cast<size_t>(copyBegin - overlayBegin),
                    static_cast<size_t>(copyEnd - copyBegin));
        range.data = std::move(patched);
        range.identity = g_nextIdentity.fetch_add(1, std::memory_order_relaxed);
    }
}

const uint8_t* GuestMemorySnapshot::Resolve(uint32_t address, uint64_t bytes) const noexcept
{
    if (!bytes)
        return nullptr;
    for (const Range& range : ranges_)
    {
        if (address < range.address)
            continue;
        const uint64_t offset = uint64_t(address) - range.address;
        if (offset <= range.bytes && bytes <= range.bytes - offset && range.data)
            return range.data->data() + static_cast<size_t>(offset);
    }
    return nullptr;
}

bool GuestMemorySnapshot::ResolveIdentity(uint32_t address, uint64_t bytes,
                                          GuestMemoryIdentity& out) const noexcept
{
    out = {};
    if (!bytes)
        return false;
    for (const Range& range : ranges_)
    {
        if (address < range.address)
            continue;
        const uint64_t offset = uint64_t(address) - range.address;
        if (offset <= range.bytes && bytes <= range.bytes - offset && range.identity)
        {
            out.token = range.identity;
            out.offset = offset;
            return true;
        }
    }
    return false;
}

const uint8_t* GuestMemorySnapshot::ResolveZeroFill(uint64_t bytes) const noexcept
{
    if (!bytes || bytes > std::numeric_limits<size_t>::max())
        return nullptr;
    try
    {
        {
            std::lock_guard<std::mutex> lock(zeroFillMutex_);
            for (const auto& data : zeroFills_)
                if (data->size() == bytes)
                    return data->data();
        }
        auto data = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(bytes), 0);
        std::lock_guard<std::mutex> lock(zeroFillMutex_);
        for (const auto& existing : zeroFills_)
            if (existing->size() == bytes)
                return existing->data();
        const uint8_t* result = data->data();
        zeroFills_.push_back(std::move(data));
        return result;
    }
    catch (...)
    {
        return nullptr;
    }
}

ScopedGuestMemorySnapshot::ScopedGuestMemorySnapshot(
    const GuestMemorySnapshot* snapshot) noexcept
    : previous_(g_activeSnapshot),
      previousReadFailed_(g_activeSnapshotReadFailed)
{
    g_activeSnapshot = snapshot;
    g_activeSnapshotReadFailed = false;
}

ScopedGuestMemorySnapshot::~ScopedGuestMemorySnapshot()
{
    g_activeSnapshot = previous_;
    g_activeSnapshotReadFailed = previousReadFailed_;
}

const uint8_t* GuestReadPtr(const uint8_t* guestBase, uint32_t address,
                            uint64_t bytes) noexcept
{
    if (g_activeSnapshot)
    {
        if (const uint8_t* resolved = g_activeSnapshot->Resolve(address, bytes))
            return resolved;
        g_missingReads.fetch_add(1, std::memory_order_relaxed);
        g_activeSnapshotReadFailed = true;
        return g_activeSnapshot->ResolveZeroFill(bytes);
    }
    return guestBase ? guestBase + address : nullptr;
}

bool GuestSnapshotReadFailed() noexcept
{
    return g_activeSnapshot && g_activeSnapshotReadFailed;
}

bool GuestReadIdentity(uint32_t address, uint64_t bytes,
                       GuestMemoryIdentity& out) noexcept
{
    out = {};
    return g_activeSnapshot && g_activeSnapshot->ResolveIdentity(address, bytes, out);
}

uint64_t GuestSnapshotCacheHitCount() noexcept
{
    return g_cacheHits.load(std::memory_order_relaxed);
}

uint64_t GuestSnapshotCacheMissCount() noexcept
{
    return g_cacheMisses.load(std::memory_order_relaxed);
}

uint64_t GuestSnapshotMissingReadCount() noexcept
{
    return g_missingReads.load(std::memory_order_relaxed);
}

void GuestSnapshotCacheShutdown()
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
#if defined(_WIN32)
    const uintptr_t base = g_guestBase.load(std::memory_order_acquire);
    if (base)
    {
        size_t page = 0;
        while (page < kPageCount)
        {
            if (!g_pageTouched[page].load(std::memory_order_acquire))
            {
                ++page;
                continue;
            }
            const size_t runBegin = page;
            while (page + 1u < kPageCount &&
                   g_pageTouched[page + 1u].load(std::memory_order_acquire))
                ++page;
            const size_t runEnd = page;
            SetAliasPageProtection(base, runBegin, runEnd, PAGE_READWRITE);
            for (size_t i = runBegin; i <= runEnd; ++i)
            {
                g_pageState[i].store(0, std::memory_order_relaxed);
                g_pageTouched[i].store(0, std::memory_order_relaxed);
                g_pageGeneration[i].store(0, std::memory_order_relaxed);
            }
            ++page;
        }
    }
    if (g_vectoredHandler)
    {
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = nullptr;
    }
#endif
    g_guestBase.store(0, std::memory_order_release);
    g_rangeCache.clear();
    g_rangeCacheLru.clear();
}

} // namespace mojorecomp::gpu
