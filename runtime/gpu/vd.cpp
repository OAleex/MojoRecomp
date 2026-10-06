#include "vd.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <xbox.h>

#include "../debug_mode.h"
#include "../cpu/guest_thread.h"
#include "../cpu/timebase.h"
#include "../kernel/guestcall.h"
#include "../kernel/heap.h"
#include "../kernel/klog.h"
#include "../kernel/memory.h"
#include "../kernel/unimplemented.h"
#include "../kernel/xex_loader.h"
#include "../host/frame_rate_policy.h"
#include "../host/timing.h"
#include "pm4.h"
#include "renderer_probe.h"

namespace {

std::atomic<uint32_t> g_interruptCallback{0};
std::atomic<uint32_t> g_interruptUserData{0};
std::atomic<uint32_t> g_ringBase{0};
std::atomic<uint32_t> g_ringSize{0};
std::atomic<uint32_t> g_readPointerWriteback{0};
std::atomic<uint32_t> g_gpuIdentifierAddress{0};
std::atomic<bool> g_interruptPumpRunning{false};
thread_local GuestThreadContext* g_graphicsPumpContext = nullptr;
std::mutex g_graphicsInterruptMutex;
std::mutex g_graphicsPumpWakeMutex;
std::condition_variable g_graphicsPumpWakeCv;
std::atomic<uint64_t> g_graphicsPumpWakeGeneration{0};

constexpr uint32_t kCpRbWptrAddress = 0x7FC80714u;
constexpr uint32_t kDisplayControllerGate = 0x7FC86544u;

uint64_t KernelInterruptTime100ns()
{
    static const auto epoch = std::chrono::steady_clock::now();
    static const uint64_t guestEpoch = mojorecomp::timebase::GuestTicks();

    // The shared Xbox interrupt-time counter is another monotonic clock the
    // title can sample directly. Keep it on the same debug timeline as mftb and
    // kernel milliseconds so F3 turbo doesn't expose contradictory elapsed
    // times to different engine subsystems.
    if (mojorecomp::timebase::DebugClockActive())
    {
        const uint64_t now = mojorecomp::timebase::GuestTicks();
        const uint64_t elapsed = now >= guestEpoch ? now - guestEpoch : 0;
        return static_cast<uint64_t>((__uint128_t(elapsed) * 10'000'000ull) /
                                     MOJORECOMP_TIMEBASE_HZ);
    }

    const auto elapsed = std::chrono::steady_clock::now() - epoch;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            elapsed).count());
}

uint64_t KernelSystemTime100ns()
{
    constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ull;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t unix100ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            now).count());
    return kWindowsToUnixEpoch100ns + unix100ns;
}

void PumpIdleWait()
{
    // Sleep while the ring is idle, but let an interrupt handshake wake the CP
    // immediately instead of paying the full polling timeout.
    const uint64_t observed = g_graphicsPumpWakeGeneration.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lock(g_graphicsPumpWakeMutex);
    g_graphicsPumpWakeCv.wait_for(lock, std::chrono::microseconds(250), [&] {
        return g_graphicsPumpWakeGeneration.load(std::memory_order_acquire) != observed;
    });
}

void WakeGraphicsProgressPump()
{
    g_graphicsPumpWakeGeneration.fetch_add(1, std::memory_order_release);
    g_graphicsPumpWakeCv.notify_one();
}

bool CpTimeProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_CP_TIME_PROFILE");
        return value && *value && std::strtoul(value, nullptr, 0) != 0;
    }();
    return enabled;
}

bool CpPeriodicDiagnosticsEnabled()
{
    static const bool enabled = [] {
        if (CpTimeProfileEnabled() || MojoRecompVerboseDiagnosticsEnabled())
            return true;
        const char* packetProfile = std::getenv("MOJORECOMP_PM4_PROFILE");
        if (packetProfile && packetProfile[0] && packetProfile[0] != '0')
            return true;
        const char* timeProfile = std::getenv("MOJORECOMP_PM4_TIME_PROFILE");
        return timeProfile && timeProfile[0] && timeProfile[0] != '0';
    }();
    return enabled;
}

