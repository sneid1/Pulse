#include "Engine/RHI/Uploader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pulse::rhi {

void Uploader::init(Device* device) {
    device_ = device;
    cmd_.init(device->d3d(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.upload.cmd");
}

ID3D12GraphicsCommandList* Uploader::ensureOpen() {
    if (!recording_) { cmd_.begin(); recording_ = true; }
    return cmd_.get();
}

Buffer Uploader::uploadBuffer(const void* data, uint64_t size, D3D12_RESOURCE_STATES finalState,
                              bool allowUav, const wchar_t* name) {
    ID3D12GraphicsCommandList* list = ensureOpen();

    Buffer staging = createUploadBuffer(device_->d3d(), size, L"pulse.staging");
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    Buffer dest = createDefaultBuffer(device_->d3d(), size, D3D12_RESOURCE_STATE_COPY_DEST, allowUav, name);
    list->CopyBufferRegion(dest.get(), 0, staging.get(), 0, size);

    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        dest.get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    list->ResourceBarrier(1, &barrier);

    staging_.push_back(std::move(staging));
    return dest;
}

Texture Uploader::uploadTexture2D(const void* pixels, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                  D3D12_RESOURCE_STATES finalState, const wchar_t* name) {
    ID3D12GraphicsCommandList* list = ensureOpen();

    // Build a full mip chain on the CPU (box filter, RGBA8) to kill minification
    // aliasing on tiled surfaces (the speckle the plan warns against).
    uint32_t mipCount = 1;
    for (uint32_t m = (width > height ? width : height); m > 1; m >>= 1) ++mipCount;

    std::vector<std::vector<uint8_t>> mipData;
    mipData.reserve(mipCount);
    mipData.emplace_back(static_cast<const uint8_t*>(pixels),
                         static_cast<const uint8_t*>(pixels) + static_cast<size_t>(width) * height * 4);
    uint32_t mw = width, mh = height;
    for (uint32_t i = 1; i < mipCount; ++i) {
        const uint32_t pw = mw, ph = mh;
        mw = pw > 1 ? pw / 2 : 1;
        mh = ph > 1 ? ph / 2 : 1;
        const std::vector<uint8_t>& src = mipData.back();
        std::vector<uint8_t> dst(static_cast<size_t>(mw) * mh * 4);
        for (uint32_t y = 0; y < mh; ++y) {
            const uint32_t y0 = y * 2, y1 = (y * 2 + 1 < ph) ? y * 2 + 1 : y * 2;
            for (uint32_t x = 0; x < mw; ++x) {
                const uint32_t x0 = x * 2, x1 = (x * 2 + 1 < pw) ? x * 2 + 1 : x * 2;
                for (int c = 0; c < 4; ++c) {
                    const uint32_t s = src[(y0 * pw + x0) * 4 + c] + src[(y0 * pw + x1) * 4 + c]
                                     + src[(y1 * pw + x0) * 4 + c] + src[(y1 * pw + x1) * 4 + c];
                    dst[(y * mw + x) * 4 + c] = static_cast<uint8_t>(s / 4);
                }
            }
        }
        mipData.push_back(std::move(dst));
    }

    // Create in COMMON (not COPY_DEST): GPU-based validation tracks a resource's
    // layout from its CREATION state at each command-list boundary, and legacy
    // barriers on the upload list don't carry across to the frame list. A COMMON
    // creation state promotes implicitly to the shader-resource layout on read
    // (and to COPY_DEST for the upload here), keeping validation clean.
    Texture tex = createTexture2D(device_->d3d(), width, height, format, TextureUsage::None,
                                  D3D12_RESOURCE_STATE_COMMON, mipCount, nullptr, name);

    const UINT64 uploadSize = GetRequiredIntermediateSize(tex.get(), 0, mipCount);
    Buffer staging = createUploadBuffer(device_->d3d(), uploadSize, L"pulse.staging.tex");

    // Assumes a 4-byte-per-texel format (R8G8B8A8 / B8G8R8A8).
    std::vector<D3D12_SUBRESOURCE_DATA> subs(mipCount);
    mw = width; mh = height;
    for (uint32_t i = 0; i < mipCount; ++i) {
        subs[i].pData = mipData[i].data();
        subs[i].RowPitch = static_cast<LONG_PTR>(mw) * 4;
        subs[i].SlicePitch = subs[i].RowPitch * mh;
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
    }

    const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopy);

    UpdateSubresources(list, tex.get(), staging.get(), 0, 0, mipCount, subs.data());

    // Back to COMMON so the read-side implicit promotion applies (finalState is
    // advisory now; static textures live in COMMON and promote on use).
    const auto toCommon = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &toCommon);
    tex.state = D3D12_RESOURCE_STATE_COMMON;
    (void)finalState;

    staging_.push_back(std::move(staging));
    return tex;
}

