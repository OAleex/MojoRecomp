// COT-specific hooks for native context switching and XMA buffer filling. Guest
// code remains responsible for saving and restoring its register state.

#include "ppc_recomp_shared.h"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <immintrin.h>

#include "runtime_state.h"
#include "cpu/guest_fiber.h"
#include "kernel/guestcall.h"

namespace {

struct DecoderFillState {
    uint32_t lastProgress = 0xFFFFFFFFu;
    uint32_t stallRetries = 0;
};

thread_local DecoderFillState g_decoderFillState;

bool NativeFibersEnabled() {
    const char* value = std::getenv("MOJORECOMP_NATIVE_FIBERS");
    // Crash of the Titans relies on these context-switch hooks during normal
    // frontend/game boot. Keeping native fibers opt-in leaves the guest alive
    // but stalls it before the real asset-loading path (black presentation with
    // only the tiny fallback shader set). Native fibers are therefore the
    // production default; setting MOJORECOMP_NATIVE_FIBERS=0 is retained only for
    // explicit regression/diagnostic comparisons with the old fallback path.
    return !value || !*value || value[0] != '0';
}

bool ProgressDiagnosticsEnabled() {
    const char* value = std::getenv("MOJORECOMP_PROGRESS_DIAGNOSTICS");
    return value && value[0] == '1';
}

uint32_t GpuPollYieldInterval() {
    static const uint32_t interval = [] {
        const char* value = std::getenv("MOJORECOMP_GPU_POLL_YIELD");
        if (!value || !*value || value[0] == '0')
            return 0u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value)
            return 1u;
        return static_cast<uint32_t>(parsed ? parsed : 1u);
    }();
    return interval;
}
uint32_t GpuPollDelayUs() {
    static const uint32_t delayUs = [] {
        const char* value = std::getenv("MOJORECOMP_GPU_POLL_DELAY_US");
        if (!value || !*value || value[0] == '0')
            return 0u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value)
            return 10u;
        return static_cast<uint32_t>(parsed);
    }();
    return delayUs;
}

void GpuPollBusyDelay(uint32_t microseconds) {
    if (!microseconds)
        return;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(microseconds);
    do {
        // Keep the host core in a polite spin instead of repeatedly re-reading
        // the CP writeback cache line. This mirrors Xenon's db16cyc intent while
        // allowing the PM4 worker to publish progress without cache-line ping-pong.
        for (int i = 0; i < 32; ++i)
            _mm_pause();
    } while (std::chrono::steady_clock::now() < deadline);
}

uint32_t GuestLoadU32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, g_mojoGuestBase + address, sizeof(value));
    return _byteswap_ulong(value);
}

uint8_t GuestLoadU8(uint32_t address) {
    return *(g_mojoGuestBase + address);
}

} // namespace

void MojoRecompBeginContextSwitch(PPCRegister& target, PPCRegister& pcr) {
    if (NativeFibersEnabled())
        mojorecomp::fiber::Begin(*g_ppcContext, target.u32, pcr.u32);
}

void MojoRecompProgressProbe(PPCRegister& stateObject) {
    if (!ProgressDiagnosticsEnabled() || !g_mojoGuestBase || !stateObject.u32)
        return;

    const uint32_t p = stateObject.u32;
    const uint32_t phase = GuestLoadU32(p + 1652);
    const uint32_t mode = GuestLoadU32(p + 1660);
    const uint32_t published = GuestLoadU32(p + 1664);
    const uint32_t lastCounter = GuestLoadU32(p + 1668);
    const uint32_t renderer = GuestLoadU32(p + 16);
    const uint32_t drawSerial = renderer ? GuestLoadU32(renderer + 16408) : 0;

    struct Snapshot {
        uint32_t phase = 0xFFFFFFFFu;
        uint32_t mode = 0xFFFFFFFFu;
        uint32_t published = 0xFFFFFFFFu;
        uint32_t lastCounter = 0xFFFFFFFFu;
        uint32_t renderer = 0xFFFFFFFFu;
        uint32_t drawSerial = 0xFFFFFFFFu;
        uint64_t calls = 0;
    };
    static thread_local Snapshot last;
    ++last.calls;

    const bool changed = phase != last.phase || mode != last.mode ||
                         published != last.published || lastCounter != last.lastCounter ||
                         renderer != last.renderer || drawSerial != last.drawSerial;
    if (changed || last.calls <= 16 || (last.calls % 120u) == 0) {
        std::fprintf(stderr,
            "[progress] update=%llu obj=%08X phase=%u mode=%u published=%u counter=%u "
            "renderer=%08X drawSerial=%u\n",
            static_cast<unsigned long long>(last.calls), p, phase, mode, published,
            lastCounter, renderer, drawSerial);
    }

    last.phase = phase;
    last.mode = mode;
    last.published = published;
    last.lastCounter = lastCounter;
    last.renderer = renderer;
    last.drawSerial = drawSerial;
}

