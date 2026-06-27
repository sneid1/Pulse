#pragma once

#include "Engine/SceneFrame.hpp"
#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Heaps.hpp"
#include "Engine/RHI/Pipeline.hpp"
#include "Engine/RHI/Accel.hpp"
#include "Engine/RHI/Uploader.hpp"
#include "Engine/RHI/Commands.hpp"
#include "Engine/RHI/Swapchain.hpp"
#include "Engine/RHI/Resource.hpp"
#include "Engine/RenderGraph/RenderGraph.hpp"
#include "Engine/Shaders/ShaderCompiler.hpp"
#include "Engine/Core/Image.hpp"
#include "Engine/UI/Font.hpp"

#include <unordered_map>
#include <vector>

namespace pulse {

// The game-facing engine: the game submits a SceneFrame, the engine owns how it
// becomes pixels. Windowed (renderFrame -> present) and headless
// (captureFrame/capturePass -> readback) share the exact same render graph. The
// present path is HDR -> AgX tonemap -> LDR (the plan's color-management spine).
class Engine {
public:
    struct Config {
        HWND     hwnd = nullptr;        // null => headless (no swapchain)
        uint32_t width = 1280;
        uint32_t height = 720;
        bool     enableDebugLayer = false;
        bool     enableGpuValidation = false;
        bool     forceRaster = false;
        // W6 quality ladder (doc 14): raster-tier screen-space passes. Low drops both so
        // the world falls back to flat ambient + analytic reflections (still readable -
        // the RT-off parity gate). High/Medium keep them. Ignored on the RT tier.
        bool     enableSsgi = true;
        bool     enableSsr = true;
        uint32_t rtRayCount = 3; // RT-tier GI hemisphere rays/pixel; Ultra raises it for fuller, cleaner GI
    };

    void init(const Config& cfg);

    // Created once at load. A zero/Invalid handle is never valid.
    MeshHandle     createMesh(const MeshData& data);
    MeshHandle     createDynamicMesh(uint32_t maxVertices, std::span<const uint32_t> indices);
    bool           updateDynamicMesh(MeshHandle mesh, std::span<const StaticVertex> vertices);
    TextureHandle  createTexture(const TextureData& data);
    // Load a BCn DDS file (asset pipeline output) as a GPU texture. Returns Invalid
    // on failure (caller fails loud).
    TextureHandle  createTextureDDS(const std::string& path);
    MaterialHandle createMaterial(const MaterialDesc& desc);

    // Windowed: build graph, execute, present.
    void renderFrame(const SceneFrame& frame);
    // Headless: same graph, no swapchain; read back the final LDR target.
    bool captureFrame(const SceneFrame& frame, Image& out);
    // Dump one named graph resource ("ldr", "hdr", "depth") as a viewable image.
    bool capturePass(const SceneFrame& frame, std::string_view passName, Image& out);
    // M3 reference path-traced capture (RT tier only): accumulate `samples` paths/pixel
    // with up to `bounces` bounces, AgX-resolve to LDR. Offline (hero stills); slow.
    bool capturePathTraced(const SceneFrame& frame, uint32_t samples, uint32_t bounces, Image& out);

    void resize(uint32_t width, uint32_t height);
    const rhi::Caps& caps() const { return device_.caps(); }
    rhi::Device& device() { return device_; }
    const Font& font() const { return font_; }       // for building UiDrawLists
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    enum class Mode { Final, Depth, VizAlbedo, VizNormal, VizMaterial, VizEmissive };

    void createTargets();
    void updateFrameData(const SceneFrame& frame);
    void recordGraph(ID3D12GraphicsCommandList* list, const SceneFrame& frame, Mode mode);
    // Rebuild the per-frame TLAS from the world (non-camera-space) instances.
    void buildTlas(ID3D12GraphicsCommandList4* cmd, const SceneFrame& frame);

