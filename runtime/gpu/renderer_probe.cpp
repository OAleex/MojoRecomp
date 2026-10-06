#include "renderer_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "../host/window.h"
#include "../config/runtime_config.h"
#include "../kernel/klog.h"
#include "pm4.h"
#include "draw_snapshot.h"
#include "gpu_executor.h"
#include "guest_memory_snapshot.h"
#include "shader_cache.h"
#include "vk_presenter.h"
#include "xenos.h"

namespace {

uint64_t g_draws = 0;
uint64_t g_swaps = 0;
std::atomic<uint64_t> g_publishedDraws{0};
std::atomic<uint64_t> g_publishedSwaps{0};
std::vector<uint64_t> g_vertexShaders;
std::vector<uint64_t> g_pixelShaders;
std::array<uint64_t, 8> g_modeDraws{};
uint64_t g_indexedDraws = 0;
uint64_t g_autoDraws = 0;
std::array<bool, 8> g_modeReported{};
bool g_targetReported = false;
bool g_resolveReported = false;
std::vector<uint32_t> g_resolveDestinations;
uint64_t g_windowResizeGeneration = 0;
std::unique_ptr<mojorecomp::gpu::OrderedGpuCommandStream> g_gpuCommandStream;
bool g_asyncExecutorEnabled = false;

bool EnvEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '0';
}

void OnShader(uint32_t type, uint64_t hash, const uint8_t* code, uint32_t sizeDwords)
{
    auto& list = type == 0 ? g_vertexShaders : g_pixelShaders;
    if (std::find(list.begin(), list.end(), hash) == list.end())
        list.push_back(hash);

    const size_t total = g_vertexShaders.size() + g_pixelShaders.size();
    if (total <= 32)
        KLOG("gpu-probe shader %s hash=%016llX size=%u dwords (distinct VS=%zu PS=%zu)\n",
             type == 0 ? "VS" : "PS", static_cast<unsigned long long>(hash),
             sizeDwords, g_vertexShaders.size(), g_pixelShaders.size());

    ShaderCache_OnBind(type, hash, code, sizeDwords);
}

void OnRegister(uint32_t index, uint32_t value)
{
    if (g_gpuCommandStream)
        g_gpuCommandStream->WriteRegister(index, value);
}

void OnRegisterBatch(const uint32_t* indices, const uint32_t* values, std::size_t count)
{
    if (g_gpuCommandStream)
        g_gpuCommandStream->WriteRegisters(indices, values, count);
}

void OnStore(uint8_t* base, uint32_t guestAddress, uint32_t value)
{
    if (!g_gpuCommandStream)
    {
        if (base)
        {
            auto* destination = reinterpret_cast<volatile uint32_t*>(base + guestAddress);
            *destination = __builtin_bswap32(value);
        }
        return;
    }
    const auto sequence = g_gpuCommandStream->Enqueue(mojorecomp::gpu::GuestStoreCommand{
        base, guestAddress, value});
    if (!sequence)
    {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            KLOG("gpu-probe: ordered guest store failed at %08X\n", guestAddress);
    }
}

void OnRenderBarrier()
{
    if (!g_gpuCommandStream)
        return;
    if (!g_gpuCommandStream->Flush())
    {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            KLOG("gpu-probe: explicit renderer barrier failed\n");
    }
}

void OnInterruptCommand()
{
    if (!g_gpuCommandStream)
        return;

    // The worker-side handshake delivers the ISR at this exact FIFO position.
    // The PM4 producer must remain free to service that handshake rather than
    // synchronously waiting for the worker that is waiting on the producer.
    const auto sequence = g_gpuCommandStream->Enqueue(
        mojorecomp::gpu::InterruptCommand{});
    if (!sequence)
    {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            KLOG("gpu-probe: ordered renderer interrupt failed\n");
    }
}

