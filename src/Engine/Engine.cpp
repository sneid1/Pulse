#include "Engine/Engine.hpp"
#include "Engine/RHI/Readback.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulse {

using namespace pulse::rhi;
using namespace pulse::render;

namespace {

constexpr DXGI_FORMAT kHdrFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kLdrFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32_TYPELESS;
constexpr DXGI_FORMAT kDepthDsv    = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kDepthSrv    = DXGI_FORMAT_R32_FLOAT;

// GBuffer channel formats (must match assets/shaders/gbuffer.hlsl MRT outputs).
constexpr DXGI_FORMAT kGbAlbedoFmt   = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kGbNormalFmt   = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kGbMaterialFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kGbEmissiveFmt = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr DXGI_FORMAT kGbVelocityFmt = DXGI_FORMAT_R16G16_FLOAT;   // screen-space motion vectors

constexpr uint32_t kShadowMapSize = 2048;   // must match kShadowTexel in resolve.hlsl
constexpr DXGI_FORMAT kAoFmt = DXGI_FORMAT_R8_UNORM;

// Clustered light culling froxel grid (must match cluster_cull.hlsl + resolve.hlsl).
constexpr uint32_t kClusterX = 16, kClusterY = 9, kClusterZ = 24;
constexpr uint32_t kClusterCount = kClusterX * kClusterY * kClusterZ;
constexpr uint32_t kClusterMaxPerCell = 64;

// Volumetric fog froxel volume (must match fog_inject/fog_integrate/resolve.hlsl).
constexpr uint32_t kFogX = 160, kFogY = 90, kFogZ = 64;
constexpr DXGI_FORMAT kFogFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr float kFogFar = 60.0f;

// GPU-side structs (must match the HLSL in assets/shaders/).
struct FrameCB {
    Mat4  viewProj;                           // jittered (raster)
    Mat4  viewProjCam;                        // jittered (raster, camera-space)
    Vec3f sunDir;     float sunIntensity;
    Vec3f sunColor;   float ambient;
    float exposure;   Vec3f clearColor;
    Vec3f fogColor;   float fogDensity;
    float nearZ;      float pad0, pad1, pad2;
    Mat4  invViewProj;                        // world position reconstruction (UNjittered)
    Vec3f cameraPos;  uint32_t lightCount;
    Mat4  sunViewProj;                        // sun shadow map projection
    // TAA reprojection: unjittered current/previous view-projections so motion
    // vectors capture only true scene/camera motion (not the sub-pixel jitter).
    Mat4  viewProjNoJitter;
    Mat4  prevViewProjNoJitter;
    Mat4  viewProjCamNoJitter;
    Mat4  prevViewProjCamNoJitter;
    // Clustered light culling: froxel grid dims + log-Z far plane.
    uint32_t clusterDimX, clusterDimY, clusterDimZ, clusterMaxPerCell;
    // Volumetric fog froxel volume dims + far distance.
    float    zFar;      uint32_t fogDimX, fogDimY, fogDimZ;
    float    fogFar;    float fpad0, fpad1, fpad2;
    Vec4f    styleBands;   // W1: x=bandShadow y=bandLit z=softness w=stylize
    Vec4f    skyZenith;    // W4: rgb overhead (+ pad)
    Vec4f    skyHorizon;   // W4: rgb horizon + .w = skyStrength
    Vec4f    styleHatch;   // doc5: x=strength y=worldScale z=width w=fadeDist (0 = off)
};
struct LightData {
    Vec3f position;  float intensity;
    Vec3f color;     float radius;
};
struct DecalData {                          // must match decal.hlsl DecalData (80B)
    Vec3f center;  float halfDepth;
    Vec3f normal;  float halfWidth;
    Vec3f tangent; float halfHeight;
    Vec3f color;   float alpha;
    uint32_t kind; float pad0, pad1, pad2;
};
struct ParticleData {                       // must match particle.hlsl Particle (48B)
    Vec3f center;   float size;
    Vec3f color;    float emissive;
    Vec3f velocity; float stretch;
};
struct HeatData {                           // must match particle.hlsl HeatSource (32B)
    Vec3f center;   float radius;
    float strength; float pad0, pad1, pad2;
};
struct InstanceData {
    Mat4     model;
    Mat4     prevModel;                       // previous-frame model (motion vectors)
    Vec4f    baseColorFactor;
    uint32_t baseTex;
    float    emissive;
    float    metallic;
    float    roughness;
    uint32_t cameraSpace;
    uint32_t normalTex;
    uint32_t ormTex;
    float    uvScale;
    uint32_t vbIndex;     // mesh vertex structured-buffer SRV (RT hit shading)
    uint32_t ibIndex;     // mesh index structured-buffer SRV (RT hit shading)
    float    metalScale;  // multiplies metalness (1 = unchanged)
    float    roughBoost;  // added to roughness, clamped (0 = unchanged)
    float    rimColor[3]; // neon-ink fresnel rim emissive (HDR; 0 = none)
    float    rimPower;    // rim falloff exponent (0 = rim off)
    uint32_t emissiveTex; // per-texel emissive map SRV (0 = none)
    float    emissiveTexStrength; // HDR multiplier on the emissive map
};
struct GBufferPush { uint32_t frameCB, instanceSB, instanceIndex, vbIndex; };
struct ResolvePush { uint32_t frameCB, albedo, normal, material, emissive, depth, lights, shadow, ao, rtIndex, rtEnabled, giIndex, reflIndex, gridCount, gridIndices, fogVol; };
struct ClusterCullPush { uint32_t frameCB, lights, gridCountUav, gridIndicesUav; };
struct FogPush { uint32_t frameCB, srcOrDst, dst, lights; };
struct RtTracePush {
    uint32_t frameCB, depth, normal, material, albedo, tlas, instanceSB, shadowAoOut, giOut, reflOut;
    uint32_t width, height, frameIndex, rayCount; float aoRadius, giRange;
};
struct RtTemporalPush {
    uint32_t giCur, reflCur, velIndex, depthIndex, giHistPrev, reflHistPrev, giHistOut, reflHistOut;
    uint32_t width, height, historyValid; float blendMin;
};
struct RtAtrousPush {
    uint32_t giIn, reflIn, depthIndex, normalIndex, giOut, reflOut, width, height, stepSize;
    float nearZ, phiNormal, phiDepth;
};
struct PtTracePush {
    uint32_t frameCB, tlas, instanceSB, accumUav, lightsSB, width, height, sampleIndex, bounces, lightCount;
    uint32_t pad0, pad1, pad2, pad3, pad4;
};
struct PtResolvePush { uint32_t accumSrv; float exposure; uint32_t pad0, pad1; };
struct ShadowPush  { uint32_t frameCB, instanceSB, instanceIndex, vbIndex; };
struct SsaoPush    { uint32_t a0, a1, a2, a3; float e, f, g, h; };
struct GBufferVizPush { uint32_t srvIndex, mode, pad0, pad1; };
struct BloomPush { uint32_t srcIndex; float a, b; uint32_t pad0; float invSrcX, invSrcY; uint32_t pad1, pad2; };
struct TonemapPush { uint32_t hdrIndex; float exposure; uint32_t bloomIndex; float bloomIntensity; float invW, invH; float sunU, sunV, godray; float sharpen, vignette, grain, caScale; float gradeEnvSat, gradeNeonSat, gradeNeonGain; float gradeTintR, gradeTintG, gradeTintB; };
struct OutlinePush { uint32_t depth, normal, material; float invW, invH, nearZ, inkR, inkG, inkB, depthSense, normalSense, thickness, strength, heroScale; };
struct FxaaPush    { uint32_t srcIndex; float invW; float invH; uint32_t pad; };
struct TaaPush     { uint32_t curIndex, histIndex, velIndex, depthIndex; float invW, invH; uint32_t historyValid; float blend; };
struct SsrPush     { uint32_t hdrIndex, depthIndex, normalIndex, materialIndex, albedoIndex, frameCB, pad0, pad1; };
struct SsgiPush    { uint32_t hdrIndex, depthIndex, normalIndex, albedoIndex, materialIndex, frameCB; float strength; uint32_t pad0; };
struct DecalPush   { uint32_t frameCB, albedoIn, depthIndex, materialIndex, normalIndex, decalBuf, decalCount, pad; };
struct BlitPush    { uint32_t srcIndex, frameCB, heatBuf, heatCount; float time, aspect, pad0, pad1; };
struct ParticlePush{ uint32_t frameCB, particleBuf, count, pad0; float camRight[3]; float pad1; float camUp[3]; float pad2; };
struct DepthvizPush { uint32_t depthIndex; float nearZ; uint32_t pad0, pad1; };

float degToRad(float d) { return d * 3.14159265358979f / 180.0f; }

// Radical-inverse Halton sequence (index >= 1) for the TAA sub-pixel jitter.
float halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

} // namespace

