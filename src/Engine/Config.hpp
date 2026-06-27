#pragma once

#include <cstdint>
#include <string>

#include "Game/Tunables.hpp"
#include "Engine/SceneFrame.hpp"   // MaterialDesc, Vec4f

namespace pulse {

struct ConfigLoadResult {
    bool loaded = false;
    std::string path;
    std::string message;
};

ConfigLoadResult loadTunablesFromDisk(Tunables& tunables);

// ---------------------------------------------------------------------------
// Neon Ink Brutalism LOCKED style library (workstream W5). Loaded from
// config/pulse.style; the single source of truth for the approved palette,
// master-material ranges, shading bands, and outline widths. See
// docs/Plan_PULSE_neon_ink_brutalism.md. Phase 0 ships data + loader; rendering
// consumption (bands, outlines, grade, material repoint) lands in later phases.

struct StyleColor { float r = 0.0f, g = 0.0f, b = 0.0f; };  // sRGB 0..1

// One master material preset. Roughness of imported assets is clamped into
// [roughMin, roughMax]; reflect is the reflection-importance hint (0 = never
// reflect, 1 = hero reflective surface).
struct StyleMaterial {
    float roughMin = 0.75f, roughMax = 0.95f;
    float metallic = 0.0f;
    float emissive = 0.0f;
    float reflect  = 0.0f;
};

enum class StyleCategory : uint32_t {
    MatteArch = 0, PaintedMetal, PolishedObsidian, EmissiveCyan, EmissiveMagenta
};

// Interface palette + metrics for the in-game menus + HUD redesign. Colours are
// role-locked (cyan = player/selection only; magenta = threat; orange = elite/
// destructive); metrics are design-space px @1920x1080. See config/pulse.style
// (ui.* block) and the design handoff. Defaults are the handoff values so the UI
// is identical whether or not the config block is present.
struct UiStyle {
    // role-locked palette (mirrors pal:: in PulseGame.cpp)
    StyleColor cyan{ 0.208f, 0.910f, 0.929f }, cyanHot{ 0.412f, 1.0f, 1.0f };
    StyleColor magenta{ 1.0f, 0.180f, 0.624f }, magentaSoft{ 1.0f, 0.455f, 0.753f };
    StyleColor orange{ 0.953f, 0.627f, 0.435f }, orangeHot{ 1.0f, 0.753f, 0.541f };
    StyleColor slate{ 0.541f, 0.592f, 0.690f }, slateDim{ 0.345f, 0.376f, 0.475f }, faint{ 0.227f, 0.259f, 0.349f };
    StyleColor textHi{ 0.902f, 0.965f, 1.0f }, textHero{ 0.918f, 0.988f, 1.0f }, textMid{ 0.761f, 0.824f, 0.894f };
    StyleColor navy{ 0.047f, 0.063f, 0.125f }, deep{ 0.027f, 0.031f, 0.063f }, border{ 0.102f, 0.133f, 0.212f };
    StyleColor inkStroke{ 0.667f, 0.722f, 0.816f };
    StyleColor tierCommon{ 0.541f, 0.592f, 0.690f }, tierUncommon{ 0.337f, 0.667f, 0.980f }, tierRare{ 0.953f, 0.627f, 0.435f };
    StyleColor roomCombat{ 0.902f, 0.965f, 1.0f }, roomElite{ 1.0f, 0.753f, 0.541f };
    StyleColor roomCache{ 0.549f, 0.776f, 1.0f }, roomBoss{ 1.0f, 0.455f, 0.753f };
    // metrics (design-space px @1920x1080; consumed live, scaled per frame)
    float marginPx = 64.0f, gridPx = 8.0f;
    float cornerPx = 12.0f, cornerSmallPx = 10.0f, cornerLargePx = 14.0f;
    float strokePx = 1.5f, strokeFocusPx = 2.0f, spinePx = 3.0f;
    float barPx = 8.0f, glowRadiusPx = 18.0f, waveformStrokePx = 2.4f;
};

struct StyleConfig {
    // palette (sRGB 0..1); defaults mirror config/pulse.style.
    StyleColor shadowIndigo{ 0.035f, 0.051f, 0.133f }, shadowIndigoHi{ 0.082f, 0.098f, 0.212f };
    StyleColor envBase{ 0.204f, 0.212f, 0.369f }, envBaseHi{ 0.396f, 0.341f, 0.486f };
    StyleColor lightConcrete{ 0.502f, 0.439f, 0.573f }, lightConcreteHi{ 0.631f, 0.549f, 0.667f };
    StyleColor enemyMagenta{ 1.0f, 0.141f, 0.624f }, enemyMagentaHot{ 1.0f, 0.231f, 0.784f };
    StyleColor friendlyCyan{ 0.208f, 0.910f, 0.929f }, friendlyCyanHot{ 0.412f, 1.0f, 1.0f };
    StyleColor navAmber{ 0.953f, 0.627f, 0.435f }, navAmberHot{ 1.0f, 0.765f, 0.580f };
    StyleColor uiText{ 0.851f, 0.973f, 1.0f };
    StyleColor inkOutline{ 0.012f, 0.017f, 0.040f };
    // sky / fog (W4)
    StyleColor skyZenith{ 0.060f, 0.063f, 0.180f };
    StyleColor skyHorizon{ 0.960f, 0.700f, 0.545f };
    StyleColor fogTint{ 0.180f, 0.165f, 0.300f };
    // stylized shading bands (W1)
    float bandShadow = 0.35f, bandLit = 0.68f, bandSoftness = 0.05f;
    // ink outline widths in px at 1080p (W2)
    float outlineEnvPx = 2.0f, outlinePropPx = 2.25f, outlineEnemyPx = 3.0f, outlineInternalScale = 0.50f;
    float outlineHeroScale = 1.6f;   // viewmodel (hero) ink boldness vs environment (W2)
    // ink hatching in shadow + contact bands (doc 5); strength 0 = off
    float hatchStrength = 0.0f, hatchScale = 6.0f, hatchWidth = 0.16f, hatchFade = 22.0f;
    // master materials (W5)
    StyleMaterial matte{ 0.75f, 0.95f, 0.0f, 0.0f, 0.0f };
    StyleMaterial metal{ 0.35f, 0.70f, 0.90f, 0.0f, 0.40f };
    StyleMaterial obsidian{ 0.06f, 0.18f, 0.10f, 0.0f, 1.0f };
    StyleMaterial emissiveCyan{ 0.20f, 0.40f, 0.0f, 2.50f, 0.20f };
    StyleMaterial emissiveMagenta{ 0.20f, 0.40f, 0.0f, 2.50f, 0.20f };
    // Interface system (Neon Ink Brutalism UI redesign; pulse.style.ui block).
    // METRICS are consumed live by the HUD/menus so spacing/stroke/corner/glow
    // retune on F5; design space is 1920x1080 and screens scale them by
    // s = min(w/1920, h/1080). COLOURS mirror the constexpr pal:: namespace in
    // PulseGame.cpp (the live draw colours); they are loaded here as data for
    // reference + future runtime theming, matching this file's data-then-consume
    // convention. Defaults match the handoff exactly so a missing block is a no-op.
    UiStyle ui;
    bool loaded = false;
};

ConfigLoadResult loadStyleFromDisk(StyleConfig& style);

// Build a MaterialDesc for a master-material category, clamped to the locked
// ranges (no textures). baseColorFactor tints the albedo. Callers pass the
// result to Engine::createMaterial.
MaterialDesc styledMaterial(const StyleConfig& style, StyleCategory category,
                            Vec4f baseColorFactor = { 1, 1, 1, 1 });

} // namespace pulse
