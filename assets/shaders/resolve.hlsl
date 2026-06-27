// M1 deferred lighting resolve. Samples the gbuffer + depth and writes linear HDR
// (tonemap.hlsl applies exposure + AgX afterwards). M1.0 reproduces the forward
// Lambert look (ambient + sun N.L + emissive); PBR/IBL, clustered local lights,
// shadows, AO, GI and fog layer in here in later M1 steps. Background pixels (no
// geometry: reverse-Z depth == 0) get the scene clear colour.

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
    float4 styleBands;   // W1: x=bandShadow y=bandLit z=softness w=stylize
    float4 skyZenith;    // W4: rgb overhead sky (+ pad)
    float4 skyHorizon;   // W4: rgb horizon sky + .w = skyStrength (0 = current sky)
    float4 styleHatch;   // doc5 hatching: x=strength y=worldScale z=width w=fadeDist (0 = off)
};

struct LightData {
    float3 position; float intensity;
    float3 color;    float radius;
};

struct Push {
    uint frameCB;
    uint albedoIndex;
    uint normalIndex;
    uint materialIndex;
    uint emissiveIndex;
    uint depthIndex;
    uint lightsIndex;
    uint shadowIndex;
    uint aoIndex;
    uint rtIndex;      // RT tier: (shadow, ao) texture from rt_trace.hlsl
    uint rtEnabled;    // 1 -> use rtIndex for sun shadow + AO; 0 -> shadow map + SSAO
    uint giIndex;      // RT tier: one-bounce diffuse GI irradiance
    uint reflIndex;    // RT tier: specular reflection radiance
    uint gridCount;    // clustered culling: per-cluster light count
    uint gridIndices;  // clustered culling: per-cluster light index list
    uint fogVol;       // integrated volumetric fog volume (rgb scatter, a transmittance)
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);
SamplerState sampPoint  : register(s2);

// PCF sun shadow: 1 = lit, 0 = fully shadowed. Standard depth (LESS): a fragment is
// lit where its (biased) light-space depth is in front of the stored occluder.
float sunShadow(float3 worldPos, float3 n, row_major float4x4 sunViewProj, uint shadowIndex) {
    if (shadowIndex == 0) return 1.0;
    float4 sc = mul(float4(worldPos + n * 0.04, 1.0), sunViewProj);   // normal offset vs acne
    float3 p = sc.xyz / sc.w;
    float2 suv = p.xy * float2(0.5, -0.5) + 0.5;
    if (any(suv < 0.0) || any(suv > 1.0) || p.z > 1.0) return 1.0;    // outside frustum -> lit
    const float kShadowTexel = 1.0 / 2048.0;   // must match kShadowMapSize in Engine.cpp
    float refZ = p.z - 0.0018;
    Texture2D<float> sm = ResourceDescriptorHeap[shadowIndex];
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float occ = sm.SampleLevel(sampPoint, suv + float2(x, y) * kShadowTexel, 0).r;
            s += (refZ <= occ) ? 1.0 : 0.0;
        }
    return s / 9.0;
}

static const float PI = 3.14159265359;

// W1 (Neon Ink Brutalism): quantize a diffuse NdotL term into ~3 soft tonal regions
// (deep shadow / midtone / lit). bands = (shadowThreshold, litThreshold, softness,
// stylize). stylize lerps between the smooth value (0) and the banded value (1), so
// stylize = 0 is byte-identical to the prior PBR look.
float bandNoL(float x, float4 bands) {
    float lo = smoothstep(bands.x - bands.z, bands.x + bands.z, x);
    float hi = smoothstep(bands.y - bands.z, bands.y + bands.z, x);
    return lerp(x, (lo + hi) * 0.5, bands.w);
}

