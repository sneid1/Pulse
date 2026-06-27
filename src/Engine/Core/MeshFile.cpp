#include "Engine/Core/MeshFile.hpp"

#include "cgltf.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace pulse {

// Append one glTF triangle primitive (POSITION/NORMAL/TEXCOORD_0/COLOR_0) to
// verts/indices, applying the column-major world transform m (m[col*4+row]);
// m==nullptr means identity. Shared by loadGltfMesh and loadGltfTiles.
static void appendPrimitive(const cgltf_primitive& prim, const float* m,
                            std::vector<StaticVertex>& verts, std::vector<uint32_t>& indices) {
    if (prim.type != cgltf_primitive_type_triangles) return;
    const cgltf_accessor* pos = nullptr;
    const cgltf_accessor* nrm = nullptr;
    const cgltf_accessor* uv  = nullptr;
    const cgltf_accessor* col = nullptr;
    for (size_t a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& at = prim.attributes[a];
        if (at.type == cgltf_attribute_type_position) pos = at.data;
        else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
        else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
        else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
    }
    if (!pos) return;
    Vec4f materialColor{ 1, 1, 1, 1 };
    if (prim.material && prim.material->has_pbr_metallic_roughness) {
        const cgltf_float* f = prim.material->pbr_metallic_roughness.base_color_factor;
        materialColor = { f[0], f[1], f[2], f[3] };
    }
    const size_t base = verts.size();
    const size_t n = pos->count;
    for (size_t i = 0; i < n; ++i) {
        StaticVertex v{};
        float p[3] = { 0, 0, 0 };
        cgltf_accessor_read_float(pos, i, p, 3);
        if (m) v.pos = { m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12],
                         m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13],
                         m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14] };
        else   v.pos = { p[0], p[1], p[2] };
        if (nrm) {
            float nn[3] = { 0, 1, 0 };
            cgltf_accessor_read_float(nrm, i, nn, 3);
            Vec3f wn = m ? Vec3f{ m[0]*nn[0] + m[4]*nn[1] + m[8]*nn[2],
                                  m[1]*nn[0] + m[5]*nn[1] + m[9]*nn[2],
                                  m[2]*nn[0] + m[6]*nn[1] + m[10]*nn[2] }
                         : Vec3f{ nn[0], nn[1], nn[2] };
            v.nrm = normalize3(wn);
        }
        if (uv) {
            float t[2] = { 0, 0 };
            cgltf_accessor_read_float(uv, i, t, 2);
            v.uv0[0] = t[0]; v.uv0[1] = t[1];
        }
        v.color = materialColor;
        if (col) {
            float c[4] = { 1, 1, 1, 1 };
            cgltf_accessor_read_float(col, i, c, 4);
            v.color = { c[0] * materialColor.x, c[1] * materialColor.y,
                        c[2] * materialColor.z, c[3] * materialColor.w };
        }
        verts.push_back(v);
    }
    if (prim.indices) {
        for (size_t i = 0; i < prim.indices->count; ++i)
            indices.push_back(static_cast<uint32_t>(base + cgltf_accessor_read_index(prim.indices, i)));
    } else {
        for (size_t i = 0; i < n; ++i) indices.push_back(static_cast<uint32_t>(base + i));
    }
}

static void fillImageRef(const cgltf_image* image, std::string& uri, std::vector<uint8_t>& embeddedBytes) {
    if (!image) return;
    if (image->uri) {
        uri = image->uri;
        return;
    }
    const cgltf_buffer_view* view = image->buffer_view;
    if (!view || view->size == 0) return;
    const uint8_t* bytes = static_cast<const uint8_t*>(view->data);
    if (!bytes && view->buffer && view->buffer->data)
        bytes = static_cast<const uint8_t*>(view->buffer->data) + view->offset;
    if (!bytes) return;
    embeddedBytes.assign(bytes, bytes + view->size);
}

