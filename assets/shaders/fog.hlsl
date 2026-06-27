// M1 froxel volumetric fog (replaces the M1.0 analytic distance fog). A 3D froxel
// volume covers the view frustum (fogDim X*Y screen tiles, Z log-distributed depth
// slices to fogFar). CSInject evaluates per-froxel in-scattered light (sun with a
// Henyey-Greenstein phase + ambient) and extinction; CSIntegrate marches each
// froxel column front-to-back accumulating scattered light + transmittance. The
// resolve samples the integrated volume trilinearly and composites:
//   final = scene * transmittance + scatteredLight.

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
    row_major float4x4 viewProjNoJitter;
    row_major float4x4 prevViewProjNoJitter;
    row_major float4x4 viewProjCamNoJitter;
    row_major float4x4 prevViewProjCamNoJitter;
    uint clusterDimX, clusterDimY, clusterDimZ, clusterMaxPerCell;
    float zFar; uint fogDimX, fogDimY, fogDimZ;
    float fogFar; float3 _fpad;
};

struct Push {
    uint frameCB;
    uint srcOrDst;   // inject: dst volume UAV.  integrate: src (inject) SRV.
    uint dst;        // integrate: dst (integrated) UAV.  inject: unused.
    uint lights;     // inject: point-light buffer SRV. integrate: unused.
};
ConstantBuffer<Push> pc : register(b0);

static const float PI = 3.14159265359;

struct LightData {
    float3 position; float intensity;
    float3 color;    float radius;
};

float sliceViewZ(uint z, uint VZ, float nearZ, float fogFar) {
    return nearZ * pow(fogFar / nearZ, float(z) / float(VZ));
}

float3 unproject(float2 uv, float ndcZ, row_major float4x4 invVP) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, ndcZ, 1.0), invVP);
    return wp.xyz / max(wp.w, 1e-6);
}

// Inject: per-froxel scattered light + extinction.
[numthreads(4, 4, 4)]
void CSInject(uint3 tid : SV_DispatchThreadID) {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    uint3 dim = uint3(f.fogDimX, f.fogDimY, f.fogDimZ);
    if (any(tid >= dim)) return;
    RWTexture3D<float4> dst = ResourceDescriptorHeap[pc.srcOrDst];

    float2 uv = (float2(tid.xy) + 0.5) / float2(dim.xy);
    float viewZ = sliceViewZ(tid.z, dim.z, f.nearZ, f.fogFar) *
                  pow(f.fogFar / f.nearZ, 0.5 / float(dim.z));   // slice centre
    float ndcZ = f.nearZ / viewZ;
    float3 world = unproject(uv, ndcZ, f.invViewProj);

    float baseExtinction = max(f.fogDensity, 1e-4);
    // Shape the volume so the haze has a little body: denser near the floor, with
    // very low-frequency drift to keep large rooms from reading as a flat overlay.
    float floorHaze = 1.0 + 0.32 * exp(-max(world.y, 0.0) * 0.58);
    float drift = sin(world.x * 0.37 + world.z * 0.21 + world.y * 0.53) *
                  sin(world.x * -0.19 + world.z * 0.43);
    float detail = clamp(0.96 + drift * 0.14, 0.80, 1.16);
    float extinction = baseExtinction * floorHaze * detail;
    float3 toFroxel = normalize(world - f.cameraPos);
    float cosT = dot(toFroxel, normalize(-f.sunDir));
    const float g = 0.48;
    float hg = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * cosT, 1e-4), 1.5));
    float3 sunScatter = f.sunColor * f.sunIntensity * hg;
    float3 ambient = f.fogColor * (f.ambient + 0.070) + f.sunColor * (f.ambient * 0.026);

    float3 localScatter = float3(0, 0, 0);
    if (f.lightCount > 0) {
        StructuredBuffer<LightData> lights = ResourceDescriptorHeap[pc.lights];
        const uint count = min(f.lightCount, 48u);
        [loop] for (uint i = 0; i < count; ++i) {
            LightData L = lights[i];
            float3 toL = L.position - world;
            float dist = length(toL);
            float att = saturate(1.0 - dist / max(L.radius, 1e-3));
            att = att * att * (3.0 - 2.0 * att);
            float3 Ldir = toL / max(dist, 1e-4);
            float localPhase = 0.35 + 0.65 * pow(saturate(dot(toFroxel, Ldir) * 0.5 + 0.5), 4.0);
            localScatter += L.color * (L.intensity * att * localPhase * 0.24);
        }
    }

    float3 inScatter = extinction * (sunScatter + ambient + localScatter);

    dst[tid] = float4(inScatter, extinction);
}

// Integrate: march each froxel column front-to-back, accumulating scatter + trans.
[numthreads(8, 8, 1)]
void CSIntegrate(uint3 tid : SV_DispatchThreadID) {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    uint3 dim = uint3(f.fogDimX, f.fogDimY, f.fogDimZ);
    if (tid.x >= dim.x || tid.y >= dim.y) return;
    Texture3D<float4>   src = ResourceDescriptorHeap[pc.srcOrDst];
    RWTexture3D<float4> dst = ResourceDescriptorHeap[pc.dst];

    float3 accum = float3(0, 0, 0);
    float  trans = 1.0;
    for (uint z = 0; z < dim.z; ++z) {
        float4 s = src[uint3(tid.xy, z)];     // (inScatter, extinction)
        float z0 = sliceViewZ(z, dim.z, f.nearZ, f.fogFar);
        float z1 = sliceViewZ(z + 1, dim.z, f.nearZ, f.fogFar);
        float thickness = max(z1 - z0, 1e-4);
        float sliceTrans = exp(-s.a * thickness);
        float3 sliceScatter = (s.a > 1e-5) ? s.rgb * (1.0 - sliceTrans) / s.a : float3(0, 0, 0);
        accum += trans * sliceScatter;
        trans *= sliceTrans;
        dst[uint3(tid.xy, z)] = float4(accum, trans);
    }
}
