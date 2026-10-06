#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace mojorecomp::host {

struct FramePacingReport
{
    uint64_t targetNs = 0;
    uint64_t meanNs = 0;
    uint64_t meanAbsoluteErrorNs = 0;
    uint64_t maxNs = 0;
    uint64_t lateFrames = 0;
    uint64_t samples = 0;
};

class FramePacingProfiler
{
public:
    using Clock = std::chrono::steady_clock;

    explicit FramePacingProfiler(uint64_t reportEvery = 120) noexcept;

    void Reset() noexcept;
    std::optional<FramePacingReport> Observe(
        Clock::time_point completion,
        std::chrono::nanoseconds targetPeriod) noexcept;

private:
    uint64_t reportEvery_ = 120;
    Clock::time_point previousCompletion_{};
    bool havePreviousCompletion_ = false;
    uint64_t samples_ = 0;
    uint64_t totalNs_ = 0;
    uint64_t absoluteErrorNs_ = 0;
    uint64_t maxNs_ = 0;
    uint64_t lateFrames_ = 0;
};

} // namespace mojorecomp::host
