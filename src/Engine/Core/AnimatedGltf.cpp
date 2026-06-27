#include "Engine/Core/AnimatedGltf.hpp"

#include "cgltf.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace pulse {
namespace {

int nodeIndex(const cgltf_data* data, const cgltf_node* node) {
    if (!node) return -1;
    return static_cast<int>(node - data->nodes);
}

int skinIndex(const cgltf_data* data, const cgltf_skin* skin) {
    if (!skin) return -1;
    return static_cast<int>(skin - data->skins);
}

int materialIndex(const cgltf_data* data, const cgltf_material* mat) {
    if (!mat) return -1;
    return static_cast<int>(mat - data->materials);
}

std::string imagePath(const std::filesystem::path& baseDir, const cgltf_texture_view& view) {
    if (!view.texture || !view.texture->image) return {};
    const cgltf_image* img = view.texture->image;
    // External image: a plain file uri (skip data: uris, handled as embedded below).
    if (img->uri && std::string_view(img->uri).substr(0, 5) != "data:") {
        std::filesystem::path p = img->uri;
        if (p.is_absolute()) return p.string();
        return (baseDir / p).lexically_normal().string();
    }
    // Embedded image (glTF stores the PNG/JPG in a bufferView - typical of .glb files and the
    // Quaternius "Textured" .gltf packs, as well as Blender glb round-trips). The downstream
    // texture loader is file-based, so extract the raw image bytes once into a temp cache file
    // and return that path. stb_image auto-detects the format from the bytes, so the cache file's
    // extension does not matter. Without this, embedded textures load as nothing (flat white).
    if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
        const cgltf_buffer_view* bv = img->buffer_view;
        const uint8_t* src = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
        const size_t n = bv->size;
        std::error_code ec;
        const std::filesystem::path cacheDir = std::filesystem::temp_directory_path(ec) / "pulse_gltfcache";
        std::filesystem::create_directories(cacheDir, ec);
        const std::string key = baseDir.string() + "|" + (img->name ? img->name : "") + "|" + std::to_string(n);
        const std::filesystem::path cacheFile = cacheDir / (std::to_string(std::hash<std::string>{}(key)) + ".img");
        if (!std::filesystem::exists(cacheFile, ec)) {
            std::ofstream out(cacheFile, std::ios::binary);
            if (out) out.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(n));
        }
        return cacheFile.string();
    }
    return {};
}

float readAccessorFloat(const cgltf_accessor* a, size_t index, int component) {
    float v[16] = {};
    cgltf_accessor_read_float(a, index, v, 16);
    return v[component];
}

Vec3f readVec3(const cgltf_accessor* a, size_t index, Vec3f fallback = {}) {
    if (!a || index >= a->count) return fallback;
    float v[3] = { fallback.x, fallback.y, fallback.z };
    cgltf_accessor_read_float(a, index, v, 3);
    return { v[0], v[1], v[2] };
}

Vec4f readVec4(const cgltf_accessor* a, size_t index, Vec4f fallback = {}) {
    if (!a || index >= a->count) return fallback;
    float v[4] = { fallback.x, fallback.y, fallback.z, fallback.w };
    cgltf_accessor_read_float(a, index, v, 4);
    return { v[0], v[1], v[2], v[3] };
}

uint16_t readJointIndex(const cgltf_accessor* a, size_t index, int component) {
    if (!a || index >= a->count) return 0;
    float v[4] = {};
    cgltf_accessor_read_float(a, index, v, 4);
    return static_cast<uint16_t>(std::max(0.0f, v[component]));
}

float channelValue(const std::vector<float>& values, int width, bool cubicSpline,
                   size_t key, int component) {
    const size_t stride = cubicSpline
        ? static_cast<size_t>(width) * 3u : static_cast<size_t>(width);
    const size_t valueOffset = cubicSpline ? static_cast<size_t>(width) : 0u;
    const size_t at = key * stride + valueOffset + static_cast<size_t>(component);
    return at < values.size() ? values[at] : 0.0f;
}

