#pragma once

// Xbox 360 guest timebase.
//
// XenonRecomp lowers PowerPC `mftb` to __rdtsc(). A desktop TSC runs in the
// GHz range while Xenon's architectural timebase is 49.875 MHz. Leaving the
// raw host counter in translated code makes guest timeouts, frame pacing and
// streaming logic run at the wrong rate even when every other subsystem is
// correct.
//
// This header is force-included over ppc/*.cpp only. It intentionally includes
// the host intrinsic declarations before shadowing __rdtsc so system headers do
// not see a macro in place of the intrinsic declaration.

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

#include <cstdint>
#include <atomic>

inline constexpr uint64_t MOJORECOMP_TIMEBASE_HZ = 49'875'000ull;

namespace mojorecomp::timebase {

extern uint64_t host_hz;
extern bool deterministic;
extern uint64_t virtual_ticks;
extern std::atomic<bool> debug_clock_active;

uint64_t host_ticks();
bool Init();
void AdvanceFrame();
uint64_t DebugGuestTicks() noexcept;
void SetDebugClock(uint32_t numerator, uint32_t denominator, bool paused) noexcept;
void AdvanceDebugFrame(uint32_t fps) noexcept;

inline bool DebugClockActive() noexcept
{
    return debug_clock_active.load(std::memory_order_relaxed);
}

inline uint64_t GuestTicks() noexcept
{
    if (deterministic)
        return virtual_ticks;

    if (debug_clock_active.load(std::memory_order_relaxed))
        return DebugGuestTicks();

    // Init is a hard startup gate before guest execution. Avoid silently
    // producing nonsense if a future probe violates that ordering.
    if (host_hz == 0)
        return 0;

    return uint64_t((__uint128_t(host_ticks()) * MOJORECOMP_TIMEBASE_HZ) / host_hz);
}

} // namespace mojorecomp::timebase

#undef __rdtsc
#define __rdtsc() (mojorecomp::timebase::GuestTicks())
