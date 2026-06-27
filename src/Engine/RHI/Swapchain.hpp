#pragma once

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Resource.hpp"

#include <vector>

namespace pulse::rhi {

// DXGI flip-model swapchain for the windowed present path. The engine renders the
// final LDR target offscreen, then copies it into the current backbuffer and
// presents, so windowed and headless share the exact same render graph. Headless
// runs never create one (the swapchain is never a dependency of rendering).
class Swapchain {
public:
    void init(Device& device, HWND hwnd, uint32_t width, uint32_t height, uint32_t bufferCount = 2);
    void resize(uint32_t width, uint32_t height);

    Texture& currentBackbuffer();        // tracks its own state for barriers
    void present(bool vsync = true);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    DXGI_FORMAT format() const { return format_; }

private:
    void acquireBuffers();

    Device*  device_ = nullptr;
    HWND     hwnd_ = nullptr;
    ComPtr<IDXGISwapChain3> swapchain_;
    std::vector<Texture> backbuffers_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bufferCount_ = 2;
    DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
};

} // namespace pulse::rhi
