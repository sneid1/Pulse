#include "Engine/RHI/Heaps.hpp"

namespace pulse::rhi {

void Heaps::init(ID3D12Device* device, uint32_t bindlessCapacity,
                 uint32_t rtvCapacity, uint32_t dsvCapacity) {
    device_ = device;
    bindless_.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, bindlessCapacity, true, L"pulse.bindless");
    rtv_.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvCapacity, false, L"pulse.rtv");
    dsv_.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvCapacity, false, L"pulse.dsv");
}

uint32_t Heaps::createTextureSrv(const Texture& tex) {
    return createTextureSrv(tex.get(), tex.format, tex.mips);
}

uint32_t Heaps::createTextureSrv(ID3D12Resource* res, DXGI_FORMAT format, uint32_t mips, uint32_t reuse) {
    const uint32_t index = (reuse == kReuseNone) ? bindless_.allocate() : reuse;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = mips;
    device_->CreateShaderResourceView(res, &desc, bindless_.cpu(index));
    return index;
}

uint32_t Heaps::createStructuredBufferSrv(ID3D12Resource* res, uint32_t numElements, uint32_t stride, uint32_t reuse) {
    const uint32_t index = (reuse == kReuseNone) ? bindless_.allocate() : reuse;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = numElements;
    desc.Buffer.StructureByteStride = stride;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device_->CreateShaderResourceView(res, &desc, bindless_.cpu(index));
    return index;
}

uint32_t Heaps::createConstantBufferView(ID3D12Resource* res, uint32_t sizeBytes) {
    const uint32_t index = bindless_.allocate();
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = res->GetGPUVirtualAddress();
    desc.SizeInBytes = (sizeBytes + 255u) & ~255u;
    device_->CreateConstantBufferView(&desc, bindless_.cpu(index));
    return index;
}

uint32_t Heaps::createTextureUav(const Texture& tex, uint32_t reuse) {
    const uint32_t index = (reuse == kReuseNone) ? bindless_.allocate() : reuse;
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = tex.format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(tex.get(), nullptr, &desc, bindless_.cpu(index));
    return index;
}

uint32_t Heaps::createTexture3DSrv(ID3D12Resource* res, DXGI_FORMAT format, uint32_t mips, uint32_t reuse) {
    const uint32_t index = (reuse == kReuseNone) ? bindless_.allocate() : reuse;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture3D.MipLevels = mips;
    device_->CreateShaderResourceView(res, &desc, bindless_.cpu(index));
    return index;
}

uint32_t Heaps::createTexture3DUav(ID3D12Resource* res, DXGI_FORMAT format, uint32_t depth, uint32_t reuse) {
    const uint32_t index = (reuse == kReuseNone) ? bindless_.allocate() : reuse;
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    desc.Texture3D.WSize = depth;
    device_->CreateUnorderedAccessView(res, nullptr, &desc, bindless_.cpu(index));
    return index;
}

uint32_t Heaps::createTlasSrv(D3D12_GPU_VIRTUAL_ADDRESS tlasAddress) {
    const uint32_t index = bindless_.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.RaytracingAccelerationStructure.Location = tlasAddress;
    // The AS SRV references the structure by GPU VA, so the resource arg is null.
    device_->CreateShaderResourceView(nullptr, &desc, bindless_.cpu(index));
    return index;
}

D3D12_CPU_DESCRIPTOR_HANDLE Heaps::createRtv(const Texture& tex) {
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_.cpu(rtv_.allocate());
    recreateRtv(handle, tex);
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE Heaps::createDsv(const Texture& tex, DXGI_FORMAT dsvFormat) {
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = dsv_.cpu(dsv_.allocate());
    recreateDsv(handle, tex, dsvFormat);
    return handle;
}

void Heaps::recreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const Texture& tex) {
    device_->CreateRenderTargetView(tex.get(), nullptr, handle);
}

void Heaps::recreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const Texture& tex, DXGI_FORMAT dsvFormat) {
    D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
    desc.Format = dsvFormat;
    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(tex.get(), &desc, handle);
}

} // namespace pulse::rhi