void OnDraw(uint8_t* base, const Pm4Draw& draw)
{
    const uint64_t number = ++g_draws;
    const auto& vs = Pm4_BoundShader(0);
    const auto& ps = Pm4_BoundShader(1);
    const uint32_t* regs = Pm4_Registers();
    const uint32_t mode = regs[xenos::kRbModeControl] & 7u;
    ++g_modeDraws[mode];
    ++(draw.indexed ? g_indexedDraws : g_autoDraws);

    bool report = number <= 8;
    if (!g_modeReported[mode])
    {
        g_modeReported[mode] = true;
        report = true;
    }
    if ((regs[xenos::kRbSurfaceInfo] || regs[xenos::kRbColorInfo]) &&
        !g_targetReported)
    {
        g_targetReported = true;
        report = true;
    }

    if (report)
        KLOG("gpu-probe draw #%llu prim=%u count=%u indexed=%u i32=%u index=%08X "
             "VS=%016llX PS=%016llX mode=%u surf=%08X color=%08X depth=%08X mask=%08X\n",
             static_cast<unsigned long long>(number), draw.primType, draw.indexCount,
             draw.indexed ? 1u : 0u, draw.index32 ? 1u : 0u, draw.indexVa,
             static_cast<unsigned long long>(vs.hash), static_cast<unsigned long long>(ps.hash),
             mode, regs[xenos::kRbSurfaceInfo], regs[xenos::kRbColorInfo],
             regs[xenos::kRbDepthInfo], regs[xenos::kRbColorMask]);

    if (mode == 6 && !g_resolveReported)
    {
        g_resolveReported = true;
        KLOG("gpu-probe first EDRAM resolve: copyCtrl=%08X dest=%08X pitch=%08X info=%08X "
             "clearColor=%08X clearDepth=%08X\n",
             regs[xenos::kRbCopyControl], regs[xenos::kRbCopyDestBase],
             regs[xenos::kRbCopyDestPitch], regs[xenos::kRbCopyDestInfo],
             regs[xenos::kRbColorClear], regs[xenos::kRbDepthClear]);
    }

    if (mode == 6)
    {
        const uint32_t dest = regs[xenos::kRbCopyDestBase];
        if (std::find(g_resolveDestinations.begin(), g_resolveDestinations.end(), dest) ==
            g_resolveDestinations.end())
        {
            g_resolveDestinations.push_back(dest);
            if (g_resolveDestinations.size() <= 16)
                KLOG("gpu-probe resolve destination #%zu: dest=%08X pitch=%08X info=%08X "
                     "copyCtrl=%08X win=%08X..%08X screen=%08X..%08X draw=%llu\n",
                     g_resolveDestinations.size(), dest, regs[xenos::kRbCopyDestPitch],
                     regs[xenos::kRbCopyDestInfo], regs[xenos::kRbCopyControl],
                     regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
                     regs[xenos::kPaScScreenScissorTl], regs[xenos::kPaScScreenScissorBr],
                     static_cast<unsigned long long>(number));
        }
    }

    if (VkPresenter_Active() && g_gpuCommandStream)
    {
        mojorecomp::gpu::DrawCommand command{};
        command.guestBase = base;
        command.draw = draw;
        command.vsHash = vs.hash;
        command.psHash = ps.hash;
        std::unique_ptr<mojorecomp::gpu::GuestMemorySnapshot> snapshot;
        mojorecomp::gpu::PendingGuestStores pendingStores;
        if (g_asyncExecutorEnabled)
        {
            pendingStores = g_gpuCommandStream->CapturePendingStores();
            snapshot = mojorecomp::gpu::BuildDrawSnapshot(
                base, draw, regs, vs.hash, ps.hash);
        }
        if (g_asyncExecutorEnabled && !snapshot)
        {
            static std::atomic<uint64_t> rejected{0};
            const uint64_t count = rejected.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 8)
                KLOG("gpu-probe: async draw rejected because its immutable snapshot is incomplete\n");
            return;
        }
        if (g_asyncExecutorEnabled)
        {
            g_gpuCommandStream->ApplyPendingStores(*snapshot, pendingStores);
            command.guestSnapshot = std::move(snapshot);
        }

        const auto sequence = g_gpuCommandStream->Enqueue(std::move(command));

        if (!sequence)
        {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                KLOG("gpu-probe: draw submission failed\n");
        }
    }
}

