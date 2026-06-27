// W2a (Neon Ink Brutalism): screen-space ink outlines. A fullscreen pass that detects
// depth + normal discontinuities in the G-buffer and composites blue-black ink lines
// (alpha-blended) into the lit HDR before TAA, so silhouettes and creases read like a
// hand-inked graphic novel. Depth jumps make thick silhouettes; normal breaks make
// thinner interior creases. Sky-interior pixels get no ink; geometry silhouetted against
// the sky still gets an edge (the sky reads as the far plane).

struct Push {
    uint  depthIndex;
    uint  normalIndex;
    uint  materialIndex;      // RT2: A = viewmodel/hero flag (thicker, bolder ink on heroes)
    float invW, invH;
    float nearZ;
    float inkR, inkG, inkB;   // blue-black ink (linear HDR, composited pre-tonemap)
    float depthSense;         // silhouette sensitivity
    float normalSense;        // interior-crease sensitivity
    float thickness;          // sample offset in pixels
    float strength;           // max ink alpha
    float heroScale;          // multiplier on thickness + alpha for viewmodel (hero) pixels
};
ConstantBuffer<Push> pc : register(b0);
SamplerState sampPoint : register(s2);

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Reverse-Z linear view distance. d == 0 is no geometry (sky / far plane): map to a
// large distance so geometry silhouetted against the sky registers a strong depth edge.
float linDepth(float d, float nearZ) {
    if (d <= 1e-6) return 1e5;
    return nearZ / d;
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float>  depthTex  = ResourceDescriptorHeap[pc.depthIndex];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[pc.normalIndex];

    float dc = depthTex.SampleLevel(sampPoint, uv, 0).r;
    if (dc <= 1e-6) return float4(0, 0, 0, 0);   // interior sky: no ink

    // Hero (viewmodel) pixels carry RT2.A == 1 (gbuffer writes it for camera-space meshes).
    // Give the weapon a thicker, bolder ink line than the architecture (doc 5: weapons get
    // ~1.5-2 px, heroes thicker than environment), driven by heroScale. World pixels read 0.
    Texture2D<float4> materialTex = ResourceDescriptorHeap[pc.materialIndex];
    float heroFlag = materialTex.SampleLevel(sampPoint, uv, 0).a;
    float hero = (heroFlag > 0.5) ? max(pc.heroScale, 1.0) : 1.0;

    float2 o = float2(pc.invW, pc.invH) * (pc.thickness * hero);
    // 8-tap (4 axis + 4 diagonal): smoother, less aliased silhouettes than a 4-tap cross.
    float2 uN = uv + float2(0, -o.y),    uS = uv + float2(0, o.y);
    float2 uE = uv + float2(o.x, 0),     uW = uv + float2(-o.x, 0);
    float2 uNE = uv + float2(o.x, -o.y), uNW = uv + float2(-o.x, -o.y);
    float2 uSE = uv + float2(o.x, o.y),  uSW = uv + float2(-o.x, o.y);

    float lc = linDepth(dc, pc.nearZ);
    float ln = linDepth(depthTex.SampleLevel(sampPoint, uN, 0).r, pc.nearZ);
    float ls = linDepth(depthTex.SampleLevel(sampPoint, uS, 0).r, pc.nearZ);
    float le = linDepth(depthTex.SampleLevel(sampPoint, uE, 0).r, pc.nearZ);
    float lw = linDepth(depthTex.SampleLevel(sampPoint, uW, 0).r, pc.nearZ);
    float lne = linDepth(depthTex.SampleLevel(sampPoint, uNE, 0).r, pc.nearZ);
    float lnw = linDepth(depthTex.SampleLevel(sampPoint, uNW, 0).r, pc.nearZ);
    float lse = linDepth(depthTex.SampleLevel(sampPoint, uSE, 0).r, pc.nearZ);
    float lsw = linDepth(depthTex.SampleLevel(sampPoint, uSW, 0).r, pc.nearZ);
    // Scale-invariant relative depth gradient (axis full weight, diagonals 0.7): big jumps are silhouettes.
    float depthEdge = ((abs(ln - lc) + abs(ls - lc) + abs(le - lc) + abs(lw - lc))
                     + 0.7 * (abs(lne - lc) + abs(lnw - lc) + abs(lse - lc) + abs(lsw - lc))) / max(lc, 1e-3);

    float3 nc = normalize(normalTex.SampleLevel(sampPoint, uv, 0).rgb * 2.0 - 1.0);
    float3 nn = normalize(normalTex.SampleLevel(sampPoint, uN, 0).rgb * 2.0 - 1.0);
    float3 ns = normalize(normalTex.SampleLevel(sampPoint, uS, 0).rgb * 2.0 - 1.0);
    float3 ne = normalize(normalTex.SampleLevel(sampPoint, uE, 0).rgb * 2.0 - 1.0);
    float3 nw = normalize(normalTex.SampleLevel(sampPoint, uW, 0).rgb * 2.0 - 1.0);
    float3 nne = normalize(normalTex.SampleLevel(sampPoint, uNE, 0).rgb * 2.0 - 1.0);
    float3 nnw = normalize(normalTex.SampleLevel(sampPoint, uNW, 0).rgb * 2.0 - 1.0);
    float3 nse = normalize(normalTex.SampleLevel(sampPoint, uSE, 0).rgb * 2.0 - 1.0);
    float3 nsw = normalize(normalTex.SampleLevel(sampPoint, uSW, 0).rgb * 2.0 - 1.0);
    float normalEdge = (1.0 - saturate(dot(nc, nn))) + (1.0 - saturate(dot(nc, ns)))
                     + (1.0 - saturate(dot(nc, ne))) + (1.0 - saturate(dot(nc, nw)))
                     + 0.7 * ((1.0 - saturate(dot(nc, nne))) + (1.0 - saturate(dot(nc, nnw)))
                            + (1.0 - saturate(dot(nc, nse))) + (1.0 - saturate(dot(nc, nsw))));

    float edge = saturate(depthEdge * pc.depthSense + normalEdge * pc.normalSense);
    edge = smoothstep(0.25, 0.75, edge);
    // Distance fade: ease the ink out in the far field so distant geometry does not collapse into a
    // dense shimmery line-mesh (lc is linear view distance; near silhouettes stay crisp).
    float distFade = saturate(1.0 - (lc - 35.0) / 45.0);
    return float4(pc.inkR, pc.inkG, pc.inkB, saturate(edge * pc.strength * hero * distFade));
}
