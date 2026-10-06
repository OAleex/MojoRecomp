#include <chrono>
#include <cstdio>

#include "../host/presentation_metrics.h"

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
    using mojorecomp::host::FramePacingProfiler;

    FramePacingProfiler profiler(3);
    const FramePacingProfiler::Clock::time_point zero{};
    constexpr auto target = 33ms;

    if (profiler.Observe(zero, target))
        return Fail("first observation must only establish a baseline");
    if (profiler.Observe(zero + 33ms, target))
        return Fail("report emitted before requested sample count");
    if (profiler.Observe(zero + 66ms, target))
        return Fail("report emitted before requested sample count");
    const auto report = profiler.Observe(zero + 132ms, target);
    if (!report)
        return Fail("report was not emitted at requested sample count");
    if (report->samples != 3 || report->lateFrames != 1)
        return Fail("sample or late-frame accounting is incorrect");
    if (report->maxNs != 66'000'000ull)
        return Fail("maximum frame interval is incorrect");

    profiler.Reset();
    if (profiler.Observe(zero + 1s, target))
        return Fail("reset must establish a fresh baseline");

    std::puts("PASS: frame pacing profiler batches cadence metrics correctly.");
    return 0;
}
