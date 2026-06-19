#include "Game/PulseGame.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <windows.h>

#include "Engine/Audio.hpp"
#include "Engine/Config.hpp"
#include "Engine/GpuSceneRenderer.hpp"
#include "Engine/Renderer.hpp"

namespace pulse {
namespace {

const std::array<std::string, 24> Arena = {
    "################################",
    "#..............##..............#",
    "#..####........##........####..#",
    "#..#.......................#...#",
    "#..#....###.........###....#...#",
    "#.......#.............#........#",
    "#.......#.....##......#........#",
    "#.............##...............#",
    "####.................####......#",
    "#........###...................#",
    "#........#..........###........#",
    "#....................#.........#",
    "#.....##.......P.....#.....##..#",
    "#.....##.............#.....##..#",
    "#............###...............#",
    "#............#.................#",
    "#..####......#......####.......#",
    "#.....#.................#......#",
    "#.....#......##.........#......#",
    "#............##................#",
    "#..####..................####..#",
    "#..............##..............#",
    "#..............##..............#",
    "################################"
};

constexpr float MaxRayDistance = 40.0f;

// The executable sets its working directory to its own folder (build/), so an
// asset may live alongside it or up in the project root. Try a few prefixes.
std::string resolveAsset(const std::string& rel) {
    const std::array<const char*, 4> prefixes = {"", "../", "../../", "../../../"};
    for (const char* prefix : prefixes) {
        std::string candidate = std::string(prefix) + rel;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return rel; // not found; the caller fails loudly
}

std::string twoDigits(int value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%02d", std::max(0, value));
    return buffer;
}

std::string threeDigits(int value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%03d", std::max(0, value));
    return buffer;
}

struct ObjIndex {
    int vertex = -1;
    int uv = -1;
    int normal = -1;
};

struct ObjMaterialInfo {
    uint32_t color = rgb(255, 255, 255);
    float emissive = 0.0f;
};

int parseObjIndexValue(const std::string& value, int count) {
    if (value.empty()) {
        return -1;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0) {
        return -1;
    }
    const long index = parsed > 0 ? parsed - 1 : static_cast<long>(count) + parsed;
    if (index < 0 || index >= count) {
        return -1;
    }
    return static_cast<int>(index);
}

ObjIndex parseObjIndexToken(const std::string& token, int vertexCount, int uvCount, int normalCount) {
    ObjIndex out;
    const size_t firstSlash = token.find('/');
    if (firstSlash == std::string::npos) {
        out.vertex = parseObjIndexValue(token, vertexCount);
        return out;
    }

    out.vertex = parseObjIndexValue(token.substr(0, firstSlash), vertexCount);
    const size_t secondSlash = token.find('/', firstSlash + 1);
    const std::string uv = secondSlash == std::string::npos
        ? token.substr(firstSlash + 1)
        : token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
    out.uv = parseObjIndexValue(uv, uvCount);
    if (secondSlash != std::string::npos) {
        out.normal = parseObjIndexValue(token.substr(secondSlash + 1), normalCount);
    }
    return out;
}

uint32_t colorFromUnitRgb(float r, float g, float b) {
    const auto ch = [](float v) {
        return static_cast<uint8_t>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return rgb(ch(r), ch(g), ch(b));
}

float luminance(float r, float g, float b) {
    return r * 0.2126f + g * 0.7152f + b * 0.0722f;
}

std::unordered_map<std::string, ObjMaterialInfo> loadObjMaterials(const std::string& objPath, const std::string& mtlName) {
    namespace fs = std::filesystem;
    std::unordered_map<std::string, ObjMaterialInfo> materials;
    if (mtlName.empty()) {
        return materials;
    }

    const fs::path objFsPath(objPath);
    const fs::path mtlPath = objFsPath.parent_path() / mtlName;
    std::ifstream in(mtlPath.string());
    if (!in) {
        return materials;
    }

    std::string currentName;
    ObjMaterialInfo current;
    bool haveCurrent = false;
    const auto flush = [&]() {
        if (haveCurrent && !currentName.empty()) {
            materials[currentName] = current;
        }
    };

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag == "newmtl") {
            flush();
            stream >> currentName;
            current = ObjMaterialInfo{};
            haveCurrent = !currentName.empty();
        } else if (tag == "Kd" && haveCurrent) {
            float r = 1.0f, g = 1.0f, b = 1.0f;
            stream >> r >> g >> b;
            if (stream) {
                current.color = colorFromUnitRgb(r, g, b);
            }
        } else if (tag == "Ke" && haveCurrent) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            stream >> r >> g >> b;
            if (stream) {
                current.emissive = clamp(luminance(r, g, b) * 0.85f, 0.0f, 2.5f);
            }
        }
    }
    flush();
    return materials;
}

GpuVec3 gpuSub(GpuVec3 a, GpuVec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

GpuVec3 gpuMul(GpuVec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

float gpuDot(GpuVec3 a, GpuVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

GpuVec3 gpuCross(GpuVec3 a, GpuVec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

GpuVec3 gpuNormalize(GpuVec3 v) {
    const float lenSq = gpuDot(v, v);
    if (lenSq <= 0.000001f) {
        return {0.0f, 1.0f, 0.0f};
    }
    return gpuMul(v, 1.0f / std::sqrt(lenSq));
}

GpuVertex gpuVertex(GpuVec3 p, GpuVec3 n, float u, float v, uint32_t color = 0x00FFFFFFu, float emissive = 0.0f) {
    uint32_t alpha = (color >> 24u) & 0xFFu;
    if (alpha == 0u) {
        alpha = 255u;
    }
    const GpuColor c = GpuSceneRenderer::colorFromRgb(color, static_cast<float>(alpha) / 255.0f);
    GpuVertex out;
    out.x = p.x;
    out.y = p.y;
    out.z = p.z;
    out.nx = n.x;
    out.ny = n.y;
    out.nz = n.z;
    out.u = u;
    out.v = v;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = c.a;
    out.emissive = emissive;
    return out;
}

void pushGpuTri(GpuMeshDesc& mesh, GpuVec3 a, GpuVec3 b, GpuVec3 c,
                float au, float av, float bu, float bv, float cu, float cv, uint32_t color = 0x00FFFFFFu,
                float emissive = 0.0f) {
    const GpuVec3 normal = gpuNormalize(gpuCross(gpuSub(b, a), gpuSub(c, a)));
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(gpuVertex(a, normal, au, av, color, emissive));
    mesh.vertices.push_back(gpuVertex(b, normal, bu, bv, color, emissive));
    mesh.vertices.push_back(gpuVertex(c, normal, cu, cv, color, emissive));
    mesh.indices.push_back(base);
    mesh.indices.push_back(base + 1u);
    mesh.indices.push_back(base + 2u);
}

void pushGpuQuad(GpuMeshDesc& mesh, GpuVec3 a, GpuVec3 b, GpuVec3 c, GpuVec3 d,
                 float u0, float v0, float u1, float v1, uint32_t color = 0x00FFFFFFu) {
    pushGpuTri(mesh, a, b, c, u0, v0, u1, v0, u1, v1, color);
    pushGpuTri(mesh, a, c, d, u0, v0, u1, v1, u0, v1, color);
}

void pushGpuBox(GpuMeshDesc& mesh, float x0, float y0, float z0, float x1, float y1, float z1, uint32_t color = 0x00FFFFFFu) {
    const GpuVec3 nnn{x0, y0, z0};
    const GpuVec3 pnn{x1, y0, z0};
    const GpuVec3 npn{x0, y1, z0};
    const GpuVec3 ppn{x1, y1, z0};
    const GpuVec3 nnp{x0, y0, z1};
    const GpuVec3 pnp{x1, y0, z1};
    const GpuVec3 npp{x0, y1, z1};
    const GpuVec3 ppp{x1, y1, z1};
    pushGpuQuad(mesh, nnn, npn, ppn, pnn, x0, y0, x1, y1, color);
    pushGpuQuad(mesh, nnp, pnp, ppp, npp, x0, y0, x1, y1, color);
    pushGpuQuad(mesh, nnn, pnn, pnp, nnp, x0, z0, x1, z1, color);
    pushGpuQuad(mesh, npn, npp, ppp, ppn, x0, z0, x1, z1, color);
    pushGpuQuad(mesh, nnn, nnp, npp, npn, z0, y0, z1, y1, color);
    pushGpuQuad(mesh, pnn, ppn, ppp, pnp, z0, y0, z1, y1, color);
}

GpuMeshDesc makePickupMesh(uint32_t color) {
    GpuMeshDesc mesh;
    const GpuVec3 top{0.0f, 1.0f, 0.0f};
    const GpuVec3 bottom{0.0f, -1.0f, 0.0f};
    const GpuVec3 front{0.0f, 0.0f, 1.0f};
    const GpuVec3 back{0.0f, 0.0f, -1.0f};
    const GpuVec3 left{-1.0f, 0.0f, 0.0f};
    const GpuVec3 right{1.0f, 0.0f, 0.0f};
    pushGpuTri(mesh, top, front, right, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, top, right, back, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, top, back, left, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, top, left, front, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, bottom, right, front, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, bottom, back, right, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, bottom, left, back, 0, 0, 1, 0, 1, 1, color);
    pushGpuTri(mesh, bottom, front, left, 0, 0, 1, 0, 1, 1, color);
    return mesh;
}

GpuMeshDesc makeTracerMesh() {
    GpuMeshDesc mesh;
    const uint32_t color = rgb(255, 226, 150);
    // A cross-ribbon survives perspective as a narrow hitscan streak. A tiny
    // box volume can disappear head-on once the tracer is correctly thin.
    pushGpuQuad(mesh,
                {-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f},
                {0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f},
                0.0f, 0.0f, 1.0f, 1.0f, color);
    pushGpuQuad(mesh,
                {0.0f, -0.5f, -0.5f}, {0.0f, 0.5f, -0.5f},
                {0.0f, 0.5f, 0.5f}, {0.0f, -0.5f, 0.5f},
                0.0f, 0.0f, 1.0f, 1.0f, color);
    return mesh;
}

// A spiky three-axis "jack" so an enemy projectile reads as a hostile bolt and
// is never mistaken for the smooth pickup gems.
GpuMeshDesc makeProjectileMesh(uint32_t color) {
    GpuMeshDesc mesh;
    const float L = 1.0f;   // spike length
    const float T = 0.34f;  // spike thickness
    pushGpuBox(mesh, -L, -T, -T, L, T, T, color);
    pushGpuBox(mesh, -T, -L, -T, T, L, T, color);
    pushGpuBox(mesh, -T, -T, -L, T, T, L, color);
    return mesh;
}

// An ammo crate: a chunky box with a raised lid band, so it reads as a supply
// box rather than a health/shield gem.
GpuMeshDesc makeAmmoPickupMesh(uint32_t color, uint32_t band) {
    GpuMeshDesc mesh;
    pushGpuBox(mesh, -0.85f, -0.6f, -0.62f, 0.85f, 0.5f, 0.62f, color); // body
    pushGpuBox(mesh, -0.9f, 0.42f, -0.68f, 0.9f, 0.72f, 0.68f, color);  // lid
    pushGpuBox(mesh, -0.92f, 0.0f, -0.2f, 0.92f, 0.18f, 0.2f, band);    // centre band
    return mesh;
}

} // namespace

PulseGame::PulseGame() {
    loadConfig(false);
    if (!loadWeaponMesh()) {
        throw std::runtime_error("Required weapon mesh missing or invalid: assets/models/pulse_carbine_viewmodel.obj");
    }
    if (!loadHandsMesh()) {
        throw std::runtime_error("Required hand meshes missing or invalid: assets/models/pulse_left_hand_viewmodel.obj and pulse_right_hand_viewmodel.obj");
    }
    if (!loadWeaponTexture()) {
        throw std::runtime_error("Required weapon texture could not be created");
    }
    if (!loadTexture("assets/textures/wall.ptex", wallTex_)) {
        throw std::runtime_error("Required world texture missing or invalid: assets/textures/wall.ptex");
    }
    if (!loadTexture("assets/textures/floor.ptex", floorTex_)) {
        throw std::runtime_error("Required world texture missing or invalid: assets/textures/floor.ptex");
    }
    if (!loadTexture("assets/textures/ceiling.ptex", ceilingTex_)) {
        throw std::runtime_error("Required world texture missing or invalid: assets/textures/ceiling.ptex");
    }
    if (!loadTexture("assets/textures/cover.ptex", coverTex_)) {
        throw std::runtime_error("Required world texture missing or invalid: assets/textures/cover.ptex");
    }
    buildEnemyMesh();
    resetRun();
}

void PulseGame::pushTri(std::vector<MeshTri3>& mesh,
                        float ax, float ay, float az, float bx, float by, float bz,
                        float cx, float cy, float cz, int part) {
    MeshTri3 t;
    t.vx[0] = ax; t.vy[0] = ay; t.vz[0] = az;
    t.vx[1] = bx; t.vy[1] = by; t.vz[1] = bz;
    t.vx[2] = cx; t.vy[2] = cy; t.vz[2] = cz;
    t.part = part;
    mesh.push_back(t);
}

// An octahedron (+X forward). Winding is fixed up at shade time.
void PulseGame::pushOcta(std::vector<MeshTri3>& m, float fwd, float back, float up, float dn, float side) {
    const float tp[3] = {0, up, 0}, bt[3] = {0, -dn, 0}, fr[3] = {fwd, 0, 0};
    const float bk[3] = {-back, 0, 0}, lf[3] = {0, 0, -side}, rt[3] = {0, 0, side};
    const auto B = [&](const float a[3], const float b[3], const float c[3]) {
        pushTri(m, a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2], 0);
    };
    B(tp, fr, rt); B(tp, rt, bk); B(tp, bk, lf); B(tp, lf, fr);
    B(bt, rt, fr); B(bt, bk, rt); B(bt, lf, bk); B(bt, fr, lf);
}

// Forward-pointing emissive eye pyramid.
void PulseGame::pushEye(std::vector<MeshTri3>& m, float cx, float cy, float r, float tipx) {
    const float tip[3] = {tipx, cy, 0}, top[3] = {cx, cy + r, 0}, bot[3] = {cx, cy - r, 0};
    const float lf[3] = {cx, cy, -r}, rt[3] = {cx, cy, r};
    pushTri(m, tip[0], tip[1], tip[2], top[0], top[1], top[2], rt[0], rt[1], rt[2], 1);
    pushTri(m, tip[0], tip[1], tip[2], rt[0], rt[1], rt[2], bot[0], bot[1], bot[2], 1);
    pushTri(m, tip[0], tip[1], tip[2], bot[0], bot[1], bot[2], lf[0], lf[1], lf[2], 1);
    pushTri(m, tip[0], tip[1], tip[2], lf[0], lf[1], lf[2], top[0], top[1], top[2], 1);
}

void PulseGame::buildEnemyMesh() {
    for (auto& m : enemyMeshes_) {
        m.clear();
    }

    // ---- Rusher: a low, wide predatory manta - swept wings, a forward fang and
    // a ground-hugging body. Reads as a diving attacker, nothing like a gem.
    {
        auto& m = enemyMeshes_[0];
        pushOcta(m, 0.72f, 0.42f, 0.20f, 0.20f, 0.24f); // flat elongated fuselage
        // Forward fang/beak jutting ahead of the body.
        pushTri(m, 1.02f, 0.0f, 0.0f, 0.40f, 0.14f, 0.16f, 0.40f, 0.02f, -0.14f, 0);
        pushTri(m, 1.02f, 0.0f, 0.0f, 0.40f, -0.10f, -0.14f, 0.40f, 0.02f, 0.16f, 0);
        pushEye(m, 0.40f, -0.02f, 0.13f, 0.78f); // eye low on the snout
        // Big swept-back wings (right, then mirrored left).
        pushTri(m, 0.18f, 0.0f, 0.16f, -0.42f, 0.0f, 0.12f, -0.06f, 0.06f, 1.22f, 2);
        pushTri(m, 0.18f, 0.0f, -0.16f, -0.42f, 0.0f, -0.12f, -0.06f, 0.06f, -1.22f, 2);
        // Raised tail fin for a touch of menace.
        pushTri(m, -0.40f, 0.0f, 0.07f, -0.40f, 0.0f, -0.07f, -0.58f, 0.34f, 0.0f, 2);
    }

    // ---- Ranged: a tall hovering sentry - vertical pod, one big eye and three
    // splayed legs hanging beneath. Leggy and upright, unmistakably a turret.
    {
        auto& m = enemyMeshes_[1];
        pushOcta(m, 0.32f, 0.32f, 0.52f, 0.18f, 0.32f); // vertical pod
        pushEye(m, 0.26f, 0.14f, 0.27f, 0.60f);         // large forward eye
        // Three downward legs (front, back-right, back-left).
        pushTri(m, 0.12f, -0.16f, 0.09f, 0.12f, -0.16f, -0.09f, 0.46f, -0.66f, 0.0f, 2);
        pushTri(m, -0.08f, -0.16f, 0.12f, 0.04f, -0.16f, 0.17f, -0.30f, -0.66f, 0.46f, 2);
        pushTri(m, -0.08f, -0.16f, -0.12f, 0.04f, -0.16f, -0.17f, -0.30f, -0.66f, -0.46f, 2);
    }

    // ---- Tank: a hulking armoured brute - a wide chassis with a sloped glacis,
    // shoulder pauldrons and a glowing visor slit. The biggest threat on screen.
    {
        auto& m = enemyMeshes_[2];
        const float fx = 0.50f, bx = 0.52f, up = 0.46f, dn = 0.56f, sd = 0.62f;
        const float slope = 0.22f; // front-top pulled back -> sloped front plate
        const float c000[3] = {-bx, -dn, -sd}, c100[3] = {fx, -dn, -sd};
        const float c001[3] = {-bx, -dn, sd}, c101[3] = {fx, -dn, sd};
        const float c010[3] = {-bx, up, -sd}, c011[3] = {-bx, up, sd};
        const float gtl[3] = {fx - slope, up, -sd}, gtr[3] = {fx - slope, up, sd};
        const auto quad = [&](const float a[3], const float b[3], const float c[3], const float d[3], int part) {
            pushTri(m, a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2], part);
            pushTri(m, a[0], a[1], a[2], c[0], c[1], c[2], d[0], d[1], d[2], part);
        };
        quad(c000, c100, c101, c001, 0); // bottom
        quad(c000, c001, c011, c010, 0); // back
        quad(gtl, gtr, c011, c010, 0);   // top deck
        quad(c100, c101, gtr, gtl, 0);   // sloped front glacis
        quad(c101, gtr, c011, c001, 0);  // right
        quad(c100, c000, c010, gtl, 0);  // left
        // Shoulder pauldrons: bold raised wedges on each top side.
        pushTri(m, 0.10f, up, sd, -bx, up, sd, -0.20f, up + 0.28f, sd + 0.10f, 2);
        pushTri(m, 0.10f, up, -sd, -bx, up, -sd, -0.20f, up + 0.28f, -sd - 0.10f, 2);
        // Glowing visor slit on the glacis (wide, low): a flattened, widened eye.
        const float ex = fx - slope * 0.4f;
        const float vtip[3] = {ex + 0.26f, 0.10f, 0.0f};
        const float vtop[3] = {ex, 0.20f, 0.0f};
        const float vbot[3] = {ex, 0.02f, 0.0f};
        const float vlf[3] = {ex, 0.11f, -0.42f};
        const float vrt[3] = {ex, 0.11f, 0.42f};
        pushTri(m, vtip[0], vtip[1], vtip[2], vtop[0], vtop[1], vtop[2], vrt[0], vrt[1], vrt[2], 1);
        pushTri(m, vtip[0], vtip[1], vtip[2], vrt[0], vrt[1], vrt[2], vbot[0], vbot[1], vbot[2], 1);
        pushTri(m, vtip[0], vtip[1], vtip[2], vbot[0], vbot[1], vbot[2], vlf[0], vlf[1], vlf[2], 1);
        pushTri(m, vtip[0], vtip[1], vtip[2], vlf[0], vlf[1], vlf[2], vtop[0], vtop[1], vtop[2], 1);
    }

    // Load the authored Blender drones. The procedural meshes above supply
    // shatter debris shards only; runtime rendering requires authored assets.
    const char* enemyObjPaths[EnemyKindCount] = {
        "assets/models/pulse_enemy_rusher.obj",
        "assets/models/pulse_enemy_ranged.obj",
        "assets/models/pulse_enemy_tank.obj",
    };
    for (int k = 0; k < EnemyKindCount; ++k) {
        enemyMeshAssets_[static_cast<size_t>(k)] = MeshAsset{};
        if (!loadObjMesh(enemyObjPaths[static_cast<size_t>(k)], enemyMeshAssets_[static_cast<size_t>(k)])) {
            throw std::runtime_error(std::string("Required enemy mesh missing or invalid: ") + enemyObjPaths[static_cast<size_t>(k)]);
        }
    }
}

PulseGame::EnemyStyle PulseGame::styleFor(EnemyKind kind) const {
    EnemyStyle s;
    switch (kind) {
    case EnemyKind::Ranged:
        s.scale = 0.44f;
        s.body = rgb(150, 96, 235);
        s.wing = rgb(96, 60, 170);
        s.eye = rgb(165, 180, 255);
        break;
    case EnemyKind::Tank:
        s.scale = std::max(0.3f, tunables_.enemyTankScale);
        s.body = rgb(196, 132, 56);  // amber-bronze armour (distinct from green health gem)
        s.wing = rgb(150, 96, 36);
        s.eye = rgb(255, 110, 70);   // hot red-orange visor
        break;
    case EnemyKind::Rusher:
    default:
        s.scale = 0.50f;
        s.body = rgb(228, 64, 52);
        s.wing = rgb(176, 44, 38);
        s.eye = rgb(255, 168, 80);
        break;
    }
    return s;
}

bool PulseGame::loadTexture(const std::string& path, Texture& out) const {
    std::ifstream in(resolveAsset(path), std::ios::binary);
    if (!in) {
        return false;
    }
    std::array<char, 8> magic{};
    uint32_t width = 0;
    uint32_t height = 0;
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    in.read(reinterpret_cast<char*>(&width), sizeof(width));
    in.read(reinterpret_cast<char*>(&height), sizeof(height));
    if (!in || std::memcmp(magic.data(), "PULSETX1", magic.size()) != 0 ||
        width == 0 || height == 0 || width > 4096 || height > 4096) {
        return false;
    }
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint32_t> pixels(pixelCount);
    in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixelCount * sizeof(uint32_t)));
    if (!in) {
        return false;
    }
    Texture::Level base;
    base.width = static_cast<int>(width);
    base.height = static_cast<int>(height);
    base.pixels = std::move(pixels);
    out.levels.clear();
    out.levels.push_back(std::move(base));
    generateMips(out);
    return true;
}

