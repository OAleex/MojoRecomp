// Asset-free handle/event integration through the guest import ABI.
#include "../kernel/guestcall.h"
#include "../kernel/unimplemented.h"
#include <xbox.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

int RunEventProbe()
{
    try
    {
        g_guestMemory.Init();
        auto* base = g_guestMemory.base;
        PPCContext ctx{};
        g_ppcContext = &ctx;
        ctx.r1.u64 = 0x30000;
        constexpr uint32_t handleOut = 0x10000, previousOut = 0x10004, timeout = 0x10008;
        PPC_STORE_U64(timeout, 0);
        const auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        const auto set = [&](uint32_t handle, uint32_t previous) {
            ctx.r3.u64 = handle;
            ctx.r4.u64 = previous;
            __imp__NtSetEvent(ctx, base);
            return ctx.r3.u32;
        };
        const auto wait = [&](uint32_t handle) {
            ctx.r3.u64 = handle;
            ctx.r4.u64 = ctx.r5.u64 = 0;
            ctx.r6.u64 = timeout;
            __imp__NtWaitForSingleObjectEx(ctx, base);
            return ctx.r3.u32;
        };
        for (uint32_t type : {0u, 1u})
        {
            ctx.r3.u64 = handleOut;
            ctx.r4.u64 = 0;
            ctx.r5.u64 = type;
            ctx.r6.u64 = 0;
            __imp__NtCreateEvent(ctx, base);
            require(ctx.r3.u32 == 0, "create event");
            const auto handle = PPC_LOAD_U32(handleOut);
            require(wait(handle) == 0x102, "unsignaled event must time out");
            require(set(handle, previousOut) == 0 && PPC_LOAD_U32(previousOut) == 0,
                    "set returns previous unsignaled state");
            require(set(handle, previousOut) == 0 && PPC_LOAD_U32(previousOut) == 1,
                    "repeated set returns previous signaled state");
            require(wait(handle) == 0, "set must release a wait");
            require(wait(handle) == (type == 0 ? 0 : 0x102), "manual/auto reset semantics");
            // The actual caller seen in the boot uses a null previous-state pointer.
            ctx.r3.u64 = handle;
            sub_82411888(ctx, base);
            require(ctx.r3.u32 == 1 && wait(handle) == 0, "real translated SetEvent wrapper");
            ctx.r3.u64 = handle;
            __imp__NtClose(ctx, base);
            PPC_STORE_U32(previousOut, 0xDEADBEEF);
            require(set(handle, previousOut) == 0xC0000008 &&
                    PPC_LOAD_U32(previousOut) == 0xDEADBEEF,
                    "invalid handle must not signal or overwrite output");
        }

        // Xbox critical sections are recursive and may be contended by native
        // host threads backing guest threads. Exercise recursion and blocking
        // through the actual guest import ABI so the host lock implementation
        // can be optimized without weakening those semantics.
        constexpr uint32_t criticalSectionAddress = 0x10800;
        auto criticalCall = [&](PPCContext& callCtx, PPCFunc fn) {
            callCtx.r3.u64 = criticalSectionAddress;
            fn(callCtx, base);
        };
        ctx.r13.u64 = 0x22000;
        criticalCall(ctx, __imp__RtlInitializeCriticalSection);
        criticalCall(ctx, __imp__RtlEnterCriticalSection);
        criticalCall(ctx, __imp__RtlEnterCriticalSection);
        auto* critical = reinterpret_cast<XRTL_CRITICAL_SECTION*>(
            base + criticalSectionAddress);
        require(uint32_t(critical->RecursionCount) == 2,
                "critical section recursive enter count");

        std::atomic<bool> criticalAttempting{false};
        std::atomic<bool> criticalAcquired{false};
        std::thread criticalWorker([&] {
            PPCContext workerCtx{};
            workerCtx.r1.u64 = 0x36000;
            workerCtx.r13.u64 = 0x23000;
            criticalAttempting.store(true, std::memory_order_release);
            criticalCall(workerCtx, __imp__RtlEnterCriticalSection);
            criticalAcquired.store(true, std::memory_order_release);
            criticalCall(workerCtx, __imp__RtlLeaveCriticalSection);
        });
        while (!criticalAttempting.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(!criticalAcquired.load(std::memory_order_acquire),
                "contended critical section must block another thread");

        criticalCall(ctx, __imp__RtlLeaveCriticalSection);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(!criticalAcquired.load(std::memory_order_acquire) &&
                    uint32_t(critical->RecursionCount) == 1,
                "recursive critical section must stay owned after partial leave");

        criticalCall(ctx, __imp__RtlLeaveCriticalSection);
        for (uint32_t spin = 0;
             spin < 500 && !criticalAcquired.load(std::memory_order_acquire);
             ++spin)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(criticalAcquired.load(std::memory_order_acquire),
                "critical section waiter must acquire after final leave");
        criticalWorker.join();
        require(uint32_t(critical->RecursionCount) == 0 &&
                    uint32_t(critical->OwningThread) == 0,
                "critical section final ownership state");

        // Header-level Ke* dispatcher objects are stored in guest memory in the
        // real title, but several native host threads access them concurrently.
        // Exercise the exact signal/wait helpers with a strict two-semaphore
        // handshake so no release may be lost or consumed twice.
        constexpr uint32_t requestAddress = 0x11000;
        constexpr uint32_t replyAddress = 0x11040;
        auto initializeSemaphore = [&](PPCContext& callCtx, uint32_t address) {
            callCtx.r3.u64 = address;
            callCtx.r4.u64 = 0;
            callCtx.r5.u64 = 1;
            __imp__KeInitializeSemaphore(callCtx, base);
        };
        auto releaseSemaphore = [&](PPCContext& callCtx, uint32_t address) {
            callCtx.r3.u64 = address;
            callCtx.r4.u64 = 0;
            callCtx.r5.u64 = 1;
            callCtx.r6.u64 = 0;
            __imp__KeReleaseSemaphore(callCtx, base);
        };
        auto waitSemaphore = [&](PPCContext& callCtx, uint32_t address) {
            callCtx.r3.u64 = address;
            callCtx.r4.u64 = 0;
            callCtx.r5.u64 = 0;
            callCtx.r6.u64 = 0;
            callCtx.r7.u64 = 0; // null timeout = infinite
            __imp__KeWaitForSingleObject(callCtx, base);
            return callCtx.r3.u32;
        };
        PPCContext mainDispatcherCtx{};
        mainDispatcherCtx.r1.u64 = 0x32000;
        initializeSemaphore(mainDispatcherCtx, requestAddress);
        initializeSemaphore(mainDispatcherCtx, replyAddress);
        constexpr uint32_t kRounds = 2000;
        std::atomic<uint32_t> workerFailures{0};
        std::thread worker([&] {
            PPCContext workerCtx{};
            workerCtx.r1.u64 = 0x34000;
            for (uint32_t i = 0; i < kRounds; ++i)
            {
                if (waitSemaphore(workerCtx, requestAddress) != 0)
                {
                    ++workerFailures;
                    return;
                }
                releaseSemaphore(workerCtx, replyAddress);
            }
        });
        for (uint32_t i = 0; i < kRounds; ++i)
        {
            releaseSemaphore(mainDispatcherCtx, requestAddress);
            require(waitSemaphore(mainDispatcherCtx, replyAddress) == 0,
                    "dispatcher semaphore reply wait");
        }
        worker.join();
        require(workerFailures.load() == 0, "dispatcher semaphore worker wait");
        const auto* request = reinterpret_cast<const XKSEMAPHORE*>(base + requestAddress);
        const auto* reply = reinterpret_cast<const XKSEMAPHORE*>(base + replyAddress);
        require(uint32_t(request->Header.SignalState) == 0 &&
                uint32_t(reply->Header.SignalState) == 0,
                "dispatcher semaphore final state");
        g_ppcContext = nullptr;
        std::puts("PASS: event handles and concurrent header dispatcher signaling");
        return 0;
    }
    catch (const MojoRecompUnimplementedImport& e)
    {
        std::fprintf(stderr, "FAIL: event import not implemented: %s\n", e.name);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: event %s\n", e.what());
    }
    return 1;
}
