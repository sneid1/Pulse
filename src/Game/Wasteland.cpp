#include "Game/Wasteland.hpp"

#include "Engine/Core/ImageFile.hpp"
#include "Engine/Core/Log.hpp"
#include "Engine/Core/MeshFile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <system_error>
#include <utility>

namespace pulse {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 11); }
    int   range(int lo, int hi) { return hi <= lo ? lo : lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
    float frange(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(next() & 0xFFFFFF) / 16777215.0f); }
};

uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B9u ^ (b + 0x85EBCA6Bu + (a << 6) + (a >> 2));
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// Resolve an asset dir by walking up from the working directory (exe runs from build/).
std::string resolveDir(const std::string& rel) {
    const char* prefixes[] = { "", "../", "../../", "../../../" };
    for (const char* p : prefixes) {
        std::string c = std::string(p) + rel;
        if (std::filesystem::exists(c)) return c;
    }
    return rel;
}

constexpr const char* kPulseEnvironmentMeshyRoot = "assets/packs/pulse_environment/meshy";
constexpr const char* kPulseEnvironmentBaseRoot = "assets/packs/pulse_environment/base_assets";
constexpr const char* kPulseEnvironmentQuaterniusRoot = "assets/packs/pulse_environment/quaternius";

std::string slashPath(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::vector<std::string> findPackModelGlbs(const std::string& relRoot) {
    std::vector<std::string> out;
    const std::string resolvedRoot = resolveDir(relRoot);
    std::error_code ec;
    if (!std::filesystem::exists(resolvedRoot, ec)) return out;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(resolvedRoot, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::filesystem::path p = entry.path();
        const std::string filename = p.filename().string();
        const std::string ext = p.extension().string();
        if (filename != "model_glb.glb" && ext != ".glb") continue;
        std::filesystem::path rel = std::filesystem::relative(p, resolvedRoot, ec);
        if (ec) {
            out.push_back(slashPath(p.string()));
            ec.clear();
        } else {
            out.push_back(slashPath(relRoot + "/" + rel.generic_string()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool nameHas(const std::string& n, std::initializer_list<const char*> subs) {
    std::string l = n;
    std::transform(l.begin(), l.end(), l.begin(), [](char c){ return static_cast<char>(std::tolower(c)); });
    for (const char* s : subs) if (l.find(s) != std::string::npos) return true;
    return false;
}

bool isFloorDecalLike(const std::string& n) {
    return nameHas(n, {
        "barcode", "crosshair", "decal", "diagonal", "emblem", "hazard",
        "label", "marker", "marking", "number_", "serial", "stripe",
        "triangle", "warning"
    });
}

void extent(const std::vector<TileSubmesh>& subs, float& dx, float& dy, float& dz) {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const TileSubmesh& s : subs)
        for (const StaticVertex& v : s.verts) {
            lo[0] = std::min(lo[0], v.pos.x); hi[0] = std::max(hi[0], v.pos.x);
            lo[1] = std::min(lo[1], v.pos.y); hi[1] = std::max(hi[1], v.pos.y);
            lo[2] = std::min(lo[2], v.pos.z); hi[2] = std::max(hi[2], v.pos.z);
        }
    dx = hi[0] >= lo[0] ? hi[0] - lo[0] : 0.0f;
    dy = hi[1] >= lo[1] ? hi[1] - lo[1] : 0.0f;
    dz = hi[2] >= lo[2] ? hi[2] - lo[2] : 0.0f;
}

} // namespace

std::vector<Wasteland::Prop>& Wasteland::poolFor(Cat c) {
    switch (c) {
        case Cat::Rock:      return rocks_;
        case Cat::SmallRock: return smallRocks_;
        case Cat::Crate:     return crates_;
        case Cat::Tree:      return trees_;
        case Cat::Grass:     return grass_;
        default:             return structures_;
    }
}

MaterialHandle Wasteland::kitMaterial(Engine& engine, const std::string& dir, const std::string& mat) {
    const std::string key = dir + "|" + mat;
    for (const auto& kv : matCache_) if (kv.first == key) return kv.second;
    const auto loadTex = [&](const std::string& file, bool srgb) -> TextureHandle {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba = loadImageRGBA(resolveDir(dir + "/textures/" + file), w, h);
        if (rgba.empty()) return TextureHandle::Invalid;
        TextureData td; td.width = w; td.height = h; td.rgba = rgba.data(); td.srgb = srgb;
        return engine.createTexture(td);
    };
    // Try the colour map under either workflow's name (_diffuse / _baseColor) and any common
    // extension (some kits ship .jpeg / .jpg instead of .png); first hit wins.
    const auto firstTex = [&](std::initializer_list<const char*> suffixes, bool srgb) -> TextureHandle {
        for (const char* suf : suffixes)
            for (const char* ext : { ".png", ".jpg", ".jpeg" }) {
                const TextureHandle t = loadTex(mat + suf + ext, srgb);
                if (t != TextureHandle::Invalid) return t;
            }
        return TextureHandle::Invalid;
    };
    MaterialDesc d;
    d.baseColor = firstTex({ "_diffuse", "_baseColor" }, true);
    d.normal    = firstTex({ "_normal" }, false);
    d.metallic  = 0.0f;
    d.roughness = 0.82f;                                                     // stone/wood/foliage: leave some sheen so the sun sculpts it (0.96 read as flat clay)
    d.uvScale   = -0.30f;   // NEGATIVE = triplanar (world scale 0.30): kills the UV stretch/tiling on rock faces
    if (d.baseColor == TextureHandle::Invalid) d.baseColorFactor = { 0.45f, 0.43f, 0.40f, 1.0f };
    const MaterialHandle m = engine.createMaterial(d);
    matCache_.emplace_back(key, m);
    return m;
}

MaterialHandle Wasteland::groundMaterial(Engine& engine, const std::string& dir, const std::string& mat, float uvTile) {
    MaterialDesc d;
    const auto loadTex = [&](const std::string& suffix, bool srgb) -> TextureHandle {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba = loadImageRGBA(resolveDir(dir + "/textures/" + mat + suffix), w, h);
        if (rgba.empty()) return TextureHandle::Invalid;
        TextureData td; td.width = w; td.height = h; td.rgba = rgba.data(); td.srgb = srgb;
        return engine.createTexture(td);
    };
    d.baseColor = loadTex("_diffuse.png", true);
    if (d.baseColor == TextureHandle::Invalid) d.baseColor = loadTex("_baseColor.png", true);
    d.normal    = loadTex("_normal.png", false);
    d.metallic  = 0.0f;
    d.roughness = 0.84f;   // ground: a touch of sheen so wet-ish highlights read under the low sun
    (void)uvTile;   // the quad bakes tiled UVs; uvScale stays 1
    if (d.baseColor == TextureHandle::Invalid) d.baseColorFactor = { 0.40f, 0.40f, 0.38f, 1.0f };
    return engine.createMaterial(d);
}

void Wasteland::loadKit(Engine& engine, const std::string& dir, Cat cat, Mode mode,
                        float targetHeight, float coverFrac, float trunk) {
    const std::string gltf = resolveDir(dir + "/scene.gltf");
    const auto buildProp = [&](const std::string& name, std::vector<TileSubmesh>& subs) {
        float dx, dy, dz; extent(subs, dx, dy, dz);
        const float md = std::max(dx, std::max(dy, dz));
        if (md <= 1e-4f) return;
        const float scale = targetHeight / md;            // normalise the LARGEST dimension (kits
                                                          // vary wildly in units; scaling by height
                                                          // alone made wide cliffs enormous)
        Prop p; p.name = name;
        for (TileSubmesh& sm : subs) {
            for (StaticVertex& v : sm.verts) { v.pos.x *= scale; v.pos.y *= scale; v.pos.z *= scale; }
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3)   // raw glTF winding -> engine convention
                std::swap(sm.indices[i + 1], sm.indices[i + 2]);
            const MeshHandle mh = engine.createMesh({ sm.verts, sm.indices });
            p.parts.push_back({ mh, kitMaterial(engine, dir, sm.material), Mat4::identity() });
        }
        p.halfX = 0.5f * dx * scale; p.halfZ = 0.5f * dz * scale; p.height = dy * scale;
        // Collision footprint. Trees use the authored trunk radius. Real cover (coverFrac>0) gets a
        // circle INSCRIBED in its footprint (the smaller half-extent, tightened a touch) so the
        // boundary sits ON the visible rock and you can brush right past it - never an oversized
        // invisible wall. Pure decoration (coverFrac==0: small rocks, grass) gets NO collision.
        p.radius = trunk > 0.0f ? trunk
                 : coverFrac > 0.0f ? std::max(0.3f, std::min(p.halfX, p.halfZ) * 0.9f)
                 : 0.0f;
        if (!p.parts.empty()) poolFor(cat).push_back(std::move(p));
    };

    if (mode == Mode::Merged) {
        KitTile whole;
        if (loadGltfWhole(gltf, whole)) buildProp(dir, whole.submeshes);
        return;
    }
    std::vector<KitTile> tiles;
    if (!loadGltfTiles(gltf, tiles)) return;
    for (KitTile& t : tiles) {
        // Skip ground/ceiling/water pieces bundled in a rock/scenery set - we want only props.
        if (nameHas(t.name, { "floor", "ceiling", "ground", "plane", "water", "terrain" })) continue;
        if (t.submeshes.empty()) continue;
        buildProp(t.name, t.submeshes);
    }
}

TextureHandle Wasteland::loadKitTexture(Engine& engine, const std::string& file, bool srgb) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba = loadImageRGBA(resolveDir(file), w, h);
    if (rgba.empty()) return TextureHandle::Invalid;
    TextureData td; td.width = w; td.height = h; td.rgba = rgba.data(); td.srgb = srgb;
    return engine.createTexture(td);
}

// Load a real-asset glTF (a Sketchfab sci-fi kit piece) with its AUTHORED PBR materials - base
// colour + normal textures and the metallic/roughness/emissive factors the engine loader now
// exposes per submesh - normalised to targetSize and centred at origin, as reusable submesh draws.
Wasteland::KitProp Wasteland::loadKitProp(Engine& engine, const std::string& gltfPath, float targetSize) {
    KitProp prop;
    prop.source = slashPath(gltfPath);
    const std::string resolved = resolveDir(gltfPath);
    KitTile tile;
    if (!loadGltfWhole(resolved, tile)) return prop;
    const float md = std::max(tile.sizeX, std::max(tile.sizeY, tile.sizeZ));
    const float scale = (md > 1e-4f && targetSize > 0.0f) ? targetSize / md : 1.0f;
    const std::string dir = std::filesystem::path(resolved).parent_path().string();
    const auto decode = [](const std::string& s) {
        std::string o; o.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) { o.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16))); i += 2; }
            else o.push_back(s[i]);
        }
        return o;
    };
    const std::string gltfNorm = slashPath(gltfPath);
    const bool meshyGenerated = gltfNorm.find("meshy_generated_models") != std::string::npos ||
                                gltfNorm.find("assets/packs/pulse_environment/meshy") != std::string::npos ||
                                gltfNorm.find("pulse_environment/meshy") != std::string::npos;
    const auto downsampleRgba = [](std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, uint32_t maxDim) {
        if (maxDim == 0) return;
        while (!rgba.empty() && (w > maxDim || h > maxDim) && w > 1 && h > 1) {
            const uint32_t nw = std::max(1u, w / 2u);
            const uint32_t nh = std::max(1u, h / 2u);
            std::vector<uint8_t> out(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4u);
            for (uint32_t y = 0; y < nh; ++y) {
                for (uint32_t x = 0; x < nw; ++x) {
                    uint32_t sum[4] = { 0, 0, 0, 0 };
                    uint32_t count = 0;
                    for (uint32_t oy = 0; oy < 2; ++oy)
                        for (uint32_t ox = 0; ox < 2; ++ox) {
                            const uint32_t sx = std::min(w - 1u, x * 2u + ox);
                            const uint32_t sy = std::min(h - 1u, y * 2u + oy);
                            const size_t si = (static_cast<size_t>(sy) * w + sx) * 4u;
                            for (int c = 0; c < 4; ++c) sum[c] += rgba[si + static_cast<size_t>(c)];
                            ++count;
                        }
                    const size_t di = (static_cast<size_t>(y) * nw + x) * 4u;
                    for (int c = 0; c < 4; ++c) out[di + static_cast<size_t>(c)] = static_cast<uint8_t>(sum[c] / count);
                }
            }
            rgba.swap(out);
            w = nw; h = nh;
        }
    };
    const auto textureFromMemory = [&](const std::vector<uint8_t>& bytes, bool srgb, uint32_t maxDim) -> TextureHandle {
        if (bytes.empty()) return TextureHandle::Invalid;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba = loadImageRGBAFromMemory(bytes.data(), bytes.size(), w, h);
        if (rgba.empty()) return TextureHandle::Invalid;
        downsampleRgba(rgba, w, h, maxDim);
        TextureData td; td.width = w; td.height = h; td.rgba = rgba.data(); td.srgb = srgb;
        return engine.createTexture(td);
    };
    const auto textureFromSource = [&](const std::string& uri, const std::vector<uint8_t>& bytes, bool srgb,
                                       uint32_t embeddedMaxDim) -> TextureHandle {
        if (!bytes.empty()) return textureFromMemory(bytes, srgb, embeddedMaxDim);
        if (uri.empty()) return TextureHandle::Invalid;
        return loadKitTexture(engine, dir + "/" + decode(uri), srgb);
    };
    for (TileSubmesh& sm : tile.submeshes) {
        if (sm.verts.empty()) continue;
        for (StaticVertex& v : sm.verts) { v.pos.x *= scale; v.pos.y *= scale; v.pos.z *= scale; }
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) std::swap(sm.indices[i + 1], sm.indices[i + 2]);
        const MeshHandle mh = engine.createMesh({ sm.verts, sm.indices });
        MaterialDesc d;
        d.baseColorFactor = sm.baseColorFactor;
        d.metallic = sm.metallic; d.roughness = sm.roughness;
        d.metalScale = meshyGenerated ? 0.72f : 1.0f;
        d.roughBoost = meshyGenerated ? 0.10f : 0.0f;
        d.baseColor = textureFromSource(sm.baseColorTex, sm.baseColorImage, true, meshyGenerated ? 1024u : 4096u);
        if (!meshyGenerated) {
            d.normal = textureFromSource(sm.normalTex, sm.normalImage, false, 2048u);
            d.orm    = textureFromSource(sm.ormTex, sm.ormImage, false, 2048u);
        }
        const bool hasEmissiveMap = !sm.emissiveTex.empty() || !sm.emissiveImage.empty();
        if (hasEmissiveMap) {
            d.emissiveTex = textureFromSource(sm.emissiveTex, sm.emissiveImage, true, meshyGenerated ? 512u : 2048u);
            d.emissiveTexStrength = meshyGenerated ? 1.6f : 2.5f;
        }
        const float em = std::max(sm.emissiveFactor.x, std::max(sm.emissiveFactor.y, sm.emissiveFactor.z));
        if (em > 0.02f && !hasEmissiveMap) {
            d.emissive = em * 2.5f;
            d.baseColorFactor = { sm.emissiveFactor.x, sm.emissiveFactor.y, sm.emissiveFactor.z, 1.0f };
        }
        const MaterialHandle mat = engine.createMaterial(d);
        prop.parts.push_back({ mh, mat, Mat4::identity() });
    }
    prop.halfX = 0.5f * tile.sizeX * scale; prop.halfZ = 0.5f * tile.sizeZ * scale; prop.height = tile.sizeY * scale;
    return prop;
}

void Wasteland::placeKitProp(std::vector<DungeonDraw>& out, const KitProp& p,
                             float wx, float yOff, float wz, float yaw, float uniformScale) const {
    if (p.parts.empty()) return;
    const Mat4 xf = mul(scaling(uniformScale, uniformScale, uniformScale),
                        mul(rotationY(yaw), translation(wx, yOff, wz)));
    for (const DungeonDraw& part : p.parts) out.push_back({ part.mesh, part.material, xf });
}

void Wasteland::loadMeshyEnvironmentProps(Engine& engine) {
    for (MeshyBiomeProps& props : meshyProps_) {
        props.focals.clear();
        props.anchors.clear();
        props.floorDetails.clear();
        props.wallDetails.clear();
        props.baseAnchors.clear();
        props.baseFloorDetails.clear();
        props.baseWallDetails.clear();
    }
    meshyCommon_ = MeshyCommonProps{};

    const auto loadRel = [&](const std::string& rel, float targetSize) {
        if (!std::filesystem::exists(resolveDir(rel))) return KitProp{};
        return loadKitProp(engine, rel, targetSize);
    };

    int loaded = 0, loadedRound1 = 0, loadedRound3 = 0, loadedBase = 0;
    const auto noteLoaded = [&](const KitProp& prop) {
        ++loaded;
        if (prop.source.find("round_1_") != std::string::npos) ++loadedRound1;
        if (prop.source.find("round_3_") != std::string::npos) ++loadedRound3;
        if (prop.source.find("base_assets") != std::string::npos) ++loadedBase;
    };
    const auto sortModels = [](std::vector<std::string>& models) {
        std::stable_sort(models.begin(), models.end(),
                         [](const std::string& a, const std::string& b) {
                             const bool ar3 = a.find("round_3_") != std::string::npos;
                             const bool br3 = b.find("round_3_") != std::string::npos;
                             if (ar3 != br3) return ar3 > br3;
                             return a < b;
                         });
    };
    const auto loadFirst = [&](const char* commonRole, float targetSize) {
        std::vector<std::string> models =
            findPackModelGlbs(std::string(kPulseEnvironmentMeshyRoot) + "/common/" + commonRole);
        sortModels(models);
        for (const std::string& model : models) {
            if (model.find("round_3_") == std::string::npos) continue;
            KitProp prop = loadRel(model, targetSize);
            if (!prop.parts.empty()) {
                noteLoaded(prop);
                return prop;
            }
        }
        for (const std::string& model : models) {
            KitProp prop = loadRel(model, targetSize);
            if (!prop.parts.empty()) {
                noteLoaded(prop);
                return prop;
            }
        }
        return KitProp{};
    };
    const auto loadCommonPool = [&](const char* commonRole, std::vector<KitProp>& pool, float targetSize) {
        std::vector<std::string> models =
            findPackModelGlbs(std::string(kPulseEnvironmentMeshyRoot) + "/common/" + commonRole);
        sortModels(models);
        for (const std::string& model : models) {
            KitProp prop = loadRel(model, targetSize);
            if (!prop.parts.empty()) {
                noteLoaded(prop);
                pool.push_back(std::move(prop));
            }
        }
    };
    const auto loadPool = [&](const char* bucket, const char* role, std::vector<KitProp>& pool, float targetSize) {
        const std::vector<std::string> models =
            findPackModelGlbs(std::string(kPulseEnvironmentMeshyRoot) + "/" + bucket + "/" + role);
        for (const std::string& model : models) {
            KitProp prop = loadRel(model, targetSize);
            if (!prop.parts.empty()) {
                noteLoaded(prop);
                pool.push_back(std::move(prop));
            }
        }
    };
    const auto loadSharedPool = [&](const char* role, float targetSize,
                                    std::vector<KitProp> MeshyBiomeProps::* member) {
        std::vector<KitProp> shared;
        loadPool("shared", role, shared, targetSize);
        for (MeshyBiomeProps& props : meshyProps_) {
            std::vector<KitProp>& dst = props.*member;
            dst.insert(dst.end(), shared.begin(), shared.end());
        }
    };
    const auto loadBiome = [&](Biome biome, const char* bucket) {
        const size_t bi = static_cast<size_t>(biome);
        if (bi >= meshyProps_.size()) return;
        MeshyBiomeProps& props = meshyProps_[bi];
        loadPool(bucket, "focals", props.focals, 3.00f);
        loadPool(bucket, "anchors", props.anchors, 2.65f);
        loadPool(bucket, "floor_details", props.floorDetails, 2.55f);
        loadPool(bucket, "wall_details", props.wallDetails, 2.60f);
    };
    const auto loadBaseAsset = [&](const char* asset, float targetSize) {
        return loadRel(std::string(kPulseEnvironmentBaseRoot) + "/meshy_generated_models/" + asset + "/model_glb.glb",
                       targetSize);
    };
    const auto addBase = [&](Biome biome, const char* asset, float targetSize,
                             std::vector<KitProp> MeshyBiomeProps::* member) {
        const size_t bi = static_cast<size_t>(biome);
        if (bi >= meshyProps_.size()) return;
        KitProp prop = loadBaseAsset(asset, targetSize);
        if (!prop.parts.empty()) {
            noteLoaded(prop);
            (meshyProps_[bi].*member).push_back(std::move(prop));
        }
    };
    const auto assignBase = [&](const char* asset, float targetSize, KitProp& dst) {
        KitProp prop = loadBaseAsset(asset, targetSize);
        if (!prop.parts.empty()) {
            noteLoaded(prop);
            dst = std::move(prop);
        }
    };

    meshyCommon_.doorSide      = loadFirst("door_side", 2.45f);
    meshyCommon_.doorLintel    = loadFirst("door_lintel", 2.85f);
    meshyCommon_.doorThreshold = loadFirst("door_threshold", 3.25f);
    meshyCommon_.wallSeam      = loadFirst("wall_seam", 2.95f);
    meshyCommon_.wallAlcove    = loadFirst("wall_alcove", 2.55f);
    meshyCommon_.baseTrim      = loadFirst("base_trim", 2.55f);
    loadCommonPool("ceiling_duct", meshyCommon_.ceilingDucts, 3.20f);
    if (!meshyCommon_.ceilingDucts.empty()) meshyCommon_.ceilingDuct = meshyCommon_.ceilingDucts.front();
    meshyCommon_.ceilingSpine  = loadFirst("ceiling_spine", 3.20f);
    meshyCommon_.deckSupport   = loadFirst("deck_support", 2.55f);
    loadCommonPool("stair_finisher", meshyCommon_.stairFinishers, 2.95f);
    if (!meshyCommon_.stairFinishers.empty()) meshyCommon_.stairFinisher = meshyCommon_.stairFinishers.front();
    meshyCommon_.floorHatch    = loadFirst("floor_hatch", 2.40f);

    assignBase("base_env_007_door_side_machinery_cyan", 2.45f, meshyCommon_.doorSide);
    assignBase("base_env_009_floor_access_hatch_cyan", 2.40f, meshyCommon_.floorHatch);
    assignBase("base_env_013_platform_support_truss", 2.55f, meshyCommon_.deckSupport);
    assignBase("base_env_017_wall_seam_cover_tall_cyan", 2.95f, meshyCommon_.wallSeam);
    if (KitProp stair = loadBaseAsset("base_env_012_ramp_stair_finisher_cyan", 2.95f); !stair.parts.empty()) {
        noteLoaded(stair);
        meshyCommon_.stairFinishers.push_back(std::move(stair));
        meshyCommon_.stairFinisher = meshyCommon_.stairFinishers.front();
    }

    loadBiome(Biome::Rocky, "foundry");
    loadBiome(Biome::Forest, "furnace");
    loadBiome(Biome::Ruins, "reliquary");
    addBase(Biome::Rocky, "base_env_001_wall_panel_cyan", 2.60f, &MeshyBiomeProps::baseWallDetails);
    addBase(Biome::Rocky, "base_env_002_corner_column_cyan", 2.70f, &MeshyBiomeProps::baseAnchors);
    addBase(Biome::Rocky, "base_env_003_wall_power_spine_cyan", 2.60f, &MeshyBiomeProps::baseWallDetails);
    addBase(Biome::Rocky, "base_env_006_maintenance_alcove_cyan", 2.55f, &MeshyBiomeProps::baseAnchors);
    addBase(Biome::Rocky, "base_env_008_floor_tile_square_cyan", 2.40f, &MeshyBiomeProps::baseFloorDetails);
    addBase(Biome::Rocky, "base_env_014_platform_edge_cap_cyan", 2.45f, &MeshyBiomeProps::baseWallDetails);
    addBase(Biome::Rocky, "base_env_015_raised_deck_panel_cyan", 2.45f, &MeshyBiomeProps::baseFloorDetails);
    addBase(Biome::Rocky, "base_env_016_coolant_trench_grate_cyan", 2.45f, &MeshyBiomeProps::baseFloorDetails);
    addBase(Biome::Forest, "base_env_004_furnace_exhaust_vent_orange", 2.70f, &MeshyBiomeProps::baseWallDetails);
    addBase(Biome::Forest, "base_env_010_furnace_slag_trough_orange", 2.50f, &MeshyBiomeProps::baseFloorDetails);
    addBase(Biome::Ruins, "base_env_005_reliquary_wall_buttress_violet", 2.75f, &MeshyBiomeProps::baseWallDetails);
    addBase(Biome::Ruins, "base_env_011_reliquary_floor_sigil_violet", 2.45f, &MeshyBiomeProps::baseFloorDetails);
    loadSharedPool("focals", 3.00f, &MeshyBiomeProps::focals);
    loadSharedPool("anchors", 2.65f, &MeshyBiomeProps::anchors);
    loadSharedPool("floor_details", 2.55f, &MeshyBiomeProps::floorDetails);
    loadSharedPool("wall_details", 2.60f, &MeshyBiomeProps::wallDetails);

    for (MeshyBiomeProps& props : meshyProps_) {
        auto& floor = props.floorDetails;
        floor.erase(std::remove_if(floor.begin(), floor.end(),
                                   [](const KitProp& p) { return isFloorDecalLike(p.source); }),
                    floor.end());
    }

    logInfo("meshy environment: loaded %d props from %s + %s (round1=%d round3=%d base=%d)",
            loaded, kPulseEnvironmentMeshyRoot, kPulseEnvironmentBaseRoot, loadedRound1, loadedRound3, loadedBase);
}

// Load one tileset piece (floor/wall) as a single merged, DOUBLE-SIDED mesh: pull its geometry
// via loadGltfWhole (raw glTF winding -> swap to engine), scale to world units, then append a
// reversed + inset back shell so a flat single-sided kit quad is never see-through or back-face
// culled from either side (the inset avoids coincident-face z-fighting). The shared atlas material
// is applied separately. Returns the mesh + its world-space bounding extent (for grid/segment fit).
Wasteland::TileMesh Wasteland::loadTileMesh(Engine& engine, const std::string& gltfPath, float scale) {
    TileMesh tm;
    KitTile tile;
    if (!loadGltfWhole(resolveDir(gltfPath), tile)) return tm;
    std::vector<StaticVertex> v;
    std::vector<uint32_t>      idx;
    for (TileSubmesh& sm : tile.submeshes) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        for (StaticVertex sv : sm.verts) { sv.pos.x *= scale; sv.pos.y *= scale; sv.pos.z *= scale; v.push_back(sv); }
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {   // raw glTF -> engine winding (swap 1,2)
            idx.push_back(base + sm.indices[i]);
            idx.push_back(base + sm.indices[i + 2]);
            idx.push_back(base + sm.indices[i + 1]);
        }
    }
    const uint32_t nv = static_cast<uint32_t>(v.size());
    const float eps = 0.02f;   // back shell inset (world units) so the two sheets never z-fight
    for (uint32_t k = 0; k < nv; ++k) {
        StaticVertex sv = v[k];
        sv.pos.x -= sv.nrm.x * eps; sv.pos.y -= sv.nrm.y * eps; sv.pos.z -= sv.nrm.z * eps;
        sv.nrm = { -sv.nrm.x, -sv.nrm.y, -sv.nrm.z };
        sv.tangent = { -sv.tangent.x, -sv.tangent.y, -sv.tangent.z, sv.tangent.w };
        v.push_back(sv);
    }
    const uint32_t ni = static_cast<uint32_t>(idx.size());
    for (uint32_t k = 0; k + 2 < ni + 1 && k + 2 < ni; k += 3) {  // reversed winding for the back shell
        idx.push_back(idx[k] + nv);
        idx.push_back(idx[k + 2] + nv);
        idx.push_back(idx[k + 1] + nv);
    }
    if (v.empty() || idx.empty()) return tm;
    tm.mesh = engine.createMesh({ v, idx });
    tm.w = tile.sizeX * scale; tm.h = tile.sizeY * scale; tm.d = tile.sizeZ * scale;
    return tm;
}

// Load the shared sci-fi tileset atlas (one material) + the floor / plain-wall / cyan-trace-wall
// tile meshes. The atlas packs baseColor + ORM (occlusion=R, rough=G, metal=B) + a normal map +
// an emissive map (the cyan circuit traces), all mapping cleanly onto the engine material once
// emissive-texture support is in. Returns false (caller keeps the procedural shell) if absent.
bool Wasteland::loadTileset(Engine& engine) {
    const std::string dir = "assets/external/sketchfab_scifi/scifi_tileset";
    const auto tex = [&](const char* file, bool srgb) {
        return loadKitTexture(engine, dir + "/textures/" + file, srgb);
    };
    const TextureHandle base = tex("Scene_-_Root_baseColor.png", true);
    if (base == TextureHandle::Invalid) return false;
    MaterialDesc d;
    d.baseColor = base;
    d.normal    = tex("Scene_-_Root_normal.png", false);
    d.orm       = tex("Scene_-_Root_metallicRoughness.png", false);   // R=AO G=rough B=metal (kit reuses MR as occlusion)
    d.emissiveTex = tex("Scene_-_Root_emissive.png", true);
    d.emissiveTexStrength = 3.0f;            // lift the cyan traces into HDR so they bloom as neon
    d.metallic = 1.0f; d.roughness = 1.0f;   // overridden per-texel by the ORM map
    d.metalScale = 0.80f; d.roughBoost = 0.08f;   // tame the raw metal so walls read as lit surfaces, not dark mirrors
    d.baseColorFactor = { 1.18f, 1.12f, 1.04f, 1.0f };   // neutral-warm brighten so the weathered metal reads true
                                                         // (the arena's cool fog/ambient supplies the mood on top)
    tilesetMat_ = engine.createMaterial(d);

    const float s = wallTileH_ / 1200.0f;    // kit wall is 1200 units tall -> 4.5 m (0.00375)
    const TileMesh fl = loadTileMesh(engine, dir + "/floor/scene.gltf", s);
    const TileMesh wp = loadTileMesh(engine, dir + "/wall_plain/scene.gltf", s);
    const TileMesh wt = loadTileMesh(engine, dir + "/wall_traced/scene.gltf", s);
    floorTileMesh_ = fl.mesh; wallTileMesh_ = wp.mesh; wallTracedMesh_ = wt.mesh;
    if (fl.w > 0.1f) floorTileW_ = fl.w;
    if (fl.d > 0.1f) floorTileD_ = fl.d;
    if (wp.d > 0.1f) wallTileW_ = wp.d;      // wall WIDTH runs along the kit's Z (sizeZ)
    if (wp.h > 0.1f) wallTileH_ = wp.h;
    tilesetReady_ = floorTileMesh_ != MeshHandle::Invalid && wallTileMesh_ != MeshHandle::Invalid;
    return tilesetReady_;
}