    rhi::Device          device_;
    rhi::Heaps           heaps_;
    rhi::ComPtr<ID3D12RootSignature> rootSig_;
    rhi::PipelineCache   pipelines_;
    rhi::ShaderCompiler  shaderCompiler_;
    rhi::Uploader        uploader_;
    rhi::RtAccel         accel_;         // DXR acceleration structures (RT tier only)
    bool                 rtEnabled_ = false;
    bool                 ssgiEnabled_ = true;   // W6: raster-tier SSGI pass (Low drops it)
    bool                 ssrEnabled_ = true;    // W6: raster-tier SSR pass (Low drops it)
    uint32_t             rtRayCount_ = 3;       // W6: RT-tier GI rays/pixel (Ultra raises it)
    render::RenderGraph  graph_;
    rhi::Swapchain       swapchain_;
    rhi::CommandList     frameCmd_;
    bool                 windowed_ = false;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    rhi::Texture ldr_;                  // owned final target (imported into the graph)
    rhi::Texture shadowMap_;            // persistent sun shadow map (imported into the graph)
    rhi::Texture history_[2];           // TAA history ping-pong (HDR, imported into the graph)
    rhi::Texture giHist_[2];            // RT denoiser GI accumulation ping-pong (rgb gi, a len)
    rhi::Texture reflHist_[2];          // RT denoiser reflection accumulation ping-pong

    ID3D12PipelineState* gbufferPso_ = nullptr;     // MRT gbuffer fill (world, back-face cull)
    ID3D12PipelineState* gbufferVmPso_ = nullptr;   // MRT gbuffer fill (viewmodel, opposite winding)
    ID3D12PipelineState* resolvePso_ = nullptr;     // deferred lighting resolve -> HDR
    ID3D12PipelineState* gbuffervizPso_ = nullptr;  // --render-pass channel viz
    ID3D12PipelineState* bloomDownPso_ = nullptr;   // prefilter + downsample
    ID3D12PipelineState* bloomBlurPso_ = nullptr;   // separable Gaussian blur
    ID3D12PipelineState* shadowPso_ = nullptr;      // sun shadow map (depth-only)
    ID3D12PipelineState* rtTracePso_ = nullptr;     // RT shadow + AO compute (RT tier)
    ID3D12PipelineState* rtDenoiseTemporalPso_ = nullptr; // RT denoiser temporal accumulation
    ID3D12PipelineState* rtDenoiseAtrousPso_ = nullptr;   // RT denoiser edge-aware a-trous
    ID3D12PipelineState* ptTracePso_ = nullptr;     // M3 reference path tracer (accumulate)
    ID3D12PipelineState* ptResolvePso_ = nullptr;   // M3 path-trace accumulator -> AgX LDR
    ID3D12PipelineState* clusterCullPso_ = nullptr; // clustered light culling compute
    ID3D12PipelineState* fogInjectPso_ = nullptr;   // volumetric fog froxel inject
    ID3D12PipelineState* fogIntegratePso_ = nullptr;// volumetric fog froxel integrate
    ID3D12PipelineState* ssaoPso_ = nullptr;        // SSAO hemisphere pass
    ID3D12PipelineState* ssaoBlurPso_ = nullptr;    // SSAO box blur
    ID3D12PipelineState* tonemapPso_ = nullptr;
    ID3D12PipelineState* fxaaPso_ = nullptr;        // legacy post AA (kept; TAA is the default)
    ID3D12PipelineState* taaPso_ = nullptr;         // temporal AA resolve (primary AA)
    ID3D12PipelineState* ssrPso_ = nullptr;         // raster-tier screen-space reflections
    ID3D12PipelineState* ssgiPso_ = nullptr;        // raster-tier screen-space GI (dynamic bounce)
    ID3D12PipelineState* decalPso_ = nullptr;       // deferred projected decals (gbuffer albedo)
    ID3D12PipelineState* particleBlitPso_ = nullptr;// scene -> composite copy (particle pass)
    ID3D12PipelineState* particlePso_ = nullptr;    // additive particle billboards
    ID3D12PipelineState* smokePso_ = nullptr;       // alpha-blended dark shadow-smoke billboards
    ID3D12PipelineState* depthvizPso_ = nullptr;
    ID3D12PipelineState* outlinePso_ = nullptr;     // W2a screen-space ink outlines
    ID3D12PipelineState* uiPso_ = nullptr;

    Font        font_;

