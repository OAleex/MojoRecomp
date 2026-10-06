#include "gpu_executor.h"

#include "guest_memory_snapshot.h"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mojorecomp::gpu {
namespace {

bool ExecuteBackend(const GpuExecutorBackend& backend,
                    const RenderCommand& command,
                    const uint32_t* registers)
{
    return std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, DrawCommand>)
        {
            ScopedGuestMemorySnapshot snapshotScope(typed.guestSnapshot.get());
            const bool ok = backend.draw ? backend.draw(typed, registers) : true;
            return ok && !GuestSnapshotReadFailed();
        }
        else if constexpr (std::is_same_v<T, PresentCommand>)
            return backend.present ? backend.present(typed) : true;
        else if constexpr (std::is_same_v<T, ResizeCommand>)
            return backend.resize ? backend.resize(typed) : true;
        else if constexpr (std::is_same_v<T, InterruptCommand>)
            return backend.interrupt ? backend.interrupt(typed) : true;
        else
            return backend.store ? backend.store(typed) : true;
    }, command);
}

void UpdateMax(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t observed = target.load(std::memory_order_relaxed);
    while (observed < value &&
           !target.compare_exchange_weak(observed, value, std::memory_order_relaxed))
    {
    }
}

} // namespace

SynchronousGpuExecutor::SynchronousGpuExecutor(GpuExecutorBackend backend)
    : backend_(std::move(backend))
{
}

void SynchronousGpuExecutor::WriteRegister(uint32_t index, uint32_t value)
{
    if (shutdown_ || index >= registers_.size())
        return;
    registers_[index] = value;
    ++registerWrites_;
}

void SynchronousGpuExecutor::WriteRegisters(const uint32_t* indices,
                                            const uint32_t* values,
                                            size_t count)
{
    if (shutdown_ || !indices || !values)
        return;
    for (size_t i = 0; i < count; ++i)
    {
        if (indices[i] >= registers_.size())
            continue;
        registers_[indices[i]] = values[i];
        ++registerWrites_;
    }
}

bool SynchronousGpuExecutor::Execute(const RenderCommand& command)
{
    return ExecuteBackend(backend_, command, registers_.data());
}

GpuSequence SynchronousGpuExecutor::Submit(RenderCommand command)
{
    if (shutdown_)
        return 0;
    const GpuSequence sequence = nextSequence_++;
    if (!Execute(command) && firstFailedSequence_ == 0)
        firstFailedSequence_ = sequence;
    completedSequence_ = sequence;
    return sequence;
}

bool SynchronousGpuExecutor::Wait(GpuSequence sequence)
{
    if (!sequence || sequence > completedSequence_)
        return false;
    return firstFailedSequence_ == 0 || firstFailedSequence_ > sequence;
}

GpuSequence SynchronousGpuExecutor::LatestSubmittedSequence() const noexcept
{
    return completedSequence_;
}

GpuSequence SynchronousGpuExecutor::CompletedSequence() const noexcept
{
    return completedSequence_;
}

GpuSequence SynchronousGpuExecutor::FirstFailedSequence() const noexcept
{
    return firstFailedSequence_;
}

PendingGuestStores SynchronousGpuExecutor::CapturePendingStores()
{
    return {};
}

GpuExecutorStats SynchronousGpuExecutor::Stats() const noexcept
{
    return {completedSequence_, completedSequence_, 0, 0, 0,
            registerWrites_, registerWrites_, completedSequence_ ? 1u : 0u,
            completedSequence_ ? 1u : 0u};
}

void SynchronousGpuExecutor::Shutdown()
{
    shutdown_ = true;
}

AsynchronousGpuExecutor::AsynchronousGpuExecutor(GpuExecutorBackend backend,
                                                 size_t queueCapacity,
                                                 size_t batchEntryLimit)
    : backend_(std::move(backend)),
      queueCapacity_(std::max<size_t>(1, queueCapacity)),
      batchEntryLimit_(std::max<size_t>(1, batchEntryLimit)),
      pendingRegisterValues_(kGpuRegisterCount, 0),
      pendingRegisterDirty_(kGpuRegisterCount, 0)
{
    pendingBatch_.entries.reserve(batchEntryLimit_);
    pendingRegisterIndices_.reserve(kGpuRegisterCount);
    worker_ = std::thread([this] { WorkerMain(); });
}

