#include <iostream>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cstdio>
#include <exception>
#include <string>
#include <stdexcept>
#include <windows.h>

#include "Engine/Audio.hpp"
#include "Engine/Input.hpp"
#include "Engine/PlatformWin32.hpp"
#include "Engine/Renderer.hpp"
#include "Game/PulseGame.hpp"

namespace {

enum class ScriptedCapture {
    None,
    Movement,
    Aim,
    Tracer
};

void setWorkingDirectoryToExecutableFolder() {
    std::array<char, 32768> pathBuffer{};
    const DWORD length = GetModuleFileNameA(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (length == 0 || length >= pathBuffer.size()) {
        throw std::runtime_error("Could not resolve executable path");
    }

    const std::filesystem::path exePath(pathBuffer.data());
    const std::filesystem::path exeDir = exePath.parent_path();
    if (exeDir.empty()) {
        throw std::runtime_error("Could not resolve executable folder");
    }
    std::filesystem::current_path(exeDir);
}

} // namespace

int runPulse(int argc, char** argv) {
    pulse::PulseGame game;

    bool smoke = false;
    bool pose = false;
    bool weaponPreview = false;
    float botTestSeconds = 0.0f;
    float recordFps = 6.0f;
    ScriptedCapture scriptedCapture = ScriptedCapture::None;
    float tracerPitch = 0.0f;
    int tracerStep = 0;
    std::string screenshotPath;
    std::string recordDir;
    std::string captureLabel;
    std::string musicWavPath;
    float musicWavSeconds = 0.0f;
    std::string sfxWavPath;
    float sfxWavSeconds = 0.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke") {
            smoke = true;
        } else if (arg == "--pose") {
            pose = true;
        } else if (arg == "--weapon-test") {
            weaponPreview = true;
        } else if (arg == "--bot-test" && i + 1 < argc) {
            botTestSeconds = std::stof(argv[++i]);
        } else if (arg == "--movement-test") {
            scriptedCapture = ScriptedCapture::Movement;
        } else if (arg == "--aim-test") {
            scriptedCapture = ScriptedCapture::Aim;
        } else if (arg == "--tracer-test") {
            scriptedCapture = ScriptedCapture::Tracer;
        } else if (arg == "--pitch" && i + 1 < argc) {
            tracerPitch = std::stof(argv[++i]);
        } else if (arg == "--tracer-step" && i + 1 < argc) {
            tracerStep = std::stoi(argv[++i]);
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else if (arg == "--record-dir" && i + 1 < argc) {
            recordDir = argv[++i];
        } else if (arg == "--record-fps" && i + 1 < argc) {
            recordFps = std::stof(argv[++i]);
        } else if (arg == "--capture-label" && i + 1 < argc) {
            captureLabel = argv[++i];
        } else if (arg == "--render-music" && i + 2 < argc) {
            musicWavPath = argv[++i];
            musicWavSeconds = std::stof(argv[++i]);
        } else if (arg == "--render-sfx" && i + 2 < argc) {
            sfxWavPath = argv[++i];
            sfxWavSeconds = std::stof(argv[++i]);
        }
    }

    if (!musicWavPath.empty()) {
        pulse::AudioSystem audio;
        const pulse::Tunables& tn = game.tunables();
        const bool ok = audio.renderMusicToWav(musicWavPath, std::max(1.0f, musicWavSeconds),
                                               tn.technoBpm, tn.technoBaseVolume, 0.0f);
        std::cout << (ok ? "Music render ok\n" : "Music render failed\n");
        return ok ? 0 : 2;
    }

    if (!sfxWavPath.empty()) {
        pulse::AudioSystem audio;
        const pulse::Tunables& tn = game.tunables();
        const bool ok = audio.renderShotsToWav(sfxWavPath, std::max(0.5f, sfxWavSeconds),
                                               pulse::SoundEventType::Fire, tn.weaponFireRate, 0.95f);
        std::cout << (ok ? "Sfx render ok\n" : "Sfx render failed\n");
        return ok ? 0 : 2;
    }

