#pragma once

#include <array>
#include <cstdint>

namespace mojorecomp::gpu {

inline constexpr std::array<uint32_t, 3> RectangleStripSources(
    uint32_t other, uint32_t diagonalA, uint32_t diagonalB)
{
    // The first three vertices of the host strip must remain a cyclic
    // permutation of guest vertices 0,1,2. The reconstructed fourth corner is
    // unchanged by swapping the diagonal endpoints, but front/back stencil
    // classification is not.
    if ((other + 1u) % 3u != diagonalA)
    {
        const uint32_t swap = diagonalA;
        diagonalA = diagonalB;
        diagonalB = swap;
    }
    return {other, diagonalA, diagonalB};
}

inline constexpr bool UsesSeparateBackfaceStencil(bool primitivePolygonal,
                                                  uint32_t depthControl)
{
    // Xenos applies the separate back-face stencil state only to polygonal
    // primitives. Point, line, rectangle and other non-polygonal primitives
    // use the front stencil state even when BACKFACE_ENABLE is set.
    return primitivePolygonal && ((depthControl >> 7) & 1u) != 0;
}

} // namespace mojorecomp::gpu
