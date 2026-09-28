#pragma once

#include <cstdint>

// Connects PM4 callbacks to renderer diagnostics and the optional Vulkan presenter.
void RendererProbe_Init();

uint64_t RendererProbe_DrawCount();
uint64_t RendererProbe_SwapCount();
uint64_t RendererProbe_DistinctVertexShaders();
uint64_t RendererProbe_DistinctPixelShaders();