    if (smoke || pose || weaponPreview || botTestSeconds > 0.0f || scriptedCapture != ScriptedCapture::None || !screenshotPath.empty() || !recordDir.empty()) {
        pulse::Renderer renderer;
        pulse::AudioSystem audio;
        pulse::InputState input;
        renderer.resize(game.tunables().windowWidth, game.tunables().windowHeight);

        if (weaponPreview) {
            game.debugRenderWeaponPreview(renderer);
            if (!captureLabel.empty()) {
                renderer.fillRect(0, 0, renderer.width(), 40, pulse::rgba(0, 0, 0, 150));
                renderer.drawText(16, 12, "WEAPON " + captureLabel, pulse::rgb(255, 232, 94), 3);
            }
            if (!screenshotPath.empty() && !renderer.saveBmp(screenshotPath)) {
                std::cerr << "Could not write screenshot: " << screenshotPath << "\n";
                return 2;
            }
            std::cout << "Weapon preview render ok\n";
            return 0;
        }

        if (pose) {
            // Static close-up of the enemy drones for model inspection.
            game.debugPose();
            // --pose-step N shatters the drones and advances N frames so the
            // death debris can be inspected mid-flight.
            int poseStep = 0;
            for (int i = 1; i < argc; ++i) {
                if (std::string(argv[i]) == "--pose-step" && i + 1 < argc) {
                    poseStep = std::stoi(argv[i + 1]);
                }
            }
            if (poseStep > 0) {
                game.debugKillAll();
                for (int s = 0; s < poseStep; ++s) {
                    game.update(input, audio, 1.0f / 60.0f, renderer.width(), renderer.height());
                }
            }
            // --pitch X fires a wall tracer at pitch X to check crosshair tracking.
            int tracerStep = 0;
            bool firedTracer = false;
            for (int i = 1; i < argc; ++i) {
                if (std::string(argv[i]) == "--tracer-step" && i + 1 < argc) {
                    tracerStep = std::stoi(argv[i + 1]);
                }
            }
            for (int i = 1; i < argc; ++i) {
                if (std::string(argv[i]) == "--pitch" && i + 1 < argc) {
                    game.debugBeginScriptedCapture();
                    game.debugFire(audio, std::stof(argv[i + 1]));
                    firedTracer = true;
                }
            }
            if (firedTracer && tracerStep > 0) {
                const int steps = std::min(tracerStep, 20);
                for (int s = 0; s < steps; ++s) {
                    game.update(input, audio, 1.0f / 60.0f, renderer.width(), renderer.height());
                    input.endFrame();
                }
            }
            game.render(renderer);
            if (!captureLabel.empty()) {
                renderer.fillRect(0, 0, renderer.width(), 40, pulse::rgba(0, 0, 0, 150));
                renderer.drawText(16, 12, "POSE " + captureLabel, pulse::rgb(255, 232, 94), 3);
            }
            if (!screenshotPath.empty() && !renderer.saveBmp(screenshotPath)) {
                std::cerr << "Could not write screenshot: " << screenshotPath << "\n";
                return 2;
            }
            std::cout << "Pose render ok\n";
            return 0;
        }
        std::cout << "Weapon asset: assets/models/pulse_carbine_viewmodel.obj vertices=" << game.weaponMeshVertexCount()
                  << " triangles=" << game.weaponMeshTriangleCount()
                  << " texture=" << game.weaponTextureWidth() << "x" << game.weaponTextureHeight()
                  << "\n";
        if (scriptedCapture != ScriptedCapture::None) {
            game.debugBeginScriptedCapture();
            if (scriptedCapture == ScriptedCapture::Tracer) {
                recordFps = std::max(recordFps, 60.0f);
                game.debugFire(audio, tracerPitch);
                for (int s = 0; s < std::min(tracerStep, 20); ++s) {
                    game.update(input, audio, 1.0f / 60.0f, renderer.width(), renderer.height());
                    input.endFrame();
                }
            }
        }
        const auto drawCaptureOverlay = [&](float elapsedSeconds) {
            if (captureLabel.empty()) {
                return;
            }
            const std::string line1 = "RUN " + captureLabel;
            const std::string line2 = "PULSE CARBINE " + std::to_string(game.weaponMeshVertexCount()) + "V " +
                std::to_string(game.weaponMeshTriangleCount()) + "T TEX " +
                std::to_string(game.weaponTextureWidth()) + "X" + std::to_string(game.weaponTextureHeight());
            char timeText[32]{};
            std::snprintf(timeText, sizeof(timeText), "T %.2F", elapsedSeconds);

            renderer.fillRect(0, 0, renderer.width(), 76, pulse::rgba(0, 0, 0, 170));
            renderer.drawText(18, 14, line1, pulse::rgb(255, 232, 94), 4);
            renderer.drawText(20, 52, line2 + " " + timeText, pulse::rgb(160, 230, 255), 2);
        };
        if (!recordDir.empty()) {
            std::filesystem::create_directories(recordDir);
        }
        recordFps = std::max(1.0f, recordFps);
        float nextRecordTime = 0.0f;
        int recordFrame = 0;
        const auto buildScriptedInput = [&](float elapsedSeconds, int frame) {
            input.keyDown.fill(false);
            input.mouseDown.fill(false);
            input.mouseDeltaX = 0;
            input.mouseDeltaY = 0;
            if (scriptedCapture == ScriptedCapture::Movement) {
                if (elapsedSeconds < 2.4f) {
                    input.keyDown['W'] = true;
                } else if (elapsedSeconds < 4.2f) {
                    input.keyDown['W'] = true;
                    input.keyDown['D'] = true;
                } else if (elapsedSeconds < 5.6f) {
                    input.keyDown['A'] = true;
                } else {
                    input.keyDown['W'] = true;
                    input.keyDown['D'] = true;
                }
                if (frame == 60 || frame == 210) {
                    input.keyPressed[VK_SHIFT] = true;
                    input.keyDown[VK_SHIFT] = true;
                }
                if (frame == 132 || frame == 300) {
                    input.keyPressed[VK_SPACE] = true;
                    input.keyDown[VK_SPACE] = true;
                }
                if (frame > 180 && frame < 330) {
                    input.mouseDeltaX = 5;
                }
            } else if (scriptedCapture == ScriptedCapture::Aim) {
                const int phase = frame % 180;
                input.mouseDeltaX = phase < 90 ? 7 : -7;
                input.mouseDeltaY = (frame % 120) < 60 ? -2 : 2;
                if ((frame >= 35 && frame < 95) || (frame >= 150 && frame < 235) || (frame >= 285 && frame < 340)) {
                    input.mouseDown[0] = true;
                    if (frame == 35 || frame == 150 || frame == 285) {
                        input.mousePressed[0] = true;
                    }
                }
                if (elapsedSeconds > 2.0f && elapsedSeconds < 4.0f) {
                    input.keyDown['D'] = true;
                }
            }
        };
        const int frames = botTestSeconds > 0.0f
            ? static_cast<int>(botTestSeconds * 60.0f)
            : scriptedCapture == ScriptedCapture::Movement
                ? 420
            : scriptedCapture == ScriptedCapture::Aim
                ? 360
            : scriptedCapture == ScriptedCapture::Tracer
                ? 60
            : 120;
        for (int i = 0; i < frames; ++i) {
            const float elapsed = static_cast<float>(i) / 60.0f;
            if (botTestSeconds > 0.0f) {
                game.buildBotInput(input, elapsed);
            } else if (scriptedCapture != ScriptedCapture::None) {
                buildScriptedInput(elapsed, i);
            }
            game.update(input, audio, 1.0f / 60.0f, renderer.width(), renderer.height());
            game.render(renderer);
            drawCaptureOverlay(elapsed);
            if (!recordDir.empty() && elapsed + 0.0001f >= nextRecordTime) {
                char fileName[64]{};
                std::snprintf(fileName, sizeof(fileName), "frame_%04d.bmp", recordFrame++);
                const std::filesystem::path framePath = std::filesystem::path(recordDir) / fileName;
                if (!renderer.saveBmp(framePath.string())) {
                    std::cerr << "Could not write frame: " << framePath.string() << "\n";
                    return 4;
                }
                nextRecordTime += 1.0f / recordFps;
            }
            input.endFrame();
        }
        if (!screenshotPath.empty() && !captureLabel.empty()) {
            drawCaptureOverlay(static_cast<float>(frames - 1) / 60.0f);
        }
        if (!screenshotPath.empty() && !renderer.saveBmp(screenshotPath)) {
            std::cerr << "Could not write screenshot: " << screenshotPath << "\n";
            return 2;
        }
        if (botTestSeconds > 0.0f) {
            std::cout << "Bot playtest: "
                      << "seconds=" << botTestSeconds
                      << " score=" << game.score()
                      << " best=" << game.bestScore()
                      << " hp=" << game.playerHp()
                      << " shield=" << game.playerShield()
                      << " enemies=" << game.activeEnemyCount()
                      << "\n";
            if (game.score() < 2 || game.playerHp() <= 0) {
                std::cerr << "Bot playtest failed thresholds\n";
                return 3;
            }
        }
        std::cout << "Native PULSE smoke ok\n";
        return 0;
    }

    pulse::Win32App app;
    return app.run(game, game.tunables().windowWidth, game.tunables().windowHeight);
}

int main(int argc, char** argv) {
    try {
        setWorkingDirectoryToExecutableFolder();
        return runPulse(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << "\n";
        return 99;
    }
}