std::chrono::nanoseconds GuestVblankPeriod()
{
    // Keep the guest display interrupt on the console's ~60 Hz cadence. Crash
    // consumes source-0 as an engine progress tick; using Xenia's vsync-disabled
    // 1 ms MarkVblank cadence here lets the title run its 30 Hz update path at
    // roughly twice the intended rate once the audio clock is also real-time.
    //
    // The PM4 worker remains independent, so renderer stalls do not drag this
    // clock down. MOJORECOMP_GUEST_VBLANK_US=1000 remains available for explicit
    // regression/A-B comparisons with the old uncapped behavior.
    uint32_t periodUs = static_cast<uint32_t>(
        mojorecomp::host::PeriodForHzRoundedUs(
            mojorecomp::host::ActiveFrameRatePolicy().guestVblankHz).count());
    if (const char* value = std::getenv("MOJORECOMP_GUEST_VBLANK_US"))
    {
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        if (parsed >= 100 && parsed <= 1000000)
            periodUs = static_cast<uint32_t>(parsed);
    }
    // F3 fast-forward is intentionally global rather than a cutscene hook. A
    // fixed-step title like Crash also advances from display/vblank interrupts,
    // so speeding only mftb/kernel time would not behave like emulator turbo.
    // Run vblank at 4x while turbo is active; keep the normal cadence while
    // paused so F6 remains a quiet, responsive freeze.
    if (mojorecomp::debug::FastForward() && !mojorecomp::debug::Paused())
        periodUs = std::max<uint32_t>(100u, periodUs / 4u);

    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::microseconds(periodUs));
}

void PublishTimeStampBundle()
{
    const uint32_t bundle = g_keTimeStampBundle.load(std::memory_order_acquire);
    if (!bundle)
        return;

    const uint64_t interruptTime = KernelInterruptTime100ns();
    *reinterpret_cast<be<uint64_t>*>(g_guestMemory.Translate(bundle + 0)) = interruptTime;
    *reinterpret_cast<be<uint64_t>*>(g_guestMemory.Translate(bundle + 8)) = KernelSystemTime100ns();
    *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(bundle + 16)) =
        static_cast<uint32_t>(interruptTime / 10000u);
}

void DeliverGraphicsInterrupt(GuestThreadContext& threadContext, uint32_t source)
{
    // Source-0 (display/vblank) and source-1 (command processor) are scheduled
    // independently by the hardware. Host-side they may therefore arrive from
    // different worker threads, but the title's interrupt callback must never be
    // entered concurrently with itself.
    std::lock_guard<std::mutex> interruptLock(g_graphicsInterruptMutex);

    const uint32_t callback = g_interruptCallback.load(std::memory_order_acquire);
    const uint32_t userData = g_interruptUserData.load(std::memory_order_relaxed);
    if (!callback)
        return;

    PPCFunc* host = g_guestMemory.FindFunction(callback);
    if (!host)
    {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            KLOG("graphics interrupt callback %08X is not translated\n", callback);
        return;
    }

    threadContext.ppc.r3.u64 = source;
    threadContext.ppc.r4.u64 = userData;
    threadContext.ppc.lr = 0;
    g_ppcContext = &threadContext.ppc;
    host(threadContext.ppc, g_guestMemory.base);
}

void DeliverCommandProcessorInterrupt()
{
    if (!g_graphicsPumpContext)
        return;

    try
    {
        DeliverGraphicsInterrupt(*g_graphicsPumpContext, 1);
    }
    catch (const MojoRecompUnimplementedImport& missing)
    {
        KLOG("graphics source-1 ISR stopped at missing import %s (lr=%08X); "
             "disabling graphics ISR delivery\n",
             missing.name, missing.lr);
        g_interruptCallback = 0;
    }
    catch (const GuestThreadExit& exit)
    {
        KLOG("graphics source-1 ISR requested thread exit %08X; disabling ISR delivery\n",
             exit.code);
        g_interruptCallback = 0;
    }
}