void PulseGame::generateMips(Texture& tex) const {
    if (tex.levels.empty()) {
        return;
    }
    // Box-filter halving down to 8x8. Each coarser level is sampled at distance
    // so high-frequency grid lines average out instead of aliasing.
    while (tex.levels.back().width > 8 && tex.levels.back().height > 8) {
        const Texture::Level& src = tex.levels.back();
        Texture::Level dst;
        dst.width = src.width / 2;
        dst.height = src.height / 2;
        dst.pixels.resize(static_cast<size_t>(dst.width) * static_cast<size_t>(dst.height));
        for (int y = 0; y < dst.height; ++y) {
            for (int x = 0; x < dst.width; ++x) {
                const int sx = x * 2;
                const int sy = y * 2;
                uint32_t r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint32_t c = src.pixels[static_cast<size_t>(sy + dy) * static_cast<size_t>(src.width) + static_cast<size_t>(sx + dx)];
                        r += (c >> 16u) & 0xFFu;
                        g += (c >> 8u) & 0xFFu;
                        b += c & 0xFFu;
                    }
                }
                dst.pixels[static_cast<size_t>(y) * static_cast<size_t>(dst.width) + static_cast<size_t>(x)] =
                    rgb(static_cast<uint8_t>(r / 4u), static_cast<uint8_t>(g / 4u), static_cast<uint8_t>(b / 4u));
            }
        }
        tex.levels.push_back(std::move(dst));
    }
}

bool PulseGame::loadConfig(bool announce) {
    const ConfigLoadResult result = loadTunablesFromDisk(tunables_);
    if (result.loaded) {
        player_.hp = std::clamp(player_.hp, 0, std::max(1, tunables_.playerMaxHealth));
        player_.shield = std::clamp(player_.shield, 0, std::max(0, tunables_.playerMaxShield));
    }
    if (announce) {
        configMessageTimer_ = 2.0f;
        configMessage_ = result.loaded ? "CONFIG RELOADED" : "CONFIG MISSING";
    }
    return result.loaded;
}

void PulseGame::buildBotInput(InputState& input, float elapsedSeconds) const {
    input.keyDown.fill(false);
    input.mouseDown.fill(false);

    const Enemy* target = nullptr;
    float bestDistSq = 1000000.0f;
    for (const Enemy& enemy : enemies_) {
        if (!enemy.active || !lineOfSight(player_.pos, enemy.pos)) {
            continue;
        }
        const float distSq = lengthSq(enemy.pos - player_.pos);
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            target = &enemy;
        }
    }

    if (target) {
        const Vec2 toTarget = target->pos - player_.pos;
        // Aim so that (aim + recoil offset) lands on the target -> the bot drags
        // down/across to control the spray, exactly like a player would.
        const float desiredYaw = std::atan2(toTarget.y, toTarget.x) - recoilOffsetYaw_;
        const float yawError = angleDiff(desiredYaw, player_.yaw);
        const float desiredPitch = -0.015f - recoilOffsetPitch_;
        const float pitchError = player_.pitch - desiredPitch;
        const float sens = std::max(0.0001f, tunables_.mouseSensitivity);

        input.mouseDeltaX = static_cast<int>(clamp(yawError / sens, -90.0f, 90.0f));
        input.mouseDeltaY = static_cast<int>(clamp(pitchError / sens, -70.0f, 70.0f));
        input.mouseDown[0] = true;

        const float dist = std::sqrt(bestDistSq);
        if (dist > 7.0f) {
            input.keyDown['W'] = true;
        } else if (dist < 3.2f) {
            input.keyDown['S'] = true;
        }
        input.keyDown[(static_cast<int>(elapsedSeconds * 1.7f) & 1) ? 'A' : 'D'] = true;

        if (player_.dashCooldown <= 0.0f && dist < 6.0f) {
            input.keyPressed[VK_SHIFT] = true;
            input.keyDown[VK_SHIFT] = true;
        }
    } else {
        input.keyDown['W'] = true;
        input.keyDown[(static_cast<int>(elapsedSeconds * 0.8f) & 1) ? 'A' : 'D'] = true;
        input.mouseDeltaX = 24;
    }

    if (weapon_.ammo <= 5 && weapon_.reserve > 0 && !weapon_.reloading) {
        input.keyPressed['R'] = true;
        input.keyDown['R'] = true;
    }

    // Periodic hop so automated playtests exercise the jump/gravity path.
    const int frame = static_cast<int>(elapsedSeconds * 60.0f + 0.5f);
    if (frame % 90 == 0 && player_.grounded) {
        input.keyPressed[VK_SPACE] = true;
        input.keyDown[VK_SPACE] = true;
    }
}

void PulseGame::debugBeginScriptedCapture() {
    scriptedDeterministic_ = true;
    enemies_.clear();
    pickups_.clear();
    tracers_.clear();
    impacts_.clear();
    bursts_.clear();
    debris_.clear();
    spawnTimer_ = 100000.0f;
    pickupSpawnTimer_ = 100000.0f;
    tunables_.spawnMaxConcurrent = 0;
    tunables_.pickupMaxActive = 0;
    weapon_.ammo = 999;
    weapon_.reserve = 0;
    weapon_.timeSinceShot = 100.0f;
    weapon_.reloading = false;
    weapon_.reloadRemaining = 0.0f;
    combatIntensity_ = 0.0f;
    hitStopTimer_ = 0.0f;
}

void PulseGame::debugPose() {
    enemies_.clear();
    const Vec2 fwd = fromAngle(player_.yaw);
    const Vec2 rt = rightFromForward(fwd);
    Enemy a;
    a.kind = EnemyKind::Rusher;
    a.pos = player_.pos + fwd * 3.0f - rt * 1.9f;
    a.health = tunables_.enemyMaxHealth;
    enemies_.push_back(a);
    Enemy b;
    b.kind = EnemyKind::Ranged;
    b.pos = player_.pos + fwd * 3.4f + rt * 1.9f;
    b.health = tunables_.enemyMaxHealth;
    b.telegraphRemaining = 0.4f;
    enemies_.push_back(b);
    Enemy c;
    c.kind = EnemyKind::Tank;
    c.pos = player_.pos + fwd * 5.2f;
    c.health = tunables_.enemyMaxHealth * tunables_.enemyTankHealthMult;
    enemies_.push_back(c);
}

void PulseGame::debugKillAll() {
    for (Enemy& enemy : enemies_) {
        if (!enemy.active) {
            continue;
        }
        enemy.active = false;
        spawnBurst(enemy.pos, false);
        spawnDebris(enemy, false);
    }
}

void PulseGame::debugFire(AudioSystem& audio, float pitch) {
    enemies_.clear();
    player_.pitch = pitch;
    weapon_.timeSinceShot = 100.0f; // bypass the fire-rate gate
    tryFire(audio, tunables_.windowWidth, tunables_.windowHeight);
}

