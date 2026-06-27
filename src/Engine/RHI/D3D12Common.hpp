#pragma once

// Common D3D12 includes + small helpers shared across the RHI. The Agility SDK
// headers (third_party/agility/include) precede the Windows SDK on the include
// path, so <d3d12.h> here is the Agility version.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dx12/d3dx12.h>    // Agility's d3dx12 helper umbrella (CD3DX12_*)

#include <cstdint>
#include <string>

#include "Engine/Core/Log.hpp"

namespace pulse::rhi {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

inline std::string hrString(HRESULT hr) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

} // namespace pulse::rhi

// Throw with file:line + the failing HRESULT if an expression fails.
#define PULSE_HR(expr)                                                              \
    do {                                                                           \
        HRESULT _hr = (expr);                                                      \
        if (FAILED(_hr)) {                                                         \
            ::pulse::fail(::pulse::formatStr("%s:%d: %s failed hr=%s",             \
                __FILE__, __LINE__, #expr, ::pulse::rhi::hrString(_hr).c_str()));  \
        }                                                                          \
    } while (0)
