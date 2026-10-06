#include <chrono>
#include <cstdio>

#include "../host/stutter_profiler.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using namespace std::chrono_literals;
    using mojorecomp::host::FrameWorkSample;
    using mojorecomp::host::StutterCause;
    using mojorecomp::host::StutterProfiler;

    StutterProfiler profiler(100ms);
    const StutterProfiler::Clock::time_point zero{};

    FrameWorkSample normal{};
    normal.frameIndex = 1;
    normal.presentNs = 1'000'000;
    if (profiler.Observe(zero, normal))
        return Fail("first observation should only establish a baseline");
    normal.frameIndex = 2;
    if (profiler.Observe(zero + 33ms, normal))
        return Fail("normal 30 FPS frame was reported as a hitch");

    FrameWorkSample pipeline{};
    pipeline.frameIndex = 3;
    pipeline.presentNs = 2'000'000;
    pipeline.drawNs = 95'000'000;
    pipeline.pipelineNs = 90'000'000;
    pipeline.pipelineCreates = 1;
    const auto pipelineReport = profiler.Observe(zero + 153ms, pipeline);
    if (!pipelineReport || pipelineReport->cause != StutterCause::Pipeline)
        return Fail("pipeline-heavy hitch was not classified as pipeline");

    FrameWorkSample fence{};
    fence.frameIndex = 4;
    fence.presentNs = 105'000'000;
    fence.fenceWaitNs = 95'000'000;
    const auto fenceReport = profiler.Observe(zero + 273ms, fence);
    if (!fenceReport || fenceReport->cause != StutterCause::FenceWait)
        return Fail("GPU fence hitch was not classified as a fence wait");

    profiler.Reset();
    FrameWorkSample queuePresent{};
    queuePresent.frameIndex = 5;
    queuePresent.presentNs = 110'000'000;
    queuePresent.queuePresentNs = 95'000'000;
    if (profiler.Observe(zero + 400ms, queuePresent))
        return Fail("reset profiler should establish a present baseline");
    queuePresent.frameIndex = 6;
    const auto queuePresentReport = profiler.Observe(zero + 520ms, queuePresent);
    if (!queuePresentReport || queuePresentReport->cause != StutterCause::QueuePresent)
        return Fail("queue-present hitch was not classified as queue present");

    profiler.Reset();
    FrameWorkSample pm4{};
    pm4.frameIndex = 7;
    pm4.presentNs = 1'000'000;
    pm4.pm4ExecuteNs = 110'000'000;
    pm4.pm4DrawSinkNs = 10'000'000;
    pm4.pm4ParserNs = 100'000'000;
    if (profiler.Observe(zero + 600ms, pm4))
        return Fail("reset profiler should establish a PM4 baseline");
    pm4.frameIndex = 8;
    const auto pm4Report = profiler.Observe(zero + 720ms, pm4);
    if (!pm4Report || pm4Report->cause != StutterCause::Pm4)
        return Fail("PM4-heavy hitch was not classified as PM4 parser work");

    profiler.Reset();
    FrameWorkSample outside{};
    outside.frameIndex = 9;
    outside.presentNs = 2'000'000;
    if (profiler.Observe(zero + 1s, outside))
        return Fail("reset profiler should establish a fresh baseline");
    const auto outsideReport = profiler.Observe(zero + 1200ms, outside);
    if (!outsideReport || outsideReport->cause != StutterCause::Unattributed ||
        outsideReport->unattributedOutsideNs < 190'000'000)
        return Fail("unattributed guest/PM4 hitch was not preserved");

    std::puts("PASS: stutter profiler ignores normal frames and classifies major hitch sources.");
    return 0;
}