void PulseGame::debugRenderWeaponPreview(Renderer& renderer) {
    if (!ensureGpuResources()) {
        const std::string detail = gpuRenderer_ ? gpuRenderer_->lastError() : "GPU renderer unavailable";
        throw std::runtime_error("GPU weapon preview failed: " + detail);
    }

    GpuSceneFrame frame;
    frame.clearColor = rgb(8, 10, 14);
    frame.camera.position = {-3.18f, 0.46f, -4.22f};
    frame.camera.yaw = 0.93f;
    frame.camera.pitch = -0.07f;
    frame.camera.fovDegrees = 34.0f;
    frame.camera.nearZ = 0.04f;
    frame.camera.farZ = 20.0f;
    frame.light.direction = {-0.38f, -0.78f, -0.46f};
    frame.light.color = {1.18f, 1.22f, 1.30f, 1.0f};
    frame.light.ambient = 0.76f;
    frame.light.intensity = 1.45f;

    GpuDrawCommand weapon;
    weapon.mesh = gpuWeaponMesh_;
    weapon.material.texture = gpuWeaponTexture_;
    weapon.material.tint = {1.55f, 1.55f, 1.48f, 1.0f};
    weapon.material.emissive = 0.08f;
    weapon.cameraSpace = false;
    weapon.depthMode = GpuDepthMode::TestWrite;
    weapon.transform = GpuSceneRenderer::multiply(
        GpuSceneRenderer::scale(0.84f, 0.84f, 0.84f),
        GpuSceneRenderer::translation(0.0f, 0.0f, 0.0f));
    frame.draws.push_back(weapon);
    GpuDrawCommand leftHand = weapon;
    leftHand.mesh = gpuLeftHandMesh_;
    leftHand.material.texture = 0;
    leftHand.material.tint = {1.60f, 1.64f, 1.72f, 1.0f};
    leftHand.material.emissive = 0.030f;
    GpuDrawCommand rightHand = leftHand;
    rightHand.mesh = gpuRightHandMesh_;
    frame.draws.push_back(leftHand);
    frame.draws.push_back(rightHand);

    std::vector<uint32_t> pixels;
    if (!gpuRenderer_->renderToPixels(renderer.width(), renderer.height(), frame, pixels) || !renderer.replacePixels(pixels)) {
        const std::string detail = gpuRenderer_ ? gpuRenderer_->lastError() : "GPU renderer unavailable";
        throw std::runtime_error("GPU weapon preview failed: " + detail);
    }
}

int PulseGame::activeEnemyCount() const {
    return static_cast<int>(std::count_if(enemies_.begin(), enemies_.end(), [](const Enemy& enemy) {
        return enemy.active;
    }));
}

bool PulseGame::loadObjMesh(const std::string& relPath, MeshAsset& out) const {
    const std::string meshPath = resolveAsset(relPath);
    std::ifstream in;
    in.open(meshPath);
    if (!in) {
        return false;
    }

    MeshAsset loaded;
    std::unordered_map<std::string, ObjMaterialInfo> materials;
    ObjMaterialInfo activeMaterial;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2) {
            continue;
        }
        std::istringstream stream(line);
        std::string tag;
        stream >> tag;
        if (tag == "v") {
            MeshVertex v;
            stream >> v.x >> v.y >> v.z;
            if (stream) {
                loaded.vertices.push_back(v);
            }
        } else if (tag == "vn") {
            MeshNormal n;
            stream >> n.x >> n.y >> n.z;
            if (stream) {
                loaded.normals.push_back(n);
            }
        } else if (tag == "vt") {
            MeshUv uv;
            stream >> uv.u >> uv.v;
            if (stream) {
                loaded.uvs.push_back(uv);
            }
        } else if (tag == "mtllib") {
            std::string mtlName;
            stream >> mtlName;
            if (!mtlName.empty()) {
                materials = loadObjMaterials(meshPath, mtlName);
            }
        } else if (tag == "usemtl") {
            std::string materialName;
            stream >> materialName;
            const auto found = materials.find(materialName);
            activeMaterial = found == materials.end() ? ObjMaterialInfo{} : found->second;
        } else if (tag == "f") {
            std::vector<ObjIndex> indices;
            std::string token;
            while (stream >> token) {
                const ObjIndex index = parseObjIndexToken(
                    token,
                    static_cast<int>(loaded.vertices.size()),
                    static_cast<int>(loaded.uvs.size()),
                    static_cast<int>(loaded.normals.size()));
                if (index.vertex >= 0) {
                    indices.push_back(index);
                } else {
                    return false;
                }
            }
            if (indices.size() >= 3) {
                for (size_t i = 1; i + 1 < indices.size(); ++i) {
                    MeshTriangle tri;
                    tri.a = indices[0].vertex;
                    tri.b = indices[i].vertex;
                    tri.c = indices[i + 1].vertex;
                    tri.ta = indices[0].uv;
                    tri.tb = indices[i].uv;
                    tri.tc = indices[i + 1].uv;
                    tri.na = indices[0].normal;
                    tri.nb = indices[i].normal;
                    tri.nc = indices[i + 1].normal;
                    tri.color = activeMaterial.color;
                    tri.emissive = activeMaterial.emissive;
                    loaded.triangles.push_back(tri);
                }
            }
        }
    }

    if (loaded.vertices.empty() || loaded.triangles.empty()) {
        return false;
    }

    MeshVertex minV{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    MeshVertex maxV{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()
    };
    for (const MeshVertex& v : loaded.vertices) {
        minV.x = std::min(minV.x, v.x);
        minV.y = std::min(minV.y, v.y);
        minV.z = std::min(minV.z, v.z);
        maxV.x = std::max(maxV.x, v.x);
        maxV.y = std::max(maxV.y, v.y);
        maxV.z = std::max(maxV.z, v.z);
    }
    loaded.center = {
        (minV.x + maxV.x) * 0.5f,
        (minV.y + maxV.y) * 0.5f,
        (minV.z + maxV.z) * 0.5f
    };
    loaded.loaded = true;
    out = std::move(loaded);
    return true;
}

