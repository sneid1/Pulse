// M3 reference path tracer (CEILING). An offline ground-truth capture mode for hero
// stills / look targets: pure camera-ray path tracing over the DXR scene (the same
// bindless TLAS + vb/ib the real-time RT tier uses), accumulated across many samples
// into an HDR buffer, then AgX-resolved to LDR. Not real-time: the engine dispatches
// CSMain once per sample (each adds 1 spp to the accumulator) and Resolve at the end.
//
//   CSMain  : one path per pixel per dispatch. Camera ray -> closest hit -> next-event
//             estimation (sun + one sampled local light, each with a shadow ray) +
//             emissive, then a BRDF-chosen bounce (glossy for metal, cosine diffuse
//             for dielectric) with Russian roulette. Sky on a miss. accum += radiance.
//   Resolve : averages the accumulator and applies the same AgX + grade + vignette as
//             the real-time tonemap, so the reference matches the game's look.

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

struct LightData {
    float3 position; float intensity;
    float3 color;    float radius;
};

static const float PI = 3.14159265359;

// ---------------------------------------------------------------- trace pass --

struct Push {
    uint frameCB;
    uint tlas;
    uint instanceSB;
    uint accumUav;     // RWTexture2D<float4> (rgb = radiance sum, a = sample count)
    uint lightsSB;
    uint width, height;
    uint sampleIndex;  // 0..samples-1 (RNG seed + accumulation)
    uint bounces;
    uint lightCount;
    uint pad0, pad1, pad2, pad3, pad4;
};
ConstantBuffer<Push> pc : register(b0);
SamplerState sampLinear : register(s0);

uint wangHash(uint s) {
    s = (s ^ 61u) ^ (s >> 16); s *= 9u; s = s ^ (s >> 4); s *= 0x27d4eb2du; s = s ^ (s >> 15);
    return s;
}
float rand01(inout uint state) { state = wangHash(state); return (state & 0xffffffu) / 16777216.0; }

float3 skyColor(float3 dir, FrameCB f) {
    float3 sky = f.sunColor * 0.8 + f.fogColor;
    float3 ground = f.fogColor * 0.6;
    return lerp(ground, sky, saturate(dir.y * 0.5 + 0.5));
}

// Cook-Torrance BRDF for one light; radiance folds colour*intensity*attenuation.
// Returns outgoing radiance toward V (diffuse + specular) already * NoL.
float3 brdf(float3 N, float3 V, float3 L, float3 albedo, float metal, float rough, float3 radiance) {
    float NoL = saturate(dot(N, L));
    if (NoL <= 0.0) return float3(0, 0, 0);
    float3 H = normalize(V + L);
    float NoV = saturate(dot(N, V)) + 1e-4;
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));
    float a = max(rough * rough, 0.002);
    float a2 = a * a;
    float dd = NoH * NoH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * dd * dd + 1e-6);
    float k = a2 * 0.5;
    float G = (NoV / (NoV * (1.0 - k) + k)) * (NoL / (NoL * (1.0 - k) + k));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 F = F0 + (1.0 - F0) * pow(saturate(1.0 - VoH), 5.0);
    float3 spec = (D * G) * F / (4.0 * NoV * NoL + 1e-5);
    float3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * radiance * NoL;
}

struct Hit {
    float3 pos;
    float3 n;
    float3 albedo;
    float3 emissive;
    float  metal;
    float  rough;
};

Hit fetchHit(RaytracingAccelerationStructure scene, uint instID, uint primIndex, float2 bary, float3 rayDir, float3 origin, float t) {
    StructuredBuffer<InstanceData> insts = ResourceDescriptorHeap[pc.instanceSB];
    InstanceData inst = insts[instID];
    StructuredBuffer<Vertex> verts = ResourceDescriptorHeap[NonUniformResourceIndex(inst.vbIndex)];
    StructuredBuffer<uint>   idx   = ResourceDescriptorHeap[NonUniformResourceIndex(inst.ibIndex)];
    uint i0 = idx[primIndex * 3 + 0], i1 = idx[primIndex * 3 + 1], i2 = idx[primIndex * 3 + 2];
    Vertex v0 = verts[i0], v1 = verts[i1], v2 = verts[i2];
    float w0 = 1.0 - bary.x - bary.y;

    Hit h;
    h.pos = origin + rayDir * t;
    float3 nObj = v0.nrm * w0 + v1.nrm * bary.x + v2.nrm * bary.y;
    h.n = normalize(mul(float4(nObj, 0.0), inst.model).xyz);
    if (dot(h.n, rayDir) > 0.0) h.n = -h.n;

    float2 uv = (v0.uv0 * w0 + v1.uv0 * bary.x + v2.uv0 * bary.y) * (inst.uvScale > 0.0 ? inst.uvScale : 1.0);
    float4 col = v0.color * w0 + v1.color * bary.x + v2.color * bary.y;
    float3 albedo = col.rgb * inst.baseColorFactor.rgb;
    if (inst.baseTex != 0) {
        Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(inst.baseTex)];
        albedo *= tex.SampleLevel(sampLinear, uv, 0).rgb;
    }
    h.albedo = albedo;
    h.emissive = albedo * inst.emissive;

    float rough = inst.roughness, metal = inst.metallic;
    if (inst.ormTex != 0) {
        Texture2D<float4> arm = ResourceDescriptorHeap[NonUniformResourceIndex(inst.ormTex)];
        float3 m = arm.SampleLevel(sampLinear, uv, 0).rgb;
        rough = m.g; metal = m.b;
    }
    h.metal = saturate(metal * inst.metalScale);
    h.rough = clamp(rough + inst.roughBoost, 0.04, 1.0);
    return h;
}

