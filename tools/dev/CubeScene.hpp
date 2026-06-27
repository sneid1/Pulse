#pragma once

// A self-contained spinning textured cube, used by the windowed dev demo
// (pulse_window). Builds its bindless resources once and renders a frame through
// the render graph into a caller-provided LDR target. Mirrors the headless cube
// stage in engine_smoke so the windowed and headless paths exercise the same code.

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Heaps.hpp"
#include "Engine/RHI/Pipeline.hpp"
#include "Engine/RHI/Uploader.hpp"
#include "Engine/RHI/Resource.hpp"
#include "Engine/RenderGraph/RenderGraph.hpp"
#include "Engine/Shaders/ShaderCompiler.hpp"
#include "Engine/Core/Mat.hpp"

#include <cstring>
#include <vector>

namespace pulse {

class CubeScene {
public:
    struct Vertex { Vec3f pos; Vec3f nrm; float u, v; Vec4f color; };
    struct CameraCB { Mat4 viewProj; Mat4 model; Vec3f lightDir; float ambient; };

    void init(rhi::Device& device) {
        device_ = &device;
        sc_.init();
        vs_ = sc_.compile("mesh.hlsl", L"VSMain", L"vs_6_6");
        ps_ = sc_.compile("mesh.hlsl", L"PSMain", L"ps_6_6");
        rootSig_ = rhi::createBindlessRootSignature(device.d3d());
        pipelines_.init(device.d3d(), rootSig_.Get());
        heaps_.init(device.d3d());
        graph_.init(&device, &heaps_, rootSig_.Get());

        rhi::Uploader up; up.init(&device);

        std::vector<Vertex> verts;
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
        indexCount_ = static_cast<uint32_t>(indices.size());

        const uint32_t TX = 64;
        std::vector<uint8_t> checker(TX * TX * 4);
        for (uint32_t y = 0; y < TX; ++y)
            for (uint32_t x = 0; x < TX; ++x) {
                const bool on = ((x / 8) ^ (y / 8)) & 1;
                uint8_t* px = &checker[(y * TX + x) * 4];
                const uint8_t v = on ? 235 : 90;
                px[0] = v; px[1] = v; px[2] = on ? 235 : 120; px[3] = 255;
            }

        vb_ = up.uploadBuffer(verts.data(), verts.size() * sizeof(Vertex),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false, L"cube.vb");
        ib_ = up.uploadBuffer(indices.data(), indices.size() * sizeof(uint32_t),
                              D3D12_RESOURCE_STATE_INDEX_BUFFER, false, L"cube.ib");
        tex_ = up.uploadTexture2D(checker.data(), TX, TX, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"cube.tex");
        up.flush();

        vbIndex_  = heaps_.createStructuredBufferSrv(vb_.get(), static_cast<uint32_t>(verts.size()), sizeof(Vertex));
        texIndex_ = heaps_.createTextureSrv(tex_);

        camBuf_ = rhi::createUploadBuffer(device.d3d(), 256, L"cube.cam");
        camIndex_ = heaps_.createConstantBufferView(camBuf_.get(), sizeof(CameraCB));

        rhi::GraphicsPipelineDesc gd;
        gd.vs = vs_; gd.ps = ps_;
        gd.rtvFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
        gd.dsvFormat = DXGI_FORMAT_D32_FLOAT;
        gd.cullMode = D3D12_CULL_MODE_BACK;
        gd.depthTest = true; gd.depthWrite = true;
        gd.depthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        pso_ = pipelines_.getGraphics(gd);

        ibv_ = { ib_.gpuAddress, indexCount_ * 4u, DXGI_FORMAT_R32_UINT };
    }

    // Update the spinning camera for time t (seconds) and the given aspect ratio.
    void updateCamera(float t, float aspect) {
        const float fov = 55.0f * 3.14159265f / 180.0f;
        CameraCB cb;
        cb.viewProj = mul(lookAtLH({ 0.0f, 1.4f, -3.2f }, { 0, 0, 0 }, { 0, 1, 0 }),
                          perspectiveReverseZ(fov, aspect, 0.05f));
        cb.model = mul(rotationY(t * 0.8f), rotationX(0.35f));
        cb.lightDir = normalize3({ 0.4f, 0.85f, -0.35f });
        cb.ambient = 0.20f;
        std::memcpy(camBuf_.mapped, &cb, sizeof(cb));
    }

    // Record a frame into the given LDR target (imported each frame; the graph
    // owns a matching transient depth). The LDR is left in RENDER_TARGET state.
    void render(ID3D12GraphicsCommandList* list, rhi::Texture& ldr) {
        const float clearColor[4] = { 0.02f, 0.03f, 0.05f, 1.0f };
        graph_.reset();
        render::RGHandle ldrH = graph_.importTexture("ldr", &ldr, D3D12_RESOURCE_STATE_RENDER_TARGET, true, clearColor);

        render::RGTextureDesc depthDesc;
        depthDesc.width = ldr.width; depthDesc.height = ldr.height; depthDesc.format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.usage = rhi::TextureUsage::DepthStencil; depthDesc.clear = true; depthDesc.clearDepth = 0.0f;
        depthDesc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
        render::RGHandle depthH = graph_.createTexture("depth", depthDesc);

        struct Push { uint32_t camIndex, vbIndex, texIndex, pad; } push{ camIndex_, vbIndex_, texIndex_, 0 };
        graph_.addRasterPass("mesh",
            [&](render::PassBuilder& b){ b.renderTarget(ldrH); b.depthTarget(depthH); },
            [&, push](render::PassContext& ctx){
                ctx.cmd->SetPipelineState(pso_);
                ctx.graphicsConstants(&push, 4);
                ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx.cmd->IASetIndexBuffer(&ibv_);
                ctx.cmd->DrawIndexedInstanced(indexCount_, 1, 0, 0, 0);
            });
        graph_.compile();
        graph_.execute(list);
    }

    rhi::Heaps& heaps() { return heaps_; }

private:
    rhi::Device* device_ = nullptr;
    rhi::ShaderCompiler sc_;
    rhi::ShaderBlob vs_, ps_;
    rhi::ComPtr<ID3D12RootSignature> rootSig_;
    rhi::PipelineCache pipelines_;
    rhi::Heaps heaps_;
    render::RenderGraph graph_;
    rhi::Buffer vb_, ib_, camBuf_;
    rhi::Texture tex_;
    uint32_t vbIndex_ = 0, texIndex_ = 0, camIndex_ = 0, indexCount_ = 0;
    ID3D12PipelineState* pso_ = nullptr;
    D3D12_INDEX_BUFFER_VIEW ibv_{};
};

} // namespace pulse