bool PulseGame::loadWeaponMesh() {
    MeshAsset loaded;
    if (!loadObjMesh("assets/models/pulse_carbine_viewmodel.obj", loaded)) {
        weaponMeshLoaded_ = false;
        return false;
    }

    MeshVertex minV{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    MeshVertex maxV{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()
    };
    // Track the frontmost (most negative Z) vertex among the upper half of the
    // mesh: that is the barrel muzzle used to align the viewmodel flash.
    MeshVertex muzzleV = loaded.vertices.front();
    float midY = 0.0f;
    for (const MeshVertex& v : loaded.vertices) {
        midY += v.y;
    }
    midY /= static_cast<float>(loaded.vertices.size());
    bool haveMuzzle = false;
    for (const MeshVertex& v : loaded.vertices) {
        minV.x = std::min(minV.x, v.x);
        minV.y = std::min(minV.y, v.y);
        minV.z = std::min(minV.z, v.z);
        maxV.x = std::max(maxV.x, v.x);
        maxV.y = std::max(maxV.y, v.y);
        maxV.z = std::max(maxV.z, v.z);
        if (v.y >= midY && (!haveMuzzle || v.z < muzzleV.z)) {
            muzzleV = v; // frontmost vertex in the upper (barrel) half
            haveMuzzle = true;
        }
    }
    weaponMuzzleVertex_ = muzzleV;

    weaponMeshVertices_ = std::move(loaded.vertices);
    weaponMeshUvs_ = std::move(loaded.uvs);
    weaponMeshNormals_ = std::move(loaded.normals);
    weaponMeshTriangles_ = std::move(loaded.triangles);
    weaponMeshCenter_ = loaded.center;
    weaponMeshMinZ_ = minV.z;
    weaponMeshLoaded_ = true;
    return true;
}

bool PulseGame::loadHandsMesh() {
    leftHandMesh_ = MeshAsset{};
    rightHandMesh_ = MeshAsset{};
    return loadObjMesh("assets/models/pulse_left_hand_viewmodel.obj", leftHandMesh_) &&
        loadObjMesh("assets/models/pulse_right_hand_viewmodel.obj", rightHandMesh_);
}

bool PulseGame::loadWeaponTexture() {
    weaponTextureWidth_ = 1;
    weaponTextureHeight_ = 1;
    weaponTexture_.assign(1, rgb(255, 255, 255));
    return true;
}

void PulseGame::resetRun() {
    bestScore_ = std::max(bestScore_, score_);
    score_ = 0;
    player_ = Player{};
    player_.hp = std::clamp(tunables_.playerStartHealth, 1, std::max(1, tunables_.playerMaxHealth));
    player_.shield = std::clamp(tunables_.playerStartShield, 0, std::max(0, tunables_.playerMaxShield));
    weapon_ = Weapon{};
    weapon_.ammo = std::max(1, tunables_.weaponMagazineCapacity);
    weapon_.reserve = std::max(0, tunables_.weaponReserveAmmo);
    enemies_.clear();
    pickups_.clear();
    tracers_.clear();
    impacts_.clear();
    bursts_.clear();
    debris_.clear();
    projectiles_.clear();
    damageMarkers_.clear();
    spawnTimer_ = 0.35f;
    pickupSpawnTimer_ = 2.5f;
    restartTimer_ = 0.0f;
    hitmarkerTimer_ = 0.0f;
    killConfirmTimer_ = 0.0f;
    damageFlashTimer_ = 0.0f;
    shieldFlashTimer_ = 0.0f;
    muzzleFlashTimer_ = 0.0f;
    fireFovKick_ = 0.0f;
    fireCameraKick_ = 0.0f;
    recoilPitch_ = 0.0f;
    recoilResidualPitch_ = 0.0f;
    recoilOffsetPitch_ = 0.0f;
    recoilOffsetYaw_ = 0.0f;
    recoilShotIndex_ = 0;
    cameraBobPhase_ = 0.0f;
    landingKick_ = 0.0f;
    strafeLean_ = 0.0f;
    combatIntensity_ = 0.0f;
    cameraShake_ = 0.0f;
    hitStopTimer_ = 0.0f;
}

void PulseGame::update(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH) {
    if (input.pressed(VK_F5)) {
        loadConfig(true);
        audio.play(SoundEventType::Config, 0.9f);
    }

    if (restartTimer_ > 0.0f) {
        restartTimer_ -= dt;
        if (restartTimer_ <= 0.0f) {
            resetRun();
        }
        updateTimers(dt);
        audio.setMusic(tunables_.technoEnabled, tunables_.technoBpm, tunables_.technoBaseVolume, combatIntensity_);
        return;
    }

    // Hit-stop: while the window is open, gameplay time is scaled down for a
    // brief crunch on kills, but feedback timers, shake and audio run on real
    // time (handled in updateTimers) so the freeze reads as a pop, not a stall.
    const float gameplayDt = hitStopTimer_ > 0.0f
        ? dt * clamp(tunables_.feelHitstopScale, 0.0f, 1.0f)
        : dt;

    updatePlayer(input, audio, gameplayDt);
    updateWeapon(input, audio, gameplayDt, screenW, screenH);
    updateEnemies(audio, gameplayDt);
    updateProjectiles(audio, gameplayDt);
    updateSpawning(gameplayDt);
    updatePickups(audio, gameplayDt);
    updateDebris(gameplayDt);
    updateTimers(dt);
    removeDeadEnemies();

    audio.setMusic(tunables_.technoEnabled, tunables_.technoBpm, tunables_.technoBaseVolume, combatIntensity_);
}

void PulseGame::updatePlayer(const InputState& input, AudioSystem& audio, float dt) {
    player_.yaw = wrapAngle(player_.yaw + static_cast<float>(input.mouseDeltaX) * tunables_.mouseSensitivity);
    player_.pitch -= static_cast<float>(input.mouseDeltaY) * tunables_.mouseSensitivity;
    const float pitchLimit = degToRad(tunables_.pitchLimitDegrees);
    player_.pitch = clamp(player_.pitch, -pitchLimit, pitchLimit);

    Vec2 localInput{};
    if (input.down('W')) localInput.y += 1.0f;
    if (input.down('S')) localInput.y -= 1.0f;
    if (input.down('D')) localInput.x += 1.0f;
    if (input.down('A')) localInput.x -= 1.0f;
    localInput = normalize(localInput);

    const Vec2 forward = fromAngle(player_.yaw);
    const Vec2 right = rightFromForward(forward);
    Vec2 desiredDir = forward * localInput.y + right * localInput.x;
    desiredDir = normalize(desiredDir);

    player_.dashCooldown = std::max(0.0f, player_.dashCooldown - dt);
    const bool groundedBeforeVertical = player_.grounded;

    if (input.pressed(VK_SHIFT) && player_.dashCooldown <= 0.0f && player_.dashTime <= 0.0f) {
        player_.dashDir = lengthSq(desiredDir) > 0.001f ? desiredDir : forward;
        player_.dashTime = std::max(0.01f, tunables_.dashDuration);
        player_.dashCooldown = std::max(0.0f, tunables_.dashCooldown);
        player_.vel = player_.dashDir * tunables_.dashImpulse;
        fireFovKick_ = std::max(fireFovKick_, tunables_.dashFovPunch);
        audio.play(SoundEventType::Dash, 0.95f);
    }

    if (player_.dashTime > 0.0f) {
        player_.dashTime -= dt;
        player_.vel = player_.dashDir * tunables_.dashImpulse;
    } else {
        const Vec2 desiredVel = desiredDir * tunables_.walkSpeed;
        if (lengthSq(desiredDir) > 0.001f) {
            const float airScale = player_.grounded ? 1.0f : clamp(tunables_.airControl, 0.0f, 1.0f);
            player_.vel = approach(player_.vel, desiredVel, tunables_.acceleration * airScale * dt);
        } else {
            player_.vel = approach(player_.vel, Vec2{}, tunables_.braking * dt);
        }
    }

    moveWithCollision(player_.pos, player_.vel, tunables_.playerRadius, dt);

    // Jump + gravity on the vertical axis (independent of floor-plane movement).
    const float eyeBase = clamp(tunables_.eyeHeight, 0.12f, 0.88f);
    if (player_.grounded) {
        player_.height = eyeBase;
        if (input.pressed(VK_SPACE) && restartTimer_ <= 0.0f) {
            player_.vz = std::max(0.0f, tunables_.jumpVelocity);
            player_.grounded = false;
            audio.play(SoundEventType::Dash, 0.5f); // soft launch whoosh
        }
    } else {
        player_.vz -= std::max(0.0f, tunables_.gravity) * dt;
        player_.height += player_.vz * dt;
        const float cap = std::min(0.92f, tunables_.jumpApexCap);
        if (player_.height > cap) {
            player_.height = cap;
            player_.vz = std::min(player_.vz, 0.0f);
        }
        if (player_.height <= eyeBase) { // landed
            player_.height = eyeBase;
            player_.vz = 0.0f;
            player_.grounded = true;
            if (!groundedBeforeVertical) {
                landingKick_ = std::max(landingKick_, degToRad(std::max(0.0f, tunables_.cameraLandingKickDegrees)));
            }
        }
    }

    const float planarSpeed = length(player_.vel);
    const float walk = std::max(0.1f, tunables_.walkSpeed);
    const float speed01 = clamp(planarSpeed / walk, 0.0f, 1.4f);
    if (player_.grounded && speed01 > 0.04f) {
        cameraBobPhase_ += dt * std::max(0.0f, tunables_.cameraBobSpeed) * (0.55f + speed01 * 0.45f);
    }
    const float strafeTarget = clamp(dot(player_.vel, right) / walk, -1.0f, 1.0f);
    strafeLean_ = approach(strafeLean_, strafeTarget, dt * 5.5f);
}

void PulseGame::updateWeapon(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH) {
    weapon_.timeSinceShot += dt;
    if (weapon_.reloading) {
        weapon_.reloadRemaining -= dt;
        if (weapon_.reloadRemaining <= 0.0f) {
            weapon_.reloading = false;
            const int missing = std::max(0, tunables_.weaponMagazineCapacity - weapon_.ammo);
            const int loaded = std::min(missing, weapon_.reserve);
            weapon_.ammo += loaded;
            weapon_.reserve -= loaded;
            audio.play(SoundEventType::ReloadEnd, 0.98f);
        }
    }

    const bool wantsFire = input.mouseDown[0];
    const bool firePressed = input.mousePressed[0];
    if ((tunables_.weaponAutoFire && wantsFire) || (!tunables_.weaponAutoFire && firePressed)) {
        tryFire(audio, screenW, screenH);
    }

    if (input.pressed('R') && !weapon_.reloading && weapon_.ammo < tunables_.weaponMagazineCapacity && weapon_.reserve > 0) {
        weapon_.reloading = true;
        weapon_.reloadRemaining = std::max(0.1f, tunables_.weaponReloadDuration);
        audio.play(SoundEventType::ReloadStart, 0.92f);
    }
}

void PulseGame::tryFire(AudioSystem& audio, int screenW, int screenH) {
    if (weapon_.reloading) {
        return;
    }

    const float minInterval = 1.0f / std::max(0.1f, tunables_.weaponFireRate);
    if (weapon_.timeSinceShot < minInterval) {
        return;
    }
    if (weapon_.timeSinceShot > 0.30f) {
        recoilShotIndex_ = 0; // fresh burst -> restart the spray pattern
    }
    weapon_.timeSinceShot = 0.0f;

    if (weapon_.ammo <= 0) {
        audio.play(SoundEventType::DryFire, 0.75f);
        if (weapon_.reserve > 0) {
            weapon_.reloading = true;
            weapon_.reloadRemaining = std::max(0.1f, tunables_.weaponReloadDuration);
            audio.play(SoundEventType::ReloadStart, 0.88f);
        }
        return;
    }

    --weapon_.ammo;
    audio.play(SoundEventType::Fire, 1.0f);
    muzzleFlashTimer_ = 0.09f;
    fireFovKick_ = std::max(fireFovKick_, tunables_.weaponFireFovPunch);
    fireCameraKick_ = std::max(fireCameraKick_, tunables_.weaponFireCameraKick);
    combatIntensity_ = std::min(1.0f, combatIntensity_ + 0.18f);
    addShake(tunables_.cameraShakeFire);

    const float spread = scriptedDeterministic_ ? 0.0f : degToRad(tunables_.weaponSpreadDegrees);
    // Bullets leave from the recoiled view centre (crosshair), so the spray
    // pattern actually pulls your shots off target until you compensate.
    const float aimYaw = player_.yaw + recoilOffsetYaw_;
    const float aimPitch = player_.pitch + recoilOffsetPitch_;
    const float shotYaw = scriptedDeterministic_ || spread <= 0.0f ? aimYaw : aimYaw + rng_.range(-spread, spread);
    const float shotPitch = scriptedDeterministic_ || spread <= 0.0f ? aimPitch : aimPitch + rng_.range(-spread, spread);

    bool headshot = false;
    const int targetIndex = acquireTarget(shotYaw, shotPitch, screenW, screenH, headshot);

    const Vec2 shotDir = fromAngle(shotYaw);
    const Vec2 shotRight = rightFromForward(shotDir);
    const Vec2 muzzle = player_.pos + shotDir * 0.44f + shotRight * 0.18f;
    const float muzzleHeight = clamp(player_.height - 0.11f, 0.08f, 0.92f);

    Tracer tracer;
    tracer.start = muzzle;
    tracer.startHeight = muzzleHeight;
    tracer.hit = targetIndex >= 0;
    tracer.duration = 0.085f; // hot AK streak with a little smoke life

    if (targetIndex >= 0 && targetIndex < static_cast<int>(enemies_.size())) {
        const Enemy& enemy = enemies_[static_cast<size_t>(targetIndex)];
        const float hover = (enemy.kind == EnemyKind::Tank ? 0.50f : 0.52f) +
            0.05f * std::sin(shakeTime_ * 2.6f + enemy.bobPhase);
        tracer.end = enemy.pos;
        tracer.endHeight = clamp(hover + (headshot ? 0.18f : 0.0f), 0.08f, 0.94f);
    } else {
        RayHit wallHit = castRay(muzzle, shotYaw, MaxRayDistance);
        float impactDistance = wallHit.distance;
        const float verticalSlope = std::tan(shotPitch);
        if (verticalSlope > 0.0001f) {
            const float ceilingDistance = (0.98f - muzzleHeight) / verticalSlope;
            if (ceilingDistance > 0.0f && ceilingDistance < impactDistance) {
                impactDistance = ceilingDistance;
            }
        } else if (verticalSlope < -0.0001f) {
            const float floorDistance = (0.02f - muzzleHeight) / verticalSlope;
            if (floorDistance > 0.0f && floorDistance < impactDistance) {
                impactDistance = floorDistance;
            }
        }
        impactDistance = clamp(impactDistance, 0.25f, MaxRayDistance);
        tracer.end = muzzle + shotDir * impactDistance;
        tracer.endHeight = clamp(muzzleHeight + verticalSlope * impactDistance, 0.02f, 0.98f);
    }
    tracers_.push_back(tracer);
    spawnImpact(tracer.end, tracer.endHeight, tracer.hit);

    if (targetIndex >= 0 && targetIndex < static_cast<int>(enemies_.size())) {
        Enemy& enemy = enemies_[static_cast<size_t>(targetIndex)];
        const float damage = headshot ? tunables_.enemyMaxHealth : tunables_.weaponDamage;
        enemy.health -= damage;
        enemy.hurtTimer = 0.08f;
        enemy.hitPunch = 1.0f;
        const Vec2 away = normalize(enemy.pos - player_.pos);
        enemy.vel = enemy.vel + away * tunables_.feelHitKnockback;
        hitmarkerTimer_ = 0.16f;
        addShake(0.16f); // tactile pop when your shot connects
        audio.play(SoundEventType::Hit, headshot ? 1.0f : 0.8f);
        if (enemy.health <= 0.0f) {
            enemy.active = false;
            ++score_;
            killConfirmTimer_ = 0.25f;
            combatIntensity_ = 1.0f;
            hitStopTimer_ = std::max(hitStopTimer_, tunables_.feelHitstopKill);
            addShake(tunables_.cameraShakeKill);
            spawnBurst(enemy.pos, headshot);
            spawnDebris(enemy, headshot);
            audio.play(SoundEventType::Kill, headshot ? 1.05f : 0.9f);
        }
    }

    // Deterministic AK-47 spray pattern: climbs hard, drifts left, then hooks
    // right. Same every burst, so it is learnable and you fight it by dragging
    // the mouse down/across. The offset moves the view + shot, not the aim.
    if (!scriptedDeterministic_) {
        static const float kAkUp[] = {
            1.00f, 1.18f, 1.06f, 0.94f, 0.84f, 0.70f, 0.56f, 0.46f, 0.40f, 0.36f,
            0.32f, 0.30f, 0.28f, 0.26f, 0.24f, 0.24f, 0.22f, 0.22f, 0.20f, 0.20f
        };
        static const float kAkSide[] = {
            0.05f, -0.06f, -0.14f, -0.24f, -0.38f, -0.52f, -0.56f, -0.44f, -0.16f, 0.22f,
            0.50f, 0.68f, 0.58f, 0.36f, 0.10f, -0.30f, -0.50f, -0.40f, 0.18f, 0.48f
        };
        constexpr int kPatternLen = static_cast<int>(sizeof(kAkUp) / sizeof(kAkUp[0]));
        const float vStrength = std::max(0.0f, tunables_.weaponRecoilPitchDegrees);
        const float hStrength = std::max(0.0f, tunables_.weaponRecoilYawJitterDegrees);
        const int pidx = std::min(recoilShotIndex_, kPatternLen - 1);
        recoilOffsetPitch_ = std::min(recoilOffsetPitch_ + degToRad(kAkUp[pidx] * vStrength), degToRad(30.0f));
        recoilOffsetYaw_ = wrapAngle(recoilOffsetYaw_ + degToRad(kAkSide[pidx] * hStrength));
        ++recoilShotIndex_;
    }

    if (weapon_.ammo <= 0 && weapon_.reserve > 0) {
        weapon_.reloading = true;
        weapon_.reloadRemaining = std::max(0.1f, tunables_.weaponReloadDuration);
        audio.play(SoundEventType::ReloadStart, 0.88f);
    }
}

int PulseGame::acquireTarget(float shotYaw, float shotPitch, int screenW, int screenH, bool& outHeadshot) const {
    outHeadshot = false;
    int best = -1;
    float bestDepth = 100000.0f;

    for (int i = 0; i < static_cast<int>(enemies_.size()); ++i) {
        const Enemy& enemy = enemies_[static_cast<size_t>(i)];
        if (!enemy.active) {
            continue;
        }
        const Projection p = projectEnemy(enemy, shotYaw, shotPitch, screenW, screenH);
        if (!p.visible) {
            continue;
        }
        const int cx = screenW / 2;
        const int cy = screenH / 2;
        if (cx < p.left || cx > p.right || cy < p.top || cy > p.bottom) {
            continue;
        }
        if (!lineOfSight(player_.pos, enemy.pos)) {
            continue;
        }
        if (p.depth < bestDepth) {
            bestDepth = p.depth;
            best = i;
            const float headLine = static_cast<float>(p.top) + static_cast<float>(p.bottom - p.top) * tunables_.enemyHeadshotMinFraction;
            outHeadshot = static_cast<float>(cy) <= headLine;
        }
    }

    return best;
}

// Soft boid separation: push away from nearby drones so the swarm spreads out
// and each enemy stays individually readable instead of stacking into a blob.
Vec2 PulseGame::separationForce(const Enemy& self) const {
    const float radius = std::max(0.1f, tunables_.enemySeparationRadius);
    const float strength = std::max(0.0f, tunables_.enemySeparationStrength);
    Vec2 push{};
    for (const Enemy& other : enemies_) {
        if (&other == &self || !other.active) {
            continue;
        }
        const Vec2 delta = self.pos - other.pos;
        const float d = length(delta);
        if (d <= 0.0001f || d > radius) {
            continue;
        }
        push += (delta / d) * ((radius - d) / radius);
    }
    return push * strength;
}

void PulseGame::updateEnemies(AudioSystem& audio, float dt) {
    for (Enemy& enemy : enemies_) {
        if (!enemy.active) {
            continue;
        }

        enemy.hurtTimer = std::max(0.0f, enemy.hurtTimer - dt);
        enemy.hitPunch = std::max(0.0f, enemy.hitPunch - dt * 5.0f);
        enemy.recover = std::max(0.0f, enemy.recover - dt);
        enemy.attackCooldown -= dt;

        const Vec2 toPlayer = player_.pos - enemy.pos;
        const float dist = std::max(0.001f, length(toPlayer));
        const Vec2 dir = toPlayer / dist;
        const bool hasLos = lineOfSight(enemy.pos, player_.pos);
        const Vec2 sep = separationForce(enemy);
        const float radius = enemy.kind == EnemyKind::Tank ? 0.34f : 0.27f;
        const float meleeRange = std::max(0.4f, tunables_.enemyMeleeRange);

        // Post-attack recovery: a vulnerable pause, the player's counter window.
        if (enemy.recover > 0.0f) {
            enemy.vel = approach(enemy.vel, sep, 9.0f * dt);
            moveWithCollision(enemy.pos, enemy.vel, radius, dt);
            continue;
        }

        // Rusher committed lunge: a locked, dodgeable dash that strikes on contact.
        if (enemy.lungeTime > 0.0f) {
            enemy.lungeTime -= dt;
            // Weak homing only, so a sideways dash still slips the lunge.
            enemy.vel = approach(enemy.vel, dir * tunables_.enemyRusherLungeSpeed + sep, 6.0f * dt);
            if (!enemy.struck && dist <= meleeRange) {
                enemy.struck = true;
                enemy.lungeTime = 0.0f;
                enemy.recover = std::max(0.2f, tunables_.enemyMeleeRecover);
                enemy.vel = dir * -2.0f; // shove back on impact for readability
                damagePlayer(audio, tunables_.enemyMeleeDamage, enemy.pos);
            } else if (enemy.lungeTime <= 0.0f) {
                enemy.recover = std::max(0.2f, tunables_.enemyMeleeRecover * 0.7f); // whiff
            }
            moveWithCollision(enemy.pos, enemy.vel, radius, dt);
            continue;
        }

        // Attack wind-up: brace and flare (telegraphRemaining drives the glow), then
        // resolve. The wind-up is the always-visible tell before any damage lands.
        if (enemy.telegraphRemaining > 0.0f) {
            enemy.telegraphRemaining -= dt;
            enemy.vel = approach(enemy.vel, sep, 20.0f * dt); // plant feet
            if (enemy.telegraphRemaining <= 0.0f) {
                enemy.telegraphRemaining = 0.0f;
                if (enemy.kind == EnemyKind::Ranged) {
                    if (hasLos) {
                        const float travel = dist / std::max(1.0f, tunables_.enemyProjectileSpeed);
                        const Vec2 aim = player_.pos + player_.vel * (travel * 0.5f); // partial lead
                        spawnProjectile(enemy.pos, 0.52f, aim - enemy.pos, tunables_.enemyRangedDamage);
                        audio.play(SoundEventType::Hit, 0.35f); // enemy shot cue
                    }
                    enemy.attackCooldown = std::max(0.3f, tunables_.enemyRangedCooldown);
                    enemy.recover = 0.35f;
                } else if (enemy.kind == EnemyKind::Rusher) {
                    enemy.struck = false;
                    enemy.lungeTime = 0.35f;
                    enemy.vel = dir * tunables_.enemyRusherLungeSpeed; // launch the dash
                } else { // Tank slam
                    if (dist <= meleeRange * 1.25f && hasLos) {
                        enemy.vel = dir * (tunables_.enemyMeleeLunge * 5.0f);
                        damagePlayer(audio, tunables_.enemyTankMeleeDamage, enemy.pos);
                    }
                    enemy.recover = std::max(0.3f, tunables_.enemyMeleeRecover * 1.2f);
                }
            }
            moveWithCollision(enemy.pos, enemy.vel, radius, dt);
            continue;
        }

        // Locomotion + decide whether to begin an attack wind-up.
        if (enemy.kind == EnemyKind::Ranged) {
            // Kite to a stand-off band and strafe within it (stable per-enemy side).
            const float nearBand = 6.0f;
            const float farBand = 9.0f;
            Vec2 desired{};
            if (dist < nearBand) {
                desired = dir * -1.0f;
            } else if (dist > farBand) {
                desired = dir;
            } else {
                const float side = std::sin(enemy.bobPhase) >= 0.0f ? 1.0f : -1.0f;
                desired = rightFromForward(dir) * side;
            }
            enemy.vel = approach(enemy.vel, normalize(desired) * tunables_.enemyRangedSpeed + sep, 10.0f * dt);
            if (enemy.attackCooldown <= 0.0f && hasLos) {
                enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyRangedTelegraph);
            }
        } else if (enemy.kind == EnemyKind::Rusher) {
            enemy.vel = approach(enemy.vel, dir * tunables_.enemyRusherSpeed + sep, 18.0f * dt);
            if (dist <= tunables_.enemyRusherLungeRange && hasLos) {
                enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyMeleeTelegraph); // wind up the lunge
            }
        } else { // Tank: slow relentless advance, prioritisation threat.
            enemy.vel = approach(enemy.vel, dir * tunables_.enemyTankSpeed + sep, 7.0f * dt);
            if (dist <= meleeRange && hasLos) {
                enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyMeleeTelegraph * 1.3f); // heavier slam tell
            }
        }
        moveWithCollision(enemy.pos, enemy.vel, radius, dt);
    }
}

