#include "Platform/Window.hpp"
#include "Engine/Core/Log.hpp"

#include <windowsx.h>

namespace pulse {

static const wchar_t* kClassName = L"PulseWindowClass";
static constexpr int kPulseIconResourceId = 101; // Matches src/pulse.rc.in.

static HICON loadPulseIcon(HINSTANCE hinst, int width, int height) {
    return static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(kPulseIconResourceId),
                                         IMAGE_ICON, width, height,
                                         LR_DEFAULTCOLOR | LR_SHARED));
}

void Window::create(const wchar_t* title, int width, int height, bool visible, DisplayMode mode) {
    width_ = width;
    height_ = height;
    windowedWidth_ = width;
    windowedHeight_ = height;

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    HICON icon = loadPulseIcon(hinst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    HICON iconSmall = loadPulseIcon(hinst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!iconSmall) iconSmall = icon;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::wndProc;
    wc.hInstance = hinst;
    wc.hIcon = icon;
    wc.hIconSm = iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);   // ignore failure if already registered

    RECT rc{ 0, 0, width, height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, hinst, this);
    PULSE_CHECK(hwnd_ != nullptr, "CreateWindowEx failed");
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    // The window is born windowed (WS_OVERLAPPEDWINDOW above); switch to borderless before
    // it is shown if that is the requested mode, so the first paint is already fullscreen.
    if (mode == DisplayMode::BorderlessFullscreen) {
        setDisplayMode(mode);
    }
    if (visible) {
        show();
    }
}

void Window::setDisplayMode(DisplayMode mode) {
    if (mode == mode_ && hwnd_) return;
    mode_ = mode;
    applyDisplayMode();
}

void Window::applyDisplayMode() {
    if (!hwnd_) return;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (mode_ == DisplayMode::BorderlessFullscreen) {
        const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
        const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        width_ = mw; height_ = mh;
    } else {
        RECT rc{ 0, 0, windowedWidth_, windowedHeight_ };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        const int wx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
        const int wy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, wx, wy, ww, wh, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        width_ = windowedWidth_; height_ = windowedHeight_;
    }
    resized_ = true;   // caller resizes the swapchain on the next consumeResize()
}

void Window::show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void Window::destroy() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool Window::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { quit_ = true; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !quit_;
}

bool Window::consumeResize() {
    const bool r = resized_;
    resized_ = false;
    return r;
}

LRESULT CALLBACK Window::wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
        case WM_CLOSE:   self->quit_ = true; return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) return 0;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            if (w > 0 && h > 0 && (w != self->width_ || h != self->height_)) {
                self->width_ = w; self->height_ = h; self->resized_ = true;
            }
            return 0;
        }
        case WM_KEYDOWN:
            // ESC is delivered to the game (it opens the pause menu); the window quits
            // only via WM_CLOSE / Alt-F4. The app decides ESC policy, not the platform.
            if (wparam < 256) self->keyDown_[wparam] = true;
            return 0;
        case WM_KEYUP:
            if (wparam < 256) self->keyDown_[wparam] = false;
            return 0;
        case WM_MOUSEMOVE:
            self->mouseX_ = GET_X_LPARAM(lparam);
            self->mouseY_ = GET_Y_LPARAM(lparam);
            return 0;
        case WM_LBUTTONDOWN:
            self->mouseX_ = GET_X_LPARAM(lparam); self->mouseY_ = GET_Y_LPARAM(lparam);
            self->mouseDown_[0] = true; SetCapture(hwnd); return 0;
        case WM_LBUTTONUP:
            self->mouseX_ = GET_X_LPARAM(lparam); self->mouseY_ = GET_Y_LPARAM(lparam);
            self->mouseDown_[0] = false; ReleaseCapture(); return 0;
        case WM_RBUTTONDOWN:
            self->mouseX_ = GET_X_LPARAM(lparam); self->mouseY_ = GET_Y_LPARAM(lparam);
            self->mouseDown_[1] = true; return 0;
        case WM_RBUTTONUP:
            self->mouseX_ = GET_X_LPARAM(lparam); self->mouseY_ = GET_Y_LPARAM(lparam);
            self->mouseDown_[1] = false; return 0;
        case WM_MBUTTONDOWN:
            self->mouseX_ = GET_X_LPARAM(lparam); self->mouseY_ = GET_Y_LPARAM(lparam);
            self->mouseDown_[2] = true; return 0;
        case WM_MBUTTONUP:
            self->mouseX_ = GET_X_LPARAM(lparam); self->mouseY_ = GET_Y_LPARAM(lparam);
            self->mouseDown_[2] = false; return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void Window::setMouseCapture(bool capture) {
    if (capture == mouseCaptured_) return;
    mouseCaptured_ = capture;
    ShowCursor(capture ? FALSE : TRUE);
}

void Window::updateMouseLook() {
    if (!mouseCaptured_ || !hwnd_) { mouseDeltaX_ = 0; mouseDeltaY_ = 0; return; }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    POINT center{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    POINT screenCenter = center;
    ClientToScreen(hwnd_, &screenCenter);
    POINT cur{};
    GetCursorPos(&cur);
    POINT curClient = cur;
    ScreenToClient(hwnd_, &curClient);
    mouseX_ = curClient.x;
    mouseY_ = curClient.y;
    mouseDeltaX_ = curClient.x - center.x;
    mouseDeltaY_ = curClient.y - center.y;
    SetCursorPos(screenCenter.x, screenCenter.y);
}

} // namespace pulse
