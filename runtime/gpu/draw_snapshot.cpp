#include "draw_snapshot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <unordered_map>

#include "guest_memory_snapshot.h"
#include "shader_cache.h"
#include "shader_metadata.h"
#include "texture_abi.h"
#include "xenos.h"

namespace mojorecomp::gpu {
namespace {

std::array<std::unordered_map<uint64_t, ShaderMetadata>, 2> g_shaderMetadata;
std::atomic<uint64_t> g_buildFailures{0};

constexpr uint32_t kGuestPhysicalBase = 0xA0000000u;
constexpr uint32_t kGuestPhysicalEnd = 0xBFFF0000u;

uint32_t PhysicalToCached(uint32_t physical)
{
    return kGuestPhysicalBase | (physical & 0x1FFFFFFFu);
}

bool CaptureRange(GuestMemorySnapshot& snapshot, const uint8_t* base,
                  uint32_t address, uint64_t bytes)
{
    if (!bytes)
        return true;
    if (address < kGuestPhysicalBase ||
        uint64_t(address) + bytes > kGuestPhysicalEnd)
        return false;
    return snapshot.Capture(base, address, bytes);
}

const ShaderMetadata* FindShaderMetadata(uint32_t type, uint64_t hash)
{
    if (type >= g_shaderMetadata.size() || !hash)
        return nullptr;
    static std::array<uint64_t, 2> lastHash{};
    static std::array<const ShaderMetadata*, 2> lastMetadata{};
    if (lastHash[type] == hash && lastMetadata[type])
        return lastMetadata[type];

    auto& cache = g_shaderMetadata[type];
    if (const auto found = cache.find(hash); found != cache.end())
    {
        lastHash[type] = hash;
        lastMetadata[type] = &found->second;
        return lastMetadata[type];
    }

    const MojoTranslatedShader* translated = ShaderCache_Find(type, hash);
    if (!translated)
        return nullptr;
    ShaderMetadata metadata{};
    if (!ParseShaderMetadata(translated->metaJson, type, metadata))
        return nullptr;
    lastHash[type] = hash;
    lastMetadata[type] = &cache.emplace(hash, std::move(metadata)).first->second;
    return lastMetadata[type];
}

uint32_t TextureMipLevels(const texture_abi::Fetch2D& fetch)
{
    uint32_t geometricMipMax = 0;
    for (uint32_t width = fetch.width, height = fetch.height;
         (width > 1u || height > 1u) && geometricMipMax < 15u; ++geometricMipMax)
    {
        width = std::max(1u, width >> 1);
        height = std::max(1u, height >> 1);
    }
    return std::min(fetch.mipMax, geometricMipMax) + 1u;
}

bool CaptureLinearMipLayout(GuestMemorySnapshot& snapshot, const uint8_t* base,
                            const texture_abi::Fetch2D& fetch,
                            uint32_t bytesPerPixel, bool r8)
{
    std::array<texture_abi::LinearRgba8MipLayout, 16> layout{};
    uint32_t count = 0;
    bool authoredMips = false;
    const uint32_t levels = TextureMipLevels(fetch);
    const bool ok = r8
        ? texture_abi::BuildLinearR8MipLayout(fetch, levels, layout, count,
                                              authoredMips)
        : texture_abi::BuildLinearRgba8MipLayout(fetch, levels, layout, count,
                                                 authoredMips);
    if (!ok)
        return false;

    const uint32_t baseAddress = PhysicalToCached(fetch.key);
    const uint32_t mipAddress = authoredMips ? PhysicalToCached(fetch.mipKey) : 0u;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& mip = layout[i];
        const uint32_t allocation = mip.mipBacking ? mipAddress : baseAddress;
        const uint64_t rowPitch = uint64_t(mip.pitchPixels) * bytesPerPixel;
        const uint64_t rowBytes = uint64_t(mip.width) * bytesPerPixel;
        const uint64_t offset = uint64_t(mip.byteOffset) +
            (uint64_t(mip.offsetY) * mip.pitchPixels + mip.offsetX) * bytesPerPixel;
        const uint64_t span = mip.height
            ? uint64_t(mip.height - 1u) * rowPitch + rowBytes
            : 0u;
        const uint64_t address = uint64_t(allocation) + offset;
        if (address > UINT32_MAX ||
            !CaptureRange(snapshot, base, static_cast<uint32_t>(address), span))
            return false;
    }
    return true;
}

bool CaptureTextureBacking(GuestMemorySnapshot& snapshot, const uint8_t* base,
                           const texture_abi::Fetch2D& fetch)
{
    if (!fetch.key || fetch.type == 0 || !fetch.width || !fetch.height)
        return true;

    if (!fetch.tiled && fetch.format == 2u)
        return CaptureLinearMipLayout(snapshot, base, fetch, 1u, true);
    if (!fetch.tiled && fetch.format == 6u)
        return CaptureLinearMipLayout(snapshot, base, fetch, 4u, false);

    const uint32_t blockBytes = texture_abi::BlockCompressedBytesPerBlock(fetch.format);
    if (!fetch.tiled && blockBytes && !fetch.mipMin && !fetch.mipMax)
    {
        const uint32_t sourcePitch = fetch.pitch ? fetch.pitch : fetch.width;
        const uint64_t blockWidth = (uint64_t(sourcePitch) + 3u) / 4u;
        const uint64_t blockHeight = (uint64_t(fetch.height) + 3u) / 4u;
        return CaptureRange(snapshot, base, PhysicalToCached(fetch.key),
                            blockWidth * blockHeight * blockBytes);
    }

    // Tiled RGBA8 normally comes from an EDRAM resolve. The guest-memory path
    // only checks a newly allocated surface for an all-zero initial state.
    if (fetch.tiled && fetch.format == 6u && !fetch.mipMin && !fetch.mipMax)
        return CaptureRange(snapshot, base, PhysicalToCached(fetch.key),
                            uint64_t(fetch.width) * fetch.height * 4u);
    return true;
}

std::unique_ptr<GuestMemorySnapshot> FailBuild()
{
    g_buildFailures.fetch_add(1, std::memory_order_relaxed);
    return {};
}

} // namespace

