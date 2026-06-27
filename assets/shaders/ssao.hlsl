// M1 screen-space ambient occlusion (hemisphere kernel) + a box blur to denoise.
// Reads the gbuffer depth + world normal, reconstructs world position, samples a
// cosine-weighted hemisphere oriented by the normal. Folded into the resolve's
// ambient so objects ground into contact darkening (the plan's anti-"floating").
//
// One root-constant layout shared by both passes:
//   AOPass:   a0=frameCB a1=depth a2=normal | e=radius f=intensity g=bias
//   BlurPass: a0=aoSrc                       | e=invW   f=invH

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

struct Push {
    uint  a0; uint a1; uint a2; uint a3;
    float e;  float f;  float g;  float h;
};
ConstantBuffer<Push> pc : register(b0);
SamplerState sampLinear : register(s1);   // linear-clamp

static const float kWorldDepthMax = 0.95;   // matches gbuffer depth-range compression

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float radicalInverse(uint bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}
float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float3 reconstructWorld(float2 uv, float ndcZ, row_major float4x4 invVP) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, ndcZ, 1.0), invVP);
    return wp.xyz / max(wp.w, 1e-6);
}

float4 AOPass(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.a0];
    Texture2D<float> depth = ResourceDescriptorHeap[pc.a1];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[pc.a2];
    const float radius = pc.e, intensity = pc.f, bias = pc.g;

    float d = depth.SampleLevel(sampLinear, uv, 0).r;
    if (d <= 0.0 || d > kWorldDepthMax) return 1.0;   // background or viewmodel: no AO

    float3 P = reconstructWorld(uv, d / kWorldDepthMax, f.invViewProj);
    float3 N = normalize(normalTex.SampleLevel(sampLinear, uv, 0).rgb * 2.0 - 1.0);

    float ang = hash12(pos.xy) * 6.2831853;
    float3 randv = float3(cos(ang), sin(ang), 0.0);
    float3 T = normalize(randv - N * dot(randv, N));
    float3x3 TBN = float3x3(T, cross(N, T), N);

    const int N_SAMPLES = 16;
    float occ = 0.0;
    [loop] for (int i = 0; i < N_SAMPLES; ++i) {
        float u1 = (float(i) + 0.5) / float(N_SAMPLES);
        float phi = 6.2831853 * radicalInverse(uint(i));
        float r = sqrt(u1);
        float3 s = float3(r * cos(phi), r * sin(phi), sqrt(1.0 - u1));   // cosine hemisphere
        s *= lerp(0.15, 1.0, u1 * u1);
        float3 samplePos = P + mul(s, TBN) * radius;

        float4 sc = mul(float4(samplePos, 1.0), f.viewProj);
        float2 suv = (sc.xy / sc.w) * float2(0.5, -0.5) + 0.5;
        if (any(suv < 0.0) || any(suv > 1.0)) continue;

        float sd = depth.SampleLevel(sampLinear, suv, 0).r;
        if (sd <= 0.0 || sd > kWorldDepthMax) continue;
        float3 sceneWorld = reconstructWorld(suv, sd / kWorldDepthMax, f.invViewProj);

        float sampleCamD = length(samplePos - f.cameraPos);
        float sceneCamD = length(sceneWorld - f.cameraPos);
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(distance(P, sceneWorld), 1e-3));
        occ += ((sceneCamD < sampleCamD - bias) ? 1.0 : 0.0) * rangeCheck;
    }
    return saturate(1.0 - (occ / float(N_SAMPLES)) * intensity);
}

// 4x4 box blur to denoise the AO. a0 = AO source SRV, e/f = inverse target size.
float4 BlurPass(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float> src = ResourceDescriptorHeap[pc.a0];
    float2 texel = float2(pc.e, pc.f);
    float sum = 0.0;
    [unroll] for (int y = -2; y <= 1; ++y)
        [unroll] for (int x = -2; x <= 1; ++x)
            sum += src.SampleLevel(sampLinear, uv + float2(x + 0.5, y + 0.5) * texel, 0).r;
    return sum / 16.0;
}
