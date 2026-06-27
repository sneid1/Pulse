#pragma once

#include "Engine/RHI/D3D12Common.hpp"
#include "Engine/Shaders/ShaderCompiler.hpp"

#include <unordered_map>
#include <vector>

namespace pulse::rhi {

// One global bindless root signature shared by every graphics + compute PSO:
//   - root param 0: 16 DWORDs of root constants at b0 (push data: resource
//     indices, draw index, etc.)
//   - the CBV/SRV/UAV heap and sampler heap are DIRECTLY_INDEXED (SM6.6
//     ResourceDescriptorHeap / SamplerDescriptorHeap)
//   - static samplers s0..s3 (linear-wrap, linear-clamp, point-wrap, aniso-wrap)
// No per-draw descriptor tables: everything is an array index.
constexpr uint32_t kRootConstantCount = 20;   // DWORDs at b0 (M7: 16 -> 20 for the tonemap gradeTint)

ComPtr<ID3D12RootSignature> createBindlessRootSignature(ID3D12Device* device);

struct GraphicsPipelineDesc {
    ShaderBlob vs;
    ShaderBlob ps;
    std::vector<DXGI_FORMAT> rtvFormats;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_CULL_MODE cullMode  = D3D12_CULL_MODE_BACK;
    bool            frontCCW  = true;     // CCW = front face (glTF convention)
    D3D12_FILL_MODE fillMode  = D3D12_FILL_MODE_SOLID;
    bool depthTest  = false;
    bool depthWrite = false;
    D3D12_COMPARISON_FUNC depthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reverse-Z
    bool blendAlpha = false;
    bool blendAdd = false;        // additive (sprites/particles): src + dst
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
};

struct ComputePipelineDesc {
    ShaderBlob cs;
};

// PSO cache keyed by a hash of the desc (shader bytecode + state). The plan's
// "PSO + root-signature cache keyed by state".
class PipelineCache {
public:
    void init(ID3D12Device* device, ID3D12RootSignature* rootSig);

    ID3D12PipelineState* getGraphics(const GraphicsPipelineDesc& desc);
    ID3D12PipelineState* getCompute(const ComputePipelineDesc& desc);
    ID3D12RootSignature* rootSignature() const { return rootSig_; }

private:
    ID3D12Device*        device_  = nullptr;
    ID3D12RootSignature* rootSig_ = nullptr;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> graphics_;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> compute_;
};

} // namespace pulse::rhi
