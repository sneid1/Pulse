// M0 raster smoke: a vertex-coloured triangle generated from SV_VertexID, no
// buffers, no transform. Proves ShaderCompiler -> root signature -> PSO -> draw
// -> RTV -> readback end to end before real meshes land.

struct VSOut {
    float4 pos : SV_Position;
    float3 col : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    // CCW winding (FrontCounterClockwise = TRUE).
    const float2 p[3] = { float2(0.0, 0.62), float2(-0.6, -0.5), float2(0.6, -0.5) };
    const float3 c[3] = { float3(1.0, 0.25, 0.2), float3(0.25, 1.0, 0.35), float3(0.3, 0.45, 1.0) };
    VSOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.col = c[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    return float4(i.col, 1.0);
}
