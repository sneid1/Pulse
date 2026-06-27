#pragma once

#include "Engine/RHI/D3D12Common.hpp"

#include <string>

namespace pulse::rhi {

// Render tier selected at runtime from Caps. RT swaps in at four seams (shadows,
// AO, GI, reflections) off the same gbuffer; RASTER is a first-class shippable
// fallback. See the plan, Part I "Capability tiers".
enum class Tier { Raster, RT };

enum class DxrTier { None, _1_0, _1_1 };

struct Caps {
    std::wstring         adapterName;
    uint64_t             dedicatedVramBytes = 0;
    D3D_FEATURE_LEVEL    featureLevel       = D3D_FEATURE_LEVEL_11_0;
    D3D_SHADER_MODEL     shaderModel        = D3D_SHADER_MODEL_6_0;
    bool                 waveOps            = false;
    uint32_t             waveLaneMin        = 0;
    uint32_t             waveLaneMax        = 0;
    D3D12_RESOURCE_BINDING_TIER bindingTier = D3D12_RESOURCE_BINDING_TIER_1;
    D3D12_RESOURCE_HEAP_TIER heapTier       = D3D12_RESOURCE_HEAP_TIER_1;
    DxrTier              dxrTier            = DxrTier::None;
    bool                 meshShaders        = false;
    bool                 enhancedBarriers   = false;
    Tier                 tier               = Tier::Raster;

    bool supportsBindless() const { return shaderModel >= D3D_SHADER_MODEL_6_6
                                        && bindingTier >= D3D12_RESOURCE_BINDING_TIER_3; }
    bool isRt() const { return tier == Tier::RT; }
};

struct DeviceConfig {
    bool enableDebugLayer    = false;   // D3D12 debug layer (Agility redist; no admin needed)
    bool enableGpuValidation = false;   // GPU-based validation (slower; debug/CI)
    bool forceRaster         = false;   // ignore DXR tier, select RASTER (test both tiers)
    bool preferWarp          = false;   // software adapter (CI without a GPU)
};

// Thin D3D12 abstraction: device, the three queues (graphics / async compute /
// copy), caps, and fence-based synchronisation. Higher-level RHI objects
// (descriptor heaps, resources, pipelines, command rings) build on this.
class Device {
public:
    Device() = default;     // empty/invalid device; fill via create() + move-assign
    static Device create(const DeviceConfig& cfg);
    ~Device();

    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    ID3D12Device*       d3d() const { return device_.Get(); }
    IDXGIFactory6*      factory() const { return factory_.Get(); }
    const Caps&         caps() const { return caps_; }

    ID3D12CommandQueue* graphicsQueue() const { return graphicsQueue_.Get(); }
    ID3D12CommandQueue* computeQueue()  const { return computeQueue_.Get(); }
    ID3D12CommandQueue* copyQueue()     const { return copyQueue_.Get(); }

    // Submit a fence signal on the graphics queue and block until the GPU
    // reaches it. Coarse, used at shutdown and between standalone operations.
    void flushGraphics();

    // Human-readable multi-line dump of caps (the --gpu-info payload).
    std::string gpuInfoString() const;

    bool valid() const { return device_ != nullptr; }

private:
    void queryCaps();
    void createQueues();

    ComPtr<IDXGIFactory6>      factory_;
    ComPtr<IDXGIAdapter1>      adapter_;
    ComPtr<ID3D12Device>       device_;
    ComPtr<ID3D12CommandQueue> graphicsQueue_;
    ComPtr<ID3D12CommandQueue> computeQueue_;
    ComPtr<ID3D12CommandQueue> copyQueue_;

    ComPtr<ID3D12Fence>        flushFence_;
    HANDLE                     flushEvent_ = nullptr;
    uint64_t                   flushValue_ = 0;

    Caps caps_;
    bool debugLayerActive_ = false;
};

} // namespace pulse::rhi