float channelInTangent(const std::vector<float>& values, int width, size_t key, int component) {
    const size_t at = key * static_cast<size_t>(width) * 3u + static_cast<size_t>(component);
    return at < values.size() ? values[at] : 0.0f;
}

float channelOutTangent(const std::vector<float>& values, int width, size_t key, int component) {
    const size_t at = key * static_cast<size_t>(width) * 3u +
        static_cast<size_t>(width) * 2u + static_cast<size_t>(component);
    return at < values.size() ? values[at] : 0.0f;
}

float hermite(float p0, float m0, float p1, float m1, float t, float dt) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0
        + (t3 - 2.0f * t2 + t) * m0 * dt
        + (-2.0f * t3 + 3.0f * t2) * p1
        + (t3 - t2) * m1 * dt;
}

Vec3f add3(Vec3f a, Vec3f b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3f scale3(Vec3f a, float s) { return { a.x * s, a.y * s, a.z * s }; }

uint8_t jointTag(std::string_view name) {
    if (name.find(".L") != std::string_view::npos) return 1;
    if (name.find("arm.R") != std::string_view::npos ||
        name.find("forearm.R") != std::string_view::npos) return 3;
    if (name.find(".R") != std::string_view::npos) return 2;
    return 0;
}

} // namespace

AnimatedGltfModel::MatC AnimatedGltfModel::identity() {
    MatC r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

AnimatedGltfModel::MatC AnimatedGltfModel::mul(const MatC& a, const MatC& b) {
    MatC r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

AnimatedGltfModel::Quat AnimatedGltfModel::normalize(Quat q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l <= 1.0e-8f) return {};
    const float inv = 1.0f / l;
    return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

AnimatedGltfModel::Quat AnimatedGltfModel::nlerp(Quat a, Quat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
        b = { -b.x, -b.y, -b.z, -b.w };
    }
    return normalize({
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t,
    });
}

AnimatedGltfModel::MatC AnimatedGltfModel::trs(Vec3f t, Quat rq, Vec3f s) {
    const Quat q = normalize(rq);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    MatC r = identity();
    r.m[0] = (1.0f - 2.0f * (yy + zz)) * s.x;
    r.m[1] = (2.0f * (xy + wz)) * s.x;
    r.m[2] = (2.0f * (xz - wy)) * s.x;

    r.m[4] = (2.0f * (xy - wz)) * s.y;
    r.m[5] = (1.0f - 2.0f * (xx + zz)) * s.y;
    r.m[6] = (2.0f * (yz + wx)) * s.y;

    r.m[8] = (2.0f * (xz + wy)) * s.z;
    r.m[9] = (2.0f * (yz - wx)) * s.z;
    r.m[10] = (1.0f - 2.0f * (xx + yy)) * s.z;

    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

Vec3f AnimatedGltfModel::transformPoint(const MatC& m, Vec3f p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
    };
}

Vec3f AnimatedGltfModel::transformVector(const MatC& m, Vec3f v) {
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z,
    };
}

Vec3f AnimatedGltfModel::toViewmodel(Vec3f p) {
    return { -p.x, p.z, -p.y };
}

