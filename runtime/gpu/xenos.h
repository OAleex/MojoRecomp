#pragma once

#include <cstdint>

// Small, title-independent subset of the Xenos register map used by the
// renderer. Keep renderer code on named registers so the PM4 decoder
// and the Vulkan backend share one definition of the hardware state.
namespace xenos {

constexpr uint32_t kRbSurfaceInfo = 0x2000;
constexpr uint32_t kRbColorInfo = 0x2001;
constexpr uint32_t kRbColor1Info = 0x2003;
constexpr uint32_t kRbDepthInfo = 0x2002;
constexpr uint32_t kPaScScreenScissorTl = 0x200E;
constexpr uint32_t kPaScScreenScissorBr = 0x200F;

constexpr uint32_t kPaScWindowOffset = 0x2080;
constexpr uint32_t kPaScWindowScissorTl = 0x2081;
constexpr uint32_t kPaScWindowScissorBr = 0x2082;

constexpr uint32_t kVgtMaxVtxIndx = 0x2100;
constexpr uint32_t kVgtMinVtxIndx = 0x2101;
constexpr uint32_t kVgtIndxOffset = 0x2102;
constexpr uint32_t kVgtMultiPrimIbResetIndx = 0x2103;
constexpr uint32_t kRbColorMask = 0x2104;
constexpr uint32_t kRbBlendRed = 0x2105; // ..0x2108 = green, blue, alpha
constexpr uint32_t kRbStencilRefMaskBack = 0x210C;
constexpr uint32_t kRbStencilRefMask = 0x210D;
constexpr uint32_t kRbAlphaRef = 0x210E;
constexpr uint32_t kPaClVportXScale = 0x210F;
constexpr uint32_t kPaClVportXOffset = 0x2110;
constexpr uint32_t kPaClVportYScale = 0x2111;
constexpr uint32_t kPaClVportYOffset = 0x2112;
constexpr uint32_t kPaClVportZScale = 0x2113;
constexpr uint32_t kPaClVportZOffset = 0x2114;
constexpr uint32_t kVgtEventInitiator = 0x21F9;

constexpr uint32_t kRbDepthControl = 0x2200;
constexpr uint32_t kRbBlendControl0 = 0x2201;
constexpr uint32_t kRbColorControl = 0x2202;
constexpr uint32_t kPaClClipCntl = 0x2204;
constexpr uint32_t kPaSuScModeCntl = 0x2205;
constexpr uint32_t kPaClVteCntl = 0x2206;
constexpr uint32_t kRbModeControl = 0x2208;
constexpr uint32_t kRbBlendControl1 = 0x2209;
constexpr uint32_t kRbBlendControl2 = 0x220A;
constexpr uint32_t kRbBlendControl3 = 0x220B;
constexpr uint32_t kVgtMultiPrimIbResetEn = 0x22A5;

constexpr uint32_t kPaSuVtxCntl = 0x2302;

constexpr uint32_t kRbCopyControl = 0x2318;
constexpr uint32_t kRbCopyDestBase = 0x2319;
constexpr uint32_t kRbCopyDestPitch = 0x231A;
constexpr uint32_t kRbCopyDestInfo = 0x231B;
constexpr uint32_t kRbDepthClear = 0x231D;
constexpr uint32_t kRbColorClear = 0x231E;

constexpr uint32_t kAluConstantBase = 0x4000;
constexpr uint32_t kFetchConstantBase = 0x4800;
constexpr uint32_t kBoolConstantBase = 0x4900;
constexpr uint32_t kLoopConstantBase = 0x4908;

enum PrimType : uint32_t
{
    kPointList = 1,
    kLineList = 2,
    kLineStrip = 3,
    kTriangleList = 4,
    kTriangleFan = 5,
    kTriangleStrip = 6,
    kRectangleList = 8,
    kLineLoop = 12,
    kQuadList = 13,
    kQuadStrip = 14,
};
enum class MsaaSamples : uint32_t
{
    k1X = 0,
    k2X = 1,
    k4X = 2,
};

constexpr uint32_t kEdramTileWidthSamples = 80;
constexpr uint32_t kEdramTileHeightSamples = 16;
constexpr uint32_t kEdramTileCount = 2048;

inline uint32_t SurfacePitchPixels(uint32_t surfaceInfo)
{
    return surfaceInfo & 0x3FFFu;
}

inline MsaaSamples SurfaceMsaaSamples(uint32_t surfaceInfo)
{
    const uint32_t raw = (surfaceInfo >> 16) & 3u;
    return raw >= 2u ? MsaaSamples::k4X : static_cast<MsaaSamples>(raw);
}

inline uint32_t ColorBaseTiles(uint32_t colorInfo)
{
    // Xenos EDRAM addressing is periodic at 11 bits. Bit 11 exists in the
    // register encoding, but aliases the same physical 10 MiB EDRAM period.
    return colorInfo & (kEdramTileCount - 1u);
}

inline uint32_t DepthBaseTiles(uint32_t depthInfo)
{
    return depthInfo & (kEdramTileCount - 1u);
}

inline uint32_t ColorFormat(uint32_t colorInfo)
{
    return (colorInfo >> 16) & 0xFu;
}

inline bool ColorFormatIs64Bpp(uint32_t colorInfo)
{
    switch (ColorFormat(colorInfo))
    {
        case 5:  // 16_16_16_16
        case 7:  // 16_16_16_16_FLOAT
            return true;
        default:
            return false;
    }
}

inline uint32_t SurfacePitchTiles(uint32_t surfaceInfo, bool is64Bpp)
{
    const MsaaSamples msaa = SurfaceMsaaSamples(surfaceInfo);
    uint32_t pitchSamples = SurfacePitchPixels(surfaceInfo);
    if (msaa == MsaaSamples::k4X)
        pitchSamples <<= 1;
    uint32_t pitchTiles =
        (pitchSamples + (kEdramTileWidthSamples - 1u)) / kEdramTileWidthSamples;
    if (is64Bpp)
        pitchTiles <<= 1;
    return pitchTiles;
}

inline uint32_t EdramTileWidthPixels(uint32_t surfaceInfo, bool is64Bpp)
{
    const MsaaSamples msaa = SurfaceMsaaSamples(surfaceInfo);
    return kEdramTileWidthSamples >>
        (uint32_t(msaa == MsaaSamples::k4X) + uint32_t(is64Bpp));
}

inline uint32_t EdramTileHeightPixels(uint32_t surfaceInfo)
{
    return kEdramTileHeightSamples >>
        uint32_t(SurfaceMsaaSamples(surfaceInfo) != MsaaSamples::k1X);
}

// Two dwords per vertex-fetch slot. A slot is constIndex * 3 + constIndexSelect.
struct VertexFetch
{
    uint32_t address;
    uint32_t sizeDwords;
    uint32_t endian;
    uint32_t type;
};

inline VertexFetch DecodeVertexFetch(const uint32_t* regs, uint32_t slot)
{
    const uint32_t d0 = regs[kFetchConstantBase + slot * 2];
    const uint32_t d1 = regs[kFetchConstantBase + slot * 2 + 1];
    return {d0 & ~3u, (d1 >> 2) & 0xFFFFFFu, d1 & 3u, d0 & 3u};
}

} // namespace xenos