AsynchronousGpuExecutor::~AsynchronousGpuExecutor()
{
    Shutdown();
}

void AsynchronousGpuExecutor::WriteRegister(uint32_t index, uint32_t value)
{
    if (!accepting_.load(std::memory_order_acquire) || index >= kGpuRegisterCount)
        return;
    registerWrites_.fetch_add(1, std::memory_order_relaxed);
    pendingRegisterValues_[index] = value;
    if (!pendingRegisterDirty_[index])
    {
        pendingRegisterDirty_[index] = 1;
        pendingRegisterIndices_.push_back(index);
    }
}

void AsynchronousGpuExecutor::WriteRegisters(const uint32_t* indices,
                                             const uint32_t* values,
                                             size_t count)
{
    if (!indices || !values || !count ||
        !accepting_.load(std::memory_order_acquire))
        return;

    uint64_t accepted = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t index = indices[i];
        if (index >= kGpuRegisterCount)
            continue;
        ++accepted;
        pendingRegisterValues_[index] = values[i];
        if (!pendingRegisterDirty_[index])
        {
            pendingRegisterDirty_[index] = 1;
            pendingRegisterIndices_.push_back(index);
        }
    }
    registerWrites_.fetch_add(accepted, std::memory_order_relaxed);
}

GpuSequence AsynchronousGpuExecutor::Submit(RenderCommand command)
{
    if (!accepting_.load(std::memory_order_acquire))
        return 0;
    if (const auto* draw = std::get_if<DrawCommand>(&command);
        draw && !draw->guestSnapshot)
        return 0;
    if (!AppendPendingRegisterWrites())
        return 0;

    const auto* store = std::get_if<GuestStoreCommand>(&command);
    const bool publishImmediately =
        std::holds_alternative<InterruptCommand>(command) ||
        std::holds_alternative<PresentCommand>(command) ||
        std::holds_alternative<ResizeCommand>(command) ||
        store != nullptr;
    const GpuSequence sequence = nextSequence_++;
    if (store)
    {
        const GpuSequence completed = CompletedSequence();
        pendingStores_.erase(
            std::remove_if(pendingStores_.begin(), pendingStores_.end(),
                           [&](const PendingStore& pending) {
                               return pending.sequence <= completed;
                           }),
            pendingStores_.end());
        auto existing = std::find_if(
            pendingStores_.begin(), pendingStores_.end(),
            [&](const PendingStore& pending) {
                return pending.address == store->guestAddress;
            });
        if (existing != pendingStores_.end())
            *existing = {store->guestAddress, store->value, sequence};
        else
            pendingStores_.push_back({store->guestAddress, store->value, sequence});
    }
    pendingBatch_.entries.emplace_back(SequencedCommand{sequence, std::move(command)});
    pendingBatch_.lastSequence = sequence;
    latestSubmitted_.store(sequence, std::memory_order_release);
    if (publishImmediately || pendingBatch_.entries.size() >= batchEntryLimit_)
        FlushPendingBatch();
    return sequence;
}

bool AsynchronousGpuExecutor::AppendPendingRegisterWrites()
{
    if (pendingRegisterIndices_.empty())
        return true;

    for (uint32_t index : pendingRegisterIndices_)
    {
        if (pendingBatch_.entries.size() >= batchEntryLimit_ && !FlushPendingBatch())
            return false;
        pendingBatch_.entries.emplace_back(
            RegisterWriteCommand{index, pendingRegisterValues_[index]});
        pendingRegisterDirty_[index] = 0;
        emittedRegisterWrites_.fetch_add(1, std::memory_order_relaxed);
    }
    pendingRegisterIndices_.clear();
    return true;
}

