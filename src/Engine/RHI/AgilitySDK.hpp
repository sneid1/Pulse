#pragma once

// DirectX Agility SDK entry points. The OS d3d12.dll looks for these EXPORTED
// symbols in the running executable; when present it loads our vendored
// D3D12Core.dll (+ d3d12SDKLayers.dll) from .\D3D12\ instead of the system
// runtime. This gives current D3D12 features and, crucially, a redistributable
// debug layer that works WITHOUT the OS "Graphics Tools" optional feature
// (which needs admin and, when absent, makes D3D12CreateDevice fail E_INVALIDARG
// the moment EnableDebugLayer() is called). See third_party/README.md.
//
// Include this header in exactly ONE translation unit per executable (the one
// that defines PULSE_AGILITY_SDK_IMPL before including it, e.g. main.cpp).

#ifdef PULSE_AGILITY_SDK_IMPL

#ifndef PULSE_AGILITY_SDK_VERSION
#error "PULSE_AGILITY_SDK_VERSION must be defined (set by CMake to match third_party/agility)"
#endif

extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = PULSE_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

#endif // PULSE_AGILITY_SDK_IMPL
