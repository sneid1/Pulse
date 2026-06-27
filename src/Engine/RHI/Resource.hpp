#pragma once

#include "Engine/RHI/D3D12Common.hpp"

namespace pulse::rhi {

struct Buffer {
    ComPtr<ID3D12Resource>    resource;
    uint64_t                  size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    void*                     mapped = nullptr;   // non-null for UPLOAD/READBACK buffers

    ID3D12Resource* get() const { return resource.Get(); }
    explicit operator bool() const { return resource != nullptr; }
};

struct Texture {
    ComPtr<ID3D12Resource> resource;
    uint32_t    width  = 0;
    uint32_t    height = 0;
    uint32_t    mips   = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

    ID3D12Resource* get() const { return resource.Get(); }
    explicit operator bool() const { return resource != nullptr; }
};

enum class TextureUsage : uint32_t {
    None         = 0,
    RenderTarget = 1 << 0,
    DepthStencil = 1 << 1,
    UnorderedAccess = 1 << 2,
};
inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool hasFlag(TextureUsage v, TextureUsage f) {
    return (static_cast<uint32_t>(v) & static_cast<uint32_t>(f)) != 0;
}

// Creation helpers (committed resources). The render graph layers transient
// aliasing on top of these for intermediate targets.
Buffer createUploadBuffer  (ID3D12Device* device, uint64_t size, const wchar_t* name = nullptr);
Buffer createReadbackBuffer(ID3D12Device* device, uint64_t size, const wchar_t* name = nullptr);
Buffer createDefaultBuffer (ID3D12Device* device, uint64_t size,
                            D3D12_RESOURCE_STATES initialState,
                            bool allowUav = false, const wchar_t* name = nullptr);

Texture createTexture2D(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
                        TextureUsage usage, D3D12_RESOURCE_STATES initialState,
                        uint32_t mips = 1, const float* clearColor = nullptr,
                        const wchar_t* name = nullptr);

} // namespace pulse::rhi