bool AnimatedGltfModel::load(const std::string& path) {
    nodes_.clear();
    skins_.clear();
    primitives_.clear();
    clips_.clear();
    materials_.clear();
    submeshes_.clear();

    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    const std::filesystem::path baseDir = std::filesystem::path(path).parent_path();

    nodes_.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& src = data->nodes[i];
        Node& n = nodes_[i];
        n.name = src.name ? src.name : "";
        n.parent = nodeIndex(data, src.parent);
        n.hasMatrix = src.has_matrix;
        if (src.has_matrix) {
            for (int k = 0; k < 16; ++k) n.matrix.m[k] = src.matrix[k];
        } else {
            n.matrix = identity();
        }
        if (src.has_translation) n.translation = { src.translation[0], src.translation[1], src.translation[2] };
        if (src.has_rotation) n.rotation = { src.rotation[0], src.rotation[1], src.rotation[2], src.rotation[3] };
        if (src.has_scale) n.scale = { src.scale[0], src.scale[1], src.scale[2] };
        for (size_t c = 0; c < src.children_count; ++c) {
            n.children.push_back(nodeIndex(data, src.children[c]));
        }
    }

    materials_.reserve(data->materials_count);
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& src = data->materials[i];
        AnimatedGltfMaterial m;
        m.name = src.name ? src.name : "";
        if (src.has_pbr_metallic_roughness) {
            const auto& pbr = src.pbr_metallic_roughness;
            m.baseColorFactor = {
                pbr.base_color_factor[0],
                pbr.base_color_factor[1],
                pbr.base_color_factor[2],
                pbr.base_color_factor[3],
            };
            m.metallicFactor = pbr.metallic_factor;
            m.roughnessFactor = pbr.roughness_factor;
            m.baseColorTexture = imagePath(baseDir, pbr.base_color_texture);
            m.metallicRoughnessTexture = imagePath(baseDir, pbr.metallic_roughness_texture);
        }
        m.normalTexture = imagePath(baseDir, src.normal_texture);
        m.emissiveTexture = imagePath(baseDir, src.emissive_texture);
        m.emissiveFactor = { src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2] };
        materials_.push_back(std::move(m));
    }

    skins_.resize(data->skins_count);
    for (size_t si = 0; si < data->skins_count; ++si) {
        const cgltf_skin& src = data->skins[si];
        Skin& skin = skins_[si];
        skin.joints.reserve(src.joints_count);
        for (size_t j = 0; j < src.joints_count; ++j) skin.joints.push_back(nodeIndex(data, src.joints[j]));
        skin.inverseBind.resize(src.joints_count, identity());
        if (src.inverse_bind_matrices) {
            for (size_t j = 0; j < src.joints_count; ++j) {
                float m[16] = {};
                cgltf_accessor_read_float(src.inverse_bind_matrices, j, m, 16);
                for (int k = 0; k < 16; ++k) skin.inverseBind[j].m[k] = m[k];
            }
        }
    }

    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;
        const int nodeSkin = skinIndex(data, node.skin);
        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* tan = nullptr;
            const cgltf_accessor* uv = nullptr;
            const cgltf_accessor* col = nullptr;
            const cgltf_accessor* joints = nullptr;
            const cgltf_accessor* weights = nullptr;
            for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& at = prim.attributes[ai];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_tangent) tan = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 0) joints = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 0) weights = at.data;
            }
            if (!pos) continue;

            Primitive out;
            out.name = node.mesh->name ? node.mesh->name : "";
            out.materialIndex = materialIndex(data, prim.material);
            out.nodeIndex = static_cast<int>(ni);
            out.skinIndex = nodeSkin;
            out.vertices.resize(pos->count);
            for (size_t vi = 0; vi < pos->count; ++vi) {
                VertexSource& v = out.vertices[vi];
                v.pos = readVec3(pos, vi);
                v.nrm = readVec3(nrm, vi, { 0, 1, 0 });
                if (tan) v.tangent = readVec4(tan, vi, { 1, 0, 0, 1 });
                if (uv) {
                    float t[2] = { 0, 0 };
                    cgltf_accessor_read_float(uv, vi, t, 2);
                    v.uv0[0] = t[0]; v.uv0[1] = t[1];
                }
                if (col) v.color = readVec4(col, vi, { 1, 1, 1, 1 });
                float weightSum = 0.0f;
                for (int c = 0; c < 4; ++c) {
                    v.joints[c] = readJointIndex(joints, vi, c);
                    v.weights[c] = weights ? readAccessorFloat(weights, vi, c) : 0.0f;
                    weightSum += v.weights[c];
                }
                if (weightSum > 1.0e-6f) {
                    for (float& w : v.weights) w /= weightSum;
                }
                if (out.skinIndex >= 0 && out.skinIndex < static_cast<int>(skins_.size())) {
                    const Skin& skin = skins_[static_cast<size_t>(out.skinIndex)];
                    float bestWeight = 0.0f;
                    int bestJoint = -1;
                    for (int c = 0; c < 4; ++c) {
                        if (v.weights[c] > bestWeight) {
                            bestWeight = v.weights[c];
                            bestJoint = static_cast<int>(v.joints[c]);
                        }
                    }
                    if (bestJoint >= 0 && bestJoint < static_cast<int>(skin.joints.size())) {
                        const int jointNode = skin.joints[static_cast<size_t>(bestJoint)];
                        if (jointNode >= 0 && jointNode < static_cast<int>(nodes_.size())) {
                            v.tag = jointTag(nodes_[static_cast<size_t>(jointNode)].name);
                        }
                    }
                }
            }
            if (prim.indices) {
                out.indices.reserve(prim.indices->count);
                for (size_t ii = 0; ii < prim.indices->count; ++ii)
                    out.indices.push_back(static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, ii)));
            } else {
                out.indices.reserve(pos->count);
                for (size_t ii = 0; ii < pos->count; ++ii) out.indices.push_back(static_cast<uint32_t>(ii));
            }
            primitives_.push_back(std::move(out));
        }
    }

    clips_.resize(data->animations_count);
    for (size_t ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& src = data->animations[ai];
        Clip& clip = clips_[ai];
        clip.name = src.name ? src.name : "";
        for (size_t ci = 0; ci < src.channels_count; ++ci) {
            const cgltf_animation_channel& csrc = src.channels[ci];
            if (!csrc.sampler || !csrc.sampler->input || !csrc.sampler->output || !csrc.target_node) continue;
            Channel ch;
            ch.node = nodeIndex(data, csrc.target_node);
            if (csrc.target_path == cgltf_animation_path_type_translation) { ch.path = ChannelPath::Translation; ch.width = 3; }
            else if (csrc.target_path == cgltf_animation_path_type_rotation) { ch.path = ChannelPath::Rotation; ch.width = 4; }
            else if (csrc.target_path == cgltf_animation_path_type_scale) { ch.path = ChannelPath::Scale; ch.width = 3; }
            else continue;
            if (csrc.sampler->interpolation == cgltf_interpolation_type_step) ch.interpolation = Interpolation::Step;
            else if (csrc.sampler->interpolation == cgltf_interpolation_type_cubic_spline) ch.interpolation = Interpolation::CubicSpline;
            else ch.interpolation = Interpolation::Linear;

            ch.times.resize(csrc.sampler->input->count);
            for (size_t t = 0; t < ch.times.size(); ++t) {
                ch.times[t] = readAccessorFloat(csrc.sampler->input, t, 0);
                clip.duration = std::max(clip.duration, ch.times[t]);
            }
            const size_t valueCount = csrc.sampler->output->count;
            ch.values.resize(valueCount * static_cast<size_t>(ch.width));
            for (size_t v = 0; v < valueCount; ++v) {
                float tmp[4] = {};
                cgltf_accessor_read_float(csrc.sampler->output, v, tmp, ch.width);
                for (int k = 0; k < ch.width; ++k) {
                    ch.values[v * static_cast<size_t>(ch.width) + static_cast<size_t>(k)] = tmp[k];
                }
            }
            clip.channels.push_back(std::move(ch));
        }
    }

    cgltf_free(data);

    sample(clipIndex("Armature|Idle"), 0.0f, submeshes_);
    if (submeshes_.empty() && !clips_.empty()) sample(0, 0.0f, submeshes_);
    if (submeshes_.empty()) sample(-1, 0.0f, submeshes_);
    return !submeshes_.empty();
}

