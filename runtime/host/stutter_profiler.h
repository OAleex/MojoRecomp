#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace mojorecomp::host {

enum class StutterCause : uint8_t
{
    Unattributed,
    Pipeline,
    Texture,
    FenceWait,
    Present,
    Acquire,
    PresentPrep,
    QueueSubmit,
    QueuePresent,
    Pm4,
    Draw,
    Resolve,
    Attachment,
    Constants,
    Vertex,
};

struct FrameWorkSample
{
    uint64_t frameIndex = 0;
    uint64_t presentNs = 0;
    uint64_t acquireNs = 0;
    uint64_t presentPrepNs = 0;
    uint64_t queueSubmitNs = 0;
    uint64_t queuePresentNs = 0;
    uint64_t pm4ExecuteNs = 0;
    uint64_t pm4DrawSinkNs = 0;
    uint64_t pm4ParserNs = 0;
    uint64_t fenceWaitNs = 0;
    uint64_t drawNs = 0;
    uint64_t attachmentNs = 0;
    uint64_t textureNs = 0;
    uint64_t pipelineNs = 0;
    uint64_t constantsNs = 0;
    uint64_t vertexNs = 0;
    uint64_t resolveNs = 0;
    uint64_t edramPipelineNs = 0;
    uint64_t edramTransferNs = 0;
    uint64_t pipelineCreates = 0;
    uint64_t textureUploadBytes = 0;
    uint64_t textureRefreshBytes = 0;
    uint64_t vertexUploadBytes = 0;
    uint64_t drawCalls = 0;
    uint64_t resolveCalls = 0;
    uint64_t pm4ExecuteCalls = 0;
};

struct StutterReport
{
    FrameWorkSample work{};
    uint64_t frameNs = 0;
    uint64_t outsidePresenterNs = 0;
    uint64_t presenterExclusiveNs = 0;
    uint64_t unattributedOutsideNs = 0;
    StutterCause cause = StutterCause::Unattributed;
};

class StutterProfiler
{
public:
    using Clock = std::chrono::steady_clock;

    explicit StutterProfiler(
        std::chrono::nanoseconds threshold = std::chrono::milliseconds(100)) noexcept;

    void SetThreshold(std::chrono::nanoseconds threshold) noexcept;
    std::chrono::nanoseconds Threshold() const noexcept;
    void Reset() noexcept;

    std::optional<StutterReport> Observe(
        Clock::time_point completion,
        const FrameWorkSample& work) noexcept;

private:
    std::chrono::nanoseconds threshold_;
    Clock::time_point previousCompletion_{};
    bool havePreviousCompletion_ = false;
};

const char* StutterCauseName(StutterCause cause) noexcept;

} // namespace mojorecomp::host