// Source-0 display/vblank is independent of command-processor progress on the
// console. Keep it on a dedicated host worker too: VkPresenter may legitimately
// block the PM4 worker on a GPU fence for longer than one display period, and
// tying both clocks to that worker would turn a rendering slowdown into a title
// simulation slowdown.
void GraphicsVblankPump()
{
    GuestThreadContext threadContext(2, 0x10000, 64, 0xEF0);
    uint64_t vblankCount = 0;
    auto vblankPeriod = GuestVblankPeriod();
    mojorecomp::host::PeriodicDeadline vblankTimer;
    vblankTimer.Reset(std::chrono::steady_clock::now(), vblankPeriod);
    auto perfWindowStart = std::chrono::steady_clock::now();
    uint64_t perfVblankIsrNs = 0;
    uint64_t perfVblankCount = 0;
    KLOG("graphics guest-vblank pump started (period=%.3f ms, independent of PM4)\n",
         double(vblankPeriod.count()) / 1.0e6);

    for (;;)
    {
        const auto requestedPeriod = GuestVblankPeriod();
        if (requestedPeriod != vblankPeriod)
        {
            vblankPeriod = requestedPeriod;
            vblankTimer.Reset(std::chrono::steady_clock::now(), vblankPeriod);
            KLOG("graphics guest-vblank period changed to %.3f ms (fast-forward=%u)\n",
                 double(vblankPeriod.count()) / 1.0e6,
                 mojorecomp::debug::FastForward() ? 1u : 0u);
        }
        mojorecomp::host::WaitUntil(vblankTimer.Deadline());

        // The kernel exports this bundle as shared data. Titles read it directly,
        // so update it from the display clock rather than from PM4 progress.
        PublishTimeStampBundle();

        const auto now = std::chrono::steady_clock::now();
        uint32_t catchupVblanks = 0;
        while (now >= vblankTimer.Deadline() && catchupVblanks < 64)
        {
            try
            {
                // Crash's ISR reads bit 0 of 0x7FC86544 before it runs its source-0
                // display/vblank work. No code in the title writes that register; it
                // is a display-controller status gate supplied by the hardware.
                auto* gate = reinterpret_cast<be<uint32_t>*>(
                    g_guestMemory.Translate(kDisplayControllerGate));
                *gate = uint32_t(*gate) | 1u;
                if (vblankCount++ == 0)
                    KLOG("delivering first vblank to %08X(0, %08X), gate=%08X\n",
                         g_interruptCallback.load(), g_interruptUserData.load(),
                         uint32_t(*gate));
                const auto isrStart = std::chrono::steady_clock::now();
                // ReXGlue/Xenia increments the command-processor counter in
                // MarkVblank before dispatching source 0. EVENT_WRITE_SHD can
                // expose this counter directly to guest memory.
                Pm4_IncrementCounter();
                DeliverGraphicsInterrupt(threadContext, 0);
                perfVblankIsrNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - isrStart).count());
                ++perfVblankCount;

                const auto perfNow = std::chrono::steady_clock::now();
                const double wallMs =
                    std::chrono::duration<double, std::milli>(perfNow - perfWindowStart).count();
                if (wallMs >= 1000.0)
                {
                    const double actualHz = wallMs > 0.0
                                                ? double(perfVblankCount) * 1000.0 / wallMs
                                                : 0.0;
                    KLOG("graphics vblank perf: vblank=%.2f Hz window=%.1f ms ISR=%.3f ms/vblank\n",
                         actualHz, wallMs,
                         perfVblankCount
                             ? double(perfVblankIsrNs) / 1.0e6 / double(perfVblankCount)
                             : 0.0);
                    perfWindowStart = perfNow;
                    perfVblankIsrNs = 0;
                    perfVblankCount = 0;
                }
            }
            catch (const MojoRecompUnimplementedImport& missing)
            {
                KLOG("graphics ISR stopped at missing import %s (lr=%08X); "
                     "ring progress remains active\n",
                     missing.name, missing.lr);
                g_interruptCallback = 0;
            }
            catch (const GuestThreadExit& exit)
            {
                KLOG("graphics ISR requested thread exit %08X; disabling ISR delivery\n",
                     exit.code);
                g_interruptCallback = 0;
            }
            vblankTimer.Advance();
            ++catchupVblanks;
        }

        // A debugger pause or pathological host stall can put us hundreds of
        // guest-vblanks behind. Don't execute an unbounded ISR storm; after 64
        // catch-up deliveries, skip only the excess periods. GPU/PM4 stalls can
        // no longer reach this path because they run on the other worker.
        if (now >= vblankTimer.Deadline())
        {
            const uint64_t missed = vblankTimer.SkipPast(now);
            static std::atomic<uint32_t> catchupReports{0};
            if (catchupReports.fetch_add(1, std::memory_order_relaxed) < 8)
                KLOG("graphics vblank catch-up capped; skipped %lld excess periods\n",
                     static_cast<long long>(missed));
        }
    }
}

