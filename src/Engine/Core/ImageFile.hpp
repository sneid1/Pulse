#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

// Decode an image file (PNG / JPG / TGA / ...) to 8-bit RGBA (4 channels).
// Returns an empty vector on failure (outW/outH set to 0). Used to load the
// sourced PBR map sets (PolyHaven diff / arm / nor_gl PNGs) at load time.
std::vector<uint8_t> loadImageRGBA(const std::string& path, uint32_t& outW, uint32_t& outH);

// Decode image bytes embedded in GLB files. stb_image detects PNG/JPG/TGA/etc from memory.
std::vector<uint8_t> loadImageRGBAFromMemory(const uint8_t* bytes, size_t byteCount,
                                             uint32_t& outW, uint32_t& outH);

} // namespace pulse