// ---- Quaternius "Modular SciFi MegaKit" (CC0) loaders ------------------------------------------
// One engine material per unique texture set (the kit shares a handful of atlases across all 117
// walls / 55 platforms / columns), so the whole assembled world costs only a few materials.
MaterialHandle Wasteland::quatMaterial(Engine& engine, const std::string& dir, const TileSubmesh& sm) {
    const std::string key = sm.baseColorTex + "|" + sm.normalTex + "|" + sm.ormTex;
    if (key != "||")
        for (const auto& kv : quatMatCache_) if (kv.first == key) return kv.second;
    const auto decode = [](const std::string& s) {              // %20 etc. -> raw
        std::string o; o.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) { o.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16))); i += 2; }
            else o.push_back(s[i]);
        }
        return o;
    };
    // The kit's per-subfolder texture copies are INCOMPLETE; the full atlas set lives at the glTF
    // root (the parent of the piece's category folder). Resolve there first, fall back to the folder.
    const std::string root = std::filesystem::path(dir).parent_path().string();
    const auto tex = [&](const std::string& uri, bool srgb) -> TextureHandle {
        if (uri.empty()) return TextureHandle::Invalid;
        const std::string file = decode(uri);
        const TextureHandle r = loadKitTexture(engine, root + "/" + file, srgb);
        return r != TextureHandle::Invalid ? r : loadKitTexture(engine, dir + "/" + file, srgb);
    };
    MaterialDesc d;
    d.baseColor = tex(sm.baseColorTex, true);
    d.normal    = tex(sm.normalTex, false);
    d.orm       = tex(sm.ormTex, false);     // T_*_ORM.png: R=AO G=rough B=metal (engine ORM convention)
    d.baseColorFactor = sm.baseColorFactor;
    const auto darkenKit = [&](float r, float g, float b) {
        d.baseColorFactor.x *= r;
        d.baseColorFactor.y *= g;
        d.baseColorFactor.z *= b;
    };
    // Several Quaternius atlases contain pale trim blocks that blow out under PULSE's warm
    // interior lights. Keep the authored texture detail, but pull the non-emissive kit albedo
    // into a darker industrial range so panels read as material, not placeholder cubes.
    if (sm.baseColorTex.find("T_Trim_02") != std::string::npos) darkenKit(0.48f, 0.42f, 0.36f);
    else if (sm.baseColorTex.find("T_PaddedWall") != std::string::npos) darkenKit(0.56f, 0.48f, 0.42f);
    else if (sm.baseColorTex.find("T_Trim_01") != std::string::npos) darkenKit(0.72f, 0.70f, 0.68f);
    else if (sm.baseColorTex.find("T_Trim_03") != std::string::npos) darkenKit(0.70f, 0.68f, 0.66f);
    d.metallic = sm.metallic; d.roughness = sm.roughness;
    // The Quaternius atlas can look a little plastic under the PULSE lighting pass. Keep the
    // authored texture identity, but bias it toward rougher industrial metal so kit panels sit
    // with the PolyHaven-backed procedural shell instead of popping as glossy demo props.
    d.metalScale = 0.82f;
    d.roughBoost = 0.08f;
    if (d.baseColor == TextureHandle::Invalid && d.orm == TextureHandle::Invalid) {
        d.roughness = 0.3f; d.metallic = 0.0f;   // untextured (e.g. glass): keep the factor, light sheen
    }
    const MaterialHandle m = engine.createMaterial(d);
    quatMatCache_.emplace_back(key, m);
    return m;
}

// Load one kit piece as reusable submesh draws (mesh + shared material) + its world bbox extent.
// The kit is metric (1 unit = 1 m) so no scaling; loadGltfWhole re-centres the footprint to the
// origin (X/Z) and the floor to Y=0, which the assembler accounts for when placing.
Wasteland::QuatPiece Wasteland::loadQuatPiece(Engine& engine, const std::string& gltfPath) {
    QuatPiece piece;
    const std::string resolved = resolveDir(gltfPath);
    KitTile tile;
    if (!loadGltfWhole(resolved, tile)) return piece;
    const std::string dir = std::filesystem::path(resolved).parent_path().string();
    for (TileSubmesh& sm : tile.submeshes) {
        if (sm.verts.empty()) continue;
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) std::swap(sm.indices[i + 1], sm.indices[i + 2]);
        const MeshHandle mh = engine.createMesh({ sm.verts, sm.indices });
        piece.parts.push_back({ mh, quatMaterial(engine, dir, sm), Mat4::identity() });
    }
    piece.sizeX = tile.sizeX; piece.sizeY = tile.sizeY; piece.sizeZ = tile.sizeZ;
    return piece;
}

void Wasteland::placeQuat(std::vector<DungeonDraw>& out, const QuatPiece& p, float wx, float yOff, float wz, float yaw) {
    const Mat4 xf = mul(rotationY(yaw), translation(wx, yOff, wz));
    for (const DungeonDraw& part : p.parts) out.push_back({ part.mesh, part.material, xf });
}

void Wasteland::scanQuatAssets() {
    quatAssetRelPath_.clear();
    struct RootSpec {
        std::string root;
        std::string aliasPrefix;
    };
    const std::string essentialsRoot = std::string(kPulseEnvironmentQuaterniusRoot) + "/Sci-Fi Essentials Kit[Pro]/glTF";
    const RootSpec roots[] = {
        { quatBase_, "" },
        { essentialsRoot, "Essentials_" },
    };
    int rootCount = 0;
    for (const RootSpec& spec : roots) {
        const std::string root = resolveDir(spec.root);
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        ++rootCount;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec || !e.is_regular_file()) continue;
            if (e.path().extension().string() != ".gltf") continue;
            const std::string name = e.path().stem().string();
            const std::filesystem::path relPath = std::filesystem::relative(e.path(), root, ec);
            if (ec) { ec.clear(); continue; }
            const std::string rel = slashPath(relPath.generic_string());
            if (quatAssetRelPath_.find(name) == quatAssetRelPath_.end())
                quatAssetRelPath_.emplace(name, QuatAssetRef{ spec.root, rel });
            if (!spec.aliasPrefix.empty()) {
                const std::string alias = spec.aliasPrefix + name;
                if (quatAssetRelPath_.find(alias) == quatAssetRelPath_.end())
                    quatAssetRelPath_.emplace(alias, QuatAssetRef{ spec.root, rel });
            }
        }
    }
    logInfo("quaternius: indexed %d glTF asset keys from %d kit roots",
            static_cast<int>(quatAssetRelPath_.size()), rootCount);
}

std::string Wasteland::resolveQuatAssetName(const std::string& name) const {
    if (name.empty()) return {};
    if (quatAssetRelPath_.find(name) != quatAssetRelPath_.end()) return name;
    std::string key = name;
    if (key.find('/') != std::string::npos || key.find('\\') != std::string::npos ||
        std::filesystem::path(key).has_extension())
        key = std::filesystem::path(key).stem().string();
    if (quatAssetRelPath_.find(key) != quatAssetRelPath_.end()) return key;
    const std::string straight = key + "_Straight";
    if (quatAssetRelPath_.find(straight) != quatAssetRelPath_.end()) return straight;
    return {};
}

// Load (and cache) any kit piece by bare name (e.g. "Prop_Pod", "WallPipe_Straight"). The
// filename registry is built once from the whole kit, so room data can name any category without
// hardcoded category search loops.
const Wasteland::QuatPiece* Wasteland::quatPieceByName(Engine& engine, const std::string& name) {
    const std::string resolved = resolveQuatAssetName(name);
    if (resolved.empty()) return nullptr;
    for (auto& kv : quatPieceCache_) if (kv.first == resolved) return &kv.second;
    const auto it = quatAssetRelPath_.find(resolved);
    if (it == quatAssetRelPath_.end()) return nullptr;
    QuatPiece p = loadQuatPiece(engine, it->second.root + "/" + it->second.rel);
    if (!p.parts.empty()) { quatPieceCache_.emplace_back(resolved, std::move(p)); return &quatPieceCache_.back().second; }
    return nullptr;
}

const Wasteland::QuatPiece* Wasteland::cachedPiece(const std::string& name) const {
    const std::string resolved = resolveQuatAssetName(name);
    if (resolved.empty()) return nullptr;
    for (const auto& kv : quatPieceCache_) if (kv.first == resolved) return &kv.second;
    return nullptr;
}

// A header may name a piece either fully ("WallAstra_Straight_Broken", "Platform_DarkPlates",
// "Prop_Pod") or by the family/cap/cover stem that takes a "_Straight" panel ("WallPipe",
// "TopAstra", "ShortWall_Band2"). Try the bare name, then the "_Straight" form.
const Wasteland::QuatPiece* Wasteland::resolveCached(const std::string& name) const {
    if (name.empty()) return nullptr;
    if (const QuatPiece* p = cachedPiece(name)) return p;
    return cachedPiece(name + "_Straight");
}

// Parse hand-crafted room templates from config/pulse.rooms (the design-agent output format):
// blocks of `KEY: value` header lines + a `GRID:` section whose rows are PIPE-DELIMITED (|...|) so
// leading/trailing void spaces survive editors and paste. Lines starting with // are comments.
bool Wasteland::loadRoomTemplates(const std::string& path) {
    std::ifstream f(resolveDir(path));
    if (!f) return false;
    roomTemplates_.clear();
    const auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r"); size_t b = s.find_last_not_of(" \t\r");
        return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    };
    const auto upper = [](std::string s) { for (char& c : s) c = static_cast<char>(std::toupper(c)); return s; };
    const auto splitList = [&](const std::string& v) {
        std::vector<std::string> out;
        size_t a = 0;
        while (a <= v.size()) {
            const size_t b = v.find(',', a);
            std::string item = trim(v.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (!item.empty()) out.push_back(std::move(item));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    };
    const auto parseSize = [&](const std::string& v) {
        const std::string u = upper(trim(v));
        if (u == "SMALL") return AreaSize::Small;
        if (u == "BIG") return AreaSize::Big;
        if (u == "CORRIDOR") return AreaSize::Corridor;
        return AreaSize::Mid;
    };
    const auto parseBiome = [&](const std::string& v) {
        const std::string u = upper(trim(v));
        if (u == "FOUNDRY" || u == "ROCKY")  return Biome::Rocky;
        if (u == "FURNACE" || u == "FOREST") return Biome::Forest;
        if (u == "RELIQUARY" || u == "RUINS") return Biome::Ruins;
        return Biome::Count;   // unspecified -> matches any sector
    };
    RoomTemplate cur; bool have = false, inGrid = false;
    int total = 0;
    const auto flush = [&]() {
        if (have && !cur.grid.empty()) {
            ++total;
            const std::string err = validateRoomTemplate(cur);   // step-9 import validation
            if (err.empty()) roomTemplates_.push_back(cur);
            else logWarn("rooms: FAIL '%s' - %s (skipped)", cur.name.c_str(), err.c_str());
        }
        cur = RoomTemplate(); have = false; inGrid = false;
    };
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (inGrid) {                                   // grid rows: take the text between the pipes
            const size_t a = line.find('|');
            if (a != std::string::npos) {
                const size_t b = line.rfind('|');
                if (b > a) cur.grid.push_back(line.substr(a + 1, b - a - 1));
                continue;
            }
            inGrid = false;                             // a non-pipe line ends the grid
        }
        const std::string t = trim(line);
        if (t.empty() || t.rfind("//", 0) == 0) continue;
        const size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = upper(trim(t.substr(0, colon)));
        const std::string val = trim(t.substr(colon + 1));
        if (key == "NAME") { flush(); cur.name = val; have = true; }
        else if (key == "SIZE")   cur.size = parseSize(val);
        else if (key == "BIOME")  cur.biome = parseBiome(val);
        else if (key == "FAMILY") cur.family = val;
        else if (key == "FLOOR")  cur.floor = val;
        else if (key == "TOP")    cur.top = val;
        else if (key == "COVER")  cur.cover = val;
        else if (key == "PILLAR") cur.pillar = val;
        else if (key == "FOCAL")  cur.focal = val;
        else if (key == "DAIS")   cur.daisFloor = val;
        else if (key == "RAMP")   cur.ramp = val;
        else if (key == "DOOR_FRAME") cur.doorFrame = val;
        else if (key == "DOOR_LEAF")  cur.doorLeaf = val;
        else if (key == "CRATE")      cur.cratePiece = val;
        else if (key == "DRESSING_POOL") cur.dressingPool = splitList(val);
        else if (key == "DECAL_GROUP")   cur.decalGroup = splitList(val);
        else if (key == "GRID")   inGrid = true;
    }
    flush();
    logInfo("rooms: loaded %d of %d templates from %s", static_cast<int>(roomTemplates_.size()), total, path.c_str());
    return !roomTemplates_.empty();
}

// Step-9 import validation (the buildContract). Operates on the raw ASCII grid (rows right-padded
// with void to the max width): exactly one 'E', the size's exit count, equal-width rows, valid
// ramp affordances, and a reachability flood from 'E'. Returns "" on success, else the first
// failing reason.
std::string Wasteland::validateRoomTemplate(const RoomTemplate& T) const {
    if (T.grid.empty()) return "empty grid";
    const int H = static_cast<int>(T.grid.size());
    int W = 0; for (const std::string& r : T.grid) W = std::max(W, static_cast<int>(r.size()));
    const int srcW = static_cast<int>(T.grid[0].size());
    const auto at = [&](int i, int j) -> char {
        if (i < 0 || j < 0 || i >= W || j >= H) return ' ';
        const std::string& r = T.grid[static_cast<size_t>(j)];
        return (i >= static_cast<int>(r.size())) ? ' ' : r[static_cast<size_t>(i)];
    };
    // equal-width rows (after the source rows, before padding - the contract authors them equal).
    for (const std::string& r : T.grid)
        if (static_cast<int>(r.size()) != srcW) return formatStr("rows not equal width (%d vs %d)", static_cast<int>(r.size()), srcW);

    // count entries/exits; locate the entry.
    int nE = 0, nD = 0, ei = -1, ej = -1;
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const char c = at(i, j);
            if (c == 'E') { ++nE; ei = i; ej = j; }
            else if (c == 'D') ++nD;
        }
    if (nE != 1) return formatStr("found %d 'E' entries, want exactly 1", nE);
    int wantD0 = 2, wantD1 = 2;   // Small
    switch (T.size) {
        case AreaSize::Mid:      wantD0 = wantD1 = 3; break;
        case AreaSize::Big:      wantD0 = wantD1 = 3; break;
        case AreaSize::Corridor: wantD0 = 1; wantD1 = 2; break;
        default:                 wantD0 = wantD1 = 2; break;  // Small
    }
    if (nD < wantD0 || nD > wantD1)
        return formatStr("found %d 'D' exits, want %s for size", nD,
                         (wantD0 == wantD1) ? formatStr("%d", wantD0).c_str() : formatStr("%d-%d", wantD0, wantD1).c_str());

    // reachability flood: passable = . c = p / D E H; blockers = void # o X. An 'H' may only be
    // ENTERED from an 'H' or a '/' (climb via ramp); every other passable adjacency is walkable.
    // A ramp also needs an actual low-side approach cell opposite its adjacent H; otherwise it can
    // validate as reachable while visually/physically backing straight into a wall.
    const auto passable = [](char c) {
        return c == '.' || c == 'c' || c == '=' || c == 'p' || c == '/' || c == 'D' || c == 'E' || c == 'H';
    };
    const int dx[4] = { 0, 0, -1, 1 }, dz[4] = { -1, 1, 0, 0 };
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            if (at(i, j) != '/') continue;
            int hSides = 0, validApproaches = 0;
            for (int s = 0; s < 4; ++s) {
                if (at(i + dx[s], j + dz[s]) != 'H') continue;
                ++hSides;
                const char approach = at(i - dx[s], j - dz[s]);
                if (passable(approach) && approach != 'H') ++validApproaches;
            }
            if (hSides == 0) return formatStr("ramp (%d,%d) has no adjacent 'H'", i, j);
            if (validApproaches == 0) return formatStr("ramp (%d,%d) has blocked low-side approach", i, j);
        }

    std::vector<uint8_t> seen(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    std::vector<std::pair<int,int>> stack{ { ei, ej } };
    seen[static_cast<size_t>(ej) * W + ei] = 1;
    while (!stack.empty()) {
        const auto [i, j] = stack.back(); stack.pop_back();
        for (int s = 0; s < 4; ++s) {
            const int ni = i + dx[s], nj = j + dz[s];
            if (ni < 0 || nj < 0 || ni >= W || nj >= H) continue;
            const char c = at(ni, nj);
            if (!passable(c) || seen[static_cast<size_t>(nj) * W + ni]) continue;
            if (c == 'H') { const char from = at(i, j); if (from != 'H' && from != '/') continue; }  // need a ramp to climb up
            seen[static_cast<size_t>(nj) * W + ni] = 1;
            stack.push_back({ ni, nj });
        }
    }
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i)
            if (passable(at(i, j)) && !seen[static_cast<size_t>(j) * W + i])
                return formatStr("walkable cell (%d,%d) unreachable from 'E'", i, j);
    return std::string();
}

const Wasteland::RoomTemplate* Wasteland::pickRoomTemplate(AreaSize size, Biome biome, uint64_t seed, int roomIndex) const {
    std::vector<const RoomTemplate*> exact, anyBiome;
    for (const RoomTemplate& t : roomTemplates_) {
        if (t.size != size) continue;
        if (t.biome == biome) exact.push_back(&t);
        else if (t.biome == Biome::Count) anyBiome.push_back(&t);
    }
    const std::vector<const RoomTemplate*>& pool = !exact.empty() ? exact : anyBiome;
    if (pool.empty()) return nullptr;
    // No-repeat within a sector: prefer templates not yet used this sector so a run surfaces more of
    // each biome's 10 distinct layouts. The used-set resets when the sector changes (or once every
    // template of this size has been shown). Forced --room captures bypass this path entirely.
    if (pendingSector_ != pickSectorTag_) { pickSectorTag_ = pendingSector_; pickUsed_.clear(); }
    std::vector<const RoomTemplate*> fresh;
    for (const RoomTemplate* t : pool) {
        bool used = false;
        for (const std::string& n : pickUsed_) if (n == t->name) { used = true; break; }
        if (!used) fresh.push_back(t);
    }
    const std::vector<const RoomTemplate*>& choose = fresh.empty() ? pool : fresh;
    const uint32_t h = mix(static_cast<uint32_t>(seed) ^ static_cast<uint32_t>(roomIndex * 2654435761u),
                           (static_cast<uint32_t>(size) << 4) + static_cast<uint32_t>(biome) + 17u);
    const RoomTemplate* picked = choose[h % choose.size()];
    pickUsed_.push_back(picked->name);
    return picked;
}

bool Wasteland::loadQuaternius(Engine& engine) {
    const std::string base = std::string(kPulseEnvironmentQuaterniusRoot) + "/Modular SciFi MegaKit[Pro]/glTF";
    quatBase_ = base;
    scanQuatAssets();
    const auto load = [&](const char* rel) { return loadQuatPiece(engine, base + "/" + rel); };
    for (const char* f : { "Platforms/Platform_Simple.gltf", "Platforms/Platform_DarkPlates.gltf",
                           "Platforms/Platform_Squares.gltf", "Platforms/Platform_Metal.gltf" }) {
        QuatPiece p = load(f); if (!p.parts.empty()) quatFloor_.push_back(std::move(p));
    }
    for (const char* w : { "Walls/WallPadded_Straight.gltf", "Walls/WallAstra_Straight.gltf",
                           "Walls/WallBand_Straight.gltf", "Walls/WallPipe_Straight.gltf" }) {
        QuatPiece p = load(w); if (!p.parts.empty()) quatWall_.push_back(std::move(p));
    }
    quatTop_       = load("Walls/TopPlates_Straight.gltf");
    quatColumn_    = load("Columns/Column_Round.gltf");
    quatDoorFrame_ = load("Platforms/Door_Frame_Square.gltf");
    quatDoorLeaf_  = load("Platforms/Door_Metal.gltf");
    // Per-biome door leaf (spec biome.doorPiece): Foundry Door_Metal, Furnace Door_DarkMetal,
    // Reliquary Door_Simple. Biome order Rocky/Forest/Ruins.
    quatDoorLeafBiome_[static_cast<size_t>(Biome::Rocky)]  = load("Platforms/Door_Metal.gltf");
    quatDoorLeafBiome_[static_cast<size_t>(Biome::Forest)] = load("Platforms/Door_DarkMetal.gltf");
    quatDoorLeafBiome_[static_cast<size_t>(Biome::Ruins)]  = load("Platforms/Door_Simple.gltf");
    for (const char* c : { "Props/Prop_Crate1.gltf", "Props/Prop_Crate2.gltf",
                           "Props/Prop_Crate3.gltf", "Props/Prop_Barrel_Large.gltf" }) {
        QuatPiece p = load(c); if (!p.parts.empty()) quatCover_.push_back(std::move(p));
    }
    for (const char* d : { "Props/Prop_Computer.gltf", "Props/Prop_AccessPoint.gltf",
                           "Props/Prop_Cable_2.gltf", "Props/Prop_Pipe_Medium_Straight.gltf" }) {
        QuatPiece p = load(d); if (!p.parts.empty()) quatDress_.push_back(std::move(p));
    }
    // Biome ramps (spec biome.ramp): Foundry/Furnace use a metal ramp; Reliquary uses monumental
    // stairs. ('/' collision is a stamped step regardless, so the piece is purely the visual.)
    quatRamp_   = load("Platforms/Platform_Ramp_2.gltf");
    if (quatRamp_.parts.empty()) quatRamp_ = load("Platforms/Platform_Ramp_4Wide.gltf");
    // Reliquary stairs read more monumental as the wide, multi-step piece (auto-scaled to the
    // 4 m ramp cell); fall back to the narrow stair if the wide one is absent.
    quatStairs_ = load("Platforms/Platform_Stairs_4Wide.gltf");
    if (quatStairs_.parts.empty()) quatStairs_ = load("Platforms/Platform_Stairs_2.gltf");

    // Per-biome 'p' dressing + 'c' crate pools (spec biome.dressing). Biome enum order is
    // Rocky=Foundry, Forest=Furnace, Ruins=Reliquary.
    const auto fillPool = [&](std::vector<QuatPiece>& dst, std::initializer_list<const char*> names) {
        for (const char* n : names) {
            QuatPiece p = loadQuatPiece(engine, base + "/Props/" + n + ".gltf");
            if (!p.parts.empty()) dst.push_back(std::move(p));
        }
    };
    fillPool(quatDressBiome_[static_cast<size_t>(Biome::Rocky)],
             { "Prop_Cable_1", "Prop_Cable_2", "Prop_Cable_3", "Prop_Pipe_Thick_Straight", "Prop_AccessPoint", "Prop_Light_Wide", "Prop_Fan_Small" });
    fillPool(quatDressBiome_[static_cast<size_t>(Biome::Forest)],
             { "Prop_Pipe_Thick_Straight", "Prop_Pipe_Thick_Curve", "Prop_Vent_Big", "Prop_Vent_Wide", "Prop_Vent_Small", "Prop_Barrel_Small", "Prop_WallBand_BrokenPlate", "Prop_Clamp" });
    fillPool(quatDressBiome_[static_cast<size_t>(Biome::Ruins)],
             { "Prop_Light_Floor", "Prop_Light_Corner", "Prop_Chest", "Prop_ItemHolder" });
    fillPool(quatCrateBiome_[static_cast<size_t>(Biome::Rocky)],  { "Prop_Crate1" });
    fillPool(quatCrateBiome_[static_cast<size_t>(Biome::Forest)], { "Prop_Crate2" });
    fillPool(quatCrateBiome_[static_cast<size_t>(Biome::Ruins)],  { "Prop_Crate3" });

    quatReady_ = !quatFloor_.empty() && !quatWall_.empty();

    // ENCLOSURE materials. A dark, near-black ceiling/shell so the sealed interior reads as mass
    // overhead (and the band above the kit walls reads as continuous wall, not open sky). Plus a
    // per-biome EMISSIVE trim (HDR radiance = baseColorFactor * emissive, blooms past the 1.55
    // bloom threshold): Foundry cyan conduit strips, Furnace molten-orange floor seams, Reliquary
    // cold-blue ceiling light-shaft panels. Biome enum order Rocky=Foundry/Forest=Furnace/Ruins=Reliquary.
    const auto matte = [&](float r, float g, float b, float rough, float metal) {
        MaterialDesc d; d.baseColorFactor = { r, g, b, 1.0f };
        d.roughness = rough; d.metallic = metal; d.emissive = 0.0f;
        return engine.createMaterial(d);
    };
    const auto loadPbr = [&](const char* id, float uvScale, float metalScale, float roughBoost,
                             Vec4f tint) -> MaterialHandle {
        const std::string pbrBase = std::string("assets/external/polyhaven/") + id + "/" + id + "_";
        const auto tex = [&](const char* map, bool srgb) -> TextureHandle {
            const std::string dds = resolveDir(pbrBase + map + "_1k.dds");
            if (std::filesystem::exists(dds)) {
                const TextureHandle h = engine.createTextureDDS(dds);
                if (h != TextureHandle::Invalid) return h;
            }
            uint32_t w = 0, ht = 0;
            std::vector<uint8_t> rgba = loadImageRGBA(resolveDir(pbrBase + map + "_1k.png"), w, ht);
            if (rgba.empty()) return TextureHandle::Invalid;
            TextureData td; td.width = w; td.height = ht; td.rgba = rgba.data(); td.srgb = srgb;
            return engine.createTexture(td);
        };
        MaterialDesc d;
        d.baseColor = tex("diff", true);
        if (d.baseColor == TextureHandle::Invalid) return MaterialHandle::Invalid;
        d.orm = tex("arm", false);
        d.normal = tex("nor_gl", false);
        d.baseColorFactor = tint;
        d.uvScale = uvScale;
        d.metalScale = metalScale;
        d.roughBoost = roughBoost;
        return engine.createMaterial(d);
    };
    const auto pbrOr = [&](MaterialHandle m, MaterialHandle fb) {
        return m != MaterialHandle::Invalid ? m : fb;
    };

    quatCeilingMat_ = engine.createMaterial({ TextureHandle::Invalid, { 0.045f, 0.050f, 0.060f, 1.0f }, 0.0f, 0.10f, 0.85f });
    quatStructMat_[static_cast<size_t>(Biome::Rocky)] =
        pbrOr(loadPbr("factory_wall", 0.42f, 0.28f, 0.20f, { 0.50f, 0.55f, 0.60f, 1.0f }),
              matte(0.17f, 0.20f, 0.23f, 0.88f, 0.10f));
    quatStructMat_[static_cast<size_t>(Biome::Forest)] =
        pbrOr(loadPbr("factory_wall", 0.42f, 0.22f, 0.24f, { 0.48f, 0.36f, 0.28f, 1.0f }),
              matte(0.22f, 0.14f, 0.10f, 0.92f, 0.08f));
    quatStructMat_[static_cast<size_t>(Biome::Ruins)] =
        pbrOr(loadPbr("factory_wall", 0.44f, 0.10f, 0.24f, { 0.68f, 0.70f, 0.72f, 1.0f }),
              matte(0.30f, 0.31f, 0.34f, 0.94f, 0.02f));
    quatFloorMassMat_[static_cast<size_t>(Biome::Rocky)] =
        pbrOr(loadPbr("concrete_floor_02", 0.34f, 0.02f, 0.14f, { 0.76f, 0.80f, 0.86f, 1.0f }),
              matte(0.38f, 0.40f, 0.45f, 0.90f, 0.02f));
    quatFloorMassMat_[static_cast<size_t>(Biome::Forest)] =
        pbrOr(loadPbr("concrete_floor_02", 0.34f, 0.00f, 0.18f, { 0.78f, 0.62f, 0.50f, 1.0f }),
              matte(0.40f, 0.30f, 0.24f, 0.92f, 0.02f));
    quatFloorMassMat_[static_cast<size_t>(Biome::Ruins)] =
        pbrOr(loadPbr("concrete_floor_02", 0.36f, 0.00f, 0.20f, { 0.82f, 0.83f, 0.82f, 1.0f }),
              matte(0.46f, 0.46f, 0.45f, 0.94f, 0.00f));
    quatDeckMassMat_[static_cast<size_t>(Biome::Rocky)] =
        pbrOr(loadPbr("metal_plate_02", 0.46f, 0.52f, 0.16f, { 0.42f, 0.48f, 0.55f, 1.0f }),
              matte(0.16f, 0.19f, 0.23f, 0.48f, 0.78f));
    quatDeckMassMat_[static_cast<size_t>(Biome::Forest)] =
        pbrOr(loadPbr("metal_plate_02", 0.42f, 0.42f, 0.34f, { 0.26f, 0.18f, 0.14f, 1.0f }),
              matte(0.13f, 0.09f, 0.07f, 0.50f, 0.82f));
    quatDeckMassMat_[static_cast<size_t>(Biome::Ruins)] =
        pbrOr(loadPbr("metal_plate_02", 0.50f, 0.34f, 0.22f, { 0.58f, 0.62f, 0.68f, 1.0f }),
              matte(0.24f, 0.26f, 0.30f, 0.64f, 0.58f));
    quatTrimMat_[static_cast<size_t>(Biome::Rocky)]  = engine.createMaterial({ TextureHandle::Invalid, { 0.12f, 0.82f, 1.05f, 1.0f }, 1.35f, 0.0f, 0.4f });  // cyan conduit
    quatTrimMat_[static_cast<size_t>(Biome::Forest)] = engine.createMaterial({ TextureHandle::Invalid, { 0.78f, 0.23f, 0.06f, 1.0f }, 0.55f, 0.0f, 0.68f });  // molten orange
    quatTrimMat_[static_cast<size_t>(Biome::Ruins)]  = engine.createMaterial({ TextureHandle::Invalid, { 0.48f, 0.62f, 0.95f, 1.0f }, 1.30f, 0.0f, 0.4f });  // cold shaft blue
    quatExitMat_  = engine.createMaterial({ TextureHandle::Invalid, { 0.58f, 0.30f, 0.06f, 1.0f }, 0.40f, 0.0f, 0.62f });  // amber exit tick
    quatEntryMat_ = engine.createMaterial({ TextureHandle::Invalid, { 0.16f, 0.62f, 0.28f, 1.0f }, 0.50f, 0.0f, 0.58f });  // green spawn tick

    // Load the hand-crafted room templates + pre-load every kit piece they name (so the runtime
    // assembler resolves pieces by name with no engine). Designed rooms in config/pulse.rooms thus
    // drop in with only a relaunch - no recompile. Pre-cache BOTH the bare name and the "_Straight"
    // form so resolveCached() finds either (e.g. family "WallPipe" -> WallPipe_Straight, or the
    // already-full "WallAstra_Straight_Broken"; top "TopAstra" -> TopAstra_Straight).
    if (loadRoomTemplates("config/pulse.rooms")) {
        const auto familyStem = [](std::string n) {
            const std::string straightBroken = "_Straight_Broken";
            const std::string straight = "_Straight";
            const size_t b = n.find(straightBroken);
            if (b != std::string::npos) n = n.substr(0, b);
            const size_t s = n.find(straight);
            if (s != std::string::npos) n = n.substr(0, s);
            return n;
        };
        for (const RoomTemplate& t : roomTemplates_) {
            const auto pre = [&](const std::string& n) { if (!n.empty()) quatPieceByName(engine, n); };
            const auto preBoth = [&](const std::string& n) { if (!n.empty()) { pre(n); pre(n + "_Straight"); } };
            const auto preWallFamily = [&](const std::string& n) {
                preBoth(n);
                const std::string f = familyStem(n);
                for (const char* suffix : { "_Corner_Square_Inner", "_Corner_Square_Outer",
                                            "_Corner_Round_Inner", "_Corner_Round_Outer" })
                    pre(f + suffix);
            };
            const auto preTopFamily = [&](const std::string& n) {
                preBoth(n);
                const std::string f = familyStem(n);
                for (const char* suffix : { "_Corner_Square_Inner", "_Corner_Square_Outer",
                                            "_Corner_Round_Inner", "_Corner_Round_Outer",
                                            "_Corner_Curve_Inner", "_Corner_Curve_Outer",
                                            "_Curve_Round_Inner", "_Curve_Round_Outer" })
                    pre(f + suffix);
            };
            preWallFamily(t.family); pre(t.floor); preTopFamily(t.top); preBoth(t.cover);
            pre(t.pillar); pre(t.focal); pre(t.daisFloor); pre(t.ramp);
            pre(t.doorFrame); pre(t.doorLeaf); pre(t.cratePiece);
            for (const std::string& n : t.dressingPool) pre(n);
            for (const std::string& n : t.decalGroup) pre(n);
        }
        // Shared architectural detail the assembler can add around authored highground without
        // requiring every room header to repeat the prop names.
        for (const char* n : {
            "Prop_Rail_4", "Prop_Rail_3", "Prop_Rail_2", "Prop_Rail_Incline_Long_L", "Prop_Rail_Incline_Long_R",
            "Platform_Rails_2", "Platform_Rails_4", "Platform_Rails_4Wide", "Platform_Rails_4WideTall",
            "Prop_Light_Floor", "Prop_Light_Wide", "Prop_AccessPoint", "Prop_PipeHolder",
            "Prop_Vent_Wide", "Prop_Vent_Big", "Prop_Vent_Small", "Prop_Cable_1", "Prop_Cable_2", "Prop_Cable_3", "Prop_Cable_4",
            "Prop_Pipe_Thick_Straight", "Prop_Pipe_Medium_Straight", "Prop_Pipe_Small_Straight",
            "Prop_Light_Small", "Prop_Fan_Small", "Prop_Barrel_Small", "Prop_Clamp", "Prop_Chest", "Prop_ItemHolder", "Prop_Computer",
            "Platform_CenterPlate", "Platform_CenterPlate_Curve", "Platform_Round1", "Platform_Round2",
            "Platform_RedAccent", "Platform_RedAccent_Curve", "Platform_X", "Platform_Metal", "Platform_Metal_Curve",
            "Platform_Metal2", "Platform_Metal2_Curve", "Platform_Squares", "Platform_Simple_Curve",
            "Platform_DarkPlates_Curves", "Platform_Stairs_4Wide", "Platform_Stairs_4", "Platform_Ramp_4",
            "Column_MetalSupport", "Column_Simple",
            "TopCables_Straight", "TopCables_Straight_Hanging",
            "BottomAccent_Straight", "BottomMetal_Straight", "BottomSimple_Straight",
            "BottomAccent_Corner_Square_Inner", "BottomAccent_Corner_Square_Outer_1", "BottomAccent_Corner_Square_Outer_2",
            "BottomMetal_Corner_Square_Inner", "BottomMetal_Corner_Square_Outer_1", "BottomMetal_Corner_Square_Outer_2",
            "BottomSimple_Corner_Square_Inner", "BottomSimple_Corner_Square_Outer_1", "BottomSimple_Corner_Square_Outer_2",
            "Decal_Caution", "Decal_Warning", "Decal_Arrows", "Decal_Code", "Decal_Code_2",
            "Decal_Line_Straight", "Decal_Line_90", "Decal_Line_90_Round", "Decal_Logo", "Decal_Logo_Small",
            "Decal_Logo_Letters", "Decal_STRNOV", "Decal_AccessPoint", "Decal_K", "Decal_V", "Decal_X", "Decal_Z",
            "Decal_Dashes", "Decal_Authorized", "Decal_Open", "Decal_Sign", "Decal_XSign"
        }) quatPieceByName(engine, n);
    }
    return quatReady_;
}

