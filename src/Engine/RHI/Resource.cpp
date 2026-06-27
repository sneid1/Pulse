#include "Engine/RHI/Resource.hpp"

namespace pulse::rhi {

Buffer createUploadBuffer(ID3D12Device* device, uint64_t size, const wchar_t* name) {
    Buffer b;
    b.size = size;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    PULSE_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&b.resource)));
    if (name) b.resource->SetName(name);
    b.gpuAddress = b.resource->GetGPUVirtualAddress();
    const CD3DX12_RANGE noRead(0, 0);
    PULSE_HR(b.resource->Map(0, &noRead, &b.mapped));
    return b;
}

Buffer createReadbackBuffer(ID3D12Device* device, uint64_t size, const wchar_t* name) {
    Buffer b;
    b.size = size;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    PULSE_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b.resource)));
    if (name) b.resource->SetName(name);
    return b;   // mapped on demand by the reader (readback timing matters)
}

Buffer createDefaultBuffer(ID3D12Device* device, uint64_t size,
                           D3D12_RESOURCE_STATES initialState, bool allowUav,
                           const wchar_t* name) {
    Buffer b;
    b.size = size;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    if (allowUav) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // D3D12 forces buffers to be created in COMMON (and warns otherwise); the
    // first copy/use promotes implicitly, after which `initialState` is reached
    // via a barrier by the caller.
    (void)initialState;
    PULSE_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b.resource)));
    if (name) b.resource->SetName(name);
    b.gpuAddress = b.resource->GetGPUVirtualAddress();
    return b;
}

Texture createTexture2D(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
                        TextureUsage usage, D3D12_RESOURCE_STATES initialState,
                        uint32_t mips, const float* clearColor, const wchar_t* name) {
    Texture t;
    t.width = w; t.height = h; t.mips = mips; t.format = format; t.state = initialState;

    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (hasFlag(usage, TextureUsage::RenderTarget))    flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (hasFlag(usage, TextureUsage::DepthStencil))    flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (hasFlag(usage, TextureUsage::UnorderedAccess)) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
        format, w, h, 1, static_cast<UINT16>(mips), 1, 0, flags);

    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* pClear = nullptr;
    if (clearColor && hasFlag(usage, TextureUsage::RenderTarget)) {
        clear.Format = format;
        for (int i = 0; i < 4; ++i) clear.Color[i] = clearColor[i];
        pClear = &clear;
    } else if (hasFlag(usage, TextureUsage::DepthStencil)) {
        // A typeless depth resource needs a TYPED clear-value format.
        clear.Format = (format == DXGI_FORMAT_R32_TYPELESS)   ? DXGI_FORMAT_D32_FLOAT
                     : (format == DXGI_FORMAT_R24G8_TYPELESS) ? DXGI_FORMAT_D24_UNORM_S8_UINT
                     : format;
        clear.DepthStencil.Depth = clearColor ? clearColor[0] : 0.0f;   // reverse-Z default
        clear.DepthStencil.Stencil = 0;
        pClear = &clear;
    }

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    PULSE_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        initialState, pClear, IID_PPV_ARGS(&t.resource)));
    if (name) t.resource->SetName(name);
    return t;
}

} // namespace pulse::rhi