void PulseGame::spawnProjectile(Vec2 from, float fromHeight, Vec2 dir, int damage) {
    const Vec2 unit = normalize(dir);
    Projectile p;
    p.origin = from;
    p.pos = from + unit * 0.45f; // emerge just ahead of the muzzle
    p.vel = unit * std::max(1.0f, tunables_.enemyProjectileSpeed);
    p.height = clamp(fromHeight, 0.1f, 0.9f);
    p.life = std::max(0.5f, tunables_.enemyProjectileLife);
    p.damage = damage;
    projectiles_.push_back(p);
    if (projectiles_.size() > 64) {
        projectiles_.erase(projectiles_.begin());
    }
}

void PulseGame::updateProjectiles(AudioSystem& audio, float dt) {
    const float pr = std::max(0.05f, tunables_.enemyProjectileRadius);
    const float hitDist = pr + tunables_.playerRadius;
    for (Projectile& p : projectiles_) {
        if (!p.active) {
            continue;
        }
        p.age += dt;
        p.pos += p.vel * dt;
        if (p.age >= p.life) {
            p.active = false;
            continue;
        }
        if (collides(p.pos, pr * 0.5f)) { // splash on a wall
            p.active = false;
            spawnImpact(p.pos, p.height, false);
            continue;
        }
        if (restartTimer_ <= 0.0f && lengthSq(p.pos - player_.pos) <= hitDist * hitDist) {
            p.active = false;
            spawnImpact(p.pos, p.height, true);
            damagePlayer(audio, p.damage, p.origin);
        }
    }
    projectiles_.erase(
        std::remove_if(projectiles_.begin(), projectiles_.end(), [](const Projectile& p) {
            return !p.active;
        }),
        projectiles_.end());
}

void PulseGame::addDamageMarker(Vec2 source, float intensity) {
    const Vec2 rel = source - player_.pos;
    DamageMarker marker;
    marker.worldAngle = lengthSq(rel) > 0.0001f ? std::atan2(rel.y, rel.x) : player_.yaw;
    marker.life = std::max(0.2f, tunables_.damageIndicatorSeconds);
    marker.intensity = clamp(intensity, 0.3f, 1.0f);
    damageMarkers_.push_back(marker);
    if (damageMarkers_.size() > 8) {
        damageMarkers_.erase(damageMarkers_.begin());
    }
}

void PulseGame::damagePlayer(AudioSystem& audio, int amount, Vec2 source) {
    if (restartTimer_ > 0.0f) {
        return;
    }
    // Always log the direction, even on a fully-absorbed hit, so the player can
    // read where pressure is coming from.
    addDamageMarker(source, clamp(static_cast<float>(amount) / 45.0f, 0.4f, 1.0f));
    int remaining = std::max(0, amount);
    if (player_.shield > 0 && remaining > 0) {
        const int absorbed = std::min(player_.shield, remaining);
        player_.shield -= absorbed;
        remaining -= absorbed;
        shieldFlashTimer_ = 0.24f;
    }
    if (remaining > 0) {
        player_.hp = std::max(0, player_.hp - remaining);
        damageFlashTimer_ = 0.28f;
    }
    combatIntensity_ = 1.0f;
    addShake(tunables_.cameraShakeHurt);
    audio.play(SoundEventType::Hurt, remaining > 0 ? 0.9f : 0.55f);
    if (player_.hp <= 0) {
        restartTimer_ = 0.75f;
        bestScore_ = std::max(bestScore_, score_);
    }
}

void PulseGame::updateSpawning(float dt) {
    spawnTimer_ -= dt;
    const int active = static_cast<int>(std::count_if(enemies_.begin(), enemies_.end(), [](const Enemy& e) {
        return e.active;
    }));
    if (spawnTimer_ <= 0.0f && active < tunables_.spawnMaxConcurrent) {
        spawnEnemy();
        spawnTimer_ = std::max(0.15f, tunables_.spawnInterval);
    }
}

void PulseGame::updatePickups(AudioSystem& audio, float dt) {
    for (Pickup& pickup : pickups_) {
        pickup.age += dt;
    }

    const int maxHealth = std::max(1, tunables_.playerMaxHealth);
    const int maxShield = std::max(0, tunables_.playerMaxShield);
    const int maxReserve = std::max(1, tunables_.weaponReserveAmmo);
    const float collectRadiusSq = std::max(0.05f, tunables_.pickupCollectRadius) * std::max(0.05f, tunables_.pickupCollectRadius);

    for (Pickup& pickup : pickups_) {
        if (lengthSq(pickup.pos - player_.pos) > collectRadiusSq) {
            continue;
        }

        bool collected = false;
        if (pickup.kind == PickupKind::Health && player_.hp < maxHealth) {
            player_.hp = std::min(maxHealth, player_.hp + std::max(1, tunables_.pickupHealthAmount));
            damageFlashTimer_ = 0.0f;
            collected = true;
        } else if (pickup.kind == PickupKind::Shield && player_.shield < maxShield) {
            player_.shield = std::min(maxShield, player_.shield + std::max(1, tunables_.pickupShieldAmount));
            shieldFlashTimer_ = 0.28f;
            collected = true;
        } else if (pickup.kind == PickupKind::Ammo && weapon_.reserve < maxReserve) {
            weapon_.reserve = std::min(maxReserve, weapon_.reserve + std::max(1, tunables_.pickupAmmoAmount));
            collected = true;
        }

        if (collected) {
            pickup.age = -1.0f;
            combatIntensity_ = std::min(1.0f, combatIntensity_ + 0.08f);
            audio.play(SoundEventType::Pickup, pickup.kind == PickupKind::Shield ? 0.85f : 0.75f);
        }
    }

    pickups_.erase(
        std::remove_if(pickups_.begin(), pickups_.end(), [](const Pickup& pickup) {
            return pickup.age < 0.0f;
        }),
        pickups_.end());

    if (scriptedDeterministic_) {
        return;
    }

    pickupSpawnTimer_ -= dt;
    const int maxActive = std::max(0, tunables_.pickupMaxActive);
    if (pickupSpawnTimer_ <= 0.0f && static_cast<int>(pickups_.size()) < maxActive) {
        const bool needsHealth = player_.hp <= maxHealth - std::max(8, tunables_.pickupHealthAmount / 2);
        const bool needsShield = player_.shield <= maxShield - std::max(8, tunables_.pickupShieldAmount / 2);
        const bool needsAmmo = (weapon_.reserve + weapon_.ammo) <= maxReserve / 2;
        const float roll = rng_.unit();
        PickupKind kind;
        if (needsAmmo && roll < 0.55f) {
            kind = PickupKind::Ammo; // running low -> keep you in the fight
        } else if (needsHealth && roll < 0.75f) {
            kind = PickupKind::Health;
        } else if (needsShield) {
            kind = PickupKind::Shield;
        } else {
            // Nothing urgent: favour ammo so you can keep shooting, then top-ups.
            kind = roll < 0.5f ? PickupKind::Ammo : (roll < 0.78f ? PickupKind::Health : PickupKind::Shield);
        }
        spawnPickup(kind);
        pickupSpawnTimer_ = std::max(1.0f, tunables_.pickupSpawnInterval) * rng_.range(0.82f, 1.18f);
    }
}

void PulseGame::spawnEnemy() {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const float angle = rng_.range(-Pi, Pi);
        const float radius = rng_.range(tunables_.spawnRingRadius * 0.75f, tunables_.spawnRingRadius);
        const Vec2 candidate = player_.pos + fromAngle(angle) * radius;
        if (collides(candidate, 0.5f) || !lineOfSight(candidate, player_.pos)) {
            continue;
        }

        Enemy enemy;
        const float roll = rng_.unit();
        if (roll < tunables_.spawnTankChance) {
            enemy.kind = EnemyKind::Tank;
        } else if (roll < tunables_.spawnTankChance + tunables_.spawnRangedChance) {
            enemy.kind = EnemyKind::Ranged;
        } else {
            enemy.kind = EnemyKind::Rusher;
        }
        enemy.pos = candidate;
        enemy.health = tunables_.enemyMaxHealth *
            (enemy.kind == EnemyKind::Tank ? std::max(1.0f, tunables_.enemyTankHealthMult) : 1.0f);
        enemy.attackCooldown = rng_.range(0.4f, 1.4f);
        enemy.bobPhase = rng_.range(0.0f, 6.28318f);
        enemies_.push_back(enemy);
        return;
    }
}

