// Day-one toolchain spike (plan Risk #2 mitigation): prove the D3D12 + dxc
// toolchain on the real GPU BEFORE committing to the RT seams. This is the
// --gpu-info probe in embryo; its logic folds into Engine/RHI Device::caps().
//
// Build (from a vcvars64 shell):
//   cl /nologo /std:c++20 /EHsc /W4 /Zc:preprocessor tools/spike/gpu_info_spike.cpp ^
//      /Fe:build/spike/gpu_info_spike.exe /link d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib
//
// It (1) enables the debug layer, (2) enumerates adapters, (3) creates a D3D12
// device on the best hardware adapter, (4) queries feature level, Shader Model,
// wave intrinsics, resource binding tier (bindless), and DXR tier, then
// (5) compiles a trivial SM6.6 shader through dxc to prove the shader toolchain.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        default: return "<11_0";
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
        case D3D_SHADER_MODEL_6_0: return "6.0";
        default: return "5.x";
    }
}

const char* dxrTierName(D3D12_RAYTRACING_TIER tier) {
    switch (tier) {
        case D3D12_RAYTRACING_TIER_1_1: return "1.1";
        case D3D12_RAYTRACING_TIER_1_0: return "1.0";
        case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "none";
        default: return "?";
    }
}

bool checkHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::printf("  [FAIL] %s hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// Compile a trivial SM6.6 compute shader through dxc to DXIL, proving the
// shader toolchain end to end (dxcompiler.dll must be locatable at runtime).
bool proveShaderToolchain() {
    std::printf("\n[dxc] proving shader toolchain (SM6.6 -> DXIL)\n");
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (!checkHr(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcCreateInstance(Utils)")) return false;
    if (!checkHr(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCreateInstance(Compiler)")) return false;

    // Touches bindless (ResourceDescriptorHeap) + a wave intrinsic so the
    // compile exercises the SM6.6 features the engine relies on.
    static const char* kSrc =
        "RWStructuredBuffer<uint> Out : register(u0);\n"
        "[numthreads(64,1,1)]\n"
        "void main(uint3 tid : SV_DispatchThreadID) {\n"
        "    uint sum = WaveActiveSum(tid.x);\n"
        "    if (WaveIsFirstLane()) Out[tid.x] = sum;\n"
        "}\n";

    DxcBuffer src{ kSrc, std::strlen(kSrc), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-T", L"cs_6_6", L"-E", L"main", L"-Qstrip_debug" };

    ComPtr<IDxcResult> result;
    if (!checkHr(compiler->Compile(&src, args, _countof(args), nullptr, IID_PPV_ARGS(&result)), "Compile")) return false;

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        std::printf("  dxc diagnostics: %s\n", errors->GetStringPointer());
    }
    if (FAILED(status)) { std::printf("  [FAIL] shader compile status=0x%08lX\n", (unsigned long)status); return false; }

    ComPtr<IDxcBlob> dxil;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
    if (!dxil || dxil->GetBufferSize() == 0) { std::printf("  [FAIL] no DXIL produced\n"); return false; }
    std::printf("  [OK] DXIL produced: %zu bytes\n", dxil->GetBufferSize());
    return true;
}

} // namespace

int main() {
    std::printf("=== PULSE --gpu-info spike ===\n");

    UINT dxgiFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            std::printf("[debug] D3D12 debug layer enabled\n");
        }
        dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory6> factory;
    if (!checkHr(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) return 1;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    D3D_FEATURE_LEVEL gotFL = D3D_FEATURE_LEVEL_11_0;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    };

    for (UINT i = 0;
         factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name[256];
        std::snprintf(name, sizeof(name), "%ws", desc.Description);
        std::printf("\n[adapter %u] %s  (software=%d, VRAM=%llu MB)\n", i, name,
                    (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0,
                    (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { std::printf("  skip (software)\n"); continue; }

        for (auto fl : levels) {
            HRESULT hr = D3D12CreateDevice(adapter.Get(), fl, IID_PPV_ARGS(&device));
            std::printf("  D3D12CreateDevice(FL %s) hr=0x%08lX %s\n",
                        featureLevelName(fl), (unsigned long)hr, SUCCEEDED(hr) ? "OK" : "");
            if (SUCCEEDED(hr)) { gotFL = fl; break; }
        }
        if (device) break;
    }

    if (!device) {
        std::printf("\n[warn] no hardware device; trying WARP (software) to isolate runtime vs hardware\n");
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            HRESULT hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
            std::printf("  WARP D3D12CreateDevice hr=0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "OK" : "");
            if (SUCCEEDED(hr)) gotFL = D3D_FEATURE_LEVEL_12_0;
        }
    }

    if (!device) { std::printf("[FAIL] no D3D12 device created\n"); return 1; }

    std::printf("\n[caps]\n");
    std::printf("  feature level: %s\n", featureLevelName(gotFL));

    // Shader Model: query downward from the highest we understand.
    D3D_SHADER_MODEL sm = D3D_SHADER_MODEL_6_9;
    for (;;) {
        D3D12_FEATURE_DATA_SHADER_MODEL d{ sm };
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &d, sizeof(d)))) {
            sm = d.HighestShaderModel;
            break;
        }
        if (sm == D3D_SHADER_MODEL_6_0) break;
        sm = static_cast<D3D_SHADER_MODEL>(sm - 1);
    }
    std::printf("  highest shader model: %s%s\n", shaderModelName(sm),
                sm >= D3D_SHADER_MODEL_6_6 ? "  (SM6.6 bindless OK)" : "  (NO SM6.6!)");

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)))) {
        std::printf("  wave ops: %s  (lane width %u..%u)\n",
                    o1.WaveOps ? "yes" : "no", o1.WaveLaneCountMin, o1.WaveLaneCountMax);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o0{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof(o0)))) {
        std::printf("  resource binding tier: %d  (tier3 = full bindless)\n", (int)o0.ResourceBindingTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    D3D12_RAYTRACING_TIER dxr = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) {
        dxr = o5.RaytracingTier;
    }
    std::printf("  DXR tier: %s  -> selected render tier: %s\n",
                dxrTierName(dxr),
                dxr >= D3D12_RAYTRACING_TIER_1_1 ? "RT" : "RASTER");

    const bool shaderOk = proveShaderToolchain();

    std::printf("\n=== spike result: %s ===\n",
                (device && sm >= D3D_SHADER_MODEL_6_6 && shaderOk) ? "PASS" : "CHECK ABOVE");
    return 0;
}
