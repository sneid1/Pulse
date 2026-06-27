// M1 GBuffer pass. Renders SceneFrame mesh instances into a deferred gbuffer so
// the lighting/AO/fog/reflection passes can run in screen space off it. Layout:
//   RT0 albedo   R8G8B8A8_UNORM     rgb = albedo
//   RT1 normal   R10G10B10A2_UNORM  rgb = world normal * 0.5 + 0.5
//   RT2 material R8G8B8A8_UNORM     R=AO G=roughness B=metallic A=flags
//   RT3 emissive R11G11B10_FLOAT    rgb = emissive radiance
//   RT4 velocity R16G16_FLOAT       screen-space motion vector (curUV - prevUV)
// Bindless vertex/instance layout (vertices pulled by SV_VertexID). Lighting is deferred to
// resolve.hlsl. Viewmodels (cameraSpace) use viewProjCam and are tagged in
// material.a so later screen-space passes can treat them specially.
//
// Motion vectors (for TAA): the rasterised position uses the jitter-perturbed
// viewProj; the velocity is computed from the *unjittered* current and previous
// clip positions so TAA reprojection sees only true scene/camera motion. The
// per-instance previous model matrix is supplied by the engine (keyed by the
// stable MeshInstance id).

struct Vertex {
    float3 pos;
    float3 nrm;
    float4 tangent;
    float2 uv0;
    float2 uv1;
    float4 color;
};

// Canonical FrameCB (must match struct FrameCB in Engine.cpp). Shaders that only
// need the lighting prefix (resolve.hlsl) may declare a shorter mirror; this is
// the full layout because the gbuffer needs the no-jitter reprojection matrices.
struct FrameCB {
    row_major float4x4 viewProj;        // jittered (raster)
    row_major float4x4 viewProjCam;     // jittered (raster, camera-space)
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
};

struct InstanceData {
    row_major float4x4 model;
    row_major float4x4 prevModel;   // previous-frame model (for motion vectors)
    float4 baseColorFactor;
    uint   baseTex;
    float  emissive;
    float  metallic;
    float  roughness;
    uint   cameraSpace;
    uint   normalTex;     // tangent-space normal map SRV (0 = none)
    uint   ormTex;        // AO/Rough/Metal map SRV (0 = none)
    float  uvScale;       // texture tiling multiplier
    uint   vbIndex;       // mesh vertex SRV (RT hit shading)
    uint   ibIndex;       // mesh index SRV (RT hit shading)
    float  metalScale;    // multiplies metalness (ORM or scalar); 1 = unchanged
    float  roughBoost;    // added to roughness then clamped; 0 = unchanged
    float3 rimColor;      // neon-ink fresnel rim emissive (HDR; 0 = none)
    float  rimPower;      // rim falloff exponent (higher = tighter edge; 0 = rim off)
    uint   emissiveTex;   // per-texel emissive map SRV (0 = none)
    float  emissiveTexStrength; // HDR multiplier on the emissive map
};

