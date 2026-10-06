#include <chrono>
#include <cstdio>

#include "../host/timing.h"

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
    using mojorecomp::host::PeriodicDeadline;

    PeriodicDeadline timer;
    const PeriodicDeadline::TimePoint zero{};

    if (timer.Active())
        return Fail("new periodic deadline should be inactive");

    timer.Reset(zero, 16ms);
    if (!timer.Active() || timer.Period() != 16ms || timer.Deadline() != zero + 16ms)
        return Fail("periodic deadline reset produced the wrong first deadline");

    timer.Advance();
    if (timer.Deadline() != zero + 32ms)
        return Fail("periodic deadline did not advance by one period");

    const uint64_t skipped = timer.SkipPast(zero + 81ms);
    if (skipped != 4 || timer.Deadline() != zero + 96ms)
        return Fail("periodic deadline catch-up arithmetic is incorrect");

    timer.RebaseDeadline(zero + 100ms);
    timer.Advance();
    if (timer.Deadline() != zero + 116ms)
        return Fail("periodic deadline rebase did not preserve its period");

    timer.Clear();
    if (timer.Active() || timer.SkipPast(zero + 1s) != 0)
        return Fail("cleared periodic deadline should stay inactive");

    std::puts("PASS: periodic deadline reset, advance, catch-up and rebase are deterministic.");
    return 0;
}