    rhi::Buffer frameCB_;               // persistent upload CBV (one frame in flight)
    uint32_t    frameCBIndex_ = 0;
    rhi::Buffer instanceBuf_;           // persistent upload structured buffer
    uint32_t    instanceSrvIndex_ = 0;
    uint32_t    instanceCapacity_ = 0;
    rhi::Buffer lightBuf_;              // persistent upload point-light buffer
    uint32_t    lightSrvIndex_ = 0;
    uint32_t    lightCapacity_ = 0;
    uint32_t    lightCount_ = 0;
    rhi::Buffer decalBuf_;              // persistent upload projected-decal buffer
    uint32_t    decalSrvIndex_ = 0;
    uint32_t    decalCapacity_ = 0;
    uint32_t    decalCount_ = 0;
    rhi::Buffer particleBuf_;           // persistent upload particle-billboard buffer
    uint32_t    particleSrvIndex_ = 0;
    uint32_t    particleCapacity_ = 0;
    uint32_t    particleCount_ = 0;
    rhi::Buffer smokeBuf_;              // persistent upload shadow-smoke billboard buffer
    uint32_t    smokeSrvIndex_ = 0;
    uint32_t    smokeCapacity_ = 0;
    uint32_t    smokeCount_ = 0;
    rhi::Buffer heatBuf_;               // persistent upload heat-haze source buffer (scene refraction)
    uint32_t    heatSrvIndex_ = 0;
    uint32_t    heatCapacity_ = 0;
    uint32_t    heatCount_ = 0;
    Vec3f       cameraRight_{ 1, 0, 0 };  // for camera-facing particle billboards
    Vec3f       cameraUp_{ 0, 1, 0 };
    rhi::Buffer uiBuf_;                 // persistent upload UI vertex buffer
    uint32_t    uiSrvIndex_ = 0;
    uint32_t    uiCapacity_ = 0;
    uint32_t    uiCount_ = 0;           // this frame's UI vertex count

    struct MeshEntry {
        rhi::Buffer vb, ib;
        uint32_t    vbSrv = 0;
        uint32_t    ibSrv = 0;          // structured (uint) SRV for RT hit-shading index fetch
        uint32_t    vertexCount = 0;
        uint32_t    vertexCapacity = 0;
        uint32_t    indexCount = 0;
        bool        dynamic = false;
        D3D12_INDEX_BUFFER_VIEW ibv{};
        rhi::Buffer blas;               // DXR bottom-level AS (built once; RT tier only)
    };
    struct TexEntry { rhi::Texture tex; uint32_t srv = 0; };
    struct MatEntry { uint32_t baseTexSrv = 0; Vec4f factor{ 1, 1, 1, 1 }; float emissive = 0;
                      float metallic = 0; float roughness = 0.8f;
                      uint32_t normalSrv = 0; uint32_t ormSrv = 0; float uvScale = 1.0f;
                      uint32_t emissiveSrv = 0; float emissiveStrength = 1.0f;  // per-texel emissive map + HDR scale
                      float metalScale = 1.0f; float roughBoost = 0.0f;
                      uint32_t styleFlags = 0; };   // W5 master-material category + masks (CPU-side; GPU plumbing deferred)

    std::vector<MeshEntry> meshes_;
    std::vector<TexEntry>  textures_;
    std::vector<MatEntry>  materials_;

    // TAA temporal state. Per-object previous transforms keyed by the stable
    // MeshInstance id; previous unjittered camera projections; jitter frame index;
    // history validity (false on first frame / after resize / headless capture).
    std::unordered_map<uint64_t, Mat4> prevTransforms_;
    Mat4     prevViewProjNoJitter_ = Mat4::identity();
    Mat4     prevViewProjCamNoJitter_ = Mat4::identity();
    uint32_t frameIndex_ = 0;
    bool     framePrevValid_ = false;
    bool     historyValid_ = false;
    // Screen-space god-ray anchor: the sun's projected screen UV + a strength (0 = no shafts,
    // e.g. sun off-screen or behind camera). Consumed by the tonemap's radial light shafts.
    float    sunScreenU_ = 0.5f, sunScreenV_ = 0.5f, godrayAmt_ = 0.0f;

    // DXR per-frame TLAS instance scratch + the current frame's TLAS bindless SRV.
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> tlasInstanceDescs_;
    uint32_t tlasSrvIndex_ = 0;     // 0 when RT disabled / no instances this frame
};

} // namespace pulse