void Engine::init(const Config& cfg) {
    width_ = cfg.width;
    height_ = cfg.height;
    windowed_ = cfg.hwnd != nullptr;

    ssgiEnabled_ = cfg.enableSsgi;
    ssrEnabled_ = cfg.enableSsr;
    rtRayCount_ = cfg.rtRayCount > 0 ? cfg.rtRayCount : 3;

    DeviceConfig dc;
    dc.enableDebugLayer = cfg.enableDebugLayer;
    dc.enableGpuValidation = cfg.enableGpuValidation;
    dc.forceRaster = cfg.forceRaster;
    device_ = Device::create(dc);

    heaps_.init(device_.d3d());
    rootSig_ = createBindlessRootSignature(device_.d3d());
    pipelines_.init(device_.d3d(), rootSig_.Get());
    shaderCompiler_.init();
    uploader_.init(&device_);
    graph_.init(&device_, &heaps_, rootSig_.Get());
    frameCmd_.init(device_.d3d(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.frame.cmd");

    // RT tier: stand up the DXR acceleration-structure manager. Selected from caps
    // (DXR 1.1 + full bindless); --force-raster turns it off for the raster path.
    rtEnabled_ = device_.caps().isRt();
    if (rtEnabled_) {
        accel_.init(&device_, &heaps_);
        ComputePipelineDesc cd;
        cd.cs = shaderCompiler_.compile("rt_trace.hlsl", L"CSMain", L"cs_6_6");
        rtTracePso_ = pipelines_.getCompute(cd);
        // RT denoiser (SVGF-class): temporal accumulation + edge-aware a-trous.
        ComputePipelineDesc tmp; tmp.cs = shaderCompiler_.compile("rt_denoise.hlsl", L"CSTemporal", L"cs_6_6");
        rtDenoiseTemporalPso_ = pipelines_.getCompute(tmp);
        ComputePipelineDesc atr; atr.cs = shaderCompiler_.compile("rt_denoise.hlsl", L"CSAtrous", L"cs_6_6");
        rtDenoiseAtrousPso_ = pipelines_.getCompute(atr);
        // M3 reference path tracer (RT tier): accumulate compute + AgX resolve.
        ComputePipelineDesc pt; pt.cs = shaderCompiler_.compile("pt_reference.hlsl", L"CSMain", L"cs_6_6");
        ptTracePso_ = pipelines_.getCompute(pt);
        GraphicsPipelineDesc ptr;
        ptr.vs = shaderCompiler_.compile("pt_reference.hlsl", L"ResolveVS", L"vs_6_6");
        ptr.ps = shaderCompiler_.compile("pt_reference.hlsl", L"ResolvePS", L"ps_6_6");
        ptr.rtvFormats = { kLdrFormat };
        ptr.cullMode = D3D12_CULL_MODE_NONE;
        ptResolvePso_ = pipelines_.getGraphics(ptr);
    }
    {
        // Clustered light culling (both tiers): one compute thread per froxel.
        ComputePipelineDesc cd;
        cd.cs = shaderCompiler_.compile("cluster_cull.hlsl", L"CSMain", L"cs_6_6");
        clusterCullPso_ = pipelines_.getCompute(cd);
    }
    {
        // Volumetric fog: inject (per-froxel scatter) + integrate (front-to-back).
        ComputePipelineDesc inj; inj.cs = shaderCompiler_.compile("fog.hlsl", L"CSInject", L"cs_6_6");
        fogInjectPso_ = pipelines_.getCompute(inj);
        ComputePipelineDesc itg; itg.cs = shaderCompiler_.compile("fog.hlsl", L"CSIntegrate", L"cs_6_6");
        fogIntegratePso_ = pipelines_.getCompute(itg);
    }

    if (windowed_) swapchain_.init(device_, cfg.hwnd, width_, height_);

    // Pipelines.
    {
        // GBuffer fill: 5 MRTs + depth. Back-face culling restored (M1.0b): world
        // geometry (RH view) and camera-space viewmodels (LH projection) have
        // opposite screen winding, so each draws with its own front-face convention
        // (two PSOs, selected per group in the gbuffer pass). All content now shares
        // one source winding after objToGpu (PulseGame reverses OBJ winding to match
        // the procedural meshes).
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("gbuffer.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("gbuffer.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kGbAlbedoFmt, kGbNormalFmt, kGbMaterialFmt, kGbEmissiveFmt, kGbVelocityFmt };
        d.dsvFormat = kDepthDsv;
        // World renders TWO-SIDED (cull none): the Quaternius MegaKit + procedural boxes have many
        // single-sided / inconsistently-wound panels, which under back-face culling vanish or read
        // inside-out from the room interior. gbuffer.hlsl flips the shading normal on back faces
        // (SV_IsFrontFace) so two-sided surfaces stay correctly lit. The viewmodel is a closed mesh
        // and keeps back-face culling under its own PSO.
        d.cullMode = D3D12_CULL_MODE_NONE;
        d.frontCCW = false;          // world: RH view+proj flips CCW source to CW screen
        d.depthTest = true; d.depthWrite = true;
        d.depthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reverse-Z
        gbufferPso_ = pipelines_.getGraphics(d);
        d.frontCCW = true;           // viewmodel: LH camera projection flips winding back
        d.cullMode = D3D12_CULL_MODE_BACK;   // viewmodel stays single-sided (closed mesh)
        gbufferVmPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("resolve.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("resolve.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        resolvePso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("gbufferviz.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("gbufferviz.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kLdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        gbuffervizPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("bloom.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("bloom.hlsl", L"DownPS", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        bloomDownPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("bloom.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("bloom.hlsl", L"BlurPS", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        bloomBlurPso_ = pipelines_.getGraphics(d);
    }
    {
        // Sun shadow map: depth-only (no PS), standard depth (clear 1, LESS).
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("shadow.hlsl", L"VSMain", L"vs_6_6");
        d.dsvFormat = kDepthDsv;
        d.cullMode = D3D12_CULL_MODE_NONE;
        d.depthTest = true; d.depthWrite = true;
        d.depthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadowPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("ssao.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("ssao.hlsl", L"AOPass", L"ps_6_6");
        d.rtvFormats = { kAoFmt };
        d.cullMode = D3D12_CULL_MODE_NONE;
        ssaoPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("ssao.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("ssao.hlsl", L"BlurPass", L"ps_6_6");
        d.rtvFormats = { kAoFmt };
        d.cullMode = D3D12_CULL_MODE_NONE;
        ssaoBlurPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("tonemap.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("tonemap.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kLdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        tonemapPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("fxaa.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("fxaa.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kLdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        fxaaPso_ = pipelines_.getGraphics(d);
    }
    {
        // W2a (Neon Ink Brutalism): screen-space ink outlines, alpha-blended into the
        // lit HDR before TAA. Same HDR format as the lit target it composites into.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("outline.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("outline.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        d.blendAlpha = true;
        outlinePso_ = pipelines_.getGraphics(d);
    }
    {
        // TAA resolve: reproject + neighbourhood-clip the HDR history. Output is HDR
        // (becomes next frame's history), so it targets the HDR format.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("taa.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("taa.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        taaPso_ = pipelines_.getGraphics(d);
    }
    {
        // SSR: raster-tier screen-space reflections (HDR in/out). RT tier uses RT
        // reflections instead, so this pass is only added on the raster tier.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("ssr.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("ssr.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        ssrPso_ = pipelines_.getGraphics(d);
    }
    {
        // SSGI: raster-tier dynamic indirect bounce (M3). HDR in/out, raster tier only.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("ssgi.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("ssgi.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        ssgiPso_ = pipelines_.getGraphics(d);
    }
    {
        // Deferred decals: fullscreen pass projecting decals onto the gbuffer albedo
        // + material (scuffs roughness/metal so marks read on glossy surfaces).
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("decal.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("decal.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kGbAlbedoFmt, kGbMaterialFmt };
        d.cullMode = D3D12_CULL_MODE_NONE;
        decalPso_ = pipelines_.getGraphics(d);
    }
    {
        // Particle composite blit: copy the resolved scene into the composite target.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("particle.hlsl", L"BlitVS", L"vs_6_6");
        d.ps = shaderCompiler_.compile("particle.hlsl", L"BlitPS", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        particleBlitPso_ = pipelines_.getGraphics(d);
    }
    {
        // Additive particle billboards.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("particle.hlsl", L"PartVS", L"vs_6_6");
        d.ps = shaderCompiler_.compile("particle.hlsl", L"PartPS", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        d.blendAdd = true;
        particlePso_ = pipelines_.getGraphics(d);
    }
    {
        // Alpha-blended shadow-smoke billboards (dark, soft): dissolve enemy silhouettes into shadow.
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("particle.hlsl", L"PartSmokeVS", L"vs_6_6");
        d.ps = shaderCompiler_.compile("particle.hlsl", L"PartSmokePS", L"ps_6_6");
        d.rtvFormats = { kHdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        d.blendAlpha = true;
        smokePso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("depthviz.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("depthviz.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kLdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        depthvizPso_ = pipelines_.getGraphics(d);
    }
    {
        GraphicsPipelineDesc d;
        d.vs = shaderCompiler_.compile("ui.hlsl", L"VSMain", L"vs_6_6");
        d.ps = shaderCompiler_.compile("ui.hlsl", L"PSMain", L"ps_6_6");
        d.rtvFormats = { kLdrFormat };
        d.cullMode = D3D12_CULL_MODE_NONE;
        d.blendAlpha = true;
        uiPso_ = pipelines_.getGraphics(d);
    }

    font_.bake(heaps_, uploader_, 18);

    // Persistent sun shadow map (resolution-independent, kept out of the transient
    // alias pool). Optimised clear value 1.0 matches the graph's DSV clear (LESS).
    {
        const float dclear[4] = { 1.0f, 0, 0, 0 };
        shadowMap_ = createTexture2D(device_.d3d(), kShadowMapSize, kShadowMapSize, kDepthTypeless,
            TextureUsage::DepthStencil, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1, dclear, L"pulse.shadowmap");
    }

    // Persistent per-frame upload buffers (one frame in flight; serialized by the
    // per-frame flush).
    frameCB_ = createUploadBuffer(device_.d3d(), sizeof(FrameCB) + 255 & ~size_t(255), L"pulse.frameCB");
    frameCBIndex_ = heaps_.createConstantBufferView(frameCB_.get(), sizeof(FrameCB));

    createTargets();
}

void Engine::createTargets() {
    const float clear[4] = { 0, 0, 0, 1 };
    ldr_ = createTexture2D(device_.d3d(), width_, height_, kLdrFormat,
        TextureUsage::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, clear, L"pulse.ldr");

    // TAA history ping-pong (HDR). Both rest in PIXEL_SHADER_RESOURCE between frames
    // (last touched as an SRV); the graph transitions whichever is written this
    // frame to RENDER_TARGET and back. Initialised to black so the first frame's
    // history read is well-defined (and ignored via historyValid_ = false).
    const float black[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 2; ++i)
        history_[i] = createTexture2D(device_.d3d(), width_, height_, kHdrFormat,
            TextureUsage::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, black,
            i == 0 ? L"pulse.history0" : L"pulse.history1");
    historyValid_ = false;

    // RT denoiser accumulation ping-pong (RT tier). UAV-written by the temporal pass
    // and compute-sampled by the a-trous pass, so they rest in NON_PIXEL_SHADER_
    // RESOURCE between frames; RenderTarget usage is carried only so they can be
    // cleared once here via an RTV (same idiom as the TAA history above).
    for (int i = 0; i < 2; ++i) {
        giHist_[i] = createTexture2D(device_.d3d(), width_, height_, kHdrFormat,
            TextureUsage::RenderTarget | TextureUsage::UnorderedAccess,
            D3D12_RESOURCE_STATE_RENDER_TARGET, 1, black,
            i == 0 ? L"pulse.giHist0" : L"pulse.giHist1");
        reflHist_[i] = createTexture2D(device_.d3d(), width_, height_, kHdrFormat,
            TextureUsage::RenderTarget | TextureUsage::UnorderedAccess,
            D3D12_RESOURCE_STATE_RENDER_TARGET, 1, black,
            i == 0 ? L"pulse.reflHist0" : L"pulse.reflHist1");
    }

    ID3D12GraphicsCommandList* list = frameCmd_.begin();
    rhi::Texture* clearTargets[] = { &history_[0], &history_[1], &giHist_[0], &giHist_[1],
                                     &reflHist_[0], &reflHist_[1] };
    const D3D12_RESOURCE_STATES restStates[] = {
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    D3D12_RESOURCE_BARRIER toSrv[6];
    for (int i = 0; i < 6; ++i) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heaps_.createRtv(*clearTargets[i]);
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        toSrv[i] = CD3DX12_RESOURCE_BARRIER::Transition(clearTargets[i]->get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, restStates[i]);
        clearTargets[i]->state = restStates[i];
    }
    list->ResourceBarrier(6, toSrv);
    frameCmd_.close();
    ID3D12CommandList* lists[] = { list };
    device_.graphicsQueue()->ExecuteCommandLists(1, lists);
    device_.flushGraphics();
}

void Engine::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) return;
    graph_.releaseResources();
    width_ = width; height_ = height;
    if (windowed_) swapchain_.resize(width, height);
    framePrevValid_ = false;
    historyValid_ = false;
    prevTransforms_.clear();
    frameIndex_ = 0;
    createTargets();   // graph recreates hdr/depth on the next build (shape change)
}

MeshHandle Engine::createMesh(const MeshData& data) {
    MeshEntry e;

    // Generate per-vertex tangents from positions + uv0 (normal mapping needs them;
    // the procedural arena meshes and OBJ assets don't ship tangents).
    std::vector<StaticVertex> verts(data.vertices.begin(), data.vertices.end());
    {
        const size_t n = verts.size();
        std::vector<Vec3f> tanAcc(n, { 0, 0, 0 });
        std::vector<Vec3f> bitAcc(n, { 0, 0, 0 });
        const auto& idx = data.indices;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            const uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
            if (i0 >= n || i1 >= n || i2 >= n) continue;
            const StaticVertex& v0 = verts[i0]; const StaticVertex& v1 = verts[i1]; const StaticVertex& v2 = verts[i2];
            const Vec3f e1 = v1.pos - v0.pos, e2 = v2.pos - v0.pos;
            const float du1x = v1.uv0[0] - v0.uv0[0], du1y = v1.uv0[1] - v0.uv0[1];
            const float du2x = v2.uv0[0] - v0.uv0[0], du2y = v2.uv0[1] - v0.uv0[1];
            const float det = du1x * du2y - du2x * du1y;
            if (std::fabs(det) < 1e-12f) continue;
            const float r = 1.0f / det;
            const Vec3f T = { (e1.x * du2y - e2.x * du1y) * r, (e1.y * du2y - e2.y * du1y) * r, (e1.z * du2y - e2.z * du1y) * r };
            const Vec3f B = { (e2.x * du1x - e1.x * du2x) * r, (e2.y * du1x - e1.y * du2x) * r, (e2.z * du1x - e1.z * du2x) * r };
            for (const uint32_t ii : { i0, i1, i2 }) { tanAcc[ii] = tanAcc[ii] + T; bitAcc[ii] = bitAcc[ii] + B; }
        }
        for (size_t i = 0; i < n; ++i) {
            const Vec3f nrm = verts[i].nrm;
            const Vec3f ortho = tanAcc[i] - nrm * dot3(nrm, tanAcc[i]);   // Gram-Schmidt
            const float len = std::sqrt(dot3(ortho, ortho));
            const Vec3f tang = len > 1e-6f ? ortho * (1.0f / len) : Vec3f{ 1, 0, 0 };
            const float w = (dot3(cross3(nrm, tang), bitAcc[i]) < 0.0f) ? -1.0f : 1.0f;
            verts[i].tangent = { tang.x, tang.y, tang.z, w };
        }
    }

    e.vb = uploader_.uploadBuffer(verts.data(), verts.size() * sizeof(StaticVertex),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false, L"mesh.vb");
    e.ib = uploader_.uploadBuffer(data.indices.data(), data.indices.size_bytes(),
        D3D12_RESOURCE_STATE_INDEX_BUFFER, false, L"mesh.ib");
    uploader_.flush();
    e.vbSrv = heaps_.createStructuredBufferSrv(e.vb.get(),
        static_cast<uint32_t>(verts.size()), sizeof(StaticVertex));
    e.vertexCount = static_cast<uint32_t>(verts.size());
    e.indexCount = static_cast<uint32_t>(data.indices.size());
    e.ibv = { e.ib.gpuAddress, static_cast<UINT>(data.indices.size_bytes()), DXGI_FORMAT_R32_UINT };

    // RT tier: build the static BLAS for this mesh and expose an index SRV (so the
    // inline ray hit-shading can fetch the triangle's vertices). The index buffer
    // rests in INDEX_BUFFER | NON_PIXEL_SHADER_RESOURCE so it serves both the raster
    // index-buffer bind and the RT structured read without a per-frame transition.
    if (rtEnabled_) {
        e.ibSrv = heaps_.createStructuredBufferSrv(e.ib.get(), e.indexCount, sizeof(uint32_t));
        ID3D12GraphicsCommandList* list = frameCmd_.begin();
        ComPtr<ID3D12GraphicsCommandList4> cmd4;
        PULSE_HR(list->QueryInterface(IID_PPV_ARGS(&cmd4)));
        const D3D12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(e.ib.get(),
            D3D12_RESOURCE_STATE_INDEX_BUFFER,
            D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toRead);
        rhi::Buffer scratch;
        e.blas = accel_.buildBlas(cmd4.Get(), e.vb, e.vertexCount, sizeof(StaticVertex),
                                  e.ib, e.indexCount, scratch);
        frameCmd_.close();
        ID3D12CommandList* lists[] = { list };
        device_.graphicsQueue()->ExecuteCommandLists(1, lists);
        device_.flushGraphics();   // scratch released after this scope
    }

    meshes_.push_back(std::move(e));
    return static_cast<MeshHandle>(meshes_.size());   // index + 1
}

MeshHandle Engine::createDynamicMesh(uint32_t maxVertices, std::span<const uint32_t> indices) {
    if (maxVertices == 0 || indices.empty()) return MeshHandle::Invalid;

    MeshEntry e;
    e.dynamic = true;
    e.vertexCapacity = maxVertices;
    e.vertexCount = maxVertices;
    e.indexCount = static_cast<uint32_t>(indices.size());
    e.vb = createUploadBuffer(device_.d3d(), static_cast<uint64_t>(maxVertices) * sizeof(StaticVertex), L"dynamic.mesh.vb");
    e.vbSrv = heaps_.createStructuredBufferSrv(e.vb.get(), maxVertices, sizeof(StaticVertex));
    e.ib = uploader_.uploadBuffer(indices.data(), indices.size_bytes(),
        D3D12_RESOURCE_STATE_INDEX_BUFFER, false, L"dynamic.mesh.ib");
    uploader_.flush();
    e.ibv = { e.ib.gpuAddress, static_cast<UINT>(indices.size_bytes()), DXGI_FORMAT_R32_UINT };

    meshes_.push_back(std::move(e));
    return static_cast<MeshHandle>(meshes_.size());
}

bool Engine::updateDynamicMesh(MeshHandle mesh, std::span<const StaticVertex> vertices) {
    if (mesh == MeshHandle::Invalid) return false;
    const uint32_t index = static_cast<uint32_t>(mesh);
    if (index == 0 || index > meshes_.size()) return false;
    MeshEntry& e = meshes_[index - 1];
    if (!e.dynamic || !e.vb.mapped || vertices.size() > e.vertexCapacity) return false;
    if (!vertices.empty()) {
        std::memcpy(e.vb.mapped, vertices.data(), vertices.size_bytes());
    }
    e.vertexCount = static_cast<uint32_t>(vertices.size());
    return true;
}

TextureHandle Engine::createTexture(const TextureData& data) {
    TexEntry e;
    // Leave static, upload-once textures in COMMON: D3D12 implicitly promotes them
    // to PIXEL_SHADER_RESOURCE on read and decays them back, which avoids the
    // cross-command-list explicit-state layout tracking that GPU-based validation
    // flags ("incompatible barrier layout: LEGACY_COPY_DEST" on a bindless SRV read).
    const DXGI_FORMAT texFmt = data.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : kLdrFormat;
    e.tex = uploader_.uploadTexture2D(data.rgba, data.width, data.height, texFmt,
        D3D12_RESOURCE_STATE_COMMON, L"tex");
    uploader_.flush();
    e.srv = heaps_.createTextureSrv(e.tex);
    textures_.push_back(std::move(e));
    return static_cast<TextureHandle>(textures_.size());
}

TextureHandle Engine::createTextureDDS(const std::string& path) {
    TexEntry e;
    e.tex = uploader_.uploadDDS(path, L"dds");
    if (!e.tex) return TextureHandle::Invalid;
    uploader_.flush();
    e.srv = heaps_.createTextureSrv(e.tex);   // BCn format + baked mips
    textures_.push_back(std::move(e));
    return static_cast<TextureHandle>(textures_.size());
}

MaterialHandle Engine::createMaterial(const MaterialDesc& desc) {
    MatEntry e;
    e.baseTexSrv = (desc.baseColor != TextureHandle::Invalid)
        ? textures_[static_cast<uint32_t>(desc.baseColor) - 1].srv : 0;
    e.factor = desc.baseColorFactor;
    e.emissive = desc.emissive;
    e.metallic = desc.metallic;
    e.roughness = desc.roughness;
    e.uvScale = desc.uvScale;
    e.normalSrv = (desc.normal != TextureHandle::Invalid)
        ? textures_[static_cast<uint32_t>(desc.normal) - 1].srv : 0;
    e.ormSrv = (desc.orm != TextureHandle::Invalid)
        ? textures_[static_cast<uint32_t>(desc.orm) - 1].srv : 0;
    e.emissiveSrv = (desc.emissiveTex != TextureHandle::Invalid)
        ? textures_[static_cast<uint32_t>(desc.emissiveTex) - 1].srv : 0;
    e.emissiveStrength = desc.emissiveTexStrength;
    e.metalScale = desc.metalScale;
    e.roughBoost = desc.roughBoost;
    e.styleFlags = desc.styleFlags;
    materials_.push_back(e);
    return static_cast<MaterialHandle>(materials_.size());
}

void Engine::updateFrameData(const SceneFrame& frame) {
    ++frameIndex_;
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Camera& cam = frame.camera;

    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float cy = std::cos(cam.yaw),   sy = std::sin(cam.yaw);
    // Matches the game's fromAngle(yaw)=(cos,sin) in the XZ plane (yaw=0 -> +X).
    const Vec3f forward = normalize3({ cp * cy, sp, cp * sy });
    // Camera right/up basis for billboarding world-space particles.
    cameraRight_ = normalize3(cross3(forward, { 0, 1, 0 }));
    cameraUp_ = cross3(cameraRight_, forward);
    const Mat4 view = lookAtRH(cam.position, cam.position + forward, { 0, 1, 0 });
    const float worldFov = std::clamp(cam.fovDeg, 45.0f, 130.0f);
    const float viewmodelFov = std::clamp(cam.viewmodelFovDeg > 0.0f ? cam.viewmodelFovDeg : cam.fovDeg, 45.0f, 135.0f);
    const Mat4 projNoJitter = perspectiveReverseZRH(degToRad(worldFov), aspect, cam.nearZ);
    // Camera-space instances (first-person weapon/hands) skip the view transform
    // and are positioned at +z forward, so they use a LEFT-handed projection
    // (the world is right-handed). Give them their own FOV so a wide gameplay
    // camera does not stretch the weapon into a screen-edge slab.
    const Mat4 projCamNoJitter = perspectiveReverseZ(degToRad(viewmodelFov), aspect, cam.nearZ);

    // Sub-pixel jitter (Halton 2,3 over an 8-frame cycle) so TAA accumulation
    // resolves sub-pixel detail. Applied to the projection's z->xy clip term only:
    // depth (clip.z/clip.w) is untouched, so depth-based reconstruction stays exact.
    // Windowed playback only; headless captures stay crisp + deterministic.
    Mat4 proj = projNoJitter;
    Mat4 projCam = projCamNoJitter;
    if (windowed_) {
        const uint32_t j = (frameIndex_ % 8u) + 1u;
        const float jx = (halton(j, 2) - 0.5f) * 2.0f / static_cast<float>(width_);
        const float jy = (halton(j, 3) - 0.5f) * 2.0f / static_cast<float>(height_);
        proj.m[2][0] = -jx;  proj.m[2][1] = -jy;     // RH reverse-Z: clip.w = -view.z
        projCam.m[2][0] = jx; projCam.m[2][1] = jy;  // LH reverse-Z: clip.w = +view.z
    }
    const Mat4 viewProjNoJitter = mul(view, projNoJitter);

    FrameCB cb{};
    cb.viewProj = mul(view, proj);          // jittered (raster)
    cb.viewProjCam = projCam;               // jittered (raster, camera-space)
    cb.sunDir = frame.sun.direction;
    cb.sunIntensity = frame.sun.intensity;
    cb.sunColor = frame.sun.color;
    cb.ambient = frame.sun.ambient;
    cb.exposure = frame.post.exposure;
    cb.clearColor = frame.clearColor;     // resolve uses it for background pixels
    // Distance + volumetric fog: fade toward the background colour for atmospheric depth.
    // Density comes from the frame (PostParams) so a dark indoor arena can stay moody while an
    // open outdoor scene runs near-clear air; callers that do not set it get the original 0.035.
    cb.fogColor = frame.clearColor;
    cb.fogDensity = frame.post.fogDensity;
    cb.nearZ = cam.nearZ;
    cb.invViewProj = inverse(viewProjNoJitter);  // resolve reconstructs world pos from depth (UNjittered)
    cb.cameraPos = cam.position;
    cb.lightCount = static_cast<uint32_t>(frame.lights.size());

    // Screen-space god rays are disabled for the current arena look. The radial tonemap pass
    // cannot distinguish sky from bright indoor emissive strips, so it smeared cyan fixtures into
    // full-screen veils over the viewmodel and enemies.
    {
        godrayAmt_ = 0.0f;
        sunScreenU_ = 0.5f;
        sunScreenV_ = 0.5f;
    }

    // Sun shadow map projection: an orthographic frustum centred on the player,
    // looking along the sun direction. Covers the play area around the camera.
    const Vec3f sunDir = normalize3(frame.sun.direction);
    const float kSunDist = 40.0f;
    const Vec3f focus = cam.position;
    const Vec3f lightPos = focus - sunDir * kSunDist;
    const Mat4 sunView = lookAtLH(lightPos, focus, { 0, 1, 0 });
    const Mat4 sunProj = orthographic(48.0f, 48.0f, 1.0f, 90.0f);
    cb.sunViewProj = mul(sunView, sunProj);

    // TAA reprojection matrices (unjittered): current + previous frame. On the
    // first frame prev == current so motion vectors start at zero.
    cb.viewProjNoJitter = viewProjNoJitter;
    cb.viewProjCamNoJitter = projCamNoJitter;
    cb.prevViewProjNoJitter = framePrevValid_ ? prevViewProjNoJitter_ : viewProjNoJitter;
    cb.prevViewProjCamNoJitter = framePrevValid_ ? prevViewProjCamNoJitter_ : projCamNoJitter;
    cb.clusterDimX = kClusterX; cb.clusterDimY = kClusterY; cb.clusterDimZ = kClusterZ;
    cb.clusterMaxPerCell = kClusterMaxPerCell;
    cb.zFar = cam.farZ;
    cb.fogDimX = kFogX; cb.fogDimY = kFogY; cb.fogDimZ = kFogZ;
    cb.fogFar = kFogFar;
    // Neon Ink Brutalism art-direction params (W1 bands now; W4 sky later).
    cb.styleBands = { frame.style.bandShadow, frame.style.bandLit, frame.style.bandSoftness, frame.style.stylize };
    cb.skyZenith  = { frame.style.skyZenith.x, frame.style.skyZenith.y, frame.style.skyZenith.z, 0.0f };
    cb.skyHorizon = { frame.style.skyHorizon.x, frame.style.skyHorizon.y, frame.style.skyHorizon.z, frame.style.skyStrength };
    cb.styleHatch = { frame.style.hatchStrength, frame.style.hatchScale, frame.style.hatchWidth, frame.style.hatchFade };
    std::memcpy(frameCB_.mapped, &cb, sizeof(cb));

    prevViewProjNoJitter_ = viewProjNoJitter;
    prevViewProjCamNoJitter_ = projCamNoJitter;
    framePrevValid_ = true;

    // Point lights -> structured buffer (accumulated in the resolve).
    lightCount_ = static_cast<uint32_t>(frame.lights.size());
    if (lightCount_ > lightCapacity_) {
        const uint32_t cap = lightCount_ < 64 ? 64 : lightCount_ + lightCount_ / 2;
        lightBuf_ = createUploadBuffer(device_.d3d(), sizeof(LightData) * cap, L"pulse.lights");
        lightSrvIndex_ = heaps_.createStructuredBufferSrv(lightBuf_.get(), cap, sizeof(LightData));
        lightCapacity_ = cap;
    }
    if (lightCount_ > 0) {
        auto* dst = static_cast<LightData*>(lightBuf_.mapped);
        for (uint32_t i = 0; i < lightCount_; ++i) {
            const LocalLight& L = frame.lights[i];
            dst[i].position = L.position; dst[i].intensity = L.intensity;
            dst[i].color = L.color;       dst[i].radius = L.radius;
        }
    }

    // Projected decals -> structured buffer (consumed by the decal pass).
    decalCount_ = static_cast<uint32_t>(frame.decals.size());
    if (decalCount_ > decalCapacity_) {
        const uint32_t cap = decalCount_ < 64 ? 64 : decalCount_ + decalCount_ / 2;
        decalBuf_ = createUploadBuffer(device_.d3d(), sizeof(DecalData) * cap, L"pulse.decals");
        decalSrvIndex_ = heaps_.createStructuredBufferSrv(decalBuf_.get(), cap, sizeof(DecalData));
        decalCapacity_ = cap;
    }
    if (decalCount_ > 0) {
        auto* dst = static_cast<DecalData*>(decalBuf_.mapped);
        for (uint32_t i = 0; i < decalCount_; ++i) {
            const Decal& s = frame.decals[i];
            DecalData& d = dst[i];
            d.center = s.center;   d.halfDepth = s.halfDepth;
            d.normal = s.normal;   d.halfWidth = s.halfWidth;
            d.tangent = s.tangent; d.halfHeight = s.halfHeight;
            d.color = s.color;     d.alpha = s.alpha;
            d.kind = s.kind;       d.pad0 = d.pad1 = d.pad2 = 0.0f;
        }
    }

    // Particle billboards -> structured buffer (rendered additively into HDR).
    particleCount_ = static_cast<uint32_t>(frame.particles.size());
    if (particleCount_ > particleCapacity_) {
        const uint32_t cap = particleCount_ < 512 ? 512 : particleCount_ + particleCount_ / 2;
        particleBuf_ = createUploadBuffer(device_.d3d(), sizeof(ParticleData) * cap, L"pulse.particles");
        particleSrvIndex_ = heaps_.createStructuredBufferSrv(particleBuf_.get(), cap, sizeof(ParticleData));
        particleCapacity_ = cap;
    }
    if (particleCount_ > 0) {
        auto* dst = static_cast<ParticleData*>(particleBuf_.mapped);
        for (uint32_t i = 0; i < particleCount_; ++i) {
            const Particle& s = frame.particles[i];
            dst[i] = { s.center, s.size, s.color, s.emissive, s.velocity, s.stretch };
        }
    }

    // Shadow-smoke billboards -> structured buffer (alpha-blended dark puffs; same layout).
    smokeCount_ = static_cast<uint32_t>(frame.smoke.size());
    if (smokeCount_ > smokeCapacity_) {
        const uint32_t cap = smokeCount_ < 256 ? 256 : smokeCount_ + smokeCount_ / 2;
        smokeBuf_ = createUploadBuffer(device_.d3d(), sizeof(ParticleData) * cap, L"pulse.smoke");
        smokeSrvIndex_ = heaps_.createStructuredBufferSrv(smokeBuf_.get(), cap, sizeof(ParticleData));
        smokeCapacity_ = cap;
    }
    if (smokeCount_ > 0) {
        auto* dst = static_cast<ParticleData*>(smokeBuf_.mapped);
        for (uint32_t i = 0; i < smokeCount_; ++i) {
            const Particle& s = frame.smoke[i];
            dst[i] = { s.center, s.size, s.color, s.emissive, s.velocity, s.stretch };
        }
    }

    // Heat-haze sources -> structured buffer (read by the scene blit to refract the composite).
    heatCount_ = static_cast<uint32_t>(frame.heat.size());
    if (heatCount_ > heatCapacity_) {
        const uint32_t cap = heatCount_ < 64 ? 64 : heatCount_ + heatCount_ / 2;
        heatBuf_ = createUploadBuffer(device_.d3d(), sizeof(HeatData) * cap, L"pulse.heat");
        heatSrvIndex_ = heaps_.createStructuredBufferSrv(heatBuf_.get(), cap, sizeof(HeatData));
        heatCapacity_ = cap;
    }
    if (heatCount_ > 0) {
        auto* dst = static_cast<HeatData*>(heatBuf_.mapped);
        for (uint32_t i = 0; i < heatCount_; ++i) {
            const HeatSource& s = frame.heat[i];
            dst[i] = { s.center, s.radius, s.strength, 0.0f, 0.0f, 0.0f };
        }
    }

    const uint32_t count = static_cast<uint32_t>(frame.instances.size());
    if (count > instanceCapacity_) {
        const uint32_t cap = count < 256 ? 256 : count + count / 2;
        instanceBuf_ = createUploadBuffer(device_.d3d(), sizeof(InstanceData) * cap, L"pulse.instances");
        instanceSrvIndex_ = heaps_.createStructuredBufferSrv(instanceBuf_.get(), cap, sizeof(InstanceData));
        instanceCapacity_ = cap;
    }
    if (count > 0) {
        auto* dst = static_cast<InstanceData*>(instanceBuf_.mapped);
        for (uint32_t i = 0; i < count; ++i) {
            const MeshInstance& mi = frame.instances[i];
            InstanceData d{};
            d.model = mi.transform;
            // Previous-frame transform for motion vectors, keyed by the stable id.
            // Absent (newly spawned, or id 0) -> no motion this frame.
            const auto pit = prevTransforms_.find(mi.id);
            d.prevModel = (mi.id != 0 && pit != prevTransforms_.end()) ? pit->second : mi.transform;
            d.cameraSpace = mi.cameraSpace ? 1u : 0u;
            if (mi.mesh != MeshHandle::Invalid) {
                const MeshEntry& me = meshes_[static_cast<uint32_t>(mi.mesh) - 1];
                d.vbIndex = me.vbSrv;   // for RT hit-shading geometry fetch
                d.ibIndex = me.ibSrv;
            }
            Vec4f factor = { 1, 1, 1, 1 };
            float emissive = 0.0f;
            d.metallic = 0.0f; d.roughness = 0.8f; d.uvScale = 1.0f;
            d.metalScale = 1.0f; d.roughBoost = 0.0f;
            d.emissiveTex = 0; d.emissiveTexStrength = 1.0f;
            if (mi.material != MaterialHandle::Invalid) {
                const MatEntry& m = materials_[static_cast<uint32_t>(mi.material) - 1];
                d.baseTex = m.baseTexSrv;
                factor = m.factor;
                emissive = m.emissive;
                d.metallic = m.metallic;
                d.roughness = m.roughness;
                d.normalTex = m.normalSrv;
                d.ormTex = m.ormSrv;
                d.uvScale = m.uvScale;
                d.metalScale = m.metalScale;
                d.roughBoost = m.roughBoost;
                d.emissiveTex = m.emissiveSrv;
                d.emissiveTexStrength = m.emissiveStrength;
            }
            d.baseColorFactor = { factor.x * mi.tint.x, factor.y * mi.tint.y,
                                  factor.z * mi.tint.z, factor.w * mi.tint.w };
            d.emissive = emissive + mi.emissiveAdd;
            d.rimColor[0] = mi.rimColor.x; d.rimColor[1] = mi.rimColor.y; d.rimColor[2] = mi.rimColor.z;
            d.rimPower = mi.rimPower;
            dst[i] = d;
        }
    }
    // Record this frame's transforms for next frame's motion vectors.
    prevTransforms_.clear();
    for (uint32_t i = 0; i < count; ++i)
        if (frame.instances[i].id != 0) prevTransforms_[frame.instances[i].id] = frame.instances[i].transform;

    // HUD vertices.
    uiCount_ = static_cast<uint32_t>(frame.ui.size());
    if (uiCount_ > uiCapacity_) {
        const uint32_t cap = uiCount_ < 1024 ? 1024 : uiCount_ + uiCount_ / 2;
        uiBuf_ = createUploadBuffer(device_.d3d(), sizeof(UiVertex) * cap, L"pulse.ui");
        uiSrvIndex_ = heaps_.createStructuredBufferSrv(uiBuf_.get(), cap, sizeof(UiVertex));
        uiCapacity_ = cap;
    }
    if (uiCount_ > 0)
        std::memcpy(uiBuf_.mapped, frame.ui.data(), sizeof(UiVertex) * uiCount_);
}

void Engine::buildTlas(ID3D12GraphicsCommandList4* cmd, const SceneFrame& frame) {
    tlasInstanceDescs_.clear();
    const uint32_t count = static_cast<uint32_t>(frame.instances.size());
    for (uint32_t i = 0; i < count; ++i) {
        const MeshInstance& mi = frame.instances[i];
        if (mi.cameraSpace || mi.mesh == MeshHandle::Invalid) continue;   // world geometry only
        const MeshEntry& mesh = meshes_[static_cast<uint32_t>(mi.mesh) - 1];
        if (!mesh.blas) continue;
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        // D3D12 instance transform is 3x4 row-major (object->world, column-vector);
        // our Mat4 is row-vector row-major, so T[r][c] = model.m[c][r].
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                d.Transform[r][c] = mi.transform.m[c][r];
        d.InstanceID = i;                  // hit shading reads instanceBuf_[InstanceID]
        d.InstanceMask = 0xFF;
        d.InstanceContributionToHitGroupIndex = 0;   // inline RT: no hit groups
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;   // double-sided meshes
        d.AccelerationStructure = mesh.blas.gpuAddress;
        tlasInstanceDescs_.push_back(d);
    }
    accel_.buildTlas(cmd, tlasInstanceDescs_.data(), static_cast<uint32_t>(tlasInstanceDescs_.size()));
}

void Engine::recordGraph(ID3D12GraphicsCommandList* list, const SceneFrame& frame, Mode mode) {
    updateFrameData(frame);

    // RT tier: rebuild the TLAS up front (recorded before the graph's passes, so
    // the inline-RayQuery passes see a finished structure). tlasSrvIndex_ stays 0
    // when RT is off or the frame has no traceable geometry.
    tlasSrvIndex_ = 0;
    if (rtEnabled_ && mode == Mode::Final) {
        ComPtr<ID3D12GraphicsCommandList4> cmd4;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&cmd4)))) {
            buildTlas(cmd4.Get(), frame);
            if (accel_.instanceCount() > 0) tlasSrvIndex_ = accel_.tlasSrv();
        }
    }

    graph_.reset();

    auto makeGbuffer = [&](const char* name, DXGI_FORMAT fmt) {
        RGTextureDesc d;
        d.width = width_; d.height = height_; d.format = fmt;
        d.usage = TextureUsage::RenderTarget; d.clear = true;
        d.clearColor[0] = d.clearColor[1] = d.clearColor[2] = d.clearColor[3] = 0.0f;
        return graph_.createTexture(name, d);
    };
    RGHandle gAlbedo   = makeGbuffer("albedo",   kGbAlbedoFmt);
    RGHandle gNormal   = makeGbuffer("normal",   kGbNormalFmt);
    RGHandle gMaterial = makeGbuffer("material", kGbMaterialFmt);
    RGHandle gEmissive = makeGbuffer("emissive", kGbEmissiveFmt);
    RGHandle gVelocity = makeGbuffer("velocity", kGbVelocityFmt);   // screen-space motion vectors

    RGTextureDesc depthDesc;
    depthDesc.width = width_; depthDesc.height = height_; depthDesc.format = kDepthTypeless;
    depthDesc.usage = TextureUsage::DepthStencil; depthDesc.clear = true; depthDesc.clearDepth = 0.0f;
    depthDesc.dsvFormat = kDepthDsv; depthDesc.srvFormat = kDepthSrv;
    RGHandle depth = graph_.createTexture("depth", depthDesc);

    RGTextureDesc hdrDesc;
    hdrDesc.width = width_; hdrDesc.height = height_; hdrDesc.format = kHdrFormat;
    // Cleared on first write so GPU-based validation sees it initialised (the
    // resolve overwrites every pixel, but transient memory may alias).
    hdrDesc.usage = TextureUsage::RenderTarget; hdrDesc.clear = true;
    RGHandle hdr = graph_.createTexture("hdr", hdrDesc);

    RGHandle ldrH = graph_.importTexture("ldr", &ldr_, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const uint32_t instanceCount = static_cast<uint32_t>(frame.instances.size());
    graph_.addRasterPass("gbuffer",
        [&](PassBuilder& b) {
            b.renderTarget(gAlbedo); b.renderTarget(gNormal);
            b.renderTarget(gMaterial); b.renderTarget(gEmissive);
            b.renderTarget(gVelocity);     // order must match gbuffer PSO rtvFormats
            b.depthTarget(depth);
        },
        [this, &frame, instanceCount](PassContext& ctx) {
            ctx.cmd->SetPipelineState(gbufferPso_);
            ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            auto drawInstance = [&](uint32_t i) {
                const MeshInstance& mi = frame.instances[i];
                if (mi.mesh == MeshHandle::Invalid) return;
                const MeshEntry& mesh = meshes_[static_cast<uint32_t>(mi.mesh) - 1];
                GBufferPush push{ frameCBIndex_, instanceSrvIndex_, i, mesh.vbSrv };
                ctx.graphicsConstants(&push, 4);
                ctx.cmd->IASetIndexBuffer(&mesh.ibv);
                ctx.cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
            };
            auto setDepthRange = [&](float mn, float mx) {
                const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(ctx.width),
                                         static_cast<float>(ctx.height), mn, mx };
                ctx.cmd->RSSetViewports(1, &vp);
            };
            // Depth-range compression: the world fills the far slice and camera-space
            // viewmodels the near slice, so the weapon always composites on top while
            // world depth stays valid for the deferred resolve (a mid-pass depth clear
            // would wipe it and the world would read as background).
            setDepthRange(0.0f, 0.95f);
            bool anyViewmodel = false;
            for (uint32_t i = 0; i < instanceCount; ++i) {
                if (frame.instances[i].cameraSpace) { anyViewmodel = true; continue; }
                drawInstance(i);
            }
            if (anyViewmodel) {
                // Viewmodels use the LH camera projection (opposite screen winding),
                // so switch to the viewmodel PSO's front-face convention.
                ctx.cmd->SetPipelineState(gbufferVmPso_);
                setDepthRange(0.95f, 1.0f);
                for (uint32_t i = 0; i < instanceCount; ++i)
                    if (frame.instances[i].cameraSpace) drawInstance(i);
            }
        });

    if (mode == Mode::Final) {
        const bool rt = (tlasSrvIndex_ != 0);

        // Deferred decals: project bullet marks / scorch onto the gbuffer albedo
        // before lighting, so they pick up the same shadows / GI / fog. The pass
        // reads the gbuffer albedo and writes a marked copy the resolve consumes;
        // when there are no decals the resolve reads the gbuffer albedo directly.
        RGHandle albedoForResolve = gAlbedo;
        RGHandle materialForResolve = gMaterial;
        if (decalCount_ > 0) {
            auto makeGbCopy = [&](const char* name, DXGI_FORMAT fmt) {
                RGTextureDesc da;
                da.width = width_; da.height = height_; da.format = fmt;
                da.usage = TextureUsage::RenderTarget; da.clear = true;
                return graph_.createTexture(name, da);
            };
            RGHandle decalAlbedo = makeGbCopy("decalAlbedo", kGbAlbedoFmt);
            RGHandle decalMaterial = makeGbCopy("decalMaterial", kGbMaterialFmt);
            graph_.addRasterPass("decal",
                [&](PassBuilder& b) {
                    b.sample(gAlbedo); b.sample(depth); b.sample(gMaterial);
                    b.sample(gNormal); b.renderTarget(decalAlbedo); b.renderTarget(decalMaterial);
                },
                [this, gAlbedo, depth, gMaterial, gNormal](PassContext& ctx) {
                    DecalPush push{ frameCBIndex_, ctx.srv(gAlbedo), ctx.srv(depth), ctx.srv(gMaterial),
                                    ctx.srv(gNormal), decalSrvIndex_, decalCount_, 0 };
                    ctx.cmd->SetPipelineState(decalPso_);
                    ctx.graphicsConstants(&push, 8);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd->DrawInstanced(3, 1, 0, 0);
                });
            albedoForResolve = decalAlbedo;
            materialForResolve = decalMaterial;
        }

        // Clustered light culling: build per-froxel light lists so the resolve loops
        // only the lights touching each pixel's cluster (both tiers).
        RGTextureDesc gcDesc;
        gcDesc.width = kClusterCount; gcDesc.height = 1; gcDesc.format = DXGI_FORMAT_R32_UINT;
        gcDesc.usage = TextureUsage::UnorderedAccess;
        RGHandle lightGridCount = graph_.createTexture("lightGridCount", gcDesc);
        RGTextureDesc giDesc;
        giDesc.width = kClusterCount; giDesc.height = kClusterMaxPerCell; giDesc.format = DXGI_FORMAT_R32_UINT;
        giDesc.usage = TextureUsage::UnorderedAccess;
        RGHandle lightGridIndices = graph_.createTexture("lightGridIndices", giDesc);
        graph_.addComputePass("cluster_cull",
            [&](PassBuilder& b) { b.uav(lightGridCount); b.uav(lightGridIndices); },
            [this, lightGridCount, lightGridIndices](PassContext& ctx) {
                ClusterCullPush push{ frameCBIndex_, lightSrvIndex_,
                                      ctx.uav(lightGridCount), ctx.uav(lightGridIndices) };
                ctx.cmd->SetPipelineState(clusterCullPso_);
                ctx.computeConstants(&push, 4);
                ctx.cmd->Dispatch((kClusterCount + 63) / 64, 1, 1);
            });

        // Volumetric fog: inject per-froxel scatter into a 3D volume, then march it
        // front-to-back into an integrated volume the resolve composites.
        auto makeFogVol = [&](const char* name) {
            RGTextureDesc d;
            d.width = kFogX; d.height = kFogY; d.depth = kFogZ; d.format = kFogFmt;
            d.usage = TextureUsage::UnorderedAccess;
            return graph_.createTexture(name, d);
        };
        RGHandle fogInject = makeFogVol("fogInject");
        RGHandle fogVolume = makeFogVol("fogVolume");
        graph_.addComputePass("fog_inject",
            [&](PassBuilder& b) { b.uav(fogInject); },
            [this, fogInject](PassContext& ctx) {
                FogPush push{ frameCBIndex_, ctx.uav(fogInject), 0, lightSrvIndex_ };
                ctx.cmd->SetPipelineState(fogInjectPso_);
                ctx.computeConstants(&push, 4);
                ctx.cmd->Dispatch((kFogX + 3) / 4, (kFogY + 3) / 4, (kFogZ + 3) / 4);
            });
        graph_.addComputePass("fog_integrate",
            [&](PassBuilder& b) { b.sampleCompute(fogInject); b.uav(fogVolume); },
            [this, fogInject, fogVolume](PassContext& ctx) {
                FogPush push{ frameCBIndex_, ctx.srv(fogInject), ctx.uav(fogVolume), 0 };
                ctx.cmd->SetPipelineState(fogIntegratePso_);
                ctx.computeConstants(&push, 4);
                ctx.cmd->Dispatch((kFogX + 7) / 8, (kFogY + 7) / 8, 1);
            });

        // Sun shadow + ambient occlusion seam. RT tier: one inline-RayQuery compute
        // pass traces both into rtShadowAo (R=shadow, G=ao). Raster tier: the sun
        // shadow map + SSAO (the shippable fallback). The deferred resolve consumes
        // whichever is produced.
        RGHandle rtShadowAo, rtGi, rtRefl, shadowMap, aoBlur;
        if (rt) {
            auto makeRtOut = [&](const char* name, DXGI_FORMAT fmt) {
                RGTextureDesc d;
                d.width = width_; d.height = height_; d.format = fmt;
                d.usage = TextureUsage::UnorderedAccess;
                return graph_.createTexture(name, d);
            };
            rtShadowAo = makeRtOut("rtShadowAo", DXGI_FORMAT_R16G16_FLOAT);
            rtGi       = makeRtOut("rtGi",       kHdrFormat);
            rtRefl     = makeRtOut("rtRefl",     kHdrFormat);
            graph_.addComputePass("rt_trace",
                [&](PassBuilder& b) {
                    b.sampleCompute(depth); b.sampleCompute(gNormal); b.sampleCompute(gMaterial);
                    b.sampleCompute(gAlbedo);
                    b.uav(rtShadowAo); b.uav(rtGi); b.uav(rtRefl);
                },
                [this, depth, gNormal, gMaterial, gAlbedo, rtShadowAo, rtGi, rtRefl](PassContext& ctx) {
                    RtTracePush push{ frameCBIndex_, ctx.srv(depth), ctx.srv(gNormal), ctx.srv(gMaterial),
                                      ctx.srv(gAlbedo), tlasSrvIndex_, instanceSrvIndex_,
                                      ctx.uav(rtShadowAo), ctx.uav(rtGi), ctx.uav(rtRefl),
                                      width_, height_, frameIndex_, rtRayCount_, 1.2f, 14.0f };
                    ctx.cmd->SetPipelineState(rtTracePso_);
                    ctx.computeConstants(&push, 16);
                    ctx.cmd->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
                });

            // RT denoiser (M2, SVGF-class): temporal accumulation of the noisy GI +
            // reflection signals (velocity reproject + neighbourhood clamp), then an
            // edge-aware a-trous spatial filter. The resolve consumes the denoised
            // signals instead of the raw trace. History ping-pongs by frame parity and
            // rests in NON_PIXEL_SHADER_RESOURCE (matching createTargets).
            const bool parityRt = (frameIndex_ & 1u) != 0;
            RGHandle giHistCur  = graph_.importTexture(parityRt ? "giHist1" : "giHist0",
                parityRt ? &giHist_[1] : &giHist_[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            RGHandle giHistPrev = graph_.importTexture(parityRt ? "giHist0" : "giHist1",
                parityRt ? &giHist_[0] : &giHist_[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            RGHandle reflHistCur  = graph_.importTexture(parityRt ? "reflHist1" : "reflHist0",
                parityRt ? &reflHist_[1] : &reflHist_[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            RGHandle reflHistPrev = graph_.importTexture(parityRt ? "reflHist0" : "reflHist1",
                parityRt ? &reflHist_[0] : &reflHist_[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // Temporal accumulation runs whenever a previous frame exists -- windowed
            // play AND headless multi-frame captures (record/bot-test sequences). A
            // single --pose still has no history (frame 0), so it stays spatial-only
            // and deterministic.
            const uint32_t denoiseValid = historyValid_ ? 1u : 0u;
            graph_.addComputePass("rt_temporal",
                [&](PassBuilder& b) {
                    b.sampleCompute(rtGi); b.sampleCompute(rtRefl);
                    b.sampleCompute(gVelocity); b.sampleCompute(depth);
                    b.sampleCompute(giHistPrev); b.sampleCompute(reflHistPrev);
                    b.uav(giHistCur); b.uav(reflHistCur);
                },
                [this, rtGi, rtRefl, gVelocity, depth, giHistPrev, reflHistPrev,
                 giHistCur, reflHistCur, denoiseValid](PassContext& ctx) {
                    RtTemporalPush push{ ctx.srv(rtGi), ctx.srv(rtRefl), ctx.srv(gVelocity), ctx.srv(depth),
                                         ctx.srv(giHistPrev), ctx.srv(reflHistPrev),
                                         ctx.uav(giHistCur), ctx.uav(reflHistCur),
                                         width_, height_, denoiseValid, 0.1f };
                    ctx.cmd->SetPipelineState(rtDenoiseTemporalPso_);
                    ctx.computeConstants(&push, 12);
                    ctx.cmd->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
                });

            // a-trous: two iterations with growing hole size, reading the temporal
            // output and bouncing through transient targets; the last feeds resolve.
            RGHandle giF0 = makeRtOut("giF0", kHdrFormat),   giF1 = makeRtOut("giF1", kHdrFormat);
            RGHandle reflF0 = makeRtOut("reflF0", kHdrFormat), reflF1 = makeRtOut("reflF1", kHdrFormat);
            const float denoiseNearZ = frame.camera.nearZ;
            auto atrous = [&](const char* name, RGHandle giIn, RGHandle reflIn,
                              RGHandle giO, RGHandle reflO, uint32_t step) {
                graph_.addComputePass(name,
                    [&](PassBuilder& b) {
                        b.sampleCompute(giIn); b.sampleCompute(reflIn);
                        b.sampleCompute(depth); b.sampleCompute(gNormal);
                        b.uav(giO); b.uav(reflO);
                    },
                    [this, giIn, reflIn, depth, gNormal, giO, reflO, step, denoiseNearZ](PassContext& ctx) {
                        RtAtrousPush push{ ctx.srv(giIn), ctx.srv(reflIn), ctx.srv(depth),
                                           ctx.srv(gNormal), ctx.uav(giO), ctx.uav(reflO),
                                           width_, height_, step, denoiseNearZ, 64.0f, 0.5f };
                        ctx.cmd->SetPipelineState(rtDenoiseAtrousPso_);
                        ctx.computeConstants(&push, 12);
                        ctx.cmd->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
                    });
            };
            atrous("rt_atrous0", giHistCur, reflHistCur, giF0, reflF0, 1u);
            atrous("rt_atrous1", giF0, reflF0, giF1, reflF1, 2u);
            rtGi = giF1; rtRefl = reflF1;   // resolve consumes the denoised signals
        } else {
            // Sun shadow map: depth-only render of world geometry from the sun's
            // orthographic viewpoint (camera-space viewmodels excluded). Persistent
            // (imported), so it stays out of the transient alias pool.
            shadowMap = graph_.importDepth("shadow", &shadowMap_,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true, 1.0f, kDepthDsv, kDepthSrv);

            graph_.addRasterPass("shadow",
                [&](PassBuilder& b) { b.depthTarget(shadowMap); },
                [this, &frame, instanceCount](PassContext& ctx) {
                    ctx.cmd->SetPipelineState(shadowPso_);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    for (uint32_t i = 0; i < instanceCount; ++i) {
                        const MeshInstance& mi = frame.instances[i];
                        if (mi.cameraSpace || mi.mesh == MeshHandle::Invalid) continue;  // world only
                        const MeshEntry& mesh = meshes_[static_cast<uint32_t>(mi.mesh) - 1];
                        ShadowPush push{ frameCBIndex_, instanceSrvIndex_, i, mesh.vbSrv };
                        ctx.graphicsConstants(&push, 4);
                        ctx.cmd->IASetIndexBuffer(&mesh.ibv);
                        ctx.cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
                    }
                });

            // SSAO: hemisphere AO from depth+normal, then a box blur.
            auto makeAoRT = [&](const char* name) {
                RGTextureDesc d;
                d.width = width_; d.height = height_; d.format = kAoFmt;
                d.usage = TextureUsage::RenderTarget; d.clear = true;
                d.clearColor[0] = d.clearColor[1] = d.clearColor[2] = d.clearColor[3] = 1.0f;  // 1 = no occlusion
                return graph_.createTexture(name, d);
            };
            RGHandle aoRaw = makeAoRT("ao");
            aoBlur = makeAoRT("aoBlur");
            graph_.addRasterPass("ssao",
                [&](PassBuilder& b) { b.sample(depth); b.sample(gNormal); b.renderTarget(aoRaw); },
                [this, depth, gNormal](PassContext& ctx) {
                    SsaoPush push{ frameCBIndex_, ctx.srv(depth), ctx.srv(gNormal), 0, 0.65f, 1.1f, 0.02f, 0 };
                    ctx.cmd->SetPipelineState(ssaoPso_);
                    ctx.graphicsConstants(&push, 8);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd->DrawInstanced(3, 1, 0, 0);
                });
            graph_.addRasterPass("ssao_blur",
                [&](PassBuilder& b) { b.sample(aoRaw); b.renderTarget(aoBlur); },
                [this, aoRaw](PassContext& ctx) {
                    SsaoPush push{ ctx.srv(aoRaw), 0, 0, 0,
                                   1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_), 0, 0 };
                    ctx.cmd->SetPipelineState(ssaoBlurPso_);
                    ctx.graphicsConstants(&push, 8);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd->DrawInstanced(3, 1, 0, 0);
                });
        }

        graph_.addRasterPass("resolve",
            [&](PassBuilder& b) {
                b.sample(albedoForResolve); b.sample(gNormal); b.sample(materialForResolve);
                b.sample(gEmissive); b.sample(depth);
                b.sample(lightGridCount); b.sample(lightGridIndices); b.sample(fogVolume);
                if (rt) { b.sample(rtShadowAo); b.sample(rtGi); b.sample(rtRefl); }
                else    { b.sample(shadowMap); b.sample(aoBlur); }
                b.renderTarget(hdr);
            },
            [this, rt, albedoForResolve, gNormal, materialForResolve, gEmissive, depth, shadowMap, aoBlur,
             rtShadowAo, rtGi, rtRefl, lightGridCount, lightGridIndices, fogVolume](PassContext& ctx) {
                ResolvePush push{};
                push.frameCB = frameCBIndex_;
                push.albedo = ctx.srv(albedoForResolve); push.normal = ctx.srv(gNormal);
                push.material = ctx.srv(materialForResolve); push.emissive = ctx.srv(gEmissive);
                push.depth = ctx.srv(depth); push.lights = lightSrvIndex_;
                push.gridCount = ctx.srv(lightGridCount); push.gridIndices = ctx.srv(lightGridIndices);
                push.fogVol = ctx.srv(fogVolume);
                if (rt) {
                    push.shadow = 0; push.ao = 0;
                    push.rtIndex = ctx.srv(rtShadowAo); push.rtEnabled = 1;
                    push.giIndex = ctx.srv(rtGi); push.reflIndex = ctx.srv(rtRefl);
                } else {
                    push.shadow = ctx.srv(shadowMap); push.ao = ctx.srv(aoBlur);
                    push.rtIndex = 0; push.rtEnabled = 0;
                    push.giIndex = 0; push.reflIndex = 0;
                }
                ctx.cmd->SetPipelineState(resolvePso_);
                ctx.graphicsConstants(&push, 16);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });

        // Raster-tier screen-space passes (the RT tier resolved GI + reflections
        // inline). SSGI (M3) adds a dynamic indirect bounce gathered from on-screen
        // lit surfaces; SSR then adds the specular on top of the GI-augmented scene.
        RGHandle litColor = hdr;
        if (!rt) {
            auto makeHdrRT = [&](const char* name) {
                RGTextureDesc d;
                d.width = width_; d.height = height_; d.format = kHdrFormat;
                d.usage = TextureUsage::RenderTarget; d.clear = true;
                return graph_.createTexture(name, d);
            };
            // W6 quality ladder: SSGI (dynamic indirect bounce) then SSR (specular) layer
            // on the raster tier. Each is gated by quality -- Low drops both, leaving flat
            // ambient + analytic reflections (the RT-off parity fallback that keeps the
            // world readable). giColor / litColor follow whichever passes ran.
            RGHandle giColor = hdr;
            if (ssgiEnabled_) {
                RGHandle hdrGi = makeHdrRT("hdrGi");
                graph_.addRasterPass("ssgi",
                    [&](PassBuilder& b) {
                        b.sample(hdr); b.sample(depth); b.sample(gNormal);
                        b.sample(gAlbedo); b.sample(gMaterial); b.renderTarget(hdrGi);
                    },
                    [this, hdr, depth, gNormal, gAlbedo, gMaterial](PassContext& ctx) {
                        SsgiPush push{ ctx.srv(hdr), ctx.srv(depth), ctx.srv(gNormal),
                                       ctx.srv(gAlbedo), ctx.srv(gMaterial), frameCBIndex_, 0.9f, 0 };
                        ctx.cmd->SetPipelineState(ssgiPso_);
                        ctx.graphicsConstants(&push, 8);
                        ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        ctx.cmd->DrawInstanced(3, 1, 0, 0);
                    });
                giColor = hdrGi;
            }

            if (ssrEnabled_) {
                RGHandle hdrSsr = makeHdrRT("hdrSsr");
                graph_.addRasterPass("ssr",
                    [&](PassBuilder& b) {
                        b.sample(giColor); b.sample(depth); b.sample(gNormal);
                        b.sample(gMaterial); b.sample(gAlbedo); b.renderTarget(hdrSsr);
                    },
                    [this, giColor, depth, gNormal, gMaterial, gAlbedo](PassContext& ctx) {
                        SsrPush push{ ctx.srv(giColor), ctx.srv(depth), ctx.srv(gNormal),
                                      ctx.srv(gMaterial), ctx.srv(gAlbedo), frameCBIndex_, 0, 0 };
                        ctx.cmd->SetPipelineState(ssrPso_);
                        ctx.graphicsConstants(&push, 8);
                        ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        ctx.cmd->DrawInstanced(3, 1, 0, 0);
                    });
                litColor = hdrSsr;
            } else {
                litColor = giColor;
            }
        }

        // W2a: ink outlines from depth + normal discontinuities, composited into the lit
        // HDR before TAA (depth + gNormal are still alive here, and TAA then temporally
        // stabilizes the lines). Skipped entirely when the caller leaves outlines off.
        if (frame.style.outlineStrength > 0.0f) {
            const float oInvW = 1.0f / static_cast<float>(width_);
            const float oInvH = 1.0f / static_cast<float>(height_);
            const float oNear = frame.camera.nearZ;
            const StyleFrame st = frame.style;
            // Scale the ink thickness with vertical resolution so the line weight stays consistent
            // (a fixed 2 px reads thinner at 1440p than at 1080p).
            const float oThick = st.outlineThickness * (static_cast<float>(height_) / 1080.0f);
            graph_.addRasterPass("outline",
                [&](PassBuilder& b) { b.sample(depth); b.sample(gNormal); b.sample(gMaterial); b.renderTarget(litColor); },
                [this, depth, gNormal, gMaterial, oInvW, oInvH, oNear, st, oThick](PassContext& ctx) {
                    OutlinePush push{ ctx.srv(depth), ctx.srv(gNormal), ctx.srv(gMaterial), oInvW, oInvH, oNear,
                                      st.inkOutline.x, st.inkOutline.y, st.inkOutline.z,
                                      st.outlineDepthSense, st.outlineNormalSense,
                                      oThick, st.outlineStrength, st.outlineHeroScale };
                    ctx.cmd->SetPipelineState(outlinePso_);
                    ctx.graphicsConstants(&push, 14);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd->DrawInstanced(3, 1, 0, 0);
                });
        }

        // TAA: reproject the previous resolved frame (history) through the velocity
        // buffer, neighbourhood-clip it, and blend. The two persistent HDR history
        // targets ping-pong by frame parity: this frame writes one and reads the
        // other; bloom + tonemap consume the freshly written one. Replaces FXAA as
        // the plan's primary AA. Headless captures keep history disabled (crisp).
        RGHandle h0 = graph_.importTexture("history0", &history_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        RGHandle h1 = graph_.importTexture("history1", &history_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const bool parity = (frameIndex_ & 1u) != 0;
        RGHandle taaColor = parity ? h1 : h0;   // written this frame (next frame's history)
        RGHandle taaPrev  = parity ? h0 : h1;   // last frame's output
        const uint32_t historyValid = (windowed_ && historyValid_) ? 1u : 0u;
        const float tInvW = 1.0f / static_cast<float>(width_);
        const float tInvH = 1.0f / static_cast<float>(height_);
        graph_.addRasterPass("taa",
            [&](PassBuilder& b) {
                b.sample(litColor); b.sample(taaPrev); b.sample(gVelocity); b.sample(depth);
                b.renderTarget(taaColor);
            },
            [this, litColor, taaPrev, gVelocity, depth, historyValid, tInvW, tInvH](PassContext& ctx) {
                TaaPush push{ ctx.srv(litColor), ctx.srv(taaPrev), ctx.srv(gVelocity), ctx.srv(depth),
                              tInvW, tInvH, historyValid, 0.92f };
                ctx.cmd->SetPipelineState(taaPso_);
                ctx.graphicsConstants(&push, 8);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });
        // Mark history valid after the first frame (windowed or headless) so the next
        // frame's RT denoiser can accumulate. TAA itself stays windowed-gated above, so
        // headless geometry stills remain crisp; only the GI/reflection denoiser reuses
        // headless history across a capture sequence.
        historyValid_ = true;

        // Particles: composite the TAA-resolved scene + additive world-space billboards
        // into a fresh HDR target (keeps the billboards out of the TAA history). Bloom
        // and tonemap then read this composite, so bright sparks bloom and grade.
        RGHandle sceneColor = taaColor;
        if (particleCount_ > 0 || smokeCount_ > 0) {
            RGTextureDesc cd;
            cd.width = width_; cd.height = height_; cd.format = kHdrFormat;
            cd.usage = TextureUsage::RenderTarget; cd.clear = true;
            RGHandle composite = graph_.createTexture("composite", cd);
            graph_.addRasterPass("particles",
                [&](PassBuilder& b) { b.sample(taaColor); b.renderTarget(composite); },
                [this, taaColor](PassContext& ctx) {
                    BlitPush bpush{ ctx.srv(taaColor), frameCBIndex_, heatSrvIndex_, heatCount_,
                                    static_cast<float>(frameIndex_) * (1.0f / 60.0f),
                                    static_cast<float>(width_) / static_cast<float>(height_), 0.0f, 0.0f };
                    ctx.cmd->SetPipelineState(particleBlitPso_);
                    ctx.graphicsConstants(&bpush, 8);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd->DrawInstanced(3, 1, 0, 0);
                    // Shadow smoke FIRST (darkens the scene around the enemy / dissolves its edge),
                    // then the additive sparks on top so they are not dimmed by the smoke.
                    if (smokeCount_ > 0) {
                        ParticlePush spush{ frameCBIndex_, smokeSrvIndex_, smokeCount_, 0,
                                            { cameraRight_.x, cameraRight_.y, cameraRight_.z }, 0.0f,
                                            { cameraUp_.x, cameraUp_.y, cameraUp_.z }, 0.0f };
                        ctx.cmd->SetPipelineState(smokePso_);
                        ctx.graphicsConstants(&spush, 12);
                        ctx.cmd->DrawInstanced(6, smokeCount_, 0, 0);
                    }
                    if (particleCount_ > 0) {
                        ParticlePush ppush{ frameCBIndex_, particleSrvIndex_, particleCount_, 0,
                                            { cameraRight_.x, cameraRight_.y, cameraRight_.z }, 0.0f,
                                            { cameraUp_.x, cameraUp_.y, cameraUp_.z }, 0.0f };
                        ctx.cmd->SetPipelineState(particlePso_);
                        ctx.graphicsConstants(&ppush, 12);
                        ctx.cmd->DrawInstanced(6, particleCount_, 0, 0);
                    }
                });
            sceneColor = composite;
        }

        // Bloom: prefilter + downsample the resolved HDR to quarter res, then a
        // separable Gaussian blur (ping-pong bloomA/bloomB). Composited in tonemap.
        const uint32_t bw = width_ / 4 > 0 ? width_ / 4 : 1u;
        const uint32_t bh = height_ / 4 > 0 ? height_ / 4 : 1u;
        auto makeBloomRT = [&](const char* name) {
            RGTextureDesc d;
            d.width = bw; d.height = bh; d.format = kHdrFormat;
            d.usage = TextureUsage::RenderTarget; d.clear = true;
            return graph_.createTexture(name, d);
        };
        RGHandle bloomA = makeBloomRT("bloomA");
        RGHandle bloomB = makeBloomRT("bloomB");
        const float invW  = 1.0f / static_cast<float>(width_);
        const float invH  = 1.0f / static_cast<float>(height_);
        const float invBw = 1.0f / static_cast<float>(bw);
        const float invBh = 1.0f / static_cast<float>(bh);

        const float bloomThr  = frame.post.bloomThreshold;
        const float bloomKnee = frame.post.bloomKnee;
        graph_.addRasterPass("bloom_down",
            [&](PassBuilder& b) { b.sample(sceneColor); b.renderTarget(bloomA); },
            [this, sceneColor, invW, invH, bloomThr, bloomKnee](PassContext& ctx) {
                BloomPush push{ ctx.srv(sceneColor), bloomThr, bloomKnee, 0, invW, invH, 0, 0 };  // threshold, knee
                ctx.cmd->SetPipelineState(bloomDownPso_);
                ctx.graphicsConstants(&push, 8);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });
        graph_.addRasterPass("bloom_blurh",
            [&](PassBuilder& b) { b.sample(bloomA); b.renderTarget(bloomB); },
            [this, bloomA, invBw, invBh](PassContext& ctx) {
                BloomPush push{ ctx.srv(bloomA), 1.0f, 0.0f, 0, invBw, invBh, 0, 0 };  // dir (1,0)
                ctx.cmd->SetPipelineState(bloomBlurPso_);
                ctx.graphicsConstants(&push, 8);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });
        graph_.addRasterPass("bloom_blurv",
            [&](PassBuilder& b) { b.sample(bloomB); b.renderTarget(bloomA); },
            [this, bloomB, invBw, invBh](PassContext& ctx) {
                BloomPush push{ ctx.srv(bloomB), 0.0f, 1.0f, 0, invBw, invBh, 0, 0 };  // dir (0,1)
                ctx.cmd->SetPipelineState(bloomBlurPso_);
                ctx.graphicsConstants(&push, 8);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });

        // Tonemap the TAA-resolved HDR straight into the final LDR target (TAA has
        // already done the AA, so no FXAA pass). UI blends on top afterwards so the
        // HUD stays crisp.
        const float exposure = frame.post.exposure;
        const float bloomInt = frame.post.bloomIntensity;
        const float pSharpen = frame.post.sharpen;
        const float pVignette = frame.post.vignette;
        const float pGrain   = frame.post.grain;
        const float pCaScale = frame.post.caScale;
        const float gEnvSat  = frame.post.gradeEnvSat;
        const float gNeonSat = frame.post.gradeNeonSat;
        const float gNeonGain= frame.post.gradeNeonGain;
        const float gTintR   = frame.post.gradeTint.x;
        const float gTintG   = frame.post.gradeTint.y;
        const float gTintB   = frame.post.gradeTint.z;
        graph_.addRasterPass("tonemap",
            [&](PassBuilder& b) { b.sample(sceneColor); b.sample(bloomA); b.renderTarget(ldrH); },
            [this, sceneColor, bloomA, exposure, bloomInt, pSharpen, pVignette, pGrain, pCaScale, gEnvSat, gNeonSat, gNeonGain, gTintR, gTintG, gTintB](PassContext& ctx) {
                TonemapPush push{ ctx.srv(sceneColor), exposure, ctx.srv(bloomA), bloomInt,
                                  1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_),
                                  sunScreenU_, sunScreenV_, godrayAmt_,
                                  pSharpen, pVignette, pGrain, pCaScale,
                                  gEnvSat, gNeonSat, gNeonGain, gTintR, gTintG, gTintB };
                ctx.cmd->SetPipelineState(tonemapPso_);
                ctx.graphicsConstants(&push, 19);   // M7: +3 for gradeTint (was 16)
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });

        if (uiCount_ > 0) {
            struct UiPush { uint32_t vbIndex, atlasIndex; float invW, invH; };
            const UiPush push{ uiSrvIndex_, font_.atlasSrv(),
                               1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_) };
            const uint32_t uiCount = uiCount_;
            graph_.addRasterPass("ui",
                [&](PassBuilder& b) { b.renderTarget(ldrH); },     // blend onto tonemap output
                [this, push, uiCount](PassContext& ctx) {
                    ctx.cmd->SetPipelineState(uiPso_);
                    ctx.graphicsConstants(&push, 4);
                    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx.cmd->DrawInstanced(uiCount, 1, 0, 0);
                });
        }
    } else if (mode == Mode::Depth) {
        const float nearZ = frame.camera.nearZ;
        graph_.addRasterPass("depthviz",
            [&](PassBuilder& b) { b.sample(depth); b.renderTarget(ldrH); },
            [this, depth, nearZ](PassContext& ctx) {
                DepthvizPush push{ ctx.srv(depth), nearZ, 0, 0 };
                ctx.cmd->SetPipelineState(depthvizPso_);
                ctx.graphicsConstants(&push, 4);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });
    } else {
        // GBuffer channel visualisation (--render-pass albedo/normal/material/emissive).
        RGHandle ch = gAlbedo;
        uint32_t vizMode = 0;
        switch (mode) {
            case Mode::VizNormal:   ch = gNormal;   break;
            case Mode::VizMaterial: ch = gMaterial; break;
            case Mode::VizEmissive: ch = gEmissive; vizMode = 1; break;
            default:                ch = gAlbedo;   break;   // VizAlbedo
        }
        graph_.addRasterPass("gbufferviz",
            [&](PassBuilder& b) { b.sample(ch); b.renderTarget(ldrH); },
            [this, ch, vizMode](PassContext& ctx) {
                GBufferVizPush push{ ctx.srv(ch), vizMode, 0, 0 };
                ctx.cmd->SetPipelineState(gbuffervizPso_);
                ctx.graphicsConstants(&push, 4);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->DrawInstanced(3, 1, 0, 0);
            });
    }

    graph_.compile();
    graph_.execute(list);
}

void Engine::renderFrame(const SceneFrame& frame) {
    PULSE_CHECK(windowed_, "renderFrame called on a headless engine");

    ID3D12GraphicsCommandList* list = frameCmd_.begin();
    recordGraph(list, frame, Mode::Final);

    Texture& bb = swapchain_.currentBackbuffer();
    D3D12_RESOURCE_BARRIER toCopy[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(ldr_.get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(bb.get(), bb.state, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list->ResourceBarrier(2, toCopy);
    list->CopyResource(bb.get(), ldr_.get());
    D3D12_RESOURCE_BARRIER toPresent[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(bb.get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        CD3DX12_RESOURCE_BARRIER::Transition(ldr_.get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    list->ResourceBarrier(2, toPresent);
    bb.state = D3D12_RESOURCE_STATE_PRESENT;

    frameCmd_.close();
    ID3D12CommandList* lists[] = { list };
    device_.graphicsQueue()->ExecuteCommandLists(1, lists);
    swapchain_.present(true);
    device_.flushGraphics();
}

bool Engine::captureFrame(const SceneFrame& frame, Image& out) {
    return capturePass(frame, "ldr", out);
}

bool Engine::capturePass(const SceneFrame& frame, std::string_view passName, Image& out) {
    Mode mode = Mode::Final;
    if (passName == "depth") {
        mode = Mode::Depth;
    } else if (passName == "ldr" || passName == "final") {
        mode = Mode::Final;
    } else if (passName == "albedo") {
        mode = Mode::VizAlbedo;
    } else if (passName == "normal") {
        mode = Mode::VizNormal;
    } else if (passName == "material" || passName == "orm") {
        mode = Mode::VizMaterial;
    } else if (passName == "emissive") {
        mode = Mode::VizEmissive;
    } else {
        fail(formatStr("Unsupported render pass '%.*s'", static_cast<int>(passName.size()), passName.data()));
    }

    ID3D12GraphicsCommandList* list = frameCmd_.begin();
    recordGraph(list, frame, mode);
    frameCmd_.close();
    ID3D12CommandList* lists[] = { list };
    device_.graphicsQueue()->ExecuteCommandLists(1, lists);
    device_.flushGraphics();

    out = readbackTexture(device_, ldr_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

bool Engine::capturePathTraced(const SceneFrame& frame, uint32_t samples, uint32_t bounces, Image& out) {
    if (!rtEnabled_)
        fail("path-traced reference capture requires the RT tier (DXR); not available on this device / --force-raster");
    samples = samples == 0 ? 1u : samples;
    bounces = bounces == 0 ? 1u : bounces;
    updateFrameData(frame);

    ID3D12CommandList* one[1];
    auto submit = [&](ID3D12GraphicsCommandList* list) {
        frameCmd_.close();
        one[0] = list;
        device_.graphicsQueue()->ExecuteCommandLists(1, one);
        device_.flushGraphics();
    };

    // 1. Build the TLAS from the frame's world instances.
    {
        ID3D12GraphicsCommandList* list = frameCmd_.begin();
        ComPtr<ID3D12GraphicsCommandList4> cmd4;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&cmd4))))
            fail("path-trace: command list lacks ID3D12GraphicsCommandList4 (DXR)");
        buildTlas(cmd4.Get(), frame);
        submit(list);
    }
    const uint32_t tlasSrv = (accel_.instanceCount() > 0) ? accel_.tlasSrv() : 0u;

    // 2. HDR accumulator (RGBA32F, UAV + RT so it can be RTV-cleared once).
    const float zero[4] = { 0, 0, 0, 0 };
    Texture accum = createTexture2D(device_.d3d(), width_, height_, DXGI_FORMAT_R32G32B32A32_FLOAT,
        TextureUsage::RenderTarget | TextureUsage::UnorderedAccess,
        D3D12_RESOURCE_STATE_RENDER_TARGET, 1, zero, L"pulse.ptaccum");
    const uint32_t accumUav = heaps_.createTextureUav(accum);
    {
        ID3D12GraphicsCommandList* list = frameCmd_.begin();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heaps_.createRtv(accum);
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(accum.get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        submit(list);
    }

    // 3. Accumulate paths in small per-submit batches (each submit stays well under
    // the OS GPU watchdog; the accumulator persists across submits).
    ID3D12DescriptorHeap* heaps[] = { heaps_.bindlessHeap() };
    if (tlasSrv != 0) {
        const uint32_t kBatch = 8;
        for (uint32_t s = 0; s < samples; ) {
            const uint32_t end = (s + kBatch < samples) ? s + kBatch : samples;
            ID3D12GraphicsCommandList* list = frameCmd_.begin();
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(rootSig_.Get());
            list->SetPipelineState(ptTracePso_);
            for (uint32_t k = s; k < end; ++k) {
                PtTracePush push{ frameCBIndex_, tlasSrv, instanceSrvIndex_, accumUav, lightSrvIndex_,
                                  width_, height_, k, bounces, lightCount_, 0, 0, 0, 0, 0 };
                list->SetComputeRoot32BitConstants(0, 15, &push, 0);
                list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
                D3D12_RESOURCE_BARRIER uavb = CD3DX12_RESOURCE_BARRIER::UAV(accum.get());
                list->ResourceBarrier(1, &uavb);
            }
            submit(list);
            s = end;
        }
    }

    // 4. Resolve: average + AgX -> ldr_, then read back.
    {
        ID3D12GraphicsCommandList* list = frameCmd_.begin();
        D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(accum.get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toSrv);
        const uint32_t accumSrv = heaps_.createTextureSrv(accum.get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 1);
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSig_.Get());
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heaps_.createRtv(ldr_);
        const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
        const D3D12_RECT rect{ 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &rect);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetPipelineState(ptResolvePso_);
        PtResolvePush rpush{ accumSrv, frame.post.exposure, 0, 0 };
        list->SetGraphicsRoot32BitConstants(0, 4, &rpush, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        submit(list);
    }

    out = readbackTexture(device_, ldr_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

} // namespace pulse