void MojoRecompGpuPollProbe(PPCRegister& pollObject, PPCRegister& owner) {
    if (!g_mojoGuestBase || !pollObject.u32 || !owner.u32)
        return;

    const uint32_t object = pollObject.u32;
    const uint32_t state = owner.u32;
    const uint32_t statusFlags = GuestLoadU8(state + 10813);
    const uint32_t currentPtrSlot = GuestLoadU32(state + 10768);
    const uint32_t currentPtr = currentPtrSlot ? GuestLoadU32(currentPtrSlot) : 0;
    const uint32_t observedPtr = GuestLoadU32(object + 8);
    const uint32_t startTick = GuestLoadU32(object + 12);

    // The original Xenon routine executes eight db16cyc instructions before
    // polling the CP read pointer. XenonRecomp currently emits those as comments,
    // which turns this into a full-speed host spin loop. When explicitly enabled,
    // periodically yield only while the CP pointer has not advanced. Guest-visible
    // state and the routine's return value remain entirely controlled by the
    // translated code after this hook returns.
    const uint32_t yieldInterval = GpuPollYieldInterval();
    const uint32_t delayUs = GpuPollDelayUs();
    const bool waitingOnCp = !(statusFlags & 0x2u) && currentPtrSlot && currentPtr == observedPtr;
    if (delayUs && waitingOnCp)
        GpuPollBusyDelay(delayUs);
    if (yieldInterval && waitingOnCp) {
        struct BackoffState {
            uint32_t object = 0;
            uint32_t observed = 0;
            uint64_t unchangedPolls = 0;
        };
        static thread_local BackoffState backoff;
        if (backoff.object != object || backoff.observed != observedPtr) {
            backoff.object = object;
            backoff.observed = observedPtr;
            backoff.unchangedPolls = 1;
        } else {
            ++backoff.unchangedPolls;
        }
        if ((backoff.unchangedPolls % yieldInterval) == 0)
            std::this_thread::yield();
    }

    if (!ProgressDiagnosticsEnabled())
        return;

    static thread_local uint64_t calls = 0;
    ++calls;
    if (calls <= 16 || (calls % 256u) == 0 || currentPtr != observedPtr) {
        std::fprintf(stderr,
            "[progress] gpu-poll=%llu poll=%08X owner=%08X flags=%02X "
            "observed=%08X current=%08X startTick=%u\n",
            static_cast<unsigned long long>(calls), object, state, statusFlags,
            observedPtr, currentPtr, startTick);
    }
}

void MojoRecompCommitContextSwitch(PPCRegister& sp, PPCRegister& thread,
                                 PPCRegister& allocationBase, PPCRegister& stackBase,
                                 PPCRegister& stackLimit) {
    if (NativeFibersEnabled())
        mojorecomp::fiber::Commit(*g_ppcContext, sp.u32, thread.u32, allocationBase.u32,
                                stackBase.u32, stackLimit.u32);
}

void MojoRecompDecoderFillBegin() {
    g_decoderFillState.lastProgress = 0xFFFFFFFFu;
    g_decoderFillState.stallRetries = 0;
}

void MojoRecompDecoderFillPrepare(PPCRegister& destination, PPCRegister& sampleCount) {
    if (!g_mojoGuestBase || !destination.u32 || sampleCount.u32 > 0x00800000u)
        return;

    std::memset(g_mojoGuestBase + destination.u32, 0,
                static_cast<std::size_t>(sampleCount.u32) * sizeof(uint16_t));
}

bool MojoRecompDecoderFillStalled(PPCRegister& progress, PPCRegister& sampleCount) {
    if (progress.u32 >= sampleCount.u32)
        return false;

    auto& state = g_decoderFillState;
    if (progress.u32 != state.lastProgress) {
        state.lastProgress = progress.u32;
        state.stallRetries = 0;
        return false;
    }

    if (++state.stallRetries < 256)
        return false;

    return true;
}
