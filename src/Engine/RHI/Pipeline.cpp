#include "Engine/RHI/Pipeline.hpp"

namespace pulse::rhi {

namespace {

uint64_t fnv1a(const void* data, size_t bytes, uint64_t seed = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t hashBlob(const ShaderBlob& b, uint64_t seed) {
    return b ? fnv1a(b.data(), b.size(), seed) : seed;
}

CD3DX12_STATIC_SAMPLER_DESC sampler(UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE mode) {
    CD3DX12_STATIC_SAMPLER_DESC s(reg, filter, mode, mode, mode);
    s.MaxAnisotropy = 16;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    return s;
}

} // namespace

ComPtr<ID3D12RootSignature> createBindlessRootSignature(ID3D12Device* device) {
    CD3DX12_ROOT_PARAMETER1 params[1];
    params[0].InitAsConstants(kRootConstantCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,  D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        sampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,  D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        sampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT,   D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        sampler(3, D3D12_FILTER_ANISOTROPIC,         D3D12_TEXTURE_ADDRESS_MODE_WRAP),
    };

    // Bindless: the CBV/SRV/UAV heap is directly indexed by the shaders
    // (ResourceDescriptorHeap); samplers are the static ones above, so no sampler
    // heap is needed. No per-draw descriptor tables.
    const D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    desc.Init_1_1(_countof(params), params, _countof(samplers), samplers, flags);

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &error);
    if (FAILED(hr)) {
        const char* msg = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
        fail(formatStr("root signature serialize failed: %s", msg));
    }

    ComPtr<ID3D12RootSignature> rootSig;
    PULSE_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&rootSig)));
    rootSig->SetName(L"pulse.bindless.rootsig");
    return rootSig;
}

void PipelineCache::init(ID3D12Device* device, ID3D12RootSignature* rootSig) {
    device_ = device;
    rootSig_ = rootSig;
}

ID3D12PipelineState* PipelineCache::getGraphics(const GraphicsPipelineDesc& d) {
    uint64_t key = hashBlob(d.vs, hashBlob(d.ps, 1469598103934665603ull));
    struct PodState {
        DXGI_FORMAT dsv; D3D12_CULL_MODE cull; D3D12_FILL_MODE fill; uint32_t frontCCW;
        uint32_t depthTest, depthWrite; D3D12_COMPARISON_FUNC depthFunc;
        uint32_t blend; D3D12_PRIMITIVE_TOPOLOGY_TYPE topo; uint32_t rtvCount;
        DXGI_FORMAT rtv[8];
    } pod{};
    pod.dsv = d.dsvFormat; pod.cull = d.cullMode; pod.fill = d.fillMode; pod.frontCCW = d.frontCCW;
    pod.depthTest = d.depthTest; pod.depthWrite = d.depthWrite; pod.depthFunc = d.depthFunc;
    pod.blend = d.blendAlpha | (d.blendAdd ? 2u : 0u); pod.topo = d.topology;
    pod.rtvCount = static_cast<uint32_t>(d.rtvFormats.size());
    for (size_t i = 0; i < d.rtvFormats.size() && i < 8; ++i) pod.rtv[i] = d.rtvFormats[i];
    key = fnv1a(&pod, sizeof(pod), key);

    if (auto it = graphics_.find(key); it != graphics_.end()) return it->second.Get();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSig_;
    desc.VS = d.vs.bytecode();
    desc.PS = d.ps.bytecode();
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = d.cullMode;
    desc.RasterizerState.FillMode = d.fillMode;
    desc.RasterizerState.FrontCounterClockwise = d.frontCCW ? TRUE : FALSE;

    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    if (d.blendAlpha) {
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (d.blendAdd) {
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;     // premultiplied-ish: alpha-weighted add
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }

    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthEnable = d.depthTest ? TRUE : FALSE;
    desc.DepthStencilState.DepthWriteMask = d.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL
                                                         : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = d.depthFunc;

    desc.InputLayout = { nullptr, 0 };                   // bindless vertex pulling: no IA layout
    desc.PrimitiveTopologyType = d.topology;
    desc.NumRenderTargets = static_cast<UINT>(d.rtvFormats.size());
    for (size_t i = 0; i < d.rtvFormats.size() && i < 8; ++i) desc.RTVFormats[i] = d.rtvFormats[i];
    desc.DSVFormat = d.dsvFormat;
    desc.SampleDesc = { 1, 0 };

    ComPtr<ID3D12PipelineState> pso;
    PULSE_HR(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    ID3D12PipelineState* raw = pso.Get();
    graphics_.emplace(key, std::move(pso));
    return raw;
}

ID3D12PipelineState* PipelineCache::getCompute(const ComputePipelineDesc& d) {
    const uint64_t key = hashBlob(d.cs, 1469598103934665603ull);
    if (auto it = compute_.find(key); it != compute_.end()) return it->second.Get();

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSig_;
    desc.CS = d.cs.bytecode();

    ComPtr<ID3D12PipelineState> pso;
    PULSE_HR(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    ID3D12PipelineState* raw = pso.Get();
    compute_.emplace(key, std::move(pso));
    return raw;
}

} // namespace pulse::rhi