bool AsynchronousGpuExecutor::FlushPendingBatch()
{
    if (pendingBatch_.entries.empty())
        return true;
    if (!accepting_.load(std::memory_order_acquire))
        return false;

    const auto waitStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (accepting_.load(std::memory_order_acquire) &&
           queue_.size() >= queueCapacity_)
    {
        progressCv_.wait_for(lock, std::chrono::microseconds(100));
        if (queue_.size() >= queueCapacity_ && backend_.producerService)
        {
            lock.unlock();
            backend_.producerService();
            lock.lock();
        }
    }
    const auto waitEnd = std::chrono::steady_clock::now();
    if (!accepting_.load(std::memory_order_acquire))
        return false;

    backpressureNs_.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitEnd - waitStart).count()),
        std::memory_order_relaxed);

    queue_.push_back(std::move(pendingBatch_));
    pendingBatch_ = {};
    pendingBatch_.entries.reserve(batchEntryLimit_);
    submittedBatches_.fetch_add(1, std::memory_order_relaxed);
    UpdateMax(maxQueueDepth_, queue_.size());
    lock.unlock();
    workCv_.notify_one();
    return true;
}

bool AsynchronousGpuExecutor::Wait(GpuSequence sequence)
{
    if (!sequence || sequence > LatestSubmittedSequence())
        return false;
    if (!FlushPendingBatch())
        return false;

    waits_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(mutex_);
    while (completedSequence_.load(std::memory_order_acquire) < sequence &&
           (accepting_.load(std::memory_order_acquire) || !queue_.empty()))
    {
        progressCv_.wait_for(lock, std::chrono::microseconds(100));
        if (completedSequence_.load(std::memory_order_acquire) >= sequence)
            break;
        if (backend_.producerService)
        {
            lock.unlock();
            backend_.producerService();
            lock.lock();
        }
    }
    const GpuSequence completed = completedSequence_.load(std::memory_order_acquire);
    const GpuSequence failed = firstFailedSequence_.load(std::memory_order_acquire);
    return completed >= sequence && (!failed || failed > sequence);
}

GpuSequence AsynchronousGpuExecutor::LatestSubmittedSequence() const noexcept
{
    return latestSubmitted_.load(std::memory_order_acquire);
}

GpuSequence AsynchronousGpuExecutor::CompletedSequence() const noexcept
{
    return completedSequence_.load(std::memory_order_acquire);
}

GpuSequence AsynchronousGpuExecutor::FirstFailedSequence() const noexcept
{
    return firstFailedSequence_.load(std::memory_order_acquire);
}

PendingGuestStores AsynchronousGpuExecutor::CapturePendingStores()
{
    if (pendingStores_.empty())
        return {};

    const GpuSequence completed = CompletedSequence();
    pendingStores_.erase(
        std::remove_if(pendingStores_.begin(), pendingStores_.end(),
                       [&](const PendingStore& pending) {
                           return pending.sequence <= completed;
                       }),
        pendingStores_.end());

    PendingGuestStores captured;
    captured.reserve(pendingStores_.size());
    for (const PendingStore& pending : pendingStores_)
        captured.push_back({pending.address, pending.value});
    return captured;
}

namespace {

void ApplyPendingGuestStores(GuestMemorySnapshot& snapshot,
                             const PendingGuestStores& stores)
{
    for (const PendingGuestStore& pending : stores)
    {
        const uint32_t stored = __builtin_bswap32(pending.value);
        snapshot.Overlay(pending.address, &stored, sizeof(stored));

        if (pending.address >= 0xA0000000u)
        {
            const uint32_t physical = pending.address & 0x1FFFFFFFu;
            for (uint32_t alias : kGuestMemoryPhysicalViews)
            {
                const uint32_t address = alias | physical;
                if (address != pending.address)
                    snapshot.Overlay(address, &stored, sizeof(stored));
            }
        }
    }
}

} // namespace

