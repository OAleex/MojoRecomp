// White-box GPU integration test: compile the actual presenter into this test
// executable so descriptor allocation, shared uploads, transitions and draws are
// exercised without adding test-only public entry points to the runtime.
#include "../gpu/vk_presenter.cpp"
#include "../gpu/shader_translator.h"
#include "../host/window.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>

// The white-box presenter probe does not exercise host Debug Mode. HostWindow and
// the presenter only need these two read/poll seams, so keep deterministic stubs
// here rather than linking the complete debug clock/audio/input stack into a GPU
// regression executable.
namespace mojorecomp::debug {
void PollHotkeys() {}
DebugOverlaySnapshot GetOverlaySnapshot() noexcept { return {}; }
}

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static void ClearLive(const VkClearColorValue& value)
{
    EndColorRendering();
    ColorBacking* backing = ActiveColorBacking();
    Require(backing != nullptr, "active color backing for clear");
    TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(g_commandBuffer, backing->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ResumeColorRendering();
}

struct ProbeImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

static ProbeImage CreateProbeImage(VkExtent2D extent, VkFormat format,
                                   VkImageUsageFlags usage)
{
    ProbeImage result{};
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Require(p_vkCreateImage(g_device, &imageInfo, nullptr, &result.image) == VK_SUCCESS,
            "create probe image");

    VkMemoryRequirements requirements{};
    p_vkGetImageMemoryRequirements(g_device, result.image, &requirements);
    const uint32_t memoryType = FindMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Require(memoryType != UINT32_MAX, "find probe image memory");
    VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = memoryType;
    Require(p_vkAllocateMemory(g_device, &memoryInfo, nullptr, &result.memory) == VK_SUCCESS,
            "allocate probe image memory");
    Require(p_vkBindImageMemory(g_device, result.image, result.memory, 0) == VK_SUCCESS,
            "bind probe image memory");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Require(p_vkCreateImageView(g_device, &viewInfo, nullptr, &result.view) == VK_SUCCESS,
            "create probe image view");
    return result;
}

static void DestroyProbeImage(ProbeImage& image)
{
    if (image.view) p_vkDestroyImageView(g_device, image.view, nullptr);
    if (image.image) p_vkDestroyImage(g_device, image.image, nullptr);
    if (image.memory) p_vkFreeMemory(g_device, image.memory, nullptr);
    image = {};
}

static void WriteProbeBmp(const std::filesystem::path& path, const uint8_t* pixels,
                          VkExtent2D extent, VkFormat format)
{
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path());
    const size_t pixelBytes = size_t(extent.width) * extent.height * 4u;
    std::vector<uint8_t> bgra(pixelBytes);
    for (size_t i = 0; i < pixelBytes; i += 4)
    {
        if (format == VK_FORMAT_B8G8R8A8_UNORM)
            std::memcpy(bgra.data() + i, pixels + i, 4);
        else
        {
            bgra[i + 0] = pixels[i + 2];
            bgra[i + 1] = pixels[i + 1];
            bgra[i + 2] = pixels[i + 0];
            bgra[i + 3] = pixels[i + 3];
        }
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(extent.width);
    info.biHeight = -static_cast<LONG>(extent.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    std::ofstream output(path, std::ios::binary);
    Require(bool(output), "open FXAA probe BMP");
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(bgra.data()),
                 static_cast<std::streamsize>(bgra.size()));
    Require(bool(output), "write FXAA probe BMP");
}

static void CheckFxaaOrientation(const std::filesystem::path& capturePath)
{
    mojorecomp::config::RuntimeConfig config{};
    config.antiAliasing = mojorecomp::config::AntiAliasing::Fxaa;
    mojorecomp::config::Set(config);
    Require(HostWindow_Init(320, 180, true), "hidden FXAA window");
    Require(VkPresenter_Init(HostWindow_NativeHandle(), 320, 180), "FXAA presenter init");
    Require(BeginFrame(), "begin FXAA orientation frame");
    Require(SwitchActiveColorSurface(0, 0), "select FXAA source surface");

    ClearLive(VkClearColorValue{{0.0f, 0.0f, 1.0f, 1.0f}});
    VkClearAttachment top{};
    top.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    top.colorAttachment = 0;
    top.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    const VkClearRect topHalf{{{0, 0}, {g_internalExtent.width,
                                       g_internalExtent.height / 2u}}, 0, 1};
    p_vkCmdClearAttachments(g_commandBuffer, 1, &top, 1, &topHalf);
    EndColorRendering();

    ColorBacking* source = ActiveColorBacking();
    Require(source != nullptr, "FXAA source backing");
    TransitionColorBacking(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ProbeImage target = CreateProbeImage(
        g_outputExtent, g_swapFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    const auto savedImages = g_images;
    const auto savedViews = g_swapViews;
    const auto savedInitialized = g_imageInitialized;
    g_images = {target.image};
    g_swapViews = {target.view};
    g_imageInitialized = {false};
    Require(DrawFxaa(0, source->image, g_extent.width, g_extent.height),
            "draw FXAA orientation frame");

    VkImageMemoryBarrier toReadback{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toReadback.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toReadback.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toReadback.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toReadback.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toReadback.srcQueueFamilyIndex = toReadback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadback.image = target.image;
    toReadback.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &toReadback);

    VkBufferImageCopy copy{};
    copy.bufferOffset = g_readbackOffset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {g_outputExtent.width, g_outputExtent.height, 1};
    p_vkCmdCopyImageToBuffer(g_commandBuffer, target.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_uploadBuffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0,
                           1, &host, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS,
            "end FXAA orientation commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS,
            "reset FXAA orientation fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS,
            "submit FXAA orientation probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS,
            "wait FXAA orientation probe");
    g_frameOpen = false;

    const uint8_t* pixels = g_uploadMapped + g_readbackOffset;
    WriteProbeBmp(capturePath, pixels, g_outputExtent, g_swapFormat);
    const auto isRed = [&](uint32_t x, uint32_t y) {
        const uint8_t* pixel = pixels + (size_t(y) * g_outputExtent.width + x) * 4u;
        const uint8_t r = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[2] : pixel[0];
        const uint8_t b = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[0] : pixel[2];
        return r > 220 && b < 32;
    };
    const auto isBlue = [&](uint32_t x, uint32_t y) {
        const uint8_t* pixel = pixels + (size_t(y) * g_outputExtent.width + x) * 4u;
        const uint8_t r = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[2] : pixel[0];
        const uint8_t b = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ? pixel[0] : pixel[2];
        return b > 220 && r < 32;
    };
    const uint32_t x = g_outputExtent.width / 2u;
    Require(isRed(x, g_outputExtent.height / 4u),
            "FXAA must preserve the red top half");
    Require(isBlue(x, g_outputExtent.height * 3u / 4u),
            "FXAA must preserve the blue bottom half");

    g_images = savedImages;
    g_swapViews = savedViews;
    g_imageInitialized = savedInitialized;
    DestroyProbeImage(target);
    VkPresenter_Shutdown();
    std::puts("PASS: FXAA preserves top-to-bottom presentation orientation");
}

static void CheckPixels(bool dummy, bool rectangle = false, bool tiledResolve = false)
{
    EndColorRendering();
    ColorBacking* backing = ActiveColorBacking();
    Require(backing != nullptr, "active color backing for readback");
    TransitionColorBacking(*backing, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = g_readbackOffset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {g_extent.width, g_extent.height, 1};
    p_vkCmdCopyImageToBuffer(g_commandBuffer, backing->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_uploadBuffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS, "end probe commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS, "reset probe fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS, "submit probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS, "wait probe");
    g_frameOpen = false;
    size_t checked = 0;
    for (uint32_t y = 16; y < g_extent.height - 16; y += 32)
        for (uint32_t x = 16; x < g_extent.width - 16; x += 32)
        {
            const uint8_t* p = g_uploadMapped + g_readbackOffset + (size_t(y) * g_extent.width + x) * 4;
            const bool red = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ?
                p[0] == 0 && p[1] == 0 && p[2] == 255 :
                p[0] == 255 && p[1] == 0 && p[2] == 0;
            const bool zero = p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
            const bool blue = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM ?
                p[0] == 255 && p[1] == 0 && p[2] == 0 :
                p[0] == 0 && p[1] == 0 && p[2] == 255;
            const bool green = p[0] == 0 && p[1] == 255 && p[2] == 0;
            const bool resolved = tiledResolve ? (x < 416 ? red : x < 832 ? green : blue) : red;
            const bool outside = rectangle && (x < 128 || x >= 1152 || y < 96 || y >= 624);
            if (!(outside ? blue && p[3] == 255 : dummy ? zero : resolved && p[3] == 255))
            {
                std::fprintf(stderr, "pixel %u,%u = %u,%u,%u,%u dummy=%u\n",
                    x, y, p[0], p[1], p[2], p[3], dummy);
                throw std::runtime_error(rectangle ? "rectangle coverage readback mismatch" :
                                                    "translated texture readback mismatch");
            }
            ++checked;
        }
    std::printf("PASS: %zu sampled pixels match %s\n", checked, dummy ? "dummy negative control" :
        tiledResolve ? "three distinct resolved tiles" : "resolved red snapshot");
}

// Reserve the guest address space, committing only this synthetic vertex page.
// No game files or runtime CPU state are needed by the raster regression.
struct SyntheticGuest
{
    uint8_t* base = nullptr;
    ~SyntheticGuest() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    void Commit(uint32_t va, size_t bytes, const char* message)
    {
        Require(VirtualAlloc(base + va, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr,
                message);
    }
    void Init()
    {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
        Require(base != nullptr, "reserve synthetic guest");
        Commit(0xA0001000u, 4096, "commit synthetic vertex page");
    }
};

static void CheckAuthoredMipUpload(SyntheticGuest& guest)
{
    using namespace mojorecomp::texture_abi;
    constexpr uint32_t basePhysical = 0x01000000u;
    constexpr uint32_t mipPhysical = 0x01200000u;
    constexpr uint32_t baseVa = 0xA1000000u;
    constexpr uint32_t mipVa = 0xA1200000u;
    constexpr uint32_t mipLevels = 7u;
    guest.Commit(baseVa, 0x100000u, "commit synthetic mip base");
    guest.Commit(mipVa, 0x60000u, "commit synthetic mip backing");
    std::memset(guest.base + baseVa, 0xCD, 0x100000u);
    std::memset(guest.base + mipVa, 0xCD, 0x60000u);

    Fetch2D fetch{};
    fetch.key = basePhysical;
    fetch.mipKey = mipPhysical;
    fetch.width = 512;
    fetch.height = 512;
    fetch.pitch = 512;
    fetch.format = 6;
    fetch.dimension = 1;
    fetch.type = 2;
    fetch.swizzle = 0x60A;
    fetch.endian = 2;
    fetch.mipMax = mipLevels - 1u;
    fetch.packedMips = true;

    std::array<LinearRgba8MipLayout, 16> layout{};
    uint32_t count = 0;
    bool authored = false;
    Require(BuildLinearRgba8MipLayout(fetch, mipLevels, layout, count, authored) &&
            authored && count == mipLevels, "build synthetic authored mip layout");

    // Use a unique RGBA value per level. Guest endian=8-in-32 means the raw
    // bytes are written reversed so the Vulkan image should contain R,G,B,A.
    for (uint32_t level = 0; level < count; ++level)
    {
        const auto& mip = layout[level];
        const uint8_t r = uint8_t(0x10u + level);
        const uint8_t g = uint8_t(0x40u + level);
        const uint8_t b = uint8_t(0x70u + level);
        const uint8_t a = uint8_t(0xA0u + level);
        const uint32_t backing = mip.mipBacking ? mipVa : baseVa;
        for (uint32_t y = 0; y < mip.height; ++y)
            for (uint32_t x = 0; x < mip.width; ++x)
            {
                uint8_t* pixel = guest.base + backing + mip.byteOffset +
                    (uint64_t(mip.offsetY + y) * mip.pitchPixels + mip.offsetX + x) * 4u;
                pixel[0] = a;
                pixel[1] = b;
                pixel[2] = g;
                pixel[3] = r;
            }
    }

    Require(BeginFrame(), "begin authored mip frame");
    GuestTexture* texture = CreateGuestRGBA8Texture(guest.base, fetch);
    Require(texture != nullptr && texture->image != VK_NULL_HANDLE,
            "create authored mip guest texture");
    if (g_rendering)
        EndColorRendering();

    VkImageMemoryBarrier toReadback{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toReadback.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toReadback.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toReadback.oldLayout = texture->layout;
    toReadback.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toReadback.srcQueueFamilyIndex = toReadback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadback.image = texture->image;
    toReadback.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    p_vkCmdPipelineBarrier(g_commandBuffer,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toReadback);

    std::array<VkBufferImageCopy, mipLevels> copies{};
    std::array<size_t, mipLevels> offsets{};
    size_t readbackBytes = 0;
    for (uint32_t level = 0; level < mipLevels; ++level)
    {
        offsets[level] = readbackBytes;
        copies[level].bufferOffset = g_readbackOffset + readbackBytes;
        copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copies[level].imageExtent = {layout[level].width, layout[level].height, 1};
        readbackBytes += size_t(layout[level].width) * layout[level].height * 4u;
    }
    Require(readbackBytes <= g_readbackBytes, "authored mip readback fits staging buffer");
    p_vkCmdCopyImageToBuffer(g_commandBuffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_uploadBuffer,
        static_cast<uint32_t>(copies.size()), copies.data());

    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    Require(p_vkEndCommandBuffer(g_commandBuffer) == VK_SUCCESS,
            "end authored mip commands");
    Require(p_vkResetFences(g_device, 1, &g_fence) == VK_SUCCESS,
            "reset authored mip fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    Require(p_vkQueueSubmit(g_queue, 1, &submit, g_fence) == VK_SUCCESS,
            "submit authored mip probe");
    Require(p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 10000000000ull) == VK_SUCCESS,
            "wait authored mip probe");
    g_frameOpen = false;

    for (uint32_t level = 0; level < mipLevels; ++level)
    {
        const uint8_t expected[4] = {
            uint8_t(0x10u + level), uint8_t(0x40u + level),
            uint8_t(0x70u + level), uint8_t(0xA0u + level)};
        const size_t pixels = size_t(layout[level].width) * layout[level].height;
        const uint8_t* data = g_uploadMapped + g_readbackOffset + offsets[level];
        for (size_t i = 0; i < pixels; ++i)
        {
            const uint8_t* pixel = data + i * 4u;
            if (std::memcmp(pixel, expected, sizeof(expected)) != 0)
            {
                std::fprintf(stderr,
                    "authored mip %u pixel %zu = %u,%u,%u,%u expected=%u,%u,%u,%u\n",
                    level, i, pixel[0], pixel[1], pixel[2], pixel[3],
                    expected[0], expected[1], expected[2], expected[3]);
                throw std::runtime_error("authored mip GPU readback mismatch");
            }
        }
    }
    std::puts("PASS: authored RGBA8 mip chain including packed tail matches GPU readback");
}

static void CheckResolveRegions()
{
    std::vector<uint32_t> regs(0x5000);
    regs[xenos::kPaSuScModeCntl] = 1u << 16;
    regs[xenos::kPaScWindowOffset] = (uint32_t(-64) & 0x7FFFu) |
                                   ((uint32_t(-32) & 0x7FFFu) << 16);
    VkImageCopy copy{};
    Require(ResolveCopyRegion(regs.data(), 64, 32, 320, 160, copy) &&
        copy.srcOffset.x == 0 && copy.srcOffset.y == 0 &&
        copy.dstOffset.x == 64 && copy.dstOffset.y == 32 &&
        copy.extent.width == 256 && copy.extent.height == 128,
        "arbitrary horizontal/vertical resolve offset");
    Require(ResolveCopyRegion(regs.data(), 0, 0, 128, 64, copy) &&
        copy.srcOffset.x == 0 && copy.srcOffset.y == 0 &&
        copy.dstOffset.x == 64 && copy.dstOffset.y == 32 &&
        copy.extent.width == 64 && copy.extent.height == 32,
        "clip source and destination together");
    Require(!ResolveCopyRegion(regs.data(), 0, 0, 32, 16, copy), "empty local source");
    regs[xenos::kPaSuScModeCntl] = 0;
    Require(ResolveCopyRegion(regs.data(), 64, 32, 320, 160, copy) &&
        copy.srcOffset.x == 64 && copy.srcOffset.y == 32,
        "disabled vertex window offset");
    regs[xenos::kPaSuScModeCntl] = 1u << 16;
    regs[xenos::kPaScWindowOffset] = 16 | (8 << 16);
    Require(ResolveCopyRegion(regs.data(), 1200, 680, 1280, 720, copy) &&
        copy.srcOffset.x == 1216 && copy.srcOffset.y == 688 &&
        copy.extent.width == 64 && copy.extent.height == 32,
        "positive offset clips at physical source edge");
}

static void CheckResolveVertexCoordinates()
{
    SyntheticGuest guest;
    guest.Init();
    g_extent = {1280, 720};
    std::vector<uint32_t> regs(0x5000);
    regs[xenos::kFetchConstantBase] = 0x1000u | 3u;
    regs[xenos::kFetchConstantBase + 1] = (6u << 2) | 2u;
    regs[xenos::kPaSuScModeCntl] = 1u << 16;
    regs[xenos::kPaSuVtxCntl] = 1u;
    regs[xenos::kPaScScreenScissorBr] = 1280u | (720u << 16);

    // Recorded resolve shapes, with synthetic vertices and no title assets.
    // Expected physical clipping follows Xenia draw_util::GetResolveInfo:
    // vertex window offset -> scissor -> eight-pixel alignment. The presenter
    // then exposes logical coordinates to its existing snapshot-copy API.
    for (uint32_t source : {0u, 1u, 4u})
    {
        for (uint32_t tile = 0; tile < 3; ++tile)
        {
            const uint32_t left = tile * 416;
            const float vx0 = source == 0 ? float(left) : 0.0f;
            const float vx1 = source == 0 ? float(left + 448)
                                        : source == 1 ? 1280.0f : 448.0f;
            const float vertices[] = {vx0, 0, vx1, 0, vx1, 720};
            CopySwapped(guest.base + 0xA0001000u,
                        reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices), 2);
            regs[xenos::kRbCopyControl] = source;
            regs[xenos::kPaScWindowScissorTl] = left;
            regs[xenos::kPaScWindowScissorBr] = (left + 448) | (720u << 16);
            regs[xenos::kPaScWindowOffset] = (0u - left) & 0x7FFFu;
            uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            const auto result = DecodeResolveRectFromVertices(
                guest.base, regs.data(), 1280, 720, x0, y0, x1, y1);
            if (source == 4 && tile == 2)
            {
                Require(result == ResolveRectDecode::Empty, "third depth rectangle is empty");
                std::printf("PASS: resolve source=%u tile=%u empty\n", source, tile);
                continue;
            }
            const uint32_t expectedRight = source == 4 ? 448 : left + 448;
            Require(result == ResolveRectDecode::Valid && x0 == left && x1 == expectedRight &&
                    y0 == 0 && y1 == 720, "recorded resolve logical rectangle");
            VkImageCopy copy{};
            Require(ResolveCopyRegion(regs.data(), x0, y0, x1, y1, copy) &&
                    copy.srcOffset.x == 0 && copy.dstOffset.x == int32_t(left) &&
                    copy.extent.width == expectedRight - left,
                    "recorded resolve physical source rectangle");
            std::printf("PASS: resolve source=%u tile=%u logical=%u..%u physical=0..%u\n",
                        source, tile, x0, x1, copy.extent.width);
        }
    }

    // A physical screen scissor is intentionally not in logical tile space.
    const float vertices[] = {416, 0, 864, 0, 864, 720};
    CopySwapped(guest.base + 0xA0001000u,
                reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices), 2);
    regs[xenos::kRbCopyControl] = 0;
    regs[xenos::kPaScWindowScissorTl] = 416;
    regs[xenos::kPaScWindowScissorBr] = 864 | (720u << 16);
    regs[xenos::kPaScWindowOffset] = uint32_t(-416) & 0x7FFFu;
    regs[xenos::kPaScScreenScissorTl] = 64;
    regs[xenos::kPaScScreenScissorBr] = 128 | (720u << 16);
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Require(DecodeResolveRectFromVertices(guest.base, regs.data(), 1280, 720,
                x0, y0, x1, y1) == ResolveRectDecode::Valid && x0 == 480 && x1 == 544,
            "physical screen scissor maps to logical destination");
    std::puts("PASS: physical screen scissor 64..128 maps to logical 480..544");
}

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 2 && std::string_view(argv[1]) == "--fxaa-orientation")
        {
            CheckFxaaOrientation(argc >= 3 ? std::filesystem::path(argv[2])
                                            : std::filesystem::path{});
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--resolve-rect-cpu")
        {
            CheckResolveVertexCoordinates();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--authored-mips")
        {
            SyntheticGuest guest;
            guest.Init();
            Require(HostWindow_Init(1280, 720, true), "hidden window");
            Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "presenter init");
            CheckAuthoredMipUpload(guest);
            VkPresenter_Shutdown();
            return 0;
        }
        const bool rectangle = argc == 3 && std::string_view(argv[2]) == "--rectangle";
        const bool resolveSwapIdentity = argc == 3 && std::string_view(argv[2]) == "--resolve-swap-rgba";
        const bool resolveSwap = resolveSwapIdentity ||
            (argc == 3 && std::string_view(argv[2]) == "--resolve-swap");
        const bool resolveSource = resolveSwap ||
            (argc == 3 && std::string_view(argv[2]) == "--resolve-source");
        const bool tiledResolve = argc == 3 && std::string_view(argv[2]) == "--tiled-resolve";
        const bool inverseExec = argc == 3 && std::string_view(argv[2]) == "--conditional-exec-not";
        const bool conditionalExec = inverseExec ||
            (argc == 3 && std::string_view(argv[2]) == "--conditional-exec");
        Require(argc == 2 || rectangle || resolveSource || tiledResolve || conditionalExec,
                "expected vertex SPIR-V path [--rectangle|--resolve-source|--tiled-resolve|--conditional-exec|--conditional-exec-not]");
        SyntheticGuest guest;
        guest.Init();
        // Original synthetic Xenos microcode, not game code: exec-end containing
        // tfetch2D r0.xy from s3, followed by an identity max export to oC0.
        std::vector<uint32_t> code = {0x00012001, 0x00002800, 0,
            1u | (3u << 20) | (4u << 26),
            0x688u | (3u << 12) | (3u << 14) | (3u << 16) | (7u << 18) | (1u << 28),
            1u << 14,
            (1u << 15) | (15u << 16) | (50u << 26), 0, (2u << 24) | (3u << 30)};
        if (conditionalExec)
        {
            // Sample red, conditionally overwrite r0 with zero, then export.
            // Boolean b37 is deliberately outside the first packed DWORD.
            auto exec = [](uint64_t op, uint64_t address, uint64_t sequence) {
                return (op << 44) | (1ull << 43) | (sequence << 16) |
                       (1ull << 12) | address;
            };
            const uint64_t fetch = exec(1, 2, 1);
            const uint64_t conditional = exec(3, 3, 0) | (37ull << 34) |
                                         (uint64_t(!inverseExec) << 42);
            const uint64_t end = exec(2, 4, 0);
            code = {uint32_t(fetch), uint32_t((fetch >> 32) | (conditional << 16)),
                    uint32_t(conditional >> 16), uint32_t(end), uint32_t(end >> 32), 0,
                    code[3], code[4], code[5],
                    (15u << 16) | (50u << 26), 0, (2u << 24),
                    code[6], code[7], code[8]};
        }
        for (auto& word : code) word = __builtin_bswap32(word);
        ShaderTranslator::Result translated;
        std::string error;
        const bool translatedOk = ShaderTranslator::Translate("ps_0000000000000001", reinterpret_cast<uint8_t*>(code.data()),
            code.size() * sizeof(uint32_t), translated, error);
        Require(translatedOk, error.c_str());
        std::printf("Synthetic translated PS metadata: %s\n", translated.metaJson.c_str());
        Require(HostWindow_Init(1280, 720, true), "hidden window");
        Require(VkPresenter_Init(HostWindow_NativeHandle(), 1280, 720), "presenter init");
        if (tiledResolve) CheckResolveRegions();
        const auto clearAttachments = reinterpret_cast<PFN_vkCmdClearAttachments>(
            p_vkGetDeviceProcAddr(g_device, "vkCmdClearAttachments"));
        Require(clearAttachments != nullptr, "load synthetic tile clear");
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        Require(bool(file), "open vertex shader");
        const size_t size = size_t(file.tellg());
        Require(size && size % 4 == 0, "vertex shader size");
        std::vector<uint32_t> vertex(size / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(vertex.data()), size);
        ShaderModuleRec vs{}, ps{};
        vs.hash = 2;
        if (rectangle) vs.attributes.push_back({0, 0, 57, 1, 1, 7, 0, 0});
        ps.hash = 1;
        ps.type = 1;
        ParseShaderMeta(translated.metaJson, ps);
        Require(ps.usesTextures && ps.textureSlots == std::vector<uint32_t>{3}, "translated sampler metadata");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = vertex.data();
        Require(p_vkCreateShaderModule(g_device, &ci, nullptr, &vs.module) == VK_SUCCESS, "vertex module");
        ci.codeSize = translated.spirv.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(translated.spirv.data());
        Require(p_vkCreateShaderModule(g_device, &ci, nullptr, &ps.module) == VK_SUCCESS, "pixel module");
        g_modules.push_back(vs);
        g_modules.push_back(ps);
        std::vector<uint32_t> regs(0x5000);
        // The synthetic stream is intentionally unclamped. A zero-initialized
        // VGT_MAX_VTX_INDX means "only vertex 0" on Xenos and now that the
        // presenter honors min/max index semantics it would collapse RECTLIST
        // corners to one point.
        regs[xenos::kVgtMinVtxIndx] = 0;
        regs[xenos::kVgtMaxVtxIndx] = kXenosVertexIndexMask;
        constexpr uint32_t address = 0x01000000;
        regs[xenos::kRbCopyDestBase] = address;
        regs[xenos::kRbCopyDestPitch] = 1280 | (720 << 16);
        regs[xenos::kPaScWindowScissorBr] = 1280 | (720 << 16);
        // A zero screen scissor is empty coverage, not an unset register.
        regs[xenos::kPaScScreenScissorBr] = 1280 | (720 << 16);
        uint32_t* fetch = regs.data() + xenos::kFetchConstantBase + 3 * 6;
        fetch[0] = 2 | (2 << 10) | (2 << 13);
        fetch[1] = address | 6;
        fetch[2] = 1279 | (719 << 13);
        fetch[3] = (0x688 << 1) | (1 << 19) | (1 << 21);
        fetch[5] = 1 << 9;
        if (resolveSwap)
        {
            // Resolve swaps R/B, then the BGRA texture fetch swaps them back.
            // This must still sample the original red from both RT0 and RT1.
            regs[xenos::kRbCopyDestInfo] = (1u << 24) | (6u << 7) | 2u;
            fetch[3] = ((resolveSwapIdentity ? 0x688u : 0x60Au) << 1) | (1 << 19) | (1 << 21);
        }
        for (uint32_t pass = 0; pass < 3; ++pass)
        {
            auto setResolveVertices = [&](float left, float right) {
                const float positions[] = {left, 0, right, 0, right, 720};
                CopySwapped(guest.base + 0xA0001000u,
                            reinterpret_cast<const uint8_t*>(positions), sizeof(positions), 2);
                regs[xenos::kFetchConstantBase] = 0x1000u | 3u;
                regs[xenos::kFetchConstantBase + 1] = (6u << 2) | 2u;
                regs[xenos::kPaSuVtxCntl] = 1u;
            };
            setResolveVertices(0, 1280);
            const bool dummy = pass == 1;
            if (conditionalExec)
            {
                // b5 must not alias b37 through the legacy OR-folded DWORD.
                regs[xenos::kBoolConstantBase] = 1u << 5;
                regs[xenos::kBoolConstantBase + 1] = (dummy != inverseExec) ? (1u << 5) : 0;
            }
            Require(BeginFrame(), "begin frame");
            const bool rt1 = (resolveSource || tiledResolve) && pass == 2;
            regs[xenos::kRbCopyControl] = rt1 ? 1 : 0;
            regs[xenos::kRbColor1Info] = 0x438;
            Require(SwitchActiveColorSurface(0, rt1 ? 0x438 : 0), "select red EDRAM surface");
            ClearLive(VkClearColorValue{{resolveSwapIdentity ? 0.0f : 1.0f, 0,
                                         resolveSwapIdentity ? 1.0f : 0.0f, 1}});
            if (resolveSource)
            {
                // The title changes RB_COLOR_INFO for its resolve without an
                // intervening draw. The last drawn surface is NOT the source.
                Require(SwitchActiveColorSurface(0, rt1 ? 0 : 0x438), "select unrelated EDRAM surface");
                ClearLive(VkClearColorValue{{0, 0, 1, 1}});
            }
            if (tiledResolve)
            {
                // Three overlapping logical tiles reuse one physical EDRAM
                // window. Different colors catch both stale reads and repeats.
                for (uint32_t tile = 0; tile < 3; ++tile)
                {
                    Require(SwitchActiveColorSurface(0, rt1 ? 0x438 : 0), "select local tile");
                    ClearLive(VkClearColorValue{{0, 0, 0, 0}});
                    VkClearAttachment attachment{};
                    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    attachment.clearValue.color.float32[tile] = 1.0f;
                    attachment.clearValue.color.float32[3] = 1.0f;
                    const VkClearRect local{{{0, 0}, {448, 720}}, 0, 1};
                    clearAttachments(g_commandBuffer, 1, &attachment, 1, &local);
                    const uint32_t x = tile * 416;
                    regs[xenos::kRbCopyDestBase] = address + MacroTileOffset(x, 0, 1280);
                    regs[xenos::kPaScWindowScissorTl] = x;
                    regs[xenos::kPaScWindowScissorBr] = (x + 448) | (720 << 16);
                    regs[xenos::kPaScWindowOffset] = (0u - x) & 0x7FFFu;
                    regs[xenos::kPaSuScModeCntl] = 1u << 16;
                    setResolveVertices(float(x), float(x + 448));
                    Require(ResolveColorSurface(guest.base, regs.data()), "resolve local tile");
                }
                regs[xenos::kPaScWindowOffset] = 0;
            }
            else Require(ResolveColorSurface(guest.base, regs.data()), "resolve known source");
            ClearLive(VkClearColorValue{{0, 0, 1, 1}});
            std::array<float, kTextureSlots> sampleScales{};
            const TextureBundle* bundle = PrepareTextures(nullptr, regs.data(), vs, ps, sampleScales, true);
            Require(bundle, "snapshot descriptors");
            // Xenos blending disabled is ONE/ZERO/ADD for color and alpha.
            const uint32_t primitive = rectangle ? xenos::kRectangleList : xenos::kTriangleList;
            const VkPipeline pipeline = GetPipeline(vs, ps, primitive, 15,
                                                     0x00010001u, 0x00010001u, 0, 0, 0, 4,
                                                     false, VK_SAMPLE_COUNT_1_BIT,
                                                     kDepthUnormFormat);
            Require(pipeline != VK_NULL_HANDLE, "texture pipeline");
            const VkDeviceSize shared = UploadShared(regs.data(), sampleScales);
            Require(shared != VK_WHOLE_SIZE, "shared upload");
            if (dummy && !conditionalExec)
            {
                uint32_t index = 15;
                std::memcpy(g_uploadMapped + shared + mojorecomp::texture_abi::IndexOffset(0, 3), &index, 4);
            }
            uint64_t push[3] = {g_uploadAddress + shared, g_uploadAddress + shared, g_uploadAddress + shared};
            p_vkCmdBindPipeline(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            p_vkCmdBindDescriptorSets(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                g_pipelineLayout, 0, 4, bundle->sets, 0, nullptr);
            const VkViewport viewport = DecodeViewport(regs.data());
            VkRect2D scissor{{0, 0}, {1280, 720}};
            const float blend[4]{};
            p_vkCmdSetViewport(g_commandBuffer, 0, 1, &viewport);
            p_vkCmdSetScissor(g_commandBuffer, 0, 1, &scissor);
            p_vkCmdSetBlendConstants(g_commandBuffer, blend);
            p_vkCmdSetStencilReference(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            p_vkCmdSetStencilCompareMask(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
            p_vkCmdSetStencilWriteMask(g_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 255);
            p_vkCmdPushConstants(g_commandBuffer, g_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
            if (rectangle)
            {
                // Consecutive perimeter corners, matching rectangle-list streams.
                // The negative control also reverses winding to cover both orders.
                float vertices[3][7] = {
                    {128, 96, 0, 0, 0, 0, 0},
                    {dummy ? 128.0f : 1152.0f, dummy ? 624.0f : 96.0f, 0, 0, 0, 0, 0},
                    {1152, 624, 0, 0, 0, 0, 0}};
                // The third pass checks the indexed guest path and a nonzero
                // base vertex while the host still consumes the expanded strip.
                // Also swap corners 1 and 2 so the rectangle diagonal is 0-1;
                // a fixed 0+2-1 reconstruction leaves the rectangle malformed.
                if (pass == 2)
                    std::swap(vertices[1], vertices[2]);
                const uint32_t baseVertex = pass == 2 ? 1u : 0u;
                CopySwapped(guest.base + 0xA0001000u + baseVertex * sizeof(vertices[0]),
                            reinterpret_cast<const uint8_t*>(vertices),
                            sizeof(vertices), 2);
                regs[xenos::kFetchConstantBase] = 0x1000u | 3u;
                regs[xenos::kFetchConstantBase + 1] = ((21u + baseVertex * 7u) << 2) | 2u;
                regs[xenos::kVgtIndxOffset] = baseVertex;
                Pm4Draw draw{};
                draw.primType = primitive;
                draw.indexCount = 3;
                if (pass == 2)
                {
                    const uint32_t indices[] = {0, 1, 2};
                    draw.indexed = true;
                    draw.index32 = true;
                    draw.indexEndian = 2;
                    draw.indexVa = 0xA0001800u;
                    CopySwapped(guest.base + draw.indexVa, reinterpret_cast<const uint8_t*>(indices),
                                sizeof(indices), draw.indexEndian);
                }
                PreparedDraw prepared{};
                Require(PrepareVertexBindings(guest.base, draw, regs.data(), vs, prepared),
                        "prepare real rectangle upload");
                Require(!prepared.indexed, "rectangle expanded to non-indexed stream");
                p_vkCmdDraw(g_commandBuffer, prepared.count, 1, 0, 0);
            }
            else p_vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
            CheckPixels(dummy, rectangle, tiledResolve);
        }
        VkPresenter_Shutdown();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        VkPresenter_Shutdown();
        return 1;
    }
}
