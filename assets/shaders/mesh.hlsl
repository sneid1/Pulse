// M0 forward textured-lit mesh: bindless vertex pulling (StructuredBuffer indexed
// by SV_VertexID), a bindless camera CBV, and a bindless albedo texture, the full
// bindless draw shape the engine uses. Root constants carry only heap indices.

struct Vertex {
    float3 pos;
    float3 nrm;
    float2 uv;
    float4 color;
};

struct Camera {
    row_major float4x4 viewProj;
    row_major float4x4 model;
    float3 lightDir;
    float  ambient;
};

struct Push {
    uint camIndex;
    uint vbIndex;
    uint texIndex;
    uint pad;
};
ConstantBuffer<Push> pc : register(b0);     // root constants

SamplerState sampLinear : register(s0);

struct VSOut {
    float4 pos   : SV_Position;
    float3 nrm   : NORMAL;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    ConstantBuffer<Camera> cam = ResourceDescriptorHeap[pc.camIndex];
    StructuredBuffer<Vertex> verts = ResourceDescriptorHeap[pc.vbIndex];
    Vertex v = verts[vid];

    float4 world = mul(float4(v.pos, 1.0), cam.model);
    VSOut o;
    o.pos = mul(world, cam.viewProj);
    o.nrm = mul(float4(v.nrm, 0.0), cam.model).xyz;
    o.uv = v.uv;
    o.color = v.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    ConstantBuffer<Camera> cam = ResourceDescriptorHeap[pc.camIndex];
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[pc.texIndex];
    float3 albedo = albedoTex.Sample(sampLinear, i.uv).rgb * i.color.rgb;

    float3 n = normalize(i.nrm);
    float ndl = saturate(dot(n, normalize(cam.lightDir)));
    float3 lit = albedo * (cam.ambient + (1.0 - cam.ambient) * ndl);
    return float4(lit, 1.0);
}
