#include "Engine/RHI/Device.hpp"

#include <sstream>

namespace pulse::rhi {

namespace {

const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        default: return "<12_0";
    }
}

const char* shaderModelName(D3D_SHADER_MODEL sm) {
    switch (sm) {
        case D3D_SHADER_MODEL_6_9: return "6.9";
        case D3D_SHADER_MODEL_6_8: return "6.8";
        case D3D_SHADER_MODEL_6_7: return "6.7";
        case D3D_SHADER_MODEL_6_6: return "6.6";
        case D3D_SHADER_MODEL_6_5: return "6.5";
        case D3D_SHADER_MODEL_6_4: return "6.4";
        case D3D_SHADER_MODEL_6_3: return "6.3";
        case D3D_SHADER_MODEL_6_2: return "6.2";
        case D3D_SHADER_MODEL_6_1: return "6.1";
        default:                   return "6.0";
    }
}

const char* dxrTierName(DxrTier t) {
    switch (t) {
        case DxrTier::_1_1: return "1.1";
        case DxrTier::_1_0: return "1.0";
        default:            return "none";
    }
}

// Route D3D12 debug-layer / GPU-validation messages to stderr so they are
// visible in a headless run (they otherwise go only to the debugger output).
void CALLBACK d3dMessageCallback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev,
                                 D3D12_MESSAGE_ID, LPCSTR desc, void*) {
    switch (sev) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:   logError("[d3d12] %s", desc); break;
        case D3D12_MESSAGE_SEVERITY_WARNING: logWarn("[d3d12] %s", desc); break;
        default:                             break;   // info/message: too noisy
    }
}

// Try to bring up the debug layer (optionally GPU-based validation). Returns
// true if EnableDebugLayer succeeded. With the Agility redist this works without
// the OS "Graphics Tools" feature; we still verify device creation downstream
// and retry without it if needed (see Device::create).
bool tryEnableDebugLayer(bool gpuValidation) {
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        logWarn("D3D12 debug layer unavailable (no debug interface)");
        return false;
    }
    debug->EnableDebugLayer();
    if (gpuValidation) {
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) {
            debug1->SetEnableGPUBasedValidation(TRUE);
            logInfo("D3D12 GPU-based validation enabled");
        }
    }
    return true;
}

// Create a device on the best hardware adapter (or WARP). Returns the device +
// the adapter it came up on, or null on failure.
ComPtr<ID3D12Device> createDeviceOnBestAdapter(IDXGIFactory6* factory, bool preferWarp,
                                               ComPtr<IDXGIAdapter1>& outAdapter,
                                               D3D_FEATURE_LEVEL& outFL) {
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    };

    auto tryAdapter = [&](IDXGIAdapter1* a) -> ComPtr<ID3D12Device> {
        for (auto fl : levels) {
            ComPtr<ID3D12Device> dev;
            if (SUCCEEDED(D3D12CreateDevice(a, fl, IID_PPV_ARGS(&dev)))) {
                outFL = fl;
                return dev;
            }
        }
        return nullptr;
    };

    if (preferWarp) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            if (auto dev = tryAdapter(warp.Get())) { outAdapter = warp; return dev; }
        }
        return nullptr;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (auto dev = tryAdapter(adapter.Get())) { outAdapter = adapter; return dev; }
    }
    return nullptr;
}

// On a GPU device-removal (a common silent-crash cause), dump the DRED data:
// the auto-breadcrumbs (how far each command list got) and the page-fault VA.
void logDeviceRemoved(ID3D12Device* device) {
    if (!device) return;
    const HRESULT reason = device->GetDeviceRemovedReason();
    logError("D3D12 DEVICE REMOVED: GetDeviceRemovedReason = 0x%08lX", static_cast<unsigned long>(reason));

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        logError("  (DRED unavailable; rebuild with the crash handler active to capture breadcrumbs)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        int n = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode; node && n < 8; node = node->pNext, ++n) {
            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            logError("  breadcrumb cmdlist '%ls' op %u/%u completed (the failing GPU op is right after)",
                     node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"<unnamed>",
                     done, node->BreadcrumbCount);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA) {
        logError("  GPU page fault VA: 0x%llx", static_cast<unsigned long long>(pf.PageFaultVA));
    }
}

} // namespace

