// M2 deferred projected decals (bullet marks, scorch). Runs after the gbuffer
// fill, before lighting: reconstructs each world pixel's position from depth,
// projects it into every decal's oriented box, and blends a procedurally generated
// mark into albedo. No decal texture atlas - the pattern comes from the local box
// coordinates and the decal kind. The deferred resolve then lights the marked
// albedo, so decals receive the same shadows / GI / fog as the surface they sit on.

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

struct DecalData {
    float3 center;  float halfDepth;
    float3 normal;  float halfWidth;
    float3 tangent; float halfHeight;
    float3 color;   float alpha;
    uint   kind;    float3 pad;
};

struct Push {
    uint frameCB;
    uint albedoIn;
    uint depthIndex;
    uint materialIndex;
    uint normalIndex;
    uint decalBuf;
    uint decalCount;
    uint pad;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

static const float kWorldDepthMax = 0.95;

// Cheap hash noise for crack/soot break-up (no texture).
float hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

// Generate a decal's coverage at local in-plane coords uv (range ~[-1,1]).
// Returns coverage in [0,1]; the caller multiplies by the decal alpha.
float decalCoverage(float2 uv, uint kind) {
    float r = length(uv);
    if (kind == 2u) {
        // Contact/grounding shadow: a smooth soft disc, darkest at the centre, feathered to nothing
        // at the edge. No noise or cracks - this fakes the ambient occlusion an actor casts where it
        // meets the floor, so enemies/props sit in the world instead of floating.
        float c = smoothstep(1.0, 0.0, r);
        return c * c;
    }
    if (kind == 1u) {
        // Scorch: soft sooty disc with a noisy, ragged edge.
        float edge = 0.85 + 0.15 * hash21(uv * 6.0);
        float c = smoothstep(edge, 0.15, r);
        return c * c;
    }
    if (kind == 3u) {
        // Hazard chevrons: diagonal warning stripes inside a feathered square (Foundry/Furnace floor
        // markings). Stripe band from the diagonal coordinate; rectangle mask feathers the edges.
        float band   = abs(frac((uv.x + uv.y) * 2.0) - 0.5) * 2.0;
        float stripe = smoothstep(0.62, 0.40, band);
        float rect   = (1.0 - smoothstep(0.78, 1.0, abs(uv.x))) * (1.0 - smoothstep(0.78, 1.0, abs(uv.y)));
        float border = smoothstep(0.04, 0.0, abs(max(abs(uv.x), abs(uv.y)) - 0.92));  // painted frame
        return saturate(stripe * rect + border);
    }
    if (kind == 4u) {
        // Panel code: rows of stencilled dashes (Foundry equipment labels).
        float row     = abs(frac(uv.y * 3.0) - 0.5) * 2.0;       // 3 rows
        float rowMask = smoothstep(0.55, 0.30, row);
        float dash    = step(0.45, frac(uv.x * 4.5));            // dashes along x
        float rect    = (1.0 - smoothstep(0.80, 1.0, abs(uv.x))) * (1.0 - smoothstep(0.85, 1.0, abs(uv.y)));
        return rowMask * dash * rect;
    }
    if (kind == 5u) {
        // Etched sigil: a ring, an inner ring, radial ticks and a centre mark (Reliquary glyph).
        float ang    = atan2(uv.y, uv.x);
        float ring   = smoothstep(0.06, 0.0, abs(r - 0.72));
        float inner  = smoothstep(0.045, 0.0, abs(r - 0.34));
        float ticks  = step(0.72, abs(frac(ang * 1.591549 * 8.0) - 0.5) * 2.0)
                       * smoothstep(0.95, 0.55, r) * step(0.45, r);   // 8 spokes near the rim
        float core   = smoothstep(0.10, 0.0, r);
        return saturate(ring + inner * 0.8 + ticks * 0.85 + core);
    }
    // Bullet mark: dark impact pit, a faint cracked ring, and short radial cracks.
    float pit = smoothstep(0.35, 0.0, r);
    float body = smoothstep(1.0, 0.25, r);
    float ang = atan2(uv.y, uv.x);
    float cracks = saturate(0.6 - abs(frac(ang * 1.591549 * 5.0) - 0.5) * 2.0);   // 5 spokes
    cracks *= smoothstep(1.0, 0.2, r) * step(0.2, r);
    float n = hash21(uv * 9.0);
    return saturate(max(pit, body * (0.55 + 0.45 * n)) + cracks * 0.5);
}

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

struct DecalOut {
    float4 albedo   : SV_Target0;
    float4 material : SV_Target1;   // R=AO G=roughness B=metal A=viewmodel tag (gbuffer layout)
};

DecalOut PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) {
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[pc.albedoIn];
    Texture2D<float4> matTex    = ResourceDescriptorHeap[pc.materialIndex];
    DecalOut o;
    o.albedo = float4(albedoTex.SampleLevel(sampLinear, uv, 0).rgb, 1.0);
    o.material = matTex.SampleLevel(sampLinear, uv, 0);

    Texture2D<float> depthTex = ResourceDescriptorHeap[pc.depthIndex];
    float d = depthTex.SampleLevel(sampLinear, uv, 0).r;
    if (d <= 0.0 || o.material.a >= 0.5 || pc.decalCount == 0u) return o;   // bg / viewmodel

    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    float ndcZ = d / kWorldDepthMax;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    float4 wp = mul(float4(ndc, ndcZ, 1.0), f.invViewProj);
    float3 worldPos = wp.xyz / max(wp.w, 1e-6);

    Texture2D<float4> normalTex = ResourceDescriptorHeap[pc.normalIndex];
    float3 surfN = normalize(normalTex.SampleLevel(sampLinear, uv, 0).rgb * 2.0 - 1.0);

    StructuredBuffer<DecalData> decals = ResourceDescriptorHeap[pc.decalBuf];
    for (uint i = 0; i < pc.decalCount; ++i) {
        DecalData dc = decals[i];
        if (dot(surfN, dc.normal) < 0.35) continue;        // only surfaces facing the decal
        float3 rel = worldPos - dc.center;
        float3 bitangent = cross(dc.normal, dc.tangent);
        float lu = dot(rel, dc.tangent) / max(dc.halfWidth, 1e-4);
        float lv = dot(rel, bitangent) / max(dc.halfHeight, 1e-4);
        float ln = dot(rel, dc.normal) / max(dc.halfDepth, 1e-4);
        if (abs(lu) > 1.0 || abs(lv) > 1.0 || abs(ln) > 1.0) continue;
        float cov = decalCoverage(float2(lu, lv), dc.kind) * dc.alpha;
        cov *= 1.0 - saturate(abs(ln));                    // fade across the projection depth
        cov = saturate(cov);
        o.albedo.rgb = lerp(o.albedo.rgb, dc.color, cov);
        // Scuff the surface: drive roughness up and metalness down so the mark breaks
        // any reflection (otherwise a dark mark vanishes on a glossy/metal floor).
        o.material.g = lerp(o.material.g, 0.95, cov);
        o.material.b = lerp(o.material.b, 0.0, cov);
    }
    return o;
}
