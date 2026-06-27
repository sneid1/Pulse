#include "Game/Settings.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace pulse {
namespace {

std::string trim(const std::string& v) {
    size_t a = 0, b = v.size();
    while (a < b && std::isspace(static_cast<unsigned char>(v[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(v[b - 1]))) --b;
    return v.substr(a, b - a);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool parseBool(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace

std::string Settings::savePath() {
    // %LOCALAPPDATA%\Pulse\settings.cfg, falling back to the working dir if the env var
    // is missing (headless/CI). Directory is created on save.
    const char* base = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir = base ? std::filesystem::path(base) / "Pulse" : std::filesystem::path(".");
    return (dir / "settings.cfg").string();
}

bool Settings::load() {
    std::ifstream f(savePath());
    if (!f.good()) return false;
    std::string line;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        try {
            if (key == "master") masterVolume = clampf(std::stof(val), 0.0f, 1.0f);
            else if (key == "sfx") sfxVolume = clampf(std::stof(val), 0.0f, 1.0f);
            else if (key == "music") musicVolume = clampf(std::stof(val), 0.0f, 1.0f);
            else if (key == "music_duck") musicDuckDepth = clampf(std::stof(val), 0.0f, 1.0f);
            else if (key == "mono_audio") monoAudio = parseBool(val);
            else if (key == "reduced_intensity_audio") reducedIntensityAudio = parseBool(val);
            else if (key == "combat_readability") combatReadability = parseBool(val);
            else if (key == "fov") fovDegrees = clampf(std::stof(val), 70.0f, 110.0f);
            else if (key == "sensitivity") sensitivity = clampf(std::stof(val), 0.25f, 3.0f);
            else if (key == "shake") shakeScale = clampf(std::stof(val), 0.0f, 1.5f);
            else if (key == "text_scale") textScale = clampf(std::stof(val), 0.85f, 1.30f);
            else if (key == "hud_scale") hudScale = clampf(std::stof(val), 0.85f, 1.20f);
            else if (key == "invert_y") invertY = parseBool(val);
            else if (key == "reduce_flashes") reduceFlashes = parseBool(val);
            else if (key == "reduce_motion") reduceMotion = parseBool(val);
            else if (key == "reduce_bloom") reduceBloom = parseBool(val);
            else if (key == "high_contrast") highContrast = parseBool(val);
            else if (key == "toggle_aim") toggleAim = parseBool(val);
            else if (key == "vsync") vsync = parseBool(val);
            else if (key == "display_mode") displayMode = static_cast<int>(clampf(std::stof(val), 0.0f, 1.0f));
            else if (key == "colorblind") colorblindPreset = static_cast<int>(clampf(std::stof(val), 0.0f, 3.0f));
            else if (key == "reticle") reticleStyle = static_cast<int>(clampf(std::stof(val), 0.0f, 2.0f));
            else if (key == "quality") graphicsQuality = static_cast<int>(clampf(std::stof(val), 0.0f, 3.0f));
        } catch (...) {
            // keep the current default on a malformed value
        }
    }
    return true;
}

void Settings::save() const {
    const std::string path = savePath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f.good()) return;
    f << "# PULSE user settings (front-end options). Edited in-game via Options.\n";
    f << "master=" << masterVolume << "\n";
    f << "sfx=" << sfxVolume << "\n";
    f << "music=" << musicVolume << "\n";
    f << "music_duck=" << musicDuckDepth << "\n";
    f << "mono_audio=" << (monoAudio ? 1 : 0) << "\n";
    f << "reduced_intensity_audio=" << (reducedIntensityAudio ? 1 : 0) << "\n";
    f << "combat_readability=" << (combatReadability ? 1 : 0) << "\n";
    f << "fov=" << fovDegrees << "\n";
    f << "sensitivity=" << sensitivity << "\n";
    f << "shake=" << shakeScale << "\n";
    f << "text_scale=" << textScale << "\n";
    f << "hud_scale=" << hudScale << "\n";
    f << "invert_y=" << (invertY ? 1 : 0) << "\n";
    f << "reduce_flashes=" << (reduceFlashes ? 1 : 0) << "\n";
    f << "reduce_motion=" << (reduceMotion ? 1 : 0) << "\n";
    f << "reduce_bloom=" << (reduceBloom ? 1 : 0) << "\n";
    f << "high_contrast=" << (highContrast ? 1 : 0) << "\n";
    f << "toggle_aim=" << (toggleAim ? 1 : 0) << "\n";
    f << "vsync=" << (vsync ? 1 : 0) << "\n";
    f << "display_mode=" << displayMode << "\n";
    f << "colorblind=" << colorblindPreset << "\n";
    f << "reticle=" << reticleStyle << "\n";
    f << "quality=" << graphicsQuality << "\n";
}

} // namespace pulse