namespace {
// Append an axis-aligned box [x0..x1]x[y0..y1]x[z0..z1] to a StaticVertex mesh, matching the
// engine's front-face winding (mirrors pushGpuBox). Outward axis normals; simple non-degenerate
// UVs so createMesh's tangent generation stays valid. Used by the Neon Ink Brutalism blocky arena.
void appendBox(std::vector<StaticVertex>& v, std::vector<uint32_t>& idx,
               float x0, float y0, float z0, float x1, float y1, float z1) {
    const Vec3f nnn{ x0, y0, z0 }, pnn{ x1, y0, z0 }, npn{ x0, y1, z0 }, ppn{ x1, y1, z0 };
    const Vec3f nnp{ x0, y0, z1 }, pnp{ x1, y0, z1 }, npp{ x0, y1, z1 }, ppp{ x1, y1, z1 };
    const auto quad = [&](Vec3f a, Vec3f b, Vec3f c, Vec3f d, Vec3f n) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        const Vec3f corners[4] = { a, b, c, d };
        for (const Vec3f& p : corners) {
            StaticVertex sv{};
            sv.pos = p; sv.nrm = n;
            // Per-face WORLD-PLANAR UVs (world units) so a material's uvScale tiles the PBR
            // concrete/metal/factory textures at a real, consistent texel density on every box face
            // (no stretch). Tangent follows the U axis for correct normal mapping.
            if (std::fabs(n.y) > 0.5f)      { sv.uv0[0] = p.x; sv.uv0[1] = p.z; sv.tangent = { 1, 0, 0, 1 }; }
            else if (std::fabs(n.x) > 0.5f) { sv.uv0[0] = p.z; sv.uv0[1] = p.y; sv.tangent = { 0, 0, 1, 1 }; }
            else                            { sv.uv0[0] = p.x; sv.uv0[1] = p.y; sv.tangent = { 1, 0, 0, 1 }; }
            sv.color = { 1, 1, 1, 1 };
            v.push_back(sv);
        }
        // Wind to match the engine's front-face convention (same handedness as the ground
        // quad): the visible/front winding is the one whose geometric normal is OPPOSITE the
        // stored outward normal. Emitting a,b,c / a,c,d here left the boxes back-facing
        // (culled front -> inside-out/hollow look), so reverse to a,c,b / a,d,c.
        idx.push_back(base); idx.push_back(base + 2); idx.push_back(base + 1);
        idx.push_back(base); idx.push_back(base + 3); idx.push_back(base + 2);
    };
    quad(nnn, npn, ppn, pnn, { 0, 0, -1 });
    quad(nnp, pnp, ppp, npp, { 0, 0,  1 });
    quad(nnn, pnn, pnp, nnp, { 0, -1, 0 });
    quad(npn, npp, ppp, ppn, { 0,  1, 0 });
    quad(nnn, nnp, npp, npn, { -1, 0, 0 });
    quad(pnn, ppn, ppp, pnp, {  1, 0, 0 });
}
} // namespace

bool Wasteland::load(Engine& engine) {
    if (brutalist_) return loadBrutalist(engine);
    // Big tiled ground quad (covers the 32x24 map to the fogged horizon; UVs tile at a fixed size).
    {
        const float lo = -60.0f, hiX = 92.0f, hiZ = 84.0f, tile = 4.0f;
        const auto vtx = [&](float x, float z) {
            StaticVertex v{}; v.pos = { x, 0.0f, z }; v.nrm = { 0, 1, 0 };
            v.tangent = { 1, 0, 0, 1 }; v.uv0[0] = x / tile; v.uv0[1] = z / tile; return v;
        };
        std::vector<StaticVertex> gv = { vtx(lo, lo), vtx(hiX, lo), vtx(hiX, hiZ), vtx(lo, hiZ) };
        std::vector<uint32_t>     gi = { 0, 1, 2, 0, 2, 3 };
        groundMesh_ = engine.createMesh({ gv, gi });
    }
    const std::string medieval = "assets/bumstrum/medieval_sceneray";
    const std::string sk = "assets/external/sketchfab/";
    groundMat_[static_cast<size_t>(Biome::Rocky)]  = groundMaterial(engine, medieval, "terrainrock", 4.0f);
    groundMat_[static_cast<size_t>(Biome::Forest)] = groundMaterial(engine, sk + "forest_mats", "grassground1", 4.0f);
    groundMat_[static_cast<size_t>(Biome::Ruins)]  = groundMaterial(engine, medieval, "cobblestone", 4.0f);

    // Medieval kit (mixed): categorise the pieces we want by name.
    {
        std::vector<KitTile> tiles;
        if (loadGltfTiles(resolveDir(medieval + "/scene.gltf"), tiles)) {
            const auto build = [&](KitTile& t, Cat cat, float targetSize, float coverFrac, float trunk) {
                float dx, dy, dz; extent(t.submeshes, dx, dy, dz);
                const float md = std::max(dx, std::max(dy, dz));
                if (md <= 1e-4f) return;
                const float scale = targetSize / md;
                Prop p; p.name = t.name;
                for (TileSubmesh& sm : t.submeshes) {
                    for (StaticVertex& v : sm.verts) { v.pos.x *= scale; v.pos.y *= scale; v.pos.z *= scale; }
                    for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) std::swap(sm.indices[i + 1], sm.indices[i + 2]);
                    const MeshHandle mh = engine.createMesh({ sm.verts, sm.indices });
                    p.parts.push_back({ mh, kitMaterial(engine, medieval, sm.material), Mat4::identity() });
                }
                p.halfX = 0.5f * dx * scale; p.halfZ = 0.5f * dz * scale; p.height = dy * scale;
                p.radius = trunk > 0.0f ? trunk
                         : coverFrac > 0.0f ? std::max(0.3f, std::min(p.halfX, p.halfZ) * 0.9f)
                         : 0.0f;   // decoration carries no collision (see loadKit)
                if (!p.parts.empty()) poolFor(cat).push_back(std::move(p));
            };
            for (KitTile& t : tiles) {
                if (t.name == "rockA" || t.name == "rockB" || t.name == "rockC" || t.name == "rocksCombined")
                    build(t, Cat::Rock, 2.8f, 0.58f, 0.0f);
                else if (t.name.compare(0, 9, "smallrock") == 0)
                    build(t, Cat::SmallRock, 0.5f, 0.0f, 0.0f);
                else if (t.name == "crate" || t.name == "cratepiece")
                    build(t, Cat::Crate, 1.3f, 0.70f, 0.0f);
                else if (t.name == "outpost" || t.name == "house2")
                    build(t, Cat::Structure, 6.0f, 0.0f, 0.0f);
            }
        }
    }

    // Extra rock variety (Sketchfab "set" kits -> individual rock pieces).
    loadKit(engine, sk + "rocks_set2",    Cat::Rock, Mode::Pieces, 3.0f, 0.55f, 0.0f);
    loadKit(engine, sk + "cave_rocks",    Cat::Rock, Mode::Pieces, 3.4f, 0.55f, 0.0f);
    loadKit(engine, sk + "rock_clusters", Cat::Rock, Mode::Pieces, 3.2f, 0.55f, 0.0f);
    loadKit(engine, sk + "cliffs",        Cat::Rock, Mode::Pieces, 4.0f, 0.55f, 0.0f);
    loadKit(engine, sk + "blocky_rocks",  Cat::Rock, Mode::Pieces, 3.0f, 0.55f, 0.0f);
    loadKit(engine, sk + "flat_rocks",    Cat::Rock, Mode::Pieces, 2.6f, 0.55f, 0.0f);
    loadKit(engine, sk + "time_stones",   Cat::Rock, Mode::Pieces, 4.2f, 0.50f, 0.0f);   // standing stones
    // Crates + barrels (cover).
    loadKit(engine, sk + "barrel_crate",  Cat::Crate, Mode::Pieces, 1.4f, 0.70f, 0.0f);
    loadKit(engine, sk + "crates",        Cat::Crate, Mode::Pieces, 1.3f, 0.70f, 0.0f);
    // Forest props.
    loadKit(engine, sk + "grass_patches", Cat::Grass, Mode::Pieces, 0.7f, 0.0f, 0.0f);
    loadKit(engine, sk + "trees1",        Cat::Tree,  Mode::Merged, 7.5f, 0.0f, 0.45f);
    loadKit(engine, sk + "tree_a",        Cat::Tree,  Mode::Merged, 7.0f, 0.0f, 0.40f);
    loadKit(engine, sk + "gnarly_trees",  Cat::Tree,  Mode::Merged, 8.0f, 0.0f, 0.45f);
    loadKit(engine, sk + "ancient_tree",  Cat::Tree,  Mode::Merged, 9.5f, 0.0f, 0.55f);
    // Structures / landmarks (backdrop in most biomes; placed IN the arena in Ruins).
    loadKit(engine, sk + "guardian_statue", Cat::Structure, Mode::Merged, 4.0f, 0.0f, 0.0f);
    loadKit(engine, sk + "stone_entrance",  Cat::Structure, Mode::Merged, 5.5f, 0.0f, 0.0f);
    loadKit(engine, sk + "statue",          Cat::Structure, Mode::Merged, 4.0f, 0.0f, 0.0f);
    loadKit(engine, sk + "house1",          Cat::Structure, Mode::Merged, 7.5f, 0.0f, 0.0f);
    loadKit(engine, sk + "wood_walls",      Cat::Structure, Mode::Pieces, 3.2f, 0.0f, 0.0f);

    // Background mountain skyline: one big ring of peaks placed far around the arena (no
    // collision) so the horizon reads as a real place with depth + scale, not a flat fog line.
    {
        KitTile whole;
        if (loadGltfWhole(resolveDir(sk + "bg_mountains/scene.gltf"), whole)) {
            float dx, dy, dz; extent(whole.submeshes, dx, dy, dz);
            const float md = std::max(dx, std::max(dy, dz));
            const float scale = md > 1e-4f ? 175.0f / md : 1.0f;   // ~175-unit-wide ring of peaks
            for (TileSubmesh& sm : whole.submeshes) {
                for (StaticVertex& v : sm.verts) { v.pos.x *= scale; v.pos.y *= scale; v.pos.z *= scale; }
                for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) std::swap(sm.indices[i + 1], sm.indices[i + 2]);
                const MeshHandle mh = engine.createMesh({ sm.verts, sm.indices });
                skyline_.parts.push_back({ mh, kitMaterial(engine, sk + "bg_mountains", sm.material), Mat4::identity() });
            }
            skyline_.height = dy * scale;
        }
    }

    ready_ = groundMesh_ != MeshHandle::Invalid && !rocks_.empty();
    return ready_;
}

void Wasteland::place(std::vector<DungeonDraw>& out, const Prop& p, float wx, float wz, float yaw, float scale) {
    const Mat4 xf = mul(scaling(scale, scale, scale), mul(rotationY(yaw), translation(wx, 0.0f, wz)));
    for (const DungeonDraw& part : p.parts)
        out.push_back({ part.mesh, part.material, xf });
    if (p.radius > 0.0f) stampSolidCircle(wx, wz, p.radius * scale, p.height * scale);
}

void Wasteland::stampSolidCircle(float wx, float wz, float radius, float height) {
    const float r2 = radius * radius;
    const int fx0 = static_cast<int>(std::floor((wx - radius) * kSub));
    const int fx1 = static_cast<int>(std::ceil ((wx + radius) * kSub));
    const int fz0 = static_cast<int>(std::floor((wz - radius) * kSub));
    const int fz1 = static_cast<int>(std::ceil ((wz + radius) * kSub));
    for (int fz = fz0; fz < fz1; ++fz)
        for (int fx = fx0; fx < fx1; ++fx) {
            if (fx < 0 || fz < 0 || fx >= 32 * kSub || fz >= 24 * kSub) continue;
            const float cx = (static_cast<float>(fx) + 0.5f) / kSub - wx;
            const float cz = (static_cast<float>(fz) + 0.5f) / kSub - wz;
            if (cx * cx + cz * cz <= r2) {
                const size_t i = static_cast<size_t>(fz) * (32 * kSub) + static_cast<size_t>(fx);
                fine_[i] = 1;
                if (height > fineHeight_[i]) fineHeight_[i] = height;   // tallest cover wins
            }
        }
}

void Wasteland::stampSolidRect(float x0, float z0, float x1, float z1, float height) {
    const int fx0 = static_cast<int>(std::floor(x0 * kSub));
    const int fx1 = static_cast<int>(std::ceil (x1 * kSub));
    const int fz0 = static_cast<int>(std::floor(z0 * kSub));
    const int fz1 = static_cast<int>(std::ceil (z1 * kSub));
    for (int fz = fz0; fz < fz1; ++fz)
        for (int fx = fx0; fx < fx1; ++fx) {
            if (fx < 0 || fz < 0 || fx >= 32 * kSub || fz >= 24 * kSub) continue;
            const size_t i = static_cast<size_t>(fz) * (32 * kSub) + static_cast<size_t>(fx);
            fine_[i] = 1;
            if (height > fineHeight_[i]) fineHeight_[i] = height;
        }
}

void Wasteland::stampRamp(float x0, float z0, float x1, float z1, float hLow, float hHigh, int ascend) {
    const int fx0 = static_cast<int>(std::floor(x0 * kSub));
    const int fx1 = static_cast<int>(std::ceil (x1 * kSub));
    const int fz0 = static_cast<int>(std::floor(z0 * kSub));
    const int fz1 = static_cast<int>(std::ceil (z1 * kSub));
    const float spanX = std::max(0.001f, x1 - x0), spanZ = std::max(0.001f, z1 - z0);
    for (int fz = fz0; fz < fz1; ++fz)
        for (int fx = fx0; fx < fx1; ++fx) {
            if (fx < 0 || fz < 0 || fx >= 32 * kSub || fz >= 24 * kSub) continue;
            const float wx = (static_cast<float>(fx) + 0.5f) / kSub;
            const float wz = (static_cast<float>(fz) + 0.5f) / kSub;
            float t = 0.0f;                                  // 0 at the low edge, 1 at the deck edge
            switch (ascend) {
                case 0: t = (wx - x0) / spanX; break;        // ascend +x
                case 1: t = (x1 - wx) / spanX; break;        // ascend -x
                case 2: t = (wz - z0) / spanZ; break;        // ascend +z
                default: t = (z1 - wz) / spanZ; break;       // ascend -z
            }
            t = std::clamp(t, 0.0f, 1.0f);
            const float h = hLow + (hHigh - hLow) * t;
            const size_t i = static_cast<size_t>(fz) * (32 * kSub) + static_cast<size_t>(fx);
            fine_[i] = 1;
            if (h > fineHeight_[i]) fineHeight_[i] = h;
        }
}

// Neon Ink Brutalism slice: a CURATED SET of flat-matte BLOCKY layouts at different sizes, so a
// run strings together small + big areas joined by doors (Hades/Returnal). Each template carries
// its open play-rect, its cover/monolith boxes (m = master material; collide marks a solid block;
// h = collision top height, 99 = unscalable), and its door openings. Perimeter WALLS are
// synthesized in code from the door specs (segments that leave the gaps), so geometry + collision
// never drift. Geometry is baked once PER TEMPLATE at load; generate only varies collision + door
// state. Door [0] is the ENTRANCE (player landing, stays sealed); the rest are exits.
namespace {

struct BBox { float x0, y0, z0, x1, y1, z1; int m; bool collide; float h; };
struct DoorSpec { Side side; float offset; float width; };
struct LayoutTemplate {
    AreaSize        size;
    float           ox0, oz0, ox1, oz1;     // open play-rect (world units); walls ring just outside
    const BBox*     boxes; int boxCount;     // floor + cover + monoliths (NOT walls)
    const DoorSpec* doors; int doorCount;    // [0] = entrance, [1..] = exits
};

// --- Small: a tight pit (1 central block + a corner monolith), 1 entrance + 2 exits. ---
const BBox kSmallBoxes[] = {
    {  8.0f, -0.6f,  5.0f, 24.0f, 0.0f, 19.0f, 1, false, 0.0f },   // floor slab
    { 13.0f, 0.005f, 10.0f, 19.0f, 0.04f, 14.0f, 2, false, 0.0f }, // obsidian floor patch
    { 15.0f, 0.0f, 11.0f, 17.0f, 0.9f, 13.0f, 0, true, 0.9f },     // central cover block
    { 11.5f, 0.0f, 14.0f, 12.8f, 4.0f, 15.2f, 2, true, 99.0f },    // corner monolith
};
const DoorSpec kSmallDoors[] = {
    { Side::S, 16.0f, 3.0f },   // entrance
    { Side::N, 13.0f, 3.0f }, { Side::N, 19.0f, 3.0f }, { Side::W, 12.0f, 3.0f },
};

// --- Mid: a cross arena (mixed cover + 2 flanking monoliths), 1 entrance + 3 exits. ---
const BBox kMidBoxes[] = {
    {  2.0f, -0.6f,  1.0f, 30.0f, 0.0f, 23.0f, 1, false, 0.0f },   // floor slab
    { 13.0f, 0.005f, 9.0f, 19.0f, 0.04f, 15.0f, 2, false, 0.0f },  // obsidian floor patch
    { 10.0f, 0.0f,  9.0f, 12.0f, 1.1f, 11.0f, 0, true, 1.1f },     // cover blocks
    { 20.0f, 0.0f, 12.0f, 22.0f, 1.3f, 14.0f, 0, true, 1.3f },
    { 15.0f, 0.0f,  7.0f, 17.0f, 0.8f,  8.5f, 1, true, 0.8f },
    {  9.0f, 0.0f, 14.0f, 11.0f, 0.9f, 16.0f, 1, true, 0.9f },
    { 18.0f, 0.0f, 15.0f, 20.0f, 0.7f, 17.0f, 1, true, 0.7f },
    {  7.0f, 0.0f,  6.0f,  8.2f, 5.0f,  7.2f, 2, true, 99.0f },    // flanking monoliths
    { 24.0f, 0.0f, 16.0f, 25.2f, 5.0f, 17.2f, 2, true, 99.0f },
};
const DoorSpec kMidDoors[] = {
    { Side::S, 16.0f, 3.0f },   // entrance
    { Side::N, 11.0f, 3.0f }, { Side::N, 21.0f, 3.0f }, { Side::W, 12.0f, 3.0f },
};

// --- Big: the full monolith arena (reuses the original brutalist layout), 1 entrance + 3 exits. ---
const BBox kBigBoxes[] = {
    { -4.0f, -0.6f, -4.0f, 36.0f, 0.0f, 28.0f, 1, false, 0.0f },   // ground slab
    {  9.0f, 0.005f, 7.0f, 23.0f, 0.04f, 17.0f, 2, false, 0.0f },  // obsidian floor patch
    {  7.0f, 0.0f,  7.0f,  9.2f, 1.4f,  9.2f, 0, true, 1.4f },     // cover blocks
    { 22.0f, 0.0f,  8.0f, 24.5f, 1.6f, 10.5f, 0, true, 1.6f },
    { 12.0f, 0.0f, 16.0f, 15.0f, 0.9f, 17.5f, 0, true, 0.9f },
    { 18.0f, 0.0f, 14.0f, 20.0f, 1.2f, 16.0f, 0, true, 1.2f },
    {  9.0f, 0.0f, 18.0f, 11.0f, 1.0f, 20.0f, 0, true, 1.0f },
    {  5.0f, 0.0f, 12.0f,  8.0f, 0.6f, 15.5f, 1, true, 0.6f },     // low platforms
    { 24.0f, 0.0f, 16.0f, 27.5f, 0.7f, 19.5f, 1, true, 0.7f },
    { 13.0f, 0.0f,  4.0f, 15.0f, 0.35f, 5.0f, 1, true, 0.35f },    // stepped ramp
    { 13.0f, 0.0f,  5.0f, 15.0f, 0.70f, 6.0f, 1, true, 0.70f },
    { 13.0f, 0.0f,  6.0f, 15.0f, 1.05f, 7.0f, 1, true, 1.05f },
    { 13.0f, 0.0f,  7.0f, 16.0f, 1.40f, 9.0f, 1, true, 1.40f },    // ramp top platform
    {  4.0f, 0.0f,  4.0f,  5.4f, 6.0f,  5.4f, 2, true, 99.0f },    // obsidian monoliths
    { 27.0f, 0.0f,  4.0f, 28.4f, 6.5f,  5.4f, 2, true, 99.0f },
    { 26.0f, 0.0f, 20.0f, 27.6f, 7.0f, 21.6f, 2, true, 99.0f },
    { 14.5f, 0.0f, 20.0f, 17.5f, 0.8f, 21.5f, 2, true, 99.0f },    // monument plinth
};
const DoorSpec kBigDoors[] = {
    { Side::S, 22.0f, 3.0f },   // entrance (clear of the central plinth)
    { Side::N, 8.0f, 3.0f }, { Side::N, 24.0f, 3.0f }, { Side::E, 12.0f, 3.0f },
};

// --- Corridor: a long thin connector with 2 pillars, 1 entrance + 1 exit. ---
const BBox kCorrBoxes[] = {
    {  1.0f, -0.6f,  7.0f, 31.0f, 0.0f, 17.0f, 1, false, 0.0f },   // floor slab
    { 10.0f, 0.0f, 11.0f, 11.5f, 3.5f, 12.5f, 2, true, 99.0f },    // pillars
    { 18.0f, 0.0f, 12.0f, 19.5f, 3.5f, 13.5f, 2, true, 99.0f },
    { 14.0f, 0.0f, 11.5f, 16.0f, 1.0f, 12.5f, 0, true, 1.0f },     // low mid cover
};
const DoorSpec kCorrDoors[] = {
    { Side::W, 12.0f, 3.0f },   // entrance
    { Side::E, 12.0f, 3.0f }, { Side::N, 8.0f, 3.0f }, { Side::S, 22.0f, 3.0f },
};

// One template per AreaSize, in enum order (Small, Mid, Big, Corridor) so index == (int)size.
const LayoutTemplate kTemplates[] = {
    { AreaSize::Small,    10.0f, 6.0f, 22.0f, 18.0f, kSmallBoxes, (int)(sizeof(kSmallBoxes)/sizeof(BBox)), kSmallDoors, (int)(sizeof(kSmallDoors)/sizeof(DoorSpec)) },
    { AreaSize::Mid,       6.0f, 5.0f, 26.0f, 19.0f, kMidBoxes,   (int)(sizeof(kMidBoxes)/sizeof(BBox)),   kMidDoors,   (int)(sizeof(kMidDoors)/sizeof(DoorSpec)) },
    { AreaSize::Big,       1.0f, 1.0f, 31.0f, 23.0f, kBigBoxes,   (int)(sizeof(kBigBoxes)/sizeof(BBox)),   kBigDoors,   (int)(sizeof(kBigDoors)/sizeof(DoorSpec)) },
    { AreaSize::Corridor,  4.0f, 10.0f, 28.0f, 14.0f, kCorrBoxes, (int)(sizeof(kCorrBoxes)/sizeof(BBox)),  kCorrDoors,  (int)(sizeof(kCorrDoors)/sizeof(DoorSpec)) },
};

int fcell(float v) { return static_cast<int>(std::lround(v * Wasteland::kSub)); }
int cellX(float v) { return std::min(std::max(static_cast<int>(std::floor(v)), 1), 30); }
int cellZ(float v) { return std::min(std::max(static_cast<int>(std::floor(v)), 1), 22); }

// Turn a DoorSpec (side + offset along its wall) into a full Door: world center (= trigger
// center, mid wall band), inward normal, the grid cell one unit inside (entry spawn), and the
// fine-cell rect of the opening (so seal/open re-stamps exactly those cells).
Door makeDoor(const DoorSpec& spec, const LayoutTemplate& T) {
    const float wt = 1.0f, hw = spec.width * 0.5f;
    Door d; d.side = spec.side; d.open = false;
    switch (spec.side) {
        case Side::N:   // low-z wall, band [oz0-wt, oz0], inward +Z
            d.worldX = spec.offset;        d.worldZ = T.oz0 - wt * 0.5f;
            d.inwardX = 0.0f;              d.inwardZ = 1.0f;
            d.spawnX = cellX(spec.offset); d.spawnZ = cellZ(T.oz0 + 1.0f);
            d.fgx0 = fcell(spec.offset - hw); d.fgx1 = fcell(spec.offset + hw);
            d.fgz0 = fcell(T.oz0 - wt);        d.fgz1 = fcell(T.oz0);
            break;
        case Side::S:   // high-z wall, band [oz1, oz1+wt], inward -Z
            d.worldX = spec.offset;        d.worldZ = T.oz1 + wt * 0.5f;
            d.inwardX = 0.0f;              d.inwardZ = -1.0f;
            d.spawnX = cellX(spec.offset); d.spawnZ = cellZ(T.oz1 - 1.0f);
            d.fgx0 = fcell(spec.offset - hw); d.fgx1 = fcell(spec.offset + hw);
            d.fgz0 = fcell(T.oz1);             d.fgz1 = fcell(T.oz1 + wt);
            break;
        case Side::W:   // low-x wall, band [ox0-wt, ox0], inward +X
            d.worldX = T.ox0 - wt * 0.5f;  d.worldZ = spec.offset;
            d.inwardX = 1.0f;              d.inwardZ = 0.0f;
            d.spawnX = cellX(T.ox0 + 1.0f); d.spawnZ = cellZ(spec.offset);
            d.fgx0 = fcell(T.ox0 - wt);        d.fgx1 = fcell(T.ox0);
            d.fgz0 = fcell(spec.offset - hw); d.fgz1 = fcell(spec.offset + hw);
            break;
        case Side::E:   // high-x wall, band [ox1, ox1+wt], inward -X
            d.worldX = T.ox1 + wt * 0.5f;  d.worldZ = spec.offset;
            d.inwardX = -1.0f;             d.inwardZ = 0.0f;
            d.spawnX = cellX(T.ox1 - 1.0f); d.spawnZ = cellZ(spec.offset);
            d.fgx0 = fcell(T.ox1);             d.fgx1 = fcell(T.ox1 + wt);
            d.fgz0 = fcell(spec.offset - hw); d.fgz1 = fcell(spec.offset + hw);
            break;
    }
    return d;
}

// Bake a template's static geometry. When realShell is FALSE (no sci-fi kit), it synthesizes the
// full procedural shell: authored boxes + four perimeter walls (leaving the door gaps) + a layer of
// NON-colliding architectural trim (bands, pilasters, columns, beams, neon). When realShell is TRUE
// the room's walls + floor come from the tiled sci-fi kit (emitTiledShell), so here we SKIP the
// procedural walls + wall trims + the floor slabs/patches and bake only the gameplay cover/monoliths
// plus the structural accents (corner columns, overhead beams, floor neon ring) that read well over
// the real surfaces too. Materials: 0 violet, 1 concrete, 2 obsidian, 3 neon cyan, 4 magenta, 5 metal.
void buildTemplateGeometry(const LayoutTemplate& T, bool realShell,
                           std::vector<StaticVertex> v[Wasteland::kMat], std::vector<uint32_t> idx[Wasteland::kMat]) {
    const float wt = 1.0f, wh = 4.5f;
    const int wm = 0;   // walls -> violet matte (material 0)
    for (int i = 0; i < T.boxCount; ++i) {
        const BBox& b = T.boxes[i];
        if (realShell && b.y1 <= 0.05f) continue;   // floor slab/patch replaced by the tiled kit floor
        appendBox(v[b.m], idx[b.m], b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
    }
    const float ox0 = T.ox0, oz0 = T.oz0, ox1 = T.ox1, oz1 = T.oz1;
    const float cxm = 0.5f * (ox0 + ox1), czm = 0.5f * (oz0 + oz1);
    const float rw = ox1 - ox0, rd = oz1 - oz0;

    if (!realShell) {
        const auto gapsOn = [&](Side s) {
            std::vector<std::pair<float, float>> g;
            for (int i = 0; i < T.doorCount; ++i)
                if (T.doors[i].side == s) {
                    const float hw = T.doors[i].width * 0.5f;
                    g.emplace_back(T.doors[i].offset - hw, T.doors[i].offset + hw);
                }
            std::sort(g.begin(), g.end());
            return g;
        };
        const auto wall = [&](float lo, float hi, const std::vector<std::pair<float, float>>& gaps,
                              const std::function<void(float, float)>& emit) {
            float cur = lo;
            for (const auto& gp : gaps) {
                const float gs = std::max(gp.first, lo), ge = std::min(gp.second, hi);
                if (gs > cur) emit(cur, gs);
                cur = std::max(cur, ge);
            }
            if (cur < hi) emit(cur, hi);
        };
        // N + S walls span x across [ox0-wt, ox1+wt] (cover the corners); W + E span z across [oz0, oz1].
        wall(ox0 - wt, ox1 + wt, gapsOn(Side::N),
             [&](float a, float b){ appendBox(v[wm], idx[wm], a, 0.0f, oz0 - wt, b, wh, oz0); });
        wall(ox0 - wt, ox1 + wt, gapsOn(Side::S),
             [&](float a, float b){ appendBox(v[wm], idx[wm], a, 0.0f, oz1, b, wh, oz1 + wt); });
        wall(oz0, oz1, gapsOn(Side::W),
             [&](float a, float b){ appendBox(v[wm], idx[wm], ox0 - wt, 0.0f, a, ox0, wh, b); });
        wall(oz0, oz1, gapsOn(Side::E),
             [&](float a, float b){ appendBox(v[wm], idx[wm], ox1, 0.0f, a, ox1 + wt, wh, b); });

        const std::vector<std::pair<float, float>> gN = gapsOn(Side::N), gS = gapsOn(Side::S),
                                                   gW = gapsOn(Side::W), gE = gapsOn(Side::E);
        // A band hugging every interior wall face at [yLo,yHi], `depth` into the room, in material m.
        const auto band = [&](float yLo, float yHi, float depth, int m) {
            wall(ox0 - wt, ox1 + wt, gN, [&](float a, float b){ appendBox(v[m], idx[m], a, yLo, oz0, b, yHi, oz0 + depth); });
            wall(ox0 - wt, ox1 + wt, gS, [&](float a, float b){ appendBox(v[m], idx[m], a, yLo, oz1 - depth, b, yHi, oz1); });
            wall(oz0, oz1, gW, [&](float a, float b){ appendBox(v[m], idx[m], ox0, yLo, a, ox0 + depth, yHi, b); });
            wall(oz0, oz1, gE, [&](float a, float b){ appendBox(v[m], idx[m], ox1 - depth, yLo, a, ox1, yHi, b); });
        };
        band(0.0f,  0.34f, 0.22f, 5);   // metal baseboard
        band(2.10f, 2.42f, 0.16f, 5);   // metal mid trim
        band(2.42f, 2.58f, 0.10f, 3);   // cyan neon strip above the mid trim
        band(3.70f, 3.96f, 0.14f, 5);   // metal high trim

        // Pilasters: vertical buttresses at intervals along each wall, skipping door gaps; alternate
        // ones carry a thin vertical neon riser on their room-facing face (cyan on N/W, magenta on S/E).
        const auto inGap = [](const std::vector<std::pair<float, float>>& gaps, float c, float hw) {
            for (const auto& g : gaps) if (c + hw > g.first - 0.4f && c - hw < g.second + 0.4f) return true;
            return false;
        };
        const float pStep = 3.6f, pHW = 0.45f, pD = 0.42f, nz = 0.07f;
        int pc = 0;
        for (float x = ox0 + 1.7f; x < ox1 - 1.0f; x += pStep, ++pc) {
            const bool lit = (pc & 1) == 0;
            if (!inGap(gN, x, pHW)) {
                appendBox(v[0], idx[0], x - pHW, 0.0f, oz0, x + pHW, wh, oz0 + pD);
                if (lit) appendBox(v[3], idx[3], x - nz, 0.5f, oz0 + pD, x + nz, wh - 0.5f, oz0 + pD + 0.06f);
            }
            if (!inGap(gS, x, pHW)) {
                appendBox(v[0], idx[0], x - pHW, 0.0f, oz1 - pD, x + pHW, wh, oz1);
                if (lit) appendBox(v[4], idx[4], x - nz, 0.5f, oz1 - pD - 0.06f, x + nz, wh - 0.5f, oz1 - pD);
            }
        }
        for (float z = oz0 + 1.7f; z < oz1 - 1.0f; z += pStep, ++pc) {
            const bool lit = (pc & 1) == 0;
            if (!inGap(gW, z, pHW)) {
                appendBox(v[0], idx[0], ox0, 0.0f, z - pHW, ox0 + pD, wh, z + pHW);
                if (lit) appendBox(v[3], idx[3], ox0 + pD, 0.5f, z - nz, ox0 + pD + 0.06f, wh - 0.5f, z + nz);
            }
            if (!inGap(gE, z, pHW)) {
                appendBox(v[0], idx[0], ox1 - pD, 0.0f, z - pHW, ox1, wh, z + pHW);
                if (lit) appendBox(v[4], idx[4], ox1 - pD - 0.06f, 0.5f, z - nz, ox1 - pD, wh - 0.5f, z + nz);
            }
        }
    }

    // Corner columns (obsidian, rising above the wall) with an inner-corner cyan riser.
    const float cc = 0.9f, ns = 0.10f;
    const float cxs[4] = { ox0, ox1 - cc, ox0, ox1 - cc };
    const float czs[4] = { oz0, oz0, oz1 - cc, oz1 - cc };
    for (int k = 0; k < 4; ++k) {
        appendBox(v[2], idx[2], cxs[k], 0.0f, czs[k], cxs[k] + cc, wh + 0.6f, czs[k] + cc);
        const float rx = (k == 0 || k == 2) ? cxs[k] + cc : cxs[k] - ns;        // inner face in x
        const float rz = (k == 0 || k == 1) ? czs[k] + cc : czs[k] - ns;        // inner face in z
        appendBox(v[3], idx[3], rx, 0.4f, rz, rx + ns, wh, rz + ns);
    }

    // Overhead beams across the SHORT axis (sit on top of the walls; open-topped arena keeps the
    // sky), each with a hanging magenta light bar slung beneath it.
    const int nBeams = std::max(2, static_cast<int>(std::max(rw, rd) / 6.0f));
    const float by0 = wh, by1 = wh + 0.45f;
    if (rw >= rd) {
        for (int k = 1; k <= nBeams; ++k) {
            const float x = ox0 + rw * static_cast<float>(k) / static_cast<float>(nBeams + 1);
            appendBox(v[5], idx[5], x - 0.3f, by0, oz0 - wt, x + 0.3f, by1, oz1 + wt);
            appendBox(v[4], idx[4], x - 0.10f, by0 - 0.42f, oz0 + 1.2f, x + 0.10f, by0 - 0.26f, oz1 - 1.2f);
        }
    } else {
        for (int k = 1; k <= nBeams; ++k) {
            const float z = oz0 + rd * static_cast<float>(k) / static_cast<float>(nBeams + 1);
            appendBox(v[5], idx[5], ox0 - wt, by0, z - 0.3f, ox1 + wt, by1, z + 0.3f);
            appendBox(v[3], idx[3], ox0 + 1.2f, by0 - 0.42f, z - 0.10f, ox1 - 1.2f, by0 - 0.26f, z + 0.10f);
        }
    }

    // Floor neon ring around the centre (flush with the floor, NON-colliding) - a focal inlay.
    const float rr = std::min(rw, rd) * 0.30f, tw = 0.13f, ry = 0.035f;
    appendBox(v[3], idx[3], cxm - rr, 0.0f, czm - rr, cxm + rr, ry, czm - rr + tw);
    appendBox(v[3], idx[3], cxm - rr, 0.0f, czm + rr - tw, cxm + rr, ry, czm + rr);
    appendBox(v[3], idx[3], cxm - rr, 0.0f, czm - rr, cxm - rr + tw, ry, czm + rr);
    appendBox(v[3], idx[3], cxm + rr - tw, 0.0f, czm - rr, cxm + rr, ry, czm + rr);
}

} // namespace

const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Rocky:  return "FOUNDRY";
        case Biome::Forest: return "FURNACE";
        case Biome::Ruins:  return "RELIQUARY";
        case Biome::Count:  break;
    }
    return "SECTOR";
}

