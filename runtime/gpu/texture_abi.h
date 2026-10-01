#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace mojorecomp::texture_abi {

// Pinned XenosRecomp: shader_recompiler.cpp RegisterSet::Sampler and
// shader_common.h register(t0, space0..2), register(s0, space3).
constexpr uint32_t kSlots = 16;
constexpr uint32_t kBanks = 4;
constexpr uint32_t kBankBytes = 64;
constexpr uint32_t IndexOffset(uint32_t bank, uint32_t slot)
{
    return bank * kBankBytes + slot * sizeof(uint32_t);
}

inline void WriteIndices(uint8_t* shared)
{
    for (uint32_t bank = 0; bank < kBanks; ++bank)
        for (uint32_t slot = 0; slot < kSlots; ++slot)
            std::memcpy(shared + IndexOffset(bank, slot), &slot, sizeof(slot));
}

struct Fetch2D
{
    uint32_t key, format, width, height, dimension, type, swizzle;
    uint32_t clampX, clampY, minFilter, magFilter, mipFilter, mipMin, mipMax;
    uint32_t mipKey;
    uint32_t endian, pitch;
    uint32_t signX, signY, signZ, signW;
    uint32_t numFormat;
    int32_t expAdjust;
    bool tiled;
    bool packedMips;
    bool simple;
};

// Xenos texture formats 18, 19 and 20 are DXT1, DXT2/3 and DXT4/5.
// Their encoded blocks are directly compatible with Vulkan BC1, BC2 and BC3
// after applying the fetch endian transform.
inline uint32_t BlockCompressedBytesPerBlock(uint32_t format)
{
    switch (format)
    {
        case 18u:
            return 8u;
        case 19u:
        case 20u:
            return 16u;
        default:
            return 0u;
    }
}

// Physical layout of one linear RGBA8 mip in a Xenos texture allocation.
// Level 0 is backed by Fetch2D::key. Authored levels > 0 are backed by
// Fetch2D::mipKey plus byteOffset. Packed tail levels share one 32x32 tile and
// are selected by offsetX/offsetY inside that tile.
struct LinearRgba8MipLayout
{
    uint32_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitchPixels = 0;
    uint32_t byteOffset = 0;
    uint32_t offsetX = 0;
    uint32_t offsetY = 0;
    bool mipBacking = false;
};

inline uint32_t RoundUp(uint32_t value, uint32_t alignment)
{
    return alignment ? ((value + alignment - 1u) / alignment) * alignment : value;
}

inline uint32_t NextPowerOfTwo(uint32_t value)
{
    if (value <= 1u)
        return 1u;
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return value + 1u;
}

inline uint32_t LinearRgba8MipPitchPixels(uint32_t width)
{
    // Xenos linear texture storage is 32-texel aligned and the byte pitch is
    // additionally aligned to 256 bytes. RGBA8 is one 4-byte block per texel.
    return RoundUp(RoundUp(width, 32u) * 4u, 256u) / 4u;
}

inline uint32_t LinearR8MipPitchPixels(uint32_t width)
{
    // R8 uses the same 32-texel and 256-byte Xenos linear alignment rules,
    // but each texel occupies one byte rather than four.
    return RoundUp(RoundUp(width, 32u), 256u);
}

inline uint32_t LinearRgba8MipStorageHeight(uint32_t height)
{
    return RoundUp(height, 32u);
}

inline bool PackedMipOffset(uint32_t width, uint32_t height,
                            uint32_t packedTile, bool baseWiderThanTall,
                            uint32_t& offsetX, uint32_t& offsetY)
{
    offsetX = 0;
    offsetY = 0;
    if ((width < height ? width : height) > 16u)
        return false;

    if (packedTile < 3u)
    {
        const uint32_t offset = 16u >> packedTile;
        if (baseWiderThanTall)
            offsetY = offset;
        else
            offsetX = offset;
    }
    else
    {
        // Xenos derives this from the long axis at the start of the packed
        // tail. Expressed in terms of this mip's logical extent, that is four
        // times the current long axis. Unlike the first three packed levels,
        // this matters for rectangular textures (for example a 32x16 tail).
        const uint32_t offset = 4u * (width > height ? width : height);
        if (baseWiderThanTall)
            offsetX = offset;
        else
            offsetY = offset;
    }
    return true;
}