static void fillTextureRef(const cgltf_texture_view& view, std::string& uri, std::vector<uint8_t>& embeddedBytes) {
    if (!view.texture || !view.texture->image) return;
    fillImageRef(view.texture->image, uri, embeddedBytes);
}

// Pull the glTF PBR material (factors + base-color/normal texture URIs/embedded bytes) into a
// TileSubmesh, so real-asset kits render with their authored materials, not just a vertex tint.
static void fillSubmeshMaterial(const cgltf_primitive& prim, TileSubmesh& sub) {
    const cgltf_material* mt = prim.material;
    if (!mt) return;
    if (mt->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pr = mt->pbr_metallic_roughness;
        sub.baseColorFactor = { pr.base_color_factor[0], pr.base_color_factor[1],
                                pr.base_color_factor[2], pr.base_color_factor[3] };
        sub.metallic = pr.metallic_factor;
        sub.roughness = pr.roughness_factor;
        fillTextureRef(pr.base_color_texture, sub.baseColorTex, sub.baseColorImage);
        // Packed occlusion/roughness/metallic map (glTF metallicRoughnessTexture). Quaternius
        // ships these as T_*_ORM.png (R=AO, G=rough, B=metal) - the engine's ORM convention.
        fillTextureRef(pr.metallic_roughness_texture, sub.ormTex, sub.ormImage);
    }
    sub.emissiveFactor = { mt->emissive_factor[0], mt->emissive_factor[1], mt->emissive_factor[2] };
    fillTextureRef(mt->normal_texture, sub.normalTex, sub.normalImage);
    fillTextureRef(mt->emissive_texture, sub.emissiveTex, sub.emissiveImage);
}

bool loadGltfMesh(const std::string& path,
                  std::vector<StaticVertex>& verts, std::vector<uint32_t>& indices) {
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    // glTF node world transforms are column-major 4x4 (m[col*4 + row]).
    auto addPrimitive = [&](const cgltf_primitive& prim, const float* m) {
        appendPrimitive(prim, m, verts, indices);
    };

    bool any = false;
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;
        float m[16];
        cgltf_node_transform_world(&node, m);
        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi) { addPrimitive(node.mesh->primitives[pi], m); any = true; }
    }
    if (!any) {   // no scene nodes referencing meshes: load meshes directly (identity)
        for (size_t mi = 0; mi < data->meshes_count; ++mi)
            for (size_t pi = 0; pi < data->meshes[mi].primitives_count; ++pi) { addPrimitive(data->meshes[mi].primitives[pi], nullptr); any = true; }
    }

    cgltf_free(data);
    return any && !verts.empty();
}

