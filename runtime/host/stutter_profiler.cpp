#include "stutter_profiler.h"

#include <algorithm>
#include <array>

namespace mojorecomp::host {
namespace {

struct CauseCandidate
{
    StutterCause cause;
    uint64_t ns;
};

} // namespace

StutterProfiler::StutterProfiler(std::chrono::nanoseconds threshold) noexcept
    : threshold_(std::max(threshold, std::chrono::nanoseconds::zero()))
{
}

void StutterProfiler::SetThreshold(std::chrono::nanoseconds threshold) noexcept
{
    threshold_ = std::max(threshold, std::chrono::nanoseconds::zero());
}

std::chrono::nanoseconds StutterProfiler::Threshold() const noexcept
{
    return threshold_;
}

void StutterProfiler::Reset() noexcept
{
    previousCompletion_ = {};
    havePreviousCompletion_ = false;
}

std::optional<StutterReport> StutterProfiler::Observe(
    Clock::time_point completion,
    const FrameWorkSample& work) noexcept
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

    const uint64_t frameNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    if (frameNs < static_cast<uint64_t>(threshold_.count()))
        return std::nullopt;

    StutterReport report{};
    report.work = work;
    report.frameNs = frameNs;
    report.outsidePresenterNs =
        frameNs > work.presentNs ? frameNs - work.presentNs : 0;
    const uint64_t presentKnownNs =
        work.fenceWaitNs + work.acquireNs + work.presentPrepNs +
        work.queueSubmitNs + work.queuePresentNs;
    report.presenterExclusiveNs =
        work.presentNs > presentKnownNs ? work.presentNs - presentKnownNs : 0;

    const uint64_t knownDrawNs =
        work.attachmentNs + work.textureNs + work.pipelineNs +
        work.constantsNs + work.vertexNs;
    const uint64_t drawOtherNs =
        work.drawNs > knownDrawNs ? work.drawNs - knownDrawNs : 0;
    const uint64_t knownOutsideNs = work.drawNs + work.resolveNs + work.pm4ParserNs;
    report.unattributedOutsideNs =
        report.outsidePresenterNs > knownOutsideNs
            ? report.outsidePresenterNs - knownOutsideNs
            : 0;

    const std::array<CauseCandidate, 15> candidates{{
        {StutterCause::Unattributed, report.unattributedOutsideNs},
        {StutterCause::Pipeline, work.pipelineNs},
        {StutterCause::Texture, work.textureNs},
        {StutterCause::FenceWait, work.fenceWaitNs},
        {StutterCause::Present, report.presenterExclusiveNs},
        {StutterCause::Acquire, work.acquireNs},
        {StutterCause::PresentPrep, work.presentPrepNs},
        {StutterCause::QueueSubmit, work.queueSubmitNs},
        {StutterCause::QueuePresent, work.queuePresentNs},
        {StutterCause::Pm4, work.pm4ParserNs},
        {StutterCause::Draw, drawOtherNs},
        {StutterCause::Resolve, work.resolveNs},
        {StutterCause::Attachment, work.attachmentNs},
        {StutterCause::Constants, work.constantsNs},
        {StutterCause::Vertex, work.vertexNs},
    }};

    const auto dominant = std::max_element(
        candidates.begin(), candidates.end(),
        [](const CauseCandidate& a, const CauseCandidate& b) {
            return a.ns < b.ns;
        });
    report.cause = dominant != candidates.end()
        ? dominant->cause
        : StutterCause::Unattributed;
    return report;
}

const char* StutterCauseName(StutterCause cause) noexcept
{
    switch (cause)
    {
        case StutterCause::Pipeline: return "pipeline";
        case StutterCause::Texture: return "texture";
        case StutterCause::FenceWait: return "gpu-fence";
        case StutterCause::Present: return "present-other";
        case StutterCause::Acquire: return "present-acquire";
        case StutterCause::PresentPrep: return "present-prep";
        case StutterCause::QueueSubmit: return "queue-submit";
        case StutterCause::QueuePresent: return "queue-present";
        case StutterCause::Pm4: return "pm4-parser";
        case StutterCause::Draw: return "draw-other";
        case StutterCause::Resolve: return "resolve";
        case StutterCause::Attachment: return "attachment";
        case StutterCause::Constants: return "constants";
        case StutterCause::Vertex: return "vertex";
        case StutterCause::Unattributed:
        default:
            return "guest-pm4-or-other";
    }
}

} // namespace mojorecomp::host
