#include "Engine/Core/ImageFile.hpp"

// stb_image is public-domain; quiet the /W4 noise from its implementation.
#pragma warning(push)
#pragma warning(disable : 4244 4242 4456 4457 4459 5219 4996 4365 4189)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma warning(pop)

namespace pulse {

std::vector<uint8_t> loadImageRGBA(const std::string& path, uint32_t& outW, uint32_t& outH) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 4);   // force RGBA
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        outW = outH = 0;
        return {};
    }
    std::vector<uint8_t> rgba(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    outW = static_cast<uint32_t>(w);
    outH = static_cast<uint32_t>(h);
    return rgba;
}

std::vector<uint8_t> loadImageRGBAFromMemory(const uint8_t* bytes, size_t byteCount,
                                             uint32_t& outW, uint32_t& outH) {
    if (!bytes || byteCount == 0) {
        outW = outH = 0;
        return {};
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* data = stbi_load_from_memory(bytes, static_cast<int>(byteCount), &w, &h, &channels, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        outW = outH = 0;
        return {};
    }
    std::vector<uint8_t> rgba(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    outW = static_cast<uint32_t>(w);
    outH = static_cast<uint32_t>(h);
    return rgba;
}

} // namespace pulse