int AnimatedGltfModel::clipIndex(std::string_view name) const {
    // Case-insensitive: rigs disagree on casing (Mixamo "idle" vs Quaternius "Idle").
    const auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    const auto ieq = [&](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (lc(a[i]) != lc(b[i])) return false;
        return true;
    };
    for (int i = 0; i < static_cast<int>(clips_.size()); ++i)
        if (ieq(clips_[static_cast<size_t>(i)].name, name)) return i;
    return -1;
}

float AnimatedGltfModel::clipDuration(int clip) const {
    return clip >= 0 && clip < static_cast<int>(clips_.size()) ? clips_[static_cast<size_t>(clip)].duration : 0.0f;
}

const std::string& AnimatedGltfModel::clipName(int clip) const {
    static const std::string empty;
    return clip >= 0 && clip < static_cast<int>(clips_.size()) ? clips_[static_cast<size_t>(clip)].name : empty;
}

void AnimatedGltfModel::sampleChannel(const Channel& ch, float timeSeconds, Pose& pose) const {
    if (ch.times.empty() || ch.values.empty()) return;
    if (timeSeconds <= ch.times.front()) timeSeconds = ch.times.front();
    if (timeSeconds >= ch.times.back()) timeSeconds = ch.times.back();

    size_t k0 = 0;
    while (k0 + 1 < ch.times.size() && ch.times[k0 + 1] < timeSeconds) ++k0;
    const size_t k1 = std::min(k0 + 1, ch.times.size() - 1);
    const float t0 = ch.times[k0], t1 = ch.times[k1];
    const float dt = std::max(1.0e-6f, t1 - t0);
    const float u = ch.interpolation == Interpolation::Step ? 0.0f : std::clamp((timeSeconds - t0) / dt, 0.0f, 1.0f);

    float out[4] = {};
    const bool cubic = ch.interpolation == Interpolation::CubicSpline;
    for (int c = 0; c < ch.width; ++c) {
        const float a = channelValue(ch.values, ch.width, cubic, k0, c);
        const float b = channelValue(ch.values, ch.width, cubic, k1, c);
        if (cubic) {
            out[c] = hermite(a, channelOutTangent(ch.values, ch.width, k0, c),
                             b, channelInTangent(ch.values, ch.width, k1, c), u, dt);
        } else {
            out[c] = a + (b - a) * u;
        }
    }

    if (ch.path == ChannelPath::Translation) pose.translation = { out[0], out[1], out[2] };
    else if (ch.path == ChannelPath::Scale) pose.scale = { out[0], out[1], out[2] };
    else if (ch.path == ChannelPath::Rotation) pose.rotation = normalize({ out[0], out[1], out[2], out[3] });
}

