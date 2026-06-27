// M1 RT tier: inline-RayQuery (DXR 1.1, no shader tables) sun shadows, ambient
// occlusion, one-bounce diffuse GI, and specular reflections. One compute thread
// per pixel reconstructs the world surface from the gbuffer and traces:
//   * a hard sun-shadow ray (any-hit)            -> shadow
//   * N cosine-hemisphere rays (closest-hit)     -> AO (near hits) + GI (shaded hits)
//   * one reflection ray (closest-hit)           -> specular radiance
// Hit shading is self-contained: the closest hit's instance/primitive/barycentrics
// fetch the triangle from the bindless vertex+index buffers and evaluate sun +
// emissive + ambient. All rays are jittered per frame; TAA accumulates the noise
// away. Outputs feed the deferred resolve. The raster tier stays the fallback.

struct Vertex {
    float3 pos;
    float3 nrm;
    float4 tangent;
    float2 uv0;
    float2 uv1;
    float4 color;
};

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
    uint frameCB;
    uint depthIndex;
    uint normalIndex;
    uint materialIndex;
    uint albedoIndex;
    uint tlasIndex;
    uint instanceSB;
    uint shadowAoOut;   // RWTexture2D<float2> (shadow, ao)
    uint giOut;         // RWTexture2D<float4> (rgb diffuse GI irradiance)
    uint reflOut;       // RWTexture2D<float4> (rgb specular reflection radiance)
    uint width;
    uint height;
    uint frameIndex;
    uint rayCount;      // hemisphere rays for AO + GI
    float aoRadius;
    float giRange;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

static const float PI = 3.14159265359;
static const float kWorldDepthMax = 0.95;

uint wangHash(uint s) {
    s = (s ^ 61u) ^ (s >> 16); s *= 9u; s = s ^ (s >> 4); s *= 0x27d4eb2du; s = s ^ (s >> 15);
    return s;
}
float rand01(inout uint state) { state = wangHash(state); return (state & 0xffffffu) / 16777216.0; }

float3 reconstructWorld(float2 uv, float ndcZ, row_major float4x4 invVP) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, ndcZ, 1.0), invVP);
    return wp.xyz / max(wp.w, 1e-6);
}

// A simple analytic sky/environment so rays that escape the arena still return
// plausible incoming light (matches the resolve's analytic env).
float3 skyColor(float3 dir, FrameCB f) {
    float3 sky = f.sunColor * 0.8 + f.fogColor;
    float3 ground = f.fogColor * 0.6;
    return lerp(ground, sky, saturate(dir.y * 0.5 + 0.5));
}

