#include "presentation_metrics.h"

#include <algorithm>

namespace mojorecomp::host {

FramePacingProfiler::FramePacingProfiler(uint64_t reportEvery) noexcept
    : reportEvery_(std::max<uint64_t>(1, reportEvery))
{
}

void FramePacingProfiler::Reset() noexcept
{
    previousCompletion_ = {};
    havePreviousCompletion_ = false;
    samples_ = 0;
    totalNs_ = 0;
    absoluteErrorNs_ = 0;
    maxNs_ = 0;
    lateFrames_ = 0;
}

std::optional<FramePacingReport> FramePacingProfiler::Observe(
    Clock::time_point completion,
    std::chrono::nanoseconds targetPeriod) noexcept
{
    if (!havePreviousCompletion_)
    {
        previousCompletion_ = completion;
        havePreviousCompletion_ = true;
        return std::nullopt;
    }

    const auto elapsed = completion - previousCompletion_;
    previousCompletion_ = completion;
    if (elapsed <= Clock::duration::zero())
        return std::nullopt;

    const uint64_t intervalNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const uint64_t targetNs = targetPeriod.count() > 0
        ? static_cast<uint64_t>(targetPeriod.count())
        : 0;

    totalNs_ += intervalNs;
    maxNs_ = std::max(maxNs_, intervalNs);
    absoluteErrorNs_ += intervalNs > targetNs
        ? intervalNs - targetNs
        : targetNs - intervalNs;
    if (targetNs && intervalNs > targetNs + targetNs / 2u)
        ++lateFrames_;
    ++samples_;

    if (samples_ < reportEvery_)
        return std::nullopt;

    FramePacingReport report{};
    report.targetNs = targetNs;
    report.meanNs = totalNs_ / samples_;
    report.meanAbsoluteErrorNs = absoluteErrorNs_ / samples_;
    report.maxNs = maxNs_;
    report.lateFrames = lateFrames_;
    report.samples = samples_;

    samples_ = 0;
    totalNs_ = 0;
    absoluteErrorNs_ = 0;
    maxNs_ = 0;
    lateFrames_ = 0;
    return report;
}

} // namespace mojorecomp::host
