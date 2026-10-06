#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace mojorecomp::host {

enum class FrameRateMode : uint8_t
{
    Compatibility30,
    HighFrameRate60,
};

struct FrameRatePolicy
{
    FrameRateMode mode = FrameRateMode::Compatibility30;
    uint32_t simulationHz = 30;
    uint32_t presentationHz = 30;
    uint32_t displayHz = 60;
    uint32_t guestVblankHz = 60;
    bool requiresTitlePatches = false;
};

struct TitleFrameGatePhase
{
    uint32_t carryUs = 0;
};

constexpr FrameRatePolicy PolicyFor(FrameRateMode mode) noexcept
{
    switch (mode)
    {
        case FrameRateMode::HighFrameRate60:
            return {FrameRateMode::HighFrameRate60, 60, 60, 60, 60, true};
        case FrameRateMode::Compatibility30:
        default:
            return {FrameRateMode::Compatibility30, 30, 30, 60, 60, false};
    }
}

constexpr FrameRateMode FrameRateModeForSetting(std::string_view value) noexcept
{
    return value == "60" ? FrameRateMode::HighFrameRate60
                         : FrameRateMode::Compatibility30;
}

// HFR remains opt-in while the title-side patch is validated.  This is a
// process-wide policy chosen once at startup so every subsystem (title hook,
// vblank, diagnostics and presentation) observes the same mode.  Unset/unknown
// values retain the proven 30 FPS compatibility path.
inline FrameRatePolicy ActiveFrameRatePolicy() noexcept
{
    static const FrameRatePolicy policy = [] {
        const char* setting = std::getenv("MOJORECOMP_FRAME_RATE");
        return PolicyFor(FrameRateModeForSetting(setting ? setting : ""));
    }();
    return policy;
}

// Pre-rendered video keeps its authored title cadence. The display and guest
// vblank clocks remain 60 Hz; only the title-side HFR patches are suspended
// while the command stream confirms that native-cadence video is being drawn.
// Keeping this as a pure policy transform makes the transition independently
// testable and avoids teaching the generic scheduler about any specific movie.
constexpr FrameRatePolicy TitlePolicyForNativeCadenceContent(
    FrameRatePolicy active, bool nativeCadenceContent) noexcept
{
    if (nativeCadenceContent && active.mode == FrameRateMode::HighFrameRate60 &&
        active.requiresTitlePatches)
        return PolicyFor(FrameRateMode::Compatibility30);
    return active;
}

constexpr uint32_t TitleFrameGateElapsedUs(uint32_t elapsedUs,
                                           FrameRatePolicy policy) noexcept
{
    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches)
        return elapsedUs;

    constexpr auto baseline = PolicyFor(FrameRateMode::Compatibility30);
    if (!baseline.presentationHz || policy.presentationHz <= baseline.presentationHz)
        return elapsedUs;

    const uint64_t scaled = uint64_t(elapsedUs) * policy.presentationHz /
                            baseline.presentationHz;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

// Preserve the exact long-term HFR cadence when the host scheduler resumes the
// title a little after the 16.667 ms boundary.  The title already passes the
// unmodified real delta to its update; this state affects only the gate test.
// Carry at most the sub-frame overshoot into the next gate, and deliberately
// drop phase after a missed whole frame so a hitch never causes catch-up bursts.
constexpr uint32_t TitleFrameGateElapsedUsPhased(uint32_t elapsedUs,
                                                 FrameRatePolicy policy,
                                                 TitleFrameGatePhase& phase) noexcept
{
    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches || !policy.presentationHz)
    {
        phase.carryUs = 0;
        return elapsedUs;
    }

    constexpr uint64_t kMicrosPerSecond = 1'000'000ull;
    const uint32_t targetUs = static_cast<uint32_t>(
        (kMicrosPerSecond + policy.presentationHz / 2u) / policy.presentationHz);
    const uint64_t effectiveUs = uint64_t(elapsedUs) + phase.carryUs;

    if (effectiveUs >= targetUs)
    {
        if (effectiveUs < uint64_t(targetUs) * 2u)
            phase.carryUs = static_cast<uint32_t>(effectiveUs - targetUs);
        else
            phase.carryUs = 0;
    }

    constexpr auto baseline = PolicyFor(FrameRateMode::Compatibility30);
    if (!baseline.presentationHz || policy.presentationHz <= baseline.presentationHz)
        return elapsedUs;

    const uint64_t scaled = effectiveUs * policy.presentationHz /
                            baseline.presentationHz;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

// The title's mode-1 progress synchronizer intentionally stores one tick ahead
// after consuming exactly one graphics-progress tick. With a 60 Hz graphics
// interrupt publisher, that makes the next call wait for two ticks and therefore
// reduces this route to ~30 Hz. The hook is placed only on that exact look-ahead
// path, immediately after the guest's +1 and before the store. In HFR mode we
// cancel only that look-ahead; compatibility mode preserves the original value.
constexpr uint32_t TitleProgressCadenceCounter(uint32_t lookaheadCounter,
                                               FrameRatePolicy policy) noexcept
{
    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches)
        return lookaheadCounter;

    return lookaheadCounter - 1u;
}

// The mode-1 scheduler already enforces its own 1/60 minimum step. Waiting for
// a *future* graphics-progress/vblank tick after that scheduler gate serializes
// two independent 60 Hz clocks and lowers the effective cadence. HFR therefore
// consumes the currently published progress value without blocking for a newer
// tick; compatibility mode retains the original wait loop exactly.
constexpr bool TitleProgressWaitBypass(FrameRatePolicy policy) noexcept
{
    return policy.mode == FrameRateMode::HighFrameRate60 &&
           policy.requiresTitlePatches;
}

// One title-side async state machine spins inside a single update while its
// worker reports status 4 (pending). On the original 30 FPS path that blocking
// poll is part of the observed cadence. Once the outer scheduler is released at
// 60 Hz, keeping the same busy-wait stalls the main thread for an entire
// 30-ish-Hz worker period and collapses presentation back to ~25-30 FPS.
//
// HFR mode therefore turns only the still-pending case into a cooperative poll:
// the state is left unchanged and the next title update polls again. Completed,
// failed and transitional states retain the original control flow. The 30 FPS
// compatibility policy is a strict no-op.
constexpr bool TitleAsyncPollShouldYield(uint32_t status,
                                         FrameRatePolicy policy) noexcept
{
    return policy.mode == FrameRateMode::HighFrameRate60 &&
           policy.requiresTitlePatches && status == 4u;
}

constexpr std::chrono::microseconds PeriodForHzRoundedUs(uint32_t hz) noexcept
{
    return hz == 0
        ? std::chrono::microseconds::zero()
        : std::chrono::microseconds((1'000'000ull + hz / 2u) / hz);
}

} // namespace mojorecomp::host
