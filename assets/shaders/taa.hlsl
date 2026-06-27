// M1 Temporal anti-aliasing (the plan's primary AA, replacing FXAA). Reprojects
// the previous resolved frame through the gbuffer velocity buffer, clips it to the
// current 3x3 neighbourhood colour AABB (YCoCg variance clipping) to reject stale
// history at disocclusions, and blends. Output is HDR linear and becomes next
// frame's history (the engine ping-pongs two history targets). The camera
// projection is jittered per frame (Halton 2,3) so accumulation resolves sub-pixel
// detail.

struct Push {
    uint  curIndex;      // current resolved HDR (this frame's lighting)
    uint  histIndex;     // previous frame's TAA output (history)
    uint  velIndex;      // gbuffer velocity (curUV - prevUV)
    uint  depthIndex;    // scene depth (reverse-Z; 0 = background)
    float invW;
    float invH;
    uint  historyValid;  // 0 on the first frame / after resize -> no history blend
    float blend;         // history weight when stable (e.g. 0.92)
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s1);   // s1 = linear-clamp
SamplerState sampPoint  : register(s2);   // s2 = point-wrap

float3 rgbToYCoCg(float3 c) {
    return float3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                   0.5  * c.r - 0.5  * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 ycoCgToRgb(float3 c) {
    float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

// Clip the history colour to the AABB of the current neighbourhood (in YCoCg),
// moving it along the line toward the box centre (Karis / Lottes style).
float3 clipAABB(float3 aabbMin, float3 aabbMax, float3 hist) {
    float3 center = 0.5 * (aabbMax + aabbMin);
    float3 extent = 0.5 * (aabbMax - aabbMin) + 1e-5;
    float3 v = hist - center;
    float3 unit = v / extent;
    float3 a = abs(unit);
    float maxComp = max(a.x, max(a.y, a.z));
    if (maxComp > 1.0) return center + v / maxComp;
    return hist;
}

void VSMain(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> cur   = ResourceDescriptorHeap[pc.curIndex];
    Texture2D<float>  depth = ResourceDescriptorHeap[pc.depthIndex];

    float3 current = cur.SampleLevel(sampPoint, uv, 0).rgb;

    float d = depth.SampleLevel(sampPoint, uv, 0).r;
    // Background (no geometry) is flat clear colour: no AA needed and velocity is 0,
    // so accumulating it would ghost under camera rotation. Output current directly.
    if (d <= 0.0 || pc.historyValid == 0)
        return float4(current, 1.0);

    Texture2D<float2> vel = ResourceDescriptorHeap[pc.velIndex];
    float2 velocity = vel.SampleLevel(sampPoint, uv, 0).xy;
    float2 histUv = uv - velocity;
    if (any(histUv < 0.0) || any(histUv > 1.0))
        return float4(current, 1.0);    // reprojected off-screen -> disocclusion

    Texture2D<float4> hist = ResourceDescriptorHeap[pc.histIndex];
    float3 history = hist.SampleLevel(sampLinear, histUv, 0).rgb;

    // Neighbourhood colour statistics (current frame) in YCoCg for variance clip.
    float2 du = float2(pc.invW, 0.0);
    float2 dv = float2(0.0, pc.invH);
    float3 m1 = 0.0, m2 = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float3 c = rgbToYCoCg(cur.SampleLevel(sampPoint, uv + x * du + y * dv, 0).rgb);
            m1 += c; m2 += c * c;
        }
    const float N = 9.0;
    float3 mean = m1 / N;
    float3 var  = max(m2 / N - mean * mean, 0.0);
    float3 sigma = sqrt(var);
    const float gamma = 1.25;
    float3 aabbMin = mean - gamma * sigma;
    float3 aabbMax = mean + gamma * sigma;

    float3 histYCoCg = clipAABB(aabbMin, aabbMax, rgbToYCoCg(history));
    history = ycoCgToRgb(histYCoCg);

    // Reduce history weight under fast motion (longer velocity -> more current).
    float speed = length(velocity * float2(1.0 / pc.invW, 1.0 / pc.invH));
    float weight = pc.blend * saturate(1.0 - speed * 0.0025);

    float3 outc = lerp(current, history, weight);
    return float4(outc, 1.0);
}