void PulseGame::spawnPickup(PickupKind kind) {
    const int rows = static_cast<int>(Arena.size());
    const int cols = static_cast<int>(Arena[0].size());
    for (int attempt = 0; attempt < 64; ++attempt) {
        const int x = rng_.rangeInt(1, cols - 2);
        const int y = rng_.rangeInt(1, rows - 2);
        Vec2 candidate{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
        if (collides(candidate, 0.34f) || lengthSq(candidate - player_.pos) < 9.0f) {
            continue;
        }
        bool crowded = false;
        for (const Pickup& pickup : pickups_) {
            if (lengthSq(pickup.pos - candidate) < 2.25f) {
                crowded = true;
                break;
            }
        }
        if (crowded) {
            continue;
        }
        for (const Enemy& enemy : enemies_) {
            if (enemy.active && lengthSq(enemy.pos - candidate) < 2.25f) {
                crowded = true;
                break;
            }
        }
        if (crowded) {
            continue;
        }

        Pickup pickup;
        pickup.kind = kind;
        pickup.pos = candidate;
        pickup.phase = rng_.range(0.0f, TwoPi);
        pickups_.push_back(pickup);
        return;
    }
}

void PulseGame::removeDeadEnemies() {
    enemies_.erase(
        std::remove_if(enemies_.begin(), enemies_.end(), [](const Enemy& enemy) {
            return !enemy.active;
        }),
        enemies_.end());
}

void PulseGame::addShake(float degrees) {
    cameraShake_ = std::min(8.0f, cameraShake_ + std::max(0.0f, degrees));
}

void PulseGame::spawnBurst(Vec2 pos, bool headshot) {
    Burst burst;
    burst.pos = pos;
    burst.duration = std::max(0.05f, tunables_.feelKillBurstSeconds);
    burst.headshot = headshot;
    bursts_.push_back(burst);
}

void PulseGame::spawnImpact(Vec2 pos, float height, bool hit) {
    Impact impact;
    impact.pos = pos;
    impact.height = clamp(height, 0.02f, 0.98f);
    impact.hit = hit;
    impact.duration = hit ? std::max(0.05f, tunables_.feelImpactHitSeconds)
                          : std::max(0.05f, tunables_.feelImpactWallSeconds);
    impacts_.push_back(impact);
    if (impacts_.size() > 96) {
        impacts_.erase(impacts_.begin(), impacts_.begin() + static_cast<long long>(impacts_.size() - 96));
    }
}

void PulseGame::spawnDebris(const Enemy& enemy, bool headshot) {
    const EnemyStyle style = styleFor(enemy.kind);
    const float scale = style.scale;
    const float hover = 0.52f;
    const uint32_t bodyCol = style.body;
    const uint32_t wingCol = style.wing;
    const uint32_t eyeCol = style.eye;
    const float speed = std::max(0.5f, tunables_.feelDebrisSpeed) * (headshot ? 1.35f : 1.0f);

    const Vec2 toP = player_.pos - enemy.pos;
    const float facing = std::atan2(toP.y, toP.x);
    const float cf = std::cos(facing);
    const float sf = std::sin(facing);

    for (const MeshTri3& tri : enemyMeshes_[static_cast<size_t>(enemy.kind)]) {
        Debris d;
        d.pos = enemy.pos;
        d.height = hover;
        const float cxl = (tri.vx[0] + tri.vx[1] + tri.vx[2]) / 3.0f;
        const float cyl = (tri.vy[0] + tri.vy[1] + tri.vy[2]) / 3.0f;
        const float czl = (tri.vz[0] + tri.vz[1] + tri.vz[2]) / 3.0f;
        const Vec2 outLocal = normalize(Vec2{cxl, czl});
        const Vec2 worldOut{outLocal.x * cf - outLocal.y * sf, outLocal.x * sf + outLocal.y * cf};
        d.vel = worldOut * (speed * (0.6f + rng_.unit() * 0.9f)) + Vec2{rng_.range(-1.2f, 1.2f), rng_.range(-1.2f, 1.2f)};
        d.vh = 1.6f + cyl * 4.5f + rng_.range(0.0f, 2.8f);
        d.yaw = facing;
        d.spin = rng_.range(-9.0f, 9.0f);
        for (int i = 0; i < 3; ++i) {
            d.vx[i] = tri.vx[i] * scale;
            d.vy[i] = tri.vy[i] * scale;
            d.vz[i] = tri.vz[i] * scale;
        }
        d.color = tri.part == 1 ? eyeCol : (tri.part == 2 ? wingCol : bodyCol);
        d.age = 0.0f;
        d.life = 0.55f + rng_.range(0.0f, 0.45f);
        debris_.push_back(d);
    }
    if (debris_.size() > 600) {
        debris_.erase(debris_.begin(), debris_.begin() + static_cast<long long>(debris_.size() - 600));
    }
}

void PulseGame::updateDebris(float dt) {
    const float gravity = 9.5f;
    for (Debris& d : debris_) {
        d.age += dt;
        d.pos = d.pos + d.vel * dt;
        d.height += d.vh * dt;
        d.vh -= gravity * dt;
        d.vel = d.vel * (1.0f - std::min(1.0f, 1.1f * dt));
        d.yaw += d.spin * dt;
        if (d.height < 0.04f) { // settle / bounce on the floor
            d.height = 0.04f;
            d.vh = -d.vh * 0.35f;
            d.vel = d.vel * 0.6f;
            if (std::fabs(d.vh) < 0.4f) {
                d.vh = 0.0f;
            }
        }
    }
    debris_.erase(
        std::remove_if(debris_.begin(), debris_.end(), [](const Debris& d) { return d.age >= d.life; }),
        debris_.end());
}

void PulseGame::updateTimers(float dt) {
    hitmarkerTimer_ = std::max(0.0f, hitmarkerTimer_ - dt);
    killConfirmTimer_ = std::max(0.0f, killConfirmTimer_ - dt);
    damageFlashTimer_ = std::max(0.0f, damageFlashTimer_ - dt);
    shieldFlashTimer_ = std::max(0.0f, shieldFlashTimer_ - dt);
    muzzleFlashTimer_ = std::max(0.0f, muzzleFlashTimer_ - dt);
    configMessageTimer_ = std::max(0.0f, configMessageTimer_ - dt);
    combatIntensity_ = std::max(0.0f, combatIntensity_ - dt * 0.28f);

    // Spray offset recovers (view eases back to aim) once you stop firing; while
    // the trigger is held it persists so the pattern reads and stays counter-able.
    const float releaseGap = 1.6f / std::max(0.1f, tunables_.weaponFireRate);
    if (weapon_.timeSinceShot > releaseGap) {
        const float settle = clamp(1.0f - std::exp(-std::max(0.0f, tunables_.weaponRecoilRecoveryRate) * dt), 0.0f, 1.0f);
        recoilOffsetPitch_ -= recoilOffsetPitch_ * settle;
        recoilOffsetYaw_ -= recoilOffsetYaw_ * settle;
        if (std::fabs(recoilOffsetPitch_) < 0.0001f) recoilOffsetPitch_ = 0.0f;
        if (std::fabs(recoilOffsetYaw_) < 0.0001f) recoilOffsetYaw_ = 0.0f;
        if (weapon_.timeSinceShot > 0.30f) recoilShotIndex_ = 0;
    }
    landingKick_ *= std::exp(-16.0f * dt);
    if (landingKick_ < 0.00001f) {
        landingKick_ = 0.0f;
    }

    // Shake runs on real time so the freeze of a hit-stop does not also freeze
    // the shake/feedback. Energy settles exponentially toward zero.
    hitStopTimer_ = std::max(0.0f, hitStopTimer_ - dt);
    shakeTime_ += dt;
    cameraShake_ *= std::exp(-std::max(0.0f, tunables_.cameraShakeDecay) * dt);
    if (cameraShake_ < 0.0005f) {
        cameraShake_ = 0.0f;
    }

    for (Burst& burst : bursts_) {
        burst.age += dt;
    }
    bursts_.erase(
        std::remove_if(bursts_.begin(), bursts_.end(), [](const Burst& burst) {
            return burst.age >= burst.duration;
        }),
        bursts_.end());

    for (Impact& impact : impacts_) {
        impact.age += dt;
    }
    impacts_.erase(
        std::remove_if(impacts_.begin(), impacts_.end(), [](const Impact& impact) {
            return impact.age >= impact.duration;
        }),
        impacts_.end());

    const float recovery = std::max(0.5f, tunables_.fireImpactRecovery) * dt;
    fireFovKick_ = approach(fireFovKick_, 0.0f, recovery * std::max(1.0f, fireFovKick_));
    fireCameraKick_ = approach(fireCameraKick_, 0.0f, recovery * std::max(1.0f, fireCameraKick_));

    for (Tracer& tracer : tracers_) {
        tracer.age += dt;
    }
    tracers_.erase(
        std::remove_if(tracers_.begin(), tracers_.end(), [](const Tracer& tracer) {
            return tracer.age >= tracer.duration;
        }),
        tracers_.end());

    // Directional damage wedges fade on real time so the hit cue persists even
    // through a hit-stop freeze.
    for (DamageMarker& marker : damageMarkers_) {
        marker.age += dt;
    }
    damageMarkers_.erase(
        std::remove_if(damageMarkers_.begin(), damageMarkers_.end(), [](const DamageMarker& marker) {
            return marker.age >= marker.life;
        }),
        damageMarkers_.end());
}

bool PulseGame::isWallCell(int x, int y) const {
    if (y < 0 || y >= static_cast<int>(Arena.size())) {
        return true;
    }
    if (x < 0 || x >= static_cast<int>(Arena[static_cast<size_t>(y)].size())) {
        return true;
    }
    return Arena[static_cast<size_t>(y)][static_cast<size_t>(x)] == '#';
}

bool PulseGame::collides(Vec2 pos, float radius) const {
    return isWallCell(static_cast<int>(std::floor(pos.x - radius)), static_cast<int>(std::floor(pos.y - radius))) ||
           isWallCell(static_cast<int>(std::floor(pos.x + radius)), static_cast<int>(std::floor(pos.y - radius))) ||
           isWallCell(static_cast<int>(std::floor(pos.x - radius)), static_cast<int>(std::floor(pos.y + radius))) ||
           isWallCell(static_cast<int>(std::floor(pos.x + radius)), static_cast<int>(std::floor(pos.y + radius)));
}

void PulseGame::moveWithCollision(Vec2& pos, Vec2& vel, float radius, float dt) const {
    Vec2 next = pos;
    next.x += vel.x * dt;
    if (!collides(next, radius)) {
        pos.x = next.x;
    } else {
        vel.x = 0.0f;
    }

    next = pos;
    next.y += vel.y * dt;
    if (!collides(next, radius)) {
        pos.y = next.y;
    } else {
        vel.y = 0.0f;
    }
}

bool PulseGame::lineOfSight(Vec2 from, Vec2 to) const {
    const Vec2 delta = to - from;
    const float dist = length(delta);
    if (dist <= 0.01f) {
        return true;
    }
    const float angle = std::atan2(delta.y, delta.x);
    return castRay(from, angle, dist).distance >= dist - 0.25f;
}

PulseGame::RayHit PulseGame::castRay(Vec2 origin, float angle, float maxDistance) const {
    const Vec2 rayDir = fromAngle(angle);
    int mapX = static_cast<int>(std::floor(origin.x));
    int mapY = static_cast<int>(std::floor(origin.y));

    const float deltaDistX = std::fabs(rayDir.x) < 0.00001f ? 1.0e30f : std::fabs(1.0f / rayDir.x);
    const float deltaDistY = std::fabs(rayDir.y) < 0.00001f ? 1.0e30f : std::fabs(1.0f / rayDir.y);

    int stepX = 0;
    int stepY = 0;
    float sideDistX = 0.0f;
    float sideDistY = 0.0f;

    if (rayDir.x < 0.0f) {
        stepX = -1;
        sideDistX = (origin.x - static_cast<float>(mapX)) * deltaDistX;
    } else {
        stepX = 1;
        sideDistX = (static_cast<float>(mapX) + 1.0f - origin.x) * deltaDistX;
    }

    if (rayDir.y < 0.0f) {
        stepY = -1;
        sideDistY = (origin.y - static_cast<float>(mapY)) * deltaDistY;
    } else {
        stepY = 1;
        sideDistY = (static_cast<float>(mapY) + 1.0f - origin.y) * deltaDistY;
    }

    RayHit hit;
    float distance = 0.0f;
    while (distance < maxDistance) {
        int side = 0;
        if (sideDistX < sideDistY) {
            sideDistX += deltaDistX;
            mapX += stepX;
            side = 0;
            distance = sideDistX - deltaDistX;
        } else {
            sideDistY += deltaDistY;
            mapY += stepY;
            side = 1;
            distance = sideDistY - deltaDistY;
        }

        if (isWallCell(mapX, mapY)) {
            hit.distance = std::max(0.001f, distance);
            hit.side = side;
            // Exact hit point along the wall face gives the texture U coordinate.
            const float wallX = side == 0
                ? origin.y + hit.distance * rayDir.y
                : origin.x + hit.distance * rayDir.x;
            hit.wallX = wallX - std::floor(wallX);
            const int rows = static_cast<int>(Arena.size());
            const int cols = static_cast<int>(Arena[0].size());
            if (mapY >= 0 && mapY < rows && mapX >= 0 && mapX < cols) {
                hit.cell = Arena[static_cast<size_t>(mapY)][static_cast<size_t>(mapX)];
            }
            // Interior obstacles get the accent texture; the outer shell stays plain.
            hit.cover = mapX > 0 && mapY > 0 && mapX < cols - 1 && mapY < rows - 1;
            return hit;
        }
    }

    hit.distance = maxDistance;
    return hit;
}

PulseGame::Projection PulseGame::projectEnemy(const Enemy& enemy, float yaw, float pitch, int screenW, int screenH) const {
    const float radius = enemy.kind == EnemyKind::Tank ? 0.66f
                       : (enemy.kind == EnemyKind::Ranged ? 0.42f : 0.48f);
    return projectPoint(enemy.pos, yaw, pitch, screenW, screenH, radius);
}

PulseGame::Projection PulseGame::projectPoint(Vec2 point, float yaw, float pitch, int screenW, int screenH, float size) const {
    Projection p;
    const Vec2 forward = fromAngle(yaw);
    const Vec2 right = rightFromForward(forward);
    const Vec2 rel = point - player_.pos;
    const float depth = dot(rel, forward);
    const float side = dot(rel, right);
    if (depth <= 0.05f) {
        return p;
    }

    const float fov = degToRad(std::max(45.0f, tunables_.cameraFovDegrees - fireFovKick_));
    const float focal = (static_cast<float>(screenW) * 0.5f) / std::tan(fov * 0.5f);
    const float centerX = static_cast<float>(screenW) * 0.5f + (side / depth) * focal;
    const float horizon = static_cast<float>(screenH) * 0.5f + pitch * static_cast<float>(screenH) * 0.72f;
    const float spriteH = (focal * size * 2.6f) / depth;
    const float spriteW = spriteH * 0.42f;
    // Shift with eye height so hit detection and bursts track the view on a jump.
    const float centerY = horizon + spriteH * 0.05f +
        (player_.height - 0.5f) / depth * static_cast<float>(screenH);

    p.left = static_cast<int>(centerX - spriteW * 0.5f);
    p.right = static_cast<int>(centerX + spriteW * 0.5f);
    p.top = static_cast<int>(centerY - spriteH * 0.75f);
    p.bottom = static_cast<int>(centerY + spriteH * 0.38f);
    p.depth = depth;
    p.side = side;
    p.visible = p.right >= 0 && p.left < screenW && p.bottom >= 0 && p.top < screenH;
    return p;
}

bool PulseGame::ensureGpuResources() {
    if (gpuResourcesReady_) {
        return true;
    }
    if (!gpuRenderer_) {
        gpuRenderer_ = std::make_unique<GpuSceneRenderer>();
    }
    if (!gpuRenderer_->initialize()) {
        return false;
    }

    gpuRenderer_->resetResources();
    gpuArenaFloorMesh_ = 0;
    gpuArenaCeilingMesh_ = 0;
    gpuArenaWallMesh_ = 0;
    gpuWeaponMesh_ = 0;
    gpuLeftHandMesh_ = 0;
    gpuRightHandMesh_ = 0;
    gpuWallTexture_ = 0;
    gpuFloorTexture_ = 0;
    gpuCeilingTexture_ = 0;
    gpuWeaponTexture_ = 0;
    gpuPickupHealthMesh_ = 0;
    gpuPickupShieldMesh_ = 0;
    gpuPickupAmmoMesh_ = 0;
    gpuTracerMesh_ = 0;
    gpuProjectileMesh_ = 0;
    gpuEnemyMeshes_.fill(0);

    const auto textureFromPixels = [](const std::vector<uint32_t>& pixels, int width, int height) {
        GpuTextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.pixelsBgra.reserve(pixels.size());
        for (uint32_t pixel : pixels) {
            desc.pixelsBgra.push_back(0xFF000000u | (pixel & 0x00FFFFFFu));
        }
        return desc;
    };
    const auto textureFromTexture = [&](const Texture& texture) {
        if (!texture.valid()) {
            return GpuTextureDesc{};
        }
        const Texture::Level& level = texture.levels.front();
        return textureFromPixels(level.pixels, level.width, level.height);
    };

    GpuTextureDesc whiteTexture;
    whiteTexture.width = 1;
    whiteTexture.height = 1;
    whiteTexture.pixelsBgra = {0xFFFFFFFFu};
    if (gpuRenderer_->createTexture(whiteTexture) == 0) {
        return false;
    }
    gpuWallTexture_ = gpuRenderer_->createTexture(textureFromTexture(wallTex_));
    gpuFloorTexture_ = gpuRenderer_->createTexture(textureFromTexture(floorTex_));
    gpuCeilingTexture_ = gpuRenderer_->createTexture(textureFromTexture(ceilingTex_));
    gpuWeaponTexture_ = gpuRenderer_->createTexture(textureFromPixels(weaponTexture_, weaponTextureWidth_, weaponTextureHeight_));

    GpuMeshDesc floorMesh;
    GpuMeshDesc ceilingMesh;
    GpuMeshDesc wallMesh;
    const int rows = static_cast<int>(Arena.size());
    const int cols = static_cast<int>(Arena.front().size());
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const char cell = Arena[static_cast<size_t>(y)][static_cast<size_t>(x)];
            const float x0 = static_cast<float>(x);
            const float x1 = static_cast<float>(x + 1);
            const float z0 = static_cast<float>(y);
            const float z1 = static_cast<float>(y + 1);
            if (cell == '#') {
                pushGpuBox(wallMesh, x0, 0.0f, z0, x1, 1.0f, z1, rgb(210, 216, 226));
                continue;
            }
            pushGpuQuad(floorMesh, {x0, 0.0f, z1}, {x1, 0.0f, z1}, {x1, 0.0f, z0}, {x0, 0.0f, z0},
                        x0, z1, x1, z0, rgb(190, 198, 210));
            pushGpuQuad(ceilingMesh, {x0, 1.0f, z0}, {x1, 1.0f, z0}, {x1, 1.0f, z1}, {x0, 1.0f, z1},
                        x0, z0, x1, z1, rgb(160, 168, 182));
        }
    }

    gpuArenaFloorMesh_ = gpuRenderer_->createMesh(floorMesh);
    gpuArenaCeilingMesh_ = gpuRenderer_->createMesh(ceilingMesh);
    gpuArenaWallMesh_ = gpuRenderer_->createMesh(wallMesh);

    const auto appendObjMesh = [&](GpuMeshDesc& mesh,
                                   const std::vector<MeshVertex>& vertices,
                                   const std::vector<MeshNormal>& normals,
                                   const std::vector<MeshUv>& uvs,
                                   const std::vector<MeshTriangle>& triangles,
                                   MeshVertex center) {
        mesh.vertices.reserve(mesh.vertices.size() + triangles.size() * 3);
        mesh.indices.reserve(mesh.indices.size() + triangles.size() * 3);
        const auto point = [&](const MeshVertex& v) {
            return GpuVec3{
                v.z - center.z,
                v.y - center.y,
                -(v.x - center.x)
            };
        };
        const auto normal = [&](int index, GpuVec3 generatedFaceNormal) {
            if (index >= 0 && index < static_cast<int>(normals.size())) {
                const MeshNormal& n = normals[static_cast<size_t>(index)];
                return gpuNormalize({n.z, n.y, -n.x});
            }
            return generatedFaceNormal;
        };
        const auto uvAt = [&](int index) {
            if (index >= 0 && index < static_cast<int>(uvs.size())) {
                return uvs[static_cast<size_t>(index)];
            }
            return MeshUv{};
        };
        for (const MeshTriangle& tri : triangles) {
            const MeshVertex& va = vertices[static_cast<size_t>(tri.a)];
            const MeshVertex& vb = vertices[static_cast<size_t>(tri.b)];
            const MeshVertex& vc = vertices[static_cast<size_t>(tri.c)];
            const MeshUv ta = uvAt(tri.ta);
            const MeshUv tb = uvAt(tri.tb);
            const MeshUv tc = uvAt(tri.tc);
            const GpuVec3 a = point(va);
            const GpuVec3 b = point(vb);
            const GpuVec3 c = point(vc);
            const GpuVec3 faceNormal = gpuNormalize(gpuCross(gpuSub(b, a), gpuSub(c, a)));
            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(gpuVertex(a, normal(tri.na, faceNormal), ta.u, 1.0f - ta.v, tri.color, tri.emissive));
            mesh.vertices.push_back(gpuVertex(b, normal(tri.nb, faceNormal), tb.u, 1.0f - tb.v, tri.color, tri.emissive));
            mesh.vertices.push_back(gpuVertex(c, normal(tri.nc, faceNormal), tc.u, 1.0f - tc.v, tri.color, tri.emissive));
            mesh.indices.push_back(base);
            mesh.indices.push_back(base + 1u);
            mesh.indices.push_back(base + 2u);
        }
    };

    GpuMeshDesc weaponMesh;
    appendObjMesh(weaponMesh, weaponMeshVertices_, weaponMeshNormals_, weaponMeshUvs_, weaponMeshTriangles_, weaponMeshCenter_);
    gpuWeaponMesh_ = gpuRenderer_->createMesh(weaponMesh);

    GpuMeshDesc leftHandGpuMesh;
    appendObjMesh(leftHandGpuMesh, leftHandMesh_.vertices, leftHandMesh_.normals, leftHandMesh_.uvs, leftHandMesh_.triangles, weaponMeshCenter_);
    gpuLeftHandMesh_ = gpuRenderer_->createMesh(leftHandGpuMesh);

    GpuMeshDesc rightHandGpuMesh;
    appendObjMesh(rightHandGpuMesh, rightHandMesh_.vertices, rightHandMesh_.normals, rightHandMesh_.uvs, rightHandMesh_.triangles, weaponMeshCenter_);
    gpuRightHandMesh_ = gpuRenderer_->createMesh(rightHandGpuMesh);

    for (int kind = 0; kind < EnemyKindCount; ++kind) {
        const MeshAsset& asset = enemyMeshAssets_[static_cast<size_t>(kind)];
        GpuMeshDesc enemyMesh;
        appendObjMesh(enemyMesh, asset.vertices, asset.normals, asset.uvs, asset.triangles, MeshVertex{});
        gpuEnemyMeshes_[static_cast<size_t>(kind)] = gpuRenderer_->createMesh(enemyMesh);
    }
    gpuPickupHealthMesh_ = gpuRenderer_->createMesh(makePickupMesh(rgb(56, 245, 138)));
    gpuPickupShieldMesh_ = gpuRenderer_->createMesh(makePickupMesh(rgb(72, 178, 255)));
    gpuPickupAmmoMesh_ = gpuRenderer_->createMesh(makeAmmoPickupMesh(rgb(225, 170, 60), rgb(60, 48, 30)));
    gpuTracerMesh_ = gpuRenderer_->createMesh(makeTracerMesh());
    gpuProjectileMesh_ = gpuRenderer_->createMesh(makeProjectileMesh(rgb(255, 120, 70)));

    gpuResourcesReady_ = gpuWallTexture_ != 0 && gpuFloorTexture_ != 0 && gpuCeilingTexture_ != 0 &&
                         gpuWeaponTexture_ != 0 && gpuArenaFloorMesh_ != 0 && gpuArenaCeilingMesh_ != 0 &&
                         gpuArenaWallMesh_ != 0 && gpuWeaponMesh_ != 0 && gpuLeftHandMesh_ != 0 && gpuRightHandMesh_ != 0 && gpuPickupHealthMesh_ != 0 &&
                         gpuPickupShieldMesh_ != 0 && gpuPickupAmmoMesh_ != 0 && gpuTracerMesh_ != 0 && gpuProjectileMesh_ != 0;
    for (uint32_t handle : gpuEnemyMeshes_) {
        gpuResourcesReady_ = gpuResourcesReady_ && handle != 0;
    }
    return gpuResourcesReady_;
}

