// M2 GPU-rendered particles. The game simulates a particle pool on the CPU (muzzle
// sparks, bullet-impact embers, explosion bursts) and submits world-space particles;
// the engine renders them here as camera-facing additive billboards composited into
// the post-TAA HDR scene, before bloom, so bright sparks bloom and pick up the grade.
//
//   Blit*   : full-screen copy of the resolved scene into the composite target (so
//             the additive particle draw has the lit scene underneath without
//             contaminating the TAA history target).
//   Part*   : one instanced quad per particle, expanded in world space along the
//             camera right/up axes, with a soft radial falloff and additive blend.

struct FrameCB {
    row_major float4x4 viewProj;        // jittered (raster)
    row_major float4x4 viewProjCam;
    float3 sunDir;   float sunIntensity;
    float3 sunColor; float ambient;
    float  exposure; float3 clearColor;
    float3 fogColor; float fogDensity;
    float  nearZ;    float3 _pad;
    row_major float4x4 invViewProj;
    float3 cameraPos; uint lightCount;
};

struct Particle {
    float3 pos;   float size;
    float3 color; float emissive;       // color * emissive = HDR radiance
    float3 vel;   float stretch;        // billboard elongates along screen-projected vel
};

// ---- full-screen blit (scene -> composite), with heat-haze refraction ----
struct BlitPush {
    uint  srcIndex;       // bindless SRV of the resolved scene
    uint  frameCB;        // for viewProj (project heat sources to screen)
    uint  heatBuf;        // StructuredBuffer<HeatSource> (0 sources = plain copy)
    uint  heatCount;
    float time;           // seconds, animates the shimmer
    float aspect;         // width/height, so the radial falloff is round
    float pad0, pad1;
};
ConstantBuffer<BlitPush> bp : register(b0);
SamplerState sampLinear : register(s1);

struct HeatSource { float3 center; float radius; float strength; float pad0, pad1, pad2; };

void BlitVS(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 BlitPS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> src = ResourceDescriptorHeap[bp.srcIndex];
    // Heat-haze: refract the scene by warping the sample UV toward an animated shimmer inside a
    // soft radius around each heat source (energy orbs, impact shocks). Far from any source the
    // displacement is zero, so this is a plain copy everywhere else.
    float2 warp = float2(0.0, 0.0);
    if (bp.heatCount > 0) {
        ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[bp.frameCB];
        StructuredBuffer<HeatSource> heat = ResourceDescriptorHeap[bp.heatBuf];
        for (uint i = 0; i < bp.heatCount; ++i) {
            HeatSource hs = heat[i];
            float4 clip = mul(float4(hs.center, 1.0), f.viewProj);
            if (clip.w <= 0.001) continue;                       // behind camera
            float2 suv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
            float  screenRraw = hs.radius / clip.w;              // nearer source -> larger on screen
            // Keep the shimmer LOCAL. An unbounded screen radius lets a near/large source
            // (an orb reaching the camera, a close impact shock) balloon its refraction
            // across the WHOLE screen, which reads as the entire image going blurry as the
            // threat comes at you. Cap the radius and fade the warp as it would dominate the
            // screen, so it stays a tight detail on the orb and never a fullscreen smear.
            const float kMaxScreenR = 0.5;                       // never warp more than ~half the screen
            float  screenR = min(screenRraw, kMaxScreenR);
            float  nearAtten = screenR / max(screenRraw, 1e-4);  // 1 far, ->0 as source fills the view
            float2 d = (uv - suv) * float2(bp.aspect, 1.0);
            float  fall = saturate(1.0 - length(d) / max(screenR, 1e-4));
            fall *= fall;                                        // tighten toward the core
            float  ph = clip.w * 0.7;                            // per-source phase (varies by depth)
            float  nx = sin(uv.y * 90.0 + bp.time * 11.0 + ph);
            float  ny = sin(uv.x * 96.0 - bp.time * 9.0 + ph * 1.3);
            warp += fall * hs.strength * nearAtten * float2(nx, ny);
        }
    }
    return float4(src.SampleLevel(sampLinear, uv + warp, 0).rgb, 1.0);
}

// ---- additive billboards ----
struct PartPush {
    uint  frameCB;
    uint  particleBuf;
    uint  count;
    uint  pad0;
    float3 camRight; float pad1;
    float3 camUp;    float pad2;
};
ConstantBuffer<PartPush> pp : register(b0);

struct PartVSOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;   // [-1,1] across the quad
    float3 color : COLOR0;
    float  round : TEXCOORD1;   // 1 = round sprite (no screen streak), 0 = elongated comet
};

