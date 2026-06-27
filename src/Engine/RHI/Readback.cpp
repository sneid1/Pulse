#include "Engine/RHI/Readback.hpp"
#include "Engine/RHI/Commands.hpp"

namespace pulse::rhi {

Image readbackTexture(Device& device, const Texture& tex, D3D12_RESOURCE_STATES currentState) {
    ID3D12Device* dev = device.d3d();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT   numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = tex.get()->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    Buffer readback = createReadbackBuffer(dev, totalBytes, L"pulse.readback");

    CommandList cmd;
    cmd.init(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, L"pulse.readback.cmd");
    ID3D12GraphicsCommandList* list = cmd.begin();

    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            tex.get(), currentState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopy);
    }

    const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.get(), footprint);
    const CD3DX12_TEXTURE_COPY_LOCATION src(tex.get(), 0);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        const auto back = CD3DX12_RESOURCE_BARRIER::Transition(
            tex.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, currentState);
        list->ResourceBarrier(1, &back);
    }

    cmd.close();
    ID3D12CommandList* lists[] = { list };
    device.graphicsQueue()->ExecuteCommandLists(1, lists);
    device.flushGraphics();

    Image img;
    img.resize(static_cast<int>(tex.width), static_cast<int>(tex.height));

    uint8_t* mapped = nullptr;
    const CD3DX12_RANGE readRange(0, static_cast<SIZE_T>(totalBytes));
    PULSE_HR(readback.get()->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
    const uint32_t rowBytes = tex.width * 4;
    for (uint32_t y = 0; y < tex.height; ++y) {
        const uint8_t* srcRow = mapped + footprint.Footprint.RowPitch * y;
        uint8_t* dstRow = &img.rgba[static_cast<size_t>(y) * rowBytes];
        std::memcpy(dstRow, srcRow, rowBytes);
    }
    const CD3DX12_RANGE noWrite(0, 0);
    readback.get()->Unmap(0, &noWrite);

    return img;
}

} // namespace pulse::rhi
