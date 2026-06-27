#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <vector>

namespace pulse {

// Make the process CWD the executable's own folder. A shipped build resolves
// assets (shaders, textures, audio) relative to the exe, so it runs from anywhere
//, double-clicked or launched from another directory.
inline void setWorkingDirectoryToExecutableFolder() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return;
    std::error_code ec;
    std::filesystem::current_path(std::filesystem::path(buf.data()).parent_path(), ec);
}

} // namespace pulse