void AnimatedGltfModel::evaluateNodes(int clip, float timeSeconds, std::vector<MatC>& world) const {
    const size_t n = nodes_.size();
    std::vector<Pose> poses(n);
    std::vector<bool> animated(n, false);
    for (size_t i = 0; i < n; ++i) {
        poses[i].translation = nodes_[i].translation;
        poses[i].rotation = nodes_[i].rotation;
        poses[i].scale = nodes_[i].scale;
    }
    if (clip >= 0 && clip < static_cast<int>(clips_.size())) {
        const Clip& c = clips_[static_cast<size_t>(clip)];
        for (const Channel& ch : c.channels) {
            if (ch.node >= 0 && ch.node < static_cast<int>(n)) {
                sampleChannel(ch, timeSeconds, poses[static_cast<size_t>(ch.node)]);
                animated[static_cast<size_t>(ch.node)] = true;
            }
        }
    }

    std::vector<MatC> local(n);
    for (size_t i = 0; i < n; ++i) {
        local[i] = (nodes_[i].hasMatrix && !animated[i])
            ? nodes_[i].matrix : trs(poses[i].translation, poses[i].rotation, poses[i].scale);
    }

    world.assign(n, identity());
    std::vector<uint8_t> done(n, 0);
    auto compute = [&](auto&& self, int idx) -> MatC {
        if (idx < 0 || idx >= static_cast<int>(n)) return identity();
        if (done[static_cast<size_t>(idx)]) return world[static_cast<size_t>(idx)];
        const int parent = nodes_[static_cast<size_t>(idx)].parent;
        world[static_cast<size_t>(idx)] = parent >= 0 ? mul(self(self, parent), local[static_cast<size_t>(idx)])
                                                      : local[static_cast<size_t>(idx)];
        done[static_cast<size_t>(idx)] = 1;
        return world[static_cast<size_t>(idx)];
    };
    for (int i = 0; i < static_cast<int>(n); ++i) compute(compute, i);
}