void OnSwap(uint8_t*, uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    HostWindow_Pump();
    bool outputMinimized = false;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    if (VkPresenter_Active() &&
        HostWindow_ConsumeResize(g_windowResizeGeneration, outputWidth, outputHeight,
                                 outputMinimized))
    {
        const auto sequence = g_gpuCommandStream
            ? g_gpuCommandStream->Enqueue(mojorecomp::gpu::ResizeCommand{
                  outputMinimized ? 0u : outputWidth,
                  outputMinimized ? 0u : outputHeight})
            : 0;
        if (!g_gpuCommandStream || !sequence)
            KLOG("gpu-probe: Vulkan output resize failed for %ux%u\n",
                 outputWidth, outputHeight);
    }
    if (VkPresenter_Active() && !outputMinimized)
    {
        const auto sequence = g_gpuCommandStream
            ? g_gpuCommandStream->Enqueue(mojorecomp::gpu::PresentCommand{
                  frontBuffer, width, height})
            : 0;
        if (!g_gpuCommandStream || !sequence)
        {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                KLOG("gpu-probe: Vulkan present failed; PM4 execution continues\n");
        }
    }

    const uint64_t number = ++g_swaps;
    g_publishedDraws.store(g_draws, std::memory_order_relaxed);
    g_publishedSwaps.store(g_swaps, std::memory_order_relaxed);
    if (number == 1)
        KLOG("gpu-probe first stream swap: front=%08X %ux%u\n",
             frontBuffer, width, height);

    if (number == 1 || (number % 60u) == 0)
    {
        const auto executorStats = g_gpuCommandStream
            ? g_gpuCommandStream->Stats()
            : mojorecomp::gpu::GpuExecutorStats{};
        KLOG("gpu-probe frame=%llu draws=%llu indexed=%llu auto=%llu "
             "modes=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
             "shaders=%zu VS/%zu PS translated=%llu failed=%llu pm4frames=%llu "
             "vkframes=%llu vkdraws=%llu gpuq=%llu/%llu batches=%llu maxDepth=%llu "
             "waits=%llu backpressure=%.3fms worker=%.3fms regs=%llu/%llu "
             "snapCache=%llu/%llu snapMissing=%llu snapFail=%llu\n",
             static_cast<unsigned long long>(number),
             static_cast<unsigned long long>(g_draws),
             static_cast<unsigned long long>(g_indexedDraws),
             static_cast<unsigned long long>(g_autoDraws),
             static_cast<unsigned long long>(g_modeDraws[0]),
             static_cast<unsigned long long>(g_modeDraws[1]),
             static_cast<unsigned long long>(g_modeDraws[2]),
             static_cast<unsigned long long>(g_modeDraws[3]),
             static_cast<unsigned long long>(g_modeDraws[4]),
             static_cast<unsigned long long>(g_modeDraws[5]),
             static_cast<unsigned long long>(g_modeDraws[6]),
             static_cast<unsigned long long>(g_modeDraws[7]),
             g_vertexShaders.size(), g_pixelShaders.size(),
             static_cast<unsigned long long>(ShaderCache_TranslatedCount()),
             static_cast<unsigned long long>(ShaderCache_FailedCount()),
             static_cast<unsigned long long>(Pm4_FrameCount()),
             static_cast<unsigned long long>(VkPresenter_FrameCount()),
             static_cast<unsigned long long>(VkPresenter_DrawCount()),
             static_cast<unsigned long long>(executorStats.completed),
             static_cast<unsigned long long>(executorStats.submitted),
             static_cast<unsigned long long>(executorStats.submittedBatches),
             static_cast<unsigned long long>(executorStats.maxQueueDepth),
             static_cast<unsigned long long>(executorStats.waits),
             double(executorStats.backpressureNs) / 1.0e6,
             double(executorStats.workerActiveNs) / 1.0e6,
             static_cast<unsigned long long>(executorStats.registerWrites),
             static_cast<unsigned long long>(executorStats.emittedRegisterWrites),
             static_cast<unsigned long long>(mojorecomp::gpu::GuestSnapshotCacheHitCount()),
             static_cast<unsigned long long>(mojorecomp::gpu::GuestSnapshotCacheMissCount()),
             static_cast<unsigned long long>(mojorecomp::gpu::GuestSnapshotMissingReadCount()),
             static_cast<unsigned long long>(
                 mojorecomp::gpu::DrawSnapshotBuildFailureCount()));
    }
}

} // namespace