int biomeRewardElement(Biome b) {
    switch (b) {
        case Biome::Rocky:  return 2;  // Foundry  -> Shock (Volt)
        case Biome::Forest: return 1;  // Furnace  -> Burn (Pyro)
        case Biome::Ruins:  return 3;  // Reliquary-> Cryo
        case Biome::Count:  break;
    }
    return 0;
}

bool Wasteland::loadBrutalist(Engine& engine) {
    const auto matte = [&](float r, float g, float b, float rough, float metal) {
        MaterialDesc d; d.baseColorFactor = { r, g, b, 1.0f };
        d.roughness = rough; d.metallic = metal; d.emissive = 0.0f;
        return engine.createMaterial(d);
    };
    const auto neon = [&](float r, float g, float b, float emis) {
        MaterialDesc d; d.baseColorFactor = { r, g, b, 1.0f };
        d.roughness = 0.45f; d.metallic = 0.0f; d.emissive = emis;   // color * emissive = HDR glow
        return engine.createMaterial(d);
    };
    // Raw-concrete industrial PBR (PolyHaven CC0 sets, diff/arm/nor_gl, prefers the BCn .dds). The
    // baseColorFactor tints the texture so the arena keeps a cool brutalist cast; uvScale tiles it.
    const auto loadPbr = [&](const char* id, float uvScale, float metalScale, float roughBoost,
                             Vec4f tint) -> MaterialHandle {
        const std::string base = std::string("assets/external/polyhaven/") + id + "/" + id + "_";
        const auto tex = [&](const char* map, bool srgb) -> TextureHandle {
            const std::string dds = resolveDir(base + map + "_1k.dds");
            if (std::filesystem::exists(dds)) {
                const TextureHandle h = engine.createTextureDDS(dds);
                if (h != TextureHandle::Invalid) return h;
            }
            uint32_t w = 0, ht = 0;
            std::vector<uint8_t> rgba = loadImageRGBA(resolveDir(base + map + "_1k.png"), w, ht);
            if (rgba.empty()) return TextureHandle::Invalid;
            TextureData td; td.width = w; td.height = ht; td.rgba = rgba.data(); td.srgb = srgb;
            return engine.createTexture(td);
        };
        MaterialDesc d;
        d.baseColor = tex("diff", true);
        if (d.baseColor == TextureHandle::Invalid) return MaterialHandle::Invalid;
        d.orm = tex("arm", false);
        d.normal = tex("nor_gl", false);
        d.baseColorFactor = tint;
        d.uvScale = uvScale; d.metalScale = metalScale; d.roughBoost = roughBoost;
        return engine.createMaterial(d);
    };
    const auto pbrOr = [&](MaterialHandle m, MaterialHandle fb) { return m != MaterialHandle::Invalid ? m : fb; };

    // M4: bake ONE palette per BIOME so each sector is a distinct place. The textured concrete/
    // metal slots share the PolyHaven sets but take a biome TINT; the two neon accents (the
    // dominant identity signal in this art style) get biome-specific colors:
    //   Foundry (Rocky)    - cold concrete + cyan/magenta (the original industrial look)
    //   Furnace (Forest)   - warm oxide concrete + amber/red neon (a hot smelting hall)
    //   Reliquary (Ruins)  - pale bone concrete + violet/green neon (an eerie vault)
    struct BiomePal {
        Vec4f wallTint, floorTint, darkTint, metalTint;
        Vec3f neonA, neonB; float emisA, emisB;
        Vec3f wallFb, floorFb, darkFb, metalFb;   // matte fallbacks if a PBR set is missing
    };
    const BiomePal pals[3] = {
        // Foundry
        { { 0.78f, 0.74f, 0.92f, 1.0f }, { 0.86f, 0.84f, 0.96f, 1.0f }, { 0.30f, 0.30f, 0.42f, 1.0f }, { 0.62f, 0.64f, 0.78f, 1.0f },
          { 0.20f, 1.30f, 1.65f }, { 1.45f, 0.30f, 1.15f }, 1.5f, 1.4f,
          { 0.32f, 0.25f, 0.54f }, { 0.40f, 0.37f, 0.52f }, { 0.05f, 0.05f, 0.12f }, { 0.10f, 0.10f, 0.14f } },
        // Furnace
        { { 0.92f, 0.72f, 0.56f, 1.0f }, { 0.90f, 0.78f, 0.66f, 1.0f }, { 0.36f, 0.24f, 0.20f, 1.0f }, { 0.80f, 0.62f, 0.50f, 1.0f },
          { 1.75f, 0.85f, 0.18f }, { 1.70f, 0.22f, 0.16f }, 1.6f, 1.5f,
          { 0.46f, 0.30f, 0.20f }, { 0.46f, 0.34f, 0.24f }, { 0.12f, 0.07f, 0.05f }, { 0.16f, 0.10f, 0.07f } },
        // Reliquary
        { { 0.90f, 0.88f, 0.82f, 1.0f }, { 0.88f, 0.86f, 0.80f, 1.0f }, { 0.34f, 0.30f, 0.40f, 1.0f }, { 0.70f, 0.70f, 0.76f, 1.0f },
          { 1.10f, 0.30f, 1.70f }, { 0.40f, 1.55f, 0.65f }, 1.5f, 1.4f,
          { 0.42f, 0.40f, 0.46f }, { 0.44f, 0.42f, 0.46f }, { 0.10f, 0.09f, 0.14f }, { 0.12f, 0.12f, 0.14f } },
    };
    for (int b = 0; b < static_cast<int>(Biome::Count); ++b) {
        const BiomePal& p = pals[b];
        brutalMat_[b][0] = pbrOr(loadPbr("factory_wall", 0.40f, 0.30f, 0.18f, p.wallTint),       // walls (concrete)
                                 matte(p.wallFb.x, p.wallFb.y, p.wallFb.z, 0.92f, 0.0f));
        brutalMat_[b][1] = pbrOr(loadPbr("concrete_floor_02", 0.38f, 0.0f, 0.12f, p.floorTint),  // platforms / light surfaces
                                 matte(p.floorFb.x, p.floorFb.y, p.floorFb.z, 0.90f, 0.0f));
        brutalMat_[b][2] = pbrOr(loadPbr("concrete_floor_02", 0.5f, 0.0f, 0.30f, p.darkTint),    // dark concrete (monoliths)
                                 matte(p.darkFb.x, p.darkFb.y, p.darkFb.z, 0.16f, 0.10f));
        brutalMat_[b][3] = neon(p.neonA.x, p.neonA.y, p.neonA.z, p.emisA);   // primary neon accent
        brutalMat_[b][4] = neon(p.neonB.x, p.neonB.y, p.neonB.z, p.emisB);   // secondary neon accent
        brutalMat_[b][5] = pbrOr(loadPbr("metal_plate_02", 0.5f, 0.55f, 0.12f, p.metalTint),     // metal trims / beams
                                 matte(p.metalFb.x, p.metalFb.y, p.metalFb.z, 0.34f, 0.82f));
    }

    // Load the real sci-fi modular tileset (shared atlas + floor/wall tile meshes) for the room
    // shell. If it loads, generateBrutalist tiles the real walls/floor and the baked templates skip
    // their procedural counterparts; if it is missing, the procedural shell stands in (fail-safe).
    loadTileset(engine);

    // Load the Quaternius MegaKit (CC0) modular palette. When present it becomes THE arena shell
    // (generate() dispatches to generateQuaternius), so the whole world is one Quaternius family.
    loadQuaternius(engine);

    // Bake one merged mesh per material slot, per curated template, once.
    for (int ti = 0; ti < static_cast<int>(AreaSize::Count); ++ti) {
        std::vector<StaticVertex> verts[kMat];
        std::vector<uint32_t>     idx[kMat];
        buildTemplateGeometry(kTemplates[ti], tilesetReady_, verts, idx);
        for (int m = 0; m < kMat; ++m)
            templateMesh_[ti][m] = verts[m].empty() ? MeshHandle::Invalid
                                                    : engine.createMesh({ verts[m], idx[m] });
    }
    // A shared unit cube (0,0,0)-(1,1,1) for per-room procedural cover and the sealed-door slabs;
    // scaled + translated per instance and drawn with the matte palette.
    {
        std::vector<StaticVertex> v; std::vector<uint32_t> bi;
        appendBox(v, bi, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        unitBoxMesh_ = engine.createMesh({ v, bi });
    }
    // Real-asset sci-fi hero structure (CC-BY, see assets/CREDITS.txt), loaded with its authored
    // PBR materials. First of the Sketchfab kit pieces wired into the brutalist arena. (The old
    // env_door wall section is gone - doorways now use animated sliding leaves drawn game-side.)
    reactor_ = loadKitProp(engine, "assets/external/sketchfab_scifi/power_reactor/scene.gltf", 4.5f);
    hallway_ = loadKitProp(engine, "assets/external/sketchfab_scifi/hallway_straight_01/scene.gltf", 6.5f);
    sketchfabHeroFocal_[static_cast<size_t>(Biome::Rocky)] =
        loadKitProp(engine, "assets/external/sketchfab_scifi/power_reactor/scene.gltf", 5.4f);
    sketchfabHeroFocal_[static_cast<size_t>(Biome::Forest)] =
        loadKitProp(engine, "assets/external/sketchfab_scifi/hallway_cross/scene.gltf", 5.0f);
    sketchfabHeroFocal_[static_cast<size_t>(Biome::Ruins)] =
        loadKitProp(engine, "assets/external/sketchfab_scifi/hallway_t/scene.gltf", 5.2f);
    loadMeshyEnvironmentProps(engine);
    ready_ = true;
    return true;
}

void Wasteland::setDoorOpen(int i, bool open) {
    if (i < 0 || i >= static_cast<int>(doors_.size())) return;
    Door& d = doors_[i];
    d.open = open;
    const int FW = 32 * kSub, FH = 24 * kSub;
    const uint8_t solid = open ? 0 : 1;
    const float   h     = open ? 0.0f : 99.0f;   // sealed -> unscalable wall
    for (int fz = d.fgz0; fz < d.fgz1; ++fz)
        for (int fx = d.fgx0; fx < d.fgx1; ++fx)
            if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                const size_t idx = static_cast<size_t>(fz) * FW + fx;
                fine_[idx] = solid;
                fineHeight_[idx] = h;
            }
}
void Wasteland::sealDoors() { for (int i = 0; i < static_cast<int>(doors_.size()); ++i) setDoorOpen(i, false); }
void Wasteland::openDoors() { for (int i = 0; i < static_cast<int>(doors_.size()); ++i) setDoorOpen(i, true); }

// Tile the real sci-fi kit into the room shell: a grid-fit floor across the play rect and
// segment-fit wall panels along each perimeter side, leaving the door openings clear. Every
// third wall panel is a cyan-trace accent (backed by a plain panel so its arch shows wall, not
// the void). Pure visual: collision stays the grid the rest of generateBrutalist already builds.
void Wasteland::emitTiledShell(float ox0, float oz0, float ox1, float oz1) {
    if (!tilesetReady_) return;
    const float rw = ox1 - ox0, rd = oz1 - oz0;

    // FLOOR: grid-fit (each tile stretched to an exact cell so the floor never gaps or overlaps).
    if (rw > 0.1f && rd > 0.1f && floorTileMesh_ != MeshHandle::Invalid && floorTileW_ > 0.1f && floorTileD_ > 0.1f) {
        const int nx = std::max(1, static_cast<int>(std::lround(rw / floorTileW_)));
        const int nz = std::max(1, static_cast<int>(std::lround(rd / floorTileD_)));
        const float tx = rw / nx, tz = rd / nz;
        const float sx = tx / floorTileW_, sz = tz / floorTileD_;
        for (int iz = 0; iz < nz; ++iz)
            for (int ix = 0; ix < nx; ++ix) {
                const float cx = ox0 + (static_cast<float>(ix) + 0.5f) * tx;
                const float cz = oz0 + (static_cast<float>(iz) + 0.5f) * tz;
                draws_.push_back({ floorTileMesh_, tilesetMat_, mul(scaling(sx, 1.0f, sz), translation(cx, 0.0f, cz)) });
            }
    }
    if (wallTileMesh_ == MeshHandle::Invalid || wallTileW_ <= 0.1f) return;

    // door gaps per side (from the actual openings: fine-cell rect / kSub).
    const auto gapsOn = [&](Side s, bool alongX) {
        std::vector<std::pair<float, float>> g;
        for (const Door& d : doors_)
            if (d.side == s)
                g.emplace_back(static_cast<float>(alongX ? d.fgx0 : d.fgz0) / kSub,
                               static_cast<float>(alongX ? d.fgx1 : d.fgz1) / kSub);
        std::sort(g.begin(), g.end());
        return g;
    };
    int panelCtr = 0;
    // walk one solid span [a,b] of a side in segment-fit steps. yaw rotates the panel so its width
    // (the kit's local Z) runs along the wall; out* points OUT of the room (for the accent backing).
    const auto runSpan = [&](float a, float b, float yaw, float fixedX, float fixedZ, float outX, float outZ) {
        if (b - a < 0.5f) return;
        const int n = std::max(1, static_cast<int>(std::lround((b - a) / wallTileW_)));
        const float seg = (b - a) / n, ws = seg / wallTileW_;
        const bool alongX = (yaw != 0.0f);
        for (int k = 0; k < n; ++k, ++panelCtr) {
            const float c = a + (static_cast<float>(k) + 0.5f) * seg;
            const float px = alongX ? c : fixedX, pz = alongX ? fixedZ : c;
            const Mat4 xf = mul(scaling(1.0f, 1.0f, ws), mul(rotationY(yaw), translation(px, 0.0f, pz)));
            if ((panelCtr % 3 == 2) && wallTracedMesh_ != MeshHandle::Invalid) {
                const Mat4 bxf = mul(scaling(1.0f, 1.0f, ws),
                                     mul(rotationY(yaw), translation(px + outX * 0.06f, 0.0f, pz + outZ * 0.06f)));
                draws_.push_back({ wallTileMesh_,   tilesetMat_, bxf });   // plain backing behind the arch
                draws_.push_back({ wallTracedMesh_, tilesetMat_, xf });    // cyan-trace accent
            } else {
                draws_.push_back({ wallTileMesh_, tilesetMat_, xf });
            }
        }
    };
    const auto runSide = [&](float lo, float hi, const std::vector<std::pair<float, float>>& gaps,
                             float yaw, float fixedX, float fixedZ, float outX, float outZ) {
        float cur = lo;
        for (const auto& gp : gaps) {
            const float gs = std::max(gp.first, lo), ge = std::min(gp.second, hi);
            if (gs > cur) runSpan(cur, gs, yaw, fixedX, fixedZ, outX, outZ);
            cur = std::max(cur, ge);
        }
        if (cur < hi) runSpan(cur, hi, yaw, fixedX, fixedZ, outX, outZ);
    };
    const float yawX = kPi * 0.5f;   // N/S walls: rotate so the panel width runs along world X
    runSide(ox0, ox1, gapsOn(Side::N, true),  yawX, 0.0f, oz0, 0.0f, -1.0f);   // N wall (room +Z)
    runSide(ox0, ox1, gapsOn(Side::S, true),  yawX, 0.0f, oz1, 0.0f,  1.0f);   // S wall (room -Z)
    runSide(oz0, oz1, gapsOn(Side::W, false), 0.0f, ox0, 0.0f, -1.0f, 0.0f);   // W wall (room +X)
    runSide(oz0, oz1, gapsOn(Side::E, false), 0.0f, ox1, 0.0f,  1.0f, 0.0f);   // E wall (room -X)
}

void Wasteland::generateBrutalist(AreaSize size, uint64_t seed, int roomIndex) {
    activeBiome_ = pendingBiome_;   // M4: this room's biome selects the palette (brutalMat_[biome])
    draws_.clear();
    doors_.clear();
    for (auto& row : grid_) row.assign(32, '#');
    const int FW = 32 * kSub, FH = 24 * kSub;
    fine_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 1);
    fineHeight_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 99.0f);
    spawnX_ = 16; spawnZ_ = 12;
    if (!ready_) return;

    const auto& bm = brutalMat_[static_cast<size_t>(activeBiome_)];   // M4: this biome's material palette
    const int ti = std::min(std::max(static_cast<int>(size), 0), static_cast<int>(AreaSize::Count) - 1);
    const LayoutTemplate& T = kTemplates[ti];
    lastSize_ = static_cast<AreaSize>(ti);
    centerX_ = 0.5f * (T.ox0 + T.ox1);
    centerZ_ = 0.5f * (T.oz0 + T.oz1);
    halfX_ = 0.5f * (T.ox1 - T.ox0);
    halfZ_ = 0.5f * (T.oz1 - T.oz0);

    // Open the interior play rect.
    for (int fz = fcell(T.oz0); fz < fcell(T.oz1); ++fz)
        for (int fx = fcell(T.ox0); fx < fcell(T.ox1); ++fx)
            if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                fine_[static_cast<size_t>(fz) * FW + fx] = 0;
                fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
            }

    // Build the doors and carve their gap channels walkable (re-sealed at the end of generate).
    for (int i = 0; i < T.doorCount; ++i) {
        Door d = makeDoor(T.doors[i], T);
        for (int fz = d.fgz0; fz < d.fgz1; ++fz)
            for (int fx = d.fgx0; fx < d.fgx1; ++fx)
                if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                    fine_[static_cast<size_t>(fz) * FW + fx] = 0;
                    fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
                }
        doors_.push_back(d);
    }

    // Stamp cover / monolith collision.
    for (int i = 0; i < T.boxCount; ++i) {
        const BBox& b = T.boxes[i];
        if (b.collide) stampSolidRect(b.x0, b.z0, b.x1, b.z1, b.h);
    }

    // Emit the baked static geometry for this template (the room's shell + architectural detail).
    for (int m = 0; m < kMat; ++m)
        if (templateMesh_[ti][m] != MeshHandle::Invalid)
            draws_.push_back({ templateMesh_[ti][m], bm[m],Mat4::identity() });

    // Real-asset sci-fi kit walls + floor tiled into the room shell (when the tileset loaded).
    emitTiledShell(T.ox0, T.oz0, T.ox1, T.oz1);

    // Real-asset sci-fi reactor hero toward the back of the room (the sci-fi door sections are
    // drawn game-side per doorway so they can open/close; see PulseGame::buildFrame).
    if (!reactor_.parts.empty()) {
        const float rz = centerZ_ - halfZ_ + reactor_.halfZ + 0.4f;   // sit against the back wall
        const Mat4 hxf = translation(centerX_, 0.0f, rz);
        for (const DungeonDraw& p : reactor_.parts) draws_.push_back({ p.mesh, p.material, hxf });
    }

    // Per-room NON-colliding decoration (deterministic from seed+roomIndex): emissive floor inlays
    // and wall screens so two rooms of the same SIZE still read differently. No stampSolidRect, so
    // the enemy AI is unaffected (unlike the reverted colliding-cover trial). Decoration is kept OUT
    // of the spawn pocket and away from the door mouths so nothing clutters where the player lands or
    // the connecting hallways read through an open door.
    if (unitBoxMesh_ != MeshHandle::Invalid) {
        Rng drng(seed ^ (0x2545F4914F6CDD1Dull * static_cast<uint64_t>(roomIndex + 3)) ^ (static_cast<uint64_t>(ti) << 40));
        const float spx = doors_.empty() ? centerX_ : static_cast<float>(doors_[0].spawnX);
        const float spz = doors_.empty() ? centerZ_ : static_cast<float>(doors_[0].spawnZ);
        const auto nearSpawn = [&](float x, float z) {
            return (x - spx) * (x - spx) + (z - spz) * (z - spz) < 3.2f * 3.2f;
        };
        const auto nearDoor = [&](float x, float z) {                 // any door opening (incl. hallway mouth)
            for (const Door& dd : doors_) {
                const float cx = dd.worldX + dd.inwardX * 1.4f, cz = dd.worldZ + dd.inwardZ * 1.4f;
                if ((x - cx) * (x - cx) + (z - cz) * (z - cz) < 2.6f * 2.6f) return true;
            }
            return false;
        };
        const float ix0 = T.ox0 + 2.2f, ix1 = T.ox1 - 2.2f, iz0 = T.oz0 + 2.2f, iz1 = T.oz1 - 2.2f;
        for (int k = 0, n = drng.range(2, 4); k < n && ix1 > ix0 && iz1 > iz0; ++k) {
            const float w = drng.frange(0.7f, 1.7f), d = drng.frange(0.7f, 1.7f);
            if (ix1 - w <= ix0 || iz1 - d <= iz0) continue;
            const float x = drng.frange(ix0, ix1 - w), z = drng.frange(iz0, iz1 - d);
            if (nearSpawn(x + w * 0.5f, z + d * 0.5f) || nearDoor(x + w * 0.5f, z + d * 0.5f)) continue;
            const int m = (drng.next() & 1u) ? 3 : 4;   // cyan or magenta inlay (flush with floor)
            draws_.push_back({ unitBoxMesh_, bm[m],mul(scaling(w, 0.03f, d), translation(x, 0.0f, z)) });
        }
        for (int k = 0, n = drng.range(1, 3); k < n; ++k) {
            const float len = drng.frange(0.8f, 1.6f), h = drng.frange(0.5f, 1.0f), y = drng.frange(1.2f, 2.5f);
            const int m = (drng.next() & 1u) ? 3 : 4;
            const int side = drng.range(0, 3);
            if (side <= 1) {                                  // N or S wall (panel varies in x)
                if (T.ox1 - 2.2f - len <= T.ox0 + 2.2f) continue;
                const float x = drng.frange(T.ox0 + 2.2f, T.ox1 - 2.2f - len);
                const float z = side == 0 ? T.oz0 + 0.03f : T.oz1 - 0.09f;
                if (nearDoor(x + len * 0.5f, z)) continue;    // never over a doorway/hallway mouth
                draws_.push_back({ unitBoxMesh_, bm[m],mul(scaling(len, h, 0.06f), translation(x, y, z)) });
            } else {                                          // W or E wall (panel varies in z)
                if (T.oz1 - 2.2f - len <= T.oz0 + 2.2f) continue;
                const float z = drng.frange(T.oz0 + 2.2f, T.oz1 - 2.2f - len);
                const float x = side == 2 ? T.ox0 + 0.03f : T.ox1 - 0.09f;
                if (nearDoor(x, z + len * 0.5f)) continue;
                draws_.push_back({ unitBoxMesh_, bm[m],mul(scaling(0.06f, h, len), translation(x, y, z)) });
            }
        }
    }

    // Down-sample fine -> coarse grid (walkable interior + open door cells read as '.').
    for (int z = 0; z < 24; ++z)
        for (int x = 0; x < 32; ++x) {
            const int fx = x * kSub + kSub / 2, fz = z * kSub + kSub / 2;
            grid_[static_cast<size_t>(z)][static_cast<size_t>(x)] = fine_[static_cast<size_t>(fz) * FW + fx] ? '#' : '.';
        }

    // Spawn just inside the entrance door [0]; clear a pocket so we never spawn inside cover.
    // Clamp the pocket to the interior so it never punches a hole through a perimeter wall.
    spawnX_ = doors_.empty() ? 16 : doors_[0].spawnX;
    spawnZ_ = doors_.empty() ? 12 : doors_[0].spawnZ;
    const int pfx0 = std::max((spawnX_ - 1) * kSub, fcell(T.ox0));
    const int pfx1 = std::min((spawnX_ + 2) * kSub, fcell(T.ox1));
    const int pfz0 = std::max((spawnZ_ - 1) * kSub, fcell(T.oz0));
    const int pfz1 = std::min((spawnZ_ + 2) * kSub, fcell(T.oz1));
    for (int fz = pfz0; fz < pfz1; ++fz)
        for (int fx = pfx0; fx < pfx1; ++fx)
            if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                fine_[static_cast<size_t>(fz) * FW + fx] = 0;
                fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
            }
    if (spawnX_ >= 0 && spawnX_ < 32 && spawnZ_ >= 0 && spawnZ_ < 24)
        grid_[static_cast<size_t>(spawnZ_)][static_cast<size_t>(spawnX_)] = 'P';

    // Combat starts with every door sealed.
    sealDoors();
}

