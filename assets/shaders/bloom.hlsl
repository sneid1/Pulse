// M1 bloom: soft-threshold prefilter + downsample, then a separable Gaussian blur
// at quarter resolution. The result is added back in tonemap.hlsl so emissive
// accents and bright highlights bleed light (the dark arena's glow). A full mip
// chain is a later quality refinement; this is the reliable first pass.
//
// One root-constant layout shared by both passes (fields reinterpreted):
//   DownPS: a = threshold, b = knee
//   BlurPS: a = dirX,      b = dirY   (blur direction in texels)

struct BloomPush {
    uint  srcIndex;
    float a;
    float b;
    uint  _p0;
    float invSrcX;
    float invSrcY;
    uint  _p1;
    uint  _p2;
};
ConstantBuffer<BloomPush> pc : register(b0);
SamplerState sampLinear : register(s0);

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// 4-tap bilinear box downsample + Karis-style soft threshold prefilter.
float4 DownPS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> src = ResourceDescriptorHeap[pc.srcIndex];
    float2 o = float2(pc.invSrcX, pc.invSrcY);
    float3 c = src.SampleLevel(sampLinear, uv + float2(-o.x, -o.y), 0).rgb;
    c += src.SampleLevel(sampLinear, uv + float2( o.x, -o.y), 0).rgb;
    c += src.SampleLevel(sampLinear, uv + float2(-o.x,  o.y), 0).rgb;
    c += src.SampleLevel(sampLinear, uv + float2( o.x,  o.y), 0).rgb;
    c *= 0.25;

    float threshold = pc.a;
    float knee = pc.b;
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contrib = max(soft, br - threshold) / max(br, 1e-4);
    return float4(c * contrib, 1.0);
}

// 9-tap separable Gaussian (binomial weights); direction (a,b) in texels.
float4 BlurPS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> src = ResourceDescriptorHeap[pc.srcIndex];
    float2 step = float2(pc.a * pc.invSrcX, pc.b * pc.invSrcY);
    const float w0 = 0.227027, w1 = 0.1945946, w2 = 0.1216216, w3 = 0.054054, w4 = 0.016216;
    float3 c = src.SampleLevel(sampLinear, uv, 0).rgb * w0;
    c += src.SampleLevel(sampLinear, uv + step * 1.0, 0).rgb * w1;
    c += src.SampleLevel(sampLinear, uv - step * 1.0, 0).rgb * w1;
    c += src.SampleLevel(sampLinear, uv + step * 2.0, 0).rgb * w2;
    c += src.SampleLevel(sampLinear, uv - step * 2.0, 0).rgb * w2;
    c += src.SampleLevel(sampLinear, uv + step * 3.0, 0).rgb * w3;
    c += src.SampleLevel(sampLinear, uv - step * 3.0, 0).rgb * w3;
    c += src.SampleLevel(sampLinear, uv + step * 4.0, 0).rgb * w4;
    c += src.SampleLevel(sampLinear, uv - step * 4.0, 0).rgb * w4;
    return float4(c, 1.0);
}