struct Push {
    uint frameCB;
    uint instanceSB;
    uint instanceIndex;
    uint vbIndex;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

// AA surface micro-detail. Flat, untextured surfaces (uniform albedo + uniform roughness +
// smooth normals) are the classic "demo clay" tell. A cheap world-anchored value-noise field
// adds weathering blotches to albedo, micro-variation to roughness, and a faint normal wobble,
// so matte surfaces catch the light with surface life instead of reading as plastic. Applied to
// world meshes only and faded with distance, so it is TAA-stable (world-anchored = no swimming).
float dHash(float2 p) { p = frac(p * float2(127.1, 311.7)); p += dot(p, p + 34.53); return frac(p.x * p.y); }
float dNoise(float2 p) {
    float2 i = floor(p), f = frac(p); f = f * f * (3.0 - 2.0 * f);
    float a = dHash(i), b = dHash(i + float2(1, 0)), c = dHash(i + float2(0, 1)), d = dHash(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float dFbm(float2 p) { float v = 0.0, a = 0.5; [unroll] for (int k = 0; k < 3; ++k) { v += a * dNoise(p); p *= 2.03; a *= 0.5; } return v; }

struct VSOut {
    float4 pos     : SV_Position;
    float3 nrm     : NORMAL;
    float3 tan     : TANGENT;
    float  tanW    : TANGENTSIGN;
    float2 uv      : TEXCOORD0;
    float4 color   : COLOR0;
    float4 curClip : CURCLIP;     // unjittered current clip pos
    float4 prevClip: PREVCLIP;    // unjittered previous clip pos
    float3 worldPos: WORLDPOS;    // world-space position (for the fresnel rim)
    nointerpolation uint inst : INST;
};

VSOut VSMain(uint vid : SV_VertexID) {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    StructuredBuffer<InstanceData> insts = ResourceDescriptorHeap[pc.instanceSB];
    StructuredBuffer<Vertex> verts = ResourceDescriptorHeap[pc.vbIndex];

    InstanceData inst = insts[pc.instanceIndex];
    Vertex v = verts[vid];

    float4 world     = mul(float4(v.pos, 1.0), inst.model);
    float4 worldPrev = mul(float4(v.pos, 1.0), inst.prevModel);

    VSOut o;
    if (inst.cameraSpace) {
        o.pos      = mul(world, f.viewProjCam);                  // jittered raster
        o.curClip  = mul(world, f.viewProjCamNoJitter);
        o.prevClip = mul(worldPrev, f.prevViewProjCamNoJitter);
    } else {
        o.pos      = mul(world, f.viewProj);                     // jittered raster
        o.curClip  = mul(world, f.viewProjNoJitter);
        o.prevClip = mul(worldPrev, f.prevViewProjNoJitter);
    }
    o.nrm = mul(float4(v.nrm, 0.0), inst.model).xyz;
    o.tan = mul(float4(v.tangent.xyz, 0.0), inst.model).xyz;
    o.tanW = v.tangent.w;
    o.uv = v.uv0 * (inst.uvScale > 0.0 ? inst.uvScale : 1.0);
    o.color = v.color * inst.baseColorFactor;
    o.worldPos = world.xyz;
    o.inst = pc.instanceIndex;
    return o;
}

struct GBufferOut {
    float4 albedo   : SV_Target0;
    float4 normal   : SV_Target1;
    float4 material : SV_Target2;
    float4 emissive : SV_Target3;
    float2 velocity : SV_Target4;
};

GBufferOut PSMain(VSOut i, bool isFront : SV_IsFrontFace) {
    StructuredBuffer<InstanceData> insts = ResourceDescriptorHeap[pc.instanceSB];
    InstanceData inst = insts[i.inst];
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];

    // World renders two-sided (cull none): single-sided kit/box panels are visible from both faces.
    // Flip the geometric normal on back faces so the interior side is lit correctly instead of
    // reading inside-out. (Viewmodels cull back faces, so isFront is always true for them.)
    if (!isFront) i.nrm = -i.nrm;

    float3 albedo = i.color.rgb;
    if (inst.baseTex != 0) {
        Texture2D<float4> tex = ResourceDescriptorHeap[inst.baseTex];   // sRGB -> linear on sample
        if (inst.uvScale < 0.0) {
            // Triplanar (a NEGATIVE uvScale enables it; |uvScale| = world tiling frequency). Projects
            // the texture on the 3 world axes blended by the normal, killing the repeating UV grid and
            // the slope stretching that make rock/ground read as obvious tiled placeholder.
            float s = -inst.uvScale;
            float3 wn = normalize(i.nrm);
            float3 bw = pow(abs(wn), 4.0); bw /= (bw.x + bw.y + bw.z + 1e-4);
            float3 cx = tex.Sample(sampLinear, i.worldPos.zy * s).rgb;
            float3 cy = tex.Sample(sampLinear, i.worldPos.xz * s).rgb;
            float3 cz = tex.Sample(sampLinear, i.worldPos.xy * s).rgb;
            albedo *= cx * bw.x + cy * bw.y + cz * bw.z;
        } else {
            albedo *= tex.Sample(sampLinear, i.uv).rgb;
        }
    }
    if (inst.cameraSpace != 0 && inst.emissive > 0.0) {
        // Hero viewmodels can carry very dark source textures; use the per-instance
        // fill value to lift the baked albedo before lighting while leaving world
        // emissive props untouched.
        albedo = saturate(albedo * (1.0 + inst.emissive * 5.0) +
                          inst.baseColorFactor.rgb * (inst.emissive * 0.55));
    }

    // Normal: tangent-space normal map (OpenGL +Y) via TBN, else the vertex normal.
    float3 n = normalize(i.nrm);
    if (inst.normalTex != 0) {
        float3 T = normalize(i.tan);
        float3 B = cross(n, T) * (i.tanW < 0.0 ? -1.0 : 1.0);
        Texture2D<float4> nmap = ResourceDescriptorHeap[inst.normalTex];
        // Reconstruct Z from XY so 2-channel BC5 normal maps work alongside RGB
        // ones (tangent-space normals are always in the +Z hemisphere).
        float2 nxy = nmap.Sample(sampLinear, i.uv).rg * 2.0 - 1.0;
        float  nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
        float3 tn = float3(nxy, nz);
        n = normalize(tn.x * T + tn.y * B + tn.z * n);
    }

    // Material: AO/Rough/Metal from the ARM map, else per-material scalars.
    float ao = 1.0, rough = inst.roughness, metal = inst.metallic;
    if (inst.ormTex != 0) {
        Texture2D<float4> arm = ResourceDescriptorHeap[inst.ormTex];
        float3 v = arm.Sample(sampLinear, i.uv).rgb;
        ao = v.r; rough = v.g; metal = v.b;
    }
    // Per-material modulation (e.g. de-metal + roughen a too-mirror-like surface so
    // it reads as a solid wall instead of a dark mirror).
    metal = saturate(metal * inst.metalScale);
    rough = clamp(rough + inst.roughBoost, 0.04, 1.0);

    // Surface weathering + detail (Industrial Relicpunk art bible: dirt concentrates at SEAMS and
    // contact points - never a uniform procedural wash - and clean panels need real micro-relief so
    // they read as worked metal/concrete under the key light). World meshes only; world-anchored
    // (TAA-stable, no swimming) and distance-faded.
    if (inst.cameraSpace == 0) {
        float dist = length(f.cameraPos - i.worldPos);
        float fade = saturate(1.0 - dist / 42.0);
        if (fade > 0.0) {
            // Project noise on the world plane facing the dominant normal axis (no slope stretch).
            float3 an = abs(n);
            float2 wp = (an.y >= an.x && an.y >= an.z) ? i.worldPos.xz
                      : (an.x >= an.z)                 ? i.worldPos.zy
                                                       : i.worldPos.xy;
            // CAVITY from the baked AO map: recesses, panel seams + contact points read dark in AO -
            // that is exactly where grime, soot and wear collect (proper, not a uniform overlay).
            float cavity = saturate(1.0 - ao);
            float broad  = dFbm(wp * 0.6);                // large-scale weathering blotches
            float fine   = dFbm(wp * 3.1);                // fine grain
            // Grime mask: pooled in cavities, broken up by the broad field, baselined low on flats.
            float grime  = saturate(cavity * 1.30 + broad * 0.35 - 0.14) * fade;
            // Darken + desaturate toward dark graphite soot where grime pools (seams go gritty).
            const float3 grimeCol = float3(0.055, 0.060, 0.075);
            albedo = lerp(albedo, grimeCol, grime * 0.55);
            albedo *= lerp(1.0, 0.90 + 0.16 * broad, fade);              // faint broad tonal variation
            // Grime + grain roughen the surface (dirt is never glossy); pooled grime most of all.
            rough = clamp(rough + grime * 0.32 + (fine - 0.5) * 0.18 * fade, 0.04, 1.0);
            // Multi-scale DETAIL NORMAL: two octaves of world-space relief (central-difference
            // gradient) so flat sci-fi panels gain worked-surface micro-relief that catches the light.
            float3 T2 = normalize(abs(n.y) < 0.99 ? cross(float3(0, 1, 0), n) : float3(1, 0, 0));
            float3 B2 = cross(n, T2);
            const float e1 = 0.5, e2 = 0.17;
            float2 gA = float2(dFbm(wp * 3.1 + float2(e1, 0)) - dFbm(wp * 3.1 - float2(e1, 0)),
                               dFbm(wp * 3.1 + float2(0, e1)) - dFbm(wp * 3.1 - float2(0, e1)));
            float2 gB = float2(dFbm(wp * 8.5 + float2(e2, 0)) - dFbm(wp * 8.5 - float2(e2, 0)),
                               dFbm(wp * 8.5 + float2(0, e2)) - dFbm(wp * 8.5 - float2(0, e2)));
            float2 grad = gA * 0.9 + gB * 0.6;
            n = normalize(n - (T2 * grad.x + B2 * grad.y) * 0.5 * fade);
        }
    }

    // Motion vector: unjittered current/previous screen UVs. Stored as curUV-prevUV
    // so TAA fetches history at (pixelUV - velocity).
    float2 curUV  = i.curClip.xy  / max(i.curClip.w, 1e-6);
    float2 prevUV = i.prevClip.xy / max(i.prevClip.w, 1e-6);
    curUV  = curUV  * float2(0.5, -0.5) + 0.5;
    prevUV = prevUV * float2(0.5, -0.5) + 0.5;

    GBufferOut o;
    o.albedo = float4(albedo, 1.0);
    o.normal = float4(n * 0.5 + 0.5, 0.0);
    o.material = float4(ao, rough, metal, inst.cameraSpace ? 1.0 : 0.0);
    float3 emissiveOut = albedo * inst.emissive;
    // Per-texel emissive map (e.g. the sci-fi kit's cyan circuit traces): sampled in sRGB,
    // scaled into HDR so the lit traces bloom as neon. Independent of the scalar emissive.
    if (inst.emissiveTex != 0) {
        Texture2D<float4> emap = ResourceDescriptorHeap[inst.emissiveTex];
        emissiveOut += emap.Sample(sampLinear, i.uv).rgb * inst.emissiveTexStrength;
    }
    // Neon-ink fresnel rim: a view-dependent edge glow (brightest on the silhouette) so a dark
    // enemy reads as a shadow-creature wreathed in magenta energy (the pinned art target).
    if (inst.rimPower > 0.0) {
        float3 Vr = normalize(f.cameraPos - i.worldPos);
        float rimF = pow(1.0 - saturate(dot(n, Vr)), inst.rimPower);
        emissiveOut += inst.rimColor * rimF;
    }
    o.emissive = float4(emissiveOut, 1.0);
    o.velocity = curUV - prevUV;
    return o;
}
