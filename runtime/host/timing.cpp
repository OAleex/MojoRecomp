#include "timing.h"

#include <cstdint>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mojorecomp::host {
namespace {

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

class ThreadWaitableTimer
{
public:
    ThreadWaitableTimer() noexcept
    {
        handle_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!handle_)
            handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ~ThreadWaitableTimer()
    {
        if (handle_)
            CloseHandle(handle_);
    }

    ThreadWaitableTimer(const ThreadWaitableTimer&) = delete;
    ThreadWaitableTimer& operator=(const ThreadWaitableTimer&) = delete;

    bool WaitFor(std::chrono::nanoseconds duration) noexcept
    {
        if (!handle_ || duration <= std::chrono::nanoseconds::zero())
            return false;

        const int64_t nanoseconds = duration.count();
        const int64_t hundredNanoseconds = (nanoseconds + 99) / 100;
        if (hundredNanoseconds <= 0)
            return false;

        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(hundredNanoseconds);
        if (!SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE))
            return false;

        return WaitForSingleObject(handle_, INFINITE) == WAIT_OBJECT_0;
    }

private:
    HANDLE handle_ = nullptr;
};

thread_local ThreadWaitableTimer g_waitableTimer;
#endif

} // namespace

void WaitUntil(SteadyClock::time_point deadline) noexcept
{
    const auto now = SteadyClock::now();
    if (deadline <= now)
        return;

    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);

#ifdef _WIN32
    if (g_waitableTimer.WaitFor(remaining))
        return;
#endif

    // Keep a small tail for scheduler jitter on platforms where the high
    // resolution timer is unavailable. This is a fallback path, not a spin loop
    // used during normal Windows gameplay.
    constexpr auto kYieldTail = std::chrono::milliseconds(1);
    if (remaining > kYieldTail)
        std::this_thread::sleep_for(remaining - kYieldTail);
    while (SteadyClock::now() < deadline)
        std::this_thread::yield();
}

void WaitFor(std::chrono::nanoseconds duration) noexcept
{
    if (duration <= std::chrono::nanoseconds::zero())
        return;
    WaitUntil(SteadyClock::now() + duration);
}

} // namespace mojorecomp::host