inline bool BuildLinearRgba8MipLayout(
    const Fetch2D& fetch, uint32_t hostMipLevels,
    std::array<LinearRgba8MipLayout, 16>& levels,
    uint32_t& levelCount, bool& authoredMips)
{
    levelCount = 0;
    authoredMips = false;
    if (!hostMipLevels || hostMipLevels > levels.size() ||
        !fetch.width || !fetch.height)
        return false;

    const uint32_t basePitch = fetch.pitch ? fetch.pitch : fetch.width;
    if (basePitch < fetch.width)
        return false;
    levels[levelCount++] = {0u, fetch.width, fetch.height, basePitch,
                            0u, 0u, 0u, false};

    authoredMips = hostMipLevels > 1u && fetch.mipKey != 0u;
    if (!authoredMips)
        return true;

    const uint32_t widthPow2 = NextPowerOfTwo(fetch.width);
    const uint32_t heightPow2 = NextPowerOfTwo(fetch.height);
    const bool baseWiderThanTall = widthPow2 > heightPow2;
    uint64_t byteOffset = 0;
    uint32_t packedMipBase = 1u;

    for (; packedMipBase < hostMipLevels; ++packedMipBase)
    {
        const uint32_t width = widthPow2 >> packedMipBase
            ? widthPow2 >> packedMipBase : 1u;
        const uint32_t height = heightPow2 >> packedMipBase
            ? heightPow2 >> packedMipBase : 1u;
        if (fetch.packedMips && (width < height ? width : height) <= 16u)
            break;

        if (byteOffset > UINT32_MAX)
            return false;
        const uint32_t pitchPixels = LinearRgba8MipPitchPixels(width);
        levels[levelCount++] = {packedMipBase, width, height, pitchPixels,
                                static_cast<uint32_t>(byteOffset), 0u, 0u, true};
        byteOffset += uint64_t(pitchPixels) *
                      LinearRgba8MipStorageHeight(height) * 4u;
    }

    for (uint32_t level = packedMipBase; level < hostMipLevels; ++level)
    {
        const uint32_t width = widthPow2 >> level ? widthPow2 >> level : 1u;
        const uint32_t height = heightPow2 >> level ? heightPow2 >> level : 1u;
        uint32_t offsetX = 0, offsetY = 0;
        if (!PackedMipOffset(width, height, level - packedMipBase,
                             baseWiderThanTall,
                             offsetX, offsetY) || byteOffset > UINT32_MAX)
            return false;
        levels[levelCount++] = {level, width, height,
                                LinearRgba8MipPitchPixels(width),
                                static_cast<uint32_t>(byteOffset),
                                offsetX, offsetY, true};
    }
    return levelCount == hostMipLevels;
}

inline bool BuildLinearR8MipLayout(
    const Fetch2D& fetch, uint32_t hostMipLevels,
    std::array<LinearRgba8MipLayout, 16>& levels,
    uint32_t& levelCount, bool& authoredMips)
{
    levelCount = 0;
    authoredMips = false;
    if (!hostMipLevels || hostMipLevels > levels.size() ||
        !fetch.width || !fetch.height)
        return false;

    const uint32_t basePitch = fetch.pitch ? fetch.pitch : fetch.width;
    if (basePitch < fetch.width)
        return false;
    levels[levelCount++] = {0u, fetch.width, fetch.height, basePitch,
                            0u, 0u, 0u, false};

    authoredMips = hostMipLevels > 1u && fetch.mipKey != 0u;
    if (!authoredMips)
        return true;

    const uint32_t widthPow2 = NextPowerOfTwo(fetch.width);
    const uint32_t heightPow2 = NextPowerOfTwo(fetch.height);
    const bool baseWiderThanTall = widthPow2 > heightPow2;
    uint64_t byteOffset = 0;
    uint32_t packedMipBase = 1u;

    for (; packedMipBase < hostMipLevels; ++packedMipBase)
    {
        const uint32_t width = widthPow2 >> packedMipBase
            ? widthPow2 >> packedMipBase : 1u;
        const uint32_t height = heightPow2 >> packedMipBase
            ? heightPow2 >> packedMipBase : 1u;
        if (fetch.packedMips && (width < height ? width : height) <= 16u)
            break;

        if (byteOffset > UINT32_MAX)
            return false;
        const uint32_t pitchPixels = LinearR8MipPitchPixels(width);
        levels[levelCount++] = {packedMipBase, width, height, pitchPixels,
                                static_cast<uint32_t>(byteOffset), 0u, 0u, true};
        byteOffset += uint64_t(pitchPixels) * LinearRgba8MipStorageHeight(height);
    }

    for (uint32_t level = packedMipBase; level < hostMipLevels; ++level)
    {
        const uint32_t width = widthPow2 >> level ? widthPow2 >> level : 1u;
        const uint32_t height = heightPow2 >> level ? heightPow2 >> level : 1u;
        uint32_t offsetX = 0, offsetY = 0;
        if (!PackedMipOffset(width, height, level - packedMipBase,
                             baseWiderThanTall,
                             offsetX, offsetY) || byteOffset > UINT32_MAX)
            return false;
        levels[levelCount++] = {level, width, height,
                                LinearR8MipPitchPixels(width),
                                static_cast<uint32_t>(byteOffset),
                                offsetX, offsetY, true};
    }
    return levelCount == hostMipLevels;
}