Device Device::create(const DeviceConfig& cfg) {
    Device d;

    // Enable DRED (Device Removed Extended Data) before device creation so a GPU
    // fault leaves breadcrumbs + a page-fault VA. Cheap, works without the debug
    // layer; logDeviceRemoved() reads it back if the device is ever lost.
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }

    UINT dxgiFlags = 0;
    if (cfg.enableDebugLayer) {
        if (tryEnableDebugLayer(cfg.enableGpuValidation)) {
            d.debugLayerActive_ = true;
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    PULSE_HR(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&d.factory_)));

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_12_0;
    d.device_ = createDeviceOnBestAdapter(d.factory_.Get(), cfg.preferWarp, d.adapter_, fl);

    // If the debug layer was on and device creation failed (e.g. a layer/runtime
    // mismatch), retry once without it and warn loudly rather than dying silently.
    if (!d.device_ && d.debugLayerActive_) {
        logWarn("device creation failed with the debug layer active; retrying WITHOUT it. "
                "GPU validation is OFF, provision the Agility SDK redist (D3D12/d3d12SDKLayers.dll) to enable it.");
        d.debugLayerActive_ = false;
        d.factory_.Reset();
        PULSE_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory_)));
        d.device_ = createDeviceOnBestAdapter(d.factory_.Get(), cfg.preferWarp, d.adapter_, fl);
    }

    PULSE_CHECK(d.device_ != nullptr,
                "no D3D12 device could be created (no DXR-capable adapter and no fallback)");

    d.caps_.featureLevel = fl;
    if (d.adapter_) {
        DXGI_ADAPTER_DESC1 desc{};
        d.adapter_->GetDesc1(&desc);
        d.caps_.adapterName = desc.Description;
        d.caps_.dedicatedVramBytes = desc.DedicatedVideoMemory;
    }

    if (d.debugLayerActive_) {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(d.device_.As(&infoQueue))) {
            DWORD cookie = 0;
            infoQueue->RegisterMessageCallback(d3dMessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
    }

    d.queryCaps();
    if (cfg.forceRaster) d.caps_.tier = Tier::Raster;

    d.createQueues();

    PULSE_HR(d.device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.flushFence_)));
    d.flushEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PULSE_CHECK(d.flushEvent_ != nullptr, "CreateEvent for flush fence failed");

    logInfo("D3D12 device up: %ws (FL %s, SM %s, DXR %s, tier %s%s)",
            d.caps_.adapterName.c_str(), featureLevelName(d.caps_.featureLevel),
            shaderModelName(d.caps_.shaderModel), dxrTierName(d.caps_.dxrTier),
            d.caps_.tier == Tier::RT ? "RT" : "RASTER",
            d.debugLayerActive_ ? ", debug-layer ON" : "");
    return d;
}

void Device::queryCaps() {
    // Shader model: query downward from the highest we name.
    D3D_SHADER_MODEL sm = D3D_SHADER_MODEL_6_9;
    for (;;) {
        D3D12_FEATURE_DATA_SHADER_MODEL data{ sm };
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
            sm = data.HighestShaderModel;
            break;
        }
        if (sm == D3D_SHADER_MODEL_6_0) break;
        sm = static_cast<D3D_SHADER_MODEL>(sm - 1);
    }
    caps_.shaderModel = sm;

    D3D12_FEATURE_DATA_D3D12_OPTIONS o0{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof(o0)))) {
        caps_.bindingTier = o0.ResourceBindingTier;
        caps_.heapTier = o0.ResourceHeapTier;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)))) {
        caps_.waveOps     = o1.WaveOps;
        caps_.waveLaneMin = o1.WaveLaneCountMin;
        caps_.waveLaneMax = o1.WaveLaneCountMax;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) {
        switch (o5.RaytracingTier) {
            case D3D12_RAYTRACING_TIER_1_1: caps_.dxrTier = DxrTier::_1_1; break;
            case D3D12_RAYTRACING_TIER_1_0: caps_.dxrTier = DxrTier::_1_0; break;
            default:                        caps_.dxrTier = DxrTier::None; break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)))) {
        caps_.meshShaders = (o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12)))) {
        caps_.enhancedBarriers = (o12.EnhancedBarriersSupported != FALSE);
    }

    // Tier selection: RT requires DXR 1.1 AND SM6.6 bindless (the RT seams use
    // ResourceDescriptorHeap). Otherwise the shippable RASTER tier.
    caps_.tier = (caps_.dxrTier == DxrTier::_1_1 && caps_.supportsBindless())
                     ? Tier::RT : Tier::Raster;
}

