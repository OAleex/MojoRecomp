// Synthetic window-coordinate input using the production SharedConstants ABI.
#include "shader_common.h"
struct Output {
    float4 position : SV_Position;
    float4 uv : TEXCOORD0;
};
Output main([[vk::location(0)]] float3 position : POSITION0) {
    Output result;
    result.position = float4(position, 1.0);
    result.position.xy = result.position.xy * g_PosScale + g_PosOffset;
    result.position.xy += g_HalfPixelOffset;
    result.uv = float4(0.5, 0.5, 0.0, 0.0);
    return result;
}