bool PulseGame::renderGpuFrame(Renderer& renderer) {
    if (!ensureGpuResources()) {
        return false;
    }

    const int w = renderer.width();
    const int h = renderer.height();
    const Vec2 forward2 = fromAngle(renderViewYaw_);
    const Vec2 right2 = rightFromForward(forward2);

    GpuSceneFrame frame;
    frame.clearColor = rgb(7, 9, 13);
    frame.camera.position = {player_.pos.x, clamp(player_.height, 0.05f, 0.95f), player_.pos.y};
    frame.camera.yaw = renderViewYaw_;
    frame.camera.pitch = renderViewPitch_;
    frame.camera.fovDegrees = std::max(45.0f, tunables_.cameraFovDegrees - fireFovKick_);
    frame.camera.nearZ = 0.035f;
    frame.camera.farZ = MaxRayDistance;
    frame.light.direction = {-0.40f, -0.82f, -0.36f};
    frame.light.color = {1.05f, 1.12f, 1.22f, 1.0f};
    frame.light.ambient = 0.22f;
    frame.light.intensity = 0.95f;
    frame.draws.reserve(32 + enemies_.size() + pickups_.size() + tracers_.size());

    const auto addDraw = [&](uint32_t mesh, uint32_t texture, const GpuTransform& transform,
                             GpuColor tint, float emissive = 0.0f, bool cameraSpace = false,
                             GpuDepthMode depthMode = GpuDepthMode::TestWrite,
                             bool clearDepthBefore = false) {
        GpuDrawCommand draw;
        draw.mesh = mesh;
        draw.material.texture = texture;
        draw.material.tint = tint;
        draw.material.emissive = emissive;
        draw.transform = transform;
        draw.cameraSpace = cameraSpace;
        draw.depthMode = depthMode;
        draw.clearDepthBefore = clearDepthBefore;
        frame.draws.push_back(draw);
    };

    addDraw(gpuArenaFloorMesh_, gpuFloorTexture_, GpuSceneRenderer::identity(), {1.0f, 1.0f, 1.0f, 1.0f});
    addDraw(gpuArenaCeilingMesh_, gpuCeilingTexture_, GpuSceneRenderer::identity(), {0.72f, 0.78f, 0.88f, 1.0f});
    addDraw(gpuArenaWallMesh_, gpuWallTexture_, GpuSceneRenderer::identity(), {0.92f, 0.97f, 1.08f, 1.0f});

    for (const Pickup& pickup : pickups_) {
        const float bob = std::sin(pickup.age * 4.2f + pickup.phase) * 0.045f;
        const float spin = pickup.age * 1.55f + pickup.phase;
        const GpuTransform transform = GpuSceneRenderer::multiply(
            GpuSceneRenderer::scale(0.16f, 0.16f, 0.16f),
            GpuSceneRenderer::multiply(
                GpuSceneRenderer::rotationYawPitchRoll(spin, 0.4f, 0.0f),
                GpuSceneRenderer::translation(pickup.pos.x, 0.48f + bob, pickup.pos.y)));
        if (pickup.kind == PickupKind::Health) {
            addDraw(gpuPickupHealthMesh_, 0, transform, {0.85f, 1.12f, 0.92f, 1.0f}, 0.35f);
        } else if (pickup.kind == PickupKind::Shield) {
            addDraw(gpuPickupShieldMesh_, 0, transform, {0.88f, 1.02f, 1.25f, 1.0f}, 0.40f);
        } else {
            addDraw(gpuPickupAmmoMesh_, 0, transform, {1.2f, 1.0f, 0.6f, 1.0f}, 0.22f);
        }
    }

    for (const Enemy& enemy : enemies_) {
        if (!enemy.active) {
            continue;
        }
        const Vec2 toPlayer = player_.pos - enemy.pos;
        const Vec2 enemyForward = normalize(toPlayer);
        const Vec2 enemyRight = rightFromForward(enemyForward);
        const EnemyStyle style = styleFor(enemy.kind);
        // Attack wind-up: swell and flare hot so the tell is impossible to miss.
        float windup = 0.0f;
        if (enemy.telegraphRemaining > 0.0f) {
            const float base = enemy.kind == EnemyKind::Ranged
                ? std::max(0.05f, tunables_.enemyRangedTelegraph)
                : std::max(0.05f, tunables_.enemyMeleeTelegraph);
            const float p = clamp(1.0f - enemy.telegraphRemaining / base, 0.0f, 1.0f);
            windup = 0.4f + 0.6f * p; // ramps toward the strike
        }
        const float scale = style.scale * (1.0f + enemy.hitPunch * 0.26f + windup * 0.14f);
        const float hover = (enemy.kind == EnemyKind::Tank ? 0.50f : 0.52f) + 0.05f * std::sin(shakeTime_ * 2.6f + enemy.bobPhase);
        const GpuTransform transform = GpuSceneRenderer::basis(
            {enemyRight.x, 0.0f, enemyRight.y},
            {0.0f, 1.0f, 0.0f},
            {enemyForward.x, 0.0f, enemyForward.y},
            {enemy.pos.x, hover, enemy.pos.y},
            scale);
        const float hurt = enemy.hurtTimer > 0.0f ? 0.35f : 0.0f;
        const float flare = 0.65f + 0.35f * std::sin(shakeTime_ * 26.0f);
        addDraw(gpuEnemyMeshes_[static_cast<size_t>(enemy.kind)], 0, transform,
                {1.0f + hurt + windup * 0.9f, 1.0f - hurt * 0.25f - windup * 0.15f, 1.0f - hurt * 0.25f - windup * 0.4f, 1.0f},
                std::max(hurt * 0.5f, windup * (0.55f + 0.6f * flare)));
    }

    // Enemy projectiles: spinning, glowing hostile bolts the player can track.
    for (const Projectile& projectile : projectiles_) {
        if (!projectile.active) {
            continue;
        }
        const float r = std::max(0.05f, tunables_.enemyProjectileRadius);
        const float spin = projectile.age * 11.0f;
        const float pulse = 1.0f + 0.14f * std::sin(projectile.age * 32.0f);
        const GpuTransform transform = GpuSceneRenderer::multiply(
            GpuSceneRenderer::scale(r * pulse, r * pulse, r * pulse),
            GpuSceneRenderer::multiply(
                GpuSceneRenderer::rotationYawPitchRoll(spin, spin * 0.7f, 0.0f),
                GpuSceneRenderer::translation(projectile.pos.x, projectile.height, projectile.pos.y)));
        addDraw(gpuProjectileMesh_, 0, transform, {1.45f, 0.55f, 0.32f, 1.0f}, 0.95f);
    }

    const float walk = std::max(0.1f, tunables_.walkSpeed);
    const float strafe = clamp(dot(player_.vel, right2) / walk, -1.0f, 1.0f);
    const float forwardMove = clamp(dot(player_.vel, forward2) / walk, -1.0f, 1.0f);
    const float swayScale = std::max(0.0f, tunables_.weaponViewmodelSway);
    const float bobSway = player_.grounded ? std::sin(cameraBobPhase_) : 0.0f;
    const float bobLift = player_.grounded ? std::fabs(std::sin(cameraBobPhase_ * 2.0f)) : 0.0f;
    const float recoil01 = clamp(fireCameraKick_ / std::max(0.1f, tunables_.weaponFireCameraKick), 0.0f, 1.3f);
    const float reloadProgress = weapon_.reloading
        ? 1.0f - clamp(weapon_.reloadRemaining / std::max(0.1f, tunables_.weaponReloadDuration), 0.0f, 1.0f)
        : 0.0f;
    const float reloadLift = std::sin(reloadProgress * Pi);
    const float weaponScale = 0.68f;
    const float weaponX = 0.84f + (-strafe * 0.036f + bobSway * 0.010f) * swayScale;
    const float weaponY = -0.94f + (bobLift * 0.014f - forwardMove * 0.008f + recoil01 * 0.022f - reloadLift * 0.18f) * swayScale;
    const float weaponZ = 2.05f;
    const GpuTransform weaponOrientation = GpuSceneRenderer::multiply(
        GpuSceneRenderer::scale(weaponScale, weaponScale, weaponScale),
        GpuSceneRenderer::rotationYawPitchRoll(0.03f, 0.050f - recoil01 * 0.050f, 0.050f + strafe * 0.010f));
    const GpuTransform weaponTransform = GpuSceneRenderer::multiply(
        weaponOrientation,
        GpuSceneRenderer::translation(weaponX, weaponY, weaponZ));

    // First-person viewmodels are a separate render layer: clear world depth
    // once, then render the authored Blender hand/weapon assembly from the same
    // origin so fingers stay locked to the fore-end and pistol grip.
    addDraw(gpuWeaponMesh_, gpuWeaponTexture_, weaponTransform, {1.25f, 1.18f, 1.05f, 1.0f}, 0.02f, true, GpuDepthMode::TestWrite, true);
    addDraw(gpuLeftHandMesh_, 0, weaponTransform, {1.02f, 1.10f, 1.22f, 1.0f}, 0.0f, true, GpuDepthMode::Disabled);
    addDraw(gpuRightHandMesh_, 0, weaponTransform, {1.02f, 1.10f, 1.22f, 1.0f}, 0.0f, true, GpuDepthMode::TestWrite);

    std::vector<uint32_t> pixels;
    if (!gpuRenderer_->renderToPixels(w, h, frame, pixels)) {
        return false;
    }
    renderer.replacePixels(pixels);
    return true;
}

void PulseGame::render(Renderer& renderer) {
    // Derive the shaken view angle once per frame. Two detuned sine waves give a
    // lively, non-repeating jitter; the offset is applied to drawing only, so
    // the aim ray in tryFire stays exact and gunplay reads "clean" (spec 5).
    const float shakeRad = degToRad(cameraShake_);
    const float shakeYaw = shakeRad * std::sin(shakeTime_ * 47.0f);
    const float shakePitch = shakeRad * 0.6f * std::sin(shakeTime_ * 59.0f + 1.3f);
    const float bob = degToRad(std::max(0.0f, tunables_.cameraBobDegrees));
    const float bobYaw = std::sin(cameraBobPhase_) * bob * 0.34f;
    const float bobPitch = std::sin(cameraBobPhase_ * 2.0f) * bob;
    const float strafeYaw = strafeLean_ * degToRad(tunables_.cameraStrafeLeanDegrees) * 0.45f;
    const float strafePitch = -std::fabs(strafeLean_) * degToRad(tunables_.cameraStrafeLeanDegrees) * 0.18f;
    renderViewYaw_ = player_.yaw + recoilOffsetYaw_ + shakeYaw + bobYaw + strafeYaw;
    renderViewPitch_ = clamp(player_.pitch + recoilOffsetPitch_ + shakePitch + bobPitch + strafePitch - landingKick_, -1.5f, 1.5f);

    if (!renderGpuFrame(renderer)) {
        const std::string detail = gpuRenderer_ ? gpuRenderer_->lastError() : "GPU renderer unavailable";
        throw std::runtime_error("GPU scene renderer failed: " + detail);
    }
    renderHud(renderer);
}

