// M1 raster-tier screen-space reflections (the reflection fallback when the RT
// tier is unavailable; the RT tier uses ray-traced reflections instead). Reads the
// resolved HDR + gbuffer, reflects the view ray, linearly marches screen space
// against the depth buffer, and adds Fresnel-weighted specular: the hit colour
// where the ray lands on-screen, else an analytic environment. Reverse-Z depth is
// stored with the gbuffer's [0, 0.95] world depth-range compression.

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
    uint materialIndex;
    uint albedoIndex;
    uint frameCB;
    uint pad0, pad1;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s1);   // linear-clamp
SamplerState sampPoint  : register(s2);   // point-wrap

static const float kWorldDepthMax = 0.95;

float3 reconstructWorld(float2 uv, float ndcZ, row_major float4x4 invVP) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, ndcZ, 1.0), invVP);
    return wp.xyz / max(wp.w, 1e-6);
}

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> hdrTex   = ResourceDescriptorHeap[pc.hdrIndex];
    Texture2D<float>  depthTex = ResourceDescriptorHeap[pc.depthIndex];
    Texture2D<float4> normalTex= ResourceDescriptorHeap[pc.normalIndex];
    Texture2D<float4> matTex   = ResourceDescriptorHeap[pc.materialIndex];
    Texture2D<float4> albedoTex= ResourceDescriptorHeap[pc.albedoIndex];

    float3 hdr = hdrTex.SampleLevel(sampPoint, uv, 0).rgb;
    float d = depthTex.SampleLevel(sampPoint, uv, 0).r;
    float4 mat = matTex.SampleLevel(sampPoint, uv, 0);
    if (d <= 0.0 || mat.a >= 0.5) return float4(hdr, 1.0);   // background / viewmodel

    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    float ndcZ = d / kWorldDepthMax;
    float3 worldPos = reconstructWorld(uv, ndcZ, f.invViewProj);
    float3 n = normalize(normalTex.SampleLevel(sampPoint, uv, 0).rgb * 2.0 - 1.0);
    float3 albedo = albedoTex.SampleLevel(sampPoint, uv, 0).rgb;
    float rough = clamp(mat.g, 0.045, 1.0);
    float metal = mat.b;

    float3 V = normalize(f.cameraPos - worldPos);
    float3 R = reflect(-V, n);
    float NoV = saturate(dot(n, V));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 Fr = F0 + (max((1.0 - rough).xxx, F0) - F0) * pow(1.0 - NoV, 5.0);

    // Analytic environment fallback (used when the ray leaves the screen / misses).
    float3 sky = f.sunColor * 0.8 + f.fogColor;
    float3 ground = f.fogColor * 0.6;
    float3 reflection = lerp(ground, sky, saturate(R.y * 0.5 + 0.5));

    // Linear screen-space march along the world-space reflection ray.
    const int   kSteps   = 40;
    const float kStepLen = 0.30;
    const float kThick   = 0.6;
    float3 p = worldPos + n * 0.02;
    [loop] for (int i = 0; i < kSteps; ++i) {
        p += R * kStepLen;
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
            reflection = hdrTex.SampleLevel(sampLinear, suv, 0).rgb;   // screen-space hit
            break;
        }
    }

    // Selective reflections (doc 6): only glossy / obsidian / wet surfaces reflect; matte
    // architecture (rough ~0.75-0.95) is masked out so the world stays restrained.
    float reflMask = saturate(1.0 - rough * 1.3);
    float3 specular = Fr * reflection * reflMask;
    return float4(hdr + specular, 1.0);
}
