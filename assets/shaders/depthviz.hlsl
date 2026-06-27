// capturePass("depth") helper: visualize the reverse-Z depth target as grayscale
// so --render-pass depth writes a readable image. Near is bright, far is dark.

struct Push {
    uint  depthIndex;   // bindless SRV of the depth target (R32_FLOAT view)
    float nearZ;
    uint  _pad0;
    uint  _pad1;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float> depth = ResourceDescriptorHeap[pc.depthIndex];
    float d = depth.SampleLevel(sampLinear, uv, 0).r;   // reverse-Z: near=1, far=0
    if (d <= 0.0) return float4(0, 0, 0, 1);            // no geometry: far/black
    // Recover view-space distance from infinite-reverse-Z: z = nearZ / d, then a
    // soft curve so a range of depths is visible (near bright, far dark).
    float viewZ = pc.nearZ / d;
    float g = saturate(1.0 - viewZ / (viewZ + 8.0));
    return float4(g, g, g, 1.0);
}
