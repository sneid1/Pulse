#pragma once

#include <string>

namespace pulse {

// User-facing options persisted to %LOCALAPPDATA%\Pulse\settings.cfg (mirrors Meta's
// profile path). These are the player's front-end choices (volumes, FOV, sensitivity,
// comfort), kept separate from the dev-tuning file config/pulse.tuning. Every default
// is identity (a no-op): any code path that never loads settings behaves exactly as it
// did before this module existed, so headless/sim/capture runs are unaffected.
struct Settings {
    float masterVolume = 1.0f;   // 0..1 overall output gain
    float sfxVolume    = 1.0f;   // 0..1 sound-effects bus
    float musicVolume  = 1.0f;   // 0..1 music bus
    // v4 player mix / accessibility options (S2). Defaults are the shipping v4 mix (Strong duck,
    // stereo, full intensity, no readability boost); all are no-ops for the v3 music path.
    float musicDuckDepth = 1.0f;       // SFX-over-music duck depth: 0 Off, 0.5 Subtle, 1 Strong
    bool  monoAudio = false;           // sum L/R to mono at the output (accessibility)
    bool  reducedIntensityAudio = false; // cap the duress submerge + overpulse/enrage loudness
    bool  combatReadability = false;   // lift enemy telegraph + confirm cues for readability
    float fovDegrees   = 95.0f;  // 70..110 camera FOV (seeded from config on first run)
    float sensitivity  = 1.0f;   // 0.25..3.0 multiplier on the config mouse sensitivity
    float shakeScale   = 1.0f;   // 0..1.5 screen-shake intensity (comfort / accessibility)
    float textScale    = 1.0f;   // 0.85..1.30 menu/HUD text-size preference
    float hudScale     = 1.0f;   // 0.85..1.20 HUD sizing preference (UI option, staged)
    bool  invertY      = false;  // invert vertical mouse-look
    bool  reduceFlashes = false; // accessibility: suppress bright combat flashes (e.g. the Pulse loss flash)
    bool  reduceMotion  = false; // accessibility: freeze non-essential UI animation (Pulse meter head pulse)
    bool  reduceBloom   = false; // accessibility: request reduced UI/combat bloom intensity
    bool  highContrast  = false; // accessibility: high-contrast UI palette preference
    bool  toggleAim     = false; // gameplay/accessibility: held aim-style inputs may become toggles
    bool  vsync        = true;   // reserved: present interval (not yet wired to the swapchain)
    // Window presentation: 0 = Windowed (captioned), 1 = Borderless fullscreen (covers the
    // monitor, no title bar). Defaults to borderless so the game launches fullscreen-windowed.
    // Only the live windowed app reads this; headless capture never creates a window, so this
    // does not affect sim/capture runs.
    int   displayMode  = 1;
    int   colorblindPreset = 0;  // 0 off, 1 deuteranopia, 2 protanopia, 3 tritanopia
    int   reticleStyle = 0;      // 0 dynamic cross, 1 static cross, 2 dot
    // W6 graphics-quality ladder (doc 14): 0 = Low, 1 = Medium, 2 = High, 3 = Ultra.
    // Low/Medium run the raster tier (Low also drops SSGI + SSR); High/Ultra request the
    // RT tier (auto-falls-back to raster on a non-RT GPU). The art style holds at every
    // preset. Read at startup to seed the engine; --quality on the CLI overrides it.
    int   graphicsQuality = 2;

    // Path under %LOCALAPPDATA%\Pulse (working-dir fallback when the env var is absent).
    static std::string savePath();
    // Overlay any saved values onto the current fields (defaults preserved when a key is
    // missing or unparsable, values clamped to range). Returns true if a file was read.
    bool load();
    void save() const;
};

} // namespace pulse
