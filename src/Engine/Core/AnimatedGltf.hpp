#pragma once

#include "Engine/SceneFrame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

struct AnimatedGltfMaterial {
    std::string name;
    std::string baseColorTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    std::string emissiveTexture;
    Vec4f baseColorFactor{ 1, 1, 1, 1 };
    Vec3f emissiveFactor{ 0, 0, 0 };
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.8f;
};

struct AnimatedGltfSubmesh {
    std::string name;
    std::vector<StaticVertex> vertices;
    std::vector<uint8_t> vertexTags;
    std::vector<uint32_t> indices;
    int materialIndex = -1;
};

class AnimatedGltfModel {
public:
    // Output coordinate convention for sample(). Viewmodel keeps the legacy
    // first-person mapping ({-x, z, -y}); World leaves the glTF scene axes
    // intact (Y-up) so the result can be oriented with a world basis() and
    // grounded on the floor (used by the skinned enemy renderer).
    enum class Space { Viewmodel, World };

    bool load(const std::string& path);

    int clipCount() const { return static_cast<int>(clips_.size()); }
    const std::vector<AnimatedGltfMaterial>& materials() const { return materials_; }
    const std::vector<AnimatedGltfSubmesh>& submeshes() const { return submeshes_; }

    int clipIndex(std::string_view name) const;
    float clipDuration(int clip) const;
    const std::string& clipName(int clip) const;

    void sample(int clip, float timeSeconds, std::vector<AnimatedGltfSubmesh>& out,
                Space space = Space::Viewmodel) const;

private:
    struct MatC {
        float m[16] = {};
    };
    struct Quat {
        float x = 0, y = 0, z = 0, w = 1;
    };
    struct Node {
        std::string name;
        int parent = -1;
        std::vector<int> children;
        bool hasMatrix = false;
        MatC matrix{};
        Vec3f translation{ 0, 0, 0 };
        Quat rotation{};
        Vec3f scale{ 1, 1, 1 };
    };
    struct Skin {
        std::vector<int> joints;
        std::vector<MatC> inverseBind;
    };
    struct VertexSource {
        Vec3f pos{};
        Vec3f nrm{ 0, 1, 0 };
        Vec4f tangent{ 1, 0, 0, 1 };
        float uv0[2] = { 0, 0 };
        Vec4f color{ 1, 1, 1, 1 };
        uint16_t joints[4] = { 0, 0, 0, 0 };
        float weights[4] = { 0, 0, 0, 0 };
        uint8_t tag = 0;
    };
    struct Primitive {
        std::string name;
        std::vector<VertexSource> vertices;
        std::vector<uint32_t> indices;
        int materialIndex = -1;
        int nodeIndex = -1;
        int skinIndex = -1;
    };
    enum class Interpolation { Linear, Step, CubicSpline };
    enum class ChannelPath { Translation, Rotation, Scale };
    struct Channel {
        int node = -1;
        ChannelPath path = ChannelPath::Translation;
        Interpolation interpolation = Interpolation::Linear;
        std::vector<float> times;
        std::vector<float> values;
        int width = 3;
    };
    struct Clip {
        std::string name;
        float duration = 0.0f;
        std::vector<Channel> channels;
    };
    struct Pose {
        Vec3f translation{};
        Quat rotation{};
        Vec3f scale{ 1, 1, 1 };
    };

    static MatC identity();
    static MatC mul(const MatC& a, const MatC& b);
    static MatC trs(Vec3f t, Quat r, Vec3f s);
    static Vec3f transformPoint(const MatC& m, Vec3f p);
    static Vec3f transformVector(const MatC& m, Vec3f v);
    static Vec3f toViewmodel(Vec3f p);
    static Quat normalize(Quat q);
    static Quat nlerp(Quat a, Quat b, float t);

    void evaluateNodes(int clip, float timeSeconds, std::vector<MatC>& world) const;
    void sampleChannel(const Channel& ch, float timeSeconds, Pose& pose) const;

    std::vector<Node> nodes_;
    std::vector<Skin> skins_;
    std::vector<Primitive> primitives_;
    std::vector<Clip> clips_;
    std::vector<AnimatedGltfMaterial> materials_;
    std::vector<AnimatedGltfSubmesh> submeshes_;
};

} // namespace pulse
