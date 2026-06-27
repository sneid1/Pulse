// FXAA (Timothy Lottes' classic "quick" variant) on the tonemapped LDR image.
// Cheap single-pass edge anti-aliasing - removes the jaggies that read as "unfinished"
// without TAA's history/motion-vector machinery. Runs before the UI so the HUD
// stays crisp.

struct Push {
    uint  srcIndex;
    float invW;
    float invH;
    uint  pad;
};
ConstantBuffer<Push> pc : register(b0);
SamplerState sampLinear : register(s1);   // linear-clamp

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> tex = ResourceDescriptorHeap[pc.srcIndex];
    float2 px = float2(pc.invW, pc.invH);

    float3 cM = tex.SampleLevel(sampLinear, uv, 0).rgb;
    float lM  = luma(cM);
    float lNW = luma(tex.SampleLevel(sampLinear, uv + float2(-px.x, -px.y), 0).rgb);
    float lNE = luma(tex.SampleLevel(sampLinear, uv + float2( px.x, -px.y), 0).rgb);
    float lSW = luma(tex.SampleLevel(sampLinear, uv + float2(-px.x,  px.y), 0).rgb);
    float lSE = luma(tex.SampleLevel(sampLinear, uv + float2( px.x,  px.y), 0).rgb);

    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < lMax * 0.125 + 0.0312) return float4(cM, 1.0);   // not an edge

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 * 0.125), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8.0, 8.0) * px;

    float3 rgbA = 0.5 * (tex.SampleLevel(sampLinear, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb
                       + tex.SampleLevel(sampLinear, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (tex.SampleLevel(sampLinear, uv + dir * -0.5, 0).rgb
                                     + tex.SampleLevel(sampLinear, uv + dir *  0.5, 0).rgb);
    float lB = luma(rgbB);
    return float4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
