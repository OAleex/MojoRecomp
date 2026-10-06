#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include "pm4.h"

namespace mojorecomp::gpu {

class GuestMemorySnapshot;

using GpuSequence = uint64_t;
constexpr uint32_t kGpuRegisterCount = 0x8000u;

struct RegisterWriteCommand
{
    uint32_t index = 0;
    uint32_t value = 0;
};

struct DrawCommand
{
    uint8_t* guestBase = nullptr;
    Pm4Draw draw{};
    uint64_t vsHash = 0;
    uint64_t psHash = 0;
    std::unique_ptr<const GuestMemorySnapshot> guestSnapshot;
};

struct PresentCommand
{
    uint32_t frontBuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct ResizeCommand
{
    uint32_t width = 0;
    uint32_t height = 0;
};

struct InterruptCommand
{
};

struct GuestStoreCommand
{
    uint8_t* guestBase = nullptr;
    uint32_t guestAddress = 0;
    uint32_t value = 0;
};

struct PendingGuestStore
{
    uint32_t address = 0;
    uint32_t value = 0;
};

using PendingGuestStores = std::vector<PendingGuestStore>;

using RenderCommand = std::variant<DrawCommand, PresentCommand, ResizeCommand,
                                   InterruptCommand, GuestStoreCommand>;

struct GpuExecutorBackend
{
    std::function<bool(const DrawCommand&, const uint32_t*)> draw;
    std::function<bool(const PresentCommand&)> present;
    std::function<bool(const ResizeCommand&)> resize;
    std::function<bool(const InterruptCommand&)> interrupt;
    std::function<bool(const GuestStoreCommand&)> store;
    // Runs only on the producer/PM4 thread while it is applying backpressure.
    // Used to service renderer-originated handshakes without allowing a
    // full-queue W<->D deadlock.
    std::function<void()> producerService;
};

struct GpuExecutorStats
{
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t waits = 0;
    uint64_t backpressureNs = 0;
    uint64_t workerActiveNs = 0;
    uint64_t registerWrites = 0;
    uint64_t emittedRegisterWrites = 0;
    uint64_t submittedBatches = 0;
    uint64_t maxQueueDepth = 0;
};

class GpuExecutor
{
public:
    virtual ~GpuExecutor() = default;

    virtual void WriteRegister(uint32_t index, uint32_t value) = 0;
    virtual void WriteRegisters(const uint32_t* indices, const uint32_t* values,
                                size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            WriteRegister(indices[i], values[i]);
    }
    virtual GpuSequence Submit(RenderCommand command) = 0;
    virtual bool Wait(GpuSequence sequence) = 0;
    virtual GpuSequence LatestSubmittedSequence() const noexcept = 0;
    virtual GpuSequence CompletedSequence() const noexcept = 0;
    virtual GpuSequence FirstFailedSequence() const noexcept = 0;
    virtual PendingGuestStores CapturePendingStores() = 0;
    virtual GpuExecutorStats Stats() const noexcept = 0;
    virtual void Shutdown() = 0;
};

class SynchronousGpuExecutor final : public GpuExecutor
{
public:
    explicit SynchronousGpuExecutor(GpuExecutorBackend backend);

    void WriteRegister(uint32_t index, uint32_t value) override;
    void WriteRegisters(const uint32_t* indices, const uint32_t* values,
                        size_t count) override;
    GpuSequence Submit(RenderCommand command) override;
    bool Wait(GpuSequence sequence) override;
    GpuSequence LatestSubmittedSequence() const noexcept override;
    GpuSequence CompletedSequence() const noexcept override;
    GpuSequence FirstFailedSequence() const noexcept override;
    PendingGuestStores CapturePendingStores() override;
    GpuExecutorStats Stats() const noexcept override;
    void Shutdown() override;

private:
    bool Execute(const RenderCommand& command);

