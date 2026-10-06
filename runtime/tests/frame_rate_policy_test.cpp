#include <chrono>
#include <cstdio>

#include "../host/frame_rate_policy.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using mojorecomp::host::ActiveFrameRatePolicy;
    using mojorecomp::host::FrameRateMode;
    using mojorecomp::host::FrameRateModeForSetting;
    using mojorecomp::host::PeriodForHzRoundedUs;
    using mojorecomp::host::PolicyFor;
    using mojorecomp::host::TitleFrameGateElapsedUs;
    using mojorecomp::host::TitleFrameGateElapsedUsPhased;
    using mojorecomp::host::TitleFrameGatePhase;
    using mojorecomp::host::TitleAsyncPollShouldYield;
    using mojorecomp::host::TitleProgressCadenceCounter;
    using mojorecomp::host::TitleProgressWaitBypass;
    using mojorecomp::host::TitlePolicyForNativeCadenceContent;

    const auto active = ActiveFrameRatePolicy();
    if (active.mode != FrameRateMode::Compatibility30 ||
        active.simulationHz != 30 || active.presentationHz != 30 ||
        active.displayHz != 60 || active.guestVblankHz != 60 ||
        active.requiresTitlePatches)
        return Fail("active policy is not the validated 30 FPS baseline");

    if (PeriodForHzRoundedUs(active.simulationHz) != std::chrono::microseconds(33333))
        return Fail("30 Hz simulation cadence changed");
    if (PeriodForHzRoundedUs(active.guestVblankHz) != std::chrono::microseconds(16667))
        return Fail("60 Hz guest vblank cadence changed");

    if (FrameRateModeForSetting("60") != FrameRateMode::HighFrameRate60 ||
        FrameRateModeForSetting("30") != FrameRateMode::Compatibility30 ||
        FrameRateModeForSetting("") != FrameRateMode::Compatibility30 ||
        FrameRateModeForSetting("invalid") != FrameRateMode::Compatibility30)
        return Fail("frame-rate setting parser lost safe fallback semantics");

    const auto high = PolicyFor(FrameRateMode::HighFrameRate60);
    if (high.simulationHz != 60 || high.presentationHz != 60 ||
        high.displayHz != 60 || high.guestVblankHz != 60 ||
        !high.requiresTitlePatches)
        return Fail("60 FPS policy lost the title-patch safety contract");

    const auto video = TitlePolicyForNativeCadenceContent(high, true);
    if (video.mode != FrameRateMode::Compatibility30 ||
        video.simulationHz != 30 || video.presentationHz != 30 ||
        video.displayHz != 60 || video.guestVblankHz != 60 ||
        video.requiresTitlePatches)
        return Fail("native-cadence content did not suspend only the HFR title policy");
    if (TitlePolicyForNativeCadenceContent(high, false).mode !=
            FrameRateMode::HighFrameRate60 ||
        TitlePolicyForNativeCadenceContent(active, true).mode !=
            FrameRateMode::Compatibility30)
        return Fail("native-cadence policy transform changed non-video behavior");

    if (TitleFrameGateElapsedUs(33333u, active) != 33333u)
        return Fail("30 FPS compatibility mode unexpectedly changes the title frame gate");
    if (TitleFrameGateElapsedUs(16666u, high) != 33332u ||
        TitleFrameGateElapsedUs(16667u, high) != 33334u)
        return Fail("60 FPS title frame gate no longer maps the 16.667 ms boundary exactly");

    TitleFrameGatePhase phase{};
    if (TitleFrameGateElapsedUsPhased(17000u, high, phase) != 34000u ||
        phase.carryUs != 333u)
        return Fail("HFR phase gate did not preserve a small scheduling overshoot");
    if (TitleFrameGateElapsedUsPhased(16333u, high, phase) != 33332u ||
        phase.carryUs != 333u)
        return Fail("HFR phase gate released one microsecond too early");
    if (TitleFrameGateElapsedUsPhased(16334u, high, phase) != 33334u ||
        phase.carryUs != 0u)
        return Fail("HFR phase gate did not repay the previous overshoot exactly");
    phase.carryUs = 500u;
    (void)TitleFrameGateElapsedUsPhased(40000u, high, phase);
    if (phase.carryUs != 0u)
        return Fail("HFR phase gate carried debt across a missed whole frame");
    phase.carryUs = 123u;
    if (TitleFrameGateElapsedUsPhased(33333u, active, phase) != 33333u ||
        phase.carryUs != 0u)
        return Fail("30 FPS compatibility mode retained HFR phase state");

    if (TitleProgressCadenceCounter(101u, active) != 101u)
        return Fail("30 FPS compatibility mode unexpectedly changes progress cadence");
    if (TitleProgressCadenceCounter(101u, high) != 100u ||
        TitleProgressCadenceCounter(0u, high) != 0xFFFFFFFFu)
        return Fail("60 FPS progress cadence no longer cancels exactly one look-ahead tick");
    if (TitleProgressWaitBypass(active) || !TitleProgressWaitBypass(high))
        return Fail("progress wait bypass lost its HFR-only safety contract");

    if (TitleAsyncPollShouldYield(4u, active))
        return Fail("30 FPS compatibility mode unexpectedly yields the async poll");
    if (!TitleAsyncPollShouldYield(4u, high) ||
        TitleAsyncPollShouldYield(0u, high) ||
        TitleAsyncPollShouldYield(1u, high) ||
        TitleAsyncPollShouldYield(3u, high))
        return Fail("60 FPS async poll yield no longer targets only pending status 4");

    std::puts("PASS: simulation, presentation and display/vblank rates are explicit and separate.");
    return 0;
}
