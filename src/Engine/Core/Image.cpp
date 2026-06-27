#include "Engine/Core/Image.hpp"

#include <cstdio>

namespace pulse {

bool Image::saveBmp(const std::string& path) const {
    if (empty()) return false;

    const int w = width;
    const int h = height;
    const int rowBytes = w * 3;
    const int rowPadded = (rowBytes + 3) & ~3;          // BMP rows are 4-byte aligned
    const uint32_t imageBytes = static_cast<uint32_t>(rowPadded) * h;
    const uint32_t fileSize = 14 + 40 + imageBytes;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    auto u16 = [&](uint16_t v) { std::fputc(v & 0xFF, f); std::fputc((v >> 8) & 0xFF, f); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (i * 8)) & 0xFF, f); };

    // BITMAPFILEHEADER
    std::fputc('B', f); std::fputc('M', f);
    u32(fileSize); u16(0); u16(0); u32(14 + 40);
    // BITMAPINFOHEADER
    u32(40); u32(static_cast<uint32_t>(w)); u32(static_cast<uint32_t>(h));
    u16(1); u16(24); u32(0); u32(imageBytes);
    u32(2835); u32(2835); u32(0); u32(0);

    std::vector<uint8_t> row(static_cast<size_t>(rowPadded), 0);
    for (int y = h - 1; y >= 0; --y) {                  // bottom-up
        const uint8_t* src = &rgba[static_cast<size_t>(y) * w * 4];
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];            // B
            row[x * 3 + 1] = src[x * 4 + 1];            // G
            row[x * 3 + 2] = src[x * 4 + 0];            // R
        }
        std::fwrite(row.data(), 1, static_cast<size_t>(rowPadded), f);
    }

    std::fclose(f);
    return true;
}

} // namespace pulse