PartVSOut PartVS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[pp.particleBuf];
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pp.frameCB];
    Particle p = parts[iid];

    // Two-triangle quad from the vertex id (0..5).
    float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1),  float2(1, -1), float2(1, 1)
    };
    float2 c = corners[vid];

    // Motion streak: build the quad in a frame whose local-x runs along the velocity's
    // screen projection, and elongate that axis by the on-screen speed. Slow/edge-on
    // particles fall back to a round sprite (v2l ~ 0 -> stretchAmt ~ 1).
    float2 v2 = float2(dot(p.vel, pp.camRight), dot(p.vel, pp.camUp));
    float v2l = length(v2);
    float2 along = v2l > 1e-4 ? v2 / v2l : float2(1, 0);
    float2 perp = float2(-along.y, along.x);
    float stretchAmt = clamp(1.0 + p.stretch * v2l, 1.0, 18.0);
    float2 local = (along * (c.x * stretchAmt) + perp * c.y) * p.size;

    // Near-camera fade/cull. A camera-facing billboard whose CENTRE is beside or behind
    // the camera but whose corners cross the near plane gets clipped into a sliver that
    // smears across the screen -- the "particles coming from weird angles / behind you"
    // artifact (these effects have no depth test, so nothing else hides it). centerW is
    // the distance in front of the camera (reverse-Z RH: <= 0 means behind). Collapse the
    // quad to a zero-area point as the centre approaches the near plane, scaled by the
    // sprite's own size so big halos/bursts cull from further out. Distant particles
    // (centerW well beyond the plane) are unaffected.
    float centerW = mul(float4(p.pos, 1.0), f.viewProj).w;
    float nearFade = saturate((centerW - f.nearZ) / (p.size * 1.5 + 0.20));
    local *= nearFade;

    float3 world = p.pos + local.x * pp.camRight + local.y * pp.camUp;

    PartVSOut o;
    o.pos = mul(float4(world, 1.0), f.viewProj);
    o.uv = c;                              // keep round UVs so the streak fades softly along its length
    o.color = p.color * p.emissive;
    // Roundness: 1 when the sprite has (almost) no screen streak (e.g. an orb travelling
    // straight at the camera), 0 once it elongates into a comet. Drives a crisper falloff
    // in the PS so head-on motes do not stack into a soft blur.
    o.round = saturate(1.0 - (stretchAmt - 1.0) * 0.6);
    return o;
}

// ---- alpha-blended shadow smoke (dissolving-edge aura) ----
// Same camera-facing billboard as PartVS, but the PS outputs a DARK colour + soft alpha so the
// blend (SrcAlpha/InvSrcAlpha) darkens the scene toward the smoke colour - a shadow aura that
// dissolves the enemy silhouette into shadow. p.color = smoke colour, p.emissive = opacity 0..1.
struct SmokeVSOut {
    float4 pos     : SV_Position;
    float2 uv      : TEXCOORD0;
    float3 color   : COLOR0;
    float  opacity : TEXCOORD1;
};

SmokeVSOut PartSmokeVS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    StructuredBuffer<Particle> parts = ResourceDescriptorHeap[pp.particleBuf];
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pp.frameCB];
    Particle p = parts[iid];
    float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1),  float2(1, -1), float2(1, 1)
    };
    float2 c = corners[vid];
    float2 local = c * p.size;
    float centerW = mul(float4(p.pos, 1.0), f.viewProj).w;
    float nearFade = saturate((centerW - f.nearZ) / (p.size * 1.5 + 0.20));
    local *= nearFade;
    float3 world = p.pos + local.x * pp.camRight + local.y * pp.camUp;
    SmokeVSOut o;
    o.pos = mul(float4(world, 1.0), f.viewProj);
    o.uv = c;
    o.color = p.color;
    o.opacity = p.emissive;
    return o;
}

float4 PartSmokePS(SmokeVSOut i) : SV_Target {
    float r2 = dot(i.uv, i.uv);
    float a = saturate(1.0 - r2);
    a = a * a;                               // soft round puff
    return float4(i.color, a * i.opacity);   // alpha blend darkens the scene toward the smoke colour
}

float4 PartPS(PartVSOut i) : SV_Target {
    float r2 = dot(i.uv, i.uv);
    float t = saturate(1.0 - r2);
    float glow = t * t * t;                 // wide soft halo
    float core = pow(t, 8.0);               // tight bright core
    // A round mote (an orb head-on) has no screen streak to give it shape, so many of
    // them stacked read as one washy blur. Tighten + concentrate the halo for round
    // sprites so the cluster stays a crisp, dense energy ball; comet streaks (round ~ 0)
    // keep their long soft tail untouched.
    float glowTight = t * t * t * t * t * 1.25;   // t^5, brightness-compensated centre
    glow = lerp(glow, glowTight, i.round);
    // Cross sparkle: faint 4-point star, strongest where the sprite is hottest, so only
    // the brightest motes (muzzle flash, bolt core, impact flash) read as starbursts.
    float cross = pow(saturate(1.0 - abs(i.uv.x)), 22.0) + pow(saturate(1.0 - abs(i.uv.y)), 22.0);
    // Art bible "colour has meaning": only WARM / neutral motes (player ballistic, sparks, fire)
    // flash a warm-white centre; saturated gameplay hues - hostile MAGENTA, player CYAN - keep their
    // own colour with a darker, coloured centre (NOT a white bloom blob). Gate the white core by
    // saturation, not luminance, so a bright magenta orb no longer reads as a white sphere.
    float maxc = max(i.color.r, max(i.color.g, i.color.b));
    float minc = min(i.color.r, min(i.color.g, i.color.b));
    float sat  = (maxc - minc) / max(maxc, 1e-3);
    float whiteCore = saturate(1.0 - sat * 1.4);   // ~1 for warm/white, ~0 for magenta/cyan
    float outer = pow(t, 2.2) * (0.18 + 0.10 * saturate(maxc * 0.25));        // larger glow skirt for bloom
    float3 radiance = i.color * (outer + glow + core * 1.8)                  // coloured body + core
                    + core * core * 1.5 * whiteCore * float3(1.0, 0.95, 0.86) // warm-white centre, warm motes only
                    + i.color * core * cross * (0.40 + 0.45 * whiteCore);     // sparkle: coloured, warm-biased
    return float4(radiance, 1.0);           // additive: contribution = rgb (alpha 1)
}