bool loadGltfTiles(const std::string& path, std::vector<KitTile>& tiles) {
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    // Mesh names follow "<tile>_<material>_<index>" (e.g. "Room.X_brickwall1_0").
    // Group every primitive under its tile, split by material, world transforms applied.
    std::unordered_map<std::string, size_t> tileIndex;
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh || !node.mesh->name) continue;
        const std::string mn = node.mesh->name;
        const size_t u1 = mn.find('_');
        if (u1 == std::string::npos) continue;
        const std::string tileName = mn.substr(0, u1);
        const size_t u2 = mn.find('_', u1 + 1);
        const std::string material =
            mn.substr(u1 + 1, (u2 == std::string::npos ? mn.size() : u2) - (u1 + 1));
        if (tileName.empty() || material.empty()) continue;      // e.g. "collision__0"
        if (tileName.compare(0, 5, "colli") == 0) continue;      // collision / collsiion proxies

        size_t ti;
        auto it = tileIndex.find(tileName);
        if (it == tileIndex.end()) {
            ti = tiles.size();
            tileIndex.emplace(tileName, ti);
            tiles.push_back(KitTile{});
            tiles[ti].name = tileName;
        } else {
            ti = it->second;
        }
        KitTile& tile = tiles[ti];

        TileSubmesh* sub = nullptr;
        for (TileSubmesh& s : tile.submeshes)
            if (s.material == material) { sub = &s; break; }
        if (!sub) {
            tile.submeshes.push_back(TileSubmesh{});
            sub = &tile.submeshes.back();
            sub->material = material;
        }

        float m[16];
        cgltf_node_transform_world(&node, m);
        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi)
            appendPrimitive(node.mesh->primitives[pi], m, sub->verts, sub->indices);
    }
    cgltf_free(data);

    // Re-center each tile: footprint centre to origin in X/Z, floor (min Y) to 0.
    for (KitTile& tile : tiles) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (const TileSubmesh& s : tile.submeshes)
            for (const StaticVertex& v : s.verts) {
                lo[0] = v.pos.x < lo[0] ? v.pos.x : lo[0]; hi[0] = v.pos.x > hi[0] ? v.pos.x : hi[0];
                lo[1] = v.pos.y < lo[1] ? v.pos.y : lo[1]; hi[1] = v.pos.y > hi[1] ? v.pos.y : hi[1];
                lo[2] = v.pos.z < lo[2] ? v.pos.z : lo[2]; hi[2] = v.pos.z > hi[2] ? v.pos.z : hi[2];
            }
        if (hi[0] < lo[0]) continue;   // tile had no geometry
        const float cx = 0.5f * (lo[0] + hi[0]);
        const float cz = 0.5f * (lo[2] + hi[2]);
        const float fy = lo[1];
        for (TileSubmesh& s : tile.submeshes)
            for (StaticVertex& v : s.verts) { v.pos.x -= cx; v.pos.y -= fy; v.pos.z -= cz; }
        tile.sizeX = hi[0] - lo[0];
        tile.sizeY = hi[1] - lo[1];
        tile.sizeZ = hi[2] - lo[2];
    }
    return !tiles.empty();
}

bool loadGltfWhole(const std::string& path, KitTile& out) {
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    out.submeshes.clear();
    out.name.clear();
    // Group every primitive by its glTF MATERIAL name (robust across kits) into submeshes.
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;
        float m[16];
        cgltf_node_transform_world(&node, m);
        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            std::string material = (prim.material && prim.material->name) ? prim.material->name : "default";
            if (material.compare(0, 5, "colli") == 0) continue;   // skip collision proxies
            TileSubmesh* sub = nullptr;
            for (TileSubmesh& s : out.submeshes)
                if (s.material == material) { sub = &s; break; }
            if (!sub) {
                out.submeshes.push_back(TileSubmesh{});
                sub = &out.submeshes.back();
                sub->material = material;
                fillSubmeshMaterial(prim, *sub);
            }
            appendPrimitive(prim, m, sub->verts, sub->indices);
        }
    }
    cgltf_free(data);

    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const TileSubmesh& s : out.submeshes)
        for (const StaticVertex& v : s.verts) {
            lo[0] = v.pos.x < lo[0] ? v.pos.x : lo[0]; hi[0] = v.pos.x > hi[0] ? v.pos.x : hi[0];
            lo[1] = v.pos.y < lo[1] ? v.pos.y : lo[1]; hi[1] = v.pos.y > hi[1] ? v.pos.y : hi[1];
            lo[2] = v.pos.z < lo[2] ? v.pos.z : lo[2]; hi[2] = v.pos.z > hi[2] ? v.pos.z : hi[2];
        }
    if (hi[0] < lo[0]) return false;   // no geometry
    const float cx = 0.5f * (lo[0] + hi[0]), cz = 0.5f * (lo[2] + hi[2]), fy = lo[1];
    for (TileSubmesh& s : out.submeshes)
        for (StaticVertex& v : s.verts) { v.pos.x -= cx; v.pos.y -= fy; v.pos.z -= cz; }
    out.sizeX = hi[0] - lo[0];
    out.sizeY = hi[1] - lo[1];
    out.sizeZ = hi[2] - lo[2];
    return true;
}

