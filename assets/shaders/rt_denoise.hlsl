// M2 RT denoiser (SVGF-class spatiotemporal). The RT trace (rt_trace.hlsl) emits a
// low-sample, jittered diffuse-GI and specular-reflection signal. TAA alone only
// cleans the final composited image, so GI/reflections ghost and lag; this denoiser
// works on the GI and reflection signals directly, before the deferred resolve
// consumes them.
//
//   CSTemporal : reproject last frame's accumulated GI/reflection through the
//                gbuffer velocity buffer, neighbourhood-clamp it to reject stale
//                history at disocclusions, and blend (exponential moving average,
//                weight ramped by per-pixel history length). Writes the new history.
//   CSAtrous   : edge-stopping a-trous wavelet spatial filter (depth + normal
//                weights), run a few times with growing step size. Cleans the
//                residual noise the temporal pass leaves, without crossing edges.
//
// The temporal history (rgb = signal, a = history length) is persistent and
// ping-ponged by the engine; the a-trous iterations bounce through transient
// targets and the last one feeds the resolve. Headless single-frame captures run
// the spatial filter only (no temporal history), matching the TAA convention.

static const float kWorldDepthMax = 0.95;

float3 decodeNormal(float3 enc) { return normalize(enc * 2.0 - 1.0); }

// Linear view-space Z from the reverse-Z world depth (undo the [0,0.95] depth-range
// compression the gbuffer applies to keep the viewmodel in the near slice).
float linearZ(float d, float nearZ) {
    float ndcZ = max(d / kWorldDepthMax, 1e-6);
    return nearZ / ndcZ;
}

// ---------------------------------------------------------------- temporal pass --

struct TemporalPush {
    uint giCur;        // rtGi SRV (noisy, this frame)
    uint reflCur;      // rtRefl SRV (noisy, this frame)
    uint velIndex;     // gbuffer velocity SRV (curUV - prevUV)
    uint depthIndex;   // scene depth SRV (reverse-Z; 0 = background)
    uint giHistPrev;   // accumulated GI history SRV (rgb gi, a length)
    uint reflHistPrev; // accumulated reflection history SRV (rgb refl)
    uint giHistOut;    // RWTexture2D<float4> new GI history
    uint reflHistOut;  // RWTexture2D<float4> new reflection history
    uint width, height;
    uint historyValid; // 0 -> first frame / headless: ignore history
    float blendMin;    // floor on the current-frame weight (e.g. 0.1)
};
ConstantBuffer<TemporalPush> tc : register(b0);

SamplerState sampLinear : register(s1);   // s1 = linear-clamp

[numthreads(8, 8, 1)]
void CSTemporal(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= tc.width || tid.y >= tc.height) return;
    const uint2 px = tid.xy;
    Texture2D<float4>  giCurTex   = ResourceDescriptorHeap[tc.giCur];
    Texture2D<float4>  reflCurTex = ResourceDescriptorHeap[tc.reflCur];
    RWTexture2D<float4> giOut      = ResourceDescriptorHeap[tc.giHistOut];
    RWTexture2D<float4> reflOut    = ResourceDescriptorHeap[tc.reflHistOut];

    float3 curGi   = giCurTex[px].rgb;
    float3 curRefl = reflCurTex[px].rgb;

    float2 uv = (float2(px) + 0.5) / float2(tc.width, tc.height);
    bool valid = (tc.historyValid != 0);
    float2 histUv = uv;
    if (valid) {
        Texture2D<float2> velTex = ResourceDescriptorHeap[tc.velIndex];
        float2 velocity = velTex[px].xy;
        histUv = uv - velocity;
        if (any(histUv < 0.0) || any(histUv > 1.0)) valid = false;
    }

    if (!valid) {
        giOut[px]   = float4(curGi, 1.0);
        reflOut[px] = float4(curRefl, 1.0);
        return;
    }

    Texture2D<float4> giHist   = ResourceDescriptorHeap[tc.giHistPrev];
    Texture2D<float4> reflHist = ResourceDescriptorHeap[tc.reflHistPrev];
    float4 hg = giHist.SampleLevel(sampLinear, histUv, 0);
    float3 histGi   = hg.rgb;
    float  histLen  = hg.a;
    float3 histRefl = reflHist.SampleLevel(sampLinear, histUv, 0).rgb;

    // Neighbourhood clamp (reject ghosting at disocclusions / moving content): bound
    // the reprojected history to the AABB of the current 3x3 signal. Cheap and needs
    // no previous-frame gbuffer (the depth/normal disocclusion proxy is the clamp).
    float3 giMin = curGi, giMax = curGi, rfMin = curRefl, rfMax = curRefl;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 q = clamp(int2(px) + int2(x, y), int2(0, 0), int2(tc.width - 1, tc.height - 1));
            float3 g = giCurTex[q].rgb;
            float3 r = reflCurTex[q].rgb;
            giMin = min(giMin, g); giMax = max(giMax, g);
            rfMin = min(rfMin, r); rfMax = max(rfMax, r);
        }
    // A little slack so converged flat regions are not clamped to single-sample noise.
    float3 giExp = (giMax - giMin) * 0.25 + 1e-4;
    float3 rfExp = (rfMax - rfMin) * 0.25 + 1e-4;
    histGi   = clamp(histGi,   giMin - giExp, giMax + giExp);
    histRefl = clamp(histRefl, rfMin - rfExp, rfMax + rfExp);

    float alpha = max(1.0 / (histLen + 1.0), tc.blendMin);
    float3 outGi   = lerp(histGi,   curGi,   alpha);
    float3 outRefl = lerp(histRefl, curRefl, alpha);
    float outLen = min(histLen + 1.0, 64.0);

    giOut[px]   = float4(outGi, outLen);
    reflOut[px] = float4(outRefl, outLen);
}