void AnimatedGltfModel::sample(int clip, float timeSeconds, std::vector<AnimatedGltfSubmesh>& out,
                              Space space) const {
    out.clear();
    const bool worldSpace = space == Space::World;
    const auto mapPoint = [worldSpace](Vec3f p) { return worldSpace ? p : toViewmodel(p); };
    std::vector<MatC> world;
    evaluateNodes(clip, timeSeconds, world);

    out.reserve(primitives_.size());
    for (const Primitive& prim : primitives_) {
        AnimatedGltfSubmesh sm;
        sm.name = prim.name;
        sm.indices = prim.indices;
        sm.materialIndex = prim.materialIndex;
        sm.vertices.resize(prim.vertices.size());
        sm.vertexTags.resize(prim.vertices.size());

        const MatC nodeWorld = (prim.nodeIndex >= 0 && prim.nodeIndex < static_cast<int>(world.size()))
            ? world[static_cast<size_t>(prim.nodeIndex)] : identity();
        const Skin* skin = (prim.skinIndex >= 0 && prim.skinIndex < static_cast<int>(skins_.size()))
            ? &skins_[static_cast<size_t>(prim.skinIndex)] : nullptr;

        std::vector<MatC> jointMats;
        if (skin) {
            jointMats.resize(skin->joints.size(), identity());
            for (size_t ji = 0; ji < skin->joints.size(); ++ji) {
                const int n = skin->joints[ji];
                const MatC jw = (n >= 0 && n < static_cast<int>(world.size())) ? world[static_cast<size_t>(n)] : identity();
                jointMats[ji] = mul(jw, skin->inverseBind[ji]);
            }
        }

        for (size_t vi = 0; vi < prim.vertices.size(); ++vi) {
            const VertexSource& src = prim.vertices[vi];
            Vec3f p{};
            Vec3f n{};
            Vec3f t{};
            if (skin && !jointMats.empty()) {
                float totalWeight = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const float w = src.weights[k];
                    if (w <= 0.0f) continue;
                    const size_t ji = std::min(static_cast<size_t>(src.joints[k]), jointMats.size() - 1);
                    p = add3(p, scale3(transformPoint(jointMats[ji], src.pos), w));
                    n = add3(n, scale3(transformVector(jointMats[ji], src.nrm), w));
                    t = add3(t, scale3(transformVector(jointMats[ji], { src.tangent.x, src.tangent.y, src.tangent.z }), w));
                    totalWeight += w;
                }
                if (totalWeight <= 1.0e-6f) {
                    p = transformPoint(nodeWorld, src.pos);
                    n = transformVector(nodeWorld, src.nrm);
                    t = transformVector(nodeWorld, { src.tangent.x, src.tangent.y, src.tangent.z });
                }
            } else {
                p = transformPoint(nodeWorld, src.pos);
                n = transformVector(nodeWorld, src.nrm);
                t = transformVector(nodeWorld, { src.tangent.x, src.tangent.y, src.tangent.z });
            }

            StaticVertex& dst = sm.vertices[vi];
            dst.pos = mapPoint(p);
            dst.nrm = normalize3(mapPoint(n));
            const Vec3f tv = normalize3(mapPoint(t));
            dst.tangent = { tv.x, tv.y, tv.z, src.tangent.w };
            dst.uv0[0] = src.uv0[0]; dst.uv0[1] = src.uv0[1];
            dst.color = src.color;
            sm.vertexTags[vi] = src.tag;
        }
        out.push_back(std::move(sm));
    }
}

} // namespace pulse