// doc 5 (Neon Ink Brutalism): world-anchored ink hatching. Sparse diagonal strokes,
// cross-hatched only in the deepest shadow, living in the shadow + contact bands.
// Anchored to WORLD position (projected onto the surface plane by the dominant normal
// axis) so the strokes stay put as the camera moves -- no swimming. Faded with view
// distance so they never moire or shimmer at range. sh = (strength, scale, width, fade).
// shadowness (0..1) gates the strokes to dark regions. Returns 0..1 ink coverage.
float hatchCoverage(float3 worldPos, float3 n, float viewZ, float4 sh, float shadowness) {
    if (sh.x <= 0.0 || shadowness <= 0.002) return 0.0;
    float distFade = saturate(1.0 - viewZ / max(sh.w, 1e-3));
    if (distFade <= 0.0) return 0.0;
    float3 an = abs(n);
    float2 p = (an.y >= an.x && an.y >= an.z) ? worldPos.xz
             : (an.x >= an.z)                 ? worldPos.zy
                                              : worldPos.xy;
    p *= sh.y;
    // Primary diagonal stroke set. t == 0 at a stroke centre, 1 between strokes; the
    // smoothstep + fwidth gives an antialiased line whose thickness is sh.z (the width).
    float s1 = p.x + p.y;
    float t1 = abs(frac(s1) - 0.5) * 2.0;
    float ink = 1.0 - smoothstep(sh.z, sh.z + fwidth(s1) + 1e-4, t1);
    // Cross strokes fade in only as the shadow deepens (denser ink in the darkest cavities).
    float cross = smoothstep(0.55, 1.0, shadowness);
    if (cross > 0.0) {
        float s2 = p.x - p.y;
        float t2 = abs(frac(s2) - 0.5) * 2.0;
        ink = max(ink, (1.0 - smoothstep(sh.z, sh.z + fwidth(s2) + 1e-4, t2)) * cross);
    }
    return ink * shadowness * distFade * sh.x;
}

// Cook-Torrance GGX BRDF for one light. radiance already folds colour*intensity
// *attenuation*shadow. The DIRECT diffuse term is band-quantized (graphic-novel
// tonal regions); specular keeps a smooth NoL so metal/obsidian stay glossy.
float3 brdf(float3 N, float3 V, float3 L, float3 albedo, float metal, float rough, float3 radiance, float4 bands) {
    float NoL = saturate(dot(N, L));
    if (NoL <= 0.0) return float3(0, 0, 0);
    float3 H = normalize(V + L);
    float NoV = saturate(dot(N, V)) + 1e-4;
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));
    float a = max(rough * rough, 0.002);
    float a2 = a * a;
    float dd = NoH * NoH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * dd * dd + 1e-6);                 // GGX NDF
    float k = a2 * 0.5;
    float G = (NoV / (NoV * (1.0 - k) + k)) * (NoL / (NoL * (1.0 - k) + k));   // Smith
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 F = F0 + (1.0 - F0) * pow(saturate(1.0 - VoH), 5.0);   // Schlick
    float3 spec = (D * G) * F / (4.0 * NoV * NoL + 1e-5);
    float3 kd = (1.0 - F) * (1.0 - metal);
    float NoLq = bandNoL(NoL, bands);                    // banded diffuse, smooth specular
    return (kd * albedo / PI * NoLq + spec * NoL) * radiance;
}

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Background sky for pixels with no geometry: a horizon->zenith gradient (the horizon is the
// clearColor, which is also the fog target, so distant geometry dissolves into it seamlessly)
// plus a sun disc + broad warm glow. Indoors clearColor is near-black so this stays dark; open
// outdoor scenes get a real graded sky. The view ray is unprojected from the pixel like the
// world-pos reconstruction below.
float skyHash(float2 p) {
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}
float skyNoise(float2 p) {
    float2 i = floor(p), fp = frac(p);
    float2 u = fp * fp * (3.0 - 2.0 * fp);
    float a = skyHash(i), b = skyHash(i + float2(1, 0));
    float c = skyHash(i + float2(0, 1)), d = skyHash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float skyFbm(float2 p) {
    float v = 0.0, amp = 0.5;
    [unroll] for (int k = 0; k < 5; ++k) { v += amp * skyNoise(p); p *= 2.02; amp *= 0.5; }
    return v;
}

float3 skyGradient(float2 uv, FrameCB f) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, 0.0001, 1.0), f.invViewProj);   // any depth on the ray works
    float3 dir = normalize(wp.xyz / max(wp.w, 1e-6) - f.cameraPos);
    float h = saturate(dir.y);                                   // 0 at horizon, 1 at zenith
    float3 zenith = f.clearColor * 0.5 + float3(0.03, 0.06, 0.14);
    float3 sky = lerp(f.clearColor, zenith, pow(h, 0.55));
    // W4 (Neon Ink Brutalism): blend toward the illustrated indigo-overhead / peach-horizon
    // palette by skyStrength (skyHorizon.w). skyStrength = 0 keeps the original procedural sky.
    float3 illSky = lerp(f.skyHorizon.rgb, f.skyZenith.rgb, pow(h, 0.55));
    sky = lerp(sky, illSky, f.skyHorizon.w);
    float m = saturate(dot(dir, normalize(-f.sunDir)));          // toward the sun
    sky += f.sunColor * (pow(m, 320.0) * 5.0 + pow(m, 8.0) * 0.25);
    // Soft procedural clouds: project the ray onto a high plane, FBM coverage, fade in above the
    // horizon (so distant geometry/fog meets clear sky), lit by the sky with a warm sun rim.
    if (dir.y > 0.015) {
        float2 cuv = dir.xz / dir.y;
        float n = skyFbm(cuv * 0.9 + 11.0);
        float cov = smoothstep(0.52, 0.82, n) * smoothstep(0.03, 0.34, dir.y);
        float3 cloudCol = lerp(f.clearColor * 1.12, f.sunColor * 1.25 + 0.25, m * 0.45 + 0.25);
        sky = lerp(sky, cloudCol, cov * 0.72);
    }
    return sky;
}

float luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 unitLumaTint(float3 c) {
    return c / max(luma(c), 1e-3);
}

float3 ambientIrradiance(float3 n, FrameCB f) {
    float up = saturate(n.y * 0.5 + 0.5);
    float side = 1.0 - saturate(abs(n.y));
    float3 skyTone = f.clearColor * 2.20 + f.sunColor * 0.18 + float3(0.020, 0.025, 0.035);
    float3 groundTone = f.clearColor * 1.10 + f.sunColor * 0.080 + float3(0.018, 0.014, 0.018);
    float3 sideTone = f.clearColor * 1.55 + f.sunColor * 0.110 + float3(0.018, 0.020, 0.028);
    float3 tone = lerp(lerp(groundTone, skyTone, up), sideTone, side * 0.45);
    float3 tint = lerp(float3(1.0, 1.0, 1.0), unitLumaTint(tone), 0.38);
    return tint * f.ambient;
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    Texture2D<float> depth = ResourceDescriptorHeap[pc.depthIndex];

    float d = depth.SampleLevel(sampLinear, uv, 0).r;   // reverse-Z: 0 == no geometry
    if (d <= 0.0) return float4(skyGradient(uv, f), 1.0);

    Texture2D<float4> albedoTex   = ResourceDescriptorHeap[pc.albedoIndex];
    Texture2D<float4> normalTex   = ResourceDescriptorHeap[pc.normalIndex];
    Texture2D<float4> materialTex = ResourceDescriptorHeap[pc.materialIndex];
    Texture2D<float4> emissiveTex = ResourceDescriptorHeap[pc.emissiveIndex];

    float3 albedo   = albedoTex.SampleLevel(sampLinear, uv, 0).rgb;
    float3 n        = normalize(normalTex.SampleLevel(sampLinear, uv, 0).rgb * 2.0 - 1.0);
    float4 mat      = materialTex.SampleLevel(sampLinear, uv, 0);
    float3 emissive = emissiveTex.SampleLevel(sampLinear, uv, 0).rgb;

    // Sun shadow + ambient occlusion come either from the RT trace (RT tier) or the
    // raster shadow map + SSAO (raster tier).
    float ao = 1.0;
    float rtShadow = 1.0;
    if (pc.rtEnabled != 0) {
        Texture2D<float2> rt = ResourceDescriptorHeap[pc.rtIndex];
        float2 sa = rt.SampleLevel(sampLinear, uv, 0).xy;
        rtShadow = sa.x;
        ao = sa.y;
    } else {
        Texture2D<float> aoTex = ResourceDescriptorHeap[pc.aoIndex];
        ao = aoTex.SampleLevel(sampLinear, uv, 0).r;
    }

    float metal = mat.b;
    float rough = clamp(mat.g, 0.045, 1.0);
    float3 Lsun = normalize(-f.sunDir);
    float ndl = saturate(dot(n, Lsun));

    // The viewmodel (material.a flag) is rendered with viewProjCam + depth-range
    // compression, so world-pos reconstruction, shadows and world fog skip it.
    bool isWorld = mat.a < 0.5;
    float ndcZ = d;
    float3 lit;
    if (isWorld) {
        // Undo the [0, kWorldDepthMax] depth-range compression, reconstruct position.
        const float kWorldDepthMax = 0.95;
        ndcZ = d / kWorldDepthMax;
        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
        float4 wp = mul(float4(ndc, ndcZ, 1.0), f.invViewProj);
        float3 worldPos = wp.xyz / max(wp.w, 1e-6);
        float3 V = normalize(f.cameraPos - worldPos);

        // Indirect lighting: Fresnel-weighted specular + diffuse ambient. On the RT
        // tier these come from ray-traced GI + reflections; on the raster tier from
        // an analytic environment (so metal reads as metal before real probes).
        float NoV = saturate(dot(n, V));
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
        float3 Fr = F0 + (max((1.0 - rough).xxx, F0) - F0) * pow(1.0 - NoV, 5.0);
        float3 kd = (1.0 - Fr) * (1.0 - metal);
        float aoT = ao * mat.r;   // (RT AO or SSAO) * baked ARM occlusion
        if (pc.rtEnabled != 0) {
            Texture2D<float4> giTex   = ResourceDescriptorHeap[pc.giIndex];
            Texture2D<float4> reflTex = ResourceDescriptorHeap[pc.reflIndex];
            float3 gi   = giTex.SampleLevel(sampLinear, uv, 0).rgb;
            float3 refl = reflTex.SampleLevel(sampLinear, uv, 0).rgb;
            // Selective RT reflections (doc 6): glossy/obsidian/wet only; matte stays restrained.
            float reflMask = saturate(1.0 - rough * 1.3);
            lit = kd * albedo * gi * aoT + Fr * refl * reflMask;
        } else {
            // Raster tier: diffuse ambient only. The specular environment reflection
            // is added by the SSR pass (screen-space hits, analytic env fallback).
            lit = kd * albedo * (ambientIrradiance(n, f) * aoT);
        }

        // Direct lighting (Cook-Torrance): shadowed sun + local point lights.
        float shadow = (pc.rtEnabled != 0) ? rtShadow
                                           : sunShadow(worldPos, n, f.sunViewProj, pc.shadowIndex);
        lit += brdf(n, V, Lsun, albedo, metal, rough, f.sunColor * (f.sunIntensity * shadow), f.styleBands);

        // Clustered point lights: find this pixel's froxel, loop only its lights.
        if (f.lightCount > 0) {
            uint CX = f.clusterDimX, CY = f.clusterDimY, CZ = f.clusterDimZ;
            float viewZ = f.nearZ / ndcZ;
            uint slice = (uint)clamp(log(viewZ / f.nearZ) / log(f.zFar / f.nearZ) * float(CZ), 0.0, float(CZ - 1));
            uint tileX = (uint)clamp(uv.x * float(CX), 0.0, float(CX - 1));
            uint tileY = (uint)clamp(uv.y * float(CY), 0.0, float(CY - 1));
            uint cluster = slice * CX * CY + tileY * CX + tileX;

            Texture2D<uint> gridCount   = ResourceDescriptorHeap[pc.gridCount];
            Texture2D<uint> gridIndices = ResourceDescriptorHeap[pc.gridIndices];
            StructuredBuffer<LightData> lights = ResourceDescriptorHeap[pc.lightsIndex];
            uint count = gridCount.Load(int3(cluster, 0, 0));
            for (uint i = 0; i < count; ++i) {
                uint li = gridIndices.Load(int3(cluster, i, 0));
                LightData L = lights[li];
                float3 toL = L.position - worldPos;
                float dist = length(toL);
                float att = saturate(1.0 - dist / max(L.radius, 1e-3));
                att *= att;
                lit += brdf(n, V, toL / max(dist, 1e-4), albedo, metal, rough, L.color * (L.intensity * att), f.styleBands);
            }
        }

        // doc 5 hatching: ink the shadow + contact bands. shadowness follows the SAME
        // 3-band thresholds as the cel shading (deep where the surface is sun-shadowed),
        // widened by crevice contact, so strokes sit exactly where the eye reads shadow.
        // Applied to lit BEFORE emissive so glowing cores are never inked over.
        float hzView = f.nearZ / ndcZ;
        float sunlit = ndl * shadow;
        float sness  = 1.0 - smoothstep(f.styleBands.x, f.styleBands.y, sunlit);
        sness = saturate(max(sness, (1.0 - aoT) * 0.85));
        lit *= 1.0 - hatchCoverage(worldPos, n, hzView, f.styleHatch, sness);
    } else {
        // Viewmodel: simple diffuse + ambient (no world-space view vector). Band the
        // diffuse so the weapon reads as an illustrated object too.
        float ndlq = bandNoL(ndl, f.styleBands);
        lit = albedo * (ambientIrradiance(n, f) + f.sunColor * (f.sunIntensity * ndlq));
    }

    lit += emissive;

    // Volumetric fog: sample the integrated froxel volume at this pixel's depth and
    // composite (scene * transmittance + in-scattered light). Viewmodel excluded.
    if (isWorld) {
        float viewZ = f.nearZ / ndcZ;
        float vz = saturate(log(viewZ / f.nearZ) / log(f.fogFar / f.nearZ));
        Texture3D<float4> fogVol = ResourceDescriptorHeap[pc.fogVol];
        float4 fg = fogVol.SampleLevel(sampLinear, float3(uv, vz), 0);
        lit = lit * fg.a + fg.rgb;
    }
    return float4(lit, 1.0);
}