void Device::createQueues() {
    auto makeQueue = [&](D3D12_COMMAND_LIST_TYPE type, const wchar_t* name) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        ComPtr<ID3D12CommandQueue> q;
        PULSE_HR(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&q)));
        q->SetName(name);
        return q;
    };
    graphicsQueue_ = makeQueue(D3D12_COMMAND_LIST_TYPE_DIRECT,  L"pulse.graphics");
    computeQueue_  = makeQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, L"pulse.compute");
    copyQueue_     = makeQueue(D3D12_COMMAND_LIST_TYPE_COPY,    L"pulse.copy");
}

void Device::flushGraphics() {
    if (!graphicsQueue_ || !flushFence_) return;
    // If the GPU faulted, surface the DRED breadcrumbs once before the failing
    // Signal/Wait below throws (turns a silent device-removal into a diagnosis).
    if (device_ && device_->GetDeviceRemovedReason() != S_OK) {
        static bool reported = false;
        if (!reported) { reported = true; logDeviceRemoved(device_.Get()); }
    }
    const uint64_t target = ++flushValue_;
    PULSE_HR(graphicsQueue_->Signal(flushFence_.Get(), target));
    if (flushFence_->GetCompletedValue() < target) {
        PULSE_HR(flushFence_->SetEventOnCompletion(target, flushEvent_));
        WaitForSingleObject(flushEvent_, INFINITE);
    }
}

std::string Device::gpuInfoString() const {
    std::ostringstream os;
    os << "=== PULSE --gpu-info ===\n";
    os << "  adapter:        ";
    for (wchar_t c : caps_.adapterName) os << static_cast<char>(c);
    os << "\n";
    os << "  dedicated VRAM: " << (caps_.dedicatedVramBytes / (1024 * 1024)) << " MB\n";
    os << "  feature level:  " << featureLevelName(caps_.featureLevel) << "\n";
    os << "  shader model:   " << shaderModelName(caps_.shaderModel)
       << (caps_.shaderModel >= D3D_SHADER_MODEL_6_6 ? "  (SM6.6 bindless)" : "  (NO SM6.6)") << "\n";
    os << "  wave ops:       " << (caps_.waveOps ? "yes" : "no")
       << "  lanes " << caps_.waveLaneMin << ".." << caps_.waveLaneMax << "\n";
    os << "  binding tier:   " << static_cast<int>(caps_.bindingTier)
       << (caps_.bindingTier >= D3D12_RESOURCE_BINDING_TIER_3 ? "  (full bindless)" : "") << "\n";
    os << "  mesh shaders:   " << (caps_.meshShaders ? "yes" : "no") << "\n";
    os << "  enhanced barriers: " << (caps_.enhancedBarriers ? "yes" : "no") << "\n";
    os << "  DXR tier:       " << dxrTierName(caps_.dxrTier) << "\n";
    os << "  >> render tier: " << (caps_.tier == Tier::RT ? "RT" : "RASTER") << "\n";
    return os.str();
}

Device::~Device() {
    if (device_) flushGraphics();
    if (flushEvent_) { CloseHandle(flushEvent_); flushEvent_ = nullptr; }
}

Device::Device(Device&& o) noexcept { *this = std::move(o); }

Device& Device::operator=(Device&& o) noexcept {
    if (this != &o) {
        factory_ = std::move(o.factory_);
        adapter_ = std::move(o.adapter_);
        device_ = std::move(o.device_);
        graphicsQueue_ = std::move(o.graphicsQueue_);
        computeQueue_ = std::move(o.computeQueue_);
        copyQueue_ = std::move(o.copyQueue_);
        flushFence_ = std::move(o.flushFence_);
        flushEvent_ = o.flushEvent_; o.flushEvent_ = nullptr;
        flushValue_ = o.flushValue_;
        caps_ = o.caps_;
        debugLayerActive_ = o.debugLayerActive_;
    }
    return *this;
}

} // namespace pulse::rhi