    GpuExecutorBackend backend_;
    std::array<uint32_t, kGpuRegisterCount> registers_{};
    GpuSequence nextSequence_ = 1;
    GpuSequence completedSequence_ = 0;
    GpuSequence firstFailedSequence_ = 0;
    uint64_t registerWrites_ = 0;
    bool shutdown_ = false;
};

class AsynchronousGpuExecutor final : public GpuExecutor
{
public:
    explicit AsynchronousGpuExecutor(GpuExecutorBackend backend,
                                     size_t queueCapacity = 32,
                                     size_t batchEntryLimit = 2048);
    ~AsynchronousGpuExecutor() override;

    void WriteRegister(uint32_t index, uint32_t value) override;
    void WriteRegisters(const uint32_t* indices, const uint32_t* values,
                        size_t count) override;
    GpuSequence Submit(RenderCommand command) override;
    bool Wait(GpuSequence sequence) override;
    GpuSequence LatestSubmittedSequence() const noexcept override;
    GpuSequence CompletedSequence() const noexcept override;
    GpuSequence FirstFailedSequence() const noexcept override;
    PendingGuestStores CapturePendingStores() override;
    GpuExecutorStats Stats() const noexcept override;
    void Shutdown() override;

private:
    struct SequencedCommand
    {
        GpuSequence sequence = 0;
        RenderCommand command;
    };
    using BatchEntry = std::variant<RegisterWriteCommand, SequencedCommand>;
    struct CommandBatch
    {
        std::vector<BatchEntry> entries;
        GpuSequence lastSequence = 0;
    };
    struct PendingStore
    {
        uint32_t address = 0;
        uint32_t value = 0;
        GpuSequence sequence = 0;
    };
    bool Execute(const RenderCommand& command);
    bool AppendPendingRegisterWrites();
    bool FlushPendingBatch();
    void WorkerMain();

    GpuExecutorBackend backend_;
    const size_t queueCapacity_;
    const size_t batchEntryLimit_;
    std::array<uint32_t, kGpuRegisterCount> registers_{};
    std::vector<uint32_t> pendingRegisterValues_;
    std::vector<uint8_t> pendingRegisterDirty_;
    std::vector<uint32_t> pendingRegisterIndices_;
    std::vector<PendingStore> pendingStores_;
    CommandBatch pendingBatch_;

    mutable std::mutex mutex_;
    std::condition_variable workCv_;
    std::condition_variable progressCv_;
    std::deque<CommandBatch> queue_;
    std::thread worker_;
    std::atomic<bool> accepting_{true};
    bool stop_ = false;
    GpuSequence nextSequence_ = 1;
    std::atomic<GpuSequence> latestSubmitted_{0};
    std::atomic<GpuSequence> completedSequence_{0};
    std::atomic<GpuSequence> firstFailedSequence_{0};

    std::atomic<uint64_t> waits_{0};
    std::atomic<uint64_t> backpressureNs_{0};
    std::atomic<uint64_t> workerActiveNs_{0};
    std::atomic<uint64_t> registerWrites_{0};
    std::atomic<uint64_t> emittedRegisterWrites_{0};
    std::atomic<uint64_t> submittedBatches_{0};
    std::atomic<uint64_t> maxQueueDepth_{0};
};

// Producer-facing command stream. Ordinary commands are enqueued without a
// completion wait, although bounded-queue backpressure may briefly block a
// saturated producer. Only Flush waits for completion of a specific sequence.
// Keeping Wait out of the ordinary submission surface prevents a caller from
// accidentally serializing the asynchronous executor command by command while
// preserving exact FIFO execution order.
class OrderedGpuCommandStream
{
public:
    explicit OrderedGpuCommandStream(std::unique_ptr<GpuExecutor> executor);

    void WriteRegister(uint32_t index, uint32_t value);
    void WriteRegisters(const uint32_t* indices, const uint32_t* values,
                        size_t count);
    GpuSequence Enqueue(RenderCommand command);
    PendingGuestStores CapturePendingStores();
    void ApplyPendingStores(GuestMemorySnapshot& snapshot,
                            const PendingGuestStores& stores) const;
    bool Flush();
    GpuExecutorStats Stats() const noexcept;
    bool Shutdown();

private:
    std::unique_ptr<GpuExecutor> executor_;
};

} // namespace mojorecomp::gpu