// PM4 is walked continuously on its own worker. Source-1 graphics interrupts are
// delivered by the parser exactly where an INTERRUPT packet appears in the stream.
void GraphicsProgressPump()
{
    SetThreadDescription(GetCurrentThread(), L"MojoRecomp PM4");
    GuestThreadContext threadContext(2, 0x10000, 64, 0xEF1);
    uint32_t lastPublishedRptr = 0xFFFFFFFFu;
    const bool periodicDiagnostics = CpPeriodicDiagnosticsEnabled();
    auto perfWindowStart = periodicDiagnostics
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    uint64_t perfFrameStart = Pm4_FrameCount();
    uint64_t perfPm4Ns = 0;
    uint64_t perfDrawSinkStart = Pm4_DrawSinkCpuNs();
    uint64_t perfPumpLoops = 0;
    g_graphicsPumpContext = &threadContext;
    Pm4_SetInterruptSink(DeliverCommandProcessorInterrupt);
    Pm4_SetInterruptWakeSink(WakeGraphicsProgressPump);
    RendererProbe_Init();
    KLOG("graphics command processor pump started (PM4) tid=%lu\n",
         static_cast<unsigned long>(GetCurrentThreadId()));

    for (;;)
    {
        const uint32_t ringBase = g_ringBase.load(std::memory_order_acquire);
        const uint32_t ringSize = g_ringSize.load(std::memory_order_relaxed);
        const uint32_t rptrSlot = g_readPointerWriteback.load(std::memory_order_acquire);

        bool pm4Advanced = false;
        bool pm4Backlogged = false;
        if (ringBase && ringSize && rptrSlot)
        {
            const uint32_t before = Pm4_Cursor();
            const uint32_t wptr =
                *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(kCpRbWptrAddress));
            Pm4_SetReadPointerSlot(rptrSlot);
            const auto pm4Start = periodicDiagnostics
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const uint32_t consumed = Pm4_Execute(g_guestMemory.base, wptr);
            if (periodicDiagnostics)
            {
                perfPm4Ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - pm4Start).count());
            }
            *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(rptrSlot)) = consumed;
            pm4Advanced = consumed != before;
            const uint32_t ringDwords = ringSize / 4u;
            if (ringDwords)
                pm4Backlogged = (consumed % ringDwords) != (wptr % ringDwords);

            if (consumed != lastPublishedRptr)
            {
                if (lastPublishedRptr == 0xFFFFFFFFu)
                    KLOG("graphics ring progress: first WPTR=%u RPTR=%u (slot %08X)\n",
                         wptr, consumed, rptrSlot);
                lastPublishedRptr = consumed;
            }
        }
        if (periodicDiagnostics)
        {
            ++perfPumpLoops;
            const auto now = std::chrono::steady_clock::now();
            const double perfWindowMs =
                std::chrono::duration<double, std::milli>(now - perfWindowStart).count();
            if (perfWindowMs >= 1000.0)
            {
                const uint64_t perfFrameEnd = Pm4_FrameCount();
                const uint64_t perfFrames = perfFrameEnd - perfFrameStart;
                const double pm4WindowMs = double(perfPm4Ns) / 1.0e6;
                const double pm4FrameMs = perfFrames ? pm4WindowMs / double(perfFrames) : 0.0;
                const uint64_t drawSinkEnd = Pm4_DrawSinkCpuNs();
                const uint64_t drawSinkNs = drawSinkEnd - perfDrawSinkStart;
                const uint64_t parserNs = perfPm4Ns > drawSinkNs ? perfPm4Ns - drawSinkNs : 0;
                const double drawSinkMs = double(drawSinkNs) / 1.0e6;
                const double parserMs = double(parserNs) / 1.0e6;
                const double drawSinkFrameMs = perfFrames ? drawSinkMs / double(perfFrames) : 0.0;
                const double parserFrameMs = perfFrames ? parserMs / double(perfFrames) : 0.0;
                if (CpTimeProfileEnabled())
                    KLOG("graphics CP perf: frame=%llu frames=%llu window=%.1f ms PM4=%.2f ms/window PM4/frame=%.3f ms sink/frame=%.3f ms parser/frame=%.3f ms loops=%llu\n",
                         static_cast<unsigned long long>(perfFrameEnd),
                         static_cast<unsigned long long>(perfFrames),
                         perfWindowMs, pm4WindowMs, pm4FrameMs, drawSinkFrameMs, parserFrameMs,
                         static_cast<unsigned long long>(perfPumpLoops));
                else
                    KLOG_DIAG("graphics CP perf: frame=%llu frames=%llu window=%.1f ms PM4=%.2f ms/window PM4/frame=%.3f ms sink/frame=%.3f ms parser/frame=%.3f ms loops=%llu\n",
                         static_cast<unsigned long long>(perfFrameEnd),
                         static_cast<unsigned long long>(perfFrames),
                         perfWindowMs, pm4WindowMs, pm4FrameMs, drawSinkFrameMs, parserFrameMs,
                         static_cast<unsigned long long>(perfPumpLoops));
                const uint32_t wptr = *reinterpret_cast<be<uint32_t>*>(
                    g_guestMemory.Translate(kCpRbWptrAddress));
                KLOG_DIAG("PM4 health: wptr=%u rptr=%u packets=%llu ib=%llu stores=%llu "
                     "ints=%llu waits=%llu stalls=%llu draws=%llu frames=%llu shaders=%llu shaderCache=%llu\n",
                     wptr, Pm4_Cursor(),
                     static_cast<unsigned long long>(Pm4_PacketCount()),
                     static_cast<unsigned long long>(Pm4_IndirectBufferCount()),
                     static_cast<unsigned long long>(Pm4_GpuStoreCount()),
                     static_cast<unsigned long long>(Pm4_InterruptCount()),
                     static_cast<unsigned long long>(Pm4_WaitCount()),
                     static_cast<unsigned long long>(Pm4_WaitStallCount()),
                     static_cast<unsigned long long>(Pm4_DrawCount()),
                     static_cast<unsigned long long>(Pm4_FrameCount()),
                     static_cast<unsigned long long>(Pm4_ShaderBindCount()),
                     static_cast<unsigned long long>(Pm4_ShaderCacheHitCount()));
                Pm4_LogPacketProfile();
                Pm4_LogTimingProfile();
                perfWindowStart = now;
                perfFrameStart = perfFrameEnd;
                perfPm4Ns = 0;
                perfDrawSinkStart = drawSinkEnd;
                perfPumpLoops = 0;
            }
        }

        // When a pass just consumed everything currently published by the guest,
        // immediately poll once more instead of voluntarily yielding the Windows
        // quantum. Crash submits many small batches around a frame boundary; a
        // yield here can turn a sub-millisecond hand-off into scheduler jitter.
        // The very next empty poll still takes the precise 250 us idle wait, so
        // this adds at most one cheap repoll and never becomes a busy loop.
        if (pm4Backlogged)
            PumpIdleWait();
        else if (!pm4Advanced)
            PumpIdleWait();
    }
}

