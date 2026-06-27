#pragma once

#include "Engine/RHI/D3D12Common.hpp"
#include <dxcapi.h>

#include <string>
#include <vector>

namespace pulse::rhi {

struct ShaderBlob {
    ComPtr<IDxcBlob> dxil;
    const void* data() const { return dxil ? dxil->GetBufferPointer() : nullptr; }
    size_t      size() const { return dxil ? dxil->GetBufferSize() : 0; }
    D3D12_SHADER_BYTECODE bytecode() const { return { data(), size() }; }
    explicit operator bool() const { return dxil && dxil->GetBufferSize() > 0; }
};

// dxc wrapper. Compiles HLSL (SM6.6 + RT) to DXIL. Dev path: compile-at-load from
// assets/shaders/ for fast iteration. Resolves shader files against a list of
// candidate roots (live source tree first in dev, then ./assets/shaders next to
// the exe for a shipped build). A missing shader fails loud (PROJECT_RULES.md).
class ShaderCompiler {
public:
    void init();

    // Compile <name>.hlsl (resolved against the roots) for the given entry/target,
    // e.g. entry="VSMain", target=L"vs_6_6". defines are "NAME=VALUE" or "NAME".
    ShaderBlob compile(std::string_view name, const wchar_t* entry, const wchar_t* target,
                       const std::vector<std::wstring>& defines = {});

    // Resolve a shader file to an absolute path, or fail loud.
    std::wstring resolve(std::string_view name) const;

private:
    ComPtr<IDxcUtils>          utils_;
    ComPtr<IDxcCompiler3>      compiler_;
    ComPtr<IDxcIncludeHandler> includeHandler_;
    std::vector<std::wstring>  roots_;
};

} // namespace pulse::rhi
