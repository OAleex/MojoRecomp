#pragma once

namespace mojorecomp::gpu {

// Writes one JSON object describing the Vulkan adapter selected by the runtime.
// The probe creates only a hidden native surface; it never boots the guest title.
int RunHardwareProbeJson();

} // namespace mojorecomp::gpu
