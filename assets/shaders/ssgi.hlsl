// M3 raster-tier screen-space GI (dynamic indirect bounce; the RT tier uses real
// ray-traced multi-bounce GI instead). For each world pixel it samples a few cosine-
// weighted hemisphere directions, marches each against the depth buffer, and where a
// ray lands on a lit on-screen surface it gathers that surface's outgoing radiance
// (the resolved HDR). albedo * average(gathered) is one bounce of diffuse indirect,
// added to the scene. The per-pixel hash keeps a single still deterministic; TAA and
// the bilateral nature of the gather smooth the rest in motion. Screen-space only:
// off-screen surfaces do not contribute (the analytic IBL ambient still covers those).

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
};

struct Push {
    uint hdrIndex;
    uint depthIndex;
    uint normalIndex;
    uint albedoIndex;
    uint materialIndex;
    uint frameCB;
    float strength;
    uint pad0;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s1);
SamplerState sampPoint  : register(s2);

static const float PI = 3.14159265359;
static const float kWorldDepthMax = 0.95;

float3 reconstructWorld(float2 uv, float ndcZ, row_major float4x4 invVP) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, ndcZ, 1.0), invVP);
    return wp.xyz / max(wp.w, 1e-6);
}

uint wangHash(uint s) {
    s = (s ^ 61u) ^ (s >> 16); s *= 9u; s = s ^ (s >> 4); s *= 0x27d4eb2du; s = s ^ (s >> 15);
    return s;
}
float rand01(inout uint state) { state = wangHash(state); return (state & 0xffffffu) / 16777216.0; }

float3 cosineDir(float3 n, inout uint state) {
    float u1 = rand01(state), u2 = rand01(state);
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    float3 up = abs(n.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
    return normalize(local.x * t + local.y * b + local.z * n);
}

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> hdrTex    = ResourceDescriptorHeap[pc.hdrIndex];
    Texture2D<float>  depthTex  = ResourceDescriptorHeap[pc.depthIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[pc.normalIndex];
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[pc.albedoIndex];
    Texture2D<float4> matTex    = ResourceDescriptorHeap[pc.materialIndex];

    float3 hdr = hdrTex.SampleLevel(sampPoint, uv, 0).rgb;
    float d = depthTex.SampleLevel(sampPoint, uv, 0).r;
    float4 mat = matTex.SampleLevel(sampPoint, uv, 0);
    if (d <= 0.0 || mat.a >= 0.5) return float4(hdr, 1.0);     // background / viewmodel

    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    float ndcZ = d / kWorldDepthMax;
    float3 worldPos = reconstructWorld(uv, ndcZ, f.invViewProj);
    float3 n = normalize(normalTex.SampleLevel(sampPoint, uv, 0).rgb * 2.0 - 1.0);
    float3 albedo = albedoTex.SampleLevel(sampPoint, uv, 0).rgb;

    uint state = wangHash(uint(pos.x) * 1973u + uint(pos.y) * 9277u + 7u);

    const int   kSamples = 6;
    const int   kSteps   = 16;
    const float kStepLen = 0.28;
    const float kThick   = 0.55;

    float3 indirect = float3(0, 0, 0);
    for (int s = 0; s < kSamples; ++s) {
        float3 dir = cosineDir(n, state);
        float3 p = worldPos + n * 0.04;
        float3 gathered = float3(0, 0, 0);
        [loop] for (int i = 0; i < kSteps; ++i) {
            p += dir * kStepLen;
            float4 clip = mul(float4(p, 1.0), f.viewProj);
            if (clip.w <= 0.0) break;
            float3 sndc = clip.xyz / clip.w;
            float2 suv = sndc.xy * float2(0.5, -0.5) + 0.5;
            if (any(suv < 0.0) || any(suv > 1.0)) break;
            float sceneD = depthTex.SampleLevel(sampPoint, suv, 0).r;
            if (sceneD <= 0.0) continue;
            float rayDist   = f.nearZ / max(sndc.z, 1e-5);
            float sceneDist = f.nearZ / max(sceneD / kWorldDepthMax, 1e-5);
            if (rayDist > sceneDist && (rayDist - sceneDist) < kThick) {
                gathered = hdrTex.SampleLevel(sampLinear, suv, 0).rgb;   // lit bounce surface
                break;
            }
        }
        indirect += gathered;
    }
    indirect /= float(kSamples);

    // Cosine sampling: the albedo/PI * integral collapses to albedo * mean(Li).
    float ao = mat.r;
    return float4(hdr + albedo * indirect * (pc.strength * ao), 1.0);
}