uint32_t PhysicalToCached(uint32_t physical)
{
    return 0xA0000000u | (physical & 0x1FFFFFFFu);
}

void VdInitializeEngines_x(uint32_t unk0, uint32_t callback, uint32_t unk1,
                           uint32_t pfnUnk0, uint32_t pfnUnk1)
{
    KLOG("VdInitializeEngines(%08X, cb=%08X, %08X, %08X, %08X)\n",
         unk0, callback, unk1, pfnUnk0, pfnUnk1);
}

void VdShutdownEngines_x()
{
    KLOG("VdShutdownEngines\n");
    g_interruptCallback = 0;
    g_interruptUserData = 0;
    g_ringBase = 0;
    g_ringSize = 0;
    g_readPointerWriteback = 0;
    Pm4_SetInterruptWakeSink(nullptr);
    Pm4_SetReadPointerSlot(0);
    Pm4_SetReadPointerUpdateFrequency(1);
}

void VdSetGraphicsInterruptCallback_x(uint32_t callback, uint32_t userData)
{
    g_interruptCallback = callback;
    g_interruptUserData = userData;
    KLOG("VdSetGraphicsInterruptCallback(cb=%08X, user=%08X)\n", callback, userData);

    bool expected = false;
    if (callback && g_interruptPumpRunning.compare_exchange_strong(expected, true))
    {
        std::thread(GraphicsProgressPump).detach();
        std::thread(GraphicsVblankPump).detach();
    }
}

