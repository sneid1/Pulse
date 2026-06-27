#include "Engine/Shaders/ShaderCompiler.hpp"

#include <filesystem>

namespace pulse::rhi {

namespace fs = std::filesystem;

namespace {
std::wstring widen(std::string_view s) {
    return std::wstring(s.begin(), s.end());
}
}

void ShaderCompiler::init() {
    PULSE_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils_)));
    PULSE_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)));
    PULSE_HR(utils_->CreateDefaultIncludeHandler(&includeHandler_));

    // Candidate roots, in priority order. Live source tree first (dev hot-edit),
    // then assets/shaders next to the exe (shipped).
    roots_.clear();
#ifdef PULSE_SOURCE_SHADER_DIR
    {
        const std::wstring src = widen(PULSE_SOURCE_SHADER_DIR);
        if (fs::exists(src)) roots_.push_back(src);
    }
#endif
    roots_.push_back(L"assets/shaders");
}

std::wstring ShaderCompiler::resolve(std::string_view name) const {
    const fs::path rel = fs::path(name);
    for (const auto& root : roots_) {
        fs::path candidate = fs::path(root) / rel;
        if (fs::exists(candidate)) return candidate.wstring();
    }
    std::string tried;
    for (const auto& r : roots_) { tried += "\n    "; for (wchar_t c : r) tried += static_cast<char>(c); }
    fail(formatStr("shader not found: %.*s (searched roots:%s)",
                   static_cast<int>(name.size()), name.data(), tried.c_str()));
}

ShaderBlob ShaderCompiler::compile(std::string_view name, const wchar_t* entry,
                                   const wchar_t* target, const std::vector<std::wstring>& defines) {
    const std::wstring path = resolve(name);

    ComPtr<IDxcBlobEncoding> source;
    PULSE_HR(utils_->LoadFile(path.c_str(), nullptr, &source));
    DxcBuffer srcBuffer{ source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };

    std::vector<const wchar_t*> args;
    args.push_back(path.c_str());                       // for diagnostics + includes
    args.push_back(L"-E"); args.push_back(entry);
    args.push_back(L"-T"); args.push_back(target);
    args.push_back(L"-HV"); args.push_back(L"2021");    // HLSL 2021
    args.push_back(L"-Zi");                              // debug info (PIX-friendly)
    args.push_back(L"-Qembed_debug");
    args.push_back(L"-enable-16bit-types");
    // Row-major matrices so engine Mat4 (row-major) maps without transpose.
    args.push_back(L"-Zpr");
    std::vector<std::wstring> defStorage;
    defStorage.reserve(defines.size());
    for (const auto& d : defines) { defStorage.push_back(d); }
    for (const auto& d : defStorage) { args.push_back(L"-D"); args.push_back(d.c_str()); }

    ComPtr<IDxcResult> result;
    PULSE_HR(compiler_->Compile(&srcBuffer, args.data(), static_cast<UINT32>(args.size()),
                                includeHandler_.Get(), IID_PPV_ARGS(&result)));

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        logWarn("dxc [%.*s]: %s", static_cast<int>(name.size()), name.data(), errors->GetStringPointer());
    }

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) {
        fail(formatStr("shader compile failed: %.*s entry=%ws target=%ws",
                       static_cast<int>(name.size()), name.data(), entry, target));
    }

    ShaderBlob blob;
    PULSE_HR(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob.dxil), nullptr));
    PULSE_CHECK(blob, "shader produced no DXIL: %.*s", static_cast<int>(name.size()), name.data());
    return blob;
}

} // namespace pulse::rhi
