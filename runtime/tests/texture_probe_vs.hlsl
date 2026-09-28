struct Output {
    float4 position : SV_Position;
    float4 uv : TEXCOORD0;
};
Output main(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    Output result;
    result.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    result.uv = float4(uv, 0.0, 0.0);
    return result;
}
