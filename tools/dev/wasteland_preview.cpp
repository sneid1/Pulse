// Standalone outdoor-arena preview. Links pulse_engine + Game/Wasteland.cpp only (no PulseGame),
// so it renders + connectivity-checks the procedural wasteland even while the game target is
// mid-refactor. Loads the real medieval-sceneray PBR textures (baseColor + normal via the
// engine's stb_image path) so the look is faithful. Dev tool only.
//
// Usage: wasteland_preview [--room N] [--template NAME] [--seed S] [--out f.bmp]
//                          [--persp|--fp|--map] [--raster] [--biome N]
//                          (biome: 0 foundry, 1 furnace, 2 reliquary)

#define PULSE_AGILITY_SDK_IMPL
#include "Engine/RHI/AgilitySDK.hpp"

#include "Engine/Engine.hpp"
#include "Engine/Core/Image.hpp"
#include "Engine/Core/ImageFile.hpp"
#include "Engine/Core/Mat.hpp"
#include "Game/Wasteland.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace pulse;

static std::string resolveAsset(const std::string& rel) {
    const char* prefixes[] = { "", "../", "../../", "../../../" };
    for (const char* p : prefixes) { std::string c = std::string(p) + rel; if (std::filesystem::exists(c)) return c; }
    return rel;
}

int main(int argc, char** argv) {
    int room = 0; unsigned seed = 1234u;
    std::string forcedTemplate;
    std::string out = "wasteland_preview.bmp";
    int mode = 0;            // 0 persp, 1 fp, 2 map
    bool raster = false;
    int biome = 0;          // 0 rocky, 1 forest
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--room" && i + 1 < argc) room = std::atoi(argv[++i]);
        else if ((a == "--template" || a == "--room-name") && i + 1 < argc) forcedTemplate = argv[++i];
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--persp") mode = 0;
        else if (a == "--fp") mode = 1;
        else if (a == "--map") mode = 2;
        else if (a == "--raster") raster = true;
        else if (a == "--biome" && i + 1 < argc) biome = std::atoi(argv[++i]);
    }

    Engine::Config cfg; cfg.hwnd = nullptr; cfg.width = 1280; cfg.height = 720; cfg.forceRaster = raster;
#if defined(PULSE_DEBUG_LAYER)
    cfg.enableDebugLayer = true;
#endif
    Engine engine; engine.init(cfg);

    Wasteland w;
    w.setBrutalist(true);
    w.setOpenTop(mode == 2);
    if (!forcedTemplate.empty()) w.setForcedTemplate(forcedTemplate);
    if (!w.load(engine)) { std::fprintf(stderr, "wasteland load failed\n"); return 2; }
    w.generate(static_cast<Biome>(biome), static_cast<uint64_t>(seed), room);

    // Connectivity self-check: flood-fill the open ground from spawn; report reachable fraction.
    {
        const int s = w.subRes(), W = 32 * s, H = 24 * s;
        std::vector<uint8_t> seen(static_cast<size_t>(W) * H, 0);
        int total = 0;
        for (int z = 0; z < H; ++z) for (int x = 0; x < W; ++x) if (!w.solidFineCell(x, z)) ++total;
        std::vector<std::pair<int,int>> st;
        const int sx = w.spawnX() * s + s / 2, sz = w.spawnZ() * s + s / 2;
        int reached = 0;
        if (!w.solidFineCell(sx, sz)) { st.push_back({ sx, sz }); seen[static_cast<size_t>(sz) * W + sx] = 1; }
        while (!st.empty()) {
            auto [cx, cz] = st.back(); st.pop_back(); ++reached;
            const int nb[4][2] = { { cx + 1, cz }, { cx - 1, cz }, { cx, cz + 1 }, { cx, cz - 1 } };
            for (auto& n : nb) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= W || n[1] >= H) continue;
                if (seen[static_cast<size_t>(n[1]) * W + n[0]] || w.solidFineCell(n[0], n[1])) continue;
                seen[static_cast<size_t>(n[1]) * W + n[0]] = 1; st.push_back({ n[0], n[1] });
            }
        }
        std::printf("[check] room %d seed %u: reachable %d / %d open cells (%.0f%%)\n",
                    room, seed, reached, total, total ? 100.0 * reached / total : 0.0);
    }

    std::vector<MeshInstance> inst;
    uint64_t id = 1;
    for (const DungeonDraw& d : w.draws()) {
        MeshInstance mi; mi.id = id++; mi.mesh = d.mesh; mi.material = d.material; mi.transform = d.transform;
        inst.push_back(mi);
    }

    SceneFrame f;
    // Golden-hour mood: a low warm sun for long dramatic shadows, a cool graded sky (the resolve
    // sky gradient reads clearColor as the horizon + sunColor for the glow), near-clear air.
    f.clearColor = { 0.48f, 0.53f, 0.62f };           // hazy horizon (also the fog target)
    f.sun.direction = normalize3({ -0.42f, -0.55f, 0.62f });
    f.sun.color = { 1.0f, 0.80f, 0.55f };             // warm key
    f.sun.intensity = 3.0f;
    f.sun.ambient = 0.32f;                            // cool sky fill (sun carries the warmth)
    f.post.fogDensity = 0.008f;                       // a touch of aerial depth
    f.instances = inst;

    if (mode == 2) {
        f.camera.position = { 16.0f, 26.0f, 12.0f }; f.camera.yaw = 1.5708f; f.camera.pitch = -1.40f;
        f.camera.fovDeg = 75.0f; f.camera.farZ = 160.0f;
    } else if (mode == 1) {
        f.camera.position = { static_cast<float>(w.spawnX()) + 0.5f, 1.65f, static_cast<float>(w.spawnZ()) + 0.5f };
        f.camera.yaw = 0.5f; f.camera.pitch = 0.02f; f.camera.fovDeg = 80.0f; f.camera.farZ = 120.0f;
    } else {
        const float px = -4.0f, py = 17.0f, pz = -4.0f, tx = 16.0f, ty = 1.0f, tz = 12.0f;
        const float dx = tx - px, dy = ty - py, dz = tz - pz, len = std::sqrt(dx*dx + dy*dy + dz*dz);
        f.camera.position = { px, py, pz };
        f.camera.yaw = std::atan2(dz, dx); f.camera.pitch = std::asin(dy / len);
        f.camera.fovDeg = 62.0f; f.camera.farZ = 200.0f;
    }

    Image img;
    if (!engine.captureFrame(f, img)) { std::fprintf(stderr, "capture failed\n"); return 2; }
    if (!img.saveBmp(out)) { std::fprintf(stderr, "save failed: %s\n", out.c_str()); return 2; }
    std::printf("wrote %s (room %d, seed %u, biome %d, %zu instances)\n",
                out.c_str(), room, seed, biome, inst.size());
    return 0;
}