// Dispatcher: prefer a hand-crafted room template (config/pulse.rooms) for this size; if none is
// authored for it, fall back to the procedural rectangular shell below.
void Wasteland::generateQuaternius(AreaSize size, uint64_t seed, int roomIndex) {
    if (!forcedTemplateName_.empty()) {   // dev/QA: build a specific named room (and its biome)
        const auto low = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(c)); return s; };
        const std::string want = low(forcedTemplateName_);
        for (const RoomTemplate& t : roomTemplates_)
            if (low(t.name) == want) {
                if (t.biome != Biome::Count) pendingBiome_ = t.biome;
                assembleQuaterniusRoom(t, seed, roomIndex);
                logInfo("rooms: --room forced '%s' (%s) draws=%d", t.name.c_str(), biomeName(pendingBiome_), static_cast<int>(draws_.size()));
                return;
            }
        logWarn("rooms: --room '%s' not found among %d templates; using normal pick",
                forcedTemplateName_.c_str(), static_cast<int>(roomTemplates_.size()));
    }
    if (const RoomTemplate* t = pickRoomTemplate(size, pendingBiome_, seed, roomIndex)) {
        assembleQuaterniusRoom(*t, seed, roomIndex);
        return;
    }
    generateQuaterniusProcedural(size, seed, roomIndex);
}

// FALLBACK procedural shell (used only when no template is authored for the size): a plain
// rectangular footprint - a floor platform per cell, perimeter wall panels (+ tops) leaving door
// gaps, corner columns, kit door frames, and crate cover. Fills the SAME outputs the rest of the
// game reads (grid_/fine_/draws_/doors_/spawn/centre), so PulseGame is unchanged.
void Wasteland::generateQuaterniusProcedural(AreaSize size, uint64_t seed, int roomIndex) {
    draws_.clear();
    doors_.clear();
    for (auto& row : grid_) row.assign(32, '#');
    const int FW = 32 * kSub, FH = 24 * kSub;
    fine_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 1);
    fineHeight_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 99.0f);
    spawnX_ = 16; spawnZ_ = 12;
    activeBiome_ = pendingBiome_;
    activeDoorLeafOverride_ = nullptr;
    if (!quatReady_) return;

    // Footprint in 4 m cells, centred in the 32x24 m world.
    int CW, CD;
    switch (size) {
        case AreaSize::Small:    CW = 5; CD = 4; break;
        case AreaSize::Corridor: CW = 7; CD = 3; break;
        case AreaSize::Big:      CW = 7; CD = 6; break;
        default:                 CW = 6; CD = 5; break;   // Mid
    }
    lastSize_ = size;
    const float cell = kQuatCell;
    const float rw = CW * cell, rd = CD * cell;
    const float ox0 = 16.0f - rw * 0.5f, oz0 = 12.0f - rd * 0.5f;
    const float ox1 = ox0 + rw, oz1 = oz0 + rd;
    centerX_ = 0.5f * (ox0 + ox1); centerZ_ = 0.5f * (oz0 + oz1);
    halfX_ = rw * 0.5f; halfZ_ = rd * 0.5f;

    const auto openRect = [&](int fx0, int fz0, int fx1, int fz1) {
        for (int fz = fz0; fz < fz1; ++fz)
            for (int fx = fx0; fx < fx1; ++fx)
                if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                    fine_[static_cast<size_t>(fz) * FW + fx] = 0;
                    fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
                }
    };
    openRect(fcell(ox0), fcell(oz0), fcell(ox1), fcell(oz1));   // walkable interior

    // ---- Doors: pick edge cells. [0] = entrance (S edge); exits on N (+W/E for bigger rooms). ----
    struct DoorCell { Side side; int cell; };
    std::vector<DoorCell> dcells;
    dcells.push_back({ Side::S, CW / 2 });                       // entrance
    dcells.push_back({ Side::N, std::max(0, CW / 2 - 1) });
    if (CW >= 6) dcells.push_back({ Side::N, std::min(CW - 1, CW / 2 + 1) });
    if (CD >= 4) dcells.push_back({ Side::W, CD / 2 });
    if (size == AreaSize::Big || size == AreaSize::Mid) dcells.push_back({ Side::E, CD / 2 });

    const float doorHalf = 1.6f;   // collision opening half-width (3.2 m walkable)
    for (const DoorCell& dc : dcells) {
        Door d; d.side = dc.side; d.open = false;
        if (dc.side == Side::N || dc.side == Side::S) {
            const float cx = ox0 + (static_cast<float>(dc.cell) + 0.5f) * cell;
            const float ez = (dc.side == Side::N) ? oz0 : oz1;
            d.worldX = cx; d.worldZ = ez;
            d.inwardX = 0.0f; d.inwardZ = (dc.side == Side::N) ? 1.0f : -1.0f;
            d.fgx0 = fcell(cx - doorHalf); d.fgx1 = fcell(cx + doorHalf);
            d.fgz0 = fcell(ez - 1.0f);     d.fgz1 = fcell(ez + 1.0f);
            d.spawnX = cellX(cx);          d.spawnZ = cellZ(ez + d.inwardZ * 1.5f);
        } else {
            const float cz = oz0 + (static_cast<float>(dc.cell) + 0.5f) * cell;
            const float ex = (dc.side == Side::W) ? ox0 : ox1;
            d.worldX = ex; d.worldZ = cz;
            d.inwardX = (dc.side == Side::W) ? 1.0f : -1.0f; d.inwardZ = 0.0f;
            d.fgx0 = fcell(ex - 1.0f);     d.fgx1 = fcell(ex + 1.0f);
            d.fgz0 = fcell(cz - doorHalf); d.fgz1 = fcell(cz + doorHalf);
            d.spawnX = cellX(ex + d.inwardX * 1.5f); d.spawnZ = cellZ(cz);
        }
        openRect(d.fgx0, d.fgz0, d.fgx1, d.fgz1);   // carve the channel (re-sealed at the end)
        doors_.push_back(d);
    }
    const auto isDoorCell = [&](Side s, int c) {
        for (const DoorCell& dc : dcells) if (dc.side == s && dc.cell == c) return true;
        return false;
    };

    Rng rng(seed ^ (0x2545F4914F6CDD1Dull * static_cast<uint64_t>(roomIndex + 5)) ^ (static_cast<uint64_t>(size) << 39));

    // ---- FLOOR: a 4x4 platform per cell (deterministic variant per cell) ----
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i) {
            const float cx = ox0 + (static_cast<float>(i) + 0.5f) * cell;
            const float cz = oz0 + (static_cast<float>(j) + 0.5f) * cell;
            const QuatPiece& fp = quatFloor_[mix(static_cast<uint32_t>(i * 73 + j * 131 + 7),
                                                 static_cast<uint32_t>(roomIndex + 1)) % quatFloor_.size()];
            placeQuat(draws_, fp, cx, 0.0f, cz, 0.0f);
        }

    // ---- WALLS: one straight panel per perimeter edge cell (skip door cells) + a top cap. The
    // panel is re-centred (spans +-sizeX/2 in X, inner face +X); place its centre offset by sizeX/2
    // off the boundary so the inner face lands ON the room edge. Rotations: W=0 E=pi N=-pi/2 S=+pi/2.
    const float kHalfPi = 1.5707963f, kPiF = 3.1415927f;
    const auto pickWall = [&](uint32_t h) -> const QuatPiece& {
        if (quatWall_.size() > 1 && (h % 4u) == 0u) return quatWall_[1 + (h / 4u) % (quatWall_.size() - 1)];
        return quatWall_[0];
    };
    const auto wallRun = [&](Side s) {
        const int n = (s == Side::N || s == Side::S) ? CW : CD;
        for (int c = 0; c < n; ++c) {
            if (isDoorCell(s, c)) continue;
            const QuatPiece& w = pickWall(mix(static_cast<uint32_t>(c) * 977u,
                                              static_cast<uint32_t>(static_cast<int>(s)) * 31u + static_cast<uint32_t>(roomIndex)));
            const float wd = (w.sizeX > 0.01f ? w.sizeX : 0.4f);
            float wx = 0, wz = 0, yaw = 0;
            if (s == Side::N)      { wx = ox0 + (static_cast<float>(c) + 0.5f) * cell; wz = oz0 - wd * 0.5f; yaw = -kHalfPi; }
            else if (s == Side::S) { wx = ox0 + (static_cast<float>(c) + 0.5f) * cell; wz = oz1 + wd * 0.5f; yaw =  kHalfPi; }
            else if (s == Side::W) { wz = oz0 + (static_cast<float>(c) + 0.5f) * cell; wx = ox0 - wd * 0.5f; yaw =  0.0f; }
            else                   { wz = oz0 + (static_cast<float>(c) + 0.5f) * cell; wx = ox1 + wd * 0.5f; yaw =  kPiF; }
            placeQuat(draws_, w, wx, 0.0f, wz, yaw);
            if (!quatTop_.parts.empty()) placeQuat(draws_, quatTop_, wx, w.sizeY, wz, yaw);   // cap above the wall
        }
    };
    wallRun(Side::N); wallRun(Side::S); wallRun(Side::W); wallRun(Side::E);

    // ---- Corner columns (mask the wall joins; 5 m vertical interest) ----
    if (!quatColumn_.parts.empty()) {
        const float cxs[4] = { ox0, ox1, ox0, ox1 };
        const float czs[4] = { oz0, oz0, oz1, oz1 };
        for (int k = 0; k < 4; ++k) placeQuat(draws_, quatColumn_, cxs[k], 0.0f, czs[k], 0.0f);
    }

    // ---- Door frames (static; PulseGame draws the animated leaves over the opening) ----
    if (!quatDoorFrame_.parts.empty())
        for (const Door& d : doors_) {
            const float yaw = (d.side == Side::W || d.side == Side::E) ? kHalfPi : 0.0f;
            placeQuat(draws_, quatDoorFrame_, d.worldX, 0.0f, d.worldZ, yaw);
        }

    // spawn just inside the entrance [0]
    spawnX_ = doors_.empty() ? 16 : doors_[0].spawnX;
    spawnZ_ = doors_.empty() ? 12 : doors_[0].spawnZ;
    const float spx = static_cast<float>(spawnX_) + 0.5f, spz = static_cast<float>(spawnZ_) + 0.5f;

    // ---- Cover: crate / barrel clusters at interior anchors (draws + collision), kept clear of the
    // spawn pocket and the door mouths so nothing clogs where the player lands or the openings. ----
    if (!quatCover_.empty()) {
        const float ix0 = ox0 + cell, ix1 = ox1 - cell, iz0 = oz0 + cell, iz1 = oz1 - cell;
        const auto nearSpawn = [&](float x, float z) { return (x - spx) * (x - spx) + (z - spz) * (z - spz) < 3.5f * 3.5f; };
        const auto nearDoor = [&](float x, float z) {
            for (const Door& dd : doors_) {
                const float cx = dd.worldX + dd.inwardX * 2.0f, cz = dd.worldZ + dd.inwardZ * 2.0f;
                if ((x - cx) * (x - cx) + (z - cz) * (z - cz) < 3.0f * 3.0f) return true;
            }
            return false;
        };
        const int nClust = (size == AreaSize::Small || size == AreaSize::Corridor) ? 2 : 4;
        for (int k = 0; k < nClust && ix1 > ix0 && iz1 > iz0; ++k) {
            float ax = 0, az = 0; bool ok = false;
            for (int a = 0; a < 12 && !ok; ++a) {
                ax = rng.frange(ix0, ix1); az = rng.frange(iz0, iz1);
                ok = !nearSpawn(ax, az) && !nearDoor(ax, az);
            }
            if (!ok) continue;
            for (int q = 0, pieces = rng.range(1, 3); q < pieces; ++q) {
                const QuatPiece& cp = quatCover_[rng.next() % quatCover_.size()];
                const float px = std::clamp(ax + rng.frange(-0.9f, 0.9f), ix0, ix1);
                const float pz = std::clamp(az + rng.frange(-0.9f, 0.9f), iz0, iz1);
                placeQuat(draws_, cp, px, 0.0f, pz, rng.frange(0.0f, 6.2831f));
                const float r = 0.5f * std::max(cp.sizeX, cp.sizeZ) + 0.05f;
                stampSolidRect(px - r, pz - r, px + r, pz + r, std::max(0.6f, cp.sizeY));
            }
        }
    }

    // clear a spawn pocket so we never spawn inside cover
    openRect((spawnX_ - 1) * kSub, (spawnZ_ - 1) * kSub, (spawnX_ + 2) * kSub, (spawnZ_ + 2) * kSub);

    // down-sample fine -> coarse grid (walkable interior + open door cells read as '.')
    for (int z = 0; z < 24; ++z)
        for (int x = 0; x < 32; ++x) {
            const int fx = x * kSub + kSub / 2, fz = z * kSub + kSub / 2;
            grid_[static_cast<size_t>(z)][static_cast<size_t>(x)] = fine_[static_cast<size_t>(fz) * FW + fx] ? '#' : '.';
        }
    if (spawnX_ >= 0 && spawnX_ < 32 && spawnZ_ >= 0 && spawnZ_ < 24)
        grid_[static_cast<size_t>(spawnZ_)][static_cast<size_t>(spawnX_)] = 'P';

    sealDoors();   // combat starts with every door shut
}

