#include <cstdint>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "../gpu/gpu_executor.h"
#include "../gpu/guest_memory_snapshot.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool WaitFor(const std::atomic<bool>& flag,
             std::chrono::milliseconds timeout = std::chrono::seconds(2))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

} // namespace

int main()
{
    using namespace mojorecomp::gpu;

    std::vector<int> order;
    GpuExecutorBackend backend{};
    backend.draw = [&](const DrawCommand& command, const uint32_t* registers) {
        if (registers[0x2208] != 4u)
            return false;
        order.push_back(100 + int(command.draw.indexCount));
        return true;
    };
    backend.resize = [&](const ResizeCommand& command) {
        order.push_back(200 + int(command.width / 100));
        return true;
    };
    backend.present = [&](const PresentCommand& command) {
        order.push_back(300 + int(command.height / 100));
        return true;
    };
    backend.store = [&](const GuestStoreCommand& command) {
        order.push_back(400 + int(command.value));
        return true;
    };

    SynchronousGpuExecutor executor(std::move(backend));
    executor.WriteRegister(0x2208, 4u);
    DrawCommand draw{};
    draw.draw.indexCount = 7;
    const GpuSequence drawSequence = executor.Submit(std::move(draw));
    const GpuSequence storeSequence = executor.Submit(GuestStoreCommand{nullptr, 0x1000, 5});
    const GpuSequence resizeSequence = executor.Submit(ResizeCommand{1280, 720});
    const GpuSequence presentSequence = executor.Submit(PresentCommand{0x12340000u, 1280, 720});

    if (drawSequence != 1 || storeSequence != 2 || resizeSequence != 3 || presentSequence != 4)
        return Fail("sequence numbers are not strictly monotonic");
    if (order != std::vector<int>({107, 405, 212, 307}))
        return Fail("commands were not executed in submission order");
    if (!executor.Wait(drawSequence) || !executor.Wait(presentSequence))
        return Fail("completed synchronous commands did not satisfy Wait");
    if (executor.CompletedSequence() != presentSequence)
        return Fail("completed sequence did not reach the last command");

    executor.Shutdown();
    if (executor.Submit(DrawCommand{}) != 0)
        return Fail("submission succeeded after shutdown");

    GpuExecutorBackend failingBackend{};
    failingBackend.present = [](const PresentCommand&) { return false; };
    SynchronousGpuExecutor failing(std::move(failingBackend));
    const GpuSequence failed = failing.Submit(PresentCommand{});
    if (failing.Wait(failed))
        return Fail("backend failure was not preserved through Wait");

    std::mutex asyncMutex;
    std::vector<int> asyncOrder;
    GpuExecutorBackend asyncBackend{};
    asyncBackend.draw = [&](const DrawCommand& command, const uint32_t* registers) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        if (registers[0x2208] != 4u)
            return false;
        asyncOrder.push_back(100 + int(command.draw.indexCount));
        return true;
    };
    asyncBackend.resize = [&](const ResizeCommand& command) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        asyncOrder.push_back(200 + int(command.width / 100));
        return true;
    };
    asyncBackend.present = [&](const PresentCommand& command) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        asyncOrder.push_back(300 + int(command.height / 100));
        return true;
    };
    asyncBackend.store = [&](const GuestStoreCommand& command) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        asyncOrder.push_back(400 + int(command.value));
        return true;
    };

    AsynchronousGpuExecutor async(std::move(asyncBackend), 1, 3);
    async.WriteRegister(0x2208, 1u);
    async.WriteRegister(0x2208, 2u);
    async.WriteRegister(0x2208, 4u);
    DrawCommand asyncDraw{};
    asyncDraw.draw.indexCount = 9;
    asyncDraw.guestSnapshot = std::make_unique<GuestMemorySnapshot>();
    const GpuSequence asyncDrawSequence = async.Submit(std::move(asyncDraw));
    const GpuSequence asyncStoreSequence = async.Submit(
        GuestStoreCommand{nullptr, 0x2000, 6});
    const GpuSequence asyncResizeSequence = async.Submit(ResizeCommand{1280, 720});
    const GpuSequence asyncPresentSequence = async.Submit(PresentCommand{0x12340000u, 1280, 720});
    if (!async.Wait(asyncPresentSequence))
        return Fail("async executor did not complete through present");
    if (asyncDrawSequence != 1 || asyncStoreSequence != 2 ||
        asyncResizeSequence != 3 || asyncPresentSequence != 4)
        return Fail("async sequence numbers are not strictly monotonic");
    {
        std::lock_guard<std::mutex> lock(asyncMutex);
        if (asyncOrder != std::vector<int>({109, 406, 212, 307}))
            return Fail("async commands were lost or reordered");
    }
    const auto asyncStats = async.Stats();
    if (asyncStats.completed != asyncPresentSequence || asyncStats.registerWrites != 3 ||
        asyncStats.emittedRegisterWrites != 1 ||
        asyncStats.submittedBatches == 0 || asyncStats.maxQueueDepth == 0)
        return Fail("async register coalescing/statistics are incorrect");
    async.Shutdown();

    GpuExecutorBackend missingSnapshotBackend{};
    missingSnapshotBackend.draw = [](const DrawCommand&, const uint32_t*) {
        return true;
    };
    AsynchronousGpuExecutor missingSnapshot(std::move(missingSnapshotBackend));
    if (missingSnapshot.Submit(DrawCommand{}) != 0)
        return Fail("async executor accepted a draw without an immutable snapshot");
    missingSnapshot.Shutdown();

    std::vector<uint8_t> snapshotGuest(64, 0x7F);
    auto incomplete = std::make_unique<GuestMemorySnapshot>();
    if (!incomplete->Capture(snapshotGuest.data(), 0, 8))
        return Fail("could not prepare incomplete snapshot regression");
    GpuExecutorBackend missingReadBackend{};
    missingReadBackend.draw = [&](const DrawCommand& command, const uint32_t*) {
        const uint8_t* bytes = GuestReadPtr(command.guestBase, 32, 4);
        return bytes && bytes[0] == 0;
    };
    AsynchronousGpuExecutor missingRead(std::move(missingReadBackend));
    DrawCommand missingReadDraw{};
    missingReadDraw.guestBase = snapshotGuest.data();
    missingReadDraw.guestSnapshot = std::move(incomplete);
    const GpuSequence missingReadSequence = missingRead.Submit(std::move(missingReadDraw));
    if (!missingReadSequence || missingRead.Wait(missingReadSequence))
        return Fail("missing snapshot read was not surfaced as a backend failure");
    missingRead.Shutdown();

    std::atomic<bool> interruptRequested{false};
    std::atomic<bool> interruptDelivered{false};
    GpuExecutorBackend handshakeBackend{};
    handshakeBackend.interrupt = [&](const InterruptCommand&) {
        interruptRequested.store(true, std::memory_order_release);
        while (!interruptDelivered.load(std::memory_order_acquire))
            std::this_thread::yield();
        return true;
    };
    handshakeBackend.producerService = [&] {
        if (interruptRequested.load(std::memory_order_acquire))
            interruptDelivered.store(true, std::memory_order_release);
    };
    AsynchronousGpuExecutor handshake(std::move(handshakeBackend), 2, 64);
    const GpuSequence interruptSequence = handshake.Submit(InterruptCommand{});
    const GpuSequence afterInterrupt = handshake.Submit(PresentCommand{});
    if (!handshake.Wait(afterInterrupt) ||
        handshake.CompletedSequence() < interruptSequence ||
        !interruptDelivered.load(std::memory_order_acquire))
        return Fail("Wait did not service a renderer interrupt handshake");
    handshake.Shutdown();

    std::atomic<bool> deferredInterruptStarted{false};
    std::atomic<bool> releaseDeferredInterrupt{false};
    std::mutex deferredOrderMutex;
    std::vector<int> deferredOrder;
    GpuExecutorBackend deferredBackend{};
    deferredBackend.interrupt = [&](const InterruptCommand&) {
        deferredInterruptStarted.store(true, std::memory_order_release);
        while (!releaseDeferredInterrupt.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::lock_guard<std::mutex> lock(deferredOrderMutex);
        deferredOrder.push_back(1);
        return true;
    };
    deferredBackend.present = [&](const PresentCommand&) {
        std::lock_guard<std::mutex> lock(deferredOrderMutex);
        deferredOrder.push_back(2);
        return true;
    };
    auto deferredExecutor = std::make_unique<AsynchronousGpuExecutor>(
        std::move(deferredBackend), 4, 64);
    OrderedGpuCommandStream deferred(std::move(deferredExecutor));
    if (!deferred.Enqueue(InterruptCommand{}))
        return Fail("ordered stream rejected the interrupt command");
    if (!WaitFor(deferredInterruptStarted))
    {
        releaseDeferredInterrupt.store(true, std::memory_order_release);
        return Fail("ordered stream worker did not start the interrupt command");
    }

    auto enqueuePresent = std::async(std::launch::async, [&] {
        return deferred.Enqueue(PresentCommand{});
    });
    if (enqueuePresent.wait_for(std::chrono::milliseconds(250)) !=
        std::future_status::ready)
    {
        releaseDeferredInterrupt.store(true, std::memory_order_release);
        enqueuePresent.wait();
        return Fail("ordered stream blocked the producer on an in-flight command");
    }
    if (!enqueuePresent.get())
    {
        releaseDeferredInterrupt.store(true, std::memory_order_release);
        return Fail("ordered stream rejected the present command");
    }

    releaseDeferredInterrupt.store(true, std::memory_order_release);
    if (!deferred.Flush())
        return Fail("ordered stream did not complete its explicit barrier");
    {
        std::lock_guard<std::mutex> lock(deferredOrderMutex);
        if (deferredOrder != std::vector<int>({1, 2}))
            return Fail("ordered stream reordered deferred commands");
    }
    if (deferred.Stats().waits != 1)
        return Fail("ordered stream waited outside its explicit barrier");
    deferred.Shutdown();

    std::vector<uint8_t> deferredStoreGuest(64, 0);
    std::atomic<bool> deferredStoreStarted{false};
    std::atomic<bool> releaseDeferredStore{false};
    GpuExecutorBackend deferredStoreBackend{};
    deferredStoreBackend.store = [&](const GuestStoreCommand& command) {
        deferredStoreStarted.store(true, std::memory_order_release);
        while (!releaseDeferredStore.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto* destination = reinterpret_cast<uint32_t*>(
            command.guestBase + command.guestAddress);
        *destination = __builtin_bswap32(command.value);
        return true;
    };
    auto deferredStoreExecutor = std::make_unique<AsynchronousGpuExecutor>(
        std::move(deferredStoreBackend), 4, 64);
    OrderedGpuCommandStream deferredStore(std::move(deferredStoreExecutor));
    if (!deferredStore.Enqueue(GuestStoreCommand{
            deferredStoreGuest.data(), 8, 0x11223344u}))
        return Fail("ordered stream rejected the deferred guest store");
    if (!WaitFor(deferredStoreStarted))
    {
        releaseDeferredStore.store(true, std::memory_order_release);
        return Fail("ordered stream worker did not start the guest store");
    }

    const auto pendingStores = deferredStore.CapturePendingStores();
    GuestMemorySnapshot afterStore;
    if (!afterStore.Capture(deferredStoreGuest.data(), 8, 4))
    {
        releaseDeferredStore.store(true, std::memory_order_release);
        return Fail("could not capture the post-store snapshot regression");
    }
    releaseDeferredStore.store(true, std::memory_order_release);
    if (!deferredStore.Flush())
        return Fail("deferred guest store did not complete at the explicit barrier");
    deferredStore.ApplyPendingStores(afterStore, pendingStores);
    const uint8_t* stored = afterStore.Resolve(8, 4);
    if (!stored || stored[0] != 0x11 || stored[1] != 0x22 ||
        stored[2] != 0x33 || stored[3] != 0x44)
    {
        return Fail("post-store snapshot observed stale guest memory");
    }
    deferredStore.Shutdown();

    std::atomic<bool> tailFailureStarted{false};
    std::atomic<bool> releaseTailFailure{false};
    GpuExecutorBackend tailFailureBackend{};
    tailFailureBackend.present = [&](const PresentCommand&) {
        tailFailureStarted.store(true, std::memory_order_release);
        while (!releaseTailFailure.load(std::memory_order_acquire))
            std::this_thread::yield();
        return false;
    };
    auto tailFailureExecutor = std::make_unique<AsynchronousGpuExecutor>(
        std::move(tailFailureBackend), 4, 64);
    OrderedGpuCommandStream tailFailure(std::move(tailFailureExecutor));
    if (!tailFailure.Enqueue(PresentCommand{}))
        return Fail("tail failure command was rejected before backend execution");
    if (!WaitFor(tailFailureStarted))
    {
        releaseTailFailure.store(true, std::memory_order_release);
        return Fail("ordered stream worker did not start the tail failure command");
    }
    releaseTailFailure.store(true, std::memory_order_release);
    if (tailFailure.Shutdown())
        return Fail("stream shutdown discarded a tail backend failure");

    std::puts("PASS: GPU executors preserve state, order, completion and failures.");
    return 0;
}
