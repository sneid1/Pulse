#pragma once

#include <array>
#include <cstdint>

namespace pulse {

struct InputState {
    std::array<bool, 256> keyDown{};
    std::array<bool, 256> keyPressed{};
    std::array<bool, 5> mouseDown{};
    std::array<bool, 5> mousePressed{};
    int mouseDeltaX = 0;
    int mouseDeltaY = 0;
    int mouseX = 0;
    int mouseY = 0;
    bool quitRequested = false;
    bool hasFocus = false;

    bool down(int vk) const {
        return vk >= 0 && vk < static_cast<int>(keyDown.size()) && keyDown[static_cast<size_t>(vk)];
    }

    bool pressed(int vk) const {
        return vk >= 0 && vk < static_cast<int>(keyPressed.size()) && keyPressed[static_cast<size_t>(vk)];
    }

    void endFrame() {
        keyPressed.fill(false);
        mousePressed.fill(false);
        mouseDeltaX = 0;
        mouseDeltaY = 0;
    }
};

} // namespace pulse
