#pragma once

#include <cstdint>
#include <vector>

namespace mojorecomp::host {

struct SystemFontAtlas
{
    std::vector<uint8_t> alpha;
    std::vector<float> advances;
};

// Rasterizes an installed Windows UI font into a caller-defined fixed-cell
// atlas. No font file is copied or distributed with MojoRecomp.
bool BuildSystemFontAtlas(uint32_t atlasWidth, uint32_t atlasHeight,
                          uint32_t firstGlyph, uint32_t glyphCount,
                          uint32_t columns, uint32_t cellSize,
                          int fontHeight, SystemFontAtlas& atlas) noexcept;

} // namespace mojorecomp::host
