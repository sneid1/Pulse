#pragma once

#include "Engine/RHI/Descriptors.hpp"
#include "Engine/RHI/Resource.hpp"

namespace pulse::rhi {

// The engine's descriptor heaps: one shader-visible CBV/SRV/UAV heap (the
// bindless table indexed by ResourceDescriptorHeap in shaders) plus CPU-only RTV
// and DSV heaps. Owned once by the Engine; the render graph and resource loaders
// allocate from it. Bump allocation, descriptors persist for the device life.
class Heaps {
public:
    // RTV/DSV heaps are CPU-only and tiny (a few bytes per slot), so they get
    // generous headroom: with descriptor reuse in place these stay near-constant,
    // but the slack keeps any unforeseen churn far from exhaustion.
    void init(ID3D12Device* device,
              uint32_t bindlessCapacity = 1u << 16,
              uint32_t rtvCapacity = 4096,
              uint32_t dsvCapacity = 1024);

    // Sentinel for the optional reuse index: pass an existing heap index to rewrite
    // a view in place (no new allocation) instead of bump-allocating a fresh slot.
    static constexpr uint32_t kReuseNone = 0xFFFFFFFFu;

    // Create an SRV for a texture in the bindless heap; returns its heap index
    // (what a shader passes to ResourceDescriptorHeap[index]).
    uint32_t createTextureSrv(const Texture& tex);
    uint32_t createTextureSrv(ID3D12Resource* res, DXGI_FORMAT format, uint32_t mips,
                              uint32_t reuse = kReuseNone);

    // Create an SRV for a structured buffer (bindless vertex/instance pulling).
    uint32_t createStructuredBufferSrv(ID3D12Resource* res, uint32_t numElements, uint32_t stride,
                                       uint32_t reuse = kReuseNone);

    // Create a CBV in the bindless heap; returns its heap index. sizeBytes is
    // rounded up to 256 (the CBV alignment).
    uint32_t createConstantBufferView(ID3D12Resource* res, uint32_t sizeBytes);

    // Create a UAV for a texture in the bindless heap; returns its heap index.
    uint32_t createTextureUav(const Texture& tex, uint32_t reuse = kReuseNone);

    // 3D texture views (froxel volumes: volumetric fog). depth = slice count.
    uint32_t createTexture3DSrv(ID3D12Resource* res, DXGI_FORMAT format, uint32_t mips,
                                uint32_t reuse = kReuseNone);
    uint32_t createTexture3DUav(ID3D12Resource* res, DXGI_FORMAT format, uint32_t depth,
                                uint32_t reuse = kReuseNone);

    // Create an SRV over a raytracing acceleration structure (the TLAS) in the
    // bindless heap; returns its index. The resource itself is referenced by GPU
    // VA in the desc (CreateShaderResourceView is called with a null resource).
    uint32_t createTlasSrv(D3D12_GPU_VIRTUAL_ADDRESS tlasAddress);

    // RTV / DSV (CPU handles, used by OMSetRenderTargets).
    D3D12_CPU_DESCRIPTOR_HANDLE createRtv(const Texture& tex);
    D3D12_CPU_DESCRIPTOR_HANDLE createDsv(const Texture& tex, DXGI_FORMAT dsvFormat);
    // Rewrite an RTV/DSV into an already-allocated CPU handle (no new allocation),
    // used when a pooled transient resource is recreated under the same slot.
    void recreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const Texture& tex);
    void recreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const Texture& tex, DXGI_FORMAT dsvFormat);

    DescriptorHeap& bindless() { return bindless_; }
    ID3D12DescriptorHeap* bindlessHeap() const { return bindless_.heap(); }

private:
    ID3D12Device*  device_ = nullptr;
    DescriptorHeap bindless_;
    DescriptorHeap rtv_;
    DescriptorHeap dsv_;
};

} // namespace pulse::rhi
