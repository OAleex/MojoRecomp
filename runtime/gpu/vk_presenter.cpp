#include "vk_presenter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#endif

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../debug_mode.h"
#include "../debug_overlay_layout.h"
#include "../config/runtime_config.h"
#include "../kernel/klog.h"
#include "primitive_utils.h"
#include "../host/host_paths.h"
#include "../host/system_font_atlas.h"
#include "../subtitles/subtitle_runtime.h"
#include "pm4.h"
#include "shader_cache.h"
#include "shader_translator.h"
#include "texture_abi.h"
#include "render_policy.h"
#include "xenos.h"

namespace {

constexpr uint32_t kMaxFramesInFlight = 3;
// Level startup can publish a large batch of persistent guest textures before
// the first gameplay present. 64 MiB was enough for the frontend, but the first
// real scene can legitimately cross that mark in a single command buffer and
// would then lose every later draw in the frame. Keep a larger per-frame staging
// slice so initial texture residency does not turn into a black frame.
constexpr VkDeviceSize kFrameUploadBytes = 128ull * 1024ull * 1024ull;
constexpr VkDeviceSize kUploadBytes = kFrameUploadBytes * kMaxFramesInFlight;
constexpr uint32_t kGuestPhysicalBase = 0xA0000000u;
constexpr uint32_t kGuestPhysicalEnd = 0xBFFF0000u;
constexpr uint32_t kVsConstBytes = 4096;
constexpr uint32_t kPsConstBytes = 4096;
constexpr uint32_t kSharedBytes = 4096;
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedSwappedTexcoords = 260;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedTextureSampleScales = 288;
constexpr uint32_t kSharedPosScale = 352;
constexpr uint32_t kSharedPosOffset = 360;
constexpr uint32_t kSharedPosDepthTransform = 368;
constexpr uint32_t kSharedDepthOutputScale = 376;
constexpr uint32_t kSharedLoopConstants = 384;
constexpr uint32_t kSharedBoolFile = 512;
constexpr uint32_t kSharedAspectScale = 544;
// XenosRecomp shader_common.h spaces 0..3, binding 0. Each SharedConstants
// index bank is 64 bytes; reject fetch slots >=16 rather than aliasing banks.
constexpr uint32_t kTextureSlots = mojorecomp::texture_abi::kSlots;
// Frontend alone reaches ~160 unique bundles and the first real 3D scene goes
// past 256 almost immediately. Keep enough persistent sets for an early level;
// the lookup below is hashed so raising this no longer makes every draw scan a
// large linear list.
constexpr uint32_t kDescriptorBundles = 2048;
VkDescriptorSetLayout g_textureLayouts[4]{};
VkDescriptorPool g_texturePool = VK_NULL_HANDLE;

// Host-side 2x MSAA resolve helper. The Vega 8 driver faults in both dynamic
// rendering resolves and vkCmdResolveImage for this path, so average color
// samples 0/1 in a tiny fullscreen shader instead.
VkDescriptorSetLayout g_msaaResolveSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_msaaResolvePool = VK_NULL_HANDLE;
VkPipelineLayout g_msaaResolvePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_msaaResolvePipeline = VK_NULL_HANDLE;
VkShaderModule g_msaaResolveVs = VK_NULL_HANDLE;
VkShaderModule g_msaaResolvePs = VK_NULL_HANDLE;
// Depth MSAA fallback for devices that can't resolve depth with SAMPLE_ZERO
// through dynamic rendering. Xenos depth resolves select sample 0, so unlike
// color this is a straight per-pixel sample load rather than an average.
VkDescriptorSetLayout g_depthMsaaResolveSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_depthMsaaResolvePool = VK_NULL_HANDLE;
VkPipelineLayout g_depthMsaaResolvePipelineLayout = VK_NULL_HANDLE;
VkShaderModule g_depthMsaaResolveVs = VK_NULL_HANDLE;
VkShaderModule g_depthMsaaResolvePs = VK_NULL_HANDLE;
VkPipeline g_depthMsaaResolveUnormPipeline = VK_NULL_HANDLE;
VkPipeline g_depthMsaaResolveFloatPipeline = VK_NULL_HANDLE;
// Guest pixel shaders are ignored in Xenos DepthOnly mode. D24FS8 still needs
// the host precision boundary, so this tiny renderer-owned fragment shader is
// used only to quantize rasterized Z to guest float20e4 without executing any
// guest texture fetches, kills or color logic.
VkShaderModule g_float24DepthOnlyPs = VK_NULL_HANDLE;
bool g_float24DepthOnlyPsAttempted = false;
struct SampledImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
SampledImage g_dummyTextures[3];
SampledImage g_neutralBlurTexture;
VkSampler g_defaultSampler = VK_NULL_HANDLE;
struct SamplerRec { uint32_t key; VkSampler sampler; };
std::vector<SamplerRec> g_samplers;
struct TextureBundle
{
    std::array<VkImageView, kTextureSlots> views{};
    std::array<VkSampler, kTextureSlots> samplers{};
    VkDescriptorSet sets[4]{};
};
std::deque<TextureBundle> g_textureBundles;
std::unordered_multimap<uint64_t, size_t> g_textureBundleLookup;
std::atomic<uint64_t> g_texturedDraws{0};
std::atomic<uint64_t> g_collapsedTileDraws{0};
bool g_textureCompressionBC = false;
uint64_t g_sceneTilingLastIndexedFrame = 0;
uint64_t g_sceneTilingLastDepthFrame = 0;
uint64_t g_sceneStitchLastIndexedFrame = 0;
// Diagnostic-only synchronization between a stable indexed-mesh signature and
// a subsequent full scene-color resolve.  This avoids comparing unrelated host
// frame numbers across separate headless runs.
std::atomic<uint64_t> g_indexHashEventFrame{0};
std::atomic<uint64_t> g_indexHashCaptureFrame{0};
thread_local const char* g_texturePrepareFailure = "none";
thread_local uint32_t g_texturePrepareFailureSlot = UINT32_MAX;
std::atomic<uint32_t> g_indexHashCaptureSnapshotKey{UINT32_MAX};

using PerfClock = std::chrono::steady_clock;
uint64_t g_perfFenceWaitNs = 0;
uint64_t g_perfPresentNs = 0;
uint64_t g_perfTextureNs = 0;
uint64_t g_perfPipelineNs = 0;
uint64_t g_perfConstantsNs = 0;
uint64_t g_perfVertexNs = 0;
uint64_t g_perfResolveNs = 0;
uint64_t g_perfAttachmentNs = 0;
uint64_t g_perfDrawTotalNs = 0;
uint64_t g_perfDrawCalls = 0;
uint64_t g_perfResolveCalls = 0;
uint64_t g_perfColorSwitches = 0;
uint64_t g_perfBackingSaves = 0;
uint64_t g_perfBackingRestores = 0;
uint64_t g_perfBackingPixels = 0;
uint64_t g_perfResolvePixels = 0;
uint64_t g_perfColorClears = 0;
uint64_t g_perfDepthClears = 0;
uint64_t g_perfClearRectPixels = 0;
uint64_t g_perfSnapshotRenderBreaks = 0;
uint64_t g_perfEdramOwnershipTransfers = 0;
uint64_t g_perfEdramOwnershipTiles = 0;
uint64_t g_perfEdramOwnershipRects = 0;
uint64_t g_perfEdramOwnershipPixels = 0;
uint64_t g_perfTextureUploadBytesR8 = 0;
uint64_t g_perfTextureUploadBytesRGBA = 0;
uint64_t g_perfTextureUploadBytesBC1 = 0;
uint64_t g_perfTextureUploadCount = 0;
uint64_t g_perfTextureRefreshBytesR8 = 0;
uint64_t g_perfTextureRefreshBytesRGBA = 0;
uint64_t g_perfTextureRefreshBytesBC1 = 0;
uint64_t g_perfTextureRefreshCount = 0;
uint64_t g_perfTextureHashBytesR8 = 0;
uint64_t g_perfTextureHashCallsR8 = 0;
uint64_t g_perfTextureHashBytesRGBA = 0;
uint64_t g_perfTextureHashCallsRGBA = 0;
uint64_t g_perfTextureHashBytesRGBAAuthored = 0;
uint64_t g_perfTextureHashCallsRGBAAuthored = 0;
uint64_t g_perfTextureHashBytesRGBAGenerated = 0;
uint64_t g_perfTextureHashCallsRGBAGenerated = 0;
uint64_t g_perfTextureHashBytesBC1 = 0;
uint64_t g_perfTextureHashCallsBC1 = 0;
uint64_t g_perfVertexUploadBytes = 0;
uint64_t g_perfVertexUploadCopies = 0;
uint64_t g_perfVertexReuseBytes = 0;
uint64_t g_perfVertexReuseHits = 0;
uint64_t g_perfIndexUploadBytes = 0;
uint64_t g_perfIndexUploadCopies = 0;
struct ShaderPairPerf
{
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint64_t draws = 0;
    uint64_t indexed = 0;
    uint64_t elements = 0;
    uint64_t scissorPixels = 0;
};
std::array<ShaderPairPerf, 64> g_perfShaderPairs{};
uint32_t g_perfShaderPairCount = 0;
struct DepthStatePerf
{
    uint32_t depthControl = 0;
    uint32_t stencilFront = 0;
    uint32_t stencilBack = 0;
    uint64_t draws = 0;
    uint64_t indexed = 0;
    uint64_t elements = 0;
    uint64_t scissorPixels = 0;
};
std::array<DepthStatePerf, 64> g_perfDepthStates{};
uint32_t g_perfDepthStateCount = 0;
PerfClock::time_point g_perfWindowStart{};
uint64_t g_perfWindowStartFrame = 0;

void ResetPerfShaderPairs()
{
    for (uint32_t i = 0; i < g_perfShaderPairCount; ++i)
        g_perfShaderPairs[i] = {};
    g_perfShaderPairCount = 0;
}

void ResetPerfDepthStates()
{
    for (uint32_t i = 0; i < g_perfDepthStateCount; ++i)
        g_perfDepthStates[i] = {};
    g_perfDepthStateCount = 0;
}

void AccumulatePerfShaderPair(uint64_t vs, uint64_t ps, bool indexed,
                              uint64_t elements, uint64_t scissorPixels)
{
    for (uint32_t i = 0; i < g_perfShaderPairCount; ++i)
    {
        auto& pair = g_perfShaderPairs[i];
        if (pair.vs != vs || pair.ps != ps)
            continue;
        ++pair.draws;
        pair.indexed += indexed ? 1u : 0u;
        pair.elements += elements;
        pair.scissorPixels += scissorPixels;
        return;
    }
    if (g_perfShaderPairCount >= g_perfShaderPairs.size())
        return;
    auto& pair = g_perfShaderPairs[g_perfShaderPairCount++];
    pair.vs = vs;
    pair.ps = ps;
    pair.draws = 1;
    pair.indexed = indexed ? 1u : 0u;
    pair.elements = elements;
    pair.scissorPixels = scissorPixels;
}

void AccumulatePerfDepthState(uint32_t depthControl, uint32_t stencilFront,
                              uint32_t stencilBack, bool indexed,
                              uint64_t elements, uint64_t scissorPixels)
{
    for (uint32_t i = 0; i < g_perfDepthStateCount; ++i)
    {
        auto& state = g_perfDepthStates[i];
        if (state.depthControl != depthControl || state.stencilFront != stencilFront ||
            state.stencilBack != stencilBack)
            continue;
        ++state.draws;
        state.indexed += indexed ? 1u : 0u;
        state.elements += elements;
        state.scissorPixels += scissorPixels;
        return;
    }
    if (g_perfDepthStateCount >= g_perfDepthStates.size())
        return;
    auto& state = g_perfDepthStates[g_perfDepthStateCount++];
    state.depthControl = depthControl;
    state.stencilFront = stencilFront;
    state.stencilBack = stencilBack;
    state.draws = 1;
    state.indexed = indexed ? 1u : 0u;
    state.elements = elements;
    state.scissorPixels = scissorPixels;
}

HMODULE g_vulkanModule = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_queueFamily = UINT32_MAX;
VkCommandPool g_commandPool = VK_NULL_HANDLE;
struct FrameContext
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    bool timestampPending = false;
    uint32_t timestampDrawCount = 0;
    uint64_t timestampFrame = 0;
};
std::array<FrameContext, kMaxFramesInFlight> g_frameContexts{};
uint32_t g_framesInFlight = 1;
uint32_t g_frameSlot = 0;
VkCommandBuffer g_commandBuffer = VK_NULL_HANDLE;
VkSemaphore g_imageAvailable = VK_NULL_HANDLE;
VkSemaphore g_renderFinished = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;
std::vector<VkImage> g_images;
std::vector<VkImageView> g_swapViews;
std::vector<bool> g_imageInitialized;
// A present-wait binary semaphore must not be reused until the presentation
// engine has consumed it. Index these by swapchain image: reacquiring an image
// is the portable proof that its previous presentation has released the
// semaphore associated with that image. Frame fences alone don't prove that.
std::vector<VkSemaphore> g_presentReady;
// Guest/logical extent. Keep this independent from the desktop/window extent so
// resize/fullscreen never changes Xbox EDRAM addressing or guest raster state.
VkExtent2D g_extent{};
// Host render-target extent. This is the guest extent multiplied by the public
// Resolution Scale setting and never changes when the desktop window resizes.
VkExtent2D g_internalExtent{};
uint32_t g_resolutionScale = 1;
// Swapchain/client extent. This may change at any time through WM_SIZE.
VkExtent2D g_outputExtent{};
VkFormat g_swapFormat = VK_FORMAT_UNDEFINED;
bool g_outputSuspended = false;
bool g_samplerAnisotropySupported = false;
float g_maxSamplerAnisotropy = 1.0f;
float g_effectiveSamplerAnisotropy = 1.0f;

VkExtent2D InternalExtentForLogical(uint32_t width, uint32_t height)
{
    return {width * g_resolutionScale, height * g_resolutionScale};
}

float ConfiguredAspectRatio()
{
    using mojorecomp::config::AspectRatio;
    switch (mojorecomp::config::Get().aspectRatio)
    {
        case AspectRatio::Ultrawide21x9: return 21.0f / 9.0f;
        case AspectRatio::SuperUltrawide32x9: return 32.0f / 9.0f;
        case AspectRatio::Ratio16x10: return 16.0f / 10.0f;
        case AspectRatio::Ratio4x3: return 4.0f / 3.0f;
        default: return 16.0f / 9.0f;
    }
}

VkRect2D OutputContentRect(bool preserveNative16x9 = false)
{
    VkRect2D rect{{0, 0}, g_outputExtent};
    if (!g_outputExtent.width || !g_outputExtent.height)
        return rect;

    const double target = preserveNative16x9
        ? (16.0 / 9.0)
        : static_cast<double>(ConfiguredAspectRatio());
    const double output = static_cast<double>(g_outputExtent.width) /
                          static_cast<double>(g_outputExtent.height);
    if (std::abs(output - target) < 0.0001)
        return rect;

    if (output > target)
    {
        const uint32_t width = std::max(1u, static_cast<uint32_t>(
            std::llround(static_cast<double>(g_outputExtent.height) * target)));
        rect.offset.x = static_cast<int32_t>((g_outputExtent.width - width) / 2u);
        rect.extent.width = width;
    }
    else
    {
        const uint32_t height = std::max(1u, static_cast<uint32_t>(
            std::llround(static_cast<double>(g_outputExtent.width) / target)));
        rect.offset.y = static_cast<int32_t>((g_outputExtent.height - height) / 2u);
        rect.extent.height = height;
    }
    return rect;
}

std::array<float, 2> AspectScaleForDraw(bool scene3D)
{
    const float targetAspect = ConfiguredAspectRatio();
    if (scene3D)
        return mojorecomp::gpu::SceneAspectScale(targetAspect);
    return mojorecomp::gpu::SafeAreaViewportScale(targetAspect);
}

VkViewport ApplySafeAreaViewport(const VkViewport& viewport)
{
    const auto scale = mojorecomp::gpu::SafeAreaViewportScale(ConfiguredAspectRatio());
    VkViewport out = viewport;
    const float centerX = static_cast<float>(g_extent.width) * 0.5f;
    const float centerY = static_cast<float>(g_extent.height) * 0.5f;
    out.x = centerX + (viewport.x - centerX) * scale[0];
    out.y = centerY + (viewport.y - centerY) * scale[1];
    out.width = viewport.width * scale[0];
    out.height = viewport.height * scale[1];
    return out;
}

VkRect2D ScaleRectToInternal(const VkRect2D& logical)
{
    VkRect2D scaled = logical;
    scaled.offset.x *= static_cast<int32_t>(g_resolutionScale);
    scaled.offset.y *= static_cast<int32_t>(g_resolutionScale);
    scaled.extent.width *= g_resolutionScale;
    scaled.extent.height *= g_resolutionScale;
    return scaled;
}

VkViewport ScaleViewportToInternal(const VkViewport& logical)
{
    VkViewport scaled = logical;
    const float scale = static_cast<float>(g_resolutionScale);
    scaled.x *= scale;
    scaled.y *= scale;
    scaled.width *= scale;
    scaled.height *= scale;
    return scaled;
}

VkImageCopy ScaleImageCopyToInternal(const VkImageCopy& logical)
{
    VkImageCopy scaled = logical;
    scaled.srcOffset.x *= static_cast<int32_t>(g_resolutionScale);
    scaled.srcOffset.y *= static_cast<int32_t>(g_resolutionScale);
    scaled.dstOffset.x *= static_cast<int32_t>(g_resolutionScale);
    scaled.dstOffset.y *= static_cast<int32_t>(g_resolutionScale);
    scaled.extent.width *= g_resolutionScale;
    scaled.extent.height *= g_resolutionScale;
    return scaled;
}

constexpr uint32_t kOverlayFirstGlyph = 32;
constexpr uint32_t kOverlayLastGlyph = 126;
constexpr uint32_t kOverlayGlyphCount = kOverlayLastGlyph - kOverlayFirstGlyph + 1;
constexpr uint32_t kOverlayAtlasColumns = 16;
constexpr uint32_t kOverlayCellSize = 28;
constexpr uint32_t kOverlayAtlasWidth = kOverlayAtlasColumns * kOverlayCellSize;
constexpr uint32_t kOverlayAtlasRows =
    (kOverlayGlyphCount + kOverlayAtlasColumns - 1) / kOverlayAtlasColumns;
constexpr uint32_t kOverlayAtlasHeight = kOverlayAtlasRows * kOverlayCellSize;

struct OverlayGlyph
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f;
};

struct OverlayVertex
{
    float position[2]{};
    float uv[2]{};
    float color[4]{};
};

std::array<OverlayGlyph, kOverlayGlyphCount> g_overlayGlyphs{};
VkImage g_overlayAtlasImage = VK_NULL_HANDLE;
VkDeviceMemory g_overlayAtlasMemory = VK_NULL_HANDLE;
VkImageView g_overlayAtlasView = VK_NULL_HANDLE;
VkSampler g_overlaySampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_overlaySetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_overlayDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_overlayDescriptor = VK_NULL_HANDLE;
VkPipelineLayout g_overlayPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_overlayPipeline = VK_NULL_HANDLE;
VkShaderModule g_overlayVs = VK_NULL_HANDLE;
VkShaderModule g_overlayPs = VK_NULL_HANDLE;
std::vector<uint8_t> g_overlayAtlasBytes;
std::vector<uint8_t> g_overlayVsSpv;
std::vector<uint8_t> g_overlayPsSpv;
bool g_overlayAssetsAttempted = false;
bool g_overlayAssetsPrepared = false;
bool g_overlayResourcesAttempted = false;
bool g_overlayResourcesReady = false;

// FXAA is a host-only final-frame post-process. The guest scene is first composed
// into this output-sized sampled image, FXAA renders it into the swapchain, and
// the debug overlay is drawn afterward so its glyphs remain crisp.
VkImage g_fxaaImage = VK_NULL_HANDLE;
VkDeviceMemory g_fxaaMemory = VK_NULL_HANDLE;
VkImageView g_fxaaView = VK_NULL_HANDLE;
VkImageLayout g_fxaaImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkExtent2D g_fxaaExtent{};
VkSampler g_fxaaSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_fxaaSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_fxaaDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_fxaaDescriptor = VK_NULL_HANDLE;
VkPipelineLayout g_fxaaPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_fxaaPipeline = VK_NULL_HANDLE;
VkShaderModule g_fxaaVs = VK_NULL_HANDLE;
VkShaderModule g_fxaaPs = VK_NULL_HANDLE;
std::vector<uint8_t> g_fxaaVsSpv;
std::vector<uint8_t> g_fxaaPsSpv;
bool g_fxaaAssetsAttempted = false;
bool g_fxaaAssetsPrepared = false;
bool g_fxaaResourcesAttempted = false;
bool g_fxaaResourcesReady = false;

constexpr uint32_t kMaxGpuTimedDraws = 1024;
constexpr uint32_t kGpuTimestampQueryCount = 2 + kMaxGpuTimedDraws * 2;
struct GpuDrawTimingRecord
{
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t depthControl = 0;
    uint32_t binSelect = 0;
    uint32_t textureKey0 = 0;
    uint32_t bool128_131 = 0;
    uint32_t mode = 0;
    uint32_t elements = 0;
    uint64_t scissorPixels = 0;
    bool indexed = false;
};
std::array<std::array<GpuDrawTimingRecord, kMaxGpuTimedDraws>, kMaxFramesInFlight>
    g_gpuDrawTimingRecords{};
bool g_gpuDrawTimestamps = false;
bool g_gpuTimestampSupported = false;
uint32_t g_gpuTimestampValidBits = 0;
float g_gpuTimestampPeriodNs = 0.0f;

bool g_hostMsaa2x = false;
bool g_hostMsaaDepthSampleZero = false;
VkSampleCountFlags g_hostColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
VkSampleCountFlags g_hostDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
VkSampleCountFlags g_hostStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
// Keep EDRAM attachment capabilities per host format. Requiring D24S8 and
// D24FS8 to intersect globally rejects devices that support the format used by
// the current guest surface but not the other depth representation. Color and
// the actual depth format are intersected when a backing/pipeline is selected.
VkSampleCountFlags g_hostEdramColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
VkSampleCountFlags g_hostEdramDepthUnormSampleCounts = 0;
VkSampleCountFlags g_hostEdramDepthFloatSampleCounts = 0;
bool g_hostDepthUnormUsesFloatFormat = false;
bool g_shaderStencilExport = false;
bool g_sampleRateShading = false;

bool HostMsaaEnabled()
{
    // Native host MSAA is substantially more expensive than the 1x ownership
    // path, so keep it as an explicit diagnostic opt-in.
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_HOST_MSAA");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

VkSampleCountFlagBits GuestSurfaceSamples(uint32_t surfaceInfo,
                                          uint32_t depthInfo = UINT32_MAX)
{
    if (!HostMsaaEnabled())
        return VK_SAMPLE_COUNT_1_BIT;

    VkSampleCountFlagBits requested = VK_SAMPLE_COUNT_1_BIT;
    switch (xenos::SurfaceMsaaSamples(surfaceInfo))
    {
        case xenos::MsaaSamples::k2X: requested = VK_SAMPLE_COUNT_2_BIT; break;
        case xenos::MsaaSamples::k4X: requested = VK_SAMPLE_COUNT_4_BIT; break;
        default: break;
    }
    VkSampleCountFlags common = g_hostEdramColorSampleCounts;
    if (depthInfo != UINT32_MAX)
    {
        const bool floatDepth = ((depthInfo >> 16) & 1u) != 0;
        common &= floatDepth ? g_hostEdramDepthFloatSampleCounts
                             : g_hostEdramDepthUnormSampleCounts;
    }
    // Do not silently turn a guest 2x/4x surface into a 1x host image. EDRAM
    // tile dimensions and sample addressing are part of the physical layout;
    // pretending the surface is 1x would make ownership transfers target the
    // wrong pixels. Callers treat zero as an unsupported attachment layout.
    return (common & requested) ? requested : VkSampleCountFlagBits(0);
}

VkSampleCountFlagBits GuestEdramSamples(uint32_t surfaceInfo)
{
    switch (xenos::SurfaceMsaaSamples(surfaceInfo))
    {
        case xenos::MsaaSamples::k2X: return VK_SAMPLE_COUNT_2_BIT;
        case xenos::MsaaSamples::k4X: return VK_SAMPLE_COUNT_4_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

uint64_t ColorSurfaceKey(uint32_t surfaceInfo, uint32_t colorInfo)
{
    return (uint64_t(surfaceInfo) << 32) | colorInfo;
}

VkImage g_colorImage = VK_NULL_HANDLE;
VkImageView g_colorView = VK_NULL_HANDLE;
VkDeviceMemory g_colorMemory = VK_NULL_HANDLE;
VkImage g_colorResolveImage = VK_NULL_HANDLE;
VkImageView g_colorResolveView = VK_NULL_HANDLE;
VkDeviceMemory g_colorResolveMemory = VK_NULL_HANDLE;
VkImageLayout g_colorResolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
bool g_colorInitialized = false;

constexpr VkFormat kDepthUnormFormat = VK_FORMAT_D24_UNORM_S8_UINT;
constexpr VkFormat kDepthFloatFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
// Legacy/fallback depth image used before a guest depth backing is selected.
constexpr VkFormat kDepthFormat = kDepthUnormFormat;

VkFormat HostDepthFormat(uint32_t depthInfo)
{
    // Match ReXGlue/Xenia's host-render-target fallback: guest D24S8 may be
    // represented by D32_SFLOAT_S8 when D24S8 doesn't provide the image/sample
    // capabilities required by the host renderer. Keep the guest encoding
    // (UNORM24 versus 20e4) separate from the Vulkan storage format.
    return ((depthInfo >> 16) & 1u) || g_hostDepthUnormUsesFloatFormat
        ? kDepthFloatFormat : kDepthUnormFormat;
}
VkImage g_depthImage = VK_NULL_HANDLE;
VkImageView g_depthView = VK_NULL_HANDLE;
VkDeviceMemory g_depthMemory = VK_NULL_HANDLE;
VkImage g_depthResolveImage = VK_NULL_HANDLE;
VkImageView g_depthResolveView = VK_NULL_HANDLE;
VkDeviceMemory g_depthResolveMemory = VK_NULL_HANDLE;
VkImageLayout g_depthResolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
bool g_depthInitialized = false;

VkBuffer g_uploadBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_uploadMemory = VK_NULL_HANDLE;
uint8_t* g_uploadMapped = nullptr;
VkDeviceAddress g_uploadAddress = 0;
VkDeviceSize g_uploadAt = 0;
VkDeviceSize g_uploadLimit = kUploadBytes;
VkDeviceSize g_readbackOffset = 0;
VkDeviceSize g_readbackBytes = 0;
bool g_readbackPending = false;
bool g_readbackReported = false;
uint32_t g_readbackAttempts = 0;
uint64_t g_frontReadbackFrame = 0;
uint32_t g_readbackFrameSlot = UINT32_MAX;

bool FrontDiagnosticFrame(uint64_t frame)
{
    static const uint64_t requestedFrame = [] {
        const char* value = std::getenv("MOJORECOMP_FRONT_CAPTURE_FRAME");
        if (!value || !*value)
            return UINT64_MAX;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : UINT64_MAX;
    }();
    if (requestedFrame)
        return frame == requestedFrame;

    static const uint64_t captureEvery = [] {
        const char* value = std::getenv("MOJORECOMP_FRONT_CAPTURE_EVERY");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    static const uint64_t captureUntil = [] {
        const char* value = std::getenv("MOJORECOMP_FRONT_CAPTURE_UNTIL");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    static const uint64_t captureAfter = [] {
        const char* value = std::getenv("MOJORECOMP_FRONT_CAPTURE_AFTER");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    if (captureEvery)
        return frame >= captureAfter && (!captureUntil || frame <= captureUntil) &&
               frame % captureEvery == 0;

    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_FRONT_DIAGNOSTICS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled && (frame <= 8 || (frame <= 1200 && frame % 60 == 0));
}

bool FrontCaptureTrigger(uint64_t frame)
{
    static const std::string triggerFile = [] {
        const char* value = std::getenv("MOJORECOMP_FRONT_CAPTURE_TRIGGER_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (triggerFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static uint64_t lastToken = 0;
    const auto now = Clock::now();
    if (now < nextPoll)
        return false;
    nextPoll = now + std::chrono::milliseconds(20);

    std::ifstream input(triggerFile);
    uint64_t token = 0;
    if (input)
        input >> token;
    if (!token || token == lastToken)
        return false;

    lastToken = token;
    KLOG("[front probe] capture trigger token=%llu frame=%llu\n",
         static_cast<unsigned long long>(token),
         static_cast<unsigned long long>(frame));
    return true;
}

bool CollapseTiledDrawsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_COLLAPSE_TILED_DRAWS");
        // Native Xenos behavior is to replay every tile selected by PM4
        // predication. The collapsed path remains opt-in for diagnostics only.
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool StitchTiledDrawsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_TILED_DRAWS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

int StitchBroadenMode()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_BROADEN_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return -1;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mode = 0;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(toggleFile);
        int next = 0;
        if (input)
            input >> next;
        mode = std::clamp(next, 0, 3);
    }
    return mode;
}

int StitchResolveSourceMode()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_RESOLVE_SOURCE_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return 0;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mode = 0;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(toggleFile);
        int next = 0;
        if (input)
            input >> next;
        mode = std::clamp(next, 0, 7);
    }
    return mode;
}

bool StitchPhysicalTileRasterEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_PHYSICAL_TILES");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool HostMsaa2xRequested()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_EDRAM_MSAA_2X");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool HostMsaa2xDepthResolveRequested()
{
    static const int mode = [] {
        const char* value = std::getenv("MOJORECOMP_EDRAM_MSAA_DEPTH_RESOLVE");
        if (!value || !value[0])
            return -1;
        return value[0] != '0' ? 1 : 0;
    }();
    // Depth SAMPLE_ZERO is the Xenos-compatible default; the environment
    // variable remains an explicit diagnostic opt-out.
    return mode != 0;
}

bool HostMsaa2xDepthResolveEnabled()
{
    return g_hostMsaa2x && g_hostMsaaDepthSampleZero &&
           HostMsaa2xDepthResolveRequested();
}

bool DiagnosticClearOnDepthSurfaceSwitchEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_CLEAR_ON_DEPTH_SURFACE_SWITCH");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool DiagnosticInvalidateColorOnDepthAliasEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_INVALIDATE_COLOR_ON_DEPTH_ALIAS");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool DiagnosticInvalidateColorAfterDepthWriteEnabled()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_EDRAM_DEPTH_WRITE_ALIAS_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static bool enabled = false;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(toggleFile);
        int next = 0;
        if (input)
            input >> next;
        enabled = next != 0;
    }
    return enabled;
}

VkSampleCountFlagBits HostRasterSamples()
{
    return g_hostMsaa2x ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
}

int SceneColorResolveTileMask()
{
    static const std::string maskFile = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_RESOLVE_TILE_MASK_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (maskFile.empty())
        return 7;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mask = 7;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(maskFile);
        int next = 7;
        if (input)
            input >> next;
        mask = std::clamp(next, 0, 7);
    }
    return mask;
}

int StableSceneFeedbackMode()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_FEEDBACK_STABLE_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return 0;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mode = 0;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(toggleFile);
        int next = 0;
        if (input)
            input >> next;
        mode = std::clamp(next, 0, 2);
    }
    return mode;
}

bool StableSceneFeedbackEnabled()
{
    return StableSceneFeedbackMode() != 0;
}

int DiagnosticTileBoundaryClearMode()
{
    static const std::string modeFile = [] {
        const char* value = std::getenv("MOJORECOMP_DIAGNOSTIC_TILE_CLEAR_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (modeFile.empty())
        return 0;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mode = 0;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(modeFile);
        int next = 0;
        if (input)
            input >> next;
        mode = std::clamp(next, 0, 3);
    }
    return mode;
}

bool StitchUnpredicatedTileDrawsEnabled()
{
    const int broadenMode = StitchBroadenMode();
    if (broadenMode >= 0)
        return (broadenMode & 1) != 0;

    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_UNPREDICATED_TILE_DRAWS");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool TraceUnpredicatedTileDrawsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_TRACE_UNPREDICATED");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool TraceStitchMissesEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_TRACE_MISSES");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool StitchOffsetPredicatedDrawsEnabled()
{
    const int broadenMode = StitchBroadenMode();
    if (broadenMode >= 0)
        return (broadenMode & 2) != 0;

    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_OFFSET_PREDICATED_DRAWS");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

bool StitchRotateTileClearsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_ROTATE_TILE_CLEARS");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

int StitchTileClearMode()
{
    static const std::string modeFile = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_CLEAR_MODE_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (modeFile.empty())
        return -1;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mode = 0;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(modeFile);
        int value = 0;
        if (input)
            input >> value;
        mode = std::clamp(value, 0, 2);
    }
    return mode;
}

bool StitchSkipTransitionReplayEnabled()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_STITCH_TRANSITION_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static bool enabled = false;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(toggleFile);
        int value = 0;
        if (input)
            input >> value;
        enabled = value != 0;
    }
    return enabled;
}

bool StableFrontEnabled()
{
    // Diagnostic fallback that retains a second fullscreen snapshot instead of
    // presenting only the surface selected by XE_SWAP.
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_STABLE_FRONT");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool EmulateReverseZLikeD3D12()
{
    // Match Xenia/ReXGlue's D3D12-safe reversed-depth handling by default:
    // expose a monotonic host depth range and reflect NDC Z in the VS when the
    // guest requests a reversed viewport. Vulkan permits reversed viewport
    // ranges, but keeping this representation avoids driver/API convention
    // differences and reproduces the Xenos depth ordering used by Crash.
    // Set MOJORECOMP_EMULATE_REVERSE_Z=0 only for diagnostics/A-B comparisons.
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_EMULATE_REVERSE_Z");
        return !value || !*value || value[0] != '0';
    }();
    return enabled;
}

bool UiDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_UI_DIAGNOSTICS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool TraceDrawFrame(uint64_t frame)
{
    static const uint64_t requestedFrame = [] {
        const char* value = std::getenv("MOJORECOMP_DRAW_TRACE_FRAME");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    static const uint64_t requestedEndFrame = [] {
        const char* value = std::getenv("MOJORECOMP_DRAW_TRACE_END_FRAME");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    if (requestedFrame)
    {
        if (requestedEndFrame >= requestedFrame)
            return frame >= requestedFrame && frame <= requestedEndFrame;
        if (frame == requestedFrame)
            return true;
    }

    static uint64_t tracedFrame = 0;
    static bool keyWasDown = false;
    const bool keyDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (keyDown && !keyWasDown)
    {
        // Input is sampled while the current guest frame is already being
        // submitted. Trace the following frame so the capture always starts
        // before its first draw/resolve instead of producing a partial frame.
        tracedFrame = frame + 1;
        KLOG("[draw frame] armed frame=%llu\n",
             static_cast<unsigned long long>(tracedFrame));
    }
    keyWasDown = keyDown;
    return tracedFrame == frame;
}

bool EdramOwnershipEnabled(uint64_t frame)
{
    // Diagnostic-only gate for reaching late title scenes at the old renderer
    // speed before measuring the currently expensive physical ownership path.
    // Zero (the default) keeps ownership enabled from the first frame.
    static const uint64_t firstFrame = [] {
        const char* value = std::getenv("MOJORECOMP_EDRAM_OWNERSHIP_AFTER_FRAME");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    return frame >= firstFrame;
}

bool EdramOwnershipPairProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_EDRAM_PAIR_PROFILE");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

void ReportFrontPixels(const uint8_t* pixels, uint64_t frame)
{
    const uint64_t count = uint64_t(g_internalExtent.width) * g_internalExtent.height;
    uint64_t white = 0, black = 0, same = 0, hash = 14695981039346656037ull;
    uint8_t lo[4]{255,255,255,255}, hi[4]{};
    for (uint64_t i = 0; i < count; ++i)
    {
        const auto* p = pixels + i * 4;
        white += p[0] >= 254 && p[1] >= 254 && p[2] >= 254;
        black += p[0] <= 1 && p[1] <= 1 && p[2] <= 1;
        same += std::memcmp(pixels, p, 4) == 0;
        for (uint32_t c = 0; c < 4; ++c)
        {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
            hash = (hash ^ p[c]) * 1099511628211ull;
        }
    }
    KLOG("[front probe] pixels frame=%llu count=%llu white=%llu black=%llu sameAsFirst=%llu "
         "min=%u,%u,%u,%u max=%u,%u,%u,%u hash=%016llX\n",
         frame, count, white, black, same, lo[0],lo[1],lo[2],lo[3],
         hi[0],hi[1],hi[2],hi[3],hash);

    // Optional raw GPU evidence, never in source/asset directories by default.
    // Caller supplies an existing ignored output directory. Top-down BGRA BMP.
    const char* directory = std::getenv("MOJORECOMP_FRONT_CAPTURE_DIR");
    if (!directory || !*directory)
        return;

    std::error_code ec;
    const std::filesystem::path captureDir(directory);
    if (!std::filesystem::is_directory(captureDir, ec))
    {
        ec.clear();
        std::filesystem::create_directories(captureDir, ec);
    }

    static bool captureConfigReported = false;
    if (!captureConfigReported)
    {
        KLOG("[front probe] capture dir='%s' format=%u ready=%u ec=%d\n",
             captureDir.string().c_str(), static_cast<unsigned>(g_swapFormat),
             std::filesystem::is_directory(captureDir) ? 1u : 0u, ec.value());
        captureConfigReported = true;
    }

    if (ec || !std::filesystem::is_directory(captureDir, ec) ||
        g_swapFormat != VK_FORMAT_B8G8R8A8_UNORM)
        return;

    const auto path = captureDir / ("front-" + std::to_string(frame) + ".bmp");
    std::ofstream output(path, std::ios::binary);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(count * 4);
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(g_internalExtent.width);
    info.biHeight = -static_cast<LONG>(g_internalExtent.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels), count * 4);
    if (!output)
        KLOG("[front probe] capture write failed: %s\n", path.string().c_str());
}

VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
bool g_frameOpen = false;
bool g_rendering = false;
bool g_active = false;

std::atomic<uint64_t> g_frames{0};
uint64_t g_physicalTileContentFrame = 0;
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_skippedMode{0};
std::atomic<uint64_t> g_skippedIndexed{0};
std::atomic<uint64_t> g_indexedDraws{0};
std::atomic<uint64_t> g_skippedShader{0};
std::atomic<uint64_t> g_skippedTexture{0};
std::atomic<uint64_t> g_skippedVertex{0};
std::atomic<uint64_t> g_skippedTopology{0};
std::atomic<uint64_t> g_skippedPipeline{0};
std::atomic<uint64_t> g_alphaTestDraws{0};
std::atomic<uint64_t> g_alphaTestUnsupported{0};
std::atomic<uint64_t> g_resolves{0};
std::atomic<uint64_t> g_resolveCopies{0};
std::atomic<uint64_t> g_duplicateResolveSkips{0};
std::atomic<uint64_t> g_resolveUnsupported{0};
std::atomic<uint64_t> g_resolveUnsupportedSource{0};
std::atomic<uint64_t> g_resolveUnsupportedRt1{0};
std::atomic<uint64_t> g_resolveUnsupportedDepth{0};
std::atomic<uint64_t> g_resolveUnsupportedRect{0};
std::atomic<uint64_t> g_resolveUnsupportedSnapshot{0};
std::atomic<uint64_t> g_snapshotPresents{0};
std::atomic<uint64_t> g_fallbackPresents{0};
uint64_t g_renderWriteGeneration = 1;

struct ResolveDedupEntry
{
    uint64_t frame = 0;
    uint64_t generation = 0;
    uint64_t signature = 0;
};
std::array<ResolveDedupEntry, 64> g_resolveDedup{};
uint32_t g_resolveDedupCursor = 0;

uint64_t MixResolveSignature(uint64_t hash, uint32_t value)
{
    hash ^= value;
    hash *= 1099511628211ull;
    return hash;
}

uint64_t ResolveCommandSignature(const uint32_t* regs)
{
    const uint32_t control = regs[xenos::kRbCopyControl];
    const uint32_t srcSelect = control & 7u;
    uint64_t hash = 1469598103934665603ull;
    hash = MixResolveSignature(hash, srcSelect);
    hash = MixResolveSignature(hash, control);
    hash = MixResolveSignature(hash, regs[xenos::kRbCopyDestBase]);
    hash = MixResolveSignature(hash, regs[xenos::kRbCopyDestPitch]);
    hash = MixResolveSignature(hash, regs[xenos::kRbCopyDestInfo]);
    hash = MixResolveSignature(hash, regs[xenos::kPaScWindowScissorTl]);
    hash = MixResolveSignature(hash, regs[xenos::kPaScWindowScissorBr]);
    hash = MixResolveSignature(hash, regs[xenos::kPaScWindowOffset]);
    hash = MixResolveSignature(hash, regs[xenos::kPaScScreenScissorTl]);
    hash = MixResolveSignature(hash, regs[xenos::kPaScScreenScissorBr]);
    if (srcSelect == 0)
        hash = MixResolveSignature(hash, regs[xenos::kRbColorInfo]);
    else if (srcSelect == 1)
        hash = MixResolveSignature(hash, regs[xenos::kRbColor1Info]);
    else if (srcSelect == 4)
        hash = MixResolveSignature(hash, regs[xenos::kRbDepthInfo]);
    return hash;
}

bool SkipDuplicateResolve(const uint32_t* regs)
{
    const uint32_t control = regs[xenos::kRbCopyControl];
    // Resolve clears are observable side effects even when the copy source did
    // not change, so only pure copies are eligible for this frame-local cache.
    if ((control & ((1u << 8) | (1u << 9))) != 0)
        return false;

    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    const uint64_t signature = ResolveCommandSignature(regs);
    for (const auto& entry : g_resolveDedup)
    {
        if (entry.frame == frame && entry.generation == g_renderWriteGeneration &&
            entry.signature == signature)
        {
            g_duplicateResolveSkips.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    auto& entry = g_resolveDedup[g_resolveDedupCursor++ % g_resolveDedup.size()];
    entry = {frame, g_renderWriteGeneration, signature};
    return false;
}

struct ResolveSnapshot
{
    uint32_t key = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool depth = false;
    bool depthFloat24 = false;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView bgraView = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Depth resolves are written to guest memory as packed 24_8 / 24_8_FLOAT
    // words, even though D3D9 programs copy_dest_format as 8_8_8_8. Keep a
    // byte-exact color alias next to the native depth image so later texture
    // fetches through an 8_8_8_8 descriptor observe the guest memory bytes,
    // rather than a decoded host depth float.
    VkImage packedDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory packedDepthMemory = VK_NULL_HANDLE;
    VkImageView packedDepthWriteView = VK_NULL_HANDLE;
    std::array<VkImageView, 4> packedDepthBgraViews{};
    VkImageLayout packedDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t packedDepthResolveEndian = 0;
    bool packedDepthInitialized = false;
    uint64_t frameSeen = 0;
    uint64_t copies = 0;
    // Backing pixels remain in canonical render-target channel order. Compose
    // the resolve's destination R/B swap with the consumer's fetch swizzle.
    bool resolveSwap = false;
    bool presentResolveSwap = false;
    // Front-buffer resolves arrive as several EDRAM tiles. Keep coverage across
    // host presents so a present between tile 0/1/2 cannot expose a partially
    // updated snapshot (the source of the frontend text flicker).
    std::array<uint64_t, 16> coverageWords{};
    bool coverageStarted = false;
    bool coverageComplete = false;
    uint64_t coverageGeneration = 0;
    uint64_t presentedGeneration = 0;
    VkImage presentImage = VK_NULL_HANDLE;
    VkDeviceMemory presentMemory = VK_NULL_HANDLE;
    VkImageView presentView = VK_NULL_HANDLE;
    VkImageView presentBgraView = VK_NULL_HANDLE;
    VkImageLayout presentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool presentValid = false;
};

// Creation helpers return pointers that remain live while later texture slots
// or MRT attachments may create more resources. A vector invalidates every
// such pointer when it grows; deque preserves element addresses on push_back.
std::deque<ResolveSnapshot> g_snapshots;
constexpr size_t kMaxResolveSnapshots = 32;

constexpr uint32_t kSnapshotCoverageTile = 32;

void ResetSnapshotCoverage(ResolveSnapshot& snapshot)
{
    snapshot.coverageWords.fill(0);
    snapshot.coverageStarted = true;
    snapshot.coverageComplete = false;
    ++snapshot.coverageGeneration;
}

bool SnapshotCoverageBit(const ResolveSnapshot& snapshot, uint32_t bit)
{
    return (snapshot.coverageWords[bit >> 6] & (1ull << (bit & 63))) != 0;
}

void MarkSnapshotCoverage(ResolveSnapshot& snapshot, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height)
{
    if (snapshot.depth || !width || !height)
        return;

    const uint32_t cols = (snapshot.width + kSnapshotCoverageTile - 1) /
                          kSnapshotCoverageTile;
    const uint32_t rows = (snapshot.height + kSnapshotCoverageTile - 1) /
                          kSnapshotCoverageTile;
    const uint32_t cellCount = cols * rows;
    if (!cols || !rows || cellCount > snapshot.coverageWords.size() * 64)
        return;

    // A new resolve touching the top-left cell after that cell was already
    // written starts the next logical tiled sweep. Host presents may happen
    // between tiles, so this generation intentionally does not use g_frames.
    if (!snapshot.coverageStarted)
        ResetSnapshotCoverage(snapshot);
    else if (x == 0 && y == 0 && SnapshotCoverageBit(snapshot, 0))
        ResetSnapshotCoverage(snapshot);

    const uint32_t rx0 = x;
    const uint32_t ry0 = y;
    const uint32_t rx1 = std::min(snapshot.width, x + width);
    const uint32_t ry1 = std::min(snapshot.height, y + height);
    const uint32_t firstCol = rx0 / kSnapshotCoverageTile;
    const uint32_t lastCol = (rx1 ? rx1 - 1 : 0) / kSnapshotCoverageTile;
    const uint32_t firstRow = ry0 / kSnapshotCoverageTile;
    const uint32_t lastRow = (ry1 ? ry1 - 1 : 0) / kSnapshotCoverageTile;

    for (uint32_t cy = firstRow; cy <= lastRow && cy < rows; ++cy)
    {
        const uint32_t cellY0 = cy * kSnapshotCoverageTile;
        const uint32_t cellY1 = std::min(snapshot.height, cellY0 + kSnapshotCoverageTile);
        if (ry0 > cellY0 || ry1 < cellY1)
            continue;
        for (uint32_t cx = firstCol; cx <= lastCol && cx < cols; ++cx)
        {
            const uint32_t cellX0 = cx * kSnapshotCoverageTile;
            const uint32_t cellX1 = std::min(snapshot.width, cellX0 + kSnapshotCoverageTile);
            if (rx0 > cellX0 || rx1 < cellX1)
                continue;
            const uint32_t bit = cy * cols + cx;
            snapshot.coverageWords[bit >> 6] |= 1ull << (bit & 63);
        }
    }

    snapshot.coverageComplete = true;
    for (uint32_t bit = 0; bit < cellCount; ++bit)
    {
        if (!SnapshotCoverageBit(snapshot, bit))
        {
            snapshot.coverageComplete = false;
            break;
        }
    }
}

struct ColorBacking
{
    uint64_t key = UINT64_MAX;
    uint32_t surfaceInfo = 0;
    uint32_t info = 0;
    uint32_t baseTiles = 0;
    uint32_t pitchTiles = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage resolveImage = VK_NULL_HANDLE;
    VkDeviceMemory resolveMemory = VK_NULL_HANDLE;
    VkImageView resolveView = VK_NULL_HANDLE;
    VkImageLayout resolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSet resolveDescriptor = VK_NULL_HANDLE;
    VkDescriptorSet transferDescriptor = VK_NULL_HANDLE;
    bool initialized = false;
    bool logicalRaster = false;
    uint64_t lastWriteFrame = 0;
    uint64_t lastWriteDraw = 0;
    uint64_t lastWriteGeneration = 0;
};

std::deque<ColorBacking> g_colorBackings;
uint64_t g_activeColorSurfaceKey = UINT64_MAX;
uint64_t g_activeColor1SurfaceKey = UINT64_MAX;
uint32_t g_activeColorInfo = UINT32_MAX;
uint32_t g_activeColor1Info = UINT32_MAX;
bool g_activeColor1Enabled = false;

struct DepthBacking
{
    uint64_t key = UINT64_MAX;
    uint32_t surfaceInfo = 0;
    uint32_t info = 0;
    uint32_t baseTiles = 0;
    uint32_t pitchTiles = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat format = kDepthUnormFormat;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView depthSampleView = VK_NULL_HANDLE;
    VkImageView stencilSampleView = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage resolveImage = VK_NULL_HANDLE;
    VkDeviceMemory resolveMemory = VK_NULL_HANDLE;
    VkImageView resolveView = VK_NULL_HANDLE;
    VkImageLayout resolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSet resolveDescriptor = VK_NULL_HANDLE;
    VkDescriptorSet transferDescriptor = VK_NULL_HANDLE;
    bool initialized = false;
};

std::deque<DepthBacking> g_depthBackings;
uint64_t g_activeDepthSurfaceKey = UINT64_MAX;
bool g_activeDepthEnabled = false;

enum class EdramOwnerKind : uint8_t
{
    None = 0,
    Color = 1,
    Depth = 2,
};

struct EdramOwner
{
    EdramOwnerKind kind = EdramOwnerKind::None;
    uint64_t key = UINT64_MAX;
};

std::array<EdramOwner, xenos::kEdramTileCount> g_edramOwners{};
struct EdramOwnerTileCount
{
    EdramOwner owner{};
    uint32_t tiles = 0;
};
std::vector<EdramOwnerTileCount> g_edramOwnerTileCounts;
struct EdramTransferPairPerf
{
    EdramOwner source{};
    EdramOwner dest{};
    uint64_t transfers = 0;
    uint64_t tiles = 0;
};
std::array<EdramTransferPairPerf, 64> g_perfEdramTransferPairs{};
uint32_t g_perfEdramTransferPairCount = 0;
uint64_t g_edramOwnerFastPathHits = 0;
VkDescriptorSetLayout g_edramTransferSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_edramTransferPool = VK_NULL_HANDLE;
VkPipelineLayout g_edramTransferPipelineLayout = VK_NULL_HANDLE;
VkShaderModule g_edramTransferVs = VK_NULL_HANDLE;
enum class EdramTransferPass : uint8_t
{
    Color,
    DepthStencilExport,
    DepthOnly,
    StencilBitPlanes,
};
struct EdramTransferPipeline
{
    EdramOwnerKind sourceKind = EdramOwnerKind::None;
    EdramOwnerKind destKind = EdramOwnerKind::None;
    VkSampleCountFlagBits sourceSamples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits destSamples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits sourceGuestSamples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits destGuestSamples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t sourcePitchTiles = 0;
    uint32_t destPitchTiles = 0;
    bool sourceDepthFloat24 = false;
    bool destDepthFloat24 = false;
    VkFormat destDepthFormat = VK_FORMAT_UNDEFINED;
    EdramTransferPass pass = EdramTransferPass::Color;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule ps = VK_NULL_HANDLE;
};
std::vector<EdramTransferPipeline> g_edramTransferPipelines;
struct DepthSnapshotPackPipeline
{
    VkSampleCountFlagBits sourceSamples = VK_SAMPLE_COUNT_1_BIT;
    bool sourceDepthFloat24 = false;
    uint32_t resolveEndian = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule ps = VK_NULL_HANDLE;
};
std::vector<DepthSnapshotPackPipeline> g_depthSnapshotPackPipelines;
uint64_t g_edramOwnershipTransfers = 0;
uint64_t g_edramOwnershipTiles = 0;
uint64_t g_edramOwnershipUnsupported = 0;

struct BlurRtWriteHistory
{
    uint64_t frame = 0;
    uint64_t draw = 0;
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint64_t binMask = 0;
    uint64_t binSelect = 0;
    uint32_t prim = 0;
    uint32_t count = 0;
    uint32_t colorInfo = 0;
    uint32_t color1Info = 0;
    uint32_t colorMask = 0;
    uint32_t blend = 0;
    uint32_t depth = 0;
    uint32_t windowTl = 0;
    uint32_t windowBr = 0;
    uint32_t windowOffset = 0;
    bool indexed = false;
    bool predicated = false;
};

std::array<BlurRtWriteHistory, 16> g_blurRtWriteHistory{};
uint32_t g_blurRtWriteHistoryCursor = 0;

ColorBacking* FindColorBacking(uint64_t key);
ColorBacking* FindColorBacking(uint32_t surfaceInfo, uint32_t info);
ColorBacking* FindColorBackingByInfo(uint32_t info);
void TransitionColorBacking(ColorBacking& backing, VkImageLayout next);
void TransitionColorResolveBacking(ColorBacking& backing, VkImageLayout next);
DepthBacking* FindDepthBacking(uint64_t key);
void TransitionDepthBacking(DepthBacking& backing, VkImageLayout next);

ColorBacking* ActiveColorBacking()
{
    return g_activeColorSurfaceKey == UINT64_MAX ? nullptr : FindColorBacking(g_activeColorSurfaceKey);
}

ColorBacking* ActiveColor1Backing()
{
    return !g_activeColor1Enabled || g_activeColor1SurfaceKey == UINT64_MAX
        ? nullptr : FindColorBacking(g_activeColor1SurfaceKey);
}

VkImage ActiveColorImage()
{
    if (ColorBacking* backing = ActiveColorBacking())
        return backing->image;
    return g_colorImage;
}

VkImageView ActiveColorView()
{
    if (ColorBacking* backing = ActiveColorBacking())
        return backing->view;
    return g_colorView;
}

DepthBacking* ActiveDepthBacking()
{
    if (!g_activeDepthEnabled || g_activeDepthSurfaceKey == UINT64_MAX)
        return nullptr;
    return FindDepthBacking(g_activeDepthSurfaceKey);
}

VkImage ActiveDepthImage()
{
    if (DepthBacking* backing = ActiveDepthBacking())
        return backing->image;
    return g_depthImage;
}

VkImageView ActiveDepthView()
{
    if (DepthBacking* backing = ActiveDepthBacking())
        return backing->view;
    return g_depthView;
}

void ReportResolve(const uint32_t* regs, const ResolveSnapshot& snapshot)
{
    static const std::string traceFile = [] {
        const char* value = std::getenv("MOJORECOMP_RESOLVE_TRACE_FILE");
        return value ? std::string(value) : std::string{};
    }();
    static std::chrono::steady_clock::time_point nextPoll{};
    static bool traceEnabled = false;
    if (!traceFile.empty())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPoll)
        {
            nextPoll = now + std::chrono::milliseconds(20);
            std::ifstream input(traceFile);
            int enabled = 0;
            if (input)
                input >> enabled;
            traceEnabled = enabled != 0;
        }
    }
    if (!traceEnabled && !FrontDiagnosticFrame(snapshot.frameSeen))
        return;
    KLOG("[front probe] resolve frame=%llu key=%08X src=%u color0=%08X color1=%08X "
         "active=%08X depth=%08X clear=%08X control=%08X dest=%08X pitch=%08X "
         "rect=%08X..%08X draws=%llu\n", snapshot.frameSeen, snapshot.key,
         regs[xenos::kRbCopyControl] & 7u, regs[xenos::kRbColorInfo],
         regs[xenos::kRbColor1Info], g_activeColorInfo, regs[xenos::kRbDepthInfo],
         regs[xenos::kRbColorClear], regs[xenos::kRbCopyControl],
         regs[xenos::kRbCopyDestBase], regs[xenos::kRbCopyDestPitch],
         regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
         g_draws.load(std::memory_order_relaxed));
}

struct GuestTexture
{
    uint32_t key = 0;
    uint32_t mipKey = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t format = 0;
    uint32_t swizzle = 0;
    uint32_t endian = 0;
    uint32_t mipMax = 0;
    bool packedMips = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t sourceHash = 0;
    uint64_t sourceCheckFrame = 0;
};

std::vector<GuestTexture> g_guestTextures;
std::unordered_multimap<uint64_t, size_t> g_guestTextureLookup;

struct VertexAttribute
{
    int32_t location = -1;
    uint32_t fetchSlot = 0;
    uint32_t format = 0;
    uint32_t isSigned = 0;
    uint32_t isInteger = 0;
    uint32_t strideDwords = 0;
    uint32_t offsetDwords = 0;
    uint32_t indirect = 0;
};

struct ShaderModuleRec
{
    uint32_t type = 0;
    uint64_t hash = 0;
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModule float24DepthModule = VK_NULL_HANDLE;
    bool float24DepthModuleAttempted = false;
    std::vector<VertexAttribute> attributes;
    bool usesTextures = false;
    std::vector<uint32_t> textureSlots;
    std::vector<uint32_t> textureDimensions;
    std::vector<uint32_t> aluConsts;
    bool aluDynamic = false;
    bool usesAlu = true;
    uint32_t colorOutputMask = 0;
    bool writesDepth = false;
};

struct PipelineRec
{
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t prim = 0;
    uint32_t colorMask = 0;
    uint32_t blendControl0 = 0;
    uint32_t blendControl1 = 0;
    uint32_t depthControl = 0;
    uint32_t rasterState = 0;
    uint32_t alphaTest = 0;
    uint32_t mode = 0;
    bool primitiveRestart = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat depthFormat = kDepthUnormFormat;
    bool depthFloat24 = false;
    uint64_t layoutHash = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// Shader pointers are used across VS/PS lookup while a missing module may be appended.
// deque keeps those references stable; a vector invalidated the first VS pointer when
// the PS was inserted and produced a bogus hash in the very first pipeline key.
std::deque<ShaderModuleRec> g_modules;
std::vector<PipelineRec> g_pipelines;
VkPipelineCache g_pipelineCache = VK_NULL_HANDLE;
std::filesystem::path g_pipelineCachePath;

bool BeginFrame();

PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr = nullptr;
PFN_vkCreateInstance p_vkCreateInstance = nullptr;
PFN_vkDestroyInstance p_vkDestroyInstance = nullptr;
PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices = nullptr;
PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 p_vkGetPhysicalDeviceProperties2 = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 p_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPhysicalDeviceImageFormatProperties p_vkGetPhysicalDeviceImageFormatProperties = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR p_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
PFN_vkCreateWin32SurfaceKHR p_vkCreateWin32SurfaceKHR = nullptr;
PFN_vkDestroySurfaceKHR p_vkDestroySurfaceKHR = nullptr;
PFN_vkCreateDevice p_vkCreateDevice = nullptr;

PFN_vkDestroyDevice p_vkDestroyDevice = nullptr;
PFN_vkGetDeviceQueue p_vkGetDeviceQueue = nullptr;
PFN_vkCreateSwapchainKHR p_vkCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR p_vkDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR p_vkGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR p_vkAcquireNextImageKHR = nullptr;
PFN_vkQueuePresentKHR p_vkQueuePresentKHR = nullptr;
PFN_vkCreateCommandPool p_vkCreateCommandPool = nullptr;
PFN_vkDestroyCommandPool p_vkDestroyCommandPool = nullptr;
PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers = nullptr;
PFN_vkResetCommandBuffer p_vkResetCommandBuffer = nullptr;
PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer p_vkEndCommandBuffer = nullptr;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier = nullptr;
PFN_vkCmdBlitImage p_vkCmdBlitImage = nullptr;
PFN_vkCmdCopyImage p_vkCmdCopyImage = nullptr;
PFN_vkCmdResolveImage p_vkCmdResolveImage = nullptr;
PFN_vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer = nullptr;
PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage = nullptr;
PFN_vkCmdClearColorImage p_vkCmdClearColorImage = nullptr;
PFN_vkCmdClearDepthStencilImage p_vkCmdClearDepthStencilImage = nullptr;
PFN_vkCmdClearAttachments p_vkCmdClearAttachments = nullptr;
PFN_vkCreateSemaphore p_vkCreateSemaphore = nullptr;
PFN_vkDestroySemaphore p_vkDestroySemaphore = nullptr;
PFN_vkCreateFence p_vkCreateFence = nullptr;
PFN_vkDestroyFence p_vkDestroyFence = nullptr;
PFN_vkWaitForFences p_vkWaitForFences = nullptr;
PFN_vkResetFences p_vkResetFences = nullptr;
PFN_vkCreateQueryPool p_vkCreateQueryPool = nullptr;
PFN_vkDestroyQueryPool p_vkDestroyQueryPool = nullptr;
PFN_vkCmdResetQueryPool p_vkCmdResetQueryPool = nullptr;
PFN_vkCmdWriteTimestamp p_vkCmdWriteTimestamp = nullptr;
PFN_vkGetQueryPoolResults p_vkGetQueryPoolResults = nullptr;
PFN_vkQueueSubmit p_vkQueueSubmit = nullptr;
PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle = nullptr;
PFN_vkCreateBuffer p_vkCreateBuffer = nullptr;
PFN_vkDestroyBuffer p_vkDestroyBuffer = nullptr;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements = nullptr;
PFN_vkAllocateMemory p_vkAllocateMemory = nullptr;
PFN_vkFreeMemory p_vkFreeMemory = nullptr;
PFN_vkBindBufferMemory p_vkBindBufferMemory = nullptr;
PFN_vkMapMemory p_vkMapMemory = nullptr;
PFN_vkUnmapMemory p_vkUnmapMemory = nullptr;
PFN_vkGetBufferDeviceAddress p_vkGetBufferDeviceAddress = nullptr;
PFN_vkCreateImage p_vkCreateImage = nullptr;
PFN_vkDestroyImage p_vkDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements = nullptr;
PFN_vkBindImageMemory p_vkBindImageMemory = nullptr;
PFN_vkCreateImageView p_vkCreateImageView = nullptr;
PFN_vkDestroyImageView p_vkDestroyImageView = nullptr;
PFN_vkCmdBeginRendering p_vkCmdBeginRendering = nullptr;
PFN_vkCmdEndRendering p_vkCmdEndRendering = nullptr;
PFN_vkCreateShaderModule p_vkCreateShaderModule = nullptr;
PFN_vkDestroyShaderModule p_vkDestroyShaderModule = nullptr;
PFN_vkCreatePipelineLayout p_vkCreatePipelineLayout = nullptr;
PFN_vkDestroyPipelineLayout p_vkDestroyPipelineLayout = nullptr;
PFN_vkCreatePipelineCache p_vkCreatePipelineCache = nullptr;
PFN_vkDestroyPipelineCache p_vkDestroyPipelineCache = nullptr;
PFN_vkGetPipelineCacheData p_vkGetPipelineCacheData = nullptr;
PFN_vkCreateGraphicsPipelines p_vkCreateGraphicsPipelines = nullptr;
PFN_vkDestroyPipeline p_vkDestroyPipeline = nullptr;
PFN_vkCmdBindPipeline p_vkCmdBindPipeline = nullptr;
PFN_vkCmdSetViewport p_vkCmdSetViewport = nullptr;
PFN_vkCmdSetScissor p_vkCmdSetScissor = nullptr;
PFN_vkCmdSetBlendConstants p_vkCmdSetBlendConstants = nullptr;
PFN_vkCmdSetStencilReference p_vkCmdSetStencilReference = nullptr;
PFN_vkCmdSetStencilCompareMask p_vkCmdSetStencilCompareMask = nullptr;
PFN_vkCmdSetStencilWriteMask p_vkCmdSetStencilWriteMask = nullptr;
PFN_vkCmdBindVertexBuffers p_vkCmdBindVertexBuffers = nullptr;
PFN_vkCmdBindIndexBuffer p_vkCmdBindIndexBuffer = nullptr;
PFN_vkCmdPushConstants p_vkCmdPushConstants = nullptr;
PFN_vkCmdDraw p_vkCmdDraw = nullptr;
PFN_vkCmdDrawIndexed p_vkCmdDrawIndexed = nullptr;
PFN_vkCreateDescriptorSetLayout p_vkCreateDescriptorSetLayout = nullptr;
PFN_vkDestroyDescriptorSetLayout p_vkDestroyDescriptorSetLayout = nullptr;
PFN_vkCreateDescriptorPool p_vkCreateDescriptorPool = nullptr;
PFN_vkDestroyDescriptorPool p_vkDestroyDescriptorPool = nullptr;
PFN_vkAllocateDescriptorSets p_vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets p_vkUpdateDescriptorSets = nullptr;
PFN_vkCmdBindDescriptorSets p_vkCmdBindDescriptorSets = nullptr;
PFN_vkCreateSampler p_vkCreateSampler = nullptr;
PFN_vkDestroySampler p_vkDestroySampler = nullptr;

struct GraphicsCommandStateCache
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport viewport{};
    VkRect2D scissor{};
    VkPipelineLayout descriptorLayout = VK_NULL_HANDLE;
    uint32_t descriptorFirstSet = 0;
    uint32_t descriptorCount = 0;
    std::array<VkDescriptorSet, 4> descriptorSets{};
    bool pipelineValid = false;
    bool viewportValid = false;
    bool scissorValid = false;
    bool descriptorsValid = false;
};

GraphicsCommandStateCache g_graphicsCommandState{};

void ResetGraphicsCommandStateCache(VkCommandBuffer commandBuffer)
{
    g_graphicsCommandState = {};
    g_graphicsCommandState.commandBuffer = commandBuffer;
}

void EnsureGraphicsCommandStateCache(VkCommandBuffer commandBuffer)
{
    if (g_graphicsCommandState.commandBuffer != commandBuffer)
        ResetGraphicsCommandStateCache(commandBuffer);
}

void CmdBindGraphicsPipelineCached(VkCommandBuffer commandBuffer, VkPipeline pipeline)
{
    EnsureGraphicsCommandStateCache(commandBuffer);
    if (g_graphicsCommandState.pipelineValid && g_graphicsCommandState.pipeline == pipeline)
        return;
    p_vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    g_graphicsCommandState.pipeline = pipeline;
    g_graphicsCommandState.pipelineValid = true;
}

void CmdSetViewportCached(VkCommandBuffer commandBuffer, const VkViewport& viewport)
{
    EnsureGraphicsCommandStateCache(commandBuffer);
    if (g_graphicsCommandState.viewportValid &&
        std::memcmp(&g_graphicsCommandState.viewport, &viewport, sizeof(viewport)) == 0)
        return;
    p_vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    g_graphicsCommandState.viewport = viewport;
    g_graphicsCommandState.viewportValid = true;
}

void CmdSetScissorCached(VkCommandBuffer commandBuffer, const VkRect2D& scissor)
{
    EnsureGraphicsCommandStateCache(commandBuffer);
    if (g_graphicsCommandState.scissorValid &&
        std::memcmp(&g_graphicsCommandState.scissor, &scissor, sizeof(scissor)) == 0)
        return;
    p_vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    g_graphicsCommandState.scissor = scissor;
    g_graphicsCommandState.scissorValid = true;
}

void CmdBindDescriptorSetsCached(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                                 uint32_t firstSet, uint32_t descriptorCount,
                                 const VkDescriptorSet* descriptorSets)
{
    EnsureGraphicsCommandStateCache(commandBuffer);
    bool same = g_graphicsCommandState.descriptorsValid &&
                g_graphicsCommandState.descriptorLayout == layout &&
                g_graphicsCommandState.descriptorFirstSet == firstSet &&
                g_graphicsCommandState.descriptorCount == descriptorCount &&
                descriptorCount <= g_graphicsCommandState.descriptorSets.size();
    if (same)
    {
        for (uint32_t i = 0; i < descriptorCount; ++i)
        {
            if (g_graphicsCommandState.descriptorSets[i] != descriptorSets[i])
            {
                same = false;
                break;
            }
        }
    }
    if (same)
        return;

    p_vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout, firstSet, descriptorCount, descriptorSets,
                              0, nullptr);
    if (descriptorCount <= g_graphicsCommandState.descriptorSets.size())
    {
        g_graphicsCommandState.descriptorLayout = layout;
        g_graphicsCommandState.descriptorFirstSet = firstSet;
        g_graphicsCommandState.descriptorCount = descriptorCount;
        for (uint32_t i = 0; i < descriptorCount; ++i)
            g_graphicsCommandState.descriptorSets[i] = descriptorSets[i];
        g_graphicsCommandState.descriptorsValid = true;
    }
    else
    {
        g_graphicsCommandState.descriptorsValid = false;
    }
}

template <typename T>
bool LoadInstance(T& fn, const char* name)
{
    fn = reinterpret_cast<T>(p_vkGetInstanceProcAddr(g_instance, name));
    if (!fn)
        KLOG("Vulkan missing instance function %s\n", name);
    return fn != nullptr;
}

template <typename T>
bool LoadDevice(T& fn, const char* name)
{
    fn = reinterpret_cast<T>(p_vkGetDeviceProcAddr(g_device, name));
    if (!fn)
        KLOG("Vulkan missing device function %s\n", name);
    return fn != nullptr;
}

std::filesystem::path PipelineCachePath(const VkPhysicalDeviceProperties& properties)
{
    std::filesystem::path dir;
    if (const char* custom = std::getenv("MOJORECOMP_PIPELINE_CACHE_DIR"); custom && *custom)
        dir = custom;
    else
        dir = HostPaths::ExeDir() / "cache" / "vulkan";

    char fileName[128]{};
    std::snprintf(fileName, sizeof(fileName), "pipeline-v2-%04X-%04X-%08X.bin",
                  properties.vendorID, properties.deviceID, properties.driverVersion);
    return dir / fileName;
}

bool CreatePersistentPipelineCache(const VkPhysicalDeviceProperties& properties)
{
    if (!p_vkCreatePipelineCache)
        return false;

    g_pipelineCachePath = PipelineCachePath(properties);
    std::vector<uint8_t> initialData;
    {
        std::ifstream input(g_pipelineCachePath, std::ios::binary | std::ios::ate);
        if (input)
        {
            const std::streamsize size = input.tellg();
            // Keep a corrupt/pathological cache from causing a huge allocation.
            if (size > 0 && size <= 64ll * 1024ll * 1024ll)
            {
                input.seekg(0, std::ios::beg);
                initialData.resize(static_cast<size_t>(size));
                if (!input.read(reinterpret_cast<char*>(initialData.data()), size))
                    initialData.clear();
            }
        }
    }

    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = initialData.size();
    ci.pInitialData = initialData.empty() ? nullptr : initialData.data();
    VkResult result = p_vkCreatePipelineCache(g_device, &ci, nullptr, &g_pipelineCache);
    if (result != VK_SUCCESS && !initialData.empty())
    {
        // Vulkan validates cache compatibility. A driver update or stale file is
        // not fatal; fall back to an empty cache and overwrite it on shutdown.
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        result = p_vkCreatePipelineCache(g_device, &ci, nullptr, &g_pipelineCache);
    }
    if (result != VK_SUCCESS)
    {
        g_pipelineCache = VK_NULL_HANDLE;
        g_pipelineCachePath.clear();
        KLOG("Vulkan pipeline cache unavailable: %d\n", result);
        return false;
    }
    KLOG("Vulkan pipeline cache: %s (%zu bytes)\n",
         initialData.empty() ? "cold" : "loaded", initialData.size());
    return true;
}

void SavePersistentPipelineCache()
{
    if (!g_device || !g_pipelineCache || !p_vkGetPipelineCacheData ||
        g_pipelineCachePath.empty())
        return;

    size_t size = 0;
    if (p_vkGetPipelineCacheData(g_device, g_pipelineCache, &size, nullptr) != VK_SUCCESS ||
        !size || size > 64ull * 1024ull * 1024ull)
        return;
    std::vector<uint8_t> data(size);
    if (p_vkGetPipelineCacheData(g_device, g_pipelineCache, &size, data.data()) != VK_SUCCESS ||
        !size)
        return;
    data.resize(size);

    std::error_code ec;
    std::filesystem::create_directories(g_pipelineCachePath.parent_path(), ec);
    if (ec)
        return;
    const auto tempPath = g_pipelineCachePath.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return;
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        if (!output)
            return;
    }
    std::filesystem::rename(tempPath, g_pipelineCachePath, ec);
    if (ec)
    {
        // Windows rename doesn't replace an existing file. Replace explicitly,
        // but only inside the already resolved cache path.
        std::error_code removeEc;
        std::filesystem::remove(g_pipelineCachePath, removeEc);
        ec.clear();
        std::filesystem::rename(tempPath, g_pipelineCachePath, ec);
    }
    if (!ec)
        KLOG_DIAG("Vulkan pipeline cache saved: %zu bytes\n", data.size());
}

float F32(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float Float20e4To32(uint32_t value)
{
    value &= 0xFFFFFFu;
    if (!value)
        return 0.0f;
    uint32_t mantissa = value & 0xFFFFFu;
    int32_t exponent = int32_t(value >> 20);
    if (!exponent)
    {
        uint32_t shift = 0;
        while ((mantissa & 0x100000u) == 0)
        {
            mantissa <<= 1;
            ++shift;
        }
        exponent = 1 - int32_t(shift);
        mantissa &= 0xFFFFFu;
    }
    const uint32_t bits = (uint32_t(exponent + 112) << 23) | (mantissa << 3);
    return F32(bits);
}

float DecodeDepthClearValue(uint32_t depthInfo, uint32_t packedClear)
{
    const uint32_t depth24 = (packedClear >> 8) & 0xFFFFFFu;
    if ((depthInfo >> 16) & 1u)
    {
        // Host depth stores the full Xenos [0, 2) float24 range in [0, 1).
        return Float20e4To32(depth24) * 0.5f;
    }
    // Match Xenia's exact UNORM24 -> float32 conversion.
    return float(depth24 + (depth24 >> 23)) * (1.0f / float(1u << 24));
}

bool CoordinateDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_COORDINATE_DIAGNOSTICS");
        return value && value[0] == '1';
    }();
    return enabled;
}

uint32_t PhysicalToCached(uint32_t physical)
{
    return kGuestPhysicalBase | (physical & 0x1FFFFFFFu);
}

bool GuestRangeOk(uint32_t va, uint64_t bytes)
{
    return va >= kGuestPhysicalBase && uint64_t(va) + bytes <= kGuestPhysicalEnd;
}

uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties props{};
    p_vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    return UINT32_MAX;
}

VkCompositeAlphaFlagBitsKHR PickCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
    constexpr VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (auto value : order)
        if (supported & value)
            return value;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

VkSampleCountFlags ImageSampleCounts(VkFormat format, VkImageUsageFlags usage)
{
    if (!p_vkGetPhysicalDeviceImageFormatProperties || !g_physicalDevice ||
        format == VK_FORMAT_UNDEFINED)
        return 0;

    VkImageFormatProperties properties{};
    const VkResult result = p_vkGetPhysicalDeviceImageFormatProperties(
        g_physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        usage, 0, &properties);
    return result == VK_SUCCESS ? properties.sampleCounts : 0;
}

bool UpdateEdramSampleCountSupport()
{
    constexpr VkImageUsageFlags colorUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    constexpr VkImageUsageFlags depthUsage =
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    const VkSampleCountFlags color = ImageSampleCounts(g_swapFormat, colorUsage);
    const VkSampleCountFlags d24 = ImageSampleCounts(kDepthUnormFormat, depthUsage);
    const VkSampleCountFlags d32 = ImageSampleCounts(kDepthFloatFormat, depthUsage);
    g_hostEdramColorSampleCounts = g_hostColorSampleCounts & color;
    const VkSampleCountFlags genericDepth =
        g_hostDepthSampleCounts & g_hostStencilSampleCounts;
    const VkSampleCountFlags d24Usable = genericDepth & d24;
    const VkSampleCountFlags d32Usable = genericDepth & d32;
    const VkSampleCountFlags guestRelevantSamples =
        g_hostEdramColorSampleCounts &
        (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT);
    g_hostDepthUnormUsesFloatFormat =
        guestRelevantSamples != 0 &&
        (d24Usable & guestRelevantSamples) != guestRelevantSamples &&
        (d32Usable & guestRelevantSamples) == guestRelevantSamples;
    g_hostEdramDepthUnormSampleCounts =
        g_hostDepthUnormUsesFloatFormat ? d32Usable : d24Usable;
    g_hostEdramDepthFloatSampleCounts = d32Usable;

    KLOG("Vulkan EDRAM format samples: color(%u)=%02X D24S8=%02X D24FS8=%02X "
         "usableColor=%02X usableD24S8=%02X usableD24FS8=%02X D24host=%s\n",
         uint32_t(g_swapFormat), uint32_t(color), uint32_t(d24), uint32_t(d32),
         uint32_t(g_hostEdramColorSampleCounts),
         uint32_t(g_hostEdramDepthUnormSampleCounts),
         uint32_t(g_hostEdramDepthFloatSampleCounts),
         g_hostDepthUnormUsesFloatFormat ? "D32S8" : "D24S8");
    if (!(g_hostEdramColorSampleCounts & VK_SAMPLE_COUNT_1_BIT))
    {
        KLOG("Vulkan EDRAM color format lacks required 1x sampled/transfer/attachment support\n");
        return false;
    }
    return true;
}

bool CreateSwapchain(uint32_t requestedWidth, uint32_t requestedHeight)
{
    VkSurfaceCapabilitiesKHR caps{};
    if (p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physicalDevice, g_surface, &caps) != VK_SUCCESS)
        return false;
    constexpr VkImageUsageFlags requiredSurfaceUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((caps.supportedUsageFlags & requiredSurfaceUsage) != requiredSurfaceUsage)
    {
        KLOG("Vulkan surface lacks transfer-destination/color-attachment support: %08X\n",
             caps.supportedUsageFlags);
        return false;
    }

    uint32_t formatCount = 0;
    if (p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physicalDevice, g_surface, &formatCount, nullptr) != VK_SUCCESS || !formatCount)
        return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    p_vkGetPhysicalDeviceSurfaceFormatsKHR(g_physicalDevice, g_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR format = formats[0];
    for (const auto& candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            format = candidate;
            break;
        }
    }

    g_outputExtent = caps.currentExtent;
    if (g_outputExtent.width == UINT32_MAX)
    {
        g_outputExtent.width = std::clamp(requestedWidth, caps.minImageExtent.width, caps.maxImageExtent.width);
        g_outputExtent.height = std::clamp(requestedHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    g_swapFormat = format.format;
    if (!UpdateEdramSampleCountSupport())
        return false;
    g_readbackBytes = VkDeviceSize(g_internalExtent.width) * g_internalExtent.height * 4u;
    if (g_readbackBytes + 4096 < kUploadBytes)
    {
        g_readbackOffset = (kUploadBytes - g_readbackBytes) & ~VkDeviceSize(255);
        g_uploadLimit = g_readbackOffset;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // Crash of the Titans is timing-sensitive. The previous ReXGlue runtime
    // explicitly disabled vsync for this title because a FIFO presentation
    // backlog makes gameplay/cinematics feel like slow motion. Preserve that
    // title baseline here while keeping FIFO as the mandatory-safe fallback.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    const bool forceVsync = mojorecomp::config::Get().vsync;
    if (!forceVsync && p_vkGetPhysicalDeviceSurfacePresentModesKHR)
    {
        uint32_t presentModeCount = 0;
        if (p_vkGetPhysicalDeviceSurfacePresentModesKHR(
                g_physicalDevice, g_surface, &presentModeCount, nullptr) == VK_SUCCESS &&
            presentModeCount)
        {
            std::vector<VkPresentModeKHR> presentModes(presentModeCount);
            if (p_vkGetPhysicalDeviceSurfacePresentModesKHR(
                    g_physicalDevice, g_surface, &presentModeCount,
                    presentModes.data()) == VK_SUCCESS)
            {
                const auto supported = [&](VkPresentModeKHR candidate) {
                    return std::find(presentModes.begin(), presentModes.end(), candidate) !=
                           presentModes.end();
                };
                if (supported(VK_PRESENT_MODE_IMMEDIATE_KHR))
                    presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                else if (supported(VK_PRESENT_MODE_MAILBOX_KHR))
                    presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = g_outputExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = requiredSurfaceUsage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = PickCompositeAlpha(caps.supportedCompositeAlpha);
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    if (p_vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain) != VK_SUCCESS)
        return false;

    uint32_t actualCount = 0;
    if (p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &actualCount, nullptr) != VK_SUCCESS || !actualCount)
        return false;
    g_images.resize(actualCount);
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &actualCount, g_images.data());
    g_swapViews.assign(actualCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < actualCount; ++i)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = g_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_swapFormat;
        viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                               VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (p_vkCreateImageView(g_device, &viewInfo, nullptr, &g_swapViews[i]) !=
            VK_SUCCESS)
            return false;
    }
    g_imageInitialized.assign(actualCount, false);
    const char* presentModeName =
        presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" :
        presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo";
    KLOG("Vulkan swapchain ready: %ux%u images=%u format=%u present=%s vsync=%u\n",
         g_outputExtent.width, g_outputExtent.height, actualCount,
         static_cast<unsigned>(g_swapFormat), presentModeName,
         presentMode == VK_PRESENT_MODE_FIFO_KHR ? 1u : 0u);
    return true;
}

void DestroyPresentReadySemaphores()
{
    for (VkSemaphore semaphore : g_presentReady)
        if (semaphore && p_vkDestroySemaphore)
            p_vkDestroySemaphore(g_device, semaphore, nullptr);
    g_presentReady.clear();
}

bool CreatePresentReadySemaphores()
{
    DestroyPresentReadySemaphores();
    g_presentReady.assign(g_images.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& semaphore : g_presentReady)
    {
        if (p_vkCreateSemaphore(g_device, &sem, nullptr, &semaphore) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool WaitForFrameQueueIdle()
{
    for (uint32_t i = 0; i < g_framesInFlight; ++i)
    {
        const VkFence fence = g_frameContexts[i].fence;
        if (fence && p_vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
    }
    return true;
}

bool RecreateSwapchain(uint32_t requestedWidth, uint32_t requestedHeight)
{
    if (!requestedWidth || !requestedHeight)
    {
        g_outputSuspended = true;
        return true;
    }
    if (!g_device || !g_surface || !g_swapchain)
        return false;
    if (!g_outputSuspended && g_outputExtent.width == requestedWidth &&
        g_outputExtent.height == requestedHeight)
        return true;
    if (!WaitForFrameQueueIdle())
        return false;

    g_readbackPending = false;
    g_readbackFrameSlot = UINT32_MAX;
    DestroyPresentReadySemaphores();
    for (VkImageView view : g_swapViews)
        if (view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, view, nullptr);
    g_swapViews.clear();
    g_images.clear();
    g_imageInitialized.clear();
    if (g_swapchain && p_vkDestroySwapchainKHR)
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;

    if (!CreateSwapchain(requestedWidth, requestedHeight) ||
        !CreatePresentReadySemaphores())
    {
        KLOG("Vulkan swapchain recreation failed for %ux%u\n",
             requestedWidth, requestedHeight);
        return false;
    }
    g_outputSuspended = false;
    KLOG("Vulkan output resized safely: %ux%u\n",
         g_outputExtent.width, g_outputExtent.height);
    return true;
}

bool CreateCommandState()
{
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_queueFamily;
    if (p_vkCreateCommandPool(g_device, &poolInfo, nullptr, &g_commandPool) != VK_SUCCESS)
        return false;

    const char* framesEnv = std::getenv("MOJORECOMP_FRAMES_IN_FLIGHT");
    const bool diagnosticsActive =
        (std::getenv("MOJORECOMP_FRONT_DIAGNOSTICS") != nullptr);
    // Two submissions remain the normal default. Allow an explicit third frame
    // for heavy EDRAM scenes so CPU command recording can overlap more GPU work
    // without changing rendering semantics. Diagnostic readback stays
    // single-buffered.
    uint32_t requestedFrames = 2;
    if (framesEnv && *framesEnv)
    {
        const unsigned long parsed = std::strtoul(framesEnv, nullptr, 10);
        if (parsed >= 1 && parsed <= kMaxFramesInFlight)
            requestedFrames = static_cast<uint32_t>(parsed);
    }
    g_framesInFlight = diagnosticsActive ? 1u : requestedFrames;
    g_frameSlot = 0;

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = g_commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = g_framesInFlight;
    std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers{};
    if (p_vkAllocateCommandBuffers(g_device, &alloc, commandBuffers.data()) != VK_SUCCESS)
        return false;

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < g_framesInFlight; ++i)
    {
        auto& frame = g_frameContexts[i];
        frame.commandBuffer = commandBuffers[i];
        if (p_vkCreateSemaphore(g_device, &sem, nullptr, &frame.imageAvailable) != VK_SUCCESS ||
            p_vkCreateSemaphore(g_device, &sem, nullptr, &frame.renderFinished) != VK_SUCCESS ||
            p_vkCreateFence(g_device, &fence, nullptr, &frame.fence) != VK_SUCCESS)
            return false;
        if (g_gpuDrawTimestamps)
        {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = kGpuTimestampQueryCount;
            if (p_vkCreateQueryPool(g_device, &queryInfo, nullptr, &frame.timestampPool) != VK_SUCCESS)
                return false;
        }
    }
    if (!CreatePresentReadySemaphores())
        return false;
    g_commandBuffer = g_frameContexts[0].commandBuffer;
    g_imageAvailable = g_frameContexts[0].imageAvailable;
    g_renderFinished = g_frameContexts[0].renderFinished;
    g_fence = g_frameContexts[0].fence;
    KLOG("Vulkan frame queue: %u frame%s in flight, %llu MiB upload segment/frame\n",
         g_framesInFlight, g_framesInFlight == 1 ? "" : "s",
         static_cast<unsigned long long>(kFrameUploadBytes / (1024 * 1024)));
    if (g_gpuDrawTimestamps)
        KLOG("Vulkan GPU draw timestamps ACTIVE: maxDraws=%u period=%.3f ns validBits=%u\n",
             kMaxGpuTimedDraws, g_gpuTimestampPeriodNs, g_gpuTimestampValidBits);
    return true;
}

bool CreateUploadArena()
{
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kUploadBytes;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(g_device, &bi, nullptr, &g_uploadBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, g_uploadBuffer, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
        return false;

    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &flags;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &g_uploadMemory) != VK_SUCCESS)
        return false;
    if (p_vkBindBufferMemory(g_device, g_uploadBuffer, g_uploadMemory, 0) != VK_SUCCESS)
        return false;
    if (p_vkMapMemory(g_device, g_uploadMemory, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&g_uploadMapped)) != VK_SUCCESS)
        return false;

    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = g_uploadBuffer;
    g_uploadAddress = p_vkGetBufferDeviceAddress(g_device, &dai);
    if (!g_uploadAddress)
    {
        KLOG("Vulkan upload arena has no device address\n");
        return false;
    }
    KLOG("Vulkan upload arena: %llu MiB deviceAddress=%016llX\n",
         static_cast<unsigned long long>(kUploadBytes / (1024 * 1024)),
         static_cast<unsigned long long>(g_uploadAddress));
    return true;
}

bool CreateColorTarget()
{
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = g_swapFormat;
    ii.extent = {g_internalExtent.width, g_internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = HostRasterSamples();
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    // ResolveColorSurface also clears the live surface via vkCmdClearColorImage.
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &g_colorImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, g_colorImage, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &g_colorMemory) != VK_SUCCESS)
        return false;
    if (p_vkBindImageMemory(g_device, g_colorImage, g_colorMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_colorImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_swapFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(g_device, &vi, nullptr, &g_colorView) != VK_SUCCESS)
        return false;

    if (!g_hostMsaa2x)
        return true;

    // Keep a single-sample mirror for Xenos resolves and presentation. Dynamic
    // rendering resolves the 2x host attachment into this image whenever the
    // render scope ends, matching color CopySampleSelect::k01.
    VkImageCreateInfo resolveInfo = ii;
    resolveInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    if (p_vkCreateImage(g_device, &resolveInfo, nullptr, &g_colorResolveImage) != VK_SUCCESS)
        return false;
    p_vkGetImageMemoryRequirements(g_device, g_colorResolveImage, &req);
    const uint32_t resolveType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (resolveType == UINT32_MAX)
        return false;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = resolveType;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &g_colorResolveMemory) != VK_SUCCESS)
        return false;
    if (p_vkBindImageMemory(g_device, g_colorResolveImage, g_colorResolveMemory, 0) != VK_SUCCESS)
        return false;
    vi.image = g_colorResolveImage;
    return p_vkCreateImageView(g_device, &vi, nullptr, &g_colorResolveView) == VK_SUCCESS;
}

bool CreateDepthTarget()
{
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kDepthFormat;
    ii.extent = {g_internalExtent.width, g_internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = HostRasterSamples();
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &g_depthImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, g_depthImage, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &g_depthMemory) != VK_SUCCESS)
        return false;
    if (p_vkBindImageMemory(g_device, g_depthImage, g_depthMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_depthImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(g_device, &vi, nullptr, &g_depthView) != VK_SUCCESS)
        return false;

    if (!g_hostMsaa2x)
        return true;

    VkImageCreateInfo resolveInfo = ii;
    resolveInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    if (p_vkCreateImage(g_device, &resolveInfo, nullptr, &g_depthResolveImage) != VK_SUCCESS)
        return false;
    p_vkGetImageMemoryRequirements(g_device, g_depthResolveImage, &req);
    const uint32_t resolveType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (resolveType == UINT32_MAX)
        return false;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = resolveType;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &g_depthResolveMemory) != VK_SUCCESS)
        return false;
    if (p_vkBindImageMemory(g_device, g_depthResolveImage, g_depthResolveMemory, 0) != VK_SUCCESS)
        return false;
    vi.image = g_depthResolveImage;
    return p_vkCreateImageView(g_device, &vi, nullptr, &g_depthResolveView) == VK_SUCCESS;
}

void EndColorRendering()
{
    if (!g_rendering)
        return;
    if (g_hostMsaa2x)
    {
        static uint32_t reports = 0;
        if (reports++ < 16)
            KLOG("[msaa2x scope] end begin frame=%llu draws=%llu\n",
                 static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                 static_cast<unsigned long long>(g_draws.load(std::memory_order_relaxed)));
    }
    p_vkCmdEndRendering(g_commandBuffer);
    if (g_hostMsaa2x)
    {
        static uint32_t reports = 0;
        if (reports++ < 16)
            KLOG("[msaa2x scope] end returned\n");
    }
    g_rendering = false;
}

void TransitionResolveTarget(VkImage image, VkImageLayout& layout, VkImageAspectFlags aspect,
                             VkImageLayout next)
{
    if (!image || layout == next)
        return;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    else if (layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }
    else if (layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = layout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    layout = next;
}

void ResumeColorRendering(bool clearColor = false)
{
    if (g_rendering)
        return;

    ColorBacking* color0 = ActiveColorBacking();
    ColorBacking* color1 = ActiveColor1Backing();
    DepthBacking* depthBacking = ActiveDepthBacking();

    if (color0 && color0->samples != VK_SAMPLE_COUNT_1_BIT)
        TransitionColorResolveBacking(*color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (color1 && color1->samples != VK_SAMPLE_COUNT_1_BIT)
        TransitionColorResolveBacking(*color1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depthBacking && depthBacking->samples != VK_SAMPLE_COUNT_1_BIT &&
        g_hostMsaaDepthSampleZero && depthBacking->resolveImage)
        TransitionResolveTarget(depthBacking->resolveImage, depthBacking->resolveLayout,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    std::array<VkRenderingAttachmentInfo, 2> attachments{};
    attachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachments[0].imageView = ActiveColorView();
    attachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    if (color0 && color0->samples != VK_SAMPLE_COUNT_1_BIT && color0->resolveView)
    {
        attachments[0].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        attachments[0].resolveImageView = color0->resolveView;
        attachments[0].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    attachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (color1)
    {
        attachments[1].imageView = color1->view;
        attachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (color1->samples != VK_SAMPLE_COUNT_1_BIT && color1->resolveView)
        {
            attachments[1].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            attachments[1].resolveImageView = color1->resolveView;
            attachments[1].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    else
    {
        attachments[1].imageView = VK_NULL_HANDLE;
        attachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depthBacking)
    {
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = depthBacking->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (depthBacking->samples != VK_SAMPLE_COUNT_1_BIT &&
            g_hostMsaaDepthSampleZero && depthBacking->resolveView)
        {
            depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depth.resolveImageView = depthBacking->resolveView;
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        stencil = depth;
        // Xenos stencil does not need a multisample resolve for the Crash path;
        // preserve it only in the multisample owner image.
        stencil.resolveMode = VK_RESOLVE_MODE_NONE;
        stencil.resolveImageView = VK_NULL_HANDLE;
    }

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = g_internalExtent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = attachments.data();
    ri.pDepthAttachment = depthBacking ? &depth : nullptr;
    ri.pStencilAttachment = depthBacking ? &stencil : nullptr;
    p_vkCmdBeginRendering(g_commandBuffer, &ri);
    g_rendering = true;
}
ResolveSnapshot* FindSnapshot(uint32_t key)
{
    for (auto& snapshot : g_snapshots)
        if (snapshot.key == key)
            return &snapshot;
    return nullptr;
}

ResolveSnapshot* FindColorSnapshot(uint32_t key, uint32_t width, uint32_t height)
{
    for (auto& snapshot : g_snapshots)
        if (snapshot.key == key && !snapshot.depth &&
            snapshot.width == width && snapshot.height == height)
            return &snapshot;
    return nullptr;
}

ResolveSnapshot* FindDepthSnapshot(uint32_t key, uint32_t width, uint32_t height,
                                   VkFormat format, bool depthFloat24)
{
    for (auto& snapshot : g_snapshots)
        if (snapshot.key == key && snapshot.depth &&
            snapshot.width == width && snapshot.height == height &&
            snapshot.format == format && snapshot.depthFloat24 == depthFloat24)
            return &snapshot;
    return nullptr;
}

bool CreatePackedDepthSnapshotResources(ResolveSnapshot& snapshot,
                                        VkExtent2D internalExtent)
{
    VkImageCreateInfo packedInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    packedInfo.imageType = VK_IMAGE_TYPE_2D;
    packedInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    packedInfo.extent = {internalExtent.width, internalExtent.height, 1};
    packedInfo.mipLevels = 1;
    packedInfo.arrayLayers = 1;
    packedInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    packedInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    packedInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    packedInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    packedInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = p_vkCreateImage(g_device, &packedInfo, nullptr,
                                      &snapshot.packedDepthImage);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan packed depth snapshot image failed (%d): key=%08X %ux%u\n",
             result, snapshot.key, internalExtent.width, internalExtent.height);
        return false;
    }

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, snapshot.packedDepthImage, &req);
    const uint32_t packedType = FindMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (packedType == UINT32_MAX)
    {
        KLOG("Vulkan packed depth snapshot has no device-local memory type: key=%08X bits=%08X\n",
             snapshot.key, req.memoryTypeBits);
        p_vkDestroyImage(g_device, snapshot.packedDepthImage, nullptr);
        snapshot.packedDepthImage = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = packedType;
    result = p_vkAllocateMemory(g_device, &ai, nullptr, &snapshot.packedDepthMemory);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan packed depth snapshot memory allocation failed (%d): key=%08X bytes=%llu\n",
             result, snapshot.key, static_cast<unsigned long long>(req.size));
        p_vkDestroyImage(g_device, snapshot.packedDepthImage, nullptr);
        snapshot.packedDepthImage = VK_NULL_HANDLE;
        return false;
    }

    result = p_vkBindImageMemory(g_device, snapshot.packedDepthImage,
                                 snapshot.packedDepthMemory, 0);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan packed depth snapshot bind failed (%d): key=%08X\n",
             result, snapshot.key);
        p_vkFreeMemory(g_device, snapshot.packedDepthMemory, nullptr);
        p_vkDestroyImage(g_device, snapshot.packedDepthImage, nullptr);
        snapshot.packedDepthMemory = VK_NULL_HANDLE;
        snapshot.packedDepthImage = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo packedViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    packedViewInfo.image = snapshot.packedDepthImage;
    packedViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    packedViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    packedViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = p_vkCreateImageView(g_device, &packedViewInfo, nullptr,
                                 &snapshot.packedDepthWriteView);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan packed depth snapshot view failed (%d): key=%08X\n",
             result, snapshot.key);
        p_vkFreeMemory(g_device, snapshot.packedDepthMemory, nullptr);
        p_vkDestroyImage(g_device, snapshot.packedDepthImage, nullptr);
        snapshot.packedDepthMemory = VK_NULL_HANDLE;
        snapshot.packedDepthImage = VK_NULL_HANDLE;
        return false;
    }

    snapshot.packedDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    snapshot.packedDepthInitialized = false;
    return true;
}

bool CreateSnapshot(uint32_t key, uint32_t width, uint32_t height, ResolveSnapshot*& out)
{
    out = FindColorSnapshot(key, width, height);
    if (out)
        return true;

    // The renderer currently rasterizes into one 1280x720 EDRAM stand-in.
    // Keep snapshots to surfaces of that same extent until the render-target allocator
    // exists; this still covers the real front-buffer resolves and avoids allocating
    // large intermediate surfaces that cannot be rendered correctly yet.
    if (width != g_extent.width || height != g_extent.height ||
        g_snapshots.size() >= kMaxResolveSnapshots)
    {
        static uint32_t rejectedReports = 0;
        if (rejectedReports++ < 32)
        {
            KLOG("[snapshot reject] kind=color key=%08X size=%ux%u host=%ux%u snapshots=%zu/%zu\n",
                 key, width, height, g_extent.width, g_extent.height,
                 g_snapshots.size(), kMaxResolveSnapshots);
        }
        return false;
    }

    ResolveSnapshot snapshot{};
    snapshot.key = key;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.format = g_swapFormat;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = g_swapFormat;
    const VkExtent2D internalExtent = InternalExtentForLogical(width, height);
    ii.extent = {internalExtent.width, internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &snapshot.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, snapshot.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &snapshot.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, snapshot.image, snapshot.memory, 0) != VK_SUCCESS)
    {
        if (snapshot.memory)
            p_vkFreeMemory(g_device, snapshot.memory, nullptr);
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = snapshot.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_swapFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &snapshot.view) != VK_SUCCESS)
    {
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        p_vkFreeMemory(g_device, snapshot.memory, nullptr);
        return false;
    }

    g_snapshots.push_back(snapshot);
    out = &g_snapshots.back();
    KLOG_DIAG("Vulkan resolve snapshot created: key=%08X %ux%u (candidates=%zu)\n",
              key, width, height, g_snapshots.size());
    return true;
}

bool EnsureSnapshotPresentImage(ResolveSnapshot& snapshot)
{
    if (snapshot.presentImage)
        return true;
    if (snapshot.depth || !snapshot.width || !snapshot.height)
        return false;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = snapshot.format;
    const VkExtent2D internalExtent = InternalExtentForLogical(snapshot.width, snapshot.height);
    ii.extent = {internalExtent.width, internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &snapshot.presentImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, snapshot.presentImage, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, snapshot.presentImage, nullptr);
        snapshot.presentImage = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &snapshot.presentMemory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, snapshot.presentImage, snapshot.presentMemory, 0) != VK_SUCCESS)
    {
        if (snapshot.presentMemory)
            p_vkFreeMemory(g_device, snapshot.presentMemory, nullptr);
        p_vkDestroyImage(g_device, snapshot.presentImage, nullptr);
        snapshot.presentImage = VK_NULL_HANDLE;
        snapshot.presentMemory = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = snapshot.presentImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = snapshot.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &snapshot.presentView) != VK_SUCCESS)
    {
        p_vkFreeMemory(g_device, snapshot.presentMemory, nullptr);
        p_vkDestroyImage(g_device, snapshot.presentImage, nullptr);
        snapshot.presentImage = VK_NULL_HANDLE;
        snapshot.presentMemory = VK_NULL_HANDLE;
        return false;
    }
    snapshot.presentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    snapshot.presentValid = false;
    return true;
}

bool CreateDepthSnapshot(uint32_t key, uint32_t width, uint32_t height,
                         VkFormat format, bool depthFloat24, ResolveSnapshot*& out)
{
    out = FindDepthSnapshot(key, width, height, format, depthFloat24);
    if (out)
        return true;
    if (width != g_extent.width || height != g_extent.height ||
        g_snapshots.size() >= kMaxResolveSnapshots)
    {
        static uint32_t rejectedReports = 0;
        if (rejectedReports++ < 32)
        {
            KLOG("[snapshot reject] kind=depth key=%08X size=%ux%u host=%ux%u format=%u snapshots=%zu/%zu\n",
                 key, width, height, g_extent.width, g_extent.height, uint32_t(format),
                 g_snapshots.size(), kMaxResolveSnapshots);
        }
        return false;
    }

    ResolveSnapshot snapshot{};
    snapshot.key = key;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.depth = true;
    snapshot.depthFloat24 = depthFloat24;
    snapshot.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    snapshot.format = format;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    const VkExtent2D internalExtent = InternalExtentForLogical(width, height);
    ii.extent = {internalExtent.width, internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &snapshot.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, snapshot.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &snapshot.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, snapshot.image, snapshot.memory, 0) != VK_SUCCESS)
    {
        if (snapshot.memory)
            p_vkFreeMemory(g_device, snapshot.memory, nullptr);
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = snapshot.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &snapshot.view) != VK_SUCCESS)
    {
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        p_vkFreeMemory(g_device, snapshot.memory, nullptr);
        return false;
    }

    // Preserve the guest resolve bit pattern separately from the native depth
    // image. Xenos depth resolves are 32-bit packed words (stencil in the low
    // byte, 24-bit depth in the upper bytes) and may later be rebound through
    // an 8_8_8_8 texture fetch. A Vulkan depth view can't expose those bytes.
    if (!CreatePackedDepthSnapshotResources(snapshot, internalExtent))
    {
        p_vkDestroyImageView(g_device, snapshot.view, nullptr);
        p_vkDestroyImage(g_device, snapshot.image, nullptr);
        p_vkFreeMemory(g_device, snapshot.memory, nullptr);
        return false;
    }
    g_snapshots.push_back(snapshot);
    out = &g_snapshots.back();
    KLOG_DIAG("Vulkan depth resolve snapshot created: key=%08X %ux%u format=%u (candidates=%zu)\n",
              key, width, height, uint32_t(format), g_snapshots.size());
    return true;
}

ColorBacking* FindColorBacking(uint64_t key)
{
    for (auto& backing : g_colorBackings)
        if (backing.key == key)
            return &backing;
    return nullptr;
}

ColorBacking* FindColorBacking(uint32_t surfaceInfo, uint32_t info)
{
    return FindColorBacking(ColorSurfaceKey(surfaceInfo, info));
}

ColorBacking* FindColorBackingByInfo(uint32_t info)
{
    // Diagnostic/front-buffer lookups sometimes only know RB_COLOR_INFO. Prefer
    // the active representation, then the most recently created matching one.
    if (g_activeColorSurfaceKey != UINT64_MAX)
    {
        ColorBacking* active = FindColorBacking(g_activeColorSurfaceKey);
        if (active && active->info == info)
            return active;
    }
    for (auto it = g_colorBackings.rbegin(); it != g_colorBackings.rend(); ++it)
        if (it->info == info)
            return &*it;
    return nullptr;
}

bool CreateColorBacking(uint32_t surfaceInfo, uint32_t info, ColorBacking*& out)
{
    const uint64_t key = ColorSurfaceKey(surfaceInfo, info);
    out = FindColorBacking(key);
    if (out)
        return true;
    if (g_colorBackings.size() >= 16)
    {
        static uint32_t reports = 0;
        if (reports++ < 32)
            KLOG("Vulkan EDRAM color backing capacity reached: key=%016llX count=%zu\n",
                 static_cast<unsigned long long>(key), g_colorBackings.size());
        return false;
    }

    ColorBacking backing{};
    backing.key = key;
    backing.surfaceInfo = surfaceInfo;
    backing.info = info;
    backing.baseTiles = xenos::ColorBaseTiles(info);
    backing.pitchTiles = xenos::SurfacePitchTiles(surfaceInfo,
                                                   xenos::ColorFormatIs64Bpp(info));
    backing.samples = GuestSurfaceSamples(surfaceInfo);
    if (!backing.samples)
    {
        KLOG("Vulkan EDRAM color surface uses unsupported guest MSAA: surface=%08X info=%08X\n",
             surfaceInfo, info);
        return false;
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = g_swapFormat;
    ii.extent = {g_internalExtent.width, g_internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = backing.samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &backing.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, backing.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, backing.image, nullptr);
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &backing.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, backing.image, backing.memory, 0) != VK_SUCCESS)
    {
        if (backing.memory)
            p_vkFreeMemory(g_device, backing.memory, nullptr);
        p_vkDestroyImage(g_device, backing.image, nullptr);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = backing.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_swapFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &backing.view) != VK_SUCCESS)
    {
        p_vkDestroyImage(g_device, backing.image, nullptr);
        p_vkFreeMemory(g_device, backing.memory, nullptr);
        return false;
    }
    if (backing.samples != VK_SAMPLE_COUNT_1_BIT)
    {
        VkImageCreateInfo resolveInfo = ii;
        resolveInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        if (p_vkCreateImage(g_device, &resolveInfo, nullptr, &backing.resolveImage) != VK_SUCCESS)
            return false;
        p_vkGetImageMemoryRequirements(g_device, backing.resolveImage, &req);
        const uint32_t resolveType = FindMemoryType(req.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (resolveType == UINT32_MAX)
            return false;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = resolveType;
        if (p_vkAllocateMemory(g_device, &ai, nullptr, &backing.resolveMemory) != VK_SUCCESS ||
            p_vkBindImageMemory(g_device, backing.resolveImage,
                                backing.resolveMemory, 0) != VK_SUCCESS)
            return false;
        vi.image = backing.resolveImage;
        if (p_vkCreateImageView(g_device, &vi, nullptr, &backing.resolveView) != VK_SUCCESS)
            return false;
    }
    backing.logicalRaster = !StitchPhysicalTileRasterEnabled();
    g_colorBackings.push_back(backing);
    out = &g_colorBackings.back();
    KLOG("Vulkan EDRAM color backing created: key=%016llX surface=%08X info=%08X "
         "base=%03X pitchTiles=%u samples=%u count=%zu\n",
         static_cast<unsigned long long>(key), surfaceInfo, info,
         backing.baseTiles, backing.pitchTiles, uint32_t(backing.samples),
         g_colorBackings.size());
    return true;
}
DepthBacking* FindDepthBacking(uint64_t key)
{
    for (auto& backing : g_depthBackings)
        if (backing.key == key)
            return &backing;
    return nullptr;
}

bool CreateDepthBacking(uint64_t key, DepthBacking*& out)
{
    out = FindDepthBacking(key);
    if (out)
        return true;
    if (g_depthBackings.size() >= 16)
    {
        static uint32_t reports = 0;
        if (reports++ < 32)
            KLOG("Vulkan EDRAM depth backing capacity reached: key=%016llX count=%zu\n",
                 static_cast<unsigned long long>(key), g_depthBackings.size());
        return false;
    }

    DepthBacking backing{};
    backing.key = key;
    backing.surfaceInfo = uint32_t(key >> 32);
    backing.info = uint32_t(key);
    backing.baseTiles = xenos::DepthBaseTiles(backing.info);
    backing.pitchTiles = xenos::SurfacePitchTiles(backing.surfaceInfo, false);
    backing.samples = GuestSurfaceSamples(backing.surfaceInfo, backing.info);
    if (!backing.samples)
    {
        KLOG("Vulkan EDRAM depth surface uses unsupported guest MSAA: surface=%08X info=%08X\n",
             backing.surfaceInfo, backing.info);
        return false;
    }
    backing.format = HostDepthFormat(backing.info);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = backing.format;
    ii.extent = {g_internalExtent.width, g_internalExtent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = backing.samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &backing.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, backing.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, backing.image, nullptr);
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &backing.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, backing.image, backing.memory, 0) != VK_SUCCESS)
    {
        if (backing.memory)
            p_vkFreeMemory(g_device, backing.memory, nullptr);
        p_vkDestroyImage(g_device, backing.image, nullptr);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = backing.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = backing.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &backing.view) != VK_SUCCESS)
        return false;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p_vkCreateImageView(g_device, &vi, nullptr, &backing.depthSampleView) != VK_SUCCESS)
        return false;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (p_vkCreateImageView(g_device, &vi, nullptr, &backing.stencilSampleView) != VK_SUCCESS)
        return false;

    if (backing.samples != VK_SAMPLE_COUNT_1_BIT)
    {
        VkImageCreateInfo resolveInfo = ii;
        resolveInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        resolveInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (p_vkCreateImage(g_device, &resolveInfo, nullptr, &backing.resolveImage) != VK_SUCCESS)
            return false;
        p_vkGetImageMemoryRequirements(g_device, backing.resolveImage, &req);
        const uint32_t resolveType = FindMemoryType(req.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (resolveType == UINT32_MAX)
            return false;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = resolveType;
        if (p_vkAllocateMemory(g_device, &ai, nullptr, &backing.resolveMemory) != VK_SUCCESS ||
            p_vkBindImageMemory(g_device, backing.resolveImage, backing.resolveMemory, 0) != VK_SUCCESS)
            return false;
        vi.image = backing.resolveImage;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (p_vkCreateImageView(g_device, &vi, nullptr, &backing.resolveView) != VK_SUCCESS)
            return false;
    }

    g_depthBackings.push_back(backing);
    out = &g_depthBackings.back();
    KLOG("Vulkan EDRAM depth backing created: key=%016llX surface=%08X info=%08X "
         "base=%03X pitchTiles=%u samples=%u format=%u count=%zu\n",
         static_cast<unsigned long long>(key), backing.surfaceInfo, backing.info,
         backing.baseTiles, backing.pitchTiles, uint32_t(backing.samples),
         uint32_t(backing.format),
         g_depthBackings.size());
    return true;
}
void TransitionDepthBacking(DepthBacking& backing, VkImageLayout next)
{
    if (!backing.image || backing.layout == next)
        return;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (backing.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    else if (backing.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }
    else if (backing.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (backing.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = backing.layout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = backing.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    backing.layout = next;
}

void TransitionColorResolveBacking(ColorBacking& backing, VkImageLayout next)
{
    if (!backing.resolveImage || backing.resolveLayout == next)
        return;
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (backing.resolveLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (backing.resolveLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (backing.resolveLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = backing.resolveLayout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = backing.resolveImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    backing.resolveLayout = next;
}

bool EnsureMsaaResolvePipeline()
{
    if (g_msaaResolvePipeline)
        return true;

    static constexpr const char* kVs = R"(
struct VSOut { float4 position : SV_Position; };
VSOut main(uint id : SV_VertexID)
{
    float2 p = id == 0 ? float2(-1.0, -1.0) :
               id == 1 ? float2(-1.0,  3.0) : float2(3.0, -1.0);
    VSOut o;
    o.position = float4(p, 0.0, 1.0);
    return o;
}
)";
    static constexpr const char* kPs = R"(
Texture2DMS<float4> g_Source : register(t0, space0);
float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 p = int2(position.xy);
    return (g_Source.Load(p, 0) + g_Source.Load(p, 1)) * 0.5;
}
)";

    std::vector<uint8_t> vsSpv, psSpv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(kVs, true, vsSpv, err))
    {
        KLOG("Vulkan MSAA resolve VS compile failed: %s\n", err.c_str());
        return false;
    }
    if (!ShaderTranslator::CompileHostHlsl(kPs, false, psSpv, err))
    {
        KLOG("Vulkan MSAA resolve PS compile failed: %s\n", err.c_str());
        return false;
    }

    auto makeModule = [](const std::vector<uint8_t>& spv, VkShaderModule& module) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spv.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
        return p_vkCreateShaderModule(g_device, &ci, nullptr, &module) == VK_SUCCESS;
    };
    if (!makeModule(vsSpv, g_msaaResolveVs) || !makeModule(psSpv, g_msaaResolvePs))
        return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (p_vkCreateDescriptorSetLayout(g_device, &setInfo, nullptr, &g_msaaResolveSetLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (p_vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_msaaResolvePool) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_msaaResolveSetLayout;
    if (p_vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &g_msaaResolvePipelineLayout) != VK_SUCCESS)
        return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_msaaResolveVs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_msaaResolvePs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g_swapFormat;
    VkGraphicsPipelineCreateInfo pipe{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipe.pNext = &rendering;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = g_msaaResolvePipelineLayout;
    if (p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &pipe, nullptr,
                                    &g_msaaResolvePipeline) != VK_SUCCESS)
        return false;

    KLOG("Vulkan MSAA shader resolve ready (2x sample0+1 average)\n");
    return true;
}

bool EnsureMsaaResolveDescriptor(ColorBacking& backing)
{
    if (backing.resolveDescriptor)
        return true;
    if (!EnsureMsaaResolvePipeline())
        return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_msaaResolvePool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_msaaResolveSetLayout;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &backing.resolveDescriptor) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo image{};
    image.imageView = backing.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = backing.resolveDescriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    p_vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    return true;
}

bool ResolveColorBackingMsaa(ColorBacking& backing, const VkImageCopy& copy)
{
    if (!g_hostMsaa2x)
        return true;
    if (!backing.resolveImage || !EnsureMsaaResolveDescriptor(backing))
        return false;

    static uint32_t reports = 0;
    const bool report = reports++ < 12;
    if (report)
        KLOG("[msaa2x resolve] begin info=%08X src=%d,%d dstLogical=%d,%d size=%ux%u primaryLayout=%u mirrorLayout=%u\n",
             backing.info, copy.srcOffset.x, copy.srcOffset.y,
             copy.dstOffset.x, copy.dstOffset.y,
             copy.extent.width, copy.extent.height,
             uint32_t(backing.layout), uint32_t(backing.resolveLayout));
    TransitionColorBacking(backing, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (report)
        KLOG("[msaa2x resolve] source-ready info=%08X layout=%u\n",
             backing.info, uint32_t(backing.layout));
    TransitionColorResolveBacking(backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (report)
        KLOG("[msaa2x resolve] mirror-ready info=%08X layout=%u\n",
             backing.info, uint32_t(backing.resolveLayout));

    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = backing.resolveView;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Preserve parts of the single-sample mirror written by previous tiled
    // resolves. Only the physical source region for this resolve is updated.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    const int32_t rx = std::max<int32_t>(0, copy.srcOffset.x);
    const int32_t ry = std::max<int32_t>(0, copy.srcOffset.y);
    const uint32_t rw = std::min<uint32_t>(copy.extent.width,
        g_extent.width > uint32_t(rx) ? g_extent.width - uint32_t(rx) : 0u);
    const uint32_t rh = std::min<uint32_t>(copy.extent.height,
        g_extent.height > uint32_t(ry) ? g_extent.height - uint32_t(ry) : 0u);
    if (!rw || !rh)
    {
        TransitionColorBacking(backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        return true;
    }
    const VkRect2D logicalRect{{rx, ry}, {rw, rh}};
    rendering.renderArea = ScaleRectToInternal(logicalRect);
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    p_vkCmdBeginRendering(g_commandBuffer, &rendering);

    VkViewport viewport{};
    viewport.width = float(g_internalExtent.width);
    viewport.height = float(g_internalExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor = ScaleRectToInternal(logicalRect);
    CmdSetViewportCached(g_commandBuffer, viewport);
    CmdSetScissorCached(g_commandBuffer, scissor);
    CmdBindGraphicsPipelineCached(g_commandBuffer, g_msaaResolvePipeline);
    CmdBindDescriptorSetsCached(g_commandBuffer, g_msaaResolvePipelineLayout,
                                0, 1, &backing.resolveDescriptor);
    p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
    p_vkCmdEndRendering(g_commandBuffer);
    if (report)
        KLOG("[msaa2x resolve] shader-average issued info=%08X\n", backing.info);
    TransitionColorResolveBacking(backing, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionColorBacking(backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (report)
        KLOG("[msaa2x resolve] complete info=%08X mirrorLayout=%u\n",
             backing.info, uint32_t(backing.resolveLayout));
    return true;
}

bool EnsureDepthMsaaResolveInfrastructure()
{
    if (g_depthMsaaResolvePipelineLayout)
        return true;

    static constexpr const char* kVs = R"(
struct VSOut { float4 position : SV_Position; };
VSOut main(uint id : SV_VertexID)
{
    float2 p = id == 0 ? float2(-1.0, -1.0) :
               id == 1 ? float2(-1.0,  3.0) : float2(3.0, -1.0);
    VSOut o;
    o.position = float4(p, 0.0, 1.0);
    return o;
}
)";
    static constexpr const char* kPs = R"(
Texture2DMS<float> g_Source : register(t0, space0);
float main(float4 position : SV_Position) : SV_Depth
{
    return g_Source.Load(int2(position.xy), 0);
}
)";

    std::vector<uint8_t> vsSpv, psSpv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(kVs, true, vsSpv, err))
    {
        KLOG("Vulkan depth MSAA fallback VS compile failed: %s\n", err.c_str());
        return false;
    }
    if (!ShaderTranslator::CompileHostHlsl(kPs, false, psSpv, err))
    {
        KLOG("Vulkan depth MSAA fallback PS compile failed: %s\n", err.c_str());
        return false;
    }

    auto makeModule = [](const std::vector<uint8_t>& spv, VkShaderModule& module) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spv.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
        return p_vkCreateShaderModule(g_device, &ci, nullptr, &module) == VK_SUCCESS;
    };
    if (!makeModule(vsSpv, g_depthMsaaResolveVs) ||
        !makeModule(psSpv, g_depthMsaaResolvePs))
        return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (p_vkCreateDescriptorSetLayout(g_device, &setInfo, nullptr,
                                      &g_depthMsaaResolveSetLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 32;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (p_vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                                 &g_depthMsaaResolvePool) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_depthMsaaResolveSetLayout;
    if (p_vkCreatePipelineLayout(g_device, &layoutInfo, nullptr,
                                 &g_depthMsaaResolvePipelineLayout) != VK_SUCCESS)
        return false;

    KLOG("Vulkan depth MSAA shader SAMPLE_ZERO fallback ready\n");
    return true;
}

VkPipeline GetDepthMsaaResolvePipeline(VkFormat format)
{
    VkPipeline* pipeline = nullptr;
    if (format == kDepthUnormFormat)
        pipeline = &g_depthMsaaResolveUnormPipeline;
    else if (format == kDepthFloatFormat)
        pipeline = &g_depthMsaaResolveFloatPipeline;
    else
        return VK_NULL_HANDLE;
    if (*pipeline)
        return *pipeline;
    if (!EnsureDepthMsaaResolveInfrastructure())
        return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_depthMsaaResolveVs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_depthMsaaResolvePs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.depthAttachmentFormat = format;

    VkGraphicsPipelineCreateInfo pipe{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipe.pNext = &rendering;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &viewport;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &depth;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = g_depthMsaaResolvePipelineLayout;
    if (p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &pipe, nullptr,
                                    pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    KLOG("Vulkan depth MSAA shader resolve pipeline ready: format=%u\n",
         uint32_t(format));
    return *pipeline;
}

bool EnsureDepthMsaaResolveDescriptor(DepthBacking& backing)
{
    if (backing.resolveDescriptor)
        return true;
    if (!EnsureDepthMsaaResolveInfrastructure())
        return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_depthMsaaResolvePool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_depthMsaaResolveSetLayout;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &backing.resolveDescriptor) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo image{};
    image.imageView = backing.depthSampleView;
    image.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = backing.resolveDescriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    p_vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    return true;
}

bool ResolveDepthBackingMsaaSampleZero(DepthBacking& backing, const VkImageCopy& copy)
{
    if (backing.samples == VK_SAMPLE_COUNT_1_BIT)
        return true;
    if (!backing.resolveImage || !backing.resolveView ||
        !EnsureDepthMsaaResolveDescriptor(backing))
        return false;
    const VkPipeline pipeline = GetDepthMsaaResolvePipeline(backing.format);
    if (!pipeline)
        return false;

    const int32_t rx = std::max<int32_t>(0, copy.srcOffset.x);
    const int32_t ry = std::max<int32_t>(0, copy.srcOffset.y);
    const uint32_t rw = std::min<uint32_t>(copy.extent.width,
        g_extent.width > uint32_t(rx) ? g_extent.width - uint32_t(rx) : 0u);
    const uint32_t rh = std::min<uint32_t>(copy.extent.height,
        g_extent.height > uint32_t(ry) ? g_extent.height - uint32_t(ry) : 0u);
    if (!rw || !rh)
        return true;

    EndColorRendering();
    TransitionDepthBacking(backing, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    TransitionResolveTarget(backing.resolveImage, backing.resolveLayout,
                            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = backing.resolveView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    const VkRect2D logicalRect{{rx, ry}, {rw, rh}};
    rendering.renderArea = ScaleRectToInternal(logicalRect);
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    p_vkCmdBeginRendering(g_commandBuffer, &rendering);

    VkViewport vp{};
    vp.width = float(g_internalExtent.width);
    vp.height = float(g_internalExtent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    const VkRect2D scissor = ScaleRectToInternal(logicalRect);
    CmdSetViewportCached(g_commandBuffer, vp);
    CmdSetScissorCached(g_commandBuffer, scissor);
    CmdBindGraphicsPipelineCached(g_commandBuffer, pipeline);
    CmdBindDescriptorSetsCached(g_commandBuffer, g_depthMsaaResolvePipelineLayout,
                                0, 1, &backing.resolveDescriptor);
    p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
    p_vkCmdEndRendering(g_commandBuffer);

    TransitionDepthBacking(backing, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    static uint32_t reports = 0;
    if (reports++ < 12)
        KLOG("[depth msaa fallback] SAMPLE_ZERO info=%08X samples=%u rect=%d,%d %ux%u\n",
             backing.info, uint32_t(backing.samples), rx, ry, rw, rh);
    return true;
}

void TransitionColorBacking(ColorBacking& backing, VkImageLayout next)
{
    if (backing.layout == next)
        return;
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (backing.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        srcAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (backing.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (backing.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }
    else if (backing.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = backing.layout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = backing.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    backing.layout = next;
}

struct EdramBackingMeta
{
    EdramOwnerKind kind = EdramOwnerKind::None;
    uint64_t key = UINT64_MAX;
    uint32_t info = 0;
    uint32_t baseTiles = 0;
    uint32_t pitchTiles = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits guestSamples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

bool GetEdramBackingMeta(const EdramOwner& owner, EdramBackingMeta& meta)
{
    meta = {};
    meta.kind = owner.kind;
    meta.key = owner.key;
    if (owner.kind == EdramOwnerKind::Color)
    {
        ColorBacking* backing = FindColorBacking(owner.key);
        if (!backing)
            return false;
        meta.info = backing->info;
        meta.baseTiles = backing->baseTiles;
        meta.pitchTiles = backing->pitchTiles;
        meta.samples = backing->samples;
        meta.guestSamples = GuestEdramSamples(backing->surfaceInfo);
        meta.format = g_swapFormat;
        return true;
    }
    if (owner.kind == EdramOwnerKind::Depth)
    {
        DepthBacking* backing = FindDepthBacking(owner.key);
        if (!backing)
            return false;
        meta.info = backing->info;
        meta.baseTiles = backing->baseTiles;
        meta.pitchTiles = backing->pitchTiles;
        meta.samples = backing->samples;
        meta.guestSamples = GuestEdramSamples(backing->surfaceInfo);
        meta.format = backing->format;
        return true;
    }
    return false;
}

bool EdramOwnerEqual(const EdramOwner& a, const EdramOwner& b)
{
    return a.kind == b.kind && a.key == b.key;
}

void RecordEdramTransferPair(const EdramOwner& source, const EdramOwner& dest,
                             uint32_t tiles)
{
    if (!EdramOwnershipPairProfileEnabled())
        return;
    for (uint32_t i = 0; i < g_perfEdramTransferPairCount; ++i)
    {
        auto& pair = g_perfEdramTransferPairs[i];
        if (EdramOwnerEqual(pair.source, source) && EdramOwnerEqual(pair.dest, dest))
        {
            ++pair.transfers;
            pair.tiles += tiles;
            return;
        }
    }
    if (g_perfEdramTransferPairCount < g_perfEdramTransferPairs.size())
    {
        auto& pair = g_perfEdramTransferPairs[g_perfEdramTransferPairCount++];
        pair.source = source;
        pair.dest = dest;
        pair.transfers = 1;
        pair.tiles = tiles;
    }
}

void ResetEdramTransferPairProfile()
{
    g_perfEdramTransferPairCount = 0;
    for (auto& pair : g_perfEdramTransferPairs)
        pair = {};
}

EdramOwnerTileCount* FindEdramOwnerTileCount(const EdramOwner& owner)
{
    if (owner.kind == EdramOwnerKind::None)
        return nullptr;
    for (auto& entry : g_edramOwnerTileCounts)
    {
        if (EdramOwnerEqual(entry.owner, owner))
            return &entry;
    }
    return nullptr;
}

uint32_t EdramOwnedTileCount(const EdramOwner& owner)
{
    if (EdramOwnerTileCount* entry = FindEdramOwnerTileCount(owner))
        return entry->tiles;
    return 0;
}

void AdjustEdramOwnedTileCount(const EdramOwner& owner, int32_t delta)
{
    if (owner.kind == EdramOwnerKind::None || delta == 0)
        return;
    EdramOwnerTileCount* entry = FindEdramOwnerTileCount(owner);
    if (!entry)
    {
        if (delta <= 0)
            return;
        g_edramOwnerTileCounts.push_back({owner, static_cast<uint32_t>(delta)});
        return;
    }
    if (delta > 0)
    {
        entry->tiles += static_cast<uint32_t>(delta);
    }
    else
    {
        const uint32_t remove = static_cast<uint32_t>(-delta);
        entry->tiles = entry->tiles > remove ? entry->tiles - remove : 0;
    }
}

bool EdramTransferFormatSupported(const EdramBackingMeta& meta)
{
    if (meta.kind == EdramOwnerKind::Color)
    {
        // The gameplay surfaces involved in the physical EDRAM alias are
        // Xenos 8_8_8_8. Other color layouts need their own bit packers.
        return xenos::ColorFormat(meta.info) == 0 &&
               !xenos::ColorFormatIs64Bpp(meta.info);
    }
    if (meta.kind == EdramOwnerKind::Depth)
    {
        // Both Xenos 24-bit depth encodings are handled by the ownership
        // transfer shader: D24S8 is UNORM24 and D24FS8 is positive 20e4.
        return true;
    }
    return false;
}

uint32_t EdramTileWidthPixels(VkSampleCountFlagBits samples)
{
    return xenos::kEdramTileWidthSamples >>
           uint32_t(samples == VK_SAMPLE_COUNT_4_BIT);
}

uint32_t EdramTileHeightPixels(VkSampleCountFlagBits samples)
{
    return xenos::kEdramTileHeightSamples >>
           uint32_t(samples != VK_SAMPLE_COUNT_1_BIT);
}

uint32_t EdramBackingTileCount(const EdramBackingMeta& meta)
{
    if (!meta.pitchTiles)
        return 0;
    const uint32_t tileHeight = EdramTileHeightPixels(meta.guestSamples);
    const uint32_t rows = (g_extent.height + tileHeight - 1u) / tileHeight;
    return std::min<uint32_t>(xenos::kEdramTileCount, meta.pitchTiles * rows);
}

bool EnsureEdramTransferInfrastructure()
{
    if (g_edramTransferPipelineLayout)
        return true;

    static constexpr const char* kVs = R"(
struct VSOut { float4 position : SV_Position; };
VSOut main(uint id : SV_VertexID)
{
    float2 p = id == 0 ? float2(-1.0, -1.0) :
               id == 1 ? float2(-1.0,  3.0) : float2(3.0, -1.0);
    VSOut o;
    o.position = float4(p, 0.0, 1.0);
    return o;
}
)";

    std::vector<uint8_t> vsSpv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(kVs, true, vsSpv, err))
    {
        KLOG("Vulkan EDRAM transfer VS compile failed: %s\n", err.c_str());
        return false;
    }
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = vsSpv.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(vsSpv.data());
    if (p_vkCreateShaderModule(g_device, &moduleInfo, nullptr, &g_edramTransferVs) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (p_vkCreateDescriptorSetLayout(g_device, &setInfo, nullptr,
                                      &g_edramTransferSetLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 128;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (p_vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                                 &g_edramTransferPool) != VK_SUCCESS)
        return false;

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(uint32_t) * 5;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_edramTransferSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (p_vkCreatePipelineLayout(g_device, &layoutInfo, nullptr,
                                 &g_edramTransferPipelineLayout) != VK_SUCCESS)
        return false;

    KLOG("Vulkan physical EDRAM transfer infrastructure ready\n");
    return true;
}

std::string BuildEdramTransferPixelShader(EdramOwnerKind sourceKind,
                                          EdramOwnerKind destKind,
                                          VkSampleCountFlagBits sourceSamples,
                                          VkSampleCountFlagBits destSamples,
                                          VkSampleCountFlagBits sourceGuestSamples,
                                          VkSampleCountFlagBits destGuestSamples,
                                          uint32_t sourcePitchTiles,
                                          uint32_t destPitchTiles,
                                          bool sourceDepthFloat24,
                                          bool destDepthFloat24,
                                          EdramTransferPass pass)
{
    const uint32_t srcSamples = uint32_t(sourceSamples);
    const uint32_t dstSamples = uint32_t(destSamples);
    std::ostringstream h;
    h << "#define SRC_MSAA " << srcSamples << "\n";
    h << "#define DST_MSAA " << dstSamples << "\n";
    h << "#define SRC_GUEST_MSAA " << uint32_t(sourceGuestSamples) << "\n";
    h << "#define DST_GUEST_MSAA " << uint32_t(destGuestSamples) << "\n";
    h << "#define SRC_PITCH_TILES " << sourcePitchTiles << "u\n";
    h << "#define DST_PITCH_TILES " << destPitchTiles << "u\n";
    h << "#define SRC_DEPTH " << (sourceKind == EdramOwnerKind::Depth ? 1 : 0) << "\n";
    h << "#define DST_DEPTH " << (destKind == EdramOwnerKind::Depth ? 1 : 0) << "\n";
    h << "#define SRC_DEPTH_FLOAT24 " << (sourceDepthFloat24 ? 1 : 0) << "\n";
    h << "#define DST_DEPTH_FLOAT24 " << (destDepthFloat24 ? 1 : 0) << "\n";
    h << "#define HOST_SCALE " << g_resolutionScale << "u\n";
    h << R"(
struct TransferPush {
    uint sourceBaseTiles;
    uint sourcePitchTiles;
    uint destBaseTiles;
    uint destPitchTiles;
    uint stencilBit;
};
[[vk::push_constant]] ConstantBuffer<TransferPush> g_Push;

#if SRC_DEPTH
  #if SRC_MSAA == 1
    Texture2D<float> g_SourceDepth : register(t0, space0);
    Texture2D<uint> g_SourceStencil : register(t1, space0);
  #else
    Texture2DMS<float> g_SourceDepth : register(t0, space0);
    Texture2DMS<uint> g_SourceStencil : register(t1, space0);
  #endif
#else
  #if SRC_MSAA == 1
    Texture2D<float4> g_SourceColor : register(t0, space0);
    Texture2D<float4> g_SourceUnused : register(t1, space0);
  #else
    Texture2DMS<float4> g_SourceColor : register(t0, space0);
    Texture2DMS<float4> g_SourceUnused : register(t1, space0);
  #endif
#endif

uint2 CanonicalizeDest(uint2 p, uint sample)
{
#if DST_GUEST_MSAA == 4
    // Host 1x is the optimized path and has no per-sample storage. In that
    // case use guest sample 0 as the representative physical sample while
    // retaining the guest 4x pixel/tile geometry. Native host 4x is exact.
  #if DST_MSAA == 4
    uint guestSample = sample;
  #else
    uint guestSample = 0u;
  #endif
    return uint2((p.x << 1) | (guestSample & 1u),
                 (p.y << 1) | ((guestSample >> 1) & 1u));
#elif DST_GUEST_MSAA == 2
    // Xenos 2x is a 1x2 sample block. Native Vulkan 2x numbering is reversed;
    // host 1x again represents the pixel with guest sample 0.
  #if DST_MSAA == 2
    uint guestSample = sample ^ 1u;
  #else
    uint guestSample = 0u;
  #endif
    return uint2(p.x, (p.y << 1) | guestSample);
#else
    return p;
#endif
}

void DecanonicalizeSource(uint2 c, out uint2 p, out uint sample)
{
#if SRC_GUEST_MSAA == 4
    p = c >> 1;
    uint guestSample = (c.x & 1u) | ((c.y & 1u) << 1);
  #if SRC_MSAA == 4
    sample = guestSample;
  #else
    sample = 0u;
  #endif
#elif SRC_GUEST_MSAA == 2
    p = uint2(c.x, c.y >> 1);
    uint guestSample = c.y & 1u;
  #if SRC_MSAA == 2
    sample = guestSample ^ 1u;
  #else
    sample = 0u;
  #endif
#else
    p = c;
    sample = 0u;
#endif
}

uint PackColor(float4 c)
{
    uint4 b = (uint4)round(saturate(c) * 255.0f);
    return (b.x & 255u) | ((b.y & 255u) << 8) |
           ((b.z & 255u) << 16) | ((b.w & 255u) << 24);
}

float4 UnpackColor(uint p)
{
    return float4(float(p & 255u), float((p >> 8) & 255u),
                  float((p >> 16) & 255u), float((p >> 24) & 255u)) / 255.0f;
}

uint Float32To20e4(float value)
{
    if (!(value > 0.0f))
        return 0u;
    uint bits = asuint(value);
    if (bits >= 0x3FFFFFF8u)
        return 0xFFFFFFu;
    if (bits < 0x38800000u)
    {
        uint shift = min(113u - (bits >> 23), 24u);
        bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
    }
    else
    {
        bits += 0xC8000000u;
    }
    bits += 3u + ((bits >> 3) & 1u);
    return (bits >> 3) & 0xFFFFFFu;
}

float Float20e4To32(uint value)
{
    value &= 0xFFFFFFu;
    if (value == 0u)
        return 0.0f;
    uint mantissa = value & 0xFFFFFu;
    int exponent = int(value >> 20);
    if (exponent == 0)
    {
        int highest = firstbithigh(mantissa);
        uint shift = uint(20 - highest);
        exponent = 1 - int(shift);
        mantissa = (mantissa << shift) & 0xFFFFFu;
    }
    return asfloat((uint(exponent + 112) << 23) | (mantissa << 3));
}

uint LoadPacked(uint2 p, uint sample)
{
#if SRC_DEPTH
  #if SRC_MSAA == 1
    float depth = g_SourceDepth.Load(int3(int2(p), 0));
    uint stencil = g_SourceStencil.Load(int3(int2(p), 0));
  #else
    float depth = g_SourceDepth.Load(int2(p), sample);
    uint stencil = g_SourceStencil.Load(int2(p), sample);
  #endif
  #if SRC_DEPTH_FLOAT24
    uint d24 = Float32To20e4(depth * 2.0f);
  #else
    uint d24 = (uint)round(saturate(depth) * 16777215.0f);
  #endif
    return (stencil & 255u) | ((d24 & 0xFFFFFFu) << 8);
#else
  #if SRC_MSAA == 1
    float4 color = g_SourceColor.Load(int3(int2(p), 0));
  #else
    float4 color = g_SourceColor.Load(int2(p), sample);
  #endif
    return PackColor(color);
#endif
}

void MapPhysical(float4 position, uint destSample, out uint2 srcPixel, out uint srcSample)
{
    // SV_Position is in host/internal pixels. Preserve every supersampled
    // subpixel while performing the Xenos EDRAM ownership mapping in guest
    // logical pixels.
    uint2 hostPixel = uint2(position.xy);
    uint2 subPixel = hostPixel % HOST_SCALE;
    uint2 dstPixel = hostPixel / HOST_SCALE;
    const uint dstTileWidth = (DST_GUEST_MSAA == 4) ? 40u : 80u;
    const uint dstTileHeight = (DST_GUEST_MSAA == 1) ? 16u : 8u;
    const uint srcTileWidth = (SRC_GUEST_MSAA == 4) ? 40u : 80u;
    const uint srcTileHeight = (SRC_GUEST_MSAA == 1) ? 16u : 8u;

    uint2 dstTile = dstPixel / uint2(dstTileWidth, dstTileHeight);
    uint2 dstLocalPixel = dstPixel % uint2(dstTileWidth, dstTileHeight);
    uint dstLocalTile = dstTile.y * DST_PITCH_TILES + dstTile.x;
    uint physicalTile = (g_Push.destBaseTiles + dstLocalTile) & 2047u;
    uint srcLocalTile = (physicalTile - g_Push.sourceBaseTiles) & 2047u;
    uint2 srcTile = uint2(srcLocalTile % SRC_PITCH_TILES,
                          srcLocalTile / SRC_PITCH_TILES);

    uint2 canonical = CanonicalizeDest(dstLocalPixel, destSample);
    uint2 srcLocalPixel;
    DecanonicalizeSource(canonical, srcLocalPixel, srcSample);

#if SRC_DEPTH != DST_DEPTH
    // Xenos color and depth swap the two 40-sample halves of every 32bpp
    // EDRAM tile. Do this after sample-layout remapping, exactly like Xenia.
    uint halfPixels = srcTileWidth >> 1;
    srcLocalPixel.x = srcLocalPixel.x < halfPixels
        ? srcLocalPixel.x + halfPixels
        : srcLocalPixel.x - halfPixels;
#endif

    srcPixel = (srcTile * uint2(srcTileWidth, srcTileHeight) + srcLocalPixel) *
               HOST_SCALE + subPixel;
}

uint LoadPhysical(float4 position, uint destSample)
{
    uint2 srcPixel;
    uint srcSample;
    MapPhysical(position, destSample, srcPixel, srcSample);
    return LoadPacked(srcPixel, srcSample);
}
)";

    if (pass == EdramTransferPass::StencilBitPlanes)
    {
        h << "\nvoid main(float4 position : SV_Position";
        if (destSamples != VK_SAMPLE_COUNT_1_BIT)
            h << ", uint sample : SV_SampleIndex";
        h << ")\n{\n    uint destSample = "
          << (destSamples == VK_SAMPLE_COUNT_1_BIT ? "0u" : "sample") << ";\n"
             "    uint packed = LoadPhysical(position, destSample);\n"
             "    if ((packed & g_Push.stencilBit) == 0u) discard;\n"
             "}\n";
    }
    else if (destKind == EdramOwnerKind::Depth)
    {
        const bool exportStencil = pass == EdramTransferPass::DepthStencilExport;
        if (exportStencil)
            h << "\nstruct PSOut { float depth : SV_Depth; uint stencil : SV_StencilRef; };\n";
        h << (exportStencil ? "PSOut" : "float")
          << " main(float4 position : SV_Position";
        if (destSamples != VK_SAMPLE_COUNT_1_BIT)
            h << ", uint sample : SV_SampleIndex";
        h << ")" << (exportStencil ? "" : " : SV_Depth")
          << "\n{\n    uint destSample = "
          << (destSamples == VK_SAMPLE_COUNT_1_BIT ? "0u" : "sample") << ";\n";
        if (exportStencil && sourceKind == EdramOwnerKind::Depth &&
            sourceDepthFloat24 == destDepthFloat24)
        {
            // Same depth encoding: only the physical pixel/sample address changes.
            // Re-encoding D24 (especially 20e4) is redundant and expensive.
            h << "    uint2 srcPixel; uint srcSample;\n"
                 "    MapPhysical(position, destSample, srcPixel, srcSample);\n"
                 "    PSOut o;\n"
                 "#if SRC_MSAA == 1\n"
                 "    o.depth = g_SourceDepth.Load(int3(int2(srcPixel), 0));\n"
                 "    o.stencil = g_SourceStencil.Load(int3(int2(srcPixel), 0));\n"
                 "#else\n"
                 "    o.depth = g_SourceDepth.Load(int2(srcPixel), srcSample);\n"
                 "    o.stencil = g_SourceStencil.Load(int2(srcPixel), srcSample);\n"
                 "#endif\n"
                 "    return o;\n"
                 "}\n";
        }
        else
        {
            h << "    uint packed = LoadPhysical(position, destSample);\n"
                 "    uint d24 = (packed >> 8) & 0xFFFFFFu;\n"
                 "#if DST_DEPTH_FLOAT24\n"
                 "    float depth = Float20e4To32(d24) * 0.5f;\n"
                 "#else\n"
                 "    float depth = float(d24) / 16777215.0f;\n"
                 "#endif\n";
            if (exportStencil)
                h << "    PSOut o; o.depth = depth; o.stencil = packed & 255u; return o;\n";
            else
                h << "    return depth;\n";
            h << "}\n";
        }
    }
    else
    {
        h << "\nfloat4 main(float4 position : SV_Position";
        if (destSamples != VK_SAMPLE_COUNT_1_BIT)
            h << ", uint sample : SV_SampleIndex";
        h << ") : SV_Target0\n{\n    uint destSample = "
          << (destSamples == VK_SAMPLE_COUNT_1_BIT ? "0u" : "sample") << ";\n";
        if (sourceKind == EdramOwnerKind::Color)
        {
            // RGBA8 -> RGBA8 needs only physical sample remapping. The source
            // image is already quantized; packing to uint and unpacking it again
            // produces the same value while wasting ALU on every transferred pixel.
            h << "    uint2 srcPixel; uint srcSample;\n"
                 "    MapPhysical(position, destSample, srcPixel, srcSample);\n"
                 "#if SRC_MSAA == 1\n"
                 "    return g_SourceColor.Load(int3(int2(srcPixel), 0));\n"
                 "#else\n"
                 "    return g_SourceColor.Load(int2(srcPixel), srcSample);\n"
                 "#endif\n"
                 "}\n";
        }
        else
        {
            h << "    return UnpackColor(LoadPhysical(position, destSample));\n}\n";
        }
    }
    return h.str();
}

EdramTransferPipeline* GetEdramTransferPipeline(EdramOwnerKind sourceKind,
                                                EdramOwnerKind destKind,
                                                VkSampleCountFlagBits sourceSamples,
                                                VkSampleCountFlagBits destSamples,
                                                VkSampleCountFlagBits sourceGuestSamples,
                                                VkSampleCountFlagBits destGuestSamples,
                                                uint32_t sourcePitchTiles,
                                                uint32_t destPitchTiles,
                                                bool sourceDepthFloat24,
                                                bool destDepthFloat24,
                                                VkFormat destDepthFormat,
                                                EdramTransferPass pass)
{
    for (auto& rec : g_edramTransferPipelines)
        if (rec.sourceKind == sourceKind && rec.destKind == destKind &&
            rec.sourceSamples == sourceSamples && rec.destSamples == destSamples &&
            rec.sourceGuestSamples == sourceGuestSamples &&
            rec.destGuestSamples == destGuestSamples &&
            rec.sourcePitchTiles == sourcePitchTiles &&
            rec.destPitchTiles == destPitchTiles &&
            rec.sourceDepthFloat24 == sourceDepthFloat24 &&
            rec.destDepthFloat24 == destDepthFloat24 &&
            rec.destDepthFormat == destDepthFormat && rec.pass == pass)
            return rec.pipeline ? &rec : nullptr;

    if (!EnsureEdramTransferInfrastructure())
        return nullptr;
    if (destSamples != VK_SAMPLE_COUNT_1_BIT && !g_sampleRateShading)
    {
        KLOG("Vulkan EDRAM transfer needs sampleRateShading for %ux destination\n",
             uint32_t(destSamples));
        return nullptr;
    }
    const std::string hlsl = BuildEdramTransferPixelShader(sourceKind, destKind,
                                                           sourceSamples, destSamples,
                                                           sourceGuestSamples,
                                                           destGuestSamples,
                                                           sourcePitchTiles,
                                                           destPitchTiles,
                                                           sourceDepthFloat24,
                                                           destDepthFloat24, pass);
    std::vector<uint8_t> psSpv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(hlsl, false, psSpv, err,
                                           pass == EdramTransferPass::DepthStencilExport))
    {
        if (const char* dumpDir = std::getenv("MOJORECOMP_EDRAM_TRANSFER_SHADER_DUMP_DIR");
            dumpDir && *dumpDir)
        {
            std::error_code ec;
            std::filesystem::create_directories(dumpDir, ec);
            char name[96]{};
            std::snprintf(name, sizeof(name), "edram-transfer-%u-%u-to-%u-%u.hlsl",
                          uint32_t(sourceKind), uint32_t(sourceSamples),
                          uint32_t(destKind), uint32_t(destSamples));
            const std::filesystem::path path = std::filesystem::path(dumpDir) / name;
            std::ofstream output(path, std::ios::binary);
            if (output)
            {
                output.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
                KLOG("[DEBUG-edram-hlsl] dumped rejected shader to %s\n",
                     path.string().c_str());
            }
        }
        KLOG("Vulkan EDRAM transfer PS compile failed src=%u/%u dst=%u/%u: %s\n",
             uint32_t(sourceKind), uint32_t(sourceSamples),
             uint32_t(destKind), uint32_t(destSamples), err.c_str());
        return nullptr;
    }

    EdramTransferPipeline rec{};
    rec.sourceKind = sourceKind;
    rec.destKind = destKind;
    rec.sourceSamples = sourceSamples;
    rec.destSamples = destSamples;
    rec.sourceGuestSamples = sourceGuestSamples;
    rec.destGuestSamples = destGuestSamples;
    rec.sourcePitchTiles = sourcePitchTiles;
    rec.destPitchTiles = destPitchTiles;
    rec.sourceDepthFloat24 = sourceDepthFloat24;
    rec.destDepthFloat24 = destDepthFloat24;
    rec.destDepthFormat = destDepthFormat;
    rec.pass = pass;
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = psSpv.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(psSpv.data());
    if (p_vkCreateShaderModule(g_device, &moduleInfo, nullptr, &rec.ps) != VK_SUCCESS)
        return nullptr;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_edramTransferVs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = rec.ps;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = destSamples;
    if (destSamples != VK_SAMPLE_COUNT_1_BIT)
    {
        ms.sampleShadingEnable = VK_TRUE;
        ms.minSampleShading = 1.0f;
    }

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    if (destKind == EdramOwnerKind::Color)
    {
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
    }

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (pass == EdramTransferPass::DepthStencilExport ||
        pass == EdramTransferPass::DepthOnly)
    {
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depth.stencilTestEnable = pass == EdramTransferPass::DepthStencilExport;
        if (depth.stencilTestEnable)
        {
            // SV_StencilRef supplies the replacement value, while the fixed-
            // function stencil state must still admit every fragment and write
            // all eight bits. A zero-initialized VkStencilOpState compares NEVER.
            depth.front.failOp = VK_STENCIL_OP_REPLACE;
            depth.front.passOp = VK_STENCIL_OP_REPLACE;
            depth.front.depthFailOp = VK_STENCIL_OP_REPLACE;
            depth.front.compareOp = VK_COMPARE_OP_ALWAYS;
            depth.front.compareMask = 0xFFu;
            depth.front.writeMask = 0xFFu;
            depth.back = depth.front;
        }
    }
    else if (pass == EdramTransferPass::StencilBitPlanes)
    {
        depth.stencilTestEnable = VK_TRUE;
        depth.front.failOp = VK_STENCIL_OP_REPLACE;
        depth.front.passOp = VK_STENCIL_OP_REPLACE;
        depth.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth.front.compareMask = 0xFFu;
        depth.front.writeMask = 0xFFu;
        depth.back = depth.front;
    }

    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    };
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = pass == EdramTransferPass::StencilBitPlanes
        ? uint32_t(std::size(dynamicStates)) : 2u;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat colorFormat = g_swapFormat;
    if (destKind == EdramOwnerKind::Color)
    {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
    }
    else
    {
        rendering.depthAttachmentFormat = destDepthFormat;
        rendering.stencilAttachmentFormat = destDepthFormat;
    }

    VkGraphicsPipelineCreateInfo pipe{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipe.pNext = &rendering;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = destKind == EdramOwnerKind::Depth ? &depth : nullptr;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = g_edramTransferPipelineLayout;
    const VkResult result = p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1,
                                                        &pipe, nullptr, &rec.pipeline);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan EDRAM transfer pipeline failed (%d) src=%u/%u dst=%u/%u\n",
             result, uint32_t(sourceKind), uint32_t(sourceSamples),
             uint32_t(destKind), uint32_t(destSamples));
        p_vkDestroyShaderModule(g_device, rec.ps, nullptr);
        return nullptr;
    }

    g_edramTransferPipelines.push_back(rec);
    KLOG_DIAG("Vulkan EDRAM transfer pipeline ready src=%s host=%ux guest=%ux%s pitch=%u "
              "dst=%s host=%ux guest=%ux%s pitch=%u\n",
              sourceKind == EdramOwnerKind::Color ? "color" : "depth", uint32_t(sourceSamples),
              uint32_t(sourceGuestSamples),
              sourceDepthFloat24 ? "/f24" : "",
              sourcePitchTiles,
              destKind == EdramOwnerKind::Color ? "color" : "depth", uint32_t(destSamples),
              uint32_t(destGuestSamples),
              destDepthFloat24 ? "/f24" : "", destPitchTiles);
    return &g_edramTransferPipelines.back();
}

DepthSnapshotPackPipeline* GetDepthSnapshotPackPipeline(
    VkSampleCountFlagBits sourceSamples, bool sourceDepthFloat24,
    uint32_t resolveEndian)
{
    resolveEndian &= 3u;
    for (auto& rec : g_depthSnapshotPackPipelines)
        if (rec.sourceSamples == sourceSamples &&
            rec.sourceDepthFloat24 == sourceDepthFloat24 &&
            rec.resolveEndian == resolveEndian)
            return rec.pipeline ? &rec : nullptr;

    if (!EnsureEdramTransferInfrastructure())
        return nullptr;

    std::ostringstream h;
    h << "#define SRC_MSAA " << uint32_t(sourceSamples) << "\n";
    h << "#define SRC_DEPTH_FLOAT24 " << (sourceDepthFloat24 ? 1 : 0) << "\n";
    h << "#define RESOLVE_ENDIAN " << resolveEndian << "\n";
    h << R"(
struct PackPush { uint srcX; uint srcY; uint dstX; uint dstY; };
[[vk::push_constant]] ConstantBuffer<PackPush> g_Push;
#if SRC_MSAA == 1
Texture2D<float> g_SourceDepth : register(t0, space0);
Texture2D<uint> g_SourceStencil : register(t1, space0);
#else
Texture2DMS<float> g_SourceDepth : register(t0, space0);
Texture2DMS<uint> g_SourceStencil : register(t1, space0);
#endif

uint Float32To20e4(float value)
{
    if (!(value > 0.0f)) return 0u;
    uint bits = asuint(value);
    if (bits >= 0x3FFFFFF8u) return 0xFFFFFFu;
    if (bits < 0x38800000u)
    {
        uint shift = min(113u - (bits >> 23), 24u);
        bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
    }
    else
    {
        bits += 0xC8000000u;
    }
    bits += 3u + ((bits >> 3) & 1u);
    return (bits >> 3) & 0xFFFFFFu;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    uint2 dst = uint2(position.xy);
    uint2 src = dst - uint2(g_Push.dstX, g_Push.dstY) +
                uint2(g_Push.srcX, g_Push.srcY);
#if SRC_MSAA == 1
    float depth = g_SourceDepth.Load(int3(int2(src), 0));
    uint stencil = g_SourceStencil.Load(int3(int2(src), 0));
#else
    // Xenos depth resolve uses a selected sample. The renderer's native depth
    // resolve currently implements SAMPLE_ZERO, so preserve the same source
    // sample in the packed guest-memory representation.
    float depth = g_SourceDepth.Load(int2(src), 0);
    uint stencil = g_SourceStencil.Load(int2(src), 0);
#endif
#if SRC_DEPTH_FLOAT24
    uint d24 = Float32To20e4(depth * 2.0f);
#else
    uint d24 = (uint)round(saturate(depth) * 16777215.0f);
#endif
    uint4 b = uint4(stencil & 255u, d24 & 255u,
                    (d24 >> 8) & 255u, (d24 >> 16) & 255u);
#if RESOLVE_ENDIAN == 1
    b = b.yxwz;
#elif RESOLVE_ENDIAN == 2
    b = b.wzyx;
#elif RESOLVE_ENDIAN == 3
    b = b.zwxy;
#endif
    return float4(b) / 255.0f;
}
)";

    std::vector<uint8_t> psSpv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(h.str(), false, psSpv, err))
    {
        KLOG("Vulkan packed depth resolve PS compile failed samples=%u f24=%u endian=%u: %s\n",
             uint32_t(sourceSamples), sourceDepthFloat24 ? 1u : 0u,
             resolveEndian, err.c_str());
        return nullptr;
    }

    DepthSnapshotPackPipeline rec{};
    rec.sourceSamples = sourceSamples;
    rec.sourceDepthFloat24 = sourceDepthFloat24;
    rec.resolveEndian = resolveEndian;
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = psSpv.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(psSpv.data());
    const VkResult moduleResult =
        p_vkCreateShaderModule(g_device, &moduleInfo, nullptr, &rec.ps);
    if (moduleResult != VK_SUCCESS)
    {
        KLOG("Vulkan packed depth resolve shader module failed (%d): samples=%u f24=%u endian=%u\n",
             moduleResult, uint32_t(sourceSamples), sourceDepthFloat24 ? 1u : 0u,
             resolveEndian);
        return nullptr;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_edramTransferVs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = rec.ps;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    const VkFormat packedFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &packedFormat;
    VkGraphicsPipelineCreateInfo pipe{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipe.pNext = &rendering;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vertex;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vp;
    pipe.pRasterizationState = &raster;
    pipe.pMultisampleState = &ms;
    pipe.pColorBlendState = &blend;
    pipe.pDynamicState = &dynamic;
    pipe.layout = g_edramTransferPipelineLayout;
    const VkResult pipelineResult =
        p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1,
                                    &pipe, nullptr, &rec.pipeline);
    if (pipelineResult != VK_SUCCESS)
    {
        KLOG("Vulkan packed depth resolve pipeline failed (%d): samples=%u f24=%u endian=%u\n",
             pipelineResult, uint32_t(sourceSamples), sourceDepthFloat24 ? 1u : 0u,
             resolveEndian);
        p_vkDestroyShaderModule(g_device, rec.ps, nullptr);
        return nullptr;
    }

    g_depthSnapshotPackPipelines.push_back(rec);
    return &g_depthSnapshotPackPipelines.back();
}

VkDescriptorSet EnsureEdramTransferDescriptor(const EdramOwner& owner)
{
    if (!EnsureEdramTransferInfrastructure())
        return VK_NULL_HANDLE;

    VkDescriptorSet* descriptor = nullptr;
    VkImageView views[2]{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (owner.kind == EdramOwnerKind::Color)
    {
        ColorBacking* backing = FindColorBacking(owner.key);
        if (!backing)
            return VK_NULL_HANDLE;
        descriptor = &backing->transferDescriptor;
        views[0] = views[1] = backing->view;
    }
    else if (owner.kind == EdramOwnerKind::Depth)
    {
        DepthBacking* backing = FindDepthBacking(owner.key);
        if (!backing)
            return VK_NULL_HANDLE;
        descriptor = &backing->transferDescriptor;
        views[0] = backing->depthSampleView;
        views[1] = backing->stencilSampleView;
        layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    else
    {
        return VK_NULL_HANDLE;
    }

    if (*descriptor)
        return *descriptor;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_edramTransferPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_edramTransferSetLayout;
    const VkResult result = p_vkAllocateDescriptorSets(g_device, &ai, descriptor);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan EDRAM transfer descriptor allocation failed (%d): kind=%u key=%016llX\n",
             result, uint32_t(owner.kind), static_cast<unsigned long long>(owner.key));
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo images[2]{};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        images[i].imageView = views[i];
        images[i].imageLayout = layout;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = *descriptor;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    p_vkUpdateDescriptorSets(g_device, 2, writes, 0, nullptr);
    return *descriptor;
}

void TransitionPackedDepthSnapshot(ResolveSnapshot& snapshot, VkImageLayout next);

bool PackDepthResolveSnapshot(ResolveSnapshot& snapshot, DepthBacking& source,
                              const VkImageCopy& copy, uint32_t resolveEndian)
{
    if (!snapshot.packedDepthImage || !copy.extent.width || !copy.extent.height)
        return false;
    const bool sourceFloat24 = ((source.info >> 16) & 1u) != 0;
    DepthSnapshotPackPipeline* pipeline = GetDepthSnapshotPackPipeline(
        source.samples, sourceFloat24, resolveEndian);
    const VkDescriptorSet descriptor = EnsureEdramTransferDescriptor(
        {EdramOwnerKind::Depth, source.key});
    if (!pipeline || !descriptor)
    {
        KLOG("Vulkan packed depth resolve prerequisites missing: key=%08X pipeline=%u descriptor=%u samples=%u f24=%u endian=%u\n",
             source.key, pipeline ? 1u : 0u, descriptor ? 1u : 0u,
             uint32_t(source.samples), sourceFloat24 ? 1u : 0u, resolveEndian & 3u);
        return false;
    }

    EndColorRendering();
    TransitionDepthBacking(source, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    TransitionPackedDepthSnapshot(snapshot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = snapshot.packedDepthWriteView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = snapshot.packedDepthInitialized
        ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, g_internalExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    p_vkCmdBeginRendering(g_commandBuffer, &rendering);

    VkViewport viewport{};
    viewport.width = float(g_internalExtent.width);
    viewport.height = float(g_internalExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    CmdSetViewportCached(g_commandBuffer, viewport);
    CmdBindGraphicsPipelineCached(g_commandBuffer, pipeline->pipeline);
    CmdBindDescriptorSetsCached(g_commandBuffer, g_edramTransferPipelineLayout,
                                0, 1, &descriptor);
    struct PackPush
    {
        uint32_t srcX, srcY, dstX, dstY;
    } push{uint32_t(copy.srcOffset.x), uint32_t(copy.srcOffset.y),
           uint32_t(copy.dstOffset.x), uint32_t(copy.dstOffset.y)};
    p_vkCmdPushConstants(g_commandBuffer, g_edramTransferPipelineLayout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    VkRect2D rect{{copy.dstOffset.x, copy.dstOffset.y},
                  {copy.extent.width, copy.extent.height}};
    CmdSetScissorCached(g_commandBuffer, rect);
    p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
    p_vkCmdEndRendering(g_commandBuffer);

    snapshot.packedDepthInitialized = true;
    snapshot.packedDepthResolveEndian = resolveEndian & 3u;
    TransitionPackedDepthSnapshot(snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

std::vector<VkRect2D> EdramTransferRects(uint32_t localStart, uint32_t count,
                                         uint32_t pitchTiles,
                                         uint32_t tileWidth, uint32_t tileHeight)
{
    std::vector<VkRect2D> rects;
    if (!count || !pitchTiles)
        return rects;

    uint32_t row = localStart / pitchTiles;
    uint32_t col = localStart % pitchTiles;
    if (col)
    {
        const uint32_t first = std::min(count, pitchTiles - col);
        rects.push_back({{int32_t(col * tileWidth), int32_t(row * tileHeight)},
                         {first * tileWidth, tileHeight}});
        count -= first;
        ++row;
        col = 0;
    }
    const uint32_t fullRows = count / pitchTiles;
    if (fullRows)
    {
        rects.push_back({{0, int32_t(row * tileHeight)},
                         {pitchTiles * tileWidth, fullRows * tileHeight}});
        count -= fullRows * pitchTiles;
        row += fullRows;
    }
    if (count)
    {
        rects.push_back({{0, int32_t(row * tileHeight)},
                         {count * tileWidth, tileHeight}});
    }

    for (auto& rect : rects)
    {
        if (rect.offset.x >= int32_t(g_extent.width) ||
            rect.offset.y >= int32_t(g_extent.height))
        {
            rect.extent = {};
            continue;
        }
        rect.extent.width = std::min(rect.extent.width,
            g_extent.width - uint32_t(rect.offset.x));
        rect.extent.height = std::min(rect.extent.height,
            g_extent.height - uint32_t(rect.offset.y));
    }
    return rects;
}

bool TransferEdramOwnershipSpan(const EdramOwner& sourceOwner,
                                const EdramOwner& destOwner,
                                uint32_t destLocalStart, uint32_t tileCount)
{
    EdramBackingMeta source{}, dest{};
    if (!GetEdramBackingMeta(sourceOwner, source) ||
        !GetEdramBackingMeta(destOwner, dest))
        return false;
    if (!EdramTransferFormatSupported(source) || !EdramTransferFormatSupported(dest))
    {
        ++g_edramOwnershipUnsupported;
        static uint32_t reports = 0;
        if (reports++ < 24)
            KLOG("[edram ownership] unsupported transfer src=%u info=%08X dst=%u info=%08X tiles=%u\n",
                 uint32_t(source.kind), source.info, uint32_t(dest.kind), dest.info, tileCount);
        // Never advance the physical ownership table without materializing the
        // previous owner's bits into the destination representation.  Claiming
        // success here would make a later resolve/read observe stale host image
        // contents even though the table says the destination owns the tiles.
        return false;
    }

    const bool sourceDepthFloat24 =
        source.kind == EdramOwnerKind::Depth && ((source.info >> 16) & 1u) != 0;
    const bool destDepthFloat24 =
        dest.kind == EdramOwnerKind::Depth && ((dest.info >> 16) & 1u) != 0;
    const auto depthTransferMode = mojorecomp::gpu::SelectEdramDepthTransferMode(
        g_shaderStencilExport);
    const EdramTransferPass mainPass = dest.kind == EdramOwnerKind::Color
        ? EdramTransferPass::Color
        : (depthTransferMode == mojorecomp::gpu::EdramDepthTransferMode::ShaderStencilExport
            ? EdramTransferPass::DepthStencilExport
            : EdramTransferPass::DepthOnly);
    EdramTransferPipeline* pipeline = GetEdramTransferPipeline(
        source.kind, dest.kind, source.samples, dest.samples,
        source.guestSamples, dest.guestSamples,
        source.pitchTiles, dest.pitchTiles,
        sourceDepthFloat24, destDepthFloat24,
        dest.kind == EdramOwnerKind::Depth ? dest.format : VK_FORMAT_UNDEFINED,
        mainPass);
    const VkPipeline mainPipeline = pipeline ? pipeline->pipeline : VK_NULL_HANDLE;
    EdramTransferPipeline* stencilPipeline = nullptr;
    if (dest.kind == EdramOwnerKind::Depth &&
        depthTransferMode == mojorecomp::gpu::EdramDepthTransferMode::FixedFunctionBitPlanes)
    {
        static bool fallbackReported = false;
        if (!fallbackReported)
        {
            fallbackReported = true;
            KLOG("Vulkan EDRAM depth transfer fallback active: fixed-function stencil bit planes\n");
        }
        stencilPipeline = GetEdramTransferPipeline(
            source.kind, dest.kind, source.samples, dest.samples,
            source.guestSamples, dest.guestSamples,
            source.pitchTiles, dest.pitchTiles,
            sourceDepthFloat24, destDepthFloat24, dest.format,
            EdramTransferPass::StencilBitPlanes);
    }
    const VkPipeline stencilBitPlanePipeline = stencilPipeline
        ? stencilPipeline->pipeline : VK_NULL_HANDLE;
    const VkDescriptorSet descriptor = EnsureEdramTransferDescriptor(sourceOwner);
    if (!mainPipeline || !descriptor ||
        (depthTransferMode == mojorecomp::gpu::EdramDepthTransferMode::FixedFunctionBitPlanes &&
         dest.kind == EdramOwnerKind::Depth && !stencilBitPlanePipeline))
        return false;

    EndColorRendering();
    if (source.kind == EdramOwnerKind::Color)
    {
        ColorBacking* backing = FindColorBacking(source.key);
        TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    else
    {
        DepthBacking* backing = FindDepthBacking(source.key);
        TransitionDepthBacking(*backing, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }

    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencilAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, g_internalExtent};
    rendering.layerCount = 1;
    if (dest.kind == EdramOwnerKind::Color)
    {
        ColorBacking* backing = FindColorBacking(dest.key);
        TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorAttachment.imageView = backing->view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;
    }
    else
    {
        DepthBacking* backing = FindDepthBacking(dest.key);
        TransitionDepthBacking(*backing, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depthAttachment.imageView = backing->view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        stencilAttachment = depthAttachment;
        rendering.pDepthAttachment = &depthAttachment;
        rendering.pStencilAttachment = &stencilAttachment;
    }

    p_vkCmdBeginRendering(g_commandBuffer, &rendering);
    VkViewport viewport{};
    viewport.width = float(g_internalExtent.width);
    viewport.height = float(g_internalExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    CmdSetViewportCached(g_commandBuffer, viewport);
    CmdBindGraphicsPipelineCached(g_commandBuffer, mainPipeline);
    CmdBindDescriptorSetsCached(g_commandBuffer, g_edramTransferPipelineLayout,
                                0, 1, &descriptor);
    struct TransferPush
    {
        uint32_t sourceBaseTiles;
        uint32_t sourcePitchTiles;
        uint32_t destBaseTiles;
        uint32_t destPitchTiles;
        uint32_t stencilBit;
    } push{source.baseTiles, source.pitchTiles, dest.baseTiles, dest.pitchTiles, 0};
    p_vkCmdPushConstants(g_commandBuffer, g_edramTransferPipelineLayout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    const uint32_t tileWidth = EdramTileWidthPixels(dest.guestSamples);
    const uint32_t tileHeight = EdramTileHeightPixels(dest.guestSamples);
    const auto rects = EdramTransferRects(destLocalStart, tileCount,
                                          dest.pitchTiles, tileWidth, tileHeight);
    for (const VkRect2D& rect : rects)
    {
        if (!rect.extent.width || !rect.extent.height)
            continue;
        ++g_perfEdramOwnershipRects;
        g_perfEdramOwnershipPixels +=
            uint64_t(rect.extent.width) * uint64_t(rect.extent.height);
        CmdSetScissorCached(g_commandBuffer, ScaleRectToInternal(rect));
        p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
    }

    if (stencilBitPlanePipeline)
    {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        clear.clearValue.depthStencil.stencil = 0;
        for (const VkRect2D& rect : rects)
        {
            if (!rect.extent.width || !rect.extent.height)
                continue;
            VkClearRect clearRect{};
            clearRect.rect = ScaleRectToInternal(rect);
            clearRect.layerCount = 1;
            p_vkCmdClearAttachments(g_commandBuffer, 1, &clear, 1, &clearRect);
        }

        CmdBindGraphicsPipelineCached(g_commandBuffer, stencilBitPlanePipeline);
        p_vkCmdSetStencilReference(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFu);
        p_vkCmdSetStencilCompareMask(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFu);
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
            push.stencilBit = 1u << bit;
            p_vkCmdSetStencilWriteMask(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK,
                                       push.stencilBit);
            p_vkCmdPushConstants(g_commandBuffer, g_edramTransferPipelineLayout,
                                 VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            for (const VkRect2D& rect : rects)
            {
                if (!rect.extent.width || !rect.extent.height)
                    continue;
                CmdSetScissorCached(g_commandBuffer, ScaleRectToInternal(rect));
                p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
            }
        }
    }
    p_vkCmdEndRendering(g_commandBuffer);

    ++g_edramOwnershipTransfers;
    g_edramOwnershipTiles += tileCount;
    ++g_perfEdramOwnershipTransfers;
    g_perfEdramOwnershipTiles += tileCount;
    RecordEdramTransferPair(sourceOwner, destOwner, tileCount);
    static uint32_t reports = 0;
    if (reports++ < 48)
        KLOG("[edram ownership] transfer src=%s key=%016llX host=%ux guest=%ux -> "
             "dst=%s key=%016llX host=%ux guest=%ux local=%u tiles=%u rects=%zu\n",
             source.kind == EdramOwnerKind::Color ? "color" : "depth",
             static_cast<unsigned long long>(source.key), uint32_t(source.samples),
             uint32_t(source.guestSamples),
             dest.kind == EdramOwnerKind::Color ? "color" : "depth",
             static_cast<unsigned long long>(dest.key), uint32_t(dest.samples),
             uint32_t(dest.guestSamples),
             destLocalStart, tileCount, rects.size());
    return true;
}

bool ClaimEdramOwnership(const EdramOwner& destOwner)
{
    EdramBackingMeta dest{};
    if (!GetEdramBackingMeta(destOwner, dest) || !dest.pitchTiles)
        return true;
    const uint32_t totalTiles = EdramBackingTileCount(dest);
    if (!totalTiles)
        return true;
    if (EdramOwnedTileCount(destOwner) == totalTiles)
    {
        ++g_edramOwnerFastPathHits;
        return true;
    }

    uint32_t local = 0;
    while (local < totalTiles)
    {
        const uint32_t physical = (dest.baseTiles + local) &
                                  (xenos::kEdramTileCount - 1u);
        const EdramOwner previous = g_edramOwners[physical];
        if (EdramOwnerEqual(previous, destOwner))
        {
            ++local;
            continue;
        }

        uint32_t span = 1;
        while (local + span < totalTiles)
        {
            const uint32_t nextPhysical = (dest.baseTiles + local + span) &
                                          (xenos::kEdramTileCount - 1u);
            if (!EdramOwnerEqual(g_edramOwners[nextPhysical], previous))
                break;
            ++span;
        }

        if (previous.kind != EdramOwnerKind::None &&
            !TransferEdramOwnershipSpan(previous, destOwner, local, span))
            return false;

        AdjustEdramOwnedTileCount(previous, -static_cast<int32_t>(span));
        AdjustEdramOwnedTileCount(destOwner, static_cast<int32_t>(span));
        for (uint32_t i = 0; i < span; ++i)
        {
            const uint32_t claimed = (dest.baseTiles + local + i) &
                                     (xenos::kEdramTileCount - 1u);
            g_edramOwners[claimed] = destOwner;
        }
        local += span;
    }
    return true;
}

bool DiscardAndClaimEdramOwnership(const EdramOwner& destOwner)
{
    // Use only when the destination backing is about to be overwritten over its
    // complete host extent (for example a collapsed full-surface resolve clear).
    // In that case materializing the previous physical EDRAM owner into this
    // backing is wasted work: the clear immediately destroys those bits anyway.
    EdramBackingMeta dest{};
    if (!GetEdramBackingMeta(destOwner, dest) || !dest.pitchTiles)
        return true;
    const uint32_t totalTiles = EdramBackingTileCount(dest);
    if (!totalTiles)
        return true;
    if (EdramOwnedTileCount(destOwner) == totalTiles)
    {
        ++g_edramOwnerFastPathHits;
        return true;
    }

    uint32_t local = 0;
    while (local < totalTiles)
    {
        const uint32_t physical = (dest.baseTiles + local) &
                                  (xenos::kEdramTileCount - 1u);
        const EdramOwner previous = g_edramOwners[physical];
        if (EdramOwnerEqual(previous, destOwner))
        {
            ++local;
            continue;
        }

        uint32_t span = 1;
        while (local + span < totalTiles)
        {
            const uint32_t nextPhysical = (dest.baseTiles + local + span) &
                                          (xenos::kEdramTileCount - 1u);
            if (!EdramOwnerEqual(g_edramOwners[nextPhysical], previous))
                break;
            ++span;
        }

        AdjustEdramOwnedTileCount(previous, -static_cast<int32_t>(span));
        AdjustEdramOwnedTileCount(destOwner, static_cast<int32_t>(span));
        for (uint32_t i = 0; i < span; ++i)
        {
            const uint32_t claimed = (dest.baseTiles + local + i) &
                                     (xenos::kEdramTileCount - 1u);
            g_edramOwners[claimed] = destOwner;
        }
        local += span;
    }
    return true;
}

bool PrepareActiveEdramOwnersForFullOverwrite(bool writeColor0, bool writeColor1,
                                              bool writeDepth)
{
    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    if (!EdramOwnershipEnabled(frame))
        return true;

    // Match normal ownership precedence: depth first, then color attachments.
    if (writeDepth && g_activeDepthEnabled && g_activeDepthSurfaceKey != UINT64_MAX &&
        !DiscardAndClaimEdramOwnership({EdramOwnerKind::Depth, g_activeDepthSurfaceKey}))
        return false;
    if (writeColor0 && g_activeColorSurfaceKey != UINT64_MAX &&
        !DiscardAndClaimEdramOwnership({EdramOwnerKind::Color, g_activeColorSurfaceKey}))
        return false;
    if (writeColor1 && g_activeColor1Enabled && g_activeColor1SurfaceKey != UINT64_MAX &&
        !DiscardAndClaimEdramOwnership({EdramOwnerKind::Color, g_activeColor1SurfaceKey}))
        return false;
    return true;
}

bool MaterializeEdramOwnerForResolve(const EdramOwner& owner,
                                     uint64_t frame, const char* sourceName)
{
    if (!EdramOwnershipEnabled(frame))
        return true;
    const uint64_t transfersBefore = g_edramOwnershipTransfers;
    if (!ClaimEdramOwnership(owner))
        return false;
    if (g_edramOwnershipTransfers != transfersBefore)
    {
        static uint32_t reports = 0;
        if (reports++ < 48)
            KLOG("[edram resolve owner] frame=%llu source=%s transfers=%llu\n",
                 static_cast<unsigned long long>(frame), sourceName,
                 static_cast<unsigned long long>(g_edramOwnershipTransfers - transfersBefore));
    }
    return true;
}

void RestoreActiveEdramAttachmentLayouts()
{
    if (ColorBacking* color = ActiveColorBacking())
        TransitionColorBacking(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (ColorBacking* color1 = ActiveColor1Backing())
        TransitionColorBacking(*color1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (DepthBacking* depth = ActiveDepthBacking())
        TransitionDepthBacking(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

bool PrepareActiveEdramOwnersForWrite(bool writeColor0, bool writeColor1,
                                      bool writeDepth, const char* reason)
{
    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    if (!EdramOwnershipEnabled(frame))
        return true;

    const bool haveDepth = writeDepth && g_activeDepthEnabled &&
                           g_activeDepthSurfaceKey != UINT64_MAX;
    const bool haveColor0 = writeColor0 && g_activeColorSurfaceKey != UINT64_MAX;
    const bool haveColor1 = writeColor1 && g_activeColor1Enabled &&
                            g_activeColor1SurfaceKey != UINT64_MAX;
    if (!haveDepth && !haveColor0 && !haveColor1)
        return true;

    // Ownership transfer may itself render into the destination backing, so it
    // must happen outside the guest rendering scope. Preserve the same physical
    // precedence used by normal draws: depth first, then color attachments.
    EndColorRendering();
    const uint64_t transfersBefore = g_edramOwnershipTransfers;
    if (haveDepth &&
        !ClaimEdramOwnership({EdramOwnerKind::Depth, g_activeDepthSurfaceKey}))
        return false;
    if (haveColor0 &&
        !ClaimEdramOwnership({EdramOwnerKind::Color, g_activeColorSurfaceKey}))
        return false;
    if (haveColor1 &&
        !ClaimEdramOwnership({EdramOwnerKind::Color, g_activeColor1SurfaceKey}))
        return false;

    RestoreActiveEdramAttachmentLayouts();
    if (g_edramOwnershipTransfers != transfersBefore)
    {
        static uint32_t reports = 0;
        if (reports++ < 48)
            KLOG("[edram write owner] frame=%llu reason=%s transfers=%llu C0=%u C1=%u D=%u\n",
                 static_cast<unsigned long long>(frame), reason ? reason : "write",
                 static_cast<unsigned long long>(g_edramOwnershipTransfers - transfersBefore),
                 haveColor0 ? 1u : 0u, haveColor1 ? 1u : 0u, haveDepth ? 1u : 0u);
    }
    return true;
}

bool SwitchActiveColorSurface(uint32_t surfaceInfo, uint32_t info, bool resume = true)
{
    const uint64_t key = ColorSurfaceKey(surfaceInfo, info);
    if (g_activeColorSurfaceKey == key && !g_activeColor1Enabled)
        return true;

    ++g_perfColorSwitches;
    EndColorRendering();
    ColorBacking* newBacking = nullptr;
    if (!CreateColorBacking(surfaceInfo, info, newBacking) || !newBacking)
        return false;

    const bool firstUse = !newBacking->initialized;
    TransitionColorBacking(*newBacking, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (newBacking->samples != VK_SAMPLE_COUNT_1_BIT)
        TransitionColorResolveBacking(*newBacking, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    g_activeColorSurfaceKey = key;
    g_activeColor1SurfaceKey = UINT64_MAX;
    g_activeColorInfo = info;
    g_activeColor1Info = UINT32_MAX;
    g_activeColor1Enabled = false;
    if (resume)
        ResumeColorRendering(firstUse);
    newBacking->initialized = true;
    return true;
}

bool SwitchActiveColorSurfaces(uint32_t surfaceInfo, uint32_t color0Info,
                               uint32_t color1Info, bool enableColor1, bool resume = true)
{
    if (enableColor1 && color1Info == color0Info)
    {
        static uint32_t aliasReports = 0;
        if (aliasReports++ < 8)
            KLOG("Vulkan MRT alias suppressed: color0=color1=%08X\n", color0Info);
        enableColor1 = false;
    }

    const uint64_t color0Key = ColorSurfaceKey(surfaceInfo, color0Info);
    const uint64_t color1Key = enableColor1 ? ColorSurfaceKey(surfaceInfo, color1Info)
                                            : UINT64_MAX;
    if (g_activeColorSurfaceKey == color0Key &&
        g_activeColor1Enabled == enableColor1 &&
        (!enableColor1 || g_activeColor1SurfaceKey == color1Key))
        return true;

    ++g_perfColorSwitches;
    EndColorRendering();

    ColorBacking* color0 = nullptr;
    if (!CreateColorBacking(surfaceInfo, color0Info, color0) || !color0)
        return false;
    const bool firstUse0 = !color0->initialized;
    TransitionColorBacking(*color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (color0->samples != VK_SAMPLE_COUNT_1_BIT)
        TransitionColorResolveBacking(*color0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    ColorBacking* color1 = nullptr;
    bool firstUse1 = false;
    if (enableColor1)
    {
        if (!CreateColorBacking(surfaceInfo, color1Info, color1) || !color1)
            return false;
        firstUse1 = !color1->initialized;
        TransitionColorBacking(*color1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (color1->samples != VK_SAMPLE_COUNT_1_BIT)
            TransitionColorResolveBacking(*color1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    g_activeColorSurfaceKey = color0Key;
    g_activeColor1SurfaceKey = color1Key;
    g_activeColorInfo = color0Info;
    g_activeColor1Info = enableColor1 ? color1Info : UINT32_MAX;
    g_activeColor1Enabled = enableColor1;

    if (resume)
        ResumeColorRendering(firstUse0);
    color0->initialized = true;
    if (color1)
    {
        if (firstUse1)
        {
            EndColorRendering();
            VkClearColorValue clear{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            TransitionColorBacking(*color1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            p_vkCmdClearColorImage(g_commandBuffer, color1->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &clear, 1, &range);
            TransitionColorBacking(*color1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (resume)
                ResumeColorRendering(false);
        }
        color1->initialized = true;
    }
    return true;
}

bool SwitchActiveDepthSurface(uint32_t surfaceInfo, uint32_t depthInfo, bool depthEnabled, bool resume = true)
{
    if (!depthEnabled)
    {
        if (g_activeDepthEnabled)
        {
            EndColorRendering();
            g_activeDepthEnabled = false;
            if (resume)
                ResumeColorRendering(false);
        }
        return true;
    }

    const uint64_t key = (uint64_t(surfaceInfo) << 32) | depthInfo;
    if (g_activeDepthEnabled && key == g_activeDepthSurfaceKey)
        return true;

    EndColorRendering();
    if (DiagnosticInvalidateColorOnDepthAliasEnabled())
    {
        const uint32_t depthBase = xenos::DepthBaseTiles(depthInfo);
        for (auto& color : g_colorBackings)
        {
            if (!color.initialized || color.baseTiles != depthBase)
                continue;

            VkClearColorValue clear{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            TransitionColorBacking(color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            p_vkCmdClearColorImage(g_commandBuffer, color.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &clear, 1, &range);
            if (color.key == g_activeColorSurfaceKey ||
                (g_activeColor1Enabled && color.key == g_activeColor1SurfaceKey))
                TransitionColorBacking(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            static uint32_t reports = 0;
            if (reports++ < 32)
                KLOG("[edram alias A/B] depth base=%03X invalidated color key=%016llX\n",
                     depthBase, static_cast<unsigned long long>(color.key));
        }
    }

    DepthBacking* backing = nullptr;
    if (!CreateDepthBacking(key, backing) || !backing)
        return false;

    const bool firstUse = !backing->initialized;
    if (firstUse)
    {
        TransitionDepthBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue clear{1.0f, 0};
        VkImageSubresourceRange range{
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        p_vkCmdClearDepthStencilImage(g_commandBuffer, backing->image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      &clear, 1, &range);
    }
    TransitionDepthBacking(*backing, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    g_activeDepthSurfaceKey = key;
    g_activeDepthEnabled = true;
    backing->initialized = true;
    if (resume)
        ResumeColorRendering(false);
    return true;
}
void TransitionSnapshot(ResolveSnapshot& snapshot, VkImageLayout next)
{
    if (snapshot.layout == next)
        return;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (snapshot.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (snapshot.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }
    else if (snapshot.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags dstAccess = next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                  ? VK_ACCESS_TRANSFER_WRITE_BIT
                                  : VK_ACCESS_TRANSFER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = snapshot.layout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = snapshot.image;
    barrier.subresourceRange.aspectMask = snapshot.aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
    snapshot.layout = next;
}

void TransitionPackedDepthSnapshot(ResolveSnapshot& snapshot, VkImageLayout next)
{
    if (!snapshot.packedDepthImage || snapshot.packedDepthLayout == next)
        return;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (snapshot.packedDepthLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        srcAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (snapshot.packedDepthLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_READ_BIT;
    }
    else if (snapshot.packedDepthLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkAccessFlags dstAccess = VK_ACCESS_SHADER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    else if (next == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = snapshot.packedDepthLayout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = snapshot.packedDepthImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
    snapshot.packedDepthLayout = next;
}

void TransitionSnapshotPresent(ResolveSnapshot& snapshot, VkImageLayout next)
{
    if (!snapshot.presentImage || snapshot.presentLayout == next)
        return;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    if (snapshot.presentLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (snapshot.presentLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }
    else if (snapshot.presentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        srcStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags dstAccess = next == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                  ? VK_ACCESS_TRANSFER_WRITE_BIT
                                  : VK_ACCESS_TRANSFER_READ_BIT;
    if (next == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = snapshot.presentLayout;
    barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = snapshot.presentImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    snapshot.presentLayout = next;
}

bool CommitSnapshotForPresent(ResolveSnapshot& snapshot)
{
    if (!snapshot.coverageComplete || snapshot.depth)
        return false;
    if (snapshot.presentValid && snapshot.presentedGeneration == snapshot.coverageGeneration)
        return true;
    if (!EnsureSnapshotPresentImage(snapshot))
        return false;

    TransitionSnapshot(snapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionSnapshotPresent(snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {snapshot.width, snapshot.height, 1};
    copy = ScaleImageCopyToInternal(copy);
    p_vkCmdCopyImage(g_commandBuffer, snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     snapshot.presentImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionSnapshotPresent(snapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    snapshot.presentValid = true;
    snapshot.presentResolveSwap = snapshot.resolveSwap;
    snapshot.presentedGeneration = snapshot.coverageGeneration;

    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 12)
        KLOG("Vulkan stable front commit: key=%08X generation=%llu copies=%llu\n",
             snapshot.key,
             static_cast<unsigned long long>(snapshot.coverageGeneration),
             static_cast<unsigned long long>(snapshot.copies));
    return true;
}

bool CommitSnapshotForSceneFeedback(ResolveSnapshot& snapshot)
{
    if (snapshot.depth || !EnsureSnapshotPresentImage(snapshot))
        return false;

    const int stableMode = StableSceneFeedbackMode();
    if (stableMode >= 2)
    {
        // Strict diagnostic mode: publish only a fully assembled scene-color
        // generation, and never republish later tile-2 resolves into the same
        // generation. This tests whether consumers are observing intermediate
        // history while the Xenos tiled sweep is still being assembled.
        if (!snapshot.coverageComplete)
        {
            static uint32_t incompleteReports = 0;
            if (incompleteReports++ < 32)
                KLOG("[scene feedback strict] key=%08X generation=%llu copies=%llu incomplete; publish deferred\n",
                     snapshot.key,
                     static_cast<unsigned long long>(snapshot.coverageGeneration),
                     static_cast<unsigned long long>(snapshot.copies));
            return false;
        }
        if (snapshot.presentValid &&
            snapshot.presentedGeneration == snapshot.coverageGeneration)
        {
            static uint32_t duplicateReports = 0;
            if (duplicateReports++ < 32)
                KLOG("[scene feedback strict] key=%08X generation=%llu copies=%llu already published\n",
                     snapshot.key,
                     static_cast<unsigned long long>(snapshot.coverageGeneration),
                     static_cast<unsigned long long>(snapshot.copies));
            return true;
        }
    }

    // Scene-color feedback is assembled through three overlapping horizontal
    // EDRAM tiles. Publish only after the last tile of the sweep has arrived so
    // shaders never observe a new left tile together with stale middle/right
    // tiles. Unlike front presentation, publish every qualifying tile-2 resolve:
    // the title performs several feedback resolves per tile and later tile-2
    // resolves may contain additional transparent/effect passes.
    TransitionSnapshot(snapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionSnapshotPresent(snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {snapshot.width, snapshot.height, 1};
    copy = ScaleImageCopyToInternal(copy);
    p_vkCmdCopyImage(g_commandBuffer, snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     snapshot.presentImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionSnapshotPresent(snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    snapshot.presentValid = true;
    snapshot.presentResolveSwap = snapshot.resolveSwap;
    snapshot.presentedGeneration = snapshot.coverageGeneration;

    static uint32_t reports = 0;
    if (reports++ < 24)
        KLOG("[scene feedback stable] key=%08X generation=%llu copies=%llu published\n",
             snapshot.key,
             static_cast<unsigned long long>(snapshot.coverageGeneration),
             static_cast<unsigned long long>(snapshot.copies));
    return true;
}

uint32_t MacroTileOffset(uint32_t x, uint32_t y, uint32_t pitch)
{
    const uint32_t tilesPerRow = std::max(pitch, 32u) >> 5;
    return ((x >> 5) + (y >> 5) * tilesPerRow) * 4096u;
}

void DecodeVertexWindowOffset(const uint32_t* regs, int32_t& x, int32_t& y,
                              bool ignoreWindowOffset = false);
VkRect2D DecodeScissor(const uint32_t* regs, bool ignoreWindowOffset);
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian);
uint32_t ReadIndexValue(const uint8_t* src, uint32_t index, bool index32, uint32_t endian);

// Xenos/D3D9 resolves are rectangle-list draws. The resolve rectangle is
// defined by three 32_32_FLOAT vertices in vertex fetch 0; the window scissor
// only clips that rectangle. Older code treated the whole window
// scissor as the resolve rectangle, which turns small intermediate resolves
// into full EDRAM-tile copies and can feed unrelated pixels back into later
// post-processing passes.
//
// Keep this helper in logical guest coordinates. ResolveCopyRegionForHost then
// performs the existing logical->physical EDRAM mapping (or leaves coordinates
// logical in stitch mode), so this change is independent from the tiled-raster
// strategy.
enum class ResolveRectDecode : uint8_t
{
    Invalid,
    Empty,
    Valid,
};

struct ResolveVertexBounds
{
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    bool valid = false;
};

ResolveRectDecode DecodeResolveRectFromVertices(uint8_t* guestBase, const uint32_t* regs,
                                                uint32_t surfW, uint32_t surfH,
                                                uint32_t& x0, uint32_t& y0,
                                                uint32_t& x1, uint32_t& y1,
                                                ResolveVertexBounds* rawBounds = nullptr)
{
    if (!guestBase || !regs || !surfW || !surfH)
        return ResolveRectDecode::Invalid;

    const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(regs, 0);
    const uint32_t va = PhysicalToCached(fetch.address);
    if (!fetch.address || fetch.sizeDwords < 6 ||
        !GuestRangeOk(va, 6u * sizeof(uint32_t)))
        return ResolveRectDecode::Invalid;

    std::array<uint32_t, 6> words{};
    CopySwapped(reinterpret_cast<uint8_t*>(words.data()), guestBase + va,
                words.size() * sizeof(uint32_t), fetch.endian);

    if (MojoRecompVerboseDiagnosticsEnabled() && surfW == 1280 && surfH == 720)
    {
        static uint32_t resolveVertexReports = 0;
        const uint32_t srcSelect = regs[xenos::kRbCopyControl] & 7u;
        static std::array<uint32_t, 8> resolveVertexReportsBySource{};
        const bool reportGeneral = resolveVertexReports++ < 24;
        const bool reportSource = srcSelect < resolveVertexReportsBySource.size() &&
                                  resolveVertexReportsBySource[srcSelect]++ < 12;
        if (reportGeneral || reportSource)
        {
            KLOG("[resolve vertices] #%u src=%u v=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) "
                 "mode=%08X win=%08X sc=%08X..%08X control=%08X dest=%08X\n",
                 resolveVertexReports, srcSelect,
                 F32(words[0]), F32(words[1]), F32(words[2]), F32(words[3]),
                 F32(words[4]), F32(words[5]), regs[xenos::kPaSuScModeCntl],
                 regs[xenos::kPaScWindowOffset], regs[xenos::kPaScWindowScissorTl],
                 regs[xenos::kPaScWindowScissorBr], regs[xenos::kRbCopyControl],
                 regs[xenos::kRbCopyDestBase]);
        }
    }

    // Xenia applies the half-pixel shift only for D3DZero pixel centers. Resolve
    // vertices in this title are integral/half-integral, so nearbyint reproduces
    // the required fixed-point conversion deterministically.
    const float halfPixel = (regs[xenos::kPaSuVtxCntl] & 1u) == 0 ? 0.5f : 0.0f;
    int32_t minFixedX = INT32_MAX;
    int32_t minFixedY = INT32_MAX;
    int32_t maxFixedX = INT32_MIN;
    int32_t maxFixedY = INT32_MIN;
    for (uint32_t i = 0; i < 3; ++i)
    {
        const float fx = F32(words[i * 2]) + halfPixel;
        const float fy = F32(words[i * 2 + 1]) + halfPixel;
        if (!std::isfinite(fx) || !std::isfinite(fy) ||
            std::fabs(fx) > 32768.0f || std::fabs(fy) > 32768.0f)
            return ResolveRectDecode::Invalid;
        const int32_t fixedX = static_cast<int32_t>(std::nearbyint(fx * 256.0f));
        const int32_t fixedY = static_cast<int32_t>(std::nearbyint(fy * 256.0f));
        minFixedX = std::min(minFixedX, fixedX);
        minFixedY = std::min(minFixedY, fixedY);
        maxFixedX = std::max(maxFixedX, fixedX);
        maxFixedY = std::max(maxFixedY, fixedY);
    }

    // Match the top-left rasterization rounding used for resolves.
    int32_t rx0 = (minFixedX + 127) >> 8;
    int32_t ry0 = (minFixedY + 127) >> 8;
    int32_t rx1 = (maxFixedX + 127) >> 8;
    int32_t ry1 = (maxFixedY + 127) >> 8;
    if (rawBounds)
    {
        rawBounds->x0 = rx0;
        rawBounds->y0 = ry0;
        rawBounds->x1 = rx1;
        rawBounds->y1 = ry1;
        rawBounds->valid = rx1 > rx0 && ry1 > ry0;
    }
    if (rx1 <= rx0 || ry1 <= ry0)
        return ResolveRectDecode::Empty;

    // Xenia resolves in physical EDRAM raster coordinates: first apply the
    // vertex window offset, then clip against the window+screen scissor. Keep
    // the destination logical by undoing only the vertex offset after physical
    // clipping. This is essential for Crash's tiled 1280x720 pass: the logical
    // 416..864 and 832..1280 rectangles both rasterize from physical 0..448.
    int32_t vertexWindowX = 0, vertexWindowY = 0;
    DecodeVertexWindowOffset(regs, vertexWindowX, vertexWindowY, false);
    rx0 += vertexWindowX;
    ry0 += vertexWindowY;
    rx1 += vertexWindowX;
    ry1 += vertexWindowY;

    const VkRect2D physicalScissor = DecodeScissor(regs, false);
    if (!physicalScissor.extent.width || !physicalScissor.extent.height)
        return ResolveRectDecode::Empty;
    const int32_t sx0 = physicalScissor.offset.x;
    const int32_t sy0 = physicalScissor.offset.y;
    const int32_t sx1 = sx0 + int32_t(physicalScissor.extent.width);
    const int32_t sy1 = sy0 + int32_t(physicalScissor.extent.height);
    rx0 = std::max(rx0, sx0);
    ry0 = std::max(ry0, sy0);
    rx1 = std::min(rx1, sx1);
    ry1 = std::min(ry1, sy1);
    if (rx1 <= rx0 || ry1 <= ry0)
        return ResolveRectDecode::Empty;

    // Xenia expands the physically clipped resolve to the hardware's 8-pixel
    // granularity. Convert the result back to logical destination coordinates
    // afterwards; ResolveCopyRegionForHost will apply the offset once more only
    // to select the physical source image region.
    rx0 &= ~7;
    ry0 &= ~7;
    rx1 = (rx1 + 7) & ~7;
    ry1 = (ry1 + 7) & ~7;

    int32_t logicalX0 = rx0 - vertexWindowX;
    int32_t logicalY0 = ry0 - vertexWindowY;
    int32_t logicalX1 = rx1 - vertexWindowX;
    int32_t logicalY1 = ry1 - vertexWindowY;
    logicalX0 = std::clamp(logicalX0, 0, int32_t(surfW));
    logicalY0 = std::clamp(logicalY0, 0, int32_t(surfH));
    logicalX1 = std::clamp(logicalX1, 0, int32_t(surfW));
    logicalY1 = std::clamp(logicalY1, 0, int32_t(surfH));
    if (logicalX1 <= logicalX0 || logicalY1 <= logicalY0)
        return ResolveRectDecode::Empty;

    x0 = static_cast<uint32_t>(logicalX0);
    y0 = static_cast<uint32_t>(logicalY0);
    x1 = static_cast<uint32_t>(logicalX1);
    y1 = static_cast<uint32_t>(logicalY1);
    return ResolveRectDecode::Valid;
}

// Some predicated tiled passes keep the resolve rectangle in the local EDRAM
// raster window while advancing RB_COPY_DEST_BASE by whole 32x32 macrotiles.
// The generic Xenos rectangle decoder above must remain unchanged: it follows
// the hardware vertex-window-offset + scissor rules used by Xenia. However,
// MojoRecomp's current host EDRAM model materializes each replay into the same
// local backing before the resolve. If a same-frame depth snapshot already
// proves the destination base of the logical surface, recover the logical
// destination from the physical scissor and use the local backing as source.
//
// This is deliberately state-driven, not title/shader driven. A recovery is
// accepted only when all of the following agree:
//   * the raw resolve rectangle already covers the physical scissor;
//   * a non-zero vertex window offset maps that physical scissor elsewhere in
//     the logical destination;
//   * RB_COPY_DEST_BASE minus that logical macro-tile offset names an existing
//     depth snapshot written earlier in the same frame.
bool RecoverPhasedDepthResolve(const uint32_t* regs,
                               uint32_t surfW, uint32_t surfH,
                               const ResolveVertexBounds& rawBounds,
                               VkFormat depthFormat, bool depthFloat24,
                               uint64_t resolveFrame, uint32_t dest,
                               uint32_t& x0, uint32_t& y0,
                               uint32_t& x1, uint32_t& y1,
                               uint32_t& key)
{
    if (!rawBounds.valid || !surfW || !surfH)
        return false;

    int32_t windowX = 0, windowY = 0;
    DecodeVertexWindowOffset(regs, windowX, windowY, false);
    if (!windowX && !windowY)
        return false;

    const VkRect2D physicalScissor = DecodeScissor(regs, false);
    if (!physicalScissor.extent.width || !physicalScissor.extent.height)
        return false;
    const int32_t sx0 = physicalScissor.offset.x;
    const int32_t sy0 = physicalScissor.offset.y;
    const int32_t sx1 = sx0 + int32_t(physicalScissor.extent.width);
    const int32_t sy1 = sy0 + int32_t(physicalScissor.extent.height);

    // A normal resolve has vertices in logical surface coordinates (for
    // example 416..864), so after WINDOW_OFFSET they line up with this physical
    // scissor. The phased-depth pattern is different: its unoffset vertices are
    // already the local physical 0..448 rectangle. Requiring the raw rectangle
    // to cover the physical scissor distinguishes the two without a hash/title
    // special case.
    if (rawBounds.x0 > sx0 || rawBounds.y0 > sy0 ||
        rawBounds.x1 < sx1 || rawBounds.y1 < sy1)
        return false;

    const int32_t logicalX0 = sx0 - windowX;
    const int32_t logicalY0 = sy0 - windowY;
    const int32_t logicalX1 = sx1 - windowX;
    const int32_t logicalY1 = sy1 - windowY;
    if (logicalX0 < 0 || logicalY0 < 0 ||
        logicalX1 <= logicalX0 || logicalY1 <= logicalY0 ||
        logicalX0 >= int32_t(surfW) || logicalY0 >= int32_t(surfH))
        return false;

    const uint32_t clippedX1 = std::min<uint32_t>(uint32_t(logicalX1), surfW);
    const uint32_t clippedY1 = std::min<uint32_t>(uint32_t(logicalY1), surfH);
    if (clippedX1 <= uint32_t(logicalX0) || clippedY1 <= uint32_t(logicalY0))
        return false;

    const uint32_t candidateKey =
        (dest - MacroTileOffset(uint32_t(logicalX0), uint32_t(logicalY0), surfW)) &
        0x1FFFFFFFu;
    ResolveSnapshot* existing =
        FindDepthSnapshot(candidateKey, surfW, surfH, depthFormat, depthFloat24);
    if (!existing || existing->frameSeen != resolveFrame || !existing->copies)
        return false;

    x0 = uint32_t(logicalX0);
    y0 = uint32_t(logicalY0);
    x1 = clippedX1;
    y1 = clippedY1;
    key = candidateKey;
    return true;
}

// Map a logical resolve window to the physical EDRAM raster window. Clip the
// pair together: clipping a negative source must advance the destination too.
bool ResolveCopyRegion(const uint32_t* regs, uint32_t x0, uint32_t y0,
                       uint32_t x1, uint32_t y1, VkImageCopy& copy)
{
    int32_t windowX, windowY;
    DecodeVertexWindowOffset(regs, windowX, windowY);
    const int32_t left = std::max(int32_t(x0), -windowX);
    const int32_t top = std::max(int32_t(y0), -windowY);
    const int32_t right = std::min(int32_t(x1), int32_t(g_extent.width) - windowX);
    const int32_t bottom = std::min(int32_t(y1), int32_t(g_extent.height) - windowY);
    if (right <= left || bottom <= top)
        return false;
    copy.srcOffset = {left + windowX, top + windowY, 0};
    copy.dstOffset = {left, top, 0};
    copy.extent = {uint32_t(right - left), uint32_t(bottom - top), 1};
    return true;
}

bool IsCollapsedHorizontalTileResolve(uint32_t surfW, uint32_t surfH,
                                      uint32_t x0, uint32_t y0,
                                      uint32_t x1, uint32_t y1)
{
    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    const bool sceneTilingActive = CollapseTiledDrawsEnabled() &&
                                   g_sceneTilingLastIndexedFrame != 0 &&
                                   frame <= g_sceneTilingLastIndexedFrame + 1;
    if (!sceneTilingActive || surfW != g_extent.width || surfH != g_extent.height ||
        y0 != 0 || y1 != surfH || x1 <= x0)
        return false;

    // Crash's 1280x720 EDRAM pass is replayed as three overlapping 448-pixel
    // horizontal windows: 0..448, 416..864 and 832..1280. Once the host path
    // collapses the matching predicated draws into one full-size raster pass,
    // those resolves must read the logical host coordinates rather than remap
    // all three windows back onto the same physical EDRAM tile.
    const uint32_t tileWidth = 448;
    const uint32_t tileStep = 416;
    return (x0 == 0 && x1 == tileWidth) ||
           (x0 == tileStep && x1 == tileStep + tileWidth) ||
           (x0 == tileStep * 2 && x1 == surfW);
}

bool IsStitchedHorizontalTileResolve(uint32_t surfW, uint32_t surfH,
                                     uint32_t x0, uint32_t y0,
                                     uint32_t x1, uint32_t y1)
{
    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    const bool sceneTilingActive = StitchTiledDrawsEnabled() &&
                                   g_sceneStitchLastIndexedFrame != 0 &&
                                   frame <= g_sceneStitchLastIndexedFrame + 1;
    if (!sceneTilingActive || surfW != g_extent.width || surfH != g_extent.height ||
        y0 != 0 || y1 != surfH || x1 <= x0)
        return false;

    const uint32_t tileWidth = 448;
    const uint32_t tileStep = 416;
    return (x0 == 0 && x1 == tileWidth) ||
           (x0 == tileStep && x1 == tileStep + tileWidth) ||
           (x0 == tileStep * 2 && x1 == surfW);
}

bool StitchedNextTileClearRect(uint32_t surfW, uint32_t surfH,
                              uint32_t x0, uint32_t y0,
                              uint32_t x1, uint32_t y1,
                              VkRect2D& rect)
{
    const int dynamicMode = StitchTileClearMode();
    const bool rotate = dynamicMode >= 1 || StitchRotateTileClearsEnabled();
    if (!rotate ||
        !IsStitchedHorizontalTileResolve(surfW, surfH, x0, y0, x1, y1))
        return false;

    constexpr uint32_t tileWidth = 448;
    constexpr uint32_t tileStep = 416;
    uint32_t nextX = 0;
    if (x0 == 0)
        nextX = tileStep;
    else if (x0 == tileStep)
        nextX = tileStep * 2;
    else
        nextX = 0;

    const uint32_t nextX1 = std::min(nextX + tileWidth, surfW);
    rect.offset = {static_cast<int32_t>(nextX), 0};
    rect.extent = {nextX1 - nextX, surfH};
    return rect.extent.width != 0 && rect.extent.height != 0;
}

bool SuppressStitchedWrapClear(uint32_t surfW, uint32_t surfH,
                               uint32_t x0, uint32_t y0,
                               uint32_t x1, uint32_t y1)
{
    return StitchTileClearMode() == 2 &&
           IsStitchedHorizontalTileResolve(surfW, surfH, x0, y0, x1, y1) &&
           x0 == 832;
}

bool ResolveCopyRegionForHost(const uint32_t* regs,
                              uint32_t surfW, uint32_t surfH,
                              uint32_t x0, uint32_t y0,
                              uint32_t x1, uint32_t y1,
                              VkImageCopy& copy)
{
    if (IsStitchedHorizontalTileResolve(surfW, surfH, x0, y0, x1, y1))
    {
        // Diagnostic source-address switch. Keep the destination logical in both
        // models; only choose whether the source is the host-stitched logical
        // rectangle or the physical Xenos EDRAM window selected by WINDOW_OFFSET.
        // Bits: 1=color0, 2=depth, 4=RT1.
        const uint32_t srcSelect = regs[xenos::kRbCopyControl] & 7u;
        const int sourceMode = StitchResolveSourceMode();
        const int sourceBit = srcSelect == 0 ? 1 : (srcSelect == 4 ? 2 : (srcSelect == 1 ? 4 : 0));
        if (StitchPhysicalTileRasterEnabled() || (sourceBit && (sourceMode & sourceBit)))
        {
            if (!ResolveCopyRegion(regs, x0, y0, x1, y1, copy))
                return false;
            static uint32_t reports = 0;
            if (reports++ < 36)
                KLOG("[stitch resolve source] mode=%d src=%u logical=%u..%u physical=%d..%d dst=%d..%d\n",
                     sourceMode, srcSelect, x0, x1,
                     copy.srcOffset.x, copy.srcOffset.x + int32_t(copy.extent.width),
                     copy.dstOffset.x, copy.dstOffset.x + int32_t(copy.extent.width));
            return true;
        }

        // In stitch mode each Xenos tile is rasterized directly into its logical
        // 1280x720 host coordinates. Resolve from that same logical rectangle;
        // applying PA_SC_WINDOW_OFFSET here would incorrectly remap tile 1/2
        // back onto the physical 0..448 EDRAM window a second time.
        copy.srcOffset = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), 0};
        copy.dstOffset = copy.srcOffset;
        copy.extent = {x1 - x0, y1 - y0, 1};
        return true;
    }

    if (!IsCollapsedHorizontalTileResolve(surfW, surfH, x0, y0, x1, y1))
    {
        if (!ResolveCopyRegion(regs, x0, y0, x1, y1, copy))
            return false;

        // Diagnostic model for the title's 448-wide / 416-step EDRAM windows.
        // The 32-pixel overlap is raster guard-band coverage, not two logical
        // destinations that both need to survive in the assembled snapshot.
        // Keep a single writer for each logical pixel: 416 + 416 + 448 = 1280.
        static const bool trimTileOverlap = [] {
            const char* value = std::getenv("MOJORECOMP_TRIM_TILE_RESOLVE_OVERLAP");
            return value && std::strcmp(value, "1") == 0;
        }();
        if (trimTileOverlap && surfW == 1280 && surfH == 720 && y0 == 0 && y1 == 720)
        {
            constexpr uint32_t kTileStep = 416;
            constexpr uint32_t kTileWidth = 448;
            if ((x0 == 0 && x1 == kTileWidth) ||
                (x0 == kTileStep && x1 == kTileStep + kTileWidth))
            {
                copy.extent.width = kTileStep;
            }
        }
        return true;
    }

    // The first collapsed tile already contains the full logical scene in the
    // host render target. Resolve the complete image once; the caller discards
    // the middle/last tile copies as redundant replays.
    if (x0 == 0)
    {
        copy.srcOffset = {0, 0, 0};
        copy.dstOffset = {0, 0, 0};
        copy.extent = {surfW, surfH, 1};
        return true;
    }

    copy.srcOffset = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), 0};
    copy.dstOffset = copy.srcOffset;
    copy.extent = {x1 - x0, y1 - y0, 1};
    return true;
}

bool IsFirstCollapsedHorizontalTile(uint32_t surfW, uint32_t surfH,
                                    uint32_t x0, uint32_t y0,
                                    uint32_t x1, uint32_t y1)
{
    return IsCollapsedHorizontalTileResolve(surfW, surfH, x0, y0, x1, y1) && x0 == 0;
}

bool IsLastCollapsedHorizontalTile(uint32_t surfW, uint32_t surfH,
                                   uint32_t x0, uint32_t y0,
                                   uint32_t x1, uint32_t y1)
{
    return IsCollapsedHorizontalTileResolve(surfW, surfH, x0, y0, x1, y1) && x1 == surfW;
}

bool ClearFullResolveAttachments(const uint32_t* regs, bool clearColor, bool clearDepth)
{
    if (!clearColor && !clearDepth)
        return true;
    if (!PrepareActiveEdramOwnersForFullOverwrite(clearColor, false, clearDepth))
        return false;
    ResumeColorRendering(false);
    std::array<VkClearAttachment, 2> attachments{};
    uint32_t attachmentCount = 0;
    if (clearColor)
    {
        ++g_perfColorClears;
        const uint32_t c = regs[xenos::kRbColorClear];
        auto& attachment = attachments[attachmentCount++];
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color.float32[0] = float(c & 0xFFu) / 255.0f;
        attachment.clearValue.color.float32[1] = float((c >> 8) & 0xFFu) / 255.0f;
        attachment.clearValue.color.float32[2] = float((c >> 16) & 0xFFu) / 255.0f;
        attachment.clearValue.color.float32[3] = float((c >> 24) & 0xFFu) / 255.0f;
    }
    if (clearDepth)
    {
        ++g_perfDepthClears;
        const uint32_t d = regs[xenos::kRbDepthClear];
        auto& attachment = attachments[attachmentCount++];
        attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        attachment.clearValue.depthStencil.depth =
            DecodeDepthClearValue(regs[xenos::kRbDepthInfo], d);
        attachment.clearValue.depthStencil.stencil = d & 0xFFu;
    }
    g_perfClearRectPixels += uint64_t(g_extent.width) * g_extent.height;
    VkClearRect rect{};
    rect.rect = {{0, 0}, g_internalExtent};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    p_vkCmdClearAttachments(g_commandBuffer, attachmentCount, attachments.data(), 1, &rect);
    ++g_renderWriteGeneration;
    return true;
}

bool ResolveDepthSurface(uint8_t* guestBase, const uint32_t* regs, uint32_t control)
{
    const uint32_t surfW = regs[xenos::kRbCopyDestPitch] & 0x3FFFu;
    const uint32_t surfH = (regs[xenos::kRbCopyDestPitch] >> 16) & 0x3FFFu;
    const uint32_t tl = regs[xenos::kPaScWindowScissorTl];
    const uint32_t br = regs[xenos::kPaScWindowScissorBr];
    uint32_t x0 = tl & 0x7FFFu;
    uint32_t y0 = (tl >> 16) & 0x7FFFu;
    uint32_t x1 = br & 0x7FFFu;
    uint32_t y1 = (br >> 16) & 0x7FFFu;
    ResolveVertexBounds rawBounds{};
    const ResolveRectDecode rectDecode =
        DecodeResolveRectFromVertices(guestBase, regs, surfW, surfH,
                                      x0, y0, x1, y1, &rawBounds);

    const uint32_t dest = regs[xenos::kRbCopyDestBase] & 0xFFFFFFFCu;
    const uint64_t resolveFrame = g_frames.load(std::memory_order_relaxed) + 1;
    DepthBacking* activeDepth = ActiveDepthBacking();
    const VkFormat resolveDepthFormat = activeDepth ? activeDepth->format
                                                    : HostDepthFormat(regs[xenos::kRbDepthInfo]);
    const bool resolveDepthFloat24 = ((regs[xenos::kRbDepthInfo] >> 16) & 1u) != 0;
    uint32_t key = 0;
    const bool recoveredPhasedResolve = RecoverPhasedDepthResolve(
        regs, surfW, surfH, rawBounds, resolveDepthFormat, resolveDepthFloat24,
        resolveFrame, dest, x0, y0, x1, y1, key);

    // Xenos clears, when requested by RB_COPY_CONTROL, apply to the region
    // covered by the resolve rectangle itself.  An empty resolve therefore has
    // neither a copy nor a post-resolve clear.  Do not fall back to the window
    // scissor here: doing so manufactures a full-tile copy/clear for a draw that
    // covers no pixels.
    if (!recoveredPhasedResolve && rectDecode == ResolveRectDecode::Empty)
        return true;
    if (!recoveredPhasedResolve && rectDecode == ResolveRectDecode::Invalid)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRect.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!surfW || !surfH || x1 <= x0 || y1 <= y0)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRect.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (!recoveredPhasedResolve)
        key = (dest - MacroTileOffset(x0, y0, surfW)) & 0x1FFFFFFFu;
    if (resolveFrame >= 390 && resolveFrame <= 398)
    {
        KLOG("PM4 resolve frame=%llu surf=%ux%u rect=%u,%u-%u,%u dest=%08X key=%08X "
             "src=%u winTL=%08X winBR=%08X winOff=%08X\n",
             static_cast<unsigned long long>(resolveFrame), surfW, surfH,
             x0, y0, x1, y1, dest, key, 4u,
             regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
             regs[xenos::kPaScWindowOffset]);
    }
    if (surfW == 1280 && surfH == 720 && resolveFrame >= 360 && resolveFrame <= 430 &&
        y0 == 0 && y1 == 720 && (x1 - x0) == 448)
    {
        KLOG("PM4 tile resolve frame=%llu rect=%u,%u-%u,%u dest=%08X key=%08X "
             "winTL=%08X winBR=%08X winOff=%08X\n",
             static_cast<unsigned long long>(resolveFrame), x0, y0, x1, y1, dest, key,
             regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
             regs[xenos::kPaScWindowOffset]);
    }
    if (surfW == 1280 && surfH == 720)
    {
        struct ResolveShape { uint32_t x0, y0, x1, y1, dest, key; };
        static std::array<ResolveShape, 96> shapes{};
        static uint32_t shapeCount = 0;
        bool duplicate = false;
        for (uint32_t i = 0; i < shapeCount; ++i)
        {
            const auto& s = shapes[i];
            duplicate |= s.x0 == x0 && s.y0 == y0 && s.x1 == x1 && s.y1 == y1 &&
                         s.dest == dest && s.key == key;
        }
        if (!duplicate && shapeCount < shapes.size())
        {
            shapes[shapeCount++] = {x0, y0, x1, y1, dest, key};
            KLOG_DIAG("[resolve shape] n=%u rect=%u,%u-%u,%u dest=%08X key=%08X color=%08X control=%08X\n",
                      shapeCount, x0, y0, x1, y1, dest, key,
                      regs[xenos::kRbColorInfo], control);
        }
    }
    if (surfW == 1280 && surfH == 720 && x0 == 0 && y0 == 0 && x1 == 448 && y1 == 720)
    {
        static std::array<uint32_t, 64> seen{};
        static uint32_t seenCount = 0;
        bool duplicate = false;
        for (uint32_t i = 0; i < seenCount; ++i)
            duplicate |= seen[i] == dest;
        if (!duplicate && seenCount < seen.size())
        {
            seen[seenCount++] = dest;
            KLOG("[tile resolve] unique=%u dest=%08X key=%08X color=%08X control=%08X destInfo=%08X copies=%llu\n",
                 seenCount, dest, key, regs[xenos::kRbColorInfo], control,
                 regs[xenos::kRbCopyDestInfo],
                 static_cast<unsigned long long>(g_resolveCopies.load(std::memory_order_relaxed)));
        }
    }
    const uint32_t copyX = std::min(x0, g_extent.width);
    const uint32_t copyY = std::min(y0, g_extent.height);
    const uint32_t copyX1 = std::min({x1, surfW, g_extent.width});
    const uint32_t copyY1 = std::min({y1, surfH, g_extent.height});
    if (copyX1 <= copyX || copyY1 <= copyY)
        return true;

    const bool clearDepth = ((control >> 9) & 1u) != 0;

    // Multisampled Xenos depth resolves use SAMPLE_ZERO here. This is now tied
    // to the actual guest surface sample count instead of the old global 2x
    // diagnostic switch.
    const bool multisampledDepth =
        activeDepth && activeDepth->samples != VK_SAMPLE_COUNT_1_BIT;
    const bool collapsedTileResolve =
        IsCollapsedHorizontalTileResolve(surfW, surfH, copyX, copyY, copyX1, copyY1);
    if (collapsedTileResolve &&
        !IsFirstCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1))
    {
        // The first collapsed depth resolve captured the entire host depth target.
        // Preserve the console's post-resolve clear only when the last tile replay
        // arrives, but do not recopy the already complete snapshot.
        if (clearDepth &&
            IsLastCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1))
            if (!ClearFullResolveAttachments(regs, false, true))
                return false;
        return true;
    }

    // Do not allocate a new snapshot for the redundant middle/last EDRAM tile.
    // Their destination pointer may still describe the physical tile, which would
    // otherwise manufacture alias keys and permanently consume snapshot slots.
    ResolveSnapshot* snapshot = nullptr;
    if (!CreateDepthSnapshot(key, surfW, surfH, resolveDepthFormat,
                             resolveDepthFloat24, snapshot) || !snapshot)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedSnapshot.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    VkImageCopy copy{};
    if (!ResolveCopyRegionForHost(regs, surfW, surfH, copyX, copyY, copyX1, copyY1, copy))
        return true;
    if (TraceDrawFrame(resolveFrame))
    {
        KLOG("[resolve frame] frame=%llu src=depth control=%08X key=%08X "
             "rect=%u,%u-%u,%u copySrc=%d,%d copyDst=%d,%d size=%ux%u "
             "clearDepth=%u surface=%08X depthInfo=%08X winOff=%08X\n",
             static_cast<unsigned long long>(resolveFrame), control, key,
             copyX, copyY, copyX1, copyY1,
             copy.srcOffset.x, copy.srcOffset.y,
             copy.dstOffset.x, copy.dstOffset.y,
             copy.extent.width, copy.extent.height, clearDepth ? 1u : 0u,
             regs[xenos::kRbSurfaceInfo], regs[xenos::kRbDepthInfo],
             regs[xenos::kPaScWindowOffset]);
    }
    if (activeDepth &&
        !MaterializeEdramOwnerForResolve(
            {EdramOwnerKind::Depth, activeDepth->key}, resolveFrame, "depth"))
        return false;
    // Vulkan's native depth resolve is used when SAMPLE_ZERO is supported. On
    // devices without it, materialize the same Xenos sample-zero semantics into
    // the backing's single-sample mirror with a tiny depth-only shader before
    // the normal snapshot copy below.
    activeDepth = ActiveDepthBacking();
    if (activeDepth && activeDepth->samples != VK_SAMPLE_COUNT_1_BIT &&
        !g_hostMsaaDepthSampleZero)
    {
        if (!ResolveDepthBackingMsaaSampleZero(*activeDepth, copy))
        {
            KLOG("Vulkan depth MSAA SAMPLE_ZERO fallback failed: key=%08X info=%08X samples=%u\n",
                 key, activeDepth->info, uint32_t(activeDepth->samples));
            return false;
        }
    }
    const VkImageCopy internalCopy = ScaleImageCopyToInternal(copy);
    if (activeDepth)
    {
        // RB_COPY_DEST_INFO's low endian bits are applied while the packed
        // depth word is written to guest memory. Preserve that exact byte order
        // in the color alias; a later texture fetch will apply its own endian
        // transform just like a normal guest-memory texture upload.
        const uint32_t resolveEndian = regs[xenos::kRbCopyDestInfo] & 3u;
        if (!PackDepthResolveSnapshot(*snapshot, *activeDepth, internalCopy,
                                      resolveEndian))
        {
            static uint32_t reports = 0;
            if (reports++ < 16)
                KLOG("Vulkan packed depth resolve unavailable: key=%08X endian=%u\n",
                     key, resolveEndian);
        }
    }
    EndColorRendering();

    VkImageMemoryBarrier depthToRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    activeDepth = ActiveDepthBacking();
    if (activeDepth && activeDepth->samples != VK_SAMPLE_COUNT_1_BIT)
    {
        TransitionResolveTarget(activeDepth->resolveImage, activeDepth->resolveLayout,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    else if (activeDepth)
    {
        TransitionDepthBacking(*activeDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    else
    {
        depthToRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthToRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        depthToRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthToRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        depthToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthToRead.image = g_depthImage;
        depthToRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                  VK_IMAGE_ASPECT_STENCIL_BIT;
        depthToRead.subresourceRange.levelCount = 1;
        depthToRead.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(g_commandBuffer,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, nullptr, 0, nullptr, 1, &depthToRead);
    }

    TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.dstSubresource.layerCount = 1;
    const VkImage depthCopySource =
        activeDepth && activeDepth->samples != VK_SAMPLE_COUNT_1_BIT
            ? activeDepth->resolveImage
            : ActiveDepthImage();
    p_vkCmdCopyImage(g_commandBuffer, depthCopySource,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     snapshot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &internalCopy);
    g_perfResolvePixels += uint64_t(copy.extent.width) * copy.extent.height;
    snapshot->frameSeen = resolveFrame;
    ++snapshot->copies;
    g_resolveCopies.fetch_add(1, std::memory_order_relaxed);

    ReportResolve(regs, *snapshot);
    // The resolve already had to leave dynamic rendering for the transfer.
    // Put the snapshot in its steady sampled layout here, before rendering is
    // resumed, instead of forcing PrepareTextures to end rendering a second
    // time on the first draw that samples this resolve.
    TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (activeDepth && activeDepth->samples != VK_SAMPLE_COUNT_1_BIT)
    {
        TransitionResolveTarget(activeDepth->resolveImage, activeDepth->resolveLayout,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
    else if (activeDepth)
    {
        TransitionDepthBacking(*activeDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
    else
    {
        VkImageMemoryBarrier depthBack{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        depthBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBack.image = g_depthImage;
        depthBack.subresourceRange = depthToRead.subresourceRange;
        depthBack.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        depthBack.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        depthBack.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBack.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &depthBack);
    }

    const bool clearCollapsedTile =
        !collapsedTileResolve || IsLastCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1);
    const bool suppressStitchedWrapClear =
        !collapsedTileResolve &&
        SuppressStitchedWrapClear(surfW, surfH, copyX, copyY, copyX1, copyY1);
    if (clearDepth && clearCollapsedTile && !suppressStitchedWrapClear)
    {
        const bool fullOverwrite = collapsedTileResolve;
        if (!(fullOverwrite
                  ? PrepareActiveEdramOwnersForFullOverwrite(false, false, true)
                  : PrepareActiveEdramOwnersForWrite(false, false, true,
                                                     "depth-resolve-clear")))
            return false;
        ResumeColorRendering(false);
        ++g_perfDepthClears;
        const bool stitchedTileResolve =
            IsStitchedHorizontalTileResolve(surfW, surfH, copyX, copyY, copyX1, copyY1);
        const bool physicalStitchedTile =
            stitchedTileResolve && StitchPhysicalTileRasterEnabled();
        VkRect2D clearRect = collapsedTileResolve
            ? VkRect2D{{0, 0}, g_extent}
            : physicalStitchedTile
                ? VkRect2D{{copy.srcOffset.x, copy.srcOffset.y},
                           {copy.extent.width, copy.extent.height}}
            : stitchedTileResolve
                ? VkRect2D{{static_cast<int32_t>(copyX), static_cast<int32_t>(copyY)},
                           {copyX1 - copyX, copyY1 - copyY}}
                : VkRect2D{{copy.srcOffset.x, copy.srcOffset.y},
                           {copy.extent.width, copy.extent.height}};
        VkRect2D stitchedClear{};
        if (!collapsedTileResolve &&
            StitchedNextTileClearRect(surfW, surfH, copyX, copyY, copyX1, copyY1, stitchedClear))
            clearRect = stitchedClear;
        g_perfClearRectPixels += uint64_t(clearRect.extent.width) * clearRect.extent.height;
        const uint32_t d = regs[xenos::kRbDepthClear];
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        attachment.clearValue.depthStencil.depth =
            DecodeDepthClearValue(regs[xenos::kRbDepthInfo], d);
        attachment.clearValue.depthStencil.stencil = d & 0xFFu;
        VkClearRect rect{};
        rect.rect = ScaleRectToInternal(clearRect);
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        p_vkCmdClearAttachments(g_commandBuffer, 1, &attachment, 1, &rect);
        ++g_renderWriteGeneration;
    }
    else
    {
        ResumeColorRendering(false);
    }
    return true;
}

bool ResolveRt1Surface(uint8_t* guestBase, const uint32_t* regs, uint32_t control)
{
    const uint32_t surfW = regs[xenos::kRbCopyDestPitch] & 0x3FFFu;
    const uint32_t surfH = (regs[xenos::kRbCopyDestPitch] >> 16) & 0x3FFFu;
    const uint32_t tl = regs[xenos::kPaScWindowScissorTl];
    const uint32_t br = regs[xenos::kPaScWindowScissorBr];
    uint32_t x0 = tl & 0x7FFFu;
    uint32_t y0 = (tl >> 16) & 0x7FFFu;
    uint32_t x1 = br & 0x7FFFu;
    uint32_t y1 = (br >> 16) & 0x7FFFu;
    const ResolveRectDecode rectDecode =
        DecodeResolveRectFromVertices(guestBase, regs, surfW, surfH, x0, y0, x1, y1);
    if (rectDecode == ResolveRectDecode::Empty)
        return true;
    if (rectDecode == ResolveRectDecode::Invalid)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRect.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!surfW || !surfH || x1 <= x0 || y1 <= y0)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRect.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const uint32_t dest = regs[xenos::kRbCopyDestBase] & 0xFFFFFFFCu;
    const uint32_t key = (dest - MacroTileOffset(x0, y0, surfW)) & 0x1FFFFFFFu;
    const uint64_t colorResolveFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (colorResolveFrame >= 390 && colorResolveFrame <= 398)
    {
        KLOG("PM4 color resolve frame=%llu surf=%ux%u rect=%u,%u-%u,%u dest=%08X key=%08X "
             "winTL=%08X winBR=%08X winOff=%08X\n",
             static_cast<unsigned long long>(colorResolveFrame), surfW, surfH,
             x0, y0, x1, y1, dest, key,
             regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
             regs[xenos::kPaScWindowOffset]);
    }
    const uint32_t copyX = std::min(x0, g_extent.width);
    const uint32_t copyY = std::min(y0, g_extent.height);
    const uint32_t copyX1 = std::min({x1, surfW, g_extent.width});
    const uint32_t copyY1 = std::min({y1, surfH, g_extent.height});
    if (copyX1 <= copyX || copyY1 <= copyY)
        return true;

    const bool collapsedTileResolve =
        IsCollapsedHorizontalTileResolve(surfW, surfH, copyX, copyY, copyX1, copyY1);
    if (collapsedTileResolve &&
        !IsFirstCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1))
        return true;

    ResolveSnapshot* snapshot = nullptr;
    if (!CreateSnapshot(key, surfW, surfH, snapshot) || !snapshot)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedSnapshot.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    VkImageCopy copy{};
    if (!ResolveCopyRegionForHost(regs, surfW, surfH, copyX, copyY, copyX1, copyY1, copy))
        return true;
    const uint32_t sourceInfo = regs[xenos::kRbColor1Info];
    EndColorRendering();
    ColorBacking* backing = FindColorBacking(regs[xenos::kRbSurfaceInfo], sourceInfo);
    if (!backing || !backing->initialized)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRt1.fetch_add(1, std::memory_order_relaxed);
        ResumeColorRendering();
        return true;
    }
    if (!MaterializeEdramOwnerForResolve(
            {EdramOwnerKind::Color, backing->key}, colorResolveFrame, "color1"))
        return false;
    static const bool traceBlurRt1 = [] {
        const char* value = std::getenv("MOJORECOMP_TRACE_BLUR_RT1");
        return value && value[0] && value[0] != '0';
    }();
    if (traceBlurRt1 && key == 0x097DD000u)
    {
        static uint64_t lastRt1ReportFrame = ~0ull;
        if (lastRt1ReportFrame != colorResolveFrame)
        {
            lastRt1ReportFrame = colorResolveFrame;
            KLOG("[blur rt1] frame=%llu key=%08X srcInfo=%08X active=%08X "
                 "lastWriteFrame=%llu lastWriteDraw=%llu lastWriteGen=%llu "
                 "currentDraw=%llu currentGen=%llu rect=%u,%u-%u,%u\n",
                 static_cast<unsigned long long>(colorResolveFrame), key, sourceInfo,
                 g_activeColorInfo,
                 static_cast<unsigned long long>(backing->lastWriteFrame),
                 static_cast<unsigned long long>(backing->lastWriteDraw),
                 static_cast<unsigned long long>(backing->lastWriteGeneration),
                 static_cast<unsigned long long>(g_draws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_renderWriteGeneration),
                 copyX, copyY, copyX1, copyY1);
        }

        // One bounded 3D-only dump of the writes that actually reached the RT1
        // backing immediately before the motion-vector resolve.  This is much
        // more useful than tracing the entire PM4 stream: it tells us whether
        // the mostly-black blur map was produced by the title's draw sequence or
        // by host-side tile/resolve handling.
        static bool dumped3dWriteHistory = false;
        if (!dumped3dWriteHistory && copyX == 0 &&
            g_indexedDraws.load(std::memory_order_relaxed) > 10000)
        {
            dumped3dWriteHistory = true;
            const uint32_t count = std::min<uint32_t>(g_blurRtWriteHistoryCursor,
                                                       g_blurRtWriteHistory.size());
            const uint32_t start = g_blurRtWriteHistoryCursor - count;
            KLOG("[blur rt1 history] begin frame=%llu entries=%u\n",
                 static_cast<unsigned long long>(colorResolveFrame), count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto& h = g_blurRtWriteHistory[(start + i) % g_blurRtWriteHistory.size()];
                KLOG("[blur rt1 history] #%u frame=%llu draw=%llu prim=%u count=%u idx=%u pred=%u "
                     "VS=%016llX PS=%016llX color=%08X color1=%08X mask=%08X blend=%08X "
                     "depth=%08X win=%08X..%08X off=%08X bin=%016llX/%016llX\n",
                     i,
                     static_cast<unsigned long long>(h.frame),
                     static_cast<unsigned long long>(h.draw),
                     h.prim, h.count, h.indexed ? 1u : 0u, h.predicated ? 1u : 0u,
                     static_cast<unsigned long long>(h.vs),
                     static_cast<unsigned long long>(h.ps),
                     h.colorInfo, h.color1Info, h.colorMask, h.blend, h.depth,
                     h.windowTl, h.windowBr, h.windowOffset,
                     static_cast<unsigned long long>(h.binMask),
                     static_cast<unsigned long long>(h.binSelect));
            }
            KLOG("[blur rt1 history] end\n");
        }
    }
    const bool sourceIsLive = backing->key == g_activeColorSurfaceKey ||
                              (g_activeColor1Enabled &&
                               backing->key == g_activeColor1SurfaceKey);
    const bool multisampled = backing->samples != VK_SAMPLE_COUNT_1_BIT;
    if (multisampled)
        TransitionColorResolveBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    else
        TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImage sourceImage = multisampled ? backing->resolveImage : backing->image;

    TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    const VkImageCopy internalCopy = ScaleImageCopyToInternal(copy);
    p_vkCmdCopyImage(g_commandBuffer, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     snapshot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &internalCopy);
    const uint64_t resolveFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (snapshot->frameSeen != resolveFrame)
        ResetSnapshotCoverage(*snapshot);
    snapshot->frameSeen = resolveFrame;
    ++snapshot->copies;
    g_resolveCopies.fetch_add(1, std::memory_order_relaxed);

    snapshot->resolveSwap = (regs[xenos::kRbCopyDestInfo] & (1u << 24)) != 0;
    MarkSnapshotCoverage(*snapshot,
                         static_cast<uint32_t>(copy.dstOffset.x),
                         static_cast<uint32_t>(copy.dstOffset.y),
                         copy.extent.width, copy.extent.height);

    ReportResolve(regs, *snapshot);
    TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (sourceIsLive)
    {
        if (multisampled)
            TransitionColorResolveBacking(*backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    ResumeColorRendering();
    return true;
}

bool ResolveColorSurface(uint8_t* guestBase, const uint32_t* regs)
{
    if (!BeginFrame())
        return false;

    g_resolves.fetch_add(1, std::memory_order_relaxed);
    if (SkipDuplicateResolve(regs))
        return true;
    const uint32_t control = regs[xenos::kRbCopyControl];
    const uint32_t srcSelect = control & 7u;
    const uint64_t dispatchFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (dispatchFrame >= 390 && dispatchFrame <= 398)
    {
        KLOG("PM4 resolve dispatch frame=%llu src=%u control=%08X dest=%08X pitch=%08X "
             "color0=%08X color1=%08X scissor=%08X..%08X winOff=%08X\n",
             static_cast<unsigned long long>(dispatchFrame), srcSelect, control,
             regs[xenos::kRbCopyDestBase], regs[xenos::kRbCopyDestPitch],
             regs[xenos::kRbColorInfo], regs[xenos::kRbColor1Info],
             regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
             regs[xenos::kPaScWindowOffset]);
    }
    if (srcSelect == 4)
        return ResolveDepthSurface(guestBase, regs, control);
    if (srcSelect == 1)
        return ResolveRt1Surface(guestBase, regs, control);
    if (srcSelect != 0)
    {
        static uint32_t depthReports = 0;
        if (srcSelect == 4 && depthReports++ < 12)
        {
            KLOG("Vulkan depth resolve request: dest=%08X pitch=%08X info=%08X control=%08X "
                 "depthInfo=%08X scissor=%08X..%08X\n",
                 regs[xenos::kRbCopyDestBase], regs[xenos::kRbCopyDestPitch],
                 regs[xenos::kRbCopyDestInfo], control, regs[xenos::kRbDepthInfo],
                 regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr]);
        }
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedSource.fetch_add(1, std::memory_order_relaxed);
        if (srcSelect == 1)
        {
            static uint32_t rt1Reports = 0;
            if (rt1Reports++ < 12)
                KLOG("Vulkan RT1 resolve request: dest=%08X pitch=%08X info=%08X control=%08X "
                     "color0=%08X color1=%08X mask=%08X\n",
                     regs[xenos::kRbCopyDestBase], regs[xenos::kRbCopyDestPitch],
                     regs[xenos::kRbCopyDestInfo], control, regs[xenos::kRbColorInfo],
                     regs[xenos::kRbColor1Info], regs[xenos::kRbColorMask]);
            g_resolveUnsupportedRt1.fetch_add(1, std::memory_order_relaxed);
        }
        else if (srcSelect == 4)
            g_resolveUnsupportedDepth.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const uint32_t surfW = regs[xenos::kRbCopyDestPitch] & 0x3FFFu;
    const uint32_t surfH = (regs[xenos::kRbCopyDestPitch] >> 16) & 0x3FFFu;
    const uint32_t tl = regs[xenos::kPaScWindowScissorTl];
    const uint32_t br = regs[xenos::kPaScWindowScissorBr];
    uint32_t x0 = tl & 0x7FFFu;
    uint32_t y0 = (tl >> 16) & 0x7FFFu;
    uint32_t x1 = br & 0x7FFFu;
    uint32_t y1 = (br >> 16) & 0x7FFFu;
    const ResolveRectDecode rectDecode =
        DecodeResolveRectFromVertices(guestBase, regs, surfW, surfH, x0, y0, x1, y1);
    if (rectDecode == ResolveRectDecode::Empty)
        return true;
    if (rectDecode == ResolveRectDecode::Invalid)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRect.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!surfW || !surfH || x1 <= x0 || y1 <= y0)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedRect.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const uint32_t dest = regs[xenos::kRbCopyDestBase] & 0xFFFFFFFCu;
    const uint32_t key = (dest - MacroTileOffset(x0, y0, surfW)) & 0x1FFFFFFFu;
    const uint32_t copyX = std::min(x0, g_extent.width);
    const uint32_t copyY = std::min(y0, g_extent.height);
    const uint32_t copyX1 = std::min({x1, surfW, g_extent.width});
    const uint32_t copyY1 = std::min({y1, surfH, g_extent.height});
    if (copyX1 <= copyX || copyY1 <= copyY)
        return true;

    const bool clearColor = ((control >> 8) & 1u) != 0;
    const bool clearDepth = ((control >> 9) & 1u) != 0;
    const bool collapsedTileResolve =
        IsCollapsedHorizontalTileResolve(surfW, surfH, copyX, copyY, copyX1, copyY1);
    if (collapsedTileResolve &&
        !IsFirstCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1))
    {
        if ((clearColor || clearDepth) &&
            IsLastCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1))
        {
            if (!SwitchActiveColorSurface(regs[xenos::kRbSurfaceInfo], regs[xenos::kRbColorInfo]))
                return false;
            if (!ClearFullResolveAttachments(regs, clearColor, clearDepth))
                return false;
        }
        return true;
    }

    ResolveSnapshot* snapshot = nullptr;
    if (!CreateSnapshot(key, surfW, surfH, snapshot) || !snapshot)
    {
        g_resolveUnsupported.fetch_add(1, std::memory_order_relaxed);
        g_resolveUnsupportedSnapshot.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    VkImageCopy copy{};
    if (!ResolveCopyRegionForHost(regs, surfW, surfH, copyX, copyY, copyX1, copyY1, copy))
        return true;
    if (TraceDrawFrame(dispatchFrame))
    {
        KLOG("[resolve frame] frame=%llu src=color0 control=%08X key=%08X "
             "rect=%u,%u-%u,%u copySrc=%d,%d copyDst=%d,%d size=%ux%u "
             "clearColor=%u clearDepth=%u surface=%08X colorInfo=%08X "
             "depthInfo=%08X winOff=%08X\n",
             static_cast<unsigned long long>(dispatchFrame), control, key,
             copyX, copyY, copyX1, copyY1,
             copy.srcOffset.x, copy.srcOffset.y,
             copy.dstOffset.x, copy.dstOffset.y,
             copy.extent.width, copy.extent.height,
             clearColor ? 1u : 0u, clearDepth ? 1u : 0u,
             regs[xenos::kRbSurfaceInfo], regs[xenos::kRbColorInfo],
             regs[xenos::kRbDepthInfo], regs[xenos::kPaScWindowOffset]);
    }
    if (CollapseTiledDrawsEnabled() && g_sceneTilingLastIndexedFrame != 0 &&
        surfW == g_extent.width && surfH == g_extent.height &&
        y0 == 0 && y1 == surfH && (x0 == 0 || x0 == 416 || x0 == 832))
    {
        static uint32_t collapseResolveReports = 0;
        if (collapseResolveReports++ < 96)
            KLOG_DIAG("[collapse resolve] frame=%llu rect=%u..%u collapsed=%u first=%u last=%u "
                 "copySrc=%d..%d copyDst=%d..%d extent=%ux%u lastIndexed=%llu\n",
                 static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                 x0, x1, collapsedTileResolve ? 1u : 0u,
                 IsFirstCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1) ? 1u : 0u,
                 IsLastCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1) ? 1u : 0u,
                 copy.srcOffset.x, copy.srcOffset.x + int32_t(copy.extent.width),
                 copy.dstOffset.x, copy.dstOffset.x + int32_t(copy.extent.width),
                 copy.extent.width, copy.extent.height,
                 static_cast<unsigned long long>(g_sceneTilingLastIndexedFrame));
    }    const uint64_t mapFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (mapFrame >= 390 && mapFrame <= 398)
    {
        KLOG_DIAG("PM4 color map frame=%llu dst=%u,%u size=%ux%u src=%d,%d "
             "mode=%08X winOff=%08X decodedOff=%d,%d\n",
             static_cast<unsigned long long>(mapFrame), copyX, copyY,
             copy.extent.width, copy.extent.height, copy.srcOffset.x, copy.srcOffset.y,
             regs[xenos::kPaSuScModeCntl], regs[xenos::kPaScWindowOffset],
             copy.srcOffset.x - copy.dstOffset.x, copy.srcOffset.y - copy.dstOffset.y);
    }

    // Resolve source is the CURRENT RB_COLOR_INFO, not the last raster draw's
    // surface. A PM4 state update may select a different RT without any draw.
    // Selecting it also ensures post-copy clears affect the requested source.
    if (!SwitchActiveColorSurface(regs[xenos::kRbSurfaceInfo], regs[xenos::kRbColorInfo]))
        return false;
    EndColorRendering();

    ColorBacking* activeBacking = ActiveColorBacking();
    if (!activeBacking)
        return false;
    if (!MaterializeEdramOwnerForResolve(
            {EdramOwnerKind::Color, activeBacking->key}, dispatchFrame, "color0"))
        return false;
    const bool sceneColorSnapshot = key == 0x0A63D000u || key == 0x09B75000u;
    const bool stitchedSceneResolve =
        IsStitchedHorizontalTileResolve(surfW, surfH, copyX, copyY, copyX1, copyY1);
    uint32_t sceneTileBit = 0;
    if (stitchedSceneResolve)
    {
        if (copyX == 0)
            sceneTileBit = 1;
        else if (copyX == 416)
            sceneTileBit = 2;
        else if (copyX == 832)
            sceneTileBit = 4;
    }
    const int sceneResolveMask = sceneColorSnapshot ? SceneColorResolveTileMask() : 7;
    const bool skipSceneSnapshotCopy =
        sceneColorSnapshot && sceneTileBit && (sceneResolveMask & int(sceneTileBit)) == 0;

    uint64_t n = g_resolveCopies.load(std::memory_order_relaxed);
    if (!skipSceneSnapshotCopy)
    {
        const bool multisampled = activeBacking->samples != VK_SAMPLE_COUNT_1_BIT;
        if (multisampled)
            TransitionColorResolveBacking(*activeBacking, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        else
            TransitionColorBacking(*activeBacking, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource = copy.srcSubresource;
        const VkImageCopy internalCopy = ScaleImageCopyToInternal(copy);
        p_vkCmdCopyImage(g_commandBuffer,
                         multisampled ? activeBacking->resolveImage : activeBacking->image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         snapshot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &internalCopy);
        g_perfResolvePixels += uint64_t(copy.extent.width) * copy.extent.height;
        const uint64_t resolveFrame = g_frames.load(std::memory_order_relaxed) + 1;
        if (snapshot->frameSeen != resolveFrame)
            ResetSnapshotCoverage(*snapshot);
        snapshot->frameSeen = resolveFrame;
        ++snapshot->copies;
        n = g_resolveCopies.fetch_add(1, std::memory_order_relaxed) + 1;
        snapshot->resolveSwap = (regs[xenos::kRbCopyDestInfo] & (1u << 24)) != 0;

        MarkSnapshotCoverage(*snapshot,
                             static_cast<uint32_t>(copy.dstOffset.x),
                             static_cast<uint32_t>(copy.dstOffset.y),
                             copy.extent.width, copy.extent.height);

        ReportResolve(regs, *snapshot);
        // For index-hash A/B diagnostics, synchronize the capture to rendering
        // content rather than a host frame number. Once a selected mesh has been
        // observed, the first complete scene-color resolve is the comparable
        // evidence point for both the observe-only control and the skip run.
        const uint64_t indexHashEvent = g_indexHashEventFrame.load(std::memory_order_relaxed);
        if (sceneColorSnapshot && indexHashEvent && resolveFrame >= indexHashEvent &&
            copy.dstOffset.x == 0 && copy.dstOffset.y == 0 &&
            copy.extent.width == surfW && copy.extent.height == surfH)
        {
            uint64_t expected = 0;
            if (g_indexHashCaptureFrame.compare_exchange_strong(
                    expected, resolveFrame, std::memory_order_relaxed))
            {
                g_indexHashCaptureSnapshotKey.store(key, std::memory_order_relaxed);
                KLOG("[index hash event] scene capture ready frame=%llu key=%08X eventFrame=%llu\n",
                     static_cast<unsigned long long>(resolveFrame), key,
                     static_cast<unsigned long long>(indexHashEvent));
            }
        }
        // In collapse mode the title performs many guest 448-pixel resolves
        // that the host expands to a full logical image. Those are intermediate
        // feedback generations while the frame is still being built. Publish
        // stable feedback only from the title's explicit final full-surface
        // resolve, after scene geometry/effects have completed for this pass.
        const bool finalFullSceneResolve =
            sceneColorSnapshot && !collapsedTileResolve &&
            copyX == 0 && copyY == 0 && copyX1 == surfW && copyY1 == surfH &&
            copy.dstOffset.x == 0 && copy.dstOffset.y == 0 &&
            copy.extent.width == surfW && copy.extent.height == surfH;
        if (sceneColorSnapshot && StableSceneFeedbackEnabled() &&
            (sceneTileBit == 4 || finalFullSceneResolve))
        {
            CommitSnapshotForSceneFeedback(*snapshot);
        }
        TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    else
    {
        static uint32_t reports = 0;
        if (reports++ < 48)
            KLOG("[scene resolve tile mask] frame=%llu key=%08X mask=%d tileBit=%u rect=%u..%u copy skipped\n",
                 static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                 key, sceneResolveMask, sceneTileBit, copyX, copyX1);
    }
    if (n <= 12)
        KLOG("Vulkan resolve copy #%llu: dest=%08X -> key=%08X tile=(%u,%u)-(%u,%u) "
             "surface=%ux%u frame=%llu\n",
             static_cast<unsigned long long>(n), dest, key, copyX, copyY, copyX1, copyY1,
             surfW, surfH, static_cast<unsigned long long>(snapshot->frameSeen));

    if (activeBacking->samples != VK_SAMPLE_COUNT_1_BIT)
        TransitionColorResolveBacking(*activeBacking, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    TransitionColorBacking(*activeBacking, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const bool clearCollapsedTile =
        !collapsedTileResolve || IsLastCollapsedHorizontalTile(surfW, surfH, copyX, copyY, copyX1, copyY1);
    const bool suppressStitchedWrapClear =
        !collapsedTileResolve &&
        SuppressStitchedWrapClear(surfW, surfH, copyX, copyY, copyX1, copyY1);
    if ((clearColor || clearDepth) && clearCollapsedTile && !suppressStitchedWrapClear)
    {
        const bool fullOverwrite = collapsedTileResolve;
        if (!(fullOverwrite
                  ? PrepareActiveEdramOwnersForFullOverwrite(clearColor, false, clearDepth)
                  : PrepareActiveEdramOwnersForWrite(clearColor, false, clearDepth,
                                                     "color-resolve-clear")))
            return false;
        ResumeColorRendering(false);
        std::array<VkClearAttachment, 2> attachments{};
        uint32_t attachmentCount = 0;
        if (clearColor)
        {
            ++g_perfColorClears;
            const uint32_t c = regs[xenos::kRbColorClear];
            auto& attachment = attachments[attachmentCount++];
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = 0;
            attachment.clearValue.color.float32[0] = float(c & 0xFFu) / 255.0f;
            attachment.clearValue.color.float32[1] = float((c >> 8) & 0xFFu) / 255.0f;
            attachment.clearValue.color.float32[2] = float((c >> 16) & 0xFFu) / 255.0f;
            attachment.clearValue.color.float32[3] = float((c >> 24) & 0xFFu) / 255.0f;
        }
        if (clearDepth)
        {
            ++g_perfDepthClears;
            const uint32_t d = regs[xenos::kRbDepthClear];
            auto& attachment = attachments[attachmentCount++];
            attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            attachment.clearValue.depthStencil.depth =
                DecodeDepthClearValue(regs[xenos::kRbDepthInfo], d);
            attachment.clearValue.depthStencil.stencil = d & 0xFFu;
        }
        const bool stitchedTileResolve =
            IsStitchedHorizontalTileResolve(surfW, surfH, copyX, copyY, copyX1, copyY1);
        const bool physicalStitchedTile =
            stitchedTileResolve && StitchPhysicalTileRasterEnabled();
        VkRect2D clearRect = collapsedTileResolve
            ? VkRect2D{{0, 0}, g_extent}
            : physicalStitchedTile
                ? VkRect2D{{copy.srcOffset.x, copy.srcOffset.y},
                           {copy.extent.width, copy.extent.height}}
            : stitchedTileResolve
                ? VkRect2D{{static_cast<int32_t>(copyX), static_cast<int32_t>(copyY)},
                           {copyX1 - copyX, copyY1 - copyY}}
                : VkRect2D{{copy.srcOffset.x, copy.srcOffset.y},
                           {copy.extent.width, copy.extent.height}};
        VkRect2D stitchedClear{};
        if (!collapsedTileResolve &&
            StitchedNextTileClearRect(surfW, surfH, copyX, copyY, copyX1, copyY1, stitchedClear))
            clearRect = stitchedClear;
        g_perfClearRectPixels += uint64_t(clearRect.extent.width) * clearRect.extent.height;
        VkClearRect rect{};
        rect.rect = ScaleRectToInternal(clearRect);
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        p_vkCmdClearAttachments(g_commandBuffer, attachmentCount, attachments.data(), 1, &rect);
        ++g_renderWriteGeneration;
    }
    else
    {
        ResumeColorRendering(false);
    }
    return true;
}

bool CreateDummyTexture(uint32_t dimension)
{
    auto& dummy = g_dummyTextures[dimension];
    const uint32_t layers = dimension == 2 ? 6 : 1;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = dimension == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.imageType = dimension == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {1, 1, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateImage(g_device, &ii, nullptr, &dummy.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, dummy.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        p_vkAllocateMemory(g_device, &ai, nullptr, &dummy.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, dummy.image, dummy.memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = dummy.image;
    vi.viewType = dimension == 2 ? VK_IMAGE_VIEW_TYPE_CUBE :
                  dimension == 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    return p_vkCreateImageView(g_device, &vi, nullptr, &dummy.view) == VK_SUCCESS;
}

bool CreateNeutralBlurTexture()
{
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {1, 1, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateImage(g_device, &ii, nullptr, &g_neutralBlurTexture.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, g_neutralBlurTexture.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        p_vkAllocateMemory(g_device, &ai, nullptr, &g_neutralBlurTexture.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, g_neutralBlurTexture.image,
                            g_neutralBlurTexture.memory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_neutralBlurTexture.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return p_vkCreateImageView(g_device, &vi, nullptr, &g_neutralBlurTexture.view) == VK_SUCCESS;
}

bool CreateTextureDescriptors()
{
    for (uint32_t set = 0; set < 4; ++set)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = set == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        binding.descriptorCount = kTextureSlots;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (p_vkCreateDescriptorSetLayout(g_device, &li, nullptr, &g_textureLayouts[set]) != VK_SUCCESS)
            return false;
    }
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorBundles * 3 * kTextureSlots},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorBundles * kTextureSlots},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kDescriptorBundles * 4;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (p_vkCreateDescriptorPool(g_device, &pi, nullptr, &g_texturePool) != VK_SUCCESS)
        return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (p_vkCreateSampler(g_device, &si, nullptr, &g_defaultSampler) != VK_SUCCESS)
        return false;
    for (uint32_t dimension = 0; dimension < 3; ++dimension)
        if (!CreateDummyTexture(dimension))
            return false;
    if (!CreateNeutralBlurTexture())
        return false;

    // Initialize all fallback descriptors with real, zero-filled images. This
    // one-time submission completes before any guest command buffer is recorded.
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(g_commandBuffer, &begin) != VK_SUCCESS)
        return false;
    ResetGraphicsCommandStateCache(g_commandBuffer);
    for (uint32_t dimension = 0; dimension < 3; ++dimension)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g_dummyTextures[dimension].image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dimension == 2 ? 6u : 1u};
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const VkClearColorValue zero{};
        p_vkCmdClearColorImage(g_commandBuffer, b.image, b.newLayout, &zero, 1, &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = b.newLayout;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g_neutralBlurTexture.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const VkClearColorValue neutral{{0.5f, 0.5f, 0.5f, 0.5f}};
        p_vkCmdClearColorImage(g_commandBuffer, b.image, b.newLayout, &neutral, 1,
                               &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = b.newLayout;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    if (p_vkEndCommandBuffer(g_commandBuffer) != VK_SUCCESS)
        return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    if (p_vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
        p_vkDeviceWaitIdle(g_device) != VK_SUCCESS)
        return false;
    KLOG("Vulkan texture ABI ready: sets=4 binding=0 slots=16 dummy=2D/3D/Cube neutralBlur=1x1\n");
    return true;
}

uint64_t HashTextureBundle(const TextureBundle& bundle)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    };
    mix(bundle.views.data(), bundle.views.size() * sizeof(bundle.views[0]));
    mix(bundle.samplers.data(), bundle.samplers.size() * sizeof(bundle.samplers[0]));
    return hash;
}

const TextureBundle* GetTextureBundle(const TextureBundle& wanted)
{
    const uint64_t hash = HashTextureBundle(wanted);
    const auto range = g_textureBundleLookup.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        const auto& bundle = g_textureBundles[it->second];
        if (bundle.views == wanted.views && bundle.samplers == wanted.samplers)
            return &bundle;
    }
    // Never update a set already referenced by a recorded command buffer.
    // Pool exhaustion stays explicit; no descriptors silently alias slot zero.
    if (g_textureBundles.size() >= kDescriptorBundles)
    {
        static uint32_t reports = 0;
        if (reports++ < 8)
            KLOG("Vulkan texture descriptor bundle cap reached: %zu/%u\n",
                 g_textureBundles.size(), kDescriptorBundles);
        return nullptr;
    }
    TextureBundle bundle = wanted;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_texturePool;
    ai.descriptorSetCount = 4;
    ai.pSetLayouts = g_textureLayouts;
    if (p_vkAllocateDescriptorSets(g_device, &ai, bundle.sets) != VK_SUCCESS)
        return nullptr;
    VkDescriptorImageInfo images[4][kTextureSlots]{};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t set = 0; set < 4; ++set)
    {
        for (uint32_t i = 0; i < kTextureSlots; ++i)
        {
            if (set == 3)
                images[set][i].sampler = bundle.samplers[i];
            else
            {
                images[set][i].imageView = set == 0 ? bundle.views[i] : g_dummyTextures[set].view;
                images[set][i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        writes[set].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[set].dstSet = bundle.sets[set];
        writes[set].descriptorCount = kTextureSlots;
        writes[set].descriptorType = set == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[set].pImageInfo = images[set];
    }
    p_vkUpdateDescriptorSets(g_device, 4, writes, 0, nullptr);
    g_textureBundles.push_back(bundle);
    g_textureBundleLookup.emplace(hash, g_textureBundles.size() - 1);
    return &g_textureBundles.back();
}

bool CreatePipelineLayout()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = 24;
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 4;
    ci.pSetLayouts = g_textureLayouts;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &range;
    return p_vkCreatePipelineLayout(g_device, &ci, nullptr, &g_pipelineLayout) == VK_SUCCESS;
}

VkSampler GetSnapshotSampler(const mojorecomp::texture_abi::Fetch2D& fetch,
                             int addressModeOverride = -1,
                             mojorecomp::gpu::TextureSource source =
                                 mojorecomp::gpu::TextureSource::ResolveSnapshot)
{
    const uint32_t clampX = addressModeOverride >= 0
        ? static_cast<uint32_t>(addressModeOverride) : fetch.clampX;
    const uint32_t clampY = addressModeOverride >= 0
        ? static_cast<uint32_t>(addressModeOverride) : fetch.clampY;
    const uint32_t filteringOverride = mojorecomp::config::Get().textureFiltering;
    const uint32_t key = clampX | (clampY << 3) |
                         (fetch.minFilter << 6) | (fetch.magFilter << 8) |
                         (fetch.mipFilter << 10) | (fetch.mipMin << 12) |
                         (fetch.mipMax << 16) |
                         (source == mojorecomp::gpu::TextureSource::GuestTexture
                              ? (1u << 23) : 0u) |
                         (filteringOverride << 24);
    for (const auto& rec : g_samplers)
        if (rec.key == key) return rec.sampler;
    constexpr VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = fetch.magFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = fetch.minFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = fetch.mipFilter == 1u ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                          : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = modes[clampX];
    si.addressModeV = modes[clampY];
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (mojorecomp::gpu::ShouldEnableAnisotropy(
            fetch, source, filteringOverride, g_effectiveSamplerAnisotropy))
    {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = g_effectiveSamplerAnisotropy;
    }
    if (fetch.mipFilter == 2u || fetch.mipMax == 0u)
    {
        si.minLod = 0.0f;
        si.maxLod = 0.0f;
    }
    else
    {
        si.minLod = static_cast<float>(fetch.mipMin);
        si.maxLod = static_cast<float>(fetch.mipMax);
    }
    VkSampler sampler = VK_NULL_HANDLE;
    if (p_vkCreateSampler(g_device, &si, nullptr, &sampler) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    g_samplers.push_back({key, sampler});
    return sampler;
}

VkImageView GetSnapshotView(ResolveSnapshot& snapshot,
                            const mojorecomp::texture_abi::Fetch2D& fetch)
{
    if (fetch.swizzle != 0x688u && fetch.swizzle != 0x60Au)
        return VK_NULL_HANDLE;
    if (snapshot.depth && fetch.format == 6u && fetch.swizzle == 0x60Au &&
        snapshot.packedDepthInitialized && snapshot.packedDepthImage)
    {
        const uint32_t endian = fetch.endian & 3u;
        if (snapshot.packedDepthBgraViews[endian])
            return snapshot.packedDepthBgraViews[endian];

        // Match the normal RGBA8 upload path exactly, except the source bytes
        // already live in a GPU color image. CopySwapped first applies the fetch
        // endian permutation, then the 0x60A fetch swizzle exposes BGRA.
        static constexpr VkComponentSwizzle kChannel[4] = {
            VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        static constexpr uint8_t kEndianMap[4][4] = {
            {0, 1, 2, 3}, // none
            {1, 0, 3, 2}, // 8-in-16
            {3, 2, 1, 0}, // 8-in-32
            {2, 3, 0, 1}, // 16-in-32
        };
        const uint8_t* e = kEndianMap[endian];
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = snapshot.packedDepthImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.components = {kChannel[e[2]], kChannel[e[1]],
                         kChannel[e[0]], kChannel[e[3]]};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (p_vkCreateImageView(g_device, &vi, nullptr,
                                &snapshot.packedDepthBgraViews[endian]) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return snapshot.packedDepthBgraViews[endian];
    }
    const bool swap = (fetch.swizzle == 0x60Au) ^ (!snapshot.depth && snapshot.resolveSwap);
    if (!swap)
        return snapshot.view;
    if (snapshot.bgraView)
        return snapshot.bgraView;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = snapshot.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    if (snapshot.depth)
    {
        // Xenos depth resolves may be rebound through an 8_8_8_8 BGRA fetch.
        // The native Vulkan depth view can't expose the individual packed D24S8
        // bytes, but it must expose numeric depth through the component selected
        // by that fetch. In particular, BGRA fetches select .x in guest shaders;
        // returning ZERO there makes every depth-presence test fail.
        vi.format = snapshot.format;
        vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
                         VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    }
    else
    {
        vi.format = g_swapFormat;
        vi.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
                         VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    if (p_vkCreateImageView(g_device, &vi, nullptr, &snapshot.bgraView) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return snapshot.bgraView;
}

VkImageView GetSnapshotPresentView(ResolveSnapshot& snapshot,
                                   const mojorecomp::texture_abi::Fetch2D& fetch)
{
    if (!snapshot.presentImage || !snapshot.presentValid)
        return VK_NULL_HANDLE;
    if (fetch.swizzle != 0x688u && fetch.swizzle != 0x60Au)
        return VK_NULL_HANDLE;
    const bool swap = (fetch.swizzle == 0x60Au) ^ snapshot.presentResolveSwap;
    if (!swap)
        return snapshot.presentView;
    if (snapshot.presentBgraView)
        return snapshot.presentBgraView;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = snapshot.presentImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = snapshot.format;
    vi.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
                     VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &snapshot.presentBgraView) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return snapshot.presentBgraView;
}

VkDeviceSize UploadAlloc(VkDeviceSize bytes, VkDeviceSize alignment = 16);

uint64_t HashGuestTextureKey(const mojorecomp::texture_abi::Fetch2D& fetch)
{
    // Metadata-only lookup key. Guest asset textures are addressed by their
    // fetch base plus the decoded layout; content coherency for the handful of
    // mutable resources is handled separately by sourceHash.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8)
        {
            h ^= uint8_t(value >> shift);
            h *= 1099511628211ull;
        }
    };
    mix(fetch.key);
    mix(fetch.width);
    mix(fetch.height);
    mix(fetch.format);
    mix(fetch.swizzle);
    mix(fetch.endian);
    mix(fetch.mipMax);
    return h;
}

GuestTexture* FindGuestTexture(const mojorecomp::texture_abi::Fetch2D& fetch)
{
    const uint64_t hash = HashGuestTextureKey(fetch);
    const auto range = g_guestTextureLookup.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second >= g_guestTextures.size())
            continue;
        auto& texture = g_guestTextures[it->second];
        if (texture.key == fetch.key && texture.width == fetch.width &&
            texture.height == fetch.height && texture.format == fetch.format &&
            texture.swizzle == fetch.swizzle && texture.endian == fetch.endian &&
            texture.mipMax == fetch.mipMax)
            return &texture;
    }
    return nullptr;
}

void RememberGuestTexture(const mojorecomp::texture_abi::Fetch2D& fetch,
                          const GuestTexture& texture)
{
    const size_t index = g_guestTextures.size();
    g_guestTextures.push_back(texture);
    g_guestTextureLookup.emplace(HashGuestTextureKey(fetch), index);
}

uint64_t HashGuestTextureBytes(const uint8_t* data, size_t bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Content-coherency hashing sits directly on the textured-draw hot path.  The
// diagnostic FNV hash above is intentionally stable because some opt-in probes
// expose its values, but using one multiply per *byte* just to tell whether a
// cached guest texture changed is unnecessarily expensive in draw-heavy scenes.
// Keep a separate 64-bit source hash that consumes eight bytes at a time.  It is
// only compared against hashes produced by this same function during the current
// process, so no persisted/debug hash ABI changes here.
uint64_t HashGuestTextureSourceBytes(const uint8_t* data, size_t bytes)
{
    // xxHash64-style mixing is a much better fit for this job than the old
    // per-8-byte avalanche chain.  The previous hash serialized three 64-bit
    // multiplies for every lane, which became ~25 ms/frame once gameplay was
    // touching ~25 MiB of otherwise unchanged texture memory.  Four independent
    // 32-byte accumulators keep the CPU pipelines busy while preserving a robust
    // 64-bit content fingerprint.  This value is process-local only; no cache or
    // file format depends on the exact hash value.
    constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
    constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
    constexpr uint64_t kPrime3 = 0x165667B19E3779F9ull;
    constexpr uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
    constexpr uint64_t kPrime5 = 0x27D4EB2F165667C5ull;
    constexpr uint64_t kSeed = 0xD6E8FEB86659FD93ull;

    auto rotl = [](uint64_t value, uint32_t bits) {
        return (value << bits) | (value >> (64u - bits));
    };
    auto read64 = [](const uint8_t* p) {
        uint64_t value = 0;
        std::memcpy(&value, p, sizeof(value));
        return value;
    };
    auto read32 = [](const uint8_t* p) {
        uint32_t value = 0;
        std::memcpy(&value, p, sizeof(value));
        return value;
    };
    auto round = [&](uint64_t acc, uint64_t lane) {
        acc += lane * kPrime2;
        acc = rotl(acc, 31);
        acc *= kPrime1;
        return acc;
    };
    auto mergeRound = [&](uint64_t acc, uint64_t lane) {
        acc ^= round(0, lane);
        acc = acc * kPrime1 + kPrime4;
        return acc;
    };

    const uint8_t* p = data;
    const uint8_t* const end = data + bytes;
    uint64_t hash = 0;
    if (bytes >= 32)
    {
        uint64_t v1 = kSeed + kPrime1 + kPrime2;
        uint64_t v2 = kSeed + kPrime2;
        uint64_t v3 = kSeed;
        uint64_t v4 = kSeed - kPrime1;
        const uint8_t* const limit = end - 32;
        do
        {
            v1 = round(v1, read64(p)); p += 8;
            v2 = round(v2, read64(p)); p += 8;
            v3 = round(v3, read64(p)); p += 8;
            v4 = round(v4, read64(p)); p += 8;
        } while (p <= limit);
        hash = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        hash = mergeRound(hash, v1);
        hash = mergeRound(hash, v2);
        hash = mergeRound(hash, v3);
        hash = mergeRound(hash, v4);
    }
    else
    {
        hash = kSeed + kPrime5;
    }

    hash += uint64_t(bytes);
    while (p + 8 <= end)
    {
        const uint64_t lane = round(0, read64(p));
        hash ^= lane;
        hash = rotl(hash, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (p + 4 <= end)
    {
        hash ^= uint64_t(read32(p)) * kPrime1;
        hash = rotl(hash, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end)
    {
        hash ^= uint64_t(*p++) * kPrime5;
        hash = rotl(hash, 11) * kPrime1;
    }

    hash ^= hash >> 33;
    hash *= kPrime2;
    hash ^= hash >> 29;
    hash *= kPrime3;
    hash ^= hash >> 32;
    return hash;
}

struct GuestRgba8MipSource
{
    uint32_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t address = 0;
    uint32_t pitchPixels = 0;
    uint32_t offsetX = 0;
    uint32_t offsetY = 0;
};

bool BuildGuestRgba8MipSources(uint8_t* guestBase,
                               const mojorecomp::texture_abi::Fetch2D& fetch,
                               uint32_t hostMipLevels,
                               std::array<GuestRgba8MipSource, 16>& sources,
                               uint32_t& sourceCount, bool& authoredMips,
                               uint64_t& sourceHash, size_t& visibleBytes)
{
    sourceCount = 0;
    authoredMips = false;
    sourceHash = 0x6A09E667F3BCC909ull;
    visibleBytes = 0;
    if (!guestBase || !hostMipLevels || hostMipLevels > sources.size())
        return false;

    std::array<mojorecomp::texture_abi::LinearRgba8MipLayout, 16> layout{};
    uint32_t layoutCount = 0;
    if (!mojorecomp::texture_abi::BuildLinearRgba8MipLayout(
            fetch, hostMipLevels, layout, layoutCount, authoredMips))
        return false;
    const uint32_t baseAddress = PhysicalToCached(fetch.key);
    const uint32_t mipBase = authoredMips ? PhysicalToCached(fetch.mipKey) : 0u;
    for (uint32_t i = 0; i < layoutCount; ++i)
    {
        const auto& mip = layout[i];
        const uint32_t addressBase = mip.mipBacking ? mipBase : baseAddress;
        const uint64_t address = uint64_t(addressBase) + mip.byteOffset;
        if (address > UINT32_MAX)
            return false;
        sources[sourceCount++] = {mip.level, mip.width, mip.height,
                                  static_cast<uint32_t>(address), mip.pitchPixels,
                                  mip.offsetX, mip.offsetY};
    }

    // Hash visible texels only. Padding belongs to the allocation layout, not to
    // the sampled image, and packed mips share the same padded tail tile.
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        const auto& mip = sources[i];
        const size_t rowBytes = size_t(mip.width) * 4u;
        sourceHash ^= (uint64_t(mip.level) << 56) ^
                      (uint64_t(mip.width) << 28) ^ uint64_t(mip.height);
        if (mip.offsetX == 0u && mip.offsetY == 0u &&
            mip.pitchPixels == mip.width)
        {
            const uint64_t bytes64 = uint64_t(rowBytes) * mip.height;
            if (bytes64 > SIZE_MAX || !GuestRangeOk(mip.address, bytes64))
                return false;
            const size_t bytes = static_cast<size_t>(bytes64);
            const uint64_t levelHash = HashGuestTextureSourceBytes(
                guestBase + mip.address, bytes);
            sourceHash ^= levelHash + 0x9E3779B97F4A7C15ull +
                          (sourceHash << 6) + (sourceHash >> 2);
            visibleBytes += bytes;
            continue;
        }
        for (uint32_t y = 0; y < mip.height; ++y)
        {
            const uint64_t rowAddress = uint64_t(mip.address) +
                (uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX) * 4u;
            if (rowAddress > UINT32_MAX ||
                !GuestRangeOk(uint32_t(rowAddress), rowBytes))
                return false;
            const uint64_t rowHash = HashGuestTextureSourceBytes(
                guestBase + uint32_t(rowAddress), rowBytes);
            sourceHash ^= rowHash + 0x9E3779B97F4A7C15ull +
                          (sourceHash << 6) + (sourceHash >> 2);
            visibleBytes += rowBytes;
        }
    }
    return true;
}

bool StageGuestRgba8MipSources(uint8_t* guestBase,
                               const std::array<GuestRgba8MipSource, 16>& sources,
                               uint32_t sourceCount, uint32_t endian,
                               VkDeviceSize& uploadAt,
                               std::array<VkBufferImageCopy, 16>& regions,
                               size_t& stagedBytes)
{
    stagedBytes = 0;
    if (!guestBase || !sourceCount || sourceCount > sources.size())
        return false;
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        const uint64_t bytes = uint64_t(sources[i].width) * sources[i].height * 4u;
        if (bytes > SIZE_MAX - stagedBytes)
            return false;
        stagedBytes += static_cast<size_t>(bytes);
    }

    uploadAt = UploadAlloc(stagedBytes, 16);
    if (uploadAt == VK_WHOLE_SIZE)
        return false;

    size_t tightOffset = 0;
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        const auto& mip = sources[i];
        const size_t rowBytes = size_t(mip.width) * 4u;
        for (uint32_t y = 0; y < mip.height; ++y)
        {
            const uint64_t rowAddress = uint64_t(mip.address) +
                (uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX) * 4u;
            if (rowAddress > UINT32_MAX ||
                !GuestRangeOk(uint32_t(rowAddress), rowBytes))
                return false;
            CopySwapped(g_uploadMapped + uploadAt + tightOffset + uint64_t(y) * rowBytes,
                        guestBase + uint32_t(rowAddress), rowBytes, endian);
        }

        VkBufferImageCopy copy{};
        copy.bufferOffset = uploadAt + tightOffset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip.level, 0, 1};
        copy.imageExtent = {mip.width, mip.height, 1};
        regions[i] = copy;
        tightOffset += rowBytes * mip.height;
    }
    return true;
}

bool UpdateGuestR8Texture(GuestTexture& texture, const uint8_t* source, uint32_t sourcePitch,
                          size_t byteCount, uint64_t sourceHash)
{
    if (texture.sourceHash == sourceHash)
        return true;

    if (texture.width < 640 || texture.height < 360)
    {
        static std::atomic<uint32_t> smallDynamicReports{0};
        const uint32_t report = smallDynamicReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 64)
            KLOG_DIAG("Vulkan dynamic small R8 refresh: key=%08X size=%ux%u old=%016llX new=%016llX frame=%llu slot=%u\n",
                      texture.key, texture.width, texture.height,
                      static_cast<unsigned long long>(texture.sourceHash),
                      static_cast<unsigned long long>(sourceHash),
                      static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                      g_frameSlot);

        // Optional UI/font-atlas evidence. Keep this completely opt-in: normal
        // runs do no filesystem work. R8 textures are written as portable PGM
        // files so their alpha/luma contents can be inspected directly.
        static std::vector<uint32_t> dumpedUiKeys;
        const char* uiDumpDir = std::getenv("MOJORECOMP_R8_DUMP_DIR");
        if (uiDumpDir && *uiDumpDir && texture.width <= 512 && texture.height <= 512 &&
            std::find(dumpedUiKeys.begin(), dumpedUiKeys.end(), texture.key) == dumpedUiKeys.end())
        {
            std::error_code ec;
            std::filesystem::create_directories(uiDumpDir, ec);
            if (!ec)
            {
                char name[96]{};
                std::snprintf(name, sizeof(name), "r8-%08X-%ux%u.pgm",
                              texture.key, texture.width, texture.height);
                const auto path = std::filesystem::path(uiDumpDir) / name;
                std::ofstream pgm(path, std::ios::binary);
                if (pgm)
                {
                    pgm << "P5\n" << texture.width << " " << texture.height << "\n255\n";
                    for (uint32_t y = 0; y < texture.height; ++y)
                        pgm.write(reinterpret_cast<const char*>(source + uint64_t(y) * sourcePitch),
                                  std::streamsize(texture.width));
                    dumpedUiKeys.push_back(texture.key);
                    KLOG("Vulkan UI R8 dumped: key=%08X frame=%llu path=%s\n",
                         texture.key,
                         static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                         path.string().c_str());
                }
            }
        }
    }

    if (MojoRecompVerboseDiagnosticsEnabled() &&
        texture.width >= 640 && texture.height >= 360)
    {
        static uint32_t dynamicR8Reports = 0;
        if (dynamicR8Reports++ < 24)
        {
            uint8_t minValue = 255, maxValue = 0;
            uint64_t nonZero = 0;
            for (uint32_t y = 0; y < texture.height; ++y)
            {
                const uint8_t* row = source + uint64_t(y) * sourcePitch;
                for (uint32_t x = 0; x < texture.width; ++x)
                {
                    minValue = std::min(minValue, row[x]);
                    maxValue = std::max(maxValue, row[x]);
                    nonZero += row[x] != 0;
                }
            }
            KLOG_DIAG("Vulkan dynamic R8 refresh: key=%08X size=%ux%u old=%016llX new=%016llX "
                      "min=%u max=%u nonzero=%llu/%zu\n",
                      texture.key, texture.width, texture.height,
                      static_cast<unsigned long long>(texture.sourceHash),
                      static_cast<unsigned long long>(sourceHash), minValue, maxValue,
                      static_cast<unsigned long long>(nonZero), byteCount);
        }

        static std::vector<uint32_t> dumpedKeys;
        const char* dumpDir = std::getenv("MOJORECOMP_BINK_PLANE_DUMP_DIR");
        if (dumpDir && *dumpDir &&
            std::find(dumpedKeys.begin(), dumpedKeys.end(), texture.key) == dumpedKeys.end())
        {
            std::error_code ec;
            std::filesystem::create_directories(dumpDir, ec);
            if (!ec)
            {
                const auto path = std::filesystem::path(dumpDir) /
                    ("plane-" + std::to_string(texture.key) + "-" +
                     std::to_string(texture.width) + "x" + std::to_string(texture.height) + ".pgm");
                std::ofstream pgm(path, std::ios::binary);
                if (pgm)
                {
                    pgm << "P5\n" << texture.width << " " << texture.height << "\n255\n";
                    for (uint32_t y = 0; y < texture.height; ++y)
                        pgm.write(reinterpret_cast<const char*>(source + uint64_t(y) * sourcePitch),
                                  std::streamsize(texture.width));
                    dumpedKeys.push_back(texture.key);
                    KLOG("Vulkan Bink plane dumped: key=%08X path=%s\n",
                         texture.key, path.string().c_str());
                }
            }
        }
    }

    const VkDeviceSize uploadAt = UploadAlloc(byteCount, 16);
    if (uploadAt == VK_WHOLE_SIZE)
        return false;
    uint8_t* upload = g_uploadMapped + uploadAt;
    for (uint32_t y = 0; y < texture.height; ++y)
        std::memcpy(upload + uint64_t(y) * texture.width,
                    source + uint64_t(y) * sourcePitch, texture.width);

    // Transfer commands are not valid inside an active dynamic-rendering
    // instance. Keep the cache-hit path above break-free, but pause rendering
    // when this guest texture actually needs to be refreshed.
    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = texture.layout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy copy{};
    copy.bufferOffset = uploadAt;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {texture.width, texture.height, 1};
    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    g_perfTextureRefreshBytesR8 += byteCount;
    ++g_perfTextureRefreshCount;

    VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.image = texture.image;
    toSample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toSample);
    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture.sourceHash = sourceHash;
    if (resumeRendering)
        ResumeColorRendering();
    return true;
}

bool UpdateGuestR8MipTexture(
    GuestTexture& texture, uint8_t* guestBase,
    const mojorecomp::texture_abi::Fetch2D& fetch,
    const std::array<mojorecomp::texture_abi::LinearRgba8MipLayout, 16>& layout,
    uint32_t sourceCount, bool authoredMips, uint32_t hostMipLevels,
    size_t stagedBytes, uint64_t sourceHash)
{
    if (texture.sourceHash == sourceHash)
        return true;

    const VkDeviceSize uploadAt = UploadAlloc(stagedBytes, 16);
    if (uploadAt == VK_WHOLE_SIZE)
        return false;
    const uint32_t baseAddress = PhysicalToCached(fetch.key);
    const uint32_t mipBase = authoredMips ? PhysicalToCached(fetch.mipKey) : 0u;
    std::array<VkBufferImageCopy, 16> copies{};
    size_t tightOffset = 0;
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        const auto& mip = layout[i];
        const uint32_t addressBase = mip.mipBacking ? mipBase : baseAddress;
        const uint64_t address = uint64_t(addressBase) + mip.byteOffset;
        for (uint32_t y = 0; y < mip.height; ++y)
        {
            const uint64_t rowAddress = address +
                uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX;
            std::memcpy(g_uploadMapped + uploadAt + tightOffset + uint64_t(y) * mip.width,
                        guestBase + static_cast<uint32_t>(rowAddress), mip.width);
        }
        copies[i].bufferOffset = uploadAt + tightOffset;
        copies[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip.level, 0, 1};
        copies[i].imageExtent = {mip.width, mip.height, 1};
        tightOffset += size_t(mip.width) * mip.height;
    }

    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = texture.layout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &toTransfer);
    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             sourceCount, copies.data());

    if (!authoredMips)
    {
        uint32_t mipWidth = texture.width;
        uint32_t mipHeight = texture.height;
        for (uint32_t level = 1; level < hostMipLevels; ++level)
        {
            VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSource.srcQueueFamilyIndex = toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSource.image = texture.image;
            toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1, 0, 1};
            p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                   1, &toSource);
            const uint32_t nextWidth = std::max(1u, mipWidth >> 1);
            const uint32_t nextHeight = std::max(1u, mipHeight >> 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(mipWidth),
                                  static_cast<int32_t>(mipHeight), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(nextWidth),
                                  static_cast<int32_t>(nextHeight), 1};
            p_vkCmdBlitImage(g_commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &blit, VK_FILTER_LINEAR);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }

    std::array<VkImageMemoryBarrier, 2> toSample{};
    uint32_t toSampleCount = 0;
    if (authoredMips || hostMipLevels == 1u)
    {
        auto& direct = toSample[toSampleCount++];
        direct.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        direct.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        direct.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        direct.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        direct.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        direct.srcQueueFamilyIndex = direct.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        direct.image = texture.image;
        direct.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    }
    else
    {
        auto& generated = toSample[toSampleCount++];
        generated.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        generated.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        generated.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        generated.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        generated.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        generated.srcQueueFamilyIndex = generated.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        generated.image = texture.image;
        generated.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels - 1u, 0, 1};
        auto& last = toSample[toSampleCount++];
        last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        last.srcQueueFamilyIndex = last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        last.image = texture.image;
        last.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, hostMipLevels - 1u, 1, 0, 1};
    }
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, toSampleCount, toSample.data());
    g_perfTextureRefreshBytesR8 += stagedBytes;
    ++g_perfTextureRefreshCount;
    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture.sourceHash = sourceHash;
    if (resumeRendering)
        ResumeColorRendering();
    return true;
}

GuestTexture* CreateGuestR8Texture(uint8_t* guestBase,
                                   const mojorecomp::texture_abi::Fetch2D& fetch)
{
    if (!guestBase || fetch.format != 2 || fetch.dimension != 1 ||
        fetch.type != 2 || fetch.tiled || fetch.endian ||
        fetch.mipMin ||
        fetch.width == 0 || fetch.height == 0 ||
        fetch.width > 4096 || fetch.height > 4096)
        return nullptr;

    // The first real Crash frontend R8 fetches are RRR1. Keep this path strict
    // until additional swizzles are observed rather than silently approximating.
    if (fetch.swizzle != 0xA00u)
        return nullptr;

    if (MojoRecompVerboseDiagnosticsEnabled() &&
        fetch.width >= 640 && fetch.height >= 360)
    {
        static uint32_t largeR8Reports = 0;
        if (largeR8Reports++ < 12)
            KLOG_DIAG("Vulkan large R8 fetch: key=%08X size=%ux%u pitch=%u tiled=%u endian=%u filter=%u/%u clamp=%u/%u\n",
                      fetch.key, fetch.width, fetch.height, fetch.pitch, fetch.tiled ? 1u : 0u,
                      fetch.endian, fetch.minFilter, fetch.magFilter, fetch.clampX, fetch.clampY);
    }

    uint32_t geometricMipMax = 0;
    for (uint32_t w = fetch.width, h = fetch.height;
         (w > 1u || h > 1u) && geometricMipMax < 15u; ++geometricMipMax)
    {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
    }
    const uint32_t hostMipLevels = std::min(fetch.mipMax, geometricMipMax) + 1u;
    GuestTexture* existing = FindGuestTexture(fetch);
    const uint64_t currentFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (existing && existing->sourceCheckFrame == currentFrame)
        return existing;
    std::array<mojorecomp::texture_abi::LinearRgba8MipLayout, 16> layout{};
    uint32_t sourceCount = 0;
    bool authoredMips = false;
    if (!mojorecomp::texture_abi::BuildLinearR8MipLayout(
            fetch, hostMipLevels, layout, sourceCount, authoredMips))
        return nullptr;

    const uint32_t baseAddress = PhysicalToCached(fetch.key);
    const uint32_t mipBase = authoredMips ? PhysicalToCached(fetch.mipKey) : 0u;
    uint64_t sourceHash = 0xBB67AE8584CAA73Bull;
    size_t visibleSourceBytes = 0;
    size_t stagedBytes = 0;
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        const auto& mip = layout[i];
        const uint32_t addressBase = mip.mipBacking ? mipBase : baseAddress;
        const uint64_t address = uint64_t(addressBase) + mip.byteOffset;
        const size_t rowBytes = mip.width;
        if (address > UINT32_MAX || rowBytes > SIZE_MAX - stagedBytes ||
            uint64_t(rowBytes) * mip.height > SIZE_MAX - stagedBytes)
            return nullptr;
        stagedBytes += rowBytes * mip.height;
        sourceHash ^= (uint64_t(mip.level) << 56) ^
                      (uint64_t(mip.width) << 28) ^ uint64_t(mip.height);
        for (uint32_t y = 0; y < mip.height; ++y)
        {
            const uint64_t rowAddress = address +
                uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX;
            if (rowAddress > UINT32_MAX ||
                !GuestRangeOk(static_cast<uint32_t>(rowAddress), rowBytes))
                return nullptr;
            const uint64_t rowHash = HashGuestTextureSourceBytes(
                guestBase + static_cast<uint32_t>(rowAddress), rowBytes);
            sourceHash ^= rowHash + 0x9E3779B97F4A7C15ull +
                          (sourceHash << 6) + (sourceHash >> 2);
            visibleSourceBytes += rowBytes;
        }
    }
    g_perfTextureHashBytesR8 += visibleSourceBytes;
    ++g_perfTextureHashCallsR8;

    if (existing)
    {
        if (hostMipLevels == 1u)
        {
            const uint32_t sourcePitch = fetch.pitch ? fetch.pitch : fetch.width;
            const uint8_t* source = guestBase + baseAddress;
            const size_t byteCount = size_t(fetch.width) * fetch.height;
            if (!UpdateGuestR8Texture(*existing, source, sourcePitch, byteCount, sourceHash))
                return nullptr;
        }
        else if (!UpdateGuestR8MipTexture(*existing, guestBase, fetch, layout,
                                          sourceCount, authoredMips, hostMipLevels,
                                          stagedBytes, sourceHash))
        {
            return nullptr;
        }
        existing->sourceCheckFrame = currentFrame;
        return existing;
    }

    const VkDeviceSize uploadAt = UploadAlloc(stagedBytes, 16);
    if (uploadAt == VK_WHOLE_SIZE)
    {
        static uint32_t r8UploadFailReports = 0;
        if (r8UploadFailReports++ < 8)
            KLOG("Vulkan guest R8 upload arena exhausted: key=%08X size=%ux%u bytes=%zu at=%llu limit=%llu\n",
                 fetch.key, fetch.width, fetch.height, stagedBytes,
                 static_cast<unsigned long long>(g_uploadAt),
                 static_cast<unsigned long long>(g_uploadLimit));
        return nullptr;
    }
    std::array<VkBufferImageCopy, 16> copies{};
    size_t tightOffset = 0;
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        const auto& mip = layout[i];
        const uint32_t addressBase = mip.mipBacking ? mipBase : baseAddress;
        const uint64_t address = uint64_t(addressBase) + mip.byteOffset;
        for (uint32_t y = 0; y < mip.height; ++y)
        {
            const uint64_t rowAddress = address +
                uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX;
            std::memcpy(g_uploadMapped + uploadAt + tightOffset + uint64_t(y) * mip.width,
                        guestBase + static_cast<uint32_t>(rowAddress), mip.width);
        }
        copies[i].bufferOffset = uploadAt + tightOffset;
        copies[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip.level, 0, 1};
        copies[i].imageExtent = {mip.width, mip.height, 1};
        tightOffset += size_t(mip.width) * mip.height;
    }

    GuestTexture texture{};
    texture.key = fetch.key;
    texture.width = fetch.width;
    texture.height = fetch.height;
    texture.pitch = fetch.pitch ? fetch.pitch : fetch.width;
    texture.format = fetch.format;
    texture.swizzle = fetch.swizzle;
    texture.endian = fetch.endian;
    texture.mipKey = fetch.mipMax ? fetch.mipKey : 0u;
    texture.mipMax = fetch.mipMax;
    texture.packedMips = fetch.mipMax && fetch.packedMips;
    texture.sourceCheckFrame = currentFrame;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8_UNORM;
    ii.extent = {fetch.width, fetch.height, 1};
    ii.mipLevels = hostMipLevels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (hostMipLevels > 1u && !authoredMips)
        ii.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &texture.image) != VK_SUCCESS)
        return nullptr;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, texture.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, texture.image, nullptr);
        return nullptr;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &texture.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        if (texture.memory) p_vkFreeMemory(g_device, texture.memory, nullptr);
        p_vkDestroyImage(g_device, texture.image, nullptr);
        return nullptr;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = texture.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                     VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &texture.view) != VK_SUCCESS)
    {
        p_vkDestroyImage(g_device, texture.image, nullptr);
        p_vkFreeMemory(g_device, texture.memory, nullptr);
        return nullptr;
    }

    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &toTransfer);

    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             sourceCount, copies.data());
    g_perfTextureUploadBytesR8 += stagedBytes;
    ++g_perfTextureUploadCount;

    if (!authoredMips)
    {
        uint32_t mipWidth = fetch.width;
        uint32_t mipHeight = fetch.height;
        for (uint32_t level = 1; level < hostMipLevels; ++level)
        {
            VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSource.srcQueueFamilyIndex = toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSource.image = texture.image;
            toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1, 0, 1};
            p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                   1, &toSource);
            const uint32_t nextWidth = std::max(1u, mipWidth >> 1);
            const uint32_t nextHeight = std::max(1u, mipHeight >> 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(mipWidth),
                                  static_cast<int32_t>(mipHeight), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(nextWidth),
                                  static_cast<int32_t>(nextHeight), 1};
            p_vkCmdBlitImage(g_commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &blit, VK_FILTER_LINEAR);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }

    std::array<VkImageMemoryBarrier, 2> toSample{};
    uint32_t toSampleCount = 0;
    if (authoredMips || hostMipLevels == 1u)
    {
        auto& direct = toSample[toSampleCount++];
        direct.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        direct.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        direct.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        direct.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        direct.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        direct.srcQueueFamilyIndex = direct.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        direct.image = texture.image;
        direct.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    }
    else
    {
        auto& generated = toSample[toSampleCount++];
        generated.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        generated.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        generated.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        generated.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        generated.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        generated.srcQueueFamilyIndex = generated.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        generated.image = texture.image;
        generated.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels - 1u, 0, 1};
        auto& last = toSample[toSampleCount++];
        last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        last.srcQueueFamilyIndex = last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        last.image = texture.image;
        last.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, hostMipLevels - 1u, 1, 0, 1};
    }
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, toSampleCount, toSample.data());
    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture.sourceHash = sourceHash;
    if (resumeRendering)
        ResumeColorRendering();

    RememberGuestTexture(fetch, texture);
    KLOG_DIAG("Vulkan guest texture uploaded: base=%08X format=R8 size=%ux%u "
              "levels=%u authored=%u swizzle=RRR1\n",
              fetch.key, fetch.width, fetch.height, hostMipLevels,
              authoredMips ? 1u : 0u);
    return &g_guestTextures.back();
}

void CopySwapped(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian);

bool UpdateGuestRGBA8Texture(
    GuestTexture& texture, uint8_t* guestBase,
    const mojorecomp::texture_abi::Fetch2D& fetch,
    const std::array<GuestRgba8MipSource, 16>& mipSources,
    uint32_t mipSourceCount, bool authoredMips, uint32_t hostMipLevels,
    uint64_t sourceHash)
{
    if (texture.sourceHash == sourceHash)
        return true;

    VkDeviceSize uploadAt = VK_WHOLE_SIZE;
    std::array<VkBufferImageCopy, 16> copies{};
    size_t stagedBytes = 0;
    if (!StageGuestRgba8MipSources(guestBase, mipSources, mipSourceCount,
                                   texture.endian, uploadAt, copies, stagedBytes))
        return false;

    // Guest allocations may be recycled for another texture while retaining the
    // same fetch descriptor. Keep the Vulkan image coherent with guest memory.
    // Transfer commands are invalid inside dynamic rendering, so temporarily
    // leave the render pass only when the contents actually changed.
    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = texture.layout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             mipSourceCount, copies.data());
    g_perfTextureRefreshBytesRGBA += stagedBytes;
    ++g_perfTextureRefreshCount;

    if (!authoredMips)
    {
        uint32_t mipWidth = texture.width;
        uint32_t mipHeight = texture.height;
        for (uint32_t level = 1; level < hostMipLevels; ++level)
        {
            VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSource.srcQueueFamilyIndex = toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSource.image = texture.image;
            toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1, 0, 1};
            p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                   1, &toSource);

            const uint32_t nextWidth = std::max(1u, mipWidth >> 1);
            const uint32_t nextHeight = std::max(1u, mipHeight >> 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(mipWidth),
                                  static_cast<int32_t>(mipHeight), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(nextWidth),
                                  static_cast<int32_t>(nextHeight), 1};
            p_vkCmdBlitImage(g_commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &blit, VK_FILTER_LINEAR);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }

    std::array<VkImageMemoryBarrier, 2> toSample{};
    uint32_t toSampleCount = 0;
    if (authoredMips || hostMipLevels == 1u)
    {
        auto& direct = toSample[toSampleCount++];
        direct.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        direct.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        direct.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        direct.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        direct.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        direct.srcQueueFamilyIndex = direct.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        direct.image = texture.image;
        direct.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    }
    else if (hostMipLevels > 1u)
    {
        auto& generated = toSample[toSampleCount++];
        generated.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        generated.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        generated.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        generated.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        generated.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        generated.srcQueueFamilyIndex = generated.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        generated.image = texture.image;
        generated.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels - 1u, 0, 1};

        auto& last = toSample[toSampleCount++];
        last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        last.srcQueueFamilyIndex = last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        last.image = texture.image;
        last.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, hostMipLevels - 1u, 1, 0, 1};
    }
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, toSampleCount, toSample.data());

    if (texture.width == 256 && texture.height == 1)
    {
        static std::atomic<uint32_t> lutRefreshReports{0};
        const uint32_t report = lutRefreshReports.fetch_add(1, std::memory_order_relaxed);
        if (report < 96)
            KLOG_DIAG("Vulkan frontend LUT refresh: key=%08X old=%016llX new=%016llX frame=%llu slot=%u\n",
                      texture.key,
                      static_cast<unsigned long long>(texture.sourceHash),
                      static_cast<unsigned long long>(sourceHash),
                      static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                      g_frameSlot);
    }

    if (authoredMips && MojoRecompVerboseDiagnosticsEnabled())
        KLOG_DIAG("Vulkan authored RGBA8 mips refreshed: key=%08X mip=%08X levels=%u bytes=%zu\n",
                  fetch.key, fetch.mipKey, hostMipLevels, stagedBytes);

    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture.sourceHash = sourceHash;
    if (resumeRendering)
        ResumeColorRendering();
    return true;
}

GuestTexture* CreateGuestRGBA8Texture(uint8_t* guestBase,
                                      const mojorecomp::texture_abi::Fetch2D& fetch)
{
    if (!guestBase || fetch.format != 6 || fetch.dimension != 1 ||
        fetch.type != 2 || fetch.tiled || fetch.mipMin ||
        fetch.width == 0 || fetch.height == 0 ||
        fetch.width > 4096 || fetch.height > 4096)
    {
        static uint32_t rgbaRejectReports = 0;
        if (rgbaRejectReports++ < 8)
        {
            KLOG("Vulkan guest RGBA8 rejected before upload: key=%08X guest=%u fmt=%u dim=%u type=%u tiled=%u mips=%u..%u size=%ux%u\n",
                 fetch.key, guestBase ? 1u : 0u, fetch.format, fetch.dimension,
                 fetch.type, fetch.tiled ? 1u : 0u, fetch.mipMin, fetch.mipMax,
                 fetch.width, fetch.height);
            if (guestBase && fetch.tiled && fetch.width && fetch.height)
            {
                const uint32_t guestAddress = PhysicalToCached(fetch.key);
                const uint64_t bytes = uint64_t(fetch.width) * fetch.height * 4u;
                if (GuestRangeOk(guestAddress, bytes))
                {
                    const size_t probeBytes = static_cast<size_t>(std::min<uint64_t>(bytes, 4096));
                    size_t nonZero = 0;
                    uint64_t hash = 1469598103934665603ull;
                    const uint8_t* src = guestBase + guestAddress;
                    for (size_t i = 0; i < probeBytes; ++i)
                    {
                        nonZero += src[i] != 0;
                        hash ^= src[i];
                        hash *= 1099511628211ull;
                    }
                    KLOG("Vulkan guest tiled initial bytes: key=%08X probe=%zu nonzero=%zu hash=%016llX\n",
                         fetch.key, probeBytes, nonZero,
                         static_cast<unsigned long long>(hash));
                }
            }
        }
        return nullptr;
    }

    // Observed frontend lookup textures use BGRA fetch swizzle with 8-in-32
    // endian conversion. Snapshot textures continue through the resolve path.
    if (fetch.swizzle != 0x60Au || fetch.endian != 2)
    {
        static uint32_t rgbaLayoutRejectReports = 0;
        if (rgbaLayoutRejectReports++ < 8)
            KLOG("Vulkan guest RGBA8 layout rejected: key=%08X swizzle=%03X endian=%u\n",
                 fetch.key, fetch.swizzle, fetch.endian);
        return nullptr;
    }

    GuestTexture* existing = FindGuestTexture(fetch);
    const uint64_t currentFrame = g_frames.load(std::memory_order_relaxed) + 1;
    const uint32_t effectivePitch = fetch.pitch ? fetch.pitch : fetch.width;
    const uint32_t effectiveMipKey = fetch.mipMax ? fetch.mipKey : 0u;
    const bool effectivePackedMips = fetch.mipMax && fetch.packedMips;
    // A texture can be referenced by many draws in the same frame. Hash its guest
    // backing at most once per frame, while still detecting allocation reuse or
    // CPU-written texture updates before the next frame samples it.
    if (existing && existing->sourceCheckFrame == currentFrame &&
        existing->pitch == effectivePitch && existing->mipKey == effectiveMipKey &&
        existing->packedMips == effectivePackedMips)
        return existing;

    uint32_t geometricMipMax = 0;
    for (uint32_t w = fetch.width, h = fetch.height;
         (w > 1u || h > 1u) && geometricMipMax < 15u; ++geometricMipMax)
    {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
    }
    const uint32_t hostMipMax = std::min(fetch.mipMax, geometricMipMax);
    const uint32_t hostMipLevels = hostMipMax + 1u;

    std::array<GuestRgba8MipSource, 16> mipSources{};
    uint32_t mipSourceCount = 0;
    bool authoredMips = false;
    uint64_t sourceHash = 0;
    size_t visibleSourceBytes = 0;
    if (!BuildGuestRgba8MipSources(guestBase, fetch, hostMipLevels, mipSources,
                                   mipSourceCount, authoredMips, sourceHash,
                                   visibleSourceBytes))
        return nullptr;
    g_perfTextureHashBytesRGBA += visibleSourceBytes;
    ++g_perfTextureHashCallsRGBA;
    if (authoredMips)
    {
        g_perfTextureHashBytesRGBAAuthored += visibleSourceBytes;
        ++g_perfTextureHashCallsRGBAAuthored;
    }
    else
    {
        g_perfTextureHashBytesRGBAGenerated += visibleSourceBytes;
        ++g_perfTextureHashCallsRGBAGenerated;
    }
    if (existing)
    {
        if (!UpdateGuestRGBA8Texture(*existing, guestBase, fetch, mipSources,
                                     mipSourceCount, authoredMips, hostMipLevels,
                                     sourceHash))
            return nullptr;
        existing->pitch = effectivePitch;
        existing->mipKey = effectiveMipKey;
        existing->packedMips = effectivePackedMips;
        existing->sourceCheckFrame = currentFrame;
        return existing;
    }

    VkDeviceSize uploadAt = VK_WHOLE_SIZE;
    std::array<VkBufferImageCopy, 16> copies{};
    size_t stagedBytes = 0;
    if (!StageGuestRgba8MipSources(guestBase, mipSources, mipSourceCount,
                                   fetch.endian, uploadAt, copies, stagedBytes))
        return nullptr;

    GuestTexture texture{};
    texture.key = fetch.key;
    texture.mipKey = effectiveMipKey;
    texture.width = fetch.width;
    texture.height = fetch.height;
    texture.pitch = effectivePitch;
    texture.format = fetch.format;
    texture.swizzle = fetch.swizzle;
    texture.endian = fetch.endian;
    texture.mipMax = fetch.mipMax;
    texture.packedMips = effectivePackedMips;
    texture.sourceHash = sourceHash;
    texture.sourceCheckFrame = currentFrame;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {fetch.width, fetch.height, 1};
    ii.mipLevels = hostMipLevels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (hostMipLevels > 1u)
        ii.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &texture.image) != VK_SUCCESS)
        return nullptr;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, texture.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, texture.image, nullptr);
        return nullptr;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &texture.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        if (texture.memory) p_vkFreeMemory(g_device, texture.memory, nullptr);
        p_vkDestroyImage(g_device, texture.image, nullptr);
        return nullptr;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = texture.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
                     VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &texture.view) != VK_SUCCESS)
    {
        p_vkDestroyImage(g_device, texture.image, nullptr);
        p_vkFreeMemory(g_device, texture.memory, nullptr);
        return nullptr;
    }

    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &toTransfer);
    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             mipSourceCount, copies.data());
    g_perfTextureUploadBytesRGBA += stagedBytes;
    ++g_perfTextureUploadCount;

    // Prefer authored guest mips whenever the fetch provides a separate mip
    // backing. Only synthesize a chain for descriptors with no guest mip data.
    if (!authoredMips)
    {
        uint32_t mipWidth = fetch.width;
        uint32_t mipHeight = fetch.height;
        for (uint32_t level = 1; level < hostMipLevels; ++level)
        {
            VkImageMemoryBarrier toSource{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSource.srcQueueFamilyIndex = toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSource.image = texture.image;
            toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1, 0, 1};
            p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                   1, &toSource);

            const uint32_t nextWidth = std::max(1u, mipWidth >> 1);
            const uint32_t nextHeight = std::max(1u, mipHeight >> 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(mipWidth),
                                  static_cast<int32_t>(mipHeight), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(nextWidth),
                                  static_cast<int32_t>(nextHeight), 1};
            p_vkCmdBlitImage(g_commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &blit, VK_FILTER_LINEAR);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }

    std::array<VkImageMemoryBarrier, 2> toSample{};
    uint32_t toSampleCount = 0;
    if (authoredMips || hostMipLevels == 1u)
    {
        auto& direct = toSample[toSampleCount++];
        direct.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        direct.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        direct.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        direct.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        direct.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        direct.srcQueueFamilyIndex = direct.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        direct.image = texture.image;
        direct.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels, 0, 1};
    }
    else if (hostMipLevels > 1u)
    {
        auto& generated = toSample[toSampleCount++];
        generated.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        generated.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        generated.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        generated.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        generated.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        generated.srcQueueFamilyIndex = generated.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        generated.image = texture.image;
        generated.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hostMipLevels - 1u, 0, 1};

        auto& last = toSample[toSampleCount++];
        last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        last.srcQueueFamilyIndex = last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        last.image = texture.image;
        last.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, hostMipLevels - 1u, 1, 0, 1};
    }
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, toSampleCount, toSample.data());
    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (resumeRendering)
        ResumeColorRendering();
    RememberGuestTexture(fetch, texture);
    if (authoredMips)
        KLOG("Vulkan guest authored RGBA8 mips uploaded: base=%08X mip=%08X size=%ux%u levels=%u packed=%u bytes=%zu\n",
             fetch.key, fetch.mipKey, fetch.width, fetch.height, hostMipLevels,
             fetch.packedMips ? 1u : 0u, stagedBytes);
    KLOG_DIAG("Vulkan guest texture uploaded: base=%08X format=RGBA8 size=%ux%u pitch=%u "
         "mips=%u..%u hostLevels=%u source=%s endian=8in32 swizzle=BGRA guestMip=%08X packed=%u bytes=%zu\n",
         fetch.key, fetch.width, fetch.height, fetch.pitch ? fetch.pitch : fetch.width,
         fetch.mipMin, fetch.mipMax, hostMipLevels,
         authoredMips ? "guest-authored" : "generated",
         fetch.mipKey, fetch.packedMips ? 1u : 0u, stagedBytes);
    return &g_guestTextures.back();
}

bool UpdateGuestBlockCompressedTexture(GuestTexture& texture,
                                       const uint8_t* source,
                                       size_t sourceRowBytes, size_t rowBytes,
                                       uint32_t blockHeight, size_t byteCount,
                                       uint64_t sourceHash)
{
    if (texture.sourceHash == sourceHash)
        return true;

    const VkDeviceSize uploadAt = UploadAlloc(byteCount, 16);
    if (uploadAt == VK_WHOLE_SIZE)
        return false;
    for (uint32_t y = 0; y < blockHeight; ++y)
    {
        CopySwapped(g_uploadMapped + uploadAt + uint64_t(y) * rowBytes,
                    source + uint64_t(y) * sourceRowBytes,
                    rowBytes, texture.endian);
    }

    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = texture.layout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy copy{};
    copy.bufferOffset = uploadAt;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {texture.width, texture.height, 1};
    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    g_perfTextureRefreshBytesBC1 += byteCount;
    ++g_perfTextureRefreshCount;

    VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.image = texture.image;
    toSample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toSample);

    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture.sourceHash = sourceHash;
    if (resumeRendering)
        ResumeColorRendering();
    return true;
}

GuestTexture* CreateGuestBlockCompressedTexture(
    uint8_t* guestBase, const mojorecomp::texture_abi::Fetch2D& fetch)
{
    // Xenos formats 18/19/20 are DXT1, DXT2/3 and DXT4/5. Vulkan's BC1/2/3
    // block layouts are bit-compatible once the fetch endian transform has
    // been applied, so the GPU can sample them without CPU decompression.
    const uint32_t bytesPerBlock =
        mojorecomp::texture_abi::BlockCompressedBytesPerBlock(fetch.format);
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    const char* formatName = nullptr;
    switch (fetch.format)
    {
        case 18u:
            imageFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            formatName = "BC1/DXT1";
            break;
        case 19u:
            imageFormat = VK_FORMAT_BC2_UNORM_BLOCK;
            formatName = "BC2/DXT2_3";
            break;
        case 20u:
            imageFormat = VK_FORMAT_BC3_UNORM_BLOCK;
            formatName = "BC3/DXT4_5";
            break;
        default:
            break;
    }
    if (!g_textureCompressionBC || !guestBase || !bytesPerBlock ||
        imageFormat == VK_FORMAT_UNDEFINED ||
        fetch.dimension != 1 || fetch.type != 2 || fetch.tiled ||
        fetch.mipMin || fetch.mipMax || fetch.width == 0 || fetch.height == 0 ||
        fetch.width > 8192 || fetch.height > 8192 || fetch.swizzle != 0x688u)
        return nullptr;

    const uint32_t sourcePitch = fetch.pitch ? fetch.pitch : fetch.width;
    if (sourcePitch < fetch.width)
        return nullptr;
    const uint32_t blockWidth = (fetch.width + 3u) / 4u;
    const uint32_t blockHeight = (fetch.height + 3u) / 4u;
    const uint32_t sourceBlockWidth = (sourcePitch + 3u) / 4u;
    const uint64_t rowBytes64 = uint64_t(blockWidth) * bytesPerBlock;
    const uint64_t sourceRowBytes64 = uint64_t(sourceBlockWidth) * bytesPerBlock;
    const uint64_t byteCount64 = rowBytes64 * blockHeight;
    const uint64_t sourceBytes64 = sourceRowBytes64 * blockHeight;
    if (byteCount64 > SIZE_MAX)
        return nullptr;

    const uint32_t guestAddress = PhysicalToCached(fetch.key);
    if (!GuestRangeOk(guestAddress, sourceBytes64))
        return nullptr;
    const size_t byteCount = static_cast<size_t>(byteCount64);
    const uint8_t* source = guestBase + guestAddress;
    GuestTexture* existing = FindGuestTexture(fetch);
    const uint64_t currentFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (existing && existing->sourceCheckFrame == currentFrame)
        return existing;

    g_perfTextureHashBytesBC1 += sourceBytes64;
    ++g_perfTextureHashCallsBC1;
    const uint64_t sourceHash =
        HashGuestTextureSourceBytes(source, static_cast<size_t>(sourceBytes64));
    if (existing)
    {
        if (!UpdateGuestBlockCompressedTexture(
                *existing, source, static_cast<size_t>(sourceRowBytes64),
                static_cast<size_t>(rowBytes64), blockHeight, byteCount,
                sourceHash))
            return nullptr;
        existing->sourceCheckFrame = currentFrame;
        return existing;
    }

    const VkDeviceSize uploadAt = UploadAlloc(byteCount, 16);
    if (uploadAt == VK_WHOLE_SIZE)
        return nullptr;
    for (uint32_t y = 0; y < blockHeight; ++y)
    {
        CopySwapped(g_uploadMapped + uploadAt + uint64_t(y) * rowBytes64,
                    source + uint64_t(y) * sourceRowBytes64,
                    static_cast<size_t>(rowBytes64), fetch.endian);
    }

    GuestTexture texture{};
    texture.key = fetch.key;
    texture.width = fetch.width;
    texture.height = fetch.height;
    texture.format = fetch.format;
    texture.swizzle = fetch.swizzle;
    texture.endian = fetch.endian;
    texture.sourceHash = sourceHash;
    texture.sourceCheckFrame = currentFrame;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = imageFormat;
    ii.extent = {fetch.width, fetch.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &ii, nullptr, &texture.image) != VK_SUCCESS)
        return nullptr;

    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, texture.image, &req);
    const uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        p_vkDestroyImage(g_device, texture.image, nullptr);
        return nullptr;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (p_vkAllocateMemory(g_device, &ai, nullptr, &texture.memory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        if (texture.memory) p_vkFreeMemory(g_device, texture.memory, nullptr);
        p_vkDestroyImage(g_device, texture.image, nullptr);
        return nullptr;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = texture.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = imageFormat;
    vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                     VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &vi, nullptr, &texture.view) != VK_SUCCESS)
    {
        p_vkDestroyImage(g_device, texture.image, nullptr);
        p_vkFreeMemory(g_device, texture.memory, nullptr);
        return nullptr;
    }

    const bool resumeRendering = g_rendering;
    if (resumeRendering)
        EndColorRendering();

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.bufferOffset = uploadAt;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {fetch.width, fetch.height, 1};
    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    g_perfTextureUploadBytesBC1 += byteCount;
    ++g_perfTextureUploadCount;
    VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.image = texture.image;
    toSample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toSample);
    texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (resumeRendering)
        ResumeColorRendering();
    RememberGuestTexture(fetch, texture);
    KLOG_DIAG("Vulkan guest texture uploaded: base=%08X format=%s size=%ux%u pitch=%u endian=%u\n",
         fetch.key, formatName, fetch.width, fetch.height, sourcePitch, fetch.endian);
    return &g_guestTextures.back();
}

ResolveSnapshot* CreateZeroSnapshotFromGuestIfEmpty(
    uint8_t* guestBase, const mojorecomp::texture_abi::Fetch2D& fetch)
{
    if (!guestBase || !fetch.tiled || fetch.type != 2 || fetch.dimension != 1 ||
        fetch.format != 6 || fetch.mipMin || fetch.mipMax ||
        fetch.width != g_extent.width || fetch.height != g_extent.height)
        return nullptr;

    const uint64_t byteCount = uint64_t(fetch.width) * fetch.height * 4u;
    const uint32_t guestAddress = PhysicalToCached(fetch.key);
    if (!GuestRangeOk(guestAddress, byteCount))
        return nullptr;

    // A newly allocated history/resolve surface can be sampled before its first
    // explicit EDRAM resolve. If the complete guest allocation is still zero,
    // tiling/endian layout cannot change the visible result: every texel is zero.
    // Materialize that exact initial state on the GPU instead of dropping the draw.
    const uint8_t* src = guestBase + guestAddress;
    for (uint64_t i = 0; i < byteCount; ++i)
        if (src[i] != 0)
            return nullptr;

    ResolveSnapshot* snapshot = nullptr;
    if (!CreateSnapshot(fetch.key, fetch.width, fetch.height, snapshot) || !snapshot)
        return nullptr;

    EndColorRendering();
    TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(g_commandBuffer, snapshot->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    snapshot->frameSeen = g_frames.load(std::memory_order_relaxed) + 1;
    snapshot->copies = 1;

    static uint32_t reports = 0;
    if (reports++ < 8)
        KLOG("Vulkan zero-initialized tiled snapshot: key=%08X size=%ux%u\n",
             fetch.key, fetch.width, fetch.height);
    return snapshot;
}

bool BlurNeutralAuxEnabled()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_BLUR_NEUTRAL_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (!toggleFile.empty())
    {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point nextPoll{};
        static bool enabled = false;
        const auto now = Clock::now();
        if (now >= nextPoll)
        {
            nextPoll = now + std::chrono::milliseconds(50);
            std::ifstream input(toggleFile);
            char value = '0';
            if (input)
                input >> value;
            enabled = value != '0';
        }
        return enabled;
    }

    // Until the Xenos motion-vector RT1 path is reproduced exactly, sampling
    // those auxiliary buffers creates large character halos/ghost trails. The
    // neutral 1x1 inputs preserve the scene color and remove the invalid blur.
    // Keep the original path available with MOJORECOMP_BLUR_NEUTRAL_AUX=0 for A/B.
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_BLUR_NEUTRAL_AUX");
        return !value || !*value || value[0] != '0';
    }();
    return enabled;
}

bool BlurBypassEnabled()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_BLUR_BYPASS_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static bool enabled = false;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(50);
        std::ifstream input(toggleFile);
        char value = '0';
        if (input)
            input >> value;
        enabled = value != '0';
    }
    return enabled;
}

bool SceneColorFeedbackNeutralEnabled()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_FEEDBACK_NEUTRAL_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static bool enabled = false;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(50);
        std::ifstream input(toggleFile);
        char value = '0';
        if (input)
            input >> value;
        enabled = value != '0';
    }
    return enabled;
}

int SceneColorFeedbackConsumerNeutralMask()
{
    static const std::string maskFile = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_FEEDBACK_CONSUMER_MASK_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (maskFile.empty())
        return 0;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mask = 0;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(maskFile);
        int next = 0;
        if (input)
            input >> next;
        mask = std::clamp(next, 0, 31);
    }
    return mask;
}

uint32_t SceneColorFeedbackConsumerBit(uint64_t vsHash, uint64_t psHash)
{
    if (vsHash == 0x1D72F92BDA785FDCull && psHash == 0xD6806DE1D0BFDE01ull)
        return 1u;  // water specular
    if (vsHash == 0x0F3237C46D3AC46Bull && psHash == 0x06996E20CC9E1124ull)
        return 2u;  // water distort
    if (vsHash == 0xF64FC44269F1E70Eull && psHash == 0x22EC5BCABF84EFF7ull)
        return 4u;
    if (vsHash == 0xD589CC02813C1884ull && psHash == 0x80428150BC1ED3ABull)
        return 8u;
    if (vsHash == 0x821F51E34929F4DDull && psHash == 0x89B4E4C8E904ABD5ull)
        return 16u;
    return 0u;
}

bool SceneColorFeedbackTraceEnabled()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_FEEDBACK_TRACE_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static bool enabled = false;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(50);
        std::ifstream input(toggleFile);
        char value = '0';
        if (input)
            input >> value;
        enabled = value != '0';
    }
    return enabled;
}

int SceneColorFeedbackSamplerMode()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_SCENE_FEEDBACK_SAMPLER_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return -1;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int mode = -1;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(50);
        std::ifstream input(toggleFile);
        int next = -1;
        if (input)
            input >> next;
        mode = (next >= 0 && next <= 2) ? next : -1;
    }
    return mode;
}

int DiagnosticEffectDummySlot()
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_EFFECT_DUMMY_SLOT_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return -1;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static int slot = -1;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(50);
        std::ifstream input(toggleFile);
        int next = -1;
        if (input)
            input >> next;
        slot = (next >= 0 && next < int(kTextureSlots)) ? next : -1;
    }
    return slot;
}

bool DiagnosticSkipShaderPair(uint64_t vsHash, uint64_t psHash)
{
    static const std::string toggleFile = [] {
        const char* value = std::getenv("MOJORECOMP_SKIP_PS_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (toggleFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    // Diagnostic selector accepts up to two comma-separated entries so a
    // screen-space pass (for example the pause blur) can be removed while a
    // second shader pair is isolated in the exact same frame.  Each entry is
    // either PS or VS:PS, preserving the original single-entry syntax.
    static std::array<uint64_t, 32> skippedVs{};
    static std::array<uint64_t, 32> skippedPs{};
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(50);
        std::ifstream input(toggleFile);
        std::string value;
        if (input)
            input >> value;

        std::array<uint64_t, 32> nextVs{};
        std::array<uint64_t, 32> nextPs{};
        size_t tokenBegin = 0;
        for (size_t selector = 0; selector < nextPs.size() && tokenBegin < value.size(); ++selector)
        {
            const size_t comma = value.find(',', tokenBegin);
            const std::string token = value.substr(
                tokenBegin, comma == std::string::npos ? std::string::npos : comma - tokenBegin);
            tokenBegin = comma == std::string::npos ? value.size() : comma + 1;
            if (token.empty() || token == "0")
                continue;

            const size_t colon = token.find(':');
            if (colon == std::string::npos)
            {
                char* end = nullptr;
                nextPs[selector] = std::strtoull(token.c_str(), &end, 16);
                if (end == token.c_str())
                    nextPs[selector] = 0;
            }
            else
            {
                const std::string vsText = token.substr(0, colon);
                const std::string psText = token.substr(colon + 1);
                char* vsEnd = nullptr;
                char* psEnd = nullptr;
                nextVs[selector] = std::strtoull(vsText.c_str(), &vsEnd, 16);
                nextPs[selector] = std::strtoull(psText.c_str(), &psEnd, 16);
                if (vsEnd == vsText.c_str() || psEnd == psText.c_str())
                    nextVs[selector] = nextPs[selector] = 0;
            }
        }
        if (nextVs != skippedVs || nextPs != skippedPs)
        {
            skippedVs = nextVs;
            skippedPs = nextPs;
            KLOG("[shader isolate] skip0 VS=%016llX PS=%016llX skip1 VS=%016llX PS=%016llX\n",
                 static_cast<unsigned long long>(skippedVs[0]),
                 static_cast<unsigned long long>(skippedPs[0]),
                 static_cast<unsigned long long>(skippedVs[1]),
                 static_cast<unsigned long long>(skippedPs[1]));
        }
    }
    for (size_t selector = 0; selector < skippedPs.size(); ++selector)
        if (skippedPs[selector] && psHash == skippedPs[selector] &&
            (!skippedVs[selector] || vsHash == skippedVs[selector]))
            return true;
    return false;
}

bool DiagnosticSkipIndexHash(uint64_t vsHash, uint64_t psHash,
                             const Pm4Draw& draw, const uint32_t* regs,
                             const uint8_t* base)
{
    constexpr uint64_t kMainVs = 0xD589CC02813C1884ull;
    constexpr uint64_t kMainPs = 0x80428150BC1ED3ABull;
    if (vsHash != kMainVs || psHash != kMainPs || !draw.indexed ||
        !draw.indexVa || !base)
        return false;

    static const std::string selectorFile = [] {
        const char* value = std::getenv("MOJORECOMP_SKIP_INDEX_HASH_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (selectorFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static std::vector<uint64_t> selectedHashes;
    static bool skipMatches = true;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(selectorFile);
        std::string value((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        for (char& c : value)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';

        std::vector<uint64_t> parsedHashes;
        bool parsedSkipMatches = true;
        std::istringstream tokens(value);
        std::string token;
        while (tokens >> token)
        {
            if (token == "observe" || token == "trace")
            {
                parsedSkipMatches = false;
                continue;
            }
            if (token == "off" || token == "-1" || token == "0")
                continue;
            char* end = nullptr;
            const uint64_t parsed = std::strtoull(token.c_str(), &end, 16);
            if (end != token.c_str() && *end == '\0' && parsed &&
                std::find(parsedHashes.begin(), parsedHashes.end(), parsed) == parsedHashes.end())
                parsedHashes.push_back(parsed);
        }
        selectedHashes = std::move(parsedHashes);
        skipMatches = parsedSkipMatches;
    }
    if (selectedHashes.empty())
        return false;

    const uint64_t indexBytes = uint64_t(draw.indexCount) * (draw.index32 ? 4u : 2u);
    const uint64_t readBytes = !draw.index32 && (draw.indexEndian & 3u) == 2u
                                   ? ((indexBytes + 3u) & ~3ull)
                                   : indexBytes;
    if (!GuestRangeOk(draw.indexVa, readBytes))
        return false;
    const uint64_t hash = HashGuestTextureBytes(base + draw.indexVa,
                                                 static_cast<size_t>(readBytes));
    if (std::find(selectedHashes.begin(), selectedHashes.end(), hash) == selectedHashes.end())
        return false;

    static const bool captureOnMatch = [] {
        const char* value = std::getenv("MOJORECOMP_CAPTURE_ON_INDEX_HASH");
        return value && value[0] && value[0] != '0';
    }();
    static const uint64_t captureAfterFrame = [] {
        const char* value = std::getenv("MOJORECOMP_INDEX_HASH_EVENT_AFTER_FRAME");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    if (captureOnMatch && frame >= captureAfterFrame)
    {
        uint64_t expected = 0;
        if (g_indexHashEventFrame.compare_exchange_strong(
                expected, frame, std::memory_order_relaxed))
        {
            KLOG("[index hash event] armed frame=%llu count=%u hash=%016llX skip=%u\n",
                 static_cast<unsigned long long>(frame), draw.indexCount,
                 static_cast<unsigned long long>(hash), skipMatches ? 1u : 0u);
        }
    }

    static uint32_t reports = 0;
    if (reports++ < 12)
    {
        uint64_t textureHash = 0;
        uint32_t textureKey = 0;
        const uint32_t* raw = regs + xenos::kFetchConstantBase;
        const auto fetch = mojorecomp::texture_abi::Decode(raw);
        const uint32_t pitch = fetch.pitch ? fetch.pitch : fetch.width;
        const uint64_t sourceBytes = uint64_t(pitch) * fetch.height * 4u;
        const uint32_t guestAddress = PhysicalToCached(fetch.key);
        if (fetch.type == 2 && fetch.dimension == 1 && fetch.format == 6 &&
            !fetch.tiled && pitch >= fetch.width &&
            GuestRangeOk(guestAddress, sourceBytes))
        {
            textureKey = fetch.key;
            textureHash = HashGuestTextureBytes(base + guestAddress,
                                                static_cast<size_t>(sourceBytes));
        }
        KLOG("[draw index hash] matched frame=%llu count=%u hash=%016llX skip=%u "
             "texKey=%08X texHash=%016llX\n",
             static_cast<unsigned long long>(frame), draw.indexCount,
             static_cast<unsigned long long>(hash), skipMatches ? 1u : 0u,
             textureKey, static_cast<unsigned long long>(textureHash));
    }
    return skipMatches;
}

bool DiagnosticSkipTextureContentHash(const ShaderModuleRec& vs,
                                      const ShaderModuleRec& ps,
                                      const uint32_t* regs)
{
    static const std::string selectorFile = [] {
        const char* value = std::getenv("MOJORECOMP_SKIP_TEXTURE_HASH_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (selectorFile.empty())
        return false;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point nextPoll{};
    static std::vector<uint64_t> selectedHashes;
    static bool skipMatches = true;
    const auto now = Clock::now();
    if (now >= nextPoll)
    {
        nextPoll = now + std::chrono::milliseconds(20);
        std::ifstream input(selectorFile);
        std::string value((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        for (char& c : value)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';

        std::vector<uint64_t> parsedHashes;
        bool parsedSkipMatches = true;
        std::istringstream tokens(value);
        std::string token;
        while (tokens >> token)
        {
            if (token == "observe" || token == "trace")
            {
                parsedSkipMatches = false;
                continue;
            }
            if (token == "off" || token == "-1" || token == "0")
                continue;
            char* end = nullptr;
            const uint64_t parsed = std::strtoull(token.c_str(), &end, 16);
            if (end != token.c_str() && *end == '\0' && parsed &&
                std::find(parsedHashes.begin(), parsedHashes.end(), parsed) == parsedHashes.end())
                parsedHashes.push_back(parsed);
        }
        selectedHashes = std::move(parsedHashes);
        skipMatches = parsedSkipMatches;
    }
    if (selectedHashes.empty())
        return false;

    std::array<bool, kTextureSlots> checked{};
    for (const ShaderModuleRec* shader : {&vs, &ps})
    {
        if (!shader->usesTextures)
            continue;
        for (uint32_t slot : shader->textureSlots)
        {
            if (slot >= kTextureSlots || checked[slot])
                continue;
            checked[slot] = true;
            const uint32_t* raw = regs + xenos::kFetchConstantBase + slot * 6;
            const auto fetch = mojorecomp::texture_abi::Decode(raw);
            GuestTexture* texture = FindGuestTexture(fetch);
            if (!texture || !texture->sourceHash)
                continue;
            if (std::find(selectedHashes.begin(), selectedHashes.end(), texture->sourceHash) ==
                selectedHashes.end())
                continue;

            static uint32_t reports = 0;
            if (reports++ < 32)
                KLOG("[draw texture hash] matched frame=%llu slot=%u key=%08X hash=%016llX skip=%u\n",
                     static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                     slot, texture->key,
                     static_cast<unsigned long long>(texture->sourceHash),
                     skipMatches ? 1u : 0u);
            return skipMatches;
        }
    }
    return false;
}

const TextureBundle* PrepareTextures(uint8_t* guestBase, const uint32_t* regs,
                                    const ShaderModuleRec& vs, const ShaderModuleRec& ps,
                                    std::array<float, kTextureSlots>& sampleScales,
                                    bool includePixelShader)
{
    g_texturePrepareFailure = "none";
    g_texturePrepareFailureSlot = UINT32_MAX;
    const auto fail = [](const char* reason, uint32_t slot = UINT32_MAX) -> const TextureBundle* {
        g_texturePrepareFailure = reason;
        g_texturePrepareFailureSlot = slot;
        return nullptr;
    };
    sampleScales.fill(1.0f);
    constexpr uint64_t kFrontProbePs = 0x89B4E4C8E904ABD5ull;
    const bool probeFrontPass = includePixelShader && ps.hash == kFrontProbePs;
    static const bool traceEffectTextures = [] {
        const char* value = std::getenv("MOJORECOMP_EFFECT_TEXTURE_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    constexpr uint64_t kEffectProbePs = 0xD6806DE1D0BFDE01ull;
    const bool probeEffectPass = includePixelShader && traceEffectTextures && ps.hash == kEffectProbePs;
    static uint32_t frontPassReports = 0;
    static uint32_t effectPassReports = 0;
    const bool reportFrontPass = (probeFrontPass && frontPassReports < 24) ||
                                 (probeEffectPass && effectPassReports < 18);
    if (reportFrontPass)
        KLOG("[front pass] begin PS=%016llX VS=%016llX\n",
             static_cast<unsigned long long>(ps.hash),
             static_cast<unsigned long long>(vs.hash));

    TextureBundle wanted{};
    wanted.views.fill(g_dummyTextures[0].view);
    wanted.samplers.fill(g_defaultSampler);
    std::array<ResolveSnapshot*, kTextureSlots> sampled{};
    const std::array<const ShaderModuleRec*, 2> shaders{&vs, includePixelShader ? &ps : nullptr};
    for (const auto* shader : shaders)
    {
        if (!shader) continue;
        if (!shader->usesTextures) continue;
        if (shader->textureSlots.empty() || shader->textureSlots.size() != shader->textureDimensions.size())
            return fail("shader-texture-metadata");
        for (size_t i = 0; i < shader->textureSlots.size(); ++i)
        {
            const uint32_t slot = shader->textureSlots[i];
            if (slot >= kTextureSlots)
                return fail("shader-texture-slot", slot);
            if (shader->textureDimensions[i] != 1)
                return fail("shader-texture-dimension", slot);
            const uint32_t* raw = regs + xenos::kFetchConstantBase + slot * 6;
            const auto fetch = mojorecomp::texture_abi::Decode(raw);
            if (reportFrontPass)
                KLOG("[front pass] slot=%u key=%08X type=%u fmt=%u size=%ux%u dim=%u "
                     "endian=%u swz=%03X sign=%u/%u/%u/%u num=%u exp=%d "
                     "filter=%u/%u clamp=%u/%u mips=%u..%u "
                     "raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
                     slot, fetch.key, fetch.type, fetch.format, fetch.width, fetch.height,
                     fetch.dimension, (raw[1] >> 6) & 3u, fetch.swizzle,
                     fetch.signX, fetch.signY, fetch.signZ, fetch.signW,
                     fetch.numFormat, fetch.expAdjust,
                     fetch.minFilter, fetch.magFilter, fetch.clampX, fetch.clampY,
                     fetch.mipMin, fetch.mipMax,
                     raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
            if (fetch.type == 0 && fetch.key == 0 && fetch.format == 0)
            {
                // Disabled/unbound Xenos fetch constant. The shader may still contain
                // a static reference to the slot behind guest control flow; keep a
                // valid dummy descriptor bound instead of rejecting the whole draw.
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=dummy-disabled\n", slot);
                continue;
            }
            if (probeEffectPass && DiagnosticEffectDummySlot() == int(slot))
            {
                wanted.views[slot] = g_dummyTextures[0].view;
                wanted.samplers[slot] = GetSnapshotSampler(fetch);
                if (!wanted.views[slot] || !wanted.samplers[slot]) return fail("effect-dummy-view-or-sampler", slot);
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=dummy-effect-diagnostic\n", slot);
                continue;
            }
            // Guest memory is routinely aliased between D24S8 and D24FS8. Keep
            // both host images alive for the same guest address and select the
            // variant described by the fetch instead of whichever snapshot was
            // created first for that key.
            ResolveSnapshot* snapshot = nullptr;
            for (auto& candidate : g_snapshots)
            {
                if (candidate.key != fetch.key || !candidate.copies ||
                    candidate.width != fetch.width || candidate.height != fetch.height)
                    continue;

                bool compatibleAlias = false;
                if (fetch.format == 22)
                    compatibleAlias = candidate.depth && !candidate.depthFloat24;
                else if (fetch.format == 23)
                    compatibleAlias = candidate.depth && candidate.depthFloat24;
                else if (fetch.format == 6 && fetch.swizzle == 0x688u)
                    compatibleAlias = !candidate.depth;
                else if (fetch.format == 6 && fetch.swizzle == 0x60Au)
                    compatibleAlias = true;
                else
                    compatibleAlias = true;

                if (!compatibleAlias)
                    continue;
                if (!snapshot || candidate.frameSeen > snapshot->frameSeen ||
                    (candidate.frameSeen == snapshot->frameSeen && candidate.copies > snapshot->copies))
                    snapshot = &candidate;
            }
            if (!snapshot)
                snapshot = FindSnapshot(fetch.key);
            const bool sceneColorFeedback =
                fetch.key == 0x0A63D000u || fetch.key == 0x09B75000u;
            const int sceneFeedbackSamplerMode = sceneColorFeedback
                ? SceneColorFeedbackSamplerMode() : -1;
            const uint32_t sceneFeedbackConsumerBit = sceneColorFeedback
                ? SceneColorFeedbackConsumerBit(vs.hash, ps.hash) : 0u;
            const bool neutralSceneFeedbackConsumer = sceneFeedbackConsumerBit &&
                ((SceneColorFeedbackConsumerNeutralMask() & int(sceneFeedbackConsumerBit)) != 0);
            // The final/front composition pass also samples the scene-color
            // resolve, but it must see the live snapshot. Feeding the stable
            // history image back into that pass can black out/freeze the final
            // frame. Stable history is only for intermediate scene-feedback
            // consumers such as water/refraction.
            const bool stableSceneFeedback = sceneColorFeedback && !probeFrontPass &&
                (sceneFeedbackConsumerBit & 3u) != 0u && StableSceneFeedbackEnabled() &&
                snapshot && snapshot->presentValid && snapshot->presentImage && snapshot->presentView &&
                snapshot->presentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (SceneColorFeedbackTraceEnabled() &&
                sceneColorFeedback)
            {
                KLOG("[scene feedback] frame=%llu VS=%016llX PS=%016llX slot=%u key=%08X "
                     "size=%ux%u fmt=%u swz=%03X filter=%u/%u clamp=%u/%u raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
                     static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                     static_cast<unsigned long long>(vs.hash),
                     static_cast<unsigned long long>(ps.hash),
                     slot, fetch.key, fetch.width, fetch.height, fetch.format, fetch.swizzle,
                     fetch.minFilter, fetch.magFilter, fetch.clampX, fetch.clampY,
                     raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
            }
            if ((SceneColorFeedbackNeutralEnabled() || neutralSceneFeedbackConsumer) &&
                sceneColorFeedback)
            {
                wanted.views[slot] = g_dummyTextures[0].view;
                wanted.samplers[slot] = GetSnapshotSampler(fetch, sceneFeedbackSamplerMode);
                if (!wanted.views[slot] || !wanted.samplers[slot]) return fail("scene-feedback-dummy-view-or-sampler", slot);
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=dummy-scene-feedback key=%08X consumerBit=%u\n",
                         slot, fetch.key, sceneFeedbackConsumerBit);
                continue;
            }
            if (probeFrontPass && BlurNeutralAuxEnabled() && (slot == 2 || slot == 3))
            {
                wanted.views[slot] = g_neutralBlurTexture.view;
                wanted.samplers[slot] = GetSnapshotSampler(fetch);
                if (!wanted.views[slot] || !wanted.samplers[slot]) return fail("blur-dummy-view-or-sampler", slot);
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=neutral-1x1 diagnostic\n", slot);
                continue;
            }
            const bool basicSnapshotFetch = fetch.type == 2 && fetch.dimension == 1 &&
                !fetch.mipMin && !fetch.mipMax && fetch.minFilter <= 1 &&
                fetch.magFilter <= 1 && fetch.clampX <= 2 && fetch.clampY <= 2;
            const bool colorSnapshotFetch = snapshot && !snapshot->depth && fetch.format == 6 &&
                (fetch.swizzle == 0x688u || fetch.swizzle == 0x60Au);
            // Xenos depth resolves are later rebound as tiled k_24_8/k_24_8_FLOAT
            // texture fetches. The resolve snapshot already owns the decoded depth
            // image, so sampling that image directly is the correct aliasing model;
            // do not reinterpret the guest destination bytes as RGBA.
            const bool depthSnapshotFetch = snapshot && snapshot->depth &&
                (((fetch.format == 22 || fetch.format == 23) && fetch.swizzle == 0x688u) ||
                 (fetch.format == 6 && fetch.swizzle == 0x60Au));
            const bool snapshotCompatible = snapshot && snapshot->copies &&
                snapshot->width == fetch.width && snapshot->height == fetch.height &&
                basicSnapshotFetch && (colorSnapshotFetch || depthSnapshotFetch);
            if (snapshotCompatible)
            {
                wanted.views[slot] = stableSceneFeedback
                    ? GetSnapshotPresentView(*snapshot, fetch)
                    : GetSnapshotView(*snapshot, fetch);
                wanted.samplers[slot] = GetSnapshotSampler(fetch, sceneFeedbackSamplerMode);
                if (!wanted.views[slot] || !wanted.samplers[slot]) return fail("snapshot-view-or-sampler", slot);
                if (snapshot->depth && fetch.format != 6u &&
                    snapshot->format == kDepthFloatFormat)
                    sampleScales[slot] = 2.0f;
                if (!stableSceneFeedback)
                    sampled[slot] = snapshot;
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=%s key=%08X copies=%llu depth=%u\n",
                         slot, stableSceneFeedback ? "snapshot-stable-scene" : "snapshot-compatible",
                         snapshot->key, static_cast<unsigned long long>(snapshot->copies),
                         snapshot->depth ? 1u : 0u);
                continue;
            }
            if (fetch.format == 2)
            {
                GuestTexture* guestTexture = CreateGuestR8Texture(guestBase, fetch);
                if (!guestTexture)
                    return fail("guest-r8-create", slot);
                wanted.views[slot] = guestTexture->view;
                wanted.samplers[slot] = GetSnapshotSampler(
                    fetch, -1, mojorecomp::gpu::TextureSource::GuestTexture);
                if (!wanted.samplers[slot]) return fail("guest-r8-sampler", slot);
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=guest-r8 key=%08X\n", slot, fetch.key);
                continue;
            }
            if (fetch.format == 6 && !snapshot)
            {
                if (ResolveSnapshot* zeroSnapshot =
                        CreateZeroSnapshotFromGuestIfEmpty(guestBase, fetch))
                {
                    wanted.views[slot] = GetSnapshotView(*zeroSnapshot, fetch);
                    wanted.samplers[slot] = GetSnapshotSampler(fetch);
                    if (!wanted.views[slot] || !wanted.samplers[slot]) return fail("zero-snapshot-view-or-sampler", slot);
                    sampled[slot] = zeroSnapshot;
                    if (reportFrontPass)
                        KLOG("[front pass] slot=%u source=zero-snapshot key=%08X\n",
                             slot, fetch.key);
                    continue;
                }
                GuestTexture* guestTexture = CreateGuestRGBA8Texture(guestBase, fetch);
                if (!guestTexture)
                    return fail("guest-rgba8-create", slot);
                wanted.views[slot] = guestTexture->view;
                wanted.samplers[slot] = GetSnapshotSampler(
                    fetch, -1, mojorecomp::gpu::TextureSource::GuestTexture);
                if (!wanted.samplers[slot]) return fail("guest-rgba8-sampler", slot);
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=guest-rgba8 key=%08X\n", slot, fetch.key);
                continue;
            }
            if (mojorecomp::texture_abi::BlockCompressedBytesPerBlock(fetch.format) &&
                !snapshot)
            {
                GuestTexture* guestTexture =
                    CreateGuestBlockCompressedTexture(guestBase, fetch);
                if (!guestTexture)
                    return fail("guest-bc-create", slot);
                wanted.views[slot] = guestTexture->view;
                wanted.samplers[slot] = GetSnapshotSampler(
                    fetch, -1, mojorecomp::gpu::TextureSource::GuestTexture);
                if (!wanted.samplers[slot]) return fail("guest-bc-sampler", slot);
                if (reportFrontPass)
                    KLOG("[front pass] slot=%u source=guest-bc format=%u key=%08X\n",
                         slot, fetch.format, fetch.key);
                continue;
            }
            if (!fetch.simple || !snapshot || !snapshot->copies ||
                snapshot->width != fetch.width || snapshot->height != fetch.height)
            {
                static uint32_t reports = 0;
                if (reports++ < 16)
                    KLOG("Vulkan texture unsupported: slot=%u base=%08X format=%u size=%ux%u "
                         "dim=%u tiled=%u endian=%u mips=%u..%u raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
                         slot, fetch.key, fetch.format, fetch.width, fetch.height, fetch.dimension,
                         raw[0] >> 31, (raw[1] >> 6) & 3, fetch.mipMin, fetch.mipMax,
                         raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
                return fail("unsupported-fetch-or-snapshot", slot);
            }
            sampled[slot] = snapshot;
            wanted.views[slot] = snapshot->view;
            wanted.samplers[slot] = GetSnapshotSampler(fetch, sceneFeedbackSamplerMode);
            if (!wanted.samplers[slot]) return fail("generic-snapshot-sampler", slot);
            if (reportFrontPass)
                KLOG("[front pass] slot=%u source=snapshot-generic key=%08X copies=%llu depth=%u\n",
                     slot, snapshot->key,
                     static_cast<unsigned long long>(snapshot->copies),
                     snapshot->depth ? 1u : 0u);
        }
    }
    const TextureBundle* bundle = GetTextureBundle(wanted);
    if (!bundle) return fail("descriptor-bundle");
    // Barriers cannot run inside this dynamic rendering instance, but don't end
    // and restart rendering merely because a shader uses textures. Most resolved
    // snapshots stay in SHADER_READ_ONLY across many draws; only break rendering
    // when at least one sampled snapshot actually needs a layout transition.
    bool needsSnapshotTransition = false;
    for (auto* snapshot : sampled)
        if (snapshot && snapshot->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            needsSnapshotTransition = true;
            break;
        }
    if (needsSnapshotTransition)
    {
        EndColorRendering();
        for (auto* snapshot : sampled)
            if (snapshot && snapshot->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                TransitionSnapshot(*snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ResumeColorRendering();
        ++g_perfSnapshotRenderBreaks;
    }

    // Opt-in diagnostic: capture a resolved texture at the exact draw that
    // samples it, before later resolves/clears can reuse the same snapshot.
    // This deliberately reuses the normal front-readback staging region, so it
    // has no allocation/lifetime impact on regular rendering and is completely
    // dormant unless both environment variables are supplied.
    static const uint64_t drawSnapshotCaptureFrame = [] {
        const char* value = std::getenv("MOJORECOMP_DRAW_SNAPSHOT_CAPTURE_FRAME");
        if (!value || !*value)
            return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return end != value ? static_cast<uint64_t>(parsed) : uint64_t{0};
    }();
    static const uint32_t drawSnapshotCaptureKey = [] {
        const char* value = std::getenv("MOJORECOMP_DRAW_SNAPSHOT_CAPTURE_KEY");
        if (!value || !*value)
            return uint32_t{0};
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 16);
        return end != value ? static_cast<uint32_t>(parsed) : uint32_t{0};
    }();
    const uint64_t drawSnapshotFrame = g_frames.load(std::memory_order_relaxed) + 1;
    if (drawSnapshotCaptureFrame && drawSnapshotCaptureKey &&
        drawSnapshotFrame == drawSnapshotCaptureFrame && !g_readbackPending &&
        g_readbackBytes)
    {
        ResolveSnapshot* captureSnapshot = nullptr;
        for (auto* snapshot : sampled)
            if (snapshot && snapshot->key == drawSnapshotCaptureKey)
            {
                captureSnapshot = snapshot;
                break;
            }

        if (captureSnapshot && !captureSnapshot->depth &&
            captureSnapshot->width == g_extent.width &&
            captureSnapshot->height == g_extent.height)
        {
            EndColorRendering();
            TransitionSnapshot(*captureSnapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            VkBufferImageCopy copy{};
            copy.bufferOffset = g_readbackOffset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {g_internalExtent.width, g_internalExtent.height, 1};
            p_vkCmdCopyImageToBuffer(g_commandBuffer, captureSnapshot->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     g_uploadBuffer, 1, &copy);

            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = g_uploadBuffer;
            host.offset = g_readbackOffset;
            host.size = g_readbackBytes;
            p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, 0,
                                   0, nullptr, 1, &host, 0, nullptr);

            TransitionSnapshot(*captureSnapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            ResumeColorRendering();

            g_frontReadbackFrame = drawSnapshotFrame;
            g_readbackPending = true;
            g_readbackFrameSlot = g_frameSlot;
            KLOG("[draw snapshot capture] frame=%llu key=%08X copies=%llu slot=%u\n",
                 static_cast<unsigned long long>(drawSnapshotFrame),
                 captureSnapshot->key,
                 static_cast<unsigned long long>(captureSnapshot->copies),
                 g_frameSlot);
        }
    }
    if (reportFrontPass)
    {
        if (probeEffectPass)
        {
            ++effectPassReports;
            KLOG("[front pass] end effect-report=%u\n", effectPassReports);
        }
        else
        {
            ++frontPassReports;
            KLOG("[front pass] end report=%u\n", frontPassReports);
        }
    }
    return bundle;
}

VkDeviceSize UploadAlloc(VkDeviceSize bytes, VkDeviceSize alignment)
{
    const VkDeviceSize at = (g_uploadAt + alignment - 1) & ~(alignment - 1);
    if (at + bytes > g_uploadLimit)
        return VK_WHOLE_SIZE;
    g_uploadAt = at + bytes;
    return at;
}

void CopySwapped(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian)
{
    switch (endian & 3u)
    {
        case 0:
            std::memcpy(dst, src, bytes);
            break;
        case 1:
        {
            size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
            for (; i + 15 < bytes; i += 16)
            {
                const __m128i value = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(src + i));
                const __m128i swapped = _mm_or_si128(
                    _mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), swapped);
            }
#endif
            for (; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
                std::memcpy(dst + i, &v, 4);
            }
            break;
        }
        case 2:
        {
            size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
            for (; i + 15 < bytes; i += 16)
            {
                const __m128i value = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(src + i));
                const __m128i byteSwapped16 = _mm_or_si128(
                    _mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
                const __m128i swapped = _mm_or_si128(
                    _mm_slli_epi32(byteSwapped16, 16),
                    _mm_srli_epi32(byteSwapped16, 16));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), swapped);
            }
#endif
            for (; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = __builtin_bswap32(v);
                std::memcpy(dst + i, &v, 4);
            }
            break;
        }
        case 3:
        {
            size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
            for (; i + 15 < bytes; i += 16)
            {
                const __m128i value = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(src + i));
                const __m128i swapped = _mm_or_si128(
                    _mm_slli_epi32(value, 16), _mm_srli_epi32(value, 16));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), swapped);
            }
#endif
            for (; i + 3 < bytes; i += 4)
            {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = (v >> 16) | (v << 16);
                std::memcpy(dst + i, &v, 4);
            }
            break;
        }
    }
}

struct FxaaPush
{
    float invExtent[2]{};
    float edgeThreshold = 0.125f;
    float edgeThresholdMin = 0.0312f;
    float subpixel = 0.75f;
    float spanMax = 8.0f;
};

void DestroyFxaaImage()
{
    if (g_fxaaView && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_fxaaView, nullptr);
    if (g_fxaaImage && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_fxaaImage, nullptr);
    if (g_fxaaMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_fxaaMemory, nullptr);
    g_fxaaView = VK_NULL_HANDLE;
    g_fxaaImage = VK_NULL_HANDLE;
    g_fxaaMemory = VK_NULL_HANDLE;
    g_fxaaImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_fxaaExtent = {};
}

bool EnsureFxaaImage()
{
    const VkRect2D contentRect = OutputContentRect();
    if (!contentRect.extent.width || !contentRect.extent.height)
        return false;
    if (g_fxaaImage && g_fxaaExtent.width == contentRect.extent.width &&
        g_fxaaExtent.height == contentRect.extent.height)
        return true;

    DestroyFxaaImage();
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = g_swapFormat;
    imageInfo.extent = {contentRect.extent.width, contentRect.extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &imageInfo, nullptr, &g_fxaaImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements{};
    p_vkGetImageMemoryRequirements(g_device, g_fxaaImage, &requirements);
    const uint32_t memoryType = FindMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = memoryType;
    if (p_vkAllocateMemory(g_device, &memoryInfo, nullptr, &g_fxaaMemory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, g_fxaaImage, g_fxaaMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_fxaaImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = g_swapFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &viewInfo, nullptr, &g_fxaaView) != VK_SUCCESS)
        return false;

    g_fxaaExtent = contentRect.extent;
    g_fxaaImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g_fxaaDescriptor)
    {
        VkDescriptorImageInfo sampledImage{};
        sampledImage.imageView = g_fxaaView;
        sampledImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = g_fxaaDescriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &sampledImage;
        p_vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    }
    KLOG("Vulkan FXAA intermediate ready: %ux%u\n",
         g_fxaaExtent.width, g_fxaaExtent.height);
    return true;
}

bool PrepareFxaaAssets()
{
    if (g_fxaaAssetsPrepared)
        return true;
    if (g_fxaaAssetsAttempted)
        return false;
    g_fxaaAssetsAttempted = true;

    static constexpr const char* kVs = R"(
struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID)
{
    VSOut output;
    const float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    // DXC's Vulkan clip-space conversion and the sampled transfer image use
    // opposite Y conventions here. Keep the post-process aligned with the
    // direct presentation blit instead of vertically mirroring the frame.
    output.uv = float2(uv.x, 1.0 - uv.y);
    return output;
}
)";

    // Deterministic presets are supplied through FxaaPush. The shader adapts
    // NVIDIA FXAA 3.11 Quality's edge-orientation, span-search and subpixel
    // model (Timothy Lottes); redistribution terms are preserved in
    // launcher/resources/licenses/NVIDIA-FXAA-LICENSE.txt. Luma is derived
    // from RGB because the guest front buffer does not carry perceptual luma
    // in alpha.
    static constexpr const char* kPs = R"(
[[vk::binding(0, 0)]] Texture2D<float4> g_Source;
[[vk::binding(1, 0)]] SamplerState g_Sampler;
struct FxaaPush
{
    float2 invExtent;
    float edgeThreshold;
    float edgeThresholdMin;
    float subpixel;
    float spanMax;
};
[[vk::push_constant]] ConstantBuffer<FxaaPush> g_Push;
struct PSIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}
float LumaAt(float2 uv)
{
    return Luma(g_Source.SampleLevel(g_Sampler, uv, 0.0).rgb);
}
float4 main(PSIn input) : SV_Target0
{
    const float2 texel = g_Push.invExtent;
    float2 posM = input.uv;
    const float3 rgbM = g_Source.SampleLevel(g_Sampler, posM, 0.0).rgb;
    const float lumaM = Luma(rgbM);
    float lumaN = LumaAt(posM + float2(0.0, -texel.y));
    float lumaS = LumaAt(posM + float2(0.0,  texel.y));
    const float lumaE = LumaAt(posM + float2( texel.x, 0.0));
    const float lumaW = LumaAt(posM + float2(-texel.x, 0.0));

    const float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    const float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    const float lumaRange = rangeMax - rangeMin;
    if (lumaRange < max(g_Push.edgeThresholdMin, rangeMax * g_Push.edgeThreshold))
        return float4(rgbM, 1.0);

    const float lumaNW = LumaAt(posM + texel * float2(-1.0, -1.0));
    const float lumaNE = LumaAt(posM + texel * float2( 1.0, -1.0));
    const float lumaSW = LumaAt(posM + texel * float2(-1.0,  1.0));
    const float lumaSE = LumaAt(posM + texel * float2( 1.0,  1.0));

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float lumaNESE = lumaNE + lumaSE;
    const float lumaNWNE = lumaNW + lumaNE;
    const float lumaNWSW = lumaNW + lumaSW;
    const float lumaSWSE = lumaSW + lumaSE;
    const float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
    const float edgeVert1 = (-2.0 * lumaM) + lumaWE;
    const float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
    const float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
    const float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
    const float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
    const float edgeHorz =
        abs(edgeHorz3) + abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    const float edgeVert =
        abs(edgeVert3) + abs(edgeVert1) * 2.0 + abs(edgeVert2);

    const bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = texel.x;
    if (!horzSpan)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    else
    {
        lengthSign = texel.y;
    }

    const float gradientN = lumaN - lumaM;
    const float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    const float lumaSS = lumaS + lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;

    const float subpixA =
        (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    const float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    const float subpixC = saturate(abs(subpixB) / max(lumaRange, 1e-6));
    const float subpixD = -2.0 * subpixC + 3.0;
    const float subpixE = subpixC * subpixC;
    const float subpixF = subpixD * subpixE;

    float2 posB = posM;
    const float2 offNP = horzSpan ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    if (!horzSpan)
        posB.x += lengthSign * 0.5;
    else
        posB.y += lengthSign * 0.5;

    float2 posN = posB - offNP;
    float2 posP = posB + offNP;
    if (!pairN)
        lumaNN = lumaSS;
    const float lumaLocalAverage = lumaNN * 0.5;
    const float gradientScaled = gradient * 0.25;
    const bool lumaMLTZero = (lumaM - lumaLocalAverage) < 0.0;

    float lumaEndN = LumaAt(posN) - lumaLocalAverage;
    float lumaEndP = LumaAt(posP) - lumaLocalAverage;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;

    [unroll]
    for (uint search = 0; search < 4; ++search)
    {
        if (doneN && doneP)
            break;
        const float step =
            search == 0 ? 1.5 :
            search == 1 ? 2.0 :
            search == 2 ? 4.0 : g_Push.spanMax;
        if (!doneN)
        {
            posN -= offNP * step;
            lumaEndN = LumaAt(posN) - lumaLocalAverage;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP)
        {
            posP += offNP * step;
            lumaEndP = LumaAt(posP) - lumaLocalAverage;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }

    float dstN = posM.x - posN.x;
    float dstP = posP.x - posM.x;
    if (!horzSpan)
    {
        dstN = posM.y - posN.y;
        dstP = posP.y - posM.y;
    }
    const bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    const bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    const bool directionN = dstN < dstP;
    const bool goodSpan = directionN ? goodSpanN : goodSpanP;
    const float spanLength = max(dstP + dstN, 1e-6);
    const float pixelOffset = 0.5 - min(dstN, dstP) / spanLength;
    const float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    const float subpixG = subpixF * subpixF;
    const float subpixH = subpixG * saturate(g_Push.subpixel);
    const float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan)
        posM.x += pixelOffsetSubpix * lengthSign;
    else
        posM.y += pixelOffsetSubpix * lengthSign;

    return float4(g_Source.SampleLevel(g_Sampler, posM, 0.0).rgb, 1.0);
}
)";

    std::string error;
    if (!ShaderTranslator::CompileHostHlsl(kVs, true, g_fxaaVsSpv, error))
    {
        KLOG("Vulkan FXAA VS compile failed: %s\n", error.c_str());
        return false;
    }
    if (!ShaderTranslator::CompileHostHlsl(kPs, false, g_fxaaPsSpv, error))
    {
        KLOG("Vulkan FXAA PS compile failed: %s\n", error.c_str());
        return false;
    }
    g_fxaaAssetsPrepared = true;
    return true;
}

bool EnsureFxaaResources()
{
    if (g_fxaaResourcesReady)
        return EnsureFxaaImage();
    if (g_fxaaResourcesAttempted)
        return false;
    g_fxaaResourcesAttempted = true;
    if (!PrepareFxaaAssets())
        return false;

    auto createModule = [](const std::vector<uint8_t>& spirv, VkShaderModule& module) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size();
        info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
        return p_vkCreateShaderModule(g_device, &info, nullptr, &module) == VK_SUCCESS;
    };
    if (!createModule(g_fxaaVsSpv, g_fxaaVs) || !createModule(g_fxaaPsSpv, g_fxaaPs))
        return false;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();
    if (p_vkCreateDescriptorSetLayout(g_device, &setInfo, nullptr, &g_fxaaSetLayout) != VK_SUCCESS)
        return false;

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (p_vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_fxaaDescriptorPool) != VK_SUCCESS)
        return false;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size = sizeof(FxaaPush);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_fxaaSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (p_vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &g_fxaaPipelineLayout) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (p_vkCreateSampler(g_device, &samplerInfo, nullptr, &g_fxaaSampler) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo descriptorAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptorAllocate.descriptorPool = g_fxaaDescriptorPool;
    descriptorAllocate.descriptorSetCount = 1;
    descriptorAllocate.pSetLayouts = &g_fxaaSetLayout;
    if (p_vkAllocateDescriptorSets(g_device, &descriptorAllocate, &g_fxaaDescriptor) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo sampler{};
    sampler.sampler = g_fxaaSampler;
    VkWriteDescriptorSet samplerWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    samplerWrite.dstSet = g_fxaaDescriptor;
    samplerWrite.dstBinding = 1;
    samplerWrite.descriptorCount = 1;
    samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerWrite.pImageInfo = &sampler;
    p_vkUpdateDescriptorSets(g_device, 1, &samplerWrite, 0, nullptr);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_fxaaVs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_fxaaPs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    constexpr std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g_swapFormat;
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = g_fxaaPipelineLayout;
    if (p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &pipelineInfo,
                                    nullptr, &g_fxaaPipeline) != VK_SUCCESS)
        return false;

    g_fxaaResourcesReady = true;
    g_fxaaVsSpv.clear();
    g_fxaaVsSpv.shrink_to_fit();
    g_fxaaPsSpv.clear();
    g_fxaaPsSpv.shrink_to_fit();
    if (!EnsureFxaaImage())
        return false;
    KLOG("Vulkan FXAA resources ready\n");
    return true;
}

bool DrawFxaa(uint32_t imageIndex, VkImage sourceImage,
              uint32_t sourceWidth, uint32_t sourceHeight,
              bool preserveNative16x9 = false)
{
    const auto aa = mojorecomp::config::Get().antiAliasing;
    if (aa == mojorecomp::config::AntiAliasing::Off ||
        imageIndex >= g_swapViews.size() || !sourceImage)
        return false;
    if (!EnsureFxaaResources())
        return false;
    const VkRect2D contentRect = OutputContentRect(preserveNative16x9);
    const auto sourceCrop = mojorecomp::gpu::ScenePresentationSourceCrop(
        sourceWidth, sourceHeight, ConfiguredAspectRatio(), preserveNative16x9);

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = g_fxaaImageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        ? VK_ACCESS_SHADER_READ_BIT : 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = g_fxaaImageLayout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = g_fxaaImage;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           g_fxaaImageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                               : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toTransfer);

    VkImageBlit compose{};
    compose.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    compose.srcOffsets[0] = {
        static_cast<int32_t>(sourceCrop.x * g_resolutionScale),
        static_cast<int32_t>(sourceCrop.y * g_resolutionScale), 0};
    compose.srcOffsets[1] = {
        static_cast<int32_t>((sourceCrop.x + sourceCrop.width) * g_resolutionScale),
        static_cast<int32_t>((sourceCrop.y + sourceCrop.height) * g_resolutionScale), 1};
    compose.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    compose.dstOffsets[1] = {static_cast<int32_t>(g_fxaaExtent.width),
                             static_cast<int32_t>(g_fxaaExtent.height), 1};
    p_vkCmdBlitImage(g_commandBuffer, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     g_fxaaImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &compose, VK_FILTER_LINEAR);

    VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.image = g_fxaaImage;
    toSample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toSample);
    g_fxaaImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkImageMemoryBarrier swapToAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    swapToAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swapToAttachment.oldLayout =
        (imageIndex < g_imageInitialized.size() && g_imageInitialized[imageIndex])
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    swapToAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapToAttachment.srcQueueFamilyIndex = swapToAttachment.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    swapToAttachment.image = g_images[imageIndex];
    swapToAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &swapToAttachment);

    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = g_swapViews[imageIndex];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkClearValue black{};
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue = black;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, g_outputExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    p_vkCmdBeginRendering(g_commandBuffer, &rendering);
    CmdBindGraphicsPipelineCached(g_commandBuffer, g_fxaaPipeline);
    VkViewport viewport{};
    viewport.x = static_cast<float>(contentRect.offset.x);
    viewport.y = static_cast<float>(contentRect.offset.y);
    viewport.width = static_cast<float>(contentRect.extent.width);
    viewport.height = static_cast<float>(contentRect.extent.height);
    viewport.maxDepth = 1.0f;
    CmdSetViewportCached(g_commandBuffer, viewport);
    CmdSetScissorCached(g_commandBuffer, contentRect);
    CmdBindDescriptorSetsCached(g_commandBuffer, g_fxaaPipelineLayout,
                                0, 1, &g_fxaaDescriptor);

    FxaaPush push{};
    push.invExtent[0] = 1.0f / static_cast<float>(g_fxaaExtent.width);
    push.invExtent[1] = 1.0f / static_cast<float>(g_fxaaExtent.height);
    if (aa == mojorecomp::config::AntiAliasing::FxaaExtreme)
    {
        push.edgeThreshold = 0.063f;
        push.edgeThresholdMin = 0.0312f;
        push.subpixel = 1.0f;
        push.spanMax = 12.0f;
    }
    p_vkCmdPushConstants(g_commandBuffer, g_fxaaPipelineLayout,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
    p_vkCmdEndRendering(g_commandBuffer);

    static bool reported[2]{};
    const uint32_t preset = aa == mojorecomp::config::AntiAliasing::FxaaExtreme ? 1u : 0u;
    if (!reported[preset])
    {
        reported[preset] = true;
        KLOG("Vulkan FXAA active: preset=%s extent=%ux%u threshold=%.4f min=%.4f subpixel=%.2f span=%.1f\n",
             mojorecomp::config::ToString(aa), g_fxaaExtent.width, g_fxaaExtent.height,
             push.edgeThreshold, push.edgeThresholdMin, push.subpixel, push.spanMax);
    }
    return true;
}

bool PrepareDebugOverlayAssets()
{
    if (g_overlayAssetsPrepared)
        return true;
    if (g_overlayAssetsAttempted)
        return false;
    g_overlayAssetsAttempted = true;

    mojorecomp::host::SystemFontAtlas fontAtlas;
    if (!mojorecomp::host::BuildSystemFontAtlas(
            kOverlayAtlasWidth, kOverlayAtlasHeight, kOverlayFirstGlyph,
            kOverlayGlyphCount, kOverlayAtlasColumns, kOverlayCellSize,
            -20, fontAtlas))
    {
        KLOG("Vulkan debug overlay font rasterization failed\n");
        return false;
    }
    g_overlayAtlasBytes = std::move(fontAtlas.alpha);
    for (uint32_t i = 0; i < kOverlayGlyphCount; ++i)
    {
        const uint32_t cellX = (i % kOverlayAtlasColumns) * kOverlayCellSize;
        const uint32_t cellY = (i / kOverlayAtlasColumns) * kOverlayCellSize;
        // Sample from texel centers rather than the exact cell boundary. The
        // atlas has adjacent glyph cells and uses linear filtering, so sampling
        // on a boundary can blend coverage from the neighboring glyph. This was
        // mostly invisible at the original debug-overlay size but became obvious
        // as small black fragments around the larger outlined subtitles.
        constexpr float kGlyphUvInset = 0.5f;
        g_overlayGlyphs[i].u0 = (float(cellX) + kGlyphUvInset) /
                                float(kOverlayAtlasWidth);
        g_overlayGlyphs[i].v0 = (float(cellY) + kGlyphUvInset) /
                                float(kOverlayAtlasHeight);
        g_overlayGlyphs[i].u1 = (float(cellX + kOverlayCellSize) - kGlyphUvInset) /
                                float(kOverlayAtlasWidth);
        g_overlayGlyphs[i].v1 = (float(cellY + kOverlayCellSize) - kGlyphUvInset) /
                                float(kOverlayAtlasHeight);
        g_overlayGlyphs[i].advance = fontAtlas.advances[i];
    }

    static constexpr const char* kVs = R"(
struct VSIn
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
VSOut main(VSIn input)
{
    VSOut output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";
    static constexpr const char* kPs = R"(
[[vk::binding(0, 0)]] Texture2D<float> g_Atlas;
[[vk::binding(1, 0)]] SamplerState g_Sampler;
struct PSIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
float4 main(PSIn input) : SV_Target0
{
    const float coverage = g_Atlas.Sample(g_Sampler, input.uv).r;
    return float4(input.color.rgb, input.color.a * coverage);
}
)";

    std::string error;
    if (!ShaderTranslator::CompileHostHlsl(kVs, true, g_overlayVsSpv, error))
    {
        KLOG("Vulkan debug overlay VS compile failed: %s\n", error.c_str());
        return false;
    }
    if (!ShaderTranslator::CompileHostHlsl(kPs, false, g_overlayPsSpv, error))
    {
        KLOG("Vulkan debug overlay PS compile failed: %s\n", error.c_str());
        return false;
    }

    g_overlayAssetsPrepared = true;
    KLOG("Vulkan debug overlay assets prepared: system-font atlas=%ux%u format=R8\n",
         kOverlayAtlasWidth, kOverlayAtlasHeight);
    return true;
}

bool EnsureDebugOverlayResources()
{
    if (g_overlayResourcesReady)
        return true;
    if (g_overlayResourcesAttempted)
        return false;
    g_overlayResourcesAttempted = true;
    if (!PrepareDebugOverlayAssets())
        return false;

    auto createModule = [](const std::vector<uint8_t>& spirv,
                           VkShaderModule& module) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size();
        info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
        return p_vkCreateShaderModule(g_device, &info, nullptr, &module) == VK_SUCCESS;
    };
    if (!createModule(g_overlayVsSpv, g_overlayVs) ||
        !createModule(g_overlayPsSpv, g_overlayPs))
        return false;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();
    if (p_vkCreateDescriptorSetLayout(g_device, &setInfo, nullptr,
                                      &g_overlaySetLayout) != VK_SUCCESS)
        return false;

    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
    }};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (p_vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                                 &g_overlayDescriptorPool) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_overlaySetLayout;
    if (p_vkCreatePipelineLayout(g_device, &layoutInfo, nullptr,
                                 &g_overlayPipelineLayout) != VK_SUCCESS)
        return false;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent = {kOverlayAtlasWidth, kOverlayAtlasHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(g_device, &imageInfo, nullptr,
                        &g_overlayAtlasImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements imageMemory{};
    p_vkGetImageMemoryRequirements(g_device, g_overlayAtlasImage, &imageMemory);
    const uint32_t memoryType = FindMemoryType(
        imageMemory.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memoryInfo.allocationSize = imageMemory.size;
    memoryInfo.memoryTypeIndex = memoryType;
    if (p_vkAllocateMemory(g_device, &memoryInfo, nullptr,
                           &g_overlayAtlasMemory) != VK_SUCCESS ||
        p_vkBindImageMemory(g_device, g_overlayAtlasImage,
                            g_overlayAtlasMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_overlayAtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                           VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(g_device, &viewInfo, nullptr,
                            &g_overlayAtlasView) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (p_vkCreateSampler(g_device, &samplerInfo, nullptr,
                          &g_overlaySampler) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo descriptorAllocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptorAllocate.descriptorPool = g_overlayDescriptorPool;
    descriptorAllocate.descriptorSetCount = 1;
    descriptorAllocate.pSetLayouts = &g_overlaySetLayout;
    if (p_vkAllocateDescriptorSets(g_device, &descriptorAllocate,
                                   &g_overlayDescriptor) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo sampledImage{};
    sampledImage.imageView = g_overlayAtlasView;
    sampledImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo sampler{};
    sampler.sampler = g_overlaySampler;
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = g_overlayDescriptor;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &sampledImage;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_overlayDescriptor;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &sampler;
    p_vkUpdateDescriptorSets(g_device, static_cast<uint32_t>(writes.size()),
                             writes.data(), 0, nullptr);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_overlayVs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_overlayPs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(OverlayVertex);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<uint32_t>(offsetof(OverlayVertex, position))};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<uint32_t>(offsetof(OverlayVertex, uv))};
    attributes[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<uint32_t>(offsetof(OverlayVertex, color))};
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                     VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT |
                                     VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    constexpr std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g_swapFormat;
    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = g_overlayPipelineLayout;
    if (p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1,
                                    &pipelineInfo, nullptr,
                                    &g_overlayPipeline) != VK_SUCCESS)
        return false;

    const VkDeviceSize uploadAt = UploadAlloc(g_overlayAtlasBytes.size(), 16);
    if (uploadAt == VK_WHOLE_SIZE)
        return false;
    std::memcpy(g_uploadMapped + uploadAt, g_overlayAtlasBytes.data(),
                g_overlayAtlasBytes.size());
    VkImageMemoryBarrier toUpload{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toUpload.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toUpload.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toUpload.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toUpload.srcQueueFamilyIndex = toUpload.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    toUpload.image = g_overlayAtlasImage;
    toUpload.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &toUpload);
    VkBufferImageCopy atlasCopy{};
    atlasCopy.bufferOffset = uploadAt;
    atlasCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    atlasCopy.imageExtent = {kOverlayAtlasWidth, kOverlayAtlasHeight, 1};
    p_vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer,
                             g_overlayAtlasImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &atlasCopy);
    VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = toSample.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    toSample.image = g_overlayAtlasImage;
    toSample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toSample);

    KLOG("Vulkan debug overlay ready\n");
    g_overlayResourcesReady = true;
    g_overlayAtlasBytes.clear();
    g_overlayAtlasBytes.shrink_to_fit();
    g_overlayVsSpv.clear();
    g_overlayVsSpv.shrink_to_fit();
    g_overlayPsSpv.clear();
    g_overlayPsSpv.shrink_to_fit();
    return true;
}

float DebugOverlayTextWidth(const char* text, float scale)
{
    float width = 0.0f;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         p && *p; ++p)
    {
        const uint32_t code = (*p >= kOverlayFirstGlyph && *p <= kOverlayLastGlyph)
            ? *p : uint32_t('?');
        width += g_overlayGlyphs[code - kOverlayFirstGlyph].advance * scale;
    }
    return width;
}

void AppendDebugOverlayText(std::vector<OverlayVertex>& vertices,
                            const char* text, float x, float y, float scale,
                            const std::array<float, 4>& color)
{
    if (!text || !*text || !g_outputExtent.width || !g_outputExtent.height)
        return;
    auto appendVertex = [&](float px, float py, float u, float v) {
        OverlayVertex vertex{};
        vertex.position[0] = px * 2.0f / float(g_outputExtent.width) - 1.0f;
        vertex.position[1] = py * 2.0f / float(g_outputExtent.height) - 1.0f;
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        std::copy(color.begin(), color.end(), vertex.color);
        vertices.push_back(vertex);
    };

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p; ++p)
    {
        const uint32_t code = (*p >= kOverlayFirstGlyph && *p <= kOverlayLastGlyph)
            ? *p : uint32_t('?');
        const auto& glyph = g_overlayGlyphs[code - kOverlayFirstGlyph];
        if (code != uint32_t(' '))
        {
            const float x1 = x + float(kOverlayCellSize) * scale;
            const float y1 = y + float(kOverlayCellSize) * scale;
            appendVertex(x,  y,  glyph.u0, glyph.v0);
            appendVertex(x1, y,  glyph.u1, glyph.v0);
            appendVertex(x1, y1, glyph.u1, glyph.v1);
            appendVertex(x,  y,  glyph.u0, glyph.v0);
            appendVertex(x1, y1, glyph.u1, glyph.v1);
            appendVertex(x,  y1, glyph.u0, glyph.v1);
        }
        x += glyph.advance * scale;
    }
}

std::array<float, 4> SubtitleSpeakerColor(std::string_view speaker)
{
    // Speaker-specific colors can be added here later. Unknown/new speakers
    // deliberately fall back to the MojoRecomp blue so untranslated or newly
    // catalogued dialogue always has a coherent default treatment.
    (void)speaker;
    return {92.0f / 255.0f, 202.0f / 255.0f, 1.0f, 1.0f};
}

void AppendOutlinedSubtitleText(std::vector<OverlayVertex>& vertices,
                                const char* text, float x, float y, float scale,
                                float resolutionScale,
                                const std::array<float, 4>& color)
{
    if (!text || !*text)
        return;

    const std::array<float, 4> outline{0.02f, 0.025f, 0.02f, 0.97f};
    const std::array<float, 4> shadow{0.0f, 0.0f, 0.0f, 0.72f};
    const float radius = std::max(1.5f, 3.0f * resolutionScale);
    const float shadowOffset = std::max(2.0f, 4.0f * resolutionScale);

    // A separate drop shadow gives the subtitle a little depth while the
    // eight-direction outline keeps it readable over Crash's very bright,
    // saturated scenery without needing a backing plate.
    AppendDebugOverlayText(vertices, text, x + shadowOffset, y + shadowOffset,
                           scale, shadow);
    static constexpr std::array<std::array<float, 2>, 8> directions{{
        {{-1.0f,  0.0f}}, {{ 1.0f,  0.0f}}, {{ 0.0f, -1.0f}}, {{ 0.0f,  1.0f}},
        {{-0.70710678f, -0.70710678f}}, {{ 0.70710678f, -0.70710678f}},
        {{-0.70710678f,  0.70710678f}}, {{ 0.70710678f,  0.70710678f}},
    }};
    for (const auto& direction : directions)
    {
        AppendDebugOverlayText(vertices, text,
                               x + direction[0] * radius,
                               y + direction[1] * radius,
                               scale, outline);
    }
    AppendDebugOverlayText(vertices, text, x, y, scale, color);
}

std::vector<std::string> WrapOverlayText(std::string_view text, float scale,
                                         float maxWidth)
{
    std::vector<std::string> lines;
    std::string current;
    size_t position = 0;
    while (position < text.size())
    {
        while (position < text.size() && text[position] == ' ')
            ++position;
        if (position >= text.size())
            break;
        const size_t end = text.find(' ', position);
        const std::string_view word = text.substr(
            position, end == std::string_view::npos ? text.size() - position : end - position);
        std::string candidate = current;
        if (!candidate.empty())
            candidate.push_back(' ');
        candidate.append(word);
        if (!current.empty() && DebugOverlayTextWidth(candidate.c_str(), scale) > maxWidth)
        {
            lines.push_back(std::move(current));
            current.assign(word);
        }
        else
            current = std::move(candidate);
        position = end == std::string_view::npos ? text.size() : end + 1u;
    }
    if (!current.empty())
        lines.push_back(std::move(current));
    return lines;
}

bool DrawDebugOverlay(uint32_t imageIndex, VkImageLayout currentLayout)
{
    const auto snapshot = mojorecomp::debug::GetOverlaySnapshot();
    const auto text = mojorecomp::debug::BuildOverlayText(snapshot);
    const auto subtitle = mojorecomp::subtitles::GetSnapshot();
    const bool subtitleVisible = subtitle.visible && subtitle.text[0];
    if ((!text.visible && !subtitleVisible) || imageIndex >= g_swapViews.size() ||
        !g_swapViews[imageIndex])
        return false;
    if (!EnsureDebugOverlayResources())
        return false;

    const float scale = std::clamp(float(g_outputExtent.height) / 1080.0f,
                                   0.75f, 2.0f);
    const float margin = 18.0f * scale;
    const float glyphHeight = float(kOverlayCellSize) * scale;
    const float lineAdvance = 25.0f * scale;
    const std::array<float, 4> shadow{0.0f, 0.0f, 0.0f, 0.82f};
    const std::array<float, 4> watermark{0.82f, 0.92f, 1.0f, 0.92f};
    const std::array<float, 4> foreground{0.96f, 0.97f, 1.0f, 0.96f};
    const float shadowOffset = std::max(1.0f, 1.5f * scale);

    std::vector<OverlayVertex> vertices;
    vertices.reserve(4096);
    const float leftY = float(g_outputExtent.height) - margin - glyphHeight;
    if (text.visible)
    {
        AppendDebugOverlayText(vertices, text.left.data(), margin + shadowOffset,
                               leftY + shadowOffset, scale, shadow);
        AppendDebugOverlayText(vertices, text.left.data(), margin, leftY,
                               scale, watermark);
    }

    const float bottomY = float(g_outputExtent.height) - margin - glyphHeight;
    if (text.visible)
    {
        for (uint32_t i = 0; i < text.rightCount; ++i)
        {
            const char* line = text.right[i].data();
            const float width = DebugOverlayTextWidth(line, scale);
            const float x = std::max(margin,
                float(g_outputExtent.width) - margin - width);
            const float y = bottomY -
                float(text.rightCount - 1u - i) * lineAdvance;
            AppendDebugOverlayText(vertices, line, x + shadowOffset,
                                   y + shadowOffset, scale, shadow);
            AppendDebugOverlayText(vertices, line, x, y, scale, foreground);
        }
    }

    if (subtitleVisible)
    {
        // Subtitle styling is intentionally independent from the debug overlay.
        // The selected "Mojo Blue" treatment targets a native ~32 px Segoe UI
        // Semibold appearance at 1080p and scales from output height. Width is
        // constrained to a centered 16:9-safe region so 21:9 output gains image
        // at the sides instead of turning dialogue into extremely long lines.
        const float subtitleResolutionScale = std::clamp(
            float(g_outputExtent.height) / 1080.0f, 0.60f, 2.50f);
        const float subtitleScale = 1.60f * subtitleResolutionScale;
        const float subtitleGlyphHeight = float(kOverlayCellSize) * subtitleScale;
        const float safeWidth = std::min(float(g_outputExtent.width),
                                         float(g_outputExtent.height) * (16.0f / 9.0f));
        const float subtitleMaxWidth = safeWidth * 0.74f;
        const float subtitleAdvance = 26.0f * subtitleScale;
        const float subtitleBottom = float(g_outputExtent.height) -
            112.0f * subtitleResolutionScale - subtitleGlyphHeight;
        const std::array<float, 4> subtitleForeground{
            248.0f / 255.0f, 251.0f / 255.0f, 1.0f, 1.0f};
        const std::string speakerPrefix = subtitle.speaker[0]
            ? std::string(subtitle.speaker.data()) + ": " : std::string{};
        const auto speakerColor = SubtitleSpeakerColor(subtitle.speaker.data());

        std::string combined;
        if (subtitle.speaker[0])
        {
            combined.assign(subtitle.speaker.data());
            combined.append(": ");
        }
        combined.append(subtitle.text.data());
        const auto lines = WrapOverlayText(combined, subtitleScale, subtitleMaxWidth);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const auto& line = lines[i];
            const float width = DebugOverlayTextWidth(line.c_str(), subtitleScale);
            const float x = (float(g_outputExtent.width) - width) * 0.5f;
            const float y = subtitleBottom -
                float(lines.size() - 1u - i) * subtitleAdvance;

            if (i == 0 && !speakerPrefix.empty() &&
                line.size() >= speakerPrefix.size() &&
                line.compare(0, speakerPrefix.size(), speakerPrefix) == 0)
            {
                const std::string body = line.substr(speakerPrefix.size());
                const float prefixWidth = DebugOverlayTextWidth(
                    speakerPrefix.c_str(), subtitleScale);
                AppendOutlinedSubtitleText(vertices, speakerPrefix.c_str(), x, y,
                                           subtitleScale, subtitleResolutionScale,
                                           speakerColor);
                if (!body.empty())
                {
                    AppendOutlinedSubtitleText(vertices, body.c_str(), x + prefixWidth, y,
                                               subtitleScale, subtitleResolutionScale,
                                               subtitleForeground);
                }
            }
            else
            {
                AppendOutlinedSubtitleText(vertices, line.c_str(), x, y,
                                           subtitleScale, subtitleResolutionScale,
                                           subtitleForeground);
            }
        }
    }
    if (vertices.empty())
        return false;

    const VkDeviceSize vertexBytes = vertices.size() * sizeof(OverlayVertex);
    const VkDeviceSize vertexAt = UploadAlloc(vertexBytes, 16);
    if (vertexAt == VK_WHOLE_SIZE)
        return false;
    std::memcpy(g_uploadMapped + vertexAt, vertices.data(),
                static_cast<size_t>(vertexBytes));

    VkImageMemoryBarrier toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toAttachment.srcAccessMask = currentLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        : VK_ACCESS_TRANSFER_WRITE_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = currentLayout;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = toAttachment.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = g_images[imageIndex];
    toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           currentLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                               : VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toAttachment);

    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = g_swapViews[imageIndex];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, g_outputExtent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    p_vkCmdBeginRendering(g_commandBuffer, &rendering);

    CmdBindGraphicsPipelineCached(g_commandBuffer, g_overlayPipeline);
    VkViewport viewport{};
    viewport.width = float(g_outputExtent.width);
    viewport.height = float(g_outputExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    CmdSetViewportCached(g_commandBuffer, viewport);
    const VkRect2D scissor{{0, 0}, g_outputExtent};
    CmdSetScissorCached(g_commandBuffer, scissor);
    CmdBindDescriptorSetsCached(g_commandBuffer, g_overlayPipelineLayout,
                                0, 1, &g_overlayDescriptor);
    const VkDeviceSize vertexOffset = vertexAt;
    p_vkCmdBindVertexBuffers(g_commandBuffer, 0, 1,
                             &g_uploadBuffer, &vertexOffset);
    p_vkCmdDraw(g_commandBuffer, static_cast<uint32_t>(vertices.size()),
                1, 0, 0);
    p_vkCmdEndRendering(g_commandBuffer);

    static bool reported = false;
    if (!reported)
    {
        KLOG("Vulkan debug overlay first draw: vertices=%zu extent=%ux%u scale=%.2f\n",
             vertices.size(), g_outputExtent.width, g_outputExtent.height, scale);
        reported = true;
    }
    return true;
}

long JsonIntField(std::string_view obj, const char* key, long fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = obj.find(needle);
    if (p == std::string_view::npos)
        return fallback;
    p = obj.find(':', p + needle.size());
    if (p == std::string_view::npos)
        return fallback;
    ++p;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\r' || obj[p] == '\n' || obj[p] == '\t'))
        ++p;
    bool neg = false;
    if (p < obj.size() && obj[p] == '-')
    {
        neg = true;
        ++p;
    }
    long v = 0;
    bool any = false;
    while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9')
    {
        any = true;
        v = v * 10 + (obj[p++] - '0');
    }
    return any ? (neg ? -v : v) : fallback;
}

bool JsonArrayNonEmpty(const std::string& text, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = text.find(needle);
    if (p == std::string::npos)
        return false;
    const size_t a = text.find('[', p);
    const size_t b = a == std::string::npos ? std::string::npos : text.find(']', a);
    if (a == std::string::npos || b == std::string::npos)
        return false;
    for (size_t i = a + 1; i < b; ++i)
        if (text[i] != ' ' && text[i] != '\r' && text[i] != '\n' && text[i] != '\t')
            return true;
    return false;
}

std::vector<uint32_t> JsonUintArray(const std::string& text, const char* key)
{
    std::vector<uint32_t> values;
    size_t at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos || (at = text.find('[', at)) == std::string::npos)
        return values;
    ++at;
    while (at < text.size())
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' ||
               text[at] == '\r' || text[at] == '\t')) ++at;
        if (at < text.size() && text[at] == ']') return values;
        uint32_t value = 0;
        const size_t start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9')
        {
            if (value > 1000) return {};
            value = value * 10 + uint32_t(text[at++] - '0');
        }
        if (at == start) return {};
        values.push_back(value);
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' ||
               text[at] == '\r' || text[at] == '\t')) ++at;
        if (at < text.size() && text[at] == ']') return values;
        if (at == text.size() || text[at++] != ',') return {};
    }
    return {};
}

void ParseShaderMeta(const std::string& text, ShaderModuleRec& out)
{
    out.usesTextures = JsonArrayNonEmpty(text, "tfetchConsts");
    out.textureSlots = JsonUintArray(text, "tfetchConsts");
    out.textureDimensions = JsonUintArray(text, "tfetchDims");
    out.aluConsts = JsonUintArray(text, "aluConsts");
    out.aluDynamic = text.find("\"aluDynamic\": true") != std::string::npos;
    out.usesAlu = !out.aluConsts.empty() || out.aluDynamic;
    const size_t at = text.find("\"attributes\"");
    if (at == std::string::npos)
        return;
    const size_t arrayEnd = text.find(']', at);
    size_t cursor = text.find('[', at);
    while (cursor != std::string::npos && cursor < arrayEnd)
    {
        const size_t open = text.find('{', cursor);
        if (open == std::string::npos || open > arrayEnd)
            break;
        const size_t close = text.find('}', open);
        if (close == std::string::npos || close > arrayEnd)
            break;
        const std::string_view obj(text.data() + open, close - open + 1);
        VertexAttribute a;
        a.location = static_cast<int32_t>(JsonIntField(obj, "location", -1));
        a.fetchSlot = static_cast<uint32_t>(JsonIntField(obj, "fetchSlot", 0));
        a.format = static_cast<uint32_t>(JsonIntField(obj, "format", 0));
        a.isSigned = static_cast<uint32_t>(JsonIntField(obj, "signed", 0));
        a.isInteger = static_cast<uint32_t>(JsonIntField(obj, "integer", 0));
        a.strideDwords = static_cast<uint32_t>(JsonIntField(obj, "strideDwords", 0));
        a.offsetDwords = static_cast<uint32_t>(JsonIntField(obj, "offsetDwords", 0));
        a.indirect = static_cast<uint32_t>(JsonIntField(obj, "indirect", 0));
        out.attributes.push_back(a);
        cursor = close + 1;
    }
}

ShaderModuleRec* GetModule(uint32_t type, uint64_t hash)
{
    for (auto& m : g_modules)
        if (m.type == type && m.hash == hash)
            return &m;
    const MojoTranslatedShader* translated = ShaderCache_Find(type, hash);
    if (!translated || translated->spirv.empty())
        return nullptr;

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = translated->spirv.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(translated->spirv.data());
    ShaderModuleRec rec{};
    rec.type = type;
    rec.hash = hash;
    if (p_vkCreateShaderModule(g_device, &ci, nullptr, &rec.module) != VK_SUCCESS)
        return nullptr;
    ParseShaderMeta(translated->metaJson, rec);
    if (type == 1)
    {
        rec.writesDepth = translated->hlsl.find("SV_Depth") != std::string::npos;
        for (uint32_t target = 0; target < 4; ++target)
        {
            const std::string semantic = "SV_Target" + std::to_string(target);
            if (translated->hlsl.find(semantic) != std::string::npos)
                rec.colorOutputMask |= 1u << target;
        }
        // A successfully translated color pixel shader historically always had
        // Target0. Keep that behavior if a translator spelling ever changes, but
        // report real MRT exports when they are present so the host can bind RT1.
        if (!rec.colorOutputMask)
            rec.colorOutputMask = 1u;
        if (rec.colorOutputMask & ~1u)
            KLOG("Vulkan PS MRT exports: PS=%016llX targets=%X\n",
                 static_cast<unsigned long long>(hash), rec.colorOutputMask);
    }
    g_modules.push_back(std::move(rec));
    return &g_modules.back();
}

VkShaderModule GetFloat24DepthModule(ShaderModuleRec& ps)
{
    if (ps.type != 1)
        return VK_NULL_HANDLE;
    if (ps.float24DepthModule)
        return ps.float24DepthModule;
    if (ps.float24DepthModuleAttempted)
        return VK_NULL_HANDLE;
    ps.float24DepthModuleAttempted = true;

    const MojoTranslatedShader* translated = ShaderCache_Find(1, ps.hash);
    if (!translated || translated->hlsl.empty())
        return VK_NULL_HANDLE;

    std::string hlsl = "#define MOJORECOMP_FLOAT24_DEPTH 1\n";
    hlsl += translated->hlsl;
    std::vector<uint8_t> spirv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(hlsl, false, spirv, err))
    {
        KLOG("Vulkan float24 depth PS compile failed PS=%016llX: %s\n",
             static_cast<unsigned long long>(ps.hash), err.c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    if (p_vkCreateShaderModule(g_device, &ci, nullptr,
                              &ps.float24DepthModule) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    KLOG("Vulkan float24 depth PS variant ready: PS=%016llX explicitDepth=%u\n",
         static_cast<unsigned long long>(ps.hash), ps.writesDepth ? 1u : 0u);
    return ps.float24DepthModule;
}

VkShaderModule GetFloat24DepthOnlyModule()
{
    if (g_float24DepthOnlyPs)
        return g_float24DepthOnlyPs;
    if (g_float24DepthOnlyPsAttempted)
        return VK_NULL_HANDLE;
    g_float24DepthOnlyPsAttempted = true;

    static constexpr const char* kPs = R"(
float MojoQuantizeFloat24Depth(float hostDepth)
{
    uint f32 = asuint(saturate(hostDepth * 2.0f));
    uint f24;
    if (f32 < 0x38800000u)
    {
        uint shift = min(113u - (f32 >> 23u), 24u);
        f24 = ((f32 & 0x7FFFFFu) | 0x800000u) >> shift;
    }
    else
    {
        f24 = f32 + 0xC8000000u;
    }
    f24 += 3u + ((f24 >> 3u) & 1u);
    f24 = (f24 >> 3u) & 0xFFFFFFu;
    if (f24 == 0u)
        return 0.0f;

    uint mantissa = f24 & 0xFFFFFu;
    int exponent = int(f24 >> 20u);
    if (exponent == 0)
    {
        int highest = firstbithigh(mantissa);
        uint shift = uint(20 - highest);
        exponent = 1 - int(shift);
        mantissa = (mantissa << shift) & 0xFFFFFu;
    }
    float guestDepth = asfloat((uint(exponent + 112) << 23u) | (mantissa << 3u));
    return guestDepth * 0.5f;
}

float main(sample float4 position : SV_Position) : SV_Depth
{
    return MojoQuantizeFloat24Depth(position.z);
}
)";

    std::vector<uint8_t> spirv;
    std::string err;
    if (!ShaderTranslator::CompileHostHlsl(kPs, false, spirv, err))
    {
        KLOG("Vulkan DepthOnly float24 PS compile failed: %s\n", err.c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    if (p_vkCreateShaderModule(g_device, &ci, nullptr, &g_float24DepthOnlyPs) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    KLOG("Vulkan DepthOnly float24 PS ready (guest pixel shader bypassed)\n");
    return g_float24DepthOnlyPs;
}

bool UseFloat24DepthShader(uint32_t depthControl, bool depthFloat24,
                           VkSampleCountFlagBits samples)
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_FLOAT24_DEPTH_PS");
        return value && value[0] && value[0] != '0';
    }();
    if (!enabled || (depthControl & (1u << 1)) == 0 || !depthFloat24)
        return false;
    if (samples == VK_SAMPLE_COUNT_1_BIT)
        return true;
    return g_sampleRateShading;
}

VkFormat VertexFormat(const VertexAttribute& a)
{
    switch (a.format)
    {
        case 6:
            if (a.isInteger)
                return a.isSigned ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED;
            return a.isSigned ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
        case 25:
            if (a.isInteger)
                return a.isSigned ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED;
            return a.isSigned ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
        case 26:
            if (a.isInteger)
                return a.isSigned ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_USCALED;
            return a.isSigned ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
        case 31: return VK_FORMAT_R16G16_SFLOAT;
        case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 33: return a.isSigned ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
        case 36: return VK_FORMAT_R32_SFLOAT;
        case 37: return VK_FORMAT_R32G32_SFLOAT;
        case 57: return VK_FORMAT_R32G32B32_SFLOAT;
        case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool FloatVertexFormat(uint32_t format)
{
    return format == 31 || format == 32 || format == 36 || format == 37 ||
           format == 57 || format == 38;
}

VkPrimitiveTopology PrimitiveTopology(uint32_t prim)
{
    switch (prim)
    {
        case xenos::kPointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case xenos::kLineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case xenos::kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case xenos::kTriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case xenos::kTriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case xenos::kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case xenos::kRectangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

uint64_t AttributeLayoutHash(const ShaderModuleRec& vs)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (const auto& a : vs.attributes)
    {
        if (a.location < 0 || a.indirect)
            continue;
        const uint64_t words[] = {uint64_t(uint32_t(a.location)), a.fetchSlot, a.format,
                                  a.isSigned, a.isInteger, a.strideDwords, a.offsetDwords};
        for (uint64_t v : words)
            h = (h ^ v) * 0x100000001B3ull;
    }
    return h;
}

VkCompareOp XenosCompareOp(uint32_t op)
{
    switch (op & 7u)
    {
        case 0: return VK_COMPARE_OP_NEVER;
        case 1: return VK_COMPARE_OP_LESS;
        case 2: return VK_COMPARE_OP_EQUAL;
        case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4: return VK_COMPARE_OP_GREATER;
        case 5: return VK_COMPARE_OP_NOT_EQUAL;
        case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

uint32_t NormalizeDepthControl(uint32_t mode, uint32_t depthControl)
{
    // Xenos ignores depth/stencil outside ColorDepth (4) and DepthOnly (5)
    // EDRAM modes. Match Xenia's normalized depth state so stale register bits
    // from copy/resolve modes don't accidentally affect host rasterization.
    if (mode != 4u && mode != 5u)
        return 0;

    // Z test enabled, no writes, ALWAYS compare is equivalent to no Z test.
    if ((depthControl & (1u << 1)) && !(depthControl & (1u << 2)) &&
        (((depthControl >> 4) & 7u) == 7u))
        depthControl &= ~(1u << 1);

    // Stencil-disabled pipelines only care about the depth-test fields. Likewise,
    // when separate back-face stencil is disabled, Xenos ignores all back-face
    // compare/op fields. Clear those don't-care bits so semantically identical
    // guest states share one Vulkan pipeline instead of creating duplicates.
    if (!(depthControl & 1u))
        depthControl &= 0x0000007Eu;
    else if (!(depthControl & (1u << 7)))
        depthControl &= 0x000FFFFFu;
    return depthControl;
}

bool PrimitiveSupportsRestart(uint32_t prim)
{
    return prim == xenos::kLineStrip || prim == xenos::kTriangleFan ||
           prim == xenos::kTriangleStrip;
}

bool PrimitiveRestartEnabled(const Pm4Draw& draw, const uint32_t* regs)
{
    return draw.indexed && PrimitiveSupportsRestart(draw.primType) &&
           (regs[xenos::kVgtMultiPrimIbResetEn] & 1u) != 0;
}

VkBlendFactor XenosBlendFactor(uint32_t factor)
{
    switch (factor & 0x1Fu)
    {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        // Reserved/unknown Xenos encodings are normalized to ZERO by Xenia.
        // Do not let them silently become ONE, which can turn an otherwise
        // harmless blend equation into an additive/opaque-looking pass.
        case 2:
        case 3: return VK_BLEND_FACTOR_ZERO;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendFactor XenosAlphaBlendFactor(uint32_t factor)
{
    // On Xenos the alpha channel is scalar. COLOR-family factors selected for
    // the alpha equation are interpreted as their ALPHA-family equivalents.
    // D3D12/Xenia models this explicitly; using the RGB factors verbatim in
    // Vulkan produces the wrong destination alpha for screen-space passes.
    switch (factor & 0x1Fu)
    {
        case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default: return XenosBlendFactor(factor);
    }
}

VkBlendOp XenosBlendOp(uint32_t op)
{
    switch (op & 7u)
    {
        case 0: return VK_BLEND_OP_ADD;
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}

VkStencilOp XenosStencilOp(uint32_t op)
{
    switch (op & 7u)
    {
        case 0: return VK_STENCIL_OP_KEEP;
        case 1: return VK_STENCIL_OP_ZERO;
        case 2: return VK_STENCIL_OP_REPLACE;
        case 3: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 4: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 5: return VK_STENCIL_OP_INVERT;
        case 6: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
}

VkPipeline GetPipeline(const ShaderModuleRec& vs, ShaderModuleRec& ps,
                       uint32_t prim, uint32_t colorMask, uint32_t blendControl0,
                       uint32_t blendControl1,
                       uint32_t depthControl, uint32_t rasterState,
                       uint32_t alphaTest, uint32_t mode, bool primitiveRestart,
                       VkSampleCountFlagBits samples, VkFormat depthFormat,
                       bool depthFloat24)
{
    const bool depthOnly = mode == 5;
    const uint64_t pipelinePsHash = depthOnly ? 0ull : ps.hash;
    const uint32_t effectiveAlphaTest = depthOnly ? 0u : alphaTest;
    const VkPrimitiveTopology topology = PrimitiveTopology(prim);
    if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
        return VK_NULL_HANDLE;
    const uint64_t layoutHash = AttributeLayoutHash(vs);
    for (const auto& p : g_pipelines)
        if (p.vs == vs.hash && p.ps == pipelinePsHash && p.prim == prim &&
            p.colorMask == colorMask && p.blendControl0 == blendControl0 &&
            p.blendControl1 == blendControl1 &&
            p.depthControl == depthControl && p.rasterState == rasterState &&
            p.alphaTest == effectiveAlphaTest && p.mode == mode &&
            p.primitiveRestart == primitiveRestart && p.samples == samples &&
            p.depthFormat == depthFormat && p.depthFloat24 == depthFloat24 &&
            p.layoutHash == layoutHash)
            return p.pipeline;

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;
    uint32_t binding = 0;
    for (const auto& a : vs.attributes)
    {
        if (a.location < 0 || a.indirect)
            continue;
        const VkFormat format = VertexFormat(a);
        if (format == VK_FORMAT_UNDEFINED || !a.strideDwords)
            return VK_NULL_HANDLE;
        bindings.push_back({binding, a.strideDwords * 4, VK_VERTEX_INPUT_RATE_VERTEX});
        attrs.push_back({static_cast<uint32_t>(a.location), binding, format, 0});
        ++binding;
    }

    const bool float24DepthShader =
        UseFloat24DepthShader(depthControl, depthFloat24, samples);
    VkShaderModule pixelModule = VK_NULL_HANDLE;
    if (depthOnly)
    {
        if (float24DepthShader)
        {
            pixelModule = GetFloat24DepthOnlyModule();
            if (!pixelModule)
                return VK_NULL_HANDLE;
        }
    }
    else
    {
        pixelModule = ps.module;
        if (float24DepthShader)
        {
            pixelModule = GetFloat24DepthModule(ps);
            if (!pixelModule)
                return VK_NULL_HANDLE;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.module;
    stages[0].pName = "main";
    uint32_t stageCount = 1;
    if (pixelModule)
    {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = pixelModule;
        stages[1].pName = "main";
        stageCount = 2;
    }
    const uint32_t specValue = effectiveAlphaTest
        ? (0x2u | (((effectiveAlphaTest - 1u) & 7u) << 8))
        : 0u;
    const VkSpecializationMapEntry specMap{0, 0, sizeof(uint32_t)};
    VkSpecializationInfo specInfo{};
    specInfo.mapEntryCount = 1;
    specInfo.pMapEntries = &specMap;
    specInfo.dataSize = sizeof(specValue);
    specInfo.pData = &specValue;
    if (effectiveAlphaTest && pixelModule)
        stages[1].pSpecializationInfo = &specInfo;

    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertex.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input.topology = topology;
    input.primitiveRestartEnable = primitiveRestart ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    const uint32_t suScMode = rasterState & 7u;
    const bool polygonal = prim == xenos::kTriangleList || prim == xenos::kTriangleFan ||
                           prim == xenos::kTriangleStrip;

    // Front-facing state matters even when culling is disabled because guest
    // pixel shaders may consume SV_IsFrontFace and stencil has separate front /
    // back state. With the positive-height host viewport, Xenos face maps
    // directly to Vulkan as in the reference renderer.
    const bool guestFrontClockwise = (suScMode & 4u) != 0;
    raster.frontFace = guestFrontClockwise ? VK_FRONT_FACE_CLOCKWISE
                                           : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.cullMode = VK_CULL_MODE_NONE;
    if (polygonal)
    {
        if (suScMode & 1u) raster.cullMode |= VK_CULL_MODE_FRONT_BIT;
        if (suScMode & 2u) raster.cullMode |= VK_CULL_MODE_BACK_BIT;
    }
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    if (float24DepthShader && samples != VK_SAMPLE_COUNT_1_BIT)
    {
        // Xenos depth is per sample. Converting interpolated Z to float24 in a
        // pixel shader must therefore also run per sample under MSAA.
        ms.sampleShadingEnable = VK_TRUE;
        ms.minSampleShading = 1.0f;
    }
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = ((depthControl >> 1) & 1u) ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = ((depthControl >> 2) & 1u) ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = XenosCompareOp((depthControl >> 4) & 7u);
    depth.minDepthBounds = 0.0f;
    depth.maxDepthBounds = 1.0f;
    depth.stencilTestEnable = (depthControl & 1u) ? VK_TRUE : VK_FALSE;
    if (depth.stencilTestEnable)
    {
        depth.front.compareOp = XenosCompareOp((depthControl >> 8) & 7u);
        depth.front.failOp = XenosStencilOp((depthControl >> 11) & 7u);
        depth.front.passOp = XenosStencilOp((depthControl >> 14) & 7u);
        depth.front.depthFailOp = XenosStencilOp((depthControl >> 17) & 7u);
        if (mojorecomp::gpu::UsesSeparateBackfaceStencil(polygonal, depthControl))
        {
            depth.back.compareOp = XenosCompareOp((depthControl >> 20) & 7u);
            depth.back.failOp = XenosStencilOp((depthControl >> 23) & 7u);
            depth.back.passOp = XenosStencilOp((depthControl >> 26) & 7u);
            depth.back.depthFailOp = XenosStencilOp((depthControl >> 29) & 7u);
        }
        else
            depth.back = depth.front;
    }
    std::array<VkPipelineColorBlendAttachmentState, 2> blends{};
    for (uint32_t target = 0; target < blends.size(); ++target)
    {
        auto& blend = blends[target];
        const uint32_t targetMask = (colorMask >> (target * 4)) & 0xFu;
        const uint32_t blendControl = target == 0 ? blendControl0 : blendControl1;
        if (mode == 4)
        {
            if (targetMask & 1) blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
            if (targetMask & 2) blend.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
            if (targetMask & 4) blend.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
            if (targetMask & 8) blend.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
        }
        blend.srcColorBlendFactor = XenosBlendFactor(blendControl & 0x1Fu);
        blend.colorBlendOp = XenosBlendOp((blendControl >> 5) & 7u);
        blend.dstColorBlendFactor = XenosBlendFactor((blendControl >> 8) & 0x1Fu);
        blend.srcAlphaBlendFactor = XenosAlphaBlendFactor((blendControl >> 16) & 0x1Fu);
        blend.alphaBlendOp = XenosBlendOp((blendControl >> 21) & 7u);
        blend.dstAlphaBlendFactor = XenosAlphaBlendFactor((blendControl >> 24) & 0x1Fu);
        const bool identityBlend =
            blend.srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
            blend.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
            blend.colorBlendOp == VK_BLEND_OP_ADD &&
            blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
            blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
            blend.alphaBlendOp == VK_BLEND_OP_ADD;
        blend.blendEnable = (mode == 4 && targetMask && !identityBlend) ? VK_TRUE : VK_FALSE;
    }
    VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blendState.attachmentCount = static_cast<uint32_t>(blends.size());
    blendState.pAttachments = blends.data();
    VkDynamicState dynStates[6] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    uint32_t dynCount = 3;
    if (depth.stencilTestEnable)
    {
        dynStates[dynCount++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
        dynStates[dynCount++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
        dynStates[dynCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    }
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = dynCount;
    dynamic.pDynamicStates = dynStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const std::array<VkFormat, 2> colorFormats{g_swapFormat, g_swapFormat};
    rendering.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
    rendering.pColorAttachmentFormats = colorFormats.data();
    rendering.depthAttachmentFormat = depthFormat;
    rendering.stencilAttachmentFormat = depthFormat;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = stageCount;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex;
    ci.pInputAssemblyState = &input;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blendState;
    ci.pDynamicState = &dynamic;
    ci.layout = g_pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = p_vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &ci, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan pipeline FAILED (%d) VS=%016llX PS=%016llX prim=%u attrs=%zu\n",
             result, static_cast<unsigned long long>(vs.hash),
             static_cast<unsigned long long>(ps.hash), prim, attrs.size());
        return VK_NULL_HANDLE;
    }
    g_pipelines.push_back({vs.hash, pipelinePsHash, prim, colorMask, blendControl0, blendControl1, depthControl,
                           rasterState, effectiveAlphaTest, mode, primitiveRestart,
                           samples, depthFormat, depthFloat24, layoutHash, pipeline});
    KLOG_DIAG("Vulkan pipeline created VS=%016llX PS=%016llX prim=%u attrs=%zu mask=%X "
         "blend0=%08X blend1=%08X depth=%08X raster=%03X alphaTest=%u mode=%u restart=%u samples=%u depthFmt=%u\n",
         static_cast<unsigned long long>(vs.hash), static_cast<unsigned long long>(ps.hash),
         prim, attrs.size(), colorMask, blendControl0, blendControl1,
         depthControl, rasterState, effectiveAlphaTest, mode, primitiveRestart ? 1u : 0u,
         uint32_t(samples), uint32_t(depthFormat));
    return pipeline;
}

uint64_t GpuTimestampDelta(uint64_t start, uint64_t end)
{
    if (g_gpuTimestampValidBits >= 64)
        return end - start;
    if (!g_gpuTimestampValidBits)
        return 0;
    const uint64_t mask = (uint64_t(1) << g_gpuTimestampValidBits) - 1u;
    return (end - start) & mask;
}

void ReportGpuDrawTimestampFrame(uint32_t slot, FrameContext& frame)
{
    if (!g_gpuDrawTimestamps || !frame.timestampPool || !frame.timestampPending)
        return;
    const uint32_t drawCount = std::min(frame.timestampDrawCount, kMaxGpuTimedDraws);
    const uint32_t queryCount = 2u + drawCount * 2u;
    std::array<uint64_t, kGpuTimestampQueryCount> timestamps{};
    const VkResult result = p_vkGetQueryPoolResults(
        g_device, frame.timestampPool, 0, queryCount,
        VkDeviceSize(queryCount) * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    frame.timestampPending = false;
    if (result != VK_SUCCESS)
    {
        KLOG("Vulkan GPU draw timestamps frame=%llu unavailable result=%d\n",
             static_cast<unsigned long long>(frame.timestampFrame), result);
        return;
    }

    const double tickToMs = double(g_gpuTimestampPeriodNs) / 1.0e6;
    const double frameMs = double(GpuTimestampDelta(timestamps[0], timestamps[1])) * tickToMs;
    std::vector<std::pair<double, uint32_t>> ranked;
    ranked.reserve(drawCount);
    double drawSumMs = 0.0;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        const uint32_t q = 2u + i * 2u;
        const double ms = double(GpuTimestampDelta(timestamps[q], timestamps[q + 1u])) * tickToMs;
        drawSumMs += ms;
        ranked.emplace_back(ms, i);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    const double topMs = ranked.empty() ? 0.0 : ranked.front().first;
    if (frameMs < 40.0 && topMs < 5.0)
        return;

    KLOG("Vulkan GPU draw timing frame=%llu gpu=%.3f ms timedDraws=%u drawSum=%.3f ms top=%.3f ms\n",
         static_cast<unsigned long long>(frame.timestampFrame), frameMs, drawCount,
         drawSumMs, topMs);
    const uint32_t reportCount = std::min<uint32_t>(static_cast<uint32_t>(ranked.size()), 12u);
    for (uint32_t rank = 0; rank < reportCount; ++rank)
    {
        const uint32_t index = ranked[rank].second;
        const auto& rec = g_gpuDrawTimingRecords[slot][index];
        KLOG("Vulkan GPU draw #%u ordinal=%u %.3f ms VS=%016llX PS=%016llX depth=%08X "
             "bin=%08X tex0=%08X bool128_131=%X mode=%u indexed=%u elements=%u "
             "scissor=%.3f Mpix\n",
             rank + 1u, index, ranked[rank].first,
             static_cast<unsigned long long>(rec.vs),
             static_cast<unsigned long long>(rec.ps), rec.depthControl, rec.binSelect,
             rec.textureKey0, rec.bool128_131, rec.mode, rec.indexed ? 1u : 0u, rec.elements,
             double(rec.scissorPixels) / 1.0e6);
    }
}

bool BeginFrame()
{
    if (g_frameOpen)
        return true;
    auto& frame = g_frameContexts[g_frameSlot];
    g_commandBuffer = frame.commandBuffer;
    g_imageAvailable = frame.imageAvailable;
    g_renderFinished = frame.renderFinished;
    g_fence = frame.fence;
    const auto fenceWaitStart = PerfClock::now();
    const VkResult fenceWaitResult =
        p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
    g_perfFenceWaitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - fenceWaitStart).count());
    if (fenceWaitResult != VK_SUCCESS)
        return false;
    ReportGpuDrawTimestampFrame(g_frameSlot, frame);

    // Temporary lifetime diagnostic: preserve the two frame slots/upload
    // segments but serialize against the immediately previous slot. If this
    // removes the frontend corruption, the slot data itself is valid and an
    // in-flight resource is being reused too early.
    const bool serializeFrames = [] {
        const char* value = std::getenv("MOJORECOMP_SERIALIZE_FRAMES");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (serializeFrames && g_framesInFlight > 1)
    {
        const uint32_t previousSlot = (g_frameSlot + g_framesInFlight - 1u) % g_framesInFlight;
        const VkFence previousFence = g_frameContexts[previousSlot].fence;
        if (previousFence != g_fence &&
            p_vkWaitForFences(g_device, 1, &previousFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
    }

    if (g_readbackPending && g_readbackFrameSlot == g_frameSlot &&
        g_uploadMapped && g_readbackBytes)
    {
        const uint8_t* pixels = g_uploadMapped + g_readbackOffset;
        if (g_frontReadbackFrame)
            ReportFrontPixels(pixels, g_frontReadbackFrame);
        g_frontReadbackFrame = 0;
        const uint64_t pixelCount = uint64_t(g_internalExtent.width) * g_internalExtent.height;
        uint64_t nonClear = 0;
        uint32_t firstX = 0, firstY = 0;
        uint8_t first[4]{};
        for (uint64_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t* p = pixels + i * 4;
            // B8G8R8A8 clear value is approximately {5,4,4,255}. Keep a one-LSB
            // tolerance for the UNORM conversion so this is a raster proof, not a
            // driver-rounding test.
            const bool clear = p[0] >= 4 && p[0] <= 6 &&
                               p[1] >= 3 && p[1] <= 5 &&
                               p[2] >= 3 && p[2] <= 5 && p[3] == 255;
            if (!clear)
            {
                if (!nonClear)
                {
                    firstX = static_cast<uint32_t>(i % g_internalExtent.width);
                    firstY = static_cast<uint32_t>(i / g_internalExtent.width);
                    std::memcpy(first, p, 4);
                }
                ++nonClear;
            }
        }
        ++g_readbackAttempts;
        if (nonClear && !g_readbackReported)
        {
            KLOG("Vulkan raster PROOF: %llu/%llu pixels differ from the frame clear; "
                 "first=(%u,%u) BGRA=%u,%u,%u,%u\n",
                 static_cast<unsigned long long>(nonClear),
                 static_cast<unsigned long long>(pixelCount),
                 firstX, firstY, first[0], first[1], first[2], first[3]);
            g_readbackReported = true;
        }
        else if (g_readbackAttempts <= 3)
            KLOG("Vulkan raster readback #%u: target is still clear (%llu pixels checked)\n",
                 g_readbackAttempts, static_cast<unsigned long long>(pixelCount));
        g_readbackPending = false;
        g_readbackFrameSlot = UINT32_MAX;
    }
    if (p_vkResetCommandBuffer(g_commandBuffer, 0) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(g_commandBuffer, &begin) != VK_SUCCESS)
        return false;
    ResetGraphicsCommandStateCache(g_commandBuffer);
    if (g_gpuDrawTimestamps && frame.timestampPool)
    {
        frame.timestampDrawCount = 0;
        frame.timestampFrame = g_frames.load(std::memory_order_relaxed) + 1u;
        p_vkCmdResetQueryPool(g_commandBuffer, frame.timestampPool, 0, kGpuTimestampQueryCount);
        p_vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              frame.timestampPool, 0);
    }
    // Diagnostic for cross-frame hazards on persistent Xenos resources. The
    // previous experiment only made writes visible to the next frame, but the
    // frontend also reuses images that were read (sampled) by the preceding
    // submission and then overwritten by a resolve. Include reads on the
    // source side so the next writes cannot overtake those samples.
    if (g_framesInFlight > 1)
    {
        const char* barrierEnv = std::getenv("MOJORECOMP_INTERFRAME_BARRIER");
        if (barrierEnv && barrierEnv[0] == '1')
        {
            VkMemoryBarrier interFrame{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            interFrame.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            interFrame.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            p_vkCmdPipelineBarrier(g_commandBuffer,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   0, 1, &interFrame, 0, nullptr, 0, nullptr);
        }
    }

    const VkDeviceSize uploadBase = VkDeviceSize(g_frameSlot) * kFrameUploadBytes;
    g_uploadAt = uploadBase;
    g_uploadLimit = std::min(uploadBase + kFrameUploadBytes,
                             g_readbackOffset ? g_readbackOffset : kUploadBytes);
    if (g_uploadLimit <= uploadBase)
        return false;

    ColorBacking* activeColor0 = ActiveColorBacking();
    if (activeColor0)
    {
        TransitionColorBacking(*activeColor0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (g_hostMsaa2x)
            TransitionColorResolveBacking(*activeColor0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    else
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = g_colorInitialized ? VK_ACCESS_TRANSFER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = g_colorInitialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = g_colorImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(g_commandBuffer,
                               g_colorInitialized ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                               0, nullptr, 0, nullptr, 1, &barrier);
    }
    if (ColorBacking* activeColor1 = ActiveColor1Backing())
    {
        TransitionColorBacking(*activeColor1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (g_hostMsaa2x)
            TransitionColorResolveBacking(*activeColor1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (g_hostMsaa2x && !ActiveColorBacking())
        TransitionResolveTarget(g_colorResolveImage, g_colorResolveLayout,
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    DepthBacking* activeDepth = ActiveDepthBacking();
    const bool depthWasInitialized = activeDepth ? activeDepth->initialized : g_depthInitialized;
    const bool activeDepthMsaaResolve =
        HostMsaa2xDepthResolveEnabled() && activeDepth &&
        activeDepth->samples != VK_SAMPLE_COUNT_1_BIT &&
        activeDepth->resolveImage && activeDepth->resolveView;
    const bool fallbackDepthMsaaResolve =
        HostMsaa2xDepthResolveEnabled() && !activeDepth && g_depthResolveView;
    if (activeDepth)
    {
        TransitionDepthBacking(*activeDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        activeDepth->initialized = true;
    }
    else if (!g_depthInitialized)
    {
        VkImageMemoryBarrier depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        depthBarrier.srcAccessMask = 0;
        depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = g_depthImage;
        depthBarrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        depthBarrier.subresourceRange.levelCount = 1;
        depthBarrier.subresourceRange.layerCount = 1;
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
        g_depthInitialized = true;
    }
    if (activeDepthMsaaResolve)
        TransitionResolveTarget(activeDepth->resolveImage, activeDepth->resolveLayout,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    else if (fallbackDepthMsaaResolve)
        TransitionResolveTarget(g_depthResolveImage, g_depthResolveLayout,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    std::array<VkRenderingAttachmentInfo, 2> attachments{};
    attachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachments[0].imageView = ActiveColorView();
    attachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].loadOp = activeColor0 && activeColor0->initialized
        ? VK_ATTACHMENT_LOAD_OP_LOAD
        : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].clearValue.color = {{0.015f, 0.015f, 0.02f, 1.0f}};
    attachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (ColorBacking* activeColor1 = ActiveColor1Backing())
    {
        attachments[1].imageView = activeColor1->view;
        attachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    else
    {
        attachments[1].imageView = VK_NULL_HANDLE;
        attachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = ActiveDepthView();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = depthWasInitialized ? VK_ATTACHMENT_LOAD_OP_LOAD
                                       : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // The clear is only for a genuinely uninitialized host image. Persistent
    // Xenos EDRAM contents are always loaded across host frame boundaries.
    depth.clearValue.depthStencil = {1.0f, 0};
    if (activeDepthMsaaResolve)
    {
        depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth.resolveImageView = activeDepth->resolveView;
        depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    else if (fallbackDepthMsaaResolve)
    {
        depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth.resolveImageView = g_depthResolveView;
        depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo stencil = depth;
    stencil.resolveMode = VK_RESOLVE_MODE_NONE;
    stencil.resolveImageView = VK_NULL_HANDLE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = g_internalExtent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(attachments.size());
    ri.pColorAttachments = attachments.data();
    ri.pDepthAttachment = &depth;
    ri.pStencilAttachment = &stencil;
    p_vkCmdBeginRendering(g_commandBuffer, &ri);
    g_rendering = true;
    g_frameOpen = true;
    return true;
}

void DecodeVertexWindowOffset(const uint32_t* regs, int32_t& x, int32_t& y,
                              bool ignoreWindowOffset)
{
    x = 0;
    y = 0;
    if (ignoreWindowOffset)
        return;
    if ((regs[xenos::kPaSuScModeCntl] & (1u << 16)) == 0)
        return;
    const uint32_t raw = regs[xenos::kPaScWindowOffset];
    auto signExtend15 = [](uint32_t value) -> int32_t {
        value &= 0x7FFFu;
        return (value & 0x4000u) ? int32_t(value | 0xFFFF8000u) : int32_t(value);
    };
    x = signExtend15(raw);
    y = signExtend15(raw >> 16);
}

VkViewport DecodeViewport(const uint32_t* regs, bool ignoreWindowOffset = false,
                          bool pixelShaderWritesDepth = false,
                          uint32_t normalizedDepthControl = 0)
{
    VkViewport vp{};
    const uint32_t vte = regs[xenos::kPaClVteCntl];
    if (vte & 1u)
    {
        const float xs = F32(regs[xenos::kPaClVportXScale]);
        const float ys = (vte & 4u) ? F32(regs[xenos::kPaClVportYScale]) : 1.0f;
        const float xo = (vte & 2u) ? F32(regs[xenos::kPaClVportXOffset]) : 0.0f;
        const float yo = (vte & 8u) ? F32(regs[xenos::kPaClVportYOffset]) : 0.0f;
        vp.x = xo - std::fabs(xs);
        vp.width = 2.0f * std::fabs(xs);
        vp.y = yo - std::fabs(ys);
        vp.height = 2.0f * std::fabs(ys);
        int32_t windowX, windowY;
        DecodeVertexWindowOffset(regs, windowX, windowY, ignoreWindowOffset);
        vp.x += float(windowX);
        vp.y += float(windowY);
    }
    else
    {
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(g_extent.width);
        vp.height = static_cast<float>(g_extent.height);
    }
    // Xenos viewport Z is part of fixed-function rasterization. Unlike XY,
    // translated guest shaders don't fold Z scale / offset into SV_Position in
    // the normal clipped path, so dropping this state changes depth ordering.
    // In particular Crash uses ZSCALE=-1, ZOFFSET=1 with GREATER/GREATER_EQUAL
    // in Episode 1: that is a real reversed-Z viewport.
    const uint32_t clipControl = regs[xenos::kPaClClipCntl];
    const bool clippingDisabled = (clipControl & (1u << 16)) != 0;
    const bool dxClipSpace = (clipControl & (1u << 19)) != 0;
    if (clippingDisabled || pixelShaderWritesDepth)
    {
        // With clipping disabled the guest viewport transform is folded into
        // the vertex shader. A pixel shader writing depth also needs the full
        // host range because its exported depth is already in viewport space.
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
    }
    else
    {
        const float zs = (vte & (1u << 4)) ? F32(regs[xenos::kPaClVportZScale]) : 1.0f;
        const float zo = (vte & (1u << 5)) ? F32(regs[xenos::kPaClVportZOffset]) : 0.0f;
        if (std::isfinite(zs) && std::isfinite(zo))
        {
            // Xenia's GetHostViewportInfo: Direct3D 0..W clip space can use the
            // guest values directly. For -W..W, the VS first maps to 0..W and
            // the equivalent host range becomes (offset-scale)..(offset+scale).
            const float hostOffset = dxClipSpace ? zo : (zo - zs);
            const float hostScale = dxClipSpace ? zs : (zs * 2.0f);
            vp.minDepth = std::clamp(hostOffset, 0.0f, 1.0f);
            vp.maxDepth = std::clamp(hostOffset + hostScale, 0.0f, 1.0f);
        }
        else
        {
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
        }
    }
    if (EmulateReverseZLikeD3D12() && vp.minDepth > vp.maxDepth)
    {
        // ReXGlue/Xenia D3D12 can't rely on a reversed host viewport. Keep the
        // host range monotonic; UploadShared mirrors this by reflecting NDC Z,
        // preserving the exact guest depth values and comparison semantics.
        std::swap(vp.minDepth, vp.maxDepth);
    }
    if ((normalizedDepthControl & (1u << 1)) &&
        ((regs[xenos::kRbDepthInfo] >> 16) & 1u))
    {
        // With host render targets, Xenia maps the full Xenos D24FS8 [0, 2)
        // range to [0, 1) so ownership transfers can round-trip every 20e4
        // value without unrestricted host depth-range support.
        vp.minDepth *= 0.5f;
        vp.maxDepth *= 0.5f;
    }
    if (!std::isfinite(vp.x) || !std::isfinite(vp.y) || !std::isfinite(vp.width) ||
        !std::isfinite(vp.height) || vp.width <= 0.0f || std::fabs(vp.height) <= 0.0f)
    {
        // Match Xenia's empty-viewport handling. A degenerate guest viewport
        // has no useful raster coverage; expanding it to the full host target
        // can turn a deliberately empty draw into a fullscreen geometry replay.
        vp.x = -1.0f;
        vp.y = -1.0f;
        vp.width = 1.0f;
        vp.height = 1.0f;
    }
    return vp;
}

VkRect2D DecodeScissor(const uint32_t* regs, bool ignoreWindowOffset = false)
{
    auto unpackWindow = [](uint32_t v, int32_t& x, int32_t& y) {
        x = int32_t(v & 0x3FFFu);
        y = int32_t((v >> 16) & 0x3FFFu);
    };

    int32_t wx0, wy0, wx1, wy1;
    const uint32_t windowTl = regs[xenos::kPaScWindowScissorTl];
    unpackWindow(windowTl, wx0, wy0);
    unpackWindow(regs[xenos::kPaScWindowScissorBr], wx1, wy1);
    if (wx1 <= wx0 || wy1 <= wy0)
        return {{0, 0}, {0, 0}};

    // Xenos applies PA_SC_WINDOW_OFFSET unless WINDOW_OFFSET_DISABLE is set in
    // PA_SC_WINDOW_SCISSOR_TL. Each offset is a signed 15-bit value. Predicated
    // tiling relies on this to map logical windows such as 416..864 and
    // 832..1280 back onto the same physical EDRAM raster window.
    if (!ignoreWindowOffset && (windowTl & 0x80000000u) == 0)
    {
        const uint32_t rawOffset = regs[xenos::kPaScWindowOffset];
        auto signExtend15 = [](uint32_t value) -> int32_t {
            value &= 0x7FFFu;
            return (value & 0x4000u) ? int32_t(value | 0xFFFF8000u) : int32_t(value);
        };
        const int32_t offsetX = signExtend15(rawOffset);
        const int32_t offsetY = signExtend15(rawOffset >> 16);
        wx0 += offsetX;
        wx1 += offsetX;
        wy0 += offsetY;
        wy1 += offsetY;
    }

    auto unpackScreen = [](uint32_t v, int32_t& x, int32_t& y) {
        x = int32_t(v & 0x7FFFu);
        y = int32_t((v >> 16) & 0x7FFFu);
    };
    int32_t sx0, sy0, sx1, sy1;
    unpackScreen(regs[xenos::kPaScScreenScissorTl], sx0, sy0);
    unpackScreen(regs[xenos::kPaScScreenScissorBr], sx1, sy1);
    wx0 = std::max(wx0, sx0);
    wy0 = std::max(wy0, sy0);
    wx1 = std::min(wx1, sx1);
    wy1 = std::min(wy1, sy1);

    wx0 = std::clamp(wx0, int32_t(0), int32_t(g_extent.width));
    wy0 = std::clamp(wy0, int32_t(0), int32_t(g_extent.height));
    wx1 = std::clamp(wx1, int32_t(0), int32_t(g_extent.width));
    wy1 = std::clamp(wy1, int32_t(0), int32_t(g_extent.height));
    if (wx1 <= wx0 || wy1 <= wy0)
    {
        // Xenos / Xenia collapse an inverted or fully clipped scissor to zero
        // coverage.  Expanding it to the full host target replays geometry from
        // another EDRAM tile over the whole frame and is never guest-correct.
        return {{0, 0}, {0, 0}};
    }

    VkRect2D rect{};
    rect.offset = {wx0, wy0};
    rect.extent = {uint32_t(wx1 - wx0), uint32_t(wy1 - wy0)};
    return rect;
}

VkRect2D DecodeScreenScissor(const uint32_t* regs)
{
    auto unpack = [](uint32_t v, int32_t& x, int32_t& y) {
        x = int32_t(v & 0x7FFFu);
        y = int32_t((v >> 16) & 0x7FFFu);
    };
    int32_t x0, y0, x1, y1;
    unpack(regs[xenos::kPaScScreenScissorTl], x0, y0);
    unpack(regs[xenos::kPaScScreenScissorBr], x1, y1);
    if (x1 <= x0 || y1 <= y0)
        return {{0, 0}, {0, 0}};
    x0 = std::clamp(x0, 0, int32_t(g_extent.width));
    y0 = std::clamp(y0, 0, int32_t(g_extent.height));
    x1 = std::clamp(x1, 0, int32_t(g_extent.width));
    y1 = std::clamp(y1, 0, int32_t(g_extent.height));
    if (x1 <= x0 || y1 <= y0)
        return {{0, 0}, {0, 0}};
    return {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}

VkDeviceSize UploadConstants(const uint32_t* regs, uint32_t baseReg, uint32_t bytes, bool needed)
{
    const VkDeviceSize at = UploadAlloc(bytes, 256);
    if (at == VK_WHOLE_SIZE)
        return at;
    if (needed)
        std::memcpy(g_uploadMapped + at, regs + baseReg, bytes);
    else
        std::memset(g_uploadMapped + at, 0, bytes);
    return at;
}

bool UsesProjectionConstants4To7(const ShaderModuleRec& vs)
{
    if (vs.aluDynamic)
        return false;
    for (uint32_t constant = 4; constant <= 7; ++constant)
        if (std::find(vs.aluConsts.begin(), vs.aluConsts.end(), constant) == vs.aluConsts.end())
            return false;
    return true;
}

bool ApplyWideProjectionToUploadedConstants(VkDeviceSize at, const ShaderModuleRec& vs,
                                            bool scene3D, bool applyConfiguredAspect)
{
    if (at == VK_WHOLE_SIZE || !scene3D || !applyConfiguredAspect ||
        !UsesProjectionConstants4To7(vs))
        return false;

    const auto scale = mojorecomp::gpu::SceneAspectScale(ConfiguredAspectRatio());
    if (std::fabs(scale[0] - 1.0f) < 0.0001f ||
        std::fabs(scale[1] - 1.0f) > 0.0001f)
        return false;

    uint8_t* constants = g_uploadMapped + at;
    for (uint32_t constant = 4; constant <= 7; ++constant)
    {
        float x = 0.0f;
        std::memcpy(&x, constants + constant * 16u, sizeof(x));
        x *= scale[0];
        std::memcpy(constants + constant * 16u, &x, sizeof(x));
    }
    return true;
}

VkDeviceSize UploadShared(const uint32_t* regs,
                          const std::array<float, kTextureSlots>& textureSampleScales,
                          bool ignoreWindowOffset = false,
                          bool scene3D = false,
                          bool applyConfiguredAspect = true)
{
    const VkDeviceSize at = UploadAlloc(kSharedBytes, 256);
    if (at == VK_WHOLE_SIZE)
        return at;
    uint8_t* shared = g_uploadMapped + at;
    std::memset(shared, 0, kSharedBytes);
    mojorecomp::texture_abi::WriteIndices(shared);
    std::memcpy(shared + kSharedBoolFile, regs + xenos::kBoolConstantBase, 8 * sizeof(uint32_t));
    std::memcpy(shared + kSharedLoopConstants, regs + xenos::kLoopConstantBase, 32 * sizeof(uint32_t));
    uint32_t boolFold = 0;
    for (uint32_t i = 0; i < 8; ++i)
        boolFold |= regs[xenos::kBoolConstantBase + i];
    std::memcpy(shared + kSharedBooleans, &boolFold, sizeof(boolFold));
    const uint32_t swapped = 0;
    std::memcpy(shared + kSharedSwappedTexcoords, &swapped, sizeof(swapped));
    // Xenos can disable the fixed-function XY viewport transform. In that mode the
    // vertex shader is exporting window-space pixels, not clip-space coordinates.
    // Publish a shader-side fold so [0,w]x[0,h] becomes [-1,1]x[-1,1]. When the
    // transform is enabled these values are the identity and DecodeViewport applies
    // the guest viewport normally.
    const uint32_t vte = regs[xenos::kPaClVteCntl];
    float posScale[2] = {1.0f, 1.0f};
    float posOffset[2] = {0.0f, 0.0f};
    float halfPixel[2] = {0.0f, 0.0f};
    float posDepthTransform[2] = {1.0f, 0.0f};
    if ((vte & 1u) == 0)
    {
        const float w = static_cast<float>(std::max(g_extent.width, 1u));
        const float h = static_cast<float>(std::max(g_extent.height, 1u));
        posScale[0] = 2.0f / w;
        posScale[1] = 2.0f / h;
        posOffset[0] = -1.0f;
        posOffset[1] = -1.0f;
        int32_t windowX, windowY;
        DecodeVertexWindowOffset(regs, windowX, windowY, ignoreWindowOffset);
        posOffset[0] += float(windowX) * posScale[0];
        posOffset[1] += float(windowY) * posScale[1];

        // PA_SU_VTX_CNTL::PIX_CENTER = D3DZero means that integer vertex
        // positions are pixel centers. Vulkan uses half-integer centers, so
        // move the translated window-space position by +0.5 guest pixel.
        if ((regs[xenos::kPaSuVtxCntl] & 1u) == 0)
        {
            halfPixel[0] = 1.0f / w;
            halfPixel[1] = 1.0f / h;
        }
    }
    else
    {
        const float xs = F32(regs[xenos::kPaClVportXScale]);
        const float ys = (vte & 4u) ? F32(regs[xenos::kPaClVportYScale]) : 1.0f;
        const float absXs = std::fabs(xs);
        const float absYs = std::fabs(ys);

        // Match the Xenia host-viewport convention: keep Vulkan viewport
        // extents positive and carry the guest viewport-scale signs in NDC.
        // This also keeps SV_IsFrontFace semantics aligned with Xenos.
        if (std::isfinite(xs) && absXs > 0.0f)
            posScale[0] = std::signbit(xs) ? -1.0f : 1.0f;
        if (std::isfinite(ys) && absYs > 0.0f)
            posScale[1] = std::signbit(ys) ? -1.0f : 1.0f;

        // Convert the +0.5-pixel D3D9 center adjustment to clip-space.
        if ((regs[xenos::kPaSuVtxCntl] & 1u) == 0)
        {
            if (std::isfinite(absXs) && absXs > 0.0f)
                halfPixel[0] = 0.5f / absXs;
            if (std::isfinite(absYs) && absYs > 0.0f)
                halfPixel[1] = 0.5f / absYs;
        }
    }
    std::memcpy(shared + kSharedHalfPixelOffset, halfPixel, sizeof(halfPixel));
    std::memcpy(shared + kSharedPosScale, posScale, sizeof(posScale));
    std::memcpy(shared + kSharedPosOffset, posOffset, sizeof(posOffset));

    // Match Xenia's clip-space handling for Z. With clipping disabled the guest
    // viewport transform has to happen in the VS because the host viewport is
    // deliberately the full depth range. With clipping enabled and OpenGL-style
    // -W..W Z, map it to Vulkan/D3D 0..W before applying the host depth range.
    const uint32_t clipControl = regs[xenos::kPaClClipCntl];
    const bool clippingDisabled = (clipControl & (1u << 16)) != 0;
    const bool dxClipSpace = (clipControl & (1u << 19)) != 0;
    const float zScale = (vte & (1u << 4)) ? F32(regs[xenos::kPaClVportZScale]) : 1.0f;
    const float zOffset = (vte & (1u << 5)) ? F32(regs[xenos::kPaClVportZOffset]) : 0.0f;
    if (clippingDisabled)
    {
        if (std::isfinite(zScale) && std::isfinite(zOffset))
        {
            posDepthTransform[0] = zScale;
            posDepthTransform[1] = zOffset;
        }
    }
    else if (!dxClipSpace)
    {
        posDepthTransform[0] = 0.5f;
        posDepthTransform[1] = 0.5f;
    }
    if (!clippingDisabled && EmulateReverseZLikeD3D12() &&
        std::isfinite(zScale) && std::isfinite(zOffset))
    {
        const float hostOffset = dxClipSpace ? zOffset : (zOffset - zScale);
        const float hostScale = dxClipSpace ? zScale : (zScale * 2.0f);
        const float hostMin = std::clamp(hostOffset, 0.0f, 1.0f);
        const float hostMax = std::clamp(hostOffset + hostScale, 0.0f, 1.0f);
        if (hostMin > hostMax)
        {
            // Same compensation as GetHostViewportInfo(... allow_reverse_z=false):
            // swapping viewport bounds requires reflecting the shader's NDC Z.
            posDepthTransform[0] = -posDepthTransform[0];
            posDepthTransform[1] = 1.0f - posDepthTransform[1];
        }
    }
    std::memcpy(shared + kSharedPosDepthTransform, posDepthTransform,
                sizeof(posDepthTransform));

    // Pixel-shader SV_Depth bypasses the fixed-function viewport depth range.
    // Xenia therefore scales explicit guest depth by 0.5 when D24FS8 is backed
    // by a conventional host depth target, matching the [0,2) -> [0,1) 20e4
    // ownership representation used by the viewport, clears and transfers.
    const float depthOutputScale = ((regs[xenos::kRbDepthInfo] >> 16) & 1u)
        ? 0.5f : 1.0f;
    std::memcpy(shared + kSharedDepthOutputScale, &depthOutputScale,
                sizeof(depthOutputScale));

    const auto aspectScale = applyConfiguredAspect
        ? AspectScaleForDraw(scene3D)
        : std::array<float, 2>{1.0f, 1.0f};
    std::memcpy(shared + kSharedAspectScale, aspectScale.data(),
                sizeof(float) * aspectScale.size());

    const uint32_t colorControl = regs[xenos::kRbColorControl];
    const float alphaThreshold = F32(regs[xenos::kRbAlphaRef]);
    std::memcpy(shared + kSharedAlphaThreshold, &alphaThreshold, sizeof(alphaThreshold));
    std::memcpy(shared + kSharedTextureSampleScales, textureSampleScales.data(),
                textureSampleScales.size() * sizeof(float));
    return at;
}

uint32_t ReadIndexValue(const uint8_t* src, uint32_t index, bool index32, uint32_t endian)
{
    if (index32)
    {
        uint32_t value = 0;
        std::memcpy(&value, src + uint64_t(index) * 4u, sizeof(value));
        uint8_t raw[4];
        std::memcpy(raw, &value, sizeof(raw));
        CopySwapped(reinterpret_cast<uint8_t*>(&value), raw, sizeof(raw), endian);
        return value;
    }

    if ((endian & 3u) == 2u)
    {
        uint32_t pair = 0;
        std::memcpy(&pair, src + uint64_t(index & ~1u) * 2u, sizeof(pair));
        pair = __builtin_bswap32(pair);
        return (index & 1u) ? (pair >> 16) : (pair & 0xFFFFu);
    }

    uint16_t value = 0;
    std::memcpy(&value, src + uint64_t(index) * 2u, sizeof(value));
    if ((endian & 3u) == 1u || (endian & 3u) == 3u)
        value = uint16_t((value >> 8) | (value << 8));
    return value;
}

constexpr uint32_t kXenosVertexIndexMask = 0x00FFFFFFu;

uint32_t RemapVertexIndex(uint32_t index, uint32_t indexOffset,
                          uint32_t minVertex, uint32_t maxVertex)
{
    uint32_t remapped = (index + indexOffset) & kXenosVertexIndexMask;
    if (minVertex <= maxVertex)
        remapped = std::clamp(remapped, minVertex, maxVertex);
    return remapped;
}

struct DrawPositionBounds
{
    float minX = FLT_MAX;
    float minY = FLT_MAX;
    float maxX = -FLT_MAX;
    float maxY = -FLT_MAX;
    const VertexAttribute* position = nullptr;
    xenos::VertexFetch fetch{};
    uint32_t va = 0;
    uint64_t strideBytes = 0;
    uint32_t firstVertex = 0;
};

bool GetDrawPositionBounds(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
                           const ShaderModuleRec& vs, DrawPositionBounds& out)
{
    if (!base || !draw.indexCount)
        return false;

    const VertexAttribute* position = nullptr;
    for (const auto& a : vs.attributes)
    {
        if (a.location == 0 && !a.indirect &&
            (a.format == 37 || a.format == 57 || a.format == 38) &&
            a.strideDwords && a.offsetDwords + 2 <= a.strideDwords)
        {
            position = &a;
            break;
        }
    }
    if (!position)
        return false;

    const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, position->fetchSlot);
    const uint32_t va = PhysicalToCached(vf.address);
    const uint64_t strideBytes = uint64_t(position->strideDwords) * 4u;
    if (!vf.address || !vf.sizeDwords ||
        !GuestRangeOk(va, uint64_t(vf.sizeDwords) * 4u))
        return false;

    out.position = position;
    out.fetch = vf;
    out.va = va;
    out.strideBytes = strideBytes;

    const uint32_t indexBytes = draw.index32 ? 4u : 2u;
    const uint64_t rawIndexBytes = uint64_t(draw.indexCount) * indexBytes;
    const uint64_t indexReadBytes = (!draw.index32 && (draw.indexEndian & 3u) == 2u)
                                        ? ((rawIndexBytes + 3u) & ~3ull)
                                        : rawIndexBytes;
    const uint8_t* indexSrc = nullptr;
    if (draw.indexed)
    {
        if (!draw.indexVa || !GuestRangeOk(draw.indexVa, indexReadBytes))
            return false;
        indexSrc = base + draw.indexVa;
    }

    const uint32_t indexOffset = regs[xenos::kVgtIndxOffset] & kXenosVertexIndexMask;
    const uint32_t minVertex = regs[xenos::kVgtMinVtxIndx] & kXenosVertexIndexMask;
    const uint32_t maxVertex = regs[xenos::kVgtMaxVtxIndx] & kXenosVertexIndexMask;
    for (uint32_t i = 0; i < draw.indexCount; ++i)
    {
        const uint32_t vertex = draw.indexed
                                    ? RemapVertexIndex(ReadIndexValue(indexSrc, i, draw.index32,
                                                                     draw.indexEndian),
                                                       indexOffset, minVertex, maxVertex)
                                    : RemapVertexIndex(i, indexOffset, minVertex, maxVertex);
        if (i == 0)
            out.firstVertex = vertex;
        if ((uint64_t(vertex) + 1u) * position->strideDwords > vf.sizeDwords)
            return false;

        uint32_t words[2]{};
        CopySwapped(reinterpret_cast<uint8_t*>(words),
                    base + va + uint64_t(vertex) * strideBytes +
                        uint64_t(position->offsetDwords) * 4u,
                    sizeof(words), vf.endian);
        const float x = F32(words[0]);
        const float y = F32(words[1]);
        if (!std::isfinite(x) || !std::isfinite(y))
            return false;
        out.minX = std::min(out.minX, x);
        out.minY = std::min(out.minY, y);
        out.maxX = std::max(out.maxX, x);
        out.maxY = std::max(out.maxY, y);
    }
    return true;
}

bool IsSafeAreaOverlayDraw(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
                           const ShaderModuleRec& vs, uint32_t mode)
{
    if (mode != 4 || !draw.predicated || draw.indexed ||
        (draw.primType != xenos::kTriangleList &&
         draw.primType != xenos::kTriangleStrip))
        return false;

    DrawPositionBounds bounds{};
    if (!GetDrawPositionBounds(base, draw, regs, vs, bounds))
        return false;

    return mojorecomp::gpu::ShouldTreatAsSafeAreaOverlay(
        bounds.minX, bounds.minY, bounds.maxX, bounds.maxY,
        g_extent.width, g_extent.height);
}

void ReportLowerUiCandidate(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
                            const ShaderModuleRec& vs, const ShaderModuleRec& ps)
{
    if (!UiDiagnosticsEnabled() || !base || draw.indexCount < 3 || draw.indexCount > 120)
        return;

    DrawPositionBounds bounds{};
    if (!GetDrawPositionBounds(base, draw, regs, vs, bounds))
        return;
    const float minX = bounds.minX;
    const float minY = bounds.minY;
    const float maxX = bounds.maxX;
    const float maxY = bounds.maxY;
    const VertexAttribute* position = bounds.position;
    const xenos::VertexFetch& vf = bounds.fetch;
    const uint32_t va = bounds.va;
    const uint64_t strideBytes = bounds.strideBytes;
    const uint32_t firstVertex = bounds.firstVertex;

    const bool titleDiagnostics = [] {
        const char* value = std::getenv("MOJORECOMP_TITLE_DIAGNOSTICS");
        return value && value[0] == '1';
    }();
    constexpr uint64_t kUiTextVs = 0xD589CC02813C1884ull;
    constexpr uint64_t kUiTextPs = 0xD7F22D636B662D38ull;
    const bool trackedTextShader = titleDiagnostics &&
                                   vs.hash == kUiTextVs && ps.hash == kUiTextPs;

    // Frontend coordinates are normally in pixels. In title diagnostics, keep
    // the known UI-text shader regardless of its raw coordinate space so a
    // transformed/offset prompt can't evade the lower-screen heuristic.
    if (!trackedTextShader &&
        (maxY < 430.0f || minY > 730.0f || maxX < 180.0f || minX > 1100.0f ||
         minX < -64.0f || maxX > 1344.0f))
        return;

    const int32_t qx0 = int32_t(std::lround(minX / 4.0f));
    const int32_t qy0 = int32_t(std::lround(minY / 4.0f));
    const int32_t qx1 = int32_t(std::lround(maxX / 4.0f));
    const int32_t qy1 = int32_t(std::lround(maxY / 4.0f));
    uint64_t signature = vs.hash ^ (ps.hash * 0x9E3779B185EBCA87ull);
    const uint32_t sigWords[] = {
        draw.primType, draw.indexCount, uint32_t(qx0), uint32_t(qy0),
        uint32_t(qx1), uint32_t(qy1), regs[xenos::kRbBlendControl0],
        regs[xenos::kRbDepthControl], regs[xenos::kRbColorControl],
        regs[xenos::kRbColorMask], regs[xenos::kPaScWindowScissorTl],
        regs[xenos::kPaScWindowScissorBr]
    };
    for (uint32_t word : sigWords)
        signature = (signature ^ word) * 0x100000001B3ull;

    static std::vector<uint64_t> seen;
    static std::atomic<uint32_t> titleReports{0};
    if (titleDiagnostics)
    {
        // Keep intro/logo geometry from exhausting the ordinary unique-draw
        // budget. The missing PRESS START prompt lives in the lower portion of
        // the title screen, so sample this band repeatedly only after the boot
        // sequence has advanced far enough to reach the frontend.
        const uint64_t diagnosticFrame = g_frames.load(std::memory_order_relaxed) + 1;
        if (diagnosticFrame < 800 ||
            (!trackedTextShader && (minY < 500.0f || maxY > 730.0f ||
                                    minX < 200.0f || maxX > 1080.0f)) ||
            titleReports.fetch_add(1) >= 512)
            return;
    }
    else
    {
        if (std::find(seen.begin(), seen.end(), signature) != seen.end() || seen.size() >= 192)
            return;
        seen.push_back(signature);
    }

    uint32_t extra[4]{};
    const VertexAttribute* payload = nullptr;
    for (const auto& a : vs.attributes)
        if (a.location != 0 && !a.indirect && a.fetchSlot == position->fetchSlot && a.strideDwords)
        {
            payload = &a;
            break;
        }
    if (payload && (uint64_t(firstVertex) + 1u) * payload->strideDwords <= vf.sizeDwords)
    {
        const uint32_t words = std::min<uint32_t>(4u, payload->strideDwords - payload->offsetDwords);
        CopySwapped(reinterpret_cast<uint8_t*>(extra),
                    base + va + uint64_t(firstVertex) * strideBytes +
                        uint64_t(payload->offsetDwords) * 4u,
                    words * 4u, vf.endian);
    }

    const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
    KLOG("[ui lower] frame=%llu bbox=%.1f,%.1f..%.1f,%.1f prim=%u count=%u "
         "VS=%016llX PS=%016llX mode=%u blend=%08X depth=%08X colorCtl=%08X "
         "alphaRef=%08X mask=%08X scissor=%08X..%08X payload=%08X,%08X,%08X,%08X\n",
         static_cast<unsigned long long>(frame), minX, minY, maxX, maxY,
         draw.primType, draw.indexCount,
         static_cast<unsigned long long>(vs.hash), static_cast<unsigned long long>(ps.hash),
         regs[xenos::kRbModeControl] & 7u, regs[xenos::kRbBlendControl0],
         regs[xenos::kRbDepthControl], regs[xenos::kRbColorControl], regs[xenos::kRbAlphaRef],
         regs[xenos::kRbColorMask], regs[xenos::kPaScWindowScissorTl],
         regs[xenos::kPaScWindowScissorBr], extra[0], extra[1], extra[2], extra[3]);

    if (titleDiagnostics)
    {
        for (const auto& a : vs.attributes)
        {
            if (a.location < 0 || a.indirect || a.fetchSlot >= 96 || !a.strideDwords)
                continue;
            const xenos::VertexFetch avf = xenos::DecodeVertexFetch(regs, a.fetchSlot);
            const uint32_t ava = PhysicalToCached(avf.address);
            if (!avf.address || !avf.sizeDwords ||
                !GuestRangeOk(ava, uint64_t(avf.sizeDwords) * 4u) ||
                (uint64_t(firstVertex) + 1u) * a.strideDwords > avf.sizeDwords)
                continue;
            std::array<uint32_t, 4> words{};
            const uint32_t available = a.strideDwords > a.offsetDwords
                                           ? a.strideDwords - a.offsetDwords
                                           : 0u;
            const uint32_t wordCount = std::min<uint32_t>(4u, available);
            if (!wordCount)
                continue;
            CopySwapped(reinterpret_cast<uint8_t*>(words.data()),
                        base + ava + uint64_t(firstVertex) * uint64_t(a.strideDwords) * 4u +
                            uint64_t(a.offsetDwords) * 4u,
                        wordCount * 4u, avf.endian);
            KLOG("[ui attr] frame=%llu loc=%d slot=%u fmt=%u signed=%u int=%u stride=%u off=%u "
                 "words=%08X,%08X,%08X,%08X f=%g,%g,%g,%g\n",
                 static_cast<unsigned long long>(frame), a.location, a.fetchSlot, a.format,
                 a.isSigned, a.isInteger, a.strideDwords, a.offsetDwords,
                 words[0], words[1], words[2], words[3],
                 F32(words[0]), F32(words[1]), F32(words[2]), F32(words[3]));
        }
    }
}

struct PreparedDraw
{
    uint32_t count = 0;
    bool indexed = false;
    VkDeviceSize indexAt = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    int32_t baseVertex = 0;
};

bool PrepareVertexBindings(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
                           const ShaderModuleRec& vs, PreparedDraw& out)
{
    std::vector<VkBuffer> buffers;
    std::vector<VkDeviceSize> offsets;
    const uint32_t indexOffset = regs[xenos::kVgtIndxOffset] & kXenosVertexIndexMask;
    const uint32_t minVertex = regs[xenos::kVgtMinVtxIndx] & kXenosVertexIndexMask;
    const uint32_t maxVertexClamp = regs[xenos::kVgtMaxVtxIndx] & kXenosVertexIndexMask;
    const bool primitiveRestart = PrimitiveRestartEnabled(draw, regs);
    const uint32_t guestRestartIndex = regs[xenos::kVgtMultiPrimIbResetIndx] &
                                       (draw.index32 ? 0xFFFFFFFFu : 0xFFFFu);
    bool remapIndices = false;
    const bool rectangle = draw.primType == xenos::kRectangleList && draw.indexCount == 3;
    out.count = rectangle ? 4u : draw.indexCount;
    out.indexed = draw.indexed && !rectangle;
    out.baseVertex = 0;

    const uint32_t indexBytes = draw.index32 ? 4u : 2u;
    const uint64_t rawIndexBytes = uint64_t(draw.indexCount) * indexBytes;
    const uint64_t indexReadBytes = (!draw.index32 && (draw.indexEndian & 3u) == 2u)
                                        ? ((rawIndexBytes + 3u) & ~3ull)
                                        : rawIndexBytes;
    const uint8_t* indexSrc = nullptr;
    uint32_t maxIndex = draw.indexCount ? draw.indexCount - 1u : 0u;
    uint32_t maxEffectiveIndex = maxIndex;
    uint32_t rectCorner[3] = {0, 1, 2};
    if (draw.indexed)
    {
        if (!draw.indexVa || !GuestRangeOk(draw.indexVa, indexReadBytes))
            return false;
        indexSrc = base + draw.indexVa;
        maxIndex = 0;
        maxEffectiveIndex = 0;
        for (uint32_t i = 0; i < draw.indexCount; ++i)
        {
            const uint32_t raw = ReadIndexValue(indexSrc, i, draw.index32, draw.indexEndian);
            if (primitiveRestart && raw == guestRestartIndex)
                continue;
            const uint32_t effective = RemapVertexIndex(raw, indexOffset,
                                                        minVertex, maxVertexClamp);
            maxIndex = std::max(maxIndex, raw);
            maxEffectiveIndex = std::max(maxEffectiveIndex, effective);
            remapIndices |= effective != raw;
        }
        if (rectangle)
            for (uint32_t i = 0; i < 3; ++i)
                rectCorner[i] = RemapVertexIndex(
                    ReadIndexValue(indexSrc, i, draw.index32, draw.indexEndian),
                    indexOffset, minVertex, maxVertexClamp);
    }
    else if (!rectangle)
    {
        const uint32_t lastRaw = draw.indexCount ? draw.indexCount - 1u : 0u;
        remapIndices = draw.indexCount &&
                       (indexOffset != 0 || minVertex != 0 || lastRaw > maxVertexClamp);
        if (remapIndices)
        {
            // Auto-indexed draws normally map 1:1 to host vertex indices. Only
            // materialize an index buffer when Xenos offset/wrap/clamp semantics
            // actually alter that sequence.
            out.indexed = true;
            maxEffectiveIndex = 0;
            for (uint32_t i = 0; i < draw.indexCount; ++i)
                maxEffectiveIndex = std::max(
                    maxEffectiveIndex,
                    RemapVertexIndex(i, indexOffset, minVertex, maxVertexClamp));
        }
    }

    // Xenos RECTLIST receives three corners, but their order is not fixed.
    // The hardware identifies the rectangle diagonal as the longest of the
    // three pairwise edges, then reconstructs the missing corner from that
    // diagonal and the remaining corner. A fixed 0+2-1 reconstruction works
    // only for one of the possible guest vertex orders and can leave UI quads
    // partially uncovered.
    uint32_t rectDiagonalA = 0;
    uint32_t rectDiagonalB = 2;
    uint32_t rectOther = 1;
    if (rectangle)
    {
        const VertexAttribute* position = nullptr;
        for (const auto& a : vs.attributes)
        {
            // Position is exported from location 0 by our translator. Restrict
            // this CPU-side ordering test to native float32 vectors where the
            // first two components can be compared without format conversion.
            if (a.location == 0 && !a.indirect &&
                (a.format == 37 || a.format == 57 || a.format == 38) &&
                a.strideDwords && a.offsetDwords + 2 <= a.strideDwords)
            {
                position = &a;
                break;
            }
        }

        if (position)
        {
            const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, position->fetchSlot);
            const uint32_t va = PhysicalToCached(vf.address);
            const uint64_t strideBytes = uint64_t(position->strideDwords) * 4u;
            bool valid = vf.address && vf.sizeDwords &&
                         GuestRangeOk(va, uint64_t(vf.sizeDwords) * 4u);
            float xy[3][2]{};
            for (uint32_t i = 0; valid && i < 3; ++i)
            {
                if ((uint64_t(rectCorner[i]) + 1u) * position->strideDwords > vf.sizeDwords)
                {
                    valid = false;
                    break;
                }
                uint32_t words[2]{};
                CopySwapped(reinterpret_cast<uint8_t*>(words),
                            base + va + uint64_t(rectCorner[i]) * strideBytes +
                                uint64_t(position->offsetDwords) * 4u,
                            sizeof(words), vf.endian);
                xy[i][0] = F32(words[0]);
                xy[i][1] = F32(words[1]);
                valid = std::isfinite(xy[i][0]) && std::isfinite(xy[i][1]);
            }

            if (valid)
            {
                auto distanceSq = [&](uint32_t a, uint32_t b) {
                    const double dx = double(xy[a][0]) - double(xy[b][0]);
                    const double dy = double(xy[a][1]) - double(xy[b][1]);
                    return dx * dx + dy * dy;
                };
                const double d01 = distanceSq(0, 1);
                const double d12 = distanceSq(1, 2);
                const double d20 = distanceSq(2, 0);
                if (d01 >= d12 && d01 >= d20)
                {
                    rectDiagonalA = 0;
                    rectDiagonalB = 1;
                    rectOther = 2;
                }
                else if (d12 >= d20)
                {
                    rectDiagonalA = 1;
                    rectDiagonalB = 2;
                    rectOther = 0;
                }
            }
        }
    }

    struct UploadedVertexFetch
    {
        bool valid = false;
        uint32_t address = 0;
        uint32_t sizeDwords = 0;
        uint32_t endian = 0;
        uint32_t strideDwords = 0;
        uint64_t sourceDwords = 0;
        VkDeviceSize uploadAt = VK_WHOLE_SIZE;
    };
    std::array<UploadedVertexFetch, 96> uploadedVertexFetches{};

    for (const auto& a : vs.attributes)
    {
        if (a.location < 0 || a.indirect)
            continue;
        if (a.fetchSlot >= 96 || !a.strideDwords)
            return false;
        if (rectangle && !FloatVertexFormat(a.format))
            return false;
        const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, a.fetchSlot);
        const uint32_t va = PhysicalToCached(vf.address);
        const uint64_t strideBytes = uint64_t(a.strideDwords) * 4u;
        if (!vf.address || !vf.sizeDwords || !GuestRangeOk(va, uint64_t(vf.sizeDwords) * 4u))
            return false;

        // One-shot proof for the fullscreen pass that initializes/updates the
        // motion-vector backing used by the blur shader.  Wait until real 3D
        // indexed work is underway so the dump describes the cutscene pass,
        // not an earlier frontend use of the same generic passthrough VS.
        if (vs.hash == 0x760AACF6212E632Cull &&
            g_indexedDraws.load(std::memory_order_relaxed) > 10000)
        {
            static uint32_t velocityFillAttributeReports = 0;
            if (velocityFillAttributeReports < vs.attributes.size())
            {
                ++velocityFillAttributeReports;
                KLOG("[velocity fill vertex] attr=%u loc=%d slot=%u fmt=%u strideDw=%u "
                     "offsetDw=%u addr=%08X sizeDw=%u endian=%u count=%u\n",
                     velocityFillAttributeReports, a.location, a.fetchSlot, a.format,
                     a.strideDwords, a.offsetDwords, vf.address, vf.sizeDwords, vf.endian,
                     draw.indexCount);
                const uint32_t wordsToRead =
                    std::min<uint32_t>(4u, a.strideDwords > a.offsetDwords
                                                ? a.strideDwords - a.offsetDwords
                                                : 0u);
                for (uint32_t v = 0; v < std::min<uint32_t>(4u, draw.indexCount); ++v)
                {
                    const uint32_t raw = draw.indexed
                                             ? ReadIndexValue(indexSrc, v, draw.index32,
                                                              draw.indexEndian)
                                             : v;
                    const uint32_t vertex = RemapVertexIndex(raw, indexOffset,
                                                             minVertex, maxVertexClamp);
                    if (!wordsToRead ||
                        (uint64_t(vertex) + 1u) * a.strideDwords > vf.sizeDwords)
                        break;
                    std::array<uint32_t, 4> words{};
                    CopySwapped(reinterpret_cast<uint8_t*>(words.data()),
                                base + va + uint64_t(vertex) * strideBytes +
                                    uint64_t(a.offsetDwords) * 4u,
                                size_t(wordsToRead) * 4u, vf.endian);
                    KLOG("[velocity fill vertex] v%u guest=%u raw=%08X,%08X,%08X,%08X "
                         "f=%g,%g,%g,%g\n",
                         v, vertex, words[0], words[1], words[2], words[3],
                         double(F32(words[0])), double(F32(words[1])),
                         double(F32(words[2])), double(F32(words[3])));
                }
            }
        }

        if (vs.hash == 0x6D73B356D2B5E61Bull)
        {
            static uint32_t binkVertexReports = 0;
            if (binkVertexReports < 2)
            {
                KLOG("[bink vertex] attr report=%u loc=%d slot=%u fmt=%u signed=%u integer=%u "
                     "strideDw=%u offsetDw=%u addr=%08X sizeDw=%u endian=%u indexed=%u count=%u\n",
                     binkVertexReports, a.location, a.fetchSlot, a.format, a.isSigned, a.isInteger,
                     a.strideDwords, a.offsetDwords, vf.address, vf.sizeDwords, vf.endian,
                     draw.indexed ? 1u : 0u, draw.indexCount);
                for (uint32_t v = 0; v < std::min<uint32_t>(4u, draw.indexCount); ++v)
                {
                    const uint32_t raw = draw.indexed
                                             ? ReadIndexValue(indexSrc, v, draw.index32,
                                                              draw.indexEndian)
                                             : v;
                    const uint32_t vertex = RemapVertexIndex(raw, indexOffset,
                                                             minVertex, maxVertexClamp);
                    if ((uint64_t(vertex) + 1u) * a.strideDwords > vf.sizeDwords)
                        break;
                    std::array<uint32_t, 7> words{};
                    CopySwapped(reinterpret_cast<uint8_t*>(words.data()),
                                base + va + uint64_t(vertex) * strideBytes,
                                size_t(std::min<uint64_t>(strideBytes, sizeof(words))), vf.endian);
                    KLOG("[bink vertex] v%u guest=%u words=%08X,%08X,%08X,%08X,%08X,%08X,%08X "
                         "f=%g,%g,%g,%g,%g,%g,%g\n",
                         v, vertex, words[0], words[1], words[2], words[3], words[4], words[5],
                         words[6], F32(words[0]), F32(words[1]), F32(words[2]), F32(words[3]),
                         F32(words[4]), F32(words[5]), F32(words[6]));
                }
                ++binkVertexReports;
            }
        }

        // Opt-in, bounded source-stream diagnostics; no title/hash-specific gate.
        if (CoordinateDiagnosticsEnabled() && a.offsetDwords == 0 && FloatVertexFormat(a.format))
        {
            static uint32_t reports = 0;
            if (reports < 8)
            {
                ++reports;
                KLOG("[VTE probe] source #%u VS=%016llX prim=%u count=%u indexOffset=%d "
                     "slot=%u fmt=%u signed=%u integer=%u stride=%u offset=%u "
                     "addr=%08X sizeDw=%u endian=%u\n",
                     reports, static_cast<unsigned long long>(vs.hash), draw.primType, draw.indexCount,
                     indexOffset, a.fetchSlot, a.format, a.isSigned, a.isInteger,
                     a.strideDwords, a.offsetDwords, vf.address, vf.sizeDwords, vf.endian);
                for (uint32_t v = 0; v < std::min<uint32_t>(3u, draw.indexCount); ++v)
                {
                    const uint32_t raw = draw.indexed
                                             ? ReadIndexValue(indexSrc, v, draw.index32,
                                                              draw.indexEndian)
                                             : v;
                    const uint32_t vertex = RemapVertexIndex(raw, indexOffset,
                                                             minVertex, maxVertexClamp);
                    if ((uint64_t(vertex) + 1u) * a.strideDwords > vf.sizeDwords)
                        break;
                    std::array<uint32_t, 7> words{};
                    CopySwapped(reinterpret_cast<uint8_t*>(words.data()),
                                base + va + uint64_t(vertex) * strideBytes,
                                size_t(std::min<uint64_t>(strideBytes, sizeof(words))), vf.endian);
                    KLOG("[VTE probe] v%u guest=%u words=%08X,%08X,%08X,%08X,%08X,%08X,%08X "
                         "f=%g,%g,%g,%g,%g,%g,%g\n",
                         v, vertex, words[0], words[1], words[2], words[3], words[4], words[5],
                         words[6], F32(words[0]), F32(words[1]), F32(words[2]), F32(words[3]),
                         F32(words[4]), F32(words[5]), F32(words[6]));
                }
            }
        }

        if (rectangle)
        {
            uint32_t corners[3] = {
                draw.indexed ? rectCorner[0] : RemapVertexIndex(0, indexOffset, minVertex, maxVertexClamp),
                draw.indexed ? rectCorner[1] : RemapVertexIndex(1, indexOffset, minVertex, maxVertexClamp),
                draw.indexed ? rectCorner[2] : RemapVertexIndex(2, indexOffset, minVertex, maxVertexClamp),
            };
            for (uint32_t corner : corners)
                if ((uint64_t(corner) + 1u) * a.strideDwords > vf.sizeDwords)
                    return false;

            const uint64_t uploadBytes = 4u * strideBytes;
            const VkDeviceSize at = UploadAlloc(uploadBytes, 16);
            if (at == VK_WHOLE_SIZE)
                return false;
            g_perfVertexUploadBytes += uploadBytes;
            ++g_perfVertexUploadCopies;
            uint8_t* dst = g_uploadMapped + at;
            // Host strip order is: remaining corner, diagonal A, diagonal B,
            // reconstructed corner. The shared diagonal is therefore between
            // strip vertices 1 and 2, covering the complete rectangle for any
            // ordering of the three guest corners.
            const auto stripSources = mojorecomp::gpu::RectangleStripSources(
                rectOther, rectDiagonalA, rectDiagonalB);
            for (uint32_t i = 0; i < 3; ++i)
                CopySwapped(dst + uint64_t(i) * strideBytes,
                            base + va + uint64_t(corners[stripSources[i]]) * strideBytes,
                            size_t(strideBytes), vf.endian);
            uint8_t* fourth = dst + 3u * strideBytes;
            for (uint32_t d = 0; d < a.strideDwords; ++d)
            {
                float other, diagonalA, diagonalB, f3;
                std::memcpy(&other, dst + d * 4, 4);
                std::memcpy(&diagonalA, dst + strideBytes + d * 4, 4);
                std::memcpy(&diagonalB, dst + 2u * strideBytes + d * 4, 4);
                f3 = diagonalA + diagonalB - other;
                std::memcpy(fourth + d * 4, &f3, 4);
            }
            buffers.push_back(g_uploadBuffer);
            offsets.push_back(at + uint64_t(a.offsetDwords) * 4u);
            continue;
        }

        const uint64_t firstVertex = 0;
        const uint64_t sourceVertices = out.indexed
                                            ? uint64_t(maxEffectiveIndex) + 1u
                                            : uint64_t(draw.indexCount);
        const uint64_t startDw = firstVertex * a.strideDwords;
        const uint64_t sourceDwords = sourceVertices * a.strideDwords;
        if (startDw + sourceDwords > vf.sizeDwords)
            return false;

        auto& cached = uploadedVertexFetches[a.fetchSlot];
        VkDeviceSize at = VK_WHOLE_SIZE;
        if (cached.valid && cached.address == vf.address &&
            cached.sizeDwords == vf.sizeDwords && cached.endian == vf.endian &&
            cached.strideDwords == a.strideDwords &&
            cached.sourceDwords == sourceDwords)
        {
            at = cached.uploadAt;
            g_perfVertexReuseBytes += sourceDwords * 4u;
            ++g_perfVertexReuseHits;
        }
        else
        {
            const uint64_t uploadBytes = sourceDwords * 4u;
            at = UploadAlloc(uploadBytes, 16);
            if (at == VK_WHOLE_SIZE)
                return false;
            g_perfVertexUploadBytes += uploadBytes;
            ++g_perfVertexUploadCopies;
            const uint8_t* src = base + va + startDw * 4u;
            uint8_t* dst = g_uploadMapped + at;
            CopySwapped(dst, src, size_t(uploadBytes), vf.endian);
            cached = {true, vf.address, vf.sizeDwords, vf.endian,
                      a.strideDwords, sourceDwords, at};
        }
        buffers.push_back(g_uploadBuffer);
        offsets.push_back(at + uint64_t(a.offsetDwords) * 4u);
    }

    if (!buffers.empty())
        p_vkCmdBindVertexBuffers(g_commandBuffer, 0, static_cast<uint32_t>(buffers.size()),
                                 buffers.data(), offsets.data());

    if (out.indexed)
    {
        const bool rewritten = remapIndices || primitiveRestart;
        const bool hostIndex32 = rewritten || draw.index32;
        const uint32_t hostIndexBytes = hostIndex32 ? 4u : 2u;
        const uint64_t indexUploadBytes = uint64_t(out.count) * hostIndexBytes;
        const VkDeviceSize at = UploadAlloc(indexUploadBytes, 4);
        if (at == VK_WHOLE_SIZE)
            return false;
        g_perfIndexUploadBytes += indexUploadBytes;
        ++g_perfIndexUploadCopies;
        uint8_t* dst = g_uploadMapped + at;
        if (rewritten)
        {
            for (uint32_t i = 0; i < out.count; ++i)
            {
                const uint32_t raw = draw.indexed
                                         ? ReadIndexValue(indexSrc, i, draw.index32,
                                                          draw.indexEndian)
                                         : i;
                const uint32_t value = primitiveRestart && raw == guestRestartIndex
                                           ? 0xFFFFFFFFu
                                           : RemapVertexIndex(raw, indexOffset,
                                                              minVertex, maxVertexClamp);
                std::memcpy(dst + uint64_t(i) * 4u, &value, sizeof(value));
            }
            out.indexType = VK_INDEX_TYPE_UINT32;
        }
        else if (draw.index32)
        {
            for (uint32_t i = 0; i < draw.indexCount; ++i)
            {
                const uint32_t value = ReadIndexValue(indexSrc, i, true, draw.indexEndian);
                std::memcpy(dst + uint64_t(i) * 4u, &value, sizeof(value));
            }
            out.indexType = VK_INDEX_TYPE_UINT32;
        }
        else
        {
            for (uint32_t i = 0; i < draw.indexCount; ++i)
            {
                const uint16_t value = uint16_t(ReadIndexValue(indexSrc, i, false,
                                                               draw.indexEndian));
                std::memcpy(dst + uint64_t(i) * 2u, &value, sizeof(value));
            }
            out.indexType = VK_INDEX_TYPE_UINT16;
        }
        out.indexAt = at;
        p_vkCmdBindIndexBuffer(g_commandBuffer, g_uploadBuffer, out.indexAt, out.indexType);
    }
    return true;
}

bool EndFrameAndPresent(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    if (!BeginFrame())
        return false;
    static uint32_t msaaPresentReports = 0;
    const bool reportMsaaPresent = g_hostMsaa2x && msaaPresentReports++ < 12;
    if (reportMsaaPresent)
        KLOG("[msaa2x present] frame=%llu begin\n",
             static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1));
    EndColorRendering();
    if (reportMsaaPresent)
        KLOG("[msaa2x present] render scope closed\n");

    uint32_t imageIndex = 0;
    VkResult result = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX,
                                              g_imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return false;

    const uint32_t frontKey = frontBuffer & 0x1FFFFFFFu;
    ResolveSnapshot* frontSnapshot = FindSnapshot(frontKey);
    const uint64_t currentFrame = g_frames.load(std::memory_order_relaxed) + 1;
    const bool indexHashEventCapture =
        g_indexHashCaptureFrame.load(std::memory_order_relaxed) == currentFrame;

    const bool completeFrontOnly = [] {
        const char* value = std::getenv("MOJORECOMP_COMPLETE_FRONT_ONLY");
        return value && std::strcmp(value, "1") == 0;
    }();

    // A front buffer can receive only part of a tiled resolve before XE_SWAP.
    // Preserve the most recent fully covered generation so an incomplete sweep
    // never becomes visible. This adds only a GPU image copy; it does not wait
    // for the previous frame on the CPU and therefore keeps frames-in-flight.
    if (completeFrontOnly && frontSnapshot && frontSnapshot->coverageComplete &&
        frontSnapshot->frameSeen == currentFrame)
        CommitSnapshotForPresent(*frontSnapshot);

    // A retail frame may be assembled through several complete EDRAM sweeps.
    // Coverage alone therefore isn't enough: promoting immediately after tile 3
    // can expose an intermediate UI layer. Require one host present with no new
    // resolves to this snapshot before publishing it. This is a GPU-side copy,
    // not a wait, so it preserves the current CPU/GPU overlap.
    if (StableFrontEnabled() && frontSnapshot && frontSnapshot->coverageComplete &&
        frontSnapshot->frameSeen < currentFrame)
        CommitSnapshotForPresent(*frontSnapshot);

    const bool haveStableFront = (StableFrontEnabled() || completeFrontOnly) && frontSnapshot && frontSnapshot->presentValid &&
                                 frontSnapshot->presentImage != VK_NULL_HANDLE;
    const bool haveCurrentFront = frontSnapshot && frontSnapshot->frameSeen == currentFrame &&
                                  (!completeFrontOnly || frontSnapshot->coverageComplete);

    // Low-volume presentation diagnostics: only report when the source-selection
    // state changes. This is intentionally independent of the verbose front probe
    // so it can stay enabled during normal interactive reproduction without
    // perturbing frame pacing.
    {
        const uint64_t generation = frontSnapshot ? frontSnapshot->coverageGeneration : 0;
        const uint64_t presented = frontSnapshot ? frontSnapshot->presentedGeneration : 0;
        const uint64_t seen = frontSnapshot ? frontSnapshot->frameSeen : 0;
        const uint64_t signature =
            (uint64_t(frontKey) << 32) ^
            (uint64_t(g_activeColorInfo) << 1) ^
            (haveStableFront ? 0x8000000000000000ull : 0ull) ^
            (haveCurrentFront ? 0x4000000000000000ull : 0ull) ^
            (frontSnapshot && frontSnapshot->coverageComplete ? 0x2000000000000000ull : 0ull) ^
            (generation * 0x9E3779B185EBCA87ull) ^
            (presented * 0xC2B2AE3D27D4EB4Full) ^ seen;
        static uint64_t lastSignature = ~0ull;
        static uint32_t reports = 0;
        if (signature != lastSignature && reports < 160)
        {
            lastSignature = signature;
            ++reports;
            KLOG("[present select] frame=%llu front=%08X key=%08X snap=%u stable=%u current=%u "
                 "complete=%u seen=%llu gen=%llu presented=%llu copies=%llu active=%08X\n",
                 static_cast<unsigned long long>(currentFrame), frontBuffer, frontKey,
                 frontSnapshot ? 1u : 0u, haveStableFront ? 1u : 0u,
                 haveCurrentFront ? 1u : 0u,
                 frontSnapshot && frontSnapshot->coverageComplete ? 1u : 0u,
                 static_cast<unsigned long long>(seen),
                 static_cast<unsigned long long>(generation),
                 static_cast<unsigned long long>(presented),
                 static_cast<unsigned long long>(frontSnapshot ? frontSnapshot->copies : 0),
                 g_activeColorInfo);
        }
    }
    VkImage sourceImage = VK_NULL_HANDLE;
    uint32_t sourceWidth = g_extent.width;
    uint32_t sourceHeight = g_extent.height;
    // Diagnostic-only snapshot presentation. This lets us inspect render-to-
    // texture inputs (for example the motion-blur vector buffers) without
    // changing normal source selection. The value is a hexadecimal guest key.
    uint32_t debugPresentSnapshotKey = []() -> uint32_t {
        static const std::string toggleFile = [] {
            const char* value = std::getenv("MOJORECOMP_DEBUG_PRESENT_SNAPSHOT_FILE");
            return value ? std::string(value) : std::string{};
        }();
        static const uint32_t fixedKey = [] {
            const char* value = std::getenv("MOJORECOMP_DEBUG_PRESENT_SNAPSHOT");
            if (!value || !*value)
                return UINT32_MAX;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 16);
            return end != value ? (static_cast<uint32_t>(parsed) & 0x1FFFFFFFu) : UINT32_MAX;
        }();

        if (toggleFile.empty())
            return fixedKey;

        using Clock = std::chrono::steady_clock;
        static Clock::time_point nextPoll{};
        static uint32_t key = UINT32_MAX;
        const auto now = Clock::now();
        if (now >= nextPoll)
        {
            nextPoll = now + std::chrono::milliseconds(50);
            std::ifstream input(toggleFile);
            std::string value;
            if (input)
                input >> value;
            if (value.empty() || value == "0")
                key = UINT32_MAX;
            else
            {
                char* end = nullptr;
                const unsigned long parsed = std::strtoul(value.c_str(), &end, 16);
                key = end != value.c_str()
                    ? (static_cast<uint32_t>(parsed) & 0x1FFFFFFFu)
                    : UINT32_MAX;
            }
        }
        return key;
    }();
    if (indexHashEventCapture)
    {
        const uint32_t eventKey = g_indexHashCaptureSnapshotKey.load(std::memory_order_relaxed);
        if (eventKey != UINT32_MAX)
            debugPresentSnapshotKey = eventKey;
    }
    ResolveSnapshot* debugPresentSnapshot =
        debugPresentSnapshotKey != UINT32_MAX ? FindSnapshot(debugPresentSnapshotKey) : nullptr;
    // The freshly resolved front buffer is authoritative for this SWAP. A
    // preserved stable image is only a fallback for a SWAP where the title did
    // not resolve that buffer again. Preferring the stable image here made an
    // old transition frame override newer complete UI frames, producing the
    // whole-screen/text flicker seen in the frontend.
    if (debugPresentSnapshot && debugPresentSnapshot->copies && !debugPresentSnapshot->depth)
    {
        TransitionSnapshot(*debugPresentSnapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        sourceImage = debugPresentSnapshot->image;
        sourceWidth = debugPresentSnapshot->width;
        sourceHeight = debugPresentSnapshot->height;
        static uint32_t debugReports = 0;
        if (debugReports++ < 8)
            KLOG("[debug snapshot present] frame=%llu key=%08X size=%ux%u copies=%llu seen=%llu\n",
                 static_cast<unsigned long long>(currentFrame), debugPresentSnapshotKey,
                 sourceWidth, sourceHeight,
                 static_cast<unsigned long long>(debugPresentSnapshot->copies),
                 static_cast<unsigned long long>(debugPresentSnapshot->frameSeen));
    }
    else if (completeFrontOnly && haveStableFront)
    {
        TransitionSnapshotPresent(*frontSnapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        sourceImage = frontSnapshot->presentImage;
        sourceWidth = frontSnapshot->width;
        sourceHeight = frontSnapshot->height;
        g_snapshotPresents.fetch_add(1, std::memory_order_relaxed);
    }
    else if (haveCurrentFront)
    {
        TransitionSnapshot(*frontSnapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        sourceImage = frontSnapshot->image;
        sourceWidth = frontSnapshot->width;
        sourceHeight = frontSnapshot->height;
        const uint64_t n = g_snapshotPresents.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4)
            KLOG("Vulkan present uses resolved front snapshot: front=%08X key=%08X "
                 "%ux%u copies=%llu frame=%llu\n",
                 frontBuffer, frontKey, sourceWidth, sourceHeight,
                 static_cast<unsigned long long>(frontSnapshot->copies),
                 static_cast<unsigned long long>(currentFrame));
    }
    else if (haveStableFront)
    {
        TransitionSnapshotPresent(*frontSnapshot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        sourceImage = frontSnapshot->presentImage;
        sourceWidth = frontSnapshot->width;
        sourceHeight = frontSnapshot->height;
        const uint64_t n = g_snapshotPresents.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4)
            KLOG("Vulkan present uses stable resolved front: front=%08X key=%08X "
                 "%ux%u generation=%llu frame=%llu\n",
                 frontBuffer, frontKey, sourceWidth, sourceHeight,
                 static_cast<unsigned long long>(frontSnapshot->presentedGeneration),
                 static_cast<unsigned long long>(currentFrame));
    }
    else
    {
        if (ColorBacking* activeBacking = ActiveColorBacking())
        {
            if (g_hostMsaa2x)
            {
                TransitionColorResolveBacking(*activeBacking, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                sourceImage = activeBacking->resolveImage;
            }
            else
            {
                TransitionColorBacking(*activeBacking, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                sourceImage = activeBacking->image;
            }
        }
        else
        {
            if (g_hostMsaa2x)
            {
                TransitionResolveTarget(g_colorResolveImage, g_colorResolveLayout,
                                        VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                sourceImage = g_colorResolveImage;
            }
            else
            {
                VkImageMemoryBarrier live{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                live.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                live.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                live.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                live.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                live.srcQueueFamilyIndex = live.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                live.image = g_colorImage;
                live.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                       0, nullptr, 0, nullptr, 1, &live);
                sourceImage = g_colorImage;
            }
        }
        const uint64_t n = g_fallbackPresents.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4)
            KLOG("Vulkan present falls back to live EDRAM: front=%08X key=%08X "
                 "swap=%ux%u snapshot=%s\n",
                 frontBuffer, frontKey, width, height,
                 frontSnapshot ? "stale" : "missing");
    }

    const bool preserveNative16x9 = mojorecomp::gpu::ShouldPreserveNativePresentation(
        g_physicalTileContentFrame == currentFrame,
        g_sceneTilingLastIndexedFrame == currentFrame);
    const bool fxaaDrawn = DrawFxaa(imageIndex, sourceImage, sourceWidth, sourceHeight,
                                    preserveNative16x9);
    VkImageLayout swapLayout = fxaaDrawn
        ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (!fxaaDrawn)
    {
        VkImageMemoryBarrier swapToWrite{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        swapToWrite.srcAccessMask = 0;
        swapToWrite.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        swapToWrite.oldLayout = (imageIndex < g_imageInitialized.size() && g_imageInitialized[imageIndex])
                                    ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        swapToWrite.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        swapToWrite.srcQueueFamilyIndex = swapToWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapToWrite.image = g_images[imageIndex];
        swapToWrite.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               0, nullptr, 0, nullptr, 1, &swapToWrite);

        const VkClearColorValue black{};
        const VkImageSubresourceRange wholeSwap{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCmdClearColorImage(g_commandBuffer, g_images[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &black, 1, &wholeSwap);

        const VkRect2D contentRect = OutputContentRect(preserveNative16x9);
        const auto sourceCrop = mojorecomp::gpu::ScenePresentationSourceCrop(
            sourceWidth, sourceHeight, ConfiguredAspectRatio(), preserveNative16x9);
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = {
            static_cast<int32_t>(sourceCrop.x * g_resolutionScale),
            static_cast<int32_t>(sourceCrop.y * g_resolutionScale), 0};
        blit.srcOffsets[1] = {
            static_cast<int32_t>((sourceCrop.x + sourceCrop.width) * g_resolutionScale),
            static_cast<int32_t>((sourceCrop.y + sourceCrop.height) * g_resolutionScale), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = {contentRect.offset.x, contentRect.offset.y, 0};
        blit.dstOffsets[1] = {
            contentRect.offset.x + static_cast<int32_t>(contentRect.extent.width),
            contentRect.offset.y + static_cast<int32_t>(contentRect.extent.height), 1};
        p_vkCmdBlitImage(g_commandBuffer, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         g_images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &blit, VK_FILTER_NEAREST);
    }

    const bool overlayDrawn = DrawDebugOverlay(imageIndex, swapLayout);
    if (overlayDrawn)
        swapLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    const bool triggeredFrontCapture = FrontCaptureTrigger(currentFrame);
    const bool explicitFrontCapture = FrontDiagnosticFrame(currentFrame) ||
                                      triggeredFrontCapture || indexHashEventCapture;
    if (indexHashEventCapture)
        KLOG("[index hash event] capture present frame=%llu\n",
             static_cast<unsigned long long>(currentFrame));
    if (explicitFrontCapture)
        KLOG("[front probe] present frame=%llu front=%08X key=%08X stable=%u current=%u "
             "seen=%llu copies=%llu generation=%llu source=%ux%u swap=%ux%u active=%08X\n",
             currentFrame, frontBuffer, frontKey, haveStableFront, haveCurrentFront,
             frontSnapshot ? frontSnapshot->frameSeen : 0,
             frontSnapshot ? frontSnapshot->copies : 0,
             frontSnapshot ? frontSnapshot->presentedGeneration : 0,
             sourceWidth, sourceHeight, g_outputExtent.width, g_outputExtent.height,
             g_activeColorInfo);
    if (triggeredFrontCapture)
    {
        KLOG("[snapshot inventory] frame=%llu count=%zu\n",
             static_cast<unsigned long long>(currentFrame), g_snapshots.size());
        for (const auto& snapshot : g_snapshots)
        {
            KLOG("[snapshot inventory] key=%08X depth=%u seen=%llu copies=%llu generation=%llu size=%ux%u\n",
                 snapshot.key, snapshot.depth ? 1u : 0u,
                 static_cast<unsigned long long>(snapshot.frameSeen),
                 static_cast<unsigned long long>(snapshot.copies),
                 static_cast<unsigned long long>(snapshot.presentedGeneration),
                 snapshot.width, snapshot.height);
        }
    }
    const bool captureRaster =
                               ((g_framesInFlight == 1 && !g_readbackReported) || explicitFrontCapture) &&
                               !g_readbackPending &&
                               g_readbackBytes && g_draws.load(std::memory_order_relaxed) != 0 &&
                               sourceWidth == g_extent.width && sourceHeight == g_extent.height;
    if (captureRaster)
    {
        g_frontReadbackFrame = explicitFrontCapture ? currentFrame : 0;
        VkBufferImageCopy copy{};
        copy.bufferOffset = g_readbackOffset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {g_internalExtent.width, g_internalExtent.height, 1};
        p_vkCmdCopyImageToBuffer(g_commandBuffer, sourceImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 g_uploadBuffer, 1, &copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = g_uploadBuffer;
        host.offset = g_readbackOffset;
        host.size = g_readbackBytes;
        p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, 0,
                               0, nullptr, 1, &host, 0, nullptr);
    }

    VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toPresent.srcAccessMask = swapLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        : VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.oldLayout = swapLayout;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = g_images[imageIndex];
    toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toPresent.subresourceRange.levelCount = 1;
    toPresent.subresourceRange.layerCount = 1;
    p_vkCmdPipelineBarrier(g_commandBuffer,
                           swapLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                               : VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toPresent);

    if (reportMsaaPresent)
        KLOG("[msaa2x present] ending command buffer\n");
    auto& frameContext = g_frameContexts[g_frameSlot];
    if (g_gpuDrawTimestamps && frameContext.timestampPool)
        p_vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              frameContext.timestampPool, 1);
    if (p_vkEndCommandBuffer(g_commandBuffer) != VK_SUCCESS)
        return false;
    if (reportMsaaPresent)
        KLOG("[msaa2x present] command buffer ended\n");
    if (p_vkResetFences(g_device, 1, &g_fence) != VK_SUCCESS)
        return false;
    const VkPipelineStageFlags waitStage = fxaaDrawn
        ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        : VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g_imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    if (imageIndex >= g_presentReady.size() || !g_presentReady[imageIndex])
        return false;
    const VkSemaphore presentReady = g_presentReady[imageIndex];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &presentReady;
    if (reportMsaaPresent)
        KLOG("[msaa2x present] queue submit begin\n");
    if (p_vkQueueSubmit(g_queue, 1, &submit, g_fence) != VK_SUCCESS)
        return false;
    if (g_gpuDrawTimestamps && frameContext.timestampPool)
        frameContext.timestampPending = true;
    if (reportMsaaPresent)
        KLOG("[msaa2x present] queue submit returned\n");
    if (captureRaster)
    {
        g_readbackPending = true;
        g_readbackFrameSlot = g_frameSlot;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &presentReady;
    present.swapchainCount = 1;
    present.pSwapchains = &g_swapchain;
    present.pImageIndices = &imageIndex;
    result = p_vkQueuePresentKHR(g_queue, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return false;

    if (imageIndex < g_imageInitialized.size())
        g_imageInitialized[imageIndex] = true;
    g_colorInitialized = true;
    g_frameOpen = false;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_frameSlot = (g_frameSlot + 1u) % g_framesInFlight;
    return true;
}

} // namespace

bool VkPresenter_Init(void* nativeWindow, uint32_t width, uint32_t height)
{
    if (g_active)
        return true;
    if (!nativeWindow)
        return false;

    // width/height are the title's guest-visible logical framebuffer size.
    // The swapchain gets its own independently resizable output extent.
    g_extent = {width, height};
    g_resolutionScale = std::clamp(mojorecomp::config::Get().resolutionScale, 1u, 3u);
    g_internalExtent = InternalExtentForLogical(width, height);
    g_outputExtent = {width, height};

    g_vulkanModule = LoadLibraryW(L"vulkan-1.dll");
    if (!g_vulkanModule)
    {
        KLOG("Vulkan loader not found (vulkan-1.dll)\n");
        return false;
    }
    p_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkanModule, "vkGetInstanceProcAddr"));
    if (!p_vkGetInstanceProcAddr)
        return false;
    p_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        p_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!p_vkCreateInstance)
        return false;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MojoRecomp";
    app.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    app.pEngineName = "MojoRecomp";
    app.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    VkResult result = p_vkCreateInstance(&instanceInfo, nullptr, &g_instance);
    if (result != VK_SUCCESS)
    {
        KLOG("vkCreateInstance failed: %d\n", result);
        return false;
    }

    bool loaded = true;
    loaded &= LoadInstance(p_vkDestroyInstance, "vkDestroyInstance");
    loaded &= LoadInstance(p_vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    loaded &= LoadInstance(p_vkEnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceImageFormatProperties,
                           "vkGetPhysicalDeviceImageFormatProperties");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    loaded &= LoadInstance(p_vkGetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    loaded &= LoadInstance(p_vkCreateWin32SurfaceKHR, "vkCreateWin32SurfaceKHR");
    loaded &= LoadInstance(p_vkDestroySurfaceKHR, "vkDestroySurfaceKHR");
    loaded &= LoadInstance(p_vkCreateDevice, "vkCreateDevice");
    loaded &= LoadInstance(p_vkGetDeviceProcAddr, "vkGetDeviceProcAddr");
    if (!loaded)
        return false;

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(nativeWindow);
    if (p_vkCreateWin32SurfaceKHR(g_instance, &surfaceInfo, nullptr, &g_surface) != VK_SUCCESS)
        return false;

    uint32_t physicalCount = 0;
    if (p_vkEnumeratePhysicalDevices(g_instance, &physicalCount, nullptr) != VK_SUCCESS || !physicalCount)
        return false;
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    p_vkEnumeratePhysicalDevices(g_instance, &physicalCount, physicalDevices.data());
    for (VkPhysicalDevice candidate : physicalDevices)
    {
        uint32_t queueCount = 0;
        p_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        p_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        for (uint32_t q = 0; q < queueCount; ++q)
        {
            VkBool32 present = VK_FALSE;
            p_vkGetPhysicalDeviceSurfaceSupportKHR(candidate, q, g_surface, &present);
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                g_physicalDevice = candidate;
                g_queueFamily = q;
                g_gpuTimestampValidBits = queues[q].timestampValidBits;
                break;
            }
        }
        if (g_physicalDevice)
            break;
    }
    if (!g_physicalDevice)
    {
        KLOG("Vulkan: no graphics+present queue found\n");
        return false;
    }

    VkPhysicalDeviceProperties properties{};
    p_vkGetPhysicalDeviceProperties(g_physicalDevice, &properties);
    if (g_internalExtent.width > properties.limits.maxImageDimension2D ||
        g_internalExtent.height > properties.limits.maxImageDimension2D)
    {
        KLOG("Vulkan Resolution Scale %ux exceeds max 2D image extent: internal=%ux%u max=%u\n",
             g_resolutionScale, g_internalExtent.width, g_internalExtent.height,
             properties.limits.maxImageDimension2D);
        return false;
    }
    KLOG("Vulkan extents: logical=%ux%u internal=%ux%u scale=%ux initialOutput=%ux%u\n",
         g_extent.width, g_extent.height,
         g_internalExtent.width, g_internalExtent.height, g_resolutionScale,
         g_outputExtent.width, g_outputExtent.height);
    g_hostColorSampleCounts = properties.limits.framebufferColorSampleCounts;
    g_hostDepthSampleCounts = properties.limits.framebufferDepthSampleCounts;
    g_hostStencilSampleCounts = properties.limits.framebufferStencilSampleCounts;
    g_gpuTimestampPeriodNs = properties.limits.timestampPeriod;
    g_gpuTimestampSupported = properties.limits.timestampComputeAndGraphics == VK_TRUE &&
                              g_gpuTimestampValidBits != 0 &&
                              g_gpuTimestampPeriodNs > 0.0f;
    g_maxSamplerAnisotropy = std::max(1.0f, properties.limits.maxSamplerAnisotropy);
    const char* gpuTimestampEnv = std::getenv("MOJORECOMP_GPU_DRAW_TIMESTAMPS");
    g_gpuDrawTimestamps = gpuTimestampEnv && std::strcmp(gpuTimestampEnv, "1") == 0 &&
                          g_gpuTimestampSupported;
    KLOG("Vulkan GPU: %s (api %u.%u.%u, driver=%08X)\n", properties.deviceName,
         VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
         VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    KLOG("Vulkan framebuffer samples: color=%02X depth=%02X stencil=%02X common=%02X\n",
         uint32_t(g_hostColorSampleCounts), uint32_t(g_hostDepthSampleCounts),
         uint32_t(g_hostStencilSampleCounts),
         uint32_t(g_hostColorSampleCounts & g_hostDepthSampleCounts & g_hostStencilSampleCounts));
    if (gpuTimestampEnv && std::strcmp(gpuTimestampEnv, "1") == 0)
        KLOG("Vulkan GPU timestamp support: requested=1 supported=%u period=%.3f ns validBits=%u\n",
             g_gpuTimestampSupported ? 1u : 0u, g_gpuTimestampPeriodNs, g_gpuTimestampValidBits);

    VkPhysicalDeviceDepthStencilResolveProperties depthResolveProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &depthResolveProperties;
    p_vkGetPhysicalDeviceProperties2(g_physicalDevice, &properties2);

    bool hasFragmentShaderInterlockExtension = false;
    bool hasShaderStencilExportExtension = false;
    uint32_t deviceExtensionCount = 0;
    if (p_vkEnumerateDeviceExtensionProperties(g_physicalDevice, nullptr,
                                                &deviceExtensionCount, nullptr) == VK_SUCCESS &&
        deviceExtensionCount)
    {
        std::vector<VkExtensionProperties> deviceExtensionProperties(deviceExtensionCount);
        if (p_vkEnumerateDeviceExtensionProperties(g_physicalDevice, nullptr,
                                                    &deviceExtensionCount,
                                                    deviceExtensionProperties.data()) == VK_SUCCESS)
        {
            for (const auto& extension : deviceExtensionProperties)
            {
                if (std::strcmp(extension.extensionName,
                                VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0)
                    hasFragmentShaderInterlockExtension = true;
                if (std::strcmp(extension.extensionName,
                                VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME) == 0)
                    hasShaderStencilExportExtension = true;
            }
        }
    }

    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT haveInterlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    have13.pNext = hasFragmentShaderInterlockExtension
        ? static_cast<void*>(&haveInterlock)
        : static_cast<void*>(&have12);
    if (hasFragmentShaderInterlockExtension)
        haveInterlock.pNext = &have12;
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have13;
    p_vkGetPhysicalDeviceFeatures2(g_physicalDevice, &have);
    g_samplerAnisotropySupported = have.features.samplerAnisotropy == VK_TRUE;
    const uint32_t requestedFiltering = mojorecomp::config::Get().textureFiltering;
    g_effectiveSamplerAnisotropy =
        requestedFiltering > 1 && g_samplerAnisotropySupported
            ? std::min(static_cast<float>(requestedFiltering), g_maxSamplerAnisotropy)
            : 1.0f;
    KLOG("Vulkan fragment shader interlock: ext=%u sample=%u pixel=%u shadingRate=%u\n",
         hasFragmentShaderInterlockExtension ? 1u : 0u,
         haveInterlock.fragmentShaderSampleInterlock ? 1u : 0u,
         haveInterlock.fragmentShaderPixelInterlock ? 1u : 0u,
         haveInterlock.fragmentShaderShadingRateInterlock ? 1u : 0u);

    const bool hostMsaa2xSupported =
        (properties.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) &&
        (properties.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_2_BIT) &&
        (properties.limits.framebufferStencilSampleCounts & VK_SAMPLE_COUNT_2_BIT);
    g_hostMsaa2x = HostMsaa2xRequested() && hostMsaa2xSupported;
    g_hostMsaaDepthSampleZero =
        (depthResolveProperties.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0 &&
        depthResolveProperties.independentResolveNone == VK_TRUE;
    KLOG("Vulkan Xenos host MSAA: requested2x=%u supported2x=%u active2x=%u\n",
         HostMsaa2xRequested() ? 1u : 0u,
         hostMsaa2xSupported ? 1u : 0u,
         g_hostMsaa2x ? 1u : 0u);
    KLOG("Vulkan Xenos depth resolve: sampleZero=%u independentNone=%u enabled=%u\n",
         (depthResolveProperties.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) ? 1u : 0u,
         depthResolveProperties.independentResolveNone ? 1u : 0u,
         HostMsaa2xDepthResolveEnabled() ? 1u : 0u);
    if (!have.features.shaderInt64 || !have12.bufferDeviceAddress || !have13.dynamicRendering)
    {
        KLOG("Vulkan raster requirements missing: shaderInt64=%u BDA=%u dynamicRendering=%u\n",
             have.features.shaderInt64, have12.bufferDeviceAddress, have13.dynamicRendering);
        return false;
    }
    if (!have12.runtimeDescriptorArray || !have.features.shaderSampledImageArrayDynamicIndexing ||
        properties.limits.maxBoundDescriptorSets < 4 ||
        properties.limits.maxPerStageDescriptorSampledImages < 3 * kTextureSlots ||
        properties.limits.maxDescriptorSetSampledImages < 3 * kTextureSlots ||
        properties.limits.maxPerStageDescriptorSamplers < kTextureSlots ||
        properties.limits.maxDescriptorSetSamplers < kTextureSlots ||
        properties.limits.maxPerStageResources < 4 * kTextureSlots)
    {
        KLOG("Vulkan texture ABI requirements missing: runtimeArray=%u dynamicSampledIndex=%u\n",
             have12.runtimeDescriptorArray, have.features.shaderSampledImageArrayDynamicIndexing);
        return false;
    }

    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enable12.bufferDeviceAddress = VK_TRUE;
    enable12.runtimeDescriptorArray = VK_TRUE;
    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.dynamicRendering = VK_TRUE;
    enable13.pNext = &enable12;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enable.features.shaderInt64 = VK_TRUE;
    enable.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    enable.features.sampleRateShading = have.features.sampleRateShading;
    enable.features.textureCompressionBC = have.features.textureCompressionBC;
    enable.features.samplerAnisotropy =
        g_effectiveSamplerAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    enable.pNext = &enable13;
    g_textureCompressionBC = have.features.textureCompressionBC == VK_TRUE;
    g_sampleRateShading = have.features.sampleRateShading == VK_TRUE;
    KLOG("Vulkan optional texture compression: BC=%u sampleRateShading=%u\n",
         g_textureCompressionBC ? 1u : 0u, g_sampleRateShading ? 1u : 0u);
    KLOG("Vulkan texture filtering: requested=%s anisotropySupported=%u max=%.1f effective=%.1f\n",
         mojorecomp::config::TextureFilteringToString(requestedFiltering),
         g_samplerAnisotropySupported ? 1u : 0u,
         g_maxSamplerAnisotropy, g_effectiveSamplerAnisotropy);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = g_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (hasShaderStencilExportExtension)
        deviceExtensions.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &enable;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    result = p_vkCreateDevice(g_physicalDevice, &deviceInfo, nullptr, &g_device);
    if (result != VK_SUCCESS)
    {
        KLOG("vkCreateDevice failed: %d\n", result);
        return false;
    }
    g_shaderStencilExport = hasShaderStencilExportExtension;
    KLOG("Vulkan shader stencil export: ext=%u enabled=%u\n",
         hasShaderStencilExportExtension ? 1u : 0u, g_shaderStencilExport ? 1u : 0u);

#define LOAD_DEVICE(name) loaded &= LoadDevice(p_##name, #name)
    loaded = true;
    LOAD_DEVICE(vkDestroyDevice);
    LOAD_DEVICE(vkGetDeviceQueue);
    LOAD_DEVICE(vkCreateSwapchainKHR);
    LOAD_DEVICE(vkDestroySwapchainKHR);
    LOAD_DEVICE(vkGetSwapchainImagesKHR);
    LOAD_DEVICE(vkAcquireNextImageKHR);
    LOAD_DEVICE(vkQueuePresentKHR);
    LOAD_DEVICE(vkCreateCommandPool);
    LOAD_DEVICE(vkDestroyCommandPool);
    LOAD_DEVICE(vkAllocateCommandBuffers);
    LOAD_DEVICE(vkResetCommandBuffer);
    LOAD_DEVICE(vkBeginCommandBuffer);
    LOAD_DEVICE(vkEndCommandBuffer);
    LOAD_DEVICE(vkCmdPipelineBarrier);
    LOAD_DEVICE(vkCmdBlitImage);
    LOAD_DEVICE(vkCmdCopyImage);
    LOAD_DEVICE(vkCmdResolveImage);
    LOAD_DEVICE(vkCmdCopyImageToBuffer);
    LOAD_DEVICE(vkCmdCopyBufferToImage);
    LOAD_DEVICE(vkCmdClearColorImage);
    LOAD_DEVICE(vkCmdClearDepthStencilImage);
    LOAD_DEVICE(vkCmdClearAttachments);
    LOAD_DEVICE(vkCreateSemaphore);
    LOAD_DEVICE(vkDestroySemaphore);
    LOAD_DEVICE(vkCreateFence);
    LOAD_DEVICE(vkDestroyFence);
    LOAD_DEVICE(vkWaitForFences);
    LOAD_DEVICE(vkResetFences);
    LOAD_DEVICE(vkCreateQueryPool);
    LOAD_DEVICE(vkDestroyQueryPool);
    LOAD_DEVICE(vkCmdResetQueryPool);
    LOAD_DEVICE(vkCmdWriteTimestamp);
    LOAD_DEVICE(vkGetQueryPoolResults);
    LOAD_DEVICE(vkQueueSubmit);
    LOAD_DEVICE(vkDeviceWaitIdle);
    LOAD_DEVICE(vkCreateBuffer);
    LOAD_DEVICE(vkDestroyBuffer);
    LOAD_DEVICE(vkGetBufferMemoryRequirements);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkBindBufferMemory);
    LOAD_DEVICE(vkMapMemory);
    LOAD_DEVICE(vkUnmapMemory);
    LOAD_DEVICE(vkGetBufferDeviceAddress);
    LOAD_DEVICE(vkCreateImage);
    LOAD_DEVICE(vkDestroyImage);
    LOAD_DEVICE(vkGetImageMemoryRequirements);
    LOAD_DEVICE(vkBindImageMemory);
    LOAD_DEVICE(vkCreateImageView);
    LOAD_DEVICE(vkDestroyImageView);
    LOAD_DEVICE(vkCmdBeginRendering);
    LOAD_DEVICE(vkCmdEndRendering);
    LOAD_DEVICE(vkCreateShaderModule);
    LOAD_DEVICE(vkDestroyShaderModule);
    LOAD_DEVICE(vkCreatePipelineLayout);
    LOAD_DEVICE(vkDestroyPipelineLayout);
    LOAD_DEVICE(vkCreatePipelineCache);
    LOAD_DEVICE(vkDestroyPipelineCache);
    LOAD_DEVICE(vkGetPipelineCacheData);
    LOAD_DEVICE(vkCreateGraphicsPipelines);
    LOAD_DEVICE(vkDestroyPipeline);
    LOAD_DEVICE(vkCmdBindPipeline);
    LOAD_DEVICE(vkCmdSetViewport);
    LOAD_DEVICE(vkCmdSetScissor);
    LOAD_DEVICE(vkCmdSetBlendConstants);
    LOAD_DEVICE(vkCmdSetStencilReference);
    LOAD_DEVICE(vkCmdSetStencilCompareMask);
    LOAD_DEVICE(vkCmdSetStencilWriteMask);
    LOAD_DEVICE(vkCmdBindVertexBuffers);
    LOAD_DEVICE(vkCmdBindIndexBuffer);
    LOAD_DEVICE(vkCmdPushConstants);
    LOAD_DEVICE(vkCmdDraw);
    LOAD_DEVICE(vkCmdDrawIndexed);
    LOAD_DEVICE(vkCreateDescriptorSetLayout);
    LOAD_DEVICE(vkDestroyDescriptorSetLayout);
    LOAD_DEVICE(vkCreateDescriptorPool);
    LOAD_DEVICE(vkDestroyDescriptorPool);
    LOAD_DEVICE(vkAllocateDescriptorSets);
    LOAD_DEVICE(vkUpdateDescriptorSets);
    LOAD_DEVICE(vkCmdBindDescriptorSets);
    LOAD_DEVICE(vkCreateSampler);
    LOAD_DEVICE(vkDestroySampler);
#undef LOAD_DEVICE
    if (!loaded)
        return false;

    // Pipeline creation is one of the largest first-use hitches in Vulkan.
    // Keep a device/driver-specific cache alive for every renderer pipeline and
    // persist it across runs. Failure is non-fatal; rendering still works with
    // VK_NULL_HANDLE just as before.
    CreatePersistentPipelineCache(properties);

    p_vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);
    if (!g_queue || !CreateSwapchain(width, height) || !CreateCommandState() ||
        !CreateUploadArena() || !CreateColorTarget() || !CreateDepthTarget() ||
        !CreateTextureDescriptors() || !CreatePipelineLayout())
        return false;

    // Keep GDI font rasterization and DXC compilation out of the final-present
    // path. Vulkan objects and the tiny atlas upload remain lazy until Debug
    // Mode is first enabled.
    if (!PrepareDebugOverlayAssets())
        KLOG("Vulkan debug overlay asset preparation failed; overlay unavailable\n");

    g_active = true;
    KLOG("Vulkan raster gate ACTIVE (BDA + dynamic rendering + translated SPIR-V)\n");
    return true;
}

bool VkPresenter_Active()
{
    return g_active;
}

bool VkPresenter_GetExtentInfo(VkPresenterExtentInfo& out)
{
    if (!g_active)
        return false;
    out.logicalWidth = g_extent.width;
    out.logicalHeight = g_extent.height;
    out.internalWidth = g_internalExtent.width;
    out.internalHeight = g_internalExtent.height;
    out.outputWidth = g_outputExtent.width;
    out.outputHeight = g_outputExtent.height;
    out.resolutionScale = g_resolutionScale;
    const VkRect2D content = OutputContentRect();
    out.contentX = content.offset.x;
    out.contentY = content.offset.y;
    out.contentWidth = content.extent.width;
    out.contentHeight = content.extent.height;
    const auto sceneScale = AspectScaleForDraw(true);
    const auto uiScale = AspectScaleForDraw(false);
    out.sceneScaleX = sceneScale[0];
    out.sceneScaleY = sceneScale[1];
    out.uiScaleX = uiScale[0];
    out.uiScaleY = uiScale[1];
    return true;
}

bool VkPresenter_SetOutputExtent(uint32_t width, uint32_t height)
{
    if (!g_active)
        return false;
    return RecreateSwapchain(width, height);
}

bool VkPresenter_ValidateConfiguredPostProcess()
{
    if (!g_active)
        return false;
    const auto aa = mojorecomp::config::Get().antiAliasing;
    if (aa == mojorecomp::config::AntiAliasing::Off)
        return true;
    return EnsureFxaaResources();
}

bool VkPresenter_Draw(uint8_t* guestBase, const Pm4Draw& draw,
                       const uint32_t* regs, uint64_t vsHash, uint64_t psHash)
{
    if (!g_active || !guestBase || !regs)
        return false;
    struct ResolveTraceLastRaster
    {
        uint64_t vs = 0;
        uint64_t ps = 0;
        uint64_t binMask = 0;
        uint64_t binSelect = 0;
        uint64_t generation = 0;
        uint32_t prim = 0;
        uint32_t count = 0;
        uint32_t scissorTl = 0;
        uint32_t scissorBr = 0;
        uint32_t windowOffset = 0;
        bool indexed = false;
        bool predicated = false;
    };
    static ResolveTraceLastRaster lastRaster{};

    const uint32_t mode = regs[xenos::kRbModeControl] & 7u;
    if (mode == 4 || mode == 5)
    {
        lastRaster.vs = vsHash;
        lastRaster.ps = psHash;
        lastRaster.binMask = draw.binMask;
        lastRaster.binSelect = draw.binSelect;
        lastRaster.generation = g_renderWriteGeneration;
        lastRaster.prim = draw.primType;
        lastRaster.count = draw.indexCount;
        lastRaster.scissorTl = regs[xenos::kPaScWindowScissorTl];
        lastRaster.scissorBr = regs[xenos::kPaScWindowScissorBr];
        lastRaster.windowOffset = regs[xenos::kPaScWindowOffset];
        lastRaster.indexed = draw.indexed;
        lastRaster.predicated = draw.predicated;

        static const bool traceTileVsConstants = [] {
            const char* value = std::getenv("MOJORECOMP_TILE_VS_TRACE");
            return value && value[0] && value[0] != '0';
        }();
        if (traceTileVsConstants && vsHash == 0x1D72F92BDA785FDCull)
        {
            const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
            const uint32_t tl = regs[xenos::kPaScWindowScissorTl];
            const uint32_t br = regs[xenos::kPaScWindowScissorBr];
            const uint32_t x0 = tl & 0x7FFFu;
            const uint32_t x1 = br & 0x7FFFu;
            uint32_t tileBit = 0;
            if (x0 == 0 && x1 == 448) tileBit = 1u;
            else if (x0 == 416 && x1 == 864) tileBit = 2u;
            else if (x0 == 832 && x1 == 1280) tileBit = 4u;
            static uint64_t reportedFrame = 0;
            static uint32_t reportedTiles = 0;
            if (frame != reportedFrame)
            {
                reportedFrame = frame;
                reportedTiles = 0;
            }
            if (tileBit && !(reportedTiles & tileBit))
            {
                reportedTiles |= tileBit;
                KLOG("[tile vs] frame=%llu tile=%u x=%u..%u winOff=%08X\n",
                     static_cast<unsigned long long>(frame), tileBit, x0, x1,
                     regs[xenos::kPaScWindowOffset]);
                constexpr uint32_t kConstants[] = {4,5,6,7,12,13,14,15};
                for (uint32_t c : kConstants)
                {
                    const uint32_t* raw = regs + xenos::kAluConstantBase + c * 4;
                    KLOG("[tile vs] vc%u=%08X,%08X,%08X,%08X f=%g,%g,%g,%g\n",
                         c, raw[0], raw[1], raw[2], raw[3],
                         F32(raw[0]), F32(raw[1]), F32(raw[2]), F32(raw[3]));
                }
            }
        }
    }
    if (mode == 6)
    {
        static const bool traceResolveBins = [] {
            const char* value = std::getenv("MOJORECOMP_RESOLVE_BIN_TRACE");
            return value && value[0] && value[0] != '0';
        }();
        if (traceResolveBins)
        {
            const uint32_t surfW = regs[xenos::kRbCopyDestPitch] & 0x3FFFu;
            const uint32_t surfH = (regs[xenos::kRbCopyDestPitch] >> 16) & 0x3FFFu;
            const uint32_t tl = regs[xenos::kPaScWindowScissorTl];
            const uint32_t br = regs[xenos::kPaScWindowScissorBr];
            const uint32_t x0 = tl & 0x7FFFu;
            const uint32_t y0 = (tl >> 16) & 0x7FFFu;
            const uint32_t x1 = br & 0x7FFFu;
            const uint32_t y1 = (br >> 16) & 0x7FFFu;
            if (surfW == 1280 && surfH == 720 && y0 == 0 && y1 == 720)
            {
                const uint32_t dest = regs[xenos::kRbCopyDestBase] & 0xFFFFFFFCu;
                const uint32_t key = (dest - MacroTileOffset(x0, y0, surfW)) & 0x1FFFFFFFu;
                const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
                if (key == 0x0A63D000u || key == 0x09B75000u ||
                    key == 0x097DD000u || key == 0x09445000u)
                {
                    KLOG("[resolve bin] frame=%llu key=%08X dest=%08X rect=%u,%u-%u,%u "
                         "ctrl=%08X pred=%u bin=%016llX/%016llX gen=%llu draws=%llu "
                         "prev=VS%016llX/PS%016llX prim=%u count=%u idx=%u pred=%u "
                         "bin=%016llX/%016llX sc=%08X..%08X off=%08X pgen=%llu\n",
                         static_cast<unsigned long long>(frame), key, dest,
                         x0, y0, x1, y1, regs[xenos::kRbCopyControl],
                         draw.predicated ? 1u : 0u,
                         static_cast<unsigned long long>(draw.binMask),
                         static_cast<unsigned long long>(draw.binSelect),
                         static_cast<unsigned long long>(g_renderWriteGeneration),
                         static_cast<unsigned long long>(g_draws.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(lastRaster.vs),
                         static_cast<unsigned long long>(lastRaster.ps),
                         lastRaster.prim, lastRaster.count,
                         lastRaster.indexed ? 1u : 0u, lastRaster.predicated ? 1u : 0u,
                         static_cast<unsigned long long>(lastRaster.binMask),
                         static_cast<unsigned long long>(lastRaster.binSelect),
                         lastRaster.scissorTl, lastRaster.scissorBr, lastRaster.windowOffset,
                         static_cast<unsigned long long>(lastRaster.generation));
                }
            }
        }
        const auto start = PerfClock::now();
        const bool ok = ResolveColorSurface(guestBase, regs);
        g_perfResolveNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            PerfClock::now() - start).count());
        ++g_perfResolveCalls;
        return ok;
    }
    if (mode != 4 && mode != 5)
    {
        g_skippedMode.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (PrimitiveTopology(draw.primType) == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
    {
        g_skippedTopology.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    struct DrawCpuScope
    {
        PerfClock::time_point start = PerfClock::now();
        ~DrawCpuScope()
        {
            g_perfDrawTotalNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    PerfClock::now() - start).count());
        }
    } drawCpuScope;

    // Crash replays an entire 1280x720 scene three times through the same EDRAM
    // raster window. A real gameplay frame shows 504 matching draw signatures in
    // each pass; the only differences are the 0/416/832 logical window and the
    // matching 0/-416/-832 window offset. The host target is already full-size,
    // so render the first pass in logical coordinates and discard the two exact
    // replays. This is broader than looking only at BIN_MASK=0x3f: the title also
    // splits some geometry across the individual bits inside each two-bin group.
    const uint64_t currentFrame = g_frames.load(std::memory_order_relaxed) + 1;
    const uint32_t windowTl = regs[xenos::kPaScWindowScissorTl];
    const uint32_t windowBr = regs[xenos::kPaScWindowScissorBr];
    const uint32_t tileX0 = windowTl & 0x7FFFu;
    const uint32_t tileY0 = (windowTl >> 16) & 0x7FFFu;
    const uint32_t tileX1 = windowBr & 0x7FFFu;
    const uint32_t tileY1 = (windowBr >> 16) & 0x7FFFu;
    int32_t tileWindowX = 0, tileWindowY = 0;
    DecodeVertexWindowOffset(regs, tileWindowX, tileWindowY, false);
    const bool firstSceneTile = tileX0 == 0 && tileX1 == 448 &&
                                tileY0 == 0 && tileY1 == g_extent.height &&
                                tileWindowX == 0 && tileWindowY == 0;
    const bool middleSceneTile = tileX0 == 416 && tileX1 == 864 &&
                                 tileY0 == 0 && tileY1 == g_extent.height &&
                                 tileWindowX == -416 && tileWindowY == 0;
    const bool lastSceneTile = tileX0 == 832 && tileX1 == g_extent.width &&
                               tileY0 == 0 && tileY1 == g_extent.height &&
                               tileWindowX == -832 && tileWindowY == 0;
    const bool sceneTileWindow = firstSceneTile || middleSceneTile || lastSceneTile;
    // Headless validation can drive the frontend with the diagnostic autopilot,
    // but all synthetic input must stop the instant the real tiled 3D scene is
    // reached.  Doing this here is race-free compared with an external log
    // poller: the first indexed, predicated scene-tile draw is the transition
    // point we care about, and normal runs are untouched unless the stop-file
    // environment variable is explicitly set.
    if (draw.indexed && draw.predicated && sceneTileWindow)
    {
        static bool diagnosticAutopilotStopped = false;
        if (!diagnosticAutopilotStopped)
        {
            diagnosticAutopilotStopped = true;
            if (const char* stopPath = std::getenv("MOJORECOMP_AUTOPILOT_STOP_FILE");
                stopPath && *stopPath)
            {
                std::ofstream stop(stopPath, std::ios::binary | std::ios::trunc);
                if (stop)
                {
                    stop << "3d";
                    stop.flush();
                    KLOG("[headless] diagnostic input stopped at first real tiled 3D draw frame=%llu\n",
                         static_cast<unsigned long long>(currentFrame));
                }
            }
        }
    }
    if (CollapseTiledDrawsEnabled() && draw.indexed && draw.predicated && sceneTileWindow)
    {
        g_sceneTilingLastIndexedFrame = currentFrame;
        if ((regs[xenos::kRbDepthControl] & 0x7u) != 0)
            g_sceneTilingLastDepthFrame = currentFrame;
    }
    if (StitchTiledDrawsEnabled() && draw.indexed && draw.predicated && sceneTileWindow)
        g_sceneStitchLastIndexedFrame = currentFrame;
    const bool sceneTilingActive = CollapseTiledDrawsEnabled() && draw.predicated &&
                                   sceneTileWindow && g_sceneTilingLastIndexedFrame != 0 &&
                                   currentFrame <= g_sceneTilingLastIndexedFrame + 1;
    const bool sceneStitchActive = StitchTiledDrawsEnabled() &&
                                   g_sceneStitchLastIndexedFrame != 0 &&
                                   currentFrame <= g_sceneStitchLastIndexedFrame + 1;
    const bool offsetSceneTileDraw = sceneStitchActive && draw.predicated && !sceneTileWindow &&
                                     tileWindowY == 0 &&
                                     (tileWindowX == -416 || tileWindowX == -832);
    if (TraceStitchMissesEnabled() && offsetSceneTileDraw)
    {
        static uint32_t reports = 0;
        if (reports < 240)
        {
            ++reports;
            KLOG("[stitch miss] #%u frame=%llu VS=%016llX PS=%016llX indexed=%u prim=%u "
                 "bin=%016llX/%016llX win=%u,%u-%u,%u off=%d,%d broaden=%u\n",
                 reports, static_cast<unsigned long long>(currentFrame),
                 static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash),
                 draw.indexed ? 1u : 0u, draw.primType,
                 static_cast<unsigned long long>(draw.binMask),
                 static_cast<unsigned long long>(draw.binSelect),
                 tileX0, tileY0, tileX1, tileY1, tileWindowX, tileWindowY,
                 StitchOffsetPredicatedDrawsEnabled() ? 1u : 0u);
        }
    }
    const bool stitchedUnpredicatedTile = sceneStitchActive && sceneTileWindow &&
                                          !draw.predicated &&
                                          StitchUnpredicatedTileDrawsEnabled();
    const bool stitchedSceneTile = (sceneStitchActive && sceneTileWindow &&
                                    (draw.predicated || stitchedUnpredicatedTile)) ||
                                   (offsetSceneTileDraw && StitchOffsetPredicatedDrawsEnabled());
    if (TraceUnpredicatedTileDrawsEnabled() && sceneStitchActive && sceneTileWindow &&
        !draw.predicated)
    {
        static uint32_t reports = 0;
        if (reports < 160)
        {
            ++reports;
            KLOG("[stitch unpred] #%u frame=%llu VS=%016llX PS=%016llX indexed=%u prim=%u "
                 "bin=%016llX/%016llX tile=%u..%u winOff=%d,%d broaden=%u\n",
                 reports, static_cast<unsigned long long>(currentFrame),
                 static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash),
                 draw.indexed ? 1u : 0u, draw.primType,
                 static_cast<unsigned long long>(draw.binMask),
                 static_cast<unsigned long long>(draw.binSelect),
                 tileX0, tileX1, tileWindowX, tileWindowY,
                 stitchedUnpredicatedTile ? 1u : 0u);
        }
    }
    if (sceneTilingActive && !firstSceneTile)
    {
        g_collapsedTileDraws.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Each EDRAM tile replay is followed by a tiny six-draw transition group
    // using the fullscreen screen-scissor rather than the 448x720 tile window.
    // On the console this advances/clears the physical tile before the next
    // logical window is rasterized. With the full-size host render target that
    // work must happen only for the first tile; replaying the 0x0c and 0x30
    // groups modifies the already complete scene and creates a visible seam at
    // the 416/448 overlap.
    const uint64_t lowTileSelect = draw.binSelect & 0x3Full;
    static const bool traceTileTransitions = [] {
        const char* value = std::getenv("MOJORECOMP_TILE_TRANSITION_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    if (traceTileTransitions && draw.predicated &&
        draw.primType == xenos::kRectangleList &&
        (lowTileSelect == 0x0Cull || lowTileSelect == 0x30ull))
    {
        static uint32_t reports = 0;
        if (reports++ < 160)
        {
            KLOG("[tile transition trace] frame=%llu indexed=%u bin=%016llX/%016llX "
                 "win=%u,%u-%u,%u off=%d,%d sceneTile=%u first=%u middle=%u last=%u\n",
                 static_cast<unsigned long long>(currentFrame),
                 draw.indexed ? 1u : 0u,
                 static_cast<unsigned long long>(draw.binMask),
                 static_cast<unsigned long long>(draw.binSelect),
                 tileX0, tileY0, tileX1, tileY1, tileWindowX, tileWindowY,
                 sceneTileWindow ? 1u : 0u, firstSceneTile ? 1u : 0u,
                 middleSceneTile ? 1u : 0u, lastSceneTile ? 1u : 0u);
        }
    }
    const bool redundantTileTransition = CollapseTiledDrawsEnabled() && draw.predicated &&
                                         draw.primType == xenos::kRectangleList &&
                                         (lowTileSelect == 0x0Cull || lowTileSelect == 0x30ull) &&
                                         g_sceneTilingLastIndexedFrame == currentFrame;
    const bool stitchedTransitionReplay = sceneStitchActive && !sceneTileWindow && draw.predicated &&
                                          draw.primType == xenos::kRectangleList &&
                                          (lowTileSelect == 0x0Cull || lowTileSelect == 0x30ull) &&
                                          g_sceneStitchLastIndexedFrame == currentFrame &&
                                          StitchSkipTransitionReplayEnabled();
    if (redundantTileTransition || stitchedTransitionReplay)
    {
        if (stitchedTransitionReplay)
        {
            static uint32_t stitchReports = 0;
            if (stitchReports++ < 24)
                KLOG("[stitch transition] skipped frame=%llu VS=%016llX PS=%016llX "
                     "prim=%u indexed=%u bin=%016llX/%016llX win=%08X..%08X off=%08X\n",
                     static_cast<unsigned long long>(currentFrame),
                     static_cast<unsigned long long>(vsHash),
                     static_cast<unsigned long long>(psHash), draw.primType,
                     draw.indexed ? 1u : 0u,
                     static_cast<unsigned long long>(draw.binMask),
                     static_cast<unsigned long long>(draw.binSelect),
                     windowTl, windowBr, regs[xenos::kPaScWindowOffset]);
        }
        g_collapsedTileDraws.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Parser-side collapse removes raster setup/draw packets from tiles 1/2,
    // including packets that are not individually predicated. Once a real
    // indexed first-tile draw has armed the collapsed scene for this frame, the
    // corresponding unpredicated raster work in tile 0 must also be broadened
    // to logical 1280x720 coverage. Otherwise that pass affects only 0..448
    // while its tile 1/2 replays are skipped, producing a vertical brightness
    // discontinuity at the physical Xenos tile boundary.
    const bool collapsedFirstTile = mojorecomp::gpu::ShouldUseLogicalFirstTile(
        CollapseTiledDrawsEnabled(), firstSceneTile, currentFrame,
        g_sceneTilingLastIndexedFrame);
    if (CollapseTiledDrawsEnabled() && sceneTileWindow && draw.indexed)
    {
        static uint32_t collapseDecisionReports = 0;
        if (collapseDecisionReports++ < 96)
            KLOG_DIAG("[collapse decision] frame=%llu idx=%u pred=%u first=%u middle=%u last=%u "
                 "lastIndexed=%llu active=%u collapsedFirst=%u win=%u..%u off=%d,%d\n",
                 static_cast<unsigned long long>(currentFrame), draw.indexed ? 1u : 0u,
                 draw.predicated ? 1u : 0u, firstSceneTile ? 1u : 0u,
                 middleSceneTile ? 1u : 0u, lastSceneTile ? 1u : 0u,
                 static_cast<unsigned long long>(g_sceneTilingLastIndexedFrame),
                 sceneTilingActive ? 1u : 0u, collapsedFirstTile ? 1u : 0u,
                 tileX0, tileX1, tileWindowX, tileWindowY);
    }
    const bool physicalStitchedSceneTile =
        stitchedSceneTile && StitchPhysicalTileRasterEnabled();
    const bool logicalTileCoordinates =
        collapsedFirstTile || (stitchedSceneTile && !physicalStitchedSceneTile);
    const bool physicalTileReplay = sceneTileWindow && !logicalTileCoordinates;

    ShaderModuleRec* vs = GetModule(0, vsHash);
    ShaderModuleRec* ps = GetModule(1, psHash);
    if (!vs || !ps)
    {
        g_skippedShader.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (DiagnosticSkipShaderPair(vsHash, psHash))
        return true;

    const bool logical3DSceneSeen = g_sceneTilingLastIndexedFrame == currentFrame;
    const bool depthBacked3DSceneSeen = g_sceneTilingLastDepthFrame == currentFrame;
    const bool safeAreaOverlayDraw = logical3DSceneSeen && depthBacked3DSceneSeen &&
                                     !logicalTileCoordinates &&
                                     IsSafeAreaOverlayDraw(guestBase, draw, regs, *vs, mode);

    for (const auto& a : vs->attributes)
        if (a.indirect)
        {
            g_skippedVertex.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

    const uint64_t diagnosticFrame = g_frames.load(std::memory_order_relaxed) + 1;
    const bool traceDrawFrame = TraceDrawFrame(diagnosticFrame);
    if (traceDrawFrame)
    {
        static uint64_t lastFrame = 0;
        static uint32_t drawInFrame = 0;
        if (lastFrame != diagnosticFrame)
        {
            lastFrame = diagnosticFrame;
            drawInFrame = 0;
        }
        const uint32_t traceIndex = drawInFrame++;
        KLOG("[draw frame] frame=%llu draw=%u prim=%u count=%u indexed=%u pred=%u forced=%u "
             "packet=%08X source=%08X pos=%u depth=%u bin=%016llX/%016llX "
             "VS=%016llX PS=%016llX mode=%u color=%08X "
             "mask=%08X blend=%08X depth=%08X colorCtl=%08X alpha=%08X "
             "clip=%08X su=%08X surface=%08X depthInfo=%08X depthClear=%08X "
             "scissor=%08X..%08X winOff=%08X\n",
             static_cast<unsigned long long>(diagnosticFrame), traceIndex,
             draw.primType, draw.indexCount, draw.indexed ? 1u : 0u,
             draw.predicated ? 1u : 0u, draw.predicateForced ? 1u : 0u,
             draw.packetVa, draw.packetSourceVa, draw.packetPosition, draw.packetDepth,
             static_cast<unsigned long long>(draw.binMask),
             static_cast<unsigned long long>(draw.binSelect),
             static_cast<unsigned long long>(vsHash),
             static_cast<unsigned long long>(psHash),
             mode, regs[xenos::kRbColorInfo], regs[xenos::kRbColorMask],
             regs[xenos::kRbBlendControl0], regs[xenos::kRbDepthControl],
             regs[xenos::kRbColorControl], regs[xenos::kRbAlphaRef],
             regs[xenos::kPaClClipCntl],
             regs[xenos::kPaSuScModeCntl],
             regs[xenos::kRbSurfaceInfo], regs[xenos::kRbDepthInfo],
             regs[xenos::kRbDepthClear],
             regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
             regs[xenos::kPaScWindowOffset]);

        if (draw.indexed || psHash == 0xD7F22D636B662D38ull)
        {
            constexpr uint32_t kTraceVertexConstants[] = {4, 5, 6, 7, 8, 12, 13, 14, 15, 100};
            for (uint32_t constant : kTraceVertexConstants)
            {
                const uint32_t* value = regs + xenos::kAluConstantBase + constant * 4;
                KLOG("[draw vc] frame=%llu draw=%u vc%u=%08X,%08X,%08X,%08X "
                     "f=%g,%g,%g,%g\n",
                     static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                     constant, value[0], value[1], value[2], value[3],
                     double(F32(value[0])), double(F32(value[1])),
                     double(F32(value[2])), double(F32(value[3])));
            }
            KLOG("[draw vte] frame=%llu draw=%u vte=%08X XS=%g XO=%g YS=%g YO=%g "
                 "ZS=%g ZO=%g winOff=%08X\n",
                 static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                 regs[xenos::kPaClVteCntl],
                 double(F32(regs[xenos::kPaClVportXScale])),
                 double(F32(regs[xenos::kPaClVportXOffset])),
                 double(F32(regs[xenos::kPaClVportYScale])),
                 double(F32(regs[xenos::kPaClVportYOffset])),
                 double(F32(regs[xenos::kPaClVportZScale])),
                 double(F32(regs[xenos::kPaClVportZOffset])),
                 regs[xenos::kPaScWindowOffset]);

            constexpr uint32_t kTracePixelConstants[] = {
                2, 20, 29, 70, 71, 72, 112, 253, 254, 255
            };
            const uint32_t pixelBase = xenos::kAluConstantBase + 1024;
            for (uint32_t constant : kTracePixelConstants)
            {
                const uint32_t* value = regs + pixelBase + constant * 4;
                KLOG("[draw pc] frame=%llu draw=%u pc%u=%08X,%08X,%08X,%08X "
                     "f=%g,%g,%g,%g\n",
                     static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                     constant, value[0], value[1], value[2], value[3],
                     double(F32(value[0])), double(F32(value[1])),
                     double(F32(value[2])), double(F32(value[3])));
            }
        }

        const VertexAttribute* tracePosition = nullptr;
        for (const auto& a : vs->attributes)
        {
            KLOG("[draw attr] frame=%llu draw=%u loc=%d slot=%u fmt=%u signed=%u int=%u "
                 "stride=%u offset=%u indirect=%u\n",
                 static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                 a.location, a.fetchSlot, a.format, a.isSigned, a.isInteger,
                 a.strideDwords, a.offsetDwords, a.indirect);
            if (a.location == 0 && !a.indirect &&
                (a.format == 37 || a.format == 57 || a.format == 38) &&
                a.strideDwords && a.offsetDwords + 2 <= a.strideDwords)
                tracePosition = &a;
        }
        if (tracePosition)
        {
            const xenos::VertexFetch vf = xenos::DecodeVertexFetch(regs, tracePosition->fetchSlot);
            const uint32_t va = PhysicalToCached(vf.address);
            const uint64_t strideBytes = uint64_t(tracePosition->strideDwords) * 4u;
            const uint32_t indexBytes = draw.index32 ? 4u : 2u;
            const uint64_t rawIndexBytes = uint64_t(draw.indexCount) * indexBytes;
            const uint64_t indexReadBytes = (!draw.index32 && (draw.indexEndian & 3u) == 2u)
                                                ? ((rawIndexBytes + 3u) & ~3ull)
                                                : rawIndexBytes;
            const uint8_t* indexSrc = nullptr;
            bool valid = vf.address && vf.sizeDwords &&
                         GuestRangeOk(va, uint64_t(vf.sizeDwords) * 4u);
            if (valid && draw.indexed)
            {
                valid = draw.indexVa && GuestRangeOk(draw.indexVa, indexReadBytes);
                if (valid)
                    indexSrc = guestBase + draw.indexVa;
            }
            const int32_t indexOffset = int32_t(regs[xenos::kVgtIndxOffset] & 0xFFFFFFu);
            float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
            float minNdcX = FLT_MAX, minNdcY = FLT_MAX, minNdcZ = FLT_MAX;
            float maxNdcX = -FLT_MAX, maxNdcY = -FLT_MAX, maxNdcZ = -FLT_MAX;
            float minClipW = FLT_MAX, maxClipW = -FLT_MAX;
            uint32_t nonPositiveW = 0, nearZeroW = 0, outsideClip = 0;
            std::vector<float> traceClipW;
            traceClipW.reserve(draw.indexCount);
            const uint32_t* traceVc4 = regs + xenos::kAluConstantBase + 4u * 4u;
            const uint32_t* traceVc5 = regs + xenos::kAluConstantBase + 5u * 4u;
            const uint32_t* traceVc6 = regs + xenos::kAluConstantBase + 6u * 4u;
            const uint32_t* traceVc7 = regs + xenos::kAluConstantBase + 7u * 4u;
            uint32_t firstVertex = 0;
            for (uint32_t i = 0; valid && i < draw.indexCount; ++i)
            {
                const uint32_t vertex = draw.indexed
                    ? uint32_t(int64_t(ReadIndexValue(indexSrc, i, draw.index32,
                                                       draw.indexEndian)) + indexOffset)
                    : uint32_t(indexOffset + int32_t(i));
                if (i == 0)
                    firstVertex = vertex;
                if ((uint64_t(vertex) + 1u) * tracePosition->strideDwords > vf.sizeDwords)
                {
                    valid = false;
                    break;
                }
                uint32_t words[2]{};
                CopySwapped(reinterpret_cast<uint8_t*>(words),
                            guestBase + va + uint64_t(vertex) * strideBytes +
                                uint64_t(tracePosition->offsetDwords) * 4u,
                            sizeof(words), vf.endian);
                const float x = F32(words[0]);
                const float y = F32(words[1]);
                if (!std::isfinite(x) || !std::isfinite(y))
                {
                    valid = false;
                    break;
                }
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                uint32_t zWord = 0;
                CopySwapped(reinterpret_cast<uint8_t*>(&zWord),
                            guestBase + va + uint64_t(vertex) * strideBytes +
                                uint64_t(tracePosition->offsetDwords + 2u) * 4u,
                            sizeof(zWord), vf.endian);
                const float z = F32(zWord);
                if (!std::isfinite(z))
                {
                    valid = false;
                    break;
                }
                float clip[4]{};
                for (uint32_t c = 0; c < 4; ++c)
                    clip[c] = x * F32(traceVc4[c]) + y * F32(traceVc5[c]) +
                              z * F32(traceVc6[c]) + F32(traceVc7[c]);
                if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) ||
                    !std::isfinite(clip[2]) || !std::isfinite(clip[3]))
                {
                    valid = false;
                    break;
                }
                minClipW = std::min(minClipW, clip[3]);
                maxClipW = std::max(maxClipW, clip[3]);
                traceClipW.push_back(clip[3]);
                nonPositiveW += clip[3] <= 0.0f;
                if (std::fabs(clip[3]) < 1.0e-5f)
                {
                    ++nearZeroW;
                    continue;
                }
                const float ndcX = clip[0] / clip[3];
                const float ndcY = clip[1] / clip[3];
                const float ndcZ = clip[2] / clip[3];
                minNdcX = std::min(minNdcX, ndcX);
                minNdcY = std::min(minNdcY, ndcY);
                minNdcZ = std::min(minNdcZ, ndcZ);
                maxNdcX = std::max(maxNdcX, ndcX);
                maxNdcY = std::max(maxNdcY, ndcY);
                maxNdcZ = std::max(maxNdcZ, ndcZ);
                outsideClip += std::fabs(ndcX) > 1.0f || std::fabs(ndcY) > 1.0f ||
                               ndcZ < 0.0f || ndcZ > 1.0f;
            }
            if (valid)
            {
                uint64_t indexHash = 0;
                if (indexSrc)
                    indexHash = HashGuestTextureBytes(indexSrc,
                        static_cast<size_t>(indexReadBytes));
                KLOG("[draw bbox] frame=%llu draw=%u first=%u indxOff=%d vf=%08X/%u endian=%u "
                     "bbox=%.2f,%.2f..%.2f,%.2f clipW=%g..%g ndc=%g,%g,%g..%g,%g,%g "
                     "wLE0=%u wNear0=%u outside=%u indexHash=%016llX\n",
                     static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                     firstVertex, indexOffset, vf.address, vf.sizeDwords, vf.endian,
                     minX, minY, maxX, maxY, double(minClipW), double(maxClipW),
                     double(minNdcX), double(minNdcY), double(minNdcZ),
                     double(maxNdcX), double(maxNdcY), double(maxNdcZ),
                     nonPositiveW, nearZeroW, outsideClip,
                     static_cast<unsigned long long>(indexHash));

                if (draw.primType == xenos::kTriangleList && traceClipW.size() >= 3)
                {
                    uint32_t frontTriangles = 0;
                    uint32_t behindTriangles = 0;
                    uint32_t crossingTriangles = 0;
                    for (size_t i = 0; i + 2 < traceClipW.size(); i += 3)
                    {
                        const uint32_t positive = uint32_t(traceClipW[i] > 0.0f) +
                            uint32_t(traceClipW[i + 1] > 0.0f) +
                            uint32_t(traceClipW[i + 2] > 0.0f);
                        if (positive == 3)
                            ++frontTriangles;
                        else if (positive == 0)
                            ++behindTriangles;
                        else
                            ++crossingTriangles;
                    }
                    KLOG("[draw triangles] frame=%llu draw=%u front=%u behind=%u crossingW=%u\n",
                         static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                         frontTriangles, behindTriangles, crossingTriangles);
                }

                // The frontend text shader carries a packed RGBA8 attribute at
                // location 4. For one explicitly traced frame, report the first
                // few guest values before upload so UI colour can be attributed
                // to the title rather than to host conversion/blending.
                if (vsHash == 0xD589CC02813C1884ull)
                {
                    const VertexAttribute* colour = nullptr;
                    for (const auto& a : vs->attributes)
                        if (a.location == 4 && !a.indirect && a.fetchSlot == tracePosition->fetchSlot &&
                            a.format == 6 && a.strideDwords)
                        {
                            colour = &a;
                            break;
                        }
                    if (colour)
                    {
                        for (uint32_t i = 0; i < std::min<uint32_t>(draw.indexCount, 8u); ++i)
                        {
                            const uint32_t vertex = draw.indexed
                                ? uint32_t(int64_t(ReadIndexValue(indexSrc, i, draw.index32,
                                                                 draw.indexEndian)) + indexOffset)
                                : uint32_t(indexOffset + int32_t(i));
                            if ((uint64_t(vertex) + 1u) * colour->strideDwords > vf.sizeDwords)
                                break;
                            uint32_t packed = 0;
                            CopySwapped(reinterpret_cast<uint8_t*>(&packed),
                                        guestBase + va + uint64_t(vertex) * strideBytes +
                                            uint64_t(colour->offsetDwords) * 4u,
                                        sizeof(packed), vf.endian);
                            KLOG("[draw rgba8] frame=%llu draw=%u vertex=%u raw=%08X rgba=%u,%u,%u,%u\n",
                                 static_cast<unsigned long long>(diagnosticFrame), traceIndex, vertex,
                                 packed, packed & 0xFFu, (packed >> 8) & 0xFFu,
                                 (packed >> 16) & 0xFFu, (packed >> 24) & 0xFFu);
                        }
                    }
                }
            }
        }
        std::array<bool, kTextureSlots> reportedSlots{};
        for (const auto* shader : {vs, ps})
        {
            for (uint32_t slot : shader->textureSlots)
            {
                if (slot >= kTextureSlots || reportedSlots[slot])
                    continue;
                reportedSlots[slot] = true;
                const uint32_t* raw = regs + xenos::kFetchConstantBase + slot * 6;
                const auto fetch = mojorecomp::texture_abi::Decode(raw);
                KLOG("[draw tex] frame=%llu draw=%u slot=%u key=%08X type=%u fmt=%u "
                     "size=%ux%u pitch=%u tiled=%u endian=%u swz=%03X filter=%u/%u "
                     "clamp=%u/%u mips=%u..%u\n",
                     static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                     slot, fetch.key, fetch.type, fetch.format, fetch.width, fetch.height,
                     fetch.pitch, fetch.tiled ? 1u : 0u, fetch.endian, fetch.swizzle,
                     fetch.minFilter, fetch.magFilter, fetch.clampX, fetch.clampY,
                     fetch.mipMin, fetch.mipMax);

                // Opt-in one-frame dump of linear R8 masks. Crash's Episode
                // selection dolls are composed from an R8 coverage/image mask
                // plus a 256x1 RGBA lookup texture, so dumping the mask lets us
                // identify an invisible list item without altering rendering.
                const char* dumpDir = std::getenv("MOJORECOMP_DRAW_TEXTURE_DUMP_DIR");
                if (dumpDir && *dumpDir && fetch.format == 2 && fetch.type == 2 &&
                    fetch.dimension == 1 && !fetch.tiled && !fetch.mipMin && !fetch.mipMax &&
                    fetch.width && fetch.height && fetch.width <= 1024 && fetch.height <= 1024)
                {
                    static std::unordered_map<uint32_t, bool> dumpedR8Textures;
                    const uint32_t pitch = fetch.pitch ? fetch.pitch : fetch.width;
                    const uint32_t address = PhysicalToCached(fetch.key);
                    const uint64_t sourceBytes = uint64_t(pitch) * fetch.height;
                    if (!dumpedR8Textures[fetch.key] && pitch >= fetch.width &&
                        GuestRangeOk(address, sourceBytes))
                    {
                        dumpedR8Textures[fetch.key] = true;
                        std::error_code ec;
                        std::filesystem::create_directories(dumpDir, ec);
                        char name[96]{};
                        std::snprintf(name, sizeof(name),
                                      "drawr8-f%llu-d%u-%08X-%ux%u.pgm",
                                      static_cast<unsigned long long>(diagnosticFrame),
                                      traceIndex, fetch.key, fetch.width, fetch.height);
                        const std::filesystem::path path =
                            std::filesystem::path(dumpDir) / name;
                        std::ofstream image(path, std::ios::binary);
                        if (image)
                        {
                            image << "P5\n" << fetch.width << " " << fetch.height << "\n255\n";
                            const uint8_t* source = guestBase + address;
                            for (uint32_t y = 0; y < fetch.height; ++y)
                                image.write(reinterpret_cast<const char*>(source + uint64_t(y) * pitch),
                                            fetch.width);
                            KLOG("[draw r8 dump] frame=%llu draw=%u key=%08X path=%s\n",
                                 static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                                 fetch.key, path.string().c_str());
                        }
                    }
                }
                // Companion lookup texture used by the same frontend shader.
                // Dump the decoded 256-entry RGBA palette so an R8 mask can be
                // reconstructed offline exactly as the guest intended it.
                if (dumpDir && *dumpDir && fetch.format == 6 && fetch.type == 2 &&
                    fetch.dimension == 1 && !fetch.tiled && !fetch.mipMin && !fetch.mipMax &&
                    fetch.width == 256 && fetch.height == 1)
                {
                    static std::unordered_map<uint32_t, bool> dumpedRgbaLuts;
                    const uint32_t pitch = fetch.pitch ? fetch.pitch : fetch.width;
                    const uint32_t address = PhysicalToCached(fetch.key);
                    const uint64_t sourceBytes = uint64_t(pitch) * 4u;
                    if (!dumpedRgbaLuts[fetch.key] && pitch >= fetch.width &&
                        GuestRangeOk(address, sourceBytes))
                    {
                        dumpedRgbaLuts[fetch.key] = true;
                        std::error_code ec;
                        std::filesystem::create_directories(dumpDir, ec);
                        char name[96]{};
                        std::snprintf(name, sizeof(name),
                                      "drawlut-f%llu-d%u-%08X.rgba",
                                      static_cast<unsigned long long>(diagnosticFrame),
                                      traceIndex, fetch.key);
                        const std::filesystem::path path =
                            std::filesystem::path(dumpDir) / name;
                        std::ofstream out(path, std::ios::binary);
                        if (out)
                        {
                            const uint8_t* source = guestBase + address;
                            for (uint32_t x = 0; x < 256; ++x)
                            {
                                uint8_t pixel[4]{};
                                CopySwapped(pixel, source + uint64_t(x) * 4u,
                                            sizeof(pixel), fetch.endian);
                                // Match the image-view BGRA component swizzle,
                                // writing the value seen by the pixel shader.
                                const uint8_t rgba[4] = {
                                    pixel[2], pixel[1], pixel[0], pixel[3]};
                                out.write(reinterpret_cast<const char*>(rgba), sizeof(rgba));
                            }
                            KLOG("[draw lut dump] frame=%llu draw=%u key=%08X path=%s\n",
                                 static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                                 fetch.key, path.string().c_str());
                        }
                    }
                }
                // One-frame, opt-in texture atlas input for correlating visibly
                // broken Episode 1 geometry with the exact guest resource. Keep
                // this behind the existing draw-frame trace so normal gameplay
                // never hashes or writes asset textures.
                if (vsHash == 0xD589CC02813C1884ull &&
                    psHash == 0x80428150BC1ED3ABull && fetch.format == 6 &&
                    fetch.type == 2 && fetch.dimension == 1 && !fetch.tiled &&
                    fetch.width && fetch.height && fetch.width <= 1024 && fetch.height <= 1024)
                {
                    static std::unordered_map<uint32_t, bool> dumpedDrawTextures;
                    const uint32_t pitch = fetch.pitch ? fetch.pitch : fetch.width;
                    const uint32_t address = PhysicalToCached(fetch.key);
                    const uint64_t sourceBytes = uint64_t(pitch) * fetch.height * 4u;
                    if (dumpDir && *dumpDir && !dumpedDrawTextures[fetch.key] &&
                        pitch >= fetch.width && GuestRangeOk(address, sourceBytes))
                    {
                        dumpedDrawTextures[fetch.key] = true;
                        std::error_code ec;
                        std::filesystem::create_directories(dumpDir, ec);
                        char name[96]{};
                        std::snprintf(name, sizeof(name),
                                      "drawtex-%08X-%ux%u.ppm", fetch.key,
                                      fetch.width, fetch.height);
                        const std::filesystem::path path =
                            std::filesystem::path(dumpDir) / name;
                        std::ofstream image(path, std::ios::binary);
                        if (image)
                        {
                            image << "P6\n" << fetch.width << " " << fetch.height << "\n255\n";
                            const uint8_t* source = guestBase + address;
                            for (uint32_t y = 0; y < fetch.height; ++y)
                            {
                                const uint8_t* row = source + uint64_t(y) * pitch * 4u;
                                for (uint32_t x = 0; x < fetch.width; ++x)
                                {
                                    uint8_t pixel[4]{};
                                    CopySwapped(pixel, row + uint64_t(x) * 4u,
                                                sizeof(pixel), fetch.endian);
                                    const uint8_t rgb[3] = {pixel[2], pixel[1], pixel[0]};
                                    image.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
                                }
                            }
                            KLOG("[draw tex dump] frame=%llu draw=%u key=%08X path=%s\n",
                                 static_cast<unsigned long long>(diagnosticFrame), traceIndex,
                                 fetch.key, path.string().c_str());
                        }
                    }
                }
            }
        }
    }

    ReportLowerUiCandidate(guestBase, draw, regs, *vs, *ps);

    const uint32_t fullColorMask = regs[xenos::kRbColorMask];
    // Normalize RB_COLOR_MASK by the targets actually exported by the guest PS,
    // like Xenia does. A masked-but-unwritten RT must not take physical EDRAM
    // ownership merely because stale register bits still enable its channels.
    uint32_t colorMask =
        (mode == 4 && (ps->colorOutputMask & 0x1u)) ? (fullColorMask & 0xFu) : 0u;
    const uint32_t depthControl =
        NormalizeDepthControl(mode, regs[xenos::kRbDepthControl]);
    const bool usesDepth = (depthControl & 0x7u) != 0;
    const bool writesRt1 = mode == 4 && (ps->colorOutputMask & 0x2u) != 0 &&
                           (fullColorMask & 0xF0u) != 0;
    if (writesRt1)
        colorMask |= fullColorMask & 0xF0u;
    if (mode == 4 && (ps->colorOutputMask & 0xCu))
    {
        static uint32_t higherMrtReports = 0;
        if (higherMrtReports++ < 8)
            KLOG("Vulkan MRT2/3 export not yet backed: PS=%016llX targets=%X mask=%08X\n",
                 static_cast<unsigned long long>(psHash), ps->colorOutputMask, fullColorMask);
    }

    const auto attachmentStart = PerfClock::now();
    if (!BeginFrame())
        return false;
    // Xenos changes color/depth views of the same physical EDRAM together.
    // The switch helpers already end the current Vulkan rendering scope when a
    // binding actually changes, and resume=false keeps it closed until both
    // color and depth have been updated. Avoid ending/restarting dynamic
    // rendering for every draw when the bindings are unchanged; cutscenes can
    // issue hundreds of draws per frame, making that redundant scope churn a
    // significant CPU cost.
    if (!SwitchActiveColorSurfaces(regs[xenos::kRbSurfaceInfo], regs[xenos::kRbColorInfo],
                                   regs[xenos::kRbColor1Info], writesRt1, false))
        return false;
    if (!SwitchActiveDepthSurface(regs[xenos::kRbSurfaceInfo],
                                  regs[xenos::kRbDepthInfo],
                                  usesDepth, false))
        return false;

    // Host VkImages are only representations of the shared 10 MiB Xenos
    // EDRAM. Make the representation that will actually be written by this
    // draw own its physical tile range before rasterization. Depth is claimed
    // first and color afterwards, matching the effective precedence used by
    // the host-render-target path when both are active. In DepthOnly mode the
    // color target remains bound but never takes ownership.
    const bool ownershipEnabled = EdramOwnershipEnabled(
        g_frames.load(std::memory_order_relaxed) + 1);
    if (ownershipEnabled && usesDepth && g_activeDepthEnabled &&
        g_activeDepthSurfaceKey != UINT64_MAX)
    {
        if (!ClaimEdramOwnership({EdramOwnerKind::Depth, g_activeDepthSurfaceKey}))
            return false;
    }
    if (ownershipEnabled && mode == 4 && (colorMask & 0xFu) != 0 &&
        g_activeColorSurfaceKey != UINT64_MAX)
    {
        if (!ClaimEdramOwnership({EdramOwnerKind::Color, g_activeColorSurfaceKey}))
            return false;
    }
    if (ownershipEnabled && mode == 4 && writesRt1 &&
        ((colorMask >> 4) & 0xFu) != 0 &&
        g_activeColor1Enabled && g_activeColor1SurfaceKey != UINT64_MAX)
    {
        if (!ClaimEdramOwnership({EdramOwnerKind::Color, g_activeColor1SurfaceKey}))
            return false;
    }
    RestoreActiveEdramAttachmentLayouts();
    ResumeColorRendering(false);
    g_perfAttachmentNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - attachmentStart).count());

    // Diagnostic for a known renderer simplification: all guest depth surfaces
    // currently share one Vulkan depth image even when RB_DEPTH_INFO / the
    // surface pitch or MSAA mode changes.  The real Xenos addresses distinct
    // EDRAM regions in those cases.  Clearing only when the guest switches to a
    // different depth surface tells us whether stale values from an unrelated
    // guest depth target are contaminating the next pass, without making this a
    // permanent emulation strategy.
    if (DiagnosticClearOnDepthSurfaceSwitchEnabled() &&
        (regs[xenos::kRbDepthControl] & 0x7u))
    {
        const uint64_t depthSurfaceKey =
            (uint64_t(regs[xenos::kRbSurfaceInfo]) << 32) | regs[xenos::kRbDepthInfo];
        if (depthSurfaceKey != g_activeDepthSurfaceKey)
        {
            if (g_activeDepthSurfaceKey != UINT64_MAX)
            {
                const uint32_t d = regs[xenos::kRbDepthClear];
                VkClearAttachment attachment{};
                attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                attachment.clearValue.depthStencil.depth =
                    DecodeDepthClearValue(regs[xenos::kRbDepthInfo], d);
                attachment.clearValue.depthStencil.stencil = d & 0xFFu;
                VkClearRect rect{};
                rect.rect = {{0, 0}, g_internalExtent};
                rect.baseArrayLayer = 0;
                rect.layerCount = 1;
                p_vkCmdClearAttachments(g_commandBuffer, 1, &attachment, 1, &rect);
                static uint32_t reports = 0;
                if (reports++ < 32)
                    KLOG("[depth surface switch clear] old=%016llX new=%016llX clear=%08X\n",
                         static_cast<unsigned long long>(g_activeDepthSurfaceKey),
                         static_cast<unsigned long long>(depthSurfaceKey), d);
            }
            g_activeDepthSurfaceKey = depthSurfaceKey;
        }
    }

    // The guest clears/reuses the physical Xenos depth tile between the three
    // horizontal scene replays.  Once those replays are collapsed into one
    // full-width host raster pass, skipping the later physical-tile work also
    // skips the depth initialization for their logical coverage.  The result is
    // deterministic: only the first ~448 pixels pass depth and the rest of the
    // scene is black.  Recreate the equivalent logical operation once, before
    // the first collapsed indexed draw of the frame, using the guest's own
    // RB_DEPTH_CLEAR value (important for reverse-Z).
    const bool automaticCollapsedDepthClear =
        CollapseTiledDrawsEnabled() && sceneTilingActive && collapsedFirstTile &&
        draw.indexed && usesDepth && regs[xenos::kRbColorInfo] == 0;
    if (automaticCollapsedDepthClear)
    {
        static uint64_t clearFrame = 0;
        if (clearFrame != currentFrame)
        {
            clearFrame = currentFrame;
            const uint32_t d = regs[xenos::kRbDepthClear];
            VkClearAttachment attachment{};
            attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            attachment.clearValue.depthStencil.depth =
                DecodeDepthClearValue(regs[xenos::kRbDepthInfo], d);
            attachment.clearValue.depthStencil.stencil = d & 0xFFu;
            VkClearRect rect{};
            rect.rect = {{0, 0}, g_internalExtent};
            rect.baseArrayLayer = 0;
            rect.layerCount = 1;
            p_vkCmdClearAttachments(g_commandBuffer, 1, &attachment, 1, &rect);
            ++g_perfDepthClears;
            g_perfClearRectPixels += uint64_t(g_extent.width) * g_extent.height;
            ++g_renderWriteGeneration;
            static uint32_t reports = 0;
            if (reports++ < 24)
                KLOG_DIAG("[collapse depth clear] frame=%llu clear=%08X depth=%f rect=1280x720\n",
                     static_cast<unsigned long long>(currentFrame), d,
                     attachment.clearValue.depthStencil.depth);
        }
    }

    // Diagnostic A/B for additional stale EDRAM contents between Crash's three
    // horizontal tile replays.  Depth is already handled above for the collapsed
    // first tile; this switch remains useful for color/depth experiments on the
    // non-collapsed tile windows.  Mode: 1=color, 2=depth, 3=both.
    const int diagnosticTileClearMode = DiagnosticTileBoundaryClearMode();
    if (diagnosticTileClearMode && draw.indexed && draw.predicated &&
        sceneTileWindow && regs[xenos::kRbColorInfo] == 0)
    {
        const uint32_t tileBit = firstSceneTile ? 1u : middleSceneTile ? 2u : 4u;
        static uint64_t clearFrame = 0;
        static uint32_t clearedTiles = 0;
        if (clearFrame != currentFrame)
        {
            clearFrame = currentFrame;
            clearedTiles = 0;
        }
        if ((clearedTiles & tileBit) == 0)
        {
            clearedTiles |= tileBit;
            std::array<VkClearAttachment, 2> attachments{};
            uint32_t attachmentCount = 0;
            if (diagnosticTileClearMode & 1)
            {
                auto& attachment = attachments[attachmentCount++];
                attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                attachment.colorAttachment = 0;
                attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
            }
            if ((diagnosticTileClearMode & 2) && !automaticCollapsedDepthClear)
            {
                auto& attachment = attachments[attachmentCount++];
                attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                const uint32_t d = regs[xenos::kRbDepthClear];
                attachment.clearValue.depthStencil.depth =
                    DecodeDepthClearValue(regs[xenos::kRbDepthInfo], d);
                attachment.clearValue.depthStencil.stencil = d & 0xFFu;
            }
            const VkRect2D clearRect =
                (CollapseTiledDrawsEnabled() && firstSceneTile)
                    ? VkRect2D{{0, 0}, g_extent}
                    : DecodeScissor(
                          regs, StitchTiledDrawsEnabled() && sceneStitchActive &&
                                    !StitchPhysicalTileRasterEnabled());
            if (attachmentCount && clearRect.extent.width && clearRect.extent.height)
            {
                VkClearRect rect{};
                rect.rect = ScaleRectToInternal(clearRect);
                rect.baseArrayLayer = 0;
                rect.layerCount = 1;
                p_vkCmdClearAttachments(g_commandBuffer, attachmentCount,
                                        attachments.data(), 1, &rect);
                ++g_renderWriteGeneration;
                static uint32_t reports = 0;
                if (reports++ < 24)
                    KLOG("[tile boundary clear] frame=%llu tile=%u mode=%d rect=%d,%d %ux%u\n",
                         static_cast<unsigned long long>(currentFrame), tileBit,
                         diagnosticTileClearMode, clearRect.offset.x, clearRect.offset.y,
                         clearRect.extent.width, clearRect.extent.height);
            }
        }
    }
    const auto textureStart = PerfClock::now();
    std::array<float, kTextureSlots> textureSampleScales{};
    const bool executeGuestPixelShader = mode != 5;
    textureSampleScales.fill(1.0f);
    const bool needsTextureDescriptors = vs->usesTextures || executeGuestPixelShader;
    const TextureBundle* textures = needsTextureDescriptors
        ? PrepareTextures(guestBase, regs, *vs, *ps, textureSampleScales,
                          executeGuestPixelShader)
        : nullptr;
    g_perfTextureNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - textureStart).count());
    if (needsTextureDescriptors && !textures)
    {
        const uint64_t skipped = g_skippedTexture.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipped <= 8 || traceDrawFrame)
        {
            const uint32_t slot = g_texturePrepareFailureSlot;
            uint32_t declaredDimension = UINT32_MAX;
            const char* shaderStage = "none";
            for (const auto* shader : {vs, executeGuestPixelShader ? ps : nullptr})
            {
                if (!shader)
                    continue;
                for (size_t i = 0; i < shader->textureSlots.size() &&
                                   i < shader->textureDimensions.size(); ++i)
                    if (shader->textureSlots[i] == slot)
                    {
                        declaredDimension = shader->textureDimensions[i];
                        shaderStage = shader == vs ? "VS" : "PS";
                        break;
                    }
                if (declaredDimension != UINT32_MAX)
                    break;
            }
            if (slot < kTextureSlots)
            {
                const uint32_t* raw = regs + xenos::kFetchConstantBase + slot * 6;
                const auto fetch = mojorecomp::texture_abi::Decode(raw);
                KLOG("[texture gate] frame=%llu reason=%s stage=%s slot=%u "
                     "shaderDim=%u fetchDim=%u key=%08X type=%u fmt=%u size=%ux%u "
                     "tiled=%u mips=%u..%u VS=%016llX PS=%016llX "
                     "raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
                     static_cast<unsigned long long>(diagnosticFrame),
                     g_texturePrepareFailure, shaderStage, slot, declaredDimension,
                     fetch.dimension, fetch.key, fetch.type, fetch.format,
                     fetch.width, fetch.height, fetch.tiled ? 1u : 0u,
                     fetch.mipMin, fetch.mipMax,
                     static_cast<unsigned long long>(vsHash),
                     static_cast<unsigned long long>(psHash),
                     raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
            }
            else
            {
                KLOG("[texture gate] frame=%llu reason=%s slot=none "
                     "VS=%016llX PS=%016llX\n",
                     static_cast<unsigned long long>(diagnosticFrame),
                     g_texturePrepareFailure,
                     static_cast<unsigned long long>(vsHash),
                     static_cast<unsigned long long>(psHash));
            }
        }
        return true;
    }
    if (fullColorMask & 0xFFF0u)
    {
        static uint32_t mrtReports = 0;
        if (mrtReports++ < 16)
            KLOG("Vulkan MRT state observed: mask=%08X color0=%08X color1=%08X VS=%016llX PS=%016llX\n",
                 fullColorMask, regs[xenos::kRbColorInfo], regs[xenos::kRbColor1Info],
                 static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash));
    }
    const uint32_t blendControl0 = regs[xenos::kRbBlendControl0];
    const uint32_t blendControl1 = regs[xenos::kRbBlendControl1];
    const VkSampleCountFlagBits rasterSamples = GuestSurfaceSamples(
        regs[xenos::kRbSurfaceInfo], usesDepth ? regs[xenos::kRbDepthInfo] : UINT32_MAX);
    const VkFormat pipelineDepthFormat = usesDepth
        ? HostDepthFormat(regs[xenos::kRbDepthInfo]) : kDepthUnormFormat;
    const bool guestDepthFloat24 =
        usesDepth && ((regs[xenos::kRbDepthInfo] >> 16) & 1u) != 0;
    const bool float24DepthShader =
        UseFloat24DepthShader(depthControl, guestDepthFloat24, rasterSamples);
    if (guestDepthFloat24 &&
        (depthControl & (1u << 1)) && rasterSamples != VK_SAMPLE_COUNT_1_BIT &&
        !g_sampleRateShading)
    {
        static uint32_t reports = 0;
        if (reports++ < 4)
            KLOG("Vulkan D24FS8 float24 PS conversion unavailable for %ux MSAA: "
                 "sampleRateShading unsupported; retaining host-precision path\n",
                 uint32_t(rasterSamples));
    }
    const bool shaderWritesDepth = executeGuestPixelShader
        ? (ps->writesDepth || float24DepthShader)
        : float24DepthShader;
    VkViewport logicalViewport =
        DecodeViewport(regs, logicalTileCoordinates, shaderWritesDepth, depthControl);
    if (safeAreaOverlayDraw)
        logicalViewport = ApplySafeAreaViewport(logicalViewport);
    const VkViewport viewport = ScaleViewportToInternal(logicalViewport);
    const uint32_t rasterState = regs[xenos::kPaSuScModeCntl] & 7u;
    const uint32_t colorControl = regs[xenos::kRbColorControl];
    uint32_t alphaTest = 0;
    if (executeGuestPixelShader && (colorControl & 0x8u))
    {
        const uint32_t func = colorControl & 0x7u;
        // Store func+1 so zero remains the pipeline-cache value for disabled /
        // ALWAYS. The specialization constant carries the exact Xenos compare.
        if (func != 7u)
            alphaTest = func + 1u;
    }
    if (alphaTest)
        g_alphaTestDraws.fetch_add(1, std::memory_order_relaxed);
    const bool primitiveRestart = PrimitiveRestartEnabled(draw, regs);
    const auto pipelineStart = PerfClock::now();
    const VkPipeline pipeline = GetPipeline(*vs, *ps, draw.primType, colorMask,
                                             blendControl0, blendControl1,
                                             depthControl, rasterState, alphaTest, mode,
                                             primitiveRestart, rasterSamples,
                                             pipelineDepthFormat, guestDepthFloat24);
    g_perfPipelineNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - pipelineStart).count());
    if (!pipeline)
    {
        const uint64_t n = g_skippedPipeline.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8)
            KLOG("Vulkan draw skipped: pipeline unsupported VS=%016llX PS=%016llX "
                 "prim=%u attrs=%zu mask=%X\n",
                 static_cast<unsigned long long>(vsHash),
                 static_cast<unsigned long long>(psHash), draw.primType,
                 vs->attributes.size(), colorMask);
        return true;
    }

    if (ps->hash == 0x89B4E4C8E904ABD5ull)
    {
        static uint32_t highPsConstReports = 0;
        if (highPsConstReports++ < 4)
        {
            const uint32_t loop30 = regs[xenos::kLoopConstantBase + 30];
            const uint32_t loop31 = regs[xenos::kLoopConstantBase + 31];
            KLOG("[front pass] loop30=%08X count=%u init=%u inc=%d "
                 "loop31=%08X count=%u init=%u inc=%d\n",
                 loop30, loop30 & 0xFFFu, (loop30 >> 12) & 0xFFFu,
                 int8_t(loop30 >> 24), loop31, loop31 & 0xFFFu,
                 (loop31 >> 12) & 0xFFFu, int8_t(loop31 >> 24));
            const uint32_t base = xenos::kAluConstantBase + 1024;
            for (uint32_t c = 0; c <= 1; ++c)
            {
                const uint32_t* v = regs + base + c * 4;
                KLOG("[front pass] pc%u=%08X,%08X,%08X,%08X f=%g,%g,%g,%g\n",
                     c, v[0], v[1], v[2], v[3],
                     double(F32(v[0])), double(F32(v[1])),
                     double(F32(v[2])), double(F32(v[3])));
            }
            for (uint32_t c = 251; c <= 255; ++c)
            {
                const uint32_t* v = regs + base + c * 4;
                KLOG("[front pass] pc%u=%08X,%08X,%08X,%08X f=%g,%g,%g,%g\n",
                     c, v[0], v[1], v[2], v[3],
                     double(F32(v[0])), double(F32(v[1])),
                     double(F32(v[2])), double(F32(v[3])));
            }
        }
    }

    if (physicalTileReplay)
        g_physicalTileContentFrame = currentFrame;
    const bool physicalTileContentSeen = g_physicalTileContentFrame == currentFrame;
    const bool physicalTileContentRecent = mojorecomp::gpu::HasRecentPhysicalTileContent(
        currentFrame, g_physicalTileContentFrame);
    const bool applyConfiguredAspect = mojorecomp::gpu::ShouldApplyConfiguredAspect(
        physicalTileReplay, physicalTileContentRecent, logical3DSceneSeen,
        logicalTileCoordinates);

    const auto constantsStart = PerfClock::now();
    const VkDeviceSize vsAt = UploadConstants(regs, xenos::kAluConstantBase,
                                               kVsConstBytes, vs->usesAlu);
    const bool projectionAspectApplied = ApplyWideProjectionToUploadedConstants(
        vsAt, *vs, logicalTileCoordinates, applyConfiguredAspect);
    // DepthOnly never executes the guest PS. Reuse a valid, already allocated
    // address for its unused push-constant pointer instead of burning 4 KiB per
    // pre-pass draw in the frame upload arena.
    const VkDeviceSize psAt = executeGuestPixelShader
        ? UploadConstants(regs, xenos::kAluConstantBase + 1024,
                          kPsConstBytes, ps->usesAlu)
        : vsAt;
    const VkDeviceSize sharedAt = UploadShared(regs, textureSampleScales,
                                               logicalTileCoordinates, usesDepth,
                                               applyConfiguredAspect && !projectionAspectApplied);
    g_perfConstantsNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - constantsStart).count());
    if (vsAt == VK_WHOLE_SIZE || psAt == VK_WHOLE_SIZE || sharedAt == VK_WHOLE_SIZE)
    {
        static bool reported = false;
        if (!reported)
        {
            KLOG("Vulkan upload arena exhausted in one frame: used=%llu MiB limit=%llu MiB "
                 "frame=%llu slot=%u; later draws skipped\n",
                 static_cast<unsigned long long>((g_uploadAt -
                     VkDeviceSize(g_frameSlot) * kFrameUploadBytes) / (1024ull * 1024ull)),
                 static_cast<unsigned long long>((g_uploadLimit -
                     VkDeviceSize(g_frameSlot) * kFrameUploadBytes) / (1024ull * 1024ull)),
                 static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed) + 1),
                 g_frameSlot);
            reported = true;
        }
        return true;
    }

    PreparedDraw prepared{};
    const auto vertexStart = PerfClock::now();
    if (!PrepareVertexBindings(guestBase, draw, regs, *vs, prepared))
    {
        if (draw.indexed)
            g_skippedIndexed.fetch_add(1, std::memory_order_relaxed);
        else
            g_skippedVertex.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_perfVertexNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - vertexStart).count());
    ++g_perfDrawCalls;

    CmdBindGraphicsPipelineCached(g_commandBuffer, pipeline);
    if (textures)
        CmdBindDescriptorSetsCached(g_commandBuffer, g_pipelineLayout,
                                    0, 4, textures->sets);
    CmdSetViewportCached(g_commandBuffer, viewport);
    VkRect2D scissor = collapsedFirstTile
        ? VkRect2D{{0, 0}, g_extent}
        : DecodeScissor(regs, stitchedSceneTile && !physicalStitchedSceneTile);
    if (!scissor.extent.width || !scissor.extent.height)
        return true;
    AccumulatePerfShaderPair(vsHash, psHash, draw.indexed, prepared.count,
                             uint64_t(scissor.extent.width) * scissor.extent.height);
    uint32_t perfStencilFront = 0;
    uint32_t perfStencilBack = 0;
    if (depthControl & 1u)
    {
        perfStencilFront = regs[xenos::kRbStencilRefMask];
        const bool perfPolygonal = draw.primType == xenos::kTriangleList ||
                                   draw.primType == xenos::kTriangleFan ||
                                   draw.primType == xenos::kTriangleStrip;
        const bool perfSeparateBack = mojorecomp::gpu::UsesSeparateBackfaceStencil(
            perfPolygonal, depthControl);
        perfStencilBack = perfSeparateBack
            ? regs[xenos::kRbStencilRefMaskBack]
            : perfStencilFront;
    }
    AccumulatePerfDepthState(depthControl, perfStencilFront, perfStencilBack,
                             draw.indexed, prepared.count,
                             uint64_t(scissor.extent.width) * scissor.extent.height);
    if (vsHash == 0x6D73B356D2B5E61Bull)
    {
        static std::vector<uint64_t> binkScissors;
        const uint64_t scissorKey = uint64_t(uint32_t(scissor.offset.x) & 0xFFFFu) |
            (uint64_t(uint32_t(scissor.offset.y) & 0xFFFFu) << 16) |
            (uint64_t(scissor.extent.width & 0xFFFFu) << 32) |
            (uint64_t(scissor.extent.height & 0xFFFFu) << 48) ^
            (uint64_t(regs[xenos::kRbColorInfo]) << 1);
        if (std::find(binkScissors.begin(), binkScissors.end(), scissorKey) == binkScissors.end())
        {
            binkScissors.push_back(scissorKey);
            KLOG("[bink tile] unique=%zu scissor=%d,%d,%u,%u winTL=%08X winBR=%08X winOff=%08X color0=%08X surface=%08X copyDest=%08X\n",
                 binkScissors.size(), scissor.offset.x, scissor.offset.y,
                 scissor.extent.width, scissor.extent.height,
                 regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
                 regs[xenos::kPaScWindowOffset], regs[xenos::kRbColorInfo],
                 regs[xenos::kRbSurfaceInfo], regs[xenos::kRbCopyDestBase]);
        }
    }
    if (vsHash == 0x6D73B356D2B5E61Bull)
    {
        static uint32_t binkVteReports = 0;
        if (binkVteReports++ < 12)
        {
            float scale[2], offset[2];
            std::memcpy(scale, g_uploadMapped + sharedAt + kSharedPosScale, sizeof(scale));
            std::memcpy(offset, g_uploadMapped + sharedAt + kSharedPosOffset, sizeof(offset));
            KLOG("[bink VTE] vte=%08X XS=%g XO=%g YS=%g YO=%g ZS=%g ZO=%g "
                 "sharedScale=%g,%g sharedOffset=%g,%g viewport=%g,%g,%g,%g "
                 "scissor=%d,%d,%u,%u winTL=%08X winBR=%08X screenTL=%08X screenBR=%08X winOff=%08X\n",
                 regs[xenos::kPaClVteCntl], F32(regs[xenos::kPaClVportXScale]),
                 F32(regs[xenos::kPaClVportXOffset]), F32(regs[xenos::kPaClVportYScale]),
                 F32(regs[xenos::kPaClVportYOffset]), F32(regs[xenos::kPaClVportZScale]),
                 F32(regs[xenos::kPaClVportZOffset]), scale[0], scale[1], offset[0], offset[1],
                 viewport.x, viewport.y, viewport.width, viewport.height,
                 scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height,
                 regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScWindowScissorBr],
                 regs[xenos::kPaScScreenScissorTl], regs[xenos::kPaScScreenScissorBr],
                 regs[xenos::kPaScWindowOffset]);
        }
    }
    if (g_frames.load(std::memory_order_relaxed) + 1 == 113)
    {
        const uint32_t windowOffset = regs[xenos::kPaScWindowOffset];
        if (windowOffset != 0)
        {
            const int32_t windowX = static_cast<int16_t>(windowOffset & 0xFFFFu);
            const int32_t windowY = static_cast<int16_t>(windowOffset >> 16);
            KLOG("[frame113 coords] draw=%llu prim=%u mode=%08X vte=%08X winOff=%d,%d winTL=%08X winBR=%08X viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u\n",
                 static_cast<unsigned long long>(g_draws.load(std::memory_order_relaxed)),
                 draw.primType, regs[xenos::kPaSuScModeCntl], regs[xenos::kPaClVteCntl],
                 windowX, windowY, regs[xenos::kPaScWindowScissorTl],
                 regs[xenos::kPaScWindowScissorBr], viewport.x, viewport.y,
                 viewport.width, viewport.height, scissor.offset.x, scissor.offset.y,
                 scissor.extent.width, scissor.extent.height);
        }
    }
    if (CoordinateDiagnosticsEnabled())
    {
        static uint32_t reportsByPrimitive[64]{};
        uint32_t& reports = reportsByPrimitive[draw.primType & 63u];
        if (reports < 8)
        {
            ++reports;
            float scale[2], offset[2], halfPixel[2];
            std::memcpy(scale, g_uploadMapped + sharedAt + kSharedPosScale, sizeof(scale));
            std::memcpy(offset, g_uploadMapped + sharedAt + kSharedPosOffset, sizeof(offset));
            std::memcpy(halfPixel, g_uploadMapped + sharedAt + kSharedHalfPixelOffset, sizeof(halfPixel));
            KLOG("[VTE probe] draw #%u VS=%016llX prim=%u vte=%08X "
                 "XS=%g XO=%g YS=%g YO=%g ZS=%g ZO=%g "
                 "scale=%g,%g offset=%g,%g half=%g,%g viewport=%g,%g,%g,%g "
                 "scissor=%d,%d,%u,%u windowTL=%08X screenTL=%08X windowOffset=%08X\n",
                 reports, static_cast<unsigned long long>(vsHash), draw.primType, regs[xenos::kPaClVteCntl],
                 F32(regs[xenos::kPaClVportXScale]), F32(regs[xenos::kPaClVportXOffset]),
                 F32(regs[xenos::kPaClVportYScale]), F32(regs[xenos::kPaClVportYOffset]),
                 F32(regs[xenos::kPaClVportZScale]), F32(regs[xenos::kPaClVportZOffset]),
                 scale[0], scale[1], offset[0], offset[1], halfPixel[0], halfPixel[1],
                 viewport.x, viewport.y, viewport.width, viewport.height,
                 scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height,
                 regs[xenos::kPaScWindowScissorTl], regs[xenos::kPaScScreenScissorTl],
                 regs[xenos::kPaScWindowOffset]);
        }
    }
    CmdSetScissorCached(g_commandBuffer, ScaleRectToInternal(scissor));
    const float blendConstants[4] = {
        F32(regs[xenos::kRbBlendRed + 0]),
        F32(regs[xenos::kRbBlendRed + 1]),
        F32(regs[xenos::kRbBlendRed + 2]),
        F32(regs[xenos::kRbBlendRed + 3]),
    };
    p_vkCmdSetBlendConstants(g_commandBuffer, blendConstants);
    if (depthControl & 1u)
    {
        const uint32_t stencilFront = regs[xenos::kRbStencilRefMask];
        const bool polygonal = draw.primType == xenos::kTriangleList ||
                               draw.primType == xenos::kTriangleFan ||
                               draw.primType == xenos::kTriangleStrip;
        const bool separateBack = mojorecomp::gpu::UsesSeparateBackfaceStencil(
            polygonal, depthControl);
        const uint32_t stencilBack = separateBack
            ? regs[xenos::kRbStencilRefMaskBack]
            : stencilFront;
        auto setStencilFace = [&](VkStencilFaceFlags face, uint32_t stencil) {
            p_vkCmdSetStencilReference(g_commandBuffer, face, stencil & 0xFFu);
            p_vkCmdSetStencilCompareMask(g_commandBuffer, face, (stencil >> 8) & 0xFFu);
            p_vkCmdSetStencilWriteMask(g_commandBuffer, face, (stencil >> 16) & 0xFFu);
        };
        if (stencilFront == stencilBack)
            setStencilFace(VK_STENCIL_FACE_FRONT_AND_BACK, stencilFront);
        else
        {
            setStencilFace(VK_STENCIL_FACE_FRONT_BIT, stencilFront);
            setStencilFace(VK_STENCIL_FACE_BACK_BIT, stencilBack);
        }
    }
    struct PushConstants { uint64_t vs, ps, shared; } push = {
        uint64_t(g_uploadAddress + vsAt),
        uint64_t(g_uploadAddress + psAt),
        uint64_t(g_uploadAddress + sharedAt),
    };
    p_vkCmdPushConstants(g_commandBuffer, g_pipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(push), &push);
    if (DiagnosticSkipIndexHash(vsHash, psHash, draw, regs, guestBase) ||
        DiagnosticSkipTextureContentHash(*vs, *ps, regs))
        return true;
    uint32_t gpuTimingIndex = UINT32_MAX;
    if (g_gpuDrawTimestamps)
    {
        auto& frame = g_frameContexts[g_frameSlot];
        if (frame.timestampPool && frame.timestampDrawCount < kMaxGpuTimedDraws)
        {
            gpuTimingIndex = frame.timestampDrawCount++;
            auto& rec = g_gpuDrawTimingRecords[g_frameSlot][gpuTimingIndex];
            rec.vs = vsHash;
            rec.ps = psHash;
            rec.depthControl = depthControl;
            rec.binSelect = draw.binSelect;
            rec.textureKey0 = mojorecomp::texture_abi::Decode(
                regs + xenos::kFetchConstantBase).key;
            rec.bool128_131 = regs[xenos::kBoolConstantBase + 4] & 0xFu;
            rec.mode = mode;
            rec.elements = prepared.count;
            rec.scissorPixels = uint64_t(scissor.extent.width) * scissor.extent.height;
            rec.indexed = prepared.indexed;
            const uint32_t query = 2u + gpuTimingIndex * 2u;
            // Use BOTTOM_OF_PIPE on both sides so each interval measures the
            // incremental completion cost of this draw rather than inheriting
            // backlog from earlier graphics work that may already have entered
            // the pipeline when a TOP_OF_PIPE timestamp is recorded.
            p_vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  frame.timestampPool, query);
        }
    }
    ++g_renderWriteGeneration;
    if (prepared.indexed)
    {
        p_vkCmdDrawIndexed(g_commandBuffer, prepared.count, 1, 0, prepared.baseVertex, 0);
        g_indexedDraws.fetch_add(1, std::memory_order_relaxed);
    }
    else
        p_vkCmdDraw(g_commandBuffer, prepared.count, 1, 0, 0);
    if (gpuTimingIndex != UINT32_MAX)
    {
        auto& frame = g_frameContexts[g_frameSlot];
        const uint32_t query = 2u + gpuTimingIndex * 2u + 1u;
        p_vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              frame.timestampPool, query);
    }

    // Diagnostic ownership A/B. In DepthOnly mode a Z-writing draw owns the
    // physical EDRAM range addressed by RB_DEPTH_INFO. The fast Vulkan path has
    // separate color/depth images, so an aliased color backing would otherwise
    // retain data that the Xenos depth write has physically overwritten. Clear
    // the aliased color backing after every actual depth write (not merely when
    // the depth binding changes) to test that stale-ownership failure mode.
    // This is intentionally opt-in; a real implementation must transfer / reinterpret
    // the D24S8 bits rather than replace them with zero.
    if (!g_hostMsaa2x && mode == 5 && (depthControl & 0x6u) == 0x6u &&
        DiagnosticInvalidateColorAfterDepthWriteEnabled())
    {
        const uint32_t depthBase = regs[xenos::kRbDepthInfo] & 0x7FFu;
        bool clearedAny = false;
        EndColorRendering();
        for (auto& color : g_colorBackings)
        {
            if (!color.initialized || (color.info & 0x7FFu) != depthBase)
                continue;
            VkClearColorValue clear{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            TransitionColorBacking(color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            p_vkCmdClearColorImage(g_commandBuffer, color.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &clear, 1, &range);
            if (color.info == g_activeColorInfo ||
                (g_activeColor1Enabled && color.info == g_activeColor1Info))
                TransitionColorBacking(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            clearedAny = true;
            static uint32_t reports = 0;
            if (reports++ < 48)
                KLOG("[edram ownership A/B] frame=%llu DepthOnly write base=%03X invalidated color=%08X depthCtl=%08X\n",
                     static_cast<unsigned long long>(currentFrame), depthBase,
                     color.info, depthControl);
        }
        if (clearedAny)
            ResumeColorRendering(false);
        else
            ResumeColorRendering(false);
    }
    if (mode == 4 && colorMask != 0)
    {
        auto recordWrite = [&](ColorBacking* backing, uint32_t targetMask)
        {
            if (!backing || !targetMask)
                return;
            if (backing->info == 0x00000438u)
            {
                auto& h = g_blurRtWriteHistory[
                    g_blurRtWriteHistoryCursor++ % g_blurRtWriteHistory.size()];
                h.frame = g_frames.load(std::memory_order_relaxed) + 1;
                h.draw = g_draws.load(std::memory_order_relaxed) + 1;
                h.vs = vsHash;
                h.ps = psHash;
                h.binMask = draw.binMask;
                h.binSelect = draw.binSelect;
                h.prim = draw.primType;
                h.count = prepared.count;
                h.colorInfo = regs[xenos::kRbColorInfo];
                h.color1Info = regs[xenos::kRbColor1Info];
                h.colorMask = regs[xenos::kRbColorMask];
                h.blend = regs[xenos::kRbBlendControl0];
                h.depth = regs[xenos::kRbDepthControl];
                h.windowTl = regs[xenos::kPaScWindowScissorTl];
                h.windowBr = regs[xenos::kPaScWindowScissorBr];
                h.windowOffset = regs[xenos::kPaScWindowOffset];
                h.indexed = prepared.indexed;
                h.predicated = draw.predicated;
            }
            backing->lastWriteFrame = g_frames.load(std::memory_order_relaxed) + 1;
            backing->lastWriteDraw = g_draws.load(std::memory_order_relaxed) + 1;
            backing->lastWriteGeneration = g_renderWriteGeneration;
        };
        recordWrite(ActiveColorBacking(), colorMask & 0xFu);
        recordWrite(ActiveColor1Backing(), (colorMask >> 4) & 0xFu);
    }
    if (vs->usesTextures || ps->usesTextures)
        g_texturedDraws.fetch_add(1, std::memory_order_relaxed);
    const uint64_t n = g_draws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        KLOG("Vulkan raster draw #%llu prim=%u count=%u indexed=%u VS=%016llX PS=%016llX "
             "viewport=%.1f,%.1f %.1fx%.1f\n",
             static_cast<unsigned long long>(n), draw.primType, prepared.count,
             prepared.indexed ? 1u : 0u,
             static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash),
             viewport.x, viewport.y, viewport.width, viewport.height);
    return true;
}

bool VkPresenter_Present(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    if (!g_active)
        return false;
    if (g_outputSuspended)
        return true;
    const auto presentStart = PerfClock::now();
    const bool ok = EndFrameAndPresent(frontBuffer, width, height);
    g_perfPresentNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        PerfClock::now() - presentStart).count());
    const uint64_t frame = g_frames.load(std::memory_order_relaxed);
    if (ok && g_perfWindowStartFrame == 0)
    {
        g_perfWindowStart = PerfClock::now();
        g_perfWindowStartFrame = frame;
        g_perfFenceWaitNs = 0;
        g_perfPresentNs = 0;
        g_perfTextureNs = 0;
        g_perfPipelineNs = 0;
        g_perfConstantsNs = 0;
        g_perfVertexNs = 0;
        g_perfResolveNs = 0;
        g_perfAttachmentNs = 0;
        g_perfDrawTotalNs = 0;
        g_perfDrawCalls = 0;
        g_perfResolveCalls = 0;
        g_perfColorSwitches = 0;
        g_perfBackingSaves = 0;
        g_perfBackingRestores = 0;
        g_perfBackingPixels = 0;
        g_perfResolvePixels = 0;
        g_perfColorClears = 0;
        g_perfDepthClears = 0;
        g_perfClearRectPixels = 0;
        g_perfEdramOwnershipTransfers = 0;
        g_perfEdramOwnershipTiles = 0;
        g_perfEdramOwnershipRects = 0;
        g_perfEdramOwnershipPixels = 0;
        ResetEdramTransferPairProfile();
        g_perfTextureUploadBytesR8 = 0;
        g_perfTextureUploadBytesRGBA = 0;
        g_perfTextureUploadBytesBC1 = 0;
        g_perfTextureUploadCount = 0;
        g_perfTextureRefreshBytesR8 = 0;
        g_perfTextureRefreshBytesRGBA = 0;
        g_perfTextureRefreshBytesBC1 = 0;
        g_perfTextureRefreshCount = 0;
        g_perfTextureHashBytesR8 = 0;
        g_perfTextureHashCallsR8 = 0;
        g_perfTextureHashBytesRGBA = 0;
        g_perfTextureHashCallsRGBA = 0;
        g_perfTextureHashBytesRGBAAuthored = 0;
        g_perfTextureHashCallsRGBAAuthored = 0;
        g_perfTextureHashBytesRGBAGenerated = 0;
        g_perfTextureHashCallsRGBAGenerated = 0;
        g_perfTextureHashBytesBC1 = 0;
        g_perfTextureHashCallsBC1 = 0;
        g_perfVertexUploadBytes = 0;
        g_perfVertexUploadCopies = 0;
        g_perfVertexReuseBytes = 0;
        g_perfVertexReuseHits = 0;
        g_perfIndexUploadBytes = 0;
        g_perfIndexUploadCopies = 0;
        ResetPerfShaderPairs();
        ResetPerfDepthStates();
    }
    else if (ok && frame > g_perfWindowStartFrame &&
             frame - g_perfWindowStartFrame >= 120)
    {
        const auto now = PerfClock::now();
        const uint64_t windowFrames = frame - g_perfWindowStartFrame;
        const double wallMs = std::chrono::duration<double, std::milli>(now - g_perfWindowStart).count();
        const double frameMs = wallMs / double(windowFrames);
        const double fps = wallMs > 0.0 ? double(windowFrames) * 1000.0 / wallMs : 0.0;
        const double fenceMs = double(g_perfFenceWaitNs) / 1.0e6 / double(windowFrames);
        const double presentMs = double(g_perfPresentNs) / 1.0e6 / double(windowFrames);
        const double guestMs = std::max(0.0, frameMs - presentMs);
        KLOG("Vulkan perf: %.2f fps %.2f ms/frame (guest+PM4 %.2f ms, present %.2f ms, fence %.2f ms)\n",
             fps, frameMs, guestMs, presentMs, fenceMs);
        const double drawTotalMs = double(g_perfDrawTotalNs) / 1.0e6 / double(windowFrames);
        const double drawKnownMs = double(g_perfAttachmentNs + g_perfTextureNs + g_perfPipelineNs +
                                          g_perfConstantsNs + g_perfVertexNs) /
                                   1.0e6 / double(windowFrames);
        const double drawOtherMs = std::max(0.0, drawTotalMs - drawKnownMs);
        KLOG("Vulkan CPU profile: drawTotal %.2f drawOther %.2f attach %.2f ms/frame tex %.2f pipe %.2f const %.2f vertex %.2f resolve %.2f "
             "draws %.1f/frame resolves %.1f/frame\n",
             drawTotalMs, drawOtherMs,
             double(g_perfAttachmentNs) / 1.0e6 / double(windowFrames),
             double(g_perfTextureNs) / 1.0e6 / double(windowFrames),
             double(g_perfPipelineNs) / 1.0e6 / double(windowFrames),
             double(g_perfConstantsNs) / 1.0e6 / double(windowFrames),
             double(g_perfVertexNs) / 1.0e6 / double(windowFrames),
             double(g_perfResolveNs) / 1.0e6 / double(windowFrames),
             double(g_perfDrawCalls) / double(windowFrames),
             double(g_perfResolveCalls) / double(windowFrames));
        KLOG("Vulkan GPU traffic: rtSwitch %.1f/frame save %.1f restore %.1f fullRT %.1f Mpix/frame "
             "resolve %.1f Mpix/frame clearColor %.1f clearDepth %.1f requestedClear %.1f Mpix/frame "
             "snapshotBreak %.1f/frame ownership %.1f xfer/frame %.1f Ktiles/frame %.1f rects/frame %.1f Mpix/frame\n",
             double(g_perfColorSwitches) / double(windowFrames),
             double(g_perfBackingSaves) / double(windowFrames),
             double(g_perfBackingRestores) / double(windowFrames),
             double(g_perfBackingPixels) / 1.0e6 / double(windowFrames),
             double(g_perfResolvePixels) / 1.0e6 / double(windowFrames),
             double(g_perfColorClears) / double(windowFrames),
             double(g_perfDepthClears) / double(windowFrames),
             double(g_perfClearRectPixels) / 1.0e6 / double(windowFrames),
             double(g_perfSnapshotRenderBreaks) / double(windowFrames),
             double(g_perfEdramOwnershipTransfers) / double(windowFrames),
             double(g_perfEdramOwnershipTiles) / 1000.0 / double(windowFrames),
             double(g_perfEdramOwnershipRects) / double(windowFrames),
             double(g_perfEdramOwnershipPixels) / 1.0e6 / double(windowFrames));
        if (EdramOwnershipPairProfileEnabled() && g_perfEdramTransferPairCount)
        {
            std::array<uint32_t, 64> pairOrder{};
            for (uint32_t i = 0; i < g_perfEdramTransferPairCount; ++i)
                pairOrder[i] = i;
            std::sort(pairOrder.begin(), pairOrder.begin() + g_perfEdramTransferPairCount,
                      [](uint32_t a, uint32_t b) {
                          return g_perfEdramTransferPairs[a].tiles >
                                 g_perfEdramTransferPairs[b].tiles;
                      });
            const uint32_t reportCount = std::min<uint32_t>(g_perfEdramTransferPairCount, 8u);
            for (uint32_t rank = 0; rank < reportCount; ++rank)
            {
                const auto& pair = g_perfEdramTransferPairs[pairOrder[rank]];
                KLOG("Vulkan EDRAM pair #%u %s:%016llX -> %s:%016llX "
                     "xfer=%.2f/frame tiles=%.2f K/frame\n",
                     rank + 1,
                     pair.source.kind == EdramOwnerKind::Color ? "C" : "D",
                     static_cast<unsigned long long>(pair.source.key),
                     pair.dest.kind == EdramOwnerKind::Color ? "C" : "D",
                     static_cast<unsigned long long>(pair.dest.key),
                     double(pair.transfers) / double(windowFrames),
                     double(pair.tiles) / 1000.0 / double(windowFrames));
            }
        }
        KLOG("Vulkan texture traffic: upload=%.3f MiB/frame (R8 %.3f RGBA %.3f BC1 %.3f, %.2f ops/frame) "
             "refresh=%.3f MiB/frame (R8 %.3f RGBA %.3f BC1 %.3f, %.2f ops/frame) "
             "hash=%.3f MiB/frame (R8 %.3f/%.1f calls RGBA %.3f/%.1f calls BC1 %.3f/%.1f calls)\n",
             double(g_perfTextureUploadBytesR8 + g_perfTextureUploadBytesRGBA +
                    g_perfTextureUploadBytesBC1) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureUploadBytesR8) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureUploadBytesRGBA) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureUploadBytesBC1) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureUploadCount) / double(windowFrames),
             double(g_perfTextureRefreshBytesR8 + g_perfTextureRefreshBytesRGBA +
                    g_perfTextureRefreshBytesBC1) /
                 (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureRefreshBytesR8) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureRefreshBytesRGBA) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureRefreshBytesBC1) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureRefreshCount) / double(windowFrames),
             double(g_perfTextureHashBytesR8 + g_perfTextureHashBytesRGBA +
                    g_perfTextureHashBytesBC1) /
                 (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureHashBytesR8) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureHashCallsR8) / double(windowFrames),
             double(g_perfTextureHashBytesRGBA) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureHashCallsRGBA) / double(windowFrames),
             double(g_perfTextureHashBytesBC1) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureHashCallsBC1) / double(windowFrames));
        KLOG("Vulkan texture hash split: RGBA-authored %.3f MiB/frame %.1f calls/frame "
             "RGBA-generated %.3f MiB/frame %.1f calls/frame\n",
             double(g_perfTextureHashBytesRGBAAuthored) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureHashCallsRGBAAuthored) / double(windowFrames),
             double(g_perfTextureHashBytesRGBAGenerated) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfTextureHashCallsRGBAGenerated) / double(windowFrames));
        KLOG("Vulkan vertex traffic: upload %.3f MiB/frame %.1f copies/frame localReuse %.3f MiB/frame "
             "%.1f hits/frame index %.3f MiB/frame %.1f copies/frame\n",
             double(g_perfVertexUploadBytes) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfVertexUploadCopies) / double(windowFrames),
             double(g_perfVertexReuseBytes) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfVertexReuseHits) / double(windowFrames),
             double(g_perfIndexUploadBytes) / (1024.0 * 1024.0) / double(windowFrames),
             double(g_perfIndexUploadCopies) / double(windowFrames));
        std::array<uint32_t, 64> pairOrder{};
        for (uint32_t i = 0; i < g_perfShaderPairCount; ++i)
            pairOrder[i] = i;
        std::sort(pairOrder.begin(), pairOrder.begin() + g_perfShaderPairCount,
                  [](uint32_t a, uint32_t b) {
                      return g_perfShaderPairs[a].scissorPixels >
                             g_perfShaderPairs[b].scissorPixels;
                  });
        const uint32_t pairReportCount = std::min<uint32_t>(g_perfShaderPairCount, 6u);
        for (uint32_t rank = 0; rank < pairReportCount; ++rank)
        {
            const auto& pair = g_perfShaderPairs[pairOrder[rank]];
            KLOG("Vulkan shader load #%u VS=%016llX PS=%016llX draws=%.1f/frame indexed=%.1f/frame "
                 "elements=%.3f M/frame scissor=%.1f Mpix/frame\n",
                 rank + 1,
                 static_cast<unsigned long long>(pair.vs),
                 static_cast<unsigned long long>(pair.ps),
                 double(pair.draws) / double(windowFrames),
                 double(pair.indexed) / double(windowFrames),
                 double(pair.elements) / 1.0e6 / double(windowFrames),
                 double(pair.scissorPixels) / 1.0e6 / double(windowFrames));
        }
        std::array<uint32_t, 64> depthOrder{};
        for (uint32_t i = 0; i < g_perfDepthStateCount; ++i)
            depthOrder[i] = i;
        std::sort(depthOrder.begin(), depthOrder.begin() + g_perfDepthStateCount,
                  [](uint32_t a, uint32_t b) {
                      return g_perfDepthStates[a].scissorPixels >
                             g_perfDepthStates[b].scissorPixels;
                  });
        const uint32_t depthReportCount = std::min<uint32_t>(g_perfDepthStateCount, 6u);
        for (uint32_t rank = 0; rank < depthReportCount; ++rank)
        {
            const auto& state = g_perfDepthStates[depthOrder[rank]];
            KLOG("Vulkan depth load #%u control=%08X stencil=%08X/%08X draws=%.1f/frame "
                 "indexed=%.1f/frame elements=%.3f M/frame scissor=%.1f Mpix/frame\n",
                 rank + 1, state.depthControl, state.stencilFront, state.stencilBack,
                 double(state.draws) / double(windowFrames),
                 double(state.indexed) / double(windowFrames),
                 double(state.elements) / 1.0e6 / double(windowFrames),
                 double(state.scissorPixels) / 1.0e6 / double(windowFrames));
        }
        g_perfWindowStart = now;
        g_perfWindowStartFrame = frame;
        g_perfFenceWaitNs = 0;
        g_perfPresentNs = 0;
        g_perfTextureNs = 0;
        g_perfPipelineNs = 0;
        g_perfConstantsNs = 0;
        g_perfVertexNs = 0;
        g_perfResolveNs = 0;
        g_perfAttachmentNs = 0;
        g_perfDrawTotalNs = 0;
        g_perfDrawCalls = 0;
        g_perfResolveCalls = 0;
        g_perfColorSwitches = 0;
        g_perfBackingSaves = 0;
        g_perfBackingRestores = 0;
        g_perfBackingPixels = 0;
        g_perfResolvePixels = 0;
        g_perfColorClears = 0;
        g_perfDepthClears = 0;
        g_perfClearRectPixels = 0;
        g_perfSnapshotRenderBreaks = 0;
        g_perfEdramOwnershipTransfers = 0;
        g_perfEdramOwnershipTiles = 0;
        g_perfEdramOwnershipRects = 0;
        g_perfEdramOwnershipPixels = 0;
        ResetEdramTransferPairProfile();
        g_perfTextureUploadBytesR8 = 0;
        g_perfTextureUploadBytesRGBA = 0;
        g_perfTextureUploadBytesBC1 = 0;
        g_perfTextureUploadCount = 0;
        g_perfTextureRefreshBytesR8 = 0;
        g_perfTextureRefreshBytesRGBA = 0;
        g_perfTextureRefreshBytesBC1 = 0;
        g_perfTextureRefreshCount = 0;
        g_perfTextureHashBytesR8 = 0;
        g_perfTextureHashCallsR8 = 0;
        g_perfTextureHashBytesRGBA = 0;
        g_perfTextureHashCallsRGBA = 0;
        g_perfTextureHashBytesRGBAAuthored = 0;
        g_perfTextureHashCallsRGBAAuthored = 0;
        g_perfTextureHashBytesRGBAGenerated = 0;
        g_perfTextureHashCallsRGBAGenerated = 0;
        g_perfTextureHashBytesBC1 = 0;
        g_perfTextureHashCallsBC1 = 0;
        g_perfVertexUploadBytes = 0;
        g_perfVertexUploadCopies = 0;
        g_perfVertexReuseBytes = 0;
        g_perfVertexReuseHits = 0;
        g_perfIndexUploadBytes = 0;
        g_perfIndexUploadCopies = 0;
        ResetPerfShaderPairs();
        ResetPerfDepthStates();
    }
    if (ok && (frame == 1 || (frame % 60u) == 0))
        KLOG("Vulkan raster health: frames=%llu draws=%llu indexed=%llu collapsedTile=%llu skipMode=%llu skipIndexed=%llu "
             "skipShader=%llu skipTexture=%llu skipVertex=%llu skipTopo=%llu skipPipe=%llu "
             "alphaTest=%llu alphaUnsupported=%llu resolves=%llu copies=%llu dedupResolve=%llu "
             "resolveUnsupported=%llu(src=%llu rt1=%llu depth=%llu rect=%llu snap=%llu) snapPresent=%llu fallbackPresent=%llu snapshots=%zu "
             "pipelines=%zu modules=%zu textured=%llu descriptorBundles=%zu ownerFast=%llu\n",
             static_cast<unsigned long long>(frame),
             static_cast<unsigned long long>(g_draws.load()),
             static_cast<unsigned long long>(g_indexedDraws.load()),
             static_cast<unsigned long long>(g_collapsedTileDraws.load()),
             static_cast<unsigned long long>(g_skippedMode.load()),
             static_cast<unsigned long long>(g_skippedIndexed.load()),
             static_cast<unsigned long long>(g_skippedShader.load()),
             static_cast<unsigned long long>(g_skippedTexture.load()),
             static_cast<unsigned long long>(g_skippedVertex.load()),
             static_cast<unsigned long long>(g_skippedTopology.load()),
             static_cast<unsigned long long>(g_skippedPipeline.load()),
             static_cast<unsigned long long>(g_alphaTestDraws.load()),
             static_cast<unsigned long long>(g_alphaTestUnsupported.load()),
             static_cast<unsigned long long>(g_resolves.load()),
             static_cast<unsigned long long>(g_resolveCopies.load()),
             static_cast<unsigned long long>(g_duplicateResolveSkips.load()),
             static_cast<unsigned long long>(g_resolveUnsupported.load()),
             static_cast<unsigned long long>(g_resolveUnsupportedSource.load()),
             static_cast<unsigned long long>(g_resolveUnsupportedRt1.load()),
             static_cast<unsigned long long>(g_resolveUnsupportedDepth.load()),
             static_cast<unsigned long long>(g_resolveUnsupportedRect.load()),
             static_cast<unsigned long long>(g_resolveUnsupportedSnapshot.load()),
             static_cast<unsigned long long>(g_snapshotPresents.load()),
             static_cast<unsigned long long>(g_fallbackPresents.load()),
             g_snapshots.size(),
             g_pipelines.size(), g_modules.size(),
             static_cast<unsigned long long>(g_texturedDraws.load()), g_textureBundles.size(),
             static_cast<unsigned long long>(g_edramOwnerFastPathHits));
    return ok;
}

void VkPresenter_Shutdown()
{
    if (g_device && p_vkDeviceWaitIdle)
        p_vkDeviceWaitIdle(g_device);
    SavePersistentPipelineCache();
    for (auto& p : g_pipelines)
        if (p.pipeline && p_vkDestroyPipeline)
            p_vkDestroyPipeline(g_device, p.pipeline, nullptr);
    for (auto& m : g_modules)
    {
        if (m.float24DepthModule && p_vkDestroyShaderModule)
            p_vkDestroyShaderModule(g_device, m.float24DepthModule, nullptr);
        if (m.module && p_vkDestroyShaderModule)
            p_vkDestroyShaderModule(g_device, m.module, nullptr);
    }
    for (auto& transfer : g_edramTransferPipelines)
    {
        if (transfer.pipeline && p_vkDestroyPipeline)
            p_vkDestroyPipeline(g_device, transfer.pipeline, nullptr);
        if (transfer.ps && p_vkDestroyShaderModule)
            p_vkDestroyShaderModule(g_device, transfer.ps, nullptr);
    }
    g_edramTransferPipelines.clear();
    for (auto& pack : g_depthSnapshotPackPipelines)
    {
        if (pack.pipeline && p_vkDestroyPipeline)
            p_vkDestroyPipeline(g_device, pack.pipeline, nullptr);
        if (pack.ps && p_vkDestroyShaderModule)
            p_vkDestroyShaderModule(g_device, pack.ps, nullptr);
    }
    g_depthSnapshotPackPipelines.clear();
    if (g_edramTransferVs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_edramTransferVs, nullptr);
    if (g_edramTransferPipelineLayout && p_vkDestroyPipelineLayout)
        p_vkDestroyPipelineLayout(g_device, g_edramTransferPipelineLayout, nullptr);
    if (g_edramTransferPool && p_vkDestroyDescriptorPool)
        p_vkDestroyDescriptorPool(g_device, g_edramTransferPool, nullptr);
    if (g_edramTransferSetLayout && p_vkDestroyDescriptorSetLayout)
        p_vkDestroyDescriptorSetLayout(g_device, g_edramTransferSetLayout, nullptr);
    if (g_msaaResolvePipeline && p_vkDestroyPipeline)
        p_vkDestroyPipeline(g_device, g_msaaResolvePipeline, nullptr);
    if (g_msaaResolveVs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_msaaResolveVs, nullptr);
    if (g_msaaResolvePs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_msaaResolvePs, nullptr);
    if (g_msaaResolvePipelineLayout && p_vkDestroyPipelineLayout)
        p_vkDestroyPipelineLayout(g_device, g_msaaResolvePipelineLayout, nullptr);
    if (g_msaaResolvePool && p_vkDestroyDescriptorPool)
        p_vkDestroyDescriptorPool(g_device, g_msaaResolvePool, nullptr);
    if (g_msaaResolveSetLayout && p_vkDestroyDescriptorSetLayout)
        p_vkDestroyDescriptorSetLayout(g_device, g_msaaResolveSetLayout, nullptr);
    if (g_float24DepthOnlyPs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_float24DepthOnlyPs, nullptr);
    if (g_depthMsaaResolveUnormPipeline && p_vkDestroyPipeline)
        p_vkDestroyPipeline(g_device, g_depthMsaaResolveUnormPipeline, nullptr);
    if (g_depthMsaaResolveFloatPipeline && p_vkDestroyPipeline)
        p_vkDestroyPipeline(g_device, g_depthMsaaResolveFloatPipeline, nullptr);
    if (g_depthMsaaResolveVs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_depthMsaaResolveVs, nullptr);
    if (g_depthMsaaResolvePs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_depthMsaaResolvePs, nullptr);
    if (g_depthMsaaResolvePipelineLayout && p_vkDestroyPipelineLayout)
        p_vkDestroyPipelineLayout(g_device, g_depthMsaaResolvePipelineLayout, nullptr);
    if (g_depthMsaaResolvePool && p_vkDestroyDescriptorPool)
        p_vkDestroyDescriptorPool(g_device, g_depthMsaaResolvePool, nullptr);
    if (g_depthMsaaResolveSetLayout && p_vkDestroyDescriptorSetLayout)
        p_vkDestroyDescriptorSetLayout(g_device, g_depthMsaaResolveSetLayout, nullptr);
    if (g_overlayPipeline && p_vkDestroyPipeline)
        p_vkDestroyPipeline(g_device, g_overlayPipeline, nullptr);
    if (g_overlayVs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_overlayVs, nullptr);
    if (g_overlayPs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_overlayPs, nullptr);
    if (g_overlayPipelineLayout && p_vkDestroyPipelineLayout)
        p_vkDestroyPipelineLayout(g_device, g_overlayPipelineLayout, nullptr);
    if (g_overlayDescriptorPool && p_vkDestroyDescriptorPool)
        p_vkDestroyDescriptorPool(g_device, g_overlayDescriptorPool, nullptr);
    if (g_overlaySetLayout && p_vkDestroyDescriptorSetLayout)
        p_vkDestroyDescriptorSetLayout(g_device, g_overlaySetLayout, nullptr);
    if (g_overlaySampler && p_vkDestroySampler)
        p_vkDestroySampler(g_device, g_overlaySampler, nullptr);
    if (g_overlayAtlasView && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_overlayAtlasView, nullptr);
    if (g_overlayAtlasImage && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_overlayAtlasImage, nullptr);
    if (g_overlayAtlasMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_overlayAtlasMemory, nullptr);
    if (g_fxaaPipeline && p_vkDestroyPipeline)
        p_vkDestroyPipeline(g_device, g_fxaaPipeline, nullptr);
    if (g_fxaaVs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_fxaaVs, nullptr);
    if (g_fxaaPs && p_vkDestroyShaderModule)
        p_vkDestroyShaderModule(g_device, g_fxaaPs, nullptr);
    if (g_fxaaPipelineLayout && p_vkDestroyPipelineLayout)
        p_vkDestroyPipelineLayout(g_device, g_fxaaPipelineLayout, nullptr);
    if (g_fxaaDescriptorPool && p_vkDestroyDescriptorPool)
        p_vkDestroyDescriptorPool(g_device, g_fxaaDescriptorPool, nullptr);
    if (g_fxaaSetLayout && p_vkDestroyDescriptorSetLayout)
        p_vkDestroyDescriptorSetLayout(g_device, g_fxaaSetLayout, nullptr);
    if (g_fxaaSampler && p_vkDestroySampler)
        p_vkDestroySampler(g_device, g_fxaaSampler, nullptr);
    DestroyFxaaImage();
    for (auto& snapshot : g_snapshots)
    {
        for (VkImageView view : snapshot.packedDepthBgraViews)
            if (view && p_vkDestroyImageView)
                p_vkDestroyImageView(g_device, view, nullptr);
        if (snapshot.packedDepthWriteView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, snapshot.packedDepthWriteView, nullptr);
        if (snapshot.packedDepthImage && p_vkDestroyImage)
            p_vkDestroyImage(g_device, snapshot.packedDepthImage, nullptr);
        if (snapshot.packedDepthMemory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, snapshot.packedDepthMemory, nullptr);
        if (snapshot.presentBgraView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, snapshot.presentBgraView, nullptr);
        if (snapshot.presentView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, snapshot.presentView, nullptr);
        if (snapshot.presentImage && p_vkDestroyImage)
            p_vkDestroyImage(g_device, snapshot.presentImage, nullptr);
        if (snapshot.presentMemory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, snapshot.presentMemory, nullptr);
        if (snapshot.bgraView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, snapshot.bgraView, nullptr);
        if (snapshot.view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, snapshot.view, nullptr);
        if (snapshot.image && p_vkDestroyImage)
            p_vkDestroyImage(g_device, snapshot.image, nullptr);
        if (snapshot.memory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, snapshot.memory, nullptr);
    }
    for (auto& backing : g_colorBackings)
    {
        if (backing.resolveView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, backing.resolveView, nullptr);
        if (backing.resolveImage && p_vkDestroyImage)
            p_vkDestroyImage(g_device, backing.resolveImage, nullptr);
        if (backing.resolveMemory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, backing.resolveMemory, nullptr);
        if (backing.view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, backing.view, nullptr);
        if (backing.image && p_vkDestroyImage)
            p_vkDestroyImage(g_device, backing.image, nullptr);
        if (backing.memory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, backing.memory, nullptr);
    }
    for (auto& backing : g_depthBackings)
    {
        if (backing.resolveView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, backing.resolveView, nullptr);
        if (backing.resolveImage && p_vkDestroyImage)
            p_vkDestroyImage(g_device, backing.resolveImage, nullptr);
        if (backing.resolveMemory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, backing.resolveMemory, nullptr);
        if (backing.depthSampleView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, backing.depthSampleView, nullptr);
        if (backing.stencilSampleView && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, backing.stencilSampleView, nullptr);
        if (backing.view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, backing.view, nullptr);
        if (backing.image && p_vkDestroyImage)
            p_vkDestroyImage(g_device, backing.image, nullptr);
        if (backing.memory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, backing.memory, nullptr);
    }
    for (auto& texture : g_guestTextures)
    {
        if (texture.view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, texture.view, nullptr);
        if (texture.image && p_vkDestroyImage)
            p_vkDestroyImage(g_device, texture.image, nullptr);
        if (texture.memory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, texture.memory, nullptr);
    }
    if (g_device && g_pipelineLayout && p_vkDestroyPipelineLayout)
        p_vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    if (g_texturePool && p_vkDestroyDescriptorPool)
        p_vkDestroyDescriptorPool(g_device, g_texturePool, nullptr);
    for (auto& layout : g_textureLayouts)
    {
        if (layout && p_vkDestroyDescriptorSetLayout)
            p_vkDestroyDescriptorSetLayout(g_device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    for (auto& dummy : g_dummyTextures)
    {
        if (dummy.view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, dummy.view, nullptr);
        if (dummy.image && p_vkDestroyImage)
            p_vkDestroyImage(g_device, dummy.image, nullptr);
        if (dummy.memory && p_vkFreeMemory)
            p_vkFreeMemory(g_device, dummy.memory, nullptr);
        dummy = {};
    }
    if (g_neutralBlurTexture.view && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_neutralBlurTexture.view, nullptr);
    if (g_neutralBlurTexture.image && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_neutralBlurTexture.image, nullptr);
    if (g_neutralBlurTexture.memory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_neutralBlurTexture.memory, nullptr);
    g_neutralBlurTexture = {};
    if (g_defaultSampler && p_vkDestroySampler)
        p_vkDestroySampler(g_device, g_defaultSampler, nullptr);
    for (const auto& sampler : g_samplers)
        if (p_vkDestroySampler) p_vkDestroySampler(g_device, sampler.sampler, nullptr);
    g_samplers.clear();
    g_defaultSampler = VK_NULL_HANDLE;
    g_texturePool = VK_NULL_HANDLE;
    g_pipelineLayout = VK_NULL_HANDLE;
    g_textureBundles.clear();
    g_textureBundleLookup.clear();
    g_guestTextureLookup.clear();
    g_textureCompressionBC = false;
    g_sampleRateShading = false;
    g_edramOwners.fill({});
    g_edramOwnerTileCounts.clear();
    g_edramOwnerFastPathHits = 0;
    g_edramOwnershipTransfers = 0;
    g_edramOwnershipTiles = 0;
    g_edramOwnershipUnsupported = 0;
    g_edramTransferSetLayout = VK_NULL_HANDLE;
    g_edramTransferPool = VK_NULL_HANDLE;
    g_edramTransferPipelineLayout = VK_NULL_HANDLE;
    g_edramTransferVs = VK_NULL_HANDLE;
    g_msaaResolveSetLayout = VK_NULL_HANDLE;
    g_msaaResolvePool = VK_NULL_HANDLE;
    g_msaaResolvePipelineLayout = VK_NULL_HANDLE;
    g_msaaResolvePipeline = VK_NULL_HANDLE;
    g_msaaResolveVs = VK_NULL_HANDLE;
    g_msaaResolvePs = VK_NULL_HANDLE;
    if (g_device && g_colorResolveView && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_colorResolveView, nullptr);
    if (g_device && g_colorResolveImage && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_colorResolveImage, nullptr);
    if (g_device && g_colorResolveMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_colorResolveMemory, nullptr);
    if (g_device && g_colorView && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_colorView, nullptr);
    if (g_device && g_colorImage && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_colorImage, nullptr);
    if (g_device && g_colorMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_colorMemory, nullptr);
    if (g_device && g_depthResolveView && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_depthResolveView, nullptr);
    if (g_device && g_depthResolveImage && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_depthResolveImage, nullptr);
    if (g_device && g_depthResolveMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_depthResolveMemory, nullptr);
    if (g_device && g_depthView && p_vkDestroyImageView)
        p_vkDestroyImageView(g_device, g_depthView, nullptr);
    if (g_device && g_depthImage && p_vkDestroyImage)
        p_vkDestroyImage(g_device, g_depthImage, nullptr);
    if (g_device && g_depthMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_depthMemory, nullptr);
    if (g_device && g_uploadMapped && p_vkUnmapMemory)
        p_vkUnmapMemory(g_device, g_uploadMemory);
    if (g_device && g_uploadBuffer && p_vkDestroyBuffer)
        p_vkDestroyBuffer(g_device, g_uploadBuffer, nullptr);
    if (g_device && g_uploadMemory && p_vkFreeMemory)
        p_vkFreeMemory(g_device, g_uploadMemory, nullptr);
    for (VkSemaphore semaphore : g_presentReady)
        if (g_device && semaphore && p_vkDestroySemaphore)
            p_vkDestroySemaphore(g_device, semaphore, nullptr);
    g_presentReady.clear();
    for (auto& frame : g_frameContexts)
    {
        if (g_device && frame.timestampPool && p_vkDestroyQueryPool)
            p_vkDestroyQueryPool(g_device, frame.timestampPool, nullptr);
        if (g_device && frame.fence && p_vkDestroyFence)
            p_vkDestroyFence(g_device, frame.fence, nullptr);
        if (g_device && frame.imageAvailable && p_vkDestroySemaphore)
            p_vkDestroySemaphore(g_device, frame.imageAvailable, nullptr);
        if (g_device && frame.renderFinished && p_vkDestroySemaphore)
            p_vkDestroySemaphore(g_device, frame.renderFinished, nullptr);
        frame = {};
    }
    g_gpuDrawTimestamps = false;
    g_gpuTimestampSupported = false;
    g_gpuTimestampValidBits = 0;
    g_gpuTimestampPeriodNs = 0.0f;
    if (g_device && g_commandPool && p_vkDestroyCommandPool)
        p_vkDestroyCommandPool(g_device, g_commandPool, nullptr);
    for (VkImageView view : g_swapViews)
        if (g_device && view && p_vkDestroyImageView)
            p_vkDestroyImageView(g_device, view, nullptr);
    g_swapViews.clear();
    if (g_device && g_swapchain && p_vkDestroySwapchainKHR)
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    if (g_device && g_pipelineCache && p_vkDestroyPipelineCache)
        p_vkDestroyPipelineCache(g_device, g_pipelineCache, nullptr);
    if (g_device && p_vkDestroyDevice)
        p_vkDestroyDevice(g_device, nullptr);
    if (g_instance && g_surface && p_vkDestroySurfaceKHR)
        p_vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_instance && p_vkDestroyInstance)
        p_vkDestroyInstance(g_instance, nullptr);
    if (g_vulkanModule)
        FreeLibrary(g_vulkanModule);

    g_active = false;
    g_frameOpen = false;
    g_rendering = false;
    g_instance = VK_NULL_HANDLE;
    g_physicalDevice = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_surface = VK_NULL_HANDLE;
    g_swapchain = VK_NULL_HANDLE;
    g_pipelineCache = VK_NULL_HANDLE;
    g_pipelineCachePath.clear();
    g_queue = VK_NULL_HANDLE;
    g_commandPool = VK_NULL_HANDLE;
    g_commandBuffer = VK_NULL_HANDLE;
    g_imageAvailable = VK_NULL_HANDLE;
    g_renderFinished = VK_NULL_HANDLE;
    g_fence = VK_NULL_HANDLE;
    g_framesInFlight = 1;
    g_frameSlot = 0;
    g_hostMsaa2x = false;
    g_depthMsaaResolveSetLayout = VK_NULL_HANDLE;
    g_depthMsaaResolvePool = VK_NULL_HANDLE;
    g_depthMsaaResolvePipelineLayout = VK_NULL_HANDLE;
    g_depthMsaaResolveVs = VK_NULL_HANDLE;
    g_depthMsaaResolvePs = VK_NULL_HANDLE;
    g_depthMsaaResolveUnormPipeline = VK_NULL_HANDLE;
    g_depthMsaaResolveFloatPipeline = VK_NULL_HANDLE;
    g_overlayAtlasImage = VK_NULL_HANDLE;
    g_overlayAtlasMemory = VK_NULL_HANDLE;
    g_overlayAtlasView = VK_NULL_HANDLE;
    g_overlaySampler = VK_NULL_HANDLE;
    g_overlaySetLayout = VK_NULL_HANDLE;
    g_overlayDescriptorPool = VK_NULL_HANDLE;
    g_overlayDescriptor = VK_NULL_HANDLE;
    g_overlayPipelineLayout = VK_NULL_HANDLE;
    g_overlayPipeline = VK_NULL_HANDLE;
    g_overlayVs = VK_NULL_HANDLE;
    g_overlayPs = VK_NULL_HANDLE;
    g_fxaaSampler = VK_NULL_HANDLE;
    g_fxaaSetLayout = VK_NULL_HANDLE;
    g_fxaaDescriptorPool = VK_NULL_HANDLE;
    g_fxaaDescriptor = VK_NULL_HANDLE;
    g_fxaaPipelineLayout = VK_NULL_HANDLE;
    g_fxaaPipeline = VK_NULL_HANDLE;
    g_fxaaVs = VK_NULL_HANDLE;
    g_fxaaPs = VK_NULL_HANDLE;
    g_fxaaAssetsAttempted = false;
    g_fxaaAssetsPrepared = false;
    g_fxaaResourcesAttempted = false;
    g_fxaaResourcesReady = false;
    g_overlayAtlasBytes.clear();
    g_overlayVsSpv.clear();
    g_overlayPsSpv.clear();
    g_overlayAssetsAttempted = false;
    g_overlayAssetsPrepared = false;
    g_overlayResourcesAttempted = false;
    g_overlayResourcesReady = false;
    g_colorImage = VK_NULL_HANDLE;
    g_colorView = VK_NULL_HANDLE;
    g_colorMemory = VK_NULL_HANDLE;
    g_colorResolveImage = VK_NULL_HANDLE;
    g_colorResolveView = VK_NULL_HANDLE;
    g_colorResolveMemory = VK_NULL_HANDLE;
    g_colorResolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_depthImage = VK_NULL_HANDLE;
    g_depthView = VK_NULL_HANDLE;
    g_depthMemory = VK_NULL_HANDLE;
    g_depthResolveImage = VK_NULL_HANDLE;
    g_depthResolveView = VK_NULL_HANDLE;
    g_depthResolveMemory = VK_NULL_HANDLE;
    g_depthResolveLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_float24DepthOnlyPs = VK_NULL_HANDLE;
    g_float24DepthOnlyPsAttempted = false;
    g_depthInitialized = false;
    g_uploadBuffer = VK_NULL_HANDLE;
    g_uploadMemory = VK_NULL_HANDLE;
    g_uploadMapped = nullptr;
    g_uploadAddress = 0;
    g_uploadAt = 0;
    g_uploadLimit = kFrameUploadBytes;
    g_readbackOffset = 0;
    g_readbackBytes = 0;
    g_readbackPending = false;
    g_readbackReported = false;
    g_readbackAttempts = 0;
    g_frontReadbackFrame = 0;
    g_modules.clear();
    g_pipelines.clear();
    g_snapshots.clear();
    g_colorBackings.clear();
    g_depthBackings.clear();
    g_activeColorInfo = UINT32_MAX;
    g_activeColor1Info = UINT32_MAX;
    g_activeColor1Enabled = false;
    g_activeDepthSurfaceKey = UINT64_MAX;
    g_guestTextures.clear();
    g_images.clear();
    g_imageInitialized.clear();
}

uint64_t VkPresenter_FrameCount()
{
    return g_frames.load(std::memory_order_relaxed);
}

uint64_t VkPresenter_DrawCount()
{
    return g_draws.load(std::memory_order_relaxed);
}
