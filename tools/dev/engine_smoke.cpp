// Engine bring-up driver. Not the game, a standalone exe that exercises engine
// layers as they land, so the RHI/render-graph/present path can be validated with
// the D3D12 debug layer ON before the game is ported. Grows with M0.

#define PULSE_AGILITY_SDK_IMPL
#include "Engine/RHI/AgilitySDK.hpp"

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Descriptors.hpp"
#include "Engine/RHI/Resource.hpp"
#include "Engine/RHI/Commands.hpp"
#include "Engine/RHI/Readback.hpp"
#include "Engine/RHI/Pipeline.hpp"
#include "Engine/RHI/Heaps.hpp"
#include "Engine/RHI/Uploader.hpp"
#include "Engine/RenderGraph/RenderGraph.hpp"
#include "Engine/Shaders/ShaderCompiler.hpp"
#include "Engine/Core/Image.hpp"
#include "Engine/Core/Mat.hpp"
#include "Engine/Core/Paths.hpp"
#include "Engine/Engine.hpp"
#include "DemoScene.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace pulse;

static rhi::Device makeDevice() {
    rhi::DeviceConfig cfg;
#if defined(PULSE_DEBUG_LAYER)
    cfg.enableDebugLayer = true;
#endif
#if defined(PULSE_GPU_VALIDATION)
    cfg.enableGpuValidation = true;
#endif
    return rhi::Device::create(cfg);
}

