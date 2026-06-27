#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>

namespace pulse {

// Minimal Win32 window + message pump. Tracks key/mouse state for the game; the
// engine's windowed present path renders into it. Headless runs never create one.
class Window {
public:
    // Presentation style. Borderless covers the whole monitor with no caption/border
    // (a "windowed fullscreen" / borderless-fullscreen present, not exclusive DXGI
    // fullscreen, so no mode-switch and Alt-Tab stays instant). Windowed is the classic
    // captioned, resizable window.
    enum class DisplayMode { Windowed, BorderlessFullscreen };

    void create(const wchar_t* title, int width, int height, bool visible = true,
                DisplayMode mode = DisplayMode::Windowed);
    void show();
    void destroy();

    // Switch between windowed and borderless-fullscreen at runtime. Updates width()/height()
    // to the new client size and flags a resize so the caller re-sizes the swapchain.
    void setDisplayMode(DisplayMode mode);
    DisplayMode displayMode() const { return mode_; }

    // Pump pending messages; returns false once a quit is requested.
    bool pumpMessages();

    HWND hwnd() const { return hwnd_; }
    int  width() const { return width_; }
    int  height() const { return height_; }

    // Returns true once after a size change, then clears the flag.
    bool consumeResize();

    // Raw input snapshot.
    bool keyDown(int vk) const { return vk >= 0 && vk < 256 && keyDown_[vk]; }
    bool mouseButton(int b) const { return b >= 0 && b < 5 && mouseDown_[b]; }
    int  mouseDeltaX() const { return mouseDeltaX_; }
    int  mouseDeltaY() const { return mouseDeltaY_; }
    int  mouseX() const { return mouseX_; }
    int  mouseY() const { return mouseY_; }
    void clearMouseDelta() { mouseDeltaX_ = mouseDeltaY_ = 0; }

    // FPS mouse look: hide + recenter the cursor each frame and report the delta.
    void setMouseCapture(bool capture);
    void updateMouseLook();      // call once per frame while captured

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void applyDisplayMode();   // re-style + reposition the window for mode_

    HWND hwnd_ = nullptr;
    int  width_ = 0;
    int  height_ = 0;
    DisplayMode mode_ = DisplayMode::Windowed;
    int  windowedWidth_ = 0;   // client size to restore when leaving borderless
    int  windowedHeight_ = 0;
    bool quit_ = false;
    bool resized_ = false;
    bool mouseCaptured_ = false;
    std::array<bool, 256> keyDown_{};
    std::array<bool, 5>   mouseDown_{};
    int  mouseDeltaX_ = 0;
    int  mouseDeltaY_ = 0;
    int  mouseX_ = 0;
    int  mouseY_ = 0;
};

} // namespace pulse
