#include "debug_mode.h"
#include "debug_hotkey_sequence.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "cpu/timebase.h"
#include "audio/xaudio.h"
#include "gpu/pm4.h"
#include "host/frame_rate_policy.h"
#include "host/input.h"
#include "host/window.h"
#include "mojorecomp_version.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace mojorecomp::debug {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kFastForwardSpeed = 4.0;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_unlockAllRequested{false};
std::atomic<bool> g_unlockAllEpisodesRequested{false};
std::atomic<bool> g_fastForward{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_showPerformance{false};

std::mutex g_pollMutex;
Clock::time_point g_nextPoll{};
Clock::time_point g_perfEpoch{};
DebugActivationSequence g_debugActivationSequence{};
uint64_t g_perfFrame = 0;
std::atomic<double> g_fps{0.0};
std::atomic<double> g_frameMs{0.0};

enum class Notification : uint8_t
{
    None,
    UpgradesUnlocked,
    UpgradesAlreadyUnlocked,
    UnlockAllFailed,
    EpisodesUnlocked,
    EpisodesAlreadyUnlocked,
    UnlockEpisodesFailed,
    FastForwardEnabled,
    FastForwardDisabled,
    Paused,
    Resumed,
    FrameAdvanced,
};

std::atomic<Notification> g_notification{Notification::None};
std::atomic<int64_t> g_notificationUntilMs{0};

const char* NotificationText(Notification notification) noexcept
{
    switch (notification)
    {
        case Notification::UpgradesUnlocked: return "All upgrades unlocked";
        case Notification::UpgradesAlreadyUnlocked: return "All upgrades already unlocked";
        case Notification::UnlockAllFailed: return "Unlock All failed";
        case Notification::EpisodesUnlocked: return "All episodes unlocked";
        case Notification::EpisodesAlreadyUnlocked: return "All episodes already unlocked";
        case Notification::UnlockEpisodesFailed: return "Unlock All Episodes failed";
        case Notification::FastForwardEnabled: return "Fast-forward enabled";
        case Notification::FastForwardDisabled: return "Fast-forward disabled";
        case Notification::Paused: return "Paused";
        case Notification::Resumed: return "Resumed";
        case Notification::FrameAdvanced: return "Frame advanced";
        default: return "";
    }
}

int64_t ClockMs(Clock::time_point now = Clock::now()) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch()).count();
}

void SetNotification(Notification notification, int64_t lifetimeMs = 2500) noexcept
{
    g_notification.store(notification, std::memory_order_relaxed);
    g_notificationUntilMs.store(ClockMs() + lifetimeMs, std::memory_order_release);
}

struct KeyEdge
{
    int vk = 0;
    bool down = false;
};

std::array<KeyEdge, 8> g_keys{{
    {VK_F1, false}, {VK_F2, false}, {VK_F3, false},
    {VK_F4, false}, {VK_F6, false}, {VK_F7, false},
    {VK_F9, false}, {VK_F10, false},
}};

bool Pressed(int vk)
{
    for (auto& key : g_keys)
    {
        if (key.vk != vk)
            continue;
        const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = now && !key.down;
        key.down = now;
        return edge;
    }
    return false;
}

void ConfigureClock()
{
    const bool paused = g_paused.load(std::memory_order_relaxed);
    if (g_fastForward.load(std::memory_order_relaxed))
        mojorecomp::timebase::SetDebugClock(4, 1, paused);
    else
        mojorecomp::timebase::SetDebugClock(1, 1, paused);
}

void DisableDebugClock()
{
    g_fastForward.store(false, std::memory_order_relaxed);
    g_paused.store(false, std::memory_order_relaxed);
    mojorecomp::timebase::SetDebugClock(1, 1, false);
    mojorecomp::audio::SetDebugPaused(false);
}

void LogState(const char* action)
{
    std::fprintf(stderr,
                 "[debug] %s enabled=%u fast=%u paused=%u speed=%.2fx perf=%u\n",
                 action,
                 g_enabled.load(std::memory_order_relaxed) ? 1u : 0u,
                 g_fastForward.load(std::memory_order_relaxed) ? 1u : 0u,
                 g_paused.load(std::memory_order_relaxed) ? 1u : 0u,
                 Speed(),
                 g_showPerformance.load(std::memory_order_relaxed) ? 1u : 0u);
}

