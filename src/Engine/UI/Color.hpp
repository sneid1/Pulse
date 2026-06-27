#pragma once

#include <cstdint>

namespace pulse {

// HUD colors pack as R | G<<8 | B<<16 | A<<24 (matches ui.hlsl unpack).
constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint32_t>(r) | static_cast<uint32_t>(g) << 8 |
           static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(a) << 24;
}
constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return rgba(r, g, b, 255); }

// Linear blend of two packed colors (t in 0..1), per channel including alpha.
constexpr uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
    const float it = 1.0f - t;
    const auto ch = [&](int s) {
        return static_cast<uint8_t>(((a >> s) & 0xFF) * it + ((b >> s) & 0xFF) * t + 0.5f);
    };
    return rgba(ch(0), ch(8), ch(16), ch(24));
}

// Same packed color with a replaced alpha (for fades without restating rgb).
constexpr uint32_t withAlpha(uint32_t c, uint8_t a) {
    return (c & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

} // namespace pulse
