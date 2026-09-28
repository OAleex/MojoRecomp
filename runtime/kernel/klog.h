#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#define KLOG(...) std::fprintf(stderr, "[kernel] " __VA_ARGS__)

// Expensive tracing that is useful while diagnosing renderer or audio issues,
// but should not be on the normal gameplay hot path. stderr formatting and
// writes happen synchronously on the calling thread, so bursts of hundreds of
// trace lines can visibly perturb frame pacing. Keep the evidence available as
// an explicit opt-in rather than deleting it.
inline bool MojoRecompVerboseDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_VERBOSE_DIAGNOSTICS");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

#define KLOG_DIAG(...)                                                        \
    do                                                                        \
    {                                                                         \
        if (MojoRecompVerboseDiagnosticsEnabled())                              \
            KLOG(__VA_ARGS__);                                                \
    } while (0)

void MojoRecompKernelCallTrace(const char* name, uint64_t hit);

inline bool MojoRecompKernelCallTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_KCALL_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

// First-occurrence logging keeps boot traces useful without letting polling
// imports dominate output. Counting itself is opt-in because an atomic increment
// on every import becomes a material cost in titles that call synchronization
// imports tens of millions of times per second.
#define KCALL(sym)                                                                    \
    do                                                                                \
    {                                                                                 \
        if (MojoRecompKernelCallTraceEnabled())                                         \
        {                                                                             \
            static std::atomic<uint64_t> _mojoKcallCount{0};                         \
            const uint64_t _mojoKcallHit =                                            \
                _mojoKcallCount.fetch_add(1, std::memory_order_relaxed);              \
            if (_mojoKcallHit == 0 || (_mojoKcallHit & 0xFFFFFFu) == 0)             \
                MojoRecompKernelCallTrace(sym, _mojoKcallHit);                          \
        }                                                                             \
    } while (0)