bool writeGlb(const std::string& path,
              const std::vector<StaticVertex>& verts, const std::vector<uint32_t>& indices) {
    if (verts.empty() || indices.empty()) return false;
    const uint32_t n = static_cast<uint32_t>(verts.size());
    const uint32_t idxCount = static_cast<uint32_t>(indices.size());

    // BIN buffer: positions | normals | uv0 | indices (each accessor contiguous).
    const uint32_t posBytes = n * 12, nrmBytes = n * 12, uvBytes = n * 8, idxBytes = idxCount * 4;
    std::vector<uint8_t> bin(posBytes + nrmBytes + uvBytes + idxBytes);
    float pmin[3] = {  1e30f,  1e30f,  1e30f };
    float pmax[3] = { -1e30f, -1e30f, -1e30f };
    auto* fp = reinterpret_cast<float*>(bin.data());
    for (uint32_t i = 0; i < n; ++i) {
        const Vec3f& p = verts[i].pos;
        fp[i*3+0] = p.x; fp[i*3+1] = p.y; fp[i*3+2] = p.z;
        pmin[0] = p.x < pmin[0] ? p.x : pmin[0]; pmax[0] = p.x > pmax[0] ? p.x : pmax[0];
        pmin[1] = p.y < pmin[1] ? p.y : pmin[1]; pmax[1] = p.y > pmax[1] ? p.y : pmax[1];
        pmin[2] = p.z < pmin[2] ? p.z : pmin[2]; pmax[2] = p.z > pmax[2] ? p.z : pmax[2];
    }
    auto* fn = reinterpret_cast<float*>(bin.data() + posBytes);
    for (uint32_t i = 0; i < n; ++i) { fn[i*3+0] = verts[i].nrm.x; fn[i*3+1] = verts[i].nrm.y; fn[i*3+2] = verts[i].nrm.z; }
    auto* fu = reinterpret_cast<float*>(bin.data() + posBytes + nrmBytes);
    for (uint32_t i = 0; i < n; ++i) { fu[i*2+0] = verts[i].uv0[0]; fu[i*2+1] = verts[i].uv0[1]; }
    std::memcpy(bin.data() + posBytes + nrmBytes + uvBytes, indices.data(), idxBytes);

    char json[2048];
    const int jlen = std::snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"pulse-import\"},"
        "\"buffers\":[{\"byteLength\":%u}],"
        "\"bufferViews\":["
          "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"target\":34962},"
          "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},"
          "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},"
          "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}],"
        "\"accessors\":["
          "{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]},"
          "{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"},"
          "{\"bufferView\":3,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}",
        static_cast<uint32_t>(bin.size()),
        posBytes, posBytes, nrmBytes, posBytes + nrmBytes, uvBytes, posBytes + nrmBytes + uvBytes, idxBytes,
        n, pmin[0], pmin[1], pmin[2], pmax[0], pmax[1], pmax[2], n, n, idxCount);
    if (jlen <= 0 || jlen >= static_cast<int>(sizeof(json))) return false;

    // Pad JSON chunk to 4 bytes with spaces, BIN chunk to 4 with zeros.
    uint32_t jsonLen = static_cast<uint32_t>(jlen);
    const uint32_t jsonPad = (4 - (jsonLen & 3)) & 3;
    const uint32_t binPad  = (4 - (static_cast<uint32_t>(bin.size()) & 3)) & 3;
    const uint32_t jsonChunkLen = jsonLen + jsonPad;
    const uint32_t binChunkLen  = static_cast<uint32_t>(bin.size()) + binPad;
    const uint32_t total = 12 + 8 + jsonChunkLen + 8 + binChunkLen;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    u32(0x46546C67u); u32(2); u32(total);                         // "glTF", version 2, total length
    u32(jsonChunkLen); u32(0x4E4F534Au);                          // JSON chunk header ("JSON")
    std::fwrite(json, 1, jsonLen, f);
    for (uint32_t i = 0; i < jsonPad; ++i) std::fputc(' ', f);
    u32(binChunkLen); u32(0x004E4942u);                           // BIN chunk header ("BIN\0")
    std::fwrite(bin.data(), 1, bin.size(), f);
    for (uint32_t i = 0; i < binPad; ++i) std::fputc('\0', f);
    std::fclose(f);
    return true;
}

