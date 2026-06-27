#pragma once

// A shared demo scene (spinning cube on a checkered ground plane) used by both
// the windowed demo (pulse_window) and the headless validator (engine_smoke), so
// renderFrame (windowed) and captureFrame (headless) provably draw the same thing.

#include "Engine/Engine.hpp"
#include "Engine/UI/UiDrawList.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace pulse::demo {

inline MeshHandle makeCube(Engine& engine) {
    std::vector<StaticVertex> verts;
    std::vector<uint32_t> indices;
    auto addFace = [&](Vec3f n, Vec3f uax, Vec3f vax) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        const Vec3f c = n * 0.5f;
        auto v = [&](Vec3f p, float u, float w) {
            StaticVertex sv; sv.pos = p; sv.nrm = n; sv.uv0[0] = u; sv.uv0[1] = w;
            sv.color = { 1, 1, 1, 1 }; verts.push_back(sv);
        };
        v(c - uax * 0.5f - vax * 0.5f, 0, 0);
        v(c + uax * 0.5f - vax * 0.5f, 1, 0);
        v(c + uax * 0.5f + vax * 0.5f, 1, 1);
        v(c - uax * 0.5f + vax * 0.5f, 0, 1);
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    };
    addFace({ 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 });
    addFace({ -1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 });
    addFace({ 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 });
    addFace({ 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 });
    addFace({ 0, 0, 1 }, { -1, 0, 0 }, { 0, 1, 0 });
    addFace({ 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 });
    return engine.createMesh({ verts, indices });
}

inline MeshHandle makeGroundPlane(Engine& engine, float halfSize) {
    const float s = halfSize;
    std::vector<StaticVertex> verts(4);
    auto set = [&](int i, float x, float z, float u, float w) {
        verts[i].pos = { x, 0, z }; verts[i].nrm = { 0, 1, 0 };
        verts[i].uv0[0] = u; verts[i].uv0[1] = w; verts[i].color = { 1, 1, 1, 1 };
    };
    set(0, -s, -s, 0, 0); set(1, s, -s, 1, 0); set(2, s, s, 1, 1); set(3, -s, s, 0, 1);
    std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
    return engine.createMesh({ verts, indices });
}

inline TextureHandle makeChecker(Engine& engine, uint32_t size, uint32_t cell) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            const bool on = ((x / cell) ^ (y / cell)) & 1;
            uint8_t* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            const uint8_t v = on ? 230 : 70;
            p[0] = v; p[1] = v; p[2] = on ? 230 : 110; p[3] = 255;
        }
    return engine.createTexture({ size, size, px.data() });
}

struct DemoScene {
    MeshHandle     cube{}, ground{};
    MaterialHandle cubeMat{}, floorMat{};
    std::optional<UiDrawList> ui;

    void create(Engine& engine) {
        cube = makeCube(engine);
        ground = makeGroundPlane(engine, 12.0f);
        const TextureHandle checker = makeChecker(engine, 256, 32);
        const TextureHandle floorTex = makeChecker(engine, 256, 16);
        cubeMat = engine.createMaterial({ checker, { 1.0f, 0.95f, 0.9f, 1 }, 0.0f });
        floorMat = engine.createMaterial({ floorTex, { 0.5f, 0.55f, 0.65f, 1 }, 0.0f });
        ui.emplace(engine.font());
    }

    // Populate `instances` + the HUD for time t and return a SceneFrame referencing
    // them. Keep `instances` and this DemoScene alive until the frame is submitted.
    SceneFrame frame(std::vector<MeshInstance>& instances, float t, uint32_t screenW, uint32_t screenH,
                     float fps = 0.0f) {
        instances.clear();
        MeshInstance floor;
        floor.id = 1; floor.mesh = ground; floor.material = floorMat;
        floor.transform = Mat4::identity();
        instances.push_back(floor);

        MeshInstance box;
        box.id = 2; box.mesh = cube; box.material = cubeMat;
        box.transform = mul(rotationY(t * 0.7f), translation(0.0f, 0.9f, 0.0f));
        instances.push_back(box);

        // HUD: a crosshair + a couple of text labels, to validate the GPU UI path.
        ui->reset();
        const float cx = screenW * 0.5f, cy = screenH * 0.5f;
        const uint32_t white = rgb(235, 242, 246);
        ui->rect(cx - 1, cy - 9, 2, 8, white);
        ui->rect(cx - 1, cy + 1, 2, 8, white);
        ui->rect(cx - 9, cy - 1, 8, 2, white);
        ui->rect(cx + 1, cy - 1, 8, 2, white);
        ui->text(20, 18, "PULSE", rgb(110, 200, 255), 2.0f);
        char line[64];
        std::snprintf(line, sizeof(line), "ENGINE M0  T %.1f", t);
        ui->text(20, 50, line, rgb(150, 160, 174), 1.5f);
        ui->rect(20, static_cast<float>(screenH) - 34, 240, 14, rgba(20, 28, 40, 200));
        ui->rect(22, static_cast<float>(screenH) - 32, 168, 10, rgb(70, 190, 255));
        char fpsText[32];
        std::snprintf(fpsText, sizeof(fpsText), "%.0f FPS", fps);
        ui->textRight(static_cast<float>(screenW) - 20, 18, fpsText, rgb(120, 230, 160), 1.5f);

        SceneFrame sf;
        sf.camera.position = { 0.0f, 2.4f, -4.8f };
        sf.camera.yaw = 1.5707963f;     // +Z (engine yaw=0 is +X)
        sf.camera.pitch = -0.32f;
        sf.camera.fovDeg = 60.0f;
        sf.sun.direction = normalize3({ -0.45f, -0.8f, -0.4f });
        sf.sun.color = { 1.0f, 0.96f, 0.9f };
        sf.sun.intensity = 2.6f;
        sf.sun.ambient = 0.16f;
        sf.post.exposure = 1.0f;
        sf.clearColor = { 0.015f, 0.02f, 0.03f };
        sf.instances = instances;
        sf.ui = ui->vertices();
        return sf;
    }
};

} // namespace pulse::demo
