#pragma once

#include <array>
#include <cstdint>

struct HostInputState;

namespace mojorecomp::debug {

struct DebugOverlaySnapshot
{
    bool enabled = false;
    bool showPerformance = false;
    bool fastForward = false;
    bool paused = false;
    double speed = 1.0;
    double fps = 0.0;
    double frameMs = 0.0;
    std::array<char, 96> notification{};
};

// Poll host-only debug hotkeys. Safe to call from the guest input path; edge
// detection and overlay snapshot publication are internally serialized.
void PollHotkeys();

// Merge short debug-generated controller pulses into the state seen by the
// title. Normal input is never removed or replaced.
void ApplyInput(HostInputState& state);

bool Enabled() noexcept;
void SetEnabled(bool enabled) noexcept;
bool ConsumeUnlockAllRequest() noexcept;
void ReportUnlockAllResult(bool alreadyUnlocked, uint32_t unlockedNow) noexcept;
bool ConsumeUnlockAllEpisodesRequest() noexcept;
void ReportUnlockAllEpisodesResult(bool alreadyUnlocked, uint32_t unlockedNow) noexcept;
bool FastForward() noexcept;
bool Paused() noexcept;
double Speed() noexcept;
bool ShowPerformance() noexcept;
DebugOverlaySnapshot GetOverlaySnapshot() noexcept;

// Used by the crash logger so F1 debug mode gets the extended crash payload
// even when MOJORECOMP_DEBUG_MODE was not supplied on the command line.
bool ExtendedCrashInfoEnabled() noexcept;

} // namespace mojorecomp::debug
