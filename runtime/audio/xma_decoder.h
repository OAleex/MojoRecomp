#pragma once

#include <cstdint>

namespace mojorecomp::audio {

// Minimal title-independent XMA2 decoder bridge. The hardware-facing context
// structure remains guest-owned; this object only maintains codec carry state.
bool XmaDecoderAvailable();
void XmaDecoderReset(uint32_t contextId);
void XmaDecoderRelease(uint32_t contextId);
bool XmaDecoderWork(uint32_t contextId, uint32_t contextGuest);

} // namespace mojorecomp::audio
