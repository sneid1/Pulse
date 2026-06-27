#include "Engine/RHI/Swapchain.hpp"

namespace pulse::rhi {

void Swapchain::init(Device& device, HWND hwnd, uint32_t width, uint32_t height, uint32_t bufferCount) {
    device_ = &device;
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;
    bufferCount_ = bufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format_;
    desc.SampleDesc = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    ComPtr<IDXGISwapChain1> sc1;
    PULSE_HR(device.factory()->CreateSwapChainForHwnd(
        device.graphicsQueue(), hwnd, &desc, nullptr, nullptr, &sc1));
    // Disable Alt+Enter fullscreen toggling (we manage size ourselves).
    device.factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    PULSE_HR(sc1.As(&swapchain_));

    acquireBuffers();
}

void Swapchain::acquireBuffers() {
    backbuffers_.clear();
    backbuffers_.resize(bufferCount_);
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        Texture& t = backbuffers_[i];
        PULSE_HR(swapchain_->GetBuffer(i, IID_PPV_ARGS(&t.resource)));
        t.width = width_; t.height = height_; t.mips = 1; t.format = format_;
        t.state = D3D12_RESOURCE_STATE_COMMON;     // becomes PRESENT after first present
        wchar_t name[32]; swprintf_s(name, L"pulse.backbuffer%u", i);
        t.resource->SetName(name);
    }
}

void Swapchain::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    device_->flushGraphics();
    backbuffers_.clear();      // release references before ResizeBuffers
    width_ = width; height_ = height;
    PULSE_HR(swapchain_->ResizeBuffers(bufferCount_, width, height, format_, 0));
    acquireBuffers();
}

Texture& Swapchain::currentBackbuffer() {
    return backbuffers_[swapchain_->GetCurrentBackBufferIndex()];
}

void Swapchain::present(bool vsync) {
    PULSE_HR(swapchain_->Present(vsync ? 1 : 0, 0));
}

} // namespace pulse::rhi
