// Color-management spine (plan Part I): linear HDR in, AgX filmic tonemap +
// exposure out. A fullscreen triangle samples the HDR target and writes the LDR
// R8G8B8A8 target. AgX is the minimal approximation (Troy Sobotka's AgX, the
// widely used polynomial fit); its output is display-encoded, written straight to
// the 8-bit target.

struct Push {
    uint  hdrIndex;       // bindless SRV of the HDR scene target
    float exposure;
    uint  bloomIndex;     // bindless SRV of the bloom target (0 = none)
    float bloomIntensity;
    float invW;           // 1/width  (M2 screen-space niceties)
    float invH;           // 1/height
    float sunU, sunV;     // sun screen UV for god rays
    float godray;         // god-ray strength (0 = off: sun off-screen / behind camera)
    float sharpen;        // W3 post knobs (defaults match the prior hard-coded values)
    float vignette;
    float grain;
    float caScale;        // chromatic-aberration multiplier (0 = off)
    float gradeEnvSat;    // W3 palette grade: environment saturation (< 1 restrains)
    float gradeNeonSat;   // cyan/magenta saturation (> 1 protects/boosts)
    float gradeNeonGain;  // neon-detection strength (0 = uniform saturation)
    float3 gradeTint;     // M7: per-biome frame-wide color multiplier ({1,1,1} = no change)
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

// Combined scene (HDR + bloom) sampled at a UV, used by the M2 chromatic-aberration
// and sharpen taps.
float3 sceneAt(float2 uv) {
    Texture2D<float4> hdr = ResourceDescriptorHeap[pc.hdrIndex];
    float3 c = hdr.SampleLevel(sampLinear, uv, 0).rgb;
    if (pc.bloomIndex != 0) {
        Texture2D<float4> bloom = ResourceDescriptorHeap[pc.bloomIndex];
        c += bloom.SampleLevel(sampLinear, uv, 0).rgb * pc.bloomIntensity;
    }
    return c;
}

float hash21(float2 p) {
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac((p.x + p.y) * p.x);
}

float3 agxDefaultContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

float3 agx(float3 val) {
    const float3x3 agxMat = float3x3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0784336,          0.0784336,          0.879142973793104);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;

    val = mul(agxMat, val);
    val = clamp(log2(val), minEv, maxEv);
    val = (val - minEv) / (maxEv - minEv);
    return agxDefaultContrastApprox(val);
}

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);          // (0,0) (2,0) (0,2)
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 dir = uv - 0.5;
    float3 center = sceneAt(uv);

    // Chromatic aberration: split the colour channels radially, stronger toward the
    // edges (lens fringing). Cheap, sells the "shot through a lens" feel.
    float2 caOff = dir * (0.004 * dot(dir, dir) + 0.0008) * pc.caScale;
    float3 c = float3(sceneAt(uv + caOff).r, center.g, sceneAt(uv - caOff).b);

    // Light sharpen (unsharp mask) so TAA does not leave the image soft.
    float3 nb = sceneAt(uv + float2(pc.invW, 0)) + sceneAt(uv - float2(pc.invW, 0))
              + sceneAt(uv + float2(0, pc.invH)) + sceneAt(uv - float2(0, pc.invH));
    c += (c - nb * 0.25) * pc.sharpen;
    c = max(c, 0.0);

    // God rays: march the HDR scene radially toward the sun's screen position, accumulating
    // brightness with decay. The bright sky/sun smears into shafts; dark occluders between gate
    // them into beams. Self-gating - a dark sky (indoors) contributes ~nothing. Added in HDR.
    if (pc.godray > 0.001) {
        float2 sunUV = float2(pc.sunU, pc.sunV);
        float2 delta = (sunUV - uv) * (0.92 / 56.0);
        float2 coord = uv;
        float3 shaft = 0.0;
        float w = 0.8;
        [unroll] for (int i = 0; i < 56; ++i) {
            coord += delta;
            shaft += max(sceneAt(coord) - 0.20, 0.0) * w;   // bias out dim geometry; keep bright sky
            w *= 0.955;
        }
        c += shaft * (pc.godray / 26.0);
    }

    c *= pc.exposure;
    c = agx(c);

    // Cinematic palette grade (W3, Neon Ink Brutalism - grounded variant). AgX output is
    // display-referred; we grade on top. Three steps give the frame the cohesion of a graded
    // film instead of a raw engine viewport:
    //   1. Split-tone: cool indigo in the shadows, warm in the highlights -> one coherent
    //      light temperature across the whole frame (this is the big "finished" tell).
    //   2. Smooth filmic contrast (S-curve): deepens shadows + lifts highlights without the
    //      hard clip of a linear contrast, killing the flat midtone wash.
    //   3. Selective saturation: protect gameplay cyan/magenta, restrain the rest, so the
    //      neon language reads against a grounded, desaturated world.
    c = saturate(c);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));

    float3 shadowTint = float3(0.93, 0.98, 1.07);   // faint cool toe (graphite), not a blue wash
    float3 highTint   = float3(1.07, 1.02, 0.93);   // warm shoulder
    c *= lerp(shadowTint, highTint, smoothstep(0.0, 1.0, luma));

    c = saturate(c);
    c = lerp(c, c * c * (3.0 - 2.0 * c), 0.26);      // stronger S-curve: heavy mass / 3 value bands (art bible)

    luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    float3 grey = float3(luma, luma, luma);
    float cyanW = saturate(min(c.g, c.b) - c.r);    // cyan-ish: green & blue above red
    float magW  = saturate(min(c.r, c.b) - c.g);    // magenta-ish: red & blue above green
    float neon  = saturate((cyanW + magW) * pc.gradeNeonGain);
    // Only PROTECT bright neon (emissive cores/orbs/strips), not mid-tone matte architecture:
    // the violet brutalist walls read as "magenta" to the detector and would get boosted into a
    // purple wash. Gating by luminance keeps the grounded world restrained while the gameplay
    // neon still pops.
    neon *= smoothstep(0.38, 0.72, luma);
    c = lerp(grey, c, lerp(pc.gradeEnvSat, pc.gradeNeonSat, neon));

    // M7 per-biome grade tint: a gentle frame-wide color cast so each sector reads as a distinct
    // place. Protect the neon (bright gameplay cyan/magenta) so only the world takes the cast.
    c *= lerp(pc.gradeTint, float3(1.0, 1.0, 1.0), neon);

    // Vignette for mood: gently darken toward the corners.
    c *= 1.0 - dot(dir, dir) * pc.vignette;

    // Film grain: subtle per-pixel noise (deterministic from pixel coords, so
    // headless captures stay stable), scaled down in highlights.
    float g = hash21(pos.xy) - 0.5;
    c += g * pc.grain * (1.0 - 0.6 * dot(c, float3(0.299, 0.587, 0.114)));
    return float4(saturate(c), 1.0);
}