Texture Uploader::uploadTexture2DNoMips(const void* pixels, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                        D3D12_RESOURCE_STATES finalState, const wchar_t* name) {
    ID3D12GraphicsCommandList* list = ensureOpen();

    Texture tex = createTexture2D(device_->d3d(), width, height, format, TextureUsage::None,
                                  D3D12_RESOURCE_STATE_COMMON, 1, nullptr, name);

    const UINT64 uploadSize = GetRequiredIntermediateSize(tex.get(), 0, 1);
    Buffer staging = createUploadBuffer(device_->d3d(), uploadSize, L"pulse.staging.tex.nomips");

    D3D12_SUBRESOURCE_DATA sub{};
    sub.pData = pixels;
    sub.RowPitch = static_cast<LONG_PTR>(width) * 4;
    sub.SlicePitch = sub.RowPitch * height;

    const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopy);

    UpdateSubresources(list, tex.get(), staging.get(), 0, 0, 1, &sub);

    const auto toCommon = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &toCommon);
    tex.state = D3D12_RESOURCE_STATE_COMMON;
    (void)finalState;

    staging_.push_back(std::move(staging));
    return tex;
}

Texture Uploader::uploadDDS(const std::string& path, const wchar_t* name) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return Texture{};
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (sz <= 0 || std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) { std::fclose(f); return Texture{}; }
    std::fclose(f);

    if (bytes.size() < 4 + 124) return Texture{};
    uint32_t magic; std::memcpy(&magic, bytes.data(), 4);
    if (magic != 0x20534444u) return Texture{};            // "DDS "
    const uint8_t* hdr = bytes.data() + 4;                 // DDS_HEADER (124 bytes)
    auto rd = [&](size_t off) { uint32_t v; std::memcpy(&v, hdr + off, 4); return v; };
    const uint32_t height = rd(8), width = rd(12);
    uint32_t mipCount = rd(24); if (mipCount == 0) mipCount = 1;
    const uint32_t pfFlags = rd(76), fourCC = rd(80);

    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    size_t dataOff = 4 + 124;
    if ((pfFlags & 0x4u) && fourCC == 0x30315844u) {       // DDPF_FOURCC + "DX10"
        uint32_t dxgi; std::memcpy(&dxgi, bytes.data() + 4 + 124, 4);   // DDS_HEADER_DXT10.dxgiFormat
        fmt = static_cast<DXGI_FORMAT>(dxgi);
        dataOff = 4 + 124 + 20;
    } else if (pfFlags & 0x4u) {                            // legacy DXT1/3/5
        if (fourCC == 0x31545844u)      fmt = DXGI_FORMAT_BC1_UNORM;
        else if (fourCC == 0x33545844u) fmt = DXGI_FORMAT_BC2_UNORM;
        else if (fourCC == 0x35545844u) fmt = DXGI_FORMAT_BC3_UNORM;
        else return Texture{};
    } else {
        return Texture{};
    }

    const bool isBC = (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC5_SNORM) ||
                      (fmt >= DXGI_FORMAT_BC6H_TYPELESS && fmt <= DXGI_FORMAT_BC7_UNORM_SRGB);
    const bool block8 = (fmt >= DXGI_FORMAT_BC1_TYPELESS && fmt <= DXGI_FORMAT_BC1_UNORM_SRGB) ||
                        (fmt >= DXGI_FORMAT_BC4_TYPELESS && fmt <= DXGI_FORMAT_BC4_SNORM);
    const uint32_t blockBytes = block8 ? 8u : 16u;

    Texture tex = createTexture2D(device_->d3d(), width, height, fmt, TextureUsage::None,
                                  D3D12_RESOURCE_STATE_COMMON, mipCount, nullptr, name);

    std::vector<D3D12_SUBRESOURCE_DATA> subs(mipCount);
    size_t off = dataOff;
    uint32_t mw = width, mh = height;
    for (uint32_t i = 0; i < mipCount; ++i) {
        uint32_t rowPitch, slicePitch;
        if (isBC) {
            const uint32_t bw = (mw + 3) / 4 > 0 ? (mw + 3) / 4 : 1;
            const uint32_t bh = (mh + 3) / 4 > 0 ? (mh + 3) / 4 : 1;
            rowPitch = bw * blockBytes; slicePitch = rowPitch * bh;
        } else {
            rowPitch = mw * 4; slicePitch = rowPitch * mh;   // uncompressed RGBA8 fallback
        }
        if (off + slicePitch > bytes.size()) return Texture{};
        subs[i].pData = bytes.data() + off;
        subs[i].RowPitch = static_cast<LONG_PTR>(rowPitch);
        subs[i].SlicePitch = static_cast<LONG_PTR>(slicePitch);
        off += slicePitch;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
    }

    // UpdateSubresources copies the CPU data into the staging buffer synchronously,
    // so `bytes` may be freed on return; the GPU copy happens at flush().
    ID3D12GraphicsCommandList* list = ensureOpen();
    const UINT64 uploadSize = GetRequiredIntermediateSize(tex.get(), 0, mipCount);
    Buffer staging = createUploadBuffer(device_->d3d(), uploadSize, L"pulse.staging.dds");
    const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopy);
    UpdateSubresources(list, tex.get(), staging.get(), 0, 0, mipCount, subs.data());
    const auto toCommon = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &toCommon);
    tex.state = D3D12_RESOURCE_STATE_COMMON;

    staging_.push_back(std::move(staging));
    return tex;
}

void Uploader::flush() {
    if (!recording_) return;
    cmd_.close();
    recording_ = false;
    ID3D12CommandList* lists[] = { cmd_.get() };
    device_->graphicsQueue()->ExecuteCommandLists(1, lists);
    device_->flushGraphics();
    staging_.clear();
}

} // namespace pulse::rhi