void VdInitializeRingBuffer_x(uint32_t basePhysical, uint32_t sizeLog2)
{
    const uint32_t base = PhysicalToCached(basePhysical);
    const uint32_t size = sizeLog2 < 29 ? (1u << (sizeLog2 + 3)) : 0;
    g_ringBase = base;
    g_ringSize = size;
    Pm4_SetRingBuffer(base, size);
    KLOG("VdInitializeRingBuffer phys=%08X -> %08X, %u bytes\n",
         basePhysical, base, size);
}

void VdEnableRingBufferRPtrWriteBack_x(uint32_t slotPhysical, uint32_t blockSizeLog2)
{
    const uint32_t slot = PhysicalToCached(slotPhysical);
    // Match Xenia's interpretation of CP_RB_CNTL.RB_BLKSZ: convert the guest's
    // log2 block size to a ring-index update frequency. Crash normally supplies
    // 6 here, which yields an update every 16 dwords instead of every packet.
    uint32_t updateFreq = blockSizeLog2 < 31
                              ? std::max(1u, (1u << blockSizeLog2) >> 2)
                              : 1u;
    if (const char* overrideValue = std::getenv("MOJORECOMP_RPTR_WRITEBACK_DWORDS");
        overrideValue && *overrideValue)
    {
        const unsigned long parsed = std::strtoul(overrideValue, nullptr, 0);
        if (parsed > 0 && parsed <= 0x100000ul)
            updateFreq = static_cast<uint32_t>(parsed);
    }
    g_readPointerWriteback = slot;
    Pm4_SetReadPointerSlot(slot);
    Pm4_SetReadPointerUpdateFrequency(updateFreq);
    if (slot)
        *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(slot)) = 0;
    KLOG("VdEnableRingBufferRPtrWriteBack phys=%08X -> %08X blockLog2=%u freq=%u\n",
         slotPhysical, slot, blockSizeLog2, updateFreq);
}

void VdSetSystemCommandBufferGpuIdentifierAddress_x(uint32_t address)
{
    g_gpuIdentifierAddress = address;
    KLOG("VdSetSystemCommandBufferGpuIdentifierAddress(%08X)\n", address);
}

uint32_t VdSetDisplayMode_x(uint32_t mode)
{
    KLOG("VdSetDisplayMode(%08X)\n", mode);
    return 0;
}

void VdGetCurrentDisplayInformation_x(uint8_t* info)
{
    if (!info)
        return;
    auto at16 = [&](size_t off) { return reinterpret_cast<be<uint16_t>*>(info + off); };
    auto at32 = [&](size_t off) { return reinterpret_cast<be<uint32_t>*>(info + off); };
    std::memset(info, 0, 0x58);
    *at16(0x00) = 1280;
    *at16(0x02) = 720;
    *at32(0x10) = 1280;
    *at32(0x14) = 720;
    *at32(0x18) = 1280;
    *at32(0x1C) = 720;
    *at32(0x20) = 1;
    *at32(0x30) = 1;
    *at16(0x40) = 320;
    *at16(0x42) = 180;
    *at16(0x44) = 320;
    *at16(0x46) = 180;
    *at16(0x48) = 1280;
    *at16(0x4A) = 720;
    *reinterpret_cast<be<float>*>(info + 0x4C) =
        static_cast<float>(mojorecomp::host::ActiveFrameRatePolicy().displayHz);
    *at16(0x56) = 1280;
}

void VdGetCurrentDisplayGamma_x(be<uint32_t>* type, be<float>* gamma)
{
    if (type)
        *type = 2;
    if (gamma)
        *gamma = 2.22222233f;
}

uint32_t VdPersistDisplay_x(uint32_t unknown, be<uint32_t>* blockOut)
{
    if (!blockOut)
        return 0;
    void* host = g_guestHeap.AllocPhysical(64, 32);
    *blockOut = host ? g_guestMemory.MapVirtual(host) : 0;
    KLOG("VdPersistDisplay(%08X) -> %08X\n", unknown, uint32_t(*blockOut));
    return host ? 1u : 0u;
}