bool occluded(RaytracingAccelerationStructure scene, float3 o, float3 dir, float tmax) {
    RayDesc ray; ray.Origin = o; ray.Direction = dir; ray.TMin = 0.001; ray.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 cosineHemisphere(float3 n, inout uint rng) {
    float u1 = rand01(rng), u2 = rand01(rng);
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    float3 t = abs(n.y) < 0.95 ? normalize(cross(float3(0, 1, 0), n)) : normalize(cross(float3(1, 0, 0), n));
    float3 b = cross(n, t);
    float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
    return normalize(local.x * t + local.y * b + local.z * n);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height) return;
    const uint2 px = tid.xy;
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[pc.tlas];
    RWTexture2D<float4> accum = ResourceDescriptorHeap[pc.accumUav];

    uint rng = wangHash(px.x * 1973u + px.y * 9277u + pc.sampleIndex * 26699u + 1u);

    // Camera ray with sub-pixel jitter (own AA; not the raster gbuffer jitter).
    float2 uv = (float2(px) + float2(rand01(rng), rand01(rng))) / float2(pc.width, pc.height);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, 0.5, 1.0), f.invViewProj);
    float3 worldP = wp.xyz / max(wp.w, 1e-6);
    float3 o = f.cameraPos;
    float3 d = normalize(worldP - f.cameraPos);

    float3 radiance = float3(0, 0, 0);
    float3 throughput = float3(1, 1, 1);
    float3 sunL = normalize(-f.sunDir);
    StructuredBuffer<LightData> lights = ResourceDescriptorHeap[pc.lightsSB];

    for (uint b = 0; b < pc.bounces; ++b) {
        RayDesc ray; ray.Origin = o; ray.Direction = d; ray.TMin = 0.001; ray.TMax = 1.0e4;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        while (q.Proceed()) {}
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            radiance += throughput * skyColor(d, f);
            break;
        }
        Hit h = fetchHit(scene, q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                         q.CommittedTriangleBarycentrics(), d, o, q.CommittedRayT());
        float3 V = -d;
        float3 sp = h.pos + h.n * 0.02;

        radiance += throughput * h.emissive;

        // NEE: sun (shadowed).
        if (dot(h.n, sunL) > 0.0 && !occluded(scene, sp, sunL, 200.0))
            radiance += throughput * brdf(h.n, V, sunL, h.albedo, h.metal, h.rough, f.sunColor * f.sunIntensity);

        // NEE: one randomly chosen local point light (weighted by count), shadowed.
        if (pc.lightCount > 0) {
            uint li = min(uint(rand01(rng) * float(pc.lightCount)), pc.lightCount - 1u);
            LightData lt = lights[li];
            float3 toL = lt.position - h.pos;
            float dist = length(toL);
            float3 ld = toL / max(dist, 1e-4);
            float att = saturate(1.0 - dist / max(lt.radius, 1e-3)); att *= att;
            if (att > 0.0 && dot(h.n, ld) > 0.0 && !occluded(scene, sp, ld, dist - 0.02))
                radiance += throughput * brdf(h.n, V, ld, h.albedo, h.metal, h.rough,
                                              lt.color * (lt.intensity * att)) * float(pc.lightCount);
        }

        // BRDF-chosen bounce: glossy for metal, cosine diffuse for dielectric.
        if (rand01(rng) < h.metal) {
            float3 mirror = reflect(d, h.n);
            float3 t = abs(mirror.y) < 0.95 ? normalize(cross(float3(0, 1, 0), mirror)) : normalize(cross(float3(1, 0, 0), mirror));
            float3 bt = cross(mirror, t);
            float spread = h.rough * h.rough * 0.6;
            float u1 = rand01(rng), u2 = rand01(rng);
            float2 disk = float2(cos(2.0 * PI * u2), sin(2.0 * PI * u2)) * sqrt(u1) * spread;
            d = normalize(mirror + disk.x * t + disk.y * bt);
            if (dot(d, h.n) < 0.0) d = mirror;
            throughput *= h.albedo;
        } else {
            d = cosineHemisphere(h.n, rng);
            throughput *= h.albedo;
        }
        o = sp;

        // Russian roulette after a couple of bounces.
        if (b >= 2u) {
            float p = max(throughput.r, max(throughput.g, throughput.b));
            if (rand01(rng) > p) break;
            throughput /= max(p, 1e-3);
        }
    }

    // Guard against NaN/Inf fireflies before accumulating.
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0, 0, 0);
    accum[px] += float4(radiance, 1.0);
}

// ------------------------------------------------------------------ resolve --

struct ResolvePush { uint accumSrv; float exposure; uint pad0, pad1; };
ConstantBuffer<ResolvePush> rp : register(b0);

float3 agxContrast(float3 x) {
    float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 agx(float3 val) {
    const float3x3 m = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                0.0784335999999992, 0.878468636469772,  0.0784336,
                                0.0784336,          0.0784336,          0.879142973793104);
    const float minEv = -12.47393, maxEv = 4.026069;
    val = mul(m, val);
    val = clamp(log2(val), minEv, maxEv);
    val = (val - minEv) / (maxEv - minEv);
    return agxContrast(val);
}

void ResolveVS(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 ResolvePS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> accum = ResourceDescriptorHeap[rp.accumSrv];
    float4 a = accum.SampleLevel(sampLinear, uv, 0);
    float3 c = a.rgb / max(a.a, 1.0);
    c *= rp.exposure;
    c = agx(c);
    c *= float3(0.96, 1.0, 1.06);
    c = saturate((c - 0.5) * 1.05 + 0.5);
    float2 q = uv - 0.5;
    c *= 1.0 - dot(q, q) * 0.55;
    return float4(saturate(c), 1.0);
}
