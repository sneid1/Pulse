#include "Engine/RHI/Commands.hpp"

namespace pulse::rhi {

void CommandList::init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, const wchar_t* name) {
    type_ = type;
    PULSE_HR(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator_)));
    PULSE_HR(device->CreateCommandList(0, type, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_)));
    PULSE_HR(list_->Close());     // created open; close so begin() can reset cleanly
    open_ = false;
    if (name) list_->SetName(name);
}

ID3D12GraphicsCommandList* CommandList::begin() {
    PULSE_CHECK(!open_, "CommandList::begin called while already open");
    PULSE_HR(allocator_->Reset());
    PULSE_HR(list_->Reset(allocator_.Get(), nullptr));
    open_ = true;
    return list_.Get();
}

void CommandList::close() {
    PULSE_CHECK(open_, "CommandList::close called while not open");
    PULSE_HR(list_->Close());
    open_ = false;
}

} // namespace pulse::rhi
