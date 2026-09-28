#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "texture_abi.h"

namespace mojorecomp::gpu {

enum class TextureSource {
    Dummy,
    ResolveSnapshot,
    GuestTexture,
};

inline bool ShouldEnableAnisotropy(
    const texture_abi::Fetch2D& fetch, TextureSource source,
    uint32_t filteringOverride, float effectiveAnisotropy)
{
    // Render targets, video planes, lookup textures and other one-level images
    // must retain the filtering requested by the guest. Forcing anisotropy on
    // them changes UI/video sampling and can expose undefined mip data.
    return source == TextureSource::GuestTexture &&
           fetch.mipMax > fetch.mipMin && fetch.mipFilter != 2u &&
           filteringOverride > 1u && effectiveAnisotropy > 1.0f;
}

inline bool HasRecentPhysicalTileContent(uint64_t currentFrame,
                                         uint64_t lastPhysicalTileFrame)
{
    return lastPhysicalTileFrame != 0 && currentFrame <= lastPhysicalTileFrame + 1;
}

inline bool ShouldTreatAsSafeAreaOverlay(float minX, float minY,
                                         float maxX, float maxY,
                                         uint32_t logicalWidth,
                                         uint32_t logicalHeight)
{
    if (!logicalWidth || !logicalHeight)
        return false;

    const float width = maxX - minX;
    const float height = maxY - minY;

    // Small normalized quads are also UI (button icons, glyph decorations).
    // Fullscreen normalized compositors cover roughly the unit square and must
    // remain neutral after the scene has already been composed.
    const float maxMagnitude = std::max({std::fabs(minX), std::fabs(minY),
                                         std::fabs(maxX), std::fabs(maxY)});
    if (maxMagnitude <= 2.0f)
        return width < 0.75f && height < 0.75f;

    const bool coversMostFrame =
        width >= static_cast<float>(logicalWidth) * 0.75f &&
        height >= static_cast<float>(logicalHeight) * 0.75f;
    return !coversMostFrame;
}

inline std::array<float, 2> SafeAreaViewportScale(float targetAspect)
{
    constexpr float nativeAspect = 16.0f / 9.0f;
    if (!std::isfinite(targetAspect) || targetAspect <= 0.0f ||
        std::fabs(targetAspect - nativeAspect) < 0.0001f)
        return {1.0f, 1.0f};

    // Safe-area UI must be fitted after native clip-space clipping. Wider
    // outputs shrink the viewport horizontally; narrower outputs shrink it
    // vertically. This keeps off-screen guest glyphs/icons clipped instead of
    // pulling them back into view by scaling each vertex before clipping.
    if (targetAspect >= nativeAspect)
        return {nativeAspect / targetAspect, 1.0f};
    return {1.0f, targetAspect / nativeAspect};
}

inline std::array<float, 2> SceneAspectScale(float targetAspect)
{
    constexpr float nativeAspect = 16.0f / 9.0f;
    if (!std::isfinite(targetAspect) || targetAspect <= 0.0f ||
        targetAspect <= nativeAspect + 0.0001f)
        return {1.0f, 1.0f};

    // Wider modes need extra horizontal field of view. Narrower modes are kept
    // in native clip space and center-cropped at presentation instead; scaling
    // final clip-space X for those modes separates tiled/multipass draws that
    // overlap correctly at the game's native 16:9 projection.
    return {nativeAspect / targetAspect, 1.0f};
}

struct PresentationSourceCrop
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

inline PresentationSourceCrop ScenePresentationSourceCrop(
    uint32_t sourceWidth, uint32_t sourceHeight, float targetAspect,
    bool preserveNative16x9)
{
    PresentationSourceCrop crop{0, 0, sourceWidth, sourceHeight};
    if (!sourceWidth || !sourceHeight || preserveNative16x9 ||
        !std::isfinite(targetAspect) || targetAspect <= 0.0f)
        return crop;

    const float sourceAspect = static_cast<float>(sourceWidth) /
                               static_cast<float>(sourceHeight);
    if (targetAspect >= sourceAspect - 0.0001f)
        return crop;

    const uint32_t croppedWidth = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround(static_cast<double>(sourceHeight) *
                                          static_cast<double>(targetAspect))),
        1u, sourceWidth);
    crop.x = (sourceWidth - croppedWidth) / 2u;
    crop.width = croppedWidth;
    return crop;
}

inline bool ShouldApplyConfiguredAspect(bool physicalTileReplay,
                                        bool physicalTileContentRecent,
                                        bool logical3DSceneSeen,
                                        bool logical3DSceneDraw)
{
    // Physical EDRAM tiles contain portions of one native 16:9 frame. Applying
    // the host aspect transform independently to each portion duplicates or
    // drops content when the tiles are composed. Once a pure 2D frame has seen
    // physical tile content, its later logical composition passes must also stay
    // native so the completed frame is fitted exactly once at presentation.
    // A real indexed 3D scene owns the configured aspect transform directly.
    if (physicalTileReplay)
        return false;
    if (logical3DSceneSeen)
        return logical3DSceneDraw;
    return !physicalTileContentRecent;
}

inline bool ShouldPreserveNativePresentation(bool physicalTileContentSeen,
                                             bool logical3DSceneSeen)
{
    // Pure 2D and video frames are assembled from physical tiles and then fitted
    // once at presentation. A collapsed logical 3D scene already received the
    // configured aspect transform and must continue to fill the selected mode.
    return physicalTileContentSeen && !logical3DSceneSeen;
}

inline bool ShouldUseLogicalFirstTile(bool collapseEnabled, bool firstSceneTile,
                                      uint64_t currentFrame,
                                      uint64_t lastIndexedSceneFrame)
{
    // Only a real indexed 3D scene arms tile collapse. Predicated 2D boot and
    // frontend passes use the same 0/416/832 windows, but every window contains
    // a physical portion that must remain physical. Promoting tile 0 alone
    // produces one complete copy followed by fragments from tiles 1 and 2.
    return collapseEnabled && firstSceneTile && lastIndexedSceneFrame != 0 &&
           currentFrame <= lastIndexedSceneFrame + 1;
}

} // namespace mojorecomp::gpu
