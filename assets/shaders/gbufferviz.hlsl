// --render-pass debug visualiser for the gbuffer channels (albedo / normal /
// material / emissive). Samples one bindless target and writes it to the LDR
// target. mode 0: raw colour clamped to [0,1] (albedo, encoded normal, packed
// material). mode 1: HDR emissive via simple Reinhard so bright values stay
// visible.

struct Push {
    uint srvIndex;
    uint mode;
    uint _pad0;
    uint _pad1;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> src = ResourceDescriptorHeap[pc.srvIndex];
    float3 c = src.SampleLevel(sampLinear, uv, 0).rgb;
    if (pc.mode == 1) c = c / (c + 1.0);   // Reinhard for HDR emissive
    return float4(saturate(c), 1.0);
}
