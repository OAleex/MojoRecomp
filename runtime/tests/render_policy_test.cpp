#include <cstdio>

#include "../gpu/primitive_utils.h"
#include "../gpu/render_policy.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using mojorecomp::gpu::RectangleStripSources;
    using mojorecomp::gpu::UsesSeparateBackfaceStencil;
    using mojorecomp::gpu::ShouldApplyConfiguredAspect;
    using mojorecomp::gpu::ShouldEnableAnisotropy;
    using mojorecomp::gpu::HasRecentPhysicalTileContent;
    using mojorecomp::gpu::SceneAspectScale;
    using mojorecomp::gpu::ScenePresentationSourceCrop;
    using mojorecomp::gpu::SafeAreaViewportScale;
    using mojorecomp::gpu::ShouldTreatAsSafeAreaOverlay;
    using mojorecomp::gpu::ShouldPreserveNativePresentation;
    using mojorecomp::gpu::ShouldUseLogicalFirstTile;
    using mojorecomp::gpu::TextureSource;
    using mojorecomp::gpu::EdramDepthTransferMode;
    using mojorecomp::gpu::SelectEdramDepthTransferMode;
    using mojorecomp::gpu::ApplyStencilBitPlane;

    if (SelectEdramDepthTransferMode(true) !=
            EdramDepthTransferMode::ShaderStencilExport ||
        SelectEdramDepthTransferMode(false) !=
            EdramDepthTransferMode::FixedFunctionBitPlanes)
        return Fail("EDRAM depth transfer did not select the portable stencil fallback");

    // The fallback clears the destination stencil, then sets each source bit
    // with fixed-function REPLACE and a one-bit write mask. Exhaust all byte
    // values so stale destination bits or an incorrect reference cannot hide.
    for (uint32_t source = 0; source <= 0xFFu; ++source)
    {
        uint8_t reconstructed = 0;
        for (uint32_t bit = 0; bit < 8; ++bit)
            reconstructed = ApplyStencilBitPlane(reconstructed,
                                                  static_cast<uint8_t>(source), bit);
        if (reconstructed != source)
            return Fail("fixed-function EDRAM stencil reconstruction lost bits");
    }

    // Xenos RECTLIST front/back classification follows the cyclic order of its
    // three guest vertices. Host strip expansion must preserve that winding for
    // every possible choice of the longest (diagonal) edge.
    if (RectangleStripSources(2, 0, 1) != std::array<uint32_t, 3>{2, 0, 1} ||
        RectangleStripSources(0, 1, 2) != std::array<uint32_t, 3>{0, 1, 2} ||
        RectangleStripSources(1, 0, 2) != std::array<uint32_t, 3>{1, 2, 0})
        return Fail("rectangle-list expansion reversed guest winding");

    // BACKFACE_ENABLE is ignored for non-polygonal Xenos primitives. This is
    // especially important for RECTLIST passes: their host triangle expansion
    // must not accidentally select the guest back-face stencil function.
    constexpr uint32_t kBackfaceEnable = 1u << 7;
    if (UsesSeparateBackfaceStencil(false, kBackfaceEnable) ||
        !UsesSeparateBackfaceStencil(true, kBackfaceEnable) ||
        UsesSeparateBackfaceStencil(true, 0))
        return Fail("back-face stencil state escaped polygonal primitives");

    mojorecomp::texture_abi::Fetch2D fetch{};
    fetch.mipMin = 0;
    fetch.mipMax = 0;
    fetch.mipFilter = 2;
    if (ShouldEnableAnisotropy(fetch, TextureSource::ResolveSnapshot, 16, 16.0f))
        return Fail("anisotropy was enabled for a resolved render target");
    if (ShouldEnableAnisotropy(fetch, TextureSource::GuestTexture, 16, 16.0f))
        return Fail("anisotropy was enabled for a one-level guest texture");

    fetch.mipMax = 6;
    fetch.mipFilter = 1;
    if (!ShouldEnableAnisotropy(fetch, TextureSource::GuestTexture, 8, 8.0f))
        return Fail("anisotropy was not enabled for a mipmapped guest texture");
    if (ShouldEnableAnisotropy(fetch, TextureSource::GuestTexture, 1, 16.0f))
        return Fail("guest default filtering was overridden");

    if (ShouldApplyConfiguredAspect(true, true, false, false))
        return Fail("aspect correction was applied inside a physical Bink tile");
    if (!ShouldApplyConfiguredAspect(false, false, false, false))
        return Fail("a logical Bink draw lost configured aspect correction");
    if (ShouldApplyConfiguredAspect(false, true, false, false))
        return Fail("aspect correction was applied again by a 2D fullscreen compositor");
    if (!ShouldApplyConfiguredAspect(false, true, true, true))
        return Fail("a logical 3D scene draw lost configured aspect correction");
    if (ShouldApplyConfiguredAspect(false, true, true, false))
        return Fail("aspect correction was applied again by a 3D frame compositor");
    const auto wideUi = SafeAreaViewportScale(21.0f / 9.0f);
    if (std::fabs(wideUi[0] - (16.0f / 21.0f)) > 0.0001f ||
        std::fabs(wideUi[1] - 1.0f) > 0.0001f)
        return Fail("21:9 HUD safe-area viewport scale is incorrect");
    const auto ratio16x10Ui = SafeAreaViewportScale(16.0f / 10.0f);
    if (std::fabs(ratio16x10Ui[0] - 1.0f) > 0.0001f ||
        std::fabs(ratio16x10Ui[1] - 0.9f) > 0.0001f)
        return Fail("16:10 HUD safe-area viewport scale is incorrect");
    const auto ratio4x3Ui = SafeAreaViewportScale(4.0f / 3.0f);
    if (std::fabs(ratio4x3Ui[0] - 1.0f) > 0.0001f ||
        std::fabs(ratio4x3Ui[1] - 0.75f) > 0.0001f)
        return Fail("4:3 HUD safe-area viewport scale is incorrect");
    const auto nativeScene = SceneAspectScale(16.0f / 9.0f);
    if (std::fabs(nativeScene[0] - 1.0f) > 0.0001f ||
        std::fabs(nativeScene[1] - 1.0f) > 0.0001f)
        return Fail("16:9 scene aspect scale is not neutral");
    const auto ratio16x10Scene = SceneAspectScale(16.0f / 10.0f);
    if (std::fabs(ratio16x10Scene[0] - 1.0f) > 0.0001f ||
        std::fabs(ratio16x10Scene[1] - 1.0f) > 0.0001f)
        return Fail("16:10 scene was scaled before center crop");
    const auto ratio4x3Scene = SceneAspectScale(4.0f / 3.0f);
    if (std::fabs(ratio4x3Scene[0] - 1.0f) > 0.0001f ||
        std::fabs(ratio4x3Scene[1] - 1.0f) > 0.0001f)
        return Fail("4:3 scene was scaled before center crop");
    const auto wideScene = SceneAspectScale(21.0f / 9.0f);
    if (std::fabs(wideScene[0] - (16.0f / 21.0f)) > 0.0001f ||
        std::fabs(wideScene[1] - 1.0f) > 0.0001f)
        return Fail("21:9 scene horizontal FOV scale is incorrect");

    const auto crop16x10 = ScenePresentationSourceCrop(
        1280, 720, 16.0f / 10.0f, false);
    if (crop16x10.x != 64 || crop16x10.y != 0 ||
        crop16x10.width != 1152 || crop16x10.height != 720)
        return Fail("16:10 scene presentation crop is incorrect");
    const auto crop4x3 = ScenePresentationSourceCrop(
        1280, 720, 4.0f / 3.0f, false);
    if (crop4x3.x != 160 || crop4x3.y != 0 ||
        crop4x3.width != 960 || crop4x3.height != 720)
        return Fail("4:3 scene presentation crop is incorrect");
    const auto cropWide = ScenePresentationSourceCrop(
        1280, 720, 21.0f / 9.0f, false);
    if (cropWide.x != 0 || cropWide.width != 1280 || cropWide.height != 720)
        return Fail("21:9 scene was incorrectly source-cropped");
    const auto cropPreserved2D = ScenePresentationSourceCrop(
        1280, 720, 4.0f / 3.0f, true);
    if (cropPreserved2D.x != 0 || cropPreserved2D.width != 1280 ||
        cropPreserved2D.height != 720)
        return Fail("native 2D presentation was incorrectly source-cropped");
    if (ShouldTreatAsSafeAreaOverlay(0.0f, 0.0f, 1.0f, 1.0f, 1280, 720))
        return Fail("a normalized fullscreen compositor was mistaken for HUD");
    if (!ShouldTreatAsSafeAreaOverlay(0.0f, 0.0f, 0.1f, 0.1f, 1280, 720))
        return Fail("a small normalized HUD quad was mistaken for a compositor");
    if (ShouldTreatAsSafeAreaOverlay(0.0f, 0.0f, 1280.0f, 720.0f, 1280, 720))
        return Fail("a full-surface compositor was mistaken for HUD");
    if (!ShouldTreatAsSafeAreaOverlay(-140.0f, 349.0f, 780.0f, 477.0f, 1280, 720))
        return Fail("a screen-space HUD ornament was not kept in the safe area");
    if (!ShouldTreatAsSafeAreaOverlay(0.0f, 0.0f, 252.0f, 45.0f, 1280, 720))
        return Fail("screen-space HUD text was not kept in the safe area");
    if (!HasRecentPhysicalTileContent(35, 35))
        return Fail("physical tile content was not recognized in its own frame");
    if (!HasRecentPhysicalTileContent(35, 34))
        return Fail("the next physical 2D frame lost its early-frame classification");
    if (HasRecentPhysicalTileContent(35, 33))
        return Fail("stale physical 2D state leaked beyond one follow-up frame");
    if (HasRecentPhysicalTileContent(35, 0))
        return Fail("an uninitialized physical frame was treated as recent");
    if (!ShouldPreserveNativePresentation(true, false))
        return Fail("a physical 2D boot frame was not fitted once at presentation");
    if (ShouldPreserveNativePresentation(true, true))
        return Fail("a logical 3D frame was incorrectly forced back to 16:9");
    if (ShouldPreserveNativePresentation(false, false))
        return Fail("an ordinary logical frame was incorrectly forced to 16:9");
    if (ShouldUseLogicalFirstTile(true, true, 50, 0))
        return Fail("a predicated 2D boot tile was promoted without a 3D scene");
    if (!ShouldUseLogicalFirstTile(true, true, 50, 50))
        return Fail("the first tile of an indexed 3D scene was not promoted");
    if (!ShouldUseLogicalFirstTile(true, true, 51, 50))
        return Fail("the immediate follow-up 3D tile pass lost logical coordinates");
    if (ShouldUseLogicalFirstTile(true, true, 52, 50))
        return Fail("stale 3D state promoted an unrelated later tile");
    if (ShouldUseLogicalFirstTile(true, false, 50, 50))
        return Fail("a non-first 3D tile was promoted");

    std::puts("OK: filtering and tiled-frame renderer policies are safe.");
    return 0;
}
