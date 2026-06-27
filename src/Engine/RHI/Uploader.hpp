#pragma once

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Resource.hpp"
#include "Engine/RHI/Commands.hpp"

#include <vector>

namespace pulse::rhi {

// Load-time uploader: stages CPU data into UPLOAD buffers and copies it into
// DEFAULT-heap buffers/textures on the graphics queue. Uploads accumulate on one
// command list; flush() executes them and waits, then releases the staging
// memory. Used at load (meshes, textures), not per frame.
class Uploader {
public:
    void init(Device* device);

    Buffer uploadBuffer(const void* data, uint64_t size, D3D12_RESOURCE_STATES finalState,
                        bool allowUav = false, const wchar_t* name = nullptr);

    Texture uploadTexture2D(const void* pixels, uint32_t width, uint32_t height, DXGI_FORMAT format,
                            D3D12_RESOURCE_STATES finalState, const wchar_t* name = nullptr);

    // UI/font atlases should not inherit the full texture mip chain used by world
    // materials: those box-filtered mips make small glyphs look soft.
    Texture uploadTexture2DNoMips(const void* pixels, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                  D3D12_RESOURCE_STATES finalState, const wchar_t* name = nullptr);

    // Load a BCn DDS (DX10 or legacy DXT header) and upload its full mip chain as a
    // block-compressed texture (no CPU mip generation; the DDS carries the mips).
    // Returns an empty Texture on failure (the caller fails loud).
    Texture uploadDDS(const std::string& path, const wchar_t* name = nullptr);

    // Execute all accumulated copies and wait for completion; frees staging.
    void flush();

private:
    ID3D12GraphicsCommandList* ensureOpen();

    Device*             device_ = nullptr;
    CommandList         cmd_;
    bool                recording_ = false;
    std::vector<Buffer> staging_;     // kept alive until flush
};

} // namespace pulse::rhi