std::unique_ptr<GuestMemorySnapshot> BuildDrawSnapshot(
    uint8_t* base, const Pm4Draw& draw, const uint32_t* registers,
    uint64_t vertexShaderHash, uint64_t pixelShaderHash)
{
    if (!base || !registers)
        return FailBuild();

    auto snapshot = std::make_unique<GuestMemorySnapshot>();
    if (draw.indexed)
    {
        const uint32_t indexBytes = draw.index32 ? 4u : 2u;
        const uint64_t rawBytes = uint64_t(draw.indexCount) * indexBytes;
        const uint64_t readBytes = (!draw.index32 && (draw.indexEndian & 3u) == 2u)
            ? ((rawBytes + 3u) & ~3ull) : rawBytes;
        if (!draw.indexVa || !CaptureRange(*snapshot, base, draw.indexVa, readBytes))
            return FailBuild();
    }

    const ShaderMetadata* vertexMetadata = FindShaderMetadata(0, vertexShaderHash);
    const ShaderMetadata* pixelMetadata = FindShaderMetadata(1, pixelShaderHash);
    if ((vertexShaderHash && !vertexMetadata) ||
        (pixelShaderHash && !pixelMetadata))
        return FailBuild();

    if (vertexMetadata)
    {
        std::array<uint64_t, 96> requiredVertexDwords{};
        uint64_t sourceVertices = draw.indexCount;
        if (!draw.indexed && draw.indexCount)
        {
            const uint32_t indexOffset =
                registers[xenos::kVgtIndxOffset] & xenos::kVertexIndexMask;
            const uint32_t minVertex =
                registers[xenos::kVgtMinVtxIndx] & xenos::kVertexIndexMask;
            const uint32_t maxVertex =
                registers[xenos::kVgtMaxVtxIndx] & xenos::kVertexIndexMask;
            const bool rectangle = draw.primType == xenos::kRectangleList &&
                                   draw.indexCount == 3u;
            const uint32_t lastRaw = draw.indexCount - 1u;
            const bool remapped = rectangle || indexOffset != 0u || minVertex != 0u ||
                                  lastRaw > maxVertex;
            if (remapped)
            {
                sourceVertices =
                    uint64_t(xenos::MaxRemappedAutoVertexIndex(
                        draw.indexCount, indexOffset, minVertex, maxVertex)) + 1u;
            }
        }

        for (const auto& attribute : vertexMetadata->attributes)
        {
            if (attribute.location < 0 || attribute.indirect ||
                attribute.fetchSlot >= requiredVertexDwords.size() ||
                !attribute.strideDwords)
                continue;
            const uint64_t dwords = draw.indexed
                ? UINT64_MAX
                : sourceVertices * attribute.strideDwords;
            requiredVertexDwords[attribute.fetchSlot] = std::max(
                requiredVertexDwords[attribute.fetchSlot], dwords);
        }

        for (uint32_t slot = 0; slot < requiredVertexDwords.size(); ++slot)
        {
            if (!requiredVertexDwords[slot])
                continue;
            const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(registers, slot);
            if (!fetch.address || !fetch.sizeDwords)
                continue;
            const uint64_t captureDwords = requiredVertexDwords[slot] == UINT64_MAX
                ? uint64_t(fetch.sizeDwords)
                : std::min<uint64_t>(requiredVertexDwords[slot], fetch.sizeDwords);
            if (!CaptureRange(*snapshot, base, PhysicalToCached(fetch.address),
                              captureDwords * 4u))
                return FailBuild();
        }
    }

    // Resolve rectangles are fixed-function reads independent of translated
    // vertex attributes, so preserve the three XY corners explicitly.
    if ((registers[xenos::kRbModeControl] & 7u) == 6u)
    {
        const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(registers, 0);
        if (fetch.address && fetch.sizeDwords >= 6u)
        {
            const uint32_t address = PhysicalToCached(fetch.address);
            constexpr uint64_t bytes = 6u * sizeof(uint32_t);
            if (!snapshot->Resolve(address, bytes) &&
                !CaptureRange(*snapshot, base, address, bytes))
                return FailBuild();
        }
    }

    std::array<bool, texture_abi::kSlots> capturedTextureSlots{};
    for (const auto* metadata : {vertexMetadata, pixelMetadata})
    {
        if (!metadata)
            continue;
        for (uint32_t slot : metadata->textureSlots)
        {
            if (slot >= capturedTextureSlots.size() || capturedTextureSlots[slot])
                continue;
            capturedTextureSlots[slot] = true;
            const uint32_t* raw = registers + xenos::kFetchConstantBase + slot * 6u;
            if (!CaptureTextureBacking(*snapshot, base, texture_abi::Decode(raw)))
                return FailBuild();
        }
    }
    return snapshot;
}

uint64_t DrawSnapshotBuildFailureCount() noexcept
{
    return g_buildFailures.load(std::memory_order_relaxed);
}

void DrawSnapshotCacheShutdown()
{
    for (auto& cache : g_shaderMetadata)
        cache.clear();
}

} // namespace mojorecomp::gpu