// Hardware fields: xenia-project/xenia src/xenia/gpu/xenos.h,
// xe_gpu_texture_fetch_t. Use shifts rather than compiler-dependent bitfields.
inline Fetch2D Decode(const uint32_t* d)
{
    Fetch2D f{};
    f.key = d[1] & 0x1FFFF000u;
    f.format = d[1] & 63u;
    f.width = (d[2] & 8191u) + 1;
    f.height = ((d[2] >> 13) & 8191u) + 1;
    f.dimension = (d[5] >> 9) & 3u;
    f.type = d[0] & 3u;
    f.signX = (d[0] >> 2) & 3u;
    f.signY = (d[0] >> 4) & 3u;
    f.signZ = (d[0] >> 6) & 3u;
    f.signW = (d[0] >> 8) & 3u;
    // Base row pitch in pixels is stored divided by 32 in bits 22..30.
    // This matters even for linear textures: Xenos requires linear rows to be
    // aligned to 256 bytes, so the storage pitch can be wider than the texture.
    f.pitch = ((d[0] >> 22) & 0x1FFu) << 5;
    f.swizzle = (d[3] >> 1) & 4095u;
    f.numFormat = d[3] & 1u;
    const uint32_t rawExpAdjust = (d[3] >> 13) & 0x3Fu;
    f.expAdjust = (rawExpAdjust & 0x20u)
        ? static_cast<int32_t>(rawExpAdjust | 0xFFFFFFC0u)
        : static_cast<int32_t>(rawExpAdjust);
    f.tiled = (d[0] >> 31) != 0;
    f.endian = (d[1] >> 6) & 3u;
    f.clampX = (d[0] >> 10) & 7u;
    f.clampY = (d[0] >> 13) & 7u;
    f.magFilter = (d[3] >> 19) & 3u;
    f.minFilter = (d[3] >> 21) & 3u;
    f.mipFilter = (d[3] >> 23) & 3u;
    f.mipMin = (d[4] >> 2) & 15u;
    f.mipMax = (d[4] >> 6) & 15u;
    f.packedMips = ((d[5] >> 11) & 1u) != 0;
    f.mipKey = d[5] & 0xFFFFF000u;
    // Initial snapshot path: unsigned normalized RGBA8, identity swizzle,
    // no exponent adjustment, integer conversion, stacking, anisotropy or LOD
    // bias. Unsupported states are rejected, not silently approximated.
    f.simple = f.type == 2 && f.dimension == 1 && f.format == 6 &&
        f.swizzle == 0x688 && !f.tiled && !f.endian &&
        !(d[0] & 0x3FCu) && !(d[1] & (1u << 10)) &&
        !(d[3] & ((63u << 13) | 1u | (7u << 25) | (7u << 28) | (1u << 31))) &&
        !(d[4] & 0xFFFFF000u) && !f.mipMin && !f.mipMax &&
        f.minFilter <= 1 && f.magFilter <= 1 && f.clampX <= 2 && f.clampY <= 2;
    return f;
}

} // namespace mojorecomp::texture_abi
