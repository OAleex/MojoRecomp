#include "guest_thread.h"
#include "guest_fiber.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <xbox.h>

#include "timebase.h"

#include "../kernel/guestcall.h"
#include "../kernel/heap.h"
#include "../kernel/memory.h"

namespace {

constexpr uint32_t kPcrSize = 0xAB0;
constexpr uint32_t kTebSize = 0x2E0;
constexpr uint32_t kDefaultStackSize = 0x40000;
constexpr uint32_t kThreadKernelTimeOffset = 0x58;

struct KernelClockState
{
    struct ThreadRec
    {
        uint32_t teb;
        GuestThreadContext* context;
    };
    std::mutex mutex;
    std::vector<ThreadRec> threads;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
};

KernelClockState& KernelClock()
{
    // Intentionally process-lifetime. The publisher is detached and the process may
    // still have guest threads alive while CRT static destruction is running.
    static KernelClockState* state = new KernelClockState();
    return *state;
}

uint32_t KernelMilliseconds()
{
    // Capture the guest-clock epoch from the first kernel-time publication,
    // before debug mode can be enabled. This keeps the published millisecond
    // clock continuous when F1 later arms pause/slow-motion.
    static const uint64_t guestEpoch = mojorecomp::timebase::GuestTicks();
    if (mojorecomp::timebase::DebugClockActive())
    {
        const uint64_t now = mojorecomp::timebase::GuestTicks();
        return now >= guestEpoch
            ? static_cast<uint32_t>(((now - guestEpoch) * 1000ull) /
                                    MOJORECOMP_TIMEBASE_HZ)
            : 0u;
    }
    const auto elapsed = std::chrono::steady_clock::now() - KernelClock().epoch;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void PublishKernelTime(uint32_t teb, uint32_t milliseconds)
{
    *reinterpret_cast<be<uint32_t>*>(
        g_guestMemory.Translate(teb + kThreadKernelTimeOffset)) = milliseconds;
}

void EnsureKernelClockPublisher()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            auto& clock = KernelClock();
            for (;;)
            {
                const uint32_t milliseconds = KernelMilliseconds();
                {
                    std::lock_guard lock(clock.mutex);
                    for (const auto& thread : clock.threads)
                        PublishKernelTime(thread.teb, milliseconds);
                    static uint32_t nextThreadReport = 1000;
                    if (std::getenv("MOJORECOMP_THREAD_DIAGNOSTICS") &&
                        milliseconds >= nextThreadReport)
                    {
                        nextThreadReport = milliseconds + 1000;
                        for (const auto& thread : clock.threads)
                        {
                            const auto& ppc = thread.context->ppc;
                            std::fprintf(stderr,
                                "[thread] ms=%u tid=%08X pcr=%08X cpu=%u lr=%08X ctr=%08X "
                                "sp=%08X r3=%08X\n",
                                milliseconds, thread.context->threadId,
                                thread.context->pcr,
                                unsigned(*reinterpret_cast<const uint8_t*>(
                                    g_guestMemory.Translate(thread.context->pcr + 0x10C))),
                                uint32_t(ppc.lr), ppc.ctr.u32,
                                ppc.r1.u32, ppc.r3.u32);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    });
}

void RegisterKernelClockThread(GuestThreadContext* context)
{
    EnsureKernelClockPublisher();
    auto& clock = KernelClock();
    std::lock_guard lock(clock.mutex);
    clock.threads.push_back({context->teb, context});
    PublishKernelTime(context->teb, KernelMilliseconds());
}

void UnregisterKernelClockThread(GuestThreadContext* context)
{
    auto& clock = KernelClock();
    std::lock_guard lock(clock.mutex);
    std::erase_if(clock.threads,
                  [context](const auto& thread) { return thread.context == context; });
}

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber, uint32_t stackSize,
                                       uint32_t tlsSlots, uint32_t requestedThreadId)
{
    threadId = requestedThreadId;
    if (!stackSize)
        stackSize = kDefaultStackSize;
    stackSize = AlignUp(stackSize, 0x1000);
    if (!tlsSlots)
        tlsSlots = 64;
    const uint32_t tlsSize = AlignUp(tlsSlots * 4u, 16);

    const uint32_t prefix = AlignUp(kPcrSize + tlsSize + kTebSize, 16);
    const uint32_t total = prefix + stackSize;
    block = static_cast<uint8_t*>(g_guestHeap.Alloc(total, 16));
    if (!block)
    {
        std::fprintf(stderr, "[cpu] failed to allocate %u-byte main guest thread block\n", total);
        std::abort();
    }
    std::memset(block, 0, total);

    pcr = g_guestMemory.MapVirtual(block);
    const uint32_t tls = pcr + kPcrSize;
    teb = tls + tlsSize;
    stackLimit = pcr + prefix;
    stackBase = pcr + total;

    *reinterpret_cast<be<uint32_t>*>(block + 0x000) = tls;
    *reinterpret_cast<be<uint32_t>*>(block + 0x100) = teb;
    block[0x10C] = static_cast<uint8_t>(cpuNumber);
    if (tlsSize >= 0x14)
        *reinterpret_cast<be<uint32_t>*>(block + kPcrSize + 0x10) = 0xFFFFFFFFu;
    *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(teb + 0x14C)) = threadId;
    *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(teb + 0xD0)) = stackBase;
    *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(teb + 0x5C)) = stackBase;
    *reinterpret_cast<be<uint32_t>*>(g_guestMemory.Translate(teb + 0x60)) = stackLimit;

    ppc.r1.u64 = stackBase;
    ppc.r13.u64 = pcr;
    ppc.fpscr.loadFromHost();
    g_ppcContext = &ppc;
    RegisterKernelClockThread(this);

    std::fprintf(stderr,
                 "[cpu] bootstrap PCR=%08X TLS=%08X TEB=%08X stack=%08X..%08X (%u bytes)\n",
                 pcr, tls, teb, stackLimit, stackBase, stackSize);
}

GuestThreadContext::~GuestThreadContext()
{
    mojorecomp::fiber::Shutdown();
    if (teb)
        UnregisterKernelClockThread(this);
    if (g_ppcContext == &ppc)
        g_ppcContext = nullptr;
    if (block)
        g_guestHeap.Free(block);
}

bool SetGuestThreadLogicalCpu(uint32_t pcr, uint32_t cpuNumber, uint32_t* previousCpu)
{
    if (!pcr || cpuNumber >= 6)
        return false;

    auto& clock = KernelClock();
    std::lock_guard lock(clock.mutex);
    for (const auto& thread : clock.threads)
    {
        if (!thread.context || thread.context->pcr != pcr)
            continue;

        auto* currentCpu = reinterpret_cast<uint8_t*>(
            g_guestMemory.Translate(pcr + 0x10C));
        if (previousCpu)
            *previousCpu = *currentCpu;
        *currentCpu = static_cast<uint8_t>(cpuNumber);
        return true;
    }
    return false;
}