// Assemble a HAND-CRAFTED room from a data-file template (config/pulse.rooms). Reads the ASCII grid
// (1 char = one 4 m cell, ' ' = void so the footprint can be any shape), lays a floor tile per
// walkable cell, AUTO-walls every edge where a walkable cell meets void/solid (so non-rectangular
// shapes + interior blocks get walled correctly), frames the doors, and places cover / pillars /
// focal / props / raised platforms from the cell legend - all from the kit pieces the template's
// header names. Fills the same outputs (grid_/fine_/draws_/doors_/spawn/centre) PulseGame reads.
void Wasteland::assembleQuaterniusRoom(const RoomTemplate& T, uint64_t seed, int roomIndex) {
    draws_.clear();
    doors_.clear();
    for (auto& row : grid_) row.assign(32, '#');
    const int FW = 32 * kSub, FH = 24 * kSub;
    fine_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 1);
    fineHeight_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 99.0f);
    spawnX_ = 16; spawnZ_ = 12;
    activeBiome_ = pendingBiome_;
    activeDoorLeafOverride_ = nullptr;
    lastSize_ = T.size;
    if (!quatReady_ || T.grid.empty()) return;

    const float cell = kQuatCell, kHalfPi = 1.5707963f, kPiF = 3.1415927f;
    const int CD = static_cast<int>(T.grid.size());
    int CW = 0; for (const std::string& r : T.grid) CW = std::max(CW, static_cast<int>(r.size()));
    if (CW <= 0) return;
    const float rw = CW * cell, rd = CD * cell;
    float ox0 = std::clamp(16.0f - rw * 0.5f, 0.0f, std::max(0.0f, 32.0f - rw));
    float oz0 = std::clamp(12.0f - rd * 0.5f, 0.0f, std::max(0.0f, 24.0f - rd));
    const float ox1 = ox0 + rw, oz1 = oz0 + rd;
    centerX_ = 0.5f * (ox0 + ox1); centerZ_ = 0.5f * (oz0 + oz1);
    halfX_ = rw * 0.5f; halfZ_ = rd * 0.5f;

    // resolve the room's kit pieces (named in the header) with sensible defaults. resolveCached
    // accepts either the bare name or the "_Straight" stem (so families/tops/covers and the
    // already-full derelict walls all bind).
    const QuatPiece* floorP = resolveCached(T.floor); if (!floorP && !quatFloor_.empty()) floorP = &quatFloor_[0];
    std::string wallFamilyName = T.family;
    const QuatPiece* wallP  = resolveCached(wallFamilyName);
    if (activeBiome_ == Biome::Forest && wallFamilyName.find("WallPadded") != std::string::npos) {
        if (const QuatPiece* p = resolveCached("WallPipe")) {
            wallP = p;
            wallFamilyName = "WallPipe";
        }
    }
    if (!wallP && !quatWall_.empty()) wallP = &quatWall_[0];
    const QuatPiece* topP   = resolveCached(T.top); if (!topP && !quatTop_.parts.empty()) topP = &quatTop_;
    const QuatPiece* colP   = resolveCached(T.pillar); if (!colP && !quatColumn_.parts.empty()) colP = &quatColumn_;
    const QuatPiece* coverP = resolveCached(T.cover);
    const QuatPiece* focalP = resolveCached(T.focal);
    const QuatPiece* daisP  = resolveCached(T.daisFloor);
    if (!daisP && activeBiome_ == Biome::Forest) {
        daisP = resolveCached("Platform_RedAccent");
        if (!daisP) daisP = resolveCached("Platform_Metal2");
        if (!daisP) daisP = resolveCached("Platform_X");
    }
    if (!daisP) daisP = floorP;   // raised-deck floor override
    if (!wallP || !floorP) return;
    // Per-biome filler pools (spec biome.dressing), with the legacy fixed pools as fallback.
    const size_t bi = static_cast<size_t>(activeBiome_) < static_cast<size_t>(Biome::Count)
                    ? static_cast<size_t>(activeBiome_) : 0;
    const std::vector<QuatPiece>& cratePool = !quatCrateBiome_[bi].empty() ? quatCrateBiome_[bi] : quatCover_;
    const std::vector<QuatPiece>& dressPool = !quatDressBiome_[bi].empty() ? quatDressBiome_[bi] : quatDress_;
    const MeshyBiomeProps& meshyProps = meshyProps_[bi];
    const auto pickMeshy = [](const std::vector<KitProp>& pool, uint32_t h) -> const KitProp* {
        return pool.empty() ? nullptr : &pool[h % static_cast<uint32_t>(pool.size())];
    };
    const auto pickMeshyToken = [&](const std::vector<KitProp>& pool, uint32_t h,
                                    const char* primary, const char* secondary = nullptr) -> const KitProp* {
        if (pool.empty()) return nullptr;
        const auto pickToken = [&](const char* token, uint32_t salt) -> const KitProp* {
            if (!token) return nullptr;
            uint32_t matches = 0;
            for (const KitProp& p : pool)
                if (p.source.find(token) != std::string::npos) ++matches;
            if (matches == 0) return nullptr;
            uint32_t wanted = salt % matches;
            for (const KitProp& p : pool) {
                if (p.source.find(token) == std::string::npos) continue;
                if (wanted-- == 0) return &p;
            }
            return nullptr;
        };
        if (const KitProp* p = pickToken(primary, h)) return p;
        if (const KitProp* p = pickToken(secondary, h >> 1)) return p;
        return pickMeshy(pool, h);
    };
    const auto pickMeshyNamed = [&](const std::vector<KitProp>& pool, uint32_t h,
                                    std::initializer_list<const char*> primary,
                                    std::initializer_list<const char*> secondary) -> const KitProp* {
        if (pool.empty()) return nullptr;
        const auto hit = [](const KitProp& p, std::initializer_list<const char*> tokens) {
            return tokens.size() > 0 && nameHas(p.source, tokens);
        };
        const auto pickNamed = [&](std::initializer_list<const char*> tokens, uint32_t salt) -> const KitProp* {
            uint32_t matches = 0;
            for (const KitProp& p : pool)
                if (hit(p, tokens)) ++matches;
            if (matches == 0) return nullptr;
            uint32_t wanted = salt % matches;
            for (const KitProp& p : pool) {
                if (!hit(p, tokens)) continue;
                if (wanted-- == 0) return &p;
            }
            return nullptr;
        };
        if (const KitProp* p = pickNamed(primary, h)) return p;
        if (const KitProp* p = pickNamed(secondary, h >> 1)) return p;
        return pickMeshy(pool, h);
    };
    std::vector<const QuatPiece*> templateDressPool;
    for (const std::string& n : T.dressingPool)
        if (const QuatPiece* p = cachedPiece(n)) templateDressPool.push_back(p);
    std::vector<const QuatPiece*> templateWallDecalPool;
    for (const std::string& n : T.decalGroup) {
        if (n.find("Line") != std::string::npos) continue;
        if (const QuatPiece* p = cachedPiece(n)) templateWallDecalPool.push_back(p);
    }
    const QuatPiece* rampP = resolveCached(T.ramp);
    if (!rampP) rampP = (activeBiome_ == Biome::Ruins && !quatStairs_.parts.empty()) ? &quatStairs_
                     : (!quatRamp_.parts.empty() ? &quatRamp_ : nullptr);
    const auto firstCellFitColumn = [&](std::initializer_list<const char*> names) -> const QuatPiece* {
        for (const char* n : names) {
            const QuatPiece* p = cachedPiece(n);
            if (!p || p->parts.empty()) continue;
            if (p->sizeX <= cell * 0.52f && p->sizeZ <= cell * 0.52f &&
                p->sizeY <= ceilingHeightFor(activeBiome_) + 0.35f)
                return p;
        }
        return nullptr;
    };
    const auto combatPillar = [&]() -> const QuatPiece* {
        if (!colP) return nullptr;
        const bool oversizedFootprint = colP->sizeX > cell * 0.52f || colP->sizeZ > cell * 0.52f;
        const bool tooTall = colP->sizeY > ceilingHeightFor(activeBiome_) + 0.35f;
        const bool landmarkOrOpen = T.pillar.find("Large") != std::string::npos ||
                                    T.pillar.find("Hollow") != std::string::npos;
        if (!oversizedFootprint && !tooTall && !landmarkOrOpen) return colP;
        if (activeBiome_ == Biome::Ruins)
            if (const QuatPiece* p = firstCellFitColumn({ "Column_Dark", "Column_SimpleSquare", "Column_Round" })) return p;
        if (activeBiome_ == Biome::Forest)
            if (const QuatPiece* p = firstCellFitColumn({ "Column_Pipes", "Column_Dark", "Column_SimpleSquare" })) return p;
        if (const QuatPiece* p = firstCellFitColumn({ "Column_MetalSupport", "Column_Dark", "Column_SimpleSquare", "Column_Round" })) return p;
        return colP;
    }();
    if (const QuatPiece* doorLeafP = resolveCached(T.doorLeaf)) activeDoorLeafOverride_ = doorLeafP;
    const QuatPiece* cratePieceP = resolveCached(T.cratePiece);
    const QuatPiece* bottomP = (activeBiome_ == Biome::Forest) ? cachedPiece("BottomMetal_Straight")
                              : (activeBiome_ == Biome::Ruins) ? cachedPiece("BottomSimple_Straight")
                              : cachedPiece("BottomAccent_Straight");
    const QuatPiece* wallFixtureA = (activeBiome_ == Biome::Forest) ? cachedPiece("Prop_Vent_Wide")
                                 : (activeBiome_ == Biome::Ruins)  ? cachedPiece("Prop_Light_Wide")
                                 : cachedPiece("Prop_AccessPoint");
    const QuatPiece* wallFixtureB = (activeBiome_ == Biome::Forest) ? cachedPiece("Prop_PipeHolder")
                                 : (activeBiome_ == Biome::Ruins)  ? cachedPiece("Prop_Light_Floor")
                                 : cachedPiece("Prop_PipeHolder");
    const QuatPiece* wallFixtureC = (activeBiome_ == Biome::Forest) ? cachedPiece("Prop_Pipe_Thick_Straight")
                                 : (activeBiome_ == Biome::Ruins)  ? cachedPiece("Prop_Cable_2")
                                 : cachedPiece("Prop_Cable_2");
    const auto firstCached = [&](std::initializer_list<const char*> names) -> const QuatPiece* {
        for (const char* n : names)
            if (const QuatPiece* p = cachedPiece(n)) return p;
        return nullptr;
    };
    const QuatPiece* decalDoor = (activeBiome_ == Biome::Ruins) ? firstCached({ "Decal_STRNOV", "Decal_Logo_Small" })
                               : (activeBiome_ == Biome::Forest) ? firstCached({ "Decal_Warning", "Decal_Caution" })
                               : firstCached({ "Decal_Caution", "Decal_Authorized" });
    const QuatPiece* decalFocal = (activeBiome_ == Biome::Ruins) ? firstCached({ "Decal_Logo", "Decal_K", "Decal_V", "Decal_X", "Decal_Z" })
                                 : (activeBiome_ == Biome::Forest) ? firstCached({ "Decal_Caution", "Decal_XSign" })
                                 : firstCached({ "Decal_Code", "Decal_Code_2", "Decal_AccessPoint" });
    const auto pickWallDecalPiece = [&](uint32_t h, const QuatPiece* fallback) -> const QuatPiece* {
        if (!templateWallDecalPool.empty()) return templateWallDecalPool[h % templateWallDecalPool.size()];
        return fallback;
    };
    const QuatPiece* focalLightP = cachedPiece("Prop_Light_Floor");
    const QuatPiece* midfieldFloorP = (activeBiome_ == Biome::Ruins) ? firstCached({ "Platform_Round2", "Platform_Round1", "Platform_CenterPlate" })
                                      : (activeBiome_ == Biome::Forest) ? firstCached({ "Platform_RedAccent", "Platform_Metal2", "Platform_X" })
                                      : firstCached({ "Platform_CenterPlate", "Platform_X", "Platform_Squares" });
    const QuatPiece* midfieldFloorAltP = (activeBiome_ == Biome::Ruins) ? firstCached({ "Platform_Round1", "Platform_Simple_Curve" })
                                         : (activeBiome_ == Biome::Forest) ? firstCached({ "Platform_DarkPlates_Curves", "Platform_CenterPlate", "Platform_Metal_Curve" })
                                         : firstCached({ "Platform_CenterPlate_Curve", "Platform_Metal2_Curve", "Platform_Metal" });
    const QuatPiece* midfieldCableA = (activeBiome_ == Biome::Forest) ? firstCached({ "Prop_Pipe_Thick_Straight", "Prop_Pipe_Medium_Straight" })
                                    : (activeBiome_ == Biome::Ruins)  ? firstCached({ "Prop_Cable_1", "Prop_Cable_2" })
                                    : firstCached({ "Prop_Cable_2", "Prop_Cable_3", "Prop_Cable_1" });
    const QuatPiece* midfieldCableB = (activeBiome_ == Biome::Forest) ? firstCached({ "Prop_Pipe_Small_Straight", "Prop_Cable_4" })
                                    : (activeBiome_ == Biome::Ruins)  ? firstCached({ "Prop_Cable_3", "Prop_Cable_4" })
                                    : firstCached({ "Prop_Pipe_Medium_Straight", "Prop_Cable_4" });
    const QuatPiece* midfieldMachineP = (activeBiome_ == Biome::Forest) ? firstCached({ "Prop_Vent_Small", "Prop_Clamp", "Prop_Barrel_Small" })
                                      : (activeBiome_ == Biome::Ruins)  ? firstCached({ "Prop_Light_Floor", "Prop_Light_Corner" })
                                      : firstCached({ "Prop_Light_Small", "Prop_Fan_Small", "Prop_Vent_Small" });
    const QuatPiece* furnaceCoverP = (activeBiome_ == Biome::Forest)
                                   ? firstCached({ "Prop_Barrel_Small", "Prop_Pipe_Thick_Curve", "Prop_PipeHolder" })
                                   : nullptr;
    std::vector<const QuatPiece*> furnaceSafeDress;
    if (activeBiome_ == Biome::Forest) {
        for (const char* n : { "Prop_Pipe_Thick_Straight", "Prop_Pipe_Thick_Curve",
                               "Prop_Pipe_Medium_Straight", "Prop_Barrel_Small",
                               "Prop_Ammo_Small", "Prop_Grenade" })
            if (const QuatPiece* p = cachedPiece(n)) furnaceSafeDress.push_back(p);
    }
    const auto familyStem = [](std::string n) {
        const std::string straightBroken = "_Straight_Broken";
        const std::string straight = "_Straight";
        const size_t b = n.find(straightBroken);
        if (b != std::string::npos) n = n.substr(0, b);
        const size_t s = n.find(straight);
        if (s != std::string::npos) n = n.substr(0, s);
        return n;
    };
    const std::string wallStem = familyStem(wallFamilyName);
    const std::string topStem = !T.top.empty() ? familyStem(T.top) : std::string("TopPlates");
    const std::string bottomStem = (activeBiome_ == Biome::Forest) ? "BottomMetal"
                                 : (activeBiome_ == Biome::Ruins)  ? "BottomSimple"
                                 : "BottomAccent";
    const QuatPiece* wallCornerOuterP = cachedPiece(wallStem + "_Corner_Square_Outer");
    const QuatPiece* wallCornerInnerP = cachedPiece(wallStem + "_Corner_Square_Inner");
    const QuatPiece* topCornerOuterP = cachedPiece(topStem + "_Corner_Square_Outer");
    const QuatPiece* topCornerInnerP = cachedPiece(topStem + "_Corner_Square_Inner");
    const QuatPiece* bottomCornerOuterP = cachedPiece(bottomStem + "_Corner_Square_Outer_1");
    if (!bottomCornerOuterP) bottomCornerOuterP = cachedPiece(bottomStem + "_Corner_Square_Outer_2");
    const QuatPiece* bottomCornerInnerP = cachedPiece(bottomStem + "_Corner_Square_Inner");
    // The Quaternius corner caps are sculptural pieces authored for fixed kit layouts. On arbitrary
    // ASCII footprints they can throw fins/wedges outside the wall line, so the assembler uses only
    // straight wall spans plus the compact structural post boxes emitted below.
    wallCornerOuterP = wallCornerInnerP = nullptr;
    topCornerOuterP = topCornerInnerP = nullptr;
    bottomCornerOuterP = bottomCornerInnerP = nullptr;
    // The kit floor PLATFORMS are flat zero-thickness planes (size.y == 0), so on their own they
    // read as 2D paper wherever an edge shows (room rim, door mouths, raised decks). Place a solid
    // slab box under each floor tile, and a full riser block under each raised deck, using
    // biome-specific PBR support materials so floors/decks read as real 3D mass instead of
    // stretched kit planes. (unitBoxMesh_ spans (0,0,0)-(1,1,1).)
    const size_t biomeIdx = static_cast<size_t>(activeBiome_);
    const MaterialHandle floorKitMat = (floorP && !floorP->parts.empty()) ? floorP->parts[0].material : MaterialHandle{};
    const MaterialHandle slabMat = (biomeIdx < static_cast<size_t>(Biome::Count) &&
                                    quatFloorMassMat_[biomeIdx] != MaterialHandle::Invalid)
                                 ? quatFloorMassMat_[biomeIdx] : floorKitMat;
    const auto placeSlab = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        if (unitBoxMesh_ == MeshHandle::Invalid) return;
        draws_.push_back({ unitBoxMesh_, slabMat, mul(scaling(x1 - x0, y1 - y0, z1 - z0), translation(x0, y0, z0)) });
    };
    // ENCLOSURE: rooms are SEALED INTERIORS (the bible's power plant / smelting hall / sealed vault),
    // not open-topped pads. CEIL is the per-biome ceiling height; a per-cell ceiling seals the room
    // overhead and a filler band seals the gap above the kit wall panels, so the sun (CSM) is occluded
    // and the biome's local light rig + emissive trim do the lighting (dark pools, not a flat sky).
    const float CEIL = ceilingHeightFor(activeBiome_);
    const float kDeckHeight = 2.0f;                                       // raised 'H' deck height (+2 m, spec)
    const float kWallTop = (wallP->sizeY > 0.5f ? wallP->sizeY : 3.0f);   // top of the kit wall band
    const MaterialHandle ceilMat = (quatCeilingMat_ != MaterialHandle::Invalid) ? quatCeilingMat_ : slabMat;
    const MaterialHandle trimMat = (biomeIdx < static_cast<size_t>(Biome::Count))
                                 ? quatTrimMat_[biomeIdx] : MaterialHandle{};
    const auto placeBoxMat = [&](MaterialHandle m, float x0, float y0, float z0, float x1, float y1, float z1) {
        if (unitBoxMesh_ == MeshHandle::Invalid || m == MaterialHandle{}) return;
        draws_.push_back({ unitBoxMesh_, m, mul(scaling(x1 - x0, y1 - y0, z1 - z0), translation(x0, y0, z0)) });
    };
    const auto placeScaledQuat = [&](const QuatPiece* p, float wx, float yOff, float wz, float yaw, float sx, float sy, float sz) {
        if (!p || p->parts.empty()) return;
        const Mat4 xf = mul(scaling(sx, sy, sz), mul(rotationY(yaw), translation(wx, yOff, wz)));
        for (const DungeonDraw& part : p->parts) draws_.push_back({ part.mesh, part.material, xf });
    };
    const auto placeWallDecal = [&](const QuatPiece* p, float wx, float wy, float wz, float nx, float nz, float scale) {
        if (!p || p->parts.empty()) return;
        const Vec3f wallNormal = normalize3({ nx, 0.0f, nz });
        if (dot3(wallNormal, wallNormal) <= 1e-6f) return;
        const Vec3f wallTangent = (std::fabs(wallNormal.x) > 0.5f) ? Vec3f{ 0.0f, 0.0f, 1.0f }
                                                                   : Vec3f{ 1.0f, 0.0f, 0.0f };
        const Mat4 xf = basis(wallTangent, wallNormal, { 0.0f, 1.0f, 0.0f }, { wx, wy, wz }, scale);
        for (const DungeonDraw& part : p->parts) draws_.push_back({ part.mesh, part.material, xf });
    };
    const auto cwx = [&](int i) { return ox0 + (static_cast<float>(i) + 0.5f) * cell; };
    const auto cwz = [&](int j) { return oz0 + (static_cast<float>(j) + 0.5f) * cell; };
    const auto rotatedPlanarExtents = [](float halfX, float halfZ, float yaw, float& outX, float& outZ) {
        const float c = std::fabs(std::cos(yaw));
        const float s = std::fabs(std::sin(yaw));
        outX = c * halfX + s * halfZ;
        outZ = s * halfX + c * halfZ;
    };
    const auto fitScaleInsideCell = [&](float halfX, float halfZ, float yaw, float desiredScale, float margin) {
        if (halfX <= 0.001f || halfZ <= 0.001f || desiredScale <= 0.001f) return desiredScale;
        float ex = 0.0f, ez = 0.0f;
        rotatedPlanarExtents(halfX, halfZ, yaw, ex, ez);
        const float maxHalf = std::max(0.20f, cell * 0.5f - margin);
        float fit = desiredScale;
        if (ex > 0.001f) fit = std::min(fit, maxHalf / ex);
        if (ez > 0.001f) fit = std::min(fit, maxHalf / ez);
        return std::clamp(fit, 0.001f, desiredScale);
    };
    const auto clampPlanarInsideCell = [&](int i, int j, float halfX, float halfZ, float yaw,
                                           float scale, float margin, float& px, float& pz) {
        float ex = 0.0f, ez = 0.0f;
        rotatedPlanarExtents(halfX * scale, halfZ * scale, yaw, ex, ez);
        const float cx = cwx(i), cz = cwz(j);
        const float minX = cx - cell * 0.5f + margin + ex;
        const float maxX = cx + cell * 0.5f - margin - ex;
        const float minZ = cz - cell * 0.5f + margin + ez;
        const float maxZ = cz + cell * 0.5f - margin - ez;
        px = (minX <= maxX) ? std::clamp(px, minX, maxX) : cx;
        pz = (minZ <= maxZ) ? std::clamp(pz, minZ, maxZ) : cz;
    };
    const auto safeKitScale = [&](const KitProp& p, float yaw, float desiredScale) {
        return fitScaleInsideCell(p.halfX, p.halfZ, yaw, desiredScale, 0.28f);
    };
    const auto clampKitToCell = [&](const KitProp& p, int i, int j, float yaw, float scale, float& px, float& pz) {
        clampPlanarInsideCell(i, j, p.halfX, p.halfZ, yaw, scale, 0.28f, px, pz);
    };
    const auto safeQuatScale = [&](const QuatPiece& p, float yaw, float desiredScale) {
        return fitScaleInsideCell(p.sizeX * 0.5f, p.sizeZ * 0.5f, yaw, desiredScale, 0.24f);
    };
    const auto safeQuatUniformScale = [&](const QuatPiece& p, float yaw, float desiredScale, float maxHeight) {
        float s = safeQuatScale(p, yaw, desiredScale);
        if (p.sizeY > 0.001f)
            s = std::min(s, std::max(0.001f, maxHeight) / p.sizeY);
        return std::clamp(s, 0.001f, desiredScale);
    };
    const auto clampQuatToCell = [&](const QuatPiece& p, int i, int j, float yaw, float scale, float& px, float& pz) {
        clampPlanarInsideCell(i, j, p.sizeX * 0.5f, p.sizeZ * 0.5f, yaw, scale, 0.24f, px, pz);
    };
    const auto stampGroundedPropCollision = [&](float wx, float yOff, float wz,
                                                float halfX, float halfZ, float height,
                                                float yaw, float sx, float sz, float maxHalf) {
        if (height <= 0.0f || yOff > 0.18f) return;
        const float top = yOff + height;
        if (top < 0.55f) return;

        float ex = 0.0f, ez = 0.0f;
        rotatedPlanarExtents(halfX * sx, halfZ * sz, yaw, ex, ez);
        const float widest = std::max(ex, ez);
        const float narrow = std::min(ex, ez);
        if (widest < 0.20f) return;
        if (narrow < 0.06f && top < 0.95f) return;

        const float hx = std::clamp(ex * 0.68f, 0.16f, maxHalf);
        const float hz = std::clamp(ez * 0.68f, 0.16f, maxHalf);
        stampSolidRect(wx - hx, wz - hz, wx + hx, wz + hz, top);
    };
    const auto stampKitPropCollision = [&](const KitProp& p, float wx, float yOff, float wz,
                                           float yaw, float uniformScale, float maxHalf) {
        stampGroundedPropCollision(wx, yOff, wz, p.halfX, p.halfZ, p.height * uniformScale,
                                   yaw, uniformScale, uniformScale, maxHalf);
    };
    const auto stampQuatPropCollision = [&](const QuatPiece& p, float wx, float yOff, float wz,
                                            float yaw, float sx, float sy, float sz, float maxHalf) {
        stampGroundedPropCollision(wx, yOff, wz, p.sizeX * 0.5f, p.sizeZ * 0.5f, p.sizeY * sy,
                                   yaw, sx, sz, maxHalf);
    };

    const auto chr = [&](int i, int j) -> char {
        if (j < 0 || j >= CD || i < 0) return ' ';
        const std::string& r = T.grid[static_cast<size_t>(j)];
        return (i >= static_cast<int>(r.size())) ? ' ' : r[static_cast<size_t>(i)];
    };
    const auto isRoomCell = [](char c) {
        return c != ' ';
    };
    const auto isFloor = [](char c) {
        return c == '.' || c == 'c' || c == '=' || c == 'o' || c == 'X' || c == 'H' ||
               c == 'p' || c == '/' || c == 'D' || c == 'E';
    };
    const int dxs[4] = { 0, 0, -1, 1 }, dzs[4] = { -1, 1, 0, 0 };
    const auto openCell = [&](int i, int j) {
        for (int fz = fcell(oz0 + j * cell); fz < fcell(oz0 + (j + 1) * cell); ++fz)
            for (int fx = fcell(ox0 + i * cell); fx < fcell(ox0 + (i + 1) * cell); ++fx)
                if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                    fine_[static_cast<size_t>(fz) * FW + fx] = 0;
                    fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
                }
    };

    struct CellInfo {
        char c = ' ';
        int i = 0, j = 0;
        float x = 0.0f, z = 0.0f;
        bool room = false, floor = false, blocked = false, door = false, raised = false, focal = false;
        bool lane = false, edgePocket = false, cornerPocket = false, doorZone = false, focalZone = false, raisedEdge = false;
        int wallAdj = 0, doorSide = -1;
    };
    std::vector<CellInfo> cells(static_cast<size_t>(CW) * static_cast<size_t>(CD));
    const auto cinfo = [&](int i, int j) -> CellInfo* {
        if (i < 0 || j < 0 || i >= CW || j >= CD) return nullptr;
        return &cells[static_cast<size_t>(j) * static_cast<size_t>(CW) + static_cast<size_t>(i)];
    };
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i) {
            CellInfo& ci = *cinfo(i, j);
            ci.c = chr(i, j); ci.i = i; ci.j = j; ci.x = cwx(i); ci.z = cwz(j);
            ci.room = isRoomCell(ci.c);
            ci.floor = isFloor(ci.c);
            ci.blocked = ci.c == ' ' || ci.c == '#' || ci.c == 'o' || ci.c == 'X';
            ci.door = ci.c == 'D' || ci.c == 'E';
            ci.raised = ci.c == 'H';
            ci.focal = ci.c == 'X';
            if (!ci.floor) continue;
            const bool laneX = isFloor(chr(i - 1, j)) && isFloor(chr(i + 1, j));
            const bool laneZ = isFloor(chr(i, j - 1)) && isFloor(chr(i, j + 1));
            ci.lane = laneX || laneZ;
            int wallAdj = 0;
            for (int s = 0; s < 4; ++s)
                if (!isFloor(chr(i + dxs[s], j + dzs[s]))) ++wallAdj;
            ci.wallAdj = wallAdj;
            ci.edgePocket = wallAdj == 1;
            ci.cornerPocket = wallAdj >= 2;
            ci.raisedEdge = ci.c == 'H' && (!isFloor(chr(i - 1, j)) || !isFloor(chr(i + 1, j)) ||
                                            !isFloor(chr(i, j - 1)) || !isFloor(chr(i, j + 1)));
        }
    for (CellInfo& ci : cells) {
        if (!ci.floor) continue;
        for (const CellInfo& anchor : cells) {
            if (!anchor.floor) continue;
            const int md = std::abs(ci.i - anchor.i) + std::abs(ci.j - anchor.j);
            if (anchor.door && md <= 1) ci.doorZone = true;
            if (anchor.focal && md <= 1) ci.focalZone = true;
        }
    }

    // pass 1: floor tiles (+ a solid slab under each for real thickness) + open walkable collision
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i)
            if (isFloor(chr(i, j))) {
                openCell(i, j);
                placeQuat(draws_, *floorP, cwx(i), 0.0f, cwz(j), 0.0f);
                placeSlab(cwx(i) - 2.0f, -0.32f, cwz(j) - 2.0f, cwx(i) + 2.0f, -0.02f, cwz(j) + 2.0f);  // floor edge mass
                for (int s = 0; s < 4; ++s) {
                    if (isRoomCell(chr(i + dxs[s], j + dzs[s]))) continue;
                    const float cx = cwx(i), cz = cwz(j);
                    if (s == 0)      placeBoxMat(slabMat, cx - 2.0f, -0.34f, cz - 2.0f, cx + 2.0f, 0.10f, cz - 1.74f);
                    else if (s == 1) placeBoxMat(slabMat, cx - 2.0f, -0.34f, cz + 1.74f, cx + 2.0f, 0.10f, cz + 2.0f);
                    else if (s == 2) placeBoxMat(slabMat, cx - 2.0f, -0.34f, cz - 2.0f, cx - 1.74f, 0.10f, cz + 2.0f);
                    else             placeBoxMat(slabMat, cx + 1.74f, -0.34f, cz - 2.0f, cx + 2.0f, 0.10f, cz + 2.0f);
                }
                if (!openTop_) {
                    // sealed ceiling tile over the cell (interior, not open sky)
                    placeBoxMat(ceilMat, cwx(i) - 2.0f, CEIL - 0.30f, cwz(j) - 2.0f, cwx(i) + 2.0f, CEIL, cwz(j) + 2.0f);
                    // FOUNDRY: overhead cyan conduit strip-lights (every other row reads as a ceiling run)
                    if (activeBiome_ == Biome::Rocky && (j % 2) == 0)
                        placeBoxMat(trimMat, cwx(i) - 1.7f, CEIL - 0.44f, cwz(j) - 0.22f, cwx(i) + 1.7f, CEIL - 0.33f, cwz(j) + 0.22f);
                }
            }

    // pass 2: structural masses + auto-walls (+ tops). Walkable cells wall only against void;
    // '#' cells are solid equipment/bunker mass and own the interior partition panels toward floor.
    const MaterialHandle wallKitMat = (!wallP->parts.empty() && wallP->parts[0].material != MaterialHandle{})
                                    ? wallP->parts[0].material : ceilMat;
    const MaterialHandle structMat = (biomeIdx < static_cast<size_t>(Biome::Count) &&
                                      quatStructMat_[biomeIdx] != MaterialHandle::Invalid)
                                   ? quatStructMat_[biomeIdx] : wallKitMat;
    const MaterialHandle deckMassMat = (biomeIdx < static_cast<size_t>(Biome::Count) &&
                                        quatDeckMassMat_[biomeIdx] != MaterialHandle::Invalid)
                                     ? quatDeckMassMat_[biomeIdx] : structMat;
    const MaterialHandle platformSideMat = (activeBiome_ == Biome::Forest) ? structMat : deckMassMat;
    int wallDecalsPlaced = 0;
    const int maxWallDecals = (T.size == AreaSize::Big) ? 10 : (T.size == AreaSize::Mid ? 7 : 4);
    int meshyWallMountsPlaced = 0;
    const int maxMeshyWallMounts = (T.size == AreaSize::Big) ? 10 : (T.size == AreaSize::Mid ? 7 : 4);
    std::vector<uint8_t> wallPosts(static_cast<size_t>(CW + 1) * static_cast<size_t>(CD + 1), 0);
    const auto markWallPost = [&](int vi, int vj) {
        if (vi < 0 || vj < 0 || vi > CW || vj > CD) return;
        wallPosts[static_cast<size_t>(vj) * static_cast<size_t>(CW + 1) + static_cast<size_t>(vi)] = 1;
    };
    const auto placeWall = [&](int side, int i, int j, bool faceCurrentCell) {
        const float wd = wallP->sizeX, cx = cwx(i), cz = cwz(j);
        const int sx = (side == 2) ? -1 : (side == 3) ? 1 : 0;
        const int sz = (side == 0) ? -1 : (side == 1) ? 1 : 0;
        const float nx = faceCurrentCell ? static_cast<float>(-sx) : static_cast<float>(sx);
        const float nz = faceCurrentCell ? static_cast<float>(-sz) : static_cast<float>(sz);
        float bx = cx, bz = cz, yaw = 0.0f;
        // Edge plane coordinate, for the solid backing, upper filler band, and baseboard.
        float ex0 = cx - 2.0f, ez0 = cz - 2.0f, ex1 = cx + 2.0f, ez1 = cz + 2.0f;   // cell footprint
        if (side == 0)      { bz = oz0 + j * cell;       ez0 = bz - 0.42f; ez1 = bz + 0.42f; } // N edge
        else if (side == 1) { bz = oz0 + (j + 1) * cell; ez0 = bz - 0.42f; ez1 = bz + 0.42f; } // S edge
        else if (side == 2) { bx = ox0 + i * cell;       ex0 = bx - 0.42f; ex1 = bx + 0.42f; } // W edge
        else                { bx = ox0 + (i + 1) * cell; ex0 = bx - 0.42f; ex1 = bx + 0.42f; } // E edge
        const float wx = bx - nx * wd * 0.5f;
        const float wz = bz - nz * wd * 0.5f;
        if (nx > 0.5f) yaw = 0.0f;
        else if (nx < -0.5f) yaw = kPiF;
        else if (nz > 0.5f) yaw = -kHalfPi;
        else yaw = kHalfPi;
        if (side == 0)      { markWallPost(i, j);     markWallPost(i + 1, j); }
        else if (side == 1) { markWallPost(i, j + 1); markWallPost(i + 1, j + 1); }
        else if (side == 2) { markWallPost(i, j);     markWallPost(i, j + 1); }
        else                { markWallPost(i + 1, j); markWallPost(i + 1, j + 1); }
        placeBoxMat(structMat, ex0, 0.0f, ez0, ex1, kWallTop + 0.06f, ez1);
        placeBoxMat(ceilMat,   ex0, kWallTop - 0.08f, ez0, ex1, kWallTop + 0.18f, ez1);
        placeQuat(draws_, *wallP, wx, 0.0f, wz, yaw);
        if (topP) placeQuat(draws_, *topP, wx, wallP->sizeY, wz, yaw);
        if (bottomP) placeQuat(draws_, *bottomP, wx, 0.02f, wz, yaw);
        // Seal the band between the kit wall top and the ceiling so no sky shows (enclosed interior).
        if (!openTop_ && CEIL > kWallTop + 0.05f) placeBoxMat(ceilMat, ex0, kWallTop, ez0, ex1, CEIL, ez1);
        // FURNACE: narrow molten seams at wall feet; accents, not glowing block faces.
        if (activeBiome_ == Biome::Forest) placeBoxMat(trimMat, ex0, 0.035f, ez0, ex1, 0.105f, ez1);
        // Biome-specific wall fixtures give the generated spans real kit detail instead of a
        // repeated-panel hallway. They sit just inside the wall, never in the middle lane.
        const uint32_t wh = mix(static_cast<uint32_t>((i + 13) * 193u + (j + 7) * 977u),
                                static_cast<uint32_t>(side * 53 + roomIndex * 19));
        const bool alongX = (side == 0 || side == 1);
        const float edgeX = bx + nx * 0.56f;
        const float edgeZ = bz + nz * 0.56f;
        const float fy = (wh % 7u == 0u) ? 0.86f : 0.06f;
        if ((wh % 5u) == 0u && wallFixtureA)
            placeQuat(draws_, *wallFixtureA, edgeX, fy, edgeZ, yaw);
        else if ((wh % 9u) == 0u && wallFixtureB)
            placeQuat(draws_, *wallFixtureB, edgeX, 0.08f, edgeZ, yaw);
        else if ((wh % 13u) == 0u && wallFixtureC) {
            const float pipeYaw = alongX ? kHalfPi : 0.0f;
            placeQuat(draws_, *wallFixtureC, edgeX, 0.10f, edgeZ, pipeYaw);
        }
        if (wallDecalsPlaced < maxWallDecals && (!templateWallDecalPool.empty() || decalDoor || decalFocal)) {
            const uint32_t dh = mix(wh, static_cast<uint32_t>(roomIndex * 733 + side * 107 + static_cast<int>(activeBiome_) * 61));
            const uint32_t chance = (activeBiome_ == Biome::Ruins) ? 30u : (activeBiome_ == Biome::Forest ? 18u : 26u);
            if ((dh % 100u) < chance) {
                const QuatPiece* wallDecal = pickWallDecalPiece(dh >> 3, (dh & 1u) ? decalDoor : decalFocal);
                if (wallDecal) {
                    const float y = std::clamp(1.05f + static_cast<float>((dh >> 8) % 4u) * 0.28f,
                                               0.90f, std::max(0.95f, kWallTop - 0.55f));
                    const float scale = (activeBiome_ == Biome::Ruins) ? 0.54f : 0.58f;
                    placeWallDecal(wallDecal, edgeX + nx * 0.025f, y, edgeZ + nz * 0.025f, nx, nz, scale);
                    ++wallDecalsPlaced;
                }
            }
        }
        if (meshyWallMountsPlaced < maxMeshyWallMounts && !meshyProps.wallDetails.empty()) {
            const uint32_t mh = mix(wh, static_cast<uint32_t>(roomIndex * 431 + static_cast<int>(activeBiome_) * 173 + side * 29));
            const uint32_t chance = (activeBiome_ == Biome::Ruins) ? 22u : (activeBiome_ == Biome::Forest ? 34u : 30u);
            if ((mh % 100u) < chance) {
                const bool lightMount = (mh & 3u) == 0u;
                const KitProp* mp = lightMount
                    ? pickMeshyNamed(meshyProps.wallDetails, mh >> 3,
                                     { "light_bar", "bar_panel", "wall_light", "industrial_wall_light" },
                                     { "cyan_light", "orange_bar" })
                    : pickMeshyNamed(meshyProps.wallDetails, mh >> 4,
                                     { "terminal", "display", "control", "status", "radar", "lens", "keypad", "network", "log" },
                                     { "wall_panel", "bar_panel" });
                if (mp && !mp->parts.empty()) {
                    const float mountY = lightMount
                        ? std::clamp(kWallTop - 1.35f, 1.55f, 2.25f)
                        : std::clamp(0.58f + (mh % 3u) * 0.22f, 0.58f, 1.10f);
                    const float scale = lightMount ? 0.44f : 0.48f;
                    const float plateHalf = lightMount ? 1.12f : 0.72f;
                    const float plateY0 = lightMount ? mountY + 0.06f : std::max(0.18f, mountY - 0.08f);
                    const float plateY1 = lightMount ? mountY + 0.22f : std::min(kWallTop - 0.24f, mountY + 1.30f);
                    if (alongX)
                        placeBoxMat(deckMassMat,
                                    edgeX - plateHalf, plateY0, edgeZ - 0.055f,
                                    edgeX + plateHalf, plateY1, edgeZ + 0.055f);
                    else
                        placeBoxMat(deckMassMat,
                                    edgeX - 0.055f, plateY0, edgeZ - plateHalf,
                                    edgeX + 0.055f, plateY1, edgeZ + plateHalf);
                    if (trimMat != MaterialHandle{}) {
                        const float trimY0 = lightMount ? plateY0 + 0.035f : plateY0 + 0.12f;
                        const float trimY1 = lightMount ? plateY1 - 0.035f : std::min(plateY1 - 0.08f, trimY0 + 0.055f);
                        const float trimHalf = lightMount ? plateHalf * 0.82f : plateHalf * 0.34f;
                        if (alongX)
                            placeBoxMat(trimMat,
                                        edgeX - trimHalf, trimY0, edgeZ + nz * 0.072f - 0.018f,
                                        edgeX + trimHalf, trimY1, edgeZ + nz * 0.072f + 0.018f);
                        else
                            placeBoxMat(trimMat,
                                        edgeX + nx * 0.072f - 0.018f, trimY0, edgeZ - trimHalf,
                                        edgeX + nx * 0.072f + 0.018f, trimY1, edgeZ + trimHalf);
                    }
                    placeKitProp(draws_, *mp,
                                 edgeX + nx * 0.10f,
                                 mountY,
                                 edgeZ + nz * 0.10f,
                                 yaw,
                                 scale);
                    ++meshyWallMountsPlaced;
                }
            }
        }
    };
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i)
            if (chr(i, j) == '#') {
                const float cx = cwx(i), cz = cwz(j);
                // Solid structures need visible mass, but not the stretched random-box look. Keep the
                // core slightly inset so the kit wall panels provide the readable exterior surfaces.
                placeBoxMat(structMat, cx - 1.72f, 0.02f, cz - 1.72f, cx + 1.72f, kWallTop, cz + 1.72f);
                placeBoxMat(ceilMat,   cx - 1.78f, kWallTop - 0.10f, cz - 1.78f, cx + 1.78f, kWallTop + 0.03f, cz + 1.78f);
            }
    struct DoorOpening {
        int i = 0, j = 0, side = -1;
        bool entrance = false;
        float x = 0.0f, z = 0.0f;
    };
    std::vector<DoorOpening> semanticDoors;
    constexpr float kDoorSpawnInset = 2.6f; // deeper than the door frame, still inside the 4 m entrance cell
    const auto makeDoorEdge = [&](int side, int i, int j, bool entrance) {
        const float cx = cwx(i), cz = cwz(j);
        Door d; d.open = false;
        if (side == 0)      { d.side = Side::N; const float e = oz0 + j * cell; d.worldX = cx; d.worldZ = e; d.inwardZ = 1.0f;
                              d.fgx0 = fcell(cx - 1.6f); d.fgx1 = fcell(cx + 1.6f); d.fgz0 = fcell(e - 1.0f); d.fgz1 = fcell(e + 1.0f);
                              d.spawnX = cellX(cx); d.spawnZ = cellZ(e + kDoorSpawnInset); }
        else if (side == 1) { d.side = Side::S; const float e = oz0 + (j + 1) * cell; d.worldX = cx; d.worldZ = e; d.inwardZ = -1.0f;
                              d.fgx0 = fcell(cx - 1.6f); d.fgx1 = fcell(cx + 1.6f); d.fgz0 = fcell(e - 1.0f); d.fgz1 = fcell(e + 1.0f);
                              d.spawnX = cellX(cx); d.spawnZ = cellZ(e - kDoorSpawnInset); }
        else if (side == 2) { d.side = Side::W; const float e = ox0 + i * cell; d.worldX = e; d.worldZ = cz; d.inwardX = 1.0f;
                              d.fgx0 = fcell(e - 1.0f); d.fgx1 = fcell(e + 1.0f); d.fgz0 = fcell(cz - 1.6f); d.fgz1 = fcell(cz + 1.6f);
                              d.spawnX = cellX(e + kDoorSpawnInset); d.spawnZ = cellZ(cz); }
        else                { d.side = Side::E; const float e = ox0 + (i + 1) * cell; d.worldX = e; d.worldZ = cz; d.inwardX = -1.0f;
                              d.fgx0 = fcell(e - 1.0f); d.fgx1 = fcell(e + 1.0f); d.fgz0 = fcell(cz - 1.6f); d.fgz1 = fcell(cz + 1.6f);
                              d.spawnX = cellX(e - kDoorSpawnInset); d.spawnZ = cellZ(cz); }
        for (int fz = d.fgz0; fz < d.fgz1; ++fz)
            for (int fx = d.fgx0; fx < d.fgx1; ++fx)
                if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) { fine_[static_cast<size_t>(fz) * FW + fx] = 0; fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f; }
        if (CellInfo* ci = cinfo(i, j)) ci->doorSide = side;
        semanticDoors.push_back({ i, j, side, entrance, d.worldX, d.worldZ });
        if (d.side == Side::N || d.side == Side::S) {
            placeBoxMat(structMat, cx - 2.0f, 0.0f, d.worldZ - 0.42f, cx - 1.58f, kWallTop + 0.08f, d.worldZ + 0.42f);
            placeBoxMat(structMat, cx + 1.58f, 0.0f, d.worldZ - 0.42f, cx + 2.0f,  kWallTop + 0.08f, d.worldZ + 0.42f);
        } else {
            placeBoxMat(structMat, d.worldX - 0.42f, 0.0f, cz - 2.0f, d.worldX + 0.42f, kWallTop + 0.08f, cz - 1.58f);
            placeBoxMat(structMat, d.worldX - 0.42f, 0.0f, cz + 1.58f, d.worldX + 0.42f, kWallTop + 0.08f, cz + 2.0f);
        }
        // Lintel: seal the band above the doorway up to the ceiling (keep the ~2.6 m opening clear).
        const float dTop = 2.6f;
        if (!openTop_ && CEIL > dTop + 0.05f) {
            if (d.side == Side::N || d.side == Side::S)
                placeBoxMat(ceilMat, cx - 2.0f, dTop, d.worldZ - 0.22f, cx + 2.0f, CEIL, d.worldZ + 0.22f);
            else
                placeBoxMat(ceilMat, d.worldX - 0.22f, dTop, cz - 2.0f, d.worldX + 0.22f, CEIL, cz + 2.0f);
        }
        if (entrance) doors_.insert(doors_.begin(), d); else doors_.push_back(d);
    };
    const auto doorSideFor = [&](int i, int j) {
        // Contract priority: prefer true exterior voids over authored interior voids, then E,W,N,S.
        const int priority[4] = { 3, 2, 0, 1 };
        int interior = -1;
        for (int k = 0; k < 4; ++k) {
            const int s = priority[k];
            const int ni = i + dxs[s], nj = j + dzs[s];
            if (chr(ni, nj) != ' ') continue;
            const bool exterior = nj < 0 || nj >= CD || ni < 0 ||
                ni >= static_cast<int>(T.grid[static_cast<size_t>(nj)].size());
            if (exterior) return s;
            if (interior < 0) interior = s;
        }
        return interior;
    };
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i) {
            const char c = chr(i, j);
            if (!isRoomCell(c)) continue;
            if (c == '#') {
                for (int s = 0; s < 4; ++s) {
                    const char nb = chr(i + dxs[s], j + dzs[s]);
                    if (nb == '#') continue;
                    if (nb == ' ' || isFloor(nb)) placeWall(s, i, j, false);
                }
                continue;
            }
            if (!isFloor(c)) continue;
            const int doorSide = (c == 'D' || c == 'E') ? doorSideFor(i, j) : -1;
            for (int s = 0; s < 4; ++s) {
                const char nb = chr(i + dxs[s], j + dzs[s]);
                if (nb == '#') continue;                  // the solid structure owns this partition
                if (nb != ' ') continue;                  // neighbour walkable -> no wall
                if (s == doorSide) makeDoorEdge(s, i, j, c == 'E');
                else placeWall(s, i, j, true);
            }
        }

    // pass 2b: real kit corner pieces at every footprint turn. The straight wall pass owns the
    // spans; these pieces finish the joints so corners stop reading as overlapped panel ends.
    const auto cornerYawForQuadrant = [&](int q) {
        // q: 0=NW, 1=NE, 2=SW, 3=SE. Kit corner meshes are centred on the grid vertex.
        if (q == 0) return kPiF;
        if (q == 1) return -kHalfPi;
        if (q == 2) return kHalfPi;
        return 0.0f;
    };
    const auto placeCornerSet = [&](const QuatPiece* wallC, const QuatPiece* topC, const QuatPiece* bottomC,
                                    float vx, float vz, float yaw) {
        if (wallC) placeQuat(draws_, *wallC, vx, 0.0f, vz, yaw);
        if (topC) placeQuat(draws_, *topC, vx, kWallTop, vz, yaw);
        if (bottomC) placeQuat(draws_, *bottomC, vx, 0.02f, vz, yaw);
    };
    if (wallCornerOuterP || wallCornerInnerP || topCornerOuterP || topCornerInnerP ||
        bottomCornerOuterP || bottomCornerInnerP) {
        for (int vj = 0; vj <= CD; ++vj)
            for (int vi = 0; vi <= CW; ++vi) {
                const bool nw = isRoomCell(chr(vi - 1, vj - 1));
                const bool ne = isRoomCell(chr(vi,     vj - 1));
                const bool sw = isRoomCell(chr(vi - 1, vj));
                const bool se = isRoomCell(chr(vi,     vj));
                const int count = (nw ? 1 : 0) + (ne ? 1 : 0) + (sw ? 1 : 0) + (se ? 1 : 0);
                if (count != 1 && count != 3) continue;
                int q = 3; // occupied quadrant for convex corners; empty quadrant for concave corners.
                if (count == 1) {
                    if (nw) q = 0; else if (ne) q = 1; else if (sw) q = 2; else q = 3;
                    placeCornerSet(wallCornerOuterP, topCornerOuterP, bottomCornerOuterP,
                                   ox0 + static_cast<float>(vi) * cell,
                                   oz0 + static_cast<float>(vj) * cell,
                                   cornerYawForQuadrant(q));
                } else {
                    if (!nw) q = 0; else if (!ne) q = 1; else if (!sw) q = 2; else q = 3;
                    placeCornerSet(wallCornerInnerP, topCornerInnerP, bottomCornerInnerP,
                                   ox0 + static_cast<float>(vi) * cell,
                                   oz0 + static_cast<float>(vj) * cell,
                                   cornerYawForQuadrant(q));
                }
            }
    }
    for (int vj = 0; vj <= CD; ++vj)
        for (int vi = 0; vi <= CW; ++vi)
            if (wallPosts[static_cast<size_t>(vj) * static_cast<size_t>(CW + 1) + static_cast<size_t>(vi)]) {
                const float vx = ox0 + static_cast<float>(vi) * cell;
                const float vz = oz0 + static_cast<float>(vj) * cell;
                placeBoxMat(structMat, vx - 0.22f, 0.0f, vz - 0.22f, vx + 0.22f, kWallTop + 0.12f, vz + 0.22f);
                placeBoxMat(ceilMat,   vx - 0.28f, kWallTop - 0.08f, vz - 0.28f, vx + 0.28f, kWallTop + 0.20f, vz + 0.28f);
            }

    // Meshy finishers: hide the "kit panel ends" look at wall joints, give doors real machinery,
    // and leave enough budget for the Round 1/3 prop pass below. Visual-only.
    int meshyWallFinishPlaced = 0;
    const int maxMeshyWallFinish = (T.size == AreaSize::Big) ? 12 : (T.size == AreaSize::Mid ? 8 : 5);
    // Vertex seam props are too silhouette-heavy for this grid assembler: at exterior turns they read
    // as fins poking through the wall. The structural post boxes above now own wall-joint cleanup.
    for (const Door& d : doors_) {
        const float tx = -d.inwardZ, tz = d.inwardX;
        const float faceYaw = std::atan2(-d.inwardX, -d.inwardZ);
        if (!meshyCommon_.doorThreshold.parts.empty())
            placeKitProp(draws_, meshyCommon_.doorThreshold,
                         d.worldX + d.inwardX * 0.38f, 0.035f,
                         d.worldZ + d.inwardZ * 0.38f, faceYaw, 0.70f);
        if (!meshyCommon_.doorSide.parts.empty()) {
            const uint32_t h = mix(static_cast<uint32_t>(roomIndex * 199 + static_cast<int>(activeBiome_) * 37),
                                   static_cast<uint32_t>(static_cast<int>(d.side) * 271 + static_cast<int>(doors_.size()) * 19));
            const float side = (h & 1u) ? 1.0f : -1.0f;
            placeKitProp(draws_, meshyCommon_.doorSide,
                         d.worldX + tx * side * 1.72f + d.inwardX * 0.42f, 0.02f,
                         d.worldZ + tz * side * 1.72f + d.inwardZ * 0.42f, faceYaw, 0.56f);
        }
        if (!meshyCommon_.doorLintel.parts.empty())
            placeKitProp(draws_, meshyCommon_.doorLintel,
                         d.worldX, 2.48f, d.worldZ + d.inwardZ * 0.28f, faceYaw, 0.54f);
    }

    int meshyAnchorsPlaced = 0;
    int meshyDeckFinishPlaced = 0;
    int meshyFloorDetailsPlaced = 0;
    // Density per the art bible ("release density is several times the blockout - on the edges").
    // Lifted for the dense biomes; Reliquary stays deliberately sparse (eerie, monumental).
    const int maxMeshyAnchors = (T.size == AreaSize::Big) ? (activeBiome_ == Biome::Ruins ? 5 : 9)
                               : (T.size == AreaSize::Mid) ? (activeBiome_ == Biome::Ruins ? 4 : 6)
                               : (activeBiome_ == Biome::Ruins ? 3 : 4);
    const int maxMeshyFloorDetails = (T.size == AreaSize::Big) ? (activeBiome_ == Biome::Ruins ? 7 : 10)
                                   : (T.size == AreaSize::Mid ? (activeBiome_ == Biome::Ruins ? 5 : 7) : 3);

    // pass 3: cell content (cover / pillar / focal / dressing / raised / ramp) + collision
    Rng rng(seed ^ (0x2545F4914F6CDD1Dull * static_cast<uint64_t>(roomIndex + 11)) ^ (static_cast<uint64_t>(T.size) << 38));
    const QuatPiece* rail4P = firstCached({ "Prop_Rail_4", "Prop_Rail_3", "Platform_Rails_4", "Platform_Rails_4Wide" });
    const QuatPiece* rail2P = firstCached({ "Prop_Rail_2", "Platform_Rails_2" });
    const auto pickDressPiece = [&](uint32_t h) -> const QuatPiece* {
        if (activeBiome_ == Biome::Forest && !furnaceSafeDress.empty()) return furnaceSafeDress[h % furnaceSafeDress.size()];
        if (!templateDressPool.empty()) return templateDressPool[h % templateDressPool.size()];
        if (!dressPool.empty()) return &dressPool[h % dressPool.size()];
        return nullptr;
    };
    const bool ceilingRoomBroad = T.size == AreaSize::Big || (T.size == AreaSize::Mid && CW * CD >= 28);
    if (!openTop_ && ceilingRoomBroad &&
        (!meshyCommon_.ceilingDucts.empty() || !meshyCommon_.ceilingDuct.parts.empty() || !meshyCommon_.ceilingSpine.parts.empty())) {
        const int maxCeiling = (T.size == AreaSize::Big) ? 3 : 2;
        int placed = 0;
        for (int j = 0; j < CD && placed < maxCeiling; ++j)
            for (int i = 0; i < CW && placed < maxCeiling; ++i) {
                const CellInfo* ci = cinfo(i, j);
                if (!ci || ci->c != '.') continue;
                if (ci->doorZone || ci->focalZone) continue;
                const bool laneX = isFloor(chr(i - 1, j)) && isFloor(chr(i + 1, j));
                const bool laneZ = isFloor(chr(i, j - 1)) && isFloor(chr(i, j + 1));
                if (!laneX && !laneZ) continue;
                const bool central = std::abs(i - CW / 2) <= 1 || std::abs(j - CD / 2) <= 1;
                const uint32_t h = mix(static_cast<uint32_t>((i + 17) * 821u + (j + 31) * 109u),
                                       static_cast<uint32_t>(roomIndex * 421 + static_cast<int>(activeBiome_) * 53));
                if (!central && (h % 100u) > 28u) continue;
                const KitProp* duct = pickMeshy(meshyCommon_.ceilingDucts, h >> 2);
                if (!duct && !meshyCommon_.ceilingDuct.parts.empty()) duct = &meshyCommon_.ceilingDuct;
                const KitProp* cp = ((h & 2u) && !meshyCommon_.ceilingSpine.parts.empty())
                                  ? &meshyCommon_.ceilingSpine
                                  : (duct ? duct : (!meshyCommon_.ceilingSpine.parts.empty() ? &meshyCommon_.ceilingSpine : nullptr));
                if (!cp || cp->parts.empty()) continue;
                const float yaw = laneX && !laneZ ? kHalfPi : ((h & 1u) ? kHalfPi : 0.0f);
                const float y = std::max(kWallTop + 0.12f, CEIL - cp->height - 0.10f);
                placeKitProp(draws_, *cp, ci->x, y, ci->z, yaw, 0.72f);
                ++placed;
            }
    }
    const auto placeDeckSkirt = [&](int i, int j, float cx, float cz) {
        // Visual support for the raised height field. Collision remains a full 4 m cell at +2 m,
        // but the art reads as skirted modular decking instead of a giant stretched cube.
        for (int s = 0; s < 4; ++s) {
            const char nb = chr(i + dxs[s], j + dzs[s]);
            if (nb == 'H' || nb == '/') continue;
            float x0 = cx - 2.0f, z0 = cz - 2.0f, x1 = cx + 2.0f, z1 = cz + 2.0f;
            if (s == 0)      { z0 = cz - 2.0f; z1 = z0 + 0.18f; }
            else if (s == 1) { z1 = cz + 2.0f; z0 = z1 - 0.18f; }
            else if (s == 2) { x0 = cx - 2.0f; x1 = x0 + 0.18f; }
            else             { x1 = cx + 2.0f; x0 = x1 - 0.18f; }
            placeBoxMat(platformSideMat, x0, 0.0f, z0, x1, kDeckHeight, z1);
            placeBoxMat(trimMat, x0, kDeckHeight - 0.045f, z0, x1, kDeckHeight + 0.005f, z1);
        }
    };
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i) {
            const char c = chr(i, j);
            const float cx = cwx(i), cz = cwz(j);
            if (c == 'o') {                                  // pillar (full-height hard cover)
                if (combatPillar) placeQuat(draws_, *combatPillar, cx, 0.0f, cz, 0.0f);
                if (combatPillar) {
                    // Some Quaternius columns are intentionally open modular shells. From the
                    // player camera that reads as unfinished blockout, so the compiler gives every
                    // combat pillar a restrained plinth and cap in the biome structural material.
                    const float halfX = std::clamp(combatPillar->sizeX * 0.50f + 0.06f, 0.42f, 0.82f);
                    const float halfZ = std::clamp(combatPillar->sizeZ * 0.50f + 0.06f, 0.42f, 0.82f);
                    const float baseX = std::min(halfX + 0.10f, 0.96f);
                    const float baseZ = std::min(halfZ + 0.10f, 0.96f);
                    const float ph = std::clamp(combatPillar->sizeY, 2.2f, CEIL - 0.12f);
                    placeBoxMat(deckMassMat, cx - baseX, 0.00f, cz - baseZ, cx + baseX, 0.08f, cz + baseZ);
                    placeBoxMat(structMat,   cx - halfX, 0.08f, cz - halfZ, cx + halfX, 0.20f, cz + halfZ);
                    placeBoxMat(trimMat,     cx - halfX, 0.20f, cz - halfZ, cx + halfX, 0.24f, cz + halfZ);
                    placeBoxMat(deckMassMat, cx - halfX, ph - 0.08f, cz - halfZ, cx + halfX, ph + 0.02f, cz + halfZ);
                }
                stampSolidCircle(cx, cz, 0.6f, 5.0f);
            } else if (c == 'c') {                           // DELIBERATE low cover: a tidy crate, grid-aligned
                // (not a random scatter - that read as "random boxes"). One crate at the cell centre,
                // axis-aligned, sometimes a second stacked on top for vertical interest. Deterministic
                // per cell so a seed reproduces the layout exactly.
                const QuatPiece* coverCrateP = furnaceCoverP ? furnaceCoverP : cratePieceP;
                if (coverCrateP || !cratePool.empty()) {
                    const uint32_t pick = static_cast<uint32_t>(i * 7 + j * 13 + roomIndex);
                    const QuatPiece& cp = coverCrateP ? *coverCrateP : cratePool[pick % cratePool.size()];
                    const float yaw = ((i + j) & 1) ? kHalfPi : 0.0f;            // aligned to the grid, not spun
                    const float ch = std::max(0.6f, cp.sizeY);
                    placeQuat(draws_, cp, cx, 0.0f, cz, yaw);
                    if (!furnaceCoverP && ((i * 3 + j * 5 + roomIndex) % 3) == 0) // occasional 2-stack
                        placeQuat(draws_, cp, cx, ch, cz, yaw + 0.18f);
                    const float r = 0.5f * std::max(cp.sizeX, cp.sizeZ) + 0.05f;
                    stampSolidRect(cx - r, cz - r, cx + r, cz + r, ch);
                }
            } else if (c == '=') {                           // low wall / railing across the lane
                const bool laneNS = isFloor(chr(i, j - 1)) || isFloor(chr(i, j + 1));
                const float yaw = laneNS ? kHalfPi : 0.0f;   // bar runs across the open lane
                if (coverP) placeQuat(draws_, *coverP, cx, 0.0f, cz, yaw);
                if (laneNS) stampSolidRect(cx - 1.9f, cz - 0.35f, cx + 1.9f, cz + 0.35f, 1.4f);
                else        stampSolidRect(cx - 0.35f, cz - 1.9f, cx + 0.35f, cz + 1.9f, 1.4f);
            } else if (c == 'X') {                           // focal hero prop (landmark)
                const uint32_t fh = mix(static_cast<uint32_t>((i + 41) * 173u + (j + 29) * 281u),
                                        static_cast<uint32_t>(roomIndex * 911 + static_cast<int>(activeBiome_) * 59));
                bool raisedFocal = false;
                if (focalP) {
                    const float yaw = (activeBiome_ == Biome::Ruins) ? kHalfPi
                                    : ((fh & 1u) ? kHalfPi : 0.0f);
                    const bool displayScale = nameHas(T.focal, { "planet", "hologram" }) &&
                        (focalP->sizeY > CEIL * 0.75f ||
                         std::max(focalP->sizeX, focalP->sizeZ) > cell * 1.15f);
                    const float maxVisualHeight = displayScale ? std::min(3.2f, CEIL - 1.8f) : (CEIL - 0.45f);
                    const float fs = safeQuatUniformScale(*focalP, yaw, 1.0f, maxVisualHeight);
                    const float visualH = focalP->sizeY * fs;
                    const float yOff = displayScale ? std::min(1.18f, std::max(0.0f, CEIL - visualH - 0.45f)) : 0.0f;
                    if (displayScale) {
                        const float plinth = std::clamp(std::max(focalP->sizeX, focalP->sizeZ) * fs * 0.34f, 0.85f, 1.35f);
                        placeBoxMat(deckMassMat, cx - plinth, 0.00f, cz - plinth, cx + plinth, 0.12f, cz + plinth);
                        placeBoxMat(trimMat,     cx - plinth, 0.12f, cz - plinth, cx + plinth, 0.17f, cz + plinth);
                        raisedFocal = true;
                    }
                    placeScaledQuat(focalP, cx, yOff, cz, yaw, fs, fs, fs);
                    const float r = std::clamp(0.5f * std::max(focalP->sizeX, focalP->sizeZ) * fs + 0.10f, 0.70f, 1.75f);
                    stampSolidRect(cx - r, cz - r, cx + r, cz + r,
                                   std::max(displayScale ? 1.45f : 1.20f, yOff + visualH));
                } else if (!T.focal.empty()) {
                    logWarn("rooms: '%s' declares missing focal '%s'; X cell left empty",
                            T.name.c_str(), T.focal.c_str());
                }
                // Compose a small lit island around the focal instead of leaving a lone prop in the
                // grid cell. This uses non-colliding kit pieces so combat lanes remain clean.
                if (focalLightP) {
                    const float off[4][2] = { { 1.55f, 0.0f }, { -1.55f, 0.0f }, { 0.0f, 1.55f }, { 0.0f, -1.55f } };
                    const float lightY = raisedFocal ? 0.08f : 0.03f;
                    for (const auto& o : off)
                        placeQuat(draws_, *focalLightP, cx + o[0], lightY, cz + o[1], (std::fabs(o[0]) > 0.1f) ? kHalfPi : 0.0f);
                }
                // RELIQUARY: a cold-blue light-shaft panel set into the ceiling over the relic - the
                // monumental shaft the game lights a god-ray pool under (bible: "light shafts").
                if (!openTop_ && activeBiome_ == Biome::Ruins)
                    placeBoxMat(trimMat, cx - 1.5f, CEIL - 0.34f, cz - 1.5f, cx + 1.5f, CEIL - 0.22f, cz + 1.5f);
            } else if (c == 'p') {                           // authored dressing prop; chunky grounded pieces collide
                const uint32_t ph = mix(static_cast<uint32_t>((i + 13) * 353u + (j + 7) * 719u),
                                        static_cast<uint32_t>(roomIndex * 167 + static_cast<int>(activeBiome_) * 43));
                if (meshyAnchorsPlaced < maxMeshyAnchors && (ph % 100u) < 70u) {
                    if (const KitProp* mp = pickMeshyToken(meshyProps.anchors, ph, "round_1_", "round_3_")) {
                        const float yaw = (ph & 1u) ? kHalfPi : 0.0f;
                        const float y = (activeBiome_ == Biome::Ruins) ? 0.04f : 0.02f;
                        float px = cx, pz = cz;
                        const float s = safeKitScale(*mp, yaw, 0.88f);
                        clampKitToCell(*mp, i, j, yaw, s, px, pz);
                        placeKitProp(draws_, *mp, px, y, pz, yaw, s);
                        stampKitPropCollision(*mp, px, y, pz, yaw, s, 0.72f);
                        ++meshyAnchorsPlaced;
                        continue;
                    }
                }
                if (const QuatPiece* dp = pickDressPiece(rng.next())) {
                    // Deliberate, not random: face the prop toward its nearest wall (the same
                    // edge-weighted intent as the tiered scatter passes) so authored 'p' dressing
                    // reads as placed against structure rather than spun at random in the cell.
                    (void)rng.next();   // keep the rng stream advancing as before
                    int dpx = 0, dpz = 0, dpw = 0;
                    for (int s = 0; s < 4; ++s)
                        if (!isFloor(chr(i + dxs[s], j + dzs[s]))) { dpx += dxs[s]; dpz += dzs[s]; ++dpw; }
                    const float dyaw = (dpw > 0) ? std::atan2(static_cast<float>(dpx), static_cast<float>(dpz))
                                                 : (((i + j) & 1) ? kHalfPi : 0.0f);
                    float px = cx, pz = cz;
                    const float qs = safeQuatUniformScale(*dp, dyaw, 1.0f, CEIL - 0.55f);
                    clampQuatToCell(*dp, i, j, dyaw, qs, px, pz);
                    placeScaledQuat(dp, px, 0.0f, pz, dyaw, qs, qs, qs);
                    stampQuatPropCollision(*dp, px, 0.0f, pz, dyaw, qs, qs, qs, 0.72f);
                }
            } else if (c == 'H') {                           // raised platform deck (stand-on highground; daisFloor override)
                placeBoxMat(platformSideMat, cx - 1.86f, 0.0f, cz - 1.86f, cx + 1.86f, kDeckHeight - 0.10f, cz + 1.86f);
                placeBoxMat(ceilMat,   cx - 1.88f, kDeckHeight - 0.12f, cz - 1.88f, cx + 1.88f, kDeckHeight + 0.02f, cz + 1.88f);
                placeDeckSkirt(i, j, cx, cz);
                placeQuat(draws_, *daisP, cx, kDeckHeight, cz, 0.0f);                 // deck top surface
                // A light rail on exposed deck edges gives the H cells a platform language without
                // closing the ramp approach or cluttering adjacent H cells.
                const QuatPiece* railP = rail4P ? rail4P : rail2P;
                if (railP) {
                    for (int s = 0; s < 4; ++s) {
                        const char nb = chr(i + dxs[s], j + dzs[s]);
                        if (nb == 'H' || nb == '/') continue;
                        float rx = cx, rz = cz, yaw = 0.0f;
                        if (s == 0)      { rz = cz - 1.82f; yaw = kHalfPi; }
                        else if (s == 1) { rz = cz + 1.82f; yaw = kHalfPi; }
                        else if (s == 2) { rx = cx - 1.82f; yaw = 0.0f; }
                        else             { rx = cx + 1.82f; yaw = 0.0f; }
                        placeQuat(draws_, *railP, rx, kDeckHeight, rz, yaw);
                    }
                }
                if (!meshyCommon_.deckSupport.parts.empty() && meshyDeckFinishPlaced < 2) {
                    for (int s = 0; s < 4; ++s) {
                        const char nb = chr(i + dxs[s], j + dzs[s]);
                        if (nb == 'H' || nb == '/') continue;
                        float rx = cx, rz = cz, yaw = 0.0f;
                        if (s == 0)      { rz = cz - 1.72f; yaw = kHalfPi; }
                        else if (s == 1) { rz = cz + 1.72f; yaw = kHalfPi; }
                        else if (s == 2) { rx = cx - 1.72f; yaw = 0.0f; }
                        else             { rx = cx + 1.72f; yaw = 0.0f; }
                        placeKitProp(draws_, meshyCommon_.deckSupport, rx, 0.04f, rz, yaw, 0.62f);
                        ++meshyDeckFinishPlaced;
                        break;
                    }
                }
                stampSolidRect(cx - 2.0f, cz - 2.0f, cx + 2.0f, cz + 2.0f, kDeckHeight);
            } else if (c == '/') {                           // ramp up toward an adjacent raised 'H' deck
                // hd = the side the deck is on (0=N/-z, 1=S/+z, 2=W/-x, 3=E/+x).
                const auto rampApproachOpen = [](char rc) {
                    return rc == '.' || rc == 'c' || rc == '=' || rc == 'p' ||
                           rc == '/' || rc == 'D' || rc == 'E';
                };
                int hd = -1, fallbackHd = -1;
                for (int s = 0; s < 4; ++s) {
                    if (chr(i + dxs[s], j + dzs[s]) != 'H') continue;
                    if (fallbackHd < 0) fallbackHd = s;
                    if (rampApproachOpen(chr(i - dxs[s], j - dzs[s]))) { hd = s; break; }
                }
                if (hd < 0) hd = (fallbackHd >= 0) ? fallbackHd : 3;
                // The kit ramp mesh is re-centred with its floor at Y=0 and rises along native -Z
                // (high end at z=0, low end at +Z).
                // Rotate so that rise points TOWARD the deck, and SCALE it to fill the whole 4 m cell and
                // climb the full deck height - so the visible ramp matches the collision slope below. (It
                // was placed native: a 2 m x 1 m piece sitting in a 4 m cell against a 2 m deck, mis-rotated
                // - so it read as a half-size, wrong-way prop and the deck behind it felt like an
                // unclimbable cube.) yaw maps mesh -Z onto the deck dir.
                const float yaw = (hd == 0) ? 0.0f : (hd == 1) ? kPiF : (hd == 2) ? kHalfPi : (hd == 3) ? -kHalfPi : 0.0f;
                if (rampP && rampP->sizeX > 0.01f && rampP->sizeY > 0.01f && rampP->sizeZ > 0.01f) {
                    const Mat4 rxf = mul(scaling(4.0f / rampP->sizeX, kDeckHeight / rampP->sizeY, 4.0f / rampP->sizeZ),
                                         mul(rotationY(yaw), translation(cx, 0.0f, cz)));
                    for (const DungeonDraw& part : rampP->parts) draws_.push_back({ part.mesh, part.material, rxf });
                }
                const KitProp* stairFinisher = pickMeshy(meshyCommon_.stairFinishers,
                                                         mix(static_cast<uint32_t>((i + 23) * 331u + (j + 19) * 557u),
                                                             static_cast<uint32_t>(roomIndex * 71 + hd * 13)));
                if (!stairFinisher && !meshyCommon_.stairFinisher.parts.empty()) stairFinisher = &meshyCommon_.stairFinisher;
                if (stairFinisher && !stairFinisher->parts.empty() && meshyDeckFinishPlaced < 3) {
                    placeKitProp(draws_, *stairFinisher, cx, 0.035f, cz, yaw, 0.60f);
                    ++meshyDeckFinishPlaced;
                }
                // WALKABLE collision slope: the top rises from ~0 at the open edge to the deck height at
                // the 'H' edge, so the player walks up onto the deck. ascend points toward the deck and
                // matches the visual rise above.
                const int ascend = (hd == 0) ? 3 : (hd == 1) ? 2 : (hd == 2) ? 1 : (hd == 3) ? 0 : 3;
                stampRamp(cx - 2.0f, cz - 2.0f, cx + 2.0f, cz + 2.0f, 0.0f, kDeckHeight, ascend);
            }
        }

    // pass 3b: immediate biome read near the entrance. These are non-colliding, tucked just off
    // the mouth so the player's first-person start sees authored machinery/relic dressing without
    // blocking the spawn pocket or door trigger.
    if (!meshyProps.anchors.empty() && meshyAnchorsPlaced < maxMeshyAnchors) {
        for (const DoorOpening& od : semanticDoors) {
            if (!od.entrance) continue;
            float ix = 0.0f, iz = 0.0f;
            if (od.side == 0) iz = 1.0f;
            else if (od.side == 1) iz = -1.0f;
            else if (od.side == 2) ix = 1.0f;
            else ix = -1.0f;
            const float tx = -iz, tz = ix;
            const uint32_t eh = mix(static_cast<uint32_t>(roomIndex * 127 + static_cast<int>(activeBiome_) * 619),
                                    static_cast<uint32_t>((od.i + 5) * 977u + (od.j + 3) * 131u));
            const float side = (eh & 1u) ? 1.0f : -1.0f;
            const float ax = std::clamp(od.x + ix * 2.30f + tx * side * 1.70f, ox0 + 0.72f, ox1 - 0.72f);
            const float az = std::clamp(od.z + iz * 2.30f + tz * side * 1.70f, oz0 + 0.72f, oz1 - 0.72f);
            const float yaw = std::atan2(-ix, -iz);
            const KitProp* mp = pickMeshyToken(meshyProps.anchors, eh, "round_3_", "round_1_");
            if (mp) {
                float px = ax, pz = az;
                const float s = safeKitScale(*mp, yaw, (activeBiome_ == Biome::Ruins) ? 0.58f : 0.54f);
                clampKitToCell(*mp, od.i, od.j, yaw, s, px, pz);
                placeKitProp(draws_, *mp, px, 0.02f, pz, yaw, s);
                ++meshyAnchorsPlaced;
            }
            break;
        }
    }

    // pass 4: entry/exit floor strips only. Decorative decal meshes are mounted in the wall pass.
    for (int j = 0; j < CD; ++j)
        for (int i = 0; i < CW; ++i) {
            const char c = chr(i, j);
            const float cx = cwx(i), cz = cwz(j);
            const CellInfo* ci = cinfo(i, j);
            if (c == 'D' || c == 'E') {
                const int ds = (ci && ci->doorSide >= 0) ? ci->doorSide : doorSideFor(i, j);
                const MaterialHandle poolMat = (c == 'E') ? quatEntryMat_ : ((c == 'D') ? quatExitMat_ : MaterialHandle{});
                if (poolMat != MaterialHandle{}) {
                    if (ds == 2 || ds == 3)
                        placeBoxMat(poolMat, cx - 0.10f, 0.056f, cz - 0.78f, cx + 0.10f, 0.064f, cz + 0.78f);
                    else
                        placeBoxMat(poolMat, cx - 0.78f, 0.056f, cz - 0.10f, cx + 0.78f, 0.064f, cz + 0.10f);
                }
                continue;
            }
        }

    const auto nearDoorOpening = [&](const CellInfo& ci, float distSq) {
        for (const DoorOpening& od : semanticDoors) {
            const float dx = ci.x - od.x, dz = ci.z - od.z;
            if (dx * dx + dz * dz < distSq) return true;
        }
        return false;
    };

    // pass 4b: base asset dressing. These are the original Meshy base-kit references promoted into
    // the runtime pack; place them deliberately as structural/readability accents, not as random clutter.
    if (!meshyProps.baseWallDetails.empty() || !meshyProps.baseAnchors.empty() || !meshyProps.baseFloorDetails.empty()) {
        const int maxBaseWalls = (T.size == AreaSize::Big) ? 4 : (T.size == AreaSize::Mid ? 3 : 2);
        const int maxBaseAnchors = (T.size == AreaSize::Big) ? 2 : 1;
        const int maxBaseFloors = (T.size == AreaSize::Big) ? 4 : (T.size == AreaSize::Mid ? 3 : 1);
        int baseWalls = 0, baseAnchors = 0, baseFloors = 0;
        for (int j = 0; j < CD; ++j)
            for (int i = 0; i < CW; ++i) {
                const CellInfo* ci = cinfo(i, j);
                if (!ci || ci->c != '.') continue;
                if (ci->doorZone || ci->focalZone) continue;
                if (nearDoorOpening(*ci, 12.0f)) continue;

                int dirX = 0, dirZ = 0, walls = 0;
                for (int s = 0; s < 4; ++s)
                    if (!isFloor(chr(i + dxs[s], j + dzs[s]))) { dirX += dxs[s]; dirZ += dzs[s]; ++walls; }
                const float norm = std::max(1.0f, static_cast<float>(std::abs(dirX) + std::abs(dirZ)));
                const float yawToWall = (walls > 0)
                                      ? std::atan2(static_cast<float>(dirX), static_cast<float>(dirZ))
                                      : 0.0f;
                const uint32_t h = mix(static_cast<uint32_t>((i + 67) * 419u + (j + 71) * 613u),
                                       static_cast<uint32_t>(roomIndex * 1237 + static_cast<int>(activeBiome_) * 149));

                if (ci->cornerPocket && walls > 0 && baseAnchors < maxBaseAnchors &&
                    meshyAnchorsPlaced < maxMeshyAnchors && !meshyProps.baseAnchors.empty() &&
                    (h % 100u) < (activeBiome_ == Biome::Ruins ? 36u : 58u)) {
                    if (const KitProp* bp = pickMeshy(meshyProps.baseAnchors, h >> 2)) {
                        float px = std::clamp(ci->x + (static_cast<float>(dirX) / norm) * 1.06f, ox0 + 0.52f, ox1 - 0.52f);
                        float pz = std::clamp(ci->z + (static_cast<float>(dirZ) / norm) * 1.06f, oz0 + 0.52f, oz1 - 0.52f);
                        const float s = safeKitScale(*bp, yawToWall, activeBiome_ == Biome::Ruins ? 0.56f : 0.62f);
                        clampKitToCell(*bp, i, j, yawToWall, s, px, pz);
                        placeKitProp(draws_, *bp, px, 0.028f, pz, yawToWall, s);
                        stampKitPropCollision(*bp, px, 0.028f, pz, yawToWall, s, 0.62f);
                        ++baseAnchors;
                        ++meshyAnchorsPlaced;
                        continue;
                    }
                }

                if ((ci->edgePocket || ci->cornerPocket) && walls > 0 && baseWalls < maxBaseWalls &&
                    meshyWallFinishPlaced < maxMeshyWallFinish && !meshyProps.baseWallDetails.empty() &&
                    (h % 100u) < (activeBiome_ == Biome::Ruins ? 42u : 64u)) {
                    if (const KitProp* bp = pickMeshy(meshyProps.baseWallDetails, h >> 4)) {
                        float px = std::clamp(ci->x + (static_cast<float>(dirX) / norm) * (ci->cornerPocket ? 1.20f : 1.34f), ox0 + 0.46f, ox1 - 0.46f);
                        float pz = std::clamp(ci->z + (static_cast<float>(dirZ) / norm) * (ci->cornerPocket ? 1.20f : 1.34f), oz0 + 0.46f, oz1 - 0.46f);
                        const float s = safeKitScale(*bp, yawToWall, ci->cornerPocket ? 0.58f : 0.52f);
                        clampKitToCell(*bp, i, j, yawToWall, s, px, pz);
                        placeKitProp(draws_, *bp, px, 0.044f, pz, yawToWall, s);
                        ++baseWalls;
                        ++meshyWallFinishPlaced;
                        continue;
                    }
                }

                const bool laneX = isFloor(chr(i - 1, j)) && isFloor(chr(i + 1, j));
                const bool laneZ = isFloor(chr(i, j - 1)) && isFloor(chr(i, j + 1));
                const bool centralBand = std::abs(i - CW / 2) <= 1 || std::abs(j - CD / 2) <= 1;
                if (!ci->edgePocket && !ci->cornerPocket && (ci->lane || centralBand || laneX || laneZ) &&
                    baseFloors < maxBaseFloors && meshyFloorDetailsPlaced < maxMeshyFloorDetails &&
                    !meshyProps.baseFloorDetails.empty() && (h % 100u) < (activeBiome_ == Biome::Ruins ? 34u : 52u)) {
                    if (const KitProp* bp = pickMeshy(meshyProps.baseFloorDetails, h >> 6)) {
                        const float yaw = laneX && !laneZ ? kHalfPi : ((h & 1u) ? kHalfPi : 0.0f);
                        float px = ci->x, pz = ci->z;
                        const float s = safeKitScale(*bp, yaw, activeBiome_ == Biome::Ruins ? 0.46f : 0.50f);
                        clampKitToCell(*bp, i, j, yaw, s, px, pz);
                        placeKitProp(draws_, *bp, px, 0.060f, pz, yaw, s);
                        ++baseFloors;
                        ++meshyFloorDetailsPlaced;
                    }
                }
            }
    }

    // pass 5: T1 anchors + T2 edge details from the room/style DRESSING_POOL. Props hug wall and
    // corner pockets, never door mouths or focal clear-zones. Chunky grounded props get small
    // collision footprints; wall plates, floor overlays, cables, and trims stay visual.
    if (!templateDressPool.empty() || !dressPool.empty()) {
        // Density per the bible: Furnace "dense with solid mass", Foundry medium, Reliquary sparse.
        const float density = (activeBiome_ == Biome::Forest) ? 0.84f
                            : (activeBiome_ == Biome::Ruins)  ? 0.20f
                            :                                    0.52f;
        for (int j = 0; j < CD; ++j)
            for (int i = 0; i < CW; ++i) {
                const CellInfo* ci = cinfo(i, j);
                if (!ci || ci->c != '.') continue;              // only plain floor (skip authored content)
                if (ci->doorZone || ci->focalZone) continue;
                if (!ci->edgePocket && !ci->cornerPocket) continue;
                int dirX = 0, dirZ = 0, walls = 0;
                for (int s = 0; s < 4; ++s)
                    if (!isFloor(chr(i + dxs[s], j + dzs[s]))) { dirX += dxs[s]; dirZ += dzs[s]; ++walls; }
                if (walls == 0) continue;                       // interior lane cell -> keep clear
                if (nearDoorOpening(*ci, 9.0f)) continue;        // keep spawn + door mouths clear
                const uint32_t h = mix(static_cast<uint32_t>((i + 19) * 131u + (j + 23) * 733u),
                                       static_cast<uint32_t>(roomIndex * 97 + static_cast<int>(activeBiome_) * 271));
                const float weight = ci->cornerPocket ? 0.68f : 0.48f;
                if ((h % 1000u) >= static_cast<uint32_t>(density * weight * 1000.0f)) continue;
                const float norm = std::max(1.0f, static_cast<float>(std::abs(dirX) + std::abs(dirZ)));
                const float px = std::clamp(ci->x + (static_cast<float>(dirX) / norm) * (ci->cornerPocket ? 1.20f : 1.34f), ox0 + 0.45f, ox1 - 0.45f);
                const float pz = std::clamp(ci->z + (static_cast<float>(dirZ) / norm) * (ci->cornerPocket ? 1.20f : 1.34f), oz0 + 0.45f, oz1 - 0.45f);
                const float yaw = (std::abs(dirX) + std::abs(dirZ) > 0)
                                ? std::atan2(static_cast<float>(dirX), static_cast<float>(dirZ))
                                : rng.frange(0.0f, 6.2831f);
                if (meshyWallFinishPlaced < maxMeshyWallFinish && (h % 100u) < 34u) {
                    const KitProp* wp = pickMeshyToken(meshyProps.wallDetails, h >> 4, "round_1_", "round_3_");
                    if (!wp || wp->parts.empty()) {
                        if ((h & 1u) && !meshyCommon_.wallAlcove.parts.empty()) wp = &meshyCommon_.wallAlcove;
                        else if (!meshyCommon_.baseTrim.parts.empty()) wp = &meshyCommon_.baseTrim;
                    }
                    if (wp && !wp->parts.empty()) {
                        float sx = px, sz = pz;
                        const float s = safeKitScale(*wp, yaw, ci->cornerPocket ? 0.62f : 0.54f);
                        clampKitToCell(*wp, i, j, yaw, s, sx, sz);
                        placeKitProp(draws_, *wp, sx, 0.035f, sz, yaw, s);
                        ++meshyWallFinishPlaced;
                        continue;
                    }
                }
                const bool meshyPocket = ci->cornerPocket || (ci->edgePocket && activeBiome_ != Biome::Ruins);
                const uint32_t meshyChance = ci->cornerPocket ? 52u : 44u;
                if (meshyAnchorsPlaced < maxMeshyAnchors && meshyPocket && (h % 100u) < meshyChance) {
                    if (const KitProp* mp = pickMeshyToken(meshyProps.anchors, h >> 3, "round_1_", "round_3_")) {
                        float sx = px, sz = pz;
                        const float s = safeKitScale(*mp, yaw, (activeBiome_ == Biome::Ruins) ? 0.82f : 0.84f);
                        clampKitToCell(*mp, i, j, yaw, s, sx, sz);
                        placeKitProp(draws_, *mp, sx, 0.02f, sz, yaw, s);
                        stampKitPropCollision(*mp, sx, 0.02f, sz, yaw, s, 0.66f);
                        ++meshyAnchorsPlaced;
                        continue;
                    }
                }
                if (const QuatPiece* dp = pickDressPiece(h >> 5)) {
                    float sx = px, sz = pz;
                    const float qs = safeQuatUniformScale(*dp, yaw, 1.0f, CEIL - 0.55f);
                    clampQuatToCell(*dp, i, j, yaw, qs, sx, sz);
                    placeScaledQuat(dp, sx, 0.0f, sz, yaw, qs, qs, qs);
                    stampQuatPropCollision(*dp, sx, 0.0f, sz, yaw, qs, qs, qs, 0.62f);
                }
            }
    }

    // pass 5b: Round 1/3 Meshy reinforcement. The new generated pack is broad enough that the old
    // conservative caps made many assets technically loaded but rarely visible. This pass spends a
    // small, deterministic budget in already-safe cells: wall plates in edge pockets, machinery in
    // corner pockets, and non-decal floor hardware in open lane cells. Machinery anchors collide;
    // wall plates and floor overlays stay visual.
    if (!meshyProps.wallDetails.empty() || !meshyProps.anchors.empty() || !meshyProps.floorDetails.empty()) {
        const int maxNewWalls = (T.size == AreaSize::Big) ? 5 : (T.size == AreaSize::Mid ? 3 : 2);
        const int maxNewAnchors = (T.size == AreaSize::Big) ? 3 : (T.size == AreaSize::Mid ? 2 : 1);
        const int maxNewFloors = (T.size == AreaSize::Big) ? 5 : (T.size == AreaSize::Mid ? 3 : 2);
        int newWalls = 0, newAnchors = 0, newFloors = 0;
        for (int j = 0; j < CD; ++j)
            for (int i = 0; i < CW; ++i) {
                const CellInfo* ci = cinfo(i, j);
                if (!ci || ci->c != '.') continue;
                if (ci->doorZone || ci->focalZone) continue;
                if (nearDoorOpening(*ci, 14.0f)) continue;

                int dirX = 0, dirZ = 0, walls = 0;
                for (int s = 0; s < 4; ++s)
                    if (!isFloor(chr(i + dxs[s], j + dzs[s]))) { dirX += dxs[s]; dirZ += dzs[s]; ++walls; }
                const float norm = std::max(1.0f, static_cast<float>(std::abs(dirX) + std::abs(dirZ)));
                const float yawToWall = (walls > 0)
                                      ? std::atan2(static_cast<float>(dirX), static_cast<float>(dirZ))
                                      : 0.0f;
                const uint32_t h = mix(static_cast<uint32_t>((i + 43) * 601u + (j + 59) * 283u),
                                       static_cast<uint32_t>(roomIndex * 587 + static_cast<int>(activeBiome_) * 997));

                if (ci->cornerPocket && newAnchors < maxNewAnchors && meshyAnchorsPlaced < maxMeshyAnchors &&
                    (h % 100u) < (activeBiome_ == Biome::Ruins ? 32u : 48u)) {
                    if (const KitProp* mp = pickMeshyToken(meshyProps.anchors, h >> 3, "round_3_", "round_1_")) {
                        float px = std::clamp(ci->x + (static_cast<float>(dirX) / norm) * 1.05f, ox0 + 0.56f, ox1 - 0.56f);
                        float pz = std::clamp(ci->z + (static_cast<float>(dirZ) / norm) * 1.05f, oz0 + 0.56f, oz1 - 0.56f);
                        const float s = safeKitScale(*mp, yawToWall, (activeBiome_ == Biome::Ruins) ? 0.58f : 0.62f);
                        clampKitToCell(*mp, i, j, yawToWall, s, px, pz);
                        placeKitProp(draws_, *mp, px, 0.025f, pz, yawToWall, s);
                        stampKitPropCollision(*mp, px, 0.025f, pz, yawToWall, s, 0.60f);
                        ++newAnchors;
                        ++meshyAnchorsPlaced;
                        continue;
                    }
                }

                if ((ci->edgePocket || ci->cornerPocket) && walls > 0 &&
                    newWalls < maxNewWalls && meshyWallFinishPlaced < maxMeshyWallFinish) {
                    if (const KitProp* wp = pickMeshyToken(meshyProps.wallDetails, h >> 5, "round_1_", "round_3_")) {
                        float px = std::clamp(ci->x + (static_cast<float>(dirX) / norm) * (ci->cornerPocket ? 1.24f : 1.38f), ox0 + 0.48f, ox1 - 0.48f);
                        float pz = std::clamp(ci->z + (static_cast<float>(dirZ) / norm) * (ci->cornerPocket ? 1.24f : 1.38f), oz0 + 0.48f, oz1 - 0.48f);
                        const float s = safeKitScale(*wp, yawToWall, ci->cornerPocket ? 0.62f : 0.56f);
                        clampKitToCell(*wp, i, j, yawToWall, s, px, pz);
                        placeKitProp(draws_, *wp, px, 0.042f, pz, yawToWall, s);
                        ++newWalls;
                        ++meshyWallFinishPlaced;
                        continue;
                    }
                }

                const bool centralBand = std::abs(i - CW / 2) <= 1 || std::abs(j - CD / 2) <= 1;
                if (!ci->edgePocket && !ci->cornerPocket && (ci->lane || centralBand) &&
                    newFloors < maxNewFloors && meshyFloorDetailsPlaced < maxMeshyFloorDetails) {
                    if (const KitProp* fp = pickMeshyToken(meshyProps.floorDetails, h >> 7, "round_1_", "round_3_")) {
                        const bool laneX = isFloor(chr(i - 1, j)) && isFloor(chr(i + 1, j));
                        const bool laneZ = isFloor(chr(i, j - 1)) && isFloor(chr(i, j + 1));
                        const float yaw = laneX && !laneZ ? kHalfPi : ((h & 1u) ? kHalfPi : 0.0f);
                        float px = ci->x, pz = ci->z;
                        const float s = safeKitScale(*fp, yaw, (activeBiome_ == Biome::Ruins) ? 0.42f : 0.48f);
                        clampKitToCell(*fp, i, j, yaw, s, px, pz);
                        placeKitProp(draws_, *fp, px, 0.064f, pz, yaw, s);
                        ++newFloors;
                        ++meshyFloorDetailsPlaced;
                    }
                }
            }
    }

    // pass 6: midfield dressing for broad rooms. Pass 5 weights the walls/corners; this pass fixes
    // the opposite failure mode: large plain floor fields that read like unfinished blockout. Floor
    // overlays and cables remain walk-through; solid-looking machine props get conservative
    // collision, so players cannot pass through chunky objects.
    const bool broadRoom = T.size == AreaSize::Big || (T.size == AreaSize::Mid && CW * CD >= 28);
    if (broadRoom && (midfieldFloorP || midfieldFloorAltP || midfieldCableA || midfieldCableB || midfieldMachineP)) {
        const auto nearAuthoredCell = [&](int i, int j, int radius) {
            for (int dz = -radius; dz <= radius; ++dz)
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx == 0 && dz == 0) continue;
                    const char nc = chr(i + dx, j + dz);
                    if (nc != '.' && nc != ' ' && nc != 'D' && nc != 'E') return true;
                }
            return false;
        };
        int interiorPlain = 0;
        for (int j = 0; j < CD; ++j)
            for (int i = 0; i < CW; ++i) {
                const CellInfo* ci = cinfo(i, j);
                if (!ci || ci->c != '.' || ci->doorZone || ci->focalZone) continue;
                if (nearDoorOpening(*ci, 14.0f)) continue;
                ++interiorPlain;
            }
        const float biomeDensity = (activeBiome_ == Biome::Forest) ? 0.74f
                                : (activeBiome_ == Biome::Ruins)  ? 0.36f
                                :                                    0.58f;
        const int maxIslands = (T.size == AreaSize::Big)
                             ? std::clamp(interiorPlain / 2, 4, 12)
                             : std::clamp(interiorPlain / 3, 2, 6);
        int islands = 0;
        for (int j = 0; j < CD && islands < maxIslands; ++j)
            for (int i = 0; i < CW && islands < maxIslands; ++i) {
                const CellInfo* ci = cinfo(i, j);
                if (!ci || ci->c != '.') continue;
                if (ci->doorZone || ci->focalZone) continue;
                if (nearDoorOpening(*ci, 14.0f)) continue;
                const bool nearAnchor = nearAuthoredCell(i, j, 1);
                const bool centralBand = (std::abs(i - CW / 2) <= 1) || (std::abs(j - CD / 2) <= 1);
                const uint32_t h = mix(static_cast<uint32_t>((i + 31) * 409u + (j + 37) * 149u),
                                       static_cast<uint32_t>(roomIndex * 313 + static_cast<int>(activeBiome_) * 811));
                float weight = biomeDensity;
                if (nearAnchor) weight *= 0.55f;
                if (ci->edgePocket || ci->cornerPocket) weight *= (activeBiome_ == Biome::Ruins) ? 0.78f : 0.46f;
                if (centralBand) weight *= 1.18f;
                if ((h % 1000u) >= static_cast<uint32_t>(std::min(0.92f, weight) * 1000.0f)) continue;

                const bool laneX = isFloor(chr(i - 1, j)) && isFloor(chr(i + 1, j));
                const bool laneZ = isFloor(chr(i, j - 1)) && isFloor(chr(i, j + 1));
                const float yaw = laneX && !laneZ ? kHalfPi : ((h & 1u) ? kHalfPi : 0.0f);
                const float patchScale = (activeBiome_ == Biome::Ruins) ? 0.58f
                                       : (activeBiome_ == Biome::Forest) ? 0.74f
                                       :                                    0.66f;
                const QuatPiece* patch = ((h & 4u) && midfieldFloorAltP) ? midfieldFloorAltP : midfieldFloorP;
                if (patch) {
                    float px = ci->x, pz = ci->z;
                    const float s = safeQuatScale(*patch, yaw, patchScale);
                    clampQuatToCell(*patch, i, j, yaw, s, px, pz);
                    placeScaledQuat(patch, px, 0.034f, pz, yaw, s, 1.0f, s);
                }
                if (meshyFloorDetailsPlaced < maxMeshyFloorDetails && (h % 100u) < 30u) {
                    const KitProp* fp = pickMeshyToken(meshyProps.floorDetails, h >> 6, "round_1_", "round_3_");
                    if ((!fp || fp->parts.empty()) && !meshyCommon_.floorHatch.parts.empty()) fp = &meshyCommon_.floorHatch;
                    if (fp && !fp->parts.empty()) {
                        float px = ci->x, pz = ci->z;
                        const float fs = safeKitScale(*fp, yaw, (activeBiome_ == Biome::Ruins) ? 0.58f : 0.52f);
                        clampKitToCell(*fp, i, j, yaw, fs, px, pz);
                        placeKitProp(draws_, *fp, px, 0.058f, pz, yaw, fs);
                        ++meshyFloorDetailsPlaced;
                    }
                }

                const float off = ((h & 8u) ? 0.72f : -0.72f);
                const float sideX = laneZ && !laneX ? off : 0.0f;
                const float sideZ = laneX && !laneZ ? off : 0.0f;
                const QuatPiece* cable = ((h & 16u) && midfieldCableB) ? midfieldCableB : midfieldCableA;
                if (cable && (activeBiome_ != Biome::Ruins || (h % 3u) != 0u)) {
                    float px = ci->x + sideX, pz = ci->z + sideZ;
                    const float s = safeQuatScale(*cable, yaw, 0.92f);
                    clampQuatToCell(*cable, i, j, yaw, s, px, pz);
                    placeScaledQuat(cable, px, 0.072f, pz, yaw, s, 1.0f, s);
                }
                if (midfieldMachineP && ((h >> 5) % ((activeBiome_ == Biome::Forest) ? 2u : 3u)) == 0u) {
                    const float mx = ci->x - sideX * 0.55f;
                    const float mz = ci->z - sideZ * 0.55f;
                    float px = mx, pz = mz;
                    const float myaw = yaw + kHalfPi;
                    const float s = safeQuatScale(*midfieldMachineP, myaw, 0.72f);
                    clampQuatToCell(*midfieldMachineP, i, j, myaw, s, px, pz);
                    placeScaledQuat(midfieldMachineP, px, 0.064f, pz, myaw, s, 1.0f, s);
                    stampQuatPropCollision(*midfieldMachineP, px, 0.064f, pz, myaw, s, 1.0f, s, 0.48f);
                }
                if (!openTop_ && trimMat != MaterialHandle{} && ((h >> 9) % 3u) == 0u) {
                    if (laneX)
                        placeBoxMat(trimMat, ci->x - 1.55f, CEIL - 0.58f, ci->z - 0.08f, ci->x + 1.55f, CEIL - 0.48f, ci->z + 0.08f);
                    else
                        placeBoxMat(trimMat, ci->x - 0.08f, CEIL - 0.58f, ci->z - 1.55f, ci->x + 0.08f, CEIL - 0.48f, ci->z + 1.55f);
                }
                ++islands;
            }
    }

    // spawn just inside the entrance [0]; clear a pocket so we never spawn inside cover
    spawnX_ = doors_.empty() ? 16 : doors_[0].spawnX;
    spawnZ_ = doors_.empty() ? 12 : doors_[0].spawnZ;
    for (int fz = (spawnZ_ - 1) * kSub; fz < (spawnZ_ + 2) * kSub; ++fz)
        for (int fx = (spawnX_ - 1) * kSub; fx < (spawnX_ + 2) * kSub; ++fx)
            if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) { fine_[static_cast<size_t>(fz) * FW + fx] = 0; fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f; }

    for (int z = 0; z < 24; ++z)
        for (int x = 0; x < 32; ++x) {
            const int fx = x * kSub + kSub / 2, fz = z * kSub + kSub / 2;
            grid_[static_cast<size_t>(z)][static_cast<size_t>(x)] = fine_[static_cast<size_t>(fz) * FW + fx] ? '#' : '.';
        }
    if (spawnX_ >= 0 && spawnX_ < 32 && spawnZ_ >= 0 && spawnZ_ < 24)
        grid_[static_cast<size_t>(spawnZ_)][static_cast<size_t>(spawnX_)] = 'P';
    sealDoors();
}