GpuExecutorStats AsynchronousGpuExecutor::Stats() const noexcept
{
    return {
        latestSubmitted_.load(std::memory_order_relaxed),
        completedSequence_.load(std::memory_order_relaxed),
        waits_.load(std::memory_order_relaxed),
        backpressureNs_.load(std::memory_order_relaxed),
        workerActiveNs_.load(std::memory_order_relaxed),
        registerWrites_.load(std::memory_order_relaxed),
        emittedRegisterWrites_.load(std::memory_order_relaxed),
        submittedBatches_.load(std::memory_order_relaxed),
        maxQueueDepth_.load(std::memory_order_relaxed),
    };
}

bool AsynchronousGpuExecutor::Execute(const RenderCommand& command)
{
    return ExecuteBackend(backend_, command, registers_.data());
}

void AsynchronousGpuExecutor::WorkerMain()
{
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"MojoRecomp Vulkan");
#endif

    for (;;)
    {
        CommandBatch batch{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workCv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty())
            {
                if (stop_)
                    break;
                continue;
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
            progressCv_.notify_all();
        }

        const auto activeStart = std::chrono::steady_clock::now();
        for (const BatchEntry& entry : batch.entries)
        {
            if (const auto* write = std::get_if<RegisterWriteCommand>(&entry))
            {
                if (write->index < registers_.size())
                    registers_[write->index] = write->value;
                continue;
            }

            const auto& sequenced = std::get<SequencedCommand>(entry);
            const bool ok = Execute(sequenced.command);
            if (!ok)
            {
                GpuSequence expected = 0;
                firstFailedSequence_.compare_exchange_strong(
                    expected, sequenced.sequence, std::memory_order_release,
                    std::memory_order_relaxed);
            }
            completedSequence_.store(sequenced.sequence, std::memory_order_release);
        }
        workerActiveNs_.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - activeStart).count()),
            std::memory_order_relaxed);
        progressCv_.notify_all();
    }
}

void AsynchronousGpuExecutor::Shutdown()
{
    if (!worker_.joinable())
        return;

    const GpuSequence submitted = LatestSubmittedSequence();
    if (submitted)
        Wait(submitted);
    else
        FlushPendingBatch();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_.store(false, std::memory_order_release);
        stop_ = true;
    }
    workCv_.notify_all();
    progressCv_.notify_all();
    if (worker_.get_id() != std::this_thread::get_id())
        worker_.join();
}

OrderedGpuCommandStream::OrderedGpuCommandStream(
    std::unique_ptr<GpuExecutor> executor)
    : executor_(std::move(executor))
{
}

void OrderedGpuCommandStream::WriteRegister(uint32_t index, uint32_t value)
{
    if (executor_)
        executor_->WriteRegister(index, value);
}

void OrderedGpuCommandStream::WriteRegisters(const uint32_t* indices,
                                             const uint32_t* values,
                                             size_t count)
{
    if (executor_)
        executor_->WriteRegisters(indices, values, count);
}

GpuSequence OrderedGpuCommandStream::Enqueue(RenderCommand command)
{
    if (!executor_)
        return 0;
    const GpuSequence sequence = executor_->Submit(std::move(command));
    const GpuSequence failed = executor_->FirstFailedSequence();
    return sequence && (!failed || failed > sequence) ? sequence : 0;
}

PendingGuestStores OrderedGpuCommandStream::CapturePendingStores()
{
    return executor_ ? executor_->CapturePendingStores() : PendingGuestStores{};
}

void OrderedGpuCommandStream::ApplyPendingStores(
    GuestMemorySnapshot& snapshot, const PendingGuestStores& stores) const
{
    ApplyPendingGuestStores(snapshot, stores);
}

bool OrderedGpuCommandStream::Flush()
{
    if (!executor_)
        return true;
    const GpuSequence sequence = executor_->LatestSubmittedSequence();
    return !sequence || executor_->Wait(sequence);
}

GpuExecutorStats OrderedGpuCommandStream::Stats() const noexcept
{
    return executor_ ? executor_->Stats() : GpuExecutorStats{};
}

bool OrderedGpuCommandStream::Shutdown()
{
    if (!executor_)
        return true;
    executor_->Shutdown();
    return executor_->FirstFailedSequence() == 0;
}

} // namespace mojorecomp::gpu
