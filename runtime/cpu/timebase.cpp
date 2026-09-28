#include "timebase.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace mojorecomp::timebase {

uint64_t host_hz = 0;
bool deterministic = false;
uint64_t virtual_ticks = 0;
std::atomic<bool> debug_clock_active{false};

namespace {
std::mutex g_debugClockMutex;
std::atomic<uint64_t> g_debugAnchorRaw{0};
std::atomic<uint64_t> g_debugAnchorGuest{0};
std::atomic<uint32_t> g_debugScaleNumerator{1};
std::atomic<uint32_t> g_debugScaleDenominator{1};
std::atomic<bool> g_debugPaused{false};

uint64_t RawGuestTicks() noexcept
{
    if (!host_hz)
        return 0;
    return uint64_t((__uint128_t(host_ticks()) * MOJORECOMP_TIMEBASE_HZ) / host_hz);
}

uint64_t ComputeDebugTicks(uint64_t raw) noexcept
{
    const uint64_t anchorRaw = g_debugAnchorRaw.load(std::memory_order_relaxed);
    const uint64_t anchorGuest = g_debugAnchorGuest.load(std::memory_order_relaxed);
    if (g_debugPaused.load(std::memory_order_relaxed) || raw <= anchorRaw)
        return anchorGuest;
    const uint32_t numerator = g_debugScaleNumerator.load(std::memory_order_relaxed);
    const uint32_t denominator = g_debugScaleDenominator.load(std::memory_order_relaxed);
    return anchorGuest + uint64_t((__uint128_t(raw - anchorRaw) * numerator) /
                                  (denominator ? denominator : 1u));
}
} // namespace

uint64_t host_ticks()
{
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t value = 0;
#if defined(_MSC_VER)
    value = _ReadStatusReg(ARM64_CNTVCT);
#else
    asm volatile("mrs %0, cntvct_el0" : "=r"(value)::"memory");
#endif
    return value;
#else
#error "MojoRecomp needs a monotonic host counter for this architecture"
#endif
}

static uint64_t Calibrate()
{
    using clock = std::chrono::steady_clock;

    // Measure long enough that scheduler jitter is a small fraction of the
    // sample, but keep startup effectively instant.
    const auto wall0 = clock::now();
    const uint64_t tick0 = host_ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const uint64_t tick1 = host_ticks();
    const auto wall1 = clock::now();

    const uint64_t ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count());
    if (ns == 0 || tick1 <= tick0)
        return 0;

    return uint64_t((__uint128_t(tick1 - tick0) * 1'000'000'000ull) / ns);
}

bool Init()
{
    host_hz = Calibrate();
    if (!host_hz)
        return false;

    deterministic = std::getenv("MOJORECOMP_DETERMINISTIC_CLOCK") != nullptr;
    if (deterministic)
    {
        // Useful only for renderer/debug comparisons. Never a normal gameplay
        // default: it deliberately changes the clock observed by the title.
        virtual_ticks = MOJORECOMP_TIMEBASE_HZ * 10;
        std::fprintf(stderr,
                     "[timebase] deterministic diagnostic clock enabled; "
                     "do not use for performance measurements\n");
    }
    return true;
}

void AdvanceFrame()
{
    if (deterministic)
        virtual_ticks += MOJORECOMP_TIMEBASE_HZ / 60;
}

uint64_t DebugGuestTicks() noexcept
{
    return ComputeDebugTicks(RawGuestTicks());
}

void SetDebugClock(uint32_t numerator, uint32_t denominator, bool paused) noexcept
{
    if (!numerator)
        numerator = 1;
    if (!denominator)
        denominator = 1;

    std::lock_guard lock(g_debugClockMutex);
    const uint64_t raw = RawGuestTicks();
    uint64_t current = raw;
    if (debug_clock_active.load(std::memory_order_relaxed))
        current = ComputeDebugTicks(raw);

    g_debugAnchorRaw.store(raw, std::memory_order_relaxed);
    g_debugAnchorGuest.store(current, std::memory_order_relaxed);
    g_debugScaleNumerator.store(numerator, std::memory_order_relaxed);
    g_debugScaleDenominator.store(denominator, std::memory_order_relaxed);
    g_debugPaused.store(paused, std::memory_order_relaxed);
    debug_clock_active.store(true, std::memory_order_release);
}

void AdvanceDebugFrame(uint32_t fps) noexcept
{
    if (!fps)
        fps = 30;
    std::lock_guard lock(g_debugClockMutex);
    if (!debug_clock_active.load(std::memory_order_relaxed) ||
        !g_debugPaused.load(std::memory_order_relaxed))
        return;
    g_debugAnchorGuest.fetch_add(MOJORECOMP_TIMEBASE_HZ / fps,
                                 std::memory_order_relaxed);
}

} // namespace mojorecomp::timebase
