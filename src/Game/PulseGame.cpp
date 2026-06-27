#include "Game/PulseGame.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
#include <string_view>
#include <unordered_map>
#include <windows.h>

#include "Engine/Audio.hpp"
#include "Engine/Config.hpp"
#include "Engine/Core/ImageFile.hpp"
#include "Engine/Core/MeshFile.hpp"
#include "Engine/Engine.hpp"
#include "Engine/UI/UiDrawList.hpp"
#include "Engine/UI/Color.hpp"

namespace pulse {
namespace {

// CPU-side geometry types used by the procedural mesh builders below. They were
// once the deleted GpuSceneRenderer's types; the builders are unchanged and the
// result is converted to an engine mesh (engineMeshFromGpu) at load.
struct GpuVec3  { float x = 0, y = 0, z = 0; };
struct GpuColor { float r = 1, g = 1, b = 1, a = 1; };
struct GpuVertex {
    float x = 0, y = 0, z = 0;
    float nx = 0, ny = 0, nz = 0;
    float u = 0, v = 0;
    float r = 1, g = 1, b = 1, a = 1;
    float emissive = 0;
};
struct GpuMeshDesc {
    std::vector<GpuVertex> vertices;
    std::vector<uint32_t>  indices;
};

// Unpack an engine-packed colour (R | G<<8 | B<<16 | A<<24, see UI/Color.hpp).
GpuColor colorFromRgb(uint32_t packed, float alpha) {
    return { (packed & 0xFFu) / 255.0f, ((packed >> 8) & 0xFFu) / 255.0f,
             ((packed >> 16) & 0xFFu) / 255.0f, alpha };
}

MusicBiome musicBiomeFromWorld(Biome biome) {
    switch (biome) {
        case Biome::Forest: return MusicBiome::Furnace;
        case Biome::Ruins:  return MusicBiome::Reliquary;
        case Biome::Rocky:
        case Biome::Count:
            break;
    }
    return MusicBiome::Foundry;
}

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
constexpr float BossVisualScale = 3.0f;
constexpr const char* kPulseEnemyInputsRoot = "assets/meshy/enemy_inputs/meshy_generated_models";
constexpr uint32_t ShotFxBurn    = 1u << 0;
constexpr uint32_t ShotFxShock   = 1u << 1;
constexpr uint32_t ShotFxCryo    = 1u << 2;
constexpr uint32_t ShotFxCorrode = 1u << 3;
constexpr uint32_t ShotFxLeech   = 1u << 4;
constexpr uint32_t DmgTextNeutral = rgb(239, 246, 255);
constexpr uint32_t DmgTextCrit    = rgb(255, 210, 77);
constexpr uint32_t DmgTextBurn    = rgb(255, 138, 42);
constexpr uint32_t DmgTextShock   = rgb(114, 214, 255);
constexpr uint32_t DmgTextCryo    = rgb(141, 235, 255);
constexpr uint32_t DmgTextCorrode = rgb(155, 255, 100);
constexpr uint32_t DmgTextLeech   = rgb(88, 240, 142);
constexpr uint32_t DmgTextCombo   = rgb(214, 133, 255);

struct SprayPoint {
    float pitchDeg = 0.0f;
    float yawDeg = 0.0f;
};

// Cumulative AK-style spray positions. Each entry is the view/shot offset held
// after a shot, so the next bullet leaves from that learned pattern point.
constexpr std::array<SprayPoint, 30> AkSprayPattern = {{
    {0.32f, 0.00f}, {0.74f, -0.10f}, {1.20f, 0.08f}, {1.70f, -0.12f}, {2.22f, -0.32f},
    {2.76f, -0.58f}, {3.30f, -0.88f}, {3.82f, -1.12f}, {4.30f, -1.00f}, {4.75f, -0.58f},
    {5.15f, 0.05f}, {5.50f, 0.84f}, {5.82f, 1.62f}, {6.10f, 2.25f}, {6.34f, 2.60f},
    {6.54f, 2.48f}, {6.70f, 1.95f}, {6.84f, 1.05f}, {6.96f, 0.05f}, {7.06f, -1.00f},
    {7.16f, -1.95f}, {7.24f, -2.62f}, {7.31f, -2.86f}, {7.37f, -2.50f}, {7.42f, -1.55f},
    {7.46f, -0.45f}, {7.50f, 0.66f}, {7.54f, 1.55f}, {7.58f, 2.10f}, {7.62f, 2.32f}
}};

SprayPoint akSprayPoint(int shotIndex) {
    const int idx = std::clamp(shotIndex, 0, static_cast<int>(AkSprayPattern.size()) - 1);
    return AkSprayPattern[static_cast<size_t>(idx)];
}

float ease01(float t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

int shotElementCount(uint32_t mask) {
    int n = 0;
    if (mask & ShotFxBurn) ++n;
    if (mask & ShotFxShock) ++n;
    if (mask & ShotFxCryo) ++n;
    if (mask & ShotFxCorrode) ++n;
    return n;
}

uint32_t damageTextColorForMask(uint32_t mask) {
    if (shotElementCount(mask) > 1) return DmgTextCombo;
    if (mask & ShotFxBurn) return DmgTextBurn;
    if (mask & ShotFxShock) return DmgTextShock;
    if (mask & ShotFxCryo) return DmgTextCryo;
    if (mask & ShotFxCorrode) return DmgTextCorrode;
    if (mask & ShotFxLeech) return DmgTextLeech;
    return DmgTextNeutral;
}

uint32_t damageTextColorForElement(Element e) {
    switch (e) {
        case Element::Burn: return DmgTextBurn;
        case Element::Shock: return DmgTextShock;
        case Element::Cryo: return DmgTextCryo;
        case Element::Corrode: return DmgTextCorrode;
        default: break;
    }
    return DmgTextNeutral;
}

Vec3f hotHue(Vec3f c, float scale = 1.35f, float cap = 2.8f) {
    return { std::min(c.x * scale, cap), std::min(c.y * scale, cap), std::min(c.z * scale, cap) };
}

float easeRange(float t, float start, float end) {
    return ease01((t - start) / std::max(0.0001f, end - start));
}

float holdRange(float t, float inStart, float inEnd, float outStart, float outEnd) {
    return easeRange(t, inStart, inEnd) * (1.0f - easeRange(t, outStart, outEnd));
}

float pulseRange(float t, float start, float end) {
    const float u = clamp((t - start) / std::max(0.0001f, end - start), 0.0f, 1.0f);
    return std::sin(u * Pi);
}

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
    const GpuColor c = colorFromRgb(color, static_cast<float>(alpha) / 255.0f);
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

// Reverse triangle winding (swap the 2nd/3rd index of each tri) WITHOUT touching
// positions or normals. The arena floor/ceiling quads were authored with the
// opposite chirality to the wall boxes + OBJ content; this makes the whole world
// share one front-face convention so back-face culling is consistent (M1.0b).
void reverseWinding(GpuMeshDesc& mesh) {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t t = mesh.indices[i + 1];
        mesh.indices[i + 1] = mesh.indices[i + 2];
        mesh.indices[i + 2] = t;
    }
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

// A faceted gem / dart orb: an octahedron with the x/y poles at +/-rxy and the z poles at
// +/-rz (rz > rxy gives a long shard/dart that the render points along its velocity). Emitted
// double-sided so triangle winding never matters - these orbs are emissive and bloom anyway.
GpuMeshDesc makeOrbGem(uint32_t color, float rxy, float rz) {
    GpuMeshDesc m;
    const GpuVec3 px{ rxy, 0, 0 }, nx{ -rxy, 0, 0 };
    const GpuVec3 py{ 0, rxy, 0 }, ny{ 0, -rxy, 0 };
    const GpuVec3 pz{ 0, 0, rz },  nz{ 0, 0, -rz };
    const auto face = [&](GpuVec3 a, GpuVec3 b, GpuVec3 c) {
        pushGpuTri(m, a, b, c, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f, color);  // front
        pushGpuTri(m, a, c, b, 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f, color);  // back (double-sided)
    };
    face(pz, px, py); face(pz, py, nx); face(pz, nx, ny); face(pz, ny, px);
    face(nz, py, px); face(nz, nx, py); face(nz, ny, nx); face(nz, px, ny);
    return m;
}

// A smooth energy-orb SPHERE: a clear round glowing ball (no faceted gem/spike/shard edges)
// for enemy projectiles + their charge-up. High subdivision keeps facet normal-breaks below
// the ink-outline threshold, so the orb gets a clean SILHOUETTE outline (on-brand) but no
// internal lines, and the emissive bloom hides the rest. Double-sided so winding never matters.
GpuMeshDesc makeSphere(uint32_t color, float radius, int segments, int rings) {
    GpuMeshDesc m;
    const auto V = [&](int ri, int si) -> GpuVec3 {
        const float phi = (static_cast<float>(ri) / static_cast<float>(rings)) * Pi;       // 0..Pi pole to pole
        const float th  = (static_cast<float>(si) / static_cast<float>(segments)) * TwoPi; // 0..2Pi around
        const float sp = std::sin(phi);
        return { radius * sp * std::cos(th), radius * std::cos(phi), radius * sp * std::sin(th) };
    };
    for (int ri = 0; ri < rings; ++ri)
        for (int si = 0; si < segments; ++si) {
            const GpuVec3 a = V(ri, si), b = V(ri, si + 1), c = V(ri + 1, si), d = V(ri + 1, si + 1);
            pushGpuTri(m, a, b, c, 0, 0, 0, 0, 0, 0, color);  pushGpuTri(m, a, c, b, 0, 0, 0, 0, 0, 0, color);
            pushGpuTri(m, b, d, c, 0, 0, 0, 0, 0, 0, color);  pushGpuTri(m, b, c, d, 0, 0, 0, 0, 0, 0, color);
        }
    return m;
}

// A unit cylinder along +Z (z in [0,1], radius 1 in XY) for the lock-on BEAM. Scaled
// (thick, thick, length) + yaw-oriented + translated to the muzzle per draw, so it is a
// SOLID emissive beam that also gets a clean ink-outline silhouette (not dotted particles).
// Sides are double-sided so winding never matters; the tiny end caps are single.
GpuMeshDesc makeCylinder(uint32_t color, int sides) {
    GpuMeshDesc m;
    const auto ring = [&](int s, float z) -> GpuVec3 {
        const float a = (static_cast<float>(s) / static_cast<float>(sides)) * TwoPi;
        return { std::cos(a), std::sin(a), z };
    };
    const GpuVec3 c0{ 0, 0, 0 }, c1{ 0, 0, 1 };
    for (int s = 0; s < sides; ++s) {
        const GpuVec3 a = ring(s, 0.0f), b = ring(s + 1, 0.0f);
        const GpuVec3 d = ring(s, 1.0f), e = ring(s + 1, 1.0f);
        pushGpuTri(m, a, d, b, 0, 0, 0, 0, 0, 0, color);  pushGpuTri(m, a, b, d, 0, 0, 0, 0, 0, 0, color);  // side (double-sided)
        pushGpuTri(m, b, d, e, 0, 0, 0, 0, 0, 0, color);  pushGpuTri(m, b, e, d, 0, 0, 0, 0, 0, 0, color);
        pushGpuTri(m, c0, b, a, 0, 0, 0, 0, 0, 0, color);                                                  // near cap
        pushGpuTri(m, c1, d, e, 0, 0, 0, 0, 0, 0, color);                                                  // far cap
    }
    return m;
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

// Upload a CPU mesh to the engine and return its handle (per-vertex emissive is
// dropped; the engine applies emissive per-instance).
MeshHandle engineMeshFromGpu(Engine& engine, const GpuMeshDesc& src) {
    std::vector<StaticVertex> verts;
    verts.reserve(src.vertices.size());
    for (const GpuVertex& gv : src.vertices) {
        StaticVertex sv;
        sv.pos = { gv.x, gv.y, gv.z };
        sv.nrm = { gv.nx, gv.ny, gv.nz };
        sv.uv0[0] = gv.u; sv.uv0[1] = gv.v;
        sv.color = { gv.r, gv.g, gv.b, gv.a };
        verts.push_back(sv);
    }
    return engine.createMesh({ verts, src.indices });
}

void offsetAnimatedTaggedVertices(std::vector<AnimatedGltfSubmesh>& submeshes, uint8_t tag, Vec3f delta) {
    for (AnimatedGltfSubmesh& sm : submeshes) {
        const size_t n = std::min(sm.vertices.size(), sm.vertexTags.size());
        for (size_t i = 0; i < n; ++i) {
            if (sm.vertexTags[i] != tag) continue;
            StaticVertex& v = sm.vertices[i];
            v.pos = v.pos + delta;
        }
    }
}

void dampAnimatedVerticesTowardNeutral(std::vector<AnimatedGltfSubmesh>& animated,
                                       const std::vector<AnimatedGltfSubmesh>& neutral,
                                       float animatedScale) {
    const float t = std::clamp(animatedScale, 0.0f, 1.0f);
    const size_t meshCount = std::min(animated.size(), neutral.size());
    for (size_t mi = 0; mi < meshCount; ++mi) {
        std::vector<StaticVertex>& av = animated[mi].vertices;
        const std::vector<StaticVertex>& nv = neutral[mi].vertices;
        const size_t vertexCount = std::min(av.size(), nv.size());
        for (size_t vi = 0; vi < vertexCount; ++vi) {
            StaticVertex& a = av[vi];
            const StaticVertex& n = nv[vi];
            a.pos = n.pos + (a.pos - n.pos) * t;
            a.nrm = normalize3(n.nrm + (a.nrm - n.nrm) * t);
            a.tangent.x = n.tangent.x + (a.tangent.x - n.tangent.x) * t;
            a.tangent.y = n.tangent.y + (a.tangent.y - n.tangent.y) * t;
            a.tangent.z = n.tangent.z + (a.tangent.z - n.tangent.z) * t;
            a.tangent.w = n.tangent.w;
        }
    }
}

// Cross-fade a posed mesh set toward another posed set of the same topology: dst = lerp(dst, src, t).
// Lets enemy animation states (idle<->walk by speed, locomotion<->attack at the window edges) ease
// into each other instead of hard-snapping between baked clip windows, which reads as robotic.
void blendAnimatedVertices(std::vector<AnimatedGltfSubmesh>& dst,
                           const std::vector<AnimatedGltfSubmesh>& src, float t) {
    const float w = std::clamp(t, 0.0f, 1.0f);
    if (w <= 0.0001f) return;
    const size_t meshCount = std::min(dst.size(), src.size());
    for (size_t mi = 0; mi < meshCount; ++mi) {
        std::vector<StaticVertex>& dv = dst[mi].vertices;
        const std::vector<StaticVertex>& sv = src[mi].vertices;
        const size_t vertexCount = std::min(dv.size(), sv.size());
        for (size_t vi = 0; vi < vertexCount; ++vi) {
            StaticVertex& a = dv[vi];
            const StaticVertex& b = sv[vi];
            a.pos = a.pos + (b.pos - a.pos) * w;
            a.nrm = normalize3(a.nrm + (b.nrm - a.nrm) * w);
            a.tangent.x = a.tangent.x + (b.tangent.x - a.tangent.x) * w;
            a.tangent.y = a.tangent.y + (b.tangent.y - a.tangent.y) * w;
            a.tangent.z = a.tangent.z + (b.tangent.z - a.tangent.z) * w;
        }
    }
}

struct EnemyPoseMetrics {
    float meanX = 0.0f;
    float meanY = 0.0f;
    float meanZ = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float radiusXZ = 0.0f;
    bool valid = false;
};

EnemyPoseMetrics measureEnemyPose(const std::vector<AnimatedGltfSubmesh>& pose,
                                  const std::vector<uint8_t>& visible) {
    double sx = 0.0;
    double sy = 0.0;
    double sz = 0.0;
    size_t count = 0;
    float minY = std::numeric_limits<float>::max();
    float maxY = -std::numeric_limits<float>::max();
    std::vector<Vec2> planar;
    for (size_t mi = 0; mi < pose.size(); ++mi) {
        if (mi < visible.size() && visible[mi] == 0) continue;
        for (const StaticVertex& v : pose[mi].vertices) {
            sx += static_cast<double>(v.pos.x);
            sy += static_cast<double>(v.pos.y);
            sz += static_cast<double>(v.pos.z);
            minY = std::min(minY, v.pos.y);
            maxY = std::max(maxY, v.pos.y);
            planar.push_back({ v.pos.x, v.pos.z });
            ++count;
        }
    }
    if (count == 0 || minY == std::numeric_limits<float>::max()) return {};
    const float meanX = static_cast<float>(sx / static_cast<double>(count));
    const float meanZ = static_cast<float>(sz / static_cast<double>(count));
    float radiusXZ = 0.0f;
    for (const Vec2& p : planar) {
        const Vec2 d{ p.x - meanX, p.y - meanZ };
        radiusXZ = std::max(radiusXZ, std::sqrt(d.x * d.x + d.y * d.y));
    }
    EnemyPoseMetrics m;
    m.meanX = meanX;
    m.meanY = static_cast<float>(sy / static_cast<double>(count));
    m.meanZ = meanZ;
    m.minY = minY;
    m.maxY = maxY;
    m.radiusXZ = radiusXZ;
    m.valid = true;
    return m;
}

void stabilizeEnemyPose(std::vector<AnimatedGltfSubmesh>& pose,
                        const std::vector<uint8_t>& visible,
                        float anchorX, float anchorY, float anchorZ, float footY,
                        bool lockGround, bool lockVerticalCenter) {
    const EnemyPoseMetrics m = measureEnemyPose(pose, visible);
    if (!m.valid) return;
    const float dx = anchorX - m.meanX;
    const float dy = lockGround ? (footY - m.minY) : (lockVerticalCenter ? (anchorY - m.meanY) : 0.0f);
    const float dz = anchorZ - m.meanZ;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return;
    for (size_t mi = 0; mi < pose.size(); ++mi) {
        if (mi < visible.size() && visible[mi] == 0) continue;
        for (StaticVertex& v : pose[mi].vertices) {
            v.pos.x += dx;
            v.pos.y += dy;
            v.pos.z += dz;
        }
    }
}

Vec3f transformPoint(const Mat4& xf, Vec3f p) {
    return {
        p.x * xf.m[0][0] + p.y * xf.m[1][0] + p.z * xf.m[2][0] + xf.m[3][0],
        p.x * xf.m[0][1] + p.y * xf.m[1][1] + p.z * xf.m[2][1] + xf.m[3][1],
        p.x * xf.m[0][2] + p.y * xf.m[1][2] + p.z * xf.m[2][2] + xf.m[3][2],
    };
}

Vec3f barrelEndFaceMuzzleCamera(const std::vector<StaticVertex>& vertices,
                                const std::vector<uint32_t>& indices,
                                const Mat4& viewmodelXf) {
    if (vertices.empty() || indices.size() < 3) return { 0, 0, 1 };

    std::vector<Vec3f> cam;
    cam.reserve(vertices.size());
    Vec3f minV{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    Vec3f maxV{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
    for (const StaticVertex& v : vertices) {
        const Vec3f p = transformPoint(viewmodelXf, v.pos);
        cam.push_back(p);
        minV.x = std::min(minV.x, p.x); maxV.x = std::max(maxV.x, p.x);
        minV.y = std::min(minV.y, p.y); maxV.y = std::max(maxV.y, p.y);
        minV.z = std::min(minV.z, p.z); maxV.z = std::max(maxV.z, p.z);
    }

    struct Candidate { uint32_t ia = 0, ib = 0, ic = 0; float area = 0.0f; };
    std::vector<Candidate> candidates;
    const Vec3f meshCenter{ (minV.x + maxV.x) * 0.5f, (minV.y + maxV.y) * 0.5f, (minV.z + maxV.z) * 0.5f };
    const float forwardSpan = std::max(0.001f, maxV.z - minV.z);
    const float endFaceDepth = std::max(0.006f, forwardSpan * 0.040f);

    std::vector<int> parent(vertices.size(), -1);
    const auto findRoot = [&](auto&& self, uint32_t idx) -> int {
        int& p = parent[static_cast<size_t>(idx)];
        if (p < 0 || p == static_cast<int>(idx)) return static_cast<int>(idx);
        p = self(self, static_cast<uint32_t>(p));
        return p;
    };
    const auto mark = [&](uint32_t idx) {
        int& p = parent[static_cast<size_t>(idx)];
        if (p < 0) p = static_cast<int>(idx);
    };
    const auto unite = [&](uint32_t a, uint32_t b) {
        mark(a); mark(b);
        const int ra = findRoot(findRoot, a);
        const int rb = findRoot(findRoot, b);
        if (ra != rb) parent[static_cast<size_t>(rb)] = ra;
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t ia = indices[i + 0];
        const uint32_t ib = indices[i + 1];
        const uint32_t ic = indices[i + 2];
        if (ia >= cam.size() || ib >= cam.size() || ic >= cam.size()) continue;

        const Vec3f a = cam[ia];
        const Vec3f b = cam[ib];
        const Vec3f c = cam[ic];
        const Vec3f centroid{ (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
        if (centroid.z < maxV.z - endFaceDepth) continue;

        const Vec3f cross = cross3(b - a, c - a);
        const Vec3f n = normalize3(cross);
        if (std::fabs(n.z) < 0.76f) continue;

        unite(ia, ib);
        unite(ia, ic);
        const float area = 0.5f * std::sqrt(std::max(0.0f, dot3(cross, cross)));
        candidates.push_back({ ia, ib, ic, area });
    }

    if (candidates.empty()) {
        return { meshCenter.x, meshCenter.y, maxV.z };
    }

    struct FaceComponent {
        Vec3f min{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        Vec3f max{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
        float area = 0.0f;
        int tris = 0;
    };
    std::unordered_map<int, FaceComponent> components;
    for (const Candidate& candidate : candidates) {
        const int root = findRoot(findRoot, candidate.ia);
        FaceComponent& comp = components[root];
        comp.area += candidate.area;
        ++comp.tris;
        for (uint32_t vi : { candidate.ia, candidate.ib, candidate.ic }) {
            const Vec3f p = cam[vi];
            comp.min.x = std::min(comp.min.x, p.x); comp.max.x = std::max(comp.max.x, p.x);
            comp.min.y = std::min(comp.min.y, p.y); comp.max.y = std::max(comp.max.y, p.y);
            comp.min.z = std::min(comp.min.z, p.z); comp.max.z = std::max(comp.max.z, p.z);
        }
    }

    const FaceComponent* best = nullptr;
    for (const auto& [_, comp] : components) {
        if (!best ||
            comp.max.z > best->max.z + endFaceDepth * 0.2f ||
            (std::fabs(comp.max.z - best->max.z) <= endFaceDepth * 0.2f && comp.area > best->area)) {
            best = &comp;
        }
    }
    if (!best) {
        return { meshCenter.x, meshCenter.y, maxV.z };
    }
    return {
        (best->min.x + best->max.x) * 0.5f,
        (best->min.y + best->max.y) * 0.5f,
        best->max.z
    };
}

} // namespace

PulseGame::PulseGame() {
    loadConfig(false);
    if (!loadAllWeaponViewmodels()) {
        throw std::runtime_error("Required authored weapon viewmodel missing or invalid; check config/pulse.weapons");
    }
    if (!loadAnimatedEnemies()) {
        throw std::runtime_error("Required bumstrum enemy rigs missing or invalid under assets/bumstrum/");
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
    meta_.load();   // Phase C: load the persistent profile (fresh if none) before the first run
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
        auto& m = enemyMeshes_[static_cast<size_t>(EnemyKind::Rusher)];
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
        auto& m = enemyMeshes_[static_cast<size_t>(EnemyKind::Ranged)];
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
        auto& m = enemyMeshes_[static_cast<size_t>(EnemyKind::Tank)];
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

    // ---- Stalker: lean ground hunter fallback for when the ghoul rig is absent.
    {
        auto& m = enemyMeshes_[static_cast<size_t>(EnemyKind::Stalker)];
        pushOcta(m, 0.58f, 0.24f, 0.34f, 0.18f, 0.22f);
        pushEye(m, 0.34f, 0.08f, 0.12f, 0.70f);
        pushTri(m, 0.18f, -0.08f, 0.18f, -0.24f, -0.12f, 0.26f, 0.46f, -0.62f, 0.46f, 2);
        pushTri(m, 0.18f, -0.08f, -0.18f, -0.24f, -0.12f, -0.26f, 0.46f, -0.62f, -0.46f, 2);
        pushTri(m, -0.22f, -0.02f, 0.18f, -0.62f, -0.06f, 0.26f, -0.34f, -0.54f, 0.58f, 2);
        pushTri(m, -0.22f, -0.02f, -0.18f, -0.62f, -0.06f, -0.26f, -0.34f, -0.54f, -0.58f, 2);
        pushTri(m, -0.42f, 0.04f, 0.0f, -0.78f, 0.22f, 0.0f, -0.52f, -0.20f, 0.0f, 0);
    }

    // Load the authored Blender drones. The procedural meshes above supply
    // shatter debris shards only; runtime rendering requires authored assets.
    const char* enemyObjPaths[EnemyKindCount] = {
        "assets/models/pulse_enemy_rusher.obj",
        "assets/models/pulse_enemy_ranged.obj",
        "assets/models/pulse_enemy_tank.obj",
        "assets/models/pulse_enemy_rusher.obj",
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
    case EnemyKind::Stalker:
        s.scale = 0.46f;
        s.body = rgb(138, 46, 42);
        s.wing = rgb(72, 35, 32);
        s.eye = rgb(255, 92, 66);
        break;
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
    loadStyleFromDisk(style_);   // W5: locked style library; missing file keeps built-in defaults. F5 hot-reloads it.
    // The config sensitivity is the base the user multiplier rides on; capture it before
    // applySettings overwrites tunables_, then re-apply so an F5 reload keeps user options.
    baseSensitivity_ = tunables_.mouseSensitivity;
    if (settingsActive_) applySettings();
    weaponProfiles_.loadFromDisk();
    if (announce) {
        build_.reloadContent(); // F5 also re-reads config/pulse.content (items/weapons), keeping stacks
        loadAllWeaponViewmodels();
    }
    if (result.loaded) {
        player_.hp = std::clamp(player_.hp, 0, std::max(1, effectiveMaxHealth()));
        player_.shield = std::clamp(player_.shield, 0, std::max(0, effectiveMaxShield()));
        if (player_.grounded) {
            player_.height = clamp(tunables_.eyeHeight, 0.12f, 0.88f);
        }
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

    // Hub: buy the first affordable not-owned unlock (so the verify exercises the
    // meta-unlock path), then START the run.
    if (phase_ == RunPhase::Hub) {
        const std::vector<UnlockDef>& u = meta_.unlockables();
        const int catCount = std::min(9, static_cast<int>(u.size()));
        const int startFocus = catCount + Meta::MirrorCount + 5;
        int targetFocus = -1;
        for (int i = 0; i < static_cast<int>(u.size()) && i < 9; ++i) {
            if (!meta_.isUnlocked(u[static_cast<size_t>(i)].id) && meta_.currency() >= u[static_cast<size_t>(i)].cost) {
                targetFocus = i;
                break;
            }
        }
        if (targetFocus < 0) targetFocus = startFocus;
        if (cardCursor_ == targetFocus) {
            input.keyPressed[VK_SPACE] = true;
            input.keyDown[VK_SPACE] = true;
        } else {
            input.keyPressed[VK_RIGHT] = true;
            input.keyDown[VK_RIGHT] = true;
        }
        return;
    }

    // Reward selection: prefer a weapon card when offered (so the verify exercises
    // every firing archetype), else take the first card. Deterministic either way.
    if (phase_ == RunPhase::RoomCleared) {
        int pick = 0;
        for (int i = 0; i < static_cast<int>(rewardOptions_.size()); ++i) {
            if (build_.describeReward(rewardOptions_[static_cast<size_t>(i)]).isWeapon) { pick = i; break; }
        }
        const char key = static_cast<char>('1' + pick);
        input.keyPressed[static_cast<size_t>(key)] = true;
        input.keyDown[static_cast<size_t>(key)] = true;
        return;
    }

    // Path choice: a deterministic policy so a seed reproduces a full automated run -
    // Cache when hurt (safe + free reward), Shop when scrap-rich (spend), Elite when
    // healthy (challenge + better loot), else the first option (usually Combat).
    if (phase_ == RunPhase::ChoosePath) {
        const std::vector<RoomSpec>& opts = run_.currentOptions();
        const int maxHp = std::max(1, effectiveMaxHealth());
        const bool hurt = player_.hp * 2 <= maxHp;
        const bool flush = scrap_ >= 40;
        int pick = 0;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
            const RoomType t = opts[static_cast<size_t>(i)].type;
            if (hurt && t == RoomType::Cache) { pick = i; break; }
            if (flush && t == RoomType::Shop) { pick = i; break; }
            if (!hurt && t == RoomType::Elite) { pick = i; break; }
        }
        const char key = static_cast<char>('1' + pick);
        input.keyPressed[static_cast<size_t>(key)] = true;
        input.keyDown[static_cast<size_t>(key)] = true;
        return;
    }

    // Shop: heal if hurt and affordable, else buy the best affordable unsold item (price
    // proxies tier), else leave. One action per frame; deterministic for the sim.
    if (phase_ == RunPhase::Shop) {
        const int maxHp = std::max(1, effectiveMaxHealth());
        const int healPrice = std::max(1, static_cast<int>(std::lround(24.0f * mods_.scrapMult)));
        if (player_.hp * 2 <= maxHp && player_.hp < maxHp && scrap_ >= healPrice) {
            input.keyPressed[static_cast<size_t>('H')] = true;
            input.keyDown[static_cast<size_t>('H')] = true;
            return;
        }
        int best = -1, bestPrice = -1;
        for (int i = 0; i < static_cast<int>(shopStock_.size()); ++i) {
            if (shopSold_[static_cast<size_t>(i)]) continue;
            if (scrap_ < shopPrices_[static_cast<size_t>(i)]) continue;
            if (shopPrices_[static_cast<size_t>(i)] > bestPrice) { bestPrice = shopPrices_[static_cast<size_t>(i)]; best = i; }
        }
        // M6: if nothing worth buying but flush with scrap, forge the active weapon (exercise the path).
        if (best < 0 && scrap_ >= 80) {
            input.keyPressed[static_cast<size_t>('F')] = true;
            input.keyDown[static_cast<size_t>('F')] = true;
            return;
        }
        const int key = best >= 0 ? ('1' + best) : VK_SPACE; // buy the best, else leave
        input.keyPressed[static_cast<size_t>(key)] = true;
        input.keyDown[static_cast<size_t>(key)] = true;
        return;
    }

    // Event/deal: accept the first deal when healthy (take the risk), else decline. This
    // exercises both the apply (curse RunModifiers) and the decline paths in the sim.
    if (phase_ == RunPhase::Event) {
        const int maxHp = std::max(1, effectiveMaxHealth());
        const bool healthy = player_.hp * 2 > maxHp;
        const int key = (healthy && !eventDeals_.empty()) ? '1' : VK_SPACE;
        input.keyPressed[static_cast<size_t>(key)] = true;
        input.keyDown[static_cast<size_t>(key)] = true;
        return;
    }
    // The play phases are the only ones the bot acts in; the rest auto-advance.
    if (phase_ != RunPhase::InRoom && phase_ != RunPhase::Boss) {
        return;
    }

    // Nearest enemy with line of sight (the firing target) and the nearest enemy
    // overall (the closest threat to kite/evade).
    const Enemy* target = nullptr; float bestLosSq = 1e9f;
    const Enemy* threat = nullptr; float bestSq = 1e9f;
    for (const Enemy& enemy : enemies_) {
        if (!enemy.active) continue;
        const float distSq = lengthSq(enemy.pos - player_.pos);
        if (distSq < bestSq) { bestSq = distSq; threat = &enemy; }
        if (lineOfSight(player_.pos, enemy.pos) && distSq < bestLosSq) {
            bestLosSq = distSq; target = &enemy;
        }
    }

    // Nearest incoming projectile (only those closing on us), for a dodge cue.
    float bestProjSq = 1e9f;
    for (const Projectile& p : projectiles_) {
        if (!p.active) continue;
        const Vec2 toUs = player_.pos - p.pos;
        if (dot(p.vel, toUs) <= 0.0f) continue; // travelling away
        const float distSq = lengthSq(toUs);
        if (distSq < bestProjSq) bestProjSq = distSq;
    }

    // Aim + fire at the LOS target (drag to counter the spray, like a player would).
    if (target) {
        const Vec2 toTarget = target->pos - player_.pos;
        const float desiredYaw = std::atan2(toTarget.y, toTarget.x) - recoilOffsetYaw_;
        const float yawError = angleDiff(desiredYaw, player_.yaw);
        const float desiredPitch = -0.015f - recoilOffsetPitch_;
        const float pitchError = player_.pitch - desiredPitch;
        const float sens = std::max(0.0001f, tunables_.mouseSensitivity);
        input.mouseDeltaX = static_cast<int>(clamp(yawError / sens, -90.0f, 90.0f));
        input.mouseDeltaY = static_cast<int>(clamp(pitchError / sens, -70.0f, 70.0f));
        input.mouseDown[0] = true;
        input.mousePressed[0] = true; // a press edge each frame so semi-auto weapons fire too
    } else {
        input.mouseDeltaX = 22; // sweep to find a target
    }

    // Movement: when healthy, stay aggressive in a tight mid band so DPS and LOS
    // stay up and waves clear fast; when low on health, kite to a wider stand-off.
    // Always orbit with a steady strafe that flips slowly so the bot keeps moving.
    const float threatDist = threat ? std::sqrt(bestSq) : 99.0f;
    const bool lowHp = static_cast<float>(player_.hp) < 0.4f * static_cast<float>(std::max(1, effectiveMaxHealth()));
    const bool strafeRight = ((static_cast<int>(elapsedSeconds * 0.45f) & 1) == 0);
    if (threat) {
        // Keep a healthy stand-off so the swarm cannot pile on melee hits, while staying
        // close enough to keep LOS + DPS up (the bot is a survivability probe, not a player).
        // M7: when hot (high Pulse), push the band IN - aggression keeps the Pulse fed and clears
        // faster, which is the intended playstyle and gives a cleaner balance signal.
        const float aggro = pulse_.meter01() > 0.45f ? 0.7f : 1.0f;
        const float nearBand = (lowHp ? 8.0f : 4.4f) * aggro;
        const float farBand = (lowHp ? 12.0f : 7.5f) * aggro;
        if (threatDist < nearBand) input.keyDown['S'] = true;            // back off
        else if (threatDist > farBand) input.keyDown['W'] = true;        // close in
        input.keyDown[strafeRight ? 'D' : 'A'] = true;                  // orbit
    } else {
        input.keyDown['W'] = true;
        input.keyDown[strafeRight ? 'D' : 'A'] = true;
    }

    const int frame = static_cast<int>(elapsedSeconds * 60.0f + 0.5f);
    const bool marketingDashBeat = marketingBot_ && threat && target && (frame % 90 == 24);
    if (marketingDashBeat) {
        if (threatDist < 5.2f) {
            input.keyDown['S'] = true;
            input.keyDown['W'] = false;
        } else if (threatDist > 8.5f) {
            input.keyDown['W'] = true;
            input.keyDown['S'] = false;
        }
        input.keyDown[strafeRight ? 'D' : 'A'] = true;
    }

    // Dash to dodge: a close incoming orb, a melee threat in our face, or low HP
    // under pressure. The dash carries the current movement keys set above, so a
    // backpedal+strafe slips the attack.
    const float projDashRange = marketingBot_ ? 6.2f : 4.0f;
    const float meleeDashRange = marketingBot_ ? 5.2f : 3.4f;
    const bool projDanger = bestProjSq < projDashRange * projDashRange;
    const bool meleeDanger = threat && threatDist < meleeDashRange;
    if (player_.dashCooldown <= 0.0f && (projDanger || meleeDanger || marketingDashBeat || (lowHp && threatDist < 6.0f))) {
        input.keyPressed[VK_SHIFT] = true;
        input.keyDown[VK_SHIFT] = true;
    }

    // Abilities (Phase B.3): lob a grenade at a threat in range; pop the ultimate
    // whenever it is charged and there is something to fight.
    if (tacticalCharge_ >= 1.0f && threat && threatDist < 12.0f) {
        input.keyPressed['G'] = true;
        input.keyDown['G'] = true;
    }
    if (ultimateCharge_ >= 1.0f && threat) {
        input.keyPressed['F'] = true;
        input.keyDown['F'] = true;
    }

    // Reload at a lull, and force it when nearly dry.
    const bool safe = threatDist > 7.0f;
    if (!weapon_.reloading && weapon_.reserve > 0 &&
        (weapon_.ammo <= 5 || (safe && weapon_.ammo < weaponMagazine() / 2))) {
        input.keyPressed['R'] = true;
        input.keyDown['R'] = true;
    }

    // Periodic hop so automated playtests exercise the jump/gravity path - and a second press a few
    // frames later while airborne so the DOUBLE JUMP path (+ its burst) is exercised too.
    const int jp = frame % 110;
    if (!marketingBot_ && jp < 20) {                // HOLD jump through the rise -> a full-height jump
        input.keyDown[VK_SPACE] = true;
        if ((jp == 0 && player_.grounded) || (jp == 9 && !player_.grounded))
            input.keyPressed[VK_SPACE] = true;      // ground-jump edge, then double-jump edge mid-air
    }
    // Occasionally cycle weapons so the loadout/swap path is exercised in verify.
    if (frame % 300 == 0 && loadout_.size() > 1 && !weapon_.reloading) {
        input.keyPressed['Q'] = true;
        input.keyDown['Q'] = true;
    }
    // M3: occasionally cycle the active weapon's aspect so the aspect path is exercised in the sim.
    if (frame % 420 == 90 && aspectsUnlocked() > 1) {
        input.keyPressed['X'] = true;
        input.keyDown['X'] = true;
    }
}

void PulseGame::debugBeginScriptedCapture() {
    scriptedDeterministic_ = true;
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    novaWaves_.clear();
    pickups_.clear();
    tracers_.clear();
    casings_.clear();
    impacts_.clear();
    bursts_.clear();
    debris_.clear();
    spawnTimer_ = 100000.0f;
    pickupSpawnTimer_ = 100000.0f;
    waveSpawnsLeft_ = 0;
    waveSpawnTimer_ = 0.0f;
    tunables_.spawnMaxConcurrent = 0;
    tunables_.pickupMaxActive = 0;
    weapon_.ammo = 999;
    weapon_.reserve = 0;
    weapon_.timeSinceShot = 100.0f;
    weapon_.reloading = false;
    weapon_.reloadMagOutPlayed = false;
    weapon_.reloadInsertPlayed = false;
    weapon_.reloadEndPlayed = false;
    weapon_.reloadRemaining = 0.0f;
    combatIntensity_ = 0.0f;
    hitStopTimer_ = 0.0f;
}

void PulseGame::debugPose() {
    debugBeginScriptedCapture(); // posed capture: no autonomous wave spawning
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    novaWaves_.clear();
    waveSpawnsLeft_ = 0;
    waveSpawnTimer_ = 0.0f;
    const auto faceMostOpen = [&]() {
        float bestYaw = player_.yaw;
        float bestOpen = -1.0f;
        for (int i = 0; i < 32; ++i) {
            const float a = -Pi + TwoPi * static_cast<float>(i) / 32.0f;
            const Vec2 f = fromAngle(a);
            float open = 0.0f;
            for (float d = 0.8f; d <= 14.0f; d += 0.35f) {
                const Vec2 p = player_.pos + f * d;
                if (collides(p, 0.34f) || !lineOfSight(player_.pos, p)) {
                    break;
                }
                open = d;
            }
            if (open > bestOpen) {
                bestOpen = open;
                bestYaw = a;
            }
        }
        player_.yaw = bestYaw;
    };
    faceMostOpen(); // face open room space so captures show the arena, not a wall
    if (!forcedRoomName_.empty()) {
        decals_.clear();
        particles_.clear();
        return;
    }
    const Vec2 fwd = fromAngle(player_.yaw);
    const Vec2 rt = rightFromForward(fwd);
    const float side = 1.9f;
    const float nearDist = 3.0f;
    const float farDist = 5.2f;
    const auto posePoint = [&](float forward, float sideOffset) {
        return player_.pos + fwd * forward + rt * sideOffset;
    };
    Enemy a;
    a.kind = EnemyKind::Rusher;
    a.visual = static_cast<int>(EnemyVisual::Rusher001);
    a.pos = posePoint(nearDist, -side * 1.1f);
    a.health = tunables_.enemyMaxHealth;
    enemies_.push_back(a);
    Enemy b;
    b.kind = EnemyKind::Ranged;
    b.visual = static_cast<int>(EnemyVisual::Gunner002);
    b.pos = posePoint(nearDist + 0.25f, -side * 0.35f);
    b.health = tunables_.enemyMaxHealth;
    enemies_.push_back(b);
    Enemy d;
    d.kind = EnemyKind::Ranged;
    d.visual = static_cast<int>(EnemyVisual::Drone005);
    d.pos = posePoint(nearDist + 0.50f, side * 0.45f);
    d.health = tunables_.enemyMaxHealth;
    d.maxHealth = d.health;
    enemies_.push_back(d);
    Enemy u;
    u.kind = EnemyKind::Ranged;
    u.visual = static_cast<int>(EnemyVisual::Summoner014);
    u.pos = posePoint(nearDist + 0.72f, side * 1.25f);
    u.health = tunables_.enemyMaxHealth;
    u.maxHealth = u.health;
    enemies_.push_back(u);
    Enemy s;
    s.kind = EnemyKind::Stalker;
    s.visual = static_cast<int>(EnemyVisual::Stalker004);
    s.pos = posePoint(farDist + 0.12f, -side * 0.50f);
    s.health = tunables_.enemyMaxHealth * 0.9f;
    s.maxHealth = s.health;
    enemies_.push_back(s);
    Enemy c;
    c.kind = EnemyKind::Tank;
    c.visual = static_cast<int>(EnemyVisual::Tank003);
    c.pos = posePoint(farDist + 0.35f, side * 0.40f);
    c.health = tunables_.enemyMaxHealth * tunables_.enemyTankHealthMult;
    enemies_.push_back(c);

    // Seed a few decals on the floor in front so the headless --pose still exercises
    // and shows the M2 decal pass (scorch + bullet marks).
    decals_.clear();
    const Vec2 scorch = player_.pos + fwd * 4.6f;
    spawnDecal({ scorch.x, 0.015f, scorch.y }, { 0, 1, 0 }, 1u, 0.55f, { 0.015f, 0.012f, 0.010f }, 0.85f);
    for (int i = 0; i < 7; ++i) {
        const Vec2 p = player_.pos + fwd * (3.4f + 0.42f * static_cast<float>(i)) +
                       rt * (1.0f - 0.3f * static_cast<float>(i));
        spawnDecal({ p.x, 0.012f, p.y }, { 0, 1, 0 }, 0u, 0.16f, { 0.03f, 0.027f, 0.024f }, 0.92f);
    }
    // Seed a particle burst so the headless --pose still shows the M2 particle pass.
    particles_.clear();
    const Vec2 burst = player_.pos + fwd * 3.0f;
    spawnParticles({ burst.x, 0.42f, burst.y }, { 0.0f, 0.0f, 0.0f }, 24,
                   0.18f, 0.6f, 0.07f, { 1.3f, 0.6f, 0.25f }, 7.0f);
}

void PulseGame::debugEnemyTracerPose() {
    debugPose();                 // orient the player into open arena space
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    beams_.clear();
    particles_.clear();
    tracers_.clear();
    impacts_.clear();
    heatPulses_.clear();

    const Vec2 fwd = fromAngle(player_.yaw);
    const Vec2 rt = rightFromForward(fwd);
    const Vec2 enemyPos = player_.pos + fwd * 4.2f + rt * 0.25f;

    Enemy e;
    e.kind = EnemyKind::Ranged;
    e.pos = enemyPos;
    e.health = tunables_.enemyMaxHealth;
    e.maxHealth = e.health;
    e.pendingRanged = true;
    e.pendingAttack = EnemyAttack::Orb;
    e.telegraphRemaining = 0.18f;
    enemies_.push_back(e);

    Projectile p;
    p.hostile = true;
    p.origin = enemyPos;
    p.pos = player_.pos + fwd * 2.25f + rt * 0.08f;
    p.vel = fwd * -std::max(3.5f, tunables_.enemyProjectileSpeed);
    p.height = 0.68f;
    p.life = 2.5f;
    p.damage = tunables_.enemyRangedDamage;
    p.color = enemyShotColor(EnemyKind::Ranged, EnemyAttack::Orb, false, 0);
    p.shape = enemyShotShape(EnemyKind::Ranged, EnemyAttack::Orb, false);
    p.sourceKind = EnemyKind::Ranged;
    p.sourceAttack = EnemyAttack::Orb;
    projectiles_.push_back(p);
}

void PulseGame::debugRewardScreen() {
    // Force the time-stopped reward-choice state for a headless HUD capture, with a
    // small seeded build so OWNED/ITEMS read too.
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    build_.clear();
    build_.add("carbine_rounds", 2);
    build_.add("incendiary_rounds", 2);   // 2 Pyro owned -> the set-progress reads 2/5 on a Pyro card
    // Mixed options: a Pyro element passive (set progress + synergy), a Rare Pyro legendary-ish, a weapon.
    rewardOptions_ = { "p:incendiary_rounds", "p:thermal_lance", "w:railbolt" };
    phase_ = RunPhase::RoomCleared;
    phaseTimer_ = 1.0e9f;
}

void PulseGame::debugHubScreen() {
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    meta_.resetToFresh();
    meta_.addCurrency(3021);               // leaves exactly 2,811 after the prototype-owned state below
    meta_.buy("w:marksman");
    meta_.buy("w:railbolt");
    meta_.buy("p:frag_payload");
    meta_.buy("p:arc_conductor");
    meta_.buy("p:volatile_rounds");
    meta_.upgradeMirror(Meta::MirrorVigor);
    meta_.upgradeMirror(Meta::MirrorVigor);
    meta_.upgradeMirror(Meta::MirrorVigor);
    meta_.upgradeMirror(Meta::MirrorMomentum);
    meta_.unlockHeat(4);                   // show the heat selector populated
    meta_.setHeat(2);
    lastPayout_ = 18;
    phase_ = RunPhase::Hub;
}

void PulseGame::debugCodexScreen(int tab) {
    debugHubScreen();        // a populated hub behind it (so the manual reads in context)
    codexOpen_ = true;       // overlay the SYSTEMS field manual
    codexTab_ = std::clamp(tab, 0, 8);   // QA: capture a specific manual tab
}

void PulseGame::debugMenuScreen(const std::string& which) {
    // Force a front-end screen for a headless capture (the AI vision-verify loop). Enables
    // the shell, loads the saved options, and parks a valid world behind the overlay.
    scriptedDeterministic_ = true;
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    frontEnd_ = true;
    settings_.fovDegrees = tunables_.cameraFovDegrees;
    settings_.load();
    settingsActive_ = true;
    applySettings();
    if (which == "pause") {
        beginRun();                       // build a valid in-run state to sit behind the pause HUD
        phase_ = RunPhase::InRoom;
        menuScreen_ = MenuScreen::Pause;
    } else if (which == "options" || which == "settings" || which == "video") {
        phase_ = RunPhase::Hub;
        settingsReturn_ = MenuScreen::Main;
        menuScreen_ = MenuScreen::Settings;
        menuTab_ = (which == "video") ? 2 : 3;   // VIDEO tab on request, else ACCESSIBILITY
    } else {
        phase_ = RunPhase::Hub;
        menuScreen_ = MenuScreen::Main;
    }
    menuSel_ = 0;
}

void PulseGame::debugPathScreen() {
    // Force the ChoosePath state for a headless capture: build a run, then advance to the
    // first branching step so currentOptions() is a real 3-way choice.
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    beginRun();            // build steps_ + enter the fixed opener
    run_.advanceRoom();    // step to the first choice (3 options, unchosen)
    phase_ = RunPhase::ChoosePath;
    phaseTimer_ = 1.0e9f;
}

void PulseGame::debugDoorsScreen() {
    // Force the spatial DoorsOpen state for a headless capture: build a run, enter the opener
    // (its arena + doors are generated by the preload frame), then clear it so the exits open
    // with their reward previews. Hold the phase (no auto-resolve) so the frame captures the doors.
    scriptedDeterministic_ = true;
    autoResolveDoors_ = false;
    enemies_.clear();
    projectiles_.clear();
    AudioSystem silent(false);
    beginRun();              // build steps_ + enter the opener (regenerates the arena + its doors)
    enterDoorsOpen(silent);  // advance the run, bind + open the exit doors with reward previews
    // Stand inside the room facing the first open exit so the capture shows the sliding door
    // (leaves retracted when open) and the connecting corridor receding behind it (no sky).
    if (!doors_.empty()) {
        const Door& ed = wasteland_.doors()[static_cast<size_t>(doors_[0].envDoorIndex)];
        player_.pos = { ed.worldX + ed.inwardX * 1.45f, ed.worldZ + ed.inwardZ * 1.45f };
        player_.yaw = std::atan2(-ed.inwardZ, -ed.inwardX);   // look back at the doorway
        player_.pitch = 0.08f;
    }
    // Snap the slide animation to target so the still shows the cleared exits fully open (the
    // live game eases this over ~0.5s; a headless capture takes no update steps).
    doorAnim_.assign(static_cast<size_t>(std::max(0, wasteland_.doorCount())), 0.0f);
    for (const DoorBind& b : doors_)
        if (b.envDoorIndex >= 0 && b.envDoorIndex < static_cast<int>(doorAnim_.size()))
            doorAnim_[static_cast<size_t>(b.envDoorIndex)] = 1.0f;   // cleared exits shown fully open
    phaseTimer_ = 1.0e9f;
}

void PulseGame::debugShopScreen() {
    // Force the Shop state for a headless capture: build a run, grant some scrap, and roll
    // the stock; leave the player hurt so the HEAL service reads as available too.
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    beginRun();
    scrap_ = 75;
    player_.hp = std::max(1, effectiveMaxHealth() * 2 / 3);
    enterShop();           // phase = Shop + roll the stock
    phaseTimer_ = 1.0e9f;
}

void PulseGame::debugEventScreen() {
    // Force the Event (deal) state for a headless capture: build a run, roll the deals.
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    beginRun();
    enterEvent();          // phase = Event + roll 2 deals
    phaseTimer_ = 1.0e9f;
}

void PulseGame::debugRunOverScreen() {
    // Force the Run Report (RunOver) state for a headless capture: build a run, advance a
    // few rooms, bank a payout, and park in a (lost) RunOver dwell so the report holds.
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    beginRun();
    run_.advanceRoom();
    run_.advanceRoom();
    build_.add("carbine_rounds", 3);
    build_.add("hollow_points", 2);
    score_ = 18450;
    if (score_ > bestScore_) bestScore_ = score_;
    meta_.unlockHeat(4);
    meta_.setHeat(2);
    lastPayout_ = 96;
    runWon_ = false;
    phase_ = RunPhase::RunOver;
    restartTimer_ = 1.0e9f;
}

void PulseGame::debugStandOnCover() {
    // Capture proof for the jump-onto-geometry feature: find a low, mountable cover cell in
    // the loaded arena, stand the player on top of it, and look down so the elevation reads.
    scriptedDeterministic_ = true;
    enemies_.clear();
    projectiles_.clear();
    phase_ = RunPhase::InRoom;
    if (!procEnvReady()) return;
    const float eyeBase = clamp(tunables_.eyeHeight, 0.12f, 0.88f);
    const int s = procEnvSub();
    const float cxArena = 16.0f, czArena = 12.0f; // arena centre (spawn)
    float bestH = -1.0f, bestScore = 1e9f; int bestFx = -1, bestFz = -1;
    for (int fz = 2 * s; fz < 22 * s; ++fz)
        for (int fx = 2 * s; fx < 30 * s; ++fx) {
            if (!procEnvSolidFine(fx, fz)) continue;
            const float h = procEnvFineHeight(fx, fz);
            if (h < 0.35f || h > 1.3f) continue;     // mountable with the current jump
            const float wx = (static_cast<float>(fx) + 0.5f) / s, wz = (static_cast<float>(fz) + 0.5f) / s;
            const float d = (wx - cxArena) * (wx - cxArena) + (wz - czArena) * (wz - czArena);
            if (d < bestScore) { bestScore = d; bestH = h; bestFx = fx; bestFz = fz; }
        }
    if (bestFx < 0) return;   // no mountable cover in this arena/seed
    player_.pos = { (static_cast<float>(bestFx) + 0.5f) / s, (static_cast<float>(bestFz) + 0.5f) / s };
    player_.vel = {};
    player_.height = bestH + eyeBase;   // standing on the cover top
    player_.vz = 0.0f;
    player_.grounded = true;
    player_.pitch = -0.55f;             // look down at the cover top + ground
    renderViewPitch_ = player_.pitch;
    renderViewYaw_ = player_.yaw;
}

void PulseGame::debugBossPose() {
    scriptedDeterministic_ = true;
    phase_ = RunPhase::Boss;
    // Seed a small affinity build so the combat HUD's affinity-set readout shows in captures.
    build_.add("incendiary_rounds", 3);   // Pyro 3-set (lit)
    build_.add("static_charge", 1);       // Volt 1 (dim)
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    novaWaves_.clear();
    waveSpawnsLeft_ = 0;
    waveSpawnTimer_ = 0.0f;
    const Vec2 fwd = fromAngle(player_.yaw);
    const Vec2 rt = rightFromForward(fwd);
    const auto posePoint = [&](float forward, float sideOffset) {
        return player_.pos + fwd * forward + rt * sideOffset;
    };
    Enemy boss;
    boss.boss = true;
    boss.kind = EnemyKind::Tank;
    boss.pos = posePoint(6.0f, 0.0f);
    boss.health = tunables_.enemyMaxHealth * 10.0f * 0.62f; // partly whittled, so the HP bar reads
    boss.maxHealth = tunables_.enemyMaxHealth * 10.0f;
    boss.telegraphRemaining = 0.4f;                          // mid-tell flare
    boss.struck = true;
    enemies_.push_back(boss);
    // Elite escorts show the full regular roster around the boss.
    const EliteAffix affixes[] = { EliteAffix::Shielded, EliteAffix::Volatile, EliteAffix::Fast, EliteAffix::Regen };
    const EnemyKind kinds[] = { EnemyKind::Rusher, EnemyKind::Stalker, EnemyKind::Ranged, EnemyKind::Rusher };
    for (int i = 0; i < 4; ++i) {
        Enemy add;
        add.kind = kinds[i];
        add.affix = affixes[i];
        const float side = 1.65f;
        add.pos = posePoint(3.4f + 0.18f * static_cast<float>(i),
                            (static_cast<float>(i) - 1.5f) * side);
        add.health = tunables_.enemyMaxHealth * (add.kind == EnemyKind::Stalker ? 0.9f : 1.0f);
        add.maxHealth = add.health;
        enemies_.push_back(add);
    }
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

void PulseGame::debugSeedShotElements(const std::string& spec) {
    // Add one element infusion per requested keyword so activeShotEffectMask() lights the right
    // bits; a single item per element is enough to turn the system ON (deeper stacks only scale it).
    const bool all = spec.find("all") != std::string::npos;
    if (all || spec.find("burn")    != std::string::npos) build_.add("incendiary_rounds", 1);
    if (all || spec.find("shock")   != std::string::npos) build_.add("tesla_coil", 1);
    if (all || spec.find("cryo")    != std::string::npos) build_.add("cryo_rounds", 1);
    if (all || spec.find("corrode") != std::string::npos) build_.add("corrosive_rounds", 1);
}

void PulseGame::debugForceWeapon(const std::string& id) {
    if (!build_.findWeapon(id)) {
        return;
    }
    const WeaponProfile& profile = weaponProfileForId(id);
    if (!profile.viewmodel.authored || !weaponProfiles_.find(id)) {
        return;
    }
    loadout_.clear();
    WeaponSlot slot;
    slot.id = id;
    slot.power = 1;
    const int mag = profile.magazine > 0 ? profile.magazine : tunables_.weaponMagazineCapacity;
    const int reserve = profile.reserve > 0 ? profile.reserve : mag * 3;
    slot.savedAmmo = Weapon{};
    slot.savedAmmo.ammo = std::max(1, mag);
    slot.savedAmmo.reserve = std::max(0, reserve);
    slot.savedAmmo.timeSinceShot = 99.0f;
    loadout_.push_back(slot);
    activeWeapon_ = 0;
    weapon_ = slot.savedAmmo;
}

void PulseGame::debugReloadPose(float progress) {
    enemies_.clear();
    pickups_.clear();
    tracers_.clear();
    casings_.clear();
    impacts_.clear();
    projectiles_.clear();
    particles_.clear();
    weapon_.ammo = std::max(0, weaponMagazine() - std::max(1, weaponMagazine() / 3));
    weapon_.reserve = std::max(1, weaponReserveMax());
    weapon_.timeSinceShot = 100.0f;
    weapon_.reloading = true;
    weapon_.reloadMagOutPlayed = progress >= 0.28f;
    weapon_.reloadInsertPlayed = progress >= 0.62f;
    weapon_.reloadEndPlayed = progress >= 0.90f;
    weapon_.reloadRemaining = reloadDuration() * (1.0f - clamp(progress, 0.0f, 1.0f));
    weaponKick_ = 0.0f;
    weaponKickSide_ = 0.0f;
    recoilOffsetPitch_ = 0.0f;
    recoilOffsetYaw_ = 0.0f;
}

int PulseGame::activeEnemyCount() const {
    return static_cast<int>(std::count_if(enemies_.begin(), enemies_.end(), [](const Enemy& enemy) {
        return enemy.active;
    }));
}

bool PulseGame::runWeaponSelfTest(std::string& report) const {
    std::ostringstream out;
    bool ok = true;
    out << "[weapon-test] profiles=" << weaponProfiles_.profiles().size() << "\n";
    for (const WeaponDef& def : build_.weaponCatalog()) {
        const WeaponProfile* profile = weaponProfiles_.find(def.id);
        if (!profile) {
            ok = false;
            out << "[weapon-test] " << def.id << " FAIL missing profile\n";
            continue;
        }
        bool pass = true;
        if (profile->damage <= 0.0f || profile->fireRate <= 0.0f || profile->magazine <= 0 || profile->reloadSeconds <= 0.0f) {
            pass = false;
        }
        if (profile->rewardEligible || def.id == "pistol") {
            bool asset = false;
            bool fireBank = false;
            bool reloadBanks = true;
            for (const char* prefix : { "", "../", "../../" }) {
                if (std::filesystem::exists(std::string(prefix) + profile->viewmodel.assetPath)) { asset = true; break; }
            }
            for (const char* prefix : { "", "../", "../../" }) {
                if (std::filesystem::exists(std::string(prefix) + "assets/audio/sfx_fire_" + def.id + ".wav")) {
                    fireBank = true;
                    break;
                }
            }
            for (const char* eventName : { "dry", "equip", "reload_start", "mag_in", "reload_end" }) {
                bool eventBank = false;
                for (const char* prefix : { "", "../", "../../" }) {
                    if (std::filesystem::exists(std::string(prefix) + "assets/audio/sfx_weapon_" + def.id + "_" + eventName + ".wav")) {
                        eventBank = true;
                        break;
                    }
                }
                reloadBanks = reloadBanks && eventBank;
            }
            if (profile->reloadMode == WeaponReloadMode::PerShell) {
                bool shellBank = false;
                for (const char* prefix : { "", "../", "../../" }) {
                    if (std::filesystem::exists(std::string(prefix) + "assets/audio/sfx_weapon_" + def.id + "_shell.wav")) {
                        shellBank = true;
                        break;
                    }
                }
                reloadBanks = reloadBanks && shellBank;
            }
            pass = pass && profile->viewmodel.authored && !profile->viewmodel.assetPath.empty() && asset && fireBank && reloadBanks;
        }
        ok = ok && pass;
        out << "[weapon-test] " << def.id << " " << (pass ? "PASS" : "FAIL")
            << " archetype=" << weaponArchetypeName(profile->archetype)
            << " reward=" << (weaponProfiles_.rewardEligible(def.id) || def.id == "pistol" ? "live" : "locked")
            << " vm=" << (profile->viewmodel.authored ? profile->viewmodel.assetPath : "MISSING-AUTHORED-ASSET")
            << "\n";
    }
    // Fire-bank coverage for EVERY profile, not just reward-eligible ones. A defined
    // weapon with no fire bank fires silently (playFire returns on a missing named
    // bank), which violates the fail-loud rule. This catches locked/future weapons
    // (e.g. railbolt, machine_pistol) before they can ever ship mute.
    for (const WeaponProfile& p : weaponProfiles_.profiles()) {
        bool fireBank = false;
        for (const char* prefix : { "", "../", "../../" }) {
            if (std::filesystem::exists(std::string(prefix) + "assets/audio/sfx_fire_" + p.id + ".wav")) {
                fireBank = true;
                break;
            }
        }
        if (!fireBank) {
            ok = false;
            out << "[weapon-test] " << p.id << " FAIL missing fire bank assets/audio/sfx_fire_"
                << p.id << ".wav\n";
        }
    }

    report = out.str();
    return ok;
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

bool PulseGame::loadAnimatedWeaponViewmodel(const WeaponProfile& profile) {
    if (!profile.viewmodel.authored || profile.viewmodel.assetPath.empty()) {
        return true; // Designed but not live; reward gating keeps it out of play.
    }
    AnimatedViewmodelRuntime runtime;
    runtime = AnimatedViewmodelRuntime{};
    if (!runtime.model.load(resolveAsset(profile.viewmodel.assetPath))) {
        return false;
    }
    const int idleClip = runtime.model.clipIndex(profile.viewmodel.idleClip);
    const int fireClip = runtime.model.clipIndex(profile.viewmodel.fireClip);
    const int reloadClip = runtime.model.clipIndex(profile.viewmodel.reloadClip);
    if (idleClip < 0) {
        return false;
    }
    runtime.idleClip = idleClip;
    runtime.fireClip = fireClip >= 0 && profile.viewmodel.fireEnd > profile.viewmodel.fireStart ? fireClip : -1;
    runtime.reloadClip = reloadClip >= 0 ? reloadClip : idleClip;
    runtime.idleStart = profile.viewmodel.idleStart;
    runtime.idleEnd = profile.viewmodel.idleEnd;
    runtime.fireStart = profile.viewmodel.fireStart;
    runtime.fireEnd = profile.viewmodel.fireEnd;
    runtime.reloadStart = profile.viewmodel.reloadStart;
    runtime.reloadEnd = profile.viewmodel.reloadEnd;
    runtime.idleDampScale = profile.viewmodel.idleDampScale;
    runtime.hideSupportHandUntilReload = false;
    runtime.model.sample(idleClip, runtime.idleStart, runtime.neutralIdle);
    runtime.sampled = runtime.neutralIdle;
    runtime.loaded = !runtime.sampled.empty();
    if (!runtime.loaded) {
        return false;
    }
    weaponViewmodels_[profile.id] = std::move(runtime);
    return true;
}

bool PulseGame::loadAllWeaponViewmodels() {
    weaponViewmodels_.clear();
    bool ok = true;
    for (const WeaponProfile& profile : weaponProfiles_.profiles()) {
        ok = loadAnimatedWeaponViewmodel(profile) && ok;
    }
    return ok;
}

bool PulseGame::ensureAnimatedViewmodelResources(AnimatedViewmodelRuntime& runtime, Engine& engine) {
    if (!runtime.loaded) return false;
    if (runtime.gpuReady) return true;

    const auto loadTextureFile = [&](const std::string& path, bool srgb) -> TextureHandle {
        if (path.empty()) return TextureHandle::Invalid;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba = loadImageRGBA(path, w, h);
        if (rgba.empty()) return TextureHandle::Invalid;
        TextureData td;
        td.width = w;
        td.height = h;
        td.rgba = rgba.data();
        td.srgb = srgb;
        return engine.createTexture(td);
    };
    const auto loadMetallicRoughness = [&](const std::string& path) -> TextureHandle {
        if (path.empty()) return TextureHandle::Invalid;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba = loadImageRGBA(path, w, h);
        if (rgba.empty()) return TextureHandle::Invalid;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            rgba[i + 0] = 255;          // glTF MR has no AO channel; shader expects ORM.r = AO
            rgba[i + 3] = 255;
        }
        TextureData td;
        td.width = w;
        td.height = h;
        td.rgba = rgba.data();
        td.srgb = false;
        return engine.createTexture(td);
    };

    runtime.materials.clear();
    runtime.materials.reserve(runtime.model.materials().size());
    for (const AnimatedGltfMaterial& src : runtime.model.materials()) {
        MaterialDesc d;
        d.baseColor = loadTextureFile(src.baseColorTexture, true);
        d.normal = loadTextureFile(src.normalTexture, false);
        d.orm = loadMetallicRoughness(src.metallicRoughnessTexture);
        d.emissiveTex = loadTextureFile(src.emissiveTexture, true);
        if (d.emissiveTex != TextureHandle::Invalid) d.emissiveTexStrength = 2.5f;
        d.baseColorFactor = src.baseColorFactor;
        const float em = std::max(src.emissiveFactor.x, std::max(src.emissiveFactor.y, src.emissiveFactor.z));
        if (em > 0.02f && d.emissiveTex == TextureHandle::Invalid) d.emissive = em * 1.8f;
        d.metallic = src.metallicFactor;
        d.roughness = src.roughnessFactor;
        d.metalScale = 1.0f;
        d.roughBoost = 0.0f;
        runtime.materials.push_back(engine.createMaterial(d));
    }

    const std::vector<AnimatedGltfSubmesh>& bind = runtime.model.submeshes();
    runtime.meshes.clear();
    runtime.meshes.reserve(bind.size());
    for (const AnimatedGltfSubmesh& sm : bind) {
        MeshHandle h = engine.createDynamicMesh(static_cast<uint32_t>(sm.vertices.size()), sm.indices);
        if (h == MeshHandle::Invalid || !engine.updateDynamicMesh(h, sm.vertices)) {
            return false;
        }
        runtime.meshes.push_back(h);
    }

    runtime.gpuReady = !runtime.meshes.empty();
    return runtime.gpuReady;
}

bool PulseGame::ensureActiveWeaponViewmodelResources(Engine& engine) {
    AnimatedViewmodelRuntime* runtime = activeViewmodelRuntime();
    // A weapon with no authored viewmodel (a designed-but-not-live entry that ends up active, e.g.
    // an un-wired starting weapon) renders WITHOUT arms rather than crashing the game - the draw
    // path already guards a null runtime. This used to be fatal, which crashed launch whenever the
    // active weapon had no viewmodel.
    if (!runtime) return true;
    return ensureAnimatedViewmodelResources(*runtime, engine);
}

void PulseGame::updateAnimatedViewmodel(AnimatedViewmodelRuntime& runtime, Engine& engine) {
    if (!runtime.loaded || !runtime.gpuReady) return;

    auto durationOf = [](float start, float end, float fallback) {
        return std::max(0.001f, end > start ? end - start : fallback);
    };
    int clip = runtime.idleClip;
    float clipTime = 0.0f;
    float duration = durationOf(runtime.idleStart, runtime.idleEnd, runtime.model.clipDuration(clip));
    bool reloadClipActive = false;
    bool idleClipActive = true;
    float reloadProgress = 0.0f;
    if (duration > 0.0f) {
        clipTime = runtime.idleStart + std::fmod(std::max(0.0f, shakeTime_), duration);
    }

    if (weapon_.reloading && runtime.reloadClip >= 0) {
        clip = runtime.reloadClip;
        duration = durationOf(runtime.reloadStart, runtime.reloadEnd, runtime.model.clipDuration(clip));
        reloadClipActive = true;
        idleClipActive = false;
        reloadProgress = 1.0f - clamp(weapon_.reloadRemaining / reloadDuration(), 0.0f, 1.0f);
        clipTime = runtime.reloadStart + clamp(reloadProgress, 0.0f, 1.0f) * duration;
    } else if (runtime.fireClip >= 0 &&
               weapon_.timeSinceShot < durationOf(runtime.fireStart, runtime.fireEnd,
                                                  runtime.model.clipDuration(runtime.fireClip))) {
        clip = runtime.fireClip;
        idleClipActive = false;
        duration = durationOf(runtime.fireStart, runtime.fireEnd, runtime.model.clipDuration(clip));
        clipTime = runtime.fireStart + clamp(weapon_.timeSinceShot, 0.0f, duration);
    }

    if (idleClipActive) {
        runtime.sampled = runtime.neutralIdle;
    } else {
        runtime.model.sample(clip, clipTime, runtime.sampled);
    }
    if (runtime.hideSupportHandUntilReload) {
        const bool showSupportHand = reloadClipActive && reloadProgress >= 0.64f;
        if (!showSupportHand) {
            offsetAnimatedTaggedVertices(runtime.sampled, 1, { -0.08f, -1.12f, -0.24f });
        }
    }
    const size_t n = std::min(runtime.sampled.size(), runtime.meshes.size());
    for (size_t i = 0; i < n; ++i) {
        engine.updateDynamicMesh(runtime.meshes[i], runtime.sampled[i].vertices);
    }
}

void PulseGame::updateActiveWeaponViewmodel(Engine& engine) {
    if (AnimatedViewmodelRuntime* runtime = activeViewmodelRuntime()) {
        updateAnimatedViewmodel(*runtime, engine);
    }
}

PulseGame::AnimatedViewmodelRuntime* PulseGame::activeViewmodelRuntime() {
    const WeaponProfile& profile = activeWeaponProfile();
    auto it = weaponViewmodels_.find(profile.id);
    if (it != weaponViewmodels_.end()) return &it->second;
    return nullptr;
}

const PulseGame::AnimatedViewmodelRuntime* PulseGame::activeViewmodelRuntime() const {
    const WeaponProfile& profile = activeWeaponProfile();
    auto it = weaponViewmodels_.find(profile.id);
    if (it != weaponViewmodels_.end()) return &it->second;
    return nullptr;
}

Vec3f PulseGame::animatedViewmodelMuzzle(const AnimatedViewmodelRuntime& runtime, const Mat4& viewmodelXf,
                                         const char* preferredA, const char* preferredB) const {
    const auto findPreferred = [&](const char* preferred) -> const AnimatedGltfSubmesh* {
        if (!preferred) return nullptr;
        for (const AnimatedGltfSubmesh& sm : runtime.sampled) {
            if (sm.name.find(preferred) != std::string::npos) {
                return &sm;
            }
        }
        return nullptr;
    };
    if (const AnimatedGltfSubmesh* sm = findPreferred(preferredA)) {
        return barrelEndFaceMuzzleCamera(sm->vertices, sm->indices, viewmodelXf);
    }
    if (const AnimatedGltfSubmesh* sm = findPreferred(preferredB)) {
        return barrelEndFaceMuzzleCamera(sm->vertices, sm->indices, viewmodelXf);
    }
    if (!runtime.sampled.empty()) {
        const AnimatedGltfSubmesh& sm = runtime.sampled.front();
        return barrelEndFaceMuzzleCamera(sm.vertices, sm.indices, viewmodelXf);
    }
    return { 0, 0, 1 };
}

Vec3f PulseGame::activeWeaponMuzzle(const WeaponProfile& profile, const Mat4& viewmodelXf) const {
    if (const AnimatedViewmodelRuntime* runtime = activeViewmodelRuntime()) {
        return animatedViewmodelMuzzle(*runtime, viewmodelXf,
                                       profile.viewmodel.muzzleA.c_str(),
                                       profile.viewmodel.muzzleB.c_str());
    }
    return { 0, 0, 1 };
}

PulseGame::AnimatedEnemyModel& PulseGame::bossModel(int bossKind) {
    const int idx = std::clamp(bossKind, 0, BossKindCount - 1);
    return bossModels_[static_cast<size_t>(idx)];
}

const PulseGame::AnimatedEnemyModel& PulseGame::bossModel(int bossKind) const {
    const int idx = std::clamp(bossKind, 0, BossKindCount - 1);
    return bossModels_[static_cast<size_t>(idx)];
}

int PulseGame::defaultEnemyVisual(EnemyKind kind) const {
    switch (kind) {
        case EnemyKind::Rusher:  return static_cast<int>(EnemyVisual::Rusher001);
        case EnemyKind::Ranged:  return static_cast<int>(EnemyVisual::Gunner002);
        case EnemyKind::Tank:    return static_cast<int>(EnemyVisual::Tank003);
        case EnemyKind::Stalker: return static_cast<int>(EnemyVisual::Stalker004);
    }
    return static_cast<int>(EnemyVisual::Rusher001);
}

int PulseGame::chooseEnemyVisual(EnemyKind kind, EliteAffix affix, uint32_t salt) const {
    salt ^= salt >> 16;
    salt *= 0x7feb352du;
    salt ^= salt >> 15;
    salt *= 0x846ca68bu;
    salt ^= salt >> 16;

    switch (affix) {
        case EliteAffix::Fast:     return static_cast<int>(EnemyVisual::Fast009);
        case EliteAffix::Shielded: return static_cast<int>(EnemyVisual::Shielded010);
        case EliteAffix::Volatile: return static_cast<int>(EnemyVisual::Volatile011);
        case EliteAffix::Regen:    return static_cast<int>(EnemyVisual::Regen012);
        case EliteAffix::None: default: break;
    }

    switch (kind) {
        case EnemyKind::Rusher:
            return (salt % 5u == 0u) ? static_cast<int>(EnemyVisual::Husk013)
                                     : static_cast<int>(EnemyVisual::Rusher001);
        case EnemyKind::Ranged:
            switch (salt % 5u) {
                case 1: return static_cast<int>(EnemyVisual::Drone005);
                case 2: return static_cast<int>(EnemyVisual::Summoner014);
                case 3: return static_cast<int>(EnemyVisual::Choir008);
                default: return static_cast<int>(EnemyVisual::Gunner002);
            }
        case EnemyKind::Tank:
            return (salt % 3u == 0u) ? static_cast<int>(EnemyVisual::Warden006)
                                     : static_cast<int>(EnemyVisual::Tank003);
        case EnemyKind::Stalker:
            switch (salt % 4u) {
                case 1: return static_cast<int>(EnemyVisual::Choir008);
                case 2: return static_cast<int>(EnemyVisual::Husk013);
                default: return static_cast<int>(EnemyVisual::Stalker004);
            }
    }
    return defaultEnemyVisual(kind);
}

int PulseGame::enemyVisualIndex(const Enemy& enemy) const {
    if (enemy.visual >= 0 && enemy.visual < EnemyVisualCount) return enemy.visual;
    switch (enemy.affix) {
        case EliteAffix::Fast:     return static_cast<int>(EnemyVisual::Fast009);
        case EliteAffix::Shielded: return static_cast<int>(EnemyVisual::Shielded010);
        case EliteAffix::Volatile: return static_cast<int>(EnemyVisual::Volatile011);
        case EliteAffix::Regen:    return static_cast<int>(EnemyVisual::Regen012);
        case EliteAffix::None: default: return defaultEnemyVisual(enemy.kind);
    }
}

int PulseGame::enemyVisualIndex(const EnemyCorpse& corpse) const {
    if (corpse.visual >= 0 && corpse.visual < EnemyVisualCount) return corpse.visual;
    return defaultEnemyVisual(corpse.kind);
}

PulseGame::EnemyKind PulseGame::enemyVisualKind(int visual) const {
    const EnemyVisual v = static_cast<EnemyVisual>(std::clamp(visual, 0, EnemyVisualCount - 1));
    switch (v) {
        case EnemyVisual::Gunner002:
        case EnemyVisual::Drone005:
        case EnemyVisual::Choir008:
        case EnemyVisual::Shielded010:
        case EnemyVisual::Summoner014:
            return EnemyKind::Ranged;
        case EnemyVisual::Tank003:
        case EnemyVisual::Warden006:
        case EnemyVisual::Volatile011:
            return EnemyKind::Tank;
        case EnemyVisual::Stalker004:
        case EnemyVisual::Regen012:
            return EnemyKind::Stalker;
        case EnemyVisual::Rusher001:
        case EnemyVisual::Fast009:
        case EnemyVisual::Husk013:
        default:
            return EnemyKind::Rusher;
    }
}

bool PulseGame::enemyVisualIsFlyer(int visual) const {
    const EnemyVisual v = static_cast<EnemyVisual>(std::clamp(visual, 0, EnemyVisualCount - 1));
    return v == EnemyVisual::Drone005 || v == EnemyVisual::Summoner014;
}

PulseGame::AnimatedEnemyModel& PulseGame::enemyRenderModel(const Enemy& enemy) {
    return enemy.boss ? bossModel(enemy.bossKind)
                      : enemyVisuals_[static_cast<size_t>(enemyVisualIndex(enemy))];
}

const PulseGame::AnimatedEnemyModel& PulseGame::enemyRenderModel(const Enemy& enemy) const {
    return enemy.boss ? bossModel(enemy.bossKind)
                      : enemyVisuals_[static_cast<size_t>(enemyVisualIndex(enemy))];
}

PulseGame::AnimatedEnemyModel& PulseGame::enemyRenderModel(const EnemyCorpse& corpse) {
    return corpse.boss ? bossModel(corpse.bossKind)
                       : enemyVisuals_[static_cast<size_t>(enemyVisualIndex(corpse))];
}

const PulseGame::AnimatedEnemyModel& PulseGame::enemyRenderModel(const EnemyCorpse& corpse) const {
    return corpse.boss ? bossModel(corpse.bossKind)
                       : enemyVisuals_[static_cast<size_t>(enemyVisualIndex(corpse))];
}

bool PulseGame::loadAnimatedEnemies() {
    // Map the full Meshy concept roster to render-only visuals. Gameplay still runs on
    // the four EnemyKind archetypes; enemy.visual selects which body/rig represents it.
    struct ModelAsset {
        EnemyKind kind;
        std::string path;
        float worldHeight;
        float yawOffset;
        float hover;                    // flyers (drones): lift the grounded model off the floor
        EnemyClipRanges ranges;
        const char* hideSubmeshSubstr;  // submeshes whose name contains this are not drawn ("" = none)
    };
    const std::string enemyRoot = std::string(kPulseEnemyInputsRoot) + "/pulse_enemy_concepts/";
    const std::string bossRoot = std::string(kPulseEnemyInputsRoot) + "/pulse_boss_concepts/";
    const std::string riggedRoot = "assets/meshy/enemies/rigged_concepts/";
    const ModelAsset assets[EnemyVisualCount] = {
        { EnemyKind::Rusher,  riggedRoot + "pulse_enemy_001_enemy_rusher_low_triangular_lunge_predator_animated.glb",       1.04f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Ranged,  enemyRoot + "002_enemy_gunner_vertical_armored_caster/model_glb.glb",                          1.62f, 0.0f, 0.22f, {}, "" },
        { EnemyKind::Tank,    riggedRoot + "pulse_enemy_003_enemy_tank_broad_frontal_shield_brute_animated.glb",             2.15f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Stalker, riggedRoot + "pulse_enemy_004_enemy_stalker_tall_flanking_pounce_hunter_animated.glb",         1.78f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Ranged,  riggedRoot + "pulse_enemy_005_enemy_drone_finned_hovering_sentinel_animated.glb",              1.06f, 0.0f, 0.58f, {}, "" },
        { EnemyKind::Tank,    riggedRoot + "pulse_enemy_006_enemy_foundry_warden_heavy_mech_animated.glb",                   2.08f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Ranged,  riggedRoot + "pulse_enemy_008_enemy_reliquary_choir_elite_animated.glb",                       1.70f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Rusher,  riggedRoot + "pulse_enemy_009_enemy_fast_elite_rusher_speed_affix_animated.glb",               1.10f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Ranged,  enemyRoot + "010_enemy_shielded_elite_gunner_integrated_defense/model_glb.glb",                1.68f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Tank,    riggedRoot + "pulse_enemy_011_enemy_volatile_elite_bruiser_pressure_reactor_animated.glb",     1.92f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Stalker, riggedRoot + "pulse_enemy_012_enemy_regen_elite_stalker_self_repair_animated.glb",             1.64f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Rusher,  riggedRoot + "pulse_enemy_013_enemy_obsidian_skeletal_husk_benchmark_animated.glb",            1.42f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Ranged,  riggedRoot + "pulse_enemy_014_enemy_summoner_prism_drone_support_animated.glb",                1.18f, 0.0f, 0.62f, {}, "" },
    };
    const ModelAsset bosses[] = {
        { EnemyKind::Tank, bossRoot + "015_boss_foundry_null_forge_marshal/model_glb.glb",          2.62f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Tank, bossRoot + "016_boss_furnace_crucible_tyrant/model_glb.glb",             2.72f, 0.0f, 0.00f, {}, "" },
        { EnemyKind::Tank, bossRoot + "017_boss_reliquary_saint_engine_oracle/model_glb.glb",       2.48f, 0.0f, 0.00f, {}, "" },
    };

    auto loadModel = [&](AnimatedEnemyModel& m, const ModelAsset& a) -> bool {
        m = AnimatedEnemyModel{};
        if (!m.model.load(resolveAsset(a.path))) return false;
        m.ranges = a.ranges;
        m.yawOffset = a.yawOffset;
        m.hoverY = a.hover;
        m.clip = m.model.clipCount() > 0 ? 0 : -1;

        // Multi-clip rig (Mixamo or Quaternius): cache role -> clip index by trying candidate names
        // across naming schemes (clipIndex is case-insensitive). Drive the locomotion + action state
        // machine instead of windowing one baked clip. Flyers (drones) may lack walk/run/death clips
        // -> those roles stay -1 and the poser handles them (they hover on idle, no flinch/death pose).
        const auto ci = [&](std::initializer_list<const char*> names) -> int {
            for (const char* n : names) { int idx = m.model.clipIndex(n); if (idx >= 0) return idx; }
            return -1;
        };
        const int idleClip = ci({ "idle" });
        if (m.clip >= 0 && idleClip >= 0 && m.model.clipCount() > 1) {
            m.multiClip = true;
            m.role.idle = idleClip;
            m.role.walk = ci({ "walk" });
            m.role.run  = ci({ "run" });
            m.role.back = ci({ "walk_back" }); m.role.strafeL = ci({ "strafe_left" }); m.role.strafeR = ci({ "strafe_right" });
            m.role.lunge = ci({ "lunge", "attack", "swordslash", "punch", "kick" });   // closing melee strike (mech: melee clips)
            m.role.cast  = ci({ "cast", "attack", "shoot" });                          // ranged fire (mech: Shoot)
            m.role.castHeavy = ci({ "cast_heavy", "attackauto", "attack", "shoot" });  // wide/heavy variant
            m.role.channel = ci({ "cast_heavy2", "cast_heavy", "charging", "charge", "attackauto" });  // charged tell
            m.role.hit = ci({ "hit", "hitrecieve_1", "hitrecieve" });                  // mech: HitRecieve_1
            m.role.hitHeavy = ci({ "hit_heavy", "hitrecieve_2" });
            m.role.death = ci({ "death", "turnoff" });
            m.clip = m.role.idle;   // measure + default-pose from the idle clip
        }

        // Fixed submesh topology: skinning moves vertices, never counts or indices.
        const std::vector<AnimatedGltfSubmesh>& bind = m.model.submeshes();
        if (bind.empty()) return false;
        m.vertCounts.reserve(bind.size());
        m.indices.reserve(bind.size());
        m.submeshMaterial.reserve(bind.size());
        m.submeshVisible.reserve(bind.size());
        // Optional per-kind hide rule: a submesh whose name contains the substring
        // (case-insensitive) is loaded but never drawn (the cultist's "smg" gun).
        const std::string hide = a.hideSubmeshSubstr ? a.hideSubmeshSubstr : "";
        for (const AnimatedGltfSubmesh& sm : bind) {
            m.vertCounts.push_back(static_cast<uint32_t>(sm.vertices.size()));
            m.indices.push_back(sm.indices);
            m.submeshMaterial.push_back(sm.materialIndex);
            bool visible = true;
            if (!hide.empty()) {
                std::string lname = sm.name;
                for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                visible = lname.find(hide) == std::string::npos;
            }
            m.submeshVisible.push_back(visible ? 1u : 0u);
        }

        // Measure the rest/idle pose in world space to derive a uniform scale that
        // lands the rig at worldHeight, and the foot plane that grounds it on y=0.
        m.model.sample(m.clip, a.ranges.idle0, enemyPoseScratch_, AnimatedGltfModel::Space::World);
        const EnemyPoseMetrics metrics = measureEnemyPose(enemyPoseScratch_, m.submeshVisible);
        if (!metrics.valid) return false;
        const float bindHeight = std::max(1.0e-4f, metrics.maxY - metrics.minY);
        m.worldScale = a.worldHeight / bindHeight;
        m.worldHeight = a.worldHeight;
        m.collisionRadius = std::clamp(metrics.radiusXZ * m.worldScale + 0.08f, 0.30f, 0.92f);
        m.footY = metrics.minY;
        m.poseAnchorX = metrics.meanX;
        m.poseAnchorY = metrics.meanY;
        m.poseAnchorZ = metrics.meanZ;
        if (!m.multiClip && m.clip >= 0) {
            // Single baked clip (legacy animated Meshy): window the whole clip as the walk loop.
            m.ranges.walk0 = 0.0f;
            m.ranges.walk1 = std::max(0.1f, m.model.clipDuration(m.clip));
        }
        m.loaded = true;
        return true;
    };
    for (size_t i = 0; i < EnemyVisualCount; ++i)
        if (!loadModel(enemyVisuals_[i], assets[i])) return false;
    for (size_t i = 0; i < BossKindCount; ++i)
        if (!loadModel(bossModels_[i], bosses[i])) return false;
    return true;
}

bool PulseGame::ensureAnimatedEnemyResources(Engine& engine) {
    const auto loadTextureFile = [&](const std::string& path, bool srgb) -> TextureHandle {
        if (path.empty()) return TextureHandle::Invalid;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba = loadImageRGBA(path, w, h);
        if (rgba.empty()) return TextureHandle::Invalid;
        TextureData td;
        td.width = w; td.height = h; td.rgba = rgba.data(); td.srgb = srgb;
        return engine.createTexture(td);
    };
    // Shared "polished obsidian + hot-magenta energy" material (Neon Ink Brutalism). The texture
    // is a dark obsidian body with bright magenta crack-veins; emissive > 0 blooms ONLY the bright
    // veins (emissive = albedo * emissive). One material across the family = a cohesive look, and
    // it gives husk_mx (whose Mixamo round-trip dropped its own texture) a body again.
    if (sharedEnemyMaterial_ == MaterialHandle::Invalid) {
        MaterialDesc d;
        d.baseColor = loadTextureFile(resolveAsset("assets/meshy/enemies/shared/obsidian_base.png"), true); // faceted obsidian + magenta veins
        d.normal    = loadTextureFile(resolveAsset("assets/meshy/enemies/shared/obsidian_n.png"), false); // facet/crack relief
        d.orm       = loadTextureFile(resolveAsset("assets/meshy/enemies/shared/obsidian_orm.png"), false); // AO/rough/metal per texel
        d.emissive  = 1.2f;    // bloom the bright magenta veins; the dark obsidian body barely emits
        sharedEnemyMaterial_ = engine.createMaterial(d);
    }

    auto buildGpu = [&](AnimatedEnemyModel& m, EnemyKind kind) -> bool {
        if (!m.loaded) return true;
        if (!m.gpuReady) {
            m.materials.clear();
            m.staticMeshes.clear();
            if (!m.model.materials().empty()) {
                // Use the asset's OWN textures (e.g. KayKit's gradient atlas). The neon look comes
                // from the brutalist rim + arena lighting on top, NOT by crushing the source art.
                for (const AnimatedGltfMaterial& src : m.model.materials()) {
                    MaterialDesc d;
                    d.baseColor = loadTextureFile(src.baseColorTexture, true);
                    d.normal    = loadTextureFile(src.normalTexture, false);
                    d.orm       = loadTextureFile(src.metallicRoughnessTexture, false);
                    d.baseColorFactor = src.baseColorFactor;
                    d.metallic  = 0.0f;
                    d.roughness = 0.6f;   // stylized matte; lighting + rim give the form
                    const float em = std::max(src.emissiveFactor.x, std::max(src.emissiveFactor.y, src.emissiveFactor.z));
                    d.emissiveTex = loadTextureFile(src.emissiveTexture, true);
                    if (d.emissiveTex != TextureHandle::Invalid) {
                        d.emissiveTexStrength = 2.8f * std::max(1.0f, em);
                    }
                    // Older Quaternius packs bake glowing parts into a sibling emissive atlas the
                    // glTF does not reference. Derive it only when the asset itself had no map.
                    if (d.emissiveTex == TextureHandle::Invalid && !src.baseColorTexture.empty()) {
                        const std::filesystem::path bc(src.baseColorTexture);
                        const std::string stem = bc.stem().string();          // e.g. "T_Enemies_BaseColor_png"
                        const size_t cut = stem.find("_BaseColor");
                        if (cut != std::string::npos) {
                            const std::string fam = stem.substr(0, cut);       // "T_Enemies" / "T_Enemies_Large"
                            const std::filesystem::path emis =
                                bc.parent_path().parent_path() / "Textures" / (fam + "_Emissive.png");
                            const TextureHandle eh = loadTextureFile(emis.string(), true);
                            if (eh != TextureHandle::Invalid) { d.emissiveTex = eh; d.emissiveTexStrength = 3.5f; }
                        }
                    }
                    if (em > 0.02f && d.emissiveTex == TextureHandle::Invalid) d.emissive = em * 2.2f;
                    m.materials.push_back(engine.createMaterial(d));
                }
                m.stylizedTextured = true;   // real textured asset -> no obsidian shadow-smoke aura
                // Neon eyes: KayKit's eye-glow is a dull yellow in the atlas. Give the "eyes"
                // submesh a bright emissive material, colour-coded PER KIND so threats read at a
                // glance (factor*tint reaches albedo, so no texture needed). Boss shares Tank=amber.
                MaterialDesc eyeD;
                switch (kind) {
                    case EnemyKind::Ranged:  eyeD.baseColorFactor = { 0.15f, 1.35f, 1.80f, 1.0f }; break; // cyan caster
                    case EnemyKind::Tank:    eyeD.baseColorFactor = { 1.80f, 0.95f, 0.12f, 1.0f }; break; // amber brute / boss
                    case EnemyKind::Stalker: eyeD.baseColorFactor = { 0.35f, 1.60f, 0.40f, 1.0f }; break; // toxic-green flanker
                    default:                 eyeD.baseColorFactor = { 1.60f, 0.10f, 0.95f, 1.0f }; break; // magenta rusher
                }
                eyeD.emissive  = 2.6f;
                eyeD.metallic  = 0.0f;
                eyeD.roughness = 0.45f;
                const int eyeIdx = static_cast<int>(m.materials.size());
                m.materials.push_back(engine.createMaterial(eyeD));
                const std::vector<AnimatedGltfSubmesh>& subs = m.model.submeshes();
                for (size_t s = 0; s < m.submeshMaterial.size() && s < subs.size(); ++s) {
                    std::string ln = subs[s].name;
                    for (char& c : ln) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ln.find("eye") != std::string::npos) m.submeshMaterial[s] = eyeIdx;
                }
            } else if (sharedEnemyMaterial_ != MaterialHandle::Invalid) {
                // Texture-less rigs (the Mixamo husk) fall back to the shared obsidian body.
                m.materials.push_back(sharedEnemyMaterial_);
                for (int& sm : m.submeshMaterial) sm = 0;
            }
            if (!m.multiClip && m.clip < 0) {
                // Static Meshy GLBs carry no animation data. Create one shared mesh per submesh
                // and animate them by transform at draw time; this avoids per-enemy dynamic uploads.
                // The loader's cached submeshes are viewmodel-space by default; enemies need the
                // same Y-up world-space sample used for measuring, or they render lying forward.
                std::vector<AnimatedGltfSubmesh> staticWorld;
                m.model.sample(-1, 0.0f, staticWorld, AnimatedGltfModel::Space::World);
                for (const AnimatedGltfSubmesh& sm : staticWorld) {
                    const MeshHandle h = engine.createMesh({ sm.vertices, sm.indices });
                    if (h == MeshHandle::Invalid) return false;
                    m.staticMeshes.push_back(h);
                }
                if (m.staticMeshes.empty()) return false;
            }
            m.gpuReady = true;
        }
        return m.gpuReady;
    };
    bool allReady = true;
    for (size_t vi = 0; vi < enemyVisuals_.size(); ++vi)
        allReady = buildGpu(enemyVisuals_[vi], enemyVisualKind(static_cast<int>(vi))) && allReady;
    for (AnimatedEnemyModel& boss : bossModels_)
        allReady = buildGpu(boss, EnemyKind::Tank) && allReady;
    return allReady;
}

PulseGame::AnimatedEnemyInstance* PulseGame::poseAnimatedEnemy(Engine& engine, const Enemy& e, size_t slot, Vec2 facing) {
    AnimatedEnemyModel& m = enemyRenderModel(e);
    if (!m.loaded || !m.gpuReady) return nullptr;
    if (!m.multiClip && m.clip < 0) {
        return commitEnemyPose(engine, e.kind, slot, e.boss, e.bossKind, enemyVisualIndex(e));
    }

    const auto smoothstep01 = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); };
    const bool lockClipToGround = m.hoverY <= 0.01f;
    const bool lockClipToHoverCenter = m.hoverY > 0.01f;
    const auto sampleClip = [&](int clip, float time, std::vector<AnimatedGltfSubmesh>& out) {
        m.model.sample(clip, time, out, AnimatedGltfModel::Space::World);
        stabilizeEnemyPose(out, m.submeshVisible, m.poseAnchorX, m.poseAnchorY, m.poseAnchorZ,
                           m.footY, lockClipToGround, lockClipToHoverCenter);
    };

    if (m.multiClip) {
        // Mixamo state machine: an idle base, the dominant DIRECTIONAL locomotion clip blended in
        // by speed (so a kiting caster back-pedals / strafes instead of moon-walking forward), then
        // a cast or melee-lunge action and a hit flinch cross-faded on top. <= 3 rig samples/enemy.
        const auto loopClip = [&](int clip, float phase, std::vector<AnimatedGltfSubmesh>& out) {
            const float dur = std::max(0.05f, m.model.clipDuration(clip));
            sampleClip(clip, std::fmod(std::max(0.0f, phase), dur), out);
        };
        loopClip(m.role.idle, e.animTime + e.bobPhase, enemyPoseScratch_);

        const float speed = std::sqrt(e.vel.x * e.vel.x + e.vel.y * e.vel.y);
        const float rawLocoW = smoothstep01((speed - 0.15f) / 0.55f);
        const float locoW = rawLocoW;
        if (locoW > 0.001f) {
            const Vec2 right = rightFromForward(facing);
            const float inv = speed > 1e-4f ? 1.0f / speed : 0.0f;
            const float fdot = (e.vel.x * facing.x + e.vel.y * facing.y) * inv;   // forwardness  -1..1
            const float sdot = (e.vel.x * right.x  + e.vel.y * right.y)  * inv;   // sideways     -1..1
            const bool run = speed > 3.0f && m.role.run >= 0;
            int locoClip = run ? m.role.run : m.role.walk;
            float best = std::max(0.0f, fdot);                                    // forward is the default
            const auto consider = [&](int clip, float w) { if (clip >= 0 && w > best) { best = w; locoClip = clip; } };
            consider(m.role.back, std::max(0.0f, -fdot));
            consider(m.role.strafeR, std::max(0.0f, sdot));
            consider(m.role.strafeL, std::max(0.0f, -sdot));
            if (locoClip >= 0) {
                loopClip(locoClip, e.walkPhase + e.bobPhase, enemyPoseScratchB_);
                blendAnimatedVertices(enemyPoseScratch_, enemyPoseScratchB_, locoW);
            }
        }

        // Action: a cast (or melee lunge) cross-faded over the locomotion pose on the
        // telegraph -> strike -> recover beats, so the wind-up and release land on-frame.
        const bool windup = e.telegraphRemaining > 0.0f;
        const bool striking = e.lungeTime > 0.0f || (e.struck && e.recover > 0.0f);
        if (windup || striking) {
            int atk = m.role.cast;
            if (!e.pendingRanged && m.role.lunge >= 0) {
                atk = m.role.lunge;                                  // closing melee strike
            } else if (e.pendingAttack == EnemyAttack::Fan && m.role.castHeavy >= 0) {
                atk = m.role.castHeavy;                              // Tank: wide two-handed zone cast
            } else if (e.pendingAttack == EnemyAttack::Beam && m.role.channel >= 0) {
                atk = m.role.channel;                                // Ranged: charged lock-on channel
            }
            if (atk >= 0) {
                float p;
                if (windup) {
                    const float base = e.kind == EnemyKind::Ranged ? std::max(0.05f, tunables_.enemyRangedTelegraph)
                                                                   : std::max(0.05f, tunables_.enemyMeleeTelegraph);
                    p = 0.55f * clamp(1.0f - e.telegraphRemaining / base, 0.0f, 1.0f);
                } else if (e.lungeTime > 0.0f) {
                    p = 0.62f;
                } else {
                    const float base = e.kind == EnemyKind::Ranged ? 0.35f : std::max(0.2f, tunables_.enemyMeleeRecover);
                    p = 0.60f + 0.40f * clamp(1.0f - e.recover / base, 0.0f, 1.0f);
                }
                float aw = 1.0f;
                if (p < 0.14f) aw = p / 0.14f;
                else if (p > 0.80f) aw = (1.0f - p) / 0.20f;
                aw = smoothstep01(aw);
                const float dur = std::max(0.05f, m.model.clipDuration(atk));
                sampleClip(atk, clamp(p, 0.0f, 1.0f) * dur, enemyPoseScratchB_);
                blendAnimatedVertices(enemyPoseScratch_, enemyPoseScratchB_, aw);
            }
        }

        // Hit flinch: hold the peak-flinch frame and fade it out as hitPunch decays.
        if (e.hitPunch > 0.02f) {
            const int hc = (e.hitPunch > 0.6f && m.role.hitHeavy >= 0) ? m.role.hitHeavy : m.role.hit;
            if (hc >= 0) {
                sampleClip(hc, 0.35f * std::max(0.05f, m.model.clipDuration(hc)), enemyPoseScratchB_);
                blendAnimatedVertices(enemyPoseScratch_, enemyPoseScratchB_, smoothstep01(e.hitPunch));
            }
        }
    } else {
        // Legacy single baked clip: cross-fade between time windows (idle <-> walk, attack overlay).
        const float idlePhase = std::max(0.0f, e.animTime + e.bobPhase);   // steady idle clock
        const float walkP = std::max(0.0f, e.walkPhase + e.bobPhase);      // distance-phased walk clock
        const float idleSpan = std::max(0.05f, m.ranges.idle1 - m.ranges.idle0);
        sampleClip(m.clip, m.ranges.idle0 + std::fmod(idlePhase, idleSpan), enemyPoseScratch_);
        const float speed = std::sqrt(e.vel.x * e.vel.x + e.vel.y * e.vel.y);
        const float locoW = smoothstep01((speed - 0.15f) / 0.55f) * 0.45f;
        if (locoW > 0.001f && m.ranges.walk1 > m.ranges.walk0) {
            const float walkSpan = std::max(0.05f, m.ranges.walk1 - m.ranges.walk0);
            sampleClip(m.clip, m.ranges.walk0 + std::fmod(walkP, walkSpan), enemyPoseScratchB_);
            blendAnimatedVertices(enemyPoseScratch_, enemyPoseScratchB_, locoW);
        }
        const bool windup = e.telegraphRemaining > 0.0f;
        const bool striking = e.lungeTime > 0.0f || (e.struck && e.recover > 0.0f);
        if (m.ranges.hasAttack && (windup || striking)) {
            const float span = std::max(0.05f, m.ranges.atk1 - m.ranges.atk0);
            float p;
            if (windup) {
                const float base = e.kind == EnemyKind::Ranged ? std::max(0.05f, tunables_.enemyRangedTelegraph)
                                                               : std::max(0.05f, tunables_.enemyMeleeTelegraph);
                p = 0.60f * clamp(1.0f - e.telegraphRemaining / base, 0.0f, 1.0f);   // wind-up = first 60%
            } else if (e.lungeTime > 0.0f) {
                p = 0.62f;                                                            // hold the strike pose mid-lunge
            } else {
                const float base = e.kind == EnemyKind::Ranged ? 0.35f : std::max(0.2f, tunables_.enemyMeleeRecover);
                p = 0.60f + 0.40f * clamp(1.0f - e.recover / base, 0.0f, 1.0f);       // strike + follow-through
            }
            float attackW = 1.0f;                                                     // fade in (first 14%) / out (last 20%)
            if (p < 0.14f) attackW = p / 0.14f;
            else if (p > 0.80f) attackW = (1.0f - p) / 0.20f;
            attackW = smoothstep01(attackW);
            sampleClip(m.clip, m.ranges.atk0 + p * span, enemyPoseScratchB_);
            blendAnimatedVertices(enemyPoseScratch_, enemyPoseScratchB_, attackW);
        }
    }

    return commitEnemyPose(engine, e.kind, slot, e.boss, e.bossKind, enemyVisualIndex(e));
}

// Pool grow + GPU upload from enemyPoseScratch_ (shared by live enemies and death-corpses).
PulseGame::AnimatedEnemyInstance* PulseGame::commitEnemyPose(Engine& engine, EnemyKind kind, size_t slot, bool boss, int bossKind, int visual) {
    const int bidx = std::clamp(bossKind, 0, BossKindCount - 1);
    const int vidx = std::clamp(visual >= 0 ? visual : defaultEnemyVisual(kind), 0, EnemyVisualCount - 1);
    AnimatedEnemyModel& m = boss ? bossModels_[static_cast<size_t>(bidx)] : enemyVisuals_[static_cast<size_t>(vidx)];
    std::vector<AnimatedEnemyInstance>& pool = boss ? bossPools_[static_cast<size_t>(bidx)] : enemyVisualPools_[static_cast<size_t>(vidx)];
    const bool staticPose = !m.multiClip && m.clip < 0;
    if (staticPose && m.staticMeshes.empty()) return nullptr;
    while (pool.size() <= slot) {
        AnimatedEnemyInstance inst;
        if (staticPose) {
            inst.meshes = m.staticMeshes;
        } else {
            bool ok = true;
            for (size_t s = 0; s < m.vertCounts.size(); ++s) {
                MeshHandle h = engine.createDynamicMesh(m.vertCounts[s], m.indices[s]);
                if (h == MeshHandle::Invalid) { ok = false; break; }
                inst.meshes.push_back(h);
            }
            if (!ok) return nullptr;
        }
        pool.push_back(std::move(inst));
    }
    AnimatedEnemyInstance& inst = pool[slot];
    if (staticPose) return &inst;
    const size_t n = std::min(enemyPoseScratch_.size(), inst.meshes.size());
    for (size_t s = 0; s < n; ++s) {
        if (s < m.submeshVisible.size() && !m.submeshVisible[s]) continue; // skip hidden (gun) submeshes
        engine.updateDynamicMesh(inst.meshes[s], enemyPoseScratch_[s].vertices);
    }
    return &inst;
}

// Pose a death-corpse: play the rig's `death` clip once at the corpse's age (held on the last
// frame after it completes), then commit to the shared pool. Multi-clip rigs only.
PulseGame::AnimatedEnemyInstance* PulseGame::poseDeadEnemy(Engine& engine, const EnemyCorpse& c, size_t slot) {
    AnimatedEnemyModel& m = enemyRenderModel(c);
    if (!m.loaded || !m.gpuReady) return nullptr;
    if (!m.multiClip && m.clip < 0) {
        return commitEnemyPose(engine, c.kind, slot, c.boss, c.bossKind, enemyVisualIndex(c));
    }
    if (!m.multiClip) return nullptr;
    if (c.fall) {
        // Flyer with no death clip: freeze the idle pose; the corpse render tumbles + drops it.
        const int clip = m.role.idle >= 0 ? m.role.idle : m.clip;
        m.model.sample(clip, 0.0f, enemyPoseScratch_, AnimatedGltfModel::Space::World);
        return commitEnemyPose(engine, c.kind, slot, c.boss, c.bossKind, enemyVisualIndex(c));
    }
    if (m.role.death < 0) return nullptr;
    const float dur = std::max(0.05f, m.model.clipDuration(m.role.death));
    m.model.sample(m.role.death, std::min(c.age, dur), enemyPoseScratch_, AnimatedGltfModel::Space::World);
    return commitEnemyPose(engine, c.kind, slot, c.boss, c.bossKind, enemyVisualIndex(c));
}

void PulseGame::resetRun() {
    bestScore_ = std::max(bestScore_, score_);
    score_ = 0;
    build_.clear();           // fresh run: drop the stacking build before re-clamping HP
    rewardOptions_.clear();
    scrap_ = 0;               // per-run economy currency (never persisted)
    scrapFlashTimer_ = 0.0f;
    shopStock_.clear();
    shopPrices_.clear();
    shopSold_.clear();
    shopRerollCount_ = 0;
    eventDeals_.clear();
    sectorCurses_.clear();
    // Fresh loadout: the chosen starting weapon (Hub pick; defaults to the pistol).
    loadout_.clear();
    const std::string starter = build_.findWeapon(meta_.startingWeapon()) ? meta_.startingWeapon() : std::string("pistol");
    loadout_.push_back(WeaponSlot{ starter, 1, 0, Weapon{} });
    activeWeapon_ = 0;
    tacticalCharge_ = 0.0f;
    ultimateCharge_ = 0.0f;
    overdriveTimer_ = 0.0f;
    player_ = Player{};
    // M6 Mirror (hybrid permanent layer): a modest, capped run-start head start. Vigor (+max HP)
    // already feeds effectiveMaxHealth; here apply Plating (+shield), Avarice (+scrap), Momentum
    // (+starting Pulse). Adrenaline/Fortune are read live at the charge/reward sites.
    player_.hp = std::clamp(tunables_.playerStartHealth, 1, std::max(1, effectiveMaxHealth()));
    player_.shield = std::clamp(tunables_.playerStartShield + meta_.mirrorBonusShield(), 0, std::max(0, effectiveMaxShield()));
    scrap_ = meta_.mirrorBonusScrap();
    weapon_ = Weapon{};
    weapon_.ammo = std::max(1, weaponMagazine());
    weapon_.reserve = std::max(0, weaponReserveMax());
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    pickups_.clear();
    tracers_.clear();
    casings_.clear();
    impacts_.clear();
    bursts_.clear();
    debris_.clear();
    decals_.clear();
    envDecals_.clear();
    particles_.clear();
    projectiles_.clear();
    beams_.clear();
    novaWaves_.clear();
    damageMarkers_.clear();
    spawnTimer_ = 0.35f;
    pickupSpawnTimer_ = 2.5f;
    restartTimer_ = 0.0f;
    hitmarkerTimer_ = 0.0f;
    precisionMarkerTimer_ = 0.0f;
    killConfirmTimer_ = 0.0f;
    damageFlashTimer_ = 0.0f;
    shieldFlashTimer_ = 0.0f;
    lifeLeechFlashTimer_ = 0.0f;
    elementFeedbackCooldown_.fill(0.0f);
    lifeLeechCarry_ = 0.0f;
    dashInvulnTimer_ = 0.0f;
    muzzleFlashTimer_ = 0.0f;
    fireFovKick_ = 0.0f;
    fireCameraKick_ = 0.0f;
    recoilPitch_ = 0.0f;
    recoilResidualPitch_ = 0.0f;
    recoilOffsetPitch_ = 0.0f;
    recoilOffsetYaw_ = 0.0f;
    recoilShotIndex_ = 0;
    weaponKick_ = 0.0f;
    weaponKickSide_ = 0.0f;
    cameraBobPhase_ = 0.0f;
    landingKick_ = 0.0f;
    strafeLean_ = 0.0f;
    combatIntensity_ = 0.0f;
    pulse_.reset();
    if (meta_.mirrorStartPulse() > 0.0f) pulse_.bump(meta_.mirrorStartPulse());   // M6 Mirror: Momentum head start
    pendingBossIntroStinger_ = false;
    musicOverpulseLatched_ = false;
    musicOverpulseCooldown_ = 0.0f;
    roomPulsePeak_ = 0.0f;
    runPulsePeak_ = 0.0f;
    lowHealthLatched_ = false;
    cameraShake_ = 0.0f;
    hitStopTimer_ = 0.0f;

    // Run/room state machine: build a fresh seeded run and enter its first room.
    runWon_ = false;
    phaseTimer_ = 0.0f;
    waveIndex_ = 0;
    waveSpawnsLeft_ = 0;
    waveSpawnTimer_ = 0.0f;
    beginRun();
    ++runCount_; // next run differs; the first run (count 0) stays deterministic
}

void PulseGame::resetForSim() {
    // Run the balance sim from a clean, reproducible in-memory profile (no disk I/O,
    // first run deterministically seeded).
    meta_.setPersistence(false);
    meta_.resetToFresh();
    runCount_ = 0;
    autoResolveDoors_ = true;   // the sim cannot path through doors; resolve by policy instantly
    resetRun();
}

void PulseGame::abandonRun() {
    resetRun(); // start the next seeded run immediately (used by the sim on a timeout)
}

void PulseGame::recomputeMods() {
    mods_ = recomputeRunMods(activeMods_);
}

void PulseGame::seedRunMods() {
    // Fresh run: clear the live set and re-seed it from the run-start sources: the heat
    // table (Feature 4) and the sim's forced set. Deals append to activeMods_ mid-run and
    // call recomputeMods() themselves.
    activeMods_.clear();
    // Sim (--sim-heat) forces a heat level via the legacy heat table; the interactive Hub uses
    // the HEAT ladder of named RUN CONTRACTS (heat N = the first N modifiers active).
    const std::vector<RunModifier> hm = forcedHeat_ >= 0 ? heatMods(forcedHeat_)
                                                         : contractMods(contractMaskForHeat(meta_.heat()));
    activeMods_.insert(activeMods_.end(), hm.begin(), hm.end());
    activeMods_.insert(activeMods_.end(), forcedMods_.begin(), forcedMods_.end());
    recomputeMods();
}

void PulseGame::setForcedMods(std::vector<RunModifier> mods) {
    forcedMods_ = std::move(mods);
    seedRunMods(); // apply immediately in case a run is already active
}

void PulseGame::beginRun() {
    // First run uses the base seed (deterministic for --bot-test); later runs vary.
    const uint32_t runSeed = 0x50554C53u ^ (static_cast<uint32_t>(runCount_) * 0x9E3779B9u);
    run_.begin(runSeed, tunables_);
    seedRunMods();   // mods ready before the first wave starts
    runWon_ = false;
    beginRoom();
}

void PulseGame::regenerateArena() {
    // Deterministic per run + room: a seed reproduces the layout; rooms differ within a run.
    if (wasteland_.ready()) {
        // ONE BIOME PER SECTOR: each biome is a multi-room stretch you work through one room at a
        // time (a reward after each - the run state machine), then the next sector is the next
        // biome. Stored in currentBiome_ so buildFrame can drive the matching lighting palette.
        currentBiome_ = forcedBiome_ >= 0
            ? static_cast<Biome>(forcedBiome_ % static_cast<int>(Biome::Count))   // dev: --biome N forces it for capture
            : static_cast<Biome>(run_.sector() % static_cast<int>(Biome::Count));
        // Area-size pacing (brutalist): make the run feel like a string of SMALL and BIG areas.
        // Boss arenas are big; service rooms are connector corridors; caches + sector openers are
        // tight; the rest alternate mid/big by seed. Every template carries 3 exits so any choice
        // (up to 3 options) fits its doors regardless of size.
        {
            const RoomType t = run_.currentType();
            AreaSize size = AreaSize::Mid;
            if (run_.currentIsBoss())                                   size = AreaSize::Big;
            else if (t == RoomType::Shop || t == RoomType::Event)       size = AreaSize::Corridor;
            else if (t == RoomType::Cache)                              size = AreaSize::Small;   // low-threat: a tight cache
            else {
                // Combat/Elite rooms travel through varied sizes (mostly mid/big so close-quarters
                // spawns stay fair; an occasional small room for a tense, Doom-tight fight).
                const uint32_t h = (run_.seed() * 2654435761u) ^ (static_cast<uint32_t>(run_.roomIndex()) * 40503u);
                const uint32_t pick = h % 5u;
                size = (pick < 2u) ? AreaSize::Mid : (pick < 4u ? AreaSize::Big : AreaSize::Small);
            }
            wasteland_.setAreaSize(size);
        }
        wasteland_.setForcedTemplate(forcedRoomName_);   // dev/QA: --room forces a specific layout (empty = normal)
        wasteland_.setOpenTop(topDownCapture_);           // dev/QA: --topdown omits the ceiling so the overhead camera sees in
        wasteland_.setSector(run_.sector());              // no-repeat room pick within a sector (surface more of the 10)
        wasteland_.generate(currentBiome_, static_cast<uint64_t>(run_.seed()), run_.roomIndex());
        if (!forcedRoomName_.empty()) currentBiome_ = wasteland_.activeBiome();   // adopt the forced room's biome for lighting
        buildEnvDecals();   // per-room biome floor markings (chevrons / code / sigils)
    } else {
        return;
    }
    player_.pos = { static_cast<float>(procEnvSpawnX()) + 0.5f, static_cast<float>(procEnvSpawnZ()) + 0.5f };
    player_.vel = {};
    // Brutalist arenas spawn at the entrance door (doors()[0]); face into the most readable open
    // space rather than blindly at the room centre, because corridor entries can have side walls
    // close to the centre ray.
    if (wasteland_.doorCount() > 0) {
        const Door& entrance = wasteland_.doors()[0];
        float bestYaw = std::atan2(entrance.inwardZ, entrance.inwardX);
        float bestOpen = -1.0f;
        for (int i = 0; i < 32; ++i) {
            const float a = -Pi + TwoPi * static_cast<float>(i) / 32.0f;
            const Vec2 f = fromAngle(a);
            float open = 0.0f;
            for (float d = 0.8f; d <= 12.0f; d += 0.35f) {
                const Vec2 p = player_.pos + f * d;
                if (collides(p, 0.34f) || !lineOfSight(player_.pos, p)) break;
                open = d;
            }
            if (open > bestOpen) { bestOpen = open; bestYaw = a; }
        }
        player_.yaw = bestYaw;
    }
}

void PulseGame::beginRoom() {
    doors_.clear();        // drop any open exit binds from the room we just left
    furthestSector_ = std::max(furthestSector_, run_.sector() + 1);  // track deepest sector for the Main Menu
    roomPulsePeak_ = 0.0f; // fresh momentum greed track for the new room
    regenerateArena();   // fresh tile layout + spawn for this room (no-op until the kit is loaded)
    doorAnim_.assign(static_cast<size_t>(std::max(0, wasteland_.doorCount())), 0.0f);  // every door starts shut
    expireSectorCurses();  // Feature 3: drop sector-scoped deal curses once the sector advances
    { const int t = static_cast<int>(run_.currentType()); if (t >= 0 && t < 6) ++roomTypeCounts_[static_cast<size_t>(t)]; } // M5 telemetry
    // Features 2/3: no-combat rooms open their own screen instead of spawning waves. (Cache
    // keeps the combat path: its empty waves auto-clear into a reward.)
    if (run_.currentType() == RoomType::Shop) { enterShop(); return; }
    if (run_.currentType() == RoomType::Event) { enterEvent(); return; }
    phase_ = run_.currentIsBoss() ? RunPhase::Boss : RunPhase::InRoom;
    if (phase_ == RunPhase::Boss) {
        ++bossesReachedTotal_;
        pendingBossIntroStinger_ = true;
    }
    waveIndex_ = 0;
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    startWave(0);
    // Boss rooms open with the Warden; the wave then trickles in escort adds.
    if (run_.currentIsBoss()) spawnBoss();
}

void PulseGame::startWave(int index) {
    const RoomSpec& room = run_.currentRoom();
    if (index < 0 || index >= static_cast<int>(room.waves.size())) {
        activeWave_ = WaveSpec{};
        waveSpawnsLeft_ = 0;
        return;
    }
    activeWave_ = room.waves[static_cast<size_t>(index)];
    // RunMods: heat/deals scale wave SIZE here - count is consumed at this site, not in
    // updateSpawning - and concurrency by the same factor, so more enemies means more
    // on-screen pressure, not a longer trickle. activeWave_ is a copy, so the room
    // template is untouched. Default enemyCountMult is 1.0 (no-op).
    if (mods_.enemyCountMult != 1.0f) {
        activeWave_.count = std::max(1, static_cast<int>(std::lround(activeWave_.count * mods_.enemyCountMult)));
        activeWave_.maxConcurrent = std::max(1, static_cast<int>(std::lround(activeWave_.maxConcurrent * mods_.enemyCountMult)));
    }
    waveSpawnsLeft_ = std::max(0, activeWave_.count);
    waveSpawnTimer_ = 0.25f; // brief lead-in before the wave's first spawn
}

// A room clears once every wave has been dispatched and the arena is empty.
bool PulseGame::roomComplete() const {
    if (phase_ != RunPhase::InRoom && phase_ != RunPhase::Boss) return false;
    if (run_.complete()) return false;
    const RoomSpec& room = run_.currentRoom();
    if (waveIndex_ < static_cast<int>(room.waves.size())) return false; // waves remain
    return waveSpawnsLeft_ <= 0 && activeEnemyCount() == 0;
}

void PulseGame::enterRoomCleared(AudioSystem& audio) {
    phase_ = RunPhase::RoomCleared;
    phaseTimer_ = 15.0f;       // generous fallback; the player picks a reward to advance
    ++roomsClearedTotal_;
    projectiles_.clear();      // tidy any in-flight threats between rooms
    audio.playMusicStinger(MusicStingerType::RoomClear, 0.75f, true);
    // Roll a 1-of-3 reward choice on the run-RNG (reproducible for a seed). Gated
    // (not-yet-unlocked) content is excluded from the pool (Phase C meta gating).
    std::vector<std::string> excluded = meta_.lockedContent();
    const std::vector<std::string> profileLocked = weaponProfiles_.invalidRewardIds();
    excluded.insert(excluded.end(), profileLocked.begin(), profileLocked.end());
    // Elite/Cache rooms upgrade the reward (tier-biased toward Uncommon/Rare) on top of
    // any heat/deal reward bias from RunMods (Feature 1 room semantics).
    const RoomType rt = run_.currentType();
    const float roomBias = (rt == RoomType::Elite || rt == RoomType::Cache) ? 0.6f : 0.0f;
    // Pulse greed: the hotter you fought this room, the better the loot rolls (the risk/reward spine).
    // Plus the M6 Mirror Fortune node (a small permanent tier-bias).
    const float greedBias = roomPulsePeak_ * Pulse::kLootBias + meta_.mirrorTierBias();
    rewardOptions_ = build_.rollRewards(run_.rng(), 3, excluded, mods_.rewardTierBias + roomBias + greedBias);
    playFeedback(audio, FeedbackEventType::UiReward, 0.9f); // room cleared, reward presented
}

void PulseGame::grantReward(int index, AudioSystem& audio) {
    bool granted = false;
    if (index >= 0 && index < static_cast<int>(rewardOptions_.size())) {
        const Build::RewardView rv = build_.describeReward(rewardOptions_[static_cast<size_t>(index)]);
        if (rv.valid && rv.isWeapon) {
            acquireWeapon(rv.rawId, audio);   // new weapon joins / dup raises power
            granted = true;
        } else if (rv.valid) {
            const int prevMaxHp = effectiveMaxHealth();
            const int prevMaxSh = effectiveMaxShield();
            build_.add(rv.rawId, 1);
            // A +max item also tops up the current pool by the amount gained.
            const int dHp = effectiveMaxHealth() - prevMaxHp;
            const int dSh = effectiveMaxShield() - prevMaxSh;
            if (dHp > 0) player_.hp = std::min(effectiveMaxHealth(), player_.hp + dHp);
            if (dSh > 0) player_.shield = std::min(effectiveMaxShield(), player_.shield + dSh);
            playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f);
            granted = true;
        }
    }
    if (granted) audio.playMusicStinger(MusicStingerType::Reward, 0.70f, true);
    rewardOptions_.clear();
    advanceToNextRoom(audio);
}

void PulseGame::advanceToNextRoom(AudioSystem& audio) {
    if (!run_.advanceRoom()) { // advanced past the final step -> a win
        enterRunOver(true, audio);
        return;
    }
    if (run_.needsChoice()) { enterChoosePath(audio); return; } // branch: pick the next room
    beginRoom();
}

// Feature 1: a time-stopped "choose your next room" state between rooms. Input keys
// 1/2/3 (handled in update) commit run_.chooseOption -> beginRoom; a generous fallback
// auto-picks option 0 so an idle/headless session never softlocks.
void PulseGame::enterChoosePath(AudioSystem& audio) {
    phase_ = RunPhase::ChoosePath;
    phaseTimer_ = 25.0f;
    projectiles_.clear();
    enemies_.clear();
    playFeedback(audio, FeedbackEventType::UiConfirm, 0.85f);
}

// Spatial doors (Hades-style merged choice). Replaces enterRoomCleared + enterChoosePath for
// brutalist arenas: advance the run to the next step, then bind each of that step's options to an
// open EXIT door (door[0] is the entrance, kept sealed). Each non-service door previews ONE reward
// rolled on the run-RNG; walking through door i commits that reward AND that route together.
void PulseGame::enterDoorsOpen(AudioSystem& audio) {
    ++roomsClearedTotal_;
    projectiles_.clear();
    audio.playMusicStinger(MusicStingerType::RoomClear, 0.75f, true);
    // v4 (C2): an anticipation riser distinct from the Reward breath, quantized into combat re-entry.
    if (tunables_.technoEnabled && tunables_.musicV4)
        audio.playMusicStinger(MusicStingerType::Anticipation, 0.62f, true);
    // Advance to the next step; if the run is exhausted that is a win.
    if (!run_.advanceRoom()) { enterRunOver(true, audio); return; }
    phase_ = RunPhase::DoorsOpen;
    cardCursor_ = 0;
    pendingDoorOption_ = -1;
    doorFadeTimer_ = 0.0f;
    doorFadeCommitted_ = false;

    std::vector<std::string> excluded = meta_.lockedContent();
    const std::vector<std::string> profileLocked = weaponProfiles_.invalidRewardIds();
    excluded.insert(excluded.end(), profileLocked.begin(), profileLocked.end());

    const std::vector<RoomSpec>& opts = run_.currentOptions();
    const std::vector<Door>& envDoors = wasteland_.doors();
    const int exits = std::max(0, static_cast<int>(envDoors.size()) - 1); // door[0] = entrance
    const int n = std::min(static_cast<int>(opts.size()), exits);
    doors_.clear();
    for (int i = 0; i < n; ++i) {
        DoorBind d;
        d.optionIndex = i;
        d.envDoorIndex = i + 1;                  // skip the entrance
        d.destType = opts[static_cast<size_t>(i)].type;
        // Service/boss rooms carry their own payoff; only Combat/Elite/Cache doors preview a reward.
        const bool rewardable = !(d.destType == RoomType::Shop || d.destType == RoomType::Event ||
                                  d.destType == RoomType::Boss);
        if (rewardable) {
            const float roomBias = (d.destType == RoomType::Elite || d.destType == RoomType::Cache) ? 0.6f : 0.0f;
            // Pulse greed: the room we just cleared rolls richer the hotter we fought it (+ Mirror Fortune).
            const float greedBias = roomPulsePeak_ * Pulse::kLootBias + meta_.mirrorTierBias();
            std::vector<std::string> one = build_.rollRewards(run_.rng(), 1, excluded, mods_.rewardTierBias + roomBias + greedBias);
            if (!one.empty()) { d.rewardId = one[0]; excluded.push_back(one[0]); } // keep doors distinct
        }
        const Door& ed = envDoors[static_cast<size_t>(d.envDoorIndex)];
        d.triggerCenter = { ed.worldX, ed.worldZ };
        d.triggerRadius = 1.2f;
        d.open = true;
        doors_.push_back(d);
        wasteland_.setDoorOpen(d.envDoorIndex, true); // unseal this exit's collision
    }
    playFeedback(audio, FeedbackEventType::UiReward, 0.9f);

    // Headless/bot: there is no walking through doors, so resolve the policy pick immediately.
    if (autoResolveDoors_) {
        const int pick = doors_.empty() ? -1 : botDoorPick();
        commitDoor(pick < 0 ? 0 : pick, audio);
    }
}

// Apply one reward by id (the per-door reward). Factored from grantReward without the room advance,
// since the door flow drives advancement itself.
void PulseGame::grantDoorReward(const std::string& rewardId, AudioSystem& audio) {
    if (rewardId.empty()) return;
    const Build::RewardView rv = build_.describeReward(rewardId);
    bool granted = false;
    if (rv.valid && rv.isWeapon) {
        acquireWeapon(rv.rawId, audio);
        granted = true;
    } else if (rv.valid) {
        const int prevMaxHp = effectiveMaxHealth();
        const int prevMaxSh = effectiveMaxShield();
        build_.add(rv.rawId, 1);
        const int dHp = effectiveMaxHealth() - prevMaxHp;
        const int dSh = effectiveMaxShield() - prevMaxSh;
        if (dHp > 0) player_.hp = std::min(effectiveMaxHealth(), player_.hp + dHp);
        if (dSh > 0) player_.shield = std::min(effectiveMaxShield(), player_.shield + dSh);
        playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f);
        granted = true;
    }
    if (granted) audio.playMusicStinger(MusicStingerType::Reward, 0.70f, true);
}

// Walk-through resolution: grant the door's reward, commit its route (the run already advanced in
// enterDoorsOpen), and load the next area. beginRoom seals the new arena + spawns at its entrance.
void PulseGame::commitDoor(int doorIndex, AudioSystem& audio) {
    if (doors_.empty()) { beginRoom(); return; }
    if (doorIndex < 0 || doorIndex >= static_cast<int>(doors_.size())) doorIndex = 0;
    const DoorBind d = doors_[static_cast<size_t>(doorIndex)];
    grantDoorReward(d.rewardId, audio);
    run_.chooseOption(d.optionIndex);
    doors_.clear();
    beginRoom();
}

// The open-door trigger the player currently overlaps (circle vs player_.pos), else -1.
int PulseGame::doorAtPlayer() const {
    for (int i = 0; i < static_cast<int>(doors_.size()); ++i) {
        if (!doors_[static_cast<size_t>(i)].open) continue;
        const float dx = player_.pos.x - doors_[static_cast<size_t>(i)].triggerCenter.x;
        const float dz = player_.pos.y - doors_[static_cast<size_t>(i)].triggerCenter.y;
        const float r  = doors_[static_cast<size_t>(i)].triggerRadius;
        if (dx * dx + dz * dz <= r * r) return i;
    }
    return -1;
}

// Leaving a Shop/Event (a connector area): in a door arena, open the connector's exits and walk
// on (Hades-consistent); otherwise fall back to the old menu advance.
void PulseGame::leaveServiceRoom(AudioSystem& audio) {
    if (envHasDoors()) enterDoorsOpen(audio);
    else advanceToNextRoom(audio);
}

// Deterministic headless door choice (no RNG): prefer a weapon reward, then a type policy
// (Cache when hurt, Shop when scrap-flush, Elite when healthy), mirroring the old ChoosePath bot.
int PulseGame::botDoorPick() const {
    if (doors_.empty()) return -1;
    const int maxHp = std::max(1, effectiveMaxHealth());
    const bool hurt = player_.hp * 2 <= maxHp;
    const bool flush = scrap_ >= 40;
    int best = 0, bestScore = -1000;
    for (int i = 0; i < static_cast<int>(doors_.size()); ++i) {
        const DoorBind& d = doors_[static_cast<size_t>(i)];
        int s = 0;
        if (!d.rewardId.empty()) {
            const Build::RewardView rv = build_.describeReward(d.rewardId);
            if (rv.valid && rv.isWeapon) s += 40; else if (rv.valid) s += 15;
        }
        switch (d.destType) {
            case RoomType::Cache:  s += hurt ? 30 : 12; break;
            case RoomType::Shop:   s += flush ? 25 : 8; break;
            case RoomType::Elite:  s += hurt ? 5 : 22; break;
            case RoomType::Event:  s += 10; break;
            case RoomType::Combat: s += 8; break;
            default:               s += 8; break;
        }
        if (s > bestScore) { bestScore = s; best = i; }
    }
    return best;
}

// Feature 2: roll the shop stock (4 mixed items on the run-RNG), priced by tier. Prices
// scale UP with scrapMult so a high-heat scrap-flush run does not trivialise the shop
// (review fix) - purchasing power stays roughly constant.
void PulseGame::rollShopStock() {
    std::vector<std::string> excluded = meta_.lockedContent();
    const std::vector<std::string> profileLocked = weaponProfiles_.invalidRewardIds();
    excluded.insert(excluded.end(), profileLocked.begin(), profileLocked.end());
    shopStock_ = build_.rollRewards(run_.rng(), 4, excluded, mods_.rewardTierBias);
    shopPrices_.clear();
    shopSold_.assign(shopStock_.size(), 0u);
    for (const std::string& id : shopStock_) {
        const Build::RewardView rv = build_.describeReward(id);
        const int base = rv.tier == ItemTier::Rare ? 60 : (rv.tier == ItemTier::Uncommon ? 28 : 12);
        shopPrices_.push_back(std::max(1, static_cast<int>(std::lround(base * mods_.scrapMult))));
    }
}

void PulseGame::enterShop() {
    phase_ = RunPhase::Shop;
    phaseTimer_ = 90.0f;       // generous fallback; the player leaves with SPACE
    enemies_.clear();
    projectiles_.clear();
    shopRerollCount_ = 0;
    rollShopStock();
}

void PulseGame::buyShopItem(int index, AudioSystem& audio) {
    if (index < 0 || index >= static_cast<int>(shopStock_.size())) return;
    if (shopSold_[static_cast<size_t>(index)]) return;
    if (scrap_ < shopPrices_[static_cast<size_t>(index)]) return;
    const Build::RewardView rv = build_.describeReward(shopStock_[static_cast<size_t>(index)]);
    if (!rv.valid) return;
    scrap_ -= shopPrices_[static_cast<size_t>(index)];
    shopSold_[static_cast<size_t>(index)] = 1u;
    if (rv.isWeapon) {
        acquireWeapon(rv.rawId, audio);   // plays its own powerup cue
    } else {
        const int prevMaxHp = effectiveMaxHealth();
        const int prevMaxSh = effectiveMaxShield();
        build_.add(rv.rawId, 1);
        const int dHp = effectiveMaxHealth() - prevMaxHp;
        const int dSh = effectiveMaxShield() - prevMaxSh;
        if (dHp > 0) player_.hp = std::min(effectiveMaxHealth(), player_.hp + dHp);
        if (dSh > 0) player_.shield = std::min(effectiveMaxShield(), player_.shield + dSh);
        playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f);
    }
    audio.playMusicStinger(MusicStingerType::Reward, 0.70f, true);
}

void PulseGame::shopHeal(AudioSystem& audio) {
    const int maxHp = std::max(1, effectiveMaxHealth());
    if (player_.hp >= maxHp) return;
    const int price = std::max(1, static_cast<int>(std::lround(24.0f * mods_.scrapMult)));
    if (scrap_ < price) return;
    scrap_ -= price;
    player_.hp = std::min(maxHp, player_.hp + healAmount(30)); // healMult scales the amount, not the price
    playFeedback(audio, FeedbackEventType::PickupHealth, 1.05f);
}

void PulseGame::shopReroll(AudioSystem& audio) {
    const int price = std::max(1, static_cast<int>(std::lround(
        (10.0f + 6.0f * static_cast<float>(shopRerollCount_)) * mods_.scrapMult)));
    if (scrap_ < price) return;
    scrap_ -= price;
    ++shopRerollCount_;
    rollShopStock();
    playFeedback(audio, FeedbackEventType::UiMove, 0.95f);
}

void PulseGame::shopForge(AudioSystem& audio) {
    // M6 build-crafting: pour scrap into the ACTIVE weapon to raise its power (more damage and
    // it unlocks the next ASPECT form to cycle with X). Price climbs with the weapon's power.
    if (activeWeapon_ < 0 || activeWeapon_ >= static_cast<int>(loadout_.size())) return;
    WeaponSlot& slot = loadout_[static_cast<size_t>(activeWeapon_)];
    const int price = std::max(1, static_cast<int>(std::lround((30.0f + 25.0f * static_cast<float>(slot.power - 1)) * mods_.scrapMult)));
    if (scrap_ < price) return;
    scrap_ -= price;
    ++slot.power;
    playFeedback(audio, FeedbackEventType::PickupPowerup, 1.1f);
    audio.playMusicStinger(MusicStingerType::Reward, 0.75f, true);
}

namespace {
// Feature 3 deal catalog. A deal is a risk/reward tradeoff applied on accept: it can grant
// items (grantsRare = a heavy-biased high-tier roll; grantItems = N rolled items), grant or
// charge scrap, and push RunModifiers (good OR bad) that combat reads through the existing
// M0 read-sites - no new combat hooks. scope 0 = whole run, 1 = this sector (expired when
// the sector advances).
struct Deal {
    const char* id;
    const char* name;
    const char* blurb;
    int grantItems;
    bool grantsRare;
    int scrap;
    int scope;                       // 0 run, 1 sector
    std::vector<RunModifier> mods;
};

const std::vector<Deal>& dealCatalog() {
    static const std::vector<Deal> kDeals = {
        { "bloodpact", "Blood Pact", "Gain 1 Rare item now - but enemies deal +25% damage all run.",
          0, true, 0, 0, { { ModKind::EnemyDamagePct, 0.25f, "" } } },
        { "reckless_greed", "Reckless Greed", "+60% scrap from drops all run - but rooms spawn +25% more enemies all run.",
          0, false, 0, 0, { { ModKind::ScrapPct, 0.60f, "" }, { ModKind::EnemyCountPct, 0.25f, "" } } },
        { "windfall", "Windfall", "Gain 35 scrap now - but healing received is -30% all run.",
          0, false, 35, 0, { { ModKind::HealReceivedPct, -0.30f, "" } } },
        { "trophy_hunt", "Trophy Hunt", "+50 loot-tier bias all run - but elite chance is +18% all run.",
          0, false, 0, 0, { { ModKind::EliteChancePct, 0.18f, "" }, { ModKind::RewardTierBias, 0.50f, "" } } },
        { "frenzy", "Frenzy", "Gain 1 item now - but waves spawn 35% faster this sector.",
          1, false, 0, 1, { { ModKind::EnemyCadencePct, 0.35f, "" } } },
        { "hard_bargain", "Hard Bargain", "Gain 1 Rare item now - but pay 25 scrap.",
          0, true, -25, 0, {} },
        // M6 expanded deal catalog (risk/reward variety): each trades a curse for a boon.
        { "iron_price", "Iron Price", "Gain 2 items now - but healing received is -25% all run.",
          2, false, 0, 0, { { ModKind::HealReceivedPct, -0.25f, "" } } },
        { "swarm_pact", "Swarm Pact", "Gain 1 item and 40 scrap now - but rooms spawn +30% more enemies all run.",
          1, false, 40, 0, { { ModKind::EnemyCountPct, 0.30f, "" } } },
        { "glass_cannon", "Glass Cannon", "Gain 1 Rare item now - but enemies deal +40% damage all run.",
          0, true, 0, 0, { { ModKind::EnemyDamagePct, 0.40f, "" } } },
        { "feast_or_famine", "Feast or Famine", "+40 loot-tier bias all run - but scrap income is -20% all run.",
          0, false, 0, 0, { { ModKind::RewardTierBias, 0.40f, "" }, { ModKind::ScrapPct, -0.20f, "" } } },
        { "blood_money", "Blood Money", "+50% scrap from drops all run - but enemies deal +20% damage all run.",
          0, false, 0, 0, { { ModKind::ScrapPct, 0.50f, "" }, { ModKind::EnemyDamagePct, 0.20f, "" } } },
        { "gauntlet", "Gauntlet", "Gain 1 item and 1 Rare now - but waves spawn 30% faster and elite chance is +15% this sector.",
          1, true, 0, 1, { { ModKind::EnemyCadencePct, 0.30f, "" }, { ModKind::EliteChancePct, 0.15f, "" } } },
        { "ascetic", "Ascetic Vow", "+60% meta payout all run - but scrap drops are disabled this sector.",
          0, false, 0, 1, { { ModKind::MetaPayoutPct, 0.60f, "" }, { ModKind::ScrapPct, -1.0f, "" } } },
        { "elite_hunt", "Elite Hunt", "Gain 1 item now - but elite chance is +30% this sector.",
          1, false, 0, 1, { { ModKind::EliteChancePct, 0.30f, "" } } },
        { "war_chest", "War Chest", "Gain 2 items now - but pay 30 scrap.",
          2, false, -30, 0, {} },
        { "tithe", "Tithe", "Gain 30 scrap now - but healing received is -15% all run.",
          0, false, 30, 0, { { ModKind::HealReceivedPct, -0.15f, "" } } },
    };
    return kDeals;
}
} // namespace

void PulseGame::enterEvent() {
    phase_ = RunPhase::Event;
    phaseTimer_ = 60.0f;       // generous fallback; the player accepts or declines
    enemies_.clear();
    projectiles_.clear();
    // Roll 2 distinct deals on the run-RNG (reproducible for a seed).
    const int n = static_cast<int>(dealCatalog().size());
    int a = run_.rng().rangeInt(0, n - 1);
    int b = run_.rng().rangeInt(0, n - 1);
    if (b == a) b = (b + 1) % n;
    eventDeals_ = { a, b };
}

void PulseGame::acceptDeal(int slot, AudioSystem& audio) {
    if (slot < 0 || slot >= static_cast<int>(eventDeals_.size())) return;
    const Deal& d = dealCatalog()[static_cast<size_t>(eventDeals_[static_cast<size_t>(slot)])];
    if (d.scrap < 0 && scrap_ < -d.scrap) return; // can't afford the scrap cost; ignore

    // Boon: scrap delta (may be a cost), then item grants via the reward roller.
    scrap_ = std::max(0, scrap_ + d.scrap);
    std::vector<std::string> excluded = meta_.lockedContent();
    const std::vector<std::string> profileLocked = weaponProfiles_.invalidRewardIds();
    excluded.insert(excluded.end(), profileLocked.begin(), profileLocked.end());
    const int grants = d.grantItems + (d.grantsRare ? 1 : 0);
    for (int g = 0; g < grants; ++g) {
        const float bias = (g == 0 && d.grantsRare) ? 6.0f : 0.6f; // grantsRare -> heavy rare bias
        const std::vector<std::string> ids = build_.rollRewards(run_.rng(), 1, excluded, bias);
        if (ids.empty()) continue;
        const Build::RewardView rv = build_.describeReward(ids[0]);
        if (!rv.valid) continue;
        if (rv.isWeapon) {
            acquireWeapon(rv.rawId, audio);
        } else {
            const int prevMaxHp = effectiveMaxHealth();
            const int prevMaxSh = effectiveMaxShield();
            build_.add(rv.rawId, 1);
            const int dHp = effectiveMaxHealth() - prevMaxHp;
            const int dSh = effectiveMaxShield() - prevMaxSh;
            if (dHp > 0) player_.hp = std::min(effectiveMaxHealth(), player_.hp + dHp);
            if (dSh > 0) player_.shield = std::min(effectiveMaxShield(), player_.shield + dSh);
        }
    }

    // Curse: push the deal's RunModifiers (the M0 read-sites pick them up). Tag with a
    // sourceId; a sector-scoped deal is registered so it expires when the sector advances.
    const std::string src = std::string("deal:") + d.id;
    for (const RunModifier& m : d.mods) activeMods_.push_back(RunModifier{ m.kind, m.value, src });
    if (d.scope == 1 && !d.mods.empty()) sectorCurses_.push_back({ src, run_.sector() });
    recomputeMods();

    playFeedback(audio, FeedbackEventType::UiConfirm, 1.0f);
    audio.playMusicStinger(MusicStingerType::Reward, 0.70f, true);
    leaveServiceRoom(audio); // the Event room is resolved -> doors (or the menu fallback)
}

void PulseGame::expireSectorCurses() {
    if (sectorCurses_.empty()) return;
    const int sec = run_.sector();
    bool changed = false;
    std::vector<std::pair<std::string, int>> kept;
    for (const auto& sc : sectorCurses_) {
        if (sec > sc.second) {
            const std::string id = sc.first;
            activeMods_.erase(std::remove_if(activeMods_.begin(), activeMods_.end(),
                [&](const RunModifier& m) { return m.sourceId == id; }), activeMods_.end());
            changed = true;
        } else {
            kept.push_back(sc);
        }
    }
    sectorCurses_.swap(kept);
    if (changed) recomputeMods();
}

void PulseGame::enterRunOver(bool won, AudioSystem& audio) {
    phase_ = RunPhase::RunOver;
    runWon_ = won;
    player_.dead = !won;
    restartTimer_ = won ? 2.4f : 1.0f; // dwell, then back to the Hub
    bestScore_ = std::max(bestScore_, score_);
    ++runsEndedTotal_;
    if (won) ++runsWonTotal_;
    // Meta payout (content-unlock currency only): reward depth + score + a win bonus.
    // RunMods: heat scales the payout (metaPayoutMult) - the persistent climb incentive.
    lastPayout_ = score_ + run_.roomIndex() * 5 + (won ? 60 : 0);
    lastPayout_ = std::max(0, static_cast<int>(std::lround(lastPayout_ * mods_.metaPayoutMult)));
    meta_.addCurrency(lastPayout_);
    // Feature 4: clearing a run at heat H unlocks H+1 (the ascension climb).
    if (won && forcedHeat_ < 0) meta_.unlockHeat(meta_.heat() + 1);
    meta_.save();   // persist immediately so a crash/quit never loses progress
    audio.playMusicStinger(won ? MusicStingerType::RunWin : MusicStingerType::RunLose, 1.0f, false);
    playFeedback(audio, won ? FeedbackEventType::RunWin : FeedbackEventType::RunLose, 1.0f);
}

void PulseGame::enterHub() {
    phase_ = RunPhase::Hub;
}

// Fold the user settings into tunables_ (FOV + effective sensitivity). Idempotent: it
// always rebuilds from settings_ and baseSensitivity_, so calling it repeatedly (slider
// drags, F5 reloads) never drifts. Shake/invert-Y read settings_ directly at their sites,
// and the volume buses are pushed in update(), so they are not touched here.
void PulseGame::applySettings() {
    tunables_.cameraFovDegrees = clamp(settings_.fovDegrees, 70.0f, 110.0f);
    tunables_.mouseSensitivity = baseSensitivity_ * clamp(settings_.sensitivity, 0.25f, 3.0f);
}

// Interactive windowed entry point: enable the shell, load + apply the saved options, and
// park the world in a valid run-free Hub state behind the main menu. Never called by the
// headless/sim/capture paths, so they never see the front-end at all.
void PulseGame::enterMainMenu() {
    frontEnd_ = true;
    settings_.fovDegrees = tunables_.cameraFovDegrees;   // seed the FOV default from config
    settings_.load();                                    // overlay the saved settings file
    settingsActive_ = true;
    applySettings();
    enemies_.clear();
    corpses_.clear();
    pendingEnemies_.clear();
    projectiles_.clear();
    beams_.clear();
    novaWaves_.clear();
    phase_ = RunPhase::Hub;
    menuScreen_ = MenuScreen::Main;
    menuSel_ = 0;
    pendingPauseConfirm_ = -1;
}

// Navigate the active front-end screen. Up/Down (or W/S) move the highlight, Left/Right
// (or A/D) adjust a slider, Enter/Space confirm, Esc backs out. Sliders apply live so the
// player hears/sees the change immediately; the options file is written on the way out.
void PulseGame::updateMenu(const InputState& input, AudioSystem& audio) {
    // Mouse over the rects buildMenuOverlay published last frame: hover highlights a row, a click
    // activates it (Main/Pause) or adjusts it (Settings rows). Settings tab + APPLY chips click too.
    bool mConfirm = false, mAdjustRight = false;
    float sliderFrac = -1.0f;   // >=0 when an Options slider bar is being click-dragged this frame
    {
        if (!input.mouseDown[0]) menuSliderDrag_ = false;   // drag ends when the button releases
        const float mx = static_cast<float>(input.mouseX), my = static_cast<float>(input.mouseY);
        int hov = -1;
        if (mx >= 0.0f && my >= 0.0f)
            for (const MenuHit& hh : menuHits_)   // bar hits are pushed before row hits, so a bar wins
                if (mx >= hh.x && mx <= hh.x + hh.w && my >= hh.y && my <= hh.y + hh.h) { hov = hh.id; break; }
        if (hov >= 300 && hov < 320) {            // an Options slider BAR -> press starts a drag
            const int row = hov - 300;
            if (menuSel_ != row) { menuSel_ = row; playFeedback(audio, FeedbackEventType::UiMove, 0.4f); }
            if (input.mousePressed[0]) menuSliderDrag_ = true;
        } else if (hov >= 0 && hov < 100) {
            if (menuSel_ != hov) { menuSel_ = hov; playFeedback(audio, FeedbackEventType::UiMove, 0.4f); }
            if (input.mousePressed[0]) { if (menuScreen_ == MenuScreen::Settings) mAdjustRight = true; else mConfirm = true; }
        } else if (hov >= 200 && hov < 205 && input.mousePressed[0]) {
            if (menuTab_ != hov - 200) { menuTab_ = hov - 200; menuSel_ = 0; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
        } else if (hov == 210 && input.mousePressed[0]) {
            mConfirm = true;   // APPLY
        }
        // While dragging, map the cursor X across the focused slider's bar to a 0..1 fraction.
        if (menuSliderDrag_ && input.mouseDown[0] && menuScreen_ == MenuScreen::Settings)
            for (const MenuHit& hh : menuHits_)
                if (hh.id == 300 + menuSel_) { sliderFrac = clamp((mx - hh.x) / std::max(1.0f, hh.w), 0.0f, 1.0f); break; }
    }
    const bool up      = input.pressed(VK_UP)    || input.pressed('W');
    const bool down    = input.pressed(VK_DOWN)  || input.pressed('S');
    const bool left    = input.pressed(VK_LEFT)  || input.pressed('A');
    const bool right   = (input.pressed(VK_RIGHT) || input.pressed('D')) || mAdjustRight;
    const bool confirm = (input.pressed(VK_RETURN) || input.pressed(VK_SPACE)) || mConfirm;
    const bool back    = input.pressed(VK_ESCAPE);

    auto navMove = [&](int count) {
        if (up)   { menuSel_ = (menuSel_ + count - 1) % count; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
        if (down) { menuSel_ = (menuSel_ + 1) % count;         playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
    };

    switch (menuScreen_) {
        case MenuScreen::Main: {
            navMove(3);   // 0 Play, 1 Options, 2 Quit
            if (confirm) {
                if (menuSel_ == 0) {
                    menuScreen_ = MenuScreen::None; phase_ = RunPhase::Hub;
                    playFeedback(audio, FeedbackEventType::UiConfirm, 0.9f);
                } else if (menuSel_ == 1) {
                    settingsReturn_ = MenuScreen::Main; menuScreen_ = MenuScreen::Settings; menuSel_ = 0; menuTab_ = 3; settingsDirty_ = false;
                    pendingPauseConfirm_ = -1;
                    playFeedback(audio, FeedbackEventType::UiConfirm, 0.8f);
                } else {
                    wantsQuit_ = true;
                }
            }
            break;
        }
        case MenuScreen::Pause: {
            const int oldSel = menuSel_;
            navMove(5);   // 0 Resume, 1 Options, 2 Restart Run, 3 Quit to Menu, 4 Quit to Desktop
            if (oldSel != menuSel_) pendingPauseConfirm_ = -1;
            if (back) {
                if (pendingPauseConfirm_ >= 0) pendingPauseConfirm_ = -1;
                else menuScreen_ = MenuScreen::None;
                playFeedback(audio, FeedbackEventType::UiCancel, 0.7f);
                break;
            }
            if (confirm) {
                switch (menuSel_) {
                    case 0: pendingPauseConfirm_ = -1; menuScreen_ = MenuScreen::None; playFeedback(audio, FeedbackEventType::UiCancel, 0.7f); break;
                    case 1: pendingPauseConfirm_ = -1; settingsReturn_ = MenuScreen::Pause; menuScreen_ = MenuScreen::Settings; menuSel_ = 0; menuTab_ = 3; settingsDirty_ = false;
                            playFeedback(audio, FeedbackEventType::UiConfirm, 0.8f); break;
                    case 2:
                        if (pendingPauseConfirm_ == menuSel_) {
                            pendingPauseConfirm_ = -1; resetRun(); menuScreen_ = MenuScreen::None;
                            playFeedback(audio, FeedbackEventType::UiConfirm, 0.9f);
                        } else { pendingPauseConfirm_ = menuSel_; playFeedback(audio, FeedbackEventType::UiMove, 0.7f); }
                        break;
                    case 3:
                        if (pendingPauseConfirm_ == menuSel_) {
                            pendingPauseConfirm_ = -1; enterMainMenu(); playFeedback(audio, FeedbackEventType::UiConfirm, 0.8f);
                        } else { pendingPauseConfirm_ = menuSel_; playFeedback(audio, FeedbackEventType::UiMove, 0.7f); }
                        break;
                    case 4:
                        if (pendingPauseConfirm_ == menuSel_) wantsQuit_ = true;
                        else { pendingPauseConfirm_ = menuSel_; playFeedback(audio, FeedbackEventType::UiMove, 0.7f); }
                        break;
                }
            }
            break;
        }
        case MenuScreen::Settings: {
            // Tabbed Options (UI guide): AUDIO / CONTROLS / VIDEO / ACCESSIBILITY / GAMEPLAY.
            // Q/E (or [ ]) switch tabs; UP/DOWN move rows; LEFT/RIGHT adjust; ENTER = APPLY (save
            // to disk + clear UNSAVED); ESC saves + backs out.
            constexpr int kTabs = 5;
            const int tabRows[kTabs] = { 7, 3, 3, 8, 2 };   // AUDIO gained 4 v4 mix/accessibility rows
            const bool tabPrev = input.pressed('Q') || input.pressed(VK_OEM_4);  // [
            const bool tabNext = input.pressed('E') || input.pressed(VK_OEM_6);  // ]
            if (tabPrev) { menuTab_ = (menuTab_ + kTabs - 1) % kTabs; menuSel_ = 0; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
            if (tabNext) { menuTab_ = (menuTab_ + 1) % kTabs; menuSel_ = 0; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
            const int rows = tabRows[menuTab_];
            if (rows > 0) navMove(rows);
            bool changed = false;
            auto adj = [&](float& v, float lo, float hi, float step) {
                if (sliderFrac >= 0.0f) {   // click-drag: set the value from the cursor, snapped to the step grid
                    float nv = clamp(lo + std::round((sliderFrac * (hi - lo)) / step) * step, lo, hi);
                    if (nv != v) { v = nv; changed = true; }
                    return;
                }
                if (left)  { v = std::max(lo, v - step); changed = true; }
                if (right) { v = std::min(hi, v + step); changed = true; }
            };
            auto toggle = [&](bool& b) { if (left || right) { b = !b; changed = true; } };
            switch (menuTab_) {
                case 0: // AUDIO
                    if (menuSel_ == 0) adj(settings_.masterVolume, 0.0f, 1.0f, 0.05f);
                    else if (menuSel_ == 1) adj(settings_.sfxVolume, 0.0f, 1.0f, 0.05f);
                    else if (menuSel_ == 2) adj(settings_.musicVolume, 0.0f, 1.0f, 0.05f);
                    else if (menuSel_ == 3) adj(settings_.musicDuckDepth, 0.0f, 1.0f, 0.5f);  // Off / Subtle / Strong
                    else if (menuSel_ == 4) toggle(settings_.monoAudio);
                    else if (menuSel_ == 5) toggle(settings_.reducedIntensityAudio);
                    else toggle(settings_.combatReadability);
                    break;
                case 1: // CONTROLS
                    if (menuSel_ == 0) adj(settings_.sensitivity, 0.25f, 3.0f, 0.05f);
                    else if (menuSel_ == 1) toggle(settings_.invertY);
                    else toggle(settings_.toggleAim);
                    break;
                case 2: // VIDEO
                    if (menuSel_ == 0) { // display mode: Windowed (0) / Borderless fullscreen (1), applied live
                        if (left)  { settings_.displayMode = 0; changed = true; }
                        if (right) { settings_.displayMode = 1; changed = true; }
                    } else if (menuSel_ == 1) adj(settings_.fovDegrees, 70.0f, 110.0f, 1.0f);
                    else if (menuSel_ == 2) { // graphics quality (applies on next launch; saved not hot-swapped)
                        if (left)  { settings_.graphicsQuality = std::max(0, settings_.graphicsQuality - 1); changed = true; }
                        if (right) { settings_.graphicsQuality = std::min(3, settings_.graphicsQuality + 1); changed = true; }
                    } else toggle(settings_.vsync);
                    break;
                case 3: // ACCESSIBILITY
                    if (menuSel_ == 0) adj(settings_.textScale, 0.85f, 1.30f, 0.05f);
                    else if (menuSel_ == 1) adj(settings_.hudScale, 0.85f, 1.20f, 0.05f);
                    else if (menuSel_ == 2) {
                        if (left)  { settings_.colorblindPreset = std::max(0, settings_.colorblindPreset - 1); changed = true; }
                        if (right) { settings_.colorblindPreset = std::min(3, settings_.colorblindPreset + 1); changed = true; }
                    } else if (menuSel_ == 3) toggle(settings_.highContrast);
                    else if (menuSel_ == 4) toggle(settings_.reduceFlashes);
                    else if (menuSel_ == 5) toggle(settings_.reduceMotion);
                    else if (menuSel_ == 6) toggle(settings_.reduceBloom);
                    else adj(settings_.shakeScale, 0.0f, 1.5f, 0.05f);
                    break;
                case 4: // GAMEPLAY
                    if (menuSel_ == 0) {
                        if (left)  { settings_.reticleStyle = std::max(0, settings_.reticleStyle - 1); changed = true; }
                        if (right) { settings_.reticleStyle = std::min(2, settings_.reticleStyle + 1); changed = true; }
                    } else if (menuSel_ == 1) toggle(settings_.toggleAim);
                    else toggle(settings_.vsync);
                    break;
                default: break;
            }
            if (changed) { applySettings(); settingsDirty_ = true; playFeedback(audio, FeedbackEventType::UiMove, 0.5f); }
            if (confirm && settingsDirty_) {   // APPLY: persist now
                settings_.save(); applySettings(); settingsDirty_ = false;
                playFeedback(audio, FeedbackEventType::UiConfirm, 0.8f);
            }
            if (back) {
                settings_.save();
                applySettings();
                settingsDirty_ = false;
                menuScreen_ = settingsReturn_;
                menuSel_ = 0; menuTab_ = 0;
                playFeedback(audio, FeedbackEventType::UiCancel, 0.7f);
            }
            break;
        }
        case MenuScreen::None:
            break;
    }
}

// Between-runs hub: one focus cursor, moved by the main navigation keys or mouse hover.
// Space/click activates the focused thing. No purchase-only hotkeys are required.
void PulseGame::updateHub(const InputState& input, AudioSystem& audio, int screenW, int screenH) {
    const std::vector<UnlockDef>& unlockables = meta_.unlockables();
    const int catCount = std::min(9, static_cast<int>(unlockables.size()));
    const int mirrorStart = catCount;
    const int loadoutPrevFocus = mirrorStart + Meta::MirrorCount;
    const int loadoutNextFocus = loadoutPrevFocus + 1;
    const int heatDownFocus = loadoutNextFocus + 1;
    const int heatUpFocus = heatDownFocus + 1;
    const int manualFocus = heatUpFocus + 1;
    const int startFocus = manualFocus + 1;
    const int focusCount = startFocus + 1;
    cardCursor_ = std::max(0, std::min(cardCursor_, focusCount - 1));

    const float s = std::min(static_cast<float>(screenW) / 1920.0f, static_cast<float>(screenH) / 1080.0f);
    const float padX = 56.0f * s, padY = 48.0f * s, gap = 24.0f * s;
    const float frameW = static_cast<float>(screenW) - padX * 2.0f;
    const float bodyY = 160.0f * s;
    const float startH = 128.0f * s, startY = static_cast<float>(screenH) - padY - startH;
    const float bodyH = startY - bodyY - gap;
    const float col1W = 386.0f * s, col3W = 430.0f * s;
    const float col2W = frameW - col1W - col3W - gap * 2.0f;
    const float col1X = padX, col2X = col1X + col1W + gap, col3X = col2X + col2W + gap;
    const float mirrorH = 336.0f * s, contractsY = bodyY + mirrorH + gap;
    const float manualW = 224.0f * s, manualH = 48.0f * s;
    const float manualX = static_cast<float>(screenW) - padX - manualW, manualY = padY + 17.0f * s;

    if (codexOpen_) {
        const float codexPadX = 56.0f * s, codexPadY = 48.0f * s;
        const float closeW = 224.0f * s, closeH = 52.0f * s;
        const float closeX = static_cast<float>(screenW) - codexPadX - closeW, closeY = codexPadY;
        const float mx = static_cast<float>(input.mouseX), my = static_cast<float>(input.mouseY);
        const bool closeHover = mx >= closeX && mx <= closeX + closeW && my >= closeY && my <= closeY + closeH;
        if (input.pressed(VK_SPACE) || (input.mousePressed[0] && closeHover)) {
            codexOpen_ = false;
            playFeedback(audio, FeedbackEventType::UiCancel, 0.75f);
            return;
        }
        // Switch the manual's tab: arrows / Q,E / A,D step it, or click a tab in the bar (hit-rects
        // are published by drawCodex). 9 tabs: PULSE..ROUTES.
        constexpr int kCodexTabs = 9;
        int newTab = codexTab_;
        if (input.pressed(VK_LEFT) || input.pressed('A') || input.pressed('Q'))
            newTab = (codexTab_ + kCodexTabs - 1) % kCodexTabs;
        if (input.pressed(VK_RIGHT) || input.pressed('D') || input.pressed('E'))
            newTab = (codexTab_ + 1) % kCodexTabs;
        if (input.mousePressed[0] && my >= codexTabBarY_ && my <= codexTabBarY_ + codexTabBarH_) {
            for (int i = 0; i < kCodexTabs; ++i)
                if (mx >= codexTabX_[static_cast<size_t>(i)] &&
                    mx <= codexTabX_[static_cast<size_t>(i)] + codexTabW_[static_cast<size_t>(i)]) { newTab = i; break; }
        }
        if (newTab != codexTab_) {
            codexTab_ = newTab;
            playFeedback(audio, FeedbackEventType::UiMove, 0.7f);
        }
        return;
    }

    auto cycleLoadout = [&](int dir) {
        const std::vector<std::string> opts = meta_.starterOptions();
        if (opts.size() <= 1) { playFeedback(audio, FeedbackEventType::UiCancel, 0.55f); return; }
        int idx = 0;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i)
            if (opts[static_cast<size_t>(i)] == meta_.startingWeapon()) { idx = i; break; }
        const int cnt = static_cast<int>(opts.size());
        meta_.setStartingWeapon(opts[static_cast<size_t>((idx + (dir > 0 ? 1 : cnt - 1)) % cnt)]);
        meta_.save();
        playFeedback(audio, FeedbackEventType::UiMove, 0.85f);
    };

    auto activate = [&](int focus) {
        if (focus >= 0 && focus < catCount) {
            if (meta_.buy(unlockables[static_cast<size_t>(focus)].id)) {
                meta_.save();
                playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f);
            } else {
                playFeedback(audio, FeedbackEventType::UiCancel, 0.55f);
            }
        } else if (focus >= mirrorStart && focus < mirrorStart + Meta::MirrorCount) {
            const int node = focus - mirrorStart;
            if (meta_.upgradeMirror(node)) { meta_.save(); playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f); }
            else playFeedback(audio, FeedbackEventType::UiCancel, 0.55f);
        } else if (focus == loadoutPrevFocus) {
            cycleLoadout(-1);
        } else if (focus == loadoutNextFocus) {
            cycleLoadout(1);
        } else if (focus == heatDownFocus) {
            meta_.setHeat(meta_.heat() - 1);
            meta_.save();
            playFeedback(audio, FeedbackEventType::UiMove, 0.7f);
        } else if (focus == heatUpFocus) {
            const int before = meta_.heat();
            meta_.setHeat(meta_.heat() + 1);
            meta_.save();
            playFeedback(audio, meta_.heat() > before ? FeedbackEventType::UiConfirm : FeedbackEventType::UiCancel, 0.7f);
        } else if (focus == manualFocus) {
            codexOpen_ = true;
            playFeedback(audio, FeedbackEventType::UiConfirm, 0.8f);
        } else if (focus == startFocus) {
            resetRun();
        }
    };

    if (input.pressed(VK_LEFT) || input.pressed(VK_UP) || input.pressed('A') || input.pressed('W')) {
        cardCursor_ = (cardCursor_ + focusCount - 1) % focusCount;
        playFeedback(audio, FeedbackEventType::UiMove, 0.6f);
    }
    if (input.pressed(VK_RIGHT) || input.pressed(VK_DOWN) || input.pressed('D') || input.pressed('S')) {
        cardCursor_ = (cardCursor_ + 1) % focusCount;
        playFeedback(audio, FeedbackEventType::UiMove, 0.6f);
    }

    struct HubHit { float x, y, w, h; int focus; };
    std::vector<HubHit> hits;
    hits.reserve(static_cast<size_t>(focusCount));
    const float catIx = col3X + 26.0f * s, catIw = col3W - 52.0f * s;
    const float catRowY = bodyY + 66.0f * s, catRowH = 54.0f * s, catRowGap = 10.0f * s;
    for (int i = 0; i < catCount; ++i)
        hits.push_back({ catIx, catRowY + static_cast<float>(i) * (catRowH + catRowGap), catIw, catRowH, i });
    const float mirIx = col2X + 18.0f * s;
    for (int i = 0; i < Meta::MirrorCount; ++i)
        hits.push_back({ mirIx, bodyY + 63.0f * s + static_cast<float>(i) * 40.0f * s, col2W - 36.0f * s, 35.0f * s, mirrorStart + i });
    const float loadIx = col1X + 26.0f * s, loadIw = col1W - 52.0f * s;
    const float loadBtnY = bodyY + bodyH - 80.0f * s;
    const float loadBtnW = (loadIw - 12.0f * s) * 0.5f;
    hits.push_back({ loadIx, loadBtnY, loadBtnW, 38.0f * s, loadoutPrevFocus });
    hits.push_back({ loadIx + loadBtnW + 12.0f * s, loadBtnY, loadBtnW, 38.0f * s, loadoutNextFocus });
    const float heatBtnY = contractsY + 17.0f * s, heatBtnW = 78.0f * s, heatBtnH = 30.0f * s;
    hits.push_back({ col2X + col2W - 30.0f * s - heatBtnW * 2.0f - 10.0f * s, heatBtnY, heatBtnW, heatBtnH, heatDownFocus });
    hits.push_back({ col2X + col2W - 30.0f * s - heatBtnW, heatBtnY, heatBtnW, heatBtnH, heatUpFocus });
    hits.push_back({ manualX, manualY, manualW, manualH, manualFocus });
    hits.push_back({ padX, startY, frameW, startH, startFocus });

    const float mx = static_cast<float>(input.mouseX), my = static_cast<float>(input.mouseY);
    int hoverFocus = -1;
    if (mx >= 0.0f && my >= 0.0f && mx < static_cast<float>(screenW) && my < static_cast<float>(screenH)) {
        for (const HubHit& h : hits) {
            if (mx >= h.x && mx <= h.x + h.w && my >= h.y && my <= h.y + h.h) {
                hoverFocus = h.focus;
                break;
            }
        }
    }
    if (hoverFocus >= 0 && hoverFocus != cardCursor_) {
        cardCursor_ = hoverFocus;
        playFeedback(audio, FeedbackEventType::UiMove, 0.35f);
    }
    if (input.mousePressed[0] && hoverFocus >= 0) activate(hoverFocus);
    else if (input.pressed(VK_SPACE)) activate(cardCursor_);
}

const char* PulseGame::phaseName() const {
    switch (phase_) {
        case RunPhase::Hub: return "Hub";
        case RunPhase::ChoosePath: return "ChoosePath";
        case RunPhase::Shop: return "Shop";
        case RunPhase::Event: return "Event";
        case RunPhase::InRoom: return "InRoom";
        case RunPhase::RoomCleared: return "RoomCleared";
        case RunPhase::Boss: return "Boss";
        case RunPhase::RunOver: return "RunOver";
        case RunPhase::DoorsOpen: return "DoorsOpen";
    }
    return "?";
}

// Phase A intensity spine: the run/wave state sets a floor under combatIntensity_
// so the techno ramps across a room and is full during a boss (a cheap down payment
// on the deferred reactive techno). RunOver lets it decay to silence.
float PulseGame::runIntensityFloor() const {
    if (phase_ == RunPhase::RoomCleared) return 0.30f;
    if (phase_ != RunPhase::InRoom && phase_ != RunPhase::Boss) return 0.0f;
    if (run_.complete()) return 0.0f;
    const RoomSpec& room = run_.currentRoom();
    const int total = std::max(1, static_cast<int>(room.waves.size()));
    const float waveProg = clamp(static_cast<float>(waveIndex_) / static_cast<float>(total), 0.0f, 1.0f);
    float floor = 0.22f + 0.30f * waveProg + 0.08f * static_cast<float>(run_.sector());
    if (run_.currentIsBoss()) floor = std::max(floor, 0.80f);
    return clamp(floor, 0.0f, 0.95f);
}

void PulseGame::update(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH) {
    if (input.pressed(VK_F5)) {
        loadConfig(true);
        audio.play(SoundEventType::Config, 0.9f);
    }
    // M7: [C] toggles the SYSTEMS field manual in any non-combat menu (a reference you read while
    // planning a build or choosing a route). Combat phases ignore it so 'C' never disrupts a fight.
    {
        const bool menuPhase = phase_ == RunPhase::Hub || phase_ == RunPhase::ChoosePath ||
                               phase_ == RunPhase::DoorsOpen || phase_ == RunPhase::RoomCleared ||
                               phase_ == RunPhase::Shop || phase_ == RunPhase::Event || phase_ == RunPhase::RunOver;
        if (menuPhase && input.pressed('C')) {
            codexOpen_ = !codexOpen_;
            playFeedback(audio, codexOpen_ ? FeedbackEventType::UiConfirm : FeedbackEventType::UiCancel, 0.8f);
        } else if (!menuPhase) {
            codexOpen_ = false;   // never leave it up into combat
        }
    }

    // Front-end shell (interactive windowed play only). A main-menu / pause / options
    // screen freezes the sim behind it; the frame a screen OPENS consumes its key here so
    // it does not immediately re-trigger inside updateMenu. Headless/sim/capture paths keep
    // frontEnd_ == false and skip all of this, so their behavior is unchanged.
    bool frozen = false;
    if (frontEnd_) {
        if (menuScreen_ == MenuScreen::None) {
            if (input.pressed(VK_ESCAPE)) {
                if (phase_ == RunPhase::Hub) { enterMainMenu(); playFeedback(audio, FeedbackEventType::UiCancel, 0.7f); }
                else { menuScreen_ = MenuScreen::Pause; menuSel_ = 0; playFeedback(audio, FeedbackEventType::UiConfirm, 0.7f); }
                frozen = true;
            }
        } else {
            updateMenu(input, audio);
            frozen = true;
        }
    }

    // Top-level run/room state machine. The live combat sim runs only InRoom/Boss;
    // RoomCleared and RunOver are short non-combat dwells (Phase B turns RoomCleared
    // into the reward-choice selection state).
    // Reset the card focus cursor whenever the active screen changes.
    if (phase_ != cardCursorPhase_) { cardCursor_ = 0; cardCursorPhase_ = phase_; }

    // Menu hover/focus pop: ease 0->1 while a focus holds, reset the instant it changes, so the
    // focused/hovered card gets a subtle grow-in glow. Shared by every run-phase menu.
    {
        const int fk = (frontEnd_ && menuScreen_ != MenuScreen::None)
            ? (0x10000 | (static_cast<int>(menuScreen_) << 12) | ((menuTab_ & 0xf) << 8) | (menuSel_ & 0xff))
            : ((static_cast<int>(phase_) << 8) | (cardCursor_ & 0xff));
        if (fk != menuFocusKey_) { menuFocusKey_ = fk; menuFocusAnim_ = 0.0f; }
        else menuFocusAnim_ = std::min(1.0f, menuFocusAnim_ + dt / 0.16f);
    }
    // Shared mouse pick over the clickable rects buildHud published last frame (one-frame lag is
    // imperceptible). Returns the id under the cursor, or -1. Used by each menu phase below.
    auto menuPick = [&]() -> int {
        const float mx = static_cast<float>(input.mouseX), my = static_cast<float>(input.mouseY);
        if (mx < 0.0f || my < 0.0f) return -1;
        for (const MenuHit& hh : menuHits_)
            if (mx >= hh.x && mx <= hh.x + hh.w && my >= hh.y && my <= hh.y + hh.h) return hh.id;
        return -1;
    };
    (void)menuPick;

    // Door transition fade: a brief frozen "load" between areas. It runs ACROSS the phase swap
    // (DoorsOpen -> the next room's InRoom/Boss/Shop) so the screen darkens, the area loads at the
    // black midpoint, then brightens. Freezing the sim for ~0.55s reads as a Returnal-style load.
    if (doorFadeTimer_ > 0.0f && !frozen) {
        doorFadeTimer_ -= dt;
        if (!doorFadeCommitted_ && doorFadeTimer_ <= doorFadeDuration_ * 0.5f) {
            doorFadeCommitted_ = true;
            commitDoor(pendingDoorOption_, audio); // load the next area while the screen is black
        }
        if (doorFadeTimer_ <= 0.0f) { doorFadeTimer_ = 0.0f; pendingDoorOption_ = -1; doorFadeCommitted_ = false; }
        else frozen = true;                        // hold the sim until the fade completes
    }

    if (!frozen) switch (phase_) {
        case RunPhase::InRoom:
        case RunPhase::Boss:
            updateCombat(input, audio, dt, screenW, screenH);
            if (roomComplete()) {
                if (run_.currentIsBoss() && run_.currentIsFinal()) {
                    enterRunOver(true, audio); // final boss cleared -> run won
                } else if (envHasDoors()) {
                    enterDoorsOpen(audio);     // spatial doors: walk through one to pick reward + route
                } else {
                    enterRoomCleared(audio);   // menu fallback (dungeon / unloaded-env sim)
                }
            }
            break;
        case RunPhase::DoorsOpen: {
            // Post-clear: enemies are dead, the player keeps full control and walks to a door.
            // Movement runs through updateCombat (identical feel); overlapping an open door's
            // trigger arms the transition fade, which commits the choice at its midpoint.
            updateCombat(input, audio, dt, screenW, screenH);
            const int d = doorAtPlayer();
            if (d >= 0) {
                pendingDoorOption_ = d;
                doorFadeTimer_ = doorFadeDuration_;
                doorFadeCommitted_ = false;
                player_.vel = {};
                playFeedback(audio, FeedbackEventType::UiConfirm, 0.85f);
            }
            break;
        }
        case RunPhase::RoomCleared: {
            // Time-stopped reward selection: arrow/WASD move the focus, Enter/Space
            // commits it, [1]/[2]/[3] jump+commit; a timeout auto-picks the focus so an
            // idle session never softlocks.
            phaseTimer_ -= dt;
            const int n = static_cast<int>(rewardOptions_.size());
            if (n > 0) {
                if (input.pressed(VK_LEFT) || input.pressed('A') || input.pressed(VK_UP) || input.pressed('W'))
                    { cardCursor_ = (cardCursor_ + n - 1) % n; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
                if (input.pressed(VK_RIGHT) || input.pressed('D') || input.pressed(VK_DOWN) || input.pressed('S'))
                    { cardCursor_ = (cardCursor_ + 1) % n; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
            }
            int pick = -1;
            if (input.pressed('1')) pick = 0;
            else if (input.pressed('2')) pick = 1;
            else if (input.pressed('3')) pick = 2;
            else if (input.pressed(VK_RETURN) || input.pressed(VK_SPACE)) pick = cardCursor_;
            { const int hov = menuPick();
              if (hov >= 0 && hov < n) {
                  if (cardCursor_ != hov) { cardCursor_ = hov; playFeedback(audio, FeedbackEventType::UiMove, 0.4f); }
                  if (input.mousePressed[0]) pick = hov;
              } }
            if (pick < 0 && phaseTimer_ <= 0.0f) pick = cardCursor_;
            if (pick >= 0 && pick < n) grantReward(pick, audio);
            updateTimers(dt);
            break;
        }
        case RunPhase::ChoosePath: {
            // Time-stopped path choice: arrow/WASD move the focus, Enter/Space commits it,
            // [1]/[2]/[3] jump+commit; a timeout auto-picks the focus so an idle/headless
            // session never softlocks.
            phaseTimer_ -= dt;
            const int n = static_cast<int>(run_.currentOptions().size());
            if (n > 0) {
                if (input.pressed(VK_LEFT) || input.pressed('A') || input.pressed(VK_UP) || input.pressed('W'))
                    { cardCursor_ = (cardCursor_ + n - 1) % n; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
                if (input.pressed(VK_RIGHT) || input.pressed('D') || input.pressed(VK_DOWN) || input.pressed('S'))
                    { cardCursor_ = (cardCursor_ + 1) % n; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
            }
            int pick = -1;
            if (input.pressed('1')) pick = 0;
            else if (input.pressed('2')) pick = 1;
            else if (input.pressed('3')) pick = 2;
            else if (input.pressed(VK_RETURN) || input.pressed(VK_SPACE)) pick = cardCursor_;
            { const int hov = menuPick();
              if (hov >= 0 && hov < n) {
                  if (cardCursor_ != hov) { cardCursor_ = hov; playFeedback(audio, FeedbackEventType::UiMove, 0.4f); }
                  if (input.mousePressed[0]) pick = hov;
              } }
            if (pick < 0 && phaseTimer_ <= 0.0f) pick = cardCursor_;
            if (pick >= 0 && pick < n) {
                run_.chooseOption(pick);
                beginRoom();
            }
            updateTimers(dt);
            break;
        }
        case RunPhase::Shop: {
            // Time-stopped shop: 1-4 buy a stock item, H heal, R reroll, SPACE leave. A
            // generous fallback leaves so an idle/headless session never softlocks.
            phaseTimer_ -= dt;
            if (input.pressed('1')) buyShopItem(0, audio);
            else if (input.pressed('2')) buyShopItem(1, audio);
            else if (input.pressed('3')) buyShopItem(2, audio);
            else if (input.pressed('4')) buyShopItem(3, audio);
            else if (input.pressed('H')) shopHeal(audio);
            else if (input.pressed('R')) shopReroll(audio);
            else if (input.pressed('F')) shopForge(audio);   // M6: forge the active weapon (+power)
            else if (input.pressed(VK_SPACE) || phaseTimer_ <= 0.0f) leaveServiceRoom(audio); // leave
            else {
                const int hov = menuPick();   // click anything in the shop: stock cards + service rows
                if (hov >= 0 && hov < static_cast<int>(shopStock_.size())) {
                    if (cardCursor_ != hov) { cardCursor_ = hov; playFeedback(audio, FeedbackEventType::UiMove, 0.4f); }
                    if (input.mousePressed[0]) buyShopItem(hov, audio);
                } else if (input.mousePressed[0]) {
                    if (hov == MenuIdHeal) shopHeal(audio);
                    else if (hov == MenuIdReroll) shopReroll(audio);
                    else if (hov == MenuIdForge) shopForge(audio);
                    else if (hov == MenuIdLeave) leaveServiceRoom(audio);
                }
            }
            updateTimers(dt);
            break;
        }
        case RunPhase::Event: {
            // Time-stopped deal: arrow/WASD move the focus, Enter accepts the focused deal,
            // [1]/[2] accept directly, SPACE declines (the safe default). A generous fallback
            // declines so an idle/headless session never softlocks.
            phaseTimer_ -= dt;
            const int n = static_cast<int>(eventDeals_.size());
            if (n > 0) {
                if (input.pressed(VK_LEFT) || input.pressed('A') || input.pressed(VK_UP) || input.pressed('W'))
                    { cardCursor_ = (cardCursor_ + n - 1) % n; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
                if (input.pressed(VK_RIGHT) || input.pressed('D') || input.pressed(VK_DOWN) || input.pressed('S'))
                    { cardCursor_ = (cardCursor_ + 1) % n; playFeedback(audio, FeedbackEventType::UiMove, 0.6f); }
            }
            if (input.pressed('1')) acceptDeal(0, audio);
            else if (input.pressed('2')) acceptDeal(1, audio);
            else if (input.pressed(VK_RETURN)) { if (cardCursor_ < n) acceptDeal(cardCursor_, audio); }
            else if (input.pressed(VK_SPACE) || phaseTimer_ <= 0.0f) leaveServiceRoom(audio); // decline
            else {
                const int hov = menuPick();   // click a pact card to accept, or the decline bar to leave
                if (hov >= 0 && hov < n) {
                    if (cardCursor_ != hov) { cardCursor_ = hov; playFeedback(audio, FeedbackEventType::UiMove, 0.4f); }
                    if (input.mousePressed[0]) acceptDeal(hov, audio);
                } else if (input.mousePressed[0] && hov == MenuIdDecline) leaveServiceRoom(audio);
            }
            updateTimers(dt);
            break;
        }
        case RunPhase::RunOver:
            restartTimer_ -= dt;
            if (input.mousePressed[0] && menuPick() == MenuIdContinue) restartTimer_ = 0.0f; // click CONTINUE
            if (restartTimer_ <= 0.0f) enterHub(); // payout already banked; go to the hub
            updateTimers(dt);
            break;
        case RunPhase::Hub:
            updateHub(input, audio, screenW, screenH); // spend meta, then START a fresh run
            updateTimers(dt);
            break;
    }

    MusicState musicState = MusicState::Combat;
    switch (phase_) {
        case RunPhase::Hub: musicState = MusicState::Hub; break;
        case RunPhase::ChoosePath:
        case RunPhase::DoorsOpen:
        case RunPhase::RoomCleared: musicState = MusicState::Reward; break;
        case RunPhase::Boss: musicState = MusicState::Boss; break;
        case RunPhase::RunOver: musicState = MusicState::RunOver; break;
        case RunPhase::InRoom: musicState = MusicState::Combat; break;
    }
    // Settings volume buses fold in here: music via its baseVolume, SFX via the global
    // gain. Both default to 1.0 when settings are inactive (headless), leaving the mix as-is.
    const float masterMul = settingsActive_ ? settings_.masterVolume : 1.0f;
    const float musicMul  = settingsActive_ ? settings_.musicVolume  : 1.0f;
    musicOverpulseCooldown_ = std::max(0.0f, musicOverpulseCooldown_ - dt);
    const bool inPulseCombat = (phase_ == RunPhase::InRoom || phase_ == RunPhase::Boss);
    const bool isOverpulse = inPulseCombat && pulse_.tier() == PulseTier::Overpulse;
    const float musicVol = tunables_.technoBaseVolume * masterMul * musicMul;
    const MusicBiome musicBiome = musicBiomeFromWorld(currentBiome_);
    // v4 reactive-tension inputs. duress ramps as the player nears death (only while in a fight);
    // boss escalation tracks the boss HP. Computed regardless of the flag so the --music-trace row
    // is meaningful, but only fed through when musicV4 is on (else the legacy setter is byte-identical).
    float duress = 0.0f;
    if (inPulseCombat) {
        const float maxHpF = static_cast<float>(std::max(1, effectiveMaxHealth()));
        const float hpFrac = std::clamp(static_cast<float>(player_.hp) / maxHpF, 0.0f, 1.0f);
        const float tdr = std::clamp((0.35f - hpFrac) / (0.35f - 0.06f), 0.0f, 1.0f);
        duress = tdr * tdr * (3.0f - 2.0f * tdr);   // smoothstep: 0 at >=35% HP, 1 approaching death
    }
    float bossEsc = 0.0f;
    if (phase_ == RunPhase::Boss) {
        for (const Enemy& e : enemies_) {
            if (e.active && e.boss && e.maxHealth > 0.0f) {
                bossEsc = std::clamp(1.0f - e.health / e.maxHealth, 0.0f, 1.0f);
                break;
            }
        }
    }
    musicBossEscCooldown_ = std::max(0.0f, musicBossEscCooldown_ - dt);
    if (tunables_.musicV4) {
        MusicContext ctx;
        ctx.enabled = tunables_.technoEnabled;
        ctx.bpm = tunables_.technoBpm;
        ctx.baseVolume = musicVol;
        ctx.intensity = combatIntensity_;
        ctx.state = musicState;
        ctx.biome = musicBiome;
        ctx.overpulseActive = isOverpulse;
        ctx.duress = duress;
        ctx.bossEscalation = bossEsc;
        audio.setMusicContext(ctx);
        // Mix / accessibility options (S2): drive the audio mix from settings each frame.
        if (settingsActive_)
            audio.setMixOptions(settings_.musicDuckDepth, settings_.combatReadability ? 1.35f : 1.0f,
                                settings_.reducedIntensityAudio, settings_.monoAudio);
        // Boss escalation stingers: fire once per upward crossing (latch + shared cooldown), mirroring
        // the overpulse latch pattern.
        const bool inBoss = tunables_.technoEnabled && phase_ == RunPhase::Boss;
        const bool enrageNow = inBoss && bossEsc >= 0.80f;
        const bool phaseNow  = inBoss && bossEsc >= 0.50f;
        if (enrageNow && !bossEnrageLatched_ && musicBossEscCooldown_ <= 0.0f) {
            audio.playMusicStinger(MusicStingerType::BossEnrage, 0.85f, true);
            musicBossEscCooldown_ = 8.0f;
        } else if (phaseNow && !enrageNow && !bossPhaseLatched_ && musicBossEscCooldown_ <= 0.0f) {
            audio.playMusicStinger(MusicStingerType::BossPhase, 0.80f, true);
            musicBossEscCooldown_ = 8.0f;
        }
        bossPhaseLatched_ = phaseNow;
        bossEnrageLatched_ = enrageNow;
    } else {
        // v3 (legacy) path: byte-identical to before this feature existed.
        audio.setMusicContext(tunables_.technoEnabled, tunables_.technoBpm,
                              musicVol, combatIntensity_, musicState, musicBiome, isOverpulse);
    }
    if (settingsActive_) audio.setSfxGain(masterMul * settings_.sfxVolume);
    if (tunables_.technoEnabled && pendingBossIntroStinger_) {
        audio.playMusicStinger(MusicStingerType::BossIntro, 0.85f, true);
        pendingBossIntroStinger_ = false;
    }
    if (tunables_.technoEnabled && isOverpulse && !musicOverpulseLatched_ && musicOverpulseCooldown_ <= 0.0f) {
        audio.playMusicStinger(MusicStingerType::Overpulse, 0.82f, true);
        musicOverpulseCooldown_ = 10.0f;
    }
    musicOverpulseLatched_ = isOverpulse;

    // v4 (S3): append a per-frame music-context row for the playtest trace analyzer (--music-trace).
    if (!musicTracePath_.empty()) {
        std::ofstream tf(musicTracePath_, musicTraceInit_ ? std::ios::app : std::ios::trunc);
        if (tf.good()) {
            if (!musicTraceInit_) {
                tf << "time_s,bpm,phase,music_state,biome,intensity,duress,boss_escalation,overpulse,pulse_tier\n";
                musicTraceInit_ = true;
            }
            static const char* kStateNames[] = { "silent", "hub", "combat", "reward", "boss", "runover" };
            static const char* kBiomeNames[] = { "foundry", "furnace", "reliquary" };
            const int si = std::clamp(static_cast<int>(musicState), 0, 5);
            const int bi = std::clamp(static_cast<int>(musicBiome), 0, 2);
            tf << musicTraceTime_ << ',' << tunables_.technoBpm << ',' << phaseName() << ','
               << kStateNames[si] << ',' << kBiomeNames[bi] << ',' << combatIntensity_ << ','
               << duress << ',' << bossEsc << ',' << (isOverpulse ? 1 : 0) << ','
               << static_cast<int>(pulse_.tier()) << '\n';
        }
        musicTraceTime_ += dt;
    }
    // Per-biome ambient bed (spec biome.audio): steady atmosphere while in an arena; faded out in
    // the hub / run-over / menus. Biome index maps Rocky/Forest/Ruins -> Foundry/Furnace/Reliquary.
    {
        const bool inArena = wasteland_.ready() &&
            (phase_ == RunPhase::InRoom || phase_ == RunPhase::Boss ||
             phase_ == RunPhase::DoorsOpen || phase_ == RunPhase::RoomCleared);
        const float sfxMul = settingsActive_ ? settings_.sfxVolume : 1.0f;
        audio.setAmbientBed(static_cast<int>(currentBiome_), inArena ? 0.22f * masterMul * sfxMul : 0.0f);
    }

    // Listener pose for spatialized SFX (enemy/world cues). Reuse the gameplay
    // forward/right so audio panning matches movement and aim exactly.
    const Vec2 listenerFwd = fromAngle(player_.yaw);
    const Vec2 listenerRight = rightFromForward(listenerFwd);
    audio.setListener(player_.pos.x, player_.pos.y,
                      listenerFwd.x, listenerFwd.y, listenerRight.x, listenerRight.y);
}

void PulseGame::updateCombat(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH) {
    // Hit-stop: while the window is open, gameplay time is scaled down for a
    // brief crunch on kills, but feedback timers, shake and audio run on real
    // time (handled in updateTimers) so the freeze reads as a pop, not a stall.
    const float gameplayDt = hitStopTimer_ > 0.0f
        ? dt * clamp(tunables_.feelHitstopScale, 0.0f, 1.0f)
        : dt;

    updatePlayer(input, audio, gameplayDt);
    updateWeapon(input, audio, gameplayDt, screenW, screenH);
    updateEnemies(audio, gameplayDt);
    updateStatuses(audio, gameplayDt);   // M2: tick elemental status (burn DoT, shock chain, decay)
    updateProjectiles(audio, gameplayDt);
    updateBeams(gameplayDt);
    updateSpawning(gameplayDt);
    updatePickups(audio, gameplayDt);
    updateDebris(gameplayDt);
    updateAmbientVfx(gameplayDt);   // per-biome atmosphere: embers / motes / shimmer / spark
    updateParticles(gameplayDt);
    // Ease each doorway's slide toward its sealed/open state (combat doors slide apart when the room
    // clears; the entrance stays shut). Real dt so it animates during the DoorsOpen phase too; purely
    // visual (collision is the wasteland grid, toggled instantly by setDoorOpen).
    if (dt > 0.0f) {
        const int dc = wasteland_.doorCount();
        if (static_cast<int>(doorAnim_.size()) != dc) doorAnim_.assign(static_cast<size_t>(std::max(0, dc)), 0.0f);
        const float step = dt / 0.5f;   // ~0.5 s for a full open/close
        for (int i = 0; i < dc; ++i) {
            const float tgt = wasteland_.doors()[static_cast<size_t>(i)].open ? 1.0f : 0.0f;
            float& a = doorAnim_[static_cast<size_t>(i)];
            a = (a < tgt) ? std::min(tgt, a + step) : std::max(tgt, a - step);
        }
    }
    // A FEW faint drifting motes for depth - kept very sparse + dim (was a constant 2/frame
    // stream that read as visual noise). ~8% of frames spawn a single low-emissive mote.
    if (gameplayDt > 0.0f && rng_.range(0.0f, 1.0f) < 0.08f) {
        const Vec3f mp{ rng_.range(2.0f, 30.0f), rng_.range(0.4f, 3.6f), rng_.range(2.0f, 22.0f) };
        const bool cyan = rng_.range(0.0f, 1.0f) < 0.5f;
        const Vec3f col = cyan ? Vec3f{ 0.30f, 1.5f, 1.8f } : Vec3f{ 1.7f, 0.25f, 1.5f };
        spawnParticles(mp, { 0.0f, 0.04f, 0.0f }, 1, 0.13f, 3.0f, 0.05f, col, 0.45f, 0.05f, 0.6f, 0.0f);
    }
    updateTimers(dt);
    removeDeadEnemies();
    // Age + retire death-corpses (visual only; they play the death clip then dissolve). Flyer
    // corpses (fall=true) have no death clip, so integrate gravity: they tumble + drop to the floor.
    if (gameplayDt > 0.0f) {
        for (EnemyCorpse& c : corpses_) {
            c.age += gameplayDt;
            if (c.fall && c.fallY > 0.0f) {
                c.vy -= 11.0f * gameplayDt;                 // gravity
                c.fallY += c.vy * gameplayDt;
                c.spin += 5.0f * gameplayDt;                // tumble while airborne
                if (c.fallY < 0.0f) { c.fallY = 0.0f; c.vy = 0.0f; }   // landed
            }
        }
        corpses_.erase(std::remove_if(corpses_.begin(), corpses_.end(),
                          [](const EnemyCorpse& c) { return c.age >= c.dur; }),
                       corpses_.end());
    }
}

void PulseGame::updatePlayer(const InputState& input, AudioSystem& audio, float dt) {
    const float invertY = (settingsActive_ && settings_.invertY) ? -1.0f : 1.0f;
    player_.yaw = wrapAngle(player_.yaw + static_cast<float>(input.mouseDeltaX) * tunables_.mouseSensitivity);
    player_.pitch -= invertY * static_cast<float>(input.mouseDeltaY) * tunables_.mouseSensitivity;
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
    dashInvulnTimer_ = std::max(0.0f, dashInvulnTimer_ - dt);
    const bool groundedBeforeVertical = player_.grounded;

    // Build mods perturb movement feel (the feel gate covers a fully-stacked run).
    const BuildStats& bs = build_.stats();
    if (input.pressed(VK_SHIFT) && player_.dashCooldown <= 0.0f && player_.dashTime <= 0.0f) {
        player_.dashDir = lengthSq(desiredDir) > 0.001f ? desiredDir : forward;
        player_.dashTime = std::max(0.01f, tunables_.dashDuration);
        player_.dashCooldown = std::max(0.0f, tunables_.dashCooldown * bs.dashCooldownMult);
        // A dash started in the AIR covers more ground (air-mobility burst). Baked at start so it
        // stays consistent even if you land partway through the dash.
        player_.dashSpeed = tunables_.dashImpulse * (player_.grounded ? 1.0f : std::max(1.0f, tunables_.airDashMult));
        // I-frames: a dash dodges all incoming damage for a window that runs a touch
        // past the movement so the escape feels generous.
        dashInvulnTimer_ = std::max(player_.dashTime, tunables_.dashInvulnSeconds);
        player_.vel = player_.dashDir * player_.dashSpeed;
        fireFovKick_ = std::max(fireFovKick_, tunables_.dashFovPunch);
        playFeedback(audio, FeedbackEventType::Dash, 0.95f);
        // Pulse: dashing INTO the fray is momentum (the Returnal/Doom dance, mechanized).
        {
            int nearby = 0;
            for (const Enemy& e : enemies_)
                if (e.active && lengthSq(e.pos - player_.pos) < 2.6f * 2.6f) ++nearby;
            pulse_.onDashThrough(nearby);
        }
        // Dash-mod (Kinetic Strike etc.): a dash deals an AoE strike along its path,
        // turning the core movement verb into an offensive option (no button).
        if (bs.dashDamage > 0.0f) {
            const Vec2 strikeAt = player_.pos + player_.dashDir * 1.0f;   // along the dash
            applyAreaDamage(audio, strikeAt, 1.8f, bs.dashDamage, -1);
            spawnParticles({ strikeAt.x, player_.height, strikeAt.y },
                           { player_.dashDir.x * 4.0f, 0.5f, player_.dashDir.y * 4.0f },
                           12, 2.6f, 0.25f, 0.05f, { 0.55f, 0.9f, 1.35f }, 5.0f);
            addShake(0.4f);
        }
    }

    if (player_.dashTime > 0.0f) {
        player_.dashTime -= dt;
        player_.vel = player_.dashDir * player_.dashSpeed;
    } else {
        const Vec2 desiredVel = desiredDir * (tunables_.walkSpeed * bs.moveSpeedMult * pulse_.moveMult());
        if (lengthSq(desiredDir) > 0.001f) {
            const float airScale = player_.grounded ? 1.0f : clamp(tunables_.airControl, 0.0f, 1.0f);
            player_.vel = approach(player_.vel, desiredVel, tunables_.acceleration * airScale * dt);
        } else {
            player_.vel = approach(player_.vel, Vec2{}, tunables_.braking * dt);
        }
    }

    // Horizontal move: cover taller than the player's feet blocks; lower cover is passed
    // over / stood on (so a high jump clears low rocks). feet = eye height minus the base
    // eye-above-feet offset.
    const float eyeBase = clamp(tunables_.eyeHeight, 0.12f, 0.88f);
    const float feetNow = player_.height - eyeBase;
    moveWithCollision(player_.pos, player_.vel, tunables_.playerRadius, dt, feetNow);

    // Jump + gravity on the vertical axis. The player rests on a SUPPORT surface (the floor
    // at 0, or the top of low cover the footprint is over), jumps off it, and lands back on
    // whatever surface is under them - so you can hop onto rocks and stand on them.
    //
    // Proper jump ability:
    //  - DOUBLE JUMP: a second (and Nth) jump in the air, each a clean RESET of vertical speed.
    //  - JUMP BUFFER: a press just before you can jump is remembered and fires the instant it can.
    //  - COYOTE TIME: a ground jump still works for a moment after walking off an edge.
    // All three make the jump feel responsive + capable for arena dodging instead of a single
    // frame-perfect hop. Tunable via movement.* in config/pulse.tuning.
    const bool jumpHeld = restartTimer_ <= 0.0f && input.pressed(VK_SPACE);
    player_.jumpBuffer = jumpHeld ? std::max(0.0f, tunables_.jumpBufferTime)
                                  : std::max(0.0f, player_.jumpBuffer - dt);
    const float feet = player_.height - eyeBase;
    const float support = groundHeightAt(player_.pos, tunables_.playerRadius, feet);
    const auto launch = [&](float vel, float vol) {
        player_.vz = std::max(0.0f, vel);
        player_.grounded = false;
        player_.jumpBuffer = 0.0f;
        player_.coyote = 0.0f;
        // Remember the launch surface so the indoor apex cap is RELATIVE to it (a jump off a +2 m
        // deck must clamp above the deck, not at a floor-relative height that drops you through it).
        player_.jumpFromH = std::max(support, player_.height - eyeBase);
        playFeedback(audio, FeedbackEventType::Jump, vol);
    };
    if (player_.grounded) {
        constexpr float kGroundStick = 0.18f; // stay planted over ramp cells and tiny lips
        if (support + kGroundStick < feet) {  // walked off a real edge -> start falling
            player_.grounded = false;
            player_.vz = 0.0f;
            player_.coyote = std::max(0.0f, tunables_.coyoteTime);   // brief late-jump grace
        } else {
            player_.height = support + eyeBase;
            player_.airJumps = std::max(0, tunables_.airJumpCount);  // refill air jumps on the ground
            player_.coyote = 0.0f;
            if (player_.jumpBuffer > 0.0f) launch(tunables_.jumpVelocity, 0.5f);  // ground hop
        }
    } else {
        player_.coyote = std::max(0.0f, player_.coyote - dt);
        if (player_.jumpBuffer > 0.0f) {
            if (player_.coyote > 0.0f) {
                launch(tunables_.jumpVelocity, 0.5f);               // late ground jump (coyote)
            } else if (player_.airJumps > 0) {
                player_.airJumps--;
                launch(tunables_.airJumpVelocity, 0.62f);           // DOUBLE JUMP
                spawnAirJumpBurst();                                // a kick of dust/energy at the feet
            }
        }
        // Weighty, controllable arc: fall faster than you rise, and if the jump key is released
        // while still rising, pull down harder so a tap is a short hop (variable jump height).
        float g = std::max(0.0f, tunables_.gravity);
        if (player_.vz < 0.0f) g *= std::max(1.0f, tunables_.fallGravityMult);
        else if (!input.down(VK_SPACE)) g *= std::max(1.0f, tunables_.lowJumpMult);
        player_.vz -= g * dt;
        player_.height += player_.vz * dt;
        // Outdoors the eye may rise well above 1.0 (standing on cover); indoors keep it under
        // the ceiling. The apex cap is RELATIVE to the surface the jump launched from, so a hop off a
        // raised deck clamps above the deck (not at a floor-relative height that would drop the player
        // through it), and is also held below the room ceiling.
        // jumpApexCap is the apex eye height above the launch surface's feet (0.9 reproduces the old
        // floor-takeoff absolute cap, since standing eye = feet + eyeBase and the old cap was 0.9).
        const float relCap = player_.jumpFromH + std::min(0.92f, tunables_.jumpApexCap);
        const float ceilCap = wasteland_.ready() ? wasteland_.ceiling() - 0.45f : 0.92f;
        const float cap = procEnvOutdoor() ? 6.0f : std::min(relCap, ceilCap);
        if (player_.height > cap) {
            player_.height = cap;
            player_.vz = std::min(player_.vz, 0.0f);
        }
        const float feet2 = player_.height - eyeBase;
        const float floorEye = groundHeightAt(player_.pos, tunables_.playerRadius, feet2) + eyeBase;
        if (player_.vz <= 0.0f && player_.height <= floorEye) { // landed on the surface below
            player_.height = floorEye;
            player_.vz = 0.0f;
            player_.grounded = true;
            if (!groundedBeforeVertical) {
                landingKick_ = std::max(landingKick_, degToRad(std::max(0.0f, tunables_.cameraLandingKickDegrees)));
                // Plant the landing: shed any carried air speed above walk so you do not slide.
                // This covers an air dash whether or not its duration is still running - a short
                // dash can expire while still airborne, and with low air control that dash speed
                // would otherwise survive the fall and slide along the ground on touchdown. Ending
                // it here lets ground movement/braking take over from walk speed immediately, so
                // the air dash repositions cleanly instead of skating past where you aimed.
                player_.dashTime = 0.0f;
                const float walk = std::max(0.1f, tunables_.walkSpeed);
                const float sp = std::sqrt(player_.vel.x * player_.vel.x + player_.vel.y * player_.vel.y);
                if (sp > walk) player_.vel = player_.vel * (walk / sp);
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
    const WeaponProfile& profile = activeWeaponProfile();
    // Sidearm with infinite reserve: keep the reserve topped up so a reload always has
    // ammo and the count never depletes, while the magazine still empties and reloads.
    if (profile.infiniteReserve) weapon_.reserve = weaponReserveMax();
    const bool wantsFire = input.mouseDown[0];
    const bool firePressed = input.mousePressed[0];

    if (weapon_.reloading) {
        if (profile.reloadMode == WeaponReloadMode::PerShell) {
            weapon_.reloadRemaining = std::max(0.0f, weapon_.reloadRemaining - dt);
            weapon_.shellReloadTimer -= dt;
            if ((wantsFire || firePressed) && weapon_.ammo > 0) {
                weapon_.reloading = false;
                weapon_.reloadEndPlayed = true;
                audio.playWeaponEvent(profile.id, WeaponEventType::ReloadEnd, 0.80f, fireSoundIndex_++);
            } else {
                while (weapon_.shellReloadTimer <= 0.0f && weapon_.ammo < weaponMagazine() && weapon_.reserve > 0) {
                    ++weapon_.ammo;
                    --weapon_.reserve;
                    weapon_.shellReloadTimer += std::max(0.05f, profile.perShellSeconds / std::max(0.2f, build_.stats().reloadSpeedMult));
                    weaponKick_ = std::max(weaponKick_, 0.46f);
                    weaponKickSide_ = -0.10f;
                    audio.playWeaponEvent(profile.id, WeaponEventType::Shell, 0.86f, fireSoundIndex_++);
                }
                if (weapon_.ammo >= weaponMagazine() || weapon_.reserve <= 0 || weapon_.reloadRemaining <= 0.0f) {
                    weapon_.reloading = false;
                    weapon_.reloadEndPlayed = true;
                    audio.playWeaponEvent(profile.id, WeaponEventType::ReloadEnd, 0.92f, fireSoundIndex_++);
                }
            }
        } else {
            weapon_.reloadRemaining -= dt;
            const float reloadDur = reloadDuration();
            const float reloadProgress = 1.0f - clamp(weapon_.reloadRemaining / reloadDur, 0.0f, 1.0f);
            if (!weapon_.reloadMagOutPlayed && reloadProgress >= 0.28f) {
                weapon_.reloadMagOutPlayed = true;
                weaponKick_ = std::max(weaponKick_, 0.44f);
                weaponKickSide_ = -0.16f;
                audio.playWeaponEvent(profile.id, WeaponEventType::MagOut, 0.84f, fireSoundIndex_++);
            }
            if (!weapon_.reloadInsertPlayed && reloadProgress >= 0.62f) {
                weapon_.reloadInsertPlayed = true;
                weaponKick_ = std::max(weaponKick_, 0.72f);
                weaponKickSide_ = 0.20f;
                audio.playWeaponEvent(profile.id, WeaponEventType::MagIn, 0.98f, fireSoundIndex_++);
            }
            if (!weapon_.reloadEndPlayed && reloadProgress >= 0.90f) {
                weapon_.reloadEndPlayed = true;
                audio.playWeaponEvent(profile.id, WeaponEventType::ReloadEnd, 0.82f, fireSoundIndex_++);
            }
            if (weapon_.reloadRemaining <= 0.0f) {
                weapon_.reloading = false;
                const int missing = std::max(0, weaponMagazine() - weapon_.ammo);
                const int loaded = std::min(missing, weapon_.reserve);
                weapon_.ammo += loaded;
                weapon_.reserve -= loaded;
                if (!weapon_.reloadInsertPlayed && loaded > 0) {
                    weapon_.reloadInsertPlayed = true;
                    audio.playWeaponEvent(profile.id, WeaponEventType::MagIn, 0.98f, fireSoundIndex_++);
                }
                if (!weapon_.reloadEndPlayed) {
                    weapon_.reloadEndPlayed = true;
                    audio.playWeaponEvent(profile.id, WeaponEventType::ReloadEnd, 0.98f, fireSoundIndex_++);
                }
            }
        }
    }

    if (!weapon_.reloading && weapon_.queuedBurstShots > 0) {
        weapon_.queuedBurstTimer -= dt;
        while (weapon_.queuedBurstShots > 0 && weapon_.queuedBurstTimer <= 0.0f) {
            if (!fireProfileShot(audio, profile, screenW, screenH, true)) {
                weapon_.queuedBurstShots = 0;
                break;
            }
            --weapon_.queuedBurstShots;
            weapon_.queuedBurstTimer += std::max(0.01f, profile.burstInterval);
        }
    }

    // Auto vs semi is now per-weapon (the pistol is semi-auto); the global tunable
    // stays a master switch. Semi-auto fires on the press edge only.
    const bool autoFire = profile.automatic && tunables_.weaponAutoFire;
    if (weapon_.queuedBurstShots <= 0 && ((autoFire && wantsFire) || (!autoFire && firePressed))) {
        tryFire(audio, screenW, screenH);
    }

    if (input.pressed('R') && !weapon_.reloading && weapon_.ammo < weaponMagazine() && weapon_.reserve > 0) {
        weapon_.reloading = true;
        weapon_.reloadMagOutPlayed = false;
        weapon_.reloadInsertPlayed = false;
        weapon_.reloadEndPlayed = false;
        weapon_.reloadRemaining = reloadDuration();
        weapon_.shellReloadTimer = profile.reloadMode == WeaponReloadMode::PerShell
            ? std::max(0.05f, profile.perShellSeconds / std::max(0.2f, build_.stats().reloadSpeedMult))
            : 0.0f;
        weapon_.queuedBurstShots = 0;
        weaponKick_ = std::max(weaponKick_, 0.40f);
        weaponKickSide_ = -0.14f;
        audio.playWeaponEvent(profile.id, WeaponEventType::ReloadStart, 0.92f, fireSoundIndex_++);
    }

    // Cycle weapons in the loadout (Phase B.2). Q swaps; a held swap doesn't fire.
    if (input.pressed('Q')) {
        swapWeapon(audio);
    }
    // V quick-draws between the pistol sidearm and your main weapon (if you carry one).
    if (input.pressed('V')) {
        quickSwapPistol(audio);
    }
    // M3: X cycles the active weapon's ASPECT (forms unlock as you pour power into the weapon).
    if (input.pressed('X')) {
        cycleAspect(audio);
    }

    // Abilities (Phase B.3): G throws the tactical grenade, F triggers the ultimate.
    if (input.pressed('G')) throwGrenade(audio);
    if (input.pressed('F')) activateUltimate(audio);
}

const WeaponDef& PulseGame::activeWeaponDef() const {
    if (activeWeapon_ >= 0 && activeWeapon_ < static_cast<int>(loadout_.size())) {
        if (const WeaponDef* d = build_.findWeapon(loadout_[static_cast<size_t>(activeWeapon_)].id)) return *d;
    }
    if (const WeaponDef* c = build_.findWeapon("pistol")) return *c;
    return build_.weaponCatalog().front();
}

const WeaponProfile& PulseGame::weaponProfileForId(const std::string& id) const {
    if (const WeaponProfile* p = weaponProfiles_.find(id)) return *p;
    if (const WeaponProfile* p = weaponProfiles_.find("pistol")) return *p;
    return weaponProfiles_.profiles().front();
}

const WeaponProfile& PulseGame::activeWeaponProfile() const {
    if (activeWeapon_ >= 0 && activeWeapon_ < static_cast<int>(loadout_.size())) {
        return weaponProfileForId(loadout_[static_cast<size_t>(activeWeapon_)].id);
    }
    return weaponProfileForId("pistol");
}

int PulseGame::activeWeaponPower() const {
    if (activeWeapon_ >= 0 && activeWeapon_ < static_cast<int>(loadout_.size()))
        return loadout_[static_cast<size_t>(activeWeapon_)].power;
    return 1;
}

// M3 aspects: how many FORMS the active weapon's power has unlocked (base + 1 per power level,
// capped by how many aspects the weapon defines). 1 = base form only.
int PulseGame::aspectsUnlocked() const {
    if (activeWeapon_ < 0 || activeWeapon_ >= static_cast<int>(loadout_.size())) return 1;
    const WeaponDef& wd = activeWeaponDef();
    const int forms = 1 + static_cast<int>(wd.aspects.size());                 // base + defined forms
    const int byPower = loadout_[static_cast<size_t>(activeWeapon_)].power;     // power 1 -> base, +1 form/level
    return std::max(1, std::min(forms, byPower));
}

// The active slot's current form (nullptr = base / aspect 0). Clamped to what power unlocked.
const WeaponAspect* PulseGame::activeAspect() const {
    if (activeWeapon_ < 0 || activeWeapon_ >= static_cast<int>(loadout_.size())) return nullptr;
    const WeaponSlot& slot = loadout_[static_cast<size_t>(activeWeapon_)];
    const WeaponDef& wd = activeWeaponDef();
    const int idx = std::min(slot.aspect, aspectsUnlocked() - 1);   // 0 = base
    if (idx <= 0 || idx - 1 >= static_cast<int>(wd.aspects.size())) return nullptr;
    return &wd.aspects[static_cast<size_t>(idx - 1)];
}

uint32_t PulseGame::activeShotEffectMask() const {
    const BuildStats& bs = build_.stats();
    uint32_t mask = 0;
    if (bs.igniteOnHit > 0.0f)  mask |= ShotFxBurn;
    if (bs.shockOnHit > 0.0f)   mask |= ShotFxShock;
    if (bs.chillOnHit > 0.0f)   mask |= ShotFxCryo;
    if (bs.corrodeOnHit > 0.0f) mask |= ShotFxCorrode;
    if (bs.lifeLeechPct > 0.0f) mask |= ShotFxLeech;
    if (const WeaponAspect* asp = activeAspect()) {
        switch (static_cast<Element>(asp->element)) {
            case Element::Burn:    mask |= ShotFxBurn; break;
            case Element::Shock:   mask |= ShotFxShock; break;
            case Element::Cryo:    mask |= ShotFxCryo; break;
            case Element::Corrode: mask |= ShotFxCorrode; break;
            default: break;
        }
    }
    return mask;
}

Vec3f PulseGame::shotEffectTint(uint32_t mask) const {
    if (mask == 0) return { 0.45f, 0.95f, 1.45f };
    Vec3f sum{};
    float n = 0.0f;
    const auto add = [&](Vec3f c, float w) {
        sum = { sum.x + c.x * w, sum.y + c.y * w, sum.z + c.z * w };
        n += w;
    };
    if (mask & ShotFxBurn)    add(elementTint(Element::Burn), 1.0f);
    if (mask & ShotFxShock)   add(elementTint(Element::Shock), 1.0f);
    if (mask & ShotFxCryo)    add(elementTint(Element::Cryo), 1.0f);
    if (mask & ShotFxCorrode) add(elementTint(Element::Corrode), 1.0f);
    if (mask & ShotFxLeech)   add({ 0.42f, 1.85f, 0.82f }, 0.9f);
    const float inv = n > 0.0f ? 1.0f / n : 1.0f;
    Vec3f c{ sum.x * inv, sum.y * inv, sum.z * inv };
    return { std::min(c.x + 0.20f, 2.35f), std::min(c.y + 0.20f, 2.35f), std::min(c.z + 0.20f, 2.35f) };
}

void PulseGame::cycleAspect(AudioSystem& audio) {
    if (activeWeapon_ < 0 || activeWeapon_ >= static_cast<int>(loadout_.size())) return;
    const int forms = aspectsUnlocked();
    if (forms <= 1) { playFeedback(audio, FeedbackEventType::UiCancel, 0.7f); return; }   // nothing unlocked yet
    WeaponSlot& slot = loadout_[static_cast<size_t>(activeWeapon_)];
    slot.aspect = (slot.aspect + 1) % forms;
    recoilShotIndex_ = 0;
    playFeedback(audio, FeedbackEventType::PickupPowerup, 0.9f);
}

float PulseGame::weaponBaseDamage() const {
    const WeaponProfile& p = activeWeaponProfile();
    const float base = p.damage > 0.0f ? p.damage : tunables_.weaponDamage;
    const float powerScale = 1.0f + 0.25f * static_cast<float>(activeWeaponPower() - 1); // +25%/level
    return base * powerScale * aspectDamageMult() * overdriveDamageMult(); // M3 aspect (Pulse is NOT a damage mult)
}

float PulseGame::weaponBaseFireRate() const {
    const WeaponProfile& p = activeWeaponProfile();
    const float base = p.fireRate > 0.0f ? p.fireRate : tunables_.weaponFireRate;
    return base * aspectFireRateMult();   // M3 aspect cadence
}

int PulseGame::weaponMagazine() const {
    const WeaponProfile& p = activeWeaponProfile();
    return p.magazine > 0 ? p.magazine : tunables_.weaponMagazineCapacity;
}

int PulseGame::weaponReserveMax() const {
    const WeaponProfile& p = activeWeaponProfile();
    return p.reserve > 0 ? p.reserve : tunables_.weaponReserveAmmo;
}

float PulseGame::weaponBaseReload() const {
    const WeaponProfile& p = activeWeaponProfile();
    const float base = p.reloadSeconds > 0.0f ? p.reloadSeconds : tunables_.weaponReloadDuration;
    return base * aspectReloadMult();   // M3 aspect (some forms reload slower for more power)
}

// Acquire a weapon reward: a new weapon joins the loadout (cap 5); a dup raises the
// owned weapon's power level (more damage + later effect unlocks).
void PulseGame::acquireWeapon(const std::string& id, AudioSystem& audio) {
    if (id != "pistol" && !weaponProfiles_.rewardEligible(id)) {
        playFeedback(audio, FeedbackEventType::UiCancel, 0.6f);
        return;
    }
    for (WeaponSlot& s : loadout_) {
        if (s.id == id) { ++s.power; playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f); return; }
    }
    if (loadout_.size() >= 5) {
        // Loadout full: upgrade the active weapon instead of dropping the reward.
        if (!loadout_.empty()) ++loadout_[static_cast<size_t>(activeWeapon_)].power;
        playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f);
        return;
    }
    WeaponSlot slot;
    slot.id = id;
    slot.power = 1;
    const WeaponProfile& profile = weaponProfileForId(id);
    const int mag = profile.magazine > 0 ? profile.magazine : tunables_.weaponMagazineCapacity;
    const int reserve = profile.reserve > 0 ? profile.reserve : mag * 3;
    slot.savedAmmo = Weapon{};
    slot.savedAmmo.ammo = std::max(1, mag);
    slot.savedAmmo.reserve = std::max(0, reserve);
    loadout_.push_back(slot);
    // Auto-switch to the freshly acquired weapon so the pickup reads immediately.
    loadout_[static_cast<size_t>(activeWeapon_)].savedAmmo = weapon_;
    activeWeapon_ = static_cast<int>(loadout_.size()) - 1;
    weapon_ = loadout_[static_cast<size_t>(activeWeapon_)].savedAmmo;
    playFeedback(audio, FeedbackEventType::PickupPowerup, 1.0f);
}

// Switch the active weapon to a specific loadout slot, parking/restoring per-weapon
// ammo state with the shared swap feel (settle, kick, equip cue).
void PulseGame::performSwapTo(int target, AudioSystem& audio) {
    if (target < 0 || target >= static_cast<int>(loadout_.size()) || target == activeWeapon_) return;
    loadout_[static_cast<size_t>(activeWeapon_)].savedAmmo = weapon_;
    activeWeapon_ = target;
    weapon_ = loadout_[static_cast<size_t>(activeWeapon_)].savedAmmo;
    weapon_.timeSinceShot = std::max(weapon_.timeSinceShot, 0.15f); // brief swap settle
    weaponKick_ = std::max(weaponKick_, 0.5f);
    recoilShotIndex_ = 0;
    audio.playWeaponEvent(activeWeaponProfile().id, WeaponEventType::Equip, 0.72f, fireSoundIndex_++);
}

// Cycle to the next weapon in the loadout (Q): reaches every weapon you carry.
void PulseGame::swapWeapon(AudioSystem& audio) {
    if (loadout_.size() <= 1 || weapon_.reloading) return;
    performSwapTo((activeWeapon_ + 1) % static_cast<int>(loadout_.size()), audio);
}

// Quick-draw toggle (V): holster to the pistol sidearm and back to your main weapon
// at will. Remembers the last non-pistol slot so the toggle always returns to it,
// regardless of how many weapons are in the loadout. No-op if the pistol is all you have.
void PulseGame::quickSwapPistol(AudioSystem& audio) {
    if (loadout_.size() <= 1 || weapon_.reloading) return;
    int pistolSlot = -1;
    for (int i = 0; i < static_cast<int>(loadout_.size()); ++i)
        if (loadout_[static_cast<size_t>(i)].id == "pistol") { pistolSlot = i; break; }

    int target = -1;
    if (pistolSlot >= 0 && activeWeapon_ != pistolSlot) {
        lastPrimaryWeapon_ = activeWeapon_;   // remember the main weapon to return to
        target = pistolSlot;
    } else {
        // On the pistol (or no pistol present): draw the remembered main, else the first non-pistol.
        if (lastPrimaryWeapon_ >= 0 && lastPrimaryWeapon_ < static_cast<int>(loadout_.size())
            && lastPrimaryWeapon_ != pistolSlot)
            target = lastPrimaryWeapon_;
        if (target < 0)
            for (int i = 0; i < static_cast<int>(loadout_.size()); ++i)
                if (i != pistolSlot) { target = i; break; }
    }
    performSwapTo(target, audio);
}

// Aggression economy: kills (and, a little, damage) fill the ability charges, so
// forward pressure buys agency and turtling starves it. Charges cap at full.
void PulseGame::addAbilityCharge(AudioSystem& audio, float tactical, float ultimate) {
    const bool tWasReady = tacticalCharge_ >= 1.0f;
    const bool uWasReady = ultimateCharge_ >= 1.0f;
    // Pulse charges abilities faster when hot - aggression buys agency (the greed spine).
    // M6 Mirror Adrenaline adds a small permanent charge-rate bump on top.
    const float chargeMul = pulse_.chargeMult() * meta_.mirrorChargeRate();
    tacticalCharge_ = std::min(1.0f, tacticalCharge_ + tactical * chargeMul);
    ultimateCharge_ = std::min(1.0f, ultimateCharge_ + ultimate * chargeMul);
    // Edge-trigger a "ready" chime the moment a charge first fills, so an ability coming
    // online is audible without watching the HUD. Ultimate takes priority if both fill.
    if (!uWasReady && ultimateCharge_ >= 1.0f) playFeedback(audio, FeedbackEventType::ChargeReady, 0.95f);
    else if (!tWasReady && tacticalCharge_ >= 1.0f) playFeedback(audio, FeedbackEventType::ChargeReady, 0.8f);
}

// Tactical: throw an arcing AoE bolt that detonates on contact or at the end of its
// short flight. A space-shaper that turns a single-target weapon into a crowd answer.
void PulseGame::throwGrenade(AudioSystem& audio) {
    if (tacticalCharge_ < 1.0f) return;
    tacticalCharge_ = 0.0f;
    const float yaw = player_.yaw + recoilOffsetYaw_;
    const float pitch = clamp(player_.pitch, -0.4f, 0.4f) + 0.12f; // slight upward toss
    const Vec2 dir = fromAngle(yaw);
    const Vec2 muzzle = player_.pos + dir * 0.5f;
    Projectile p;
    p.hostile = false;
    p.origin = muzzle;
    p.pos = muzzle + dir * 0.4f;
    p.vel = dir * 13.0f;
    p.height = clamp(player_.height - 0.05f + std::tan(pitch) * 0.5f, 0.15f, 0.9f);
    p.life = 0.85f;                  // detonates at the end of flight if it hits nothing
    p.damage = std::max(1, static_cast<int>(std::lround(70.0f)));
    p.splashRadius = 3.0f;           // a wide blast (build damageMult applies on detonation)
    p.color = { 1.6f, 0.7f, 0.25f }; // warm AoE bolt: drives its trail + light
    projectiles_.push_back(p);
    weaponKick_ = std::max(weaponKick_, 0.6f);
    playFeedback(audio, FeedbackEventType::AbilityTactical, 0.95f);
}

// Ultimate: Overdrive - an execution-window "moment" (damage + fire-rate amp) rather
// than a fire-and-forget nuke, so it rewards timing into a dense fight.
void PulseGame::activateUltimate(AudioSystem& audio) {
    if (ultimateCharge_ < 1.0f) return;
    ultimateCharge_ = 0.0f;
    overdriveTimer_ = 5.0f;
    pulse_.bump(0.5f);   // the ultimate slams you into the groove
    fireFovKick_ = std::max(fireFovKick_, tunables_.dashFovPunch);
    addShake(tunables_.cameraShakeKill);
    playFeedback(audio, FeedbackEventType::AbilityUltimate, 1.1f);
}

void PulseGame::spawnPlayerProjectile(float shotYaw, float shotPitch, float damage, float speed, float splashRadius) {
    const Vec2 dir = fromAngle(shotYaw);
    const Vec2 shotRight = rightFromForward(dir);
    const Vec2 muzzle = player_.pos + dir * 0.44f + shotRight * 0.18f;
    Projectile p;
    p.hostile = false;
    p.origin = muzzle;
    p.pos = muzzle + dir * 0.45f;
    p.vel = dir * std::max(4.0f, speed);
    p.height = clamp(player_.height - 0.11f + std::tan(shotPitch) * 0.5f, 0.1f, 0.9f);
    p.life = 3.0f;
    p.damage = std::max(1, static_cast<int>(std::lround(damage)));
    p.splashRadius = splashRadius;
    p.effectMask = activeShotEffectMask();
    p.color = shotEffectTint(p.effectMask);   // build/aspect tint drives the trail + light
    projectiles_.push_back(p);
    if (projectiles_.size() > 96) projectiles_.erase(projectiles_.begin());
    // Muzzle: a forward ray-lance of streaked cyan motes + a flash.
    const Vec3f m3{ muzzle.x, p.height, muzzle.y };
    spawnBeam(m3, { m3.x + dir.x * 2.4f, m3.y, m3.z + dir.y * 2.4f }, p.color, 0.05f, 0.3f);
    spawnParticles(m3, { dir.x * 6.0f, 0.3f, dir.y * 6.0f }, 8, 2.0f, 0.16f, 0.045f, p.color, 7.0f, 3.0f, 2.0f, 0.14f);
    if (p.effectMask != 0) {
        if (p.effectMask & ShotFxBurn) {
            const Vec3f c = elementTint(Element::Burn);
            spawnParticles(m3, { dir.x * 4.4f, 1.15f, dir.y * 4.4f }, 6, 1.15f, 0.22f, 0.040f, c, 6.8f, 0.2f, 2.2f, 0.18f);
        }
        if (p.effectMask & ShotFxShock) {
            const Vec3f c = elementTint(Element::Shock);
            spawnBeam({ m3.x + shotRight.x * 0.12f, m3.y + 0.02f, m3.z + shotRight.y * 0.12f },
                      { m3.x + dir.x * 1.85f - shotRight.x * 0.18f, m3.y + 0.08f, m3.z + dir.y * 1.85f - shotRight.y * 0.18f },
                      c, 0.018f, 0.28f);
            spawnParticles(m3, { dir.x * 5.4f, 0.35f, dir.y * 5.4f }, 4, 1.45f, 0.16f, 0.026f, c, 8.0f, 0.0f, 4.0f, 0.22f);
        }
        if (p.effectMask & ShotFxCryo) {
            const Vec3f c = elementTint(Element::Cryo);
            spawnParticles(m3, { dir.x * 3.7f, -0.10f, dir.y * 3.7f }, 7, 0.78f, 0.30f, 0.030f, c, 5.4f, 1.8f, 3.2f, 0.06f);
        }
        if (p.effectMask & ShotFxCorrode) {
            const Vec3f c = elementTint(Element::Corrode);
            spawnParticles(m3, { dir.x * 3.2f, -0.45f, dir.y * 3.2f }, 6, 0.95f, 0.34f, 0.040f, c, 5.8f, 5.5f, 2.2f, 0.10f);
        }
        if (p.effectMask & ShotFxLeech) {
            const Vec3f c{ 0.36f, 1.95f, 0.82f };
            spawnBeam(m3, { m3.x + dir.x * 1.35f, m3.y - 0.04f, m3.z + dir.y * 1.35f }, c, 0.016f, 0.34f);
        }
    }
}

// Resolve one hitscan ray at (shotYaw, shotPitch) dealing baseDamage (before build
// mults). Shared by every hitscan archetype (AK-47, beam, per-pellet spread,
// per-round burst) so they all inherit the proven impact/decal/kill feedback.
void PulseGame::resolveHitscan(AudioSystem& audio, float shotYaw, float shotPitch, float baseDamage, int screenW, int screenH) {
    shotPitch = clamp(shotPitch, -1.5f, 1.5f);
    bool headshot = false;
    int targetIndex = acquireTarget(shotYaw, shotPitch, screenW, screenH, headshot);

    const Vec2 shotDir = fromAngle(shotYaw);
    const Vec2 shotRight = rightFromForward(shotDir);
    const Vec2 muzzle = player_.pos + shotDir * 0.44f + shotRight * 0.18f;
    const float muzzleHeight = clamp(player_.height - 0.11f, 0.08f, 0.92f);
    const float verticalSlope = std::tan(shotPitch);

    int propIdx = -1;
    const float propDist = propRayHit(muzzle, shotDir, MaxRayDistance, muzzleHeight, verticalSlope, propIdx);
    if (targetIndex >= 0 && targetIndex < static_cast<int>(enemies_.size())) {
        if (propIdx >= 0 && propDist < length(enemies_[static_cast<size_t>(targetIndex)].pos - muzzle))
            targetIndex = -1;   // a prop is in the way
    }

    Tracer tracer;
    tracer.start = muzzle;
    tracer.startHeight = muzzleHeight;
    tracer.hit = targetIndex >= 0;
    tracer.duration = std::max(0.02f, activeWeaponProfile().tracerSeconds);
    tracer.effectMask = activeShotEffectMask();
    tracer.color = shotEffectTint(tracer.effectMask);

    if (targetIndex >= 0 && targetIndex < static_cast<int>(enemies_.size())) {
        const Enemy& enemy = enemies_[static_cast<size_t>(targetIndex)];
        // Tracer ends at the enemy's actual body centre (flyers hover well off the floor), so the
        // visible beam lands where the hit registered instead of at the ground under a drone.
        const float aimY = enemyAimY(enemy) + 0.05f * std::sin(shakeTime_ * 2.6f + enemy.bobPhase);
        tracer.end = enemy.pos;
        tracer.endHeight = clamp(aimY + (headshot ? 0.18f : 0.0f), 0.08f, 3.0f);
    } else {
        RayHit wallHit = castRay(muzzle, shotYaw, MaxRayDistance);
        float impactDistance = wallHit.distance;
        int surface = 0;   // 0 = wall, 1 = ceiling, 2 = floor
        if (verticalSlope > 0.0001f) {
            const float ceilingDistance = (0.98f - muzzleHeight) / verticalSlope;
            if (ceilingDistance > 0.0f && ceilingDistance < impactDistance) {
                impactDistance = ceilingDistance; surface = 1;
            }
        } else if (verticalSlope < -0.0001f) {
            const float floorDistance = (0.02f - muzzleHeight) / verticalSlope;
            if (floorDistance > 0.0f && floorDistance < impactDistance) {
                impactDistance = floorDistance; surface = 2;
            }
        }
        int hitProp = -1;
        if (propIdx >= 0 && propDist < impactDistance) {
            impactDistance = propDist; surface = 0; hitProp = propIdx;
        }
        impactDistance = clamp(impactDistance, 0.25f, MaxRayDistance);
        tracer.end = muzzle + shotDir * impactDistance;
        tracer.endHeight = clamp(muzzleHeight + verticalSlope * impactDistance, 0.02f, 0.98f);

        Vec3f normal{ 0, 1, 0 };
        if (hitProp >= 0) {
            const Vec2 pc{ props_[static_cast<size_t>(hitProp)].pos.x, props_[static_cast<size_t>(hitProp)].pos.z };
            const Vec2 n2 = normalize(Vec2{ tracer.end.x - pc.x, tracer.end.y - pc.y });
            normal = { n2.x, 0.0f, n2.y };
        }
        else if (surface == 1) normal = { 0, -1, 0 };
        else if (surface == 2) normal = { 0, 1, 0 };
        else if (wallHit.side == 0) normal = { shotDir.x > 0.0f ? -1.0f : 1.0f, 0, 0 };
        else normal = { 0, 0, shotDir.y > 0.0f ? -1.0f : 1.0f };
        const Vec3f hitPos = { tracer.end.x + normal.x * 0.02f, tracer.endHeight + normal.y * 0.02f,
                               tracer.end.y + normal.z * 0.02f };
        spawnDecal(hitPos, normal, 0u, 0.06f + rng_.range(0.0f, 0.03f),
                   { 0.028f, 0.026f, 0.024f }, 0.9f);
        const float impact = std::max(0.35f, activeWeaponProfile().impactScale);
        spawnParticles(hitPos, normal * (3.5f * impact), static_cast<int>(std::lround(9.0f * impact)),
                       2.6f * impact, 0.30f, 0.035f * impact,
                       tracer.effectMask ? tracer.color : Vec3f{ 1.0f, 0.6f, 0.25f }, 5.0f * impact);
    }
    tracers_.push_back(tracer);
    spawnImpact(tracer.end, tracer.endHeight, tracer.hit);

    if (targetIndex >= 0 && targetIndex < static_cast<int>(enemies_.size())) {
        const BuildStats& bs = build_.stats();
        Enemy& enemy = enemies_[static_cast<size_t>(targetIndex)];
        bool crit = false;
        float damage;
        if (headshot) {
            damage = tunables_.enemyMaxHealth;
        } else {
            damage = baseDamage * bs.damageMult;
            if (!scriptedDeterministic_ && bs.critChance > 0.0f && rng_.unit() < bs.critChance) {
                crit = true;
                damage *= bs.critDamageMult;
            }
        }
        const bool precision = headshot || crit;
        const Vec2 hitPos = enemy.pos;
        float appliedDamage = 0.0f, corrodeBonus = 0.0f, shatterBonus = 0.0f;
        damageEnemy(enemy, damage, &appliedDamage, &corrodeBonus, &shatterBonus);
        if (appliedDamage > 0.0f) {
            // Main number = the weapon's own damage; corrode/shatter split into their own "+N" pops,
            // so the numbers add up to the real hit (display split, no balance change).
            const float baseDmg = std::max(0.0f, appliedDamage - corrodeBonus - shatterBonus);
            const int shown = std::max(1, static_cast<int>(std::lround(baseDmg)));
            spawnCombatText({ enemy.pos.x, tracer.endHeight + 0.34f, enemy.pos.y },
                            std::to_string(shown),
                            precision ? DmgTextCrit : damageTextColorForMask(tracer.effectMask),
                            precision ? 1.18f : 1.0f);
            spawnStatusBonusText(enemy, corrodeBonus, shatterBonus, tracer.endHeight);
        }
        applyLifeLeech(audio, appliedDamage, { enemy.pos.x, tracer.endHeight, enemy.pos.y });
        // Weight: a heavier weapon staggers the target harder and lands with more crunch,
        // so a shotgun slug reads as mass and the SMG as a fast tap.
        const WeaponProfile& hitProfile = activeWeaponProfile();
        const float impactW = std::clamp(hitProfile.impactScale, 0.4f, 1.8f);
        enemy.hurtTimer = std::max(0.08f, 0.07f * impactW);
        enemy.hitPunch = std::min(1.6f, impactW * (precision ? 1.25f : 1.0f));
        const Vec2 away = normalize(enemy.pos - player_.pos);
        enemy.vel = enemy.vel + away * (tunables_.feelHitKnockback * bs.knockbackMult * impactW);
        hitmarkerTimer_ = 0.16f;
        if (precision) precisionMarkerTimer_ = 0.18f;
        addShake((precision ? 0.26f : 0.16f) * std::max(0.7f, impactW));
        playFeedback(audio, precision ? FeedbackEventType::HitCrit : FeedbackEventType::Hitmarker,
                     precision ? 1.0f : 0.85f);
        // Per-connect micro hit-stop: the world bites for a frame when a shot lands so each
        // hit reads as impact. Heavy single-shot guns bite hard; automatics get only a light
        // touch (scaled down) so sustained fire never stutters.
        {
            float hitFreeze = tunables_.feelHitstopHit * impactW * (precision ? 1.35f : 1.0f);
            if (hitProfile.automatic) hitFreeze *= 0.30f;
            hitStopTimer_ = std::max(hitStopTimer_, std::min(hitFreeze, 0.060f));
        }
        if (enemy.health > 0.0f) {
            playEnemyAudio(audio, enemy, EnemyEventType::Hurt, precision ? 0.82f : 0.64f);
            applyShotElements(audio, targetIndex);   // M2/M3: build + aspect elements ride every shot
        }

        if (bs.explodeOnHitDamage > 0.0f) {
            applyAreaDamage(audio, hitPos, 1.6f, bs.explodeOnHitDamage, targetIndex, true);
        }
        if (bs.chainOnHitDamage > 0.0f) {
            int nearest = -1; float nearestSq = 4.0f * 4.0f;
            for (int j = 0; j < static_cast<int>(enemies_.size()); ++j) {
                if (j == targetIndex || !enemies_[static_cast<size_t>(j)].active) continue;
                const float d2 = lengthSq(enemies_[static_cast<size_t>(j)].pos - hitPos);
                if (d2 < nearestSq) { nearestSq = d2; nearest = j; }
            }
            if (nearest >= 0) applyAreaDamage(audio, enemies_[static_cast<size_t>(nearest)].pos, 0.6f, bs.chainOnHitDamage, -1);
        }

        if (enemy.active && enemy.health <= 0.0f) {
            enemy.active = false;
            onEnemyKilled(audio, enemy, precision);
        }
    }
}

// Per-weapon muzzle character, shared by the base report (spawnMuzzleSignature) AND the element
// overlays (spawnElementMuzzleFx) so an SMG's fire reads tight+fast and a shotgun's reads wide+heavy
// in BOTH the powder flash and the elemental spit. smoke = lingering puff, cone = lateral spread,
// bloom = barrel core glow, sparks/sparkSpeed = ejecta count + reach, energy = charged-plasma look.
namespace {
struct MuzzleCharacter {
    float smoke = 0.45f;
    float cone = 0.50f;
    float bloom = 0.75f;
    float sparkSpeed = 4.4f;
    int   sparks = 4;
    bool  energy = false;
    bool  automatic = true;
};
MuzzleCharacter weaponMuzzleCharacter(const WeaponProfile& profile) {
    MuzzleCharacter ch;
    ch.automatic = profile.automatic;
    const std::string& id = profile.id;
    if      (id == "ak47")           { ch.smoke = 0.95f; ch.cone = 0.55f; ch.bloom = 1.10f; ch.sparks = 8;  ch.sparkSpeed = 6.2f; }
    else if (id == "pistol")         { ch.smoke = 0.35f; ch.cone = 0.40f; ch.bloom = 0.60f; ch.sparks = 3;  ch.sparkSpeed = 4.2f; }
    else if (id == "carbine")        { ch.smoke = 0.55f; ch.cone = 0.42f; ch.bloom = 0.82f; ch.sparks = 5;  ch.sparkSpeed = 5.2f; }
    else if (id == "pulse_smg" ||
             id == "machine_pistol") { ch.smoke = 0.25f; ch.cone = 0.50f; ch.bloom = 0.70f; ch.sparks = 5;  ch.sparkSpeed = 5.6f; ch.energy = true; }
    else if (id == "scattergun")     { ch.smoke = 1.30f; ch.cone = 1.10f; ch.bloom = 1.30f; ch.sparks = 11; ch.sparkSpeed = 7.0f; }
    else if (id == "marksman")       { ch.smoke = 0.80f; ch.cone = 0.26f; ch.bloom = 1.28f; ch.sparks = 6;  ch.sparkSpeed = 8.6f; }
    else if (id == "railbolt")       { ch.smoke = 0.50f; ch.cone = 0.45f; ch.bloom = 1.45f; ch.sparks = 6;  ch.sparkSpeed = 5.0f; ch.energy = true; }
    else {
        switch (profile.archetype) {
            case WeaponArchetype::Spread:     ch.smoke = 1.20f; ch.cone = 1.00f; ch.bloom = 1.20f; ch.sparks = 10; ch.sparkSpeed = 7.0f; break;
            case WeaponArchetype::Beam:       ch.smoke = 0.25f; ch.cone = 0.50f; ch.bloom = 0.70f; ch.sparks = 5;  ch.sparkSpeed = 5.6f; ch.energy = true; break;
            case WeaponArchetype::Projectile: ch.smoke = 0.50f; ch.cone = 0.45f; ch.bloom = 1.35f; ch.sparks = 6;  ch.sparkSpeed = 5.0f; ch.energy = true; break;
            case WeaponArchetype::Burst:      ch.smoke = 0.55f; ch.cone = 0.44f; ch.bloom = 0.82f; ch.sparks = 5;  ch.sparkSpeed = 5.2f; break;
            default: break;
        }
    }
    const float fscale = clamp(profile.muzzleFlashScale, 0.45f, 1.4f);
    ch.smoke *= 0.70f + 0.40f * fscale;
    ch.bloom *= 0.80f + 0.30f * fscale;
    return ch;
}
} // namespace

void PulseGame::spawnMuzzleSignature(const WeaponProfile& profile, Vec3f muzzle,
                                     Vec2 aimDir, Vec2 aimRight, float flash, uint32_t shotMask) {
    (void)aimRight;
    const Vec3f fwd{ aimDir.x, 0.0f, aimDir.y };
    const Vec3f flashCol = shotMask ? shotEffectTint(shotMask) : Vec3f{
        clamp(profile.muzzleFlashR / 255.0f, 0.0f, 1.4f),
        clamp(profile.muzzleFlashG / 255.0f, 0.0f, 1.4f),
        clamp(profile.muzzleFlashB / 255.0f, 0.0f, 1.4f) };
    const MuzzleCharacter ch = weaponMuzzleCharacter(profile);

    // Near-barrel core bloom: one bright, very short puff that sits right on the muzzle.
    spawnParticles({ muzzle.x + fwd.x * 0.06f, muzzle.y, muzzle.z + fwd.z * 0.06f },
                   { fwd.x * 0.6f, 0.04f, fwd.z * 0.6f }, 1, 0.05f, 0.06f,
                   0.040f + 0.055f * ch.bloom, flashCol, 3.6f + flash * 2.8f, 0.0f, 6.0f);

    // Spark fan: hot ejecta thrown down-range and spread across the cone, snapping to a halt (high
    // drag, streaked). Powder weapons throw warm sparks; energy weapons spit cyan plasma.
    if (ch.sparks > 0) {
        const Vec3f sparkCol = ch.energy ? Vec3f{ 0.70f, 1.85f, 2.35f } : Vec3f{ 1.95f, 1.05f, 0.40f };
        spawnParticles({ muzzle.x + fwd.x * 0.12f, muzzle.y, muzzle.z + fwd.z * 0.12f },
                       { fwd.x * ch.sparkSpeed, 0.10f, fwd.z * ch.sparkSpeed }, ch.sparks,
                       ch.sparkSpeed * 0.26f * (0.6f + ch.cone), 0.13f, 0.012f, sparkCol,
                       7.0f, 1.2f, 5.5f, 0.45f);
    }

    // Smoke: pale gray powder puff that lingers and drifts up off the barrel. Energy weapons vent a
    // faint charged haze instead.
    if (ch.smoke > 0.05f && !ch.energy) {
        const int n = std::max(1, static_cast<int>(std::lround(ch.smoke * 3.0f)));
        spawnParticles({ muzzle.x + fwd.x * 0.18f, muzzle.y + 0.02f, muzzle.z + fwd.z * 0.18f },
                       { fwd.x * 1.1f, 0.35f, fwd.z * 1.1f }, n,
                       0.18f + 0.10f * ch.cone, 0.34f + 0.22f * ch.smoke, 0.060f + 0.05f * ch.smoke,
                       { 0.26f, 0.25f, 0.24f }, 0.5f, -0.10f, 2.8f, 0.10f);
    } else if (ch.energy && ch.smoke > 0.05f) {
        spawnParticles({ muzzle.x + fwd.x * 0.16f, muzzle.y + 0.02f, muzzle.z + fwd.z * 0.16f },
                       { fwd.x * 1.3f, 0.20f, fwd.z * 1.3f }, 2, 0.16f, 0.22f, 0.050f,
                       { 0.40f, 0.95f, 1.35f }, 1.8f, 0.0f, 3.4f, 0.10f);
    }
}

// The elemental tell at the barrel. Each element keeps its identity (burn rises orange, shock arcs
// cyan, cryo sinks blue, corrode splatters green) but inherits the weapon's character: a shotgun
// sprays a wide cone of fire, a marksman lances a tight bolt, an SMG stutters short fast arcs. When
// the build carries TWO elements the muzzle shows the matching pair-reaction (the same five named
// combos that fire on enemies); three or more vents an unstable prismatic overload. Leech is a
// separate utility tell drawn alongside whatever else is active.
void PulseGame::spawnElementMuzzleFx(const WeaponProfile& profile, Vec3f muzzle,
                                     Vec2 aimDir, Vec2 aimRight, float flash, uint32_t shotMask) {
    if (!shotMask) return;
    const MuzzleCharacter ch = weaponMuzzleCharacter(profile);
    const Vec3f fwd{ aimDir.x, 0.0f, aimDir.y };
    const Vec3f rgt{ aimRight.x, 0.0f, aimRight.y };
    const float reach  = 2.8f + ch.sparkSpeed * 0.38f + flash;   // forward throw (sniper long, smg short)
    const float spread = 0.42f + 0.95f * ch.cone;                // lateral scatter (shotgun wide, sniper tight)
    const int   dens   = ch.automatic ? 2 : 4;                   // per-shot density (sustained vs punchy)

    // Hot wash in the blended element tint, weapon-scaled so the cone reads right.
    spawnParticles(muzzle, { fwd.x * reach * 0.65f, 0.16f, fwd.z * reach * 0.65f },
                   dens + 2, 0.55f + 0.55f * ch.cone, 0.14f, 0.020f,
                   shotEffectTint(shotMask), 5.2f, 1.4f, 2.8f, 0.12f);

    const bool b  = (shotMask & ShotFxBurn) != 0;
    const bool s  = (shotMask & ShotFxShock) != 0;
    const bool cy = (shotMask & ShotFxCryo) != 0;
    const bool co = (shotMask & ShotFxCorrode) != 0;
    const int  elemCount = static_cast<int>(b) + static_cast<int>(s) + static_cast<int>(cy) + static_cast<int>(co);

    if (elemCount >= 3) {
        // OVERLOAD: the build is carrying most of the table - the muzzle vents an unstable prism.
        spawnBlast(muzzle, { 1.55f, 1.40f, 1.85f }, 0.50f + 0.22f * flash);
        const Vec3f hue[4] = { elementTint(Element::Burn), elementTint(Element::Shock),
                               elementTint(Element::Cryo), elementTint(Element::Corrode) };
        for (int k = 0; k < 4; ++k) {
            const float sgnR = (k & 1) ? 1.0f : -1.0f;
            const float sgnY = (k & 2) ? 1.0f : -1.0f;
            spawnParticles({ muzzle.x + rgt.x * sgnR * spread * 0.6f, muzzle.y, muzzle.z + rgt.z * sgnR * spread * 0.6f },
                           { fwd.x * reach, 0.4f + 0.25f * sgnY, fwd.z * reach },
                           dens + 1, spread * 0.8f, 0.20f, 0.026f, hue[k], 7.4f, 0.6f, 3.6f, 0.24f);
        }
        spawnParticles(muzzle, { fwd.x * reach, 0.5f, fwd.z * reach },
                       dens + 2, spread * 0.8f, 0.14f, 0.030f, { 1.6f, 1.5f, 1.9f }, 9.0f, 0.2f, 4.5f, 0.18f);
    } else if (elemCount == 2) {
        if (b && s) {
            // PLASMA SURGE: ionized violet flare with a hot forward jet and a stray arc.
            const Vec3f col{ 1.65f, 0.75f, 1.95f };
            spawnBlast(muzzle, col, 0.42f + 0.18f * flash);
            spawnParticles(muzzle, { fwd.x * reach * 1.2f, 0.5f, fwd.z * reach * 1.2f },
                           dens + 4, spread * 0.9f, 0.18f, 0.026f, col, 8.6f, 0.2f, 4.0f, 0.30f);
            spawnBeam(muzzle, { muzzle.x + fwd.x * (1.1f + reach * 0.18f), muzzle.y + 0.05f,
                                muzzle.z + fwd.z * (1.1f + reach * 0.18f) }, { 0.95f, 1.90f, 2.30f }, 0.014f, 0.24f);
        } else if (b && cy) {
            // THERMAL SHOCK: heat meets cold - a billowing white steam burst shot through with ice flecks.
            spawnBlast(muzzle, { 1.20f, 1.25f, 1.50f }, 0.50f + 0.20f * flash);
            spawnParticles(muzzle, { fwd.x * reach * 0.7f, 0.6f, fwd.z * reach * 0.7f },
                           dens + 5, spread * 1.2f, 0.34f, 0.060f, { 1.10f, 1.15f, 1.35f }, 2.2f, -0.05f, 3.4f, 0.10f);
            spawnParticles(muzzle, { fwd.x * reach, 0.3f, fwd.z * reach },
                           dens + 2, spread, 0.16f, 0.020f, { 0.85f, 1.40f, 1.90f }, 6.0f, 1.2f, 3.5f, 0.20f);
        } else if (b && co) {
            // CAUSTIC FIRE: orange flame fused with green acid globs that streak forward and spit down.
            spawnParticles(muzzle, { fwd.x * reach * 0.95f, 0.5f, fwd.z * reach * 0.95f },
                           dens + 4, spread, 0.24f, 0.038f, { 1.50f, 0.95f, 0.25f }, 6.4f, 1.0f, 2.0f, 0.26f);
            spawnParticles(muzzle, { fwd.x * reach * 0.7f, -0.2f, fwd.z * reach * 0.7f },
                           dens + 2, spread, 0.30f, 0.030f, { 0.75f, 1.50f, 0.35f }, 5.6f, 3.5f, 2.0f, 0.12f);
        } else if (s && cy) {
            // SUPERCONDUCT: a long cold-blue arc lance with frosted sparks riding it.
            const float arcLen = 1.0f + reach * 0.28f;
            spawnBeam(muzzle, { muzzle.x + fwd.x * arcLen, muzzle.y + 0.04f, muzzle.z + fwd.z * arcLen },
                      { 0.55f, 1.50f, 2.30f }, 0.018f, 0.26f);
            spawnParticles(muzzle, { fwd.x * reach, 0.1f, fwd.z * reach },
                           dens + 4, spread, 0.18f, 0.024f, { 0.55f, 1.35f, 2.20f }, 7.6f, 0.6f, 3.8f, 0.24f);
        } else if (s && co) {
            // GALVANIC MELT: a sputtering green-cyan arc that throws corrosive electric flecks.
            const float arcLen = 0.9f + reach * 0.20f;
            spawnBeam(muzzle, { muzzle.x + fwd.x * arcLen - rgt.x * 0.10f, muzzle.y + 0.03f,
                                muzzle.z + fwd.z * arcLen - rgt.z * 0.10f }, { 0.60f, 1.90f, 1.30f }, 0.015f, 0.22f);
            spawnParticles(muzzle, { fwd.x * reach * 0.9f, -0.1f, fwd.z * reach * 0.9f },
                           dens + 4, spread, 0.22f, 0.030f, { 0.56f, 1.85f, 1.25f }, 7.0f, 2.4f, 3.0f, 0.20f);
        } else if (cy && co) {
            // BRITTLE FROST-ACID (no named reaction): a teal sheet of frosted acid that sinks and clings.
            spawnParticles(muzzle, { fwd.x * reach * 0.8f, -0.3f, fwd.z * reach * 0.8f },
                           dens + 4, spread * 1.05f, 0.28f, 0.032f, { 0.55f, 1.50f, 1.00f }, 5.6f, 2.6f, 2.6f, 0.14f);
            spawnParticles(muzzle, { fwd.x * reach * 0.6f, -0.1f, fwd.z * reach * 0.6f },
                           dens + 2, spread, 0.24f, 0.024f, { 0.60f, 1.70f, 1.60f }, 5.0f, 1.4f, 3.0f, 0.10f);
        }
    } else {
        // Single element, weapon-flavored.
        if (b) {
            spawnParticles(muzzle, { fwd.x * reach, 0.95f, fwd.z * reach },
                           dens + 3, spread * 0.7f, 0.20f, 0.034f, elementTint(Element::Burn), 6.6f, -0.2f, 2.2f, 0.22f);
        } else if (s) {
            const float arcLen = 0.85f + reach * 0.22f;
            spawnBeam({ muzzle.x + rgt.x * 0.08f, muzzle.y + 0.02f, muzzle.z + rgt.z * 0.08f },
                      { muzzle.x + fwd.x * arcLen - rgt.x * 0.16f, muzzle.y + 0.06f, muzzle.z + fwd.z * arcLen - rgt.z * 0.16f },
                      elementTint(Element::Shock), 0.016f, 0.30f);
            spawnParticles(muzzle, { fwd.x * reach * 1.15f, 0.30f, fwd.z * reach * 1.15f },
                           dens + 2, spread, 0.14f, 0.022f, elementTint(Element::Shock), 8.4f, 0.0f, 4.6f, 0.26f);
        } else if (cy) {
            spawnParticles(muzzle, { fwd.x * reach * 0.85f, -0.15f, fwd.z * reach * 0.85f },
                           dens + 3, spread, 0.26f, 0.027f, elementTint(Element::Cryo), 5.4f, 1.6f, 3.2f, 0.10f);
        } else if (co) {
            spawnParticles(muzzle, { fwd.x * reach * 0.8f, -0.45f, fwd.z * reach * 0.8f },
                           dens + 3, spread * 1.1f, 0.30f, 0.037f, elementTint(Element::Corrode), 5.6f, 4.5f, 2.1f, 0.12f);
        }
    }

    // Life-leech is a utility tell, not an element: a green draw-beam drawn alongside whatever fires.
    if (shotMask & ShotFxLeech) {
        const Vec3f c{ 0.36f, 1.95f, 0.82f };
        spawnBeam(muzzle, { muzzle.x + fwd.x * 1.15f, muzzle.y - 0.04f, muzzle.z + fwd.z * 1.15f }, c, 0.014f, 0.34f);
        spawnParticles(muzzle, { fwd.x * 2.5f, 0.05f, fwd.z * 2.5f },
                       ch.automatic ? 1 : 3, 0.55f, 0.24f, 0.030f, c, 5.2f, -0.1f, 2.7f, 0.12f);
    }
}

bool PulseGame::fireProfileShot(AudioSystem& audio, const WeaponProfile& profile, int screenW, int screenH, bool consumeAmmo) {
    if (consumeAmmo) {
        if (weapon_.ammo <= 0) return false;
        --weapon_.ammo;
    }

    weapon_.timeSinceShot = 0.0f;
    audio.playWeaponEvent(profile.id, WeaponEventType::Fire, profile.fireVolume, fireSoundIndex_++);
    muzzleFlashTimer_ = std::max(muzzleFlashTimer_, std::max(0.01f, profile.muzzleFlashSeconds));
    fireFovKick_ = std::max(fireFovKick_, profile.fovPunch);
    fireCameraKick_ = clamp(std::max(fireCameraKick_, profile.cameraKick) + profile.cameraKick * 0.08f,
                            0.0f, std::max(profile.cameraKick, profile.cameraKick * 1.35f));
    // (Music intensity is the Pulse meter, floored by the run/wave state - firing alone no
    //  longer spikes it; momentum comes from kills, not trigger-pulls.)
    addShake(tunables_.cameraShakeFire * std::max(0.35f, profile.impactScale));
    spawnCasing(profile, screenW, screenH);

    const int shotIndex = recoilShotIndex_;
    const float spray01 = clamp(static_cast<float>(shotIndex) / 12.0f, 0.0f, 1.0f);
    const float move01 = clamp(length(player_.vel) / std::max(0.1f, tunables_.walkSpeed), 0.0f, 1.25f);
    const float inaccuracyDeg = scriptedDeterministic_ ? 0.0f
        : std::max(0.0f, profile.firstShotInaccuracyDeg)
            + std::max(0.0f, profile.spreadDeg) * (profile.archetype == WeaponArchetype::Spread ? 0.0f : 1.0f)
            + std::max(0.0f, profile.sprayInaccuracyDeg) * spray01
            + std::max(0.0f, profile.moveInaccuracyDeg) * move01 * move01
            + (!player_.grounded ? std::max(0.0f, profile.airborneInaccuracyDeg) : 0.0f);
    const float inaccuracy = degToRad(inaccuracyDeg);
    const RecoilPoint recoilPoint = profile.recoilPattern.empty()
        ? RecoilPoint{}
        : profile.recoilPattern[static_cast<size_t>(std::clamp(shotIndex, 0, static_cast<int>(profile.recoilPattern.size()) - 1))];
    weaponKick_ = std::max(weaponKick_, profile.viewmodelKick * (1.0f + spray01 * 0.42f));
    weaponKickSide_ = clamp(recoilPoint.yawDeg * profile.viewmodelSideScale, -0.65f, 0.65f);

    const float aimYaw = player_.yaw + recoilOffsetYaw_;
    const float aimPitch = player_.pitch + recoilOffsetPitch_;
    {
        const Vec2 aimDir = fromAngle(aimYaw);
        const Vec2 aimRight = rightFromForward(aimDir);
        const float muzzleH = clamp(player_.height - 0.11f, 0.08f, 0.92f);
        const Vec3f muzzleWorld{
            player_.pos.x + aimDir.x * 0.42f + aimRight.x * 0.18f,
            muzzleH,
            player_.pos.y + aimDir.y * 0.42f + aimRight.y * 0.18f
        };
        const float flash = clamp(profile.muzzleFlashScale, 0.45f, 1.4f);
        const uint32_t shotMask = activeShotEffectMask();
        const Vec3f profileFlash{
            clamp(profile.muzzleFlashR / 255.0f, 0.0f, 1.4f),
            clamp(profile.muzzleFlashG / 255.0f, 0.0f, 1.4f),
            clamp(profile.muzzleFlashB / 255.0f, 0.0f, 1.4f)
        };
        const Vec3f flashColor = shotMask ? shotEffectTint(shotMask) : profileFlash;
        spawnParticles(muzzleWorld,
                       { aimDir.x * (1.7f + flash * 1.2f), 0.06f, aimDir.y * (1.7f + flash * 1.2f) },
                       profile.automatic ? 2 : 3,
                       0.55f + flash * 0.22f,
                       0.055f + profile.muzzleFlashSeconds * 0.35f,
                       0.010f + flash * 0.006f,
                       flashColor,
                       2.6f + flash * 2.2f);
        // Weapon-specific report: barrel bloom, hot spark fan and powder/charge smoke. Gives each
        // gun a distinct muzzle silhouette on top of its tuned flash colour.
        spawnMuzzleSignature(profile, muzzleWorld, aimDir, aimRight, flash, shotMask);
        // Elemental tell at the barrel: weapon-flavored single-element spit, the matching pair-reaction
        // muzzle for two active elements, or a prismatic overload for three-plus (spawnElementMuzzleFx).
        spawnElementMuzzleFx(profile, muzzleWorld, aimDir, aimRight, flash, shotMask);
    }
    float shotYaw = aimYaw;
    float shotPitch = aimPitch;
    if (inaccuracy > 0.0f) {
        const float spreadAngle = rng_.range(0.0f, TwoPi);
        const float spreadRadius = std::sqrt(rng_.unit()) * inaccuracy;
        shotYaw += std::cos(spreadAngle) * spreadRadius;
        shotPitch += std::sin(spreadAngle) * spreadRadius;
    }
    shotPitch = clamp(shotPitch, -1.5f, 1.5f);

    const float baseDmg = weaponBaseDamage();
    switch (profile.archetype) {
        case WeaponArchetype::Spread: {
            const int pellets = std::max(1, profile.pellets);
            const float cone = degToRad(std::max(0.0f, profile.spreadDeg));
            for (int pel = 0; pel < pellets; ++pel) {
                float py = aimYaw, pp = aimPitch;
                if (!scriptedDeterministic_ && cone > 0.0f) {
                    const float ang = rng_.range(0.0f, TwoPi);
                    const float rad = std::sqrt(rng_.unit()) * cone;
                    py += std::cos(ang) * rad;
                    pp += std::sin(ang) * rad;
                }
                resolveHitscan(audio, py, pp, baseDmg, screenW, screenH);
            }
            break;
        }
        case WeaponArchetype::Projectile:
            spawnPlayerProjectile(shotYaw, shotPitch, baseDmg, profile.projectileSpeed, profile.splashRadius);
            break;
        case WeaponArchetype::Burst:
        case WeaponArchetype::HitscanAuto:
        case WeaponArchetype::Beam:
        default:
            resolveHitscan(audio, shotYaw, shotPitch, baseDmg, screenW, screenH);
            break;
    }

    if (!scriptedDeterministic_) {
        recoilOffsetPitch_ = std::min(degToRad(recoilPoint.pitchDeg * std::max(0.0f, profile.recoilPitchScale)), degToRad(16.0f));
        recoilOffsetYaw_ = wrapAngle(degToRad(recoilPoint.yawDeg * std::max(0.0f, profile.recoilYawScale)));
        ++recoilShotIndex_;
    }

    if (weapon_.ammo <= 0 && weapon_.reserve > 0) {
        weapon_.reloading = true;
        weapon_.reloadMagOutPlayed = false;
        weapon_.reloadInsertPlayed = false;
        weapon_.reloadEndPlayed = false;
        weapon_.reloadRemaining = reloadDuration();
        weapon_.shellReloadTimer = profile.reloadMode == WeaponReloadMode::PerShell
            ? std::max(0.05f, profile.perShellSeconds / std::max(0.2f, build_.stats().reloadSpeedMult))
            : 0.0f;
        weapon_.queuedBurstShots = 0;
        weaponKick_ = std::max(weaponKick_, 0.40f);
        weaponKickSide_ = -0.14f;
        audio.playWeaponEvent(profile.id, WeaponEventType::ReloadStart, 0.88f, fireSoundIndex_++);
    }
    return true;
}

void PulseGame::tryFire(AudioSystem& audio, int screenW, int screenH) {
    if (weapon_.reloading) return;

    const WeaponProfile& profile = activeWeaponProfile();
    const float minInterval = 1.0f / std::max(0.1f, weaponBaseFireRate() * build_.stats().fireRateMult * overdriveFireRateMult());
    if (weapon_.timeSinceShot < minInterval) return;

    const float recoilReset = std::max(minInterval * 1.85f, std::max(0.05f, profile.recoilResetSeconds));
    if (weapon_.timeSinceShot > recoilReset) {
        recoilShotIndex_ = 0;
        // Starting a fresh spray after an idle pause should fire one responsive
        // round, not spend seconds of accumulated idle time in a single frame.
        if (profile.automatic && tunables_.weaponAutoFire) {
            weapon_.timeSinceShot = minInterval;
        }
    }

    const bool scheduledAuto = profile.automatic && tunables_.weaponAutoFire;
    const int maxCatchupShots = scheduledAuto ? 4 : 1;
    int shotsThisUpdate = 0;
    while (!weapon_.reloading && weapon_.timeSinceShot >= minInterval && shotsThisUpdate < maxCatchupShots) {
        if (weapon_.ammo <= 0) {
            audio.playWeaponEvent(profile.id, WeaponEventType::DryFire, 0.75f, fireSoundIndex_++);
            if (weapon_.reserve > 0) {
                weapon_.reloading = true;
                weapon_.reloadMagOutPlayed = false;
                weapon_.reloadInsertPlayed = false;
                weapon_.reloadEndPlayed = false;
                weapon_.reloadRemaining = reloadDuration();
                weapon_.shellReloadTimer = profile.reloadMode == WeaponReloadMode::PerShell
                    ? std::max(0.05f, profile.perShellSeconds / std::max(0.2f, build_.stats().reloadSpeedMult))
                    : 0.0f;
                weaponKick_ = std::max(weaponKick_, 0.40f);
                weaponKickSide_ = -0.14f;
                audio.playWeaponEvent(profile.id, WeaponEventType::ReloadStart, 0.88f, fireSoundIndex_++);
            }
            return;
        }

        const float carriedShotTime = weapon_.timeSinceShot;
        if (!fireProfileShot(audio, profile, screenW, screenH, true)) return;
        ++shotsThisUpdate;
        weapon_.timeSinceShot = scheduledAuto
            ? std::max(0.0f, carriedShotTime - minInterval)
            : 0.0f;

        if (profile.archetype == WeaponArchetype::Burst && profile.burstCount > 1 && weapon_.ammo > 0 && !weapon_.reloading) {
            weapon_.queuedBurstShots = std::min(profile.burstCount - 1, weapon_.ammo);
            weapon_.queuedBurstTimer = std::max(0.01f, profile.burstInterval);
            break;
        }
        if (!scheduledAuto) break;
    }
}

// Shared kill resolution: score + the decisive kill feedback (spec s5) + the build's
// on-kill hooks (aggression economy: defense from offense). Reached from a direct
// shot, a chain/explosion, or an explode-on-kill cascade. The enemy is already
// marked inactive by the caller.
void PulseGame::onEnemyKilled(AudioSystem& audio, const Enemy& enemy, bool headshot) {
    ++score_;
    killConfirmTimer_ = 0.25f;
    pulse_.onKill(headshot);   // Pulse: kills build momentum (chains ramp harder); headshots hit harder
    // Aggression charges the abilities: ~3 kills = a grenade, ~11 = an ultimate.
    addAbilityCharge(audio, headshot ? 0.42f : 0.34f, headshot ? 0.12f : 0.09f);
    hitStopTimer_ = std::max(hitStopTimer_,
        tunables_.feelHitstopKill * (headshot ? std::max(1.0f, tunables_.feelHitstopPrecisionMult) : 1.0f));
    addShake(tunables_.cameraShakeKill);
    spawnBurst(enemy.pos, headshot);
    // Bone shatter (Neon Ink Brutalism skeletons): white bone-chip gibs scatter on death. A
    // HEADSHOT detonates the whole skeleton into bone (it explodes - no falling corpse); a body
    // kill keeps the graceful death-clip corpse + a lighter bone spray. Scales up for the boss.
    const AnimatedEnemyModel& dm = enemyRenderModel(enemy);
    const float bossSc = enemy.boss ? BossVisualScale : 1.0f;
    const float bH = std::max(0.6f, dm.worldHeight * bossSc);
    const Vec3f bone{ 0.92f, 0.88f, 0.78f };
    const int gibs = static_cast<int>((headshot ? 30 : 12) * (enemy.boss ? 1.8f : 1.0f));
    spawnParticles({ enemy.pos.x, bH * 0.55f, enemy.pos.y }, { 0.0f, headshot ? 3.4f : 2.2f, 0.0f },
                   gibs, headshot ? 3.0f : 2.0f, 0.85f, 0.06f * bossSc, bone, 0.12f, 7.0f, 0.5f, 0.0f);
    if (headshot) {
        spawnParticles({ enemy.pos.x, bH * 0.92f, enemy.pos.y }, { 0.0f, 3.8f, 0.0f },   // skull shards
                       7, 1.8f, 1.0f, 0.10f * bossSc, bone, 0.18f, 8.0f, 0.35f, 0.0f);
    }
    if (!headshot && dm.loaded && dm.multiClip && dm.role.death >= 0 && corpses_.size() < 24) {
        EnemyCorpse corpse;
        corpse.kind = enemy.kind;
        corpse.visual = enemyVisualIndex(enemy);
        corpse.boss = enemy.boss;
        corpse.bossKind = enemy.bossKind;
        corpse.pos = enemy.pos;
        const Vec2 toPlayer = player_.pos - enemy.pos;
        corpse.facing = lengthSq(toPlayer) > 1e-4f ? normalize(toPlayer) : Vec2{ 1.0f, 0.0f };
        corpse.scale = dm.worldScale * bossSc;
        corpse.dur = std::min(1.6f, std::max(0.6f, dm.model.clipDuration(dm.role.death))) + 0.4f;
        corpses_.push_back(corpse);
    } else if (!headshot && dm.loaded && corpses_.size() < 24) {
        // Static Meshy enemies have no death clip, so make the whole body perform a short
        // transform-driven knockdown/tumble, then dissolve into the floor.
        EnemyCorpse corpse;
        corpse.kind = enemy.kind;
        corpse.visual = enemyVisualIndex(enemy);
        corpse.boss = enemy.boss;
        corpse.bossKind = enemy.bossKind;
        corpse.pos = enemy.pos;
        const Vec2 toPlayer = player_.pos - enemy.pos;
        corpse.facing = lengthSq(toPlayer) > 1e-4f ? normalize(toPlayer) : Vec2{ 1.0f, 0.0f };
        corpse.scale = dm.worldScale * bossSc;
        corpse.fall = true;
        corpse.fallY = std::max(dm.hoverY, 0.10f * dm.worldHeight * bossSc);
        corpse.vy = 0.55f;          // a small upward pop before gravity takes over
        corpse.dur = enemy.boss ? 2.8f : 2.1f;
        corpses_.push_back(corpse);
    } else if (!dm.loaded) {
        spawnDebris(enemy, headshot);   // legacy non-skinned fallback
    }
    playEnemyAudio(audio, enemy, EnemyEventType::Death, enemy.boss ? 1.10f : (headshot ? 0.96f : 0.82f));
    // Player kill confirmation: a beefier "elite" confirm for bosses/elites, the crisp
    // standard confirm otherwise. This is the player bus, layered over the enemy death.
    const bool eliteKill = enemy.boss || enemy.affix != EliteAffix::None;
    playFeedback(audio, eliteKill ? FeedbackEventType::KillElite : FeedbackEventType::Kill,
                 enemy.boss ? 1.15f : (headshot ? 1.05f : 0.9f));

    // Feature 2: scrap drops by kind / affix / boss, scaled by RunMods.scrapMult (heat /
    // deals). Deterministic (no RNG draw) so the run-RNG's offered rolls stay pure and the
    // economy is reproducible given the path. Auto-collected with a brief "+N" HUD pop.
    {
        int base = enemy.kind == EnemyKind::Tank ? 3
                 : (enemy.kind == EnemyKind::Ranged || enemy.kind == EnemyKind::Stalker) ? 2
                 : 1; // Rusher
        if (enemy.boss) base = 16 + 4 * (run_.complete() ? 0 : run_.sector());
        if (enemy.affix != EliteAffix::None) base *= 2;
        const int award = std::max(1, static_cast<int>(std::lround(base * mods_.scrapMult * pulse_.scrapMult())));
        scrap_ += award;
        scrapFlashAmount_ = award;
        scrapFlashTimer_ = 0.7f;
    }

    // Build on-kill hooks: heal / shield / ammo on kill, then a detonation.
    const BuildStats& bs = build_.stats();
    if (bs.healOnKill > 0 && player_.hp > 0) {
        player_.hp = std::min(effectiveMaxHealth(), player_.hp + healAmount(bs.healOnKill));
    }
    if (bs.shieldOnKill > 0) {
        player_.shield = std::min(effectiveMaxShield(), player_.shield + healAmount(bs.shieldOnKill));
        shieldFlashTimer_ = std::max(shieldFlashTimer_, 0.12f);
    }
    if (bs.ammoOnKill > 0) {
        weapon_.reserve = std::min(std::max(1, weaponReserveMax()), weapon_.reserve + bs.ammoOnKill);
    }
    if (bs.explodeOnKillDamage > 0.0f) {
        const Vec2 at = enemy.pos;
        spawnBurst(at, false);
        spawnParticles({ at.x, 0.55f, at.y }, { 0.0f, 1.0f, 0.0f }, 16, 3.2f, 0.35f, 0.06f,
                       { 1.0f, 0.55f, 0.25f }, 6.0f);
        spawnDecal({ at.x, 0.015f, at.y }, { 0, 1, 0 }, 1u, 0.6f, { 0.02f, 0.015f, 0.012f }, 0.8f);
        applyAreaDamage(audio, at, 2.4f, bs.explodeOnKillDamage, -1);
    }

    // M3 affinity 5-set SIGNATURES (the build-identity payoff). Read the dying enemy's status.
    if (bs.burnDetonateOnKill && enemy.status.burn > 0.0f) {        // Pyro 5: burns detonate on death
        const Vec2 at = enemy.pos;
        const float dmg = 14.0f + 6.0f * enemy.status.burn;
        spawnParticles({ at.x, 0.6f, at.y }, { 0.0f, 1.5f, 0.0f }, 18, 3.8f, 0.42f, 0.06f,
                       elementTint(Element::Burn), 7.0f);
        applyAreaDamage(audio, at, 2.7f, dmg, -1);
        addShake(0.5f);
    }
    if (bs.corrodeSpread && enemy.status.corrode > 0.0f) {          // Acid 5: corrode spreads on death
        for (Enemy& n : enemies_)
            if (n.active && lengthSq(n.pos - enemy.pos) < 3.0f * 3.0f)
                applyElement(n, Element::Corrode, enemy.status.corrode * 0.6f, &audio);
    }
    if (bs.cryoNova && enemy.status.frozen > 0.0f) {                // Cryo 5: a shattered foe emits a chill nova
        spawnParticles({ enemy.pos.x, 0.6f, enemy.pos.y }, { 0.0f, 1.0f, 0.0f }, 14, 3.2f, 0.42f, 0.05f,
                       elementTint(Element::Cryo), 6.0f);
        for (Enemy& n : enemies_)
            if (n.active && lengthSq(n.pos - enemy.pos) < 3.2f * 3.2f)
                applyElement(n, Element::Cryo, 0.5f, &audio);
    }

    // Volatile elite: detonates on death - punishes careless point-blank kills, and
    // can wipe a cluster (a reward for spacing the explosion well).
    if (enemy.affix == EliteAffix::Volatile) {
        const Vec2 at = enemy.pos;
        spawnBurst(at, true);
        spawnParticles({ at.x, 0.55f, at.y }, { 0.0f, 1.0f, 0.0f }, 22, 4.0f, 0.40f, 0.07f,
                       { 1.0f, 0.45f, 0.2f }, 7.0f);
        applyAreaDamage(audio, at, 2.6f, 28.0f, -1);
        if (lengthSq(player_.pos - at) < 2.6f * 2.6f) damagePlayer(audio, 24, at);
        addShake(1.2f);
    }
}

// Apply damage to an enemy, honoring its elite affix. Shielded bleeds incoming
// damage (so it needs commitment / priority, not more base HP). Any hit pauses a
// Regen elite's healing. Returns true if this damage drops it.
bool PulseGame::damageEnemy(Enemy& e, float dmg, float* appliedDamage,
                            float* corrodeBonus, float* shatterBonus) {
    if (e.affix == EliteAffix::Shielded) dmg *= 0.5f;
    // M2: corrode melts armor (every point of damage hits harder); a frozen foe is brittle
    // (shatter bonus). Both stay true to the no-HP-sponge rule - they make kills FASTER.
    const float preStatus = dmg;   // damage before the corrode/shatter status multipliers
    if (e.status.corrode > 0.0f) dmg *= (1.0f + e.status.corrode * stat::kCorrodeAmpPerStack);
    const float afterCorrode = dmg;
    if (e.status.frozen > 0.0f) dmg *= stat::kShatterMult;
    const float afterShatter = dmg;
    if (e.boss && e.bossVulnTimer > 0.0f) dmg *= 1.6f;   // M5 weak-point: punish the post-attack recovery
    const float applied = std::max(0.0f, std::min(e.health, dmg));
    if (appliedDamage) *appliedDamage = applied;
    // Report the corrode/shatter share of the applied damage (proportional under the health clamp) so
    // a caller can pop it as its own number. DISPLAY ONLY - the health subtraction below is unchanged.
    if (corrodeBonus || shatterBonus) {
        const float scale = dmg > 0.0001f ? applied / dmg : 0.0f;
        if (corrodeBonus) *corrodeBonus = (afterCorrode - preStatus) * scale;
        if (shatterBonus) *shatterBonus = (afterShatter - afterCorrode) * scale;
    }
    e.health -= dmg;
    e.regenCooldown = 1.5f;
    return e.health <= 0.0f;
}

void PulseGame::spawnStatusBonusText(const Enemy& e, float corrodeBonus, float shatterBonus, float baseHeight) {
    // Corrode armor-melt and freeze-shatter pop as their own small "+N" numbers, offset to either
    // side of the main hit number so the player SEES each status pulling its weight on the hit.
    if (corrodeBonus >= 0.5f)
        spawnCombatText({ e.pos.x + 0.34f, baseHeight + 0.18f, e.pos.y },
                        "+" + std::to_string(std::max(1, static_cast<int>(std::lround(corrodeBonus)))),
                        DmgTextCorrode, 0.74f);
    if (shatterBonus >= 0.5f)
        spawnCombatText({ e.pos.x - 0.34f, baseHeight + 0.32f, e.pos.y },
                        "+" + std::to_string(std::max(1, static_cast<int>(std::lround(shatterBonus)))),
                        DmgTextCryo, 0.80f);
}

void PulseGame::playElementFeedback(AudioSystem* audio, Element elem, float volume) {
    if (!audio || volume <= 0.0f) return;
    int idx = -1;
    FeedbackEventType event = FeedbackEventType::Hitmarker;
    switch (elem) {
        case Element::Burn:    idx = 0; event = FeedbackEventType::ElementBurn; break;
        case Element::Shock:   idx = 1; event = FeedbackEventType::ElementShock; break;
        case Element::Cryo:    idx = 2; event = FeedbackEventType::ElementCryo; break;
        case Element::Corrode: idx = 3; event = FeedbackEventType::ElementCorrode; break;
        default: break;
    }
    if (idx < 0) return;
    float& cd = elementFeedbackCooldown_[static_cast<size_t>(idx)];
    if (cd > 0.0f) return;
    cd = 0.105f;
    playFeedback(*audio, event, volume);
}

void PulseGame::playComboFeedback(AudioSystem* audio, float volume) {
    if (!audio || volume <= 0.0f) return;
    float& cd = elementFeedbackCooldown_[4];
    if (cd > 0.0f) return;
    cd = 0.18f;
    playFeedback(*audio, FeedbackEventType::ElementCombo, volume);
}

void PulseGame::playLeechFeedback(AudioSystem* audio, float volume) {
    if (!audio || volume <= 0.0f) return;
    float& cd = elementFeedbackCooldown_[5];
    if (cd > 0.0f) return;
    cd = 0.14f;
    playFeedback(*audio, FeedbackEventType::ElementLeech, volume);
}

void PulseGame::applyLifeLeech(AudioSystem& audio, float appliedDamage, Vec3f from) {
    const BuildStats& bs = build_.stats();
    if (bs.lifeLeechPct <= 0.0f || appliedDamage <= 0.0f || player_.hp <= 0) return;
    const int maxHp = std::max(1, effectiveMaxHealth());
    if (player_.hp >= maxHp) {
        lifeLeechCarry_ = 0.0f;
    }
    const Vec3f to{ player_.pos.x, clamp(player_.height - 0.16f, 0.12f, 1.2f), player_.pos.y };
    const Vec3f leechCol{ 0.36f, 1.95f, 0.82f };
    // A visible siphon strand makes leech read as an effect on the enemy body, not a quiet HP tick.
    spawnBeam(from, to, leechCol, 0.026f, 0.48f);
    spawnParticles(from, { 0.0f, 0.45f, 0.0f }, 4, 0.55f, 0.20f, 0.030f, leechCol, 5.8f, 0.6f, 2.0f, 0.08f);
    playLeechFeedback(&audio, player_.hp >= maxHp ? 0.32f : 0.56f);
    if (player_.hp >= maxHp) return;
    lifeLeechCarry_ += appliedDamage * bs.lifeLeechPct * std::max(0.0f, mods_.healMult);
    const int heal = static_cast<int>(std::floor(lifeLeechCarry_));
    if (heal <= 0 || player_.hp >= maxHp) return;
    const int before = player_.hp;
    player_.hp = std::min(maxHp, player_.hp + heal);
    const int actual = player_.hp - before;
    if (actual > 0) {
        lifeLeechCarry_ = std::max(0.0f, lifeLeechCarry_ - static_cast<float>(heal));
        lifeLeechFlashTimer_ = std::max(lifeLeechFlashTimer_, 0.22f);
        damageFlashTimer_ = 0.0f;
        spawnCombatText({ from.x, from.y + 0.42f, from.z }, "+" + std::to_string(actual), DmgTextLeech, 0.92f);
        spawnParticles(to, { 0.0f, 0.8f, 0.0f }, 5, 0.6f, 0.24f, 0.034f, leechCol, 5.0f, -0.2f, 1.5f, 0.04f);
    }
}

// M2: apply `stacks` of an element to one enemy (Corrode amps further application). The
// per-element behaviors (DoT / chain / freeze / amp) are driven from updateStatuses + damageEnemy.
void PulseGame::applyElement(Enemy& e, Element elem, float stacks, AudioSystem* audio) {
    if (stacks <= 0.0f || elem == Element::None) return;
    if (!e.active) return;
    StatusState& s = e.status;
    const bool hadBurn = s.burn > 0.35f;
    const bool hadShock = s.shock > 0.65f;
    const bool hadCryo = s.chill > 0.18f || s.frozen > 0.0f;
    const bool hadCorrode = s.corrode > 0.35f;
    bool frozeNow = false;
    stacks *= (1.0f + s.corrode * stat::kCorrodeStatusAmpPerStack);   // corrosion eats resistances
    switch (elem) {
        case Element::Burn:  s.burn = std::min(stat::kBurnCap, s.burn + stacks); break;
        case Element::Shock: s.shock += stacks; break;
        case Element::Cryo:
            // Bosses resist freeze hard (chill builds ~4x slower) so a cryo build SLOWS the
            // Warden but cannot chain-lock it into a non-fight; M5 can refine per-boss.
            if (s.frozen <= 0.0f) {
                s.chill = std::min(e.boss ? 0.9f : 1.0f, s.chill + stacks * (e.boss ? 0.25f : 1.0f));
                if (s.chill >= 1.0f) { s.frozen = stat::kFreezeDuration; e.vel = {}; frozeNow = true; }  // chill maxed -> freeze
            }
            break;
        case Element::Corrode: s.corrode = std::min(stat::kCorrodeCap, s.corrode + stacks); break;
        default: break;
    }
    const float hitY = clamp(enemyAimY(e), 0.42f, e.boss ? 2.8f : 1.25f);
    const Vec3f hit{ e.pos.x, hitY, e.pos.y };
    const float intensity = clamp(stacks, 0.4f, 3.0f);   // a bigger application reads as a bigger burst
    switch (elem) {
        case Element::Burn: {
            // Fire reads in three layers: licking flame tongues that rise (negative gravity, streaked),
            // scattering embers that arc and fall, and a slow dark smoke wisp peeling off the top.
            spawnParticles(hit, { 0.0f, 2.7f, 0.0f }, 3 + static_cast<int>(intensity),
                           0.55f, 0.26f, 0.050f, { 1.95f, 0.92f, 0.28f }, 7.4f, -0.7f, 2.4f, 0.34f);   // tongues
            spawnParticles(hit, { 0.0f, 1.4f, 0.0f }, 4 + static_cast<int>(intensity),
                           1.5f, 0.42f, 0.024f, elementTint(Element::Burn), 5.4f, 2.4f, 1.6f, 0.10f);  // embers
            spawnParticles({ hit.x, hit.y + 0.12f, hit.z }, { 0.0f, 0.7f, 0.0f }, 2,
                           0.22f, 0.74f, 0.100f, { 0.20f, 0.17f, 0.15f }, 0.35f, -0.18f, 2.6f, 0.12f); // smoke
            break;
        }
        case Element::Shock: {
            // A small storm: a fan of three jagged arcs flung off the body plus a radial crackle of
            // hot sparks that snaps to a halt (high drag, streaked), capped with a bright core flash.
            const Vec3f c = elementTint(Element::Shock);
            const Vec3f hot{ 0.85f, 1.95f, 2.40f };
            const float base = e.bobPhase + shakeTime_ * 3.7f + stacks * 0.31f;
            for (int b = 0; b < 3; ++b) {
                const float a = base + static_cast<float>(b) * 2.094f;
                const float reach = 0.42f + 0.16f * static_cast<float>(b & 1);
                spawnBeam({ hit.x, hit.y + 0.10f, hit.z },
                          { hit.x + std::cos(a) * reach, hit.y + 0.06f + std::sin(a * 1.7f) * 0.22f,
                            hit.z + std::sin(a) * reach },
                          (b == 1) ? hot : c, 0.013f, 0.20f);
            }
            spawnParticles(hit, { 0.0f, 0.5f, 0.0f }, 6, 2.6f, 0.16f, 0.020f, hot, 8.2f, 0.4f, 6.5f, 0.42f);  // crackle
            spawnParticles(hit, { 0.0f, 0.9f, 0.0f }, 3, 0.6f, 0.10f, 0.030f, c, 9.0f, 0.0f, 5.0f, 0.0f);     // core flash
            break;
        }
        case Element::Cryo: {
            // Frost forms in sharp shards that settle outward and down, wrapped in a low sinking vapor.
            const Vec3f c = elementTint(Element::Cryo);
            spawnParticles({ hit.x, hit.y - 0.05f, hit.z }, { 0.0f, 0.2f, 0.0f }, 5, 0.85f, 0.42f, 0.030f,
                           { 0.78f, 1.55f, 2.05f }, 4.6f, 1.4f, 2.6f, 0.22f);                              // shards
            spawnParticles({ hit.x, std::max(0.20f, hit.y - 0.22f), hit.z }, { 0.0f, -0.35f, 0.0f }, 3,
                           0.70f, 0.55f, 0.085f, c, 1.6f, 0.5f, 3.4f, 0.06f);                              // sinking vapor
            if (frozeNow) {
                // Ice encasement: a cold flash plus a radial shell of shards thrown out around the body.
                spawnBlast({ hit.x, std::max(0.28f, hit.y - 0.10f), hit.z }, c, 0.70f);
                for (int k = 0; k < 8; ++k) {
                    const float a = e.bobPhase + static_cast<float>(k) * 0.7854f;
                    spawnParticles({ hit.x + std::cos(a) * 0.18f, hit.y - 0.04f, hit.z + std::sin(a) * 0.18f },
                                   { std::cos(a) * 1.6f, 0.5f, std::sin(a) * 1.6f }, 2, 0.35f, 0.30f, 0.034f,
                                   { 0.86f, 1.75f, 2.30f }, 6.2f, 1.8f, 3.2f, 0.30f);
                }
            }
            break;
        }
        case Element::Corrode: {
            // Acid clings and runs: heavy globs streak down off the body while it fizzes a sickly haze.
            const Vec3f c = elementTint(Element::Corrode);
            spawnParticles({ hit.x, hit.y + 0.02f, hit.z }, { 0.0f, -1.4f, 0.0f }, 4 + static_cast<int>(intensity),
                           0.42f, 0.55f, 0.045f, c, 5.2f, 3.2f, 0.8f, 0.34f);                              // drips
            spawnParticles({ hit.x, hit.y - 0.06f, hit.z }, { 0.0f, 0.4f, 0.0f }, 6, 0.85f, 0.30f, 0.018f,
                           { 0.85f, 1.85f, 0.45f }, 6.4f, 1.6f, 4.2f, 0.0f);                               // fizz bubbles
            spawnParticles({ hit.x, std::max(0.18f, hit.y - 0.20f), hit.z }, { 0.0f, -0.2f, 0.0f }, 2,
                           0.50f, 0.60f, 0.075f, { 0.42f, 0.70f, 0.22f }, 0.6f, -0.1f, 3.0f, 0.08f);       // haze
            break;
        }
        default:
            break;
    }
    playElementFeedback(audio, elem, (frozeNow ? 0.88f : 0.52f) + clamp(stacks * 0.035f, 0.0f, 0.18f));
    triggerElementCombo(e, elem, stacks, hadBurn, hadShock, hadCryo, hadCorrode, audio);
}

void PulseGame::triggerElementCombo(Enemy& e, Element incoming, float stacks,
                                    bool hadBurn, bool hadShock, bool hadCryo, bool hadCorrode,
                                    AudioSystem* audio) {
    if (!e.active || e.comboCooldown > 0.0f || incoming == Element::None) return;
    StatusState& s = e.status;

    int combo = 0;
    float damage = 0.0f;
    float radius = 0.0f;
    Vec3f color{ 1.0f, 1.0f, 1.0f };
    const Vec2 at = e.pos;
    const auto setCombo = [&](int kind, float dmg, float rad, Vec3f col) {
        combo = kind;
        damage = dmg;
        radius = rad;
        color = col;
    };

    if ((incoming == Element::Cryo && hadBurn) || (incoming == Element::Burn && hadCryo)) {
        // THERMAL SHOCK: heat and cold rip each other off the target in a steam burst.
        const float stored = std::min(s.burn, 7.0f) * 2.0f + s.chill * 12.0f + (s.frozen > 0.0f ? 8.0f : 0.0f);
        s.burn *= 0.32f;
        s.chill *= 0.35f;
        s.frozen *= 0.45f;
        setCombo(2, stat::kThermalShockDamage + stored, 1.7f, { 1.10f, 0.92f, 1.55f });
    } else if ((incoming == Element::Shock && hadCryo) || (incoming == Element::Cryo && hadShock)) {
        // SUPERCONDUCT: cold metal carries the charge outward and primes nearby targets.
        s.shock += stat::kShockThreshold * 0.55f;
        s.chill = std::min(1.0f, s.chill + 0.18f);
        for (Enemy& n : enemies_) {
            if (&n == &e || !n.active || lengthSq(n.pos - at) > 3.0f * 3.0f) continue;
            n.status.shock += stat::kShockThreshold * 0.24f;
            n.status.chill = std::min(1.0f, n.status.chill + 0.18f);
        }
        setCombo(4, stat::kSuperconductDamage + (s.frozen > 0.0f ? 8.0f : 0.0f), 2.6f, { 0.50f, 1.35f, 2.20f });
    } else if ((incoming == Element::Corrode && hadShock) || (incoming == Element::Shock && hadCorrode)) {
        // GALVANIC MELT: acid turns shock into an immediate armor-melting discharge.
        s.corrode = std::min(stat::kCorrodeCap, s.corrode + 0.9f + stacks * 0.2f);
        s.shock = std::max(s.shock, stat::kShockThreshold);
        setCombo(5, stat::kGalvanicMeltDamage + s.corrode * 1.2f, 2.0f, { 0.56f, 1.85f, 1.25f });
    } else if ((incoming == Element::Corrode && hadBurn) || (incoming == Element::Burn && hadCorrode)) {
        // CAUSTIC FIRE: acid keeps the flame stuck to armor and spits corrosion outward.
        s.burn = std::min(stat::kBurnCap, s.burn + stat::kCausticFireBurn);
        for (Enemy& n : enemies_) {
            if (&n == &e || !n.active || lengthSq(n.pos - at) > 2.7f * 2.7f) continue;
            n.status.corrode = std::min(stat::kCorrodeCap, n.status.corrode + 0.55f);
            n.status.burn = std::min(stat::kBurnCap, n.status.burn + 0.35f);
        }
        setCombo(3, 8.0f + s.burn * 0.55f, 2.3f, { 1.55f, 1.10f, 0.26f });
    } else if ((incoming == Element::Shock && hadBurn) || (incoming == Element::Burn && hadShock)) {
        // PLASMA SURGE: burning air ionizes, producing a compact splash and more shock charge.
        s.shock += stat::kShockThreshold * 0.35f;
        setCombo(1, stat::kPlasmaDamage + std::min(s.burn, 6.0f), 1.8f, { 1.65f, 0.75f, 1.95f });
    }

    if (combo == 0) return;
    e.comboKind = combo;
    e.comboTimer = 0.42f;
    e.comboCooldown = stat::kComboCooldown;
    spawnBlast({ at.x, 0.66f, at.y }, color, 0.8f + radius * 0.16f);
    spawnParticles({ at.x, 0.70f, at.y }, { 0.0f, 1.2f, 0.0f }, 10 + combo * 2, 2.2f,
                   0.30f, 0.045f, color, 7.5f, 1.2f, 2.3f, 0.12f);
    if (radius > 0.0f && damage > 0.0f && audio) {
        for (Enemy& n : enemies_) {
            if (!n.active || &n == &e) continue;
            if (lengthSq(n.pos - at) > radius * radius) continue;
            float applied = 0.0f;
            const bool killed = damageEnemy(n, damage * 0.45f, &applied);
            if (applied > 0.0f)
                spawnCombatText({ n.pos.x, 0.94f, n.pos.y },
                                std::to_string(std::max(1, static_cast<int>(std::lround(applied)))),
                                DmgTextCombo, 0.88f);
            n.hurtTimer = std::max(n.hurtTimer, 0.08f);
            n.hitPunch = std::max(n.hitPunch, 0.9f);
            spawnBeam({ at.x, 0.68f, at.y }, { n.pos.x, 0.62f, n.pos.y }, color, 0.020f, 0.55f);
            if (killed && n.active) { n.active = false; onEnemyKilled(*audio, n, false); }
        }
    }
    if (damage > 0.0f && audio && e.active) {
        float applied = 0.0f;
        const bool killed = damageEnemy(e, damage, &applied);
        if (applied > 0.0f)
            spawnCombatText({ e.pos.x, 1.05f, e.pos.y },
                            std::to_string(std::max(1, static_cast<int>(std::lround(applied)))),
                            DmgTextCombo, 1.04f);
        e.hurtTimer = std::max(e.hurtTimer, 0.10f);
        e.hitPunch = std::max(e.hitPunch, 1.2f);
        if (killed && e.active) { e.active = false; onEnemyKilled(*audio, e, false); }
    }
    playComboFeedback(audio, 0.88f);
}

void PulseGame::applyBuildElements(AudioSystem* audio, int enemyIndex) {
    if (enemyIndex < 0 || enemyIndex >= static_cast<int>(enemies_.size())) return;
    const BuildStats& bs = build_.stats();
    if (bs.igniteOnHit <= 0.0f && bs.shockOnHit <= 0.0f && bs.chillOnHit <= 0.0f && bs.corrodeOnHit <= 0.0f) return;
    Enemy& e = enemies_[static_cast<size_t>(enemyIndex)];
    // M3 affinity 3-set amplifiers scale the stacks applied (burnApplyMult etc., default 1.0).
    // The Pulse AMPLIFIES status application (its signature role, not flat damage): a hotter
    // Pulse pushes more element stacks / set magnitude per hit.
    const float pm = pulse_.statusMult();
    if (bs.igniteOnHit > 0.0f)  applyElement(e, Element::Burn, bs.igniteOnHit * bs.burnApplyMult * pm, audio);
    if (bs.shockOnHit > 0.0f)   applyElement(e, Element::Shock, bs.shockOnHit * bs.shockApplyMult * pm, audio);
    if (bs.chillOnHit > 0.0f)   applyElement(e, Element::Cryo, bs.chillOnHit * bs.chillApplyMult * pm, audio);
    if (bs.corrodeOnHit > 0.0f) applyElement(e, Element::Corrode, bs.corrodeOnHit * bs.corrodeApplyMult * pm, audio);
}

void PulseGame::applyShotElements(AudioSystem& audio, int enemyIndex) {
    if (enemyIndex < 0 || enemyIndex >= static_cast<int>(enemies_.size())) return;
    applyBuildElements(&audio, enemyIndex);
    Enemy& e = enemies_[static_cast<size_t>(enemyIndex)];
    if (!e.active) return;
    if (const WeaponAspect* asp = activeAspect())
        if (asp->element > 0 && asp->elementStacks > 0.0f)
            applyElement(e, static_cast<Element>(asp->element), asp->elementStacks, &audio);
}

// M2: tick every enemy's elemental status each frame - Burn DoT, Shock chain discharge, decay.
// Freeze countdown + the chill slow are handled in updateEnemies/damageEnemy. Kills from DoT or
// chain route through onEnemyKilled exactly like a shot kill (score, Pulse, scrap, on-kill hooks).
void PulseGame::updateStatuses(AudioSystem& audio, float dt) {
    const size_t count = enemies_.size();   // stable: onEnemyKilled never push_backs to enemies_
    for (size_t i = 0; i < count; ++i) {
        Enemy& e = enemies_[i];
        if (!e.active) continue;
        // Telemetry: fraction of active enemy-frames carrying any element (status uptime).
        ++statusEnemyFrames_;
        if (e.status.any()) ++statusActiveFrames_;
        if (!e.status.any()) continue;
        StatusState& s = e.status;

        // Burn: damage-over-time on a fixed cadence (no knockback; pure attrition).
        if (s.burn > 0.0f) {
            s.burnTick += dt;
            int dotSeq = 0;
            while (s.burnTick >= stat::kBurnTickRate && e.active) {
                s.burnTick -= stat::kBurnTickRate;
                const float dot = stat::kBurnDotPerStack * s.burn * stat::kBurnTickRate;
                // Each tick licks a couple of streaked flame tongues up the body plus a stray ember.
                spawnParticles({ e.pos.x, 0.6f, e.pos.y }, { 0.0f, 2.0f, 0.0f }, 2, 0.6f, 0.22f, 0.034f,
                               { 1.9f, 0.84f, 0.26f }, 6.0f, -0.5f, 2.2f, 0.30f);
                spawnParticles({ e.pos.x, 0.55f, e.pos.y }, { 0.0f, 1.2f, 0.0f }, 1, 0.9f, 0.30f, 0.018f,
                               elementTint(Element::Burn), 4.6f, 2.2f, 1.5f, 0.0f);
                float applied = 0.0f;
                const bool killed = damageEnemy(e, dot, &applied);
                if (applied > 0.0f) {
                    // Pop each tick as its own small number, jittered AROUND the body so a sustained
                    // burn reads as a lively series of chip-damage numbers, not a smear at one point.
                    const float ph = shakeTime_ * 7.3f + e.bobPhase + static_cast<float>(dotSeq) * 1.9f;
                    spawnCombatText({ e.pos.x + std::cos(ph) * 0.28f,
                                      enemyAimY(e) + 0.26f,
                                      e.pos.y + std::sin(ph) * 0.22f },
                                    std::to_string(std::max(1, static_cast<int>(std::lround(applied)))),
                                    DmgTextBurn, 0.72f);
                }
                if (killed && e.active) { e.active = false; onEnemyKilled(audio, e, false); }
                ++dotSeq;
            }
            if (!e.active) continue;
        }

        // Shock: a charge crossing the threshold discharges - a burst on this foe + an arc that
        // damages and primes nearby foes (a self-propagating chain through a packed crowd).
        if (s.shock >= stat::kShockThreshold) {
            s.shock = 0.0f;
            const Vec3f shockCol = elementTint(Element::Shock);
            const Vec3f shockHot{ 0.90f, 2.00f, 2.40f };
            // Discharge: a fat radial crackle, a white-hot inner pop, and a pair of crossed arcs that
            // forks off the body so the chain trigger reads as a violent snap, not a puff.
            spawnParticles({ e.pos.x, 0.7f, e.pos.y }, { 0.0f, 1.0f, 0.0f }, 12, 3.6f, 0.26f, 0.04f,
                           shockCol, 8.4f, 0.4f, 5.5f, 0.35f);
            spawnParticles({ e.pos.x, 0.7f, e.pos.y }, { 0.0f, 1.4f, 0.0f }, 5, 0.8f, 0.14f, 0.05f,
                           shockHot, 9.6f);
            spawnBeam({ e.pos.x - 0.34f, 0.82f, e.pos.y + 0.12f },
                      { e.pos.x + 0.38f, 0.98f, e.pos.y - 0.18f },
                      shockCol, 0.017f, 0.26f);
            spawnBeam({ e.pos.x + 0.30f, 0.86f, e.pos.y + 0.16f },
                      { e.pos.x - 0.32f, 1.02f, e.pos.y - 0.20f },
                      shockHot, 0.014f, 0.20f);
            playElementFeedback(&audio, Element::Shock, 0.82f);
            float struckApplied = 0.0f;
            const bool struckKilled = damageEnemy(e, stat::kShockBurst, &struckApplied);
            if (struckApplied > 0.0f)
                spawnCombatText({ e.pos.x, 1.02f, e.pos.y },
                                std::to_string(std::max(1, static_cast<int>(std::lround(struckApplied)))),
                                DmgTextShock, 0.96f);
            // Volt 5-set (shockConduct): the chain reaches further, arcs to more foes, and the
            // CONDUCTED arc carries the whole build's on-hit elements into each struck neighbour.
            const bool conduct = build_.stats().shockConduct;
            const int maxArcs = stat::kShockArcCount + (conduct ? 2 : 0);
            const float arcRange = stat::kShockArcRange * (conduct ? 1.4f : 1.0f);
            int arcs = 0;
            for (size_t j = 0; j < count && arcs < maxArcs; ++j) {
                if (j == i || !enemies_[j].active) continue;
                Enemy& n = enemies_[j];
                if (lengthSq(n.pos - e.pos) > arcRange * arcRange) continue;
                spawnParticles({ (e.pos.x + n.pos.x) * 0.5f, 0.7f, (e.pos.y + n.pos.y) * 0.5f },
                               { 0.0f, 0.5f, 0.0f }, 4, 2.4f, 0.20f, 0.03f, shockCol, 6.0f);
                spawnBeam({ e.pos.x, 0.74f, e.pos.y }, { n.pos.x, 0.70f, n.pos.y }, shockCol, 0.016f, 0.42f);
                n.status.shock += stat::kShockThreshold * 0.4f;   // primes a follow-on chain
                if (conduct) applyBuildElements(&audio, static_cast<int>(j));   // conduct the build's elements down the chain
                float arcApplied = 0.0f;
                const bool arcKilled = damageEnemy(n, stat::kShockArc, &arcApplied);
                if (arcApplied > 0.0f)
                    spawnCombatText({ n.pos.x, 0.92f, n.pos.y },
                                    std::to_string(std::max(1, static_cast<int>(std::lround(arcApplied)))),
                                    DmgTextShock, 0.82f);
                if (arcKilled && n.active) { n.active = false; onEnemyKilled(audio, n, false); }
                ++arcs;
            }
            if (struckKilled && e.active) { e.active = false; onEnemyKilled(audio, e, false); }
            if (!e.active) continue;
        }

        statusDecay(s, dt);
    }
}

// Phase D boss: the sector "Warden". A high-HP priority target (the one HP exception
// the plan allows - a deliberate climax, not a regular sponge) with a telegraphed
// radial-orb pattern that escalates by phase and summons adds, all readable/dodgeable.
const char* PulseGame::bossName(int kind) {
    switch (kind) {
        case 1: return "SMELTER";
        case 2: return "CHOIR";
        default: return "WARDEN";
    }
}

void PulseGame::spawnBoss() {
    Enemy b;
    b.boss = true;
    b.kind = EnemyKind::Tank;
    b.pos = player_.pos + Vec2{ 0.0f, 8.0f };  // fallback placement
    const float bossRadius = enemyCollisionRadius(b);
    bool placed = false;
    for (int attempt = 0; attempt < 48; ++attempt) {
        const float angle = rng_.range(-Pi, Pi);
        const Vec2 candidate = player_.pos + fromAngle(angle) * 9.0f;
        if (collides(candidate, bossRadius) || !lineOfSight(candidate, player_.pos)) continue;
        b.pos = candidate;
        placed = true;
        break;
    }
    if (!placed && procEnvReady()) {
        const float hx = std::max(2.0f, wasteland_.halfExtentX() - 1.5f);
        const float hz = std::max(2.0f, wasteland_.halfExtentZ() - 1.5f);
        for (int attempt = 0; attempt < 96; ++attempt) {
            const Vec2 candidate{
                wasteland_.centerX() + rng_.range(-hx, hx),
                wasteland_.centerZ() + rng_.range(-hz, hz)
            };
            if (lengthSq(candidate - player_.pos) < 36.0f) continue;
            if (collides(candidate, bossRadius)) continue;
            b.pos = candidate;
            placed = true;
            if (lineOfSight(candidate, player_.pos)) break;
        }
    }
    if (collides(b.pos, bossRadius) && !pushOutOfCollision(b.pos, bossRadius)) {
        return;
    }
    // M5: a distinct boss per biome (Warden/Smelter/Choir). Picked by sector so each sector
    // climaxes in its own fight; the boss kind drives the attack patterns in updateBoss.
    b.bossKind = run_.complete() ? 0 : (run_.sector() % 3);
    b.bossPattern = 0;
    b.bossVulnTimer = 0.0f;
    // Bosses are the deliberate HP exception: they should live long enough for phases,
    // element combos, EXPOSED windows, and leech sustain to matter. Per-kind trim:
    // Choir is evasive so a touch squishier; Smelter is a bruiser so a touch tankier.
    const float kindHp = b.bossKind == 1 ? 1.12f : (b.bossKind == 2 ? 0.88f : 1.0f);
    const float hpScale = (6.5f + 2.0f * static_cast<float>(run_.complete() ? 0 : run_.sector())) * kindHp;
    b.health = tunables_.enemyMaxHealth * hpScale;
    b.maxHealth = b.health;
    b.bobPhase = rng_.range(0.0f, TwoPi);
    b.bossAttackTimer = 2.2f;
    enemies_.push_back(b);
}

// Summon a small pack of adds at the boss, respecting the crowd cap (the --balance-sim signal:
// endless adds made the boss room unkillable). Returns how many were queued.
int PulseGame::summonBossAdds(Enemy& e, int count, bool preferStalker) {
    int made = 0;
    for (int k = 0; k < count; ++k) {
        if (static_cast<int>(enemies_.size() + pendingEnemies_.size()) >= 8) break;
        Enemy add;
        add.kind = (preferStalker ? run_.rng().unit() < 0.6f : run_.rng().unit() < 0.45f)
                       ? EnemyKind::Stalker : EnemyKind::Rusher;
        add.health = tunables_.enemyMaxHealth * (add.kind == EnemyKind::Stalker ? 0.9f : 1.0f);
        add.maxHealth = add.health;
        add.bobPhase = rng_.range(0.0f, TwoPi);
        const float addRadius = enemyCollisionRadius(add);
        bool placed = false;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const float dist = rng_.range(1.2f, 2.8f);
            const Vec2 cand = e.pos + fromAngle(rng_.range(-Pi, Pi)) * dist;
            if (collides(cand, addRadius)) continue;
            add.pos = cand;
            placed = true;
            break;
        }
        if (!placed) continue;
        const uint32_t visualSalt = static_cast<uint32_t>(made * 0x9e3779b9u)
                                  ^ static_cast<uint32_t>(std::lround(add.pos.x * 113.0f))
                                  ^ static_cast<uint32_t>(std::lround(add.pos.y * 157.0f));
        add.visual = chooseEnemyVisual(add.kind, add.affix, visualSalt);
        pendingEnemies_.push_back(add);
        ++made;
    }
    return made;
}

void PulseGame::updateBoss(Enemy& e, AudioSystem& audio, float dt, Vec2 dir, float dist, bool hasLos) {
    // Phase by remaining health: angrier the more it is whittled down.
    const float hpFrac = e.maxHealth > 0.0f ? e.health / e.maxHealth : 0.0f;
    e.bossPhase = hpFrac < 0.34f ? 2 : (hpFrac < 0.67f ? 1 : 0);
    e.bossVulnTimer = std::max(0.0f, e.bossVulnTimer - dt);   // M5: weak-point window decays

    // Movement per boss kind: the Warden is a slow zoner, the Smelter a relentless bruiser, the
    // Choir keeps its distance and blinks. All still pressure the player to keep moving.
    const Vec2 sep = separationForce(e);
    float spd = 1.7f + 0.5f * static_cast<float>(e.bossPhase);
    Vec2 want = dir;
    if (e.bossKind == 1) spd = 2.4f + 0.7f * static_cast<float>(e.bossPhase);          // Smelter: charges in
    else if (e.bossKind == 2) {                                                         // Choir: hold a mid range
        const float band = 7.0f;
        if (dist < band - 1.0f) want = dir * -1.0f;        // too close -> back off
        else if (dist > band + 1.5f) want = dir;           // too far -> close a little
        else want = rightFromForward(dir);                 // in band -> strafe
        spd = 2.0f + 0.4f * static_cast<float>(e.bossPhase);
    }
    e.vel = approach(e.vel, want * spd + sep, 6.0f * dt);

    e.bossAttackTimer -= dt;
    e.telegraphRemaining = std::max(0.0f, e.telegraphRemaining - dt);
    if (e.telegraphRemaining > 0.0f) return;   // mid wind-up (the flare reads as the tell)

    const Vec3f bossCol = enemyShotColor(e.kind, EnemyAttack::Orb, true, e.bossKind);
    const int   bossShape = enemyShotShape(e.kind, EnemyAttack::Orb, true);
    const int   dmg = static_cast<int>(std::lround(tunables_.enemyRangedDamage));

    if (e.struck) {
        e.struck = false;
        const Vec2 novaAt = e.pos;   // capture the fire position before CHOIR's post-fire blink moves e.pos
        const float aimAng = std::atan2(dir.y, dir.x);
        switch (e.bossKind) {
            case 1: {  // SMELTER (Pyro): alternate a tight aimed flame-lance volley and a close nova ring.
                if ((e.bossPattern & 1) == 0) {
                    const int lance = 5 + e.bossPhase;     // aimed fan the player sidesteps
                    for (int i = 0; i < lance; ++i) {
                        const float a = aimAng + (static_cast<float>(i) - (lance - 1) * 0.5f) * 0.12f;
                        spawnProjectile(e.pos, 0.55f, fromAngle(a), dmg, bossCol, bossShape, e.kind, e.boss,
                                        EnemyAttack::Fan, e.bossKind);
                    }
                } else {
                    const int ring = 10 + 3 * e.bossPhase; // expanding nova - dash OUT through a gap
                    const float spin = e.bobPhase + 0.2f;
                    for (int i = 0; i < ring; ++i)
                        spawnProjectile(e.pos, 0.55f, fromAngle(spin + TwoPi * i / ring), dmg, bossCol, bossShape,
                                        e.kind, e.boss, EnemyAttack::Fan, e.bossKind);
                    if (e.bossPhase >= 1) summonBossAdds(e, 1, false);
                }
                break;
            }
            case 2: {  // CHOIR (Cryo): a sweeping SPIRAL (rotating arms) + a blink, summons stalkers.
                const int arms = 3;
                const int perArm = 3 + e.bossPhase;
                for (int arm = 0; arm < arms; ++arm)
                    for (int i = 0; i < perArm; ++i) {
                        const float a = e.bobPhase + arm * (TwoPi / arms) + i * 0.42f;
                        spawnProjectile(e.pos, 0.55f, fromAngle(a), dmg, bossCol, bossShape, e.kind, e.boss,
                                        EnemyAttack::Burst, e.bossKind);
                    }
                e.bobPhase += 0.55f;                       // advance the spiral so the next sweep rotates
                if (e.bossPhase >= 1) summonBossAdds(e, e.bossPhase, true);
                // Blink to a fresh angle around the player (keeps the fight kinetic).
                for (int attempt = 0; attempt < 24; ++attempt) {
                    const Vec2 cand = player_.pos + fromAngle(rng_.range(-Pi, Pi)) * 7.5f;
                    if (!collides(cand, enemyCollisionRadius(e)) && lineOfSight(cand, player_.pos)) { e.pos = cand; e.vel = {}; break; }
                }
                break;
            }
            default: {  // WARDEN (Volt): the radial orb ring with a gap + a final-phase summon.
                const int orbs = 8 + 3 * e.bossPhase;
                const float spin = e.bobPhase;
                for (int i = 0; i < orbs; ++i)
                    spawnProjectile(e.pos, 0.55f, fromAngle(spin + TwoPi * i / orbs), dmg, bossCol, bossShape,
                                    e.kind, e.boss, EnemyAttack::Orb, e.bossKind);
                if (e.bossPhase >= 2) summonBossAdds(e, 1, false);
                break;
            }
        }
        // Crucible wave: an expanding ground shockwave under the burst (drawn in buildFrame). VFX only
        // (no rng, no hitbox) - the dodgeable threat remains the orbs; the wave sells the discharge.
        novaWaves_.push_back({ { novaAt.x, roomFloorY_ + 0.05f, novaAt.y }, 0.0f, 0.6f, bossCol });
        if (novaWaves_.size() > 8) novaWaves_.erase(novaWaves_.begin());
        addShake(0.8f);
        playEnemyAudio(audio, e, EnemyEventType::BossBurst, 1.05f);
        ++e.bossPattern;
        // M5 WEAK POINT: after committing to an attack the boss is briefly EXPOSED - punish the
        // recovery for the EXPOSED multiplier (rewards reading the telegraph, not just chipping it down).
        e.bossVulnTimer = 1.1f - 0.2f * static_cast<float>(e.bossPhase);
        e.bossAttackTimer = (e.bossKind == 1 ? 2.0f : e.bossKind == 2 ? 2.3f : 2.6f) - 0.6f * static_cast<float>(e.bossPhase);
        return;
    }

    if (e.bossAttackTimer <= 0.0f && hasLos) {
        e.struck = true;                                   // begin the telegraph
        e.telegraphRemaining = 0.75f - 0.15f * static_cast<float>(e.bossPhase);
        playEnemyAudio(audio, e, EnemyEventType::Telegraph, 1.0f);
    }
}

// Damage every active enemy (except ignoreIndex) inside radius, resolving any kills
// through onEnemyKilled. Kills go inactive immediately, so an explode-on-kill chain
// terminates when no new enemy dies (bounded by the small live enemy count).
void PulseGame::applyAreaDamage(AudioSystem& audio, Vec2 center, float radius, float damage, int ignoreIndex,
                                bool carryShotEffects) {
    if (damage <= 0.0f) return;
    const float r2 = radius * radius;
    for (int i = 0; i < static_cast<int>(enemies_.size()); ++i) {
        if (i == ignoreIndex) continue;
        Enemy& e = enemies_[static_cast<size_t>(i)];
        if (!e.active) continue;
        if (lengthSq(e.pos - center) > r2) continue;
        float applied = 0.0f;
        const bool killed = damageEnemy(e, damage, &applied);
        if (applied > 0.0f) {
            const uint32_t col = carryShotEffects ? damageTextColorForMask(activeShotEffectMask()) : DmgTextNeutral;
            spawnCombatText({ e.pos.x, 0.88f, e.pos.y },
                            std::to_string(std::max(1, static_cast<int>(std::lround(applied)))),
                            col, carryShotEffects ? 0.94f : 0.82f);
        }
        e.hurtTimer = std::max(e.hurtTimer, 0.08f);
        e.hitPunch = std::max(e.hitPunch, 0.8f);
        if (killed) {
            e.active = false;
            onEnemyKilled(audio, e, false);
        } else {
            playEnemyAudio(audio, e, EnemyEventType::Hurt, 0.48f);
            if (carryShotEffects) applyShotElements(audio, i);
        }
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
        // Height-aware LoS: the shot leaves the player's actual (possibly elevated) muzzle and
        // travels to the enemy's body centre, so low cover - including the block the player is
        // standing ON - no longer wrongly blocks the hit (the flat 2D lineOfSight bug). Ground enemies
        // sit at ~0.5; flyers hover off the floor, so use the per-enemy aim height for both.
        const float muzzleH = player_.height - 0.11f;
        if (!shotClearTo(player_.pos, muzzleH, enemy.pos, enemyAimY(enemy))) {
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

float PulseGame::enemyCollisionRadius(const Enemy& enemy) const {
    const float fallback = enemy.boss ? 0.85f
                         : (enemy.kind == EnemyKind::Tank ? 0.42f
                         : (enemy.kind == EnemyKind::Stalker ? 0.34f : 0.31f));
    const AnimatedEnemyModel& model = enemyRenderModel(enemy);
    const float visual = model.loaded
        ? model.collisionRadius * (enemy.boss ? BossVisualScale : 1.0f)
        : fallback;
    return std::max(fallback, visual);
}

void PulseGame::updateEnemies(AudioSystem& audio, float dt) {
    for (Enemy& enemy : enemies_) {
        if (!enemy.active) {
            continue;
        }

        enemy.hurtTimer = std::max(0.0f, enemy.hurtTimer - dt);
        enemy.hitPunch = std::max(0.0f, enemy.hitPunch - dt * 5.0f);
        enemy.comboTimer = std::max(0.0f, enemy.comboTimer - dt);
        enemy.comboCooldown = std::max(0.0f, enemy.comboCooldown - dt);
        enemy.blinkFlash = std::max(0.0f, enemy.blinkFlash - dt * 3.0f);
        enemy.recover = std::max(0.0f, enemy.recover - dt);
        enemy.attackCooldown -= dt;
        if (enemy.beamFireTimer > 0.0f) enemy.beamFireTimer -= dt;   // bright beam render countdown
        // Burst stream: keep firing the remaining quick orbs along the locked aim, independent of
        // the enemy's state machine (so the stream completes even as it recovers/repositions).
        if (enemy.burstShotsLeft > 0) {
            enemy.burstTimer -= dt;
            if (enemy.burstTimer <= 0.0f) {
                spawnEnemyShot(enemy, enemy.pos + enemy.attackAim * 8.0f);
                playEnemyAudio(audio, enemy, EnemyEventType::Shot, 0.58f);
                if (--enemy.burstShotsLeft > 0) enemy.burstTimer = 0.11f;
            }
        }

        // Two decoupled per-entity clocks (both advance every frame, before any early-out, with a
        // per-enemy bobPhase so the swarm stays out of lockstep):
        //  - animTime: a STEADY clock for the idle loop + breathing (fixed rate).
        //  - walkPhase: driven by DISTANCE travelled, so the stride is locked to the ground and the
        //    feet do not slide/skate regardless of speed (the old time-based rate was clamped, which
        //    forced the feet to outrun/lag the body at the speed extremes). 3.2 = world units the
        //    body covers per clip-second of the walk window (tuned so the cadence reads natural).
        enemy.animTime += dt;
        enemy.walkPhase += length(enemy.vel) * dt / 3.2f;

        // M2 Cryo: a FROZEN enemy is hard-CC'd - no movement, no attacks (it still animates,
        // and its status ticks/decays in updateStatuses). A frozen foe is also brittle (shatter
        // bonus in damageEnemy). The non-frozen chill slow folds into speedMul below.
        if (enemy.status.frozen > 0.0f) {
            enemy.vel = {};
            enemy.telegraphRemaining = 0.0f;   // cancel any wind-up
            enemy.pendingRanged = false;
            enemy.burstShotsLeft = 0;
            continue;
        }

        // Elite affixes: Regen heals once disengaged; Fast moves quicker.
        enemy.regenCooldown = std::max(0.0f, enemy.regenCooldown - dt);
        if (enemy.affix == EliteAffix::Regen && enemy.regenCooldown <= 0.0f && enemy.health < enemy.maxHealth) {
            enemy.health = std::min(enemy.maxHealth, enemy.health + enemy.maxHealth * 0.12f * dt);
        }
        float speedMul = (enemy.affix == EliteAffix::Fast) ? 1.45f : 1.0f;
        if (enemy.status.chill > 0.0f) speedMul *= std::max(0.15f, 1.0f - enemy.status.chill * stat::kChillMaxSlow);  // M2 Cryo slow

        const Vec2 toPlayer = player_.pos - enemy.pos;
        const float dist = std::max(0.001f, length(toPlayer));
        const Vec2 dir = toPlayer / dist;
        const bool hasLos = lineOfSight(enemy.pos, player_.pos);
        const Vec2 sep = separationForce(enemy);
        // Boss body radius: full-size in the open outdoor arena; shrunk only in the dungeon so
        // the Warden still fits the corridor channel (~0.62) instead of jamming in a doorway.
        const float radius = enemyCollisionRadius(enemy);
        const float meleeRange = std::max(0.4f, tunables_.enemyMeleeRange);

        // Phase D boss: its own telegraphed pattern (slam + summon), not the basic FSM.
        if (enemy.boss) {
            updateBoss(enemy, audio, dt, dir, dist, hasLos);
            moveWithCollision(enemy.pos, enemy.vel, radius, dt);
            continue;
        }

        // Post-attack recovery: a vulnerable pause, the player's counter window.
        if (enemy.recover > 0.0f) {
            enemy.vel = approach(enemy.vel, sep, 9.0f * dt);
            moveWithCollision(enemy.pos, enemy.vel, radius, dt);
            continue;
        }

        // Rusher/Stalker committed burst: a locked, dodgeable dash that strikes on contact.
        if (enemy.lungeTime > 0.0f) {
            enemy.lungeTime -= dt;
            // Weak homing only, so a sideways dash still slips the lunge.
            const bool stalker = enemy.kind == EnemyKind::Stalker;
            const float burstSpeed = stalker ? tunables_.enemyStalkerPounceSpeed : tunables_.enemyRusherLungeSpeed;
            enemy.vel = approach(enemy.vel, dir * burstSpeed + sep, (stalker ? 9.0f : 6.0f) * dt);
            if (!enemy.struck && dist <= meleeRange) {
                enemy.struck = true;
                enemy.lungeTime = 0.0f;
                enemy.attackCooldown = stalker ? 0.85f : 0.25f;
                enemy.recover = std::max(0.2f, tunables_.enemyMeleeRecover * (stalker ? 0.75f : 1.0f));
                enemy.vel = dir * -2.0f; // shove back on impact for readability
                playEnemyAudio(audio, enemy, EnemyEventType::MeleeHit, stalker ? 0.86f : 0.78f);
                damagePlayer(audio, tunables_.enemyMeleeDamage, enemy.pos);
            } else if (enemy.lungeTime <= 0.0f) {
                enemy.attackCooldown = stalker ? 0.65f : 0.15f;
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
            if (enemy.pendingRanged && enemy.telegraphRemaining > 0.0f) {
                const float telDur = std::max(0.05f, tunables_.enemyRangedTelegraph *
                                                     (enemy.pendingAttack == EnemyAttack::Beam ? 1.25f : 1.0f));
                const float prog = clamp(1.0f - enemy.telegraphRemaining / telDur, 0.0f, 1.0f);
                if (enemy.pendingAttack == EnemyAttack::Beam) {
                    // Lock-on beam tell: a thin targeting line (drawn in buildFrame from beamDir)
                    // TRACKS the player for the first ~55% of the wind-up, then LOCKS - freezing
                    // the line so the player has the rest of the wind-up to step off it (the dodge).
                    if (prog < 0.55f) { enemy.beamDir = normalize(player_.pos - enemy.pos); enemy.beamLocked = false; }
                    else { enemy.beamLocked = true; }
                } else {
                    // The forming charge orb drawn in buildFrame carries the wind-up read. Moving
                    // magenta motes here stack into a dotted pre-shot tracer at capture distance.
                }
            }
            if (enemy.telegraphRemaining <= 0.0f) {
                enemy.telegraphRemaining = 0.0f;
                if (enemy.pendingRanged) {
                    // Every kind can open fire: a slow, dodgeable energy bolt with a
                    // bright muzzle bloom (spawnEnemyShot). Melee kinds wind down to a
                    // longer cooldown so shooting stays their fallback, not their main.
                    enemy.pendingRanged = false;
                    if (hasLos) {
                        // Aim at the player with a SMALL, CLAMPED predictive lead so the shot
                        // always reads as aimed AT the player (an unbounded lead throws it wide).
                        const float travel = dist / std::max(1.0f, tunables_.enemyProjectileSpeed);
                        Vec2 lead = player_.vel * (std::min(travel, 0.7f) * 0.5f);
                        const float maxOff = dist * 0.22f;            // <= ~12 deg off the true bearing
                        if (lengthSq(lead) > maxOff * maxOff) lead = normalize(lead) * maxOff;
                        const Vec2 aim = player_.pos + lead;
                        const Vec2 aimDir = normalize(aim - enemy.pos);
                        // Execute the chosen ARCHETYPE (Returnal-style variety; see beginShot).
                        switch (enemy.pendingAttack) {
                            case EnemyAttack::Orb:
                                spawnEnemyShot(enemy, aim);
                                break;
                            case EnemyAttack::Fan: {
                                // A wide fan of orbs to ZONE the player - dodge through a gap.
                                const int n = 5; const float spread = 0.20f;   // ~11 deg between orbs
                                for (int i = 0; i < n; ++i) {
                                    const float off = (static_cast<float>(i) - (n - 1) * 0.5f) * spread;
                                    const float ca = std::cos(off), sa = std::sin(off);
                                    const Vec2 d2{ aimDir.x * ca - aimDir.y * sa, aimDir.x * sa + aimDir.y * ca };
                                    spawnEnemyShot(enemy, enemy.pos + d2 * 8.0f);
                                }
                                break;
                            }
                            case EnemyAttack::Burst:
                                // A fast stream: one now, the rest fired on a timer (burst block).
                                enemy.attackAim = aimDir;
                                spawnEnemyShot(enemy, aim);
                                enemy.burstShotsLeft = 2;
                                enemy.burstTimer = 0.11f;
                                break;
                            case EnemyAttack::Beam: {
                                // Lock-on laser: fire along the LOCKED direction (frozen during the
                                // wind-up, so the player had time to step off). Hitscan to the wall,
                                // one-shot the line, arm the bright beam render (anchored to the LIVE
                                // enemy each frame -> never floats in thin air as the enemy moves).
                                if (lengthSq(enemy.beamDir) < 1e-4f) enemy.beamDir = aimDir;
                                const Vec2 bd = normalize(enemy.beamDir);
                                const float range = std::max(6.0f, tunables_.enemyShootRange * 1.4f);
                                const RayHit wall = castRay(enemy.pos, std::atan2(bd.y, bd.x), range);
                                enemy.beamLen = std::min(range, wall.distance);
                                enemy.beamDir = bd;
                                enemy.beamFireTimer = 0.22f;
                                const Vec2 end2 = enemy.pos + bd * enemy.beamLen;
                                const Vec2 ab = end2 - enemy.pos;
                                const float abLen2 = std::max(1e-4f, dot(ab, ab));
                                const float tt = clamp(dot(player_.pos - enemy.pos, ab) / abLen2, 0.0f, 1.0f);
                                const Vec2 closest = enemy.pos + ab * tt;
                                const float beamR = 0.5f + tunables_.playerRadius;
                                if (restartTimer_ <= 0.0f && lengthSq(player_.pos - closest) <= beamR * beamR)
                                    damagePlayer(audio, tunables_.enemyRangedDamage, enemy.pos);
                                const float mh = enemy.boss ? 0.95f : 0.68f;
                                spawnBlast({ end2.x, mh, end2.y },
                                           enemyShotColor(enemy.kind, EnemyAttack::Beam, enemy.boss, enemy.bossKind), 1.1f);
                                addShake(0.45f);
                                break;
                            }
                        }
                        playEnemyAudio(audio, enemy,
                                       enemy.pendingAttack == EnemyAttack::Beam ? EnemyEventType::Beam : EnemyEventType::Shot,
                                       enemy.kind == EnemyKind::Tank ? 0.82f : 0.68f);
                    }
                    const float cdMul = enemy.kind == EnemyKind::Ranged ? 1.0f : 1.4f;
                    enemy.attackCooldown = std::max(0.3f, tunables_.enemyRangedCooldown * cdMul);
                    // Hold the ATTACK pose through the shot + follow-through (struck + recover drive
                    // the strike animation), so the enemy CASTS instead of snapping back to its walk
                    // the instant the orb/beam leaves. Beam + burst linger a touch longer (still firing).
                    enemy.struck = true;
                    enemy.recover = (enemy.pendingAttack == EnemyAttack::Beam ||
                                     enemy.pendingAttack == EnemyAttack::Burst) ? 0.5f : 0.4f;
                } else if (enemy.kind == EnemyKind::Rusher || enemy.kind == EnemyKind::Stalker) {
                    const bool stalker = enemy.kind == EnemyKind::Stalker;
                    enemy.struck = false;
                    enemy.lungeTime = stalker ? 0.24f : 0.35f;
                    enemy.vel = dir * (stalker ? tunables_.enemyStalkerPounceSpeed
                                               : tunables_.enemyRusherLungeSpeed); // launch the burst
                    playEnemyAudio(audio, enemy, EnemyEventType::Lunge, stalker ? 0.82f : 0.76f);
                } else { // Tank slam
                    if (dist <= meleeRange * 1.25f && hasLos) {
                        enemy.vel = dir * (tunables_.enemyMeleeLunge * 5.0f);
                        playEnemyAudio(audio, enemy, EnemyEventType::MeleeHit, 0.98f);
                        damagePlayer(audio, tunables_.enemyTankMeleeDamage, enemy.pos);
                    }
                    enemy.recover = std::max(0.3f, tunables_.enemyMeleeRecover * 1.2f);
                }
            }
            moveWithCollision(enemy.pos, enemy.vel, radius, dt);
            continue;
        }

        // Locomotion + decide whether to begin an attack wind-up. Every kind now
        // opens fire when the player is in line of sight but out of its own melee
        // range (kept as the readable, close-up secondary attack).
        const float shootRange = std::max(2.0f, tunables_.enemyShootRange);
        const auto beginShot = [&]() {
            enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyRangedTelegraph);
            enemy.pendingRanged = true;
            // Pick a ranged ARCHETYPE by kind so the threats read differently (Returnal-style
            // variety) instead of every enemy firing the same straight orb:
            //   Ranged (cultist) -> aimed orb, sometimes a telegraphed lock-on BEAM
            //   Tank             -> a wide FAN of orbs to zone the player
            //   Stalker          -> a fast BURST stream
            //   Rusher/other     -> a single aimed orb (ranged is only its fallback)
            const float roll = rng_.unit();
            switch (enemy.kind) {
                case EnemyKind::Ranged:  enemy.pendingAttack = roll < 0.40f ? EnemyAttack::Beam : EnemyAttack::Orb; break;
                case EnemyKind::Tank:    enemy.pendingAttack = EnemyAttack::Fan; break;
                case EnemyKind::Stalker: enemy.pendingAttack = EnemyAttack::Burst; break;
                default:                 enemy.pendingAttack = EnemyAttack::Orb; break;
            }
            // Beams want a slightly longer wind-up (the lock-on is the dodge window).
            if (enemy.pendingAttack == EnemyAttack::Beam)
                enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyRangedTelegraph * 1.25f);
            enemy.beamLocked = false;
            playEnemyAudio(audio, enemy, EnemyEventType::Telegraph,
                           enemy.kind == EnemyKind::Ranged ? 0.78f : 0.62f);
        };
        if (enemy.kind == EnemyKind::Ranged) {
            // Kite to a stand-off band and strafe within it (stable per-enemy side).
            // With no line of sight it closes in to regain it, so it can never get
            // stranded behind cover and stall a room from ever clearing.
            const float nearBand = 6.0f;
            const float farBand = 9.0f;
            Vec2 desired{};
            if (!hasLos) {
                desired = dir; // advance to re-acquire the player
            } else if (dist < nearBand) {
                desired = dir * -1.0f;
            } else if (dist > farBand) {
                desired = dir;
            } else {
                const float side = std::sin(enemy.bobPhase) >= 0.0f ? 1.0f : -1.0f;
                desired = rightFromForward(dir) * side;
            }
            enemy.vel = approach(enemy.vel, normalize(desired) * (tunables_.enemyRangedSpeed * speedMul) + sep, 10.0f * dt);
            if (enemy.attackCooldown <= 0.0f && hasLos) {
                beginShot(); // the cultist is a pure shooter
            }
        } else if (enemy.kind == EnemyKind::Stalker) {
            const Vec2 sideDir = rightFromForward(dir);
            const float side = std::sin(enemy.animTime * 2.1f + enemy.bobPhase) >= 0.0f ? 1.0f : -1.0f;
            const Vec2 advance = dist > 2.35f ? dir * 0.86f : dir * -0.28f;
            const Vec2 desired = normalize(advance + sideDir * (side * 0.58f));
            enemy.vel = approach(enemy.vel, desired * (tunables_.enemyStalkerSpeed * speedMul) + sep, 16.0f * dt);
            if (enemy.attackCooldown <= 0.0f && hasLos) {
                if (dist <= tunables_.enemyStalkerPounceRange) {
                    enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyMeleeTelegraph * 0.78f); // pounce
                    playEnemyAudio(audio, enemy, EnemyEventType::Telegraph, 0.72f);
                } else if (dist <= shootRange) {
                    beginShot(); // spit a bolt while closing
                }
            }
        } else if (enemy.kind == EnemyKind::Rusher) {
            enemy.vel = approach(enemy.vel, dir * (tunables_.enemyRusherSpeed * speedMul) + sep, 18.0f * dt);
            if (enemy.attackCooldown <= 0.0f && hasLos) {
                if (dist <= tunables_.enemyRusherLungeRange) {
                    enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyMeleeTelegraph); // wind up the lunge
                    playEnemyAudio(audio, enemy, EnemyEventType::Telegraph, 0.70f);
                } else if (dist <= shootRange) {
                    beginShot(); // fire on the approach
                }
            }
        } else { // Tank: slow relentless advance, prioritisation threat.
            enemy.vel = approach(enemy.vel, dir * (tunables_.enemyTankSpeed * speedMul) + sep, 7.0f * dt);
            if (enemy.attackCooldown <= 0.0f && hasLos) {
                if (dist <= meleeRange) {
                    enemy.telegraphRemaining = std::max(0.1f, tunables_.enemyMeleeTelegraph * 1.3f); // heavier slam tell
                    playEnemyAudio(audio, enemy, EnemyEventType::Telegraph, 0.90f);
                } else if (dist <= shootRange) {
                    beginShot(); // lob a heavy bolt at range
                }
            }
        }
        // Stuck recovery (walled arenas only): the straight-line approach can wedge an enemy
        // against cover with the player behind it and no line of sight, stalling the room. If it
        // wants to reach the player but is not advancing, steer tangentially to round the obstacle;
        // if truly stuck, blink to open floor near the player. Gated on a real loaded environment so
        // the headless balance-sim (box-arena fallback) keeps its clean, unchanged signal.
        if (procEnvReady()) {
            const float moved = length(enemy.pos - enemy.prevPos);
            const bool tryingToReach = !hasLos && dist > meleeRange + 0.6f;
            if (tryingToReach && moved < 0.045f) enemy.stuckTimer += dt;
            else enemy.stuckTimer = std::max(0.0f, enemy.stuckTimer - dt * 2.5f);
            if (enemy.stuckTimer > 0.5f) {
                const float sign = std::fmod(enemy.stuckTimer, 2.0f) < 1.0f ? 1.0f : -1.0f;  // try both ways
                const Vec2 tangent = { -dir.y * sign, dir.x * sign };
                const Vec2 steer = normalize(dir * 0.35f + tangent);
                const float spd = std::max(length(enemy.vel), tunables_.enemyRusherSpeed * 0.7f);
                enemy.vel = steer * spd + sep;
            }
            if (enemy.stuckTimer > 1.25f) {
                Vec2 best{};
                float bestScore = std::numeric_limits<float>::max();
                for (int r = 0; r < 3; ++r) {
                    const float probe = 0.65f + 0.45f * static_cast<float>(r);
                    for (int a = 0; a < 16; ++a) {
                        const float ang = TwoPi * static_cast<float>(a) / 16.0f +
                                          (r & 1 ? Pi / 16.0f : 0.0f);
                        const Vec2 cand = enemy.pos + fromAngle(ang) * probe;
                        if (collides(cand, radius)) continue;
                        const float distScore = lengthSq(cand - player_.pos);
                        const float losBonus = lineOfSight(cand, player_.pos) ? 18.0f : 0.0f;
                        const float advance = dot(normalize(cand - enemy.pos), dir) * 2.0f;
                        const float score = distScore - losBonus - advance;
                        if (score < bestScore) {
                            bestScore = score;
                            best = cand;
                        }
                    }
                }
                if (bestScore < std::numeric_limits<float>::max()) {
                    const float spd = std::max(length(enemy.vel), tunables_.enemyRusherSpeed * 0.9f);
                    enemy.vel = normalize(best - enemy.pos) * spd + sep;
                    enemy.stuckTimer = std::min(enemy.stuckTimer, 1.5f);
                }
            }
        }
        moveWithCollision(enemy.pos, enemy.vel, radius, dt);
        enemy.prevPos = enemy.pos;
    }

    // Hard player/camera standoff: no enemy may overlap the player. The camera sits at the player,
    // so an overlapping enemy envelops the view (the point-blank "see-through" clip). After all the
    // movement branches, push each enemy back out to (its body radius + a player/camera margin) and
    // kill the inward velocity so it stops ramming. Skip the push if it would jam into a wall.
    for (Enemy& e : enemies_) {
        if (!e.active) continue;
        const float er = enemyCollisionRadius(e);
        const float standoff = er + 0.6f;   // player body (~0.34) + a camera buffer
        const Vec2 toE = e.pos - player_.pos;
        const float d = length(toE);
        if (d < standoff) {
            const Vec2 n = d > 1e-4f ? Vec2{ toE.x / d, toE.y / d } : fromAngle(player_.yaw + Pi);
            const Vec2 want = player_.pos + n * standoff;
            if (!collides(want, er)) {
                Vec2 correctionVel = (want - e.pos) / std::max(0.001f, dt);
                moveWithCollision(e.pos, correctionVel, er, dt);
            }
            const float inward = e.vel.x * n.x + e.vel.y * n.y;
            if (inward < 0.0f) { e.vel.x -= n.x * inward; e.vel.y -= n.y * inward; }
            if (collides(e.pos, er) && pushOutOfCollision(e.pos, er)) {
                e.vel = {};
            }
        }
    }

    // Flush boss-summoned adds now that iteration over enemies_ is done.
    if (!pendingEnemies_.empty()) {
        for (Enemy& add : pendingEnemies_) enemies_.push_back(add);
        pendingEnemies_.clear();
    }
}

void PulseGame::spawnProjectile(Vec2 from, float fromHeight, Vec2 dir, int damage, Vec3f color, int shape,
                                EnemyKind sourceKind, bool sourceBoss, EnemyAttack sourceAttack, int sourceBossKind) {
    const Vec2 unit = normalize(dir);
    Projectile p;
    p.origin = from;
    p.pos = from + unit * 0.45f; // emerge just ahead of the muzzle
    p.vel = unit * std::max(1.0f, tunables_.enemyProjectileSpeed);
    p.height = clamp(fromHeight, 0.1f, 0.9f);
    p.life = std::max(0.5f, tunables_.enemyProjectileLife);
    p.damage = damage;
    p.color = color;
    p.shape = shape;
    p.sourceKind = sourceKind;
    p.sourceAttack = sourceAttack;
    p.sourceBoss = sourceBoss;
    p.sourceBossKind = sourceBossKind;
    projectiles_.push_back(p);
    if (projectiles_.size() > 64) {
        projectiles_.erase(projectiles_.begin());
    }
    // spawnEnemyShot owns the muzzle VFX. Keeping spawnProjectile data-only prevents fan/burst
    // attacks from double-emitting big round particles that read as dotted tracer beads.
}

const char* PulseGame::enemyAudioBankId(EnemyKind kind, bool boss) const {
    if (boss) return "boss";
    switch (kind) {
        case EnemyKind::Rusher:  return "rusher";
        case EnemyKind::Ranged:  return "ranged";
        case EnemyKind::Tank:    return "tank";
        case EnemyKind::Stalker: return "stalker";
    }
    return "rusher";
}

const char* PulseGame::enemyAudioBankId(const Enemy& e) const {
    return enemyAudioBankId(e.kind, e.boss);
}

void PulseGame::playEnemyAudio(AudioSystem& audio, const Enemy& e, EnemyEventType event, float volume) {
    // v4 (S2): the "combat readability" accessibility option lifts the enemy telegraph/shot cues so
    // incoming threats are easier to parse.
    float vol = volume;
    if (settingsActive_ && settings_.combatReadability &&
        (event == EnemyEventType::Telegraph || event == EnemyEventType::Shot))
        vol *= 1.35f;
    // Place the cue at the enemy's ground position so it pans/attenuates from its
    // direction relative to the player (listener pose is set in the audio update).
    audio.playEnemyEvent(enemyAudioBankId(e), event, vol, enemySoundIndex_++, e.pos.x, e.pos.y);
}

// Player-feedback bus: the crisp, synthetic cues for the player's own actions/state.
// Routed through one free-running counter so every repeat walks the round-robin bank,
// and kept distinct from the enemy/world banks so a confirm never reads as a threat.
void PulseGame::playFeedback(AudioSystem& audio, FeedbackEventType event, float volume) {
    // v4 (S2): "combat readability" lifts the confirm/threat-state cues the player reads in a fight.
    float vol = volume;
    if (settingsActive_ && settings_.combatReadability &&
        (event == FeedbackEventType::Hitmarker || event == FeedbackEventType::HitCrit ||
         event == FeedbackEventType::Kill || event == FeedbackEventType::KillElite ||
         event == FeedbackEventType::UiConfirm || event == FeedbackEventType::LowHealth))
        vol *= 1.35f;
    audio.playFeedback(event, vol, feedbackSoundIndex_++);
}

void PulseGame::setMusicTracePath(const std::string& path) {
    musicTracePath_ = path;
    musicTraceTime_ = 0.0;
    musicTraceInit_ = false;   // header + truncate happen on the first row written in update()
}

// Returnal-style energy palette: colour communicates behaviour, not decoration.
// Magenta = standard aimed orb, orange = heavy zoning fan/nova, violet = rapid
// burst pressure, crimson = lock-on beam. Boss hues carry their moveset element.
Vec3f PulseGame::enemyShotColor(EnemyKind kind, bool boss) const {
    return enemyShotColor(kind, EnemyAttack::Orb, boss, 0);
}

Vec3f PulseGame::enemyShotColor(EnemyKind kind, EnemyAttack attack, bool boss, int bossKind) const {
    if (boss) {
        switch (std::clamp(bossKind, 0, BossKindCount - 1)) {
            case 1:  return { 2.10f, 0.58f, 0.16f }; // SMELTER: pyro lances / nova rings
            case 2:  return { 0.88f, 0.66f, 2.20f }; // CHOIR: cryo-violet spiral pressure
            default: return { 1.34f, 0.40f, 2.25f }; // WARDEN: volt-violet radial control
        }
    }

    switch (attack) {
        case EnemyAttack::Beam:  return { 2.20f, 0.16f, 0.36f }; // lock-on line: leave it now
        case EnemyAttack::Fan:   return { 2.02f, 0.62f, 0.18f }; // zoning spread: dodge through a gap
        case EnemyAttack::Burst: return { 1.16f, 0.38f, 2.12f }; // pressure stream: keep moving
        case EnemyAttack::Orb:
            break;
    }

    switch (kind) {
        case EnemyKind::Tank:    return { 2.02f, 0.62f, 0.18f };
        case EnemyKind::Stalker: return { 1.16f, 0.38f, 2.12f };
        case EnemyKind::Rusher:  return { 1.95f, 0.22f, 0.52f };
        case EnemyKind::Ranged:  return { 1.65f, 0.24f, 0.92f };
    }
    return { 1.65f, 0.24f, 0.92f };
}

// Orb silhouette per threat type (paired with the colour above): aimed orbs are smooth,
// zoning/heavy shots are spiky, and burst pressure is a sharp travel-aligned dart.
int PulseGame::enemyShotShape(EnemyKind kind, bool boss) const {
    return enemyShotShape(kind, EnemyAttack::Orb, boss);
}

int PulseGame::enemyShotShape(EnemyKind kind, EnemyAttack attack, bool boss) const {
    if (boss) return 1;                         // spike
    switch (attack) {
        case EnemyAttack::Fan:   return 1;      // spiky zoning hazard
        case EnemyAttack::Burst: return 2;      // shard stream
        case EnemyAttack::Beam:  return 0;      // unused by projectiles
        case EnemyAttack::Orb:   break;
    }
    switch (kind) {
        case EnemyKind::Ranged:  return 0;      // smooth gem (cultist caster)
        case EnemyKind::Rusher:  return 1;      // spike
        case EnemyKind::Stalker: return 2;      // shard / dart
        case EnemyKind::Tank:    return 1;      // spike
    }
    return 0;
}

// Fire one energy bolt from an enemy toward aimPoint, with a bright muzzle bloom.
void PulseGame::spawnEnemyShot(const Enemy& e, Vec2 aimPoint) {
    const Vec3f col = enemyShotColor(e.kind, e.pendingAttack, e.boss, e.bossKind);
    const Vec2 toAim = aimPoint - e.pos;
    const Vec2 unit = normalize(toAim);
    const float muzzleH = e.boss ? 0.95f : 0.68f;  // cast from the chest/hands
    spawnProjectile(e.pos, muzzleH, toAim, tunables_.enemyRangedDamage, col,
                    enemyShotShape(e.kind, e.pendingAttack, e.boss), e.kind, e.boss,
                    e.pendingAttack, e.bossKind);

    // Muzzle bloom: keep hostile shot VFX local. Moving forward/ring sparks become dotted
    // tracers after a few frames, fighting the continuous projectile streak.
    const Vec3f origin{ e.pos.x + unit.x * 0.45f, muzzleH, e.pos.y + unit.y * 0.45f };
    // Hue-preserving bright core (multiply, not +0.8 which desaturated toward white) + lower emissive
    // and smaller sizes, so the muzzle reads as a CONTAINED magenta flash, not a white bloom blob.
    const Vec3f hot = hotHue(col, 1.35f, 2.3f);
    spawnParticles(origin, { 0.0f, 0.0f, 0.0f }, 1, 0.0f, 0.08f, 0.16f, hot, 3.8f, 0.0f, 4.0f);    // flash core
    spawnParticles(origin, { 0.0f, 0.0f, 0.0f }, 2, 0.0f, 0.10f, 0.070f, col, 1.8f, 0.0f, 5.0f);  // colour halo
    // NOTE: the brighter launch FLARE is drawn deterministically in buildFrame (young-projectile
    // branch), NOT here - spawnEnemyShot runs in the sim path and consumes rng_, so adding particles
    // here would perturb the deterministic bot-test sim. Keep this muzzle footprint byte-stable.
}

// A radial BLAST nova: a white-hot flash, a flat shock ring of streaked embers fired
// outward across the ground, an upward dome of sparks, and lingering smoke. power
// (~0.6 small hit .. 2.0 big kill) scales the count, speed and size.
void PulseGame::spawnBlast(Vec3f at, Vec3f color, float power) {
    const float p = clamp(power, 0.3f, 3.0f);
    const Vec3f hot{ std::min(color.x * 1.35f, 2.4f), std::min(color.y * 1.35f, 2.4f), std::min(color.z * 1.35f, 2.4f) };
    // Flash core - contained (lower emissive + smaller) so the impact is a brief punch, not a white blob.
    spawnParticles(at, { 0.0f, 0.0f, 0.0f }, 2, 0.0f, 0.10f, 0.16f * p + 0.10f, hot, 4.5f, 0.0f, 5.0f);
    // Flat shock ring: streaked embers blown radially across the floor plane.
    const int ring = static_cast<int>(20.0f * p) + 12;
    for (int i = 0; i < ring; ++i) {
        const float a = TwoPi * static_cast<float>(i) / static_cast<float>(ring) + rng_.range(-0.12f, 0.12f);
        const Vec3f d{ std::cos(a), 0.06f, std::sin(a) };
        const float sp = (6.0f + 4.0f * p) * rng_.range(0.8f, 1.2f);
        spawnParticles(at, { d.x * sp, d.y * sp, d.z * sp }, 1, 0.5f, 0.34f, 0.045f * p + 0.03f,
                       (i & 1) ? color : hot, 4.0f, 1.0f, 3.0f, 0.20f);
    }
    // Upward dome of sparks + slow rising smoke embers.
    spawnParticles(at, { 0.0f, 5.0f * p, 0.0f }, static_cast<int>(24.0f * p) + 12, 3.4f * p, 0.5f, 0.035f,
                   hot, 4.0f, 5.0f, 2.0f, 0.12f);
    spawnParticles(at, { 0.0f, 1.6f, 0.0f }, static_cast<int>(6.0f * p) + 4, 1.2f, 0.8f, 0.10f * p + 0.06f,
                   color, 2.4f, -0.6f, 1.2f);
    // Heat shock: a brief expanding refraction ring that warps the scene at the blast (Returnal hit).
    heatPulses_.push_back({ at, 0.0f, 0.42f, p });
    if (heatPulses_.size() > 48)
        heatPulses_.erase(heatPulses_.begin(), heatPulses_.begin() + static_cast<long long>(heatPulses_.size() - 48));
}

// A mid-air (double) jump kicks a flat ring of dust out + a downward spray of cool energy motes
// from the feet, so the second jump reads as a real push off the air, not a silent teleport.
void PulseGame::spawnAirJumpBurst() {
    const float eyeBase = clamp(tunables_.eyeHeight, 0.12f, 0.88f);
    const Vec3f at{ player_.pos.x, std::max(0.05f, player_.height - eyeBase), player_.pos.y };
    const Vec3f dust{ 0.72f, 0.74f, 0.82f };
    const Vec3f energy{ 0.45f, 1.05f, 1.75f };   // cool cyan kick (matches the player's bolt hue)
    for (int i = 0; i < 14; ++i) {
        const float a = TwoPi * static_cast<float>(i) / 14.0f + rng_.range(-0.22f, 0.22f);
        const Vec3f d{ std::cos(a) * 4.2f, -0.4f, std::sin(a) * 4.2f };
        spawnParticles(at, d, 1, 0.6f, 0.34f, 0.05f, dust, 2.2f, 1.5f, 4.5f, 0.14f);
    }
    spawnParticles(at, { 0.0f, -2.4f, 0.0f }, 8, 1.8f, 0.30f, 0.05f, energy, 5.0f, 0.6f, 3.0f, 0.10f);
}

// A particle BEAM/ray from -> to: a packed line of streaked core motes (elongated along
// the beam axis) wrapped in a soft colour glow, with bright end caps. Re-called each
// frame by a live Beam so it reads as a continuous lance.
void PulseGame::spawnBeam(Vec3f from, Vec3f to, Vec3f color, float coreSize, float spacing) {
    const Vec3f d{ to.x - from.x, to.y - from.y, to.z - from.z };
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-3f) return;
    const Vec3f u{ d.x / len, d.y / len, d.z / len };
    const Vec3f hot = hotHue(color, 1.45f, 2.6f);
    const float step = std::max(0.18f, spacing);
    const int n = std::max(2, static_cast<int>(len / step));
    const Vec3f axis{ u.x * 9.0f, u.y * 9.0f, u.z * 9.0f }; // velocity along the beam -> axial streak
    for (int i = 0; i <= n; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(n);
        const Vec3f pos{ from.x + d.x * f, from.y + d.y * f, from.z + d.z * f };
        spawnParticles(pos, axis, 1, 0.05f, 0.10f, coreSize, hot, 9.0f, 0.0f, 7.0f, 0.30f);        // hot core
        spawnParticles(pos, { 0.0f, 0.0f, 0.0f }, 1, 0.04f, 0.12f, coreSize * 2.4f, color, 4.0f, 0.0f, 5.0f); // glow
    }
    // Bright flash caps at both ends (callers add a spawnBlast nova at the impact if wanted).
    spawnParticles(to,   { 0.0f, 0.0f, 0.0f }, 3, 0.3f, 0.10f, coreSize * 2.6f, hot, 9.0f, 0.0f, 5.0f);
    spawnParticles(from, { 0.0f, 0.0f, 0.0f }, 3, 0.4f, 0.10f, coreSize * 2.0f, hot, 9.0f, 0.0f, 5.0f);
}

// Fire an instant, telegraphed beam from an enemy toward aimPoint. Hitscans walls for
// the endpoint, applies a one-shot line hit-test on the player (dodged during the
// wind-up), and registers a Beam that re-lances its particle ray each frame.
void PulseGame::fireEnemyBeam(const Enemy& e, Vec2 aimPoint, AudioSystem& audio) {
    const Vec2 toAim = aimPoint - e.pos;
    if (lengthSq(toAim) < 1e-4f) return;
    const Vec2 dir = normalize(toAim);
    const float muzzleH = e.boss ? 0.95f : 0.68f;
    const Vec3f col = enemyShotColor(e.kind, EnemyAttack::Beam, e.boss, e.bossKind);
    const float range = std::max(6.0f, tunables_.enemyShootRange * 1.25f);
    const RayHit wall = castRay(e.pos, std::atan2(dir.y, dir.x), range);
    const float dist = std::min(range, wall.distance);
    const Vec2 end2 = e.pos + dir * dist;

    // One-shot damage if the player is on the line when it fires (the telegraph was the
    // window to step off it). Point-to-segment distance in the ground plane.
    const Vec2 ab = end2 - e.pos;
    const float abLen2 = std::max(1e-4f, dot(ab, ab));
    const float t = clamp(dot(player_.pos - e.pos, ab) / abLen2, 0.0f, 1.0f);
    const Vec2 closest = e.pos + ab * t;
    const float beamRadius = 0.55f + tunables_.playerRadius;
    if (restartTimer_ <= 0.0f && lengthSq(player_.pos - closest) <= beamRadius * beamRadius)
        damagePlayer(audio, tunables_.enemyRangedDamage, e.pos);

    Beam b;
    b.from = { e.pos.x, muzzleH, e.pos.y };
    b.to = { end2.x, muzzleH, end2.y };
    b.color = col;
    beams_.push_back(b);
    if (beams_.size() > 16) beams_.erase(beams_.begin());
    spawnBeam(b.from, b.to, col, 0.08f, 0.32f);   // first lance this frame
    spawnBlast(b.to, col, 1.0f);                  // scorch where it lands
    addShake(0.4f);
    playEnemyAudio(audio, e, EnemyEventType::Beam, 0.86f);
}

void PulseGame::updateBeams(float dt) {
    for (Beam& b : beams_) {
        b.age += dt;
        spawnBeam(b.from, b.to, b.color, 0.07f, 0.34f); // re-lance so the ray stays lit for its life
    }
    beams_.erase(std::remove_if(beams_.begin(), beams_.end(),
                                [](const Beam& b) { return b.age >= b.life; }),
                 beams_.end());
}

void PulseGame::updateProjectiles(AudioSystem& audio, float dt) {
    const float pr = std::max(0.05f, tunables_.enemyProjectileRadius);
    const float hitDist = pr + tunables_.playerRadius;
    // Returnal-style impact: a colour-matched blast nova (flash + shock ring + sparks).
    const auto boltImpact = [&](const Projectile& p) {
        spawnBlast({ p.pos.x, p.height, p.pos.y }, p.color, p.splashRadius > 0.0f ? 1.6f : 0.9f);
    };
    for (Projectile& p : projectiles_) {
        if (!p.active) {
            continue;
        }
        p.age += dt;
        p.pos += p.vel * dt;
        // Energy trail (every bolt): a white-hot core streak that inherits the bolt's
        // motion (born moving, high drag halts it within a frame, so it reads as a
        // motion-blurred streak), a wide soft colour halo, and frequent flung embers.
        // The result is a dense glowing comet tail that blooms in the bolt's own hue.
        {
            const Vec3f at{ p.pos.x, p.height, p.pos.y };
            const Vec3f flow{ p.vel.x, 0.0f, p.vel.y };
            const Vec3f hot{ std::min(p.color.x + 0.8f, 2.9f), std::min(p.color.y + 0.8f, 2.9f),
                             std::min(p.color.z + 0.8f, 2.9f) };
            // Enemy orbs get a FATTER, denser Returnal comet: a doubled white-hot core streak, a
            // big soft bloom halo, and an extra inner glow - so the threat reads as a searing
            // energy ball trailing fire. Player railbolts keep the leaner original tail.
            if (p.hostile) {
                // Hostile orb tail is now a CONTINUOUS render-path ribbon drawn in buildFrame (a row of
                // overlapping motes back along -velocity). The old per-sim-frame single mote left gaps
                // at projectile speed -> the "dotted line" tracer. Nothing spawned here, so the trail no
                // longer depends on the sim tick rate and never reads as beads. (at/flow/hot below are
                // used by the player-bolt branch.)
            } else {
                spawnParticles(at, flow * 0.55f, 2, 0.10f, 0.26f, 0.065f, hot, 9.0f, 0.0f, 6.0f, 0.22f);   // streaked core
                spawnParticles(at, { 0.0f, 0.0f, 0.0f }, 1, 0.06f, 0.34f, 0.16f, p.color, 4.0f, 0.0f, 4.0f); // soft halo
                if ((static_cast<int>(p.age * 60.0f) & 1) == 0)                                              // flung embers
                    spawnParticles(at, { 0.0f, 0.5f, 0.0f }, 2, 1.0f, 0.5f, 0.03f, hot, 4.0f, 1.5f, 3.0f, 0.10f);
            }
        }
        if (p.age >= p.life) {
            p.active = false;
            // A player AoE bolt (grenade) detonates at the end of its flight.
            if (!p.hostile && p.splashRadius > 0.0f) {
                spawnBlast({ p.pos.x, p.height, p.pos.y }, p.color, 2.4f); // big detonation nova
                playFeedback(audio, FeedbackEventType::Explosion, 0.9f);
                applyAreaDamage(audio, p.pos, p.splashRadius, static_cast<float>(p.damage) * build_.stats().damageMult, -1, true);
            }
            continue;
        }
        if (collides(p.pos, pr * 0.5f)) { // splash on a wall
            p.active = false;
            spawnImpact(p.pos, p.height, false);
            if (p.hostile) {
                boltImpact(p);
                audio.playEnemyEvent(enemyAudioBankId(p.sourceKind, p.sourceBoss), EnemyEventType::Impact, 0.58f, enemySoundIndex_++);
            } else {
                spawnBlast({ p.pos.x, p.height, p.pos.y }, p.color, p.splashRadius > 0.0f ? 2.4f : 0.7f);
            }
            if (!p.hostile && p.splashRadius > 0.0f) {
                playFeedback(audio, FeedbackEventType::Explosion, 0.85f);
                applyAreaDamage(audio, p.pos, p.splashRadius, static_cast<float>(p.damage), -1, true);
            }
            continue;
        }
        if (p.hostile) {
            // Enemy orb: dodgeable threat that damages the player on contact.
            if (restartTimer_ <= 0.0f && lengthSq(p.pos - player_.pos) <= hitDist * hitDist) {
                p.active = false;
                spawnImpact(p.pos, p.height, true);
                boltImpact(p);
                audio.playEnemyEvent(enemyAudioBankId(p.sourceKind, p.sourceBoss), EnemyEventType::Impact, 0.82f, enemySoundIndex_++);
                damagePlayer(audio, p.damage, p.origin);
            }
        } else {
            // Player bolt (Projectile archetype): the first enemy it touches takes the
            // hit, then it splashes (or single-targets). Direct enemy damage scales
            // with the build's damage mult, matching the hitscan path.
            const float er = pr + 0.34f;
            int hitIdx = -1; float bestSq = er * er;
            for (int i = 0; i < static_cast<int>(enemies_.size()); ++i) {
                const Enemy& e = enemies_[static_cast<size_t>(i)];
                if (!e.active) continue;
                const float d2 = lengthSq(e.pos - p.pos);
                if (d2 < bestSq) { bestSq = d2; hitIdx = i; }
            }
            if (hitIdx >= 0) {
                p.active = false;
                spawnImpact(p.pos, p.height, true);
                const float dmg = static_cast<float>(p.damage) * build_.stats().damageMult;
                if (p.splashRadius > 0.0f) {
                    playFeedback(audio, FeedbackEventType::Explosion, 0.85f);
                    applyAreaDamage(audio, enemies_[static_cast<size_t>(hitIdx)].pos, p.splashRadius, dmg, -1, true);
                } else {
                    Enemy& e = enemies_[static_cast<size_t>(hitIdx)];
                    float appliedDamage = 0.0f, corrodeBonus = 0.0f, shatterBonus = 0.0f;
                    const bool killed = damageEnemy(e, dmg, &appliedDamage, &corrodeBonus, &shatterBonus);
                    if (appliedDamage > 0.0f) {
                        const float baseDmg = std::max(0.0f, appliedDamage - corrodeBonus - shatterBonus);
                        spawnCombatText({ e.pos.x, p.height + 0.36f, e.pos.y },
                                        std::to_string(std::max(1, static_cast<int>(std::lround(baseDmg)))),
                                        damageTextColorForMask(p.effectMask), 1.08f);
                        spawnStatusBonusText(e, corrodeBonus, shatterBonus, p.height);
                    }
                    applyLifeLeech(audio, appliedDamage, { e.pos.x, p.height, e.pos.y });
                    // Player projectiles (railbolt) are heavy single shots: a hard stagger,
                    // a knockback shove, and a satisfying connect freeze.
                    e.hurtTimer = 0.12f; e.hitPunch = 1.5f;
                    e.vel = e.vel + normalize(e.pos - player_.pos) * (tunables_.feelHitKnockback * 1.6f);
                    hitmarkerTimer_ = 0.16f;
                    addShake(0.30f);
                    hitStopTimer_ = std::max(hitStopTimer_, std::min(tunables_.feelHitstopHit * 1.6f, 0.060f));
                    playFeedback(audio, FeedbackEventType::Hitmarker, 0.95f);
                    if (killed) {
                        e.active = false;
                        onEnemyKilled(audio, e, false);
                    } else {
                        playEnemyAudio(audio, e, EnemyEventType::Hurt, 0.56f);
                        applyShotElements(audio, hitIdx);
                    }
                }
            }
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
    // Dash i-frames: a well-timed dash negates the hit entirely. Read it as a clean
    // dodge (a brief cyan phase-flash around the player + a soft cue), no damage taken.
    if (dashInvulnTimer_ > 0.0f && amount > 0) {
        spawnParticles({ player_.pos.x, player_.height, player_.pos.y },
                       { 0.0f, 0.6f, 0.0f }, 10, 3.0f, 0.22f, 0.05f, { 0.55f, 0.95f, 1.5f }, 6.0f, 1.0f, 3.0f, 0.12f);
        addShake(0.18f);
        playFeedback(audio, FeedbackEventType::Dash, 0.5f); // clean dodge whoosh (i-frames)
        return;
    }
    // RunMods: deals/heat scale the damage enemies deal to the player (escalate by
    // damage-to-player, never enemy HP). Default enemyDamageMult is 1.0 (no-op).
    const int incoming = amount > 0
        ? std::max(1, static_cast<int>(std::lround(amount * mods_.enemyDamageMult)))
        : amount;
    // Always log the direction, even on a fully-absorbed hit, so the player can
    // read where pressure is coming from.
    addDamageMarker(source, clamp(static_cast<float>(incoming) / 45.0f, 0.4f, 1.0f));
    // Build mitigation: armour reduces the incoming amount (min 1 so it always stings).
    const float reduced = static_cast<float>(incoming) * (1.0f - build_.stats().damageReduction);
    int remaining = incoming > 0 ? std::max(1, static_cast<int>(std::lround(reduced))) : 0;
    const int shieldBefore = player_.shield;
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
    pulse_.onHit();   // taking a hit costs momentum (knocks the Pulse down + breaks the chain)
    addShake(tunables_.cameraShakeHurt);
    // Player damage bus: shield events are distinct from flesh damage. A drained shield
    // shatters, a partial absorb clinks, and HP damage keeps the organic "hurt" cue.
    const bool shieldHit = shieldBefore > 0 && player_.shield < shieldBefore;
    if (shieldHit && player_.shield == 0) playFeedback(audio, FeedbackEventType::ShieldBreak, 0.95f);
    else if (shieldHit) playFeedback(audio, FeedbackEventType::ShieldAbsorb, 0.85f);
    if (remaining > 0) audio.play(SoundEventType::Hurt, 0.9f);
    else if (!shieldHit) audio.play(SoundEventType::Hurt, 0.55f); // unshielded chip / glance
    // Low-health warning: edge-triggered once as HP first dips into the danger band, so
    // it never loops. Re-arms when HP climbs back out of the band.
    const int maxHpForCue = std::max(1, effectiveMaxHealth());
    const bool lowNow = player_.hp > 0 && player_.hp <= (maxHpForCue * 3) / 10;
    if (lowNow && !lowHealthLatched_) { playFeedback(audio, FeedbackEventType::LowHealth, 0.9f); lowHealthLatched_ = true; }
    else if (!lowNow) lowHealthLatched_ = false;
    if (player_.hp <= 0) {
        // Record what felled us (the Run Ended report): the nearest active enemy to the killing
        // blow - its boss name, or a label for its kind.
        felledBy_.clear();
        float best = 1e9f; const Enemy* killer = nullptr;
        for (const Enemy& e : enemies_) {
            if (!e.active) continue;
            const float d2 = lengthSq(e.pos - source);
            if (d2 < best) { best = d2; killer = &e; }
        }
        if (killer) {
            if (killer->boss) felledBy_ = bossName(killer->bossKind);
            else switch (killer->kind) {
                case EnemyKind::Rusher:  felledBy_ = "a Rusher"; break;
                case EnemyKind::Ranged:  felledBy_ = "a Gunner"; break;
                case EnemyKind::Tank:    felledBy_ = "a Brute"; break;
                case EnemyKind::Stalker: felledBy_ = "a Stalker"; break;
                default: felledBy_ = "the swarm"; break;
            }
        } else {
            felledBy_ = "the arena";
        }
        enterRunOver(false, audio); // death ends the run (RunOver -> dwell -> resetRun)
    }
}

// Wave-based spawning (Phase A): stream the active wave's enemies in at its cadence
// under its concurrency cap; when a wave is fully spawned and cleared, advance to
// the next. roomComplete() observes when the last wave empties.
void PulseGame::updateSpawning(float dt) {
    if (phase_ != RunPhase::InRoom && phase_ != RunPhase::Boss) return;
    if (scriptedDeterministic_) return;   // debug capture: no autonomous spawns
    if (run_.complete()) return;

    const RoomSpec& room = run_.currentRoom();
    if (waveIndex_ >= static_cast<int>(room.waves.size())) return; // all waves dispatched

    const int active = activeEnemyCount();
    if (waveSpawnsLeft_ > 0) {
        waveSpawnTimer_ -= dt;
        if (waveSpawnTimer_ <= 0.0f && active < activeWave_.maxConcurrent) {
            if (spawnEnemy(activeWave_)) {
                --waveSpawnsLeft_;
                // RunMods: cadence only (count was consumed in startWave). A higher
                // enemyCadenceMult shortens the interval -> faster waves. Guaranteed >= 0.05.
                waveSpawnTimer_ = std::max(0.1f, activeWave_.spawnInterval / mods_.enemyCadenceMult);
            } else {
                waveSpawnTimer_ = 0.15f; // placement failed (arena crowded); retry soon
            }
        }
    } else if (active == 0) {
        // Active wave fully spawned and cleared -> advance to the next wave.
        ++waveIndex_;
        if (waveIndex_ < static_cast<int>(room.waves.size())) {
            startWave(waveIndex_);
        }
    }
}

void PulseGame::updatePickups(AudioSystem& audio, float dt) {
    for (Pickup& pickup : pickups_) {
        pickup.age += dt;
    }

    const int maxHealth = std::max(1, effectiveMaxHealth());
    const int maxShield = std::max(0, effectiveMaxShield());
    const int maxReserve = std::max(1, weaponReserveMax());
    const float collectRadiusSq = std::max(0.05f, tunables_.pickupCollectRadius) * std::max(0.05f, tunables_.pickupCollectRadius);

    for (Pickup& pickup : pickups_) {
        if (lengthSq(pickup.pos - player_.pos) > collectRadiusSq) {
            continue;
        }

        bool collected = false;
        if (pickup.kind == PickupKind::Health && player_.hp < maxHealth) {
            player_.hp = std::min(maxHealth, player_.hp + healAmount(std::max(1, tunables_.pickupHealthAmount)));
            damageFlashTimer_ = 0.0f;
            collected = true;
        } else if (pickup.kind == PickupKind::Shield && player_.shield < maxShield) {
            player_.shield = std::min(maxShield, player_.shield + healAmount(std::max(1, tunables_.pickupShieldAmount)));
            shieldFlashTimer_ = 0.28f;
            collected = true;
        } else if (pickup.kind == PickupKind::Ammo && weapon_.reserve < maxReserve) {
            weapon_.reserve = std::min(maxReserve, weapon_.reserve + std::max(1, tunables_.pickupAmmoAmount));
            collected = true;
        }

        if (collected) {
            pickup.age = -1.0f;
            // (Pickups no longer nudge intensity; the Pulse is aggression-driven only.)
            // Distinct per-kind pickup cue so the player hears WHAT they topped up.
            const FeedbackEventType fb =
                pickup.kind == PickupKind::Shield ? FeedbackEventType::PickupShield
              : pickup.kind == PickupKind::Ammo   ? FeedbackEventType::PickupAmmo
                                                  : FeedbackEventType::PickupHealth;
            playFeedback(audio, fb, 0.85f);
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

// Spawn one enemy of the given wave's composition. Placement uses the combat rng_
// (it depends on the live player position, so it is not part of the reproducible
// run structure anyway); the enemy KIND is drawn from the run-RNG so a seed's wave
// character reproduces no matter how the player shoots. Returns false if no valid
// spawn point was found (arena crowded) so the caller can retry.
bool PulseGame::spawnEnemy(const WaveSpec& wave) {
    // The first 32 attempts use the authored ring EXACTLY (byte-identical draws), so open arenas
    // and the headless balance-sim (no env) are unchanged. If all 32 fail - cramped/cluttered
    // curated layouts where the big-arena ring lands in walls - later attempts SAMPLE the real
    // open play-rect (from the loaded env) so a wave always finds floor instead of stalling. The
    // last few attempts also drop the line-of-sight requirement as a final guarantee.
    const bool bounded = procEnvOutdoor();
    const int maxAttempts = procEnvReady() ? 64 : 32;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        Vec2 candidate;
        if (attempt < 32 || !bounded) {
            const float angle = rng_.range(-Pi, Pi);
            const float radius = rng_.range(tunables_.spawnRingRadius * 0.75f, tunables_.spawnRingRadius);
            candidate = player_.pos + fromAngle(angle) * radius;
        } else {
            const float hx = std::max(2.0f, wasteland_.halfExtentX() - 1.2f);
            const float hz = std::max(2.0f, wasteland_.halfExtentZ() - 1.2f);
            candidate = { wasteland_.centerX() + rng_.range(-hx, hx), wasteland_.centerZ() + rng_.range(-hz, hz) };
            if (lengthSq(candidate - player_.pos) < 9.0f) continue;   // keep a minimum standoff
        }
        // Always require LOS on the ring attempts (keeps open arenas + the sim unchanged); only the
        // bounded-sampling tail may drop it, as a last-resort guarantee a wave never stalls.
        const bool needLos = !bounded || attempt < maxAttempts - 6;
        // Validate with the largest regular enemy footprint; the actual kind/visual is chosen just
        // below, and wide bodies must not spawn with their shoulders already inside geometry.
        if (collides(candidate, 0.95f) || (needLos && !lineOfSight(candidate, player_.pos))) {
            continue;
        }

        Enemy enemy;
        const float roll = run_.rng().unit();
        float tankChance = std::clamp(wave.tankChance, 0.0f, 0.75f);
        float rangedChance = std::clamp(wave.rangedChance, 0.0f, 0.75f);
        float stalkerChance = std::clamp(wave.stalkerChance, 0.0f, 0.75f);
        const float totalSpecial = tankChance + rangedChance + stalkerChance;
        if (totalSpecial > 0.92f) {
            const float scale = 0.92f / totalSpecial;
            tankChance *= scale;
            rangedChance *= scale;
            stalkerChance *= scale;
        }
        if (roll < tankChance) {
            enemy.kind = EnemyKind::Tank;
        } else if (roll < tankChance + rangedChance) {
            enemy.kind = EnemyKind::Ranged;
        } else if (roll < tankChance + rangedChance + stalkerChance) {
            enemy.kind = EnemyKind::Stalker;
        } else {
            enemy.kind = EnemyKind::Rusher;
        }
        enemy.pos = candidate;
        const float healthMul = enemy.kind == EnemyKind::Tank ? std::max(1.0f, tunables_.enemyTankHealthMult)
                              : (enemy.kind == EnemyKind::Stalker ? 0.9f : 1.0f);
        enemy.health = tunables_.enemyMaxHealth * healthMul;
        enemy.maxHealth = enemy.health;
        enemy.attackCooldown = rng_.range(0.4f, 1.4f);
        enemy.bobPhase = rng_.range(0.0f, 6.28318f);

        // Elite affixes drip in by sector (run-RNG so a seed is reproducible). They
        // recombine the threat, never inflate base HP (boss rooms run hotter).
        const int sector = run_.complete() ? 0 : run_.sector();
        float eliteChance = 0.05f * static_cast<float>(sector);          // 0 / 0.05 / 0.10
        if (run_.currentIsBoss()) eliteChance += 0.18f;                  // escorts are nastier
        eliteChance += mods_.eliteChanceAdd;                            // RunMods: heat / Elite rooms
        const bool eliteRoom = !run_.complete() && run_.currentType() == RoomType::Elite;
        if (eliteRoom) eliteChance = std::max(eliteChance, 0.90f);       // an Elite room is mostly elites
        eliteChance = std::min(eliteChance, eliteRoom ? 0.95f : 0.85f);  // never fully saturate
        if (run_.rng().unit() < eliteChance) {
            int pick = run_.rng().rangeInt(0, 3);
            if (eliteRoom && run_.rng().unit() >= 0.25f) {
                // Bias away from Shielded (a 2x-TTK sponge - the no-HP-sponge principle)
                // toward the puzzle affixes Fast / Volatile / Regen (~1-in-4 stays Shielded).
                const int nonShield[3] = { 0, 2, 3 }; // Fast, Volatile, Regen offsets
                pick = nonShield[run_.rng().rangeInt(0, 2)];
            }
            enemy.affix = static_cast<EliteAffix>(static_cast<int>(EliteAffix::Fast) + pick);
        }
        uint32_t visualSalt = 0x9e3779b9u;
        visualSalt ^= static_cast<uint32_t>(std::lround(candidate.x * 97.0f)) + 0x85ebca6bu + (visualSalt << 6) + (visualSalt >> 2);
        visualSalt ^= static_cast<uint32_t>(std::lround(candidate.y * 131.0f)) + 0xc2b2ae35u + (visualSalt << 6) + (visualSalt >> 2);
        visualSalt ^= static_cast<uint32_t>(enemies_.size() * 2654435761ull);
        visualSalt ^= static_cast<uint32_t>((run_.complete() ? 0 : run_.sector()) * 0x27d4eb2du);
        enemy.visual = chooseEnemyVisual(enemy.kind, enemy.affix, visualSalt);
        enemies_.push_back(enemy);
        return true;
    }
    return false;
}

void PulseGame::spawnPickup(PickupKind kind) {
    const int rows = static_cast<int>(activeMap().size());
    const int cols = static_cast<int>(activeMap()[0].size());
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
    const float scale = settingsActive_ ? settings_.shakeScale : 1.0f;   // comfort/accessibility slider
    cameraShake_ = std::min(8.0f, cameraShake_ + std::max(0.0f, degrees * scale));
}

void PulseGame::spawnBurst(Vec2 pos, bool headshot) {
    Burst burst;
    burst.pos = pos;
    burst.duration = std::max(0.05f, tunables_.feelKillBurstSeconds);
    burst.headshot = headshot;
    bursts_.push_back(burst);
    // Scorch the floor where the drone died (M2 decals).
    spawnDecal({ pos.x, 0.015f, pos.y }, { 0.0f, 1.0f, 0.0f }, 1u,
               0.30f + rng_.range(0.0f, 0.08f), { 0.012f, 0.010f, 0.009f }, 0.78f);
    // Death blast: a full hot nova (flash + shock ring + sparks + smoke), bigger on a
    // headshot. Replaces the old flat ember puff.
    spawnBlast({ pos.x, 0.55f, pos.y }, { 1.55f, 0.65f, 0.28f }, headshot ? 2.0f : 1.4f);
}

void PulseGame::spawnDecal(Vec3f center, Vec3f normal, uint32_t kind, float size, Vec3f color, float alpha) {
    Decal d;
    d.center = center;
    const float nl = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    const Vec3f n = nl > 1e-4f ? Vec3f{ normal.x / nl, normal.y / nl, normal.z / nl } : Vec3f{ 0, 1, 0 };
    d.normal = n;
    // Tangent: project a reference axis (not parallel to n) onto the decal plane.
    const Vec3f ref = std::fabs(n.y) < 0.9f ? Vec3f{ 0, 1, 0 } : Vec3f{ 1, 0, 0 };
    const float rn = ref.x * n.x + ref.y * n.y + ref.z * n.z;
    Vec3f t = { ref.x - n.x * rn, ref.y - n.y * rn, ref.z - n.z * rn };
    const float tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    d.tangent = tl > 1e-4f ? Vec3f{ t.x / tl, t.y / tl, t.z / tl } : Vec3f{ 1, 0, 0 };
    d.halfWidth = size; d.halfHeight = size; d.halfDepth = 0.12f;
    d.color = color; d.alpha = alpha; d.kind = kind;
    decals_.push_back(d);
    constexpr size_t kMaxDecals = 48;
    if (decals_.size() > kMaxDecals)
        decals_.erase(decals_.begin(), decals_.begin() + static_cast<long long>(decals_.size() - kMaxDecals));
}

// Per-room biome floor markings (the spec's decal kit): a central mark at the room centre (a sigil
// ring around the focal in Reliquary, a hazard/code panel elsewhere) plus edge-weighted scatter that
// keeps the open combat lanes clean. Rebuilt on room generation; appended to frame_.decals every
// frame so it never gets evicted by the combat bullet-mark FIFO. Decal kinds: 3 chevrons, 4 code,
// 5 sigil (see decal.hlsl). Albedo-projected, so they receive the room's lighting/GI/fog.
void PulseGame::buildEnvDecals() {
    envDecals_.clear();
    if (!wasteland_.ready()) return;
    const std::array<std::string, 24>& g = wasteland_.grid();
    const float sx = static_cast<float>(wasteland_.spawnX()) + 0.5f, sz = static_cast<float>(wasteland_.spawnZ()) + 0.5f;
    const float ccx = wasteland_.centerX(), ccz = wasteland_.centerZ();

    // deterministic per room (does not perturb the gameplay rng)
    uint32_t s = static_cast<uint32_t>(run_.seed()) ^ (static_cast<uint32_t>(run_.roomIndex()) * 2654435761u) ^ 0x0DECA1u;
    const auto rnd  = [&]() { s = s * 1664525u + 1013904223u; return s; };
    const auto frnd = [&](float a, float b) { return a + (b - a) * (static_cast<float>(rnd() >> 8) / 16777216.0f); };

    // per-biome decal kinds + etch colours (albedo marks). Foundry: hazard chevrons + panel code;
    // Furnace: hazard chevrons + scorch; Reliquary: etched sigils.
    uint32_t kinds[2] = { 3u, 4u };
    Vec3f colA{ 0.46f, 0.38f, 0.10f }, colB{ 0.10f, 0.42f, 0.50f };
    float alpha = 0.80f; int scatter = 4;
    if (currentBiome_ == Biome::Forest) {            // FURNACE
        kinds[0] = 3u; kinds[1] = 1u; colA = { 0.42f, 0.17f, 0.05f }; colB = { 0.020f, 0.016f, 0.014f };
        alpha = 0.85f; scatter = 5;
    } else if (currentBiome_ == Biome::Ruins) {      // RELIQUARY
        kinds[0] = 5u; kinds[1] = 5u; colA = { 0.34f, 0.46f, 0.50f }; colB = { 0.30f, 0.40f, 0.46f };
        alpha = 0.72f; scatter = 3;
    }
    const auto place = [&](float wx, float wz, uint32_t kind, float half, Vec3f color, float yaw) {
        Decal d;
        d.center = { wx, 0.02f, wz }; d.normal = { 0.0f, 1.0f, 0.0f };
        d.tangent = { std::cos(yaw), 0.0f, std::sin(yaw) };
        d.halfWidth = half; d.halfHeight = half; d.halfDepth = 0.30f;
        d.color = color; d.alpha = alpha; d.kind = kind;
        envDecals_.push_back(d);
    };

    // central mark (a big sigil under the Reliquary altar; a hazard/code panel elsewhere)
    place(ccx, ccz, kinds[0], (currentBiome_ == Biome::Ruins) ? 2.1f : 1.6f, colA, frnd(0.0f, 1.57f));

    // edge-weighted scatter: collect open floor cells away from spawn + centre, prefer wall-adjacent.
    struct Cell { float x, z; };
    std::vector<Cell> edges, opens;
    for (int z = 1; z < 23; ++z)
        for (int x = 1; x < 31; ++x) {
            if (g[static_cast<size_t>(z)][static_cast<size_t>(x)] != '.') continue;
            const float wx = static_cast<float>(x) + 0.5f, wz = static_cast<float>(z) + 0.5f;
            if ((wx - sx) * (wx - sx) + (wz - sz) * (wz - sz) < 12.0f) continue;     // clear of spawn
            if ((wx - ccx) * (wx - ccx) + (wz - ccz) * (wz - ccz) < 6.0f) continue;  // clear of central mark
            const bool nearWall = g[static_cast<size_t>(z)][static_cast<size_t>(x - 1)] == '#' || g[static_cast<size_t>(z)][static_cast<size_t>(x + 1)] == '#'
                               || g[static_cast<size_t>(z - 1)][static_cast<size_t>(x)] == '#' || g[static_cast<size_t>(z + 1)][static_cast<size_t>(x)] == '#';
            (nearWall ? edges : opens).push_back({ wx, wz });
        }
    // deterministic shuffle (Fisher-Yates) of the edge pool, then fall back to open cells.
    for (size_t i = edges.size(); i > 1; --i) { const size_t j = rnd() % i; std::swap(edges[i - 1], edges[j]); }
    for (size_t i = opens.size(); i > 1; --i) { const size_t j = rnd() % i; std::swap(opens[i - 1], opens[j]); }
    for (int k = 0; k < scatter; ++k) {
        const Cell* c = (static_cast<size_t>(k) < edges.size()) ? &edges[static_cast<size_t>(k)]
                      : (static_cast<size_t>(k) - edges.size() < opens.size() ? &opens[static_cast<size_t>(k) - edges.size()] : nullptr);
        if (!c) break;
        const uint32_t kind = kinds[rnd() & 1u];
        place(c->x, c->z, kind, frnd(0.85f, 1.15f), (rnd() & 1u) ? colA : colB, frnd(0.0f, 3.1416f));
    }
}

// Per-biome ambient atmosphere (spec biome.vfx): Furnace rises embers + heat-shimmer, Reliquary
// drifts cold frost motes, Foundry floats cool dust + the odd arc-spark at a wall. Paced by an
// accumulator so the rate is frame-independent; particles are cheap CPU billboards (additive).
void PulseGame::updateAmbientVfx(float dt) {
    if (!wasteland_.ready() || dt <= 0.0f) return;
    const float cx = wasteland_.centerX(), cz = wasteland_.centerZ();
    const float hx = std::max(2.0f, wasteland_.halfExtentX() - 1.0f);
    const float hz = std::max(2.0f, wasteland_.halfExtentZ() - 1.0f);
    const auto rx = [&]() { return cx + rng_.range(-hx, hx); };
    const auto rz = [&]() { return cz + rng_.range(-hz, hz); };

    const float interval = (currentBiome_ == Biome::Forest) ? 0.05f
                         : (currentBiome_ == Biome::Ruins)  ? 0.11f
                         :                                    0.08f;
    ambientSpawnAccum_ += dt;
    int guard = 0;
    while (ambientSpawnAccum_ >= interval && guard++ < 8) {
        ambientSpawnAccum_ -= interval;
        const float px = rx(), pz = rz();
        if (currentBiome_ == Biome::Forest) {            // FURNACE: rising embers + heat shimmer
            const Vec3f ev{ rng_.range(-0.25f, 0.25f), rng_.range(0.9f, 1.9f), rng_.range(-0.25f, 0.25f) };
            spawnParticles({ px, rng_.range(0.1f, 0.7f), pz }, ev, 1, 0.10f, rng_.range(1.6f, 2.8f), 0.030f,
                           { 1.5f, 0.55f, 0.16f }, 3.4f, -0.12f, 1.1f, 0.05f);   // negative gravity = buoyant rise
            if ((rng_.nextU32() % 6u) == 0u) heatPulses_.push_back({ { px, 0.25f, pz }, 0.0f, 0.95f, 0.55f });
        } else if (currentBiome_ == Biome::Ruins) {      // RELIQUARY: drifting cold frost/dust motes
            const Vec3f dv{ rng_.range(-0.12f, 0.12f), rng_.range(-0.04f, 0.10f), rng_.range(-0.12f, 0.12f) };
            spawnParticles({ px, rng_.range(0.5f, 3.4f), pz }, dv, 1, 0.10f, rng_.range(2.4f, 4.4f), 0.024f,
                           { 0.55f, 0.72f, 0.82f }, 1.5f, 0.015f, 0.7f, 0.0f);
        } else {                                         // FOUNDRY: cool dust motes + occasional arc-spark
            const Vec3f dv{ rng_.range(-0.12f, 0.12f), rng_.range(0.0f, 0.22f), rng_.range(-0.12f, 0.12f) };
            spawnParticles({ px, rng_.range(0.4f, 2.6f), pz }, dv, 1, 0.10f, rng_.range(1.8f, 3.2f), 0.020f,
                           { 0.45f, 0.70f, 0.95f }, 1.2f, 0.0f, 0.7f, 0.0f);
            if ((rng_.nextU32() % 40u) == 0u) {             // arc-spark at a side wall
                const Vec3f w{ cx + (rng_.nextU32() & 1u ? hx : -hx) * 0.95f, rng_.range(0.6f, 2.6f), cz + rng_.range(-hz, hz) };
                spawnParticles(w, { 0.0f, 0.0f, 0.0f }, 3, 0.55f, 0.22f, 0.028f, { 0.40f, 1.45f, 1.95f }, 6.5f, 2.0f, 3.0f, 0.16f);
            }
        }
    }
}

void PulseGame::spawnParticles(Vec3f origin, Vec3f baseVel, int count, float spread,
                               float life, float size, Vec3f color, float emissive,
                               float gravity, float drag, float stretch) {
    for (int i = 0; i < count; ++i) {
        WorldParticle p;
        p.pos = origin;
        p.vel = { baseVel.x + rng_.range(-spread, spread),
                  baseVel.y + rng_.range(-spread, spread),
                  baseVel.z + rng_.range(-spread, spread) };
        p.maxLife = life * rng_.range(0.7f, 1.0f);
        p.life = p.maxLife;
        p.size = size * rng_.range(0.7f, 1.3f);
        p.color = color;
        p.emissive = emissive;
        p.gravity = gravity;
        p.drag = drag;
        p.stretch = stretch;
        particles_.push_back(p);
    }
    // Headroom for the dense, streaked energy VFX (oldest motes trim first, so the
    // freshest trail/impact/flash particles always survive).
    constexpr size_t kMaxParticles = 9000;
    if (particles_.size() > kMaxParticles)
        particles_.erase(particles_.begin(),
                         particles_.begin() + static_cast<long long>(particles_.size() - kMaxParticles));
}


const std::array<std::string, 24>& PulseGame::activeMap() const {
    // The brutalist arena's generated grid drives collision + spawn; Arena is the static
    // safety fallback only while the arena is still loading (procEnvReady() == false).
    if (procEnvReady())
        return procEnvGrid();
    return Arena;
}


void PulseGame::buildArenaProps() {
    props_.clear();
    if (wasteland_.ready()) return; // Quaternius rooms dress themselves; legacy props caused glowing blockout panels.
    const std::array<std::string, 24>& map = activeMap();
    const int rows = static_cast<int>(map.size());
    int placed = 0;
    for (int y = 2; y < rows - 2 && placed < 16; ++y) {
        const int cols = static_cast<int>(map[static_cast<size_t>(y)].size());
        for (int x = 2; x < cols - 2 && placed < 16; ++x) {
            const auto at = [&](int xx, int yy) { return map[static_cast<size_t>(yy)][static_cast<size_t>(xx)]; };
            if (at(x, y) != '.') continue;
            const bool nearWall = at(x - 1, y) == '#' || at(x + 1, y) == '#' ||
                                  at(x, y - 1) == '#' || at(x, y + 1) == '#';
            if (!nearWall) continue;
            if (((x * 7 + y * 11) % 17) != 0) continue;         // deterministic sparse scatter
            if (std::abs(x - 16) < 3 && std::abs(y - 12) < 3) continue;   // clear of the spawn
            Prop p;
            p.pos = { static_cast<float>(x) + 0.5f, 0.0f, static_cast<float>(y) + 0.5f };
            p.kind = (x + y) % 4;
            p.yaw = static_cast<float>((x * 3 + y) % 4) * 1.5707963f;
            p.scale = 0.85f + 0.12f * static_cast<float>((x * 5 + y) % 3);
            switch (p.kind) {
                case 0:  p.tint = { 0.85f, 0.80f, 0.72f }; p.emissive = 0.0f;
                         p.radius = 0.42f * p.scale; p.height = 0.90f * p.scale; break;  // crate
                case 1:  p.tint = { 0.78f, 0.82f, 0.90f }; p.emissive = 0.0f;
                         p.radius = 0.38f * p.scale; p.height = 1.15f * p.scale; break;  // barrel
                case 2:  p.tint = { 0.80f, 0.82f, 0.85f }; p.emissive = 0.0f;
                         p.radius = 0.36f * p.scale; p.height = 1.0f; break;             // pillar (full)
                default: p.tint = { 0.45f, 0.95f, 1.25f }; p.emissive = 1.7f;
                         p.radius = 0.34f * p.scale; p.height = 1.0f * p.scale; break;   // glowing panel
            }
            props_.push_back(p);
            ++placed;
        }
    }
}

void PulseGame::updateParticles(float dt) {
    for (WorldParticle& p : particles_) {
        p.life -= dt;
        p.vel.y -= p.gravity * dt;
        const float damp = std::max(0.0f, 1.0f - p.drag * dt);
        p.vel = p.vel * damp;
        p.pos = p.pos + p.vel * dt;
        if (p.pos.y < 0.02f && p.vel.y < 0.0f) {   // floor bounce
            p.pos.y = 0.02f;
            p.vel.y = -p.vel.y * 0.3f;
            p.vel.x *= 0.6f; p.vel.z *= 0.6f;
        }
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const WorldParticle& p) { return p.life <= 0.0f; }),
                     particles_.end());
    // Age the impact heat-shock rings; drop the spent ones.
    for (HeatPulse& h : heatPulses_) h.age += dt;
    heatPulses_.erase(std::remove_if(heatPulses_.begin(), heatPulses_.end(),
                                     [](const HeatPulse& h) { return h.age >= h.life; }),
                      heatPulses_.end());
    // Age the boss nova shockwaves (pure VFX; no rng, no gameplay).
    for (NovaWave& nv : novaWaves_) nv.age += dt;
    novaWaves_.erase(std::remove_if(novaWaves_.begin(), novaWaves_.end(),
                                    [](const NovaWave& nv) { return nv.age >= nv.life; }),
                     novaWaves_.end());
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

void PulseGame::spawnCombatText(Vec3f pos, const std::string& text, uint32_t color, float scale) {
    if (text.empty()) return;
    CombatText ct;
    ct.pos = pos;
    float h = std::sin(pos.x * 12.9898f + pos.z * 78.233f + pos.y * 27.17f) * 43758.5453f;
    h -= std::floor(h);
    const float side = (h - 0.5f) * 0.34f;
    ct.vel = { side, 0.92f + h * 0.18f, -side * 0.45f };
    ct.text = text;
    ct.color = color;
    ct.scale = clamp(scale, 0.65f, 1.45f);
    ct.life = 0.82f;
    combatTexts_.push_back(ct);
    if (combatTexts_.size() > 96)
        combatTexts_.erase(combatTexts_.begin(), combatTexts_.begin() + static_cast<long long>(combatTexts_.size() - 96));
}

void PulseGame::spawnCasing(const WeaponProfile& profile, int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) return;
    if (profile.archetype == WeaponArchetype::Projectile) return;

    const float impact = clamp(profile.impactScale, 0.45f, 1.35f);
    const float autoBias = profile.automatic ? 0.82f : 1.0f;
    ScreenCasing casing;
    casing.x = muzzleFracX_ * static_cast<float>(screenW) + static_cast<float>(screenW) * rng_.range(0.022f, 0.044f);
    casing.y = muzzleFracY_ * static_cast<float>(screenH) - static_cast<float>(screenH) * rng_.range(0.012f, 0.030f);
    casing.vx = static_cast<float>(screenW) * rng_.range(0.12f, 0.20f) * autoBias;
    casing.vy = -static_cast<float>(screenH) * rng_.range(0.18f, 0.31f);
    casing.angle = rng_.range(-0.8f, 0.8f);
    casing.spin = rng_.range(-20.0f, 24.0f);
    casing.life = rng_.range(0.30f, 0.48f) * autoBias;
    casing.size = (0.82f + impact * 0.34f) * clamp(profile.casingScale, 0.25f, 1.6f);
    casings_.push_back(casing);
    constexpr size_t kMaxCasings = 42;
    if (casings_.size() > kMaxCasings) {
        casings_.erase(casings_.begin(),
                       casings_.begin() + static_cast<long long>(casings_.size() - kMaxCasings));
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
    precisionMarkerTimer_ = std::max(0.0f, precisionMarkerTimer_ - dt);
    killConfirmTimer_ = std::max(0.0f, killConfirmTimer_ - dt);
    damageFlashTimer_ = std::max(0.0f, damageFlashTimer_ - dt);
    shieldFlashTimer_ = std::max(0.0f, shieldFlashTimer_ - dt);
    lifeLeechFlashTimer_ = std::max(0.0f, lifeLeechFlashTimer_ - dt);
    for (float& t : elementFeedbackCooldown_) t = std::max(0.0f, t - dt);
    muzzleFlashTimer_ = std::max(0.0f, muzzleFlashTimer_ - dt);
    scrapFlashTimer_ = std::max(0.0f, scrapFlashTimer_ - dt);
    configMessageTimer_ = std::max(0.0f, configMessageTimer_ - dt);
    // M1: the Pulse is the momentum mechanic AND the music driver. It rises from aggression
    // (fed at the kill/dash sites), drains when idle or out of combat, and is knocked down by
    // hits. The techno intensity is the Pulse meter, floored by the run/wave state so combat
    // always has a musical baseline; RunOver lets it decay to silence.
    const bool pulseInCombat = (phase_ == RunPhase::InRoom || phase_ == RunPhase::Boss) && !run_.complete();
    pulse_.update(dt, pulseInCombat);
    const float intensityFloor = (phase_ != RunPhase::RunOver) ? runIntensityFloor() : 0.0f;
    combatIntensity_ = std::max(pulseInCombat ? pulse_.meter01() : 0.0f, intensityFloor);
    // Track the room/run peak Pulse for the greed loot bias (on clear) and the run-over stat,
    // and accumulate the sim-batch Pulse distribution telemetry.
    if (pulseInCombat) {
        roomPulsePeak_ = std::max(roomPulsePeak_, pulse_.meter01());
        runPulsePeak_ = std::max(runPulsePeak_, pulse_.meter01());
        ++pulseSampleFrames_;
        pulseMeterSum_ += static_cast<double>(pulse_.meter01());
        ++pulseTierFrames_[static_cast<size_t>(pulse_.tier())];
    }

    // Spray offset recovers (view eases back to aim) once you stop firing; while
    // the trigger is held it persists so the pattern reads and stays counter-able.
    const WeaponProfile& recoilProfile = activeWeaponProfile();
    const float releaseGap = 1.6f / std::max(0.1f, recoilProfile.fireRate);
    const float resetGap = std::max(releaseGap, std::max(0.05f, recoilProfile.recoilResetSeconds));
    if (weapon_.timeSinceShot > releaseGap) {
        const float settle = clamp(1.0f - std::exp(-std::max(0.0f, recoilProfile.recoilRecoveryRate) * dt), 0.0f, 1.0f);
        recoilOffsetPitch_ -= recoilOffsetPitch_ * settle;
        recoilOffsetYaw_ -= recoilOffsetYaw_ * settle;
        if (std::fabs(recoilOffsetPitch_) < 0.0001f) recoilOffsetPitch_ = 0.0f;
        if (std::fabs(recoilOffsetYaw_) < 0.0001f) recoilOffsetYaw_ = 0.0f;
        if (weapon_.timeSinceShot > resetGap) recoilShotIndex_ = 0;
    }
    const float kickSettle = clamp(1.0f - std::exp(-std::max(0.1f, recoilProfile.viewmodelKickRecovery) * dt), 0.0f, 1.0f);
    weaponKick_ -= weaponKick_ * kickSettle;
    weaponKickSide_ -= weaponKickSide_ * kickSettle;
    if (weaponKick_ < 0.0005f) weaponKick_ = 0.0f;
    if (std::fabs(weaponKickSide_) < 0.0005f) weaponKickSide_ = 0.0f;
    landingKick_ *= std::exp(-16.0f * dt);
    if (landingKick_ < 0.00001f) {
        landingKick_ = 0.0f;
    }

    // Shake runs on real time so the freeze of a hit-stop does not also freeze
    // the shake/feedback. Energy settles exponentially toward zero.
    hitStopTimer_ = std::max(0.0f, hitStopTimer_ - dt);
    overdriveTimer_ = std::max(0.0f, overdriveTimer_ - dt); // ultimate window runs on real time
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

    for (ScreenCasing& casing : casings_) {
        casing.age += dt;
        casing.x += casing.vx * dt;
        casing.y += casing.vy * dt;
        casing.vy += 880.0f * dt;
        casing.vx *= std::exp(-2.2f * dt);
        casing.spin *= std::exp(-0.8f * dt);
        casing.angle += casing.spin * dt;
    }
    casings_.erase(
        std::remove_if(casings_.begin(), casings_.end(), [](const ScreenCasing& casing) {
            return casing.age >= casing.life;
        }),
        casings_.end());

    const float fovRecovery = std::max(0.5f, tunables_.fireImpactRecovery) * dt;
    const float cameraRecovery = std::max(0.5f, recoilProfile.cameraKickRecovery) * dt;
    fireFovKick_ = approach(fireFovKick_, 0.0f, fovRecovery * std::max(1.0f, fireFovKick_));
    fireCameraKick_ = approach(fireCameraKick_, 0.0f, cameraRecovery * std::max(1.0f, fireCameraKick_));

    for (Tracer& tracer : tracers_) {
        tracer.age += dt;
    }
    tracers_.erase(
        std::remove_if(tracers_.begin(), tracers_.end(), [](const Tracer& tracer) {
            return tracer.age >= tracer.duration;
        }),
        tracers_.end());

    for (CombatText& t : combatTexts_) {
        t.age += dt;
        t.pos = t.pos + t.vel * dt;
        t.vel.y -= 0.65f * dt;
    }
    combatTexts_.erase(
        std::remove_if(combatTexts_.begin(), combatTexts_.end(), [](const CombatText& t) {
            return t.age >= t.life;
        }),
        combatTexts_.end());

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
    if (y < 0 || y >= static_cast<int>(activeMap().size())) {
        return true;
    }
    if (x < 0 || x >= static_cast<int>(activeMap()[static_cast<size_t>(y)].size())) {
        return true;
    }
    return activeMap()[static_cast<size_t>(y)][static_cast<size_t>(x)] == '#';
}

bool PulseGame::collides(Vec2 pos, float radius, float feet) const {
    // Cover taller than the player's feet blocks; cover at/below feet is passable (you have
    // jumped over it or stand on it). kStep is the auto-step height: a surface up to kStep above the
    // feet is walkable (so you walk UP a ramp/stairs and over small lips instead of being stopped).
    // It must exceed the per-frame rise of the steepest ramp at move speed (~0.06 m at walkSpeed 7)
    // yet stay below the shortest real cover (crates ~0.6 m) so cover still blocks.
    const float kStep = 0.30f;
    if (procEnvReady()) {
        // Sub-cell procedural collision: test every fine cell the radius-AABB overlaps, so
        // movement lines up with the drawn walls/rocks far closer than the 1-unit grid.
        const int s = procEnvSub();
        const int fx0 = static_cast<int>(std::floor((pos.x - radius) * s));
        const int fx1 = static_cast<int>(std::floor((pos.x + radius) * s));
        const int fz0 = static_cast<int>(std::floor((pos.y - radius) * s));
        const int fz1 = static_cast<int>(std::floor((pos.y + radius) * s));
        for (int fz = fz0; fz <= fz1; ++fz)
            for (int fx = fx0; fx <= fx1; ++fx)
                if (procEnvSolidFine(fx, fz) && procEnvFineHeight(fx, fz) > feet + kStep) return true;
        return false;
    }
    if (isWallCell(static_cast<int>(std::floor(pos.x - radius)), static_cast<int>(std::floor(pos.y - radius))) ||
        isWallCell(static_cast<int>(std::floor(pos.x + radius)), static_cast<int>(std::floor(pos.y - radius))) ||
        isWallCell(static_cast<int>(std::floor(pos.x - radius)), static_cast<int>(std::floor(pos.y + radius))) ||
        isWallCell(static_cast<int>(std::floor(pos.x + radius)), static_cast<int>(std::floor(pos.y + radius))))
        return true;   // box-arena walls are full height
    // M2.5 decorative props: solid circular footprint, but passable once feet clear the top.
    for (const Prop& p : props_) {
        if (p.height <= feet + kStep) continue;
        const float rr = radius + p.radius;
        const Vec2 d{ pos.x - p.pos.x, pos.y - p.pos.z };
        if (d.x * d.x + d.y * d.y < rr * rr) return true;
    }
    return false;
}

// Smooth procedural support under the player centre. Using the footprint maximum makes ramp
// fine-cells read as little stairs; the centre sample keeps the camera planted and continuous.
float PulseGame::smoothProcEnvGroundHeightAt(Vec2 pos, float feet, float landTol) const {
    if (!procEnvReady()) return 0.0f;

    const float s = static_cast<float>(procEnvSub());
    const float u = pos.x * s - 0.5f;
    const float v = pos.y * s - 0.5f;
    const int fx0 = static_cast<int>(std::floor(u));
    const int fz0 = static_cast<int>(std::floor(v));
    const float tx = clamp(u - static_cast<float>(fx0), 0.0f, 1.0f);
    const float tz = clamp(v - static_cast<float>(fz0), 0.0f, 1.0f);

    const auto sample = [&](int fx, int fz) {
        if (!procEnvSolidFine(fx, fz)) return 0.0f;
        const float h = procEnvFineHeight(fx, fz);
        return h <= feet + landTol ? h : 0.0f;
    };
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };

    const float h00 = sample(fx0,     fz0);
    const float h10 = sample(fx0 + 1, fz0);
    const float h01 = sample(fx0,     fz0 + 1);
    const float h11 = sample(fx0 + 1, fz0 + 1);
    return mix(mix(h00, h10, tx), mix(h01, h11, tx), tz);
}

float PulseGame::groundHeightAt(Vec2 pos, float radius, float feet) const {
    // Must roughly match collides()'s kStep so walkable upward surfaces snap cleanly while
    // cover that is too high still blocks instead of becoming an auto-step.
    const float landTol = 0.30f;
    float support = 0.0f;
    if (procEnvReady()) {
        return smoothProcEnvGroundHeightAt(pos, feet, landTol);
    }
    for (const Prop& p : props_) {
        if (p.height > feet + landTol || p.height <= support) continue;
        const float rr = radius + p.radius;
        const Vec2 d{ pos.x - p.pos.x, pos.y - p.pos.z };
        if (d.x * d.x + d.y * d.y < rr * rr) support = p.height;
    }
    return support;
}

float PulseGame::propRayHit(Vec2 origin, Vec2 dir, float maxDist, float originHeight,
                            float verticalSlope, int& outProp) const {
    float best = maxDist;
    outProp = -1;
    for (size_t pi = 0; pi < props_.size(); ++pi) {
        const Prop& p = props_[pi];
        const Vec2 pc{ p.pos.x, p.pos.z };
        const Vec2 toC = pc - origin;
        const float along = dot(toC, dir);          // dir is a unit vector
        if (along < 0.0f) continue;
        const Vec2 closest = origin + dir * along;
        const Vec2 perp = pc - closest;
        const float perp2 = perp.x * perp.x + perp.y * perp.y;
        if (perp2 > p.radius * p.radius) continue;
        const float entry = along - std::sqrt(std::max(0.0f, p.radius * p.radius - perp2));
        if (entry < 0.25f || entry >= best) continue;
        const float h = originHeight + verticalSlope * entry;
        if (h < 0.0f || h > p.height) continue;      // shot passes over or under the prop
        best = entry;
        outProp = static_cast<int>(pi);
    }
    return best;
}

bool PulseGame::pushOutOfCollision(Vec2& pos, float radius, float feet) const {
    if (!collides(pos, radius, feet)) {
        return true;
    }

    const Vec2 origin = pos;
    const float cell = procEnvReady()
        ? 1.0f / static_cast<float>(std::max(1, procEnvSub()))
        : 1.0f;
    const float step = std::max(0.06f, std::min(0.25f, cell * 0.5f));
    const int rings = procEnvReady() ? 64 : 24;

    for (int ring = 1; ring <= rings; ++ring) {
        const float r = step * static_cast<float>(ring);
        const int samples = std::min(32, 8 + ring * 2);
        const float phase = (ring & 1) ? 0.0f : (Pi / static_cast<float>(samples));
        for (int i = 0; i < samples; ++i) {
            const float a = phase + TwoPi * static_cast<float>(i) / static_cast<float>(samples);
            Vec2 candidate = origin + fromAngle(a) * r;
            if (!collides(candidate, radius, feet)) {
                pos = candidate;
                return true;
            }
        }
    }

    return false;
}

void PulseGame::moveWithCollision(Vec2& pos, Vec2& vel, float radius, float dt, float feet) const {
    if (dt <= 0.0f) return;

    if (collides(pos, radius, feet) && !pushOutOfCollision(pos, radius, feet)) {
        vel = {};
        return;
    }

    const float moveLen = std::sqrt(vel.x * vel.x + vel.y * vel.y) * dt;
    const float fineStep = procEnvReady()
        ? 0.5f / static_cast<float>(std::max(1, procEnvSub()))
        : 0.25f;
    const float maxStep = std::max(0.05f, std::min(fineStep, std::max(0.05f, radius * 0.5f)));
    const int steps = std::clamp(static_cast<int>(std::ceil(moveLen / maxStep)), 1, 64);
    const float stepDt = dt / static_cast<float>(steps);

    for (int i = 0; i < steps; ++i) {
        Vec2 next = pos;
        next.x += vel.x * stepDt;
        if (!collides(next, radius, feet)) {
            pos.x = next.x;
        } else {
            vel.x = 0.0f;
        }

        next = pos;
        next.y += vel.y * stepDt;
        if (!collides(next, radius, feet)) {
            pos.y = next.y;
        } else {
            vel.y = 0.0f;
        }

        if (vel.x == 0.0f && vel.y == 0.0f) break;
    }

    pushOutOfCollision(pos, radius, feet);
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

// Height-aware LoS for a shot: DDA the fine grid (like castRay) but at each SOLID cell compare the
// ray's interpolated height against the obstacle top - a low obstacle UNDER the ray (e.g. the cover
// the player is standing on, or a knee-high block between an elevated shooter and a ground enemy)
// no longer blocks. Only an obstacle that rises above the ray blocks it.
bool PulseGame::shotClearTo(Vec2 from, float fromH, Vec2 to, float toH) const {
    const Vec2 delta = to - from;
    const float dist = length(delta);
    if (dist <= 0.01f) return true;
    if (!procEnvReady()) return lineOfSight(from, to);   // non-proc arenas keep the flat 2D check
    const Vec2 dir = delta / dist;
    const float S = static_cast<float>(procEnvSub());
    const float ox = from.x * S, oy = from.y * S;
    int fx = static_cast<int>(std::floor(ox)), fy = static_cast<int>(std::floor(oy));
    const float ddX = std::fabs(dir.x) < 1e-5f ? 1e30f : std::fabs(1.0f / dir.x);
    const float ddY = std::fabs(dir.y) < 1e-5f ? 1e30f : std::fabs(1.0f / dir.y);
    const int stepX = dir.x < 0.0f ? -1 : 1, stepY = dir.y < 0.0f ? -1 : 1;
    float sdX = dir.x < 0.0f ? (ox - static_cast<float>(fx)) * ddX : (static_cast<float>(fx) + 1.0f - ox) * ddX;
    float sdY = dir.y < 0.0f ? (oy - static_cast<float>(fy)) * ddY : (static_cast<float>(fy) + 1.0f - oy) * ddY;
    const float maxF = dist * S;
    const float margin = 0.10f;   // let the ray graze an obstacle top a touch (lenient = shots connect)
    float distF = 0.0f;
    while (distF < maxF) {
        if (sdX < sdY) { sdX += ddX; fx += stepX; distF = sdX - ddX; }
        else           { sdY += ddY; fy += stepY; distF = sdY - ddY; }
        if (distF >= maxF) break;
        if (procEnvSolidFine(fx, fy)) {
            const float t = (distF / S) / dist;                 // 0..1 along the segment
            const float rayH = fromH + (toH - fromH) * t;       // ray height where it crosses this cell
            if (procEnvFineHeight(fx, fy) > rayH + margin) return false;
        }
    }
    return true;
}

PulseGame::RayHit PulseGame::castRay(Vec2 origin, float angle, float maxDistance) const {
    const Vec2 rayDir = fromAngle(angle);

    // Procedural environment: trace the high-res collision grid so shots + line-of-sight match
    // the visible walls/rocks. The coarse 1-unit grid blocks clear shots (enemies "shielded").
    if (procEnvReady()) {
        const float S = static_cast<float>(procEnvSub());
        const float ox = origin.x * S, oy = origin.y * S;
        int fx = static_cast<int>(std::floor(ox));
        int fy = static_cast<int>(std::floor(oy));
        const float ddX = std::fabs(rayDir.x) < 0.00001f ? 1.0e30f : std::fabs(1.0f / rayDir.x);
        const float ddY = std::fabs(rayDir.y) < 0.00001f ? 1.0e30f : std::fabs(1.0f / rayDir.y);
        const int stepX = rayDir.x < 0.0f ? -1 : 1, stepY = rayDir.y < 0.0f ? -1 : 1;
        float sdX = rayDir.x < 0.0f ? (ox - static_cast<float>(fx)) * ddX : (static_cast<float>(fx) + 1.0f - ox) * ddX;
        float sdY = rayDir.y < 0.0f ? (oy - static_cast<float>(fy)) * ddY : (static_cast<float>(fy) + 1.0f - oy) * ddY;
        RayHit hit;
        const float maxF = maxDistance * S;
        float distF = 0.0f;
        while (distF < maxF) {
            int side;
            if (sdX < sdY) { sdX += ddX; fx += stepX; side = 0; distF = sdX - ddX; }
            else           { sdY += ddY; fy += stepY; side = 1; distF = sdY - ddY; }
            if (procEnvSolidFine(fx, fy)) {
                hit.distance = std::max(0.001f, distF / S);
                hit.side = side;
                hit.cover = true;
                return hit;
            }
        }
        hit.distance = maxDistance;
        return hit;
    }

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
            const int rows = static_cast<int>(activeMap().size());
            const int cols = static_cast<int>(activeMap()[0].size());
            if (mapY >= 0 && mapY < rows && mapX >= 0 && mapX < cols) {
                hit.cell = activeMap()[static_cast<size_t>(mapY)][static_cast<size_t>(mapX)];
            }
            // Interior obstacles get the accent texture; the outer shell stays plain.
            hit.cover = mapX > 0 && mapY > 0 && mapX < cols - 1 && mapY < rows - 1;
            return hit;
        }
    }

    hit.distance = maxDistance;
    return hit;
}

float PulseGame::enemyAimY(const Enemy& enemy) const {
    if (enemy.boss) {
        const AnimatedEnemyModel& bm = bossModel(enemy.bossKind);
        return bm.hoverY + 0.5f * bm.worldHeight * BossVisualScale;
    }
    const AnimatedEnemyModel& m = enemyRenderModel(enemy);
    // Flyers (drones) are rendered lifted by hoverY; the body centre sits roughly half the model
    // height above that lift, so the hit-box must track up there (not the floor).
    if (m.loaded && m.hoverY > 0.01f) return m.hoverY + 0.5f * m.worldHeight;
    return 0.5f;   // grounded enemy body centre (the prior baked constant)
}

PulseGame::Projection PulseGame::projectEnemy(const Enemy& enemy, float yaw, float pitch, int screenW, int screenH) const {
    // projectPoint turns this size into sprite height with a 2.6x factor; use the boss's
    // full visual height so the asymmetric target box covers torso-to-head instead of feet-only.
    const AnimatedEnemyModel& m = enemyRenderModel(enemy);
    const float visualRadius = m.loaded ? std::max(0.42f, m.worldHeight * (enemy.boss ? BossVisualScale : 1.0f) / 2.6f)
                                        : 0.56f;
    const float radius = enemy.boss ? std::max(1.20f, visualRadius)
                       : (m.loaded ? visualRadius
                       : (enemy.kind == EnemyKind::Tank ? 0.78f
                       : (enemy.kind == EnemyKind::Ranged ? 0.54f
                       : (enemy.kind == EnemyKind::Stalker ? 0.50f : 0.56f))));
    return projectPoint(enemy.pos, yaw, pitch, screenW, screenH, radius, enemyAimY(enemy));
}

PulseGame::Projection PulseGame::projectPoint(Vec2 point, float yaw, float pitch, int screenW, int screenH, float size, float bodyY) const {
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
    // Place the box at the target's body-centre world height (bodyY): the term is the screen shift
    // from the eye-to-body height difference, so a hovering drone's box rises off the floor to where
    // it is drawn. bodyY defaults to ~0.5 (grounded enemy), so ground targets are unchanged.
    const float centerY = horizon + spriteH * 0.05f +
        (player_.height - bodyY) / depth * static_cast<float>(screenH);

    p.left = static_cast<int>(centerX - spriteW * 0.5f);
    p.right = static_cast<int>(centerX + spriteW * 0.5f);
    p.top = static_cast<int>(centerY - spriteH * 0.75f);
    p.bottom = static_cast<int>(centerY + spriteH * 0.38f);
    p.depth = depth;
    p.side = side;
    p.visible = p.right >= 0 && p.left < screenW && p.bottom >= 0 && p.top < screenH;
    return p;
}

bool PulseGame::ensureGpuResources(Engine& engine) {
    if (gpuResourcesReady_) {
        return true;
    }

    // Arena geometry from the level grid.
    GpuMeshDesc floorMesh, ceilingMesh, wallMesh;
    const int rows = static_cast<int>(activeMap().size());
    const int cols = static_cast<int>(activeMap().front().size());
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const char cell = activeMap()[static_cast<size_t>(y)][static_cast<size_t>(x)];
            const float x0 = static_cast<float>(x), x1 = static_cast<float>(x + 1);
            const float z0 = static_cast<float>(y), z1 = static_cast<float>(y + 1);
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
    // Floor/ceiling quads were authored opposite to the walls/OBJ; unify winding so
    // back-face culling keeps them (positions + normals are untouched).
    reverseWinding(floorMesh);
    reverseWinding(ceilingMesh);
    gpuArenaFloorMesh_ = engineMeshFromGpu(engine, floorMesh);
    gpuArenaCeilingMesh_ = engineMeshFromGpu(engine, ceilingMesh);
    gpuArenaWallMesh_ = engineMeshFromGpu(engine, wallMesh);

    // Convert an authored OBJ (game axis: front=-X, up=+Z) to engine space
    // (x=right, y=up, z=forward), matching the prototype's importer convention.
    const auto objToGpu = [&](const std::vector<MeshVertex>& V, const std::vector<MeshNormal>& N,
                              const std::vector<MeshUv>& U, const std::vector<MeshTriangle>& T,
                              MeshVertex center) {
        GpuMeshDesc mesh;
        mesh.vertices.reserve(T.size() * 3);
        mesh.indices.reserve(T.size() * 3);
        const auto point = [&](const MeshVertex& v) {
            return GpuVec3{ v.z - center.z, v.y - center.y, -(v.x - center.x) };
        };
        const auto nrm = [&](int idx, GpuVec3 gen) {
            if (idx >= 0 && idx < static_cast<int>(N.size())) {
                const MeshNormal& n = N[static_cast<size_t>(idx)];
                return gpuNormalize({n.z, n.y, -n.x});
            }
            return gen;
        };
        const auto uvAt = [&](int idx) {
            return (idx >= 0 && idx < static_cast<int>(U.size())) ? U[static_cast<size_t>(idx)] : MeshUv{};
        };
        for (const MeshTriangle& tri : T) {
            const GpuVec3 a = point(V[static_cast<size_t>(tri.a)]);
            const GpuVec3 b = point(V[static_cast<size_t>(tri.b)]);
            const GpuVec3 c = point(V[static_cast<size_t>(tri.c)]);
            const MeshUv ta = uvAt(tri.ta), tb = uvAt(tri.tb), tc = uvAt(tri.tc);
            const GpuVec3 fn = gpuNormalize(gpuCross(gpuSub(b, a), gpuSub(c, a)));
            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(gpuVertex(a, nrm(tri.na, fn), ta.u, 1.0f - ta.v, tri.color, tri.emissive));
            mesh.vertices.push_back(gpuVertex(b, nrm(tri.nb, fn), tb.u, 1.0f - tb.v, tri.color, tri.emissive));
            mesh.vertices.push_back(gpuVertex(c, nrm(tri.nc, fn), tc.u, 1.0f - tc.v, tri.color, tri.emissive));
            mesh.indices.push_back(base); mesh.indices.push_back(base + 1u); mesh.indices.push_back(base + 2u);
        }
        return mesh;
    };

    for (int k = 0; k < EnemyKindCount; ++k) {
        const MeshAsset& as = enemyMeshAssets_[static_cast<size_t>(k)];
        gpuEnemyMeshes_[static_cast<size_t>(k)] = engineMeshFromGpu(engine, objToGpu(as.vertices, as.normals, as.uvs, as.triangles, MeshVertex{}));
    }

    gpuPickupHealthMesh_ = engineMeshFromGpu(engine, makePickupMesh(rgb(56, 245, 138)));
    gpuPickupShieldMesh_ = engineMeshFromGpu(engine, makePickupMesh(rgb(72, 178, 255)));
    gpuPickupAmmoMesh_ = engineMeshFromGpu(engine, makeAmmoPickupMesh(rgb(225, 170, 60), rgb(60, 48, 30)));
    gpuTracerMesh_ = engineMeshFromGpu(engine, makeTracerMesh());
    gpuProjectileMesh_ = engineMeshFromGpu(engine, makeProjectileMesh(rgb(255, 120, 70)));   // spike (shape 1)
    gpuOrbSmoothMesh_  = engineMeshFromGpu(engine, makeOrbGem(rgb(255, 150, 95), 1.0f, 1.0f)); // gem  (shape 0)
    gpuOrbShardMesh_   = engineMeshFromGpu(engine, makeOrbGem(rgb(255, 150, 95), 0.34f, 1.9f)); // dart (shape 2)
    gpuObjectiveMesh_  = engineMeshFromGpu(engine, makeOrbGem(rgb(255, 255, 255), 0.62f, 1.25f)); // cyan crystal objective (tinted at draw)
    gpuEnergyOrbMesh_  = engineMeshFromGpu(engine, makeSphere(rgb(255, 255, 255), 1.0f, 28, 20));   // unit energy-orb sphere (scaled + tinted per draw, Returnal-style)
    gpuBeamMesh_       = engineMeshFromGpu(engine, makeCylinder(rgb(255, 255, 255), 14));            // unit beam cylinder (lock-on laser)

    // Meshy hero env GLBs (W9): load once, Meshy textures discarded; bind to W5 master
    // materials. All are source-centred at origin (~2 units), placed + scaled at draw in
    // the brutalist arena. A missing GLB leaves the handle Invalid -> draw falls back to
    // the procedural form, so the arena still works without the assets.
    {
        const auto loadGlb = [&](const char* rel) -> MeshHandle {
            std::vector<StaticVertex> v; std::vector<uint32_t> ix;
            if (loadGltfMesh(resolveAsset(rel), v, ix) && !v.empty())
                return engine.createMesh({ v, ix });
            return MeshHandle::Invalid;
        };
        gpuCrystalGlbMesh_ = loadGlb("assets/meshy/raw/slice_cyan_crystal.glb");
        gpuMonumentMesh_   = loadGlb("assets/meshy/raw/slice_monument_obelisk.glb");
        gpuGatewayMesh_    = loadGlb("assets/meshy/raw/slice_gateway_arch.glb");
        // W5 polished obsidian: very dark navy-violet, near-mirror, hero reflective (doc 4).
        matObsidianHero_ = engine.createMaterial(
            styledMaterial(style_, StyleCategory::PolishedObsidian, { 0.05f, 0.05f, 0.11f, 1.0f }));
    }

    // M2.5 procedural prop meshes (boxes of varying proportions, base on the floor).
    const auto boxProp = [&](float hx, float hy, float hz) {
        GpuMeshDesc m; pushGpuBox(m, -hx, 0.0f, -hz, hx, hy, hz, 0x00FFFFFFu);
        return engineMeshFromGpu(engine, m);
    };
    gpuCrateMesh_  = boxProp(0.45f, 0.9f, 0.45f);    // crate
    gpuBarrelMesh_ = boxProp(0.36f, 1.15f, 0.36f);   // drum / barrel
    gpuPillarMesh_ = boxProp(0.34f, 1.0f, 0.34f);    // pillar (scaled floor-to-ceiling)
    gpuPanelMesh_  = boxProp(0.62f, 1.0f, 0.07f);    // thin glowing wall panel

    // World textures (.ptex pixels are 0x00RRGGBB) -> engine RGBA.
    const auto makeTex = [&](const Texture& t) -> TextureHandle {
        const Texture::Level& lv = t.levels.front();
        std::vector<uint8_t> rgba(static_cast<size_t>(lv.width) * lv.height * 4);
        for (size_t i = 0; i < lv.pixels.size(); ++i) {
            const uint32_t p = lv.pixels[i];
            rgba[i * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF);
            rgba[i * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);
            rgba[i * 4 + 2] = static_cast<uint8_t>(p & 0xFF);
            rgba[i * 4 + 3] = 255;
        }
        return engine.createTexture({ static_cast<uint32_t>(lv.width), static_cast<uint32_t>(lv.height), rgba.data() });
    };
    const TextureHandle floorTex = makeTex(floorTex_);
    const TextureHandle ceilTex = makeTex(ceilingTex_);
    const TextureHandle wallTexH = makeTex(wallTex_);
    // PBR params: {baseColor, factor, emissive, metallic, roughness}. Concrete/
    // painted surfaces are rough dielectrics; the weapon + enemies carry metal so
    // they catch specular (metal reads as metal under the Cook-Torrance resolve).
    matFloor_ = engine.createMaterial({ floorTex, {1, 1, 1, 1}, 0.0f, 0.0f, 0.84f });   // matte concrete (art bible: no uniform shiny floor; wet reserved for coolant)
    matCeiling_ = engine.createMaterial({ ceilTex, {1, 1, 1, 1}, 0.0f, 0.0f, 0.88f });
    matWall_ = engine.createMaterial({ wallTexH, {1, 1, 1, 1}, 0.0f, 0.0f, 0.60f });
    matUntextured_ = engine.createMaterial({ TextureHandle::Invalid, {1, 1, 1, 1}, 0.0f, 0.55f, 0.42f });
    if (!ensureActiveWeaponViewmodelResources(engine)) {
        throw std::runtime_error("Active weapon viewmodel GPU resources could not be created");
    }
    if (!ensureAnimatedEnemyResources(engine)) {
        throw std::runtime_error("Bumstrum enemy GPU resources could not be created");
    }

    // Sourced CC0 PolyHaven PBR sets (diff = sRGB albedo, arm = AO/Rough/Metal,
    // nor_gl = tangent normal) dress the arena. Falls back to the procedural
    // materials above if a map is missing.
    // General PBR loader: maps live under <dir>/<id>/<id>_<map><suffix>.{dds,png}, with
    // map names diff (sRGB albedo) / arm (R=AO,G=rough,B=metal, linear) / nor_gl
    // (tangent normal). Used for the CC0 arena sets and the sourced hero assets alike.
    auto loadPbrMaterialEx = [&](const std::string& dir, const std::string& id, const std::string& suffix,
                                 float uvScale, float metalScale = 1.0f, float roughBoost = 0.0f) -> MaterialHandle {
        auto loadTex = [&](const char* map, bool srgb) -> TextureHandle {
            const std::string base = dir + "/" + id + "/" + id + "_" + map + suffix;
            // Prefer the BCn DDS produced by --import-asset (BC7 sRGB albedo, BC5
            // normal, BC7 linear ORM); the format encodes the colour space. Fall
            // back to the source PNG (decoded + CPU-mipped) when no DDS is present.
            const std::string dds = resolveAsset(base + ".dds");
            if (std::filesystem::exists(dds)) {
                const TextureHandle h = engine.createTextureDDS(dds);
                if (h != TextureHandle::Invalid) return h;
            }
            uint32_t w = 0, ht = 0;
            std::vector<uint8_t> rgba = loadImageRGBA(resolveAsset(base + ".png"), w, ht);
            if (rgba.empty()) return TextureHandle::Invalid;
            TextureData td; td.width = w; td.height = ht; td.rgba = rgba.data(); td.srgb = srgb;
            return engine.createTexture(td);
        };
        MaterialDesc d;
        d.baseColor = loadTex("diff", true);
        if (d.baseColor == TextureHandle::Invalid) return MaterialHandle::Invalid;
        d.orm = loadTex("arm", false);
        d.normal = loadTex("nor_gl", false);
        d.uvScale = uvScale;   // tile across the large arena surfaces for texel density
        d.metalScale = metalScale;
        d.roughBoost = roughBoost;
        return engine.createMaterial(d);
    };
    // Arena CC0 sets keep the original polyhaven/<id>_<map>_1k layout.
    auto loadPbrMaterial = [&](const char* id, float uvScale,
                               float metalScale = 1.0f, float roughBoost = 0.0f) -> MaterialHandle {
        return loadPbrMaterialEx("assets/external/polyhaven", id, "_1k", uvScale, metalScale, roughBoost);
    };
    if (MaterialHandle m = loadPbrMaterial("concrete_floor_02", 5.0f); m != MaterialHandle::Invalid) {
        matFloor_ = m; matCeiling_ = m;
    }
    // Walls: de-metal + roughen the metal-plate set so the dark mirror-like plate
    // reads as a solid industrial wall instead of a translucent-looking surface.
    if (MaterialHandle m = loadPbrMaterial("metal_plate_02", 3.0f, 0.2f, 0.3f); m != MaterialHandle::Invalid) matWall_ = m;

    // M2.5 named PBR material library for props (CC0 sets re-tuned via metalScale /
    // roughBoost; falls back to the plain untextured material if a set is missing).
    const auto orFallback = [&](MaterialHandle m) { return m != MaterialHandle::Invalid ? m : matUntextured_; };
    matCrate_  = orFallback(loadPbrMaterial("metal_plate_02", 1.6f, 0.12f, 0.45f));   // matte painted crate
    matBarrel_ = orFallback(loadPbrMaterial("metal_plate_02", 1.3f, 0.75f, 0.12f));   // metal drum
    matPillar_ = orFallback(loadPbrMaterial("concrete_floor_02", 2.2f, 0.0f, 0.10f)); // concrete pillar
    matPanel_  = matUntextured_;   // emissive accent uses per-instance emissive

    // Procedural OUTDOOR arenas (CC-BY DJMaesen kits): the Wasteland self-loads its kit library
    // (medieval-sceneray + the sketchfab rock/tree/grass sets) and scatters them by BIOME (rocky
    // canyon / forest) over open ground - the default environment, open space + clear sightlines
    // for the arena combat. Different rooms travel through different biomes (regenerateArena).
    // The one and only environment: the Quaternius brutalist arena. Wasteland self-loads its kit
    // and generateQuaternius assembles each room; setBrutalist(true) selects the MegaKit shell.
    wasteland_.setBrutalist(true);
    if (!wasteland_.load(engine))
        throw std::runtime_error("Required Quaternius arena kit missing/empty (assets/packs/pulse_environment/quaternius)");
    regenerateArena();   // first-room layout + spawn (beginRoom ran before the kit was loaded)

    buildArenaProps();

    gpuResourcesReady_ = true;
    return true;
}

const SceneFrame& PulseGame::buildFrame(Engine& engine, int screenW, int screenH) {
    ensureGpuResources(engine);
    ensureActiveWeaponViewmodelResources(engine);

    // Shaken view angle (drawing only; the aim ray in tryFire stays exact).
    const float shakeRad = degToRad(cameraShake_);
    const float shakeYaw = shakeRad * std::sin(shakeTime_ * 47.0f);
    const float shakePitch = shakeRad * 0.6f * std::sin(shakeTime_ * 59.0f + 1.3f);
    const float bobAmt = degToRad(std::max(0.0f, tunables_.cameraBobDegrees));
    const float bobYaw = std::sin(cameraBobPhase_) * bobAmt * 0.34f;
    const float bobPitch = std::sin(cameraBobPhase_ * 2.0f) * bobAmt;
    const float strafeYaw = strafeLean_ * degToRad(tunables_.cameraStrafeLeanDegrees) * 0.45f;
    const float strafePitch = -std::fabs(strafeLean_) * degToRad(tunables_.cameraStrafeLeanDegrees) * 0.18f;
    // Per-shot camera punch (render-only; the aim ray stays exact). Scaled up for more
    // felt weight per shot -- this snaps with the shot and recovers via cameraKickRecovery.
    const float transientCameraPitch = degToRad(fireCameraKick_ * 0.090f);
    const float transientCameraYaw = degToRad(weaponKickSide_ * 0.22f);
    renderViewYaw_ = player_.yaw + recoilOffsetYaw_ + shakeYaw + bobYaw + strafeYaw + transientCameraYaw;
    renderViewPitch_ = clamp(player_.pitch + recoilOffsetPitch_ + transientCameraPitch + shakePitch + bobPitch + strafePitch - landingKick_, -1.5f, 1.5f);

    instances_.clear();
    // The id is stable across frames (per logical entity), so the engine can track
    // each object's previous transform for motion vectors. Ranges: world 1-9,
    // viewmodel 10-19, pickups 100+, enemies 1000+, projectiles 5000+.
    const auto addDraw = [&](uint64_t id, MeshHandle mesh, MaterialHandle mat, const Mat4& xf, Vec4f tint,
                             float emissive = 0.0f, bool cameraSpace = false,
                             Vec3f rimColor = { 0, 0, 0 }, float rimPower = 0.0f) {
        MeshInstance mi;
        mi.id = id;
        mi.mesh = mesh; mi.material = mat; mi.transform = xf;
        mi.tint = tint; mi.emissiveAdd = emissive; mi.cameraSpace = cameraSpace;
        mi.rimColor = rimColor; mi.rimPower = rimPower;
        instances_.push_back(mi);
    };

    if (procEnvReady()) {
        // Procedural environment (outdoor arena / dungeon): static geometry, ids in a high range
        // (no motion-vector collision with the dynamic 1..9 / 1000+ ranges).
        uint64_t did = 300000;
        for (const DungeonDraw& d : procEnvDraws())
            addDraw(did++, d.mesh, d.material, d.transform, {1, 1, 1, 1});

        // Spatial door frames: a gateway arch at every door. During combat all sit dark/sealed;
        // when a room clears the bound exits glow in their destination's colour so the player can
        // see where to go. The entrance (door 0) is never bound, so it always reads sealed.
        if (gpuGatewayMesh_ != MeshHandle::Invalid) {
            const auto doorTypeColor = [](RoomType t) -> Vec3f {
                switch (t) {
                    case RoomType::Elite: return { 1.9f, 0.7f, 0.3f };
                    case RoomType::Cache: return { 1.7f, 1.4f, 0.4f };
                    case RoomType::Shop:  return { 0.4f, 1.6f, 0.7f };
                    case RoomType::Event: return { 1.2f, 0.6f, 1.8f };
                    case RoomType::Boss:  return { 1.9f, 0.3f, 0.35f };
                    default:              return { 0.3f, 1.5f, 1.9f }; // Combat = cyan
                }
            };
            const std::vector<Door>& envDoors = wasteland_.doors();
            uint64_t dgid = 310000;
            const float sub = static_cast<float>(wasteland_.subRes());
            // Door leaves are the SAME Quaternius family as the walls: two kit door panels per opening,
            // each scaled to half the gap, sliding apart by the animation phase. Falls back to plain
            // metal boxes only if the kit leaf is unavailable.
            const std::vector<DungeonDraw>& quatLeaf = wasteland_.doorLeafParts();
            const float leafNativeW = wasteland_.doorLeafWidth(), leafNativeH = wasteland_.doorLeafHeight();
            const bool useQuatLeaf = currentBiome_ != Biome::Forest && !quatLeaf.empty() && leafNativeW > 0.1f && leafNativeH > 0.1f;
            const Vec4f quatLeafTint =
                currentBiome_ == Biome::Forest ? Vec4f{ 0.58f, 0.42f, 0.32f, 1.0f } :
                currentBiome_ == Biome::Ruins  ? Vec4f{ 0.72f, 0.76f, 0.82f, 1.0f } :
                                                 Vec4f{ 0.82f, 0.88f, 0.92f, 1.0f };
            const MaterialHandle doorMetal = wasteland_.brutalMaterial(5);   // fallback leaf material
            for (int i = 0; i < static_cast<int>(envDoors.size()); ++i) {
                const Door& ed = envDoors[static_cast<size_t>(i)];
                const DoorBind* bind = nullptr;
                for (const DoorBind& b : doors_) if (b.envDoorIndex == i) { bind = &b; break; }

                // ACTUAL DOORS: a bi-parting sliding door fills the gap - two leaves that meet in the
                // middle when shut (slide=0, sealed during combat) and retract when the room clears
                // (slide=1). Eased per frame in update(); collision is the wasteland grid.
                const float slide = (i < static_cast<int>(doorAnim_.size()))
                                ? doorAnim_[static_cast<size_t>(i)] : (ed.open ? 1.0f : 0.0f);
                const float doorH = 4.0f;
                const bool alongX = (ed.side == Side::N || ed.side == Side::S);
                const float a0 = alongX ? static_cast<float>(ed.fgx0) / sub : static_cast<float>(ed.fgz0) / sub;
                const float a1 = alongX ? static_cast<float>(ed.fgx1) / sub : static_cast<float>(ed.fgz1) / sub;
                const float ctr = 0.5f * (a0 + a1), hw = 0.5f * (a1 - a0), off = slide * hw;  // hw = one leaf's width
                if (useQuatLeaf) {
                    // leaf yaw faces the room (panel native faces +Z, width along +X): N=0 S=pi W=pi/2 E=-pi/2
                    const float yawLeaf = (ed.side == Side::N) ? 0.0f : (ed.side == Side::S) ? 3.1415927f
                                        : (ed.side == Side::W) ? 1.5707963f : -1.5707963f;
                    const float ws = hw / leafNativeW, hs = doorH / leafNativeH;
                    const float fixed = alongX ? ed.worldZ : ed.worldX;
                    const auto drawLeaf = [&](float centerAlong) {
                        const float cx = alongX ? centerAlong : fixed;
                        const float cz = alongX ? fixed : centerAlong;
                        const Mat4 xf = mul(scaling(ws, hs, 1.0f),
                                            mul(rotationYawPitchRoll(yawLeaf, 0.0f, 0.0f), translation(cx, 0.0f, cz)));
                        for (const DungeonDraw& pp : quatLeaf) addDraw(dgid++, pp.mesh, pp.material, xf, quatLeafTint, 0.0f);
                    };
                    drawLeaf(a0 + hw * 0.5f - off);
                    drawLeaf(a1 - hw * 0.5f + off);
                } else if (wasteland_.boxMesh() != MeshHandle::Invalid) {
                    const float thick = 0.22f, inset = 0.24f;
                    const Vec4f leafTint =
                        currentBiome_ == Biome::Forest ? Vec4f{ 0.28f, 0.24f, 0.20f, 1.0f } :
                        currentBiome_ == Biome::Ruins  ? Vec4f{ 0.46f, 0.48f, 0.54f, 1.0f } :
                                                         Vec4f{ 0.36f, 0.42f, 0.48f, 1.0f };
                    const auto box = [&](float x0, float x1, float z0, float z1) {
                        const Mat4 xf = mul(scaling(std::max(0.02f, x1 - x0), doorH, std::max(0.02f, z1 - z0)),
                                            translation(x0, 0.0f, z0));
                        addDraw(dgid++, wasteland_.boxMesh(), doorMetal, xf, leafTint, 0.0f);
                    };
                    if (alongX) {
                        const float zc = ed.worldZ + ed.inwardZ * inset, z0 = zc - thick * 0.5f, z1 = zc + thick * 0.5f;
                        box(a0 - off, ctr - off, z0, z1); box(ctr + off, a1 + off, z0, z1);
                    } else {
                        const float xc = ed.worldX + ed.inwardX * inset, x0 = xc - thick * 0.5f, x1 = xc + thick * 0.5f;
                        box(x0, x1, a0 - off, ctr - off); box(x0, x1, ctr + off, a1 + off);
                    }
                }

                if (bind && (ed.open || slide > 0.65f)) {
                    // Cleared exit: a thin destination-coloured status bar across the top of the opening
                    // (the at-a-glance route cue; the reward tooltip names it) + a real lit corridor
                    // behind. The opening itself is the Quaternius door frame + sliding leaves - the old
                    // glowing gateway arch is gone so the doorway stays one Quaternius family.
                    const Vec3f c = doorTypeColor(bind->destType);
                    const float pulse = 2.2f + 0.5f * std::sin(shakeTime_ * 3.4f + static_cast<float>(i));
                    if (wasteland_.boxMesh() != MeshHandle::Invalid) {
                        const float by = 3.7f, bh = 0.16f, bt = 0.14f;
                        const Mat4 bxf = alongX
                            ? mul(scaling(std::max(0.3f, a1 - a0), bh, bt), translation(a0, by, ed.worldZ - bt * 0.5f))
                            : mul(scaling(bt, bh, std::max(0.3f, a1 - a0)), translation(ed.worldX - bt * 0.5f, by, a0));
                        addDraw(dgid++, wasteland_.boxMesh(), wasteland_.brutalMaterial(1), bxf, { c.x, c.y, c.z, 1.0f }, pulse);
                    }

                    const float ox = -ed.inwardX, oz = -ed.inwardZ;          // outward (axis-aligned)
                    const float qx = -oz, qz = ox;                           // perpendicular
                    const std::vector<DungeonDraw>& hall = wasteland_.hallwayParts();
                    if (!hall.empty() && wasteland_.boxMesh() != MeshHandle::Invalid) {
                        const float hyaw = std::atan2(ox, oz);               // align the kit's +Z with outward
                        const float span = std::max(2.0f, wasteland_.hallwaySpan());
                        const int nseg = 2;
                        for (int k = 0; k < nseg; ++k) {
                            const float dd = (static_cast<float>(k) + 0.5f) * span;
                            const Mat4 hxf = mul(rotationYawPitchRoll(hyaw, 0.0f, 0.0f),
                                                 translation(ed.worldX + ox * dd, 0.0f, ed.worldZ + oz * dd));
                            for (const DungeonDraw& p : hall) addDraw(dgid++, p.mesh, p.material, hxf, { 1, 1, 1, 1 }, 0.0f);
                        }
                        // Dark end-cap + glow at the far end so no void/sky shows past the corridor.
                        const float L = static_cast<float>(nseg) * span;
                        const auto cap = [&](float a0, float a1, float p0, float p1, float y0, float y1, int mat, float emis) {
                            const float cx0 = ed.worldX + ox * a0 + qx * p0, cz0 = ed.worldZ + oz * a0 + qz * p0;
                            const float cx1 = ed.worldX + ox * a1 + qx * p1, cz1 = ed.worldZ + oz * a1 + qz * p1;
                            const Mat4 xf = mul(scaling(std::max(0.05f, std::fabs(cx1 - cx0)), std::max(0.05f, y1 - y0),
                                                        std::max(0.05f, std::fabs(cz1 - cz0))),
                                                translation(std::min(cx0, cx1), y0, std::min(cz0, cz1)));
                            addDraw(dgid++, wasteland_.boxMesh(), wasteland_.brutalMaterial(mat), xf, { 1, 1, 1, 1 }, emis);
                        };
                        cap(L, L + 0.4f, -2.4f, 2.4f, 0.0f, 4.6f, 2, 0.0f);          // end wall
                        cap(L - 0.1f, L, -0.9f, 0.9f, 1.1f, 3.2f, 3, 0.0f);          // end glow panel
                    }
                }
            }
        }
    } else {
        addDraw(1, gpuArenaFloorMesh_, matFloor_, Mat4::identity(), {1, 1, 1, 1});
        addDraw(2, gpuArenaCeilingMesh_, matCeiling_, Mat4::identity(), {0.72f, 0.78f, 0.88f, 1});
        addDraw(3, gpuArenaWallMesh_, matWall_, Mat4::identity(), {0.92f, 0.97f, 1.08f, 1});
    }

    // M2.5 decorative props (crates / barrels / pillars / glowing panels). Static, so
    // prevModel == model (no motion vectors). Ids 20+ stay out of the dynamic ranges.
    for (size_t i = 0; i < props_.size(); ++i) {
        const Prop& pr = props_[i];
        MeshHandle mesh = gpuCrateMesh_;
        MaterialHandle mat = matCrate_;
        float sy = pr.scale;
        switch (pr.kind) {
            case 1: mesh = gpuBarrelMesh_; mat = matBarrel_; break;
            case 2: mesh = gpuPillarMesh_; mat = matPillar_; sy = 1.0f; break;  // floor-to-ceiling
            case 3: mesh = gpuPanelMesh_;  mat = matPanel_;  break;
            default: break;                                                     // 0 = crate
        }
        const Mat4 xf = mul(scaling(pr.scale, sy, pr.scale),
                            mul(rotationYawPitchRoll(pr.yaw, 0.0f, 0.0f),
                                translation(pr.pos.x, pr.pos.y, pr.pos.z)));
        addDraw(20 + static_cast<uint64_t>(i), mesh, mat, xf,
                { pr.tint.x, pr.tint.y, pr.tint.z, 1.0f }, pr.emissive);
    }

    for (const Pickup& pk : pickups_) {
        const uint64_t pkId = 100 + static_cast<uint64_t>(&pk - pickups_.data());
        const float bob = std::sin(pk.age * 4.2f + pk.phase) * 0.045f;
        const float spin = pk.age * 1.55f + pk.phase;
        const Mat4 xf = mul(scaling(0.115f, 0.115f, 0.115f),
                            mul(rotationYawPitchRoll(spin, 0.4f, 0.0f),
                                translation(pk.pos.x, 0.48f + bob, pk.pos.y)));
        if (pk.kind == PickupKind::Health)      addDraw(pkId, gpuPickupHealthMesh_, matUntextured_, xf, {0.85f, 1.08f, 0.92f, 1}, 0.22f);
        else if (pk.kind == PickupKind::Shield) addDraw(pkId, gpuPickupShieldMesh_, matUntextured_, xf, {0.88f, 1.00f, 1.18f, 1}, 0.25f);
        else                                    addDraw(pkId, gpuPickupAmmoMesh_, matUntextured_, xf, {1.12f, 0.96f, 0.60f, 1}, 0.16f);
    }

    // The room compiler owns focal composition now. The old universal cyan floating crystal plus
    // fixed Meshy gateway/obelisk props fought the authored Quaternius layouts and made every room
    // share the same "magic orb" read, so designed rooms intentionally do not draw them.
    objectivePos_ = { wasteland_.centerX(), 2.7f, wasteland_.centerZ() };

    // Skinned Meshy enemy visuals. Each live enemy claims a pool slot for its selected
    // visual so different bodies of the same gameplay kind animate independently.
    std::array<size_t, EnemyVisualCount> visualSlot{};
    std::array<size_t, BossKindCount> bossSlot{};
    enemySmokeRender_.clear();   // shadow-smoke aura, rebuilt per frame (alpha-blended billboards)
    enemyCoreRender_.clear();    // additive white-hot core glows, rebuilt per frame
    enemyBeamLines_.clear();     // projected hostile beam/tail strokes, rebuilt per frame
    // Firing enemy beams collected this frame; consumed in the light / decal / heat-haze sections
    // below so each lance is GROUNDED in the world - a sustained impact light + scorch mark + a
    // thin heat-haze band along its length (the modelled endpoints from the beam VFX spec).
    struct ActiveBeam { Vec3f from{}; Vec3f to{}; Vec3f color{}; float intensity = 1.0f; };
    struct ActiveEnemyLight { Vec3f pos{}; Vec3f color{}; float intensity = 1.0f; float radius = 1.0f; };
    std::vector<ActiveBeam> activeBeams;
    std::vector<ActiveEnemyLight> activeEnemyLights;
    struct EnemyLightAnchor {
        float height = 0.55f;   // fraction of visible body height above the feet
        float forward = 0.06f;  // fraction of body height toward the enemy's front
        float right = 0.0f;     // fraction of body height to the enemy's right
        float size = 0.085f;    // billboard radius fraction of body height
    };
    const auto enemyLightAnchor = [](EnemyKind kind, bool boss, int bossKind) {
        if (boss) {
            switch (std::clamp(bossKind, 0, BossKindCount - 1)) {
                case 0: return EnemyLightAnchor{ 0.54f, 0.055f, 0.0f, 0.060f };
                case 1: return EnemyLightAnchor{ 0.50f, 0.060f, 0.0f, 0.058f };
                case 2: return EnemyLightAnchor{ 0.48f, 0.045f, 0.0f, 0.052f };
            }
        }
        switch (kind) {
            case EnemyKind::Rusher:  return EnemyLightAnchor{ 0.45f, 0.120f, 0.0f, 0.082f };
            case EnemyKind::Ranged:  return EnemyLightAnchor{ 0.52f, 0.055f, 0.0f, 0.070f };
            case EnemyKind::Tank:    return EnemyLightAnchor{ 0.55f, 0.075f, 0.0f, 0.080f };
            case EnemyKind::Stalker: return EnemyLightAnchor{ 0.54f, 0.105f, 0.0f, 0.075f };
        }
        return EnemyLightAnchor{};
    };
    for (const Enemy& e : enemies_) {
        if (!e.active) continue;
        const uint64_t eId = 1000 + static_cast<uint64_t>(&e - enemies_.data()) * 8;
        const Vec2 ef = normalize(player_.pos - e.pos);
        float windup = 0.0f;
        if (e.telegraphRemaining > 0.0f) {
            const float base = e.kind == EnemyKind::Ranged ? std::max(0.05f, tunables_.enemyRangedTelegraph)
                                                           : std::max(0.05f, tunables_.enemyMeleeTelegraph);
            windup = 0.4f + 0.6f * clamp(1.0f - e.telegraphRemaining / base, 0.0f, 1.0f);
        }
        // Per-archetype attack VFX, all anchored to the LIVE enemy (so nothing floats in thin air):
        // a firing BEAM, a beam TELEGRAPH aim line, or the CHARGE-UP orb for orb/fan/burst.
        const Vec3f ecol = enemyShotColor(e.kind, e.pendingAttack, e.boss, e.bossKind);
        // Beam / charge VFX ids live in a high range (620000+) so the layered beam + charge meshes
        // never collide with the enemy's body-submesh ids (eId + s) for motion-vector tracking.
        const uint64_t vId = 620000 + (eId - 1000);
        const AnimatedEnemyModel& vfxModel = enemyRenderModel(e);
        const float vfxBossScale = e.boss ? BossVisualScale : 1.0f;
        const float vfxBodyH = (vfxModel.loaded ? vfxModel.worldHeight : 1.0f) * vfxBossScale;
        const EnemyLightAnchor vfxAnchor = enemyLightAnchor(e.kind, e.boss, e.bossKind);
        const Vec2 vfxRight2 = rightFromForward(ef);
        const Vec3f vfxSource{
            e.pos.x + ef.x * (vfxAnchor.forward * vfxBodyH) + vfxRight2.x * (vfxAnchor.right * vfxBodyH),
            roomFloorY_ + vfxModel.hoverY + vfxAnchor.height * vfxBodyH,
            e.pos.y + ef.y * (vfxAnchor.forward * vfxBodyH) + vfxRight2.y * (vfxAnchor.right * vfxBodyH),
        };
        // Enemy lock-on beams draw as projected continuous strokes. The old 3D cylinder path could
        // pick up stylized resolve artifacts at shallow angles, reading as dotted beads in captures.
        const auto drawEnergyBeam = [&](Vec2 bdir, float len, float baseThick, Vec3f col, float intensity) {
            if (lengthSq(bdir) < 1e-6f || len < 0.1f) return;
            const Vec2 u = normalize(bdir);
            const Vec3f muzzle{ vfxSource.x + u.x * 0.06f * vfxBodyH, vfxSource.y, vfxSource.z + u.y * 0.06f * vfxBodyH };
            const Vec3f endPt{ vfxSource.x + u.x * len, vfxSource.y, vfxSource.z + u.y * len };
            enemyBeamLines_.push_back({ muzzle, endPt, col, intensity, baseThick });
        };
        if (e.beamFireTimer > 0.0f) {
            // FIRING: a searing lance along the locked line. Thickest at the snap, thinning + fading
            // over its short life (a clean cutoff, not an instant disappear). Anchored to e.pos so it
            // tracks the enemy; the modelled endpoints (muzzle flare + sustained impact pool) follow.
            const float ft = clamp(e.beamFireTimer / 0.22f, 0.0f, 1.0f);
            const float baseThick = 0.045f + 0.045f * ft;
            const float intensity = clamp(ft * 1.35f, 0.0f, 1.0f);   // hold bright, then fade at the cutoff
            drawEnergyBeam(e.beamDir, e.beamLen, baseThick, ecol, intensity);
            const Vec2 bu = normalize(e.beamDir);
            const Vec3f muzzle{ vfxSource.x + bu.x * 0.06f * vfxBodyH, vfxSource.y, vfxSource.z + bu.y * 0.06f * vfxBodyH };
            const Vec3f endPt{ vfxSource.x + bu.x * e.beamLen, vfxSource.y, vfxSource.z + bu.y * e.beamLen };
            // Origin flare: a tight bright burst where the beam is born.
            spawnParticles(muzzle, { 0.0f, 0.0f, 0.0f }, 2, 0.0f, 0.05f, 0.30f, ecol, 7.0f, 0.0f, 6.0f);
            // The solid beam layers + heat-haze carry the flow. Avoid per-frame motes along the
            // length as the main read; tiny offset flecks below are corona, not the beam body.
            // Sustained impact pool: sparks raining off the burning hit point each frame.
            spawnParticles(endPt, { 0.0f, 1.6f, 0.0f }, 2, 2.2f, 0.22f, 0.05f, ecol, 4.5f, 3.0f, 2.5f, 0.18f);
            {
                const Vec3f side3{ -bu.y, 0.0f, bu.x };
                const Vec3f hot = hotHue(ecol, 1.42f, 2.7f);
                for (int i = 0; i < 6; ++i) {
                    const float t = (static_cast<float>(i) + 0.5f) / 6.0f;
                    const float ph = shakeTime_ * 46.0f + e.bobPhase + static_cast<float>(i) * 1.618f;
                    Particle fleck;
                    fleck.center = {
                        muzzle.x + (endPt.x - muzzle.x) * t + side3.x * std::sin(ph) * 0.08f,
                        muzzle.y + (endPt.y - muzzle.y) * t + std::cos(ph * 0.7f) * 0.05f,
                        muzzle.z + (endPt.z - muzzle.z) * t + side3.z * std::sin(ph) * 0.08f
                    };
                    fleck.size = 0.030f + 0.018f * ft;
                    fleck.color = (i & 1) ? hot : ecol;
                    fleck.emissive = (1.45f + 1.25f * ft) * intensity;
                    fleck.velocity = { side3.x * std::cos(ph) * 2.2f, 0.35f, side3.z * std::cos(ph) * 2.2f };
                    fleck.stretch = 0.30f;
                    enemyCoreRender_.push_back(fleck);
                }
            }
            activeBeams.push_back({ muzzle, endPt, ecol, intensity });
        } else if (e.pendingRanged && e.telegraphRemaining > 0.0f && e.pendingAttack == EnemyAttack::Beam) {
            // TELEGRAPH (the dodge cue): a thin charge-LINE drawn along the exact fire path. It tracks
            // the player, then brightens + thickens once LOCKED - the readability contract from the
            // spec (the last window to step off the line).
            const float pdist = length(player_.pos - e.pos);
            const float len = std::min(pdist + 1.0f, 16.0f);
            drawEnergyBeam(e.beamDir, len, e.beamLocked ? 0.018f : 0.009f, ecol, e.beamLocked ? 0.85f : 0.40f);
            // Charge gather: a faint forming glow at the muzzle that brightens toward the lock.
            const Vec2 bu = normalize(e.beamDir);
            const float csz = e.beamLocked ? 0.12f : 0.07f;
            const Vec3f cp{ vfxSource.x + bu.x * 0.04f * vfxBodyH, vfxSource.y, vfxSource.z + bu.y * 0.04f * vfxBodyH };
            const Mat4 cxf = mul(scaling(csz, csz, csz), translation(cp.x, cp.y, cp.z));
            addDraw(vId + 6, gpuEnergyOrbMesh_, matUntextured_, cxf, { ecol.x, ecol.y, ecol.z, 1.0f },
                    e.beamLocked ? 2.4f : 1.2f);
            activeEnemyLights.push_back({ cp, ecol, e.beamLocked ? 1.25f : 0.55f, e.beamLocked ? 2.2f : 1.45f });
            {
                const Vec3f side3{ -bu.y, 0.0f, bu.x };
                const int n = e.beamLocked ? 4 : 2;
                for (int i = 0; i < n; ++i) {
                    const float ph = shakeTime_ * 34.0f + e.bobPhase + static_cast<float>(i) * Pi;
                    Particle mote;
                    mote.center = {
                        cp.x + side3.x * std::sin(ph) * csz * 1.15f,
                        cp.y + std::cos(ph * 1.3f) * csz * 0.55f,
                        cp.z + side3.z * std::sin(ph) * csz * 1.15f
                    };
                    mote.size = csz * (e.beamLocked ? 0.42f : 0.34f);
                    mote.color = (i & 1) ? hotHue(ecol, 1.35f, 2.6f) : ecol;
                    mote.emissive = e.beamLocked ? 2.1f : 1.2f;
                    mote.velocity = { side3.x * std::cos(ph) * 1.6f, 0.0f, side3.z * std::cos(ph) * 1.6f };
                    mote.stretch = 0.22f;
                    enemyCoreRender_.push_back(mote);
                }
            }
        } else if (e.pendingRanged && e.telegraphRemaining > 0.0f) {
            // CHARGE-UP orb (orb / fan / burst): forms + grows at the hands, brightening to release.
            // Layered like the bolt it becomes - a saturated body wrapping a thin white-hot core, so
            // it never reads as a fat white-pink bloom blob even at full charge.
            const float telDur = std::max(0.05f, tunables_.enemyRangedTelegraph);
            const float prog = clamp(1.0f - e.telegraphRemaining / telDur, 0.0f, 1.0f);
            const float csz = (0.030f + 0.058f * prog) * (1.0f + 0.06f * std::sin(shakeTime_ * 34.0f));
            const Vec3f cp{ vfxSource.x + ef.x * 0.035f * vfxBodyH, vfxSource.y, vfxSource.z + ef.y * 0.035f * vfxBodyH };
            const Mat4 cxf = mul(scaling(csz, csz, csz), translation(cp.x, cp.y, cp.z));
            addDraw(vId + 6, gpuEnergyOrbMesh_, matUntextured_, cxf, { ecol.x, ecol.y, ecol.z, 1.0f }, 0.75f + 0.95f * prog);
            activeEnemyLights.push_back({ cp, ecol, 0.32f + 0.95f * prog, 0.95f + 0.78f * prog });
            // Gathering-energy aura: a soft additive halo that swells + brightens toward release, so
            // the orb visibly charges up (and blooms) on the enemy before it launches.
            {
                Particle h; h.center = cp; h.size = csz * (1.25f + 0.35f * prog); h.color = ecol;
                h.emissive = 0.55f + 0.85f * prog; h.velocity = { 0.0f, 0.0f, 0.0f }; h.stretch = 0.0f;
                enemyCoreRender_.push_back(h);
            }
            const float ksz = csz * 0.34f;
            const Mat4 kxf = mul(scaling(ksz, ksz, ksz), translation(cp.x, cp.y, cp.z));
            const Vec3f coreCol = hotHue(ecol, 1.38f, 2.7f);
            addDraw(vId + 7, gpuEnergyOrbMesh_, matUntextured_, kxf, { coreCol.x, coreCol.y, coreCol.z, 1.0f }, 0.85f + 0.95f * prog);
            {
                const Vec3f side3{ -ef.y, 0.0f, ef.x };
                const int n = e.pendingAttack == EnemyAttack::Fan ? 5 : (e.pendingAttack == EnemyAttack::Burst ? 4 : 3);
                for (int i = 0; i < n; ++i) {
                    const float fi = static_cast<float>(i);
                    const float ph = shakeTime_ * (e.pendingAttack == EnemyAttack::Burst ? 44.0f : 28.0f) + e.bobPhase + fi * 2.094f;
                    const float orbit = csz * (e.pendingAttack == EnemyAttack::Fan ? 1.95f : 1.45f);
                    Particle mote;
                    mote.center = {
                        cp.x + side3.x * std::sin(ph) * orbit - ef.x * csz * 0.30f * fi,
                        cp.y + std::cos(ph * 1.2f) * csz * 0.60f,
                        cp.z + side3.z * std::sin(ph) * orbit - ef.y * csz * 0.30f * fi
                    };
                    mote.size = csz * (e.pendingAttack == EnemyAttack::Burst ? 0.34f : 0.42f) * (0.75f + 0.55f * prog);
                    mote.color = (i & 1) ? coreCol : ecol;
                    mote.emissive = (1.0f + 1.75f * prog) * (e.pendingAttack == EnemyAttack::Fan ? 1.08f : 1.0f);
                    mote.velocity = { side3.x * std::cos(ph) * 1.2f, e.pendingAttack == EnemyAttack::Fan ? -0.12f : 0.08f,
                                      side3.z * std::cos(ph) * 1.2f };
                    mote.stretch = e.pendingAttack == EnemyAttack::Burst ? 0.28f : 0.18f;
                    enemyCoreRender_.push_back(mote);
                }
            }
        }
        // Boss nova telegraph: a danger RING painted flat on the floor around the boss during its
        // wind-up, brightening toward the strike - "the edge is the hitbox, telegraphed flat on the
        // floor first" (the area-burst read from the spec). Deterministic placement, pushed straight
        // into the additive core list (no rng, no pool growth).
        if (e.boss && e.telegraphRemaining > 0.0f) {
            const float ringR = 3.4f;
            const float k = 0.4f + 0.6f * windup;
            const float ph = 1.0f + 0.06f * std::sin(shakeTime_ * 22.0f);   // subtle live pulse
            // Hard bright leading EDGE: dense overlapping motes so the wind-up reads as ONE continuous
            // glowing danger ring (the dodge-out edge = the hitbox), not the old string of 36 dots.
            const int n = 100;
            for (int i = 0; i < n; ++i) {
                const float a = TwoPi * static_cast<float>(i) / static_cast<float>(n);
                Particle d;
                d.center   = { e.pos.x + std::cos(a) * ringR, roomFloorY_ + 0.06f, e.pos.y + std::sin(a) * ringR };
                d.size     = 0.17f * ph;
                d.color    = ecol;
                d.emissive = 2.4f * k;
                d.velocity = { 0.0f, 0.0f, 0.0f };
                d.stretch  = 0.0f;
                enemyCoreRender_.push_back(d);
            }
            // Soft inner glow band just inside the edge, so the ring has body and grounds the threat
            // area on the floor instead of reading as a thin wire.
            const int m2 = 60;
            for (int i = 0; i < m2; ++i) {
                const float a = TwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(m2);
                Particle d;
                d.center   = { e.pos.x + std::cos(a) * (ringR - 0.45f), roomFloorY_ + 0.05f, e.pos.y + std::sin(a) * (ringR - 0.45f) };
                d.size     = 0.50f;
                d.color    = ecol;
                d.emissive = 0.7f * k;
                d.velocity = { 0.0f, 0.0f, 0.0f };
                d.stretch  = 0.0f;
                enemyCoreRender_.push_back(d);
            }
        }
        const AnimatedEnemyModel& m = enemyRenderModel(e);
        // Boss visual: big + imposing in the open arena; shrunk only in the dungeon so it fits
        // the corridors it has to chase through.
        const float bossScale = e.boss ? BossVisualScale : 1.0f;   // Skeleton Lord: towering apex threat
        const float wobble = 1.0f + e.hitPunch * 0.26f + windup * 0.14f;
        const bool skinned = m.loaded && m.gpuReady;
        // Face the player, with a per-rig yaw correction; feet sit on the floor (y=0).
        const float faceA = std::atan2(ef.y, ef.x) + m.yawOffset;
        const Vec2 ff{ std::cos(faceA), std::sin(faceA) };
        const Vec2 rr = rightFromForward(ff);
        // Idle life: a subtle breathing scale-pulse that fades out as the enemy starts moving, so a
        // standing enemy reads as alive (chest rise/fall) instead of frozen between moves.
        float locoWE = clamp((length(e.vel) - 0.15f) / 0.55f, 0.0f, 1.0f);
        locoWE = locoWE * locoWE * (3.0f - 2.0f * locoWE);
        const bool flyerPose = skinned && m.hoverY > 0.05f;
        const float breathe = flyerPose ? 0.0f
            : std::sin(e.animTime * 1.8f + e.bobPhase) * (1.0f - locoWE) * 0.012f;
        const float sc = (skinned ? m.worldScale * bossScale * wobble
                                  : styleFor(e.kind).scale * bossScale * wobble) * (1.0f + breathe);
        const bool staticEnemyPose = skinned && !m.multiClip && m.clip < 0;
        const float walkCycle = e.walkPhase * TwoPi + e.bobPhase;
        const float walkSin = std::sin(walkCycle);
        const float walkCos = std::cos(walkCycle);
        const float walkStep = std::fabs(walkSin);
        const float motionScale = e.kind == EnemyKind::Tank ? 0.72f : (e.kind == EnemyKind::Rusher || e.kind == EnemyKind::Stalker ? 1.22f : 1.0f);
        const float follow01 = (staticEnemyPose && e.struck && e.recover > 0.0f)
            ? clamp(e.recover / (e.kind == EnemyKind::Ranged ? 0.5f : std::max(0.2f, tunables_.enemyMeleeRecover)), 0.0f, 1.0f)
            : 0.0f;
        const float strike01 = staticEnemyPose ? ((e.lungeTime > 0.0f) ? 1.0f : follow01 * 0.55f) : 0.0f;
        const float walkLift = staticEnemyPose ? std::min(0.22f, 0.060f * m.worldHeight * bossScale) *
            walkStep * locoWE * motionScale : 0.0f;
        const float hoverLift = staticEnemyPose ? std::sin(e.animTime * 2.25f + e.bobPhase) *
            (e.kind == EnemyKind::Ranged ? 0.025f : 0.014f) * m.worldHeight * bossScale : 0.0f;
        // Static Meshy bodies have no bones, so the root carries the life: light stride bounce,
        // wind-up crouch/brace, and a subtle horizontal squash sell motion without changing hitboxes.
        const float heightStretch = staticEnemyPose
            ? 1.0f - 0.055f * locoWE * walkStep - 0.075f * windup + 0.055f * strike01
            : 1.0f;
        const float widthSquash = staticEnemyPose
            ? 1.0f + 0.038f * locoWE * walkStep + 0.070f * windup - 0.030f * strike01
            : 1.0f;
        float groundY = skinned
            ? (roomFloorY_ - m.footY * sc + m.hoverY)
            : ((e.kind == EnemyKind::Tank ? 0.50f : 0.52f) + 0.05f * std::sin(shakeTime_ * 2.6f + e.bobPhase));
        if (flyerPose) {
            groundY += std::sin(e.animTime * 1.35f + e.bobPhase) * 0.022f * m.worldHeight * bossScale;
        }
        if (staticEnemyPose) groundY += walkLift + hoverLift - windup * 0.070f * m.worldHeight * bossScale;
        // Hit flinch: tilt the body BACK (away from facing = away from the shooter) on a hit, pivoting
        // at the feet, decaying with hitPunch - a readable stagger on top of the scale pop.
        const float attackLean = staticEnemyPose ? (windup * 0.20f - strike01 * 0.26f) : 0.0f;
        const float leanA = clamp(e.hitPunch * 0.26f + attackLean, -0.32f, 0.42f);
        const float sideLean = staticEnemyPose ? walkSin * 0.130f * locoWE * motionScale : 0.0f;
        const Vec3f upV = (leanA > 0.001f || std::fabs(sideLean) > 0.001f)
            ? normalize3(Vec3f{ rr.x * sideLean - ff.x * std::sin(leanA), std::cos(leanA), rr.y * sideLean - ff.y * std::sin(leanA) })
            : Vec3f{ 0.0f, 1.0f, 0.0f };
        const Vec3f fwV = leanA > 0.001f
            ? Vec3f{ ff.x * std::cos(leanA), std::sin(leanA), ff.y * std::cos(leanA) }
            : Vec3f{ ff.x, 0.0f, ff.y };
        const float visualForward = staticEnemyPose
            ? (walkCos * 0.035f * locoWE * motionScale - windup * 0.050f + strike01 * 0.145f) * m.worldHeight * bossScale
            : 0.0f;
        const float visualSide = staticEnemyPose
            ? walkSin * 0.030f * locoWE * motionScale * m.worldHeight * bossScale
            : 0.0f;
        Vec2 visualPos = e.pos + ff * visualForward + rr * visualSide;
        if (collides(visualPos, enemyCollisionRadius(e))) {
            visualPos = e.pos;
        }
        const Mat4 xf = basis({ rr.x * widthSquash, 0.0f, rr.y * widthSquash },
                              { upV.x * heightStretch, upV.y * heightStretch, upV.z * heightStretch },
                              { fwV.x * widthSquash, fwV.y * widthSquash, fwV.z * widthSquash },
                              { visualPos.x, groundY, visualPos.y }, sc);
        const float hurt = e.hurtTimer > 0.0f ? 0.35f : 0.0f;
        const float flare = 0.65f + 0.35f * std::sin(shakeTime_ * 26.0f);
        // Elite/boss colour cue so the threat reads at a glance (Phase D). Tints
        // multiply the textured base colour, so neutral enemies stay (1,1,1).
        Vec4f tint{ 1.0f + hurt + windup * 0.9f, 1.0f - hurt * 0.25f - windup * 0.15f, 1.0f - hurt * 0.25f - windup * 0.4f, 1.0f };
        float emissive = std::max(hurt * 0.5f, windup * (0.55f + 0.6f * flare));
        switch (e.kind) {
            case EnemyKind::Rusher:
                tint = { tint.x * 0.16f, tint.y * 0.24f, tint.z * 0.16f, 1.0f };
                emissive += 0.005f;
                break;
            case EnemyKind::Ranged:
                tint = { tint.x * 1.18f, tint.y * 0.92f, tint.z * 1.10f, 1.0f };
                emissive += 0.06f;
                break;
            case EnemyKind::Tank:
                tint = { tint.x * 1.16f, tint.y * 1.08f, tint.z * 0.94f, 1.0f };
                emissive += 0.05f;
                break;
            case EnemyKind::Stalker:
                tint = { tint.x * 0.72f, tint.y * 0.52f, tint.z * 0.48f, 1.0f };
                emissive += 0.035f;
                break;
        }
        switch (e.affix) {
            case EliteAffix::Fast:     tint = { tint.x * 1.3f, tint.y * 1.25f, tint.z * 0.5f, 1.0f }; emissive += 0.25f; break;
            case EliteAffix::Shielded: tint = { tint.x * 0.5f, tint.y * 0.8f, tint.z * 1.5f, 1.0f }; emissive += 0.25f; break;
            case EliteAffix::Volatile: tint = { tint.x * 1.5f, tint.y * 0.55f, tint.z * 0.3f, 1.0f }; emissive += 0.4f + 0.3f * flare; break;
            case EliteAffix::Regen:    tint = { tint.x * 0.5f, tint.y * 1.4f, tint.z * 0.6f, 1.0f }; emissive += 0.25f; break;
            case EliteAffix::None: default: break;
        }
        // Boss: a warm/red cast that still lets the skeleton's bone + cloth read,
        // with a strong flare on the telegraph so the slam tell stays loud.
        if (e.boss) { tint = { 1.35f, 0.80f + windup * 0.45f, 0.74f, 1.0f }; emissive = 0.12f + windup * 0.85f; }
        // Neon Ink Brutalism: uniform polished-obsidian body (charcoal-violet). REPLACE the
        // per-kind tints rather than multiply them - compounding their dark multipliers (e.g. the
        // Rusher's 0.16x) crushed the Meshy obsidian texture to near-black. This base is bright
        // enough that the facets + surface read; the hurt/windup flash carries the threat
        // without competing with status colors.
        // Grounded direction: enemies ARE gameplay, so the readable body + attack-flash threat read
        // applies in EVERY scene (the raw per-kind textures read as toys); the boss keeps its own
        // per-scene identity outside the brutalist arena.
        // Cohesive Quaternius enemies carry their own sci-fi textures, so keep the body tint
        // NEAR-NEUTRAL - show the art and let the engine lighting + grade do the work. Only the
        // hit/windup flash spikes it. Element/status VFX now carry the colored gameplay read.
        // (Charcoal-crush + de-chibi band-aids retired with the toy meshes.)
        const bool obsidianBody = true;   // all enemies get the neon rim + core grounding cues
        {
            const float flash = hurt * 0.55f + windup * 0.40f;
            const float hp = hurt * 1.4f;   // hit pop: briefly whiten the whole body so shots read as connected
            tint = { 1.0f + flash + hp, 1.0f + hp, 1.0f + hp, 1.0f };
        }

        // Element read: layer status colors instead of picking one winner. A multi-element target
        // should look unstable because that is where pair reactions can fire.
        {
            const StatusState& st = e.status;
            const float burn01 = clamp(st.burn / 6.0f, 0.0f, 1.0f);
            const float shock01 = clamp(st.shock / stat::kShockThreshold, 0.0f, 1.0f);
            const float chill01 = clamp(st.chill, 0.0f, 1.0f);
            const float freeze01 = st.frozen > 0.0f ? clamp(st.frozen / stat::kFreezeDuration, 0.0f, 1.0f) : 0.0f;
            const float corrode01 = clamp(st.corrode / 6.0f, 0.0f, 1.0f);
            const float combo01 = e.comboTimer > 0.0f ? clamp(e.comboTimer / 0.42f, 0.0f, 1.0f) : 0.0f;
            if (burn01 > 0.0f) {
                tint = { tint.x + 0.72f * burn01, tint.y * (1.0f - 0.12f * burn01), tint.z * (1.0f - 0.36f * burn01), 1.0f };
                emissive += (0.24f + 0.18f * flare) * burn01;
            }
            if (corrode01 > 0.0f) {
                tint = { tint.x * (1.0f - 0.16f * corrode01), tint.y + 0.50f * corrode01, tint.z * (1.0f - 0.10f * corrode01), 1.0f };
                emissive += 0.20f * corrode01;
            }
            if (chill01 > 0.0f) {
                tint = { tint.x * (1.0f - 0.20f * chill01), tint.y + 0.18f * chill01, tint.z + 0.50f * chill01, 1.0f };
                emissive += 0.10f * chill01;
            }
            if (freeze01 > 0.0f) {
                tint = { tint.x * 0.50f, tint.y * (0.86f + 0.12f * freeze01), tint.z + 0.85f * freeze01, 1.0f };
                emissive += 0.34f * freeze01;
            }
            if (shock01 > 0.0f) {
                const float flicker = 0.55f + 0.45f * std::sin(shakeTime_ * 46.0f + e.bobPhase);
                tint = { tint.x + 0.12f * shock01 * flicker, tint.y + 0.34f * shock01 * flicker, tint.z + 0.48f * shock01 * flicker, 1.0f };
                emissive += 0.38f * shock01 * flicker;
            }
            if (combo01 > 0.0f) {
                tint = { tint.x + 0.72f * combo01, tint.y + 0.30f * combo01, tint.z + 0.92f * combo01, 1.0f };
                emissive += 0.65f * combo01;
            }
        }

        // Keep the base enemy model visually neutral so elemental effects own the color read.
        // Rim is zero at rest; hurt flashes white, windup flashes warm red. The focused
        // chest core below stays magenta, but the full body does not glow magenta.
        const Vec3f rimCol = Vec3f{ hurt * 1.00f + windup * 1.28f, hurt * 0.90f + windup * 0.22f, hurt * 0.92f + windup * 0.08f };
        const float rimPow = 7.0f;   // very tight edge, and only visible while flashing
        const int visualIdx = e.boss ? -1 : enemyVisualIndex(e);
        const int bossIdx = std::clamp(e.bossKind, 0, BossKindCount - 1);
        const size_t poseSlot = e.boss ? bossSlot[static_cast<size_t>(bossIdx)]
                                       : visualSlot[static_cast<size_t>(visualIdx)];
        AnimatedEnemyInstance* inst = skinned
            ? poseAnimatedEnemy(engine, e, poseSlot, ff) : nullptr;
        if (inst) {
            if (e.boss) ++bossSlot[static_cast<size_t>(bossIdx)];
            else ++visualSlot[static_cast<size_t>(visualIdx)];
            for (size_t s = 0; s < inst->meshes.size(); ++s) {
                if (s < m.submeshVisible.size() && !m.submeshVisible[s]) continue; // gun hidden
                const int matIdx = s < m.submeshMaterial.size() ? m.submeshMaterial[s] : -1;
                const MaterialHandle mat = (matIdx >= 0 && matIdx < static_cast<int>(m.materials.size()))
                    ? m.materials[static_cast<size_t>(matIdx)] : matUntextured_;
                addDraw(eId + s, inst->meshes[s], mat, xf, tint, emissive, false, rimCol, rimPow);
            }
        } else {
            addDraw(eId, gpuEnemyMeshes_[static_cast<size_t>(e.kind)], matUntextured_, xf, tint, emissive, false, rimCol, rimPow);
        }
        // Enemy effect anchor. The focused chest core stays as the enemy identity cue; the body
        // tint/rim above stays neutral so statuses and combos own the large color read.
        if (obsidianBody) {
            const EnemyLightAnchor anchor = enemyLightAnchor(e.kind, e.boss, e.bossKind);
            const float bodyH = m.worldHeight * bossScale * wobble * (1.0f + breathe);
            const Vec3f coreRight{ rr.x * widthSquash, 0.0f, rr.y * widthSquash };
            const Vec3f coreUp{ upV.x * heightStretch, upV.y * heightStretch, upV.z * heightStretch };
            const Vec3f coreForward{ fwV.x * widthSquash, fwV.y * widthSquash, fwV.z * widthSquash };
            const Vec3f coreBase{ e.pos.x, groundY, e.pos.y };
            const Vec3f corePos = coreBase
                + coreRight * (anchor.right * bodyH)
                + coreUp * (anchor.height * bodyH)
                + coreForward * (anchor.forward * bodyH);
            const float corePulse = 1.0f + 0.10f * std::sin(shakeTime_ * 7.0f + e.bobPhase);
            Particle core;
            core.center   = corePos;
            core.size     = anchor.size * bodyH * corePulse;
            core.color    = { 1.0f, 0.45f, 0.80f };
            core.emissive = 2.6f * corePulse;
            core.velocity = { 0.0f, 0.0f, 0.0f };
            core.stretch  = 0.0f;
            enemyCoreRender_.push_back(core);
            activeEnemyLights.push_back({
                corePos,
                e.boss ? Vec3f{ 1.95f, 0.30f, 1.05f } : Vec3f{ 1.45f, 0.28f, 0.90f },
                ((e.boss ? 1.45f : 0.48f) + hurt * 0.30f) * corePulse,
                e.boss ? std::max(4.0f, bodyH * 0.78f) : clamp(bodyH * 0.88f, 1.4f, 2.7f)
            });

            const float threatGlow = clamp(hurt * 0.80f + windup * 1.05f, 0.0f, 1.0f);
            if (threatGlow > 0.02f) {
                Particle threat;
                threat.center   = corePos;
                threat.size     = anchor.size * bodyH * corePulse * (0.92f + 0.38f * threatGlow);
                threat.color    = { 1.65f, 0.22f + hurt * 0.62f, 0.10f + hurt * 0.55f };
                threat.emissive = (1.3f + 0.85f * windup) * corePulse * threatGlow;
                threat.velocity = { 0.0f, 0.0f, 0.0f };
                threat.stretch  = 0.0f;
                enemyCoreRender_.push_back(threat);
                activeEnemyLights.push_back({
                    corePos,
                    { 1.70f, 0.22f + hurt * 0.58f, 0.10f + hurt * 0.52f },
                    ((e.boss ? 1.25f : 0.45f) + windup * 0.65f + hurt * 0.35f) * corePulse * threatGlow,
                    e.boss ? std::max(3.8f, bodyH * 0.72f) : clamp(bodyH * 0.82f, 1.3f, 2.6f)
                });
            }

            const StatusState& st = e.status;
            if (st.any() || e.comboTimer > 0.0f) {
                const float burn01 = clamp(st.burn / 6.0f, 0.0f, 1.0f);
                const float shock01 = clamp(st.shock / stat::kShockThreshold, 0.0f, 1.0f);
                const float chill01 = std::max(clamp(st.chill, 0.0f, 1.0f),
                                               st.frozen > 0.0f ? 0.85f : 0.0f);
                const float corrode01 = clamp(st.corrode / 6.0f, 0.0f, 1.0f);
                const float combo01 = e.comboTimer > 0.0f ? clamp(e.comboTimer / 0.42f, 0.0f, 1.0f) : 0.0f;
                Vec3f lightCol{};
                float lightN = 0.0f;
                const auto addLight = [&](Vec3f c, float w) {
                    lightCol = { lightCol.x + c.x * w, lightCol.y + c.y * w, lightCol.z + c.z * w };
                    lightN += w;
                };
                if (burn01 > 0.0f) addLight(elementTint(Element::Burn), burn01);
                if (shock01 > 0.0f) addLight(elementTint(Element::Shock), shock01);
                if (chill01 > 0.0f) addLight(elementTint(Element::Cryo), chill01);
                if (corrode01 > 0.0f) addLight(elementTint(Element::Corrode), corrode01);
                if (combo01 > 0.0f) addLight({ 1.45f, 0.64f, 2.10f }, combo01 * 1.4f);
                if (lightN > 0.0f) {
                    const float inv = 1.0f / lightN;
                    activeEnemyLights.push_back({
                        corePos,
                        { lightCol.x * inv, lightCol.y * inv, lightCol.z * inv },
                        (0.40f + 0.75f * combo01) * (e.boss ? 1.45f : 1.0f),
                        clamp(bodyH * (0.95f + 0.75f * combo01), 1.8f, e.boss ? 5.6f : 3.3f)
                    });
                }

                const auto pushMote = [&](Vec3f at, Vec3f col, float size, float em, Vec3f vel = {}) {
                    Particle mte;
                    mte.center = at;
                    mte.size = size;
                    mte.color = col;
                    mte.emissive = em;
                    mte.velocity = vel;
                    mte.stretch = (std::fabs(vel.x) + std::fabs(vel.y) + std::fabs(vel.z)) > 0.0001f ? 0.16f : 0.0f;
                    enemyCoreRender_.push_back(mte);
                };
                const auto ringMotes = [&](Element elem, float amount, int count, float phase, float height, float spin) {
                    if (amount <= 0.02f) return;
                    const Vec3f col = elementTint(elem);
                    const int n = std::max(1, static_cast<int>(std::lround(static_cast<float>(count) * (0.45f + 0.55f * amount))));
                    for (int k = 0; k < n; ++k) {
                        const float fr = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
                        const float a = e.bobPhase + phase + fr * TwoPi + shakeTime_ * spin;
                        const float rad = bodyH * (0.20f + 0.08f * amount + 0.03f * std::sin(a * 2.1f));
                        const float y = groundY + bodyH * (height + 0.10f * std::sin(a * 1.7f + phase));
                        pushMote({ e.pos.x + std::cos(a) * rad, y, e.pos.y + std::sin(a) * rad },
                                 col, bodyH * (0.035f + 0.025f * amount), 1.6f + amount * 1.8f);
                    }
                };
                ringMotes(Element::Burn, burn01, 5, 0.0f, 0.44f, 2.4f);
                ringMotes(Element::Shock, shock01, 5, 1.6f, 0.66f, 6.4f);
                ringMotes(Element::Cryo, chill01, 4, 3.1f, 0.24f, -1.2f);
                ringMotes(Element::Corrode, corrode01, 4, 4.5f, 0.34f, 0.9f);
                if (burn01 > 0.02f) {
                    const Vec3f col = elementTint(Element::Burn);
                    const int n = e.boss ? 12 : 7;
                    for (int k = 0; k < n; ++k) {
                        float f = shakeTime_ * (0.72f + burn01 * 0.10f) + static_cast<float>(k) * 0.173f + e.bobPhase * 0.05f;
                        f -= std::floor(f);
                        const float a = e.bobPhase + static_cast<float>(k) * 2.399f + shakeTime_ * 0.62f;
                        const float rad = bodyH * (0.08f + 0.13f * (1.0f - f));
                        pushMote({ e.pos.x + std::cos(a) * rad,
                                   groundY + bodyH * (0.22f + 0.68f * f),
                                   e.pos.y + std::sin(a) * rad },
                                 col,
                                 bodyH * (0.024f + 0.035f * (1.0f - f)) * (0.65f + 0.35f * burn01),
                                 2.2f + burn01 * 2.1f,
                                 { std::cos(a) * 0.05f, 0.62f + burn01 * 0.28f, std::sin(a) * 0.05f });
                    }
                }
                if (shock01 > 0.02f) {
                    const Vec3f col = elementTint(Element::Shock);
                    const int n = e.boss ? 10 : 6;
                    for (int k = 0; k < n; ++k) {
                        const float fr = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
                        const float a = e.bobPhase + fr * TwoPi + shakeTime_ * 12.0f;
                        const float side = ((k & 1) ? 1.0f : -1.0f) * bodyH * (0.16f + 0.08f * shock01);
                        const float forward = std::sin(a * 0.7f) * bodyH * 0.08f;
                        pushMote({ e.pos.x + std::cos(a) * forward + std::sin(a) * side,
                                   groundY + bodyH * (0.42f + 0.42f * fr),
                                   e.pos.y + std::sin(a) * forward - std::cos(a) * side },
                                 (k % 3 == 0) ? Vec3f{ 0.85f, 1.95f, 2.35f } : col,
                                 bodyH * (0.024f + 0.018f * shock01),
                                 3.2f + shock01 * 2.4f,
                                 { std::cos(a) * 0.85f, 0.02f, std::sin(a) * 0.85f });
                    }
                }
                if (chill01 > 0.02f) {
                    const Vec3f col = elementTint(Element::Cryo);
                    const int n = e.boss ? 11 : 7;
                    for (int k = 0; k < n; ++k) {
                        const float fr = static_cast<float>(k) / static_cast<float>(n);
                        const float a = e.bobPhase + fr * TwoPi + shakeTime_ * -0.55f;
                        const float rad = bodyH * (0.19f + 0.06f * std::sin(a * 1.8f));
                        const float tier = static_cast<float>(k % 3) / 2.0f;
                        pushMote({ e.pos.x + std::cos(a) * rad,
                                   groundY + bodyH * (0.045f + 0.13f * tier),
                                   e.pos.y + std::sin(a) * rad },
                                 (k & 1) ? Vec3f{ 0.82f, 1.85f, 2.30f } : col,
                                 bodyH * (0.030f + 0.020f * chill01),
                                 1.7f + chill01 * 1.8f);
                    }
                    if (st.frozen > 0.0f) {
                        pushMote({ e.pos.x, groundY + bodyH * 0.56f, e.pos.y },
                                 { 0.72f, 1.72f, 2.28f },
                                 bodyH * 0.11f,
                                 2.6f);
                    }
                }
                if (corrode01 > 0.02f) {
                    const Vec3f col = elementTint(Element::Corrode);
                    const int n = e.boss ? 10 : 6;
                    for (int k = 0; k < n; ++k) {
                        float f = shakeTime_ * (0.62f + corrode01 * 0.10f) + static_cast<float>(k) * 0.211f + e.bobPhase * 0.03f;
                        f -= std::floor(f);
                        const float a = e.bobPhase + static_cast<float>(k) * 1.91f;
                        const float rad = bodyH * (0.14f + 0.06f * std::sin(a + f * TwoPi));
                        pushMote({ e.pos.x + std::cos(a) * rad,
                                   groundY + bodyH * (0.76f - 0.62f * f),
                                   e.pos.y + std::sin(a) * rad },
                                 (k & 1) ? Vec3f{ 0.88f, 2.15f, 0.42f } : col,
                                 bodyH * (0.026f + 0.032f * f),
                                 2.0f + corrode01 * 2.0f,
                                 { std::cos(a) * 0.04f, -0.54f, std::sin(a) * 0.04f });
                        if (f > 0.76f) {
                            pushMote({ e.pos.x + std::cos(a) * rad * 1.15f,
                                       groundY + bodyH * 0.035f,
                                       e.pos.y + std::sin(a) * rad * 1.15f },
                                     col,
                                     bodyH * (0.040f + 0.018f * corrode01),
                                     1.6f + corrode01);
                        }
                    }
                }
                if (combo01 > 0.0f) {
                    const int n = e.boss ? 12 : 8;
                    for (int k = 0; k < n; ++k) {
                        const float fr = static_cast<float>(k) / static_cast<float>(n);
                        const float a = e.bobPhase + fr * TwoPi + shakeTime_ * 7.5f;
                        const float rad = bodyH * (0.30f + 0.12f * combo01);
                        pushMote({ e.pos.x + std::cos(a) * rad,
                                   groundY + bodyH * (0.50f + 0.18f * std::sin(a * 1.4f)),
                                   e.pos.y + std::sin(a) * rad },
                                 { 1.45f, 0.64f, 2.10f },
                                 bodyH * (0.055f + 0.035f * combo01),
                                 3.2f * combo01,
                                 { std::cos(a) * 0.45f, 0.0f, std::sin(a) * 0.45f });
                    }
                }
            }

            // Shadow-smoke aura DISABLED: it read as an ethereal "made of shadow" creature, which is
            // wrong for the metal robots (looked like floating blobs around a machine). The dark inked
            // body + hit/telegraph flash + emissive optics carry the threat instead.
            const int nPuff = 0;
            for (int k = 0; k < nPuff; ++k) {
                const float fr = (static_cast<float>(k) + 0.5f) / static_cast<float>(nPuff); // 0 feet .. head/plume
                const float ang = fr * 11.0f + e.bobPhase + shakeTime_ * 0.7f;               // spiral up the body
                const float rad = (0.14f + 0.16f * std::sin(ang * 1.7f + fr * 3.0f)) * bodyH;
                const float drift = 0.14f * bodyH * std::sin(shakeTime_ * 0.9f + static_cast<float>(k));
                Particle pf;
                pf.center = { e.pos.x + std::cos(ang) * rad,
                              groundY + fr * bodyH * 1.22f + drift,            // taller -> rises into a head plume
                              e.pos.y + std::sin(ang) * rad };
                pf.size = (0.32f + 0.34f * fr) * bodyH;                        // bigger puffs -> more dissolve
                pf.color = { 0.05f, 0.045f, 0.10f };                          // deep indigo shadow
                pf.emissive = 0.26f * (0.6f + 0.4f * std::sin(ang * 1.3f));    // per-puff opacity (reused field)
                pf.velocity = { 0.0f, 0.0f, 0.0f };
                pf.stretch = 0.0f;
                enemySmokeRender_.push_back(pf);
            }
        }
    }

    // Death-corpses: play the death clip + dissolve, reusing each visual's pose pool
    // after live enemies of the same visual have claimed their slots.
    for (const EnemyCorpse& c : corpses_) {
        const AnimatedEnemyModel& m = enemyRenderModel(c);
        if (!m.loaded || !m.gpuReady) continue;
        const int visualIdx = c.boss ? -1 : enemyVisualIndex(c);
        const int bossIdx = std::clamp(c.bossKind, 0, BossKindCount - 1);
        const size_t poseSlot = c.boss ? bossSlot[static_cast<size_t>(bossIdx)]
                                       : visualSlot[static_cast<size_t>(visualIdx)];
        AnimatedEnemyInstance* inst = poseDeadEnemy(engine, c, poseSlot);
        if (!inst) continue;
        if (c.boss) ++bossSlot[static_cast<size_t>(bossIdx)];
        else ++visualSlot[static_cast<size_t>(visualIdx)];
        const float faceA = std::atan2(c.facing.y, c.facing.x) + m.yawOffset;
        const Vec2 ff{ std::cos(faceA), std::sin(faceA) };
        const Vec2 rr = rightFromForward(ff);
        // Dissolve over the last 0.4s by sinking into the floor (works in the opaque pass). A falling
        // flyer corpse rides its fallY above the floor and tumbles (pitch about the right axis).
        const float fade = clamp((c.dur - c.age) / 0.4f, 0.0f, 1.0f);
        const float groundY = roomFloorY_ - m.footY * c.scale + std::max(0.0f, c.fallY) - (1.0f - fade) * 0.6f;
        Vec3f upV{ 0.0f, 1.0f, 0.0f }, fwV{ ff.x, 0.0f, ff.y };
        if (c.fall && c.spin > 0.001f) {
            const float s = std::sin(c.spin), cs = std::cos(c.spin);
            upV = { ff.x * s, cs, ff.y * s };
            fwV = { ff.x * cs, -s, ff.y * cs };
        }
        const Mat4 xf = basis({ rr.x, 0.0f, rr.y }, upV, fwV, { c.pos.x, groundY, c.pos.y }, c.scale);
        // Match the LIVE inked robots: show the enemy's own texture (m.materials), just dimmed to read
        // as inert/powered-down metal - NOT the old dark-grey crush + magenta rim (that was the stale
        // shadow-creature corpse). The ink outline pass still draws its black silhouette; the dissolve
        // sinks + fades it. No hostile rim (it is dead).
        const Vec4f tint = Vec4f{ 0.80f, 0.80f, 0.85f, 1.0f };
        const Vec3f rimCol = Vec3f{ 0.0f, 0.0f, 0.0f };
        const float rimPow = 0.0f;
        const uint64_t cId = 8000 + static_cast<uint64_t>(&c - corpses_.data()) * 16;
        for (size_t s = 0; s < inst->meshes.size(); ++s) {
            if (s < m.submeshVisible.size() && !m.submeshVisible[s]) continue;
            const int matIdx = s < m.submeshMaterial.size() ? m.submeshMaterial[s] : -1;
            const MaterialHandle mat = (matIdx >= 0 && matIdx < static_cast<int>(m.materials.size()))
                ? m.materials[static_cast<size_t>(matIdx)] : matUntextured_;
            addDraw(cId + s, inst->meshes[s], mat, xf, tint, 0.0f, false, rimCol, rimPow);
        }
    }

    for (const Projectile& pr : projectiles_) {
        if (!pr.active) continue;
        const uint64_t prId = 5000 + static_cast<uint64_t>(&pr - projectiles_.data());
        const float r = std::max(0.05f, tunables_.enemyProjectileRadius);
        const float pulse = 1.0f + 0.12f * std::sin(pr.age * 30.0f);
        if (pr.hostile) {
            // Returnal-tier energy bolt: a SOLID bright orb core that blooms to a white-hot
            // centre, wrapped in a layered additive glow aura, dragging a CONTINUOUS tapered
            // comet trail. All the glow/trail are additive billboards pushed into
            // enemyCoreRender_ (composited post-TAA into HDR -> they bloom and never ghost),
            // so a fan/burst reads as a spread of distinct blooming orbs (the bullet-hell look),
            // not the flat dotted slash the old 2D projection produced. The trail is rebuilt at
            // render-rate (not shed per sim frame), so it is always a smooth ribbon, never beads.
            const Vec2 vdir2 = lengthSq(pr.vel) > 1e-4f ? normalize(pr.vel) : Vec2{ 0.0f, 1.0f };
            const Vec3f vdir{ vdir2.x, 0.0f, vdir2.y };
            const Vec3f head{ pr.pos.x, pr.height, pr.pos.y };
            const Vec3f col = pr.color;
            // Per-orb visual variation (deterministic hash from the fire origin + direction): desync
            // the pulse and jitter the size so a stream/ring of orbs reads as distinct living energy,
            // not a mechanical row of identical balls. Render-only, so the sim stays byte-identical.
            float h = std::sin(pr.origin.x * 12.9898f + pr.origin.y * 78.233f
                             + pr.vel.x * 37.719f + pr.vel.y * 11.135f) * 43758.5453f;
            h -= std::floor(h);                                  // [0,1)
            const float orbPulse = 1.0f + 0.13f * std::sin(pr.age * 30.0f + h * TwoPi);
            // Boss orbs render BIGGER + hotter for set-piece punch and to read at arena range. This is
            // VISUAL ONLY - the hitbox stays tunables_.enemyProjectileRadius, so a big boss orb stays
            // fair/forgiving (the "visual > hitbox" rule the orbs already follow).
            const float bossBoost = pr.sourceBoss ? 1.8f : 1.0f;
            const float emB       = pr.sourceBoss ? 1.30f : 1.0f;
            const float vr        = r * bossBoost * (0.9f + 0.2f * h);   // jittered visual radius
            // White-hot core that KEEPS its hue: multiplying the chosen semantic colour preserves
            // orange/violet/crimson reads, instead of pushing every hostile shot toward white.
            const Vec3f hot = hotHue(col, pr.sourceBoss ? 1.30f : 1.26f, 2.8f);
            const auto pushGlow = [&](Vec3f at, float size, Vec3f c, float emis) {
                Particle g; g.center = at; g.size = size; g.color = c; g.emissive = emis;
                g.velocity = { 0.0f, 0.0f, 0.0f }; g.stretch = 0.0f;
                enemyCoreRender_.push_back(g);
            };
            // Tight brightness budget (only TWO head billboards + a solid ball), so even a head-on
            // orb at point-blank stays a defined saturated orb that blooms, not a screen-filling
            // white wash. The solid sphere gives the round silhouette; the halo gives the bloom.
            const float vis = vr * 1.05f * orbPulse;
            const Mat4 oxf = mul(scaling(vis, vis, vis), translation(head.x, head.y, head.z));
            addDraw(prId, gpuEnergyOrbMesh_, matUntextured_, oxf, { col.x, col.y, col.z, 1.0f }, 1.9f * emB);
            pushGlow(head, vr * 2.0f * orbPulse, col, 1.15f * emB);   // soft outer glow (blooms in-hue)
            pushGlow(head, vr * 0.5f * orbPulse, hot, 2.4f * emB);    // tight white-hot core point
            // Comet TAIL via a velocity-stretched billboard, NOT a row of motes. A stretched quad
            // elongates along the orb's on-SCREEN motion and auto-rounds when it flies straight at
            // the camera (v2l -> 0), so the tail is one continuous streak at every view angle -
            // never the chain-of-equal-balls a discrete-mote trail produces when seen side-on.
            const auto pushStreak = [&](float size, float emis, float stretch) {
                Particle s; s.center = { head.x - vdir.x * vr * 1.1f, head.y, head.z - vdir.z * vr * 1.1f };
                s.size = size; s.color = col; s.emissive = emis;
                s.velocity = { pr.vel.x, 0.0f, pr.vel.y }; s.stretch = stretch;
                enemyCoreRender_.push_back(s);
            };
            pushStreak(vr * 1.55f, 0.70f * emB, 0.30f);   // wide soft glow tail
            pushStreak(vr * 0.85f, 1.55f * emB, 0.34f);   // bright inner streak

            // Semantic particle shedding. The orb's solid head/trail does the readability work;
            // these small motes communicate attack behaviour: orange fans throw wide ember chips,
            // violet bursts crackle rapidly, standard magenta orbs spiral, and bosses add an
            // element-flavoured corona. Deterministic render-only math keeps gameplay untouched.
            const Vec3f side{ -vdir.z, 0.0f, vdir.x };
            const auto pushAttackMote = [&](Vec3f at, Vec3f c, float size, float emis, Vec3f vel, float stretch) {
                Particle m; m.center = at; m.size = size; m.color = c; m.emissive = emis;
                m.velocity = vel; m.stretch = stretch;
                enemyCoreRender_.push_back(m);
            };
            const bool fanLike = pr.sourceAttack == EnemyAttack::Fan || (pr.sourceBoss && pr.sourceBossKind == 1);
            const bool burstLike = pr.sourceAttack == EnemyAttack::Burst || (pr.sourceBoss && pr.sourceBossKind == 2);
            const bool voltLike = pr.sourceBoss && pr.sourceBossKind == 0;
            const int moteCount = pr.sourceBoss ? 7 : (burstLike ? 5 : fanLike ? 4 : 3);
            const float tempo = burstLike ? 48.0f : (fanLike ? 24.0f : 32.0f);
            for (int i = 0; i < moteCount; ++i) {
                const float fi = static_cast<float>(i);
                const float ph = pr.age * tempo + h * TwoPi + fi * 2.399963f;
                const float back = vr * (1.35f + 0.48f * fi);
                const float sideScale = fanLike ? 0.95f : (burstLike ? 0.70f : 0.48f);
                const float sideAmt = std::sin(ph) * vr * sideScale;
                const float lift = std::cos(ph * 0.73f) * vr * (burstLike ? 0.42f : 0.28f)
                                 - (fanLike ? vr * 0.16f * static_cast<float>(i & 1) : 0.0f);
                const Vec3f at{
                    head.x - vdir.x * back + side.x * sideAmt,
                    std::max(roomFloorY_ + 0.05f, head.y + lift),
                    head.z - vdir.z * back + side.z * sideAmt
                };
                const Vec3f moteCol = ((i + static_cast<int>(pr.sourceAttack)) % 3 == 0) ? hot : col;
                const float size = vr * (burstLike ? 0.24f : fanLike ? 0.34f : 0.28f) *
                                   (0.88f + 0.18f * std::sin(ph * 1.3f));
                const float lateral = fanLike ? 1.25f : (burstLike || voltLike ? 1.55f : 0.65f);
                const Vec3f vel{
                    pr.vel.x * (burstLike ? 0.22f : 0.12f) + side.x * std::cos(ph) * lateral,
                    fanLike ? -0.28f : (burstLike ? 0.18f : 0.05f),
                    pr.vel.y * (burstLike ? 0.22f : 0.12f) + side.z * std::cos(ph) * lateral
                };
                pushAttackMote(at, moteCol, std::max(0.018f, size), (burstLike ? 2.55f : fanLike ? 2.15f : 1.85f) * emB,
                               vel, burstLike ? 0.32f : 0.20f);
            }
            if (fanLike) {
                for (int i = 0; i < 2; ++i) {
                    const float sgn = i == 0 ? -1.0f : 1.0f;
                    pushAttackMote({ head.x - vdir.x * vr * 1.85f + side.x * sgn * vr * 0.78f,
                                     std::max(roomFloorY_ + 0.04f, head.y - vr * 0.54f),
                                     head.z - vdir.z * vr * 1.85f + side.z * sgn * vr * 0.78f },
                                   hot, vr * 0.40f, 2.35f * emB,
                                   { side.x * sgn * 1.6f, -0.35f, side.z * sgn * 1.6f }, 0.26f);
                }
            } else if (burstLike || voltLike) {
                for (int i = 0; i < 2; ++i) {
                    const float sgn = i == 0 ? -1.0f : 1.0f;
                    pushAttackMote({ head.x + side.x * sgn * vr * 0.82f - vdir.x * vr * 0.65f,
                                     head.y + vr * (0.18f + 0.14f * static_cast<float>(i)),
                                     head.z + side.z * sgn * vr * 0.82f - vdir.z * vr * 0.65f },
                                   hot, vr * 0.30f, (voltLike ? 3.0f : 2.65f) * emB,
                                   { side.x * sgn * 2.0f, 0.10f, side.z * sgn * 2.0f }, 0.36f);
                }
            }
            // Launch flare: a bright ignition burst at the muzzle for the first ~0.14s of flight, so
            // the bolt visibly LAUNCHES from the enemy. Deterministic + render-only (no rng), so it
            // does not perturb the sim - the reason the muzzle spark spawn was kept out of spawnEnemyShot.
            if (pr.age < 0.14f) {
                const float lf = 1.0f - pr.age / 0.14f;          // 1 at spawn -> 0
                const Vec3f muz{ pr.origin.x + vdir.x * 0.42f, pr.height, pr.origin.y + vdir.z * 0.42f };
                pushGlow(muz, vr * (1.4f + 1.6f * lf), col, 1.4f * lf * emB);
                pushGlow(muz, vr * 0.7f, hot, 2.8f * lf * emB);
            }
        } else {
            const float spin = pr.age * 11.0f;
            const float vis = r * pulse;
            float sx = vis, sy = vis, sz = vis;
            if (pr.effectMask & ShotFxBurn)    { sx *= 1.22f; sy *= 0.92f; sz *= 0.92f; }
            if (pr.effectMask & ShotFxShock)   { sx *= 0.82f; sy *= 1.28f; sz *= 0.82f; }
            if (pr.effectMask & ShotFxCryo)    { sx *= 0.96f; sy *= 1.12f; sz *= 1.10f; }
            if (pr.effectMask & ShotFxCorrode) { sx *= 1.12f; sy *= 0.92f; sz *= 1.18f; }
            const Mat4 rot = rotationYawPitchRoll(spin, spin * 0.7f, 0.0f);
            const Mat4 xf = mul(scaling(sx, sy, sz), mul(rot, translation(pr.pos.x, pr.height, pr.pos.y)));
            const Vec3f col = pr.color;
            addDraw(prId, gpuProjectileMesh_, matUntextured_, xf, { col.x, col.y, col.z, 1.0f },
                    pr.effectMask ? (1.85f + 0.10f * static_cast<float>(shotElementCount(pr.effectMask))) : 1.25f);
            if (pr.effectMask) {
                const Vec3f head{ pr.pos.x, pr.height, pr.pos.y };
                const Vec2 vdir2 = lengthSq(pr.vel) > 1e-4f ? normalize(pr.vel) : Vec2{ 0.0f, 1.0f };
                const Vec3f vdir{ vdir2.x, 0.0f, vdir2.y };
                const Vec3f side{ -vdir.z, 0.0f, vdir.x };
                Particle halo;
                halo.center = head;
                halo.size = r * 1.85f * pulse;
                halo.color = col;
                halo.emissive = 1.45f;
                halo.velocity = { pr.vel.x, 0.0f, pr.vel.y };
                halo.stretch = 0.22f;
                enemyCoreRender_.push_back(halo);
                const auto pushFx = [&](uint32_t bit, Element elem, float phase) {
                    if ((pr.effectMask & bit) == 0) return;
                    const Vec3f ec = elementTint(elem);
                    const float a = pr.age * 18.0f + phase;
                    Particle g;
                    g.center = { head.x + std::cos(a) * r * 0.62f - vdir.x * r * 0.55f,
                                 head.y + std::sin(a * 1.7f) * r * 0.30f,
                                 head.z + std::sin(a) * r * 0.62f - vdir.z * r * 0.55f };
                    g.size = r * 0.54f;
                    g.color = ec;
                    g.emissive = 2.15f;
                    g.velocity = { 0.0f, 0.0f, 0.0f };
                    g.stretch = 0.0f;
                    enemyCoreRender_.push_back(g);
                };
                pushFx(ShotFxBurn, Element::Burn, 0.0f);
                pushFx(ShotFxShock, Element::Shock, 1.4f);
                pushFx(ShotFxCryo, Element::Cryo, 2.8f);
                pushFx(ShotFxCorrode, Element::Corrode, 4.2f);
                const auto pushShotMote = [&](Vec3f at, Vec3f c, float size, float em, Vec3f vel, float stretch) {
                    Particle g;
                    g.center = at;
                    g.size = size;
                    g.color = c;
                    g.emissive = em;
                    g.velocity = vel;
                    g.stretch = stretch;
                    enemyCoreRender_.push_back(g);
                };
                if (pr.effectMask & ShotFxBurn) {
                    const Vec3f c = elementTint(Element::Burn);
                    pushShotMote({ head.x - vdir.x * r * 1.30f, head.y + r * 0.10f, head.z - vdir.z * r * 1.30f },
                                 c, r * 1.05f, 2.55f,
                                 { pr.vel.x * 0.42f, 0.45f, pr.vel.y * 0.42f }, 0.30f);
                    pushShotMote({ head.x - vdir.x * r * 1.70f + side.x * r * 0.25f, head.y - r * 0.02f,
                                   head.z - vdir.z * r * 1.70f + side.z * r * 0.25f },
                                 { 1.95f, 0.58f, 0.16f }, r * 0.62f, 2.0f,
                                 { pr.vel.x * 0.20f, 0.38f, pr.vel.y * 0.20f }, 0.22f);
                }
                if (pr.effectMask & ShotFxShock) {
                    const Vec3f c = elementTint(Element::Shock);
                    const float flick = 0.7f + 0.3f * std::sin(pr.age * 56.0f);
                    pushShotMote({ head.x + side.x * r * 0.72f, head.y + r * 0.18f, head.z + side.z * r * 0.72f },
                                 { 0.88f, 2.05f, 2.45f }, r * 0.46f, 3.6f * flick,
                                 { side.x * 1.0f, 0.0f, side.z * 1.0f }, 0.26f);
                    pushShotMote({ head.x - side.x * r * 0.72f - vdir.x * r * 0.55f, head.y - r * 0.12f,
                                   head.z - side.z * r * 0.72f - vdir.z * r * 0.55f },
                                 c, r * 0.42f, 3.1f * flick,
                                 { -side.x * 1.0f, 0.0f, -side.z * 1.0f }, 0.26f);
                }
                if (pr.effectMask & ShotFxCryo) {
                    const Vec3f c = elementTint(Element::Cryo);
                    for (int k = 0; k < 3; ++k) {
                        const float f = static_cast<float>(k) - 1.0f;
                        pushShotMote({ head.x + side.x * r * 0.46f * f - vdir.x * r * (0.30f + 0.28f * static_cast<float>(k)),
                                       head.y + r * (0.16f - 0.12f * static_cast<float>(k)),
                                       head.z + side.z * r * 0.46f * f - vdir.z * r * (0.30f + 0.28f * static_cast<float>(k)) },
                                     (k == 1) ? Vec3f{ 0.82f, 1.85f, 2.30f } : c,
                                     r * (0.42f + 0.08f * static_cast<float>(k)),
                                     2.1f,
                                     { pr.vel.x * 0.10f, -0.10f, pr.vel.y * 0.10f },
                                     0.08f);
                    }
                }
                if (pr.effectMask & ShotFxCorrode) {
                    const Vec3f c = elementTint(Element::Corrode);
                    pushShotMote({ head.x - vdir.x * r * 0.90f, head.y - r * 0.45f, head.z - vdir.z * r * 0.90f },
                                 c, r * 0.92f, 2.25f,
                                 { pr.vel.x * 0.20f, -0.55f, pr.vel.y * 0.20f }, 0.22f);
                    pushShotMote({ head.x + side.x * r * 0.34f - vdir.x * r * 1.35f, head.y - r * 0.62f,
                                   head.z + side.z * r * 0.34f - vdir.z * r * 1.35f },
                                 { 0.82f, 2.10f, 0.44f }, r * 0.55f, 1.95f,
                                 { 0.0f, -0.42f, 0.0f }, 0.16f);
                }
                if (pr.effectMask & ShotFxLeech) {
                    Particle g;
                    g.center = { head.x - vdir.x * r * 0.95f, head.y - r * 0.20f, head.z - vdir.z * r * 0.95f };
                    g.size = r * 0.68f;
                    g.color = { 0.36f, 1.95f, 0.82f };
                    g.emissive = 2.0f;
                    g.velocity = { pr.vel.x, 0.0f, pr.vel.y };
                    g.stretch = 0.20f;
                    enemyCoreRender_.push_back(g);
                }
            }
        }
    }

    // First-person weapon + hands (camera space).
    const Vec2 forward2 = fromAngle(renderViewYaw_);
    const Vec2 right2 = rightFromForward(forward2);
    const float walk = std::max(0.1f, tunables_.walkSpeed);
    const float strafe = clamp(dot(player_.vel, right2) / walk, -1.0f, 1.0f);
    const float forwardMove = clamp(dot(player_.vel, forward2) / walk, -1.0f, 1.0f);
    const float swayScale = std::max(0.0f, tunables_.weaponViewmodelSway);
    const float bobSway = player_.grounded ? std::sin(cameraBobPhase_) : 0.0f;
    const float bobLift = player_.grounded ? std::fabs(std::sin(cameraBobPhase_ * 2.0f)) : 0.0f;
    const float recoil01 = clamp(fireCameraKick_ / std::max(0.1f, activeWeaponProfile().cameraKick), 0.0f, 1.3f);
    const float gunKick = clamp(weaponKick_ * std::max(0.0f, tunables_.weaponViewmodelKickScale), 0.0f, 1.7f);
    const float gunSide = clamp(weaponKickSide_ * std::max(0.0f, tunables_.weaponViewmodelKickScale), -0.8f, 0.8f);
    const std::string activeWeaponId =
        activeWeapon_ >= 0 && activeWeapon_ < static_cast<int>(loadout_.size())
            ? loadout_[static_cast<size_t>(activeWeapon_)].id
            : std::string("pistol");
    const WeaponProfile& vmProfile = activeWeaponProfile();
    updateActiveWeaponViewmodel(engine);
    const float activeViewmodelFov = vmProfile.viewmodel.fovDegrees;

    // Bumstrum viewmodels are complete authored FPS rigs. Keep this transform layer
    // boring: place the source pose in camera space and add only tiny gameplay sway,
    // instead of re-posing the hands or faking reload parts over the source asset.
    const float authoredSway = 0.55f * swayScale;
    const float weaponScale = vmProfile.viewmodel.scale;
    const float weaponX = vmProfile.viewmodel.x + (-strafe * 0.010f + bobSway * 0.003f - gunSide * 0.005f) * authoredSway;
    // Firing kick punches the gun up + back toward the camera harder for felt weight.
    const float weaponY = vmProfile.viewmodel.y + (bobLift * 0.005f - forwardMove * 0.002f) * authoredSway + gunKick * 0.011f;
    const float weaponZ = vmProfile.viewmodel.z - gunKick * 0.021f;
    const Mat4 sourceCorrection = rotationYawPitchRoll(vmProfile.viewmodel.sourceYaw,
                                                       vmProfile.viewmodel.sourcePitch,
                                                       vmProfile.viewmodel.sourceRoll);
    const Mat4 weaponBase = mul(mul(scaling(weaponScale, weaponScale, weaponScale), sourceCorrection),
                                rotationX(1.5707963f));
    const Mat4 weaponOri = mul(weaponBase,
                               rotationYawPitchRoll(vmProfile.viewmodel.yaw + gunSide * 0.009f,
                                                    vmProfile.viewmodel.pitch - recoil01 * 0.004f - gunKick * 0.024f,
                                                    vmProfile.viewmodel.roll + strafe * 0.002f + gunSide * 0.010f));
    const Mat4 weaponXf = mul(weaponOri, translation(weaponX, weaponY, weaponZ));

    // Project the real barrel-end face through the baked viewmodel transform, so the
    // muzzle flash and tracer leave the actual muzzle and track its kick/sway.
    {
        const Vec3f muzzleCam = activeWeaponMuzzle(vmProfile, weaponXf);
        const float camX = muzzleCam.x;
        const float camY = muzzleCam.y;
        const float camZ = muzzleCam.z;
        if (camZ > 0.01f && screenH > 0) {
            const float aspect = static_cast<float>(screenW) / static_cast<float>(screenH);
            const float vmFov = clamp(activeViewmodelFov, 45.0f, 135.0f);
            const float yScale = 1.0f / std::tan(degToRad(vmFov) * 0.5f);
            const float xScale = yScale / aspect;
            muzzleFracX_ = clamp((camX * xScale / camZ) * 0.5f + 0.5f, 0.0f, 1.0f);
            muzzleFracY_ = clamp(0.5f - (camY * yScale / camZ) * 0.5f, 0.0f, 1.0f);
        }
    }
    // Viewmodel per active weapon: Bumstrum meshes are complete baked weapon+arms
    // rigs, so there is no separate static weapon/hand fallback.
    const auto drawAnimatedVm = [&](AnimatedViewmodelRuntime& runtime, const Mat4& xf,
                                    Vec4f armsTint, Vec4f weaponTint,
                                    float armsFill, float weaponFill) {
        const size_t n = std::min(runtime.meshes.size(), runtime.sampled.size());
        for (size_t i = 0; i < n; ++i) {
            const AnimatedGltfSubmesh& sm = runtime.sampled[i];
            MaterialHandle mat = matUntextured_;
            std::string materialName;
            if (sm.materialIndex >= 0 && sm.materialIndex < static_cast<int>(runtime.materials.size())) {
                mat = runtime.materials[static_cast<size_t>(sm.materialIndex)];
                materialName = runtime.model.materials()[static_cast<size_t>(sm.materialIndex)].name;
            }
            std::string semanticName = materialName + " " + sm.name;
            for (char& c : semanticName) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool armsMaterial = semanticName.find("arm") != std::string::npos;
            const bool lensMaterial = semanticName.find("lens") != std::string::npos;
            const Vec4f tint = lensMaterial ? Vec4f{ 0.75f, 1.18f, 1.35f, 1.0f }
                                            : (armsMaterial ? armsTint : weaponTint);
            const float fill = lensMaterial ? 0.080f : (armsMaterial ? armsFill : weaponFill);
            addDraw(10 + static_cast<uint64_t>(i), runtime.meshes[i], mat, xf, tint, fill, true);
        }
    };
    if (AnimatedViewmodelRuntime* runtime = activeViewmodelRuntime()) {
        const bool pistolLike = activeWeaponId == "pistol" || activeWeaponId == "machine_pistol";
        const bool smgLike = activeWeaponId == "pulse_smg";
        const bool carbineLike = activeWeaponId == "carbine";
        const Vec4f armsTint = carbineLike ? Vec4f{ 1.06f, 1.03f, 0.96f, 1.0f }
                             : pistolLike ? Vec4f{ 1.22f, 1.18f, 1.10f, 1.0f }
                             : smgLike ? Vec4f{ 1.25f, 1.20f, 1.12f, 1.0f }
                                       : Vec4f{ 1.28f, 1.23f, 1.15f, 1.0f };
        const Vec4f weaponTint = carbineLike ? Vec4f{ 0.84f, 0.86f, 0.88f, 1.0f }
                               : pistolLike ? Vec4f{ 1.38f, 1.30f, 1.16f, 1.0f }
                               : smgLike ? Vec4f{ 1.38f, 1.34f, 1.22f, 1.0f }
                                         : Vec4f{ 1.42f, 1.36f, 1.24f, 1.0f };
        const float armsFill = carbineLike ? 0.010f : (pistolLike ? 0.018f : (smgLike ? 0.022f : 0.025f));
        const float weaponFill = carbineLike ? 0.012f : (pistolLike ? 0.034f : (smgLike ? 0.038f : 0.040f));
        drawAnimatedVm(*runtime, weaponXf, armsTint, weaponTint, armsFill, weaponFill);
    }

    const float eyeY = clamp(player_.height, 0.05f, 6.0f);  // eye rises when standing on cover
    frame_.camera.position = { player_.pos.x, eyeY, player_.pos.y };
    frame_.camera.yaw = renderViewYaw_;
    frame_.camera.pitch = renderViewPitch_;
    frame_.camera.fovDeg = std::max(45.0f, tunables_.cameraFovDegrees - fireFovKick_);
    frame_.camera.viewmodelFovDeg = std::max(45.0f, activeViewmodelFov);
    frame_.camera.nearZ = 0.035f;
    frame_.camera.farZ = MaxRayDistance;
    if (topDownCapture_ && wasteland_.ready()) {   // dev/QA: oblique 3/4 view from above a room edge,
        // so vertical faces (walls / boxes / decks) are visible from the side - the angle where
        // single-sided / inside-out geometry actually shows (a pure top-down only sees the caps).
        const float cx = wasteland_.centerX(), cz = wasteland_.centerZ(), hz = wasteland_.halfExtentZ();
        frame_.camera.position = { cx, 17.0f, cz + hz + 7.0f };
        frame_.camera.yaw = -1.5708f;     // face -Z (north), looking into the room
        frame_.camera.pitch = -0.66f;     // oblique downward
        frame_.camera.fovDeg = 74.0f;
        frame_.camera.nearZ = 0.05f;
    }
    frame_.sun.direction = { -0.55f, -0.48f, -0.38f };   // grazing angle -> long cast shadows
    frame_.sun.color = { 0.70f, 0.78f, 0.95f };     // cool key light (casts shadows)
    frame_.sun.intensity = 0.75f;
    frame_.sun.ambient = 0.12f;                     // dark base; sun + local lights carry it
    // Background doubles as the volumetric fog colour. Underground the void/distance must
    // read as darkness, not a blue-grey "sky" at the end of corridors; elsewhere keep the
    // cool industrial blue.
    frame_.clearColor = Vec3f{ 5.0f / 255.0f, 6.0f / 255.0f, 10.0f / 255.0f };   // cool industrial blue (per-biome overrides below)

    // Outdoor arena: golden-hour mood. A low warm sun throws long dramatic shadows; a cool graded
    // sky (the resolve sky gradient reads clearColor as the hazy horizon + sunColor for the sun
    // disc/glow) fills the shadows; near-clear air so the player sees across the arena. No torch
    // fixtures - the sun is not occluded out here.
    if (wasteland_.ready()) {
        // Sun front-right-high of the player's default facing (-Z). Each BIOME gets its own palette
        // so the run's sectors read as distinct places (rocky golden hour / cooler greener forest).
        frame_.sun.direction = { -0.42f, -0.55f, 0.62f };
        // Lower exposure so the bright illustrated sky rolls off through AgX instead of washing the
        // frame; the strong directional sun then carries the ground with real contrast. Cinematic
        // key/fill ratio: a STRONG sun over a LOW, tinted fill. High ambient (the old 0.32-0.46)
        // floods the shadows and reads as a flat "overcast tech demo"; crushing the fill + keeping
        // air near-clear (haze washes against a bright sky, it does not dramatize) lets the sun
        // sculpt the rock with long shadows. (Exposure is set once in THE LOOK block below.)
        if (currentBiome_ == Biome::Forest) {
            frame_.sun.color = { 1.00f, 0.93f, 0.70f }; frame_.sun.intensity = 3.3f;
            frame_.sun.ambient = 0.22f; frame_.post.fogDensity = 0.006f;
            frame_.clearColor = { 0.30f, 0.40f, 0.42f };
        } else if (currentBiome_ == Biome::Ruins) {   // moody late-afternoon, cool but directional
            frame_.sun.color = { 0.92f, 0.88f, 0.92f }; frame_.sun.intensity = 2.8f;
            frame_.sun.ambient = 0.26f; frame_.post.fogDensity = 0.007f;
            frame_.clearColor = { 0.40f, 0.42f, 0.50f };
        } else {   // Rocky: golden hour, warm + dramatic
            frame_.sun.color = { 1.0f, 0.74f, 0.46f }; frame_.sun.intensity = 4.0f;
            frame_.sun.ambient = 0.18f; frame_.post.fogDensity = 0.006f;
            frame_.clearColor = { 0.42f, 0.44f, 0.56f };
        }
    }

    // Neon Ink Brutalism arena: M4 makes the LIGHTING biome-aware too (the palette already is),
    // so each sector reads as its own place - cold Foundry, hot Furnace, eerie Reliquary - while
    // keeping the brutalist key/ambient balance that makes the kit legible alongside the HDR neon.
    {
        // The rooms are now SEALED INTERIORS (ceiling occludes the sun via CSM), so the per-biome
        // identity is carried by the LOCAL light rig + emissive trim built below; the sun here is a
        // weak directional accent (rakes through doorways) + a coloured low ambient floor. Local
        // fixture pools now also light the fog volume, so the air shows the room's identity instead
        // of sitting as a neutral veil.
        frame_.sun.direction = { -0.42f, -0.50f, 0.55f };
        switch (currentBiome_) {
            case Biome::Forest:   // FURNACE: deep warm shadow, brutal contrast, ember haze
                frame_.sun.color     = { 1.14f, 0.72f, 0.42f };
                frame_.sun.intensity = 0.95f;
                frame_.sun.ambient   = 0.135f;
                frame_.post.fogDensity = 0.038f;                  // hot particulate haze; local molten pools light the air
                frame_.clearColor = { 0.105f, 0.045f, 0.028f };
                frame_.post.gradeTint = { 1.10f, 0.95f, 0.80f };  // warm cast
                break;
            case Biome::Ruins:    // RELIQUARY: near-black fill, cold shafts, very high contrast
                frame_.sun.color     = { 0.78f, 0.90f, 1.18f };
                frame_.sun.intensity = 0.70f;
                frame_.sun.ambient   = 0.090f;
                frame_.post.fogDensity = 0.022f;                  // thin but visible cold shafts
                frame_.clearColor = { 0.024f, 0.030f, 0.070f };
                frame_.post.gradeTint = { 0.96f, 1.00f, 1.08f };  // cold violet-blue
                break;
            default:              // FOUNDRY: crisp cool, medium-high contrast, thin cool air
                frame_.sun.color     = { 0.82f, 0.94f, 1.16f };
                frame_.sun.intensity = 1.18f;
                frame_.sun.ambient   = 0.255f;
                frame_.post.fogDensity = 0.014f;                  // clean industrial haze, enough to reveal strip lights
                frame_.clearColor = { 0.030f, 0.042f, 0.068f };
                frame_.post.gradeTint = { 0.97f, 1.00f, 1.05f };  // faint cool
                break;
        }
    }

    // W1 (Neon Ink Brutalism): drive the stylized renderer from the locked style
    // library (config/pulse.style). stylize = 1 turns on the 3-band diffuse shading;
    // F5 reloads pulse.style so the bands are tunable without a rebuild.
    frame_.style.bandShadow   = style_.bandShadow;
    frame_.style.bandLit      = style_.bandLit;
    frame_.style.bandSoftness = style_.bandSoftness;
    frame_.style.stylize      = 0.0f;   // smooth PBR shading (cel banding off - let the lighting read realistically)
    // W4: illustrated zenith->horizon sky, enabled only where a real sky is visible (outdoor).
    frame_.style.skyZenith    = { style_.skyZenith.r, style_.skyZenith.g, style_.skyZenith.b };
    frame_.style.skyHorizon   = { style_.skyHorizon.r, style_.skyHorizon.g, style_.skyHorizon.b };
    frame_.style.skyStrength  = wasteland_.ready() ? 1.0f : 0.0f;
    // A muted deep-twilight sky, tinted PER BIOME (M4) - the open-topped arena shows this sky, so it
    // is the dominant at-a-glance biome cue. Foundry cold indigo, Furnace ember red-black, Reliquary
    // cold violet. Kept dim so the neon stays brightest.
    {
        switch (currentBiome_) {
            case Biome::Forest:   // FURNACE
                frame_.style.skyZenith  = { 0.100f, 0.050f, 0.045f };   // lifted warm-dark
                frame_.style.skyHorizon = { 0.70f, 0.34f, 0.16f };      // hot ember horizon
                break;
            case Biome::Ruins:    // RELIQUARY
                frame_.style.skyZenith  = { 0.070f, 0.060f, 0.140f };   // lifted cold
                frame_.style.skyHorizon = { 0.40f, 0.30f, 0.50f };      // visible violet horizon
                break;
            default:              // FOUNDRY
                frame_.style.skyZenith  = { 0.075f, 0.115f, 0.205f };   // clean deep-blue zenith
                frame_.style.skyHorizon = { 0.62f, 0.52f, 0.44f };      // warm hazy horizon (warm/cool sky contrast)
                break;
        }
        frame_.style.skyStrength = 1.0f;
    }
    // W2: blue-black ink outlines (silhouettes + creases). Thickness from the locked style
    // library; strength = 1 turns them on. F5 reloads pulse.style to retune live.
    frame_.style.inkOutline       = { style_.inkOutline.r, style_.inkOutline.g, style_.inkOutline.b };
    frame_.style.outlineThickness = style_.outlineEnvPx;
    frame_.style.outlineStrength  = 0.0f;   // ink outlines off (clean realistic edges, not inked-comic)
    frame_.style.outlineHeroScale = style_.outlineHeroScale;   // bolder ink on the weapon (W2)
    // doc 5 hatching: ink the shadow + contact bands. World-anchored + distance-faded in
    // the shader; values come from the locked style library so F5 retunes them live.
    frame_.style.hatchStrength = style_.hatchStrength;   // cross-hatch ink in the shadow bands (the reference's scribbled shading)
    frame_.style.hatchScale    = style_.hatchScale;
    frame_.style.hatchWidth    = style_.hatchWidth;
    frame_.style.hatchFade     = style_.hatchFade;
    // ===================== THE LOOK - single source of truth =====================
    // ALL global art-look params are set ONCE here (exposure + grade + post filter); there is NO
    // per-arena override anywhere else. Cel bands / ink outlines / hatch are set just above from
    // config/pulse.style (F5-tunable). Per-SCENE differences (sun, sky, clearColor, biome gradeTint,
    // fog) live in the arena/biome blocks above and do NOT touch these. Restrained brutalist look.
    frame_.post.exposure       = 1.22f;
    frame_.post.gradeEnvSat    = 1.00f;
    frame_.post.gradeNeonSat   = 1.22f;
    frame_.post.gradeNeonGain  = 1.65f;
    frame_.post.bloomThreshold = 1.62f;
    frame_.post.bloomKnee      = 0.80f;
    frame_.post.bloomIntensity = settings_.reduceBloom ? 0.11f : 0.24f;
    frame_.post.caScale        = 0.0f;    // no chromatic aberration
    frame_.post.vignette       = 0.11f;   // light frame
    frame_.post.grain          = 0.012f;  // faint texture
    frame_.post.sharpen        = 0.20f;
    // =============================================================================

    // Local lights: gameplay-driven pools of colour that make the dark arena read
    // (the substance of the section 3e atmosphere). A soft warm fill around the player
    // keeps the play space legible; emissive entities cast their own coloured light.
    lights_.clear();
    const Vec3f playerFill = currentBiome_ == Biome::Forest ? Vec3f{ 1.10f, 0.82f, 0.58f }
                           : currentBiome_ == Biome::Ruins  ? Vec3f{ 0.72f, 0.88f, 1.20f }
                                                             : Vec3f{ 0.86f, 0.96f, 1.08f };
    lights_.push_back({ { player_.pos.x, 0.95f, player_.pos.y }, playerFill, 1.12f, 9.0f });
    // Designed rooms carry their focal read through kit props, decals, emissive trim, and biome
    // lighting. No global cyan objective glow: it made every arena feel like the same room.
    // Neon fixture pools: moody local colour at the corner risers + a cool overhead key, so the
    // baked neon accents (emissive geometry) also spill onto the matte surfaces around them.
    if (wasteland_.ready()) {
        const float cx = wasteland_.centerX(), cz = wasteland_.centerZ();
        const float hx = wasteland_.halfExtentX(), hz = wasteland_.halfExtentZ();
        const float ceil = wasteland_.ceiling();
        const float maxh = std::max(hx, hz);
        const auto airY = [&](float k) {
            return roomFloorY_ + (ceil - roomFloorY_) * k;
        };
        if (currentBiome_ == Biome::Forest) {
            // FURNACE: hot molten UPLIGHT from the floor (low, orange) with deep shadow overhead -
            // brutal contrast, glow "almost everywhere" near the ground (bible). Matches the molten
            // floor seams the assembler emits.
            const Vec3f molten{ 1.95f, 0.62f, 0.15f };
            lights_.push_back({ { cx, 0.45f, cz }, molten, 3.05f, maxh + 6.0f });
            lights_.push_back({ { cx, airY(0.30f), cz }, { 1.35f, 0.40f, 0.16f }, 1.05f, maxh * 0.85f });
            lights_.push_back({ { cx - hx + 1.6f, 0.38f, cz - hz + 1.6f }, molten, 2.15f, 10.5f });
            lights_.push_back({ { cx + hx - 1.6f, 0.38f, cz - hz + 1.6f }, molten, 2.15f, 10.5f });
            lights_.push_back({ { cx - hx + 1.6f, 0.38f, cz + hz - 1.6f }, molten, 2.15f, 10.5f });
            lights_.push_back({ { cx + hx - 1.6f, 0.38f, cz + hz - 1.6f }, molten, 2.15f, 10.5f });
            lights_.push_back({ { cx, ceil - 0.7f, cz }, { 1.00f, 0.52f, 0.32f }, 0.82f, maxh * 1.90f }); // dim smoky top fill
        } else if (currentBiome_ == Biome::Ruins) {
            // RELIQUARY: strong cold light SHAFTS down from the ceiling panels (god-ray pools) over
            // near-black fill - the eerie, monumental identity. Thin cold fog (set above) makes the
            // shafts visible in air.
            const Vec3f shaft{ 0.66f, 0.86f, 1.48f };
            lights_.push_back({ { cx, ceil - 0.6f, cz }, shaft, 4.15f, ceil + 7.0f });                 // central shaft over the relic
            lights_.push_back({ { cx, airY(0.66f), cz }, shaft, 1.25f, 3.6f });                        // visible column in the fog
            lights_.push_back({ { cx, airY(0.38f), cz }, { 0.44f, 0.64f, 1.18f }, 0.72f, 2.9f });
            lights_.push_back({ { cx - hx * 0.55f, ceil - 0.6f, cz - hz * 0.45f }, shaft, 2.75f, ceil + 4.5f });
            lights_.push_back({ { cx + hx * 0.55f, ceil - 0.6f, cz + hz * 0.45f }, shaft, 2.75f, ceil + 4.5f });
            lights_.push_back({ { cx, 0.6f, cz }, { 0.24f, 0.34f, 0.72f }, 0.72f, maxh * 1.10f });      // faint cool floor pool
        } else {
            // FOUNDRY: crisp cool-white overhead STRIP lights in a grid (industrial hall) + a cyan
            // conduit spill that matches the ceiling strips the assembler emits. Medium-high contrast.
            const Vec3f key{ 0.88f, 0.99f, 1.16f }, roomFill{ 0.32f, 0.48f, 0.68f }, cyan{ 0.10f, 0.68f, 0.96f };
            for (float lx = cx - hx * 0.55f; lx <= cx + hx * 0.55f + 0.01f; lx += std::max(0.1f, hx * 0.55f))
                for (float lz = cz - hz * 0.55f; lz <= cz + hz * 0.55f + 0.01f; lz += std::max(0.1f, hz * 0.55f)) {
                    lights_.push_back({ { lx, ceil - 0.7f, lz }, key, 2.05f, 14.0f });
                    lights_.push_back({ { lx, airY(0.58f), lz }, { 0.42f, 0.60f, 0.86f }, 0.42f, 5.2f });
                }
            lights_.push_back({ { cx, 2.2f, cz }, roomFill, 1.35f, maxh * 1.70f });       // broad readable room fill
            lights_.push_back({ { cx, ceil - 0.8f, cz }, cyan, 0.72f, maxh * 0.95f });    // cyan conduit accent
        }
        // Door fixtures double as wayfinding: spawn green, exits amber.
        const std::vector<Door>& envDoors = wasteland_.doors();
        for (size_t i = 0; i < envDoors.size(); ++i) {
            const Door& dr = envDoors[i];
            const bool entry = i == 0;
            const Vec3f doorCol = entry ? Vec3f{ 0.34f, 1.0f, 0.56f } : Vec3f{ 1.0f, 0.72f, 0.30f };
            lights_.push_back({ { dr.worldX, 2.35f, dr.worldZ }, doorCol, entry ? 1.15f : 1.35f, 7.0f });
        }
    }
    for (const ActiveEnemyLight& l : activeEnemyLights)
        lights_.push_back({ l.pos, l.color, l.intensity, l.radius });
    // Firing enemy beams: a bright SUSTAINED impact pool where each lance lands (this is what grounds
    // the beam on a surface) + a softer muzzle glow at the origin. Both fade with the beam's cutoff.
    // These are submitted before decorative prop lights so the fog pass prioritizes combat glow.
    for (const ActiveBeam& b : activeBeams) {
        lights_.push_back({ b.to,   b.color, 2.45f * b.intensity, 3.1f });  // tight, sharp impact pool
        lights_.push_back({ b.from, b.color, 1.65f * b.intensity, 2.6f });  // muzzle glow
        for (int i = 1; i <= 3; ++i) {
            const float t = static_cast<float>(i) * 0.25f;
            const Vec3f p{ b.from.x + (b.to.x - b.from.x) * t,
                           b.from.y + (b.to.y - b.from.y) * t,
                           b.from.z + (b.to.z - b.from.z) * t };
            lights_.push_back({ p, b.color, 0.55f * b.intensity, 1.65f });
        }
    }
    // M2.5 glowing prop panels cast their own coloured pool of light.
    for (const Prop& pr : props_)
        if (pr.emissive > 0.0f)
            lights_.push_back({ { pr.pos.x, 0.55f, pr.pos.z }, pr.tint, 1.5f, 4.5f });
    for (const Pickup& pk : pickups_) {
        const Vec3f col = pk.kind == PickupKind::Health ? Vec3f{ 0.40f, 1.3f, 0.60f }
                        : pk.kind == PickupKind::Shield ? Vec3f{ 0.50f, 0.8f, 1.40f }
                                                        : Vec3f{ 1.40f, 1.0f, 0.40f };
        lights_.push_back({ { pk.pos.x, 0.5f, pk.pos.y }, col, 0.75f, 2.35f });
    }
    for (const Projectile& pr : projectiles_) {
        if (!pr.active) continue;
        const Vec3f col = pr.hostile ? pr.color : Vec3f{ 0.45f, 0.95f, 1.5f };
        lights_.push_back({ { pr.pos.x, pr.height, pr.pos.y }, col, 1.9f, 3.1f });
    }
    // Boss nova shockwaves: a bright flash pool at the centre that fades + widens as the ring expands.
    for (const NovaWave& nv : novaWaves_) {
        const float t = clamp(nv.age / std::max(0.001f, nv.life), 0.0f, 1.0f);
        lights_.push_back({ nv.pos, nv.color, 3.2f * (1.0f - t), 1.5f + 6.0f * t });
    }
    frame_.lights = lights_;
    frame_.instances = instances_;

    // Grounding contact shadows (decal kind 2): a soft dark disc projected on the floor under each
    // actor so enemies/props read as SITTING in the world instead of floating - it fakes the ambient
    // occlusion an actor casts where it meets the ground. Rebuilt per frame on top of the persistent
    // bullet-mark/scorch decals. The skinned enemies' feet sit at roomFloorY_; halfDepth tolerates
    // small ground height variation.
    frameDecals_.assign(decals_.begin(), decals_.end());
    frameDecals_.insert(frameDecals_.end(), envDecals_.begin(), envDecals_.end());   // per-room biome floor markings
    const auto addContactShadow = [&](float wx, float wz, float footY, float radius, float alpha) {
        Decal d;
        d.center    = { wx, footY + 0.05f, wz };
        d.normal    = { 0.0f, 1.0f, 0.0f };
        d.tangent   = { 1.0f, 0.0f, 0.0f };
        d.halfWidth = radius; d.halfHeight = radius; d.halfDepth = 0.6f;
        d.color     = { 0.010f, 0.010f, 0.016f };   // near-black
        d.alpha     = alpha;
        d.kind      = 2u;
        frameDecals_.push_back(d);
    };
    for (const Enemy& e : enemies_)
        if (e.active) addContactShadow(e.pos.x, e.pos.y, roomFloorY_, e.boss ? 2.0f : 0.62f, 0.60f);
    for (const Prop& pr : props_)
        if (pr.radius > 0.05f) addContactShadow(pr.pos.x, pr.pos.z, roomFloorY_, std::max(0.40f, pr.radius * 0.9f), 0.50f);
    // Beam scorch: a transient burn mark at each firing beam's impact point, projected back along the
    // lance onto the surface it is burning - the modelled "scorch decal" endpoint from the spec.
    for (const ActiveBeam& b : activeBeams) {
        const Vec3f d = b.to - b.from;
        const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dl < 0.2f) continue;
        const Vec3f n{ -d.x / dl, -d.y / dl, -d.z / dl };          // face back toward the muzzle
        const Vec3f up = std::fabs(n.y) < 0.9f ? Vec3f{ 0.0f, 1.0f, 0.0f } : Vec3f{ 1.0f, 0.0f, 0.0f };
        Decal s;
        s.center    = b.to;
        s.normal    = n;
        s.tangent   = normalize3(cross3(up, n));
        s.halfWidth = 0.5f; s.halfHeight = 0.5f; s.halfDepth = 0.4f;
        s.color     = { 0.030f, 0.012f, 0.022f };
        s.alpha     = 0.55f * b.intensity;
        s.kind      = 1u;   // scorch
        frameDecals_.push_back(s);
    }
    frame_.decals = frameDecals_;

    // Build the engine particle billboards from the CPU pool (fade size + emissive
    // over each particle's life).
    particleRender_.clear();
    particleRender_.reserve(particles_.size());
    for (const WorldParticle& p : particles_) {
        const float t = clamp(p.life / std::max(0.001f, p.maxLife), 0.0f, 1.0f);
        Particle r;
        r.center = p.pos;
        r.size = p.size * (0.45f + 0.55f * t);
        // Colour-over-life ("die cool"): a mote keeps its authored hue for most of its life, then in
        // the last third desaturates + darkens toward a dim ash, so trails/sparks read as COOLING
        // energy instead of a hard pop-out. (Born-hot white core + brightness ramp are handled by the
        // particle PS temperature model and emissive*t^2; this completes the curve at the tail.)
        if (t < 0.34f) {
            const float k = t / 0.34f;                                        // 0 at death -> 1 entering the tail
            const float lum = 0.30f * p.color.x + 0.59f * p.color.y + 0.11f * p.color.z;
            const float ash = lum * 0.45f;
            r.color = { ash + (p.color.x - ash) * k, ash + (p.color.y - ash) * k, ash + (p.color.z - ash) * k };
        } else {
            r.color = p.color;
        }
        r.emissive = p.emissive * t * t;
        r.velocity = p.vel;     // drives the screen-aligned streak
        r.stretch = p.stretch;
        particleRender_.push_back(r);
    }
    for (const Particle& c : enemyCoreRender_) particleRender_.push_back(c);  // white-hot enemy cores (additive)

    // Boss nova SHOCKWAVE: an expanding ground ring with a HARD bright leading edge (the read-the-edge
    // hitbox) over a soft inner fill (the spec's area-burst "crucible wave"). Deterministic additive
    // particles - no rng, no pool growth - so the sim stays byte-identical.
    for (const NovaWave& nv : novaWaves_) {
        const float t = clamp(nv.age / std::max(0.001f, nv.life), 0.0f, 1.0f);
        const float fade = (1.0f - t) * (1.0f - t);
        const float ringR = 0.4f + 6.2f * t;
        const Vec3f hot{ std::min(nv.color.x + 0.9f, 2.8f), std::min(nv.color.y + 0.6f, 2.2f),
                         std::min(nv.color.z + 0.8f, 2.6f) };
        const int edge = std::clamp(static_cast<int>(ringR * 9.0f), 28, 96);
        for (int i = 0; i < edge; ++i) {       // hard leading edge: dense bright rim at ringR
            const float a = TwoPi * static_cast<float>(i) / static_cast<float>(edge);
            Particle d;
            d.center   = { nv.pos.x + std::cos(a) * ringR, nv.pos.y, nv.pos.z + std::sin(a) * ringR };
            d.size     = 0.16f;  d.color = hot;       d.emissive = 3.0f * fade;
            d.velocity = { 0.0f, 0.0f, 0.0f };  d.stretch = 0.0f;
            particleRender_.push_back(d);
        }
        const int fill = std::max(16, edge / 2);
        const float fillR = ringR * 0.62f;
        for (int i = 0; i < fill; ++i) {       // soft fill: dimmer, larger inner ring (body behind the edge)
            const float a = TwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(fill);
            Particle d;
            d.center   = { nv.pos.x + std::cos(a) * fillR, nv.pos.y, nv.pos.z + std::sin(a) * fillR };
            d.size     = 0.34f;  d.color = nv.color;  d.emissive = 1.0f * fade;
            d.velocity = { 0.0f, 0.0f, 0.0f };  d.stretch = 0.0f;
            particleRender_.push_back(d);
        }
    }

    frame_.particles = particleRender_;
    frame_.smoke = enemySmokeRender_;

    // Heat-haze refraction: ONLY the brief expanding impact shocks. The continuous per-orb
    // shimmer was removed - a 1.4m warp region around every flying orb refracted the scene as
    // orbs approached the camera, which read as the whole image going blurry (a rendering
    // effect, not the orb). Impacts are brief + dramatic, so they keep their shockwave warp.
    // An empty list makes the scene blit a plain copy (no warp).
    heatRender_.clear();
    for (const HeatPulse& h : heatPulses_) {
        const float t = clamp(h.age / std::max(0.001f, h.life), 0.0f, 1.0f);
        const float radius = 0.6f + 3.0f * t * clamp(h.power, 0.4f, 2.0f);          // expands outward
        const float strength = (1.0f - t) * (1.0f - t) * 0.05f * clamp(h.power, 0.4f, 2.0f); // fades out
        heatRender_.push_back({ h.pos, radius, strength });
    }
    // Heat-haze band along each firing beam: a few low-strength shimmer samples down the length so the
    // air around the lance distorts as a thin band (the spec's heat-haze layer) - small radius + low
    // strength per sample, so it stays a tight detail on the beam, never a fullscreen smear.
    for (const ActiveBeam& b : activeBeams) {
        constexpr int samples = 6;
        for (int s = 1; s <= samples; ++s) {
            const float f = static_cast<float>(s) / static_cast<float>(samples + 1);
            const Vec3f p{ b.from.x + (b.to.x - b.from.x) * f,
                           b.from.y + (b.to.y - b.from.y) * f,
                           b.from.z + (b.to.z - b.from.z) * f };
            heatRender_.push_back({ p, 0.34f, 0.018f * b.intensity });
        }
    }
    // Boss nova shockwave: a SUBTLE air distortion over the expanding ring (kept low - a strong warp
    // over the boss reads as "the image went blurry", the documented per-orb-heat failure mode).
    for (const NovaWave& nv : novaWaves_) {
        const float t = clamp(nv.age / std::max(0.001f, nv.life), 0.0f, 1.0f);
        heatRender_.push_back({ nv.pos, 0.5f + 5.5f * t, (1.0f - t) * 0.020f });
    }
    frame_.heat = heatRender_;

    if (!ui_) ui_.emplace(engine.font());
    ui_->reset();
    buildHud(*ui_, screenW, screenH);
    // Door transition fade (drawn last, over all HUD): a black wipe whose alpha triangles 0 -> 1 ->
    // 0 across the ~0.55s load, with the next area swapping in at the fully-black midpoint.
    if (doorFadeTimer_ > 0.0f) {
        const float k = 1.0f - std::fabs(doorFadeTimer_ / std::max(0.0001f, doorFadeDuration_) * 2.0f - 1.0f);
        ui_->rect(0.0f, 0.0f, static_cast<float>(screenW), static_cast<float>(screenH),
                  rgba(0, 0, 0, static_cast<uint8_t>(255.0f * clamp(k, 0.0f, 1.0f))));
    }
    frame_.ui = ui_->vertices();
    return frame_;
}

namespace {
// ---------------------------------------------------------------------------
// HUD / menu visual system. One named palette + a few small helpers so the in-game
// HUD and every menu (and the M1 ChoosePath/Shop/Event screens) speak one visual
// language instead of ad-hoc colours and magic offsets scattered per draw site.
namespace pal {
    // ====================================================================
    // Neon Ink Brutalism, role-locked (PULSE UI Overhaul handoff). COLOR LAW:
    // cyan is RESERVED for player focus / selection / navigation / Pulse building;
    // the Pulse progression climbs dark -> cyan -> bright cyan -> ice-white -> pure
    // white (ion-white). There is NO magenta / hot-pink anywhere (it was removed;
    // the old --magenta token is now ion-white). Threat / damage / low-health use
    // danger RED. PACTS have their own identity: crimson (never pink). Amber =
    // readiness / unsaved / Heat; gold = exceptional reward / progression / max;
    // green (good) = positive / affordable / unlock / repair. Tiers + room types
    // stay OFF-cyan so selection reads base-colour-independent on any card. Glow
    // (the *Glow / *Hi variants) is reserved for active / selected / dangerous /
    // luminous; every critical value stays legible with glow removed.
    // --- core neutrals / surfaces (spec: bg/surface-0..2/line) -----------
    constexpr uint32_t deepBg    = rgb(4, 6, 10);           // app/void background  #04060a
    constexpr uint32_t surface0  = rgb(9, 15, 22);          // base panel           #090f16
    constexpr uint32_t navy      = rgb(14, 24, 34);         // raised panel fill    #0e1822
    constexpr uint32_t surface2  = rgb(18, 30, 42);         // header strip
    constexpr uint32_t scrim     = rgba(7, 10, 15, 224);    // full-screen menu dim
    constexpr uint32_t hudScrim  = rgba(7, 10, 15, 158);    // local HUD-group scrim (~.62)
    constexpr uint32_t panelTop  = rgba(15, 20, 28, 245);   // panel body gradient top    (surface-1)
    constexpr uint32_t panelBot  = rgba(11, 15, 21, 245);   // panel body gradient bottom (surface-0)
    constexpr uint32_t headTop   = rgba(20, 27, 37, 250);   // header band top    (surface-2)
    constexpr uint32_t headBot   = rgba(15, 20, 28, 250);   // header band bottom (surface-1)
    constexpr uint32_t plateTop  = rgba(11, 15, 21, 178);   // in-world HUD module top (~.7)
    constexpr uint32_t plateBot  = rgba(8, 11, 17, 178);    // in-world HUD module bottom
    constexpr uint32_t plate     = rgba(12, 16, 24, 180);   // flat HUD alias
    constexpr uint32_t panelFill = rgb(15, 20, 28);         // flat menu alias
    constexpr uint32_t headFill  = rgba(20, 27, 37, 250);   // flat header alias
    constexpr uint32_t border    = rgb(32, 54, 66);         // hairline panel border
    constexpr uint32_t lineSoft  = rgb(27, 47, 58);         // inner divider
    constexpr uint32_t borderHi  = rgb(43, 214, 245);       // accented border (cyan)
    constexpr uint32_t bevel     = rgba(255, 255, 255, 18); // 1px top inner highlight
    constexpr uint32_t track     = rgba(154, 166, 180, 42); // bar track / empty
    constexpr uint32_t inkStroke = rgba(154, 166, 180, 80); // HUD module frame line
    // --- text / ink ------------------------------------------------------
    constexpr uint32_t textHero  = rgb(243, 250, 254);      // hero values
    constexpr uint32_t textHi    = rgb(238, 246, 251);      // primary ink
    constexpr uint32_t textMid   = rgb(174, 190, 203);      // secondary ink
    constexpr uint32_t textDim   = rgb(111, 138, 153);      // labels ink
    constexpr uint32_t textFaint = rgb(72, 96, 110);        // faint ink
    constexpr uint32_t disabled  = rgb(58, 66, 80);         // disabled / empty / separators
    // --- roles: energy / state ------------------------------------------
    constexpr uint32_t accent     = rgb(43, 214, 245);      // cyan: player / selection / Pulse building #2bd6f5
    constexpr uint32_t accentGlow = rgb(70, 233, 255);      // cyan hot: selection glow / ready
    constexpr uint32_t ionWhite   = rgb(234, 244, 255);     // high-Pulse / Burning energy (NOT magenta) #eaf4ff
    constexpr uint32_t ionGlow    = rgb(255, 255, 255);     // OVERPULSE pure white, max bloom
    constexpr uint32_t danger     = rgb(255, 77, 106);      // red: threat / damage / low-health #ff4d6a
    constexpr uint32_t dangerHi   = rgb(255, 138, 158);     // red soft: damage value / warning
    constexpr uint32_t bossCore   = rgb(255, 77, 106);      // boss / hostile core (red, no magenta)
    constexpr uint32_t bossCoreHi = rgb(255, 138, 158);     // boss label glow
    constexpr uint32_t crimson    = rgb(194, 58, 78);       // PACT identity (curse / contract) #c23a4e
    constexpr uint32_t crimsonHi  = rgb(224, 96, 116);      // pact curse text
    constexpr uint32_t crimsonDeep= rgb(122, 31, 46);       // pact shadow / fracture #7a1f2e
    constexpr uint32_t warn       = rgb(255, 178, 77);      // amber: readiness / unsaved / Heat / warning #ffb24d
    constexpr uint32_t warnHi     = rgb(255, 200, 121);     // amber hot
    constexpr uint32_t amber      = rgb(255, 178, 77);      // amber alias (readiness / Heat)
    constexpr uint32_t gold       = rgb(255, 210, 77);      // gold: exceptional reward / progression / max #ffd24d
    constexpr uint32_t goldGlow   = rgb(255, 228, 140);
    constexpr uint32_t destructive= rgb(199, 120, 120);     // destructive helper text (reddish)
    // Player vitals + positive.
    constexpr uint32_t good       = rgb(125, 255, 138);     // green: positive / affordable / unlock / repair
    constexpr uint32_t goodGlow   = rgb(151, 255, 163);
    constexpr uint32_t shieldCol  = rgb(255, 177, 61);      // shield (amber, distinct from cyan HP)
    constexpr uint32_t dashCharge = rgb(34, 116, 130);      // dash bar while recharging (dim cyan)
    // --- loot tiers / rarity (deliberately off-cyan) ---------------------
    constexpr uint32_t tierCommon     = rgb(159, 176, 189); // slate
    constexpr uint32_t tierUncommon   = rgb(77, 155, 255);  // blue
    constexpr uint32_t tierUncommonHi = rgb(155, 196, 255);
    constexpr uint32_t tierRare       = rgb(255, 178, 77);  // amber
    constexpr uint32_t tierLegendary  = rgb(255, 225, 77);  // gold
    constexpr uint32_t synergyHi      = rgb(95, 230, 166);  // green - reward synergy marker
    // --- affinity (one colour + one glyph each) --------------------------
    constexpr uint32_t affPyro    = rgb(255, 106, 61);      // PYRO
    constexpr uint32_t affVolt    = rgb(77, 155, 255);      // VOLT
    constexpr uint32_t affCryo    = rgb(111, 232, 255);     // CRYO
    constexpr uint32_t affAcid    = rgb(125, 255, 138);     // ACID
    constexpr uint32_t affKinetic = rgb(184, 255, 92);      // KINETIC
    constexpr uint32_t affBulwark = rgb(174, 191, 208);     // BULWARK
    // --- room types ------------------------------------------------------
    constexpr uint32_t roomCombat = rgb(234, 242, 248);     // steel/white (NOT cyan)
    constexpr uint32_t roomElite  = rgb(255, 177, 61);      // amber
    constexpr uint32_t roomCache  = rgb(150, 195, 255);     // blue
    constexpr uint32_t roomBoss   = rgb(255, 77, 106);      // red (was magenta)
    // Layout. margin is the 64px design-space safe margin; screens scale it per
    // frame by s = min(w/1920, h/1080) (see uiScale()).
    constexpr float margin = 64.0f;
}

// A premium card surface: soft drop shadow, vertical-gradient body, a 1px top bevel
// highlight, then a hairline border. The depth is what reads as designed over a busy
// scene. Used by every HUD plate, reward card, and menu panel.
void cardPanel(UiDrawList& ui, float x, float y, float w, float h, uint32_t top, uint32_t bot,
               uint32_t border, float t, bool shadow) {
    if (shadow) {
        ui.rect(x - 3.0f, y + 6.0f, w + 6.0f, h, rgba(0, 0, 0, 40));
        ui.rect(x - 1.0f, y + 3.0f, w + 2.0f, h, rgba(0, 0, 0, 78));
    }
    ui.gradientV(x, y, w, h, top, bot, 12);
    ui.rect(x + 1.0f, y + 1.0f, w - 2.0f, 1.0f, pal::bevel);   // top bevel highlight
    ui.rectOutline(x, y, w, h, border, t);
}

// Text with a soft glow halo (4 low-alpha offset passes) under a crisp core pass, for
// titles / currency / emphasis that should feel luminous. Returns the tracked width.
float glowText(UiDrawList& ui, float x, float y, const std::string& s, uint32_t glow,
               uint32_t core, float scale, float tracking) {
    const uint32_t halo = withAlpha(glow, 58);
    ui.textTracked(x - 1.5f, y, s, halo, scale, tracking);
    ui.textTracked(x + 1.5f, y, s, halo, scale, tracking);
    ui.textTracked(x, y - 1.5f, s, halo, scale, tracking);
    ui.textTracked(x, y + 1.5f, s, halo, scale, tracking);
    ui.textTracked(x, y, s, core, scale, tracking);
    return ui.textTrackedWidth(s, scale, tracking);
}
void glowCentered(UiDrawList& ui, float cx, float y, const std::string& s, uint32_t glow,
                  uint32_t core, float scale, float tracking) {
    glowText(ui, cx - ui.textTrackedWidth(s, scale, tracking) * 0.5f, y, s, glow, core, scale, tracking);
}
// DISPLAY-face (Chakra Petch) glow title: same halo treatment but proportional. Returns width.
float glowTextD(UiDrawList& ui, float x, float y, const std::string& s, uint32_t glow,
                uint32_t core, float scale) {
    const uint32_t halo = withAlpha(glow, 58);
    ui.textD(x - 1.5f, y, s, halo, scale);
    ui.textD(x + 1.5f, y, s, halo, scale);
    ui.textD(x, y - 1.5f, s, halo, scale);
    ui.textD(x, y + 1.5f, s, halo, scale);
    ui.textD(x, y, s, core, scale);
    return ui.textDWidth(s, scale);
}
void glowCenteredD(UiDrawList& ui, float cx, float y, const std::string& s, uint32_t glow,
                   uint32_t core, float scale) {
    glowTextD(ui, cx - ui.textDWidth(s, scale) * 0.5f, y, s, glow, core, scale);
}

// A bar with a subtle top gloss (lighter upper band of the fill) for a soft 3D sheen.
void glossBar(UiDrawList& ui, float x, float y, float w, float h, float frac,
              uint32_t track, uint32_t fill) {
    frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    ui.rect(x, y, w, h, track);
    const float fw = w * frac;
    if (fw > 0.0f) {
        ui.rect(x, y, fw, h, fill);
        ui.rect(x, y, fw, std::max(1.0f, h * 0.42f), lerpColor(fill, rgb(255, 255, 255), 0.22f));
    }
}

// A keycap chip: a small gradient box around a short label ([1], Q, SPACE). Returns its
// width so callers can flow text after it. hot = highlighted (affordable / active).
float keycap(UiDrawList& ui, float x, float y, const std::string& label, float ts, bool hot) {
    const float sc = 1.6f * ts;
    const float w = ui.textWidth(label, sc) + 16.0f;
    const float hgt = ui.lineHeight(sc) + 8.0f;
    ui.gradientV(x, y, w, hgt, hot ? rgba(40, 64, 88, 245) : rgba(27, 34, 46, 240),
                 hot ? rgba(24, 41, 58, 245) : rgba(16, 21, 30, 240), 5);
    ui.rectOutline(x, y, w, hgt, hot ? pal::borderHi : pal::border, 1.5f);
    ui.text(x + 8.0f, y + 4.0f, label, hot ? pal::accentGlow : pal::textHi, sc);
    return w;
}

// "LABEL   value": dim label at x, value left-aligned at valueX on a shared baseline.
void statRow(UiDrawList& ui, float x, float valueX, float y, const std::string& label,
             const std::string& value, uint32_t valueCol, float ts) {
    ui.text(x, y, label, pal::textDim, 1.45f * ts);
    ui.text(valueX, y, value, valueCol, 1.45f * ts);
}

// "label .......... value": label left at x0, value right-aligned at x1 (menu rows).
void panelRow(UiDrawList& ui, float x0, float x1, float y, const std::string& label,
              const std::string& value, uint32_t labelCol, uint32_t valueCol, float ts) {
    ui.text(x0, y, label, labelCol, 1.5f * ts);
    ui.textRight(x1, y, value, valueCol, 1.5f * ts);
}

// Greedy word-wrap for the monospace HUD font. Returns the y past the last line.
// Word-wrap `s` into `maxW`, returning the Y just past the last drawn line. When
// `maxLines > 0` the text is clamped to that many lines and the final line is
// ellipsized, so callers that draw fixed content below the text never collide
// with an unexpectedly long string.
float textWrapped(UiDrawList& ui, float x, float y, float maxW, const std::string& s,
                  uint32_t col, float scale, float lineGap, int maxLines = 0) {
    const float cw = ui.textWidth("M", scale);
    const int maxChars = std::max(1, static_cast<int>(maxW / std::max(1.0f, cw)));
    std::string line;
    int drawn = 0;
    auto flush = [&]() {
        if (!line.empty()) { ui.text(x, y, line, col, scale); y += ui.lineHeight(scale) + lineGap; ++drawn; line.clear(); }
    };
    size_t i = 0;
    bool clamped = false;
    while (i < s.size()) {
        const size_t sp = s.find(' ', i);
        const std::string word = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = (sp == std::string::npos) ? s.size() : sp + 1;
        if (word.empty()) continue;
        if (line.empty()) line = word;
        else if (static_cast<int>(line.size() + 1 + word.size()) <= maxChars) line += " " + word;
        else {
            // The current line is full; if drawing another would exceed the cap,
            // ellipsize this line in place and stop.
            if (maxLines > 0 && drawn + 1 >= maxLines) { clamped = true; break; }
            flush();
            line = word;
        }
    }
    if (clamped) {
        if (static_cast<int>(line.size()) > maxChars - 2) line = line.substr(0, std::max(0, maxChars - 2));
        line += "..";
    }
    flush();
    return y;
}

float textDWrapped(UiDrawList& ui, float x, float y, float maxW, const std::string& s,
                   uint32_t col, float scale, float lineGap, int maxLines = 0) {
    std::string line;
    int drawn = 0;
    auto drawLine = [&](std::string out) {
        ui.textD(x, y, out, col, scale);
        y += ui.dLineHeight(scale) + lineGap;
        ++drawn;
    };
    auto ellipsize = [&](std::string out) {
        while (!out.empty() && ui.textDWidth(out + "..", scale) > maxW) out.pop_back();
        return out + "..";
    };
    auto flush = [&]() {
        if (!line.empty()) {
            drawLine(line);
            line.clear();
        }
    };
    size_t i = 0;
    while (i < s.size()) {
        const size_t sp = s.find(' ', i);
        std::string word = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = (sp == std::string::npos) ? s.size() : sp + 1;
        if (word.empty()) continue;

        if (!line.empty() && ui.textDWidth(line + " " + word, scale) <= maxW) {
            line += " " + word;
            continue;
        }
        if (line.empty() && ui.textDWidth(word, scale) <= maxW) {
            line = word;
            continue;
        }
        if (maxLines > 0 && drawn + 1 >= maxLines) {
            const std::string tail = line.empty() ? word : (line + " " + word);
            drawLine(ellipsize(tail));
            return y;
        }
        flush();
        line = word;
        if (maxLines > 0 && drawn >= maxLines) return y;
    }
    if (!line.empty()) {
        if (maxLines > 0 && drawn + 1 > maxLines) drawLine(ellipsize(line));
        else drawLine(line);
    }
    return y;
}

// ===========================================================================
// Neon Ink Brutalism component kit (In-game menus and UI design handoff). One
// chamfer language, role-locked colour, one signature (the pulse waveform). The
// design is authored @1920x1080; every screen scales design-space px by
// s = uiScale(w, h) so the layout is proportional on any back-buffer.
// ---------------------------------------------------------------------------

// Uniform UI scale: design space is 1920x1080; keep content inside the buffer.
inline float uiScale(int w, int h) {
    return std::min(static_cast<float>(w) / 1920.0f, static_cast<float>(h) / 1080.0f);
}
// Design-space font px -> engine text scale (glyph cell height == designPx * s).
inline float fpx(const UiDrawList& ui, float designPx, float s) {
    return designPx * s / std::max(1.0f, ui.lineHeight(1.0f));
}

float textDTrackedWidth(const UiDrawList& ui, std::string_view text, float scale, float tracking) {
    float w = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i > 0) w += tracking;
        w += ui.textDWidth(std::string_view(&text[i], 1), scale);
    }
    return w;
}

float textDTracked(UiDrawList& ui, float x, float y, std::string_view text,
                   uint32_t color, float scale, float tracking) {
    const float start = x;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i > 0) x += tracking;
        if (text[i] != ' ') ui.textD(x, y, std::string_view(&text[i], 1), color, scale);
        x += ui.textDWidth(std::string_view(&text[i], 1), scale);
    }
    return x - start;
}

void textDTrackedRight(UiDrawList& ui, float right, float y, std::string_view text,
                       uint32_t color, float scale, float tracking) {
    textDTracked(ui, right - textDTrackedWidth(ui, text, scale, tracking), y, text, color, scale, tracking);
}

void textTrackedRight(UiDrawList& ui, float right, float y, std::string_view text,
                      uint32_t color, float scale, float tracking) {
    ui.textTracked(right - ui.textTrackedWidth(text, scale, tracking), y, text, color, scale, tracking);
}

// The signature chamfer: cut the TOP-LEFT and BOTTOM-RIGHT corners at `cut` px
// (1:1 / 45deg). Drawn as the hexagon hull directly so the cut corners are
// transparent on ANY background (over the live world or a scrim). Hull points:
//   A(x0+cut,y0) B(x1,y0) C(x1,y1-cut) D(x1-cut,y1) E(x0,y1) F(x0,y0+cut)
void chamferFill(UiDrawList& ui, float x, float y, float w, float h, float cut, uint32_t col) {
    cut = std::max(0.0f, std::min(cut, std::min(w, h) * 0.5f));
    const float x0 = x, x1 = x + w, y0 = y, y1 = y + h;
    // Hull A B C D E F as a convex polygon so the chamfer cut corners get AA silhouettes.
    const float hull[12] = { x0 + cut, y0, x1, y0, x1, y1 - cut, x1 - cut, y1, x0, y1, x0, y0 + cut };
    ui.convexFill(hull, 6, col);
}
void chamferOutline(UiDrawList& ui, float x, float y, float w, float h, float cut, uint32_t col, float t) {
    cut = std::max(0.0f, std::min(cut, std::min(w, h) * 0.5f));
    const float x0 = x, x1 = x + w, y0 = y, y1 = y + h;
    ui.line(x0 + cut, y0, x1, y0, t, col);        // top    A-B
    ui.line(x1, y0, x1, y1 - cut, t, col);        // right  B-C
    ui.line(x1, y1 - cut, x1 - cut, y1, t, col);  // BR cut C-D
    ui.line(x1 - cut, y1, x0, y1, t, col);        // bottom D-E
    ui.line(x0, y1, x0, y0 + cut, t, col);        // left   E-F
    ui.line(x0, y0 + cut, x0 + cut, y0, t, col);  // TL cut F-A
}
// A chamfered panel/card: optional drop shadow, flat fill (brutalist restraint),
// a 1px top bevel highlight, a faint bottom darken for depth, then the chamfer
// border. The ONE surface primitive behind every menu panel, card, and HUD plate.
void chamferPanel(UiDrawList& ui, float x, float y, float w, float h, float cut,
                  uint32_t fill, uint32_t border, float t, bool shadow) {
    if (shadow) {
        chamferFill(ui, x - 5.0f, y + 10.0f, w + 10.0f, h + 2.0f, cut + 2.0f, rgba(0, 0, 0, 30));
        chamferFill(ui, x - 2.0f, y + 5.0f, w + 4.0f, h, cut + 1.0f, rgba(0, 0, 0, 60));
        chamferFill(ui, x + 1.0f, y + 2.0f, w, h, cut, rgba(0, 0, 0, 72));
    }
    chamferFill(ui, x, y, w, h, cut, fill);
    // bottom-left + top-right corners are square, so these bands stay inside the
    // hull (top spans A->B, bottom spans E->D).
    ui.rect(x + cut, y + 1.0f, w - cut, 1.0f, pal::bevel);       // top bevel
    ui.rect(x + cut, y + 2.0f, w - cut - 2.0f, 1.0f, rgba(255, 255, 255, 8));
    ui.rect(x + 1.0f, y + h - 5.0f, w - cut - 1.0f, 5.0f, rgba(0, 0, 0, 38));  // bottom darken
    ui.rect(x + w - 2.0f, y + 2.0f, 1.0f, h - cut - 3.0f, rgba(255, 255, 255, 8));
    chamferOutline(ui, x, y, w, h, cut, border, t);
}

void notchBRFill(UiDrawList& ui, float x, float y, float w, float h, float cut, uint32_t col) {
    cut = std::max(0.0f, std::min(cut, std::min(w, h) * 0.5f));
    const float x0 = x, x1 = x + w, y0 = y, y1 = y + h;
    const float hull[10] = { x0, y0, x1, y0, x1, y1 - cut, x1 - cut, y1, x0, y1 };
    ui.convexFill(hull, 5, col);
}

void notchBROutline(UiDrawList& ui, float x, float y, float w, float h, float cut,
                    uint32_t col, float t) {
    cut = std::max(0.0f, std::min(cut, std::min(w, h) * 0.5f));
    const float x0 = x, x1 = x + w, y0 = y, y1 = y + h;
    ui.line(x0, y0, x1, y0, t, col);
    ui.line(x1, y0, x1, y1 - cut, t, col);
    ui.line(x1, y1 - cut, x1 - cut, y1, t, col);
    ui.line(x1 - cut, y1, x0, y1, t, col);
    ui.line(x0, y1, x0, y0, t, col);
}

void notchBRPanel(UiDrawList& ui, float x, float y, float w, float h, float cut,
                  uint32_t fill, uint32_t border, float t) {
    notchBRFill(ui, x, y, w, h, cut, fill);
    ui.rect(x + 1.0f, y + 1.0f, w - 2.0f, 1.0f, rgba(120, 200, 230, 16));
    notchBROutline(ui, x, y, w, h, cut, border, t);
}

void screenBrackets(UiDrawList& ui, float w, float h, float s) {
    const uint32_t c = rgba(80, 210, 235, 115);
    const float m = 30.0f * s, len = 36.0f * s, t = std::max(1.5f, 2.0f * s);
    ui.line(m, m, m + len, m, t, c); ui.line(m, m, m, m + len, t, c);
    ui.line(w - m, m, w - m - len, m, t, c); ui.line(w - m, m, w - m, m + len, t, c);
    ui.line(m, h - m, m + len, h - m, t, c); ui.line(m, h - m, m, h - m - len, t, c);
    ui.line(w - m, h - m, w - m - len, h - m, t, c); ui.line(w - m, h - m, w - m, h - m - len, t, c);
}

void scanlines(UiDrawList& ui, float w, float h, float s) {
    const float step = std::max(3.0f, 3.0f * s);
    for (float y = 2.0f * s; y < h; y += step)
        ui.rect(0.0f, y, w, std::max(1.0f, 1.0f * s), rgba(0, 0, 0, 56));
}

// The base-colour-INDEPENDENT card selection marker: an inset cyan chamfer ring
// (2px) with a soft outer glow. Reads identically on any tier/type, so selection
// never collides with a card's own colour (the handoff's core focus rule).
void focusRing(UiDrawList& ui, float x, float y, float w, float h, float cut, float s) {
    const float inset = 4.0f * s;
    const float gx = x + inset, gy = y + inset, gw = w - inset * 2.0f, gh = h - inset * 2.0f;
    const float gc = std::max(2.0f, cut - inset);
    chamferOutline(ui, gx - 5.0f, gy - 5.0f, gw + 10.0f, gh + 10.0f, gc + 5.0f, withAlpha(pal::accentGlow, 24), std::max(3.0f, 5.0f * s));
    chamferOutline(ui, gx - 2.0f, gy - 2.0f, gw + 4.0f, gh + 4.0f, gc + 2.0f, withAlpha(pal::accentGlow, 58), std::max(2.0f, 3.0f * s));
    chamferOutline(ui, gx, gy, gw, gh, gc, pal::accent, std::max(1.5f, 2.0f * s));
}

// The pulse waveform (the brand mark). Heartbeat polyline across [x, x+w] about
// baseline cy, amplitude amp. Bright `fill` up to `frac` of the width over a
// faint full-width `track`. Used ONLY for the combat intensity meter and the Hub
// Start Run button (the handoff restricts the signature to these two places).
void pulseWave(UiDrawList& ui, float x, float cy, float w, float amp, float frac,
               uint32_t track, uint32_t fill, float stroke) {
    static const float pts[7][2] = {
        { 0.00f, 0.50f }, { 0.40f, 0.50f }, { 0.46f, 0.18f }, { 0.52f, 0.82f },
        { 0.58f, 0.29f }, { 0.63f, 0.50f }, { 1.00f, 0.50f }
    };
    auto PX = [&](int i) { return x + pts[i][0] * w; };
    auto PY = [&](int i) { return cy + (pts[i][1] - 0.5f) * 2.0f * amp; };
    // A round join at a vertex (a small AA disc) so the sharp spike corners read smooth
    // instead of leaving a mitered notch between two butt-capped segments.
    auto join = [&](float jx, float jy, float r, uint32_t col) {
        float h[16];
        for (int k = 0; k < 8; ++k) {
            const float a = static_cast<float>(k) * 0.7853982f;   // 8-gon ~ disc
            h[k * 2] = jx + std::cos(a) * r; h[k * 2 + 1] = jy + std::sin(a) * r;
        }
        ui.convexFill(h, 8, col);
    };
    for (int i = 0; i + 1 < 7; ++i) ui.line(PX(i), PY(i), PX(i + 1), PY(i + 1), stroke * 0.85f, track);
    for (int i = 1; i < 6; ++i) join(PX(i), PY(i), stroke * 0.42f, track);
    const float fx = x + clamp(frac, 0.0f, 1.0f) * w;
    for (int i = 0; i + 1 < 7; ++i) {
        float ax = PX(i), ay = PY(i), bx = PX(i + 1), by = PY(i + 1);
        if (ax >= fx) break;
        if (bx > fx) { const float tt = (fx - ax) / std::max(0.001f, bx - ax); bx = ax + (bx - ax) * tt; by = ay + (by - ay) * tt; }
        ui.line(ax, ay, bx, by, stroke, fill);
        if (i >= 1 && PX(i) <= fx) join(PX(i), PY(i), stroke * 0.5f, fill);
    }
}

void gradientH(UiDrawList& ui, float x, float y, float w, float h,
               uint32_t left, uint32_t right, int bands = 24) {
    if (bands < 1) bands = 1;
    const float bw = w / static_cast<float>(bands);
    for (int i = 0; i < bands; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(bands);
        ui.rect(x + bw * static_cast<float>(i), y, bw + 1.0f, h, lerpColor(left, right, t));
    }
}

void pulseWaveLong(UiDrawList& ui, float x, float cy, float w, float amp, uint32_t col,
                   uint32_t faint, float stroke) {
    static const float pts[][2] = {
        {0.000f, .50f}, {0.123f, .50f}, {0.140f, .50f}, {0.151f, .23f},
        {0.161f, .77f}, {0.171f, .50f}, {0.267f, .50f}, {0.278f, .37f},
        {0.288f, .61f}, {0.298f, .50f}, {0.427f, .50f}, {0.449f, .15f},
        {0.460f, .85f}, {0.471f, .50f}, {0.552f, .50f}, {0.563f, .39f},
        {0.575f, .59f}, {0.586f, .50f}, {0.698f, .50f}, {0.726f, .22f},
        {0.738f, .78f}, {0.749f, .50f}, {0.865f, .50f}, {0.889f, .42f},
        {0.900f, .56f}, {0.910f, .50f}, {1.000f, .50f}
    };
    const int n = static_cast<int>(sizeof(pts) / sizeof(pts[0]));
    auto px = [&](int i) { return x + pts[i][0] * w; };
    auto py = [&](int i) { return cy + (pts[i][1] - 0.5f) * 2.0f * amp; };
    for (int i = 0; i + 1 < n; ++i) ui.line(px(i), py(i), px(i + 1), py(i + 1), stroke * 0.72f, faint);
    for (int i = 0; i + 1 < n; ++i) ui.line(px(i), py(i), px(i + 1), py(i + 1), stroke, col);
}

// A solid diamond (rotated square) centred at (cx,cy), half-extent r.
void diamond(UiDrawList& ui, float cx, float cy, float r, uint32_t col) {
    const float hull[8] = { cx, cy - r, cx + r, cy, cx, cy + r, cx - r, cy };
    ui.convexFill(hull, 4, col);
}
void diamondOutline(UiDrawList& ui, float cx, float cy, float r, uint32_t col, float t) {
    ui.line(cx, cy - r, cx + r, cy, t, col); ui.line(cx + r, cy, cx, cy + r, t, col);
    ui.line(cx, cy + r, cx - r, cy, t, col); ui.line(cx - r, cy, cx, cy - r, t, col);
}

// The authoritative Pulse state colour (the color law: dark -> cyan -> bright cyan ->
// ice-white -> pure white). Used by the meter, the boss afflictions, and the Field Manual
// so every surface tracks the same band hue. No magenta.
uint32_t pulseStateColor(PulseTier t) {
    switch (t) {
        case PulseTier::Dormant:   return pal::textDim;
        case PulseTier::Charged:   return lerpColor(pal::accent, pal::textDim, 0.40f);
        case PulseTier::Surging:   return pal::accent;
        case PulseTier::Burning:   return pal::ionWhite;
        case PulseTier::Overpulse: return pal::ionGlow;
    }
    return pal::textDim;
}

// THE PULSE METER (signature). Anatomy left->right: a chamfered number cap (cut bottom-left)
// holding the live 0-100 integer; a diagonal-hatch track (cut bottom-right); a gradient fill
// (base teal -> state colour) to value%; a bloomed leading head that opacity-pulses (~1.1s);
// threshold notches at 20 / 50 / 80 (80 warm). STATE . DIRECTION is drawn under it. Built once
// so the compact HUD meter and the expanded Field Manual meter share one definition.
// `clock` is a free-running seconds value (drives the head pulse). value 0..100.
void drawPulseMeter(UiDrawList& ui, float x, float y, float w, float h, int value,
                    PulseTier tier, PulseDir dir, float loss01, float clock, float s,
                    bool labelBelow) {
    value = value < 0 ? 0 : (value > 100 ? 100 : value);
    const float frac = static_cast<float>(value) / 100.0f;
    const uint32_t stateCol = pulseStateColor(tier);
    const uint32_t numCol   = (tier == PulseTier::Dormant) ? pal::textMid : stateCol;
    const float cut = std::min(8.0f * s, h * 0.40f);

    // 1) Number cap (cut bottom-left).
    const float numSc = fpx(ui, h * 0.62f, 1.0f);  // glyph height ~62% of the bar height
    const float capW  = std::max(h * 1.55f, ui.textWidth("100", numSc) + 16.0f * s);
    const float cx0 = x, cy0 = y, cx1 = x + capW, cy1 = y + h;
    {
        const float hull[10] = { cx0, cy0, cx1, cy0, cx1, cy1, cx0 + cut, cy1, cx0, cy1 - cut };
        ui.convexFill(hull, 5, pal::surface2);
        ui.line(cx0, cy0, cx1, cy0, 1.0f, stateCol);
        ui.line(cx1, cy0, cx1, cy1, std::max(1.5f, 1.5f * s), stateCol);
        ui.line(cx1, cy1, cx0 + cut, cy1, 1.0f, stateCol);
        ui.line(cx0 + cut, cy1, cx0, cy1 - cut, std::max(1.5f, 1.5f * s), stateCol);  // BL cut
        ui.line(cx0, cy1 - cut, cx0, cy0, 1.0f, stateCol);
    }
    const std::string num = std::to_string(value);
    ui.textCentered(cx0 + capW * 0.5f, cy0 + (h - ui.lineHeight(numSc)) * 0.5f, num, numCol, numSc);

    // 2) Track (cut bottom-right) over the remaining width.
    const float gap = 5.0f * s;
    const float tx = cx1 + gap, tw = std::max(20.0f * s, (x + w) - tx), ty = y, th = h;
    const float tx1 = tx + tw, ty1 = ty + th;
    {
        const float hull[10] = { tx, ty, tx1, ty, tx1, ty1 - cut, tx1 - cut, ty1, tx, ty1 };
        ui.convexFill(hull, 5, rgb(10, 18, 26));   // empty: deep hatched void base
    }
    // diagonal-hatch the EMPTY portion (135deg ticks kept inside the bar bounds).
    const float fillEdge = tx + frac * tw;
    for (float hx = tx + 2.0f * s; hx + (th - 4.0f * s) <= tx1; hx += 9.0f * s) {
        if (hx < fillEdge - 1.0f) continue;        // only over empty
        ui.line(hx, ty1 - 2.0f * s, hx + (th - 4.0f * s), ty + 2.0f * s, 1.0f, rgba(120, 150, 175, 16));
    }
    // 3) Gradient fill (base teal -> state colour along the width) up to value%.
    if (frac > 0.001f) {
        const float fw = fillEdge - tx;
        const int bands = std::max(2, static_cast<int>(fw / (6.0f * s)));
        const uint32_t base = rgb(16, 86, 108);
        for (int b = 0; b < bands; ++b) {
            const float u = static_cast<float>(b) / static_cast<float>(bands);
            const float bx = tx + u * fw, bw = fw / static_cast<float>(bands) + 1.0f;
            ui.rect(bx, ty + 1.0f * s, bw, th - 2.0f * s, lerpColor(base, stateCol, u));
        }
        // 4) Leading head: a bright bloomed bar at the fill edge that opacity-pulses.
        const float headPulse = 0.55f + 0.45f * std::sin(clock * (6.2831853f / 1.1f));
        const uint32_t headBright = lerpColor(stateCol, rgb(255, 255, 255), 0.45f);
        ui.rect(fillEdge - 6.0f * s, ty - 2.0f * s, 8.0f * s, th + 4.0f * s,
                withAlpha(headBright, static_cast<uint8_t>(60.0f * headPulse)));
        ui.rect(fillEdge - std::max(2.0f, 2.0f * s), ty, std::max(3.0f, 3.5f * s), th,
                withAlpha(headBright, static_cast<uint8_t>(190.0f + 60.0f * headPulse > 255.0f ? 255.0f : 190.0f + 60.0f * headPulse)));
    }
    // 5) Threshold notches at 20 / 50 / 80 (80 tinted warm).
    for (int n = 0; n < 3; ++n) {
        const int marks[3] = { 20, 50, 80 };
        const float nx = tx + (static_cast<float>(marks[n]) / 100.0f) * tw;
        const uint32_t nc = marks[n] == 80 ? withAlpha(pal::amber, 150) : withAlpha(pal::textFaint, 130);
        ui.rect(nx, ty + 2.0f * s, std::max(1.0f, 1.0f * s), th - 4.0f * s, nc);
    }
    // track outline last (over fill/notches), with the bottom-right cut.
    ui.line(tx, ty, tx1, ty, 1.0f, withAlpha(stateCol, 150));
    ui.line(tx1, ty, tx1, ty1 - cut, 1.0f, withAlpha(stateCol, 150));
    ui.line(tx1, ty1 - cut, tx1 - cut, ty1, 1.0f, withAlpha(stateCol, 150));
    ui.line(tx1 - cut, ty1, tx, ty1, 1.0f, withAlpha(stateCol, 150));
    ui.line(tx, ty1, tx, ty, 1.0f, withAlpha(stateCol, 150));
    // loss flash: a brief red wash after a hit (the FALLING / knock-down tell).
    if (loss01 > 0.001f)
        ui.rect(tx, ty, tw, th, withAlpha(pal::danger, static_cast<uint8_t>(80.0f * clamp(loss01, 0.0f, 1.0f))));

    // Label row ABOVE the bar (per the UI guide): a small "PULSE" tag at the left and the
    // STATE . DIRECTION readout right-aligned (words, never arrows alone).
    if (labelBelow) {
        const float lblSc = fpx(ui, 13.0f, s), trk = 1.6f * s;
        const float lyy = y - ui.lineHeight(lblSc) - 5.0f * s;
        ui.textTracked(x, lyy, "PULSE", pal::textFaint, lblSc, trk);
        const std::string st = pulseTierName(tier);
        const std::string dr = pulseDirName(dir);
        const std::string sep = "  .  ";
        const float wState = ui.textTrackedWidth(st, lblSc, trk);
        const float wSep   = ui.textTrackedWidth(sep, lblSc, trk);
        const float wDir   = ui.textTrackedWidth(dr, lblSc, trk);
        float lx = (x + w) - (wState + wSep + wDir);
        ui.textTracked(lx, lyy, st, stateCol, lblSc, trk); lx += wState;
        ui.textTracked(lx, lyy, sep, pal::textFaint, lblSc, trk); lx += wSep;
        ui.textTracked(lx, lyy, dr, pal::textMid, lblSc, trk);
    }
}

// HUD ability pip (30x30 design). state 0 ready / 1 charging / 2 empty;
// shape 0 diamond (dash) / 1 triangle (tactical) / 2 square (ult). Charging shows
// a bottom-up slate fill = charge. Key letter drawn beneath. No ability name text.
void abilityPip(UiDrawList& ui, float x, float y, float sz, int state, int shape,
                float charge, const std::string& key, float s) {
    const float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.5f;
    const float t = std::max(1.5f, 1.5f * s);
    if (state == 0) {                               // ready: filled cyan + glow
        diamond(ui, cx, cy, r + 2.0f * s, withAlpha(pal::accentGlow, 60));
        if (shape == 1) ui.triangle(cx, y, x + sz, y + sz, x, y + sz, pal::accent);
        else if (shape == 2) ui.rect(x, y, sz, sz, pal::accent);
        else diamond(ui, cx, cy, r, pal::accent);
    } else if (state == 1) {                        // charging: outline + bottom-up fill
        const float fh = sz * clamp(charge, 0.0f, 1.0f);
        ui.rect(x, y + sz - fh, sz, fh, withAlpha(pal::textDim, 150));
        if (shape == 1) { ui.line(cx, y, x + sz, y + sz, t, pal::textDim); ui.line(x + sz, y + sz, x, y + sz, t, pal::textDim); ui.line(x, y + sz, cx, y, t, pal::textDim); }
        else if (shape == 2) ui.rectOutline(x, y, sz, sz, pal::textDim, t);
        else diamondOutline(ui, cx, cy, r, pal::textDim, t);
    } else {                                        // empty: faint outline only
        if (shape == 1) { ui.line(cx, y, x + sz, y + sz, t, pal::disabled); ui.line(x + sz, y + sz, x, y + sz, t, pal::disabled); ui.line(x, y + sz, cx, y, t, pal::disabled); }
        else if (shape == 2) ui.rectOutline(x, y, sz, sz, pal::disabled, t);
        else diamondOutline(ui, cx, cy, r, pal::disabled, t);
    }
    ui.textCentered(cx, y + sz + 3.0f * s, key, state == 0 ? rgb(205, 252, 255) : pal::textDim, fpx(ui, 13.0f, s));
}

// HUD ability CHIP (the UI-guide pill): a chamfered button with the ability label. Ready =
// filled + bordered in readyCol; charging = a left-to-right charge fill + the label, with the
// percent appended when showPct (used for the slow-charging ULT, e.g. "ULT 60%"). Returns its
// width so the caller can flow the next chip.
float abilityChip(UiDrawList& ui, float x, float y, const std::string& label, bool ready,
                  float charge01, uint32_t readyCol, bool showPct, float s) {
    const float lblSc = fpx(ui, 12.0f, s);
    std::string txt = label;
    if (!ready && showPct) {
        const int pct = static_cast<int>(std::lround(clamp(charge01, 0.0f, 1.0f) * 100.0f));
        if (pct > 0) txt += " " + std::to_string(pct) + "%";   // suppress a noisy "0%" at empty charge
    }
    const float pad = 9.0f * s, h = 22.0f * s, cutc = 6.0f * s;
    const float wch = ui.textTrackedWidth(txt, lblSc, 0.8f * s) + pad * 2.0f;
    chamferFill(ui, x, y, wch, h, cutc, rgba(12, 16, 24, 200));
    if (!ready && charge01 > 0.0f) {        // charge fill (clipped to the pill width)
        const float fw = (wch - 2.0f * s) * clamp(charge01, 0.0f, 1.0f);
        ui.rect(x + 1.0f * s, y + 1.0f * s, fw, h - 2.0f * s, withAlpha(pal::accent, 46));
    }
    chamferOutline(ui, x, y, wch, h, cutc, ready ? readyCol : pal::inkStroke, std::max(1.0f, (ready ? 1.5f : 1.0f) * s));
    ui.textTracked(x + pad, y + (h - ui.lineHeight(lblSc)) * 0.5f, txt, ready ? readyCol : pal::textDim, lblSc, 0.8f * s);
    return wch;
}

// HUD status chip: tinted fill + a 2px left edge in the state colour + a small
// diamond marker + NAME + timer value. buff = cyan, debuff = danger red. Shown only
// while active. Returns its width so callers can stack chips.
float statusChip(UiDrawList& ui, float x, float y, const std::string& name,
                 const std::string& timer, bool debuff, float s) {
    const uint32_t edge = debuff ? pal::danger : pal::accent;
    const float nameSc = fpx(ui, 13.0f, s), timeSc = fpx(ui, 14.0f, s);
    const float pad = 10.0f * s, h = 28.0f * s, dia = 4.0f * s;
    const float nameW = ui.textTrackedWidth(name, nameSc, 0.6f * s);
    const float timeW = timer.empty() ? 0.0f : ui.textWidth(timer, timeSc) + 8.0f * s;
    const float w = pad + dia * 2.0f + 6.0f * s + nameW + 8.0f * s + timeW + pad;
    ui.rect(x, y, w, h, withAlpha(edge, 30));
    ui.rect(x, y, std::max(2.0f, 2.0f * s), h, edge);
    diamond(ui, x + pad + dia, y + h * 0.5f, dia, edge);
    ui.textTracked(x + pad + dia * 2.0f + 6.0f * s, y + (h - ui.lineHeight(nameSc)) * 0.5f, name, debuff ? pal::dangerHi : pal::accentGlow, nameSc, 0.6f * s);
    if (!timer.empty()) ui.textRight(x + w - pad, y + (h - ui.lineHeight(timeSc)) * 0.5f, timer, pal::textHi, timeSc);
    return w;
}

// Drawn primitive icons (no external image assets). A chamfered tile holds the
// silhouette; tinted by the card's tier/type colour.
enum class IconKind { Scope, Rifle, Bullet, Lightning, Shield, Crate, Star, Reticle, Diamond, Beam,
                      Flame, Snow, Drop, Bolt, Heart, Coin, Run, Anvil };
void drawIcon(UiDrawList& ui, IconKind k, float cx, float cy, float r, uint32_t col, float s) {
    const float t = std::max(1.5f, 2.0f * s);
    switch (k) {
        case IconKind::Flame: {  // pyro / burn: a clean AA flame (single smooth hull)
            const float h[10] = { cx, cy - r,  cx + 0.60f * r, cy + 0.32f * r,  cx + 0.40f * r, cy + 0.95f * r,
                                  cx - 0.40f * r, cy + 0.95f * r,  cx - 0.60f * r, cy + 0.32f * r };
            ui.convexFill(h, 5, col);
            break;
        }
        case IconKind::Snow:    // cryo: a six-spoke star
            for (int i = 0; i < 3; ++i) {
                const float a = static_cast<float>(i) * 1.0472f;   // 60 deg
                ui.line(cx - std::cos(a) * r, cy - std::sin(a) * r, cx + std::cos(a) * r, cy + std::sin(a) * r, t, col);
            }
            break;
        case IconKind::Drop: {   // acid / corrode: a clean AA teardrop
            const float h[10] = { cx, cy - r,  cx + 0.56f * r, cy + 0.18f * r,  cx + 0.40f * r, cy + 0.82f * r,
                                  cx - 0.40f * r, cy + 0.82f * r,  cx - 0.56f * r, cy + 0.18f * r };
            ui.convexFill(h, 5, col);
            break;
        }
        case IconKind::Bolt:    // kinetic/dash: a forward chevron pair
            ui.triangle(cx - r * 0.6f, cy - r * 0.7f, cx + r * 0.1f, cy, cx - r * 0.6f, cy + r * 0.7f, col);
            ui.triangle(cx, cy - r * 0.7f, cx + r * 0.7f, cy, cx, cy + r * 0.7f, col);
            break;
        case IconKind::Heart:   // health: two lobes + a base
            ui.triangle(cx - r * 0.5f, cy - r * 0.3f, cx, cy + r * 0.1f, cx - r, cy + r * 0.2f, col);
            ui.triangle(cx + r * 0.5f, cy - r * 0.3f, cx, cy + r * 0.1f, cx + r, cy + r * 0.2f, col);
            ui.triangle(cx - r, cy + r * 0.2f, cx + r, cy + r * 0.2f, cx, cy + r, col);
            break;
        case IconKind::Coin:    // scrap/currency: a diamond ring
            diamond(ui, cx, cy, r * 0.85f, col);
            diamond(ui, cx, cy, r * 0.4f, rgb(20, 24, 32));
            break;
        case IconKind::Run:     // momentum/move: three speed lines
            ui.line(cx - r, cy - r * 0.5f, cx + r * 0.4f, cy - r * 0.5f, t, col);
            ui.line(cx - r, cy, cx + r, cy, t, col);
            ui.line(cx - r, cy + r * 0.5f, cx + r * 0.4f, cy + r * 0.5f, t, col);
            break;
        case IconKind::Anvil:   // forge: a blocky anvil
            ui.rect(cx - r * 0.8f, cy - r * 0.2f, r * 1.6f, r * 0.5f, col);
            ui.rect(cx - r * 0.3f, cy + r * 0.3f, r * 0.6f, r * 0.5f, col);
            break;
        case IconKind::Reticle:
        case IconKind::Scope:
            ui.line(cx - r, cy, cx - r * 0.45f, cy, t, col); ui.line(cx + r * 0.45f, cy, cx + r, cy, t, col);
            ui.line(cx, cy - r, cx, cy - r * 0.45f, t, col); ui.line(cx, cy + r * 0.45f, cx, cy + r, t, col);
            ui.line(cx - r * 0.7f, cy - r * 0.7f, cx + r * 0.7f, cy - r * 0.7f, t, col);
            ui.line(cx + r * 0.7f, cy - r * 0.7f, cx + r * 0.7f, cy + r * 0.7f, t, col);
            ui.line(cx + r * 0.7f, cy + r * 0.7f, cx - r * 0.7f, cy + r * 0.7f, t, col);
            ui.line(cx - r * 0.7f, cy + r * 0.7f, cx - r * 0.7f, cy - r * 0.7f, t, col);
            ui.rect(cx - 1.5f * s, cy - 1.5f * s, 3.0f * s, 3.0f * s, col);
            break;
        case IconKind::Rifle:
            ui.line(cx - r * 0.88f, cy - r * 0.05f, cx + r * 0.72f, cy - r * 0.05f, t, col);
            ui.line(cx + r * 0.72f, cy - r * 0.05f, cx + r, cy - r * 0.17f, std::max(1.0f, t * 0.7f), col);
            ui.line(cx - r * 0.82f, cy - r * 0.02f, cx - r * 0.48f, cy + r * 0.28f, t, col);
            ui.line(cx - r * 0.30f, cy + r * 0.02f, cx - r * 0.12f, cy + r * 0.62f, t, col);
            ui.line(cx + r * 0.03f, cy + r * 0.02f, cx + r * 0.28f, cy + r * 0.50f, t, col);
            ui.line(cx - r * 0.04f, cy + r * 0.02f, cx + r * 0.38f, cy + r * 0.02f, std::max(1.0f, t * 0.65f), col);
            ui.line(cx + r * 0.14f, cy - r * 0.30f, cx + r * 0.42f, cy - r * 0.30f, std::max(1.0f, t * 0.65f), col);
            break;
        case IconKind::Beam:
            ui.line(cx - r, cy, cx + r, cy, t * 1.4f, col);
            ui.triangle(cx + r * 0.5f, cy - r * 0.4f, cx + r, cy, cx + r * 0.5f, cy + r * 0.4f, col);
            break;
        case IconKind::Bullet:
            ui.triangle(cx, cy - r, cx + r * 0.55f, cy - r * 0.3f, cx - r * 0.55f, cy - r * 0.3f, col);
            ui.rect(cx - r * 0.55f, cy - r * 0.3f, r * 1.1f, r * 1.1f, col);
            break;
        case IconKind::Lightning:   // volt / shock: a clean AA bolt (smooth 3-segment polyline)
            ui.line(cx + 0.34f * r, cy - r,       cx - 0.24f * r, cy + 0.10f * r, t * 1.35f, col);
            ui.line(cx - 0.24f * r, cy + 0.10f * r, cx + 0.20f * r, cy - 0.02f * r, t * 1.35f, col);
            ui.line(cx + 0.20f * r, cy - 0.02f * r, cx - 0.30f * r, cy + r,        t * 1.35f, col);
            break;
        case IconKind::Shield: {    // bulwark: a clean AA shield
            const float h[12] = { cx, cy - r,  cx + 0.78f * r, cy - 0.52f * r,  cx + 0.66f * r, cy + 0.42f * r,
                                  cx, cy + r,  cx - 0.66f * r, cy + 0.42f * r,  cx - 0.78f * r, cy - 0.52f * r };
            ui.convexFill(h, 6, col);
            break;
        }
        case IconKind::Crate:
            ui.rectOutline(cx - r * 0.8f, cy - r * 0.8f, r * 1.6f, r * 1.6f, col, t);
            ui.line(cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, t, col);
            ui.line(cx + r * 0.8f, cy - r * 0.8f, cx - r * 0.8f, cy + r * 0.8f, t, col);
            break;
        case IconKind::Star:
            for (int i = 0; i < 5; ++i) {
                const float a0 = -1.5708f + static_cast<float>(i) * 1.2566f;
                const float a1 = a0 + 1.2566f;
                ui.triangle(cx, cy, cx + std::cos(a0) * r, cy + std::sin(a0) * r,
                            cx + std::cos(a1) * r, cy + std::sin(a1) * r, col);
            }
            break;
        case IconKind::Diamond: diamond(ui, cx, cy, r * 0.9f, col); break;
    }
}
void iconTile(UiDrawList& ui, float x, float y, float sz, float cut, IconKind k, uint32_t col, float s) {
    chamferFill(ui, x, y, sz, sz, cut, withAlpha(col, 22));
    chamferOutline(ui, x, y, sz, sz, cut, col, std::max(1.5f, 1.5f * s));
    drawIcon(ui, k, x + sz * 0.5f, y + sz * 0.5f, sz * 0.32f, col, s);
}

// M7 SHARED VISUAL LANGUAGE: every surface (reward cards, combat HUD, hub, shop) renders the four
// elements and six affinities the SAME way - one colour + one glyph + plain-language effect text -
// so a player learns "orange flame = Burn = damage over time" once and reads it everywhere.
struct ElemVis { uint32_t col; IconKind icon; const char* name; const char* effect; };
ElemVis elemVis(int e) {   // e maps to Status Element: 1 Burn, 2 Shock, 3 Cryo, 4 Corrode
    switch (e) {
        case 1: return { pal::affPyro,  IconKind::Flame,     "BURN",    "damage over time" };
        case 2: return { pal::affVolt,  IconKind::Lightning, "SHOCK",   "builds up, then chain-arcs" };
        case 3: return { pal::affCryo,  IconKind::Snow,      "CRYO",    "slow, freeze, then shatter" };
        case 4: return { pal::affAcid,  IconKind::Drop,      "CORRODE", "melts armor (+dmg taken)" };
    }
    return { rgb(138, 151, 176), IconKind::Diamond, "", "" };
}
struct AffVis { uint32_t col; IconKind icon; const char* name; };
AffVis affVis(Affinity a) {
    switch (a) {
        case Affinity::Pyro:    return { pal::affPyro,    IconKind::Flame,     "PYRO" };
        case Affinity::Volt:    return { pal::affVolt,    IconKind::Lightning, "VOLT" };
        case Affinity::Cryo:    return { pal::affCryo,    IconKind::Snow,      "CRYO" };
        case Affinity::Acid:    return { pal::affAcid,    IconKind::Drop,      "ACID" };
        case Affinity::Kinetic: return { pal::affKinetic, IconKind::Run,       "KINETIC" };
        case Affinity::Bulwark: return { pal::affBulwark, IconKind::Shield,    "BULWARK" };
        default: return { rgb(138, 151, 176), IconKind::Diamond, "" };
    }
}
// What an affinity SET unlocks at a threshold (n = 3 or 5) - the build-identity payoff, in words.
const char* affinitySetText(Affinity a, int n) {
    const bool five = n >= 5;
    switch (a) {
        case Affinity::Pyro:    return five ? "burns DETONATE on kill"       : "+50% burn applied";
        case Affinity::Volt:    return five ? "chains carry your elements"   : "+50% shock applied";
        case Affinity::Cryo:    return five ? "shatters emit a freeze nova"  : "+50% chill applied";
        case Affinity::Acid:    return five ? "corrode SPREADS on kill"      : "+50% corrode applied";
        case Affinity::Kinetic: return five ? "+15% damage"                  : "+10% move, faster dash";
        case Affinity::Bulwark: return five ? "+40 max health"               : "+8% damage reduction";
        default: return "";
    }
}

std::string affinityProgressLine(const BuildStats& stats, Affinity a) {
    if (a == Affinity::None) return "";
    const AffVis av = affVis(a);
    const int have = stats.affinityCount[static_cast<int>(a)];
    const int target = have < 3 ? 3 : 5;
    const int after = std::min(have + 1, target);
    return std::string("Build: ") + av.name + " " + std::to_string(have) + " -> " +
           std::to_string(after) + "/" + std::to_string(target) + ". " +
           (target == 3 ? "3-set: " : "5-set: ") + affinitySetText(a, target);
}

struct RouteVisual {
    const char* tag;
    const char* name;
    const char* stake;
    const char* reward;
    const char* buildHook;
    const char* risk;
    int riskLevel;
    uint32_t col;
    IconKind icon;
};
RouteVisual routeVisual(RoomType t) {
    switch (t) {
        case RoomType::Elite:
            return { "SPICY", "Elite Fight", "Harder enemies. Better tier odds.",
                     "High-tier reward roll + more scrap pressure.",
                     "Best when your build can survive a damage spike.", "HIGH", 3,
                     pal::roomElite, IconKind::Star };
        case RoomType::Cache:
            return { "SAFE", "Supply Cache", "No fight. Take the pickup now.",
                     "Guaranteed upgrade or resources.",
                     "Protects health and ammo, but skips combat payout.", "NONE", 0,
                     pal::roomCache, IconKind::Crate };
        case RoomType::Shop:
            return { "SPEND", "Field Shop", "Spend scrap before the next fight.",
                     "Stock, repair, reroll, and forge services.",
                     "Turns saved scrap into immediate power.", "SAFE", 0,
                     pal::gold, IconKind::Coin };
        case RoomType::Event:
            return { "PACT", "Pact Offer", "Take a boon with a named curse.",
                     "A quantified trade you can decline.",
                     "Power now, cost for the listed duration.", "PACT", 2,
                     pal::crimsonHi, IconKind::Diamond };
        case RoomType::Boss:
            return { "BOSS", "Sector Boss", "Boss gate. Win the sector.",
                     "Sector payout and route advance.",
                     "Commit when you can finish the fight.", "BOSS", 3,
                     pal::roomBoss, IconKind::Reticle };
        case RoomType::Combat:
        default:
            return { "STEADY", "Combat Fight", "Standard waves. Standard payout.",
                     "Fresh reward choice after the room.",
                     "Good for building tempo without extra modifiers.", "LOW", 1,
                     pal::roomCombat, IconKind::Reticle };
    }
}
uint32_t riskColorForLevel(int level) {
    if (level >= 3) return pal::danger;
    if (level == 2) return pal::warn;
    return pal::good;
}

// A small right-pointing chevron (focused menu-row marker). ~12px design.
void chevron(UiDrawList& ui, float x, float cy, float sz, uint32_t col, float t) {
    ui.line(x, cy - sz, x + sz, cy, t, col);
    ui.line(x + sz, cy, x, cy + sz, t, col);
}
// A small up-pointing caution triangle (destructive marker). Orange.
void caution(UiDrawList& ui, float cx, float cy, float r, uint32_t col) {
    ui.triangle(cx, cy - r, cx + r, cy + r, cx - r, cy + r, col);
    ui.rect(cx - 1.0f, cy - r * 0.2f, 2.0f, r * 0.7f, rgb(30, 22, 14));   // exclamation stem
    ui.rect(cx - 1.0f, cy + r * 0.6f, 2.0f, 2.0f, rgb(30, 22, 14));       // exclamation dot
}
} // namespace

// M4 route rail: the whole run as a row of biome-colored step nodes (squares = rooms, diamonds =
// bosses), past steps dimmed, the current step ringed - so the path screens read as a journey
// through the biomes, not just "pick the next room".
void PulseGame::drawRouteRail(UiDrawList& ui, float cx, float topY, float s) const {
    const int total = run_.roomCount();
    if (total <= 0) return;
    const int cur = run_.roomIndex();
    const float spacing = std::min(34.0f * s, (640.0f * s) / static_cast<float>(std::max(1, total - 1)));
    const float railW = static_cast<float>(total - 1) * spacing;
    const float x0 = cx - railW * 0.5f;
    const auto biomeCol = [](int sector) -> uint32_t {
        switch (sector % 3) {
            case 1:  return rgb(255, 170, 70);   // Furnace amber
            case 2:  return rgb(190, 130, 255);  // Reliquary violet
            default: return rgb(90, 210, 255);   // Foundry cyan
        }
    };
    ui.rect(x0, topY - 1.0f * s, railW, 2.0f * s, withAlpha(pal::border, 180));
    int lastSector = -1;
    for (int i = 0; i < total; ++i) {
        const int sector = run_.sectorOfStep(i);
        const uint32_t col = biomeCol(sector);
        const float x = x0 + static_cast<float>(i) * spacing;
        const bool isCur = (i == cur);
        const uint32_t nodeCol = (i < cur) ? withAlpha(col, 110) : col;
        if (run_.stepIsBoss(i)) {
            diamond(ui, x, topY, (isCur ? 8.0f : 6.0f) * s, nodeCol);
        } else {
            const float r = (isCur ? 5.0f : 3.5f) * s;
            ui.rect(x - r, topY - r, r * 2.0f, r * 2.0f, nodeCol);
        }
        if (isCur) diamondOutline(ui, x, topY, 11.0f * s, pal::textHero, std::max(1.5f, 1.5f * s));
        if (sector != lastSector) {
            ui.textTracked(x - 4.0f * s, topY + 16.0f * s, biomeName(static_cast<Biome>(sector % static_cast<int>(Biome::Count))),
                           withAlpha(col, 220), fpx(ui, 10.0f, s), 1.0f * s);
            lastSector = sector;
        }
    }
}

void PulseGame::drawCodex(UiDrawList& ui, int w, int h) const {
    const float s = uiScale(w, h);
    // One polished chrome (background, header, BACK TO HUB, filled-cyan-chip tab bar) for EVERY tab,
    // then a single switch over the per-tab body in matching notched panels. No special-cased branch.
    const float W = static_cast<float>(w), H = static_cast<float>(h);
    const float padX = 56.0f * s, padY = 48.0f * s, gap = 24.0f * s;
    const float frameW = W - padX * 2.0f;
    ui.rect(0.0f, 0.0f, W, H, rgb(5, 8, 14));
    ui.gradientV(0.0f, 0.0f, W, H, rgb(7, 13, 22), rgb(5, 8, 14), 60);
    for (int xi = 0; xi <= 34; ++xi)
        ui.rect(static_cast<float>(xi) * 56.0f * s, 0.0f, 1.0f, H, rgba(56, 200, 232, 8));

    ui.textTracked(padX, padY, "[ SYSTEMS ]", rgb(72, 96, 110), fpx(ui, 13.0f, s), 4.9f * s);
    ui.textD(padX, padY + 24.0f * s, "FIELD MANUAL", pal::textHi, fpx(ui, 48.0f, s));
    ui.textTracked(padX, padY + 84.0f * s, "PULSE COMBAT DOCTRINE / REV 2.6 / UNIT D-01 ///",
                   rgb(86, 110, 124), fpx(ui, 12.0f, s), 2.2f * s);
    {
        const float closeW = 224.0f * s, closeH = 52.0f * s;
        const float closeX = W - padX - closeW, closeY = padY;
        chamferFill(ui, closeX, closeY, closeW, closeH, 14.0f * s, rgba(13, 40, 52, 178));
        chamferOutline(ui, closeX, closeY, closeW, closeH, 14.0f * s, rgba(70, 140, 165, 128), std::max(1.0f, 1.0f * s));
        ui.textTracked(closeX + 38.0f * s, closeY + 18.0f * s, "< BACK TO HUB", rgb(111, 232, 255), fpx(ui, 15.0f, s), 3.2f * s);
        textTrackedRight(ui, W - padX, closeY + 70.0f * s, "CLASSIFIED // CLEARANCE ONLY",
                         rgb(63, 86, 99), fpx(ui, 11.0f, s), 2.7f * s);
    }

    const char* tabs[9] = { "PULSE", "AFFINITIES", "STATUSES", "SETS", "RARITY", "HEAT", "PACTS", "WEAPONS", "ROUTES" };
    const float tabY = 170.0f * s;
    codexTabBarY_ = tabY - 2.0f * s;
    codexTabBarH_ = 36.0f * s;
    {
        float tx = padX;
        for (int i = 0; i < 9; ++i) {
            const bool active = i == codexTab_;
            const float sc = fpx(ui, 15.0f, s), tr = 2.4f * s;
            const float textW = ui.textTrackedWidth(tabs[i], sc, tr);
            const float tw = textW + (active ? 36.0f * s : 0.0f);
            codexTabX_[static_cast<size_t>(i)] = tx;
            codexTabW_[static_cast<size_t>(i)] = tw;
            if (active) {
                ui.rect(tx - 3.0f * s, tabY - 5.0f * s, tw + 6.0f * s, 38.0f * s, rgba(43, 214, 245, 40));
                chamferFill(ui, tx, tabY - 2.0f * s, tw, 32.0f * s, 8.0f * s, pal::accent);
                ui.textTracked(tx + 18.0f * s, tabY + 7.0f * s, tabs[i], pal::deepBg, sc, tr);
            } else {
                ui.textTracked(tx, tabY + 7.0f * s, tabs[i], rgb(111, 138, 153), sc, tr);
            }
            tx += tw + 30.0f * s;
        }
    }
    ui.rect(padX, tabY + 44.0f * s, frameW, 1.0f, rgba(90, 170, 200, 42));

    // Shared body geometry + a notched panel helper matching the Affinities tab. cX/cY/cW/cH and
    // `panel` keep every case below identical in chrome to the polished reference layout.
    const float cX = padX, cY = 230.0f * s, cW = frameW, cH = H - padY - cY;
    (void)gap;
    auto panel = [&](float x, float y, float pw, float ph, const char* title, uint32_t titleCol) {
        notchBRPanel(ui, x, y, pw, ph, 16.0f * s, rgba(14, 24, 34, 154), rgba(90, 170, 200, 42), std::max(1.0f, 1.0f * s));
        ui.textTracked(x + 30.0f * s, y + 24.0f * s, title, titleCol, fpx(ui, 13.0f, s), 2.9f * s);
    };

    switch (codexTab_) {
    case 0: {   // PULSE
        panel(cX, cY, cW, cH, "PULSE / MOMENTUM - THE AMPLIFIER", pal::ionWhite);
        ui.text(cX + 28.0f * s, cY + 64.0f * s,
                "The Pulse is not flat damage - it AMPLIFIES your whole build. Aggression and kills build it;",
                pal::textMid, fpx(ui, 18.0f, s));
        ui.text(cX + 28.0f * s, cY + 92.0f * s,
                "taking hits and idling DRAIN it. Ride the high bands and everything you own hits harder.",
                pal::textMid, fpx(ui, 18.0f, s));
        struct PulseRow { const char* range; const char* name; const char* text; PulseTier tier; };
        const PulseRow rows[5] = {
            { "0 - 19",  "DORMANT",   "no amplifier - build it back up", PulseTier::Dormant },
            { "20 - 49", "CHARGED",   "minor build amplification", PulseTier::Charged },
            { "50 - 79", "SURGING",   "status + set amplifiers rise", PulseTier::Surging },
            { "80 - 99", "BURNING",   "high amplifiers + loot greed", PulseTier::Burning },
            { "100",     "OVERPULSE", "amplifiers maxed + extra loot tier", PulseTier::Overpulse },
        };
        float yy = cY + 138.0f * s;
        for (const PulseRow& r : rows) {
            const uint32_t col = pulseStateColor(r.tier);
            ui.rect(cX + 28.0f * s, yy + 4.0f * s, 14.0f * s, 14.0f * s, col);
            ui.text(cX + 56.0f * s, yy, r.range, col, fpx(ui, 20.0f, s));
            ui.textTracked(cX + 174.0f * s, yy + 1.0f * s, r.name, col, fpx(ui, 18.0f, s), 1.4f * s);
            ui.text(cX + 430.0f * s, yy, r.text, pal::textMid, fpx(ui, 19.0f, s));
            yy += 46.0f * s;
        }
        ui.text(cX + 28.0f * s, yy + 14.0f * s,
                "AMPLIFIES: status tick rate / set signatures / ability + aspect charge / loot tier.",
                rgb(207, 226, 255), fpx(ui, 16.0f, s));
        break;
    }
    case 1: {   // AFFINITIES
        const float panelH = 402.0f * s;
        notchBRPanel(ui, cX, cY, cW, panelH, 16.0f * s, rgba(14, 24, 34, 154),
                     rgba(90, 170, 200, 42), std::max(1.0f, 1.0f * s));
        ui.textTracked(cX + 30.0f * s, cY + 24.0f * s,
                       "AFFINITIES & ELEMENTS - ONE AFFINITY, ONE STATUS, TWO THRESHOLDS",
                       pal::amber, fpx(ui, 13.0f, s), 2.9f * s);
        const float c0 = cX + 30.0f * s;
        const float c1 = c0 + 224.0f * s;
        const float c2 = c1 + 344.0f * s;
        const float c3 = c2 + (cW - 60.0f * s - 200.0f * s - 320.0f * s - gap * 3.0f) * 0.5f + gap;
        const float headY = cY + 66.0f * s;
        ui.textTracked(c0, headY, "AFFINITY", rgb(72, 96, 110), fpx(ui, 11.0f, s), 2.2f * s);
        ui.textTracked(c1, headY, "APPLIES (STATUS)", rgb(72, 96, 110), fpx(ui, 11.0f, s), 2.2f * s);
        ui.textTracked(c2, headY, "3-SET AMPLIFIER", rgb(72, 96, 110), fpx(ui, 11.0f, s), 2.2f * s);
        ui.textTracked(c3, headY, "5-SET SIGNATURE", rgb(72, 96, 110), fpx(ui, 11.0f, s), 2.2f * s);
        ui.rect(cX + 30.0f * s, headY + 28.0f * s, cW - 60.0f * s, 1.0f, rgba(90, 170, 200, 26));
        struct AffRow { Affinity a; const char* status; uint32_t bright; uint32_t soft; };
        const AffRow rows[6] = {
            { Affinity::Pyro,    "BURN . damage over time",      rgb(255, 125, 84),  rgb(255, 154, 115) },
            { Affinity::Volt,    "SHOCK . chain arcs",           rgb(111, 176, 255), rgb(155, 196, 255) },
            { Affinity::Cryo,    "CHILL . freeze/shatter",       rgb(142, 238, 255), rgb(166, 238, 255) },
            { Affinity::Acid,    "CORRODE . armor melt",         rgb(151, 255, 163), rgb(168, 255, 177) },
            { Affinity::Kinetic, "NONE . stat affinity",         rgb(201, 255, 126), rgb(111, 138, 153) },
            { Affinity::Bulwark, "NONE . stat affinity",         rgb(194, 210, 224), rgb(111, 138, 153) },
        };
        // Even cells: the 6 rows fill the band from the column-header rule to the panel
        // bottom, so every row is the same height. (A fixed pitch left the first row
        // carrying the header gap and the last row cramped against the panel edge.)
        const float bandTop = headY + 28.0f * s;          // the column-header underline
        const float bandBot = cY + panelH - 4.0f * s;     // panel inner bottom
        const float cellH   = (bandBot - bandTop) / 6.0f;
        for (int i = 0; i < 6; ++i) {
            const AffRow& r = rows[i];
            const AffVis av = affVis(r.a);
            const float cellTop = bandTop + static_cast<float>(i) * cellH;
            const float mid = cellTop + cellH * 0.5f;     // vertical centre of the cell
            if (i > 0) ui.rect(cX + 30.0f * s, cellTop, cW - 60.0f * s, 1.0f, rgba(90, 170, 200, 18));
            diamond(ui, c0 + 5.0f * s, mid, 5.0f * s, av.col);
            ui.textD(c0 + 24.0f * s, mid - 9.0f * s, av.name, r.bright, fpx(ui, 18.0f, s));
            ui.text(c1, mid - 7.5f * s, r.status, r.soft, fpx(ui, 15.0f, s));
            ui.text(c2, mid - 7.5f * s, affinitySetText(r.a, 3), rgb(169, 188, 199), fpx(ui, 15.0f, s));
            ui.text(c3, mid - 7.5f * s, affinitySetText(r.a, 5), r.soft, fpx(ui, 15.0f, s));
        }
        const float bottomY = cY + panelH + gap;
        const float bottomH = H - padY - bottomY;
        const float halfW = (cW - gap) * 0.5f;
        auto bottomPanel = [&](float x, const char* title) {
            notchBRPanel(ui, x, bottomY, halfW, bottomH, 14.0f * s, rgba(14, 24, 34, 154),
                         rgba(90, 170, 200, 42), std::max(1.0f, 1.0f * s));
            ui.textD(x + 28.0f * s, bottomY + 24.0f * s, title, pal::textHi, fpx(ui, 18.0f, s));
        };
        bottomPanel(cX, "PULSE / MOMENTUM");
        ui.text(cX + 28.0f * s, bottomY + 62.0f * s,
                "Aggression builds it. Taking hits + idling DRAIN it.", rgb(111, 138, 153), fpx(ui, 13.0f, s));
        struct PulseLine { const char* range; const char* desc; uint32_t col; };
        const PulseLine pl[5] = {
            { "0 - 19",  "no amplifier",                    rgb(94, 126, 142) },
            { "20 - 49", "minor build amplification",        rgb(111, 176, 255) },
            { "50 - 79", "status + set amplifiers rise",     pal::affCryo },
            { "80 - 99", "high amplifiers + loot greed",     pal::amber },
            { "100",     "Overpulse: amplifiers maxed",      rgb(157, 123, 255) },
        };
        for (int i = 0; i < 5; ++i) {
            const float y = bottomY + (102.0f + static_cast<float>(i) * 29.0f) * s;
            ui.text(cX + 28.0f * s, y, pl[i].range, pl[i].col, fpx(ui, 15.0f, s));
            ui.text(cX + 122.0f * s, y, pl[i].desc, rgb(169, 188, 199), fpx(ui, 15.0f, s));
        }
        ui.textTracked(cX + 28.0f * s, bottomY + bottomH - 30.0f * s,
                       "AMPLIFIES YOUR BUILD: STATUS TICKS / SET SIGNATURES / ABILITY + ASPECT CHARGE / LOOT TIER.",
                       rgb(63, 86, 99), fpx(ui, 11.0f, s), 1.2f * s);
        const float rx = cX + halfW + gap;
        bottomPanel(rx, "RARITY");
        struct RRow { const char* name; const char* text; uint32_t col; uint32_t nameCol; };
        const RRow rr[4] = {
            { "COMMON",    "core stat boosts",            pal::tierCommon,    rgb(194, 210, 224) },
            { "UNCOMMON",  "stronger stats + status",     pal::tierUncommon,  rgb(111, 176, 255) },
            { "RARE",      "build pivots + effects",      pal::tierRare,      pal::warnHi },
            { "LEGENDARY", "build-defining capstones",    pal::tierLegendary, rgb(255, 232, 138) },
        };
        for (int i = 0; i < 4; ++i) {
            const float y = bottomY + (66.0f + static_cast<float>(i) * 44.0f) * s;
            ui.rect(rx + 28.0f * s, y + 5.0f * s, 14.0f * s, 14.0f * s, rr[i].col);
            ui.textD(rx + 58.0f * s, y, rr[i].name, rr[i].nameCol, fpx(ui, 16.0f, s));
            ui.text(rx + 198.0f * s, y + 2.0f * s, rr[i].text, rgb(138, 160, 173), fpx(ui, 13.0f, s));
        }
        ui.text(rx + 28.0f * s, bottomY + bottomH - 36.0f * s,
                "Tip: first run teaches one system at a time - this manual is the deep reference.",
                rgb(63, 86, 99), fpx(ui, 12.0f, s));
        break;
    }
    case 2: {   // STATUSES
        const float half = (cW - 24.0f * s) * 0.5f;
        panel(cX, cY, half, cH, "STATUS ELEMENTS", pal::gold);
        struct St { const char* name; uint32_t col; const char* l1; const char* l2; };
        const St st[4] = {
            { "BURN",    DmgTextBurn,    "damage over time, ticks ~4x/sec",  "stacks deepen it; pure attrition" },
            { "SHOCK",   DmgTextShock,   "charge builds; at the threshold",  "it DISCHARGES: burst + chain arc" },
            { "CRYO",    DmgTextCryo,    "chill slows; at full it FREEZES,", "frozen foes SHATTER (1.8x hits)" },
            { "CORRODE", DmgTextCorrode, "armor melt: +damage taken per",    "stack; also boosts status apply" },
        };
        float yy = cY + 66.0f * s;
        for (const St& e : st) {
            ui.rect(cX + 28.0f * s, yy + 5.0f * s, 14.0f * s, 14.0f * s, e.col);
            ui.textTracked(cX + 54.0f * s, yy, e.name, e.col, fpx(ui, 22.0f, s), 1.4f * s);
            ui.text(cX + 54.0f * s, yy + 32.0f * s, e.l1, pal::textMid, fpx(ui, 16.0f, s));
            ui.text(cX + 54.0f * s, yy + 54.0f * s, e.l2, pal::textDim, fpx(ui, 16.0f, s));
            yy += 96.0f * s;
        }
        const float rx = cX + half + 24.0f * s;
        panel(rx, cY, half, cH, "PAIR REACTIONS - TWO ELEMENTS, ONE FOE", pal::accent);
        struct Combo { const char* name; const char* pair; };
        const Combo cb[5] = {
            { "PLASMA SURGE",  "BURN + SHOCK   -  splash burst + shock charge" },
            { "THERMAL SHOCK", "BURN + CRYO    -  steam burst, strips stacks" },
            { "SUPERCONDUCT",  "SHOCK + CRYO   -  cold arc, primes neighbours" },
            { "GALVANIC MELT", "SHOCK + CORRODE-  forces an instant discharge" },
            { "CAUSTIC FIRE",  "BURN + CORRODE -  flame sticks, acid spreads" },
        };
        float ry = cY + 66.0f * s;
        for (const Combo& c : cb) {
            ui.textTracked(rx + 28.0f * s, ry, c.name, pal::textHero, fpx(ui, 18.0f, s), 1.2f * s);
            ui.text(rx + 28.0f * s, ry + 26.0f * s, c.pair, pal::textDim, fpx(ui, 15.0f, s));
            ry += 72.0f * s;
        }
        break;
    }
    case 3: {   // SETS
        panel(cX, cY, cW, cH, "AFFINITY SETS - 3 LIT (AMPLIFIER) / 5 LIT (SIGNATURE)", pal::gold);
        const float colA = cX + 28.0f * s, col3 = cX + 280.0f * s, col5 = cX + cW * 0.60f;
        ui.textTracked(colA, cY + 56.0f * s, "AFFINITY", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        ui.textTracked(col3, cY + 56.0f * s, "3-SET AMPLIFIER", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        ui.textTracked(col5, cY + 56.0f * s, "5-SET SIGNATURE", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        float yy = cY + 88.0f * s;
        for (int i = 1; i < static_cast<int>(Affinity::Count); ++i) {
            const Affinity a = static_cast<Affinity>(i);
            const AffVis av = affVis(a);
            drawIcon(ui, av.icon, colA + 10.0f * s, yy + 13.0f * s, 8.0f * s, av.col, s);
            ui.textTracked(colA + 30.0f * s, yy, av.name, av.col, fpx(ui, 20.0f, s), 1.0f * s);
            ui.text(col3, yy + 1.0f * s, affinitySetText(a, 3), pal::textMid, fpx(ui, 17.0f, s));
            ui.text(col5, yy + 1.0f * s, affinitySetText(a, 5), withAlpha(av.col, 235), fpx(ui, 17.0f, s));
            yy += 50.0f * s;
        }
        ui.text(colA, yy + 14.0f * s,
                "Collect items of one affinity to LIGHT its set. Kinetic and Bulwark are stat affinities.",
                pal::textFaint, fpx(ui, 15.0f, s));
        break;
    }
    case 4: {   // RARITY
        panel(cX, cY, cW, cH, "RARITY - DROP TIERS", pal::accent);
        struct RarityRow { const char* name; uint32_t col; const char* text; };
        const RarityRow rr[4] = {
            { "COMMON",    pal::tierCommon,    "core stat boosts - the backbone of a build" },
            { "UNCOMMON",  pal::tierUncommon,  "stronger stats + the status infusions" },
            { "RARE",      pal::tierRare,      "build pivots + on-hit / on-kill effects" },
            { "LEGENDARY", pal::tierLegendary, "build-defining capstones - one can carry a run" },
        };
        float yy = cY + 72.0f * s;
        for (const RarityRow& r : rr) {
            ui.rect(cX + 28.0f * s, yy + 4.0f * s, 18.0f * s, 18.0f * s, r.col);
            ui.textTracked(cX + 60.0f * s, yy, r.name, r.col, fpx(ui, 22.0f, s), 1.2f * s);
            ui.text(cX + 320.0f * s, yy + 2.0f * s, r.text, pal::textDim, fpx(ui, 19.0f, s));
            yy += 58.0f * s;
        }
        ui.text(cX + 28.0f * s, yy + 18.0f * s,
                "A high Pulse and Elite/Boss kills bias the pool toward higher tiers (loot greed).",
                rgb(207, 226, 255), fpx(ui, 16.0f, s));
        break;
    }
    case 5: {   // HEAT
        panel(cX, cY, cW, cH, "HEAT / ASCENSION - VOLUNTARY DIFFICULTY", pal::crimson);
        ui.text(cX + 28.0f * s, cY + 64.0f * s,
                "Raise HEAT before a run to toughen the gauntlet for richer rewards. Each level stacks a",
                pal::textMid, fpx(ui, 18.0f, s));
        ui.text(cX + 28.0f * s, cY + 92.0f * s,
                "modifier; clearing a run unlocks the next. Set it on the Hub launch board.",
                pal::textMid, fpx(ui, 18.0f, s));
        struct HeatRow { const char* lvl; const char* text; };
        const HeatRow hr[5] = {
            { "H1",  "tougher enemies - more health and pressure" },
            { "H2",  "faster, more aggressive enemy behaviour" },
            { "H3",  "elite density up; fewer free openings" },
            { "H4",  "the boss gains pressure; weak-points shrink" },
            { "H5+", "compounding modifiers; top-end loot + score" },
        };
        float yy = cY + 138.0f * s;
        for (const HeatRow& r : hr) {
            ui.textTracked(cX + 28.0f * s, yy, r.lvl, pal::crimsonHi, fpx(ui, 20.0f, s), 1.2f * s);
            ui.text(cX + 120.0f * s, yy, r.text, pal::textDim, fpx(ui, 18.0f, s));
            yy += 40.0f * s;
        }
        char cur[64];
        std::snprintf(cur, sizeof(cur), "CURRENT SELECTION: HEAT %d", meta_.heat());
        ui.textTracked(cX + 28.0f * s, yy + 16.0f * s, cur, pal::gold, fpx(ui, 16.0f, s), 1.4f * s);
        break;
    }
    case 6: {   // PACTS
        panel(cX, cY, cW, cH, "PACTS - RISK FOR REWARD", pal::crimson);
        ui.text(cX + 28.0f * s, cY + 64.0f * s,
                "Pacts are deals struck at Event rooms: accept a lasting drawback in exchange for power,",
                pal::textMid, fpx(ui, 18.0f, s));
        ui.text(cX + 28.0f * s, cY + 92.0f * s,
                "scrap, or rare loot. They persist for the run - weigh each against your build.",
                pal::textMid, fpx(ui, 18.0f, s));
        struct PactRow { const char* give; const char* get; };
        const PactRow pr[4] = {
            { "less max health",      "+ damage, or a free legendary pick" },
            { "tougher enemies",      "+ scrap and richer drops" },
            { "a weapon restriction", "+ a build-defining effect" },
            { "spend scrap / health", "+ heal, reroll, or upgrade now" },
        };
        float yy = cY + 138.0f * s;
        for (const PactRow& r : pr) {
            ui.textTracked(cX + 28.0f * s, yy, "GIVE", pal::crimsonHi, fpx(ui, 15.0f, s), 1.4f * s);
            ui.text(cX + 110.0f * s, yy - 1.0f * s, r.give, pal::textDim, fpx(ui, 18.0f, s));
            ui.textTracked(cX + cW * 0.5f, yy, "GET", pal::accent, fpx(ui, 15.0f, s), 1.4f * s);
            ui.text(cX + cW * 0.5f + 70.0f * s, yy - 1.0f * s, r.get, pal::textMid, fpx(ui, 18.0f, s));
            yy += 48.0f * s;
        }
        ui.text(cX + 28.0f * s, yy + 16.0f * s,
                "You can always decline. The Shop offers safer, scrap-priced services instead.",
                pal::textFaint, fpx(ui, 16.0f, s));
        break;
    }
    case 7: {   // WEAPONS
        panel(cX, cY, cW, cH, "WEAPONS - ARSENAL", pal::accent);
        const float wc0 = cX + 28.0f * s, wc1 = cX + cW * 0.42f, wc2 = cX + cW * 0.62f,
                    wc3 = cX + cW * 0.76f, wc4 = cX + cW * 0.90f;
        ui.textTracked(wc0, cY + 56.0f * s, "WEAPON", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        ui.textTracked(wc1, cY + 56.0f * s, "TYPE", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        ui.textTracked(wc2, cY + 56.0f * s, "DMG", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        ui.textTracked(wc3, cY + 56.0f * s, "ROF", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        ui.textTracked(wc4, cY + 56.0f * s, "MAG", pal::textFaint, fpx(ui, 14.0f, s), 1.6f * s);
        float yy = cY + 88.0f * s;
        for (const WeaponProfile& wp : weaponProfiles_.profiles()) {
            if (yy > cY + cH - 30.0f * s) break;
            ui.text(wc0, yy, wp.role.empty() ? wp.id.c_str() : wp.role.c_str(), pal::textHero, fpx(ui, 17.0f, s));
            ui.text(wc1, yy, weaponArchetypeName(wp.archetype), pal::textMid, fpx(ui, 16.0f, s));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(wp.damage)));
            ui.text(wc2, yy, buf, pal::textMid, fpx(ui, 16.0f, s));
            std::snprintf(buf, sizeof(buf), "%.1f/s", static_cast<double>(wp.fireRate));
            ui.text(wc3, yy, buf, pal::textMid, fpx(ui, 16.0f, s));
            std::snprintf(buf, sizeof(buf), "%d", wp.magazine);
            ui.text(wc4, yy, buf, pal::textMid, fpx(ui, 16.0f, s));
            yy += 42.0f * s;
        }
        break;
    }
    case 8: {   // ROUTES
        panel(cX, cY, cW, cH, "ROUTES / BIOMES - THE RUN", pal::ionWhite);
        ui.text(cX + 28.0f * s, cY + 64.0f * s,
                "A run threads rooms joined by doors. Clear a room, then choose an exit: each previews its",
                pal::textMid, fpx(ui, 18.0f, s));
        ui.text(cX + 28.0f * s, cY + 92.0f * s,
                "reward and route. Shops sell services for scrap; Events offer Pacts; sectors end in a Boss.",
                pal::textMid, fpx(ui, 18.0f, s));
        ui.textTracked(cX + 28.0f * s, cY + 140.0f * s, "ROOM TYPES", pal::gold, fpx(ui, 15.0f, s), 1.6f * s);
        const char* rooms[5] = {
            "COMBAT  -  clear the waves to open the exits",
            "ELITE   -  a tougher fight for a better reward",
            "SHOP    -  spend scrap: heal, reroll, upgrade",
            "EVENT   -  a Pact: a drawback traded for power",
            "BOSS    -  the sector gate; beat it to descend",
        };
        float yy = cY + 170.0f * s;
        for (const char* r : rooms) { ui.text(cX + 28.0f * s, yy, r, pal::textDim, fpx(ui, 17.0f, s)); yy += 32.0f * s; }
        ui.textTracked(cX + 28.0f * s, yy + 12.0f * s, "BIOMES", pal::gold, fpx(ui, 15.0f, s), 1.6f * s);
        float bx = cX + 28.0f * s; const float by = yy + 44.0f * s;
        for (int i = 0; i < static_cast<int>(Biome::Count); ++i) {
            const char* bn = biomeName(static_cast<Biome>(i));
            ui.textTracked(bx, by, bn, pal::accent, fpx(ui, 18.0f, s), 1.2f * s);
            bx += ui.textTrackedWidth(bn, fpx(ui, 18.0f, s), 1.2f * s) + 40.0f * s;
        }
        break;
    }
    default: break;
    }
}

void PulseGame::menuFocusHalo(UiDrawList& ui, float x, float y, float w, float h, float cut, uint32_t col, float s) const {
    // The focused/hovered element gets a soft outer glow that grows in as the focus lands
    // (menuFocusAnim_ eases 0->1) and gently breathes - the "subtle hover animation".
    const float breathe = 0.5f + 0.5f * std::sin(shakeTime_ * 3.4f);
    const float t = menuFocusAnim_;
    const uint8_t a = static_cast<uint8_t>(clamp(16.0f + 30.0f * t + 14.0f * breathe, 0.0f, 74.0f));
    const float grow = (1.5f + 4.0f * t) * s;
    chamferOutline(ui, x - grow, y - grow, w + grow * 2.0f, h + grow * 2.0f, cut + grow,
                   withAlpha(col, a), std::max(1.0f, 1.5f * s));
}

void PulseGame::buildHud(UiDrawList& ui, int w, int h) {
    menuHits_.clear();   // rebuilt each frame: the run-phase menus push their clickable rects below
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float ts = 0.55f;   // map the prototype's integer text scale to the GDI font
    // Neon Ink Brutalism is authored @1920x1080; scale design-space px uniformly to
    // the back-buffer so every screen reads the same on any resolution.
    const float s = uiScale(w, h);
    const float margin = pal::margin * s;          // 64px design safe margin, scaled
    const float cut = 12.0f * s;                   // panel/card chamfer (TL + BR)
    auto darkBoardBg = [&](uint32_t top = rgb(8, 17, 27), uint32_t bottom = pal::deepBg) {
        const float W = static_cast<float>(w), H = static_cast<float>(h);
        ui.rect(0.0f, 0.0f, W, H, pal::deepBg);
        ui.gradientV(0.0f, 0.0f, W, H, top, bottom, 72);
        for (int i = 0; i < 22; ++i) {
            const float t = static_cast<float>(i) / 21.0f;
            const uint8_t a = static_cast<uint8_t>(5.0f + t * t * 20.0f);
            ui.rect(0.0f, i * 9.0f * s, W, 9.0f * s + 1.0f, rgba(0, 0, 0, a));
            ui.rect(0.0f, H - (i + 1) * 10.0f * s, W, 10.0f * s + 1.0f, rgba(0, 0, 0, a));
            ui.rect(i * 10.0f * s, 0.0f, 10.0f * s + 1.0f, H, rgba(0, 0, 0, a));
            ui.rect(W - (i + 1) * 10.0f * s, 0.0f, 10.0f * s + 1.0f, H, rgba(0, 0, 0, a));
        }
        const float grid = 56.0f * s;
        for (float x = 0.0f; x <= W + grid; x += grid)
            ui.rect(x, 0.0f, std::max(1.0f, 1.0f * s), H, rgba(56, 200, 232, 10));
        for (float y = 0.0f; y <= H + grid; y += grid)
            ui.rect(0.0f, y, W, std::max(1.0f, 1.0f * s), rgba(56, 200, 232, 8));
        ui.rect(76.0f * s, 82.0f * s, W - 152.0f * s, std::max(1.0f, 1.0f * s), rgba(90, 170, 200, 34));
        ui.rect(76.0f * s, H - 82.0f * s, W - 152.0f * s, std::max(1.0f, 1.0f * s), rgba(255, 178, 77, 24));
        pulseWaveLong(ui, W - 720.0f * s, 104.0f * s, 520.0f * s, 42.0f * s,
                      withAlpha(pal::accent, 102), withAlpha(pal::accentGlow, 20), std::max(2.0f, 2.0f * s));
        screenBrackets(ui, W, H, s);
    };
    // The main menu (and options reached from it) replace the HUD entirely: there is no
    // live run to read behind them, so draw only the overlay and skip the rest.
    if (frontEnd_ && (menuScreen_ == MenuScreen::Main ||
                      (menuScreen_ == MenuScreen::Settings && settingsReturn_ == MenuScreen::Main))) {
        buildMenuOverlay(ui, w, h);
        return;
    }
    const WeaponProfile& hudWeaponProfile = activeWeaponProfile();
    const float flashDuration = std::max(0.01f, hudWeaponProfile.muzzleFlashSeconds);
    const float flashScale = std::max(0.1f, hudWeaponProfile.muzzleFlashScale);
    const auto fxByte = [](float v) -> uint8_t {
        return static_cast<uint8_t>(clamp(v, 0.0f, 255.0f));
    };

    // Crosshair. The gap blooms with movement, accumulated recoil, and each shot
    // so the reticle reads the current accuracy state (CS/COD/Valorant staple), then
    // tightens to base when you stand still and hold fire.
    const uint32_t cross = rgb(235, 242, 246);
    const float chWalk = std::max(0.1f, tunables_.walkSpeed);
    const float chMove = clamp(length(player_.vel) / chWalk, 0.0f, 1.2f);
    const float chRecoil = clamp((std::fabs(recoilOffsetPitch_) + std::fabs(recoilOffsetYaw_)) / degToRad(8.0f), 0.0f, 1.0f);
    const float chFire = clamp(muzzleFlashTimer_ / flashDuration, 0.0f, 1.0f);
    const float chBloom = chMove * 0.6f + chRecoil * 0.85f + chFire * 0.5f;
    const float chGap = tunables_.crosshairBaseGap + tunables_.crosshairBloomScale * chBloom;
    const float chArm = 8.0f;
    ui.rect(cx - 1, cy - chGap - chArm, 2, chArm, cross);
    ui.rect(cx - 1, cy + chGap, 2, chArm, cross);
    ui.rect(cx - chGap - chArm, cy - 1, chArm, 2, cross);
    ui.rect(cx + chGap, cy - 1, chArm, 2, cross);
    ui.rect(cx - 1.5f, cy - 1.5f, 3.0f, 3.0f, pal::accent);   // cyan centre dot: the strongest central element

    // Muzzle flash.
    if (muzzleFlashTimer_ > 0.0f) {
        const float t = clamp(muzzleFlashTimer_ / flashDuration, 0.0f, 1.0f);
        const float hot = t * t;
        const float mx = muzzleFracX_ * w;
        const float my = muzzleFracY_ * h;
        float dx = cx - mx, dy = cy - my;
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (dl > 0.001f) { dx /= dl; dy /= dl; } else { dx = -0.35f; dy = -0.94f; }
        const float px = -dy, py = dx;
        const float flashAlpha = clamp(0.52f + flashScale * 0.36f, 0.40f, 1.0f);
        const float blast = std::min(w, h) * (0.021f + 0.010f * hot) * flashScale;
        const float width = std::min(w, h) * (0.0065f + 0.0045f * hot) * (0.70f + flashScale * 0.22f);
        const uint8_t coreA = static_cast<uint8_t>(clamp(118.0f * hot * flashAlpha, 0.0f, 190.0f));
        const uint8_t flameA = static_cast<uint8_t>(clamp(138.0f * t * flashAlpha, 0.0f, 205.0f));
        const uint8_t fr = fxByte(hudWeaponProfile.muzzleFlashR);
        const uint8_t fg = fxByte(hudWeaponProfile.muzzleFlashG);
        const uint8_t fb = fxByte(hudWeaponProfile.muzzleFlashB);
        ui.triangle(mx + px * width, my + py * width, mx - px * width, my - py * width,
                    mx + dx * blast, my + dy * blast, rgba(fr, fg, fb, coreA));
        ui.triangle(mx + px * width * 0.45f, my + py * width * 0.45f,
                    mx - px * width * 0.45f, my - py * width * 0.45f,
                    mx - dx * blast * 0.18f, my - dy * blast * 0.18f,
                    rgba(fxByte(hudWeaponProfile.muzzleFlashR * 0.82f),
                         fxByte(hudWeaponProfile.muzzleFlashG * 0.70f),
                         fxByte(hudWeaponProfile.muzzleFlashB * 0.58f), flameA));
        const float coreSize = 2.3f + 1.1f * flashScale;
        ui.rect(mx - coreSize * 0.5f, my - coreSize * 0.5f, coreSize, coreSize,
                rgba(fr, fg, fb, static_cast<uint8_t>(clamp(72.0f * hot * flashAlpha, 0.0f, 145.0f))));
    }

    for (const ScreenCasing& casing : casings_) {
        const float t = clamp(casing.age / std::max(0.001f, casing.life), 0.0f, 1.0f);
        const float fade = (1.0f - t) * (1.0f - t);
        const uint8_t shadowA = static_cast<uint8_t>(clamp(125.0f * fade, 0.0f, 125.0f));
        const uint8_t brassA = static_cast<uint8_t>(clamp(205.0f * fade, 0.0f, 205.0f));
        const float ca = std::cos(casing.angle);
        const float sa = std::sin(casing.angle);
        const float halfLen = (4.8f + casing.size * 2.7f) * (1.0f - t * 0.12f);
        const float x0 = casing.x - ca * halfLen;
        const float y0 = casing.y - sa * halfLen;
        const float x1 = casing.x + ca * halfLen;
        const float y1 = casing.y + sa * halfLen;
        ui.line(x0, y0, x1, y1, 3.2f * casing.size, rgba(33, 23, 12, shadowA));
        ui.line(x0, y0, x1, y1, 1.7f * casing.size, rgba(228, 172, 76, brassA));
    }

    // Hitscan tracer streak.
    const auto projectWorldPoint = [&](Vec3f world, float& sx, float& sy) -> bool {
        const float eyeY = clamp(player_.height, 0.05f, 6.0f);  // eye rises when standing on cover
        const Vec3f eye{ player_.pos.x, eyeY, player_.pos.y };
        const float cp = std::cos(renderViewPitch_);
        const float sp = std::sin(renderViewPitch_);
        const float cyaw = std::cos(renderViewYaw_);
        const float syaw = std::sin(renderViewYaw_);
        const Vec3f forward = normalize3({ cp * cyaw, sp, cp * syaw });
        Vec3f right = normalize3(cross3(forward, { 0.0f, 1.0f, 0.0f }));
        if (dot3(right, right) < 0.0001f) right = { 1.0f, 0.0f, 0.0f };
        const Vec3f up = cross3(right, forward);
        const Vec3f rel = world - eye;
        const float depth = dot3(rel, forward);
        if (depth <= 0.025f) return false;
        const float aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        const float fov = degToRad(std::max(45.0f, tunables_.cameraFovDegrees - fireFovKick_));
        const float yScale = 1.0f / std::tan(fov * 0.5f);
        const float xScale = yScale / aspect;
        sx = (dot3(rel, right) * xScale / depth * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (0.5f - dot3(rel, up) * yScale / depth * 0.5f) * static_cast<float>(h);
        return sx > -static_cast<float>(w) * 0.4f && sx < static_cast<float>(w) * 1.4f &&
               sy > -static_cast<float>(h) * 0.4f && sy < static_cast<float>(h) * 1.4f;
    };
    for (const EnemyBeamLine& beam : enemyBeamLines_) {
        const float power = clamp(beam.intensity, 0.0f, 1.25f);
        if (power <= 0.01f) continue;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!projectWorldPoint(beam.from, x0, y0) || !projectWorldPoint(beam.to, x1, y1)) continue;
        float dx = x1 - x0, dy = y1 - y0;
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (dl <= 1.0f) continue;
        dx /= dl; dy /= dl;
        const float shimmer = 0.92f + 0.08f * std::sin(shakeTime_ * 48.0f + beam.from.x * 2.7f + beam.from.z * 1.9f);
        const float width = clamp(static_cast<float>(std::min(w, h)) * (0.0018f + beam.worldWidth * 0.055f), 1.15f, 4.6f);
        const float startPad = std::min(6.0f, dl * 0.05f);
        const float endPad = std::min(4.0f, dl * 0.035f);
        x0 += dx * startPad; y0 += dy * startPad;
        x1 -= dx * endPad;   y1 -= dy * endPad;
        const float px = -dy, py = dx;
        const auto beamByte = [&](float c, float scale) -> uint8_t {
            return fxByte(c * scale);
        };
        const uint8_t auraA = static_cast<uint8_t>(clamp(power * 34.0f, 0.0f, 58.0f));
        const uint8_t haloA = static_cast<uint8_t>(clamp(power * 68.0f, 0.0f, 96.0f));
        const uint8_t bodyA = static_cast<uint8_t>(clamp(power * 138.0f * shimmer, 0.0f, 172.0f));
        const uint8_t coreA = static_cast<uint8_t>(clamp(power * 214.0f * shimmer, 0.0f, 232.0f));
        const uint8_t filamentA = static_cast<uint8_t>(clamp(power * 96.0f * shimmer, 0.0f, 128.0f));
        const float filamentOff = width * (1.10f + 0.22f * shimmer);
        ui.line(x0, y0, x1, y1, width * 7.4f,
                rgba(beamByte(beam.color.x, 42.0f), beamByte(beam.color.y, 34.0f),
                     beamByte(beam.color.z, 48.0f), auraA));
        ui.line(x0, y0, x1, y1, width * 4.2f,
                rgba(beamByte(beam.color.x, 70.0f), beamByte(beam.color.y, 48.0f),
                     beamByte(beam.color.z, 78.0f), haloA));
        ui.line(x0, y0, x1, y1, width * 1.65f,
                rgba(beamByte(beam.color.x, 112.0f), beamByte(beam.color.y, 82.0f),
                     beamByte(beam.color.z, 118.0f), bodyA));
        ui.line(x0 + px * filamentOff, y0 + py * filamentOff, x1 + px * filamentOff, y1 + py * filamentOff,
                std::max(1.0f, width * 0.42f),
                rgba(beamByte(beam.color.x, 128.0f), beamByte(beam.color.y, 88.0f),
                     beamByte(beam.color.z, 132.0f), filamentA));
        ui.line(x0 - px * filamentOff, y0 - py * filamentOff, x1 - px * filamentOff, y1 - py * filamentOff,
                std::max(1.0f, width * 0.34f),
                rgba(beamByte(beam.color.x, 92.0f), beamByte(beam.color.y, 64.0f),
                     beamByte(beam.color.z, 104.0f), static_cast<uint8_t>(filamentA * 0.72f)));
        ui.line(x0, y0, x1, y1, std::max(1.0f, width * 0.55f),
                rgba(255, 226, 255, coreA));
    }
    for (const Tracer& tr : tracers_) {
        const float life = 1.0f - clamp(tr.age / std::max(0.001f, tr.duration), 0.0f, 1.0f);
        if (life <= 0.0f) continue;
        float ex = cx, ey = cy;
        if (!projectWorldPoint({ tr.end.x, tr.endHeight, tr.end.y }, ex, ey)) continue;
        const float mx = muzzleFracX_ * w;
        const float my = muzzleFracY_ * h;
        float dx = ex - mx, dy = ey - my;
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (dl <= 1.0f) continue;
        dx /= dl; dy /= dl;
        const float widthScale = clamp(hudWeaponProfile.tracerWidthScale, 0.20f, 1.65f);
        const float alphaScale = clamp(hudWeaponProfile.tracerAlphaScale, 0.20f, 1.40f);
        const float tracerWidth = (0.92f + 0.54f * clamp(hudWeaponProfile.impactScale, 0.45f, 1.35f)) *
            (0.52f + life * 0.34f) * widthScale;
        const uint8_t glowA = static_cast<uint8_t>(clamp(life * alphaScale * (tr.hit ? 76.0f : 58.0f), 0.0f, 120.0f));
        const uint8_t coreA = static_cast<uint8_t>(clamp(life * alphaScale * (tr.hit ? 170.0f : 125.0f), 0.0f, 210.0f));
        const float startPad = std::min(18.0f, dl * 0.16f);
        const float endPad = std::min(10.0f, dl * 0.08f);
        const float x0 = mx + dx * startPad;
        const float y0 = my + dy * startPad;
        const float x1 = ex - dx * endPad;
        const float y1 = ey - dy * endPad;
        const bool fxShot = tr.effectMask != 0;
        const uint8_t gr = fxShot ? fxByte(tr.color.x * 96.0f)  : fxByte(hudWeaponProfile.tracerR * 0.76f);
        const uint8_t gg = fxShot ? fxByte(tr.color.y * 96.0f)  : fxByte(hudWeaponProfile.tracerG * 0.72f);
        const uint8_t gb = fxShot ? fxByte(tr.color.z * 96.0f)  : fxByte(hudWeaponProfile.tracerB * 0.68f);
        const uint8_t cr = fxShot ? fxByte(tr.color.x * 150.0f) : fxByte(hudWeaponProfile.tracerR);
        const uint8_t cg = fxShot ? fxByte(tr.color.y * 150.0f) : fxByte(hudWeaponProfile.tracerG);
        const uint8_t cb = fxShot ? fxByte(tr.color.z * 150.0f) : fxByte(hudWeaponProfile.tracerB);
        ui.line(x0, y0, x1, y1, tracerWidth * (fxShot ? 2.8f : 2.0f),
                rgba(gr, gg, gb, glowA));
        ui.line(x0, y0, x1, y1, tracerWidth * (fxShot ? 1.12f : 1.0f),
                rgba(cr, cg, cb, coreA));
        if (fxShot) {
            const float nx = -dy, ny = dx;
            int stripe = 0;
            const auto drawStripe = [&](uint32_t bit, uint32_t col) {
                if ((tr.effectMask & bit) == 0) return;
                const float off = (static_cast<float>(stripe) - 1.5f) * std::max(1.25f, tracerWidth * 1.35f);
                ui.line(x0 + nx * off, y0 + ny * off, x1 + nx * off, y1 + ny * off,
                        std::max(1.0f, tracerWidth * 0.46f), withAlpha(col, static_cast<uint8_t>(coreA * 0.82f)));
                ++stripe;
            };
            drawStripe(ShotFxBurn, DmgTextBurn);
            drawStripe(ShotFxShock, DmgTextShock);
            drawStripe(ShotFxCryo, DmgTextCryo);
            drawStripe(ShotFxCorrode, DmgTextCorrode);
            drawStripe(ShotFxLeech, DmgTextLeech);
            if (tr.effectMask & ShotFxBurn) {
                const uint8_t a = static_cast<uint8_t>(clamp(static_cast<float>(glowA) * 0.82f, 0.0f, 255.0f));
                ui.line(x0, y0, x1, y1, tracerWidth * 4.3f, withAlpha(DmgTextBurn, a));
            }
            if (tr.effectMask & ShotFxShock) {
                const uint8_t a = static_cast<uint8_t>(clamp(static_cast<float>(coreA) * 0.82f, 0.0f, 255.0f));
                float px = x0, py = y0;
                constexpr int Segs = 6;
                for (int i = 1; i <= Segs; ++i) {
                    const float f = static_cast<float>(i) / static_cast<float>(Segs);
                    const bool end = i == Segs;
                    const float wobble = end ? 0.0f
                        : ((i & 1) ? 1.0f : -1.0f) * std::max(2.0f, tracerWidth * 1.85f)
                            * (0.70f + 0.30f * std::sin(shakeTime_ * 48.0f + f * 9.0f));
                    const float qx = x0 + (x1 - x0) * f + nx * wobble;
                    const float qy = y0 + (y1 - y0) * f + ny * wobble;
                    ui.line(px, py, qx, qy, std::max(1.0f, tracerWidth * 0.72f), withAlpha(DmgTextShock, a));
                    px = qx; py = qy;
                }
            }
            if (tr.effectMask & ShotFxCryo) {
                const uint8_t a = static_cast<uint8_t>(clamp(static_cast<float>(coreA) * 0.68f, 0.0f, 255.0f));
                ui.line(x0, y0, x1, y1, std::max(1.0f, tracerWidth * 0.55f), withAlpha(DmgTextCryo, a));
                for (int i = 1; i <= 4; ++i) {
                    const float f = static_cast<float>(i) / 5.0f;
                    const float px = x0 + (x1 - x0) * f;
                    const float py = y0 + (y1 - y0) * f;
                    const float tick = std::max(3.0f, tracerWidth * 2.3f);
                    ui.line(px - nx * tick, py - ny * tick, px + nx * tick, py + ny * tick,
                            std::max(1.0f, tracerWidth * 0.44f), withAlpha(DmgTextCryo, a));
                }
            }
            if (tr.effectMask & ShotFxCorrode) {
                const uint8_t a = static_cast<uint8_t>(clamp(static_cast<float>(coreA) * 0.70f, 0.0f, 255.0f));
                for (int i = 0; i < 6; ++i) {
                    const float f = (static_cast<float>(i) + 0.65f) / 6.8f;
                    const float off = std::sin(shakeTime_ * 13.0f + f * 11.0f) * std::max(1.4f, tracerWidth * 1.1f);
                    const float len = std::max(4.0f, dl * 0.020f);
                    const float px = x0 + (x1 - x0) * f + nx * off;
                    const float py = y0 + (y1 - y0) * f + ny * off;
                    ui.line(px - dx * len, py - dy * len, px + dx * len, py + dy * len,
                            std::max(1.2f, tracerWidth * 0.72f), withAlpha(DmgTextCorrode, a));
                }
            }
            if (tr.effectMask & ShotFxLeech) {
                const uint8_t a = static_cast<uint8_t>(clamp(static_cast<float>(coreA) * 0.78f, 0.0f, 255.0f));
                ui.line(x1, y1, x0, y0, std::max(1.0f, tracerWidth * 0.76f), withAlpha(DmgTextLeech, a));
            }
        }
    }

    // Floating combat numbers, projected from enemy-space to screen-space. They inherit the
    // source effect colour so damage-over-time, chains, combos and leech are readable in combat.
    for (const CombatText& t : combatTexts_) {
        const float life = 1.0f - clamp(t.age / std::max(0.001f, t.life), 0.0f, 1.0f);
        if (life <= 0.0f) continue;
        float tx = 0.0f, ty = 0.0f;
        if (!projectWorldPoint(t.pos, tx, ty)) continue;
        const float pop = 1.0f + 0.18f * (1.0f - std::fabs(life * 2.0f - 1.0f));
        tx = std::round(tx);
        ty = std::round(ty);
        const float sc = fpx(ui, 20.0f * t.scale * pop, s);
        const float sh = std::round(1.6f * s);
        const uint8_t a = static_cast<uint8_t>(clamp(life * 255.0f, 0.0f, 255.0f));
        ui.textDCentered(tx + sh, ty + sh, t.text, rgba(0, 0, 0, static_cast<uint8_t>(a * 0.78f)), sc);
        ui.textDCentered(tx, ty, t.text, withAlpha(t.color, a), sc);
    }

    // Directional damage wedges.
    for (const DamageMarker& m : damageMarkers_) {
        const float t = clamp(m.age / std::max(0.05f, m.life), 0.0f, 1.0f);
        const float fade = 1.0f - t * t;
        const uint8_t a = static_cast<uint8_t>(clamp(fade * (0.5f + 0.5f * m.intensity), 0.0f, 1.0f) * 210.0f);
        if (a == 0u) continue;
        const float rel = wrapAngle(m.worldAngle - player_.yaw);
        const float screenAngle = std::atan2(-std::cos(rel), std::sin(rel));
        const float half = degToRad(22.0f);
        const float ringR = std::min(w, h) * 0.17f;
        const float thickness = std::min(w, h) * 0.052f;
        const uint32_t col = rgba(255, 46, 32, a);
        const int segs = 8;
        for (int si = 0; si < segs; ++si) {
            const float a0 = screenAngle - half + (2.0f * half) * (static_cast<float>(si) / segs);
            const float a1 = screenAngle - half + (2.0f * half) * (static_cast<float>(si + 1) / segs);
            const float t0 = thickness * (0.35f + 0.65f * (1.0f - std::fabs(static_cast<float>(si) / segs - 0.5f) * 2.0f));
            const float t1 = thickness * (0.35f + 0.65f * (1.0f - std::fabs(static_cast<float>(si + 1) / segs - 0.5f) * 2.0f));
            const float ix0 = cx + std::cos(a0) * ringR, iy0 = cy + std::sin(a0) * ringR;
            const float ox0 = cx + std::cos(a0) * (ringR + t0), oy0 = cy + std::sin(a0) * (ringR + t0);
            const float ix1 = cx + std::cos(a1) * ringR, iy1 = cy + std::sin(a1) * ringR;
            const float ox1 = cx + std::cos(a1) * (ringR + t1), oy1 = cy + std::sin(a1) * (ringR + t1);
            ui.triangle(ix0, iy0, ox0, oy0, ox1, oy1, col);
            ui.triangle(ix0, iy0, ox1, oy1, ix1, iy1, col);
        }
    }

    if (hitmarkerTimer_ > 0.0f || precisionMarkerTimer_ > 0.0f) {
        const bool prec = precisionMarkerTimer_ > 0.0f;        // gold = headshot/precision
        const uint32_t c = prec ? rgb(255, 214, 84) : rgb(255, 255, 255);
        const float o = prec ? 24.0f : 19.0f;                 // precision marker reaches wider
        const float i = prec ? 9.0f : 7.0f;
        ui.line(cx - o, cy - o, cx - i, cy - i, 4, c); ui.line(cx + o, cy - o, cx + i, cy - i, 4, c);
        ui.line(cx - o, cy + o, cx - i, cy + i, 4, c); ui.line(cx + o, cy + o, cx + i, cy + i, 4, c);
        if (prec) ui.rect(cx - 2, cy - 2, 4, 4, c);           // center stamp
    }
    if (killConfirmTimer_ > 0.0f) {
        const uint32_t c = rgb(255, 44, 44);
        ui.line(cx - 24, cy - 24, cx - 10, cy - 10, 4, c); ui.line(cx + 24, cy - 24, cx + 10, cy - 10, 4, c);
        ui.line(cx - 24, cy + 24, cx - 10, cy + 10, 4, c); ui.line(cx + 24, cy + 24, cx + 10, cy + 10, 4, c);
    }

    // Boss health bar (M5): a top-centre bar while the boss is alive; the name + an EXPOSED tell
    // when the weak-point window is open (read the telegraph, dodge, then punish the EXPOSED window).
    for (const Enemy& e : enemies_) {
        if (!e.active || !e.boss) continue;
        const float frac = e.maxHealth > 0.0f ? clamp(e.health / e.maxHealth, 0.0f, 1.0f) : 0.0f;
        const float bw = w * 0.44f, bx = cx - bw * 0.5f, by = 40.0f;
        const std::string bn = e.bossVulnTimer > 0.0f ? std::string(bossName(e.bossKind)) + "  -  EXPOSED"
                                                       : std::string(bossName(e.bossKind));
        glowCentered(ui, cx, by - 30.0f, bn, withAlpha(pal::bossCore, 90),
                     e.bossVulnTimer > 0.0f ? pal::synergyHi : pal::bossCoreHi, 1.9f * ts, 3.0f);
        cardPanel(ui, bx - 4.0f, by - 4.0f, bw + 8.0f, 20.0f, rgba(20, 13, 14, 236), rgba(11, 7, 8, 236),
                  withAlpha(pal::bossCore, 150), 1.0f, true);
        glossBar(ui, bx, by, bw, 12.0f, frac, rgb(44, 20, 22), rgb(236, 74, 62));
        break;
    }

    // ===== in-game HUD (Neon Ink Brutalism): edges only, the centre stays clear ====
    // Everything lives on the screen edges so the reticle + the world own the middle.
    // The combat clusters only read in an active room/boss; menus draw their own scrim.
    const bool inRun = (phase_ == RunPhase::InRoom || phase_ == RunPhase::Boss);
    const bool worldChoiceOverlay = inRun || phase_ == RunPhase::DoorsOpen;

    // Top-left: SCORE + the run breadcrumb (location). HUD law: score + breadcrumb live
    // top-left; the boss bar owns top-centre and the Pulse owns bottom-centre.
    {
        if (worldChoiceOverlay)
            ui.rect(margin - 14.0f * s, margin - 10.0f * s, 560.0f * s, inRun ? 100.0f * s : 60.0f * s,
                    withAlpha(pal::hudScrim, 112));
        ui.textTracked(margin, margin, "SCORE", pal::textFaint, fpx(ui, 13.0f, s), 2.0f * s);
        ui.textTracked(margin, margin + 17.0f * s, std::to_string(score_), pal::textMid, fpx(ui, 30.0f, s), 0.5f * s);
        if (inRun) {
            const std::string roomV = run_.currentIsBoss() ? std::string("BOSS") : std::to_string(run_.roomInSector());
            const int waveTot = static_cast<int>(run_.currentRoom().waves.size());
            const int waveN = std::min(waveIndex_ + 1, std::max(1, waveTot));
            const std::string crumb = std::string(biomeName(currentBiome_)) + "  /  SECTOR " +
                std::to_string(run_.sector() + 1) + "  /  ROOM " + roomV +
                "  /  WAVE " + std::to_string(waveN) + "/" + std::to_string(std::max(1, waveTot));
            ui.textTracked(margin, margin + 52.0f * s, crumb, pal::textDim, fpx(ui, 13.0f, s), 1.1f * s);
            const int hostiles = activeEnemyCount() + waveSpawnsLeft_;
            ui.textTracked(margin, margin + 70.0f * s, std::to_string(hostiles) + " HOSTILES",
                           hostiles > 0 ? pal::danger : pal::textDim, fpx(ui, 13.0f, s), 1.1f * s);
        } else {
            ui.text(margin, margin + 48.0f * s, "BEST " + std::to_string(bestScore_), pal::textFaint, fpx(ui, 12.0f, s));
        }
    }
    // Top-right: SCRAP (orange), with a brief +N pop on a kill.
    if (inRun || scrap_ > 0) {
        if (worldChoiceOverlay)
            ui.rect(w - margin - 164.0f * s, margin - 10.0f * s, 178.0f * s, 72.0f * s,
                    withAlpha(pal::hudScrim, 104));
        const float lblSc = fpx(ui, 13.0f, s);
        ui.textTracked(w - margin - ui.textTrackedWidth("SCRAP", lblSc, 2.0f * s), margin, "SCRAP", pal::textFaint, lblSc, 2.0f * s);
        ui.textRight(w - margin, margin + 17.0f * s, std::to_string(scrap_), pal::gold, fpx(ui, 30.0f, s));
        if (scrapFlashTimer_ > 0.0f)
            ui.textRight(w - margin, margin + 48.0f * s, "+" + std::to_string(scrapFlashAmount_),
                         withAlpha(pal::goldGlow, static_cast<uint8_t>(220.0f * clamp(scrapFlashTimer_ / 0.7f, 0.0f, 1.0f))), fpx(ui, 14.0f, s));
    }

    // Bottom-centre: THE PULSE METER (signature). On the crosshair sightline; slim; with the
    // number cap + hatch track + gradient fill + bloomed leading head + 20/50/80 notches and a
    // PULSE label (left) + STATE . DIRECTION (right) on the row above the bar.
    if (inRun) {
        const float pmw = 600.0f * s, pmh = 30.0f * s;
        const float pmx = cx - pmw * 0.5f;
        const float pmy = h - margin - pmh;
        // Local adaptive scrim behind the group (never a full-screen overlay); covers the
        // label row above the bar too.
        ui.rect(pmx - 16.0f * s, pmy - 28.0f * s, pmw + 32.0f * s, pmh + 36.0f * s, withAlpha(pal::hudScrim, 120));
        // Accessibility: suppress the loss-flash wash (reduce flashes) and freeze the leading-head
        // pulse (reduce motion) by feeding a constant clock.
        const float loss = settings_.reduceFlashes ? 0.0f : pulse_.lossFlash01();
        const float pmClock = settings_.reduceMotion ? 0.0f : shakeTime_;
        drawPulseMeter(ui, pmx, pmy, pmw, pmh, pulse_.value(), pulse_.tier(), pulse_.dir(),
                       loss, pmClock, s, true);
    }

    // Bottom-left: optional status chip + the vitals module + ability pips + build tally.
    if (inRun) {
        const float mw = 232.0f * s, mh = 84.0f * s;
        const float clusterH = mh + 12.0f * s + 64.0f * s;     // module + gap + ability pills + affinity row
        const float mx = margin, my = h - margin - clusterH;
        if (overdriveTimer_ > 0.0f)
            statusChip(ui, mx, my - 36.0f * s, "OVERDRIVE",
                       std::to_string(static_cast<int>(std::ceil(overdriveTimer_))) + "s", false, s);
        chamferPanel(ui, mx, my, mw, mh, cut, pal::navy, pal::inkStroke, std::max(1.5f, 1.5f * s), true);
        ui.rect(mx, my + cut, std::max(3.0f, 3.0f * s), mh - cut, pal::accent);   // 3px cyan left spine
        const float ix = mx + 18.0f * s;
        const int maxHp = std::max(1, effectiveMaxHealth());
        const int maxSh = std::max(1, effectiveMaxShield());
        const bool lowHp = player_.hp <= maxHp / 3;
        const uint32_t hpCol = lowHp ? pal::danger : pal::accent;
        ui.rect(ix, my + 20.0f * s, 14.0f * s, 4.0f * s, hpCol);                  // + health icon
        ui.rect(ix + 5.0f * s, my + 15.0f * s, 4.0f * s, 14.0f * s, hpCol);
        const std::string hpv = std::to_string(player_.hp);
        ui.textTracked(ix + 28.0f * s, my + 8.0f * s, hpv, lowHp ? pal::danger : pal::textHero, fpx(ui, 40.0f, s), 0.0f);
        const float hpw = ui.textTrackedWidth(hpv, fpx(ui, 40.0f, s), 0.0f);
        ui.text(ix + 28.0f * s + hpw + 5.0f * s, my + 24.0f * s, "/" + std::to_string(maxHp), pal::textDim, fpx(ui, 20.0f, s));
        ui.textRight(mx + mw - 14.0f * s, my + 12.0f * s, std::string(player_.shield > 0 ? "+" : "") + std::to_string(player_.shield) + " SHLD",
                     player_.shield > 0 ? pal::shieldCol : pal::textDim, fpx(ui, 14.0f, s));
        const float barX = ix, barW = mw - 32.0f * s;
        glossBar(ui, barX, my + mh - 22.0f * s, barW, 8.0f * s, static_cast<float>(player_.hp) / static_cast<float>(maxHp),
                 pal::track, lowHp ? pal::danger : pal::accent);                  // 8px HP bar (cyan player / red low)
        glossBar(ui, barX, my + mh - 10.0f * s, barW, 4.0f * s, static_cast<float>(player_.shield) / static_cast<float>(maxSh),
                 pal::track, pal::shieldCol);                                     // 4px orange shield bar
        // ability CHIPS (UI-guide pills): DASH / GRENADE / ULT, each ready-lit or showing charge.
        const float pipY = my + mh + 12.0f * s;
        const float dashReady = player_.dashCooldown <= 0.0f ? 1.0f
            : 1.0f - clamp(player_.dashCooldown / std::max(0.01f, tunables_.dashCooldown), 0.0f, 1.0f);
        float chx = mx;
        chx += abilityChip(ui, chx, pipY, "DASH", dashReady >= 1.0f, dashReady, pal::accent, false, s) + 8.0f * s;
        chx += abilityChip(ui, chx, pipY, "GRENADE", tacticalCharge_ >= 1.0f, clamp(tacticalCharge_, 0.0f, 1.0f), pal::accent, false, s) + 8.0f * s;
        chx += abilityChip(ui, chx, pipY, ultimateCharge_ >= 1.0f ? "ULT READY" : "ULT", ultimateCharge_ >= 1.0f,
                           clamp(ultimateCharge_, 0.0f, 1.0f), pal::amber, true, s) + 8.0f * s;
        // M7 build readout: an AFFINITY strip - your owned affinities as coloured glyphs with
        // counts, LIT (with a ring) when a SET is active (3 = amp, 5 = signature). The build
        // identity reads at a glance, replacing the anonymous tier-dot tally.
        float ax = mx + 10.0f * s; const float ay = pipY + 36.0f * s;   // a row below the ability pills
        const BuildStats& bst = build_.stats();
        bool anyAff = false;
        for (int a = 1; a < static_cast<int>(Affinity::Count); ++a) {
            const int cnt = bst.affinityCount[a];
            if (cnt <= 0) continue;
            anyAff = true;
            const AffVis av = affVis(static_cast<Affinity>(a));
            const bool set3 = cnt >= 3, set5 = cnt >= 5;
            if (set5) { diamondOutline(ui, ax, ay, 11.0f * s, av.col, std::max(1.5f, 1.6f * s)); diamondOutline(ui, ax, ay, 13.0f * s, withAlpha(av.col, 90), std::max(1.0f, 1.0f * s)); }
            else if (set3) diamondOutline(ui, ax, ay, 10.0f * s, withAlpha(av.col, 190), std::max(1.0f, 1.2f * s));
            drawIcon(ui, av.icon, ax, ay, 6.0f * s, set3 ? av.col : withAlpha(av.col, 160), s);
            ui.text(ax + 12.0f * s, ay - 7.0f * s, std::to_string(cnt), set3 ? av.col : pal::textDim, fpx(ui, 14.0f, s));
            ax += 34.0f * s;
        }
        if (!anyAff)   // no affinity items yet: a tiny item count so the tally still reads
            ui.textTracked(mx, pipY + 30.0f * s, std::to_string(build_.totalItems()) + " ITEMS", pal::textDim, fpx(ui, 13.0f, s), 1.1f * s);
    }

    // Bottom-right: the active weapon module (ammo / reserve / mag bar).
    if (inRun) {
        const WeaponDef& wd = activeWeaponDef();
        const int mag = std::max(1, weaponMagazine());
        const int lvl = activeWeaponPower();
        const float mw = 264.0f * s, mh = 92.0f * s;
        const float mx = w - margin - mw, my = h - margin - mh;
        // header: fire-mode tag + weapon name + level.
        const float hy = my - 30.0f * s;
        const std::string mode = wd.automatic ? "AUTO" : "SEMI";
        const float modeSc = fpx(ui, 12.0f, s);
        const float tagW = ui.textTrackedWidth(mode, modeSc, 1.0f * s) + 14.0f * s, tagH = 18.0f * s;
        chamferOutline(ui, mx, hy, tagW, tagH, 4.0f * s, pal::textDim, std::max(1.0f, 1.0f * s));
        ui.textTracked(mx + 7.0f * s, hy + 3.0f * s, mode, pal::textDim, modeSc, 1.0f * s);
        ui.textTracked(mx + tagW + 10.0f * s, hy + 1.0f * s, wd.name, pal::textHi, fpx(ui, 22.0f, s), 0.6f * s);
        if (lvl > 1) ui.textRight(mx + mw, hy + 4.0f * s, "Lv" + std::to_string(lvl), pal::accent, fpx(ui, 16.0f, s));
        // M3: active weapon form. Base form is real too, so say that before aspects are cycled.
        const int forms = aspectsUnlocked();
        if (forms > 1) {
            if (const WeaponAspect* asp = activeAspect())
                ui.textTracked(mx + tagW + 10.0f * s, hy + 22.0f * s, std::string("ASPECT: ") + asp->name + " . X cycle",
                               pal::tierLegendary, fpx(ui, 12.0f, s), 0.8f * s);
            else
                ui.textTracked(mx + tagW + 10.0f * s, hy + 22.0f * s, "BASE FORM . X cycle aspect",
                               pal::textFaint, fpx(ui, 11.0f, s), 0.8f * s);
        } else if (!wd.aspects.empty()) {
            ui.textTracked(mx + tagW + 10.0f * s, hy + 22.0f * s, "ASPECT LOCKED . forge to Lv2",
                           pal::textFaint, fpx(ui, 11.0f, s), 0.8f * s);
        }
        // module.
        chamferPanel(ui, mx, my, mw, mh, cut, pal::navy, pal::inkStroke, std::max(1.5f, 1.5f * s), true);
        ui.rect(mx + mw - std::max(3.0f, 3.0f * s), my, std::max(3.0f, 3.0f * s), mh - cut, pal::accent);  // 3px cyan right spine
        const int lowAmmo = std::max(1, mag / 4);
        const bool low = !weapon_.reloading && weapon_.ammo <= lowAmmo;
        const uint32_t ammoCol = weapon_.reloading ? pal::accent : (low ? pal::danger : pal::textHero);
        const std::string reserveTxt = activeWeaponProfile().infiniteReserve ? std::string("INF") : std::to_string(weapon_.reserve);
        ui.textRight(mx + mw - 18.0f * s, my + 12.0f * s, std::to_string(weapon_.ammo) + " / " + std::to_string(mag), ammoCol, fpx(ui, 40.0f, s));
        ui.textRight(mx + mw - 18.0f * s, my + mh - 30.0f * s, "RESERVE " + reserveTxt, pal::textDim, fpx(ui, 14.0f, s));
        const float magFrac = weapon_.reloading
            ? (reloadDuration() > 0.0f ? 1.0f - clamp(weapon_.reloadRemaining / reloadDuration(), 0.0f, 1.0f) : 0.0f)
            : static_cast<float>(weapon_.ammo) / static_cast<float>(mag);
        glossBar(ui, mx + 18.0f * s, my + mh - 12.0f * s, mw - 36.0f * s, 4.0f * s, magFrac,
                 pal::track, weapon_.reloading ? pal::accent : (low ? pal::danger : pal::textDim));
        if (weapon_.reloading)
            ui.text(mx + 18.0f * s, my + 12.0f * s, "RELOADING", pal::accent, fpx(ui, 14.0f, s));
        else if (loadout_.size() > 1)
            ui.text(mx + 18.0f * s, my + 12.0f * s, "[Q] " + std::to_string(activeWeapon_ + 1) + "/" + std::to_string(loadout_.size()), pal::textFaint, fpx(ui, 12.0f, s));
    }

    if (overdriveTimer_ > 0.0f && !inRun)
        glowCentered(ui, cx, 70.0f, "OVERDRIVE", withAlpha(pal::accentGlow, 95), pal::accentGlow, 2.4f * ts, 5.0f);

    if (shieldFlashTimer_ > 0.0f)
        ui.rect(0, 0, static_cast<float>(w), static_cast<float>(h),
                rgba(50, 180, 255, static_cast<uint8_t>(75.0f * clamp(shieldFlashTimer_ / 0.28f, 0.0f, 1.0f))));
    if (lifeLeechFlashTimer_ > 0.0f)
        ui.rect(0, 0, static_cast<float>(w), static_cast<float>(h),
                rgba(42, 220, 118, static_cast<uint8_t>(46.0f * clamp(lifeLeechFlashTimer_ / 0.22f, 0.0f, 1.0f))));
    if (damageFlashTimer_ > 0.0f)
        ui.rect(0, 0, static_cast<float>(w), static_cast<float>(h),
                rgba(255, 30, 20, static_cast<uint8_t>(110.0f * clamp(damageFlashTimer_ / 0.28f, 0.0f, 1.0f))));
    // Spatial doors: an in-world label floats above each open exit (destination type + previewed
    // reward) so the merged choice reads at a glance. Drawn under the transition fade below.
    if (phase_ == RunPhase::DoorsOpen && doorFadeTimer_ <= 0.0f && !doors_.empty()) {
        const auto typeName = [](RoomType t) -> const char* {
            switch (t) {
                case RoomType::Elite: return "ELITE";  case RoomType::Cache: return "CACHE";
                case RoomType::Shop:  return "SHOP";    case RoomType::Event: return "EVENT";
                case RoomType::Boss:  return "BOSS";    default: return "COMBAT";
            }
        };
        const auto typeRoute = [](RoomType t) -> const char* {
            switch (t) {
                case RoomType::Elite: return "ELITE ROUTE";
                case RoomType::Cache: return "CACHE ROUTE";
                case RoomType::Shop:  return "FIELD SHOP";
                case RoomType::Event: return "PACT ROUTE";
                case RoomType::Boss:  return "SECTOR BOSS";
                default:              return "COMBAT ROUTE";
            }
        };
        const auto typeSummary = [](RoomType t) -> const char* {
            switch (t) {
                case RoomType::Elite: return "Hard fight. Better payout.";
                case RoomType::Cache: return "No fight. Free resources.";
                case RoomType::Shop:  return "Spend scrap on repairs and forge work.";
                case RoomType::Event: return "Boon now. Curse attached.";
                case RoomType::Boss:  return "Kill the sector boss.";
                default:              return "Clean fight. Standard payout.";
            }
        };
        const auto typeCol = [](RoomType t) -> uint32_t {
            switch (t) {
                case RoomType::Elite: return pal::roomElite;
                case RoomType::Cache: return pal::roomCache;
                case RoomType::Shop:  return pal::good;
                case RoomType::Event: return pal::crimsonHi;
                case RoomType::Boss:  return pal::roomBoss;
                default:              return pal::roomCombat;
            }
        };
        const auto typeIcon = [](RoomType t) -> IconKind {
            switch (t) {
                case RoomType::Elite: return IconKind::Star;
                case RoomType::Cache: return IconKind::Crate;
                case RoomType::Shop:  return IconKind::Coin;
                case RoomType::Event: return IconKind::Diamond;
                case RoomType::Boss:  return IconKind::Reticle;
                default:              return IconKind::Reticle;
            }
        };
        // RISK = 1..3 filled squares (the fight's danger / commitment), coloured by severity, so a
        // glance reads the cost of a door alongside its previewed reward (the doors paradigm).
        const auto riskLevel = [](RoomType t) -> int {
            switch (t) {
                case RoomType::Elite: case RoomType::Boss: return 3;
                case RoomType::Event:                      return 2;
                default:                                   return 1;   // Combat / Cache / Shop
            }
        };
        const auto riskText = [](RoomType t) -> const char* {
            switch (t) {
                case RoomType::Elite: return "HIGH RISK";
                case RoomType::Boss:  return "BOSS RISK";
                case RoomType::Event: return "PACT RISK";
                case RoomType::Cache: return "NO FIGHT";
                case RoomType::Shop:  return "SAFE ROOM";
                default:              return "LOW RISK";
            }
        };
        const auto riskCol = [](RoomType t) -> uint32_t {
            switch (t) {
                case RoomType::Elite: return pal::tierRare;  case RoomType::Boss: return pal::danger;
                case RoomType::Event: return pal::warn;      default:             return pal::good;
            }
        };

        // Exit command strip: the state change first, then the route context. This replaces the old
        // tiny breadcrumb stack and gives the player one readable order: cleared -> exits -> choose.
        const float deckW = std::min(static_cast<float>(w) - margin * 2.0f, 1120.0f * s);
        const float deckH = 138.0f * s;
        const float deckX = cx - deckW * 0.5f;
        const float deckY = 34.0f * s;
        const float deckCut = 16.0f * s;
        chamferFill(ui, deckX - 10.0f * s, deckY + 10.0f * s, deckW + 20.0f * s, deckH + 4.0f * s, deckCut + 4.0f * s, rgba(0, 0, 0, 72));
        chamferPanel(ui, deckX, deckY, deckW, deckH, deckCut, rgba(7, 10, 15, 220), withAlpha(pal::border, 230), std::max(1.0f, 1.3f * s), false);
        ui.rect(deckX + deckCut, deckY + 1.0f * s, deckW - deckCut * 2.0f, 2.0f * s, withAlpha(pal::roomCombat, 92));
        ui.rect(deckX, deckY + deckCut, std::max(4.0f, 4.0f * s), deckH - deckCut, pal::roomCombat);

        const std::string loc = std::string(biomeName(currentBiome_)) + " / SECTOR " +
            std::to_string(run_.sector() + 1) + " / ROOM " + std::to_string(run_.roomInSector());
        ui.textTracked(deckX + 32.0f * s, deckY + 18.0f * s, loc, pal::textDim, fpx(ui, 13.0f, s), 2.0f * s);
        ui.textD(deckX + 32.0f * s, deckY + 42.0f * s, "EXITS OPEN", pal::textHero, fpx(ui, 46.0f, s));
        ui.text(deckX + 34.0f * s, deckY + 99.0f * s, "Walk through a door to lock the route and take its previewed reward.",
                pal::textMid, fpx(ui, 15.0f, s));

        const float railCx = deckX + deckW - 350.0f * s;
        ui.textTracked(railCx - 244.0f * s, deckY + 22.0f * s, "RUN ROUTE", pal::textFaint, fpx(ui, 12.0f, s), 2.0f * s);
        drawRouteRail(ui, railCx, deckY + 62.0f * s, s * 1.08f);   // M4: the run-route map (biome journey)
        const std::string stepText = "STEP " + std::to_string(run_.roomIndex() + 1) + " / " +
            std::to_string(std::max(1, run_.roomCount()));
        ui.textRight(deckX + deckW - 32.0f * s, deckY + 96.0f * s, stepText, pal::textDim, fpx(ui, 14.0f, s));

        const int focus = doorAtPlayer();
        for (size_t di = 0; di < doors_.size(); ++di) {
            const DoorBind& d = doors_[di];
            const Projection p = projectPoint(d.triggerCenter, renderViewYaw_, renderViewPitch_, w, h, 0.6f);
            if (!p.visible) continue;
            const bool atDoor = (static_cast<int>(di) == focus);
            const RouteVisual route = routeVisual(d.destType);
            const uint32_t tc = route.col;
            const std::string title = route.name;

            // Reward block: the pickup/service name, its plain-language effect, and the build hook.
            std::string rewardName;
            std::string rewardMeta;
            std::string rewardEffect;
            uint32_t rewardCol = pal::textHero;
            IconKind rewardIcon = typeIcon(d.destType);
            Affinity rewardAff = Affinity::None;
            if (!d.rewardId.empty()) {
                const Build::RewardView rv = build_.describeReward(d.rewardId);
                if (rv.valid) {
                    rewardAff = rv.affinity;
                    rewardName = rv.name;
                    rewardEffect = rv.blurb;
                    rewardMeta = std::string(tierName(rv.tier)) + " " + (rv.isWeapon ? "WEAPON" : "PASSIVE");
                    if (rewardAff != Affinity::None) {
                        const AffVis av = affVis(rewardAff);
                        rewardMeta = std::string(av.name) + " BUILD / " + tierName(rv.tier);
                        rewardCol = av.col;
                        rewardIcon = av.icon;
                    } else {
                        switch (rv.tier) {
                            case ItemTier::Legendary: rewardCol = pal::tierLegendary; break;
                            case ItemTier::Rare:      rewardCol = pal::tierRare;      break;
                            case ItemTier::Uncommon:  rewardCol = pal::tierUncommon;  break;
                            default:                  rewardCol = pal::tierCommon;    break;
                        }
                        rewardIcon = rv.isWeapon ? IconKind::Rifle : IconKind::Crate;
                    }
                }
            }
            if (rewardName.empty()) {
                switch (d.destType) {
                    case RoomType::Shop:
                        rewardName = "SHOP ACCESS"; rewardMeta = "SERVICE"; rewardEffect = "Buy stock, repair, reroll, or forge the weapon."; rewardCol = pal::good; rewardIcon = IconKind::Coin; break;
                    case RoomType::Event:
                        rewardName = "PACT OFFER"; rewardMeta = "BOON / CURSE"; rewardEffect = "A quantified power trade with a lasting cost."; rewardCol = pal::crimsonHi; rewardIcon = IconKind::Diamond; break;
                    case RoomType::Boss:
                        rewardName = "WARDEN SIGNAL"; rewardMeta = "BOSS"; rewardEffect = "Defeat the sector boss to advance the run."; rewardCol = pal::danger; rewardIcon = IconKind::Reticle; break;
                    case RoomType::Cache:
                        rewardName = "SUPPLY CACHE"; rewardMeta = "NO FIGHT"; rewardEffect = "Take resources without burning health or ammo."; rewardCol = pal::roomCache; rewardIcon = IconKind::Crate; break;
                    default:
                        rewardName = "ROOM PAYOUT"; rewardMeta = "STANDARD REWARD"; rewardEffect = "Clear the next fight to choose from a fresh reward."; rewardCol = pal::textMid; rewardIcon = IconKind::Reticle; break;
                }
            }

            // Build line: how the reward feeds your run, not just what it is called.
            std::string buildVal; uint32_t buildCol = pal::textMid;
            if (rewardAff != Affinity::None) {
                buildVal = affinityProgressLine(build_.stats(), rewardAff);
                buildCol = affVis(rewardAff).col;
            } else {
                buildVal = route.buildHook;
            }

            // A fixed, readable route card (does NOT shrink with door distance); clamped on-screen.
            const float sc      = atDoor ? 1.0f : 0.90f;
            const float pad     = 24.0f * sc * s;
            const float cardW   = (atDoor ? 720.0f : 640.0f) * sc * s;
            const float cardH   = (atDoor ? 332.0f : 302.0f) * sc * s;
            const float lx      = static_cast<float>(p.left + p.right) * 0.5f;
            float cx0 = clamp(lx - cardW * 0.5f, 12.0f * s, static_cast<float>(w) - cardW - 12.0f * s);
            float cy0 = std::max(static_cast<float>(p.top) - cardH - 22.0f * s, deckY + deckH + 22.0f * s);
            cy0 = std::min(cy0, static_cast<float>(h) - cardH - 104.0f * s);
            const float cardCut = 16.0f * sc * s;
            const uint32_t borderCol = atDoor ? pal::accent : withAlpha(tc, 215);

            // Door locator: card connects to the door, so the label feels spatial, not like a random HUD box.
            const float anchorY = std::max(cy0 + cardH + 12.0f * s, static_cast<float>(p.top) - 10.0f * s);
            ui.line(lx, cy0 + cardH, lx, anchorY, std::max(1.0f, 1.2f * s), withAlpha(borderCol, atDoor ? 180 : 105));
            diamond(ui, lx, anchorY, atDoor ? 7.0f * s : 5.0f * s, atDoor ? pal::accent : withAlpha(tc, 180));

            chamferPanel(ui, cx0, cy0, cardW, cardH, cardCut, rgba(7, 10, 15, 236), borderCol,
                         std::max(1.5f, (atDoor ? 2.2f : 1.5f) * s), true);
            ui.rect(cx0, cy0 + cardCut, std::max(4.0f, 4.0f * sc * s), cardH - cardCut, tc);
            if (atDoor) focusRing(ui, cx0, cy0, cardW, cardH, cardCut, s);

            const float iconSz = 58.0f * sc * s;
            iconTile(ui, cx0 + pad, cy0 + 24.0f * sc * s, iconSz, 10.0f * sc * s, route.icon, tc, s);
            ui.textTracked(cx0 + pad + iconSz + 18.0f * s, cy0 + 23.0f * sc * s,
                           "[" + std::to_string(di + 1) + "] " + std::string(route.tag) + " ROUTE",
                           tc, fpx(ui, 13.0f * sc, s), 1.6f * sc * s);
            float titleSc = fpx(ui, 30.0f * sc, s);
            const float titleMax = cardW - pad * 2.0f - iconSz - 38.0f * s - 174.0f * sc * s;
            if (ui.textDWidth(title, titleSc) > titleMax) titleSc = fpx(ui, 25.0f * sc, s);
            ui.textD(cx0 + pad + iconSz + 18.0f * s, cy0 + 44.0f * sc * s, title, pal::textHero, titleSc);
            textDWrapped(ui, cx0 + pad + iconSz + 18.0f * s, cy0 + 82.0f * sc * s,
                         cardW - pad * 2.0f - iconSz - 210.0f * sc * s,
                         route.stake, pal::textMid, fpx(ui, 17.0f * sc, s), 2.0f * sc * s, 2);

            // Risk tag: text + pips, so it survives colorblind mode and tiny captures.
            {
                const int lvl = route.riskLevel;
                const uint32_t rc = riskColorForLevel(lvl);
                const float tagW = 168.0f * sc * s, tagH = 42.0f * sc * s;
                const float tx = cx0 + cardW - pad - tagW, ty = cy0 + 24.0f * sc * s;
                chamferFill(ui, tx, ty, tagW, tagH, 8.0f * sc * s, withAlpha(rc, 22));
                chamferOutline(ui, tx, ty, tagW, tagH, 8.0f * sc * s, withAlpha(rc, 190), std::max(1.0f, 1.1f * s));
                ui.textTracked(tx + 13.0f * sc * s, ty + 8.0f * sc * s,
                               std::string("RISK ") + route.risk, rc, fpx(ui, 12.0f * sc, s), 1.0f * sc * s);
                const float sq = 8.0f * sc * s, gap = 4.0f * sc * s;
                const float rx = tx + tagW - 13.0f * sc * s - (sq * 3.0f + gap * 2.0f);
                for (int k = 0; k < 3; ++k)
                    ui.rect(rx + k * (sq + gap), ty + 27.0f * sc * s, sq, sq, k < lvl ? rc : pal::disabled);
            }

            const float divY = cy0 + 124.0f * sc * s;
            ui.rect(cx0 + pad, divY, cardW - pad * 2.0f, std::max(1.0f, 1.0f * s), pal::lineSoft);

            const float rewardY = divY + 18.0f * sc * s;
            const float rewardIconSz = 48.0f * sc * s;
            iconTile(ui, cx0 + pad, rewardY, rewardIconSz, 8.0f * sc * s, rewardIcon, rewardCol, s);
            ui.textTracked(cx0 + pad + rewardIconSz + 14.0f * s, rewardY + 1.0f * sc * s,
                           rewardMeta.empty() ? "REWARD" : rewardMeta, pal::textDim, fpx(ui, 12.0f * sc, s), 1.2f * sc * s);
            float rewardNameSc = fpx(ui, 27.0f * sc, s);
            const float rewardNameMax = cardW - pad * 2.0f - rewardIconSz - 22.0f * s;
            if (ui.textDWidth(rewardName, rewardNameSc) > rewardNameMax) rewardNameSc = fpx(ui, 20.0f * sc, s);
            ui.textD(cx0 + pad + rewardIconSz + 14.0f * s, rewardY + 20.0f * sc * s, rewardName, rewardCol, rewardNameSc);

            const float effectY = rewardY + 66.0f * sc * s;
            textDWrapped(ui, cx0 + pad, effectY, cardW - pad * 2.0f,
                         rewardEffect.empty() ? route.reward : rewardEffect,
                         pal::textMid, fpx(ui, 17.0f * sc, s), 3.0f * sc * s, 2);

            const float buildY = cy0 + cardH - 70.0f * sc * s;
            ui.rect(cx0 + pad, buildY - 12.0f * sc * s, cardW - pad * 2.0f, std::max(1.0f, 1.0f * s), pal::lineSoft);
            textDWrapped(ui, cx0 + pad, buildY + 5.0f * sc * s, cardW - pad * 2.0f,
                         buildVal, buildCol, fpx(ui, 16.0f * sc, s), 2.0f * sc * s, 1);

            const std::string commit = atDoor ? "IN RANGE - STEP FORWARD TO COMMIT" : "APPROACH DOOR TO COMMIT";
            // This line sits below the card directly over the lit doorway, so back it with a
            // scrim chip and keep the idle state at mid ink (not textDim) to stay legible.
            const float commitSc = fpx(ui, 12.0f * sc, s);
            const float commitW = ui.textWidth(commit, commitSc);
            const float commitCx = cx0 + cardW * 0.5f, commitY = cy0 + cardH + 10.0f * s;
            ui.rect(commitCx - commitW * 0.5f - 10.0f * s, commitY - 4.0f * s,
                    commitW + 20.0f * s, ui.lineHeight(commitSc) + 8.0f * s, withAlpha(pal::hudScrim, 165));
            ui.textCentered(commitCx, commitY, commit,
                            atDoor ? pal::accentGlow : pal::textMid, commitSc);
        }
    }
    if (phase_ == RunPhase::RoomCleared) {
        darkBoardBg();
        auto tierColor = [](ItemTier t) -> uint32_t {
            switch (t) {
                case ItemTier::Common:   return pal::tierCommon;
                case ItemTier::Uncommon: return pal::tierUncommon;
                case ItemTier::Rare:     return pal::tierRare;
                case ItemTier::Legendary: return pal::tierLegendary;
            }
            return pal::textMid;
        };
        auto tierIcon = [](const Build::RewardView& rv, int i) -> IconKind {
            if (rv.isWeapon) return IconKind::Rifle;
            const IconKind opt[4] = { IconKind::Scope, IconKind::Bullet, IconKind::Lightning, IconKind::Shield };
            return opt[i % 4];
        };
        auto weaponArchName = [](WeaponArchetype a) -> std::string {
            switch (a) {
                case WeaponArchetype::HitscanAuto: return "HITSCAN";
                case WeaponArchetype::Burst:       return "BURST";
                case WeaponArchetype::Spread:      return "SPREAD";
                case WeaponArchetype::Projectile:  return "PROJECTILE";
                case WeaponArchetype::Beam:        return "BEAM";
            }
            return "";
        };

        // Header and cards mirror the HTML guide: 48px top pad, 440px cards, 34px gaps.
        const float rewardTop = 48.0f * s;
        const std::string kicker = std::string(biomeName(currentBiome_)) + " . SECTOR " +
            std::to_string(run_.sector() + 1) + " . ROOM " + std::to_string(run_.roomInSector());
        ui.textTracked(cx - ui.textTrackedWidth(kicker, fpx(ui, 18.0f, s), 3.0f * s) * 0.5f,
                       rewardTop, kicker, pal::textFaint, fpx(ui, 18.0f, s), 3.0f * s);
        const std::string banner = run_.currentIsBoss() ? "SECTOR CLEARED" : "ROOM CLEARED";
        glowCenteredD(ui, cx, rewardTop + 28.0f * s, banner, withAlpha(pal::accentGlow, 60),
                      pal::textHero, fpx(ui, 54.0f, s));
        ui.rect(cx - 60.0f * s, rewardTop + 86.0f * s, 120.0f * s, 2.0f * s, pal::accent);
        ui.textCentered(cx, rewardTop + 104.0f * s, "Choose one", pal::textMid, fpx(ui, 22.0f, s));

        const int n = static_cast<int>(rewardOptions_.size());
        const float focusW = 520.0f * s, sideW = 420.0f * s;
        const float focusH = 404.0f * s, sideH = 360.0f * s, gap = 30.0f * s;
        float totalW = 0.0f;
        for (int i = 0; i < n; ++i) totalW += (i == cardCursor_ ? focusW : sideW) + (i > 0 ? gap : 0.0f);
        const float x0 = cx - totalW * 0.5f;
        float nextX = x0;
        for (int i = 0; i < n; ++i) {
            const Build::RewardView rv = build_.describeReward(rewardOptions_[static_cast<size_t>(i)]);
            const uint32_t col = rv.valid ? tierColor(rv.tier) : pal::textDim;
            const bool sel = (i == cardCursor_);
            const float cardW = sel ? focusW : sideW;
            const float cardH = sel ? focusH : sideH;
            const float y0 = sel ? 188.0f * s : 224.0f * s;
            const float x = nextX;
            nextX += cardW + gap;
            menuHits_.push_back({ x, y0, cardW, cardH, i });   // clickable
            if (sel) this->menuFocusHalo(ui, x, y0, cardW, cardH, 16.0f * s, pal::accentGlow, s);
            chamferPanel(ui, x, y0, cardW, cardH, 16.0f * s, pal::surface0,
                         sel ? pal::accent : withAlpha(col, 150),
                         sel ? std::max(1.5f, 1.6f * s) : std::max(1.0f, 1.0f * s), true);
            if (sel) focusRing(ui, x, y0, cardW, cardH, 16.0f * s, s);
            if (!rv.valid) continue;
            const bool hasAff = rv.affinity != Affinity::None;
            const AffVis av = affVis(rv.affinity);

            iconTile(ui, x + 26.0f * s, y0 + 22.0f * s, 64.0f * s, 10.0f * s,
                     hasAff ? av.icon : tierIcon(rv, i), hasAff ? av.col : col, s);
            const float metaSc = fpx(ui, 15.0f, s), metaTrack = 1.6f * s;
            ui.textTracked(x + 104.0f * s, y0 + 24.0f * s, rv.isWeapon ? "WEAPON" : "PASSIVE",
                           rgb(126, 138, 153), metaSc, metaTrack);
            // The SET BONUS / SYNERGY chip claims the top-right corner when an affinity is
            // already 2+ owned; suppress the tier label there so it cannot bleed out from
            // under the chip (and would otherwise be unreadable behind it anyway).
            const bool chipCovers = hasAff && build_.stats().affinityCount[static_cast<int>(rv.affinity)] >= 2;
            if (!chipCovers) {
                const std::string tier = tierName(rv.tier);
                ui.textTracked(x + cardW - 26.0f * s - ui.textTrackedWidth(tier, metaSc, metaTrack),
                               y0 + 24.0f * s, tier, col, metaSc, metaTrack);
            }

            if (hasAff) {
                const float chW = ui.textTrackedWidth(av.name, fpx(ui, 14.0f, s), 1.0f * s) + 36.0f * s;
                chamferOutline(ui, x + 104.0f * s, y0 + 48.0f * s, chW, 24.0f * s, 5.0f * s, av.col, std::max(1.0f, 1.0f * s));
                drawIcon(ui, av.icon, x + 116.0f * s, y0 + 60.0f * s, 6.0f * s, av.col, s);
                ui.textTracked(x + 132.0f * s, y0 + 53.0f * s, av.name, av.col, fpx(ui, 14.0f, s), 1.0f * s);
            } else if (rv.isWeapon) {
                std::string chip = "PROJECTILE / BURST";
                if (const WeaponDef* wd = build_.findWeapon(rv.rawId))
                    chip = weaponArchName(wd->archetype) + " / " + (wd->automatic ? "AUTO" : "SEMI");
                const float chW = ui.textTrackedWidth(chip, fpx(ui, 13.0f, s), 1.0f * s) + 22.0f * s;
                chamferOutline(ui, x + 104.0f * s, y0 + 48.0f * s, chW, 24.0f * s, 5.0f * s, pal::lineSoft, std::max(1.0f, 1.0f * s));
                ui.textTracked(x + 114.0f * s, y0 + 53.0f * s, chip, pal::textDim, fpx(ui, 13.0f, s), 1.0f * s);
            }

            float itemNameSc = fpx(ui, 34.0f, s);
            if (ui.textDWidth(rv.name, itemNameSc) > cardW - 52.0f * s) itemNameSc = fpx(ui, 29.0f, s);
            ui.textD(x + 26.0f * s, y0 + 104.0f * s, rv.name, pal::textHero, itemNameSc);
            // Weapon cards carry a stat block below the blurb, so wrap the flavour at
            // a slightly smaller size (one line in the narrow side cards) and clamp it;
            // passive cards have room for two lines. The stat/owned block is then placed
            // below the ACTUAL wrapped text instead of a fixed offset, so it can never
            // overlap the description (the old bug: "splashes on impact" over "DAMAGE").
            const float blurbPx = rv.isWeapon ? 17.0f : 20.0f;
            const int blurbLines = sel ? 2 : 1;
            const float blurbBottom = textDWrapped(ui, x + 26.0f * s, y0 + 150.0f * s, cardW - 52.0f * s,
                                                   rv.blurb, pal::textMid, fpx(ui, blurbPx, s), 5.0f * s, blurbLines);

            if (rv.isWeapon) {
                if (const WeaponDef* wd = build_.findWeapon(rv.rawId)) {
                    const int fr10 = static_cast<int>(std::lround(wd->fireRate * 10.0f));
                    float wy = std::max(y0 + 182.0f * s, blurbBottom + 6.0f * s);
                    auto wrow = [&](const std::string& lab, const std::string& val) {
                        ui.text(x + 26.0f * s, wy, lab, rgb(126, 138, 153), fpx(ui, 17.0f, s));
                        ui.textRight(x + cardW - 26.0f * s, wy, val, pal::textHi, fpx(ui, 17.0f, s));
                        wy += 24.0f * s;
                    };
                    wrow("DAMAGE", std::to_string(static_cast<int>(std::lround(wd->damage))));
                    wrow("FIRE RATE", std::to_string(fr10 / 10) + "." + std::to_string(fr10 % 10) + " / s");
                    wrow("MAGAZINE", std::to_string(wd->magazine));
                }
            } else {
                const int owned = build_.stacks(rv.rawId);
                const std::string line = owned > 0
                    ? ("Already owned x" + std::to_string(owned) + ". Taking it stacks.")
                    : "New passive.";
                ui.textD(x + 26.0f * s, std::max(y0 + 196.0f * s, blurbBottom + 6.0f * s),
                         line, rgb(126, 138, 153), fpx(ui, 17.0f, s));
            }

            const float setY = y0 + cardH - 88.0f * s;
            if (hasAff) {
                const int have = build_.stats().affinityCount[static_cast<int>(rv.affinity)];
                const int after = have + 1;
                ui.rect(x, setY - 14.0f * s, cardW, 1.0f, pal::lineSoft);
                for (int p = 0; p < 5; ++p) {
                    const float ppx = x + 30.0f * s + static_cast<float>(p) * 16.0f * s, ppy = setY + 17.0f * s;
                    const bool thr = (p == 2 || p == 4);   // 3rd + 5th are the bonus breakpoints
                    if (p < after) diamond(ui, ppx, ppy, thr ? 5.0f * s : 3.2f * s, av.col);
                    else diamondOutline(ui, ppx, ppy, thr ? 5.0f * s : 3.2f * s, withAlpha(av.col, 130), std::max(1.0f, 1.0f * s));
                }
                const int nextT = after <= 3 ? 3 : 5;
                textDWrapped(ui, x + 118.0f * s, setY - 1.0f * s, cardW - 144.0f * s,
                             affinityProgressLine(build_.stats(), rv.affinity),
                             av.col, fpx(ui, 16.0f, s), 2.0f * s, sel ? 2 : 1);
                ui.textRight(x + cardW - 26.0f * s, setY + 29.0f * s,
                             nextT == 3 ? "NEXT: 3-SET" : "NEXT: 5-SET",
                             pal::good, fpx(ui, 13.0f, s));
            } else if (rv.isWeapon) {
                ui.rect(x, setY - 14.0f * s, cardW, 1.0f, pal::lineSoft);
                ui.text(x + 26.0f * s, setY + 2.0f * s, "adds to loadout",
                        pal::textMid, fpx(ui, 16.0f, s));
                ui.text(x + 26.0f * s, setY + 27.0f * s, "[Q] swap weapon",
                        pal::textDim, fpx(ui, 13.0f, s));
                ui.textRight(x + cardW - 26.0f * s, setY + 15.0f * s, "forge for aspects",
                             pal::amber, fpx(ui, 16.0f, s));
            }
            if (hasAff && build_.stats().affinityCount[static_cast<int>(rv.affinity)] >= 2) {
                const int have = build_.stats().affinityCount[static_cast<int>(rv.affinity)];
                const char* lbl = (have + 1 == 3 || have + 1 == 5) ? "SET BONUS!" : "SYNERGY";
                chamferFill(ui, x + cardW - 136.0f * s, y0 + 18.0f * s, 110.0f * s, 22.0f * s, 5.0f * s, pal::synergyHi);
                ui.textTracked(x + cardW - 126.0f * s, y0 + 23.0f * s, lbl, pal::navy, fpx(ui, 12.0f, s), 1.0f * s);
            }

            const float footerY = y0 + cardH - 38.0f * s;
            ui.rect(x, footerY, cardW, 1.0f, sel ? withAlpha(pal::accent, 70) : pal::lineSoft);
            if (sel) ui.rect(x, footerY + 1.0f, cardW, cardH - (footerY - y0) - 1.0f, rgb(13, 32, 40));
            ui.textCentered(x + cardW * 0.5f, footerY + 10.0f * s,
                            "[" + std::to_string(i + 1) + "] " + (sel ? "SELECTED" : "SELECT"),
                            sel ? pal::accent : pal::textFaint, fpx(ui, 18.0f, s));
        }

        // Full-width footer strip from the guide: build identity, stat deltas, controls.
        const float stripX = 80.0f * s, stripH = 54.0f * s, stripW = static_cast<float>(w) - stripX * 2.0f;
        const float spx = stripX, spy = static_cast<float>(h) - 42.0f * s - stripH;
        chamferFill(ui, spx, spy, stripW, stripH, 10.0f * s, pal::surface0);
        chamferOutline(ui, spx, spy, stripW, stripH, 10.0f * s, pal::border, std::max(1.0f, 1.0f * s));
        ui.textTracked(spx + 30.0f * s, spy + 17.0f * s, "YOUR BUILD", pal::textFaint, fpx(ui, 16.0f, s), 2.0f * s);
        float pdx = spx + 184.0f * s; int pdn = 0;
        for (const auto& kv : build_.inventory()) {
            const ItemDef* d = build_.find(kv.first);
            const uint32_t pc = !d ? pal::tierCommon : (d->tier == ItemTier::Legendary ? pal::tierLegendary : d->tier == ItemTier::Rare ? pal::tierRare : (d->tier == ItemTier::Uncommon ? pal::tierUncommon : pal::tierCommon));
            for (int k = 0; k < kv.second && pdn < 16; ++k) { diamond(ui, pdx + 5.0f * s, spy + 27.0f * s, 4.0f * s, pc); pdx += 13.0f * s; ++pdn; }
        }
        const BuildStats& bs = build_.stats();
        const int dmg = static_cast<int>(std::lround((bs.damageMult - 1.0f) * 100.0f));
        const int crit = static_cast<int>(std::lround(bs.critChance * 100.0f));
        const int fr = static_cast<int>(std::lround((bs.fireRateMult - 1.0f) * 100.0f));
        auto sgn = [](int v) { return (v >= 0 ? "+" : "") + std::to_string(v); };
        const float statX = spx + 460.0f * s;
        ui.rect(statX - 28.0f * s, spy + 14.0f * s, 1.0f, 26.0f * s, pal::lineSoft);
        ui.text(statX, spy + 16.0f * s, "DMG " + sgn(dmg) + "%", pal::textHi, fpx(ui, 20.0f, s));
        ui.text(statX + 150.0f * s, spy + 16.0f * s, "CRIT " + std::to_string(crit) + "%", pal::textHi, fpx(ui, 20.0f, s));
        ui.text(statX + 305.0f * s, spy + 16.0f * s, "FIRE " + sgn(fr) + "%", pal::textHi, fpx(ui, 20.0f, s));
        ui.text(statX + 480.0f * s, spy + 16.0f * s, "SHLD +" + std::to_string(bs.maxShieldBonus), pal::textHi, fpx(ui, 20.0f, s));
        ui.textRight(spx + stripW - 30.0f * s, spy + 18.0f * s,
                     "1-3 select . ENTER commit . [C] systems", pal::textFaint, fpx(ui, 16.0f, s));
    }
    if (phase_ == RunPhase::RunOver) {
        // Faithful to the UI Overhaul doc: header (ion slash + RUN ENDED + FELLED BY box), a
        // single bordered stats grid, a BUILD RECAP strip, the META + next-unlock band, an
        // INSIGHT line, and two CTAs - filling the frame.
        darkBoardBg(rgb(20, 13, 18), rgb(9, 11, 15));
        const bool won = runWon_;
        const uint32_t ink2b = rgb(126, 138, 153), costDim = rgb(90, 101, 117);
        const float pad = 120.0f * s, topY = 64.0f * s, gridW = static_cast<float>(w) - pad * 2.0f;
        const float gx = pad;

        // ---- Header ----
        ui.rect(gx, topY, 12.0f * s, 76.0f * s, won ? pal::accent : pal::ionWhite);   // slash
        glowTextD(ui, gx + 28.0f * s, topY - 6.0f * s, won ? "VICTORY" : "RUN ENDED",
                  withAlpha(won ? pal::accentGlow : pal::ionWhite, 60), pal::textHero, fpx(ui, 64.0f, s));
        const std::string cause = won
            ? std::string("All sectors cleared")
            : ("Felled at " + std::string(biomeName(currentBiome_)) + " . Sector " + std::to_string(run_.sector() + 1) +
               ", Room " + std::to_string(run_.roomInSector()));
        ui.text(gx + 28.0f * s, topY + 52.0f * s, cause, pal::textMid, fpx(ui, 20.0f, s));
        if (!won && !felledBy_.empty()) {   // FELLED BY box, top-right
            const float fbW = std::max(260.0f * s, ui.textWidth(felledBy_, fpx(ui, 26.0f, s)) + 48.0f * s), fbH = 60.0f * s;
            const float fbx = gx + gridW - fbW, fby = topY + 2.0f * s;
            chamferFill(ui, fbx, fby, fbW, fbH, 10.0f * s, rgb(26, 15, 20));
            chamferOutline(ui, fbx, fby, fbW, fbH, 10.0f * s, withAlpha(pal::ionWhite, 70), std::max(1.0f, 1.0f * s));
            ui.textTracked(fbx + 22.0f * s, fby + 12.0f * s, "FELLED BY", rgb(207, 226, 255), fpx(ui, 15.0f, s), 2.0f * s);
            ui.textTracked(fbx + 22.0f * s, fby + 32.0f * s, felledBy_, pal::ionWhite, fpx(ui, 26.0f, s), 0.0f);
        }

        // ---- Stats grid (single bordered box, 3 x 2) ----
        const float gy = topY + 112.0f * s;
        const float cellW = gridW / 3.0f, r1H = 92.0f * s, r2H = 72.0f * s, gridH = r1H + r2H;
        ui.gradientV(gx, gy, gridW, gridH, pal::surface0, pal::surface0, 2);
        ui.rectOutline(gx, gy, gridW, gridH, pal::border, std::max(1.0f, 1.2f * s));
        struct Cell { std::string label; std::string value; uint32_t col; float vSz; };
        const Cell cells[6] = {
            { "SCORE",         std::to_string(score_),     pal::textHi,  46.0f },
            { "BEST SCORE",    std::to_string(bestScore_), pal::gold,    46.0f },
            { "FURTHEST",      "S" + std::to_string(run_.sector() + 1) + " R" + std::to_string(run_.roomInSector()), pal::textHi, 46.0f },
            { "ROOMS CLEARED", std::to_string(run_.roomIndex()),    pal::textHi,  34.0f },
            { "BUILD ITEMS",   std::to_string(build_.totalItems()), pal::textHi,  34.0f },
            { "HEAT",          std::to_string(std::min(meta_.heat(), static_cast<int>(runContracts().size()))), pal::affPyro, 34.0f },
        };
        for (int i = 0; i < 6; ++i) {
            const int r = i / 3, c = i % 3;
            const float ccx = gx + static_cast<float>(c) * cellW, ccy = gy + (r == 0 ? 0.0f : r1H);
            const float chh = r == 0 ? r1H : r2H;
            if (c > 0) ui.rect(ccx, gy + 14.0f * s, 1.0f, gridH - 28.0f * s, rgb(28, 36, 48));
            if (r > 0) ui.rect(gx + 14.0f * s, gy + r1H, gridW - 28.0f * s, 1.0f, rgb(28, 36, 48));
            ui.textTracked(ccx + 28.0f * s, ccy + (r == 0 ? 22.0f * s : 16.0f * s), cells[i].label, ink2b, fpx(ui, r == 0 ? 16.0f : 15.0f, s), 2.0f * s);
            ui.text(ccx + 28.0f * s, ccy + (r == 0 ? 44.0f * s : 36.0f * s), cells[i].value, cells[i].col, fpx(ui, cells[i].vSz, s));
            (void)chh;
        }

        // ---- BUILD RECAP ----
        const float brY = gy + gridH + 20.0f * s, brH = 96.0f * s;
        chamferPanel(ui, gx, brY, gridW, brH, 12.0f * s, pal::surface0, pal::border, std::max(1.0f, 1.2f * s), true);
        ui.textTracked(gx + 30.0f * s, brY + 16.0f * s, "BUILD RECAP", pal::textDim, fpx(ui, 16.0f, s), 3.0f * s);
        {
            const float ry = brY + 44.0f * s;
            float rx = gx + 30.0f * s;
            ui.textTracked(rx, ry, "WEAPON", ink2b, fpx(ui, 14.0f, s), 1.2f * s);
            std::string wv = activeWeaponDef().name;
            const WeaponAspect* asp = activeAspect();
            ui.text(rx, ry + 18.0f * s, wv, pal::textHi, fpx(ui, 24.0f, s));
            if (asp) ui.text(rx + ui.textWidth(wv + " ", fpx(ui, 24.0f, s)), ry + 20.0f * s, "/ ASPECT: " + std::string(asp->name), pal::affPyro, fpx(ui, 18.0f, s));
            rx += 380.0f * s; ui.rect(rx - 20.0f * s, ry, 1.0f, 40.0f * s, pal::border);
            ui.textTracked(rx, ry, "SETS", ink2b, fpx(ui, 14.0f, s), 1.2f * s);
            const BuildStats& bs = build_.stats();
            std::string setsStr; uint32_t setCol = pal::textMid;
            for (int a = 1; a < static_cast<int>(Affinity::Count); ++a) {
                if (bs.affinityCount[a] >= 2) {
                    const AffVis av = affVis(static_cast<Affinity>(a));
                    if (!setsStr.empty()) setsStr += " . ";
                    setsStr += std::string(av.name) + " " + std::to_string(std::min(bs.affinityCount[a], 5)) + "/5";
                    setCol = av.col;
                }
            }
            ui.text(rx, ry + 18.0f * s, setsStr.empty() ? "no sets yet" : setsStr, setsStr.empty() ? costDim : setCol, fpx(ui, 22.0f, s));
            rx += 420.0f * s; ui.rect(rx - 20.0f * s, ry, 1.0f, 40.0f * s, pal::border);
            ui.textTracked(rx, ry, "PEAK PULSE", ink2b, fpx(ui, 14.0f, s), 1.2f * s);
            ui.text(rx, ry + 18.0f * s, std::to_string(static_cast<int>(std::lround(clamp(runPulsePeak_, 0.0f, 1.0f) * 100.0f))), pal::ionWhite, fpx(ui, 24.0f, s));
        }

        // ---- META + next-unlock band ----
        const float mbY = brY + brH + 20.0f * s, mbH = 150.0f * s;
        chamferPanel(ui, gx, mbY, gridW, mbH, 12.0f * s, rgb(22, 15, 8), withAlpha(pal::amber, 120), std::max(1.0f, 1.2f * s), true);
        ui.textTracked(gx + 30.0f * s, mbY + 18.0f * s, "META EARNED", pal::amber, fpx(ui, 15.0f, s), 2.0f * s);
        const float payoutMult = std::max(0.01f, mods_.metaPayoutMult);
        const int base = static_cast<int>(std::lround(static_cast<float>(lastPayout_) / payoutMult));
        const int mm = static_cast<int>(std::lround(payoutMult * 100.0f));
        const int nActive = std::min(meta_.heat(), static_cast<int>(runContracts().size()));
        ui.text(gx + 30.0f * s, mbY + 40.0f * s,
                "Base " + std::to_string(base) + " . Heat " + std::to_string(nActive) +
                " payout x" + std::to_string(mm / 100) + "." + (mm % 100 < 10 ? std::string("0") : std::string()) + std::to_string(mm % 100),
                ink2b, fpx(ui, 18.0f, s));
        glowText(ui, gx + gridW - 30.0f * s - ui.textWidth("+" + std::to_string(lastPayout_), fpx(ui, 60.0f, s)), mbY + 18.0f * s,
                 "+" + std::to_string(lastPayout_), withAlpha(pal::goldGlow, 80), pal::gold, fpx(ui, 60.0f, s), 0.0f);
        // next-unlock progress bar
        {
            const UnlockDef* next = nullptr;
            for (const UnlockDef& u : meta_.unlockables())
                if (!meta_.isUnlocked(u.id) && (!next || u.cost < next->cost)) next = &u;
            const float by = mbY + mbH - 50.0f * s;
            if (next) {
                ui.text(gx + 30.0f * s, by - 2.0f * s, "toward " + next->name + " unlock", ink2b, fpx(ui, 18.0f, s));
                ui.textRight(gx + gridW - 30.0f * s, by - 2.0f * s, std::to_string(meta_.currency()) + " / " + std::to_string(next->cost), pal::gold, fpx(ui, 18.0f, s));
                const float bw2 = gridW - 60.0f * s, frac = clamp(static_cast<float>(meta_.currency()) / std::max(1.0f, static_cast<float>(next->cost)), 0.0f, 1.0f);
                ui.rect(gx + 30.0f * s, by + 20.0f * s, bw2, 8.0f * s, rgb(42, 29, 12));
                ui.rect(gx + 30.0f * s, by + 20.0f * s, bw2 * frac, 8.0f * s, pal::gold);
            } else {
                // No next unlock: center the single line in the band instead of bottom-anchoring
                // it, so the META band does not read as half-empty.
                ui.text(gx + 30.0f * s, mbY + mbH * 0.5f + 4.0f * s, "All content unlocked - bank meta toward the Mirror.", costDim, fpx(ui, 18.0f, s));
            }
        }

        // ---- INSIGHT (one actionable line) ----
        {
            std::string insight;
            if (!won && run_.sector() == 0) insight = "Dash through packs and keep moving - momentum is survival.";
            else if (runPulsePeak_ < 0.50f) insight = "Chain kills to hold a higher Pulse - it amplifies your whole build.";
            else if (build_.stats().topAffinity != 0 && build_.stats().affinityCount[build_.stats().topAffinity] < 3)
                insight = "Commit to one affinity - 3 items unlock an amplifier, 5 a signature.";
            else if (build_.totalItems() < 4) insight = "Take more rewards - a stacked build snowballs fast.";
            else insight = won ? "Clean run. Raise the Heat for a richer payout." : "Read the boss telegraph - the EXPOSED window is your damage.";
            const float iy = h - 116.0f * s;
            ui.textTracked(gx, iy, "INSIGHT", pal::accent, fpx(ui, 18.0f, s), 2.2f * s);
            ui.text(gx + ui.textTrackedWidth("INSIGHT", fpx(ui, 18.0f, s), 2.2f * s) + 16.0f * s, iy, insight, pal::textMid, fpx(ui, 18.0f, s));
        }

        // ---- CTAs: RUN IT BACK (cyan, dominant) + Return to hub ----
        const float ctaY = h - 64.0f * s, ctaH = 56.0f * s, hubW = 380.0f * s;
        const float runW = gridW - hubW - 20.0f * s;
        chamferFill(ui, gx, ctaY, runW, ctaH, 12.0f * s, rgb(12, 28, 34));
        chamferOutline(ui, gx, ctaY, runW, ctaH, 12.0f * s, pal::accent, std::max(1.5f, 1.5f * s));
        glowCentered(ui, gx + runW * 0.5f - 30.0f * s, ctaY + 12.0f * s, "RUN IT BACK", withAlpha(pal::accentGlow, 80), pal::textHero, fpx(ui, 32.0f, s), 0.0f);
        ui.text(gx + runW * 0.5f + ui.textWidth("RUN IT BACK", fpx(ui, 32.0f, s)) * 0.5f - 18.0f * s, ctaY + 18.0f * s, "[SPACE]", rgb(90, 160, 200), fpx(ui, 16.0f, s));
        chamferFill(ui, gx + runW + 20.0f * s, ctaY, hubW, ctaH, 12.0f * s, pal::surface2);
        chamferOutline(ui, gx + runW + 20.0f * s, ctaY, hubW, ctaH, 12.0f * s, withAlpha(pal::textDim, 200), std::max(1.0f, 1.2f * s));
        ui.textCentered(gx + runW + 20.0f * s + hubW * 0.5f, ctaY + 16.0f * s, "Return to hub", pal::textHi, fpx(ui, 24.0f, s));
    }
    if (phase_ == RunPhase::Hub) {
        auto multStr = [](float v) {
            const int hh = static_cast<int>(std::lround(v * 100.0f));
            return "x" + std::to_string(hh / 100) + "." + (hh % 100 < 10 ? std::string("0") : std::string()) + std::to_string(hh % 100);
        };
        auto archName = [](WeaponArchetype a) -> std::string {
            switch (a) {
                case WeaponArchetype::HitscanAuto: return "HITSCAN";
                case WeaponArchetype::Burst:       return "BURST";
                case WeaponArchetype::Spread:      return "SPREAD";
                case WeaponArchetype::Projectile:  return "PROJECTILE";
                case WeaponArchetype::Beam:        return "BEAM";
            }
            return "";
        };
        const std::vector<UnlockDef>& hubUnlockables = meta_.unlockables();
        const int hubCatCount = std::min(9, static_cast<int>(hubUnlockables.size()));
        const int hubMirrorStart = hubCatCount;
        const int hubLoadoutPrevFocus = hubMirrorStart + Meta::MirrorCount;
        const int hubLoadoutNextFocus = hubLoadoutPrevFocus + 1;
        const int hubHeatDownFocus = hubLoadoutNextFocus + 1;
        const int hubHeatUpFocus = hubHeatDownFocus + 1;
        const int hubManualFocus = hubHeatUpFocus + 1;
        const int hubStartFocus = hubManualFocus + 1;
        auto hubFocused = [&](int focus) { return cardCursor_ == focus; };

        // Design-reference Hub: 48x56 padding, 386 / flex / 430 columns, and a full-width launch bar.
        {
            const float W = static_cast<float>(w), H = static_cast<float>(h);
            auto commaNew = [](int v) {
                std::string in = std::to_string(v < 0 ? -v : v), out; int c = 0;
                for (int i = static_cast<int>(in.size()) - 1; i >= 0; --i) {
                    out.insert(out.begin(), in[static_cast<size_t>(i)]);
                    if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
                }
                return (v < 0 ? "-" : "") + out;
            };
            auto tierColor = [](ItemTier t) {
                switch (t) {
                    case ItemTier::Legendary: return pal::tierLegendary;
                    case ItemTier::Rare:      return pal::tierRare;
                    case ItemTier::Uncommon:  return pal::tierUncommon;
                    default:                  return pal::tierCommon;
                }
            };
            auto statBar = [&](float x, float y, float ww, const std::string& label,
                               const std::string& value, float frac) {
                ui.textTracked(x, y, label, rgb(138, 160, 173), fpx(ui, 13.0f, s), 2.6f * s);
                ui.textDRight(x + ww, y - 3.0f * s, value, pal::textHi, fpx(ui, 18.0f, s));
                ui.rect(x, y + 28.0f * s, ww, 4.0f * s, rgba(90, 170, 200, 36));
                gradientH(ui, x, y + 28.0f * s, ww * clamp(frac, 0.0f, 1.0f), 4.0f * s,
                          pal::accent, rgb(10, 143, 176), 12);
            };
            auto panelTitle = [&](float x, float y, const std::string& title, uint32_t col = rgb(93, 210, 236)) {
                ui.rect(x, y + 5.0f * s, 7.0f * s, 7.0f * s, pal::accent);
                ui.textTracked(x + 17.0f * s, y, title, col, fpx(ui, 13.0f, s), 3.6f * s);
            };

            ui.rect(0.0f, 0.0f, W, H, rgb(5, 8, 14));
            ui.gradientV(0.0f, 0.0f, W, H, rgb(7, 13, 22), rgb(5, 8, 14), 60);
            for (int xi = 0; xi <= 34; ++xi) {
                const float x = static_cast<float>(xi) * 56.0f * s;
                ui.rect(x, 0.0f, 1.0f, H, rgba(56, 200, 232, 10));
            }
            for (int yi = 0; yi <= 20; ++yi) {
                const float y = static_cast<float>(yi) * 56.0f * s;
                const uint8_t a = static_cast<uint8_t>(12.0f * clamp(1.0f - y / std::max(1.0f, H), 0.0f, 1.0f));
                ui.rect(0.0f, y, W, 1.0f, rgba(56, 200, 232, a));
            }

            const float padX = 56.0f * s, padY = 48.0f * s, gap = 24.0f * s;
            const float frameW = W - padX * 2.0f;
            const float headerY = padY;
            const float bodyY = 160.0f * s;
            const float startH = 128.0f * s, startY = H - padY - startH;
            const float bodyH = startY - bodyY - gap;
            const float col1W = 386.0f * s, col3W = 430.0f * s;
            const float col2W = frameW - col1W - col3W - gap * 2.0f;
            const float col1X = padX, col2X = col1X + col1W + gap, col3X = col2X + col2W + gap;
            const float cutPanel = 16.0f * s;
            const uint32_t panelFill = rgba(14, 24, 34, 218);
            const uint32_t panelBorder = rgba(90, 170, 200, 42);
            const uint32_t faint = rgb(72, 96, 110);
            const uint32_t label = rgb(111, 138, 153);
            const uint32_t muted = rgb(169, 188, 199);
            const uint32_t idleRow = rgba(13, 22, 30, 128);
            const uint32_t activeRow = rgba(13, 40, 52, 128);

            ui.textTracked(padX, headerY, "BETWEEN RUNS", faint, fpx(ui, 13.0f, s), 4.9f * s);
            ui.textD(padX, headerY + 24.0f * s, "THE HUB", pal::textHi, fpx(ui, 52.0f, s));
            ui.textTracked(padX, headerY + 86.0f * s, "ARROWS / WASD MOVE FOCUS . SPACE SELECTS",
                           rgb(86, 110, 124), fpx(ui, 12.0f, s), 2.2f * s);
            const std::string metaVal = commaNew(meta_.currency());
            const float metaX = W - padX - 224.0f * s - 92.0f * s - 138.0f * s;
            textTrackedRight(ui, metaX + 138.0f * s, headerY + 3.0f * s, "META", faint, fpx(ui, 12.0f, s), 3.6f * s);
            glowTextD(ui, metaX + 138.0f * s - ui.textDWidth(metaVal, fpx(ui, 46.0f, s)), headerY + 22.0f * s,
                      metaVal, withAlpha(pal::accentGlow, 70), pal::accent, fpx(ui, 46.0f, s));
            const float manualW = 224.0f * s, manualH = 48.0f * s;
            const float manualX = W - padX - manualW, manualY = headerY + 17.0f * s;
            if (hubFocused(hubManualFocus)) menuFocusHalo(ui, manualX, manualY, manualW, manualH, 14.0f * s, pal::accentGlow, s);
            chamferFill(ui, manualX, manualY, manualW, manualH, 14.0f * s,
                        hubFocused(hubManualFocus) ? rgba(13, 56, 72, 210) : rgba(13, 30, 40, 178));
            chamferOutline(ui, manualX, manualY, manualW, manualH, 14.0f * s,
                           hubFocused(hubManualFocus) ? pal::accent : rgba(70, 140, 165, 102), std::max(1.0f, 1.0f * s));
            if (hubFocused(hubManualFocus)) focusRing(ui, manualX, manualY, manualW, manualH, 14.0f * s, s);
            ui.textTracked(manualX + 24.0f * s, manualY + 17.0f * s, "FIELD MANUAL >",
                           rgb(111, 232, 255), fpx(ui, 13.0f, s), 2.8f * s);

            // Column 1: Starting loadout.
            notchBRPanel(ui, col1X, bodyY, col1W, bodyH, cutPanel, panelFill, panelBorder, std::max(1.0f, 1.0f * s));
            {
                const float ix = col1X + 26.0f * s, iw = col1W - 52.0f * s;
                panelTitle(ix, bodyY + 26.0f * s, "STARTING LOADOUT");
                const WeaponDef* sw = build_.findWeapon(meta_.startingWeapon());
                const std::string swName = sw ? sw->name : meta_.startingWeapon();
                const float cardY = bodyY + 66.0f * s, cardH = 118.0f * s;
                chamferFill(ui, ix, cardY, iw, cardH, 14.0f * s, rgba(13, 52, 68, 140));
                chamferOutline(ui, ix, cardY, iw, cardH, 14.0f * s, rgba(60, 200, 235, 82), std::max(1.0f, 1.0f * s));
                pulseWave(ui, ix + iw - 206.0f * s, cardY + 43.0f * s, 180.0f * s, 24.0f * s,
                          1.0f, rgba(43, 214, 245, 36), rgba(43, 214, 245, 90), std::max(1.5f, 2.0f * s));
                ui.textD(ix + 22.0f * s, cardY + 24.0f * s, swName, pal::textHero, fpx(ui, 38.0f, s));
                if (sw) ui.textTracked(ix + 22.0f * s, cardY + 72.0f * s,
                    archName(sw->archetype) + " / " + (sw->automatic ? "AUTO" : "SEMI"), label, fpx(ui, 12.0f, s), 2.6f * s);

                if (sw) {
                    const int fr10 = static_cast<int>(std::lround(sw->fireRate * 10.0f));
                    statBar(ix, bodyY + 208.0f * s, iw, "DAMAGE", std::to_string(static_cast<int>(std::lround(sw->damage))), 0.55f);
                    statBar(ix, bodyY + 268.0f * s, iw, "FIRE RATE", std::to_string(fr10 / 10) + "." + std::to_string(fr10 % 10) + " / s", 0.70f);
                    statBar(ix, bodyY + 328.0f * s, iw, "MAGAZINE", std::to_string(sw->magazine), 0.40f);
                }

                const float readyY = bodyY + bodyH - 168.0f * s;
                ui.rect(ix, readyY, iw, 1.0f, rgba(90, 170, 200, 36));
                ui.textTracked(ix, readyY + 24.0f * s, "RUN READINESS", faint, fpx(ui, 11.0f, s), 3.0f * s);
                ui.textD(ix, readyY + 50.0f * s, "FOUNDRY", pal::textHi, fpx(ui, 24.0f, s));
                textTrackedRight(ui, col1X + col1W - 26.0f * s, readyY + 58.0f * s, "SECTOR 1",
                                 rgb(111, 232, 255), fpx(ui, 13.0f, s), 2.4f * s);
                const float btnY = readyY + 88.0f * s, btnH = 38.0f * s, btnGap = 12.0f * s;
                const float btnW = (iw - btnGap) * 0.5f;
                const bool canCycle = meta_.starterOptions().size() > 1;
                auto loadBtn = [&](float bx, int focus, const char* txt) {
                    const bool hot = hubFocused(focus);
                    ui.rect(bx, btnY, btnW, btnH, hot ? rgba(13, 56, 72, 165) : rgba(13, 22, 30, 154));
                    ui.rectOutline(bx, btnY, btnW, btnH, hot ? pal::accent : rgba(90, 170, 200, 56), std::max(1.0f, 1.0f * s));
                    if (hot) focusRing(ui, bx, btnY, btnW, btnH, 8.0f * s, s);
                    ui.textTracked(bx + (btnW - ui.textTrackedWidth(txt, fpx(ui, 12.0f, s), 2.4f * s)) * 0.5f,
                                   btnY + 12.0f * s, txt, canCycle ? (hot ? pal::accent : rgb(138, 160, 173)) : faint,
                                   fpx(ui, 12.0f, s), 2.4f * s);
                };
                loadBtn(ix, hubLoadoutPrevFocus, "PREV");
                loadBtn(ix + btnW + btnGap, hubLoadoutNextFocus, "NEXT");
                ui.textTracked(ix, readyY + 140.0f * s, std::to_string(static_cast<int>(meta_.starterOptions().size())) + " OF 6 WEAPONS UNLOCKED",
                               rgb(63, 86, 99), fpx(ui, 11.0f, s), 2.0f * s);
            }

            // Column 2: Mirror + contracts.
            const float mirrorH = 336.0f * s;
            const float contractsY = bodyY + mirrorH + gap;
            const float contractsH = bodyH - mirrorH - gap;
            notchBRPanel(ui, col2X, bodyY, col2W, mirrorH, cutPanel, panelFill, panelBorder, std::max(1.0f, 1.0f * s));
            {
                const float ix = col2X + 30.0f * s, iw = col2W - 60.0f * s;
                panelTitle(ix, bodyY + 26.0f * s, "THE MIRROR");
                textTrackedRight(ui, col2X + col2W - 30.0f * s, bodyY + 27.0f * s, "CAPPED . SKILL FIRST",
                                 faint, fpx(ui, 11.0f, s), 2.2f * s);
                static const char* const mEff[Meta::MirrorCount] = { "+6 HP", "+8 SHLD", "+Pulse", "+8 scrap", "tier bias", "+6% chg" };
                const float rowY = bodyY + 68.0f * s, rowH = 33.0f * s;
                for (int i = 0; i < Meta::MirrorCount; ++i) {
                    const int lvl = meta_.mirrorLevel(i);
                    const int shownMax = 5;
                    const int cost = meta_.mirrorCost(i);
                    const bool maxed = cost < 0;
                    const bool afford = !maxed && meta_.currency() >= cost;
                    const int focus = hubMirrorStart + i;
                    const bool hot = hubFocused(focus);
                    const float y = rowY + static_cast<float>(i) * (rowH + 7.0f * s);
                    if (hot) {
                        ui.rect(ix - 12.0f * s, y - 5.0f * s, iw + 24.0f * s, rowH + 2.0f * s, rgba(13, 56, 72, 72));
                        focusRing(ui, ix - 12.0f * s, y - 5.0f * s, iw + 24.0f * s, rowH + 2.0f * s, 6.0f * s, s);
                    }
                    ui.textD(ix, y, meta_.mirrorName(i), pal::textHi, fpx(ui, 18.0f, s));
                    const float pipX = ix + 148.0f * s;
                    for (int p = 0; p < shownMax; ++p) {
                        const float px = pipX + static_cast<float>(p) * 24.0f * s;
                        const bool on = p < lvl;
                        ui.rect(px, y + 6.0f * s, 18.0f * s, 11.0f * s,
                                on ? (i == Meta::MirrorMomentum ? rgb(157, 123, 255) : pal::accent) : rgba(90, 170, 200, 46));
                    }
                    textTrackedRight(ui, col2X + col2W - 78.0f * s, y + 3.0f * s, mEff[i],
                                     i == Meta::MirrorMomentum ? rgb(182, 163, 255) : muted, fpx(ui, 13.0f, s), 1.8f * s);
                    const std::string c = maxed ? "MAX" : std::to_string(cost);
                    ui.textDRight(col2X + col2W - 30.0f * s, y, c,
                                  maxed ? pal::tierLegendary : (afford ? pal::textHi : faint), fpx(ui, 17.0f, s));
                }
                ui.textTracked(ix, bodyY + mirrorH - 36.0f * s, "PERMANENT UPGRADES",
                               rgb(63, 86, 99), fpx(ui, 11.0f, s), 2.5f * s);
            }

            notchBRPanel(ui, col2X, contractsY, col2W, contractsH, cutPanel,
                         rgba(28, 20, 12, 154), rgba(150, 110, 55, 86), std::max(1.0f, 1.0f * s));
            {
                const std::vector<RunContract>& contracts = runContracts();
                const int nC = static_cast<int>(contracts.size());
                const int heat = std::min(meta_.heat(), nC);
                const unsigned mask = contractMaskForHeat(heat);
                const std::string head = "HEAT " + std::to_string(heat) + " / " + std::to_string(nC) + " . RUN CONTRACTS";
                const float ix = col2X + 30.0f * s, iw = col2W - 60.0f * s;
                ui.textTracked(ix, contractsY + 24.0f * s, head, pal::amber, fpx(ui, 13.0f, s), 3.1f * s);
                ui.text(ix + ui.textTrackedWidth(head, fpx(ui, 13.0f, s), 3.1f * s) + 18.0f * s,
                        contractsY + 24.0f * s, "payout " + multStr(contractPayout(mask)), rgb(157, 131, 88), fpx(ui, 13.0f, s));
                const float heatY = contractsY + 17.0f * s, heatW = 78.0f * s, heatH = 30.0f * s;
                auto heatBtn = [&](float bx, int focus, const char* txt) {
                    const bool hot = hubFocused(focus);
                    ui.rect(bx, heatY, heatW, heatH, hot ? rgba(48, 33, 16, 190) : rgba(18, 13, 10, 140));
                    ui.rectOutline(bx, heatY, heatW, heatH, hot ? pal::amber : rgba(150, 110, 55, 102), std::max(1.0f, 1.0f * s));
                    if (hot) focusRing(ui, bx, heatY, heatW, heatH, 5.0f * s, s);
                    ui.textTracked(bx + 15.0f * s, heatY + 9.0f * s, txt, rgb(199, 154, 94), fpx(ui, 12.0f, s), 2.1f * s);
                };
                heatBtn(col2X + col2W - 30.0f * s - heatW * 2.0f - 10.0f * s, hubHeatDownFocus, "LESS");
                heatBtn(col2X + col2W - 30.0f * s - heatW, hubHeatUpFocus, "MORE");

                const int shown = std::min(heat + 1, nC);
                const float chipGap = 14.0f * s;
                const float chipY = contractsY + 68.0f * s;
                const float chipH = std::max(74.0f * s, contractsH - 98.0f * s);
                const float chipW = shown > 0 ? (iw - static_cast<float>(shown - 1) * chipGap) / static_cast<float>(shown) : iw;
                for (int i = 0; i < shown; ++i) {
                    const RunContract& c = contracts[static_cast<size_t>(i)];
                    const bool active = i < heat;
                    const bool primary = i == 0;
                    const float x = ix + static_cast<float>(i) * (chipW + chipGap);
                    const uint32_t fill = active ? rgba(48, 33, 16, primary ? 154 : 112) : rgba(28, 22, 14, 90);
                    ui.rect(x, chipY, chipW, chipH, fill);
                    ui.rectOutline(x, chipY, chipW, chipH,
                                   primary ? rgba(255, 178, 77, 178) : (active ? rgba(255, 178, 77, 76) : rgba(120, 95, 55, 76)),
                                   std::max(1.0f, 1.0f * s));
                    if (primary) {
                        ui.rect(x, chipY, chipW, 3.0f * s, pal::amber);
                        ui.rect(x + 1.0f * s, chipY + 1.0f * s, chipW - 2.0f * s, chipH - 2.0f * s, rgba(255, 178, 77, 18));
                        textTrackedRight(ui, x + chipW - 16.0f * s, chipY + 15.0f * s, "> FOCUS",
                                         pal::amber, fpx(ui, 9.0f, s), 1.8f * s);
                    }
                    // Name + payout sit on a row below the FOCUS badge header strip so they do not collide.
                    const float nameY = chipY + 42.0f * s;
                    ui.textD(x + 18.0f * s, nameY, c.name,
                             active ? pal::warnHi : rgb(199, 154, 94), fpx(ui, 18.0f, s));
                    ui.textDRight(x + chipW - 18.0f * s, nameY,
                                  "+" + std::to_string(static_cast<int>(std::lround(c.payoutWeight * 100.0f))) + "%",
                                  active ? pal::amber : rgb(199, 154, 94), fpx(ui, 16.0f, s));
                    ui.text(x + 18.0f * s, nameY + 32.0f * s, c.effect,
                            active ? rgb(224, 200, 156) : rgb(196, 168, 120), fpx(ui, 12.0f, s));
                }
            }

            // Column 3: Content unlocks.
            notchBRPanel(ui, col3X, bodyY, col3W, bodyH, cutPanel, panelFill, panelBorder, std::max(1.0f, 1.0f * s));
            {
                const float ix = col3X + 26.0f * s, iw = col3W - 52.0f * s;
                int owned = 0; for (const UnlockDef& u : hubUnlockables) if (meta_.isUnlocked(u.id)) ++owned;
                panelTitle(ix, bodyY + 26.0f * s, "CONTENT UNLOCKS");
                ui.textDRight(col3X + col3W - 26.0f * s, bodyY + 21.0f * s,
                              std::to_string(owned) + " / " + std::to_string(std::max(1, static_cast<int>(hubUnlockables.size()))),
                              pal::good, fpx(ui, 18.0f, s));
                const float rowY = bodyY + 66.0f * s, rowH = 54.0f * s, rowGap = 10.0f * s;
                const uint32_t designChips[5] = { pal::tierRare, pal::tierUncommon, pal::tierUncommon, pal::tierRare, pal::tierCommon };
                for (int i = 0; i < hubCatCount; ++i) {
                    const UnlockDef& u = hubUnlockables[static_cast<size_t>(i)];
                    const bool own = meta_.isUnlocked(u.id);
                    const bool afford = meta_.currency() >= u.cost;
                    const bool hot = hubFocused(i) || i == 0;
                    const float y = rowY + static_cast<float>(i) * (rowH + rowGap);
                    if (hubFocused(i)) menuFocusHalo(ui, ix, y, iw, rowH, 6.0f * s, pal::accentGlow, s);
                    ui.rect(ix, y, iw, rowH, hot ? activeRow : idleRow);
                    ui.rectOutline(ix, y, iw, rowH, hot ? rgba(60, 200, 235, 102) : rgba(90, 170, 200, 46), std::max(1.0f, 1.0f * s));
                    if (hubFocused(i)) focusRing(ui, ix, y, iw, rowH, 6.0f * s, s);
                    const bool isWeapon = u.id.rfind("w:", 0) == 0;
                    const std::string tag = isWeapon ? "WEAPON" : "PASSIVE";
                    ui.textTracked(ix + 16.0f * s, y + 20.0f * s, tag,
                                   hot ? rgb(93, 210, 236) : rgb(94, 126, 142), fpx(ui, 10.0f, s), 1.7f * s);
                    const uint32_t chip = (i < 5) ? designChips[i] : tierColor(build_.describeReward(u.id).tier);
                    ui.rect(ix + 93.0f * s, y + 22.0f * s, 9.0f * s, 9.0f * s, chip);
                    ui.textD(ix + 116.0f * s, y + 14.0f * s, u.name,
                             hot ? pal::textHi : rgb(205, 217, 226), fpx(ui, 18.0f, s));
                    std::string state = own ? "OWNED" : (afford ? ("BUY " + std::to_string(u.cost)) : ("NEED " + std::to_string(u.cost - meta_.currency())));
                    textTrackedRight(ui, ix + iw - 16.0f * s, y + 21.0f * s, state,
                                     own ? (hot ? pal::good : label) : (afford ? pal::accent : pal::danger), fpx(ui, 11.0f, s), 1.8f * s);
                }
                ui.textTracked(ix, bodyY + bodyH - 36.0f * s, "EXPANDS FUTURE RUN REWARD POOLS",
                               rgb(63, 86, 99), fpx(ui, 11.0f, s), 2.3f * s);
            }

            // Full-width Start Run bar.
            if (hubFocused(hubStartFocus)) menuFocusHalo(ui, padX, startY, frameW, startH, 20.0f * s, pal::accentGlow, s);
            chamferFill(ui, padX - 2.0f * s, startY - 2.0f * s, frameW + 4.0f * s, startH + 4.0f * s,
                        20.0f * s, rgba(43, 214, 245, 102));
            chamferFill(ui, padX, startY, frameW, startH, 20.0f * s, rgba(13, 56, 72, 245));
            gradientH(ui, padX + 2.0f * s, startY + 2.0f * s, frameW - 4.0f * s, startH - 4.0f * s,
                      rgba(13, 56, 72, 245), rgba(8, 24, 34, 235), 34);
            chamferOutline(ui, padX, startY, frameW, startH, 20.0f * s, pal::accent, std::max(2.0f, 2.0f * s));
            if (hubFocused(hubStartFocus)) focusRing(ui, padX, startY, frameW, startH, 20.0f * s, s);
            ui.textD(padX + 44.0f * s, startY + 28.0f * s, "START RUN", pal::textHero, fpx(ui, 54.0f, s));
            ui.textTracked(padX + 46.0f * s, startY + 84.0f * s,
                           "SPACE SELECTS . HEAT " + std::to_string(meta_.heat()) + " . NEW SEED",
                           rgb(111, 201, 224), fpx(ui, 13.0f, s), 2.6f * s);
            pulseWaveLong(ui, padX + frameW - 602.0f * s, startY + 64.0f * s, 420.0f * s, 30.0f * s,
                          pal::accentGlow, rgba(43, 214, 245, 62), std::max(2.0f, 2.5f * s));
            textTrackedRight(ui, padX + frameW - 44.0f * s, startY + 42.0f * s, "FLOW STATE",
                             rgb(111, 201, 224), fpx(ui, 12.0f, s), 2.6f * s);
            textTrackedRight(ui, padX + frameW - 44.0f * s, startY + 72.0f * s, "DORMANT",
                             pal::good, fpx(ui, 15.0f, s), 2.4f * s);
        }
        if (codexOpen_) drawCodex(ui, w, h);
        return;

    }
    if (phase_ == RunPhase::ChoosePath) {
        darkBoardBg();

        const int maxHp = std::max(1, effectiveMaxHealth());
        const std::string kicker = std::string(biomeName(currentBiome_)) + " . SECTOR " +
            std::to_string(run_.sector() + 1) + " / " + std::to_string(run_.sectorCount());
        ui.textTracked(margin, 50.0f * s, kicker, pal::textFaint, fpx(ui, 16.0f, s), 2.2f * s);
        glowTextD(ui, margin, 76.0f * s, "CHOOSE EXIT", withAlpha(pal::accentGlow, 50),
                  pal::textHero, fpx(ui, 58.0f, s));
        ui.textD(margin, 142.0f * s, "Pick the next room by what it costs, pays, and does to your build.",
                 pal::textMid, fpx(ui, 22.0f, s));

        const float statX = static_cast<float>(w) - margin - 340.0f * s;
        auto statChip = [&](float y, const std::string& label, const std::string& value, uint32_t col) {
            chamferFill(ui, statX, y, 340.0f * s, 44.0f * s, 8.0f * s, rgba(8, 12, 18, 210));
            chamferOutline(ui, statX, y, 340.0f * s, 44.0f * s, 8.0f * s, withAlpha(col, 155), std::max(1.0f, 1.0f * s));
            ui.textTracked(statX + 18.0f * s, y + 14.0f * s, label, pal::textFaint, fpx(ui, 13.0f, s), 1.6f * s);
            ui.textRight(statX + 318.0f * s, y + 9.0f * s, value, col, fpx(ui, 24.0f, s));
        };
        statChip(62.0f * s, "HEALTH", std::to_string(player_.hp) + " / " + std::to_string(maxHp), pal::good);
        statChip(114.0f * s, "SCRAP", std::to_string(scrap_), pal::gold);
        drawRouteRail(ui, cx, 182.0f * s, s * 1.05f);

        const std::vector<RoomSpec>& opts = run_.currentOptions();
        const int n = static_cast<int>(opts.size());
        const float frameX = margin, frameW = static_cast<float>(w) - margin * 2.0f;
        const float gap = 26.0f * s;
        const float cardW = n > 0 ? (frameW - gap * static_cast<float>(std::max(0, n - 1))) / static_cast<float>(n) : 0.0f;
        const float cardH = 490.0f * s;
        const float y0 = 266.0f * s;
        for (int i = 0; i < n; ++i) {
            const RoomType t = opts[static_cast<size_t>(i)].type;
            const RouteVisual route = routeVisual(t);
            const bool sel = i == cardCursor_;
            const float x = frameX + static_cast<float>(i) * (cardW + gap);
            const uint32_t border = sel ? pal::accent : withAlpha(route.col, 180);

            menuHits_.push_back({ x, y0, cardW, cardH, i });   // clickable route card
            if (sel) menuFocusHalo(ui, x, y0, cardW, cardH, 18.0f * s, pal::accentGlow, s);
            chamferPanel(ui, x, y0, cardW, cardH, 18.0f * s, rgb(10, 14, 20), border,
                         sel ? std::max(1.8f, 1.8f * s) : std::max(1.0f, 1.1f * s), true);
            ui.rect(x, y0 + 18.0f * s, std::max(5.0f, 5.0f * s), cardH - 18.0f * s, route.col);
            if (sel) focusRing(ui, x, y0, cardW, cardH, 18.0f * s, s);

            chamferFill(ui, x + 24.0f * s, y0 + 24.0f * s, 44.0f * s, 44.0f * s, 10.0f * s, route.col);
            ui.textCentered(x + 46.0f * s, y0 + 34.0f * s, std::to_string(i + 1), pal::navy, fpx(ui, 24.0f, s));
            iconTile(ui, x + cardW - 92.0f * s, y0 + 24.0f * s, 58.0f * s, 10.0f * s, route.icon, route.col, s);
            ui.textTracked(x + 86.0f * s, y0 + 30.0f * s, route.tag, route.col, fpx(ui, 14.0f, s), 2.0f * s);
            ui.textTracked(x + 86.0f * s, y0 + 52.0f * s, sel ? "SELECTED" : "AVAILABLE",
                           sel ? pal::accent : pal::textFaint, fpx(ui, 12.0f, s), 1.6f * s);

            float nameSc = fpx(ui, 40.0f, s);
            if (ui.textDWidth(route.name, nameSc) > cardW - 58.0f * s) nameSc = fpx(ui, 34.0f, s);
            ui.textD(x + 30.0f * s, y0 + 104.0f * s, route.name, pal::textHero, nameSc);
            textDWrapped(ui, x + 30.0f * s, y0 + 156.0f * s, cardW - 60.0f * s,
                         route.stake, pal::textMid, fpx(ui, 22.0f, s), 5.0f * s, 2);

            const float ruleY = y0 + 234.0f * s;
            ui.rect(x + 30.0f * s, ruleY, cardW - 60.0f * s, 1.0f, pal::lineSoft);
            ui.textTracked(x + 30.0f * s, ruleY + 25.0f * s, "PAYOUT", pal::textFaint, fpx(ui, 13.0f, s), 2.0f * s);
            textDWrapped(ui, x + 30.0f * s, ruleY + 48.0f * s, cardW - 60.0f * s,
                         route.reward, route.col, fpx(ui, 21.0f, s), 4.0f * s, 2);
            ui.textTracked(x + 30.0f * s, ruleY + 126.0f * s, "BUILD HOOK", pal::textFaint, fpx(ui, 13.0f, s), 2.0f * s);
            textDWrapped(ui, x + 30.0f * s, ruleY + 149.0f * s, cardW - 60.0f * s,
                         route.buildHook, pal::textMid, fpx(ui, 18.0f, s), 3.0f * s, 2);

            const float footerY = y0 + cardH - 66.0f * s;
            ui.rect(x, footerY, cardW, 1.0f, sel ? withAlpha(pal::accent, 70) : pal::lineSoft);
            const int risk = route.riskLevel;
            const uint32_t rc = riskColorForLevel(risk);
            ui.textTracked(x + 30.0f * s, footerY + 21.0f * s,
                           std::string("RISK ") + route.risk, rc, fpx(ui, 14.0f, s), 1.5f * s);
            const float sq = 10.0f * s, sqGap = 6.0f * s;
            const float rx = x + 152.0f * s;
            for (int k = 0; k < 3; ++k)
                ui.rect(rx + static_cast<float>(k) * (sq + sqGap), footerY + 24.0f * s,
                        sq, sq, k < risk ? rc : pal::disabled);
            ui.textRight(x + cardW - 30.0f * s, footerY + 18.0f * s,
                         "[" + std::to_string(i + 1) + "] " + (sel ? "COMMIT" : "SELECT"),
                         sel ? pal::accent : pal::textFaint, fpx(ui, 17.0f, s));
        }

        const float bossY = static_cast<float>(h) - 90.0f * s;
        ui.rect(margin, bossY - 16.0f * s, static_cast<float>(w) - margin * 2.0f, 1.0f, withAlpha(pal::bossCore, 70));
        ui.textTracked(margin, bossY, "SECTOR BOSS AHEAD", pal::dangerHi, fpx(ui, 13.0f, s), 2.0f * s);
        ui.textRight(static_cast<float>(w) - margin, bossY, bossName(run_.sector() % 3), pal::bossCoreHi, fpx(ui, 20.0f, s));
        ui.textCentered(cx, static_cast<float>(h) - 38.0f * s,
                        "1-3 select . ENTER commit . [C] systems", pal::textFaint, fpx(ui, 18.0f, s));
        if (codexOpen_) drawCodex(ui, w, h);
        return;
    }
    if (phase_ == RunPhase::Shop) {
        {
            darkBoardBg();
            auto tierColorShop = [](ItemTier t) -> uint32_t {
                switch (t) {
                    case ItemTier::Common: return pal::tierCommon;
                    case ItemTier::Uncommon: return pal::tierUncommon;
                    case ItemTier::Rare: return pal::tierRare;
                    case ItemTier::Legendary: return pal::tierLegendary;
                }
                return pal::textMid;
            };
            const float pad = 80.0f * s, topY = 52.0f * s, frameW = static_cast<float>(w) - pad * 2.0f;
            glowTextD(ui, pad, topY, "SHOP", withAlpha(pal::accentGlow, 45), pal::textHero, fpx(ui, 58.0f, s));
            ui.textTracked(pad, topY + 66.0f * s, "STOCK RACK  .  REPAIR  .  REROLL  .  FORGE BAY", pal::textFaint, fpx(ui, 14.0f, s), 2.0f * s);
            ui.textRight(w - pad, topY + 4.0f * s, "SCRAP", pal::textDim, fpx(ui, 16.0f, s));
            ui.textRight(w - pad, topY + 30.0f * s, std::to_string(scrap_), pal::tierRare, fpx(ui, 46.0f, s));

            const float rackX = pad, rackY = topY + 120.0f * s, rackW = 940.0f * s, rackH = 586.0f * s;
            chamferPanel(ui, rackX, rackY, rackW, rackH, 14.0f * s, pal::surface0, pal::border, std::max(1.0f, 1.2f * s), true);
            ui.textTracked(rackX + 28.0f * s, rackY + 22.0f * s, "BUY STOCK", pal::accent, fpx(ui, 15.0f, s), 3.0f * s);
            const int items = std::min(4, static_cast<int>(shopStock_.size()));
            const float cardGap = 18.0f * s;
            const float cardW = (rackW - 56.0f * s - cardGap) * 0.5f;
            const float cardH = 236.0f * s;
            for (int i = 0; i < items; ++i) {
                const Build::RewardView rv = build_.describeReward(shopStock_[static_cast<size_t>(i)]);
                const bool sold = shopSold_[static_cast<size_t>(i)] != 0;
                const int price = shopPrices_[static_cast<size_t>(i)];
                const bool afford = !sold && scrap_ >= price;
                const bool sel = !sold && i == cardCursor_;
                const uint32_t col = rv.valid ? tierColorShop(rv.tier) : pal::textDim;
                const float x = rackX + 28.0f * s + static_cast<float>(i % 2) * (cardW + cardGap);
                const float y = rackY + 62.0f * s + static_cast<float>(i / 2) * (cardH + cardGap);
                menuHits_.push_back({ x, y, cardW, cardH, i });   // clickable stock card
                if (sel) menuFocusHalo(ui, x, y, cardW, cardH, 12.0f * s, pal::accentGlow, s);
                chamferPanel(ui, x, y, cardW, cardH, 12.0f * s, rgb(10, 14, 20),
                             sel ? pal::accent : (afford ? withAlpha(col, 190) : pal::border),
                             sel ? std::max(1.5f, 1.6f * s) : std::max(1.0f, 1.0f * s), true);
                if (sel) focusRing(ui, x, y, cardW, cardH, 12.0f * s, s);
                if (!rv.valid) continue;
                const bool hasAff = rv.affinity != Affinity::None;
                const AffVis av = affVis(rv.affinity);
                ui.textTracked(x + 18.0f * s, y + 18.0f * s,
                               "[" + std::to_string(i + 1) + "] " + (rv.isWeapon ? "WEAPON" : "PASSIVE"),
                               pal::textDim, fpx(ui, 13.0f, s), 1.2f * s);
                ui.textRight(x + cardW - 18.0f * s, y + 18.0f * s, tierName(rv.tier), col, fpx(ui, 13.0f, s));
                if (hasAff) {
                    drawIcon(ui, av.icon, x + 22.0f * s, y + 58.0f * s, 6.0f * s, av.col, s);
                    ui.textTracked(x + 38.0f * s, y + 52.0f * s, av.name, av.col, fpx(ui, 13.0f, s), 1.0f * s);
                }
                ui.textD(x + 18.0f * s, y + 76.0f * s, rv.name, sold ? pal::textDim : pal::textHi, fpx(ui, 28.0f, s));
                textDWrapped(ui, x + 18.0f * s, y + 112.0f * s, cardW - 36.0f * s,
                             rv.blurb, sold ? pal::textFaint : pal::textMid, fpx(ui, 16.0f, s), 3.0f * s, 2);
                if (hasAff) {
                    textDWrapped(ui, x + 18.0f * s, y + cardH - 72.0f * s, cardW - 36.0f * s,
                                 affinityProgressLine(build_.stats(), rv.affinity),
                                 sold ? pal::textFaint : av.col, fpx(ui, 14.0f, s), 1.0f * s, 1);
                } else if (rv.isWeapon) {
                    textDWrapped(ui, x + 18.0f * s, y + cardH - 72.0f * s, cardW - 36.0f * s,
                                 "Adds a weapon option. Forge later for power.",
                                 sold ? pal::textFaint : pal::amber, fpx(ui, 14.0f, s), 1.0f * s, 1);
                }
                ui.rect(x, y + cardH - 42.0f * s, cardW, 1.0f, pal::lineSoft);
                const std::string buyState = sold ? "SOLD" : (afford ? "BUY NOW" : ("NEED +" + std::to_string(price - scrap_)));
                ui.textTracked(x + 18.0f * s, y + cardH - 28.0f * s, buyState,
                               sold ? pal::textFaint : (afford ? pal::accent : pal::danger), fpx(ui, 14.0f, s), 1.0f * s);
                ui.textRight(x + cardW - 18.0f * s, y + cardH - 30.0f * s,
                             std::to_string(price), afford ? pal::tierRare : pal::danger, fpx(ui, 18.0f, s));
            }

            const float bayX = rackX + rackW + 28.0f * s, bayY = rackY, bayW = frameW - rackW - 28.0f * s, bayH = rackH;
            chamferPanel(ui, bayX, bayY, bayW, bayH, 16.0f * s, rgb(18, 12, 8), withAlpha(pal::amber, 150), std::max(1.0f, 1.4f * s), true);
            ui.rect(bayX, bayY + 16.0f * s, 5.0f * s, bayH - 16.0f * s, pal::amber);
            ui.textTracked(bayX + 30.0f * s, bayY + 24.0f * s, "FORGE BAY", pal::gold, fpx(ui, 16.0f, s), 3.0f * s);
            const int forgePower = (activeWeapon_ >= 0 && activeWeapon_ < static_cast<int>(loadout_.size()))
                ? loadout_[static_cast<size_t>(activeWeapon_)].power : 1;
            const int forgePrice = std::max(1, static_cast<int>(std::lround((30.0f + 25.0f * static_cast<float>(forgePower - 1)) * mods_.scrapMult)));
            const bool canForge = scrap_ >= forgePrice;
            const WeaponDef& forgeDef = activeWeaponDef();
            const int aspectTotal = static_cast<int>(forgeDef.aspects.size());
            const int formsBefore = aspectsUnlocked();
            const int formsAfter = std::max(1, std::min(1 + aspectTotal, forgePower + 1));
            std::string currentForm;
            if (const WeaponAspect* asp = activeAspect())
                currentForm = std::string("Current form: ") + asp->name + " . Cycle with X in combat.";
            else if (formsBefore > 1)
                currentForm = "Current form: Base . Cycle unlocked aspects with X.";
            else if (aspectTotal > 0)
                currentForm = "Current form: Base . First aspect unlocks at Lv2.";
            else
                currentForm = "Current form: Base . This weapon has no aspect forms.";
            std::string forgeOutcome;
            if (aspectTotal == 0) {
                forgeOutcome = "Next forge: +25% damage. No aspect unlock on this weapon.";
            } else if (formsAfter > formsBefore) {
                const WeaponAspect& nextAsp = forgeDef.aspects[static_cast<size_t>(formsAfter - 2)];
                forgeOutcome = "Next forge unlocks aspect: " + nextAsp.name + ".";
            } else {
                forgeOutcome = "Next forge: +25% damage. All aspects already unlocked.";
            }
            ui.textD(bayX + 30.0f * s, bayY + 72.0f * s, forgeDef.name, pal::textHero, fpx(ui, 42.0f, s));
            ui.textTracked(bayX + 30.0f * s, bayY + 124.0f * s,
                           "LV" + std::to_string(forgePower) + "  ->  LV" + std::to_string(forgePower + 1),
                           pal::amber, fpx(ui, 18.0f, s), 1.2f * s);
            ui.textD(bayX + 30.0f * s, bayY + 156.0f * s, currentForm,
                     rgb(255, 200, 120), fpx(ui, 16.0f, s));
            ui.textD(bayX + 30.0f * s, bayY + 182.0f * s, forgeOutcome,
                     pal::textMid, fpx(ui, 16.0f, s));
            ui.rect(bayX + 30.0f * s, bayY + 206.0f * s, bayW - 60.0f * s, 1.0f, rgba(255, 177, 61, 54));
            ui.textTracked(bayX + 30.0f * s, bayY + 236.0f * s, "PRESS F", canForge ? pal::gold : pal::textDim, fpx(ui, 20.0f, s), 2.0f * s);
            ui.textRight(bayX + bayW - 30.0f * s, bayY + 236.0f * s, std::to_string(forgePrice) + " SCRAP",
                         canForge ? pal::tierRare : pal::danger, fpx(ui, 22.0f, s));
            const int healPrice = std::max(1, static_cast<int>(std::lround(24.0f * mods_.scrapMult)));
            const int rerollPrice = std::max(1, static_cast<int>(std::lround((10.0f + 6.0f * static_cast<float>(shopRerollCount_)) * mods_.scrapMult)));
            auto serviceRow = [&](float y, uint32_t col, const std::string& label, const std::string& sub, int price) {
                chamferOutline(ui, bayX + 30.0f * s, y, bayW - 60.0f * s, 54.0f * s, 8.0f * s, withAlpha(col, 150), std::max(1.0f, 1.0f * s));
                ui.rect(bayX + 30.0f * s, y + 8.0f * s, 4.0f * s, 46.0f * s, col);
                ui.text(bayX + 48.0f * s, y + 10.0f * s, label, pal::textHi, fpx(ui, 18.0f, s));
                ui.text(bayX + 48.0f * s, y + 32.0f * s, sub, pal::textFaint, fpx(ui, 13.0f, s));
                ui.textRight(bayX + bayW - 46.0f * s, y + 16.0f * s, std::to_string(price), pal::tierRare, fpx(ui, 18.0f, s));
            };
            // Bottom-anchor the two service rows so the forge bay fills its full height
            // (it shares the stock rack's height but has less content) instead of leaving
            // a large empty band below.
            serviceRow(bayY + bayH - 142.0f * s, pal::good, "REPAIR +" + std::to_string(healAmount(30)) + " HP", "hold H", healPrice);
            serviceRow(bayY + bayH - 74.0f * s, pal::accent, "REROLL STOCK", "press R", rerollPrice);
            // Everything in the shop is clickable too (the keyboard shortcuts still work).
            menuHits_.push_back({ bayX + 30.0f * s, bayY + 214.0f * s, bayW - 60.0f * s, 46.0f * s, MenuIdForge });
            menuHits_.push_back({ bayX + 30.0f * s, bayY + bayH - 142.0f * s, bayW - 60.0f * s, 54.0f * s, MenuIdHeal });
            menuHits_.push_back({ bayX + 30.0f * s, bayY + bayH - 74.0f * s, bayW - 60.0f * s, 54.0f * s, MenuIdReroll });
            menuHits_.push_back({ cx - 360.0f * s, h - 50.0f * s, 720.0f * s, 34.0f * s, MenuIdLeave });
            ui.textCentered(cx, h - 34.0f * s,
                            "1-4 buy . H repair . R reroll . F forge . [C] systems . SPACE / click here to leave",
                            pal::textFaint, fpx(ui, 17.0f, s));
            if (codexOpen_) drawCodex(ui, w, h);
        }
        return;
    }
    if (phase_ == RunPhase::Event) {
        darkBoardBg(rgb(20, 8, 16), pal::deepBg);
        for (int r = 10; r >= 1; --r)
            diamond(ui, cx, cy, static_cast<float>(r) * 72.0f * s,
                    withAlpha(pal::crimson, static_cast<uint8_t>(4 + (10 - r))));
        glowTextD(ui, cx - ui.textDWidth("A PACT", fpx(ui, 64.0f, s)) * 0.5f, 64.0f * s,
                  "A PACT", withAlpha(pal::crimsonHi, 62), pal::crimson, fpx(ui, 64.0f, s));
        ui.textCentered(cx, 136.0f * s, "Power has a price.", rgb(217, 179, 187), fpx(ui, 22.0f, s));

        const int n = std::min(2, static_cast<int>(eventDeals_.size()));
        const float cardW = 620.0f * s, cardH = 392.0f * s, gap = 48.0f * s;
        const float totalW = n > 0 ? cardW * n + gap * static_cast<float>(n - 1) : 0.0f;
        const float x0 = cx - totalW * 0.5f, y0 = 220.0f * s;
        for (int i = 0; i < n; ++i) {
            const Deal& d = dealCatalog()[static_cast<size_t>(eventDeals_[static_cast<size_t>(i)])];
            const bool sel = i == cardCursor_;
            const bool afford = !(d.scrap < 0 && scrap_ < -d.scrap);
            const float x = x0 + static_cast<float>(i) * (cardW + gap);
            menuHits_.push_back({ x, y0, cardW, cardH, i });   // clickable pact card
            if (sel) menuFocusHalo(ui, x, y0, cardW, cardH, 16.0f * s, afford ? pal::accentGlow : pal::crimsonHi, s);
            chamferPanel(ui, x, y0, cardW, cardH, 16.0f * s, rgb(18, 10, 16),
                         sel ? (afford ? pal::accent : pal::danger) : withAlpha(pal::crimson, 150),
                         sel ? std::max(1.5f, 1.6f * s) : std::max(1.0f, 1.0f * s), true);
            ui.rect(x, y0 + 16.0f * s, std::max(5.0f, 5.0f * s), cardH - 16.0f * s, pal::crimson);
            if (sel) focusRing(ui, x, y0, cardW, cardH, 16.0f * s, s);
            ui.rect(x, y0 + 72.0f * s, cardW, 1.0f, sel ? pal::border : rgb(42, 22, 32));
            ui.textD(x + 32.0f * s, y0 + 24.0f * s, d.name, pal::textHero, fpx(ui, 38.0f, s));

            const std::string blurb = d.blurb ? d.blurb : "";
            std::string boon = blurb, curse;
            const char* seps[4] = { " - but ", ", but ", " but ", " - " };
            for (const char* raw : seps) {
                const std::string sep = raw;
                const size_t p = blurb.find(sep);
                if (p != std::string::npos) {
                    boon = blurb.substr(0, p);
                    curse = blurb.substr(p + sep.size());
                    break;
                }
            }
            const std::string scope = d.scope == 1 ? "this sector" : "all run";
            auto pactBlock = [&](float by, uint32_t col, const std::string& label, const std::string& body) {
                ui.rect(x + 32.0f * s, by, std::max(3.0f, 3.0f * s), 90.0f * s, col);
                ui.textTracked(x + 54.0f * s, by + 10.0f * s, label, col, fpx(ui, 15.0f, s), 2.0f * s);
                textDWrapped(ui, x + 54.0f * s, by + 38.0f * s, cardW - 92.0f * s,
                             body, label.find("CURSE") != std::string::npos ? rgb(226, 194, 200) : pal::textHi,
                             fpx(ui, 22.0f, s), 4.0f * s, 2);
            };
            pactBlock(y0 + 104.0f * s, pal::good, "BOON . EXACT GAIN", boon);
            pactBlock(y0 + 224.0f * s, pal::crimson, "CURSE . DURATION " + scope, curse.empty() ? "A lasting cost." : curse);
            ui.rect(x, y0 + cardH - 46.0f * s, cardW, 1.0f, sel ? withAlpha(afford ? pal::accent : pal::danger, 80) : rgb(42, 22, 32));
            if (sel) ui.rect(x, y0 + cardH - 45.0f * s, cardW, 45.0f * s, afford ? rgb(13, 32, 40) : rgb(42, 18, 22));
            ui.textCentered(x + cardW * 0.5f, y0 + cardH - 31.0f * s,
                            afford ? ("[" + std::to_string(i + 1) + "] ACCEPT")
                                   : ("NEED " + std::to_string(-d.scrap) + " SCRAP"),
                            sel ? (afford ? pal::accent : pal::danger) : (afford ? pal::textMid : pal::textFaint), fpx(ui, 20.0f, s));
        }
        const float declineW = 420.0f * s, declineY = h - 146.0f * s;
        menuHits_.push_back({ cx - declineW * 0.5f, declineY, declineW, 44.0f * s, MenuIdDecline });   // clickable
        chamferOutline(ui, cx - declineW * 0.5f, declineY, declineW, 44.0f * s, 6.0f * s,
                       pal::border, std::max(1.0f, 1.0f * s));
        ui.textCentered(cx, declineY + 12.0f * s, "[SPACE] / click to DECLINE - leave with nothing",
                        pal::textMid, fpx(ui, 18.0f, s));
        ui.textCentered(cx, declineY + 58.0f * s,
                        "Accepting is irreversible - the curse holds for its full duration.",
                        pal::crimson, fpx(ui, 14.0f, s));
        ui.textCentered(cx, h - 40.0f * s, "1-2 / ENTER accept . SPACE decline . [C] SYSTEMS",
                        rgb(107, 85, 96), fpx(ui, 16.0f, s));
        if (codexOpen_) drawCodex(ui, w, h);
        return;
    }
    if (configMessageTimer_ > 0.0f)
        ui.textRight(w - 34.0f, 32.0f, configMessage_, rgb(125, 220, 255), 2 * ts);

    // M7: the SYSTEMS field manual ([C]) overlays the menu HUD - a reference you read while
    // planning a build or choosing a route. The toggle is gated to menu phases in update().
    if (codexOpen_) drawCodex(ui, w, h);

    // Pause / options-from-pause draw over the frozen in-run HUD (the main-menu and
    // options-from-main cases already returned at the top of buildHud).
    if (frontEnd_ && menuScreen_ != MenuScreen::None)
        buildMenuOverlay(ui, w, h);
}

// The front-end shell: a dim scrim plus a titled panel. Main/Pause share a vertical
// option list; Settings shows labelled sliders + a toggle. Uses the same pal/cardPanel/
// glow kit as the in-game menus so it reads as one designed language.
void PulseGame::buildMenuOverlay(UiDrawList& ui, int w, int h) {
    const float cx = w * 0.5f;
    const float s = uiScale(w, h);
    const float cut = 12.0f * s;

    // A right-pointing thousands-grouped integer for the footer / score readouts.
    auto comma = [](int v) {
        std::string in = std::to_string(v < 0 ? -v : v), out;
        int c = 0;
        for (int i = static_cast<int>(in.size()) - 1; i >= 0; --i) {
            out.insert(out.begin(), in[static_cast<size_t>(i)]);
            if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
        }
        return (v < 0 ? "-" : "") + out;
    };
    // topDecor=false reserves the top band for screens that place their own controls there
    // (the Options screen: a high OPTIONS title plus the APPLY / UNSAVED buttons). It drops the
    // full-width top rule and the top-right heartbeat so neither draws behind those controls.
    // The bottom rule, grid, vignette and corner brackets are unaffected.
    auto menuBackdrop = [&](uint32_t top = rgb(8, 17, 27), uint32_t bottom = pal::deepBg,
                            bool topDecor = true) {
        const float W = static_cast<float>(w), H = static_cast<float>(h);
        ui.rect(0.0f, 0.0f, W, H, pal::deepBg);
        ui.gradientV(0.0f, 0.0f, W, H, top, bottom, 72);
        for (int i = 0; i < 22; ++i) {
            const float t = static_cast<float>(i) / 21.0f;
            const uint8_t a = static_cast<uint8_t>(5.0f + t * t * 20.0f);
            ui.rect(0.0f, i * 9.0f * s, W, 9.0f * s + 1.0f, rgba(0, 0, 0, a));
            ui.rect(0.0f, H - (i + 1) * 10.0f * s, W, 10.0f * s + 1.0f, rgba(0, 0, 0, a));
            ui.rect(i * 10.0f * s, 0.0f, 10.0f * s + 1.0f, H, rgba(0, 0, 0, a));
            ui.rect(W - (i + 1) * 10.0f * s, 0.0f, 10.0f * s + 1.0f, H, rgba(0, 0, 0, a));
        }
        const float grid = 56.0f * s;
        for (float x = 0.0f; x <= W + grid; x += grid)
            ui.rect(x, 0.0f, std::max(1.0f, 1.0f * s), H, rgba(56, 200, 232, 10));
        for (float y = 0.0f; y <= H + grid; y += grid)
            ui.rect(0.0f, y, W, std::max(1.0f, 1.0f * s), rgba(56, 200, 232, 8));
        if (topDecor)
            ui.rect(76.0f * s, 82.0f * s, W - 152.0f * s, std::max(1.0f, 1.0f * s), rgba(90, 170, 200, 34));
        ui.rect(76.0f * s, H - 82.0f * s, W - 152.0f * s, std::max(1.0f, 1.0f * s), rgba(255, 178, 77, 24));
        if (topDecor)
            pulseWaveLong(ui, W - 720.0f * s, 104.0f * s, 520.0f * s, 42.0f * s,
                          withAlpha(pal::accent, 102), withAlpha(pal::accentGlow, 20), std::max(2.0f, 2.0f * s));
        screenBrackets(ui, W, H, s);
    };

    // ---- Options ----------------------------------------------------------
    if (menuScreen_ == MenuScreen::Settings) {
        menuBackdrop(rgb(8, 17, 27), pal::deepBg, false);   // top band reserved for title + APPLY/UNSAVED
        auto pct = [](float v) { return std::to_string(static_cast<int>(std::lround(v * 100.0f))) + "%"; };
        auto dec2 = [](float v) {
            const int hh = static_cast<int>(std::lround(v * 100.0f));
            return std::to_string(hh / 100) + "." + (hh % 100 < 10 ? std::string("0") : std::string()) + std::to_string(hh % 100);
        };

        const float padX = 200.0f * s, topY = 70.0f * s;
        const float frameW = static_cast<float>(w) - padX * 2.0f;
        ui.textTracked(padX, topY - 24.0f * s, "SYSTEM", rgb(94, 126, 142), fpx(ui, 14.0f, s), 3.0f * s);
        glowTextD(ui, padX, topY, "OPTIONS", withAlpha(pal::accentGlow, 45), pal::textHi, fpx(ui, 60.0f, s));
        ui.rect(padX, topY + 68.0f * s, 54.0f * s, 3.0f * s, pal::amber);
        gradientH(ui, padX + 54.0f * s, topY + 68.0f * s, 420.0f * s, 2.0f * s,
                  pal::accent, rgba(43, 214, 245, 0), 24);
        const float applyW = 104.0f * s, applyH = 34.0f * s;
        const float applyX = w - padX - applyW, applyY = topY + 10.0f * s;
        menuHits_.push_back({ applyX, applyY, applyW, applyH, 210 });   // clickable APPLY
        if (settingsDirty_) menuFocusHalo(ui, applyX, applyY, applyW, applyH, 7.0f * s, pal::accentGlow, s);
        chamferFill(ui, applyX, applyY, applyW, applyH, 7.0f * s,
                    settingsDirty_ ? pal::accent : rgb(42, 51, 64));
        ui.textCentered(applyX + applyW * 0.5f, applyY + 8.0f * s, "APPLY",
                        settingsDirty_ ? pal::deepBg : pal::textDim, fpx(ui, 15.0f, s));
        const float unsW = 122.0f * s, unsX = applyX - unsW - 12.0f * s;
        chamferOutline(ui, unsX, applyY, unsW, applyH, 7.0f * s,
                       settingsDirty_ ? pal::amber : pal::border, std::max(1.0f, 1.0f * s));
        ui.textTracked(unsX + 12.0f * s, applyY + 9.0f * s, "UNSAVED",
                       settingsDirty_ ? pal::amber : pal::textFaint, fpx(ui, 15.0f, s), 1.0f * s);

        // Heartbeat flourish, parked to the LEFT of the UNSAVED / APPLY buttons (with a clear gap)
        // and centred on the button row. The shared backdrop's top-right wave is suppressed for this
        // screen, so this is the only EKG line drawn and nothing runs behind the controls or title.
        const float ekgRight = unsX - 40.0f * s;
        float ekgLeft = ekgRight - 340.0f * s;
        const float ekgMinLeft = padX + 380.0f * s;          // stay clear of the OPTIONS title
        if (ekgLeft < ekgMinLeft) ekgLeft = ekgMinLeft;
        if (ekgRight - ekgLeft > 120.0f * s)
            pulseWaveLong(ui, ekgLeft, applyY + applyH * 0.5f, ekgRight - ekgLeft, 24.0f * s,
                          withAlpha(pal::accent, 102), withAlpha(pal::accentGlow, 20), std::max(2.0f, 2.0f * s));

        static const char* const tabNames[5] = { "AUDIO", "CONTROLS", "VIDEO", "ACCESSIBILITY", "GAMEPLAY" };
        float tx = padX, tabY = topY + 80.0f * s;
        for (int t = 0; t < 5; ++t) {
            const bool active = t == menuTab_;
            const float tw = ui.textTrackedWidth(tabNames[t], fpx(ui, 17.0f, s), 1.6f * s) + 36.0f * s;
            menuHits_.push_back({ tx, tabY, tw, 42.0f * s, 200 + t });   // clickable options tab
            if (active) {
                chamferFill(ui, tx, tabY, tw, 42.0f * s, 8.0f * s, pal::accent);
            } else {
                chamferOutline(ui, tx, tabY, tw, 42.0f * s, 8.0f * s,
                               rgba(80, 180, 210, 46), std::max(1.0f, 1.0f * s));
            }
            ui.textTracked(tx + 18.0f * s, tabY + 12.0f * s, tabNames[t],
                           active ? pal::deepBg : pal::textDim, fpx(ui, 17.0f, s), 1.6f * s);
            tx += tw + 6.0f * s;
        }
        ui.rect(padX, tabY + 54.0f * s, frameW, 1.0f, rgba(90, 170, 200, 36));

        static const char* const qnames[4] = { "Low", "Medium", "High", "Ultra" };
        static const char* const cbnames[4] = { "OFF", "Deuteranopia", "Protanopia", "Tritanopia" };
        const int qi = std::min(3, std::max(0, settings_.graphicsQuality));
        const int cbi = std::min(3, std::max(0, settings_.colorblindPreset));

        const float bodyY = tabY + 82.0f * s, gap = 60.0f * s;
        const float colW = (frameW - gap) * 0.5f;
        const float leftX = padX, rightX = padX + colW + gap;
        auto groupTitle = [&](float x, float y, const std::string& label, uint32_t col) {
            ui.textTracked(x, y, label, col, fpx(ui, 18.0f, s), 3.0f * s);
        };
        // Row index counter: each row helper consumes one id in call order (which matches menuSel_),
        // so every Options row publishes a clickable rect + (sliders) a drag bar, with no per-call edits.
        int rowI = 0;
        auto settingsColumnEnd = [&](float x) {
            // Left column ends at its own right edge (leftX + colW), NOT x + colW: the rows are inset
            // ~28px on the left, so x + colW would push the content - and the selection box drawn
            // around it - right up against the vertical column divider. Ending at leftX + colW leaves
            // a clean gutter so the focus box clears the divider. Right column ends at the panel inset.
            const float dividerX = leftX + colW + gap * 0.5f;
            return (x < dividerX) ? (leftX + colW) : (padX + frameW - 28.0f * s);
        };
        auto settingsColumnW = [&](float x) {
            return std::max(80.0f * s, settingsColumnEnd(x) - x);
        };
        auto rowHit = [&](float x, float y, int idx) {
            const float cw = settingsColumnW(x);
            const float hx = x - 14.0f * s, hy = y - 8.0f * s;
            const float hw = cw + 14.0f * s, hh = 40.0f * s;
            menuHits_.push_back({ hx, hy, hw, hh, idx });
            if (menuSel_ == idx) menuFocusHalo(ui, hx, hy, hw, hh, 8.0f * s, pal::accentGlow, s);
        };
        auto sliderAt = [&](float x, float y, const std::string& label, const std::string& value,
                            float frac, bool hot) {
            const int idx = rowI++;
            const float cw = settingsColumnW(x);
            const float barX = x + 240.0f * s, valW = 80.0f * s;
            const float barW = std::max(44.0f * s, cw - 240.0f * s - valW - 16.0f * s);
            menuHits_.push_back({ barX, y - 2.0f * s, barW, 30.0f * s, 300 + idx });   // drag bar (pushed before the row)
            rowHit(x, y, idx);
            ui.textD(x, y, label, hot ? pal::textHi : rgb(199, 208, 220), fpx(ui, 24.0f, s));
            ui.rect(barX, y + 14.0f * s, barW, 6.0f * s, pal::lineSoft);
            ui.rect(barX, y + 14.0f * s, barW * clamp(frac, 0.0f, 1.0f), 6.0f * s,
                    hot ? pal::accent : rgb(58, 70, 86));
            ui.textRight(x + cw, y + 1.0f * s, value,
                         hot ? pal::textHi : pal::tierCommon, fpx(ui, 22.0f, s));
        };
        auto selectAt = [&](float x, float y, const std::string& label, const std::string& value, bool hot) {
            const float cw = settingsColumnW(x);
            rowHit(x, y, rowI++);
            ui.textD(x, y, label, pal::textMid, fpx(ui, 24.0f, s));
            ui.textRight(x + cw, y + 2.0f * s, "< " + value + " >",
                         hot ? pal::accent : pal::tierCommon, fpx(ui, 20.0f, s));
        };
        auto toggleAt = [&](float x, float y, const std::string& label, bool on, bool amberOn, bool hot = false) {
            const float cw = settingsColumnW(x);
            rowHit(x, y, rowI++);
            if (hot) ui.rect(x - 12.0f * s, y + 6.0f * s, std::max(3.0f, 3.0f * s), 22.0f * s, pal::accent);
            ui.textD(x, y, label, hot ? pal::textHi : pal::textMid, fpx(ui, 24.0f, s));
            const float segW = 58.0f * s, segH = 30.0f * s;
            const float sx = x + cw - segW * 2.0f;
            const uint32_t offCol = !on ? pal::accent : pal::textFaint;
            const uint32_t onCol = on ? (amberOn ? pal::amber : pal::accent) : pal::textFaint;
            ui.rectOutline(sx, y, segW, segH, hot && !on ? pal::accent : rgb(42, 51, 64), std::max(1.0f, 1.0f * s));
            ui.textCentered(sx + segW * 0.5f, y + 6.0f * s, "OFF", offCol, fpx(ui, 18.0f, s));
            ui.rectOutline(sx + segW, y, segW, segH, hot && on ? (amberOn ? pal::amber : pal::accent) : rgb(42, 51, 64), std::max(1.0f, 1.0f * s));
            ui.textCentered(sx + segW * 1.5f, y + 6.0f * s, "ON", onCol, fpx(ui, 18.0f, s));
        };

        const float panelY = bodyY - 24.0f * s, panelH = 556.0f * s;
        notchBRPanel(ui, padX, panelY, frameW, panelH, 18.0f * s, pal::surface0,
                     pal::border, std::max(1.0f, 1.2f * s));
        // Header rule sits just under the group title, well clear of the first row so the focused
        // row's selection box (which extends ~13px above its label) never crosses it.
        ui.rect(padX, panelY + 46.0f * s, frameW, 1.0f, pal::lineSoft);
        ui.rect(leftX + colW + gap * 0.5f, panelY + 76.0f * s, 1.0f, panelH - 104.0f * s, pal::lineSoft);
        // No per-row hairline dividers in Options: a single full-width rule spans BOTH columns at
        // one y, but the left and right columns run at different row pitches, so any such line cuts
        // through a row (or its ~50px selection halo) in one column or the other. Rows are delimited
        // by spacing, the selection halo, and their widgets; the panel keeps its header rule, the
        // column divider, and its border.
        auto lockedRow = [&](float x, float y, const std::string& label, const std::string& value) {
            const float cw = settingsColumnW(x);
            ui.textD(x, y, label, pal::textMid, fpx(ui, 24.0f, s));
            chamferOutline(ui, x + cw - 128.0f * s, y, 128.0f * s, 30.0f * s, 7.0f * s,
                           pal::amber, std::max(1.0f, 1.0f * s));
            ui.textCentered(x + cw - 64.0f * s, y + 6.0f * s, value, pal::amber, fpx(ui, 18.0f, s));
        };

        if (menuTab_ == 0) {
            groupTitle(leftX + 28.0f * s, panelY + 22.0f * s, "AUDIO", pal::textFaint);
            ui.textRight(padX + frameW - 28.0f * s, panelY + 24.0f * s,
                         "mix + accessibility", pal::textFaint, fpx(ui, 15.0f, s));
            sliderAt(leftX + 28.0f * s, bodyY + 42.0f * s, "Master volume", pct(settings_.masterVolume),
                     settings_.masterVolume, menuSel_ == 0);
            sliderAt(leftX + 28.0f * s, bodyY + 114.0f * s, "SFX volume", pct(settings_.sfxVolume),
                     settings_.sfxVolume, menuSel_ == 1);
            sliderAt(leftX + 28.0f * s, bodyY + 186.0f * s, "Music volume", pct(settings_.musicVolume),
                     settings_.musicVolume, menuSel_ == 2);
            static const char* const duckNames[3] = { "Off", "Subtle", "Strong" };
            const int di = std::min(2, std::max(0, static_cast<int>(std::lround(settings_.musicDuckDepth * 2.0f))));
            selectAt(rightX, bodyY + 42.0f * s, "Music duck", duckNames[di], menuSel_ == 3);
            toggleAt(rightX, bodyY + 96.0f * s, "Mono downmix", settings_.monoAudio, false, menuSel_ == 4);
            toggleAt(rightX, bodyY + 150.0f * s, "Reduced-intensity audio", settings_.reducedIntensityAudio, false, menuSel_ == 5);
            toggleAt(rightX, bodyY + 204.0f * s, "Combat readability", settings_.combatReadability, false, menuSel_ == 6);
        } else if (menuTab_ == 1) {
            groupTitle(leftX + 28.0f * s, panelY + 22.0f * s, "CONTROLS", pal::textFaint);
            ui.textRight(padX + frameW - 28.0f * s, panelY + 24.0f * s,
                         "keyboard + controller readable", pal::textFaint, fpx(ui, 15.0f, s));
            sliderAt(leftX + 28.0f * s, bodyY + 62.0f * s, "Look sensitivity", dec2(settings_.sensitivity),
                     (settings_.sensitivity - 0.25f) / 2.75f, menuSel_ == 0);
            toggleAt(leftX + 28.0f * s, bodyY + 132.0f * s, "Invert Y-axis", settings_.invertY, false, menuSel_ == 1);
            toggleAt(leftX + 28.0f * s, bodyY + 202.0f * s, "Aim input toggles", settings_.toggleAim, false, menuSel_ == 2);
            ui.text(rightX, bodyY + 72.0f * s, "Rebind capture panel: pending implementation.",
                    pal::amber, fpx(ui, 18.0f, s));
            ui.text(rightX, bodyY + 110.0f * s, "Conflicts will show next to the captured action.",
                    pal::textDim, fpx(ui, 18.0f, s));
        } else if (menuTab_ == 2) {
            static const char* const dmnames[2] = { "Windowed", "Fullscreen" };
            const int dmi = std::min(1, std::max(0, settings_.displayMode));
            groupTitle(leftX + 28.0f * s, panelY + 22.0f * s, "VIDEO", pal::textFaint);
            ui.textRight(padX + frameW - 28.0f * s, panelY + 24.0f * s,
                         std::to_string(w) + " x " + std::to_string(h), pal::textFaint, fpx(ui, 15.0f, s));
            selectAt(leftX + 28.0f * s, bodyY + 42.0f * s, "Display mode", dmnames[dmi], menuSel_ == 0);
            sliderAt(leftX + 28.0f * s, bodyY + 114.0f * s, "Field of view",
                     std::to_string(static_cast<int>(std::lround(settings_.fovDegrees))),
                     (settings_.fovDegrees - 70.0f) / 40.0f, menuSel_ == 1);
            selectAt(leftX + 28.0f * s, bodyY + 186.0f * s, "Graphics quality", qnames[qi], menuSel_ == 2);
            toggleAt(leftX + 28.0f * s, bodyY + 256.0f * s, "VSync", settings_.vsync, false, menuSel_ == 3);
            ui.text(rightX, bodyY + 42.0f * s, "Fullscreen is borderless: fills the screen, no title bar.",
                    pal::textDim, fpx(ui, 18.0f, s));
            ui.text(rightX, bodyY + 80.0f * s, "Low drops expensive screen-space effects.",
                    pal::textDim, fpx(ui, 18.0f, s));
            ui.text(rightX, bodyY + 118.0f * s, "High and Ultra request the RT tier when supported.",
                    pal::textDim, fpx(ui, 18.0f, s));
        } else if (menuTab_ == 3) {
            groupTitle(leftX + 28.0f * s, panelY + 22.0f * s, "ACCESSIBILITY", pal::amber);
            ui.textRight(padX + frameW - 28.0f * s, panelY + 24.0f * s,
                         "glyphs + labels always on", pal::amber, fpx(ui, 15.0f, s));
            sliderAt(leftX + 28.0f * s, bodyY + 42.0f * s, "Text scale", pct(settings_.textScale),
                     (settings_.textScale - 0.85f) / 0.45f, menuSel_ == 0);
            sliderAt(leftX + 28.0f * s, bodyY + 86.0f * s, "HUD scale", pct(settings_.hudScale),
                     (settings_.hudScale - 0.85f) / 0.35f, menuSel_ == 1);
            selectAt(leftX + 28.0f * s, bodyY + 130.0f * s, "Colorblind preset", cbnames[cbi], menuSel_ == 2);
            toggleAt(leftX + 28.0f * s, bodyY + 174.0f * s, "High-contrast HUD", settings_.highContrast, false, menuSel_ == 3);
            toggleAt(rightX, bodyY + 42.0f * s, "Reduce flashes", settings_.reduceFlashes, false, menuSel_ == 4);
            toggleAt(rightX, bodyY + 86.0f * s, "Reduce motion", settings_.reduceMotion, false, menuSel_ == 5);
            toggleAt(rightX, bodyY + 130.0f * s, "Reduce bloom", settings_.reduceBloom, false, menuSel_ == 6);
            sliderAt(rightX, bodyY + 174.0f * s, "Screen shake", pct(settings_.shakeScale),
                     settings_.shakeScale / 1.5f, menuSel_ == 7);
            lockedRow(rightX, bodyY + 232.0f * s, "Element + rarity glyphs", "ON");
        } else {
            static const char* const reticles[3] = { "Dynamic cross", "Static cross", "Dot" };
            const int ri = std::min(2, std::max(0, settings_.reticleStyle));
            groupTitle(leftX + 28.0f * s, panelY + 22.0f * s, "GAMEPLAY", pal::textFaint);
            ui.textRight(padX + frameW - 28.0f * s, panelY + 24.0f * s,
                         "comfort and aiming", pal::textFaint, fpx(ui, 15.0f, s));
            selectAt(leftX + 28.0f * s, bodyY + 62.0f * s, "Reticle style", reticles[ri], menuSel_ == 0);
            toggleAt(leftX + 28.0f * s, bodyY + 132.0f * s, "Aim input toggles", settings_.toggleAim, false, menuSel_ == 1);
            ui.text(rightX, bodyY + 72.0f * s, "Gameplay-critical states are labelled, not color-only.",
                    pal::textDim, fpx(ui, 18.0f, s));
        }

        ui.textCentered(cx, h - 46.0f * s,
                        "Q / E tabs    UP / DOWN select    LEFT / RIGHT adjust    ENTER apply    ESC back",
                        pal::textFaint, fpx(ui, 20.0f, s));
        return;
    }

    // ---- shared navigation row -------------------------------------------
    // kind: 0 normal, 1 framed-destructive (orange spine + caution + helper),
    //       2 unframed warm-slate destructive (quit to desktop). The focused row
    //       grows + gains the 4px cyan spine, a 2px cyan edge, and a chevron.
    auto navRow = [&](float x, float y, float rw, float rh, const std::string& label,
                      const std::string& helper, bool focused, int kind) {
        // Nav labels use the DISPLAY face (Chakra Petch), like the doc - clean proportional, not mono.
        const float lblSc = fpx(ui, 26.0f, s);
        const float helpSc = fpx(ui, 14.0f, s);
        const float ty = y + (rh - ui.dLineHeight(lblSc)) * 0.5f;
        const float hy = y + (rh - ui.lineHeight(helpSc)) * 0.5f;
        if (kind == 2 && !focused) {                       // unframed, warm slate (quit-to-desktop)
            caution(ui, x + 12.0f * s, y + rh * 0.5f, 8.0f * s, pal::warn);
            ui.textD(x + 30.0f * s, ty, label, rgb(154, 132, 114), lblSc);
            if (!helper.empty())
                ui.textRight(x + rw, hy, helper, pal::destructive, helpSc);
            return;
        }
        const uint32_t fill = focused ? rgb(13, 38, 48) : rgb(12, 17, 24);   // doc: focused #0d2630, idle #0c1118
        const uint32_t bord = focused ? pal::accent : (kind == 1 ? rgb(58, 42, 26) : pal::border);
        chamferPanel(ui, x, y, rw, rh, cut, fill, bord, focused ? std::max(2.0f, 2.0f * s) : std::max(1.0f, 1.2f * s), true);
        if (focused)        ui.rect(x, y + cut, std::max(4.0f, 4.0f * s), rh - cut, pal::accent);
        else if (kind == 1) ui.rect(x, y + cut, std::max(3.0f, 3.0f * s), rh - cut, pal::warn);   // amber left edge
        float tx = x + 26.0f * s;
        if (kind == 1) { tx = x + 34.0f * s; caution(ui, x + 20.0f * s, y + rh * 0.5f, 8.0f * s, pal::warn); }
        ui.textD(tx, ty, label, focused ? pal::textHero : pal::textMid, lblSc);
        if (!helper.empty())
            ui.textRight(x + rw - (focused ? 30.0f * s : 16.0f * s), hy, helper,
                         kind == 1 ? rgb(122, 106, 76) : pal::textDim, helpSc);
        if (focused) chevron(ui, x + rw - 18.0f * s, y + rh * 0.5f, 7.0f * s, pal::accentGlow, std::max(2.0f, 2.0f * s));
    };
    auto shard = [&](float x, float y, float wv, float hv, uint32_t col) {
        const float sk = hv * 0.12f;
        const float hull[8] = { x + sk, y, x + wv + sk, y, x + wv, y + hv, x, y + hv };
        ui.convexFill(hull, 4, col);
    };

    const bool main = (menuScreen_ == MenuScreen::Main);

    // ---- Main menu --------------------------------------------------------
    // Title Direction A: Oscilloscope. 1920x1080 authored values scaled by uiScale().
    if (main) {
        const float W = static_cast<float>(w), H = static_cast<float>(h);
        ui.rect(0.0f, 0.0f, W, H, pal::deepBg);
        ui.gradientV(0.0f, 0.0f, W, H, rgb(8, 17, 27), rgb(3, 5, 10), 72);
        for (int i = 0; i < 34; ++i) {
            const float t = static_cast<float>(i) / 33.0f;
            const float bw = (760.0f + t * 820.0f) * s;
            const float bh = (520.0f + t * 420.0f) * s;
            ui.rect(0.0f + t * 8.0f * s, 110.0f * s + t * 6.0f * s, bw, bh,
                    rgba(20, 78, 98, static_cast<uint8_t>((1.0f - t) * 10.0f)));
        }
        {
            const float floorTop = H - H * 0.48f;
            for (int i = -18; i <= 18; ++i) {
                const float bx = W * 0.5f + static_cast<float>(i) * 70.0f * s;
                const float tx = W * 0.5f + static_cast<float>(i) * 12.0f * s;
                ui.line(tx, floorTop, bx, H, std::max(1.0f, 1.0f * s), rgba(56, 200, 232, 30));
            }
            for (int j = 0; j < 10; ++j) {
                const float t = static_cast<float>(j) / 9.0f;
                const float y = H - std::pow(t, 1.85f) * (H - floorTop);
                ui.line(0.0f, y, W, y, std::max(1.0f, 1.0f * s),
                        rgba(56, 200, 232, static_cast<uint8_t>(34.0f * (1.0f - t * 0.72f))));
            }
        }
        pulseWaveLong(ui, W * 0.38f, 485.0f * s, W * 0.62f, 105.0f * s,
                      withAlpha(pal::accent, 128), withAlpha(pal::accentGlow, 28), std::max(2.0f, 2.5f * s));
        scanlines(ui, W, H, s);
        for (int i = 0; i < 26; ++i) {
            const float t = static_cast<float>(i) / 25.0f;
            const uint8_t a = static_cast<uint8_t>(4.0f + t * t * 14.0f);
            ui.rect(0.0f, i * 8.0f * s, W, 8.0f * s + 1.0f, rgba(0, 0, 0, a));
            ui.rect(0.0f, H - (i + 1) * 10.0f * s, W, 10.0f * s + 1.0f, rgba(0, 0, 0, a));
            ui.rect(i * 10.0f * s, 0.0f, 10.0f * s + 1.0f, H, rgba(0, 0, 0, a));
            ui.rect(W - (i + 1) * 10.0f * s, 0.0f, 10.0f * s + 1.0f, H, rgba(0, 0, 0, a));
        }
        // (Removed the faint "FOUNDRY BAY 01" right-side structure: its chamfer outline +
        // horizontal segment lines read as an empty menu container rather than atmosphere.
        // The waveform, perspective grid, and corner brackets carry the right negative space.)
        screenBrackets(ui, W, H, s);

        const float padX = 92.0f * s, padY = 78.0f * s;
        const float mono13 = fpx(ui, 13.0f, s), mono15 = fpx(ui, 15.0f, s);
        float kx = padX;
        ui.textTracked(kx, padY, "DROP PROTOCOL", rgb(94, 126, 142), mono15, 3.6f * s);
        kx += ui.textTrackedWidth("DROP PROTOCOL", mono15, 3.6f * s) + 16.0f * s;
        ui.text(kx, padY, "/", pal::accent, mono15);
        kx += ui.textWidth("/", mono15) + 16.0f * s;
        ui.textTracked(kx, padY, "D-01", rgb(94, 126, 142), mono15, 3.6f * s);
        textDTrackedRight(ui, W - padX, padY, "SECTOR 01 . FOUNDRY", rgb(72, 96, 110), mono13, 3.1f * s);
        {
            const std::string lhs = "FLOW STATE ";
            const std::string rhs = "DORMANT";
            const float tr = 2.6f * s, y = padY + 26.0f * s;
            const float total = ui.textTrackedWidth(lhs, mono13, tr) + ui.textTrackedWidth(rhs, mono13, tr);
            const float x0 = W - padX - total;
            ui.textTracked(x0, y, lhs, rgb(72, 96, 110), mono13, tr);
            ui.textTracked(x0 + ui.textTrackedWidth(lhs, mono13, tr), y, rhs, pal::good, mono13, tr);
        }

        const float heroY = 214.0f * s;
        ui.rect(padX, heroY + 4.0f * s, 26.0f * s, 166.0f * s, withAlpha(pal::accentGlow, 44));
        gradientH(ui, padX, heroY + 4.0f * s, 26.0f * s, 166.0f * s, pal::accentGlow, rgb(11, 187, 224), 6);
        ui.textD(padX + 46.0f * s, heroY, "PULSE", pal::textHi, fpx(ui, 206.0f, s));
        const float ruleY = heroY + 194.0f * s;
        ui.rect(padX, ruleY, 54.0f * s, 3.0f * s, pal::amber);
        gradientH(ui, padX + 54.0f * s, ruleY, 706.0f * s, 2.0f * s, pal::accent, rgba(43, 214, 245, 0), 28);
        float sx0 = padX;
        ui.textTracked(sx0, ruleY + 22.0f * s, "FIRST DROP", rgb(219, 232, 240), fpx(ui, 16.0f, s), 3.2f * s);
        sx0 += ui.textTrackedWidth("FIRST DROP", fpx(ui, 16.0f, s), 3.2f * s) + 22.0f * s;
        const char* parts[3] = { "SIDEARM", "EMPTY HEAT", "SECTOR 01" };
        for (int i = 0; i < 3; ++i) {
            ui.text(sx0, ruleY + 22.0f * s, ".", rgb(57, 80, 93), fpx(ui, 16.0f, s));
            sx0 += ui.textWidth(".", fpx(ui, 16.0f, s)) + 22.0f * s;
            ui.textTracked(sx0, ruleY + 22.0f * s, parts[i], rgb(111, 138, 153), fpx(ui, 16.0f, s), 3.2f * s);
            sx0 += ui.textTrackedWidth(parts[i], fpx(ui, 16.0f, s), 3.2f * s) + 22.0f * s;
        }

        const float navX = padX, navY = 626.0f * s, navW = 640.0f * s;
        auto menuItem = [&](int idx, float y, float rh, const char* code, const char* label) {
            const bool focus = menuSel_ == idx;
            const bool quit = idx == 2;
            menuHits_.push_back({ navX, y, navW, rh, idx });   // clickable menu row
            if (focus) menuFocusHalo(ui, navX, y, navW, rh, (idx == 0 ? 18.0f : 16.0f) * s,
                                     quit ? pal::amber : pal::accentGlow, s);
            const float pc = (idx == 0 ? 18.0f : 16.0f) * s;
            const uint32_t ring = quit ? rgba(150, 110, 55, focus ? 190 : 72) : rgba(70, 140, 165, focus ? 230 : 56);
            const uint32_t fillA = quit ? rgba(24, 18, 10, 235) : rgba(13, 22, 30, 235);
            const uint32_t fillB = quit ? rgba(12, 10, 8, 218) : rgba(9, 15, 22, 218);
            if (focus) {
                chamferFill(ui, navX - 4.0f * s, y - 4.0f * s, navW + 8.0f * s, rh + 8.0f * s,
                            pc + 4.0f * s, quit ? rgba(255, 178, 77, 58) : rgba(43, 214, 245, 84));
            }
            chamferFill(ui, navX, y, navW, rh, pc, focus ? (quit ? pal::amber : pal::accent) : ring);
            chamferFill(ui, navX + (focus ? 2.0f : 1.0f) * s, y + (focus ? 2.0f : 1.0f) * s,
                        navW - (focus ? 4.0f : 2.0f) * s, rh - (focus ? 4.0f : 2.0f) * s,
                        std::max(1.0f, pc - (focus ? 2.0f : 1.0f) * s),
                        focus ? (quit ? rgba(48, 33, 16, 236) : rgba(13, 56, 72, 247)) : fillA);
            if (!focus) gradientH(ui, navX + 8.0f * s, y + 8.0f * s, navW - 16.0f * s, rh - 16.0f * s, fillA, fillB, 22);
            const float textY = y + (idx == 0 ? 23.0f : 20.0f) * s;
            ui.textTracked(navX + 34.0f * s, y + (idx == 0 ? 29.0f : 25.0f) * s, code,
                           focus ? (quit ? pal::amber : pal::accent) : (quit ? rgb(106, 86, 52) : rgb(63, 86, 99)),
                           fpx(ui, 13.0f, s), 2.6f * s);
            const float lx = navX + 106.0f * s;
            if (quit) caution(ui, lx - 22.0f * s, y + rh * 0.5f, 7.0f * s, focus ? pal::amber : rgb(106, 86, 52));
            ui.textD(lx + (quit ? 12.0f * s : 0.0f), textY, label,
                     focus ? (quit ? pal::amber : pal::textHero) : (quit ? rgb(230, 184, 115) : pal::textMid),
                     fpx(ui, idx == 0 ? 34.0f : 30.0f, s));
            if (focus) {
                textTrackedRight(ui, navX + navW - 34.0f * s, y + 30.0f * s, "[ENTER] . (A)",
                                 quit ? rgb(230, 184, 115) : rgb(95, 212, 236), fpx(ui, 15.0f, s), 2.7f * s);
            }
        };
        menuItem(0, navY, 78.0f * s, "01", "PLAY");
        menuItem(1, navY + 94.0f * s, 70.0f * s, "02", "OPTIONS");
        menuItem(2, navY + 180.0f * s, 70.0f * s, "03", "QUIT");

        const float footerTop = H - 162.0f * s;
        ui.rect(padX, footerTop, W - padX * 2.0f, 1.0f, rgba(90, 170, 200, 36));
        const float opX = padX, opY = footerTop + 22.0f * s, opW = 402.0f * s, opH = 66.0f * s;
        chamferFill(ui, opX, opY, opW, opH, 12.0f * s, rgba(11, 20, 28, 154));
        chamferOutline(ui, opX, opY, opW, opH, 12.0f * s, rgba(90, 170, 200, 56), std::max(1.0f, 1.0f * s));
        ui.rect(opX + 22.0f * s, opY + 14.0f * s, 3.0f * s, 38.0f * s, pal::accent);
        ui.textTracked(opX + 41.0f * s, opY + 13.0f * s, "CURRENT OPERATION", rgb(94, 126, 142), fpx(ui, 12.0f, s), 3.4f * s);
        ui.textD(opX + 41.0f * s, opY + 34.0f * s, bestScore_ <= 0 && runsEnded() <= 0 ? "FOUNDRY ENTRY" : ("BEST " + comma(bestScore_)),
                 pal::textHi, fpx(ui, 23.0f, s));
        const float hintY = footerTop + 42.0f * s;
        textTrackedRight(ui, W - padX - ui.textTrackedWidth("[UP DOWN] NAVIGATE", fpx(ui, 15.0f, s), 3.0f * s) - 34.0f * s,
                         hintY, "[ENTER] SELECT", pal::accent, fpx(ui, 15.0f, s), 3.0f * s);
        textTrackedRight(ui, W - padX, hintY, "[UP DOWN] NAVIGATE", rgb(94, 126, 142), fpx(ui, 15.0f, s), 3.0f * s);
        return;
    }

    // ---- Pause ------------------------------------------------------------
    menuBackdrop();
    const float titleX = 120.0f * s, titleY = 200.0f * s;
    ui.textTracked(titleX, titleY - 42.0f * s, "RUN INTERRUPT", rgb(94, 126, 142), fpx(ui, 14.0f, s), 3.0f * s);
    shard(titleX, titleY, 12.0f * s, 74.0f * s, pal::accent);
    glowTextD(ui, titleX + 32.0f * s, titleY - 7.0f * s, "PAUSED",
              withAlpha(pal::accentGlow, 62), pal::textHero, fpx(ui, 72.0f, s));
    ui.rect(titleX, titleY + 86.0f * s, 54.0f * s, 3.0f * s, pal::amber);
    gradientH(ui, titleX + 54.0f * s, titleY + 86.0f * s, 430.0f * s, 2.0f * s,
              pal::accent, rgba(43, 214, 245, 0), 24);

    const char* labels[5] = { "Resume", "Options", "Restart run", "Quit to menu", "Quit to desktop" };
    const char* help[5] = { "", "", "RESETS TO ROOM 1", "ENDS CURRENT RUN", "" };
    const float navX = titleX, navW = 600.0f * s;
    float ny = titleY + 122.0f * s;
    for (int i = 0; i < 5; ++i) {
        const bool focus = i == menuSel_;
        const bool awaiting = pendingPauseConfirm_ == i;
        if (i < 4) {
            const bool warn = i >= 2;
            const float rh = (focus ? 70.0f : 64.0f) * s;
            menuHits_.push_back({ navX, ny, navW, rh, i });   // clickable pause row
            if (focus) menuFocusHalo(ui, navX, ny, navW, rh, warn ? 0.0f : 12.0f * s,
                                     warn ? pal::amber : pal::accentGlow, s);
            const float c = warn ? 0.0f : 12.0f * s;
            chamferFill(ui, navX, ny, navW, rh, c, focus ? rgb(13, 38, 48) : rgb(12, 17, 24));
            chamferOutline(ui, navX, ny, navW, rh, c,
                           focus ? pal::accent : (warn ? rgb(58, 42, 26) : pal::border),
                           focus ? std::max(1.5f, 1.5f * s) : std::max(1.0f, 1.0f * s));
            if (focus) ui.rect(navX, ny + 12.0f * s, std::max(4.0f, 4.0f * s), rh - 12.0f * s, pal::accent);
            if (warn) ui.rect(navX, ny, std::max(3.0f, 3.0f * s), rh, pal::amber);
            ui.textD(navX + 26.0f * s, ny + (focus ? 19.0f : 18.0f) * s,
                     labels[i], focus ? pal::textHero : pal::tierCommon, fpx(ui, i == 0 ? 28.0f : 26.0f, s));
            const std::string htxt = awaiting ? "PRESS ENTER AGAIN" : help[i];
            if (!htxt.empty())
                ui.textTracked(navX + navW - ui.textTrackedWidth(htxt, fpx(ui, 16.0f, s), 1.0f * s) - 22.0f * s,
                               ny + 24.0f * s, htxt, awaiting ? pal::amber : rgb(122, 106, 76), fpx(ui, 16.0f, s), 1.0f * s);
            if (focus) ui.textRight(navX + navW - 24.0f * s, ny + 22.0f * s, ">", pal::accent, fpx(ui, 22.0f, s));
            ny += rh + 12.0f * s;
        } else {
            ny += 6.0f * s;
            menuHits_.push_back({ navX, ny - 4.0f * s, navW, 38.0f * s, i });   // clickable quit-to-desktop row
            caution(ui, navX + 12.0f * s, ny + 14.0f * s, 8.0f * s, pal::amber);
            ui.textTracked(navX + 38.0f * s, ny, labels[i],
                           focus ? pal::amber : pal::tierCommon, fpx(ui, 22.0f, s), 0.4f * s);
            if (awaiting)
                ui.textRight(navX + navW, ny, "PRESS ENTER AGAIN", pal::amber, fpx(ui, 15.0f, s));
        }
    }

    // Gather + sort the owned build BEFORE sizing the panel, so the panel fits its content
    // instead of a fixed 650px box that reads as an unfinished void when the build is small.
    auto tierColOf = [](ItemTier t) {
        switch (t) {
            case ItemTier::Legendary: return pal::tierLegendary;
            case ItemTier::Rare:      return pal::tierRare;
            case ItemTier::Uncommon:  return pal::tierUncommon;
            default:                  return pal::tierCommon;
        }
    };
    std::vector<std::pair<const ItemDef*, int>> items;
    for (const auto& kv : build_.inventory()) {
        const ItemDef* d = build_.find(kv.first);
        if (d) items.push_back({ d, kv.second });
    }
    std::sort(items.begin(), items.end(), [](const std::pair<const ItemDef*, int>& a,
                                             const std::pair<const ItemDef*, int>& b) {
        if (static_cast<int>(a.first->tier) != static_cast<int>(b.first->tier))
            return static_cast<int>(a.first->tier) > static_cast<int>(b.first->tier);
        return a.first->name < b.first->name;
    });
    const int rowH = 34;
    const int shownMax = std::min(static_cast<int>(items.size()), 8);
    const BuildStats& bs = build_.stats();
    std::vector<int> activeSets;
    for (int a = 1; a < static_cast<int>(Affinity::Count); ++a)
        if (bs.affinityCount[a] >= 2) activeSets.push_back(a);
    std::sort(activeSets.begin(), activeSets.end(), [&](int a, int b){ return bs.affinityCount[a] > bs.affinityCount[b]; });

    const float pw = 660.0f * s;
    // header(58) + 4 meta rows(168) + divider gap(40) + build header(34) + rows + sets + bottom pad.
    const float bodyH = 58.0f + 168.0f + 40.0f + 34.0f +
                        (shownMax > 0 ? shownMax * static_cast<float>(rowH) + 6.0f : 30.0f) +
                        (activeSets.empty() ? 0.0f : 54.0f) + 34.0f;
    const float ph = bodyH * s;
    const float px = w - 120.0f * s - pw, py = 200.0f * s;
    notchBRPanel(ui, px, py, pw, ph, 18.0f * s, pal::surface0, pal::border, std::max(1.0f, 1.2f * s));
    const float ix = px + 40.0f * s, rx = px + pw - 40.0f * s;
    ui.textTracked(ix, py + 34.0f * s, "CURRENT RUN", pal::textDim, fpx(ui, 18.0f, s), 3.0f * s);
    const int maxHp = std::max(1, effectiveMaxHealth());
    const int peakPct = static_cast<int>(std::lround(clamp(runPulsePeak_, 0.0f, 1.0f) * 100.0f));
    const std::string loc = run_.currentIsBoss()
        ? (std::string(biomeName(currentBiome_)) + " . Boss")
        : (std::string(biomeName(currentBiome_)) + " . Sector " + std::to_string(run_.sector() + 1) +
           " / Room " + std::to_string(run_.roomInSector()));
    float ry = py + 84.0f * s;
    auto metaRow = [&](const std::string& label, const std::string& value, uint32_t col) {
        ui.text(ix, ry, label, rgb(126, 138, 153), fpx(ui, 24.0f, s));
        ui.textRight(rx, ry, value, col, fpx(ui, 24.0f, s));
        ry += 42.0f * s;
    };
    metaRow("LOCATION", loc, pal::textHi);
    metaRow("HEALTH", std::to_string(player_.hp) + " / " + std::to_string(maxHp), pal::textHi);
    metaRow("PEAK PULSE", std::to_string(peakPct) + "%", pal::ionWhite);
    {
        // WEAPON: aspect rendered in amber (BURNLINE etc.) so it reads as a distinct modifier.
        std::string wv = activeWeaponDef().name;
        const WeaponAspect* asp = activeAspect();
        ui.text(ix, ry, "WEAPON", rgb(126, 138, 153), fpx(ui, 24.0f, s));
        if (asp) {
            const std::string an = std::string(" . ") + asp->name;
            const float aw = ui.textWidth(an, fpx(ui, 24.0f, s));
            ui.textRight(rx - aw, ry, wv, pal::textHi, fpx(ui, 24.0f, s));
            ui.textRight(rx, ry, an, pal::amber, fpx(ui, 24.0f, s));
        } else {
            ui.textRight(rx, ry, wv, pal::textHi, fpx(ui, 24.0f, s));
        }
        ry += 42.0f * s;
    }
    ui.rect(ix, ry + 12.0f * s, pw - 80.0f * s, 1.0f, pal::border);
    ry += 40.0f * s;
    ui.textTracked(ix, ry, "BUILD . " + std::to_string(build_.totalItems()) + " ITEMS",
                   pal::textDim, fpx(ui, 16.0f, s), 2.4f * s);
    ui.textRight(rx, ry, "[TAB] inspect", pal::textFaint, fpx(ui, 14.0f, s));
    ry += 34.0f * s;

    if (items.empty())
        ui.text(ix, ry, "no items yet . clear rooms to build your run", pal::textFaint, fpx(ui, 17.0f, s));
    int shown = 0;
    for (const auto& it : items) {
        if (shown >= shownMax) break;
        const ItemDef* d = it.first;
        ui.rect(ix, ry + 7.0f * s, 9.0f * s, 9.0f * s, tierColOf(d->tier));   // rarity swatch
        const float nx = ix + 22.0f * s;
        ui.text(nx, ry, d->name, pal::textHi, fpx(ui, 17.0f, s));
        float ax = nx + ui.textWidth(d->name, fpx(ui, 17.0f, s)) + 14.0f * s;   // tags flow right after the name
        if (d->affinity != Affinity::None) {
            const AffVis av = affVis(d->affinity);
            ui.text(ax, ry, av.name, av.col, fpx(ui, 14.0f, s));
            ax += ui.textWidth(av.name, fpx(ui, 14.0f, s)) + 12.0f * s;
        }
        if (it.second > 1) ui.text(ax, ry, "x" + std::to_string(it.second), pal::gold, fpx(ui, 15.0f, s));
        ui.textRight(rx, ry, d->blurb, pal::textMid, fpx(ui, 15.0f, s));
        ry += rowH * s;
        ++shown;
    }
    if (static_cast<int>(items.size()) > shownMax) {
        ui.text(ix + 22.0f * s, ry, "+ " + std::to_string(static_cast<int>(items.size()) - shownMax) + " more items",
                pal::textFaint, fpx(ui, 14.0f, s));
        ry += 26.0f * s;
    }
    // Active affinity sets (strongest first) - the build-identity readout.
    if (!activeSets.empty()) {
        ui.rect(ix, ry + 8.0f * s, pw - 80.0f * s, 1.0f, pal::lineSoft);
        float sx = ix; const float sy = ry + 28.0f * s;
        for (int a : activeSets) {
            const int n = bs.affinityCount[a];
            const AffVis av = affVis(static_cast<Affinity>(a));
            const bool act = n >= 3;
            std::string set = std::string(av.name) + " " + std::to_string(std::min(n, 5)) + "/5";
            if (act) set += " . " + std::string(affinitySetText(static_cast<Affinity>(a), 3)) + " ACTIVE";
            const float wseg = ui.textWidth(set, fpx(ui, 16.0f, s));
            if (sx > ix && sx + wseg > rx) break;
            ui.text(sx, sy, set, act ? av.col : withAlpha(av.col, 175), fpx(ui, 16.0f, s));
            sx += wseg + 28.0f * s;
        }
    }
    ui.textCentered(cx, h - 60.0f * s, "UP / DOWN select    ENTER select    ESC resume",
                    pal::textFaint, fpx(ui, 20.0f, s));
}

} // namespace pulse