// Trace a hard sun-shadow ray from a hit point. Returns 0 (occluded) or 1 (lit).
float traceSunShadow(RaytracingAccelerationStructure scene, float3 p, float3 n, float3 sunDir) {
    RayDesc ray;
    ray.Origin = p + n * 0.02; ray.Direction = normalize(-sunDir);
    ray.TMin = 0.01; ray.TMax = 120.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// A surface fetched from a committed RayQuery hit: world position, geometric normal
// (flipped to face the ray), albedo, and emissive amount.
struct Surface { float3 pos; float3 n; float3 albedo; float emissiveAmt; };

Surface fetchSurface(uint instanceID, uint primIndex, float2 bary, float3 rayDir) {
    StructuredBuffer<InstanceData> insts = ResourceDescriptorHeap[pc.instanceSB];
    InstanceData inst = insts[instanceID];
    // The hit instance varies per lane, so its bindless indices are non-uniform.
    StructuredBuffer<Vertex> verts = ResourceDescriptorHeap[NonUniformResourceIndex(inst.vbIndex)];
    StructuredBuffer<uint>   idx   = ResourceDescriptorHeap[NonUniformResourceIndex(inst.ibIndex)];

    uint i0 = idx[primIndex * 3 + 0];
    uint i1 = idx[primIndex * 3 + 1];
    uint i2 = idx[primIndex * 3 + 2];
    Vertex v0 = verts[i0], v1 = verts[i1], v2 = verts[i2];
    float w0 = 1.0 - bary.x - bary.y;

    Surface s;
    float3 nObj = v0.nrm * w0 + v1.nrm * bary.x + v2.nrm * bary.y;
    s.n = normalize(mul(float4(nObj, 0.0), inst.model).xyz);
    if (dot(s.n, rayDir) > 0.0) s.n = -s.n;     // two-sided meshes
    float3 pObj = v0.pos * w0 + v1.pos * bary.x + v2.pos * bary.y;
    s.pos = mul(float4(pObj, 1.0), inst.model).xyz;

    float4 col = v0.color * w0 + v1.color * bary.x + v2.color * bary.y;
    float3 albedo = col.rgb * inst.baseColorFactor.rgb;
    if (inst.baseTex != 0) {
        float2 uv = (v0.uv0 * w0 + v1.uv0 * bary.x + v2.uv0 * bary.y);
        uv *= (inst.uvScale > 0.0 ? inst.uvScale : 1.0);
        Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(inst.baseTex)];
        albedo *= tex.SampleLevel(sampLinear, uv, 0).rgb;
    }
    s.albedo = albedo;
    s.emissiveAmt = inst.emissive;
    return s;
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

// Direct outgoing radiance at a surface (ambient + shadowed sun N.L + emissive).
float3 directLight(Surface s, FrameCB f, RaytracingAccelerationStructure scene, bool shadowSun) {
    float shadow = shadowSun ? traceSunShadow(scene, s.pos, s.n, f.sunDir) : 1.0;
    float ndl = saturate(dot(s.n, normalize(-f.sunDir)));
    float3 lit = s.albedo * (ambientIrradiance(s.n, f) + f.sunColor * (f.sunIntensity * ndl * shadow));
    lit += s.albedo * s.emissiveAmt;
    return lit;
}

// Convenience: shade a committed reflection hit (single bounce, sun-shadowed).
float3 shadeHit(uint instanceID, uint primIndex, float2 bary, float3 rayDir, FrameCB f,
                RaytracingAccelerationStructure scene, bool shadowSun) {
    return directLight(fetchSurface(instanceID, primIndex, bary, rayDir), f, scene, shadowSun);
}

// Cosine-weighted hemisphere direction around n, advancing the RNG.
float3 cosineDir(float3 n, inout uint state) {
    float u1 = rand01(state), u2 = rand01(state);
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    float3 up = abs(n.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
    return normalize(local.x * t + local.y * b + local.z * n);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height) return;
    RWTexture2D<float2> shadowAoTex = ResourceDescriptorHeap[pc.shadowAoOut];
    RWTexture2D<float4> giTex       = ResourceDescriptorHeap[pc.giOut];
    RWTexture2D<float4> reflTex     = ResourceDescriptorHeap[pc.reflOut];
    const uint2 px = tid.xy;

    Texture2D<float>  depthTex  = ResourceDescriptorHeap[pc.depthIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[pc.normalIndex];
    Texture2D<float4> matTex    = ResourceDescriptorHeap[pc.materialIndex];

    float d = depthTex[px].r;
    float matFlag = matTex[px].a;
    if (d <= 0.0 || matFlag >= 0.5) {     // background / camera-space viewmodel
        shadowAoTex[px] = float2(1.0, 1.0);
        giTex[px] = float4(0, 0, 0, 0);
        reflTex[px] = float4(0, 0, 0, 0);
        return;
    }

    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[pc.tlasIndex];

    float2 uv = (float2(px) + 0.5) / float2(pc.width, pc.height);
    float ndcZ = d / kWorldDepthMax;
    float3 worldPos = reconstructWorld(uv, ndcZ, f.invViewProj);
    float3 n = normalize(normalTex[px].rgb * 2.0 - 1.0);
    float roughness = matTex[px].g;
    float3 V = normalize(f.cameraPos - worldPos);
    float3 origin = worldPos + n * 0.02;

    // --- Hard sun shadow (any-hit) ---
    float shadow = 1.0;
    {
        RayDesc ray;
        ray.Origin = origin; ray.Direction = normalize(-f.sunDir);
        ray.TMin = 0.01; ray.TMax = 120.0;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) shadow = 0.0;
    }

    // --- Cosine-hemisphere AO + multi-bounce GI (M3: fuller RTGI) ---
    // Each hemisphere sample gathers TWO diffuse bounces: the first hit is sun-
    // shadowed (real contact darkening), and a second bounce off it adds the light
    // that reaches it indirectly. The denoiser cleans the extra noise.
    uint rays = max(pc.rayCount, 1u);
    uint state = wangHash(px.x + px.y * 9277u + pc.frameIndex * 26699u);
    float occluded = 0.0;
    float3 gi = float3(0, 0, 0);
    for (uint i = 0; i < rays; ++i) {
        float3 dir = cosineDir(n, state);
        RayDesc ray;
        ray.Origin = origin; ray.Direction = dir;
        ray.TMin = 0.01; ray.TMax = pc.giRange;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        while (q.Proceed()) {}
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            if (q.CommittedRayT() < pc.aoRadius) occluded += 1.0;
            Surface s1 = fetchSurface(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                      q.CommittedTriangleBarycentrics(), dir);
            float3 L = directLight(s1, f, scene, true);     // first bounce, sun-shadowed

            // Second bounce off the first hit -> fuller indirect.
            float3 dir2 = cosineDir(s1.n, state);
            RayDesc ray2;
            ray2.Origin = s1.pos + s1.n * 0.02; ray2.Direction = dir2;
            ray2.TMin = 0.01; ray2.TMax = pc.giRange;
            RayQuery<RAY_FLAG_FORCE_OPAQUE> q2;
            q2.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray2);
            while (q2.Proceed()) {}
            float3 L2 = (q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
                ? directLight(fetchSurface(q2.CommittedInstanceID(), q2.CommittedPrimitiveIndex(),
                                           q2.CommittedTriangleBarycentrics(), dir2), f, scene, false)
                : skyColor(dir2, f);
            gi += L + s1.albedo * L2;
        } else {
            gi += skyColor(dir, f);       // escaped the scene -> sky irradiance
        }
    }
    float ao = saturate(1.0 - occluded / float(rays));
    gi /= float(rays);

    // --- Specular reflection (one closest-hit ray, roughness-aware) ---
    // Perturb the mirror direction inside a cone that widens with surface roughness;
    // the denoiser cleans the resulting glossy noise. The reflected hit is sun-
    // shadowed so reflections of shadowed geometry read correctly.
    float3 refl = float3(0, 0, 0);
    {
        float3 mirror = reflect(-V, n);
        float3 rUp = abs(mirror.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 rT = normalize(cross(rUp, mirror));
        float3 rB = cross(mirror, rT);
        float spread = roughness * roughness * 0.5;
        float ru1 = rand01(state), ru2 = rand01(state);
        float2 disk = float2(cos(2.0 * PI * ru2), sin(2.0 * PI * ru2)) * sqrt(ru1) * spread;
        float3 dir = normalize(mirror + disk.x * rT + disk.y * rB);
        if (dot(dir, n) < 0.0) dir = mirror;     // keep above the surface

        RayDesc ray;
        ray.Origin = origin; ray.Direction = dir;
        ray.TMin = 0.01; ray.TMax = 120.0;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        while (q.Proceed()) {}
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            refl = shadeHit(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                            q.CommittedTriangleBarycentrics(), dir, f, scene, true);
        } else {
            refl = skyColor(dir, f);
        }
    }

    shadowAoTex[px] = float2(shadow, ao);
    giTex[px]   = float4(gi, 1.0);
    reflTex[px] = float4(refl, 1.0);
}
