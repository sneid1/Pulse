#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

// CPU-side LDR image, R8G8B8A8 (one byte each, R,G,B,A order), matches the
// engine's R8G8B8A8_UNORM readback texel layout, so a GPU readback copies in
// directly. Headless capture writes these to BMP for the vision loop.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4

    void resize(int w, int h) {
        width = w; height = h;
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    }
    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }

    // 24-bit bottom-up BMP (universally readable; the verify loop does BMP->PNG).
    bool saveBmp(const std::string& path) const;
};

} // namespace pulse
