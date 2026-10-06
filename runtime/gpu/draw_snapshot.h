#pragma once

#include <cstdint>
#include <memory>

#include "pm4.h"

namespace mojorecomp::gpu {

class GuestMemorySnapshot;

std::unique_ptr<GuestMemorySnapshot> BuildDrawSnapshot(
    uint8_t* base, const Pm4Draw& draw, const uint32_t* registers,
    uint64_t vertexShaderHash, uint64_t pixelShaderHash);

uint64_t DrawSnapshotBuildFailureCount() noexcept;
void DrawSnapshotCacheShutdown();

} // namespace mojorecomp::gpu
