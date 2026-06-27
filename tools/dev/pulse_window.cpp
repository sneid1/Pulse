// Windowed engine demo. Drives the real Engine (SceneFrame -> HDR forward -> AgX
// tonemap -> present) with a spinning cube on a ground plane. ESC or close quits.

#define PULSE_AGILITY_SDK_IMPL
#include "Engine/RHI/AgilitySDK.hpp"

#include "Engine/Engine.hpp"
#include "Engine/Core/Paths.hpp"
#include "Platform/Window.hpp"
#include "DemoScene.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace pulse;

int main() {
    setWorkingDirectoryToExecutableFolder();

    uint32_t W = 1280, H = 720;
    Window window;
    window.create(L"Pulse spinning cube demo", static_cast<int>(W), static_cast<int>(H));

    Engine engine;
    Engine::Config cfg;
    cfg.hwnd = window.hwnd();
    cfg.width = W; cfg.height = H;
#if defined(PULSE_DEBUG_LAYER)
    cfg.enableDebugLayer = true;
#endif
    engine.init(cfg);
    std::printf("%s\n[pulse_window] running. press ESC or close the window to quit\n",
                engine.device().gpuInfoString().c_str());

    demo::DemoScene scene;
    scene.create(engine);

    std::vector<MeshInstance> instances;
    using Clock = std::chrono::high_resolution_clock;
    auto last = Clock::now();
    float t = 0.0f;
    float smoothedFps = 0.0f;
    uint64_t frame = 0;
    while (window.pumpMessages()) {
        if (window.consumeResize()) {
            engine.resize(static_cast<uint32_t>(window.width()), static_cast<uint32_t>(window.height()));
        }
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        t += dt;
        if (dt > 0.0f) {
            const float inst = 1.0f / dt;
            smoothedFps = smoothedFps == 0.0f ? inst : smoothedFps * 0.9f + inst * 0.1f;
        }

        const SceneFrame sf = scene.frame(instances, t, engine.width(), engine.height(), smoothedFps);
        engine.renderFrame(sf);
        ++frame;
    }

    std::printf("[pulse_window] exited after %llu frames\n", (unsigned long long)frame);
    return 0;
}