bool loadObjMesh(const std::string& path,
                 std::vector<StaticVertex>& verts, std::vector<uint32_t>& indices) {
    std::ifstream in(path);
    if (!in) return false;
    std::vector<Vec3f> positions, normals;
    std::vector<float> uvU, uvV;
    std::unordered_map<uint64_t, uint32_t> unique;   // (v,vt,vn) packed -> output index

    auto emit = [&](long vi, long ti, long ni) {
        const long P = static_cast<long>(positions.size());
        const long T = static_cast<long>(uvU.size());
        const long N = static_cast<long>(normals.size());
        const uint32_t pv = static_cast<uint32_t>(vi < 0 ? P + vi : vi - 1);
        const uint32_t pt = ti == 0 ? 0xFFFFFFu : static_cast<uint32_t>(ti < 0 ? T + ti : ti - 1);
        const uint32_t pn = ni == 0 ? 0xFFFFFFu : static_cast<uint32_t>(ni < 0 ? N + ni : ni - 1);
        const uint64_t key = (static_cast<uint64_t>(pv) << 40) ^ (static_cast<uint64_t>(pt) << 20) ^ pn;
        auto it = unique.find(key);
        if (it != unique.end()) { indices.push_back(it->second); return; }
        StaticVertex v{};
        if (pv < positions.size()) v.pos = positions[pv];
        if (pn != 0xFFFFFFu && pn < normals.size()) v.nrm = normals[pn];
        if (pt != 0xFFFFFFu && pt < uvU.size()) { v.uv0[0] = uvU[pt]; v.uv0[1] = uvV[pt]; }
        v.color = { 1, 1, 1, 1 };
        const uint32_t idx = static_cast<uint32_t>(verts.size());
        verts.push_back(v);
        unique.emplace(key, idx);
        indices.push_back(idx);
    };

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "v") { Vec3f p; ss >> p.x >> p.y >> p.z; positions.push_back(p); }
        else if (tag == "vn") { Vec3f n; ss >> n.x >> n.y >> n.z; normals.push_back(n); }
        else if (tag == "vt") { float u = 0, vv = 0; ss >> u >> vv; uvU.push_back(u); uvV.push_back(vv); }
        else if (tag == "f") {
            std::vector<std::array<long, 3>> face;
            std::string tok;
            while (ss >> tok) {
                long vi = 0, ti = 0, ni = 0;   // OBJ forms: v, v/t, v//n, v/t/n
                const size_t s1 = tok.find('/');
                if (s1 == std::string::npos) {
                    vi = std::strtol(tok.c_str(), nullptr, 10);
                } else {
                    vi = std::strtol(tok.substr(0, s1).c_str(), nullptr, 10);
                    const size_t s2 = tok.find('/', s1 + 1);
                    if (s2 == std::string::npos) {
                        ti = std::strtol(tok.substr(s1 + 1).c_str(), nullptr, 10);
                    } else {
                        const std::string tstr = tok.substr(s1 + 1, s2 - s1 - 1);
                        const std::string nstr = tok.substr(s2 + 1);
                        if (!tstr.empty()) ti = std::strtol(tstr.c_str(), nullptr, 10);
                        if (!nstr.empty()) ni = std::strtol(nstr.c_str(), nullptr, 10);
                    }
                }
                face.push_back({ vi, ti, ni });
            }
            for (size_t i = 2; i < face.size(); ++i) {   // triangulate fan
                emit(face[0][0], face[0][1], face[0][2]);
                emit(face[i-1][0], face[i-1][1], face[i-1][2]);
                emit(face[i][0], face[i][1], face[i][2]);
            }
        }
    }
    return !verts.empty() && !indices.empty();
}

} // namespace pulse