// ------------------------------------------------------------------ a-trous pass --

struct AtrousPush {
    uint giIn, reflIn;       // SRVs (signal to filter)
    uint depthIndex;         // scene depth SRV
    uint normalIndex;        // gbuffer world-normal SRV
    uint giOut, reflOut;     // UAVs (filtered signal)
    uint width, height;
    uint stepSize;           // a-trous hole size: 1, 2, 4, ...
    float nearZ;
    float phiNormal;         // normal edge-stop exponent
    float phiDepth;          // depth edge-stop sigma (relative)
};
ConstantBuffer<AtrousPush> ac : register(b0);

[numthreads(8, 8, 1)]
void CSAtrous(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= ac.width || tid.y >= ac.height) return;
    const int2 px = int2(tid.xy);
    Texture2D<float4>  giInTex   = ResourceDescriptorHeap[ac.giIn];
    Texture2D<float4>  reflInTex = ResourceDescriptorHeap[ac.reflIn];
    Texture2D<float>   depthTex  = ResourceDescriptorHeap[ac.depthIndex];
    Texture2D<float4>  normalTex = ResourceDescriptorHeap[ac.normalIndex];
    RWTexture2D<float4> giOut     = ResourceDescriptorHeap[ac.giOut];
    RWTexture2D<float4> reflOut    = ResourceDescriptorHeap[ac.reflOut];

    float dC = depthTex[px].r;
    float4 giC = giInTex[px];
    float4 rfC = reflInTex[px];
    if (dC <= 0.0) {                 // background: nothing to filter
        giOut[px]   = giC;
        reflOut[px] = rfC;
        return;
    }
    float3 nC = decodeNormal(normalTex[px].rgb);
    float  zC = linearZ(dC, ac.nearZ);

    // 5-tap separable-ish a-trous weights (1, 2/3, 1/6 along each axis via the
    // classic B3 spline kernel sampled at the hole positions).
    const float kernel[3] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };

    float3 sumGi = giC.rgb;
    float3 sumRefl = rfC.rgb;
    float sumW = 1.0;
    int step = int(ac.stepSize);
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) {
            if (x == 0 && y == 0) continue;
            int2 q = px + int2(x, y) * step;
            if (q.x < 0 || q.y < 0 || q.x >= int(ac.width) || q.y >= int(ac.height)) continue;
            float dT = depthTex[q].r;
            if (dT <= 0.0) continue;
            float3 nT = decodeNormal(normalTex[q].rgb);
            float  zT = linearZ(dT, ac.nearZ);

            float wKernel = kernel[abs(x)] * kernel[abs(y)];
            float wN = pow(max(dot(nC, nT), 0.0), ac.phiNormal);
            float wZ = exp(-abs(zC - zT) / (ac.phiDepth * max(zC, 1e-3)));
            float w = wKernel * wN * wZ;

            sumGi   += giInTex[q].rgb * w;
            sumRefl += reflInTex[q].rgb * w;
            sumW    += w;
        }

    giOut[px]   = float4(sumGi / max(sumW, 1e-4), giC.a);
    reflOut[px] = float4(sumRefl / max(sumW, 1e-4), rfC.a);
}
