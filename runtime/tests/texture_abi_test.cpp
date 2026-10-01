#include "../gpu/texture_abi.h"
#include <array>
#include <cstdio>

int main()
{
    using namespace mojorecomp::texture_abi;
    std::array<uint8_t, 288> shared;
    shared.fill(0xCD);
    WriteIndices(shared.data());
    for (uint32_t bank = 0; bank < kBanks; ++bank)
        for (uint32_t slot = 0; slot < kSlots; ++slot)
        {
            uint32_t actual;
            std::memcpy(&actual, shared.data() + IndexOffset(bank, slot), 4);
            if (actual != slot) return 1;
        }
    for (size_t i = 256; i < shared.size(); ++i)
        if (shared[i] != 0xCD) return 2;
    uint32_t fetch[6] = {2 | (2 << 10) | (2 << 13), 0xA1234006,
        1279 | (719 << 13), (0x688 << 1) | (1 << 19) | (1 << 21), 0, 1 << 9};
    const auto f = Decode(fetch);
    if (!f.simple || f.key != 0x01234000 || f.width != 1280 || f.height != 720) return 3;
    fetch[4] |= 1 << 6;
    if (Decode(fetch).simple) return 4;
    fetch[4] = 0;
    fetch[3] ^= 1 << 1;
    if (Decode(fetch).simple) return 5;

    // Xenos compressed texture formats map directly to Vulkan BC blocks after
    // applying the fetch endian transform. DXT1 uses 8-byte blocks, while
    // DXT2/3 and DXT4/5 use 16-byte blocks.
    if (BlockCompressedBytesPerBlock(18u) != 8u ||
        BlockCompressedBytesPerBlock(19u) != 16u ||
        BlockCompressedBytesPerBlock(20u) != 16u ||
        BlockCompressedBytesPerBlock(6u) != 0u)
        return 23;

    // Real Crash of the Titans RGBA8 fetch observed in Episode 1. Its authored
    // mip chain lives at a separate address and levels <=16x16 share one packed
    // 32x32 tail tile.
    const uint32_t crashPackedFetch[6] = {
        0x04000002u, 0x0DBDB086u, 0x003FE1FFu,
        0x0AA80C14u, 0x00000D83u, 0x0DCDBA00u};
    const auto crash = Decode(crashPackedFetch);
    if (crash.key != 0x0DBDB000u || crash.mipKey != 0x0DCDB000u ||
        crash.format != 6u || crash.width != 512u || crash.height != 512u ||
        crash.pitch != 512u || crash.endian != 2u || crash.swizzle != 0x60Au ||
        crash.mipFilter != 1u || crash.mipMin != 0u || crash.mipMax != 6u ||
        !crash.packedMips)
        return 6;

    std::array<LinearRgba8MipLayout, 16> mips{};
    uint32_t mipCount = 0;
    bool authored = false;
    if (!BuildLinearRgba8MipLayout(crash, 7u, mips, mipCount, authored) ||
        !authored || mipCount != 7u)
        return 7;

    const uint32_t expectedWidth[7] = {512, 256, 128, 64, 32, 16, 8};
    const uint32_t expectedPitch[7] = {512, 256, 128, 64, 64, 64, 64};
    const uint32_t expectedOffset[7] = {0, 0, 0x40000, 0x50000,
                                        0x54000, 0x56000, 0x56000};
    const uint32_t expectedX[7] = {0, 0, 0, 0, 0, 16, 8};
    for (uint32_t level = 0; level < 7; ++level)
    {
        const auto& mip = mips[level];
        if (mip.level != level || mip.width != expectedWidth[level] ||
            mip.height != expectedWidth[level] || mip.pitchPixels != expectedPitch[level] ||
            mip.byteOffset != expectedOffset[level] || mip.offsetX != expectedX[level] ||
            mip.offsetY != 0u || mip.mipBacking != (level != 0u))
            return 8 + static_cast<int>(level);
    }

    auto generatedOnly = crash;
    generatedOnly.mipKey = 0;
    mipCount = 0;
    authored = true;
    if (!BuildLinearRgba8MipLayout(generatedOnly, 7u, mips, mipCount, authored) ||
        authored || mipCount != 1u || mips[0].mipBacking)
        return 16;

    auto unpacked = crash;
    unpacked.packedMips = false;
    mipCount = 0;
    authored = false;
    if (!BuildLinearRgba8MipLayout(unpacked, 7u, mips, mipCount, authored) ||
        !authored || mipCount != 7u || mips[5].byteOffset != 0x56000u ||
        mips[6].byteOffset != 0x58000u || mips[5].offsetX || mips[6].offsetX)
        return 17;

    // Rectangular authored textures are common in the real Episode 1 trace.
    // The short axis determines where the packed tail begins, while the base
    // aspect ratio determines whether the tail is laid out along X or Y.
    auto checkRect = [&](uint32_t width, uint32_t height, uint32_t levels,
                         uint32_t packedLevel, uint32_t packedByteOffset,
                         uint32_t firstX, uint32_t firstY,
                         uint32_t secondX, uint32_t secondY) {
        auto rect = crash;
        rect.width = width;
        rect.height = height;
        rect.pitch = width;
        rect.mipMax = levels - 1u;
        mipCount = 0;
        authored = false;
        if (!BuildLinearRgba8MipLayout(rect, levels, mips, mipCount, authored) ||
            !authored || mipCount != levels)
            return false;
        const auto& first = mips[packedLevel];
        const auto& second = mips[packedLevel + 1u];
        return first.byteOffset == packedByteOffset &&
               second.byteOffset == packedByteOffset &&
               first.offsetX == firstX && first.offsetY == firstY &&
               second.offsetX == secondX && second.offsetY == secondY;
    };
    if (!checkRect(64u, 128u, 4u, 2u, 0x4000u, 16u, 0u, 8u, 0u))
        return 18;
    if (!checkRect(128u, 64u, 4u, 2u, 0x2000u, 0u, 16u, 0u, 8u))
        return 19;
    if (!checkRect(256u, 128u, 5u, 3u, 0xA000u, 0u, 16u, 0u, 8u))
        return 20;
    if (!checkRect(512u, 256u, 6u, 4u, 0x2A000u, 0u, 16u, 0u, 8u))
        return 21;

    // Deep synthetic chain: after a 512x256 texture reaches 1x1, its current
    // dimensions no longer reveal that the original texture was wider. Xenos
    // still packs that final level along X, based on the original aspect ratio.
    auto deepWide = crash;
    deepWide.width = 512u;
    deepWide.height = 256u;
    deepWide.pitch = 512u;
    deepWide.mipMax = 9u;
    mipCount = 0;
    authored = false;
    if (!BuildLinearRgba8MipLayout(deepWide, 10u, mips, mipCount, authored) ||
        !authored || mipCount != 10u ||
        mips[7].offsetX != 16u || mips[7].offsetY != 0u ||
        mips[8].offsetX != 8u || mips[8].offsetY != 0u ||
        mips[9].width != 1u || mips[9].height != 1u ||
        mips[9].offsetX != 4u || mips[9].offsetY != 0u)
        return 22;

    // Episode 9 uses linear R8 masks with authored packed mip chains. This is
    // the exact fetch captured when the renderer previously gated every draw.
    const uint32_t episode9R8Fetch[6] = {
        0x02000002u, 0x0C21A002u, 0x000FE07Fu,
        0x01001400u, 0x00000100u, 0x0C222A00u};
    const auto episode9R8 = Decode(episode9R8Fetch);
    if (episode9R8.format != 2u || episode9R8.width != 128u ||
        episode9R8.height != 128u || episode9R8.mipMax != 4u ||
        episode9R8.mipKey != 0x0C222000u || !episode9R8.packedMips)
        return 24;
    mipCount = 0;
    authored = false;
    if (!BuildLinearR8MipLayout(episode9R8, 5u, mips, mipCount, authored) ||
        !authored || mipCount != 5u)
        return 25;
    const uint32_t expectedR8Width[5] = {128u, 64u, 32u, 16u, 8u};
    const uint32_t expectedR8Offset[5] = {0u, 0u, 0x4000u, 0x6000u, 0x6000u};
    const uint32_t expectedR8X[5] = {0u, 0u, 0u, 16u, 8u};
    for (uint32_t level = 0; level < 5u; ++level)
    {
        const auto& mip = mips[level];
        if (mip.width != expectedR8Width[level] ||
            mip.height != expectedR8Width[level] ||
            mip.pitchPixels != 256u ||
            mip.byteOffset != expectedR8Offset[level] ||
            mip.offsetX != expectedR8X[level] || mip.offsetY != 0u)
            return 26 + static_cast<int>(level);
    }

    std::puts("PASS: descriptor banks, fetch decode, and Xenos linear RGBA8/R8 packed mip layouts");
    return 0;
}
