// M1 sun shadow map: depth-only render of world geometry from the sun's
// orthographic point of view. No pixel shader (depth-only). The resolve samples
// this with PCF to shadow the directional sun term. Camera-space viewmodels are
// excluded by the engine (not part of the world).

struct Vertex {
    float3 pos;
    float3 nrm;
    float4 tangent;
    float2 uv0;
    float2 uv1;
    float4 color;
};

struct FrameCB {
    row_major float4x4 viewProj;
    row_major float4x4 viewProjCam;
    float3 sunDir;   float sunIntensity;
    float3 sunColor; float ambient;
    float  exposure; float3 clearColor;
    float3 fogColor; float fogDensity;
    float  nearZ;    float3 _pad;
    row_major float4x4 invViewProj;
    float3 cameraPos; uint lightCount;
    row_major float4x4 sunViewProj;
};

// Must match the engine InstanceData stride (192B); the VS only reads model, but a
// StructuredBuffer stride mismatch is a validation error, so keep the full layout.
struct InstanceData {
    row_major float4x4 model;
    row_major float4x4 prevModel;
    float4 baseColorFactor;
    uint   baseTex;
    float  emissive;
    float  metallic;
    float  roughness;
    uint   cameraSpace;
    uint   normalTex;
    uint   ormTex;
    float  uvScale;
    uint   vbIndex;
    uint   ibIndex;
    float  metalScale;
    float  roughBoost;
    float3 rimColor;   // matches the shared InstanceData stride (unused in this pass)
    float  rimPower;
    uint   emissiveTex;
    float  emissiveTexStrength;
};

struct Push {
    uint frameCB;
    uint instanceSB;
    uint instanceIndex;
    uint vbIndex;
};
ConstantBuffer<Push> pc : register(b0);

float4 VSMain(uint vid : SV_VertexID) : SV_Position {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    StructuredBuffer<InstanceData> insts = ResourceDescriptorHeap[pc.instanceSB];
    StructuredBuffer<Vertex> verts = ResourceDescriptorHeap[pc.vbIndex];

    InstanceData inst = insts[pc.instanceIndex];
    float4 world = mul(float4(verts[vid].pos, 1.0), inst.model);
    return mul(world, f.sunViewProj);
}
