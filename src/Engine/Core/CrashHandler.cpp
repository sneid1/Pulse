#include "Engine/Core/CrashHandler.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdint>
#include <string>

namespace pulse {
namespace {

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case 0xE06D7363u:                     return "C++ exception (uncaught)";
        default:                              return "UNKNOWN";
    }
}

// Directory of the running exe, so the log/dump land next to pulse.exe.
std::wstring exeDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"pulse.exe";
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

void describeModule(void* addr, char* out, size_t outSize) {
    std::snprintf(out, outSize, "module: <unknown>");
    if (!addr) return;
    HMODULE hmod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &hmod) && hmod) {
        wchar_t mod[MAX_PATH] = {};
        GetModuleFileNameW(hmod, mod, MAX_PATH);
        const uintptr_t base = reinterpret_cast<uintptr_t>(hmod);
        const uintptr_t off = reinterpret_cast<uintptr_t>(addr) - base;
        std::snprintf(out, outSize, "module: %ls  base=0x%p  offset=0x%llx (open the .dmp, or map this offset via the .map/.pdb)",
                      mod, reinterpret_cast<void*>(base), static_cast<unsigned long long>(off));
    }
}

void writeMinidump(const std::wstring& path, EXCEPTION_POINTERS* ep) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    // Enough context to get a real stack + locals in VS/WinDbg without a huge file.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type,
                      ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(h);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    const DWORD code = rec ? rec->ExceptionCode : 0;
    void* addr = rec ? rec->ExceptionAddress : nullptr;

    char modLine[600];
    describeModule(addr, modLine, sizeof(modLine));

    char accessLine[160] = "";
    if (code == EXCEPTION_ACCESS_VIOLATION && rec && rec->NumberParameters >= 2) {
        const ULONG_PTR rw = rec->ExceptionInformation[0];
        const ULONG_PTR at = rec->ExceptionInformation[1];
        std::snprintf(accessLine, sizeof(accessLine), "  access: %s 0x%llx",
                      rw == 1 ? "WRITE to" : (rw == 8 ? "EXECUTE at" : "READ from"),
                      static_cast<unsigned long long>(at));
    }

    const std::wstring dir = exeDir();
    const std::wstring logPath = dir + L"\\pulse_crash.log";
    const std::wstring dmpPath = dir + L"\\pulse_crash.dmp";

    if (FILE* f = _wfopen(logPath.c_str(), L"a")) {
        std::fprintf(f, "==== PULSE CRASH (thread %lu) ====\n", GetCurrentThreadId());
        std::fprintf(f, "exception 0x%08lX (%s) at 0x%p%s\n", code, exceptionName(code), addr, accessLine);
        std::fprintf(f, "%s\n", modLine);
        std::fprintf(f, "minidump: %ls\n\n", dmpPath.c_str());
        std::fclose(f);
    }
    std::fprintf(stderr, "\n==== PULSE CRASH (thread %lu) ====\n", GetCurrentThreadId());
    std::fprintf(stderr, "exception 0x%08lX (%s) at 0x%p%s\n", code, exceptionName(code), addr, accessLine);
    std::fprintf(stderr, "%s\n", modLine);

    writeMinidump(dmpPath, ep);
    std::fprintf(stderr, "wrote %ls and pulse_crash.log\n", dmpPath.c_str());
    std::fflush(stderr);

    return EXCEPTION_EXECUTE_HANDLER; // we have logged + dumped; let the process die
}

} // namespace

void installCrashHandler() {
    SetUnhandledExceptionFilter(crashFilter);
    // Suppress the OS "stopped working" dialog so a crash exits promptly after we
    // have written the dump (the dialog otherwise blocks an unattended/headless run).
    SetErrorMode(SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
}

} // namespace pulse
