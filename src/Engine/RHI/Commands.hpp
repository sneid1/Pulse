#pragma once

#include "Engine/RHI/D3D12Common.hpp"

namespace pulse::rhi {

// A command allocator + graphics command list pair. begin() resets both and
// returns the open list; close() finalises it for submission. One-shot use for
// headless capture; the windowed path keeps a ring of these (one per frame).
class CommandList {
public:
    void init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT,
              const wchar_t* name = nullptr);

    ID3D12GraphicsCommandList* begin();   // reset allocator + list, returns the open list
    void close();                          // close for submission

    ID3D12GraphicsCommandList* get() const { return list_.Get(); }
    ID3D12GraphicsCommandList* operator->() const { return list_.Get(); }

private:
    ComPtr<ID3D12CommandAllocator>    allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    D3D12_COMMAND_LIST_TYPE           type_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool open_ = false;
};

} // namespace pulse::rhi
