#pragma once

#include "Engine/RHI/D3D12Common.hpp"

namespace pulse::rhi {

// A bump-allocated descriptor heap. The shader-visible CBV/SRV/UAV heap is the
// engine's single bindless table (SM6.6 ResourceDescriptorHeap indexes into it);
// RTV/DSV heaps are CPU-only. Bump allocation only, descriptors persist for the
// device lifetime (resources are created once at load), which is all M0 needs.
class DescriptorHeap {
public:
    void init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
              uint32_t capacity, bool shaderVisible, const wchar_t* name);

    // Reserve one descriptor slot; returns its index. Fails loud if exhausted.
    uint32_t allocate();

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t index) const {
        return { cpuStart_.ptr + static_cast<SIZE_T>(index) * increment_ };
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t index) const {
        return { gpuStart_.ptr + static_cast<UINT64>(index) * increment_ };
    }

    ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
    uint32_t increment() const { return increment_; }
    uint32_t allocated() const { return next_; }
    uint32_t capacity() const { return capacity_; }
    bool shaderVisible() const { return shaderVisible_; }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE  cpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE  gpuStart_{};
    uint32_t increment_     = 0;
    uint32_t capacity_      = 0;
    uint32_t next_          = 0;
    bool     shaderVisible_ = false;
};

} // namespace pulse::rhi