void RendererProbe_Init()
{
    Pm4_SetShaderSink(OnShader);
    Pm4_SetRegisterSink(OnRegister);
    Pm4_SetRegisterBatchSink(OnRegisterBatch);
    Pm4_SetStoreSink(OnStore);
    Pm4_SetDrawSink(OnDraw);
    Pm4_SetSwapSink(OnSwap);
    KLOG("gpu-probe attached to shader/draw/swap PM4 seams\n");

    const bool wantVulkan = EnvEnabled("MOJORECOMP_VULKAN_PRESENT");
    // A Vulkan renderer cannot draw translated Xenos shaders without shader
    // translation. Keep the explicit translation switch useful for shader-only
    // probes, but make Vulkan presentation self-contained by enabling it here.
    ShaderCache_SetTranslationEnabled(wantVulkan || EnvEnabled("MOJORECOMP_TRANSLATE_SHADERS"));

    if (wantVulkan)
    {
        const bool hidden = EnvEnabled("MOJORECOMP_HEADLESS");
        const auto& runtimeConfig = mojorecomp::config::Get();
        const bool fullscreen =
            runtimeConfig.displayMode == mojorecomp::config::DisplayMode::Fullscreen;
        float aspect = 16.0f / 9.0f;
        switch (runtimeConfig.aspectRatio)
        {
            case mojorecomp::config::AspectRatio::Ultrawide21x9: aspect = 21.0f / 9.0f; break;
            case mojorecomp::config::AspectRatio::SuperUltrawide32x9: aspect = 32.0f / 9.0f; break;
            case mojorecomp::config::AspectRatio::Ratio16x10: aspect = 16.0f / 10.0f; break;
            case mojorecomp::config::AspectRatio::Ratio4x3: aspect = 4.0f / 3.0f; break;
            default: break;
        }
        const uint32_t initialHeight = 720;
        const uint32_t initialWidth = std::max(1u, static_cast<uint32_t>(
            std::lround(static_cast<double>(initialHeight) * aspect)));
        if (HostWindow_Init(initialWidth, initialHeight, hidden, fullscreen) &&
            VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720))
        {
            mojorecomp::gpu::GpuExecutorBackend backend{};
            backend.draw = [](const mojorecomp::gpu::DrawCommand& command,
                              const uint32_t* registers) {
                return VkPresenter_Draw(command.guestBase, command.draw, registers,
                                        command.vsHash, command.psHash);
            };
            backend.present = [](const mojorecomp::gpu::PresentCommand& command) {
                return VkPresenter_Present(command.frontBuffer, command.width, command.height);
            };
            backend.resize = [](const mojorecomp::gpu::ResizeCommand& command) {
                return VkPresenter_SetOutputExtent(command.width, command.height);
            };
            backend.interrupt = [](const mojorecomp::gpu::InterruptCommand&) {
                Pm4_RendererInterruptHandshake();
                return true;
            };
            backend.store = [](const mojorecomp::gpu::GuestStoreCommand& command) {
                if (!command.guestBase)
                    return false;
                auto* destination = reinterpret_cast<volatile uint32_t*>(
                    command.guestBase + command.guestAddress);
                *destination = __builtin_bswap32(command.value);
                return true;
            };
            backend.producerService = [] {
                Pm4_ServiceRendererInterrupts();
            };
            // The ordered asynchronous executor is the production path. Keep
            // a zero-valued environment override as an emergency/debug rollback
            // to the old synchronous submission model.
            const char* asyncOverride = std::getenv("MOJORECOMP_GPU_EXECUTOR_ASYNC");
            g_asyncExecutorEnabled = !asyncOverride || asyncOverride[0] != '0';
            if (g_asyncExecutorEnabled)
            {
                auto executor = std::make_unique<mojorecomp::gpu::AsynchronousGpuExecutor>(
                    std::move(backend));
                g_gpuCommandStream =
                    std::make_unique<mojorecomp::gpu::OrderedGpuCommandStream>(
                        std::move(executor));
                Pm4_SetInterruptCommandSink(OnInterruptCommand);
            }
            else
            {
                auto executor = std::make_unique<mojorecomp::gpu::SynchronousGpuExecutor>(
                    std::move(backend));
                g_gpuCommandStream =
                    std::make_unique<mojorecomp::gpu::OrderedGpuCommandStream>(
                        std::move(executor));
                Pm4_SetInterruptCommandSink(nullptr);
            }
            KLOG("gpu-probe Vulkan executor enabled (%s)\n",
                 g_asyncExecutorEnabled ? "async-batched+ordered-fences" : "synchronous");
        }
        else
            KLOG("gpu-probe Vulkan present seam requested but initialization failed\n");
    }
}

void RendererProbe_Flush()
{
    OnRenderBarrier();
}

void RendererProbe_Shutdown()
{
    Pm4_SetInterruptCommandSink(nullptr);
    Pm4_SetStoreSink(nullptr);
    Pm4_SetRegisterBatchSink(nullptr);
    Pm4_SetRegisterSink(nullptr);
    Pm4_SetDrawSink(nullptr);
    Pm4_SetSwapSink(nullptr);
    if (g_gpuCommandStream)
    {
        if (!g_gpuCommandStream->Shutdown())
            KLOG("gpu-probe: renderer command stream shut down after a backend failure\n");
        g_gpuCommandStream.reset();
    }
    mojorecomp::gpu::GuestSnapshotCacheShutdown();
    mojorecomp::gpu::DrawSnapshotCacheShutdown();
    if (VkPresenter_Active())
        VkPresenter_Shutdown();
    HostWindow_Shutdown();
}

uint64_t RendererProbe_DrawCount() { return g_publishedDraws.load(std::memory_order_relaxed); }
uint64_t RendererProbe_SwapCount() { return g_publishedSwaps.load(std::memory_order_relaxed); }
uint64_t RendererProbe_DistinctVertexShaders() { return g_vertexShaders.size(); }
uint64_t RendererProbe_DistinctPixelShaders() { return g_pixelShaders.size(); }
