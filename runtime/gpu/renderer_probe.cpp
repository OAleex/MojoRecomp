#include "renderer_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "../host/window.h"
#include "../config/runtime_config.h"
#include "../kernel/klog.h"
#include "pm4.h"
#include "shader_cache.h"
#include "vk_presenter.h"
#include "xenos.h"

namespace {

std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_swaps{0};
std::vector<uint64_t> g_vertexShaders;
std::vector<uint64_t> g_pixelShaders;
std::array<std::atomic<uint64_t>, 8> g_modeDraws{};
std::atomic<uint64_t> g_indexedDraws{0};
std::atomic<uint64_t> g_autoDraws{0};
std::array<std::atomic<bool>, 8> g_modeReported{};
std::atomic<bool> g_targetReported{false};
std::atomic<bool> g_resolveReported{false};
std::vector<uint32_t> g_resolveDestinations;
uint64_t g_windowResizeGeneration = 0;

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

void OnDraw(uint8_t* base, const Pm4Draw& draw)
{
    const uint64_t number = g_draws.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto& vs = Pm4_BoundShader(0);
    const auto& ps = Pm4_BoundShader(1);
    const uint32_t* regs = Pm4_Registers();
    const uint32_t mode = regs[xenos::kRbModeControl] & 7u;
    g_modeDraws[mode].fetch_add(1, std::memory_order_relaxed);
    (draw.indexed ? g_indexedDraws : g_autoDraws).fetch_add(1, std::memory_order_relaxed);

    bool report = number <= 8;
    if (!g_modeReported[mode].exchange(true))
        report = true;
    if ((regs[xenos::kRbSurfaceInfo] || regs[xenos::kRbColorInfo]) &&
        !g_targetReported.exchange(true))
        report = true;

    if (report)
        KLOG("gpu-probe draw #%llu prim=%u count=%u indexed=%u i32=%u index=%08X "
             "VS=%016llX PS=%016llX mode=%u surf=%08X color=%08X depth=%08X mask=%08X\n",
             static_cast<unsigned long long>(number), draw.primType, draw.indexCount,
             draw.indexed ? 1u : 0u, draw.index32 ? 1u : 0u, draw.indexVa,
             static_cast<unsigned long long>(vs.hash), static_cast<unsigned long long>(ps.hash),
             mode, regs[xenos::kRbSurfaceInfo], regs[xenos::kRbColorInfo],
             regs[xenos::kRbDepthInfo], regs[xenos::kRbColorMask]);

    if (mode == 6 && !g_resolveReported.exchange(true))
        KLOG("gpu-probe first EDRAM resolve: copyCtrl=%08X dest=%08X pitch=%08X info=%08X "
             "clearColor=%08X clearDepth=%08X\n",
             regs[xenos::kRbCopyControl], regs[xenos::kRbCopyDestBase],
             regs[xenos::kRbCopyDestPitch], regs[xenos::kRbCopyDestInfo],
             regs[xenos::kRbColorClear], regs[xenos::kRbDepthClear]);

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

    if (VkPresenter_Active())
        VkPresenter_Draw(base, draw, regs, vs.hash, ps.hash);
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
        if (!VkPresenter_SetOutputExtent(outputMinimized ? 0u : outputWidth,
                                         outputMinimized ? 0u : outputHeight))
            KLOG("gpu-probe: Vulkan output resize failed for %ux%u\n",
                 outputWidth, outputHeight);
    }
    if (VkPresenter_Active() && !outputMinimized &&
        !VkPresenter_Present(frontBuffer, width, height))
    {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            KLOG("gpu-probe: Vulkan present failed; PM4 execution continues\n");
    }

    const uint64_t number = g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (number == 1)
        KLOG("gpu-probe first stream swap: front=%08X %ux%u\n",
             frontBuffer, width, height);

    if (number == 1 || (number % 60u) == 0)
        KLOG("gpu-probe frame=%llu draws=%llu indexed=%llu auto=%llu "
             "modes=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
             "shaders=%zu VS/%zu PS translated=%llu failed=%llu pm4frames=%llu "
             "vkframes=%llu vkdraws=%llu\n",
             static_cast<unsigned long long>(number),
             static_cast<unsigned long long>(g_draws.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_indexedDraws.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_autoDraws.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[0].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[1].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[2].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[3].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[4].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[5].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[6].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_modeDraws[7].load(std::memory_order_relaxed)),
             g_vertexShaders.size(), g_pixelShaders.size(),
             static_cast<unsigned long long>(ShaderCache_TranslatedCount()),
             static_cast<unsigned long long>(ShaderCache_FailedCount()),
             static_cast<unsigned long long>(Pm4_FrameCount()),
             static_cast<unsigned long long>(VkPresenter_FrameCount()),
             static_cast<unsigned long long>(VkPresenter_DrawCount()));
}

} // namespace

void RendererProbe_Init()
{
    Pm4_SetShaderSink(OnShader);
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
            KLOG("gpu-probe Vulkan present seam enabled\n");
        else
            KLOG("gpu-probe Vulkan present seam requested but initialization failed\n");
    }
}

uint64_t RendererProbe_DrawCount() { return g_draws.load(std::memory_order_relaxed); }
uint64_t RendererProbe_SwapCount() { return g_swaps.load(std::memory_order_relaxed); }
uint64_t RendererProbe_DistinctVertexShaders() { return g_vertexShaders.size(); }
uint64_t RendererProbe_DistinctPixelShaders() { return g_pixelShaders.size(); }
