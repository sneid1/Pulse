#include "Engine/RHI/Descriptors.hpp"

namespace pulse::rhi {

void DescriptorHeap::init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                          uint32_t capacity, bool shaderVisible, const wchar_t* name) {
    capacity_ = capacity;
    shaderVisible_ = shaderVisible;
    next_ = 0;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                               : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    PULSE_HR(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)));
    if (name) heap_->SetName(name);

    increment_ = device->GetDescriptorHandleIncrementSize(type);
    cpuStart_ = heap_->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) gpuStart_ = heap_->GetGPUDescriptorHandleForHeapStart();
}

uint32_t DescriptorHeap::allocate() {
    PULSE_CHECK(next_ < capacity_, "descriptor heap exhausted (%u/%u)", next_, capacity_);
    return next_++;
}

} // namespace pulse::rhi