void UpdatePerformance(Clock::time_point now)
{
    const uint64_t frame = Pm4_FrameCount();
    if (g_perfEpoch.time_since_epoch().count() == 0)
    {
        g_perfEpoch = now;
        g_perfFrame = frame;
        return;
    }
    const double seconds = std::chrono::duration<double>(now - g_perfEpoch).count();
    if (seconds < 0.50)
        return;
    const uint64_t frames = frame - g_perfFrame;
    const double fps = seconds > 0.0 ? double(frames) / seconds : 0.0;
    g_fps.store(fps, std::memory_order_relaxed);
    g_frameMs.store(fps > 0.001 ? 1000.0 / fps : 0.0,
                    std::memory_order_relaxed);
    g_perfEpoch = now;
    g_perfFrame = frame;
}

} // namespace

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) noexcept
{
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool ConsumeUnlockAllRequest() noexcept
{
    return g_unlockAllRequested.exchange(false, std::memory_order_acq_rel);
}

void ReportUnlockAllResult(bool alreadyUnlocked, uint32_t unlockedNow) noexcept
{
    if (alreadyUnlocked)
    {
        std::fprintf(stderr, "[debug] All upgrades already unlocked\n");
        SetNotification(Notification::UpgradesAlreadyUnlocked, 3500);
    }
    else if (unlockedNow)
    {
        std::fprintf(stderr, "[debug] All upgrades unlocked (%u newly unlocked)\n",
                     unlockedNow);
        SetNotification(Notification::UpgradesUnlocked, 3500);
    }
    else
    {
        std::fprintf(stderr, "[debug] Unlock All failed\n");
        SetNotification(Notification::UnlockAllFailed, 3500);
    }
}

bool ConsumeUnlockAllEpisodesRequest() noexcept
{
    return g_unlockAllEpisodesRequested.exchange(false, std::memory_order_acq_rel);
}

void ReportUnlockAllEpisodesResult(bool alreadyUnlocked, uint32_t unlockedNow) noexcept
{
    if (alreadyUnlocked)
    {
        std::fprintf(stderr, "[debug] All episodes already unlocked\n");
        SetNotification(Notification::EpisodesAlreadyUnlocked, 3500);
    }
    else if (unlockedNow)
    {
        std::fprintf(stderr, "[debug] All episodes unlocked (%u newly unlocked)\n",
                     unlockedNow);
        SetNotification(Notification::EpisodesUnlocked, 3500);
    }
    else
    {
        std::fprintf(stderr, "[debug] Unlock All Episodes failed\n");
        SetNotification(Notification::UnlockEpisodesFailed, 3500);
    }
}

bool FastForward() noexcept
{
    return g_fastForward.load(std::memory_order_relaxed);
}

bool Paused() noexcept
{
    return g_paused.load(std::memory_order_relaxed);
}

double Speed() noexcept
{
    return g_fastForward.load(std::memory_order_relaxed) ? kFastForwardSpeed : 1.0;
}

bool ShowPerformance() noexcept
{
    return g_showPerformance.load(std::memory_order_relaxed);
}

DebugOverlaySnapshot GetOverlaySnapshot() noexcept
{
    DebugOverlaySnapshot snapshot{};
    snapshot.enabled = g_enabled.load(std::memory_order_relaxed);
    snapshot.showPerformance = g_showPerformance.load(std::memory_order_relaxed);
    snapshot.fastForward = g_fastForward.load(std::memory_order_relaxed);
    snapshot.paused = g_paused.load(std::memory_order_relaxed);
    snapshot.speed = snapshot.fastForward ? kFastForwardSpeed : 1.0;
    snapshot.fps = g_fps.load(std::memory_order_relaxed);
    snapshot.frameMs = g_frameMs.load(std::memory_order_relaxed);
    std::snprintf(snapshot.runtimeLabel.data(), snapshot.runtimeLabel.size(),
                  "Crash of the Titans - %s Runtime", mojorecomp::version::kCotRuntime);
    if (ClockMs() <= g_notificationUntilMs.load(std::memory_order_acquire))
    {
        std::snprintf(snapshot.notification.data(), snapshot.notification.size(), "%s",
                      NotificationText(g_notification.load(std::memory_order_relaxed)));
    }
    return snapshot;
}

bool ExtendedCrashInfoEnabled() noexcept
{
    if (Enabled())
        return true;
    const char* value = std::getenv("MOJORECOMP_DEBUG_MODE");
    return value && *value && std::strtoul(value, nullptr, 0) != 0;
}

void PollHotkeys()
{
    std::lock_guard lock(g_pollMutex);
    const auto now = Clock::now();
    if (now < g_nextPoll)
        return;
    g_nextPoll = now + std::chrono::milliseconds(5);

    UpdatePerformance(now);

    // Debug keys are local to the game. Do not let F-keys typed in another
    // application mutate a running title in the background.
    const HWND gameWindow = reinterpret_cast<HWND>(HostWindow_NativeHandle());
    if (gameWindow && GetForegroundWindow() != gameWindow)
        return;

    if (Pressed(VK_F9))
    {
        g_showPerformance.store(!g_showPerformance.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        LogState("perf-toggle");
    }

    if (Pressed(VK_F1))
    {
        if (RegisterDebugActivationPress(g_debugActivationSequence, ClockMs(now)))
        {
            const bool enabled = !g_enabled.load(std::memory_order_relaxed);
            g_enabled.store(enabled, std::memory_order_relaxed);
            if (!enabled)
            {
                g_unlockAllRequested.store(false, std::memory_order_relaxed);
                g_unlockAllEpisodesRequested.store(false, std::memory_order_relaxed);
                g_notification.store(Notification::None, std::memory_order_relaxed);
                g_notificationUntilMs.store(0, std::memory_order_relaxed);
                DisableDebugClock();
            }
            else
            {
                ConfigureClock();
                std::fprintf(stderr,
                             "[debug] hotkeys: F1 x10 mode | F2 unlock all | F3 fast-forward 4x | "
                             "F4 unlock all episodes | F6 pause | F7 step | F9 perf\n");
            }
            LogState(enabled ? "mode-on" : "mode-off");
        }
    }

    if (g_enabled.load(std::memory_order_relaxed))
    {
        if (Pressed(VK_F2))
        {
            // One-way Unlock All. The guest-side handler consumes this request
            // on the active PPC thread and routes each missing move through the
            // title's native DO_UnlockMove implementation. Pressing F2 again
            // never re-locks progression.
            g_unlockAllRequested.store(true, std::memory_order_release);
            LogState("unlock-all-request");
        }
        if (Pressed(VK_F3))
        {
            // Emulator-style turbo: accelerate the guest clock globally. The
            // display/vblank pump observes this flag too, so fixed-step gameplay
            // and cinematics both advance faster instead of only changing an
            // animation timer or injecting a scene-specific input event.
            const bool enabled = !g_fastForward.load(std::memory_order_relaxed);
            g_fastForward.store(enabled, std::memory_order_relaxed);
            ConfigureClock();
            SetNotification(enabled ? Notification::FastForwardEnabled
                                    : Notification::FastForwardDisabled);
            LogState(enabled ? "fast-forward-on" : "fast-forward-off");
        }
        if (Pressed(VK_F4))
        {
            // One-way Unlock All Episodes. The guest-side handler resolves the
            // title's own 20 timeline level IDs and fills only missing entries in
            // the persistent LEVEL_COMPLETE/unlocked list. Pressing F4 again
            // never re-locks anything.
            g_unlockAllEpisodesRequested.store(true, std::memory_order_release);
            LogState("unlock-all-episodes-request");
        }
        if (Pressed(VK_F6))
        {
            const bool paused = !g_paused.load(std::memory_order_relaxed);
            g_paused.store(paused, std::memory_order_relaxed);
            // Pause the physical audio endpoint and the guest/XMA audio workers
            // together with the guest clock. Otherwise cutscene dialogue keeps
            // consuming samples while video time is frozen and the title must
            // catch up to the audio master clock after resume.
            mojorecomp::audio::SetDebugPaused(paused);
            ConfigureClock();
            SetNotification(paused ? Notification::Paused : Notification::Resumed);
            LogState("pause-toggle");
        }
        if (Pressed(VK_F7) && g_paused.load(std::memory_order_relaxed))
        {
            mojorecomp::timebase::AdvanceDebugFrame(
                mojorecomp::host::ActiveFrameRatePolicy().simulationHz);
            SetNotification(Notification::FrameAdvanced, 1200);
            std::fprintf(stderr, "[debug] frame-step 1/%us\n",
                         mojorecomp::host::ActiveFrameRatePolicy().simulationHz);
        }
        (void)Pressed(VK_F10); // Reserved for a verified restart-level hook.
    }
}

void ApplyInput(HostInputState& state)
{
    // No debug hotkey currently injects controller buttons. Keep the hook as a
    // no-op for future verified features without exposing the removed F5 path.
    (void)state;
}

} // namespace mojorecomp::debug