void PulseGame::renderHud(Renderer& renderer) {
    const int w = renderer.width();
    const int h = renderer.height();
    const int cx = w / 2;
    const int cy = h / 2;

    {
        const float wf = static_cast<float>(w);
        const float hf = static_cast<float>(h);
        const float s = static_cast<float>(std::min(w, h));
        const auto quad = [&renderer](float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, uint32_t color) {
            renderer.fillTriangle(ax, ay, bx, by, cx, cy, color);
            renderer.fillTriangle(ax, ay, cx, cy, dx, dy, color);
        };

        const float baseOuterX = wf * 0.505f;
        const float baseOuterY = hf * 1.060f;
        const float baseInnerX = wf * 0.575f;
        const float baseInnerY = hf * 1.030f;
        const float midOuterX = wf * 0.538f;
        const float midOuterY = hf * 0.890f;
        const float midInnerX = wf * 0.597f;
        const float midInnerY = hf * 0.860f;
        const float wristOuterX = wf * 0.575f;
        const float wristOuterY = hf * 0.780f;
        const float wristInnerX = wf * 0.622f;
        const float wristInnerY = hf * 0.758f;
        const float cuffW = s * 0.054f;
        const float cuffH = s * 0.026f;

        quad(baseOuterX, baseOuterY,
             baseInnerX, baseInnerY,
             midInnerX, midInnerY,
             midOuterX, midOuterY,
             rgba(14, 23, 46, 216));
        quad(midOuterX, midOuterY,
             midInnerX, midInnerY,
             wristInnerX, wristInnerY,
             wristOuterX, wristOuterY,
             rgba(24, 43, 80, 218));
        quad(wf * 0.530f, hf * 1.020f,
             wf * 0.553f, hf * 1.005f,
             wf * 0.584f, hf * 0.787f,
             wf * 0.563f, hf * 0.797f,
             rgba(58, 92, 148, 118));
        quad(wf * 0.498f, hf * 1.040f,
             wf * 0.515f, hf * 1.030f,
             wf * 0.544f, hf * 0.895f,
             wf * 0.528f, hf * 0.905f,
             rgba(6, 12, 26, 138));

        quad(wristOuterX - cuffW * 0.22f, wristOuterY + cuffH * 0.82f,
             wristInnerX + cuffW * 0.20f, wristInnerY + cuffH * 0.38f,
             wristInnerX + cuffW * 0.12f, wristInnerY - cuffH * 1.02f,
             wristOuterX - cuffW * 0.10f, wristOuterY - cuffH * 0.72f,
             rgba(36, 51, 82, 226));
        quad(wristOuterX - cuffW * 0.08f, wristOuterY + cuffH * 0.32f,
             wristInnerX + cuffW * 0.12f, wristInnerY + cuffH * 0.10f,
             wristInnerX + cuffW * 0.04f, wristInnerY - cuffH * 0.58f,
             wristOuterX + cuffW * 0.02f, wristOuterY - cuffH * 0.42f,
             rgba(98, 138, 204, 150));
        renderer.drawLine(wf * 0.528f, hf * 1.030f,
                          wf * 0.565f, hf * 0.795f,
                          rgba(108, 148, 212, 92), 2);
        renderer.drawLine(wf * 0.552f, hf * 0.885f,
                          wf * 0.595f, hf * 0.765f,
                          rgba(8, 15, 32, 120), 2);
        renderer.drawLine(wristOuterX - cuffW * 0.08f, wristOuterY + cuffH * 0.22f,
                          wristInnerX + cuffW * 0.10f, wristInnerY - cuffH * 0.36f,
                          rgba(150, 182, 232, 126), 2);
    }

    if (muzzleFlashTimer_ > 0.0f) {
        const float t = clamp(muzzleFlashTimer_ / 0.09f, 0.0f, 1.0f);
        const float hot = t * t;
        const float muzzleX = clamp(tunables_.weaponMuzzleScreenX, 0.0f, 1.0f) * static_cast<float>(w);
        const float muzzleY = clamp(tunables_.weaponMuzzleScreenY, 0.0f, 1.0f) * static_cast<float>(h);
        float dirX = static_cast<float>(cx) - muzzleX;
        float dirY = static_cast<float>(cy) - muzzleY;
        const float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
        if (dirLen > 0.001f) {
            dirX /= dirLen;
            dirY /= dirLen;
        } else {
            dirX = -0.35f;
            dirY = -0.94f;
        }
        const float perpX = -dirY;
        const float perpY = dirX;
        const float blast = static_cast<float>(std::min(w, h)) * (0.038f + 0.018f * hot);
        const float width = static_cast<float>(std::min(w, h)) * (0.014f + 0.010f * hot);
        const uint8_t coreA = static_cast<uint8_t>(225.0f * hot);
        const uint8_t flameA = static_cast<uint8_t>(175.0f * t);

        renderer.fillTriangle(
            muzzleX + perpX * width, muzzleY + perpY * width,
            muzzleX - perpX * width, muzzleY - perpY * width,
            muzzleX + dirX * blast, muzzleY + dirY * blast,
            rgba(255, 238, 164, coreA));
        renderer.fillTriangle(
            muzzleX + dirX * blast * 0.08f + perpX * width * 0.95f,
            muzzleY + dirY * blast * 0.08f + perpY * width * 0.95f,
            muzzleX + dirX * blast * 0.08f - perpX * width * 0.95f,
            muzzleY + dirY * blast * 0.08f - perpY * width * 0.95f,
            muzzleX - dirX * blast * 0.08f,
            muzzleY - dirY * blast * 0.08f,
            rgba(255, 118, 38, flameA));
        renderer.drawLine(muzzleX - perpX * width * 1.15f, muzzleY - perpY * width * 1.15f,
                          muzzleX + perpX * width * 1.15f, muzzleY + perpY * width * 1.15f,
                          rgba(255, 194, 86, flameA), 2);
        renderer.fillRect(static_cast<int>(muzzleX) - 4, static_cast<int>(muzzleY) - 4, 8, 8, rgba(255, 248, 208, coreA));
    }

    float tracerLife = 0.0f;
    for (const Tracer& tracer : tracers_) {
        const float life = 1.0f - clamp(tracer.age / std::max(0.001f, tracer.duration), 0.0f, 1.0f);
        tracerLife = std::max(tracerLife, life);
    }
    if (tracerLife > 0.0f) {
        const float muzzleX = clamp(tunables_.weaponMuzzleScreenX, 0.0f, 1.0f) * static_cast<float>(w);
        const float muzzleY = clamp(tunables_.weaponMuzzleScreenY, 0.0f, 1.0f) * static_cast<float>(h);
        float dirX = static_cast<float>(cx) - muzzleX;
        float dirY = static_cast<float>(cy) - muzzleY;
        const float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
        if (dirLen > 0.001f) {
            dirX /= dirLen;
            dirY /= dirLen;
        }
        const float startX = muzzleX + dirX * 2.0f;
        const float startY = muzzleY + dirY * 2.0f;
        const float endX = static_cast<float>(cx) - dirX * 8.0f;
        const float endY = static_cast<float>(cy) - dirY * 8.0f;
        const uint8_t glowA = static_cast<uint8_t>(clamp(tracerLife * 155.0f, 0.0f, 155.0f));
        const uint8_t coreA = static_cast<uint8_t>(clamp(tracerLife * 245.0f, 0.0f, 245.0f));
        renderer.drawLine(startX, startY, endX, endY, rgba(255, 122, 42, glowA), 2);
        renderer.drawLine(startX, startY, endX, endY, coreA > 24u ? rgb(255, 238, 158) : rgba(255, 238, 158, coreA), 2);
    }

    const uint32_t cross = rgb(235, 242, 246);
    renderer.fillRect(cx - 1, cy - 14, 2, 8, cross);
    renderer.fillRect(cx - 1, cy + 6, 2, 8, cross);
    renderer.fillRect(cx - 14, cy - 1, 8, 2, cross);
    renderer.fillRect(cx + 6, cy - 1, 8, 2, cross);

    // Directional damage indicators: a fading red wedge at the screen edge
    // pointing toward each recent hit source, so damage always reads as coming
    // "from there" rather than out of nowhere.
    if (!damageMarkers_.empty()) {
        const float ringR = static_cast<float>(std::min(w, h)) * 0.17f;
        const float thickness = static_cast<float>(std::min(w, h)) * 0.052f;
        const float cxf = static_cast<float>(cx);
        const float cyf = static_cast<float>(cy);
        for (const DamageMarker& marker : damageMarkers_) {
            const float t = clamp(marker.age / std::max(0.05f, marker.life), 0.0f, 1.0f);
            const float fade = 1.0f - t * t; // hold bright, then fall off fast
            const uint8_t a = static_cast<uint8_t>(clamp(fade * (0.5f + 0.5f * marker.intensity), 0.0f, 1.0f) * 210.0f);
            if (a == 0u) {
                continue;
            }
            // Screen angle: 0 rad of relative bearing = straight ahead (top).
            const float rel = wrapAngle(marker.worldAngle - player_.yaw);
            const float screenAngle = std::atan2(-std::cos(rel), std::sin(rel));
            const float half = degToRad(22.0f);
            const uint32_t col = rgba(255, 46, 32, a);
            const uint32_t edgeCol = rgba(255, 150, 120, static_cast<uint8_t>(a * 0.7f));
            const int segs = 8;
            for (int s = 0; s < segs; ++s) {
                const float a0 = screenAngle - half + (2.0f * half) * (static_cast<float>(s) / segs);
                const float a1 = screenAngle - half + (2.0f * half) * (static_cast<float>(s + 1) / segs);
                // Taper toward the ends so the wedge points at the threat.
                const float mid0 = 1.0f - std::fabs(static_cast<float>(s) / segs - 0.5f) * 2.0f;
                const float mid1 = 1.0f - std::fabs(static_cast<float>(s + 1) / segs - 0.5f) * 2.0f;
                const float r0 = ringR;
                const float t0 = thickness * (0.35f + 0.65f * mid0);
                const float t1 = thickness * (0.35f + 0.65f * mid1);
                const float ix0 = cxf + std::cos(a0) * r0, iy0 = cyf + std::sin(a0) * r0;
                const float ox0 = cxf + std::cos(a0) * (r0 + t0), oy0 = cyf + std::sin(a0) * (r0 + t0);
                const float ix1 = cxf + std::cos(a1) * r0, iy1 = cyf + std::sin(a1) * r0;
                const float ox1 = cxf + std::cos(a1) * (r0 + t1), oy1 = cyf + std::sin(a1) * (r0 + t1);
                renderer.fillTriangle(ix0, iy0, ox0, oy0, ox1, oy1, col);
                renderer.fillTriangle(ix0, iy0, ox1, oy1, ix1, iy1, col);
                renderer.drawLine(ix0, iy0, ix1, iy1, edgeCol, 1); // bright inner rim
            }
        }
    }

    if (hitmarkerTimer_ > 0.0f) {
        renderer.drawLine(cx - 19.0f, cy - 19.0f, cx - 7.0f, cy - 7.0f, rgb(255, 255, 255), 4);
        renderer.drawLine(cx + 19.0f, cy - 19.0f, cx + 7.0f, cy - 7.0f, rgb(255, 255, 255), 4);
        renderer.drawLine(cx - 19.0f, cy + 19.0f, cx - 7.0f, cy + 7.0f, rgb(255, 255, 255), 4);
        renderer.drawLine(cx + 19.0f, cy + 19.0f, cx + 7.0f, cy + 7.0f, rgb(255, 255, 255), 4);
    }
    if (killConfirmTimer_ > 0.0f) {
        renderer.drawLine(cx - 24.0f, cy - 24.0f, cx - 10.0f, cy - 10.0f, rgb(255, 44, 44), 4);
        renderer.drawLine(cx + 24.0f, cy - 24.0f, cx + 10.0f, cy - 10.0f, rgb(255, 44, 44), 4);
        renderer.drawLine(cx - 24.0f, cy + 24.0f, cx - 10.0f, cy + 10.0f, rgb(255, 44, 44), 4);
        renderer.drawLine(cx + 24.0f, cy + 24.0f, cx + 10.0f, cy + 10.0f, rgb(255, 44, 44), 4);
    }

    const uint32_t ammoColor = weapon_.reloading ? rgb(105, 190, 255) : (weapon_.ammo <= 8 ? rgb(255, 90, 50) : rgb(232, 238, 244));
    renderer.drawTextRight(w - 42, h - 62, twoDigits(weapon_.ammo) + "/" + threeDigits(weapon_.reserve), ammoColor, 3);
    if (weapon_.reloading) {
        renderer.drawTextRight(w - 42, h - 90, "RELOADING", rgb(105, 190, 255), 2);
    }

    renderer.drawText(32, 30, "PULSE", rgb(110, 200, 255), 3);
    renderer.drawText(34, 72, "SCORE " + std::to_string(score_), rgb(226, 232, 240), 2);
    renderer.drawText(34, 96, "BEST " + std::to_string(bestScore_), rgb(150, 160, 174), 2);
    renderer.drawText(34, h - 52, "HP " + std::to_string(player_.hp), player_.hp <= 35 ? rgb(255, 88, 58) : rgb(228, 238, 232), 3);

    const int maxShield = std::max(1, tunables_.playerMaxShield);
    const float shield01 = clamp(static_cast<float>(player_.shield) / static_cast<float>(maxShield), 0.0f, 1.0f);
    renderer.drawText(34, h - 90, "SH " + std::to_string(player_.shield), player_.shield > 0 ? rgb(125, 220, 255) : rgb(82, 96, 112), 2);
    renderer.fillRect(34, h - 104, 120, 8, rgb(24, 34, 45));
    renderer.fillRect(34, h - 104, static_cast<int>(120.0f * shield01), 8, rgb(70, 190, 255));

    const float dashReady = player_.dashCooldown <= 0.0f ? 1.0f : 1.0f - clamp(player_.dashCooldown / std::max(0.01f, tunables_.dashCooldown), 0.0f, 1.0f);
    renderer.fillRect(34, h - 128, 120, 6, rgb(38, 43, 52));
    renderer.fillRect(34, h - 128, static_cast<int>(120.0f * dashReady), 6, rgb(90, 190, 255));

    if (shieldFlashTimer_ > 0.0f) {
        const uint8_t a = static_cast<uint8_t>(75.0f * clamp(shieldFlashTimer_ / 0.28f, 0.0f, 1.0f));
        renderer.fillRect(0, 0, w, h, rgba(50, 180, 255, a));
    }

    if (damageFlashTimer_ > 0.0f) {
        const uint8_t a = static_cast<uint8_t>(110.0f * clamp(damageFlashTimer_ / 0.28f, 0.0f, 1.0f));
        renderer.fillRect(0, 0, w, h, rgba(255, 30, 20, a));
    }

    if (restartTimer_ > 0.0f) {
        renderer.fillRect(0, 0, w, h, rgba(0, 0, 0, 145));
        renderer.drawText(cx - 90, cy - 28, "RUN RESET", rgb(255, 68, 58), 4);
    }

    if (configMessageTimer_ > 0.0f) {
        renderer.drawTextRight(w - 34, 32, configMessage_, rgb(125, 220, 255), 2);
    }
}

} // namespace pulse