void Wasteland::generate(Biome biome, uint64_t seed, int roomIndex) {
    pendingBiome_ = biome;   // M4: honor the biome on the brutalist path (was dropped before)
    if (brutalist_) {
        if (quatReady_) generateQuaternius(pendingSize_, seed, roomIndex);  // Quaternius is the cohesive shell
        else            generateBrutalist(pendingSize_, seed, roomIndex);
        return;
    }
    draws_.clear();
    for (auto& row : grid_) row.assign(32, '#');
    const int FW = 32 * kSub, FH = 24 * kSub;
    fine_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 1);
    fineHeight_.assign(static_cast<size_t>(FW) * static_cast<size_t>(FH), 99.0f); // boundary unscalable
    spawnX_ = 16; spawnZ_ = 12;
    if (!ready_) return;
    const bool forest = (biome == Biome::Forest);
    const bool ruins  = (biome == Biome::Ruins);

    Rng rng(seed ^ (0x9E3779B9ull * static_cast<uint64_t>(roomIndex + 1)) ^ (static_cast<uint64_t>(biome) << 40));

    const int margin = 2 + rng.range(0, 1);
    const int px0 = margin, px1 = 31 - margin, pz0 = margin, pz1 = 23 - margin;
    const float fx0 = static_cast<float>(px0), fx1 = static_cast<float>(px1 + 1);
    const float fz0 = static_cast<float>(pz0), fz1 = static_cast<float>(pz1 + 1);
    for (int fz = pz0 * kSub; fz < (pz1 + 1) * kSub; ++fz)
        for (int fx = px0 * kSub; fx < (px1 + 1) * kSub; ++fx) {
            fine_[static_cast<size_t>(fz) * FW + fx] = 0;
            fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
        }

    draws_.push_back({ groundMesh_, groundMat_[static_cast<size_t>(biome)], Mat4::identity() });
    const float cx = 0.5f * (fx0 + fx1), cz = 0.5f * (fz0 + fz1);

    // Mountain skyline ring (sunk so its base hides behind the rock berm; random yaw per room).
    if (!skyline_.parts.empty()) {
        const Mat4 xf = mul(rotationY(rng.frange(0.0f, 2.0f * kPi)), translation(cx, -2.5f, cz));
        for (const DungeonDraw& part : skyline_.parts) draws_.push_back({ part.mesh, part.material, xf });
    }

    // Scatter ground-hugging clutter (small rocks, + grass tufts in the forest) around a point so
    // big props sit IN the ground instead of pasted onto a bare plane, and the flat tiled ground
    // never reads empty. No collision - pure visual grounding.
    const auto groundClutter = [&](float gx, float gz, float spread, int count) {
        for (int k = 0; k < count; ++k) {
            const float a = rng.frange(0.0f, 2.0f * kPi), d = rng.frange(0.3f, spread);
            const float px = gx + std::cos(a) * d, pz = gz + std::sin(a) * d;
            if (px < fx0 + 0.2f || px > fx1 - 0.2f || pz < fz0 + 0.2f || pz > fz1 - 0.2f) continue;
            const bool g = forest && !grass_.empty() && (rng.range(0, 1) == 0);
            const Prop* r = pick(g ? grass_ : smallRocks_, rng.next());
            if (r) place(draws_, *r, px, pz, rng.frange(0.0f, 2.0f * kPi), rng.frange(0.8f, 1.5f));
        }
    };

    // Backdrop landmark fully outside the play rect (a silhouette on the skyline).
    if (!structures_.empty() && rng.range(0, 3) != 0) {
        const Prop* s = pick(structures_, mix(static_cast<uint32_t>(roomIndex + 1), 0xA53u));
        if (s) {
            const int side = rng.range(0, 3);
            const float off = std::max(s->halfX, s->halfZ) + 2.0f;
            float bx = cx, bz = cz;
            if (side == 0) bz = fz0 - off; else if (side == 1) bz = fz1 + off;
            else if (side == 2) bx = fx0 - off; else bx = fx1 + off;
            place(draws_, *s, bx, bz, rng.frange(0.0f, 2.0f * kPi), 1.0f);
        }
    }

    // Layered rock berm boundary (the play-rect edge is already solid, so a visual gap never lets
    // the player out). In the forest, trees crowd the berm for an enclosed clearing.
    {
        const auto edgeRock = [&](float ex, float ez, float inX, float inZ) {
            const Prop* r = pick(rocks_, rng.next());
            if (r) place(draws_, *r, ex + rng.frange(-0.6f, 0.6f) + inX * rng.frange(-0.3f, 0.6f),
                            ez + rng.frange(-0.6f, 0.6f) + inZ * rng.frange(-0.3f, 0.6f),
                            rng.frange(0.0f, 2.0f * kPi), rng.frange(1.3f, 2.2f));
            if (rng.range(0, 1) == 0) {
                const Prop* r2 = pick(rocks_, rng.next());
                if (r2) place(draws_, *r2, ex + inX * 0.9f + rng.frange(-0.4f, 0.4f),
                                ez + inZ * 0.9f + rng.frange(-0.4f, 0.4f),
                                rng.frange(0.0f, 2.0f * kPi), rng.frange(0.8f, 1.3f));
            }
            if (forest && !trees_.empty()) {                         // trees behind the berm
                const Prop* t = pick(trees_, rng.next());
                if (t) place(draws_, *t, ex + inX * -1.4f, ez + inZ * -1.4f, rng.frange(0.0f, 2.0f * kPi), rng.frange(0.8f, 1.3f));
            }
        };
        for (float x = fx0; x <= fx1; x += 2.4f + rng.frange(-0.4f, 0.7f)) { edgeRock(x, fz0, 0, 1); edgeRock(x, fz1, 0, -1); }
        for (float z = fz0 + 2.4f; z < fz1; z += 2.4f + rng.frange(-0.4f, 0.7f)) { edgeRock(fx0, z, 1, 0); edgeRock(fx1, z, -1, 0); }
    }

    // ---- DESIGNED LAYOUT: cover CLUSTERS at archetype anchor points ----
    // Instead of scattering cover at random (which reads as noise), each room picks a layout
    // archetype: a handful of anchor points in normalised arena space that keep the CENTRE open as
    // a combat/kiting zone and leave clear LANES between anchors. At each anchor we build a composed
    // cover ISLAND - one hero rock (or a ruins structure landmark) with a few smaller rocks/crates
    // nestled around its base - so the space reads as deliberately built, with mixed-height cover to
    // fight around, not a field of identical rocks.
    struct Placed { float x, z, r; };
    std::vector<Placed> taken; taken.push_back({ cx, cz, 4.6f });   // keep the centre + spawn open
    const float ix0 = fx0 + 2.0f, ix1 = fx1 - 2.0f, iz0 = fz0 + 2.0f, iz1 = fz1 - 2.0f;
    const float halfW = 0.5f * (ix1 - ix0), halfH = 0.5f * (iz1 - iz0);

    struct A2 { float x, z; };
    static const A2 quad[]     = { { -0.60f, -0.55f }, { 0.60f, 0.55f }, { 0.60f, -0.55f }, { -0.60f, 0.55f } };
    static const A2 cardinal[] = { { 0.0f, -0.80f }, { 0.0f, 0.80f }, { -0.84f, 0.0f }, { 0.84f, 0.0f } };
    static const A2 flanks[]   = { { -0.80f, -0.42f }, { -0.78f, 0.46f }, { 0.80f, 0.42f }, { 0.78f, -0.46f } };
    static const A2 diag[]     = { { -0.66f, -0.50f }, { -0.20f, -0.78f }, { 0.50f, 0.62f }, { 0.74f, 0.20f } };
    static const A2 landmark[] = { { 0.0f, -0.50f }, { -0.74f, 0.34f }, { 0.74f, 0.34f }, { 0.0f, 0.74f } };
    struct Layout { const A2* a; int n; };
    static const Layout layouts[] = { { quad, 4 }, { cardinal, 4 }, { flanks, 4 }, { diag, 4 }, { landmark, 4 } };
    const Layout& L = layouts[mix(static_cast<uint32_t>(roomIndex) + 1u, static_cast<uint32_t>(biome) + 11u) % 5u];

    const auto clearSpot = [&](float wx, float wz, float rad) {
        for (const Placed& t : taken)
            if ((wx - t.x) * (wx - t.x) + (wz - t.z) * (wz - t.z) < (rad + t.r + 0.5f) * (rad + t.r + 0.5f)) return false;
        return true;
    };

    for (int ci = 0; ci < L.n; ++ci) {
        const bool hero = (ci == 0);
        const float wx = std::clamp(cx + L.a[ci].x * halfW + rng.frange(-0.8f, 0.8f), ix0, ix1);
        const float wz = std::clamp(cz + L.a[ci].z * halfH + rng.frange(-0.8f, 0.8f), iz0, iz1);
        // A ruins biome makes its islands STRUCTURES (broken courtyard); other biomes use big rocks.
        const bool asStruct = ruins && !structures_.empty() && (hero || rng.range(0, 2) == 0);
        if (asStruct) {
            const Prop* s = pick(structures_, rng.next());
            const float rad = s ? std::max(s->halfX, s->halfZ) * 0.55f : 0.0f;
            if (s && clearSpot(wx, wz, rad)) {
                place(draws_, *s, wx, wz, rng.frange(0.0f, 2.0f * kPi), 1.0f);
                stampSolidCircle(wx, wz, rad, 9.0f);              // tall structure - unscalable
                taken.push_back({ wx, wz, rad });
                groundClutter(wx, wz, rad + 2.4f, rng.range(5, 9));
            }
            continue;
        }
        const Prop* h = pick(rocks_, rng.next());
        if (!h) continue;
        const float hScale = hero ? rng.frange(1.7f, 2.3f) : rng.frange(1.1f, 1.6f);
        const float hRad = h->radius * hScale;
        if (!clearSpot(wx, wz, hRad)) continue;
        place(draws_, *h, wx, wz, rng.frange(0.0f, 2.0f * kPi), hScale);
        taken.push_back({ wx, wz, hRad });
        // Satellites nestled around the hero -> a composed island of mixed-height cover.
        for (int s = 0, sats = hero ? rng.range(2, 4) : rng.range(1, 2); s < sats; ++s) {
            const bool crate = !crates_.empty() && (rng.range(0, 2) == 0);
            const Prop* sp = pick(crate ? crates_ : rocks_, rng.next());
            if (!sp) continue;
            const float sScale = crate ? rng.frange(0.8f, 1.2f) : rng.frange(0.8f, 1.25f);
            const float sRad = sp->radius * sScale;
            const float a = rng.frange(0.0f, 2.0f * kPi), d = hRad + sRad + rng.frange(0.15f, 1.0f);
            const float sx = std::clamp(wx + std::cos(a) * d, ix0, ix1);
            const float sz = std::clamp(wz + std::sin(a) * d, iz0, iz1);
            if (!clearSpot(sx, sz, sRad)) continue;
            place(draws_, *sp, sx, sz, rng.frange(0.0f, 2.0f * kPi), sScale);
            taken.push_back({ sx, sz, sRad });
        }
        groundClutter(wx, wz, hRad + 2.4f, rng.range(5, 9));   // debris field carpets the island base
    }

    // Trees: the forest fills the berm (above) + scatters a few in the outer band; rocky gets an
    // occasional lone tree; ruins none. Kept off the open centre + the cover islands.
    for (int i = 0, n = forest ? rng.range(5, 9) : (ruins ? 0 : rng.range(0, 1)); i < n && !trees_.empty(); ++i) {
        const Prop* t = pick(trees_, rng.next());
        if (!t) continue;
        float wx = 0.0f, wz = 0.0f; bool ok = false;
        for (int a = 0; a < 16 && !ok; ++a) {
            wx = rng.frange(ix0, ix1); wz = rng.frange(iz0, iz1);
            ok = clearSpot(wx, wz, t->radius + 0.3f);
        }
        if (ok) { place(draws_, *t, wx, wz, rng.frange(0.0f, 2.0f * kPi), rng.frange(0.85f, 1.2f)); taken.push_back({ wx, wz, t->radius }); }
    }

    // Decoration (NO collision now): grass carpet in the forest, light tufts elsewhere, plus a light
    // global scatter of ground rocks. The heavy grounding debris sits around the islands (above), so
    // the open ground reads clean and readable instead of uniformly noisy.
    for (int i = 0, n = forest ? rng.range(50, 80) : rng.range(8, 14); i < n && !grass_.empty(); ++i) {
        const Prop* g = pick(grass_, rng.next());
        if (g) place(draws_, *g, rng.frange(fx0, fx1), rng.frange(fz0, fz1), rng.frange(0.0f, 2.0f * kPi), rng.frange(0.7f, 1.6f));
    }
    for (int i = 0, n = rng.range(16, 28); i < n && !smallRocks_.empty(); ++i) {
        const Prop* r = pick(smallRocks_, rng.next());
        if (r) place(draws_, *r, rng.frange(fx0, fx1), rng.frange(fz0, fz1), rng.frange(0.0f, 2.0f * kPi), rng.frange(0.7f, 1.6f));
    }

    for (int z = 0; z < 24; ++z)
        for (int x = 0; x < 32; ++x) {
            const int fx = x * kSub + kSub / 2, fz = z * kSub + kSub / 2;
            grid_[static_cast<size_t>(z)][static_cast<size_t>(x)] = fine_[static_cast<size_t>(fz) * FW + fx] ? '#' : '.';
        }

    spawnX_ = std::min(std::max(static_cast<int>(cx), 1), 30);
    spawnZ_ = std::min(std::max(static_cast<int>(cz), 1), 22);
    for (int z = spawnZ_ - 1; z <= spawnZ_ + 1; ++z)
        for (int x = spawnX_ - 1; x <= spawnX_ + 1; ++x)
            if (x >= 1 && x < 31 && z >= 1 && z < 23) grid_[static_cast<size_t>(z)][static_cast<size_t>(x)] = '.';
    grid_[static_cast<size_t>(spawnZ_)][static_cast<size_t>(spawnX_)] = 'P';
    for (int fz = (spawnZ_ - 1) * kSub; fz < (spawnZ_ + 2) * kSub; ++fz)
        for (int fx = (spawnX_ - 1) * kSub; fx < (spawnX_ + 2) * kSub; ++fx)
            if (fx >= 0 && fz >= 0 && fx < FW && fz < FH) {
                fine_[static_cast<size_t>(fz) * FW + fx] = 0;
                fineHeight_[static_cast<size_t>(fz) * FW + fx] = 0.0f;
            }
}

} // namespace pulse
