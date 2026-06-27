#pragma once

#include "Engine/Core/Mat.hpp"

#include <cstdint>
#include <span>

namespace pulse {

// Opaque handles. 0 == Invalid, so a zero-init is never a valid resource.
enum class MeshHandle     : uint32_t { Invalid = 0 };
enum class TextureHandle  : uint32_t { Invalid = 0 };
enum class MaterialHandle : uint32_t { Invalid = 0 };

// CPU authoring input for a mesh; the engine uploads it to GPU buffers.
struct StaticVertex {
    Vec3f pos;
    Vec3f nrm;
    Vec4f tangent;
    float uv0[2] = { 0, 0 };
    float uv1[2] = { 0, 0 };
    Vec4f color = { 1, 1, 1, 1 };
};

struct MeshData {
    std::span<const StaticVertex> vertices;
    std::span<const uint32_t>     indices;
};

// Raw 8-bit texture pixels (RGBA, one byte per channel). The asset pipeline adds
// BCn DDS loading later; this covers the game's procedurally generated textures.
struct TextureData {
    uint32_t    width = 0;
    uint32_t    height = 0;
    const void* rgba = nullptr;   // width*height*4 bytes
    bool        srgb = false;     // albedo/emissive maps: decode to linear on sample
};

struct MaterialDesc {
    TextureHandle baseColor = TextureHandle::Invalid;
    Vec4f         baseColorFactor = { 1, 1, 1, 1 };
    float         emissive = 0.0f;
    float         metallic = 0.0f;     // 0 = dielectric, 1 = metal (fallback if no ORM map)
    float         roughness = 0.8f;    // 0 = mirror, 1 = fully rough (fallback if no ORM map)
    TextureHandle normal = TextureHandle::Invalid;   // tangent-space (OpenGL +Y)
    TextureHandle orm = TextureHandle::Invalid;      // R=AO G=roughness B=metallic
    TextureHandle emissiveTex = TextureHandle::Invalid;  // per-texel emissive (e.g. neon trace maps)
    float         emissiveTexStrength = 1.0f;        // HDR multiplier on the emissive map (>1 to bloom)
    float         uvScale = 1.0f;                    // tiling multiplier for uv0
    float         metalScale = 1.0f;  // scales metalness (ORM or scalar); < 1 de-metals
    float         roughBoost = 0.0f;  // added to roughness then clamped; > 0 roughens
    uint32_t      styleFlags = 0;     // W5 master-material category + masks (see Config.hpp styledMaterial)
};

// The per-frame description the game submits. The engine owns everything about
// how this becomes pixels.
struct Camera {
    Vec3f position;
    float yaw = 0, pitch = 0, roll = 0;
    float fovDeg = 70.0f;
    float viewmodelFovDeg = 70.0f;
    float nearZ = 0.04f;
    float farZ = 80.0f;
};

struct SunLight {
    Vec3f direction = { -0.4f, -0.82f, -0.36f };   // direction the light travels
    Vec3f color = { 1, 1, 1 };
    float intensity = 1.0f;
    float ambient = 0.18f;
};

// Point light: the substance of the arena's atmosphere (moody local pools of
// colour + emissive accents). Clustered later; M1 accumulates them in the resolve.
struct LocalLight {
    Vec3f position = { 0, 0, 0 };
    Vec3f color = { 1, 1, 1 };
    float intensity = 1.0f;
    float radius = 5.0f;       // falloff reaches zero here
};

struct MeshInstance {
    uint64_t       id = 0;                 // stable across frames (future motion vectors)
    MeshHandle     mesh = MeshHandle::Invalid;
    MaterialHandle material = MaterialHandle::Invalid;
    Mat4           transform = Mat4::identity();
    bool           cameraSpace = false;    // first-person weapon / hands
    Vec4f          tint = { 1, 1, 1, 1 };  // per-instance color multiplier (flashes, etc.)
    float          emissiveAdd = 0.0f;     // added to the material emissive
    Vec3f          rimColor = { 0, 0, 0 }; // neon-ink fresnel rim emissive (HDR; 0 = none)
    float          rimPower = 0.0f;        // rim falloff exponent (0 = rim off)
};

// A projected decal (bullet mark, scorch). Deferred: projected onto the gbuffer in
// the decal pass, modifying albedo before lighting. The pattern is generated in the
// shader from kind (no decal texture atlas needed). Box = center +- the three axes.
struct Decal {
    Vec3f    center;
    float    halfDepth = 0.12f;     // projection thickness along normal
    Vec3f    normal = { 0, 1, 0 };  // surface/projection axis
    float    halfWidth = 0.25f;
    Vec3f    tangent = { 1, 0, 0 };  // in-plane U axis (bitangent = normal x tangent)
    float    halfHeight = 0.25f;
    Vec3f    color = { 0.04f, 0.035f, 0.03f };
    float    alpha = 0.85f;
    uint32_t kind = 0;              // 0 = bullet mark, 1 = scorch
    float    pad0 = 0, pad1 = 0, pad2 = 0;
};

// A world-space particle billboard (muzzle spark, impact ember, explosion mote).
// The game simulates the pool on the CPU; the engine renders camera-facing additive
// sprites composited into the HDR scene before bloom, so bright sparks bloom.
struct Particle {
    Vec3f center;
    float size = 0.05f;
    Vec3f color = { 1, 0.7f, 0.3f };
    float emissive = 2.0f;     // color * emissive = HDR radiance
    Vec3f velocity = { 0, 0, 0 }; // world-space; the billboard stretches along its screen projection
    float stretch = 0.0f;      // 0 = round; >0 elongates into a motion streak, scaled by screen speed
};

// A screen-space heat-haze source: the scene is refracted (UV-warped) in a soft radius around
// this world point, with an animated shimmer. Submitted by the game for energy orbs in flight
// and brief expanding impact shockwaves; an empty list makes the blit a plain copy (no warp).
struct HeatSource {
    Vec3f center;
    float radius = 0.5f;     // world-ish size; the blit scales it by depth to a screen radius
    float strength = 0.01f;  // peak UV displacement at the centre (0 = none)
};

struct PostParams {
    float exposure = 1.0f;
    // Distance + volumetric fog extinction (fades toward clearColor). The dark indoor arenas
    // want a moody ~0.035; open outdoor scenes want near-clear air (e.g. 0.006) so the player
    // can see across the arena. Default keeps the original look for callers that do not set it.
    float fogDensity = 0.035f;
    // W3 (Neon Ink Brutalism): runtime-tunable post knobs. Defaults reproduce the prior
    // hard-coded look EXACTLY (no behavior change); later phases restrain bloom, lock the
    // palette grade, and drop chromatic aberration in gameplay (set caScale = 0).
    float bloomThreshold = 1.0f;   // HDR stops where bloom begins (raise to preserve shapes)
    float bloomKnee      = 0.6f;   // soft-threshold knee width
    float bloomIntensity = 0.55f;  // additive bloom strength at composite
    float sharpen        = 0.20f;  // post-reconstruction unsharp amount
    float vignette       = 0.34f;  // corner darkening strength
    float grain          = 0.022f; // film-grain amplitude
    float caScale        = 1.0f;   // chromatic-aberration multiplier (0 = off)
    // W3 palette-lock grade (Neon Ink Brutalism). Defaults (env == neon sat, gain 0)
    // reproduce the prior uniform 1.18 saturation lift exactly; the game pushes a
    // selective grade that keeps cyan/magenta vivid and restrains the environment.
    float gradeEnvSat    = 1.18f;  // environment saturation (< 1 restrains the world)
    float gradeNeonSat   = 1.18f;  // cyan/magenta saturation (> 1 protects/boosts neon)
    float gradeNeonGain  = 0.0f;   // neon-detection strength (0 = uniform saturation)
    // M7: a final frame-wide color multiplier on the graded image. Default {1,1,1} = no change;
    // the game sets it PER BIOME so each sector reads as a distinct place (the decisive at-a-glance
    // cue, since the shared sci-fi tileset dominates over the per-biome materials/lighting).
    Vec3f gradeTint      = { 1.0f, 1.0f, 1.0f };
};

// W1/W4 (Neon Ink Brutalism): renderer-side art-direction params the deferred path
// consumes. Defaults are INERT (stylize = 0 -> smooth PBR diffuse, skyStrength = 0 ->
// current procedural sky), so a caller that does not set this changes nothing. The
// game fills it from the locked style library (config/pulse.style).
struct StyleFrame {
    Vec3f skyZenith    = { 0.060f, 0.063f, 0.180f };  // deep indigo overhead (W4; linear)
    Vec3f skyHorizon   = { 0.960f, 0.700f, 0.545f };  // peach/coral horizon (W4; linear)
    float bandShadow   = 0.33f;   // NdotL shadow->midtone threshold (W1)
    float bandLit      = 0.66f;   // NdotL midtone->lit threshold (W1)
    float bandSoftness = 0.08f;   // smoothstep transition width (W1)
    float stylize      = 0.0f;    // 0 = smooth PBR diffuse; 1 = full 3-band quantization
    float skyStrength  = 0.0f;    // 0 = current sky; 1 = illustrated zenith/horizon (W4)
    // Ink outlines (W2): blue-black screen-space lines from depth + normal breaks.
    Vec3f inkOutline        = { 0.012f, 0.017f, 0.040f };  // near-black blue ink (bold, linear)
    float outlineThickness  = 2.0f;    // sample offset in px at the render resolution
    float outlineDepthSense = 0.65f;   // silhouette sensitivity (bolder)
    float outlineNormalSense= 0.85f;   // interior-crease sensitivity
    float outlineStrength   = 0.0f;    // 0 = no outlines (inert default); game enables it
    float outlineHeroScale  = 1.0f;    // W2: thicker/bolder ink on viewmodel (hero) pixels (1 = uniform)
    // Ink hatching (W2 / doc 5): sparse world-anchored strokes in the shadow + contact
    // bands, cross-hatched in the deepest shadow, faded with view distance so they never
    // swim with the camera or shimmer at range. hatchStrength = 0 -> no hatching (inert).
    float hatchStrength = 0.0f;   // peak ink darkening in deep shadow (0 = off)
    float hatchScale    = 6.0f;   // world-space stroke frequency (strokes per metre-ish)
    float hatchWidth    = 0.16f;  // stroke thickness (0..0.5 of a cell; smaller = finer)
    float hatchFade     = 22.0f;  // view distance (metres) where hatching fades to nothing
};

// One screen-space HUD vertex (built by UiDrawList; rgba is R|G<<8|B<<16|A<<24).
struct UiVertex {
    float    x = 0, y = 0;     // screen pixels
    float    u = 0, v = 0;     // font atlas
    uint32_t rgba = 0xFFFFFFFFu;
};

struct SceneFrame {
    Camera                        camera;
    SunLight                      sun;
    std::span<const LocalLight>   lights;   // point lights, accumulated in the resolve
    std::span<const MeshInstance> instances;
    std::span<const Decal>        decals;   // projected onto the gbuffer before lighting
    std::span<const Particle>     particles; // additive billboards composited into HDR
    std::span<const Particle>     smoke;     // alpha-blended dark shadow-smoke billboards (enemy aura)
    std::span<const HeatSource>   heat;      // scene heat-haze refraction (orbs + impact shocks)
    PostParams                    post;
    StyleFrame                    style;    // Neon Ink Brutalism art-direction params (W1/W4)
    Vec3f                         clearColor = { 0.02f, 0.03f, 0.05f };
    std::span<const UiVertex>     ui;       // GPU HUD (triangle list), drawn after tonemap
};

} // namespace pulse