uint32_t VdRetrainEDRAM_x(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
    return 0;
}

uint32_t VdRetrainEDRAMWorker_x(uint32_t) { return 0; }
uint32_t VdIsHSIOTrainingSucceeded_x() { return 1; }

void VdGetSystemCommandBuffer_x(be<uint32_t>* bufferOut, be<uint32_t>* identifierOut)
{
    if (bufferOut)
    {
        std::memset(bufferOut, 0, 0x94);
        bufferOut[0] = 0xBEEF0000;
    }
    if (identifierOut)
        *identifierOut = 0xBEEF0001;
}

void VdCallGraphicsNotificationRoutines_x(uint32_t kind, uint32_t args)
{
    KLOG("VdCallGraphicsNotificationRoutines(%08X, %08X)\n", kind, args);
}

void VdEnableDisableClockGating_x(uint32_t enable)
{
    (void)enable;
}

} // namespace

GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines_x)
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines_x)
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback_x)
GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer_x)
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack_x)
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress,
                    VdSetSystemCommandBufferGpuIdentifierAddress_x)
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode_x)
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation_x)
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma_x)
GUEST_FUNCTION_HOOK(__imp__VdPersistDisplay, VdPersistDisplay_x)
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAM, VdRetrainEDRAM_x)
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker_x)
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded_x)
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer_x)
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines,
                    VdCallGraphicsNotificationRoutines_x)
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating_x)

PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    KCALL("__imp__VdInitializeScalerCommandBuffer");
    const uint32_t destination =
        *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 2 * 8);
    const uint32_t dwords =
        *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 3 * 8);
    KLOG("VdInitializeScalerCommandBuffer src=%08X dst=%08X dwords=%u -> 0\n",
         ctx.r4.u32, destination, dwords);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdSwap)
{
    KCALL("VdSwap");

    const uint32_t buffer = ctx.r3.u32;
    const uint32_t fetchPtr = ctx.r4.u32;
    const uint32_t frontBufferPtr = ctx.r8.u32;
    const uint32_t widthPtr =
        *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 0 * 8);
    const uint32_t heightPtr =
        *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + 1 * 8);

    if (!buffer)
    {
        ctx.r3.u64 = 0;
        return;
    }

    const uint32_t frontBuffer = frontBufferPtr ? PPC_LOAD_U32(frontBufferPtr) : 0;
    const uint32_t width = widthPtr ? PPC_LOAD_U32(widthPtr) : 1280;
    const uint32_t height = heightPtr ? PPC_LOAD_U32(heightPtr) : 720;

    uint32_t at = buffer;
    auto emit = [&](uint32_t dword) {
        PPC_STORE_U32(at, dword);
        at += 4;
    };

    // Preserve the fetch constant composed by the guest, then append the kernel's
    // swap packet. The caller reserves exactly 64 dwords and advances past all of
    // them unconditionally, so the unused tail must be valid PM4 no-ops.
    emit(0x00054800);
    for (uint32_t i = 0; i < 6; ++i)
        emit(fetchPtr ? PPC_LOAD_U32(fetchPtr + i * 4) : 0);
    emit(0xC0036400);
    emit(0x53574150); // 'SWAP'
    emit(frontBuffer);
    emit(width);
    emit(height);

    constexpr uint32_t kReservationDwords = 64;
    const uint32_t written = (at - buffer) / 4;
    for (uint32_t i = written; i < kReservationDwords; ++i)
        emit(0x80000000); // PM4 type-2 NOP

    static std::atomic<uint64_t> swaps{0};
    if (swaps.fetch_add(1, std::memory_order_relaxed) == 0)
        KLOG("VdSwap first packet at %08X front=%08X %ux%u (%u/%u dwords)\n",
             buffer, frontBuffer, width, height, written, kReservationDwords);
    ctx.r3.u64 = kReservationDwords;
}

MojoRecompVdState MojoRecompVdGetState()
{
    return {g_interruptCallback.load(), g_interruptUserData.load(), g_ringBase.load(),
            g_ringSize.load(), g_readPointerWriteback.load(), g_gpuIdentifierAddress.load()};
}
