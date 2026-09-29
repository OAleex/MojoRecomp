#pragma once

#include <cstdint>

namespace mojorecomp::audio {

// Minimal title-independent XMA2 decoder bridge. The hardware-facing context
// structure remains guest-owned; this object only maintains codec carry state.
bool XmaDecoderAvailable();
void XmaDecoderReset(uint32_t contextId);
void XmaDecoderRelease(uint32_t contextId);
bool XmaDecoderWork(uint32_t contextId, uint32_t contextGuest);
// Observe the final six-channel render mix. Subtitle timing is anchored by
// matching a short PCM fingerprint decoded from the active RSD against the
// corresponding center-channel audio in the final guest mix.
void XmaDecoderObserveRenderCenter(const float* samples, uint32_t sampleCount,
                                   uint32_t sampleRate);

} // namespace mojorecomp::audio
