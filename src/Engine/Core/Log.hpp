#pragma once

// Logging + fail-loud helpers. PROJECT_RULES.md: a missing required engine path,
// shader, or asset must fail loudly with a clear error, never a quiet substitute.

#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <string>

namespace pulse {

inline void logLine(const char* level, const char* fmt, va_list args) {
    std::fprintf(stderr, "[pulse:%s] ", level);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
}

inline void logInfo(const char* fmt, ...) {
    va_list a; va_start(a, fmt); logLine("info", fmt, a); va_end(a);
}
inline void logWarn(const char* fmt, ...) {
    va_list a; va_start(a, fmt); logLine("warn", fmt, a); va_end(a);
}
inline void logError(const char* fmt, ...) {
    va_list a; va_start(a, fmt); logLine("error", fmt, a); va_end(a);
}

[[noreturn]] inline void fail(const std::string& msg) {
    std::fprintf(stderr, "[pulse:fatal] %s\n", msg.c_str());
    throw std::runtime_error(msg);
}

inline std::string formatStr(const char* fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    return std::string(buf);
}

} // namespace pulse

// Fail loudly with a formatted message unless the condition holds.
#define PULSE_CHECK(cond, ...) \
    do { if (!(cond)) ::pulse::fail(::pulse::formatStr("%s:%d: " , __FILE__, __LINE__) + ::pulse::formatStr(__VA_ARGS__)); } while (0)