// Stage 1: clear an offscreen LDR target, read it back, write a BMP. Exercises
// descriptor heaps, committed textures, the command list, the graphics-queue
// fence, and the readback path end to end under GPU validation.
static int clearToBmp(rhi::Device& device) {
    const uint32_t W = 256, H = 144;
    const float clearColor[4] = { 0.10f, 0.45f, 0.85f, 1.0f };

    rhi::Texture ldr = rhi::createTexture2D(device.d3d(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
        rhi::TextureUsage::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, clearColor, L"pulse.ldr");

    rhi::DescriptorHeap rtvHeap;
    rtvHeap.init(device.d3d(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, false, L"pulse.rtv");
    const uint32_t rtvIndex = rtvHeap.allocate();
    device.d3d()->CreateRenderTargetView(ldr.get(), nullptr, rtvHeap.cpu(rtvIndex));

    rhi::CommandList cmd;
    cmd.init(device.d3d(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.clear.cmd");
    ID3D12GraphicsCommandList* list = cmd.begin();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap.cpu(rtvIndex);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmd.close();
    ID3D12CommandList* lists[] = { list };
    device.graphicsQueue()->ExecuteCommandLists(1, lists);
    device.flushGraphics();

    Image img = rhi::readbackTexture(device, ldr, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const uint8_t* p = &img.rgba[(static_cast<size_t>(H / 2) * W + W / 2) * 4];
    std::printf("[engine_smoke] clear: centre RGBA=(%d,%d,%d,%d) %s\n",
                p[0], p[1], p[2], p[3], img.saveBmp("engine_smoke_clear.bmp") ? "-> bmp" : "BMP FAIL");
    return 0;
}

// Stage 2: render a vertex-coloured triangle to an LDR target via the bindless
// root signature + PSO cache + dxc, read it back, write a BMP.
static int triangleToBmp(rhi::Device& device) {
    const uint32_t W = 512, H = 288;
    const float clearColor[4] = { 0.04f, 0.05f, 0.07f, 1.0f };

    rhi::ShaderCompiler sc; sc.init();
    rhi::ShaderBlob vs = sc.compile("triangle.hlsl", L"VSMain", L"vs_6_6");
    rhi::ShaderBlob ps = sc.compile("triangle.hlsl", L"PSMain", L"ps_6_6");
    std::printf("[engine_smoke] triangle shaders: VS %zu B, PS %zu B\n", vs.size(), ps.size());

    rhi::ComPtr<ID3D12RootSignature> rootSig = rhi::createBindlessRootSignature(device.d3d());
    rhi::PipelineCache pso; pso.init(device.d3d(), rootSig.Get());

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs; gd.ps = ps;
    gd.rtvFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
    gd.cullMode = D3D12_CULL_MODE_NONE;
    gd.depthTest = false;
    ID3D12PipelineState* trianglePso = pso.getGraphics(gd);

    rhi::DescriptorHeap bindless;
    bindless.init(device.d3d(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, true, L"pulse.bindless");

    rhi::Texture ldr = rhi::createTexture2D(device.d3d(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
        rhi::TextureUsage::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, clearColor, L"pulse.ldr");
    rhi::DescriptorHeap rtvHeap;
    rtvHeap.init(device.d3d(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, false, L"pulse.rtv");
    const uint32_t rtvIndex = rtvHeap.allocate();
    device.d3d()->CreateRenderTargetView(ldr.get(), nullptr, rtvHeap.cpu(rtvIndex));

    rhi::CommandList cmd;
    cmd.init(device.d3d(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.triangle.cmd");
    ID3D12GraphicsCommandList* list = cmd.begin();

    ID3D12DescriptorHeap* heaps[] = { bindless.heap() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSig.Get());

    const D3D12_VIEWPORT vp{ 0, 0, float(W), float(H), 0.0f, 1.0f };
    const D3D12_RECT sc2{ 0, 0, LONG(W), LONG(H) };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc2);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap.cpu(rtvIndex);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    list->SetPipelineState(trianglePso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    cmd.close();
    ID3D12CommandList* lists[] = { list };
    device.graphicsQueue()->ExecuteCommandLists(1, lists);
    device.flushGraphics();

    Image img = rhi::readbackTexture(device, ldr, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const uint8_t* c = &img.rgba[(static_cast<size_t>(H / 2) * W + W / 2) * 4];
    std::printf("[engine_smoke] triangle: centre RGBA=(%d,%d,%d,%d) %s\n",
                c[0], c[1], c[2], c[3], img.saveBmp("engine_smoke_triangle.bmp") ? "-> bmp" : "BMP FAIL");
    return 0;
}

// Stage 3: render the triangle THROUGH the render graph. Proves pass authoring,
// auto-barriers, pass culling (an unused pass is dropped), and the --render-pass
// named-resource dump.
static int graphTriangleToBmp(rhi::Device& device) {
    const uint32_t W = 512, H = 288;

    rhi::ShaderCompiler sc; sc.init();
    rhi::ShaderBlob vs = sc.compile("triangle.hlsl", L"VSMain", L"vs_6_6");
    rhi::ShaderBlob ps = sc.compile("triangle.hlsl", L"PSMain", L"ps_6_6");

    rhi::ComPtr<ID3D12RootSignature> rootSig = rhi::createBindlessRootSignature(device.d3d());
    rhi::PipelineCache pipelines; pipelines.init(device.d3d(), rootSig.Get());
    rhi::Heaps heaps; heaps.init(device.d3d());

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs; gd.ps = ps;
    gd.rtvFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
    gd.cullMode = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* trianglePso = pipelines.getGraphics(gd);

    // The output target, imported into the graph.
    const float clearColor[4] = { 0.04f, 0.05f, 0.07f, 1.0f };
    rhi::Texture ldr = rhi::createTexture2D(device.d3d(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
        rhi::TextureUsage::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, clearColor, L"pulse.ldr");

    render::RenderGraph graph;
    graph.init(&device, &heaps, rootSig.Get());
    graph.reset();

    render::RGHandle ldrH = graph.importTexture("ldr", &ldr, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                /*clear*/ true, clearColor);

    // A transient nobody consumes -> the pass writing it must be culled.
    render::RGTextureDesc deadDesc; deadDesc.width = 64; deadDesc.height = 64;
    deadDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM; deadDesc.usage = rhi::TextureUsage::RenderTarget;
    deadDesc.clear = true;
    render::RGHandle deadH = graph.createTexture("dead_unused", deadDesc);
    graph.addRasterPass("cullme",
        [&](render::PassBuilder& b){ b.renderTarget(deadH); },
        [&](render::PassContext&){ /* should never run */ });

    graph.addRasterPass("scene",
        [&](render::PassBuilder& b){ b.renderTarget(ldrH); },
        [&](render::PassContext& ctx){
            ctx.cmd->SetPipelineState(trianglePso);
            ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx.cmd->DrawInstanced(3, 1, 0, 0);
        });

    graph.compile();

    rhi::CommandList cmd;
    cmd.init(device.d3d(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.graph.cmd");
    ID3D12GraphicsCommandList* list = cmd.begin();
    graph.execute(list);
    cmd.close();
    ID3D12CommandList* lists[] = { list };
    device.graphicsQueue()->ExecuteCommandLists(1, lists);
    device.flushGraphics();

    // --render-pass dump by name.
    rhi::Texture* dumped = graph.findTexture("ldr");
    if (!dumped) { std::printf("[engine_smoke] graph: findTexture(ldr) FAILED\n"); return 5; }
    Image img = rhi::readbackTexture(device, *dumped, graph.stateOf("ldr"));
    std::printf("[engine_smoke] graph: dumped 'ldr' %s\n",
                img.saveBmp("engine_smoke_graph.bmp") ? "-> bmp" : "BMP FAIL");
    return 0;
}

// Stage 4: textured 3D cube. Proves the uploader (vertex/index/texture to DEFAULT
// heap), bindless vertex pulling + bindless camera CBV + bindless albedo texture,
// engine matrices, reverse-Z depth, all through the render graph (colour + depth).
namespace {
struct CubeVertex { pulse::Vec3f pos; pulse::Vec3f nrm; float u, v; pulse::Vec4f color; };
struct CameraCB   { pulse::Mat4 viewProj; pulse::Mat4 model; pulse::Vec3f lightDir; float ambient; };
}

static int cubeToBmp(rhi::Device& device) {
    using namespace pulse;
    const uint32_t W = 512, H = 288;

    rhi::ShaderCompiler sc; sc.init();
    rhi::ShaderBlob vs = sc.compile("mesh.hlsl", L"VSMain", L"vs_6_6");
    rhi::ShaderBlob ps = sc.compile("mesh.hlsl", L"PSMain", L"ps_6_6");

    rhi::ComPtr<ID3D12RootSignature> rootSig = rhi::createBindlessRootSignature(device.d3d());
    rhi::PipelineCache pipelines; pipelines.init(device.d3d(), rootSig.Get());
    rhi::Heaps heaps; heaps.init(device.d3d());
    rhi::Uploader up; up.init(&device);

    // --- cube geometry (6 faces, outward CCW) ---
    std::vector<CubeVertex> verts;
    std::vector<uint32_t> indices;
    auto addFace = [&](Vec3f n, Vec3f uax, Vec3f vax, Vec4f col) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        const Vec3f c = n * 0.5f;
        verts.push_back({ c - uax * 0.5f - vax * 0.5f, n, 0, 0, col });
        verts.push_back({ c + uax * 0.5f - vax * 0.5f, n, 1, 0, col });
        verts.push_back({ c + uax * 0.5f + vax * 0.5f, n, 1, 1, col });
        verts.push_back({ c - uax * 0.5f + vax * 0.5f, n, 0, 1, col });
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    };
    addFace({ 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1.0f, 0.55f, 0.4f, 1 });
    addFace({ -1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0.4f, 0.6f, 1.0f, 1 });
    addFace({ 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0.6f, 1.0f, 0.55f, 1 });
    addFace({ 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 1.0f, 0.9f, 0.4f, 1 });
    addFace({ 0, 0, 1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0.9f, 0.5f, 1.0f, 1 });
    addFace({ 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0.5f, 0.9f, 1.0f, 1 });

    // --- checker albedo texture ---
    const uint32_t TX = 64;
    std::vector<uint8_t> checker(TX * TX * 4);
    for (uint32_t y = 0; y < TX; ++y)
        for (uint32_t x = 0; x < TX; ++x) {
            const bool on = ((x / 8) ^ (y / 8)) & 1;
            uint8_t* px = &checker[(y * TX + x) * 4];
            const uint8_t v = on ? 235 : 90;
            px[0] = v; px[1] = v; px[2] = on ? 235 : 120; px[3] = 255;
        }

    rhi::Buffer vb = up.uploadBuffer(verts.data(), verts.size() * sizeof(CubeVertex),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false, L"cube.vb");
    rhi::Buffer ib = up.uploadBuffer(indices.data(), indices.size() * sizeof(uint32_t),
                                     D3D12_RESOURCE_STATE_INDEX_BUFFER, false, L"cube.ib");
    rhi::Texture tex = up.uploadTexture2D(checker.data(), TX, TX, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"cube.tex");
    up.flush();

    const uint32_t vbIndex  = heaps.createStructuredBufferSrv(vb.get(), static_cast<uint32_t>(verts.size()), sizeof(CubeVertex));
    const uint32_t texIndex = heaps.createTextureSrv(tex);

    // --- camera CBV ---
    const float fov = 55.0f * 3.14159265f / 180.0f;
    CameraCB cb;
    cb.viewProj = mul(lookAtLH({ 2.2f, 1.7f, -3.0f }, { 0, 0, 0 }, { 0, 1, 0 }),
                      perspectiveReverseZ(fov, float(W) / float(H), 0.05f));
    cb.model = mul(rotationY(0.7f), rotationX(0.35f));
    cb.lightDir = normalize3({ 0.4f, 0.85f, -0.35f });
    cb.ambient = 0.22f;
    rhi::Buffer camBuf = rhi::createUploadBuffer(device.d3d(), 256, L"cube.cam");
    std::memcpy(camBuf.mapped, &cb, sizeof(cb));
    const uint32_t camIndex = heaps.createConstantBufferView(camBuf.get(), sizeof(cb));

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs; gd.ps = ps;
    gd.rtvFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
    gd.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    gd.cullMode = D3D12_CULL_MODE_NONE;
    gd.depthTest = true; gd.depthWrite = true;
    gd.depthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;   // reverse-Z
    ID3D12PipelineState* meshPso = pipelines.getGraphics(gd);

    const float clearColor[4] = { 0.02f, 0.03f, 0.05f, 1.0f };
    rhi::Texture ldr = rhi::createTexture2D(device.d3d(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM,
        rhi::TextureUsage::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, clearColor, L"pulse.ldr");

    render::RenderGraph graph;
    graph.init(&device, &heaps, rootSig.Get());
    graph.reset();
    render::RGHandle ldrH = graph.importTexture("ldr", &ldr, D3D12_RESOURCE_STATE_RENDER_TARGET, true, clearColor);

    render::RGTextureDesc depthDesc;
    depthDesc.width = W; depthDesc.height = H; depthDesc.format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.usage = rhi::TextureUsage::DepthStencil; depthDesc.clear = true; depthDesc.clearDepth = 0.0f;
    depthDesc.dsvFormat = DXGI_FORMAT_D32_FLOAT; depthDesc.srvFormat = DXGI_FORMAT_R32_FLOAT;
    render::RGHandle depthH = graph.createTexture("depth", depthDesc);

    struct Push { uint32_t camIndex, vbIndex, texIndex, pad; } push{ camIndex, vbIndex, texIndex, 0 };
    const D3D12_INDEX_BUFFER_VIEW ibv{ ib.gpuAddress, static_cast<UINT>(indices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT };

    graph.addRasterPass("mesh",
        [&](render::PassBuilder& b){ b.renderTarget(ldrH); b.depthTarget(depthH); },
        [&](render::PassContext& ctx){
            ctx.cmd->SetPipelineState(meshPso);
            ctx.graphicsConstants(&push, 4);
            ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx.cmd->IASetIndexBuffer(&ibv);
            ctx.cmd->DrawIndexedInstanced(static_cast<UINT>(indices.size()), 1, 0, 0, 0);
        });
    graph.compile();

    rhi::CommandList cmd;
    cmd.init(device.d3d(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.cube.cmd");
    ID3D12GraphicsCommandList* list = cmd.begin();
    graph.execute(list);
    cmd.close();
    ID3D12CommandList* lists[] = { list };
    device.graphicsQueue()->ExecuteCommandLists(1, lists);
    device.flushGraphics();

    Image img = rhi::readbackTexture(device, ldr, graph.stateOf("ldr"));
    std::printf("[engine_smoke] cube: %zu verts, %zu indices %s\n",
                verts.size(), indices.size(), img.saveBmp("engine_smoke_cube.bmp") ? "-> bmp" : "BMP FAIL");
    return 0;
}

// Stage 5: the full Engine, headless. Renders the shared demo scene via the same
// HDR forward + AgX tonemap graph the windowed path uses, then captures the final
// image and capturePass("depth"). This is the headless half of the M0 gate
// "renderFrame (windowed) and captureFrame (headless) produce the same image".
static int engineHeadless() {
    Engine engine;
    Engine::Config cfg;
    cfg.hwnd = nullptr;            // headless: no swapchain
    cfg.width = 640; cfg.height = 360;
#if defined(PULSE_DEBUG_LAYER)
    cfg.enableDebugLayer = true;
#endif
#if defined(PULSE_GPU_VALIDATION)
    cfg.enableGpuValidation = true;
#endif
    engine.init(cfg);

    demo::DemoScene scene;
    scene.create(engine);
    std::vector<MeshInstance> instances;
    const SceneFrame sf = scene.frame(instances, 0.6f, cfg.width, cfg.height);

    Image finalImg;
    engine.captureFrame(sf, finalImg);
    const bool a = finalImg.saveBmp("engine_headless_final.bmp");

    Image depthImg;
    engine.capturePass(sf, "depth", depthImg);
    const bool b = depthImg.saveBmp("engine_headless_depth.bmp");

    std::printf("[engine_smoke] engine headless: final %s, depth %s\n",
                a ? "-> bmp" : "FAIL", b ? "-> bmp" : "FAIL");
    return (a && b) ? 0 : 6;
}

int main() {
    setWorkingDirectoryToExecutableFolder();
    rhi::Device device = makeDevice();
    std::printf("%s\n", device.gpuInfoString().c_str());

    int rc = clearToBmp(device);
    if (rc == 0) rc = triangleToBmp(device);
    if (rc == 0) rc = graphTriangleToBmp(device);
    if (rc == 0) rc = cubeToBmp(device);
    device.flushGraphics();
    if (rc == 0) rc = engineHeadless();
    std::printf("[engine_smoke] %s\n", rc == 0 ? "ALL STAGES OK" : "FAILED");
    return rc;
}
