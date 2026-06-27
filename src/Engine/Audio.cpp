#include "Engine/Audio.hpp"
#include "Engine/Core/Log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

// stb_vorbis (vendored) for the OGG music bed / SFX. Declarations only here; the
// implementation lives in its own TU (StbVorbisImpl.cpp) so its internal macros do
// not leak into this file.
#pragma warning(push, 0)
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#pragma warning(pop)

namespace pulse {

namespace {

constexpr float kPi = 3.14159265358979f;

float fastNoise(int n) {
    uint32_t x = static_cast<uint32_t>(n);
    x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
    return (static_cast<float>(x & 0xFFFFu) / 32767.5f) - 1.0f;
}

float sine(double t, float hz) {
    return std::sin(static_cast<float>(2.0 * kPi * hz * t));
}

float saw(double t, float hz) {
    const double p = std::fmod(t * hz, 1.0);
    return static_cast<float>(p * 2.0 - 1.0);
}

float pulse(double t, float hz, float width) {
    const double p = std::fmod(t * hz, 1.0);
    return p < static_cast<double>(width) ? 1.0f : -1.0f;
}

float envExp(float t, float speed) {
    return std::exp(-t * speed);
}

float softClip(float x, float drive) {
    return std::tanh(x * drive);
}

float voiceLengthSeconds(SoundEventType type) {
    switch (type) {
        case SoundEventType::Fire:        return 0.22f;
        case SoundEventType::Hit:         return 0.08f;
        case SoundEventType::Kill:        return 0.24f;
        case SoundEventType::Hurt:        return 0.20f;
        case SoundEventType::Dash:        return 0.15f;
        case SoundEventType::DryFire:     return 0.07f;
        case SoundEventType::ReloadStart: return 0.46f;
        case SoundEventType::ReloadEnd:   return 0.52f;
        case SoundEventType::Pickup:      return 0.18f;
        case SoundEventType::Config:      return 0.12f;
    }
    return 0.1f;
}

uint32_t voiceLengthSamples(SoundEventType type, uint32_t sr) {
    return static_cast<uint32_t>(voiceLengthSeconds(type) * sr);
}

// One mono sample for a voice, given its progress. This is the SneidGame palette:
// saturated, low-heavy, and mechanical, then baked into PULSE's sample bank.
float voiceSample(SoundEventType type, uint32_t age, uint32_t len, uint32_t& seed, uint32_t sr) {
    (void)seed;
    (void)len;
    const float local = static_cast<float>(age) / static_cast<float>(sr);
    const int sampleIndex = static_cast<int>(age);
    float out = 0.0f;
    switch (type) {
        case SoundEventType::Fire: {
            // Layered gunshot: a sharp broadband crack, a punchy low-mid body with a
            // fast downward pitch drop, a sub thump, and a short report tail. Tuned to
            // read as a chunky rifle instead of a thin synth blip.
            const float crackE = envExp(local, 300.0f);  // ~3ms transient
            const float bodyE  = envExp(local, 40.0f);    // punch
            const float subE   = envExp(local, 24.0f);    // low thump
            const float tailE  = envExp(local, 13.0f);    // report tail
            const float bodyHz = 68.0f + 250.0f * envExp(local, 55.0f);
            const float body = softClip(sine(local, bodyHz) * 1.9f, 2.6f) * bodyE;
            float lo = 0.0f;
            for (int j = 0; j < 6; ++j) lo += fastNoise((sampleIndex - j) * 7 + 3);
            lo *= (1.0f / 6.0f);
            const float hi = fastNoise(sampleIndex * 47 + 19);
            out  = body * 1.25f;                                // low-mid punch
            out += softClip(lo * 3.2f, 2.4f) * bodyE * 0.85f;   // saturated body noise
            out += hi * crackE * 0.95f;                          // sharp crack transient
            out += sine(local, 50.0f) * subE * 0.55f;            // sub thump
            out += softClip(lo * 2.0f, 2.0f) * tailE * 0.45f;    // report tail
            out = softClip(out, 1.85f);
            break;
        }
        case SoundEventType::Hit:
            // Snappy hitmarker tick: a bright transient snap over two short sines so a
            // connect reads as a crisp "thock" rather than a soft beep.
            out  = fastNoise(sampleIndex * 31 + 7) * envExp(local, 130.0f) * 0.34f; // snap
            out += sine(local, 1180.0f) * envExp(local, 58.0f) * 0.32f;            // bright tick
            out += sine(local, 760.0f) * envExp(local, 40.0f) * 0.20f;             // body
            out = softClip(out, 1.5f);
            break;
        case SoundEventType::Kill: {
            const float sweep = 70.0f + 200.0f * envExp(local, 20.0f);
            out = sine(local, sweep) * envExp(local, 8.5f) * 0.58f;
            out += sine(local, 900.0f + 600.0f * envExp(local, 30.0f)) * envExp(local, 24.0f) * 0.34f;
            out += fastNoise(sampleIndex * 13) * envExp(local, 30.0f) * 0.22f;
            break;
        }
        case SoundEventType::Dash:
            out = fastNoise(sampleIndex * 5) * envExp(local, 12.0f) * 0.28f;
            out += sine(local, 240.0f + 540.0f * local) * envExp(local, 10.0f) * 0.22f;
            break;
        case SoundEventType::DryFire:
            out = sine(local, 1600.0f) * envExp(local, 80.0f) * 0.20f;
            out += fastNoise(sampleIndex) * envExp(local, 95.0f) * 0.10f;
            break;
        case SoundEventType::ReloadStart: {
            const float latch = envExp(local, 220.0f);
            out = fastNoise(sampleIndex * 29 + 3) * latch * 0.58f;
            out += softClip(sine(local, 420.0f) + sine(local, 880.0f) * 0.45f, 3.8f)
                * envExp(local, 95.0f) * 0.34f;
            const float rock = local - 0.055f;
            if (rock > 0.0f) {
                out += fastNoise(sampleIndex * 17 + 9) * envExp(rock, 55.0f) * 0.18f;
                out += softClip(sine(rock, 150.0f), 3.5f) * envExp(rock, 32.0f) * 0.38f;
                out += sine(rock, 310.0f) * envExp(rock, 50.0f) * 0.18f;
            }
            const float drop = local - 0.185f;
            if (drop > 0.0f) {
                out += fastNoise(sampleIndex * 41 + 21) * envExp(drop, 105.0f) * 0.34f;
                out += softClip(sine(drop, 112.0f) + sine(drop, 245.0f) * 0.55f, 3.0f)
                    * envExp(drop, 38.0f) * 0.42f;
            }
            const float cloth = local - 0.285f;
            if (cloth > 0.0f) {
                out += fastNoise(sampleIndex * 7 + 5) * envExp(cloth, 24.0f) * 0.10f;
            }
            out = softClip(out, 1.65f);
            break;
        }
        case SoundEventType::ReloadEnd: {
            const float shove = envExp(local, 110.0f);
            out = fastNoise(sampleIndex * 13 + 7) * shove * 0.36f;
            out += softClip(sine(local, 118.0f) + sine(local, 240.0f) * 0.45f, 3.8f)
                * envExp(local, 36.0f) * 0.56f;
            out += sine(local, 520.0f) * envExp(local, 82.0f) * 0.16f;
            const float slap = local - 0.065f;
            if (slap > 0.0f) {
                out += fastNoise(sampleIndex * 23 + 15) * envExp(slap, 115.0f) * 0.42f;
                out += softClip(sine(slap, 190.0f), 4.0f) * envExp(slap, 55.0f) * 0.28f;
            }
            const float snap = local - 0.185f;
            if (snap > 0.0f) {
                out += fastNoise(sampleIndex * 29 + 1) * envExp(snap, 180.0f) * 0.52f;
                out += softClip(sine(snap, 340.0f) + sine(snap, 1120.0f) * 0.35f, 5.0f)
                    * envExp(snap, 82.0f) * 0.38f;
            }
            const float bolt = local - 0.245f;
            if (bolt > 0.0f) {
                out += fastNoise(sampleIndex * 53 + 27) * envExp(bolt, 145.0f) * 0.36f;
                out += softClip(sine(bolt, 165.0f) + sine(bolt, 760.0f) * 0.25f, 4.2f)
                    * envExp(bolt, 62.0f) * 0.40f;
            }
            out = softClip(out, 1.75f);
            break;
        }
        case SoundEventType::Hurt:
            out = sine(local, 92.0f) * envExp(local, 8.0f) * 0.44f;
            out += fastNoise(sampleIndex * 7) * envExp(local, 18.0f) * 0.14f;
            break;
        case SoundEventType::Pickup:
            out = sine(local, 660.0f + 420.0f * local) * envExp(local, 16.0f) * 0.22f;
            out += sine(local, 1320.0f + 520.0f * local) * envExp(local, 20.0f) * 0.18f;
            break;
        case SoundEventType::Config:
            out = sine(local, 880.0f) * envExp(local, 32.0f) * 0.18f;
            out += sine(local, 1320.0f) * envExp(local, 36.0f) * 0.14f;
            break;
        }
    return out;
}

} // namespace

// SneidGame-style music authoring prototype: relentless four-on-the-floor techno,
// hard sidechain pump, distorted acid bass/riff, and bar-level chaos. PULSE bakes
// this into synchronized stems; the runtime mixer owns state/intensity layering.
struct MusicSynth {
    double sr = 48000.0;
    double bpm = 140.0;
    double pos = 0.0;

    void renderLayerStereo(int layer, float& left, float& right) {
        const double t = pos / sr;
        const int sampleIndex = static_cast<int>(pos);
        pos += 1.0;

        const float beat = 60.0f / static_cast<float>(std::max(40.0, bpm));
        const float six = beat * 0.25f;
        const float bar = beat * 4.0f;
        const float barPhase = static_cast<float>(std::fmod(t, bar));
        const float beatPhase = static_cast<float>(std::fmod(t, beat));
        const float sixPhase = static_cast<float>(std::fmod(t, six));
        const int step = static_cast<int>(std::floor(barPhase / six)) & 15;
        const int barIndex = static_cast<int>(std::floor(t / bar));
        const int songBar = barIndex & 7;
        const float chaos = 0.5f * (fastNoise(step * 101 + barIndex * 131 + songBar * 37 + 7) + 1.0f);
        static const float roots[8] = { 55.00f, 55.00f, 43.65f, 49.00f, 55.00f, 65.41f, 49.00f, 41.20f };
        static const int chordDegrees[8][4] = {
            { 0, 3, 7, 10 }, { 0, 3, 7, 12 }, { 0, 4, 7, 10 }, { 0, 4, 7, 10 },
            { 0, 3, 7, 10 }, { 0, 4, 7, 12 }, { 0, 4, 7, 10 }, { 0, 4, 7, 10 }
        };
        const float root = roots[songBar];
        const float pump = 0.22f + 0.78f * (1.0f - envExp(beatPhase, 7.0f));
        const float barLift = ((songBar == 3 || songBar == 7) && barPhase > bar * 0.5f)
            ? (barPhase - bar * 0.5f) / (bar * 0.5f) : 0.0f;
        const float phraseTime = static_cast<float>(songBar) * bar + barPhase;
        const float phraseLen = 8.0f * bar;
        const float phraseIn = std::clamp(phraseTime / beat, 0.0f, 1.0f);
        const float phraseOut = std::clamp((phraseLen - phraseTime) / (beat * 0.75f), 0.0f, 1.0f);
        const float loopGuard = std::min(phraseIn, phraseOut);
        left = 0.0f;
        right = 0.0f;

        auto addPan = [&](float value, float pan) {
            const float p = std::clamp(pan, -1.0f, 1.0f);
            const float lg = std::sqrt(0.5f * (1.0f - p));
            const float rg = std::sqrt(0.5f * (1.0f + p));
            left += value * lg;
            right += value * rg;
        };
        auto noise = [&](int salt) {
            return fastNoise(sampleIndex * 19 + step * 113 + barIndex * 271 + salt);
        };
        auto brightNoise = [&](int salt) {
            return noise(salt) - 0.72f * fastNoise((sampleIndex - 1) * 19 + step * 113 + barIndex * 271 + salt);
        };
        auto semitone = [](float st) {
            return std::pow(2.0f, st / 12.0f);
        };
        auto hitAt = [](float phase, float at, float speed) {
            return phase >= at ? std::exp(-(phase - at) * speed) : 0.0f;
        };
        auto chordHz = [&](int voice, float octave) {
            return root * octave * semitone(static_cast<float>(chordDegrees[songBar][voice]));
        };

        switch (layer) {
            case 0: { // bed: kick, sidechained harmonic bed, and the A-minor pressure thesis
                const float kEnv = envExp(beatPhase, 11.4f);
                const float kPitch = 41.0f + 158.0f * envExp(beatPhase, 34.0f);
                const float kick = softClip(sine(beatPhase, kPitch) * 1.54f, 2.75f) * kEnv
                    + brightNoise(3) * envExp(beatPhase, 285.0f) * 0.050f;
                addPan(kick * 0.94f, 0.0f);

                const float offPulse = hitAt(beatPhase, beat * 0.50f, 7.5f);
                const float padEnv = (0.42f + 0.34f * pump + 0.16f * offPulse) * (0.96f - 0.08f * barLift) * loopGuard;
                float padL = 0.0f, padR = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    const float h = chordHz(i, i == 0 ? 0.50f : 1.00f);
                    const float breath = 0.82f + 0.18f * sine(t, 0.071f + 0.013f * static_cast<float>(i));
                    padL += softClip(saw(t, h * (0.996f - 0.0015f * i)) * 0.48f + sine(t, h * 0.5f) * 0.18f, 1.45f) * breath;
                    padR += softClip(saw(t, h * (1.004f + 0.0012f * i)) * 0.45f + sine(t, h * 0.5f) * 0.18f, 1.45f) * breath;
                }
                addPan(softClip(padL * 0.070f, 1.4f) * padEnv, -0.34f);
                addPan(softClip(padR * 0.070f, 1.4f) * padEnv, 0.34f);
                addPan(sine(t, 55.0f * 0.25f) * (0.08f + 0.05f * pump) * loopGuard, 0.0f);
                break;
            }
            case 1: { // bass: composed acid line with offbeat drive and bar-to-bar answers
                static const int kRest = 99;
                static const int bassNotes[8][16] = {
                    { kRest, 0, 0, 3,  kRest, 0, 7, 0,  kRest, 0, 3, 0,  kRest, 7, 5, -2 },
                    { kRest, 0, 3, 0,  kRest, 7, 0, 10, kRest, 0, 3, 7,  kRest, 12, 10, 7 },
                    { kRest, 0, 0, 4,  kRest, 0, 7, 0,  kRest, 0, 4, 7,  kRest, 10, 7, 5 },
                    { kRest, 0, 2, 4,  kRest, 7, 4, 2,  kRest, 0, 7, 10, kRest, 12, 10, 7 },
                    { kRest, 0, 0, 3,  kRest, 7, 0, 3,  kRest, 10, 7, 3, kRest, 12, 10, 7 },
                    { kRest, 0, 4, 7,  kRest, 12, 7, 4, kRest, 0, 4, 7,  kRest, 12, 10, 7 },
                    { kRest, 0, 2, 4,  kRest, 7, 4, 2,  kRest, 0, 7, 10, kRest, 14, 12, 10 },
                    { kRest, 0, 4, 7,  kRest, 10, 7, 4, kRest, 0, 4, 7,  10, 7, 4, -1 }
                };
                const int note = bassNotes[songBar][step];
                if (note != kRest) {
                    const float bEnv = envExp(sixPhase, 8.0f) * (sixPhase < six * 0.88f ? 1.0f : 0.0f);
                    const float accent = (step == 1 || step == 9) ? 1.18f : ((step == 14 || step == 15) ? 1.05f : 0.88f);
                    const float cutoffGesture = 0.62f + 0.38f * envExp(sixPhase, 24.0f);
                    const float bf = root * 2.0f * semitone(static_cast<float>(note));
                    const float acid = softClip(
                        saw(t, bf * 0.997f) * 0.46f
                      + pulse(t, bf * 1.003f, 0.36f) * 0.28f
                      + sine(t, bf * 0.5f) * 0.60f, 3.7f + 1.7f * cutoffGesture);
                    const float sub = sine(t, root * 0.5f) * 0.30f;
                    const float click = brightNoise(31) * envExp(sixPhase, 145.0f) * 0.030f;
                    addPan((acid * 0.44f + sub + click) * bEnv * pump * accent, 0.0f);
                    addPan(acid * bEnv * pump * 0.055f * accent, (step & 1) ? 0.10f : -0.08f);
                }
                break;
            }
            case 2: { // drums: mechanical hats, claps, fills, and chord stabs
                const bool openHat = (step == 2 || step == 6 || step == 10 || step == 14);
                const float hEnv = envExp(sixPhase, openHat ? 18.0f : ((step & 1) ? 52.0f : 105.0f));
                const float hAcc = openHat ? 1.05f : ((step & 1) ? 0.62f : 0.42f);
                const float hat = (brightNoise(17) * 0.31f + sine(t, openHat ? 8200.0f : 10400.0f) * 0.052f) * hEnv * hAcc;
                addPan(hat * 0.086f, (step & 1) ? -0.52f : 0.50f);

                if (step == 4 || step == 12) {
                    const float clap = brightNoise(51) * hitAt(sixPhase, 0.0f, 21.0f) * 0.13f
                        + brightNoise(57) * hitAt(sixPhase, 0.012f, 29.0f) * 0.10f
                        + sine(t, 185.0f) * hitAt(sixPhase, 0.0f, 24.0f) * 0.032f;
                    addPan(clap, -0.18f);
                    addPan(clap * 0.78f, 0.22f);
                }
                const bool ghostKick = step == 7 || step == 11 || step == 14 || (songBar == 7 && step >= 12);
                if (ghostKick) {
                    const float gk = envExp(sixPhase, 16.0f);
                    const float pitch = 58.0f + 118.0f * envExp(sixPhase, 35.0f);
                    addPan(softClip(sine(sixPhase, pitch) * 1.15f, 2.0f) * gk * (songBar == 7 && step >= 12 ? 0.16f : 0.22f), 0.0f);
                }
                if (step == 2 || step == 6 || step == 10 || step == 14) {
                    const float sEnv = envExp(sixPhase, 13.0f);
                    float stab = 0.0f;
                    for (int i = 0; i < 3; ++i)
                        stab += saw(t, chordHz(i, 4.0f) * (0.996f + 0.003f * i)) * (i == 0 ? 0.34f : 0.22f);
                    addPan(softClip(stab, 2.3f) * sEnv * pump * 0.064f, (step == 6 || step == 14) ? 0.34f : -0.34f);
                }
                if (songBar == 7 && step >= 12) {
                    const float fillPitch = 230.0f - 24.0f * static_cast<float>(step - 12);
                    const float fill = brightNoise(63) * envExp(sixPhase, 42.0f) * 0.058f
                        + sine(t, fillPitch) * envExp(sixPhase, 26.0f) * 0.050f;
                    addPan(fill, (step & 1) ? -0.42f : 0.42f);
                }
                break;
            }
            case 3: { // pressure: authored acid lead phrase with call/response over eight bars
                static const int kRest = 99;
                static const int leadNotes[8][16] = {
                    { kRest, 0, kRest, 3,  7, kRest, 10, 7,  kRest, 12, 10, 7,  kRest, 3, 5, kRest },
                    { kRest, 0, kRest, 3,  7, kRest, 10, 12, kRest, 15, 12, 10, 7, kRest, 5, 3 },
                    { kRest, 7, kRest, 5,  4, kRest, 7, 12,  kRest, 10, 7, 5,   4, kRest, 5, 7 },
                    { kRest, 0, kRest, 2,  7, kRest, 10, 12, 14, kRest, 12, 10, 7, 5, 3, 2 },
                    { kRest, 12, kRest, 10, 7, kRest, 3, 5,  kRest, 7, 10, 12,  kRest, 15, 12, 10 },
                    { kRest, 7, kRest, 4,  0, kRest, 4, 7,   kRest, 12, 11, 7,  4, kRest, 7, 12 },
                    { kRest, 14, kRest, 12, 10, kRest, 7, 4, kRest, 7, 10, 12, 14, kRest, 12, 10 },
                    { kRest, 12, 10, 7,    4, kRest, 0, 4,   7, 10, 12, 10,     7, 4, 3, -1 }
                };
                const int note = leadNotes[songBar][step];
                if (note != kRest) {
                    const float phraseAccent = (songBar >= 4 ? 1.08f : 0.94f) + (songBar == 7 ? 0.12f : 0.0f);
                    const float noteLen = songBar == 7 ? 0.72f : 0.58f;
                    const float riffEnv = envExp(sixPhase, 6.7f) * (sixPhase < six * noteLen ? 1.0f : 0.0f);
                    const float af = root * 4.0f * semitone(static_cast<float>(note));
                    const float vibrato = 1.0f + 0.012f * sine(t, 6.1f + chaos * 2.5f) + 0.006f * sine(t, 10.7f);
                    const float leftLead = softClip(saw(t, af * vibrato * 0.996f) * 0.48f + pulse(t, af * 1.002f, 0.30f) * 0.30f, 4.1f);
                    const float rightLead = softClip(saw(t, af * vibrato * 1.007f) * 0.45f + pulse(t, af * 0.991f, 0.36f) * 0.32f, 4.1f);
                    addPan(leftLead * riffEnv * pump * 0.132f * phraseAccent, -0.58f);
                    addPan(rightLead * riffEnv * pump * 0.132f * phraseAccent, 0.58f);
                }
                if (barLift > 0.0f) {
                    const float riser = (brightNoise(73) * 0.28f + sine(t, 320.0f + 4800.0f * barLift * barLift) * 0.26f)
                        * barLift * barLift * loopGuard;
                    addPan(riser * 0.11f, 0.48f);
                    addPan(riser * 0.075f, -0.36f);
                }
                break;
            }
            case 4: { // boss: antagonist version of the lead motif, plus narrow low machinery
                static const int kRest = 99;
                static const int bossNotes[8][16] = {
                    { 0, kRest, 6, kRest, 7, kRest, 3, kRest, 0, kRest, -1, kRest, 3, kRest, 6, 7 },
                    { 0, kRest, 6, kRest, 10, kRest, 7, kRest, 3, kRest, 0, kRest, 7, kRest, 10, 12 },
                    { 0, kRest, 5, kRest, 7, kRest, 4, kRest, 0, kRest, -2, kRest, 4, kRest, 7, 10 },
                    { 0, kRest, 6, kRest, 7, kRest, 10, kRest, 12, kRest, 10, 7, 6, 3, 1, 0 },
                    { 12, kRest, 10, kRest, 7, kRest, 6, kRest, 3, kRest, 0, kRest, 6, kRest, 7, 10 },
                    { 0, kRest, 4, kRest, 7, kRest, 11, kRest, 12, kRest, 11, kRest, 7, kRest, 4, 0 },
                    { 0, kRest, 6, kRest, 10, kRest, 12, kRest, 14, kRest, 12, 10, 7, 6, 3, 1 },
                    { 0, 4, 7, 10, 12, 10, 7, 4, 0, 4, 7, 10, 12, 10, 7, -1 }
                };
                const float drone = softClip(
                    saw(t, root * 0.50f * (1.0f + 0.006f * sine(t, 0.19f))) * 0.28f
                  + sine(t, root * 0.25f) * 0.22f
                  + saw(t, root * 0.75f) * 0.10f, 3.0f) * (0.58f + 0.42f * pump) * loopGuard;
                addPan(drone * 0.31f, 0.0f);

                const int note = bossNotes[songBar][step];
                if (note != kRest) {
                    const float e = envExp(sixPhase, songBar == 7 ? 5.6f : 7.8f) * (sixPhase < six * 0.82f ? 1.0f : 0.0f);
                    const float hz = root * 2.0f * semitone(static_cast<float>(note));
                    const float tone = softClip(
                        saw(t, hz * 0.996f) * 0.38f
                      + pulse(t, hz * semitone(6.0f), 0.42f) * 0.20f
                      + sine(t, hz * 0.5f) * 0.18f, 4.8f);
                    addPan(tone * e * pump * 0.18f, (step & 2) ? -0.48f : 0.48f);
                }
                if ((step == 3 || step == 7 || step == 12 || step == 15) && chaos > 0.28f) {
                    const float alarm = brightNoise(89) * envExp(sixPhase, 25.0f) * 0.070f
                        + sine(t, root * 8.0f * semitone(static_cast<float>((step & 1) ? 6 : 10))) * envExp(sixPhase, 34.0f) * 0.040f;
                    addPan(alarm, (step & 1) ? -0.65f : 0.65f);
                }
                break;
            }
            default:
                break;
        }

        if (layer >= 2) {
            constexpr int kDelayMask = 8191;
            const int tapA = (delayWrite_ - 1739) & kDelayMask;
            const int tapB = (delayWrite_ - 3119) & kDelayMask;
            const float wetL = delayL_[static_cast<size_t>(tapA)];
            const float wetR = delayR_[static_cast<size_t>(tapB)];
            left += wetR * 0.12f;
            right += wetL * 0.12f;
            delayL_[static_cast<size_t>(delayWrite_)] = left * 0.18f + wetR * 0.38f;
            delayR_[static_cast<size_t>(delayWrite_)] = right * 0.18f + wetL * 0.38f;
            delayWrite_ = (delayWrite_ + 1) & kDelayMask;
        }

        left = softClip(left, 1.15f);
        right = softClip(right, 1.15f);
    }

    float renderLayer(int layer) {
        float l = 0.0f, r = 0.0f;
        renderLayerStereo(layer, l, r);
        return 0.5f * (l + r);
    }

    std::array<float, 8192> delayL_{};
    std::array<float, 8192> delayR_{};
    int delayWrite_ = 0;
};

namespace {

void writeWavHeaderN(std::FILE* f, uint32_t sr, uint16_t channels, uint32_t dataBytes) {
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint16_t bytesPerFrame = static_cast<uint16_t>(channels * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(channels);   // PCM
    u32(sr); u32(sr * bytesPerFrame); u16(bytesPerFrame); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
}
void writeWavHeader(std::FILE* f, uint32_t sr, uint32_t dataBytes) { writeWavHeaderN(f, sr, 2, dataBytes); }

constexpr std::array<const char*, 4> kAssetSearchPrefixes = { "", "../", "../../", "../../../" };

// Resolve an asset path by walking up from the executable folder (mirrors the
// game's resolveAsset). Returns rel unchanged if not found (caller fails loud).
std::string resolveAsset(const std::string& rel) {
    for (const char* p : kAssetSearchPrefixes) {
        const std::string candidate = std::string(p) + rel;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return rel;
}

std::vector<std::filesystem::path> assetDirectoryCandidates(const std::string& rel) {
    std::vector<std::filesystem::path> dirs;
    for (const char* p : kAssetSearchPrefixes) {
        const std::filesystem::path candidate = std::filesystem::path(std::string(p) + rel);
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec)) continue;
        std::filesystem::path normalized = std::filesystem::weakly_canonical(candidate, ec);
        if (ec) normalized = candidate.lexically_normal();
        bool seen = false;
        for (const auto& existing : dirs) {
            if (existing == normalized) { seen = true; break; }
        }
        if (!seen) dirs.push_back(std::move(normalized));
    }
    return dirs;
}

const char* sfxName(SoundEventType t) {
    switch (t) {
        case SoundEventType::Config:      return "config";
        case SoundEventType::Dash:        return "dash";
        case SoundEventType::Fire:        return "fire";
        case SoundEventType::Hit:         return "hit";
        case SoundEventType::Kill:        return "kill";
        case SoundEventType::Hurt:        return "hurt";
        case SoundEventType::DryFire:     return "dryfire";
        case SoundEventType::ReloadStart: return "reload_start";
        case SoundEventType::ReloadEnd:   return "reload_end";
        case SoundEventType::Pickup:      return "pickup";
    }
    return "unknown";
}

const char* weaponEventBankName(WeaponEventType event) {
    switch (event) {
        case WeaponEventType::DryFire:     return "dry";
        case WeaponEventType::ReloadStart: return "reload_start";
        case WeaponEventType::ReloadEnd:   return "reload_end";
        case WeaponEventType::MagOut:      return "mag_out";
        case WeaponEventType::MagIn:       return "mag_in";
        case WeaponEventType::Bolt:        return "bolt";
        case WeaponEventType::Shell:       return "shell";
        case WeaponEventType::Equip:       return "equip";
        default:                           return "";
    }
}

const char* enemyEventBankName(EnemyEventType event) {
    switch (event) {
        case EnemyEventType::Telegraph: return "telegraph";
        case EnemyEventType::Shot:      return "shot";
        case EnemyEventType::Impact:    return "impact";
        case EnemyEventType::Beam:      return "beam";
        case EnemyEventType::Lunge:     return "lunge";
        case EnemyEventType::MeleeHit:  return "melee_hit";
        case EnemyEventType::Hurt:      return "hurt";
        case EnemyEventType::Death:     return "death";
        case EnemyEventType::BossBurst: return "boss_burst";
    }
    return "";
}

const char* feedbackEventName(FeedbackEventType event) {
    switch (event) {
        case FeedbackEventType::Hitmarker:       return "hitmarker";
        case FeedbackEventType::HitCrit:         return "hit_crit";
        case FeedbackEventType::Kill:            return "kill";
        case FeedbackEventType::KillElite:       return "kill_elite";
        case FeedbackEventType::Dash:            return "dash";
        case FeedbackEventType::Jump:            return "jump";
        case FeedbackEventType::AbilityTactical: return "ability_tactical";
        case FeedbackEventType::AbilityUltimate: return "ability_ultimate";
        case FeedbackEventType::ChargeReady:     return "charge_ready";
        case FeedbackEventType::Explosion:       return "explosion";
        case FeedbackEventType::ShieldAbsorb:    return "shield_absorb";
        case FeedbackEventType::ShieldBreak:     return "shield_break";
        case FeedbackEventType::LowHealth:       return "low_health";
        case FeedbackEventType::PickupHealth:    return "pickup_health";
        case FeedbackEventType::PickupShield:    return "pickup_shield";
        case FeedbackEventType::PickupAmmo:      return "pickup_ammo";
        case FeedbackEventType::PickupScrap:     return "pickup_scrap";
        case FeedbackEventType::PickupPowerup:   return "pickup_powerup";
        case FeedbackEventType::ElementBurn:     return "element_burn";
        case FeedbackEventType::ElementShock:    return "element_shock";
        case FeedbackEventType::ElementCryo:     return "element_cryo";
        case FeedbackEventType::ElementCorrode:  return "element_corrode";
        case FeedbackEventType::ElementCombo:    return "element_combo";
        case FeedbackEventType::ElementLeech:    return "element_leech";
        case FeedbackEventType::UiMove:          return "ui_move";
        case FeedbackEventType::UiConfirm:       return "ui_confirm";
        case FeedbackEventType::UiCancel:        return "ui_cancel";
        case FeedbackEventType::UiReward:        return "ui_reward";
        case FeedbackEventType::RunWin:          return "run_win";
        case FeedbackEventType::RunLose:         return "run_lose";
    }
    return "";
}

// Graceful degradation: if an authored feedback bank is missing, route to the
// closest existing one-shot SFX so the cue is never silent (the player bus must
// always confirm). Build.bat regenerates the banks so shipping builds use them.
SoundEventType feedbackFallback(FeedbackEventType event) {
    switch (event) {
        case FeedbackEventType::Hitmarker:
        case FeedbackEventType::HitCrit:
        case FeedbackEventType::ShieldAbsorb:    return SoundEventType::Hit;
        case FeedbackEventType::Kill:
        case FeedbackEventType::KillElite:
        case FeedbackEventType::AbilityUltimate:
        case FeedbackEventType::Explosion:       return SoundEventType::Kill;
        case FeedbackEventType::ElementCombo:    return SoundEventType::Kill;
        case FeedbackEventType::Dash:
        case FeedbackEventType::Jump:
        case FeedbackEventType::AbilityTactical: return SoundEventType::Dash;
        case FeedbackEventType::ElementShock:
        case FeedbackEventType::ElementCryo:
        case FeedbackEventType::ElementBurn:
        case FeedbackEventType::ElementCorrode:  return SoundEventType::Hit;
        case FeedbackEventType::ShieldBreak:
        case FeedbackEventType::LowHealth:
        case FeedbackEventType::RunLose:         return SoundEventType::Hurt;
        case FeedbackEventType::ElementLeech:    return SoundEventType::Pickup;
        case FeedbackEventType::UiMove:
        case FeedbackEventType::UiCancel:        return SoundEventType::Config;
        default:                                 return SoundEventType::Pickup;
    }
}

int proceduralFeedbackIndex(FeedbackEventType event) {
    switch (event) {
        case FeedbackEventType::ElementBurn:    return 0;
        case FeedbackEventType::ElementShock:   return 1;
        case FeedbackEventType::ElementCryo:    return 2;
        case FeedbackEventType::ElementCorrode: return 3;
        case FeedbackEventType::ElementCombo:   return 4;
        case FeedbackEventType::ElementLeech:   return 5;
        default: break;
    }
    return -1;
}

float proceduralFeedbackSeconds(FeedbackEventType event) {
    switch (event) {
        case FeedbackEventType::ElementBurn:    return 0.18f;
        case FeedbackEventType::ElementShock:   return 0.14f;
        case FeedbackEventType::ElementCryo:    return 0.22f;
        case FeedbackEventType::ElementCorrode: return 0.24f;
        case FeedbackEventType::ElementCombo:   return 0.34f;
        case FeedbackEventType::ElementLeech:   return 0.26f;
        default:                                return 0.16f;
    }
}

Sample makeProceduralFeedbackSample(FeedbackEventType event) {
    constexpr uint32_t sr = 48000;
    Sample s;
    s.channels = 1;
    s.rate = sr;
    const uint32_t frames = static_cast<uint32_t>(proceduralFeedbackSeconds(event) * sr);
    s.data.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        const float u = frames > 1 ? static_cast<float>(i) / static_cast<float>(frames - 1) : 0.0f;
        const float fadeOut = 1.0f - u;
        const float clickGuard = std::min(1.0f, t * 260.0f) * std::min(1.0f, fadeOut * 18.0f);
        const int n = static_cast<int>(i);
        float out = 0.0f;
        switch (event) {
            case FeedbackEventType::ElementBurn: {
                const float crack = fastNoise(n * 17 + 11) * envExp(t, 42.0f);
                const float hiss = fastNoise(n * 5 + 3) * envExp(t, 12.0f) * (0.45f + 0.55f * std::sin(t * 260.0f));
                const float body = sine(t, 180.0f + 360.0f * envExp(t, 18.0f)) * envExp(t, 15.0f);
                out = crack * 0.36f + hiss * 0.28f + body * 0.26f;
                break;
            }
            case FeedbackEventType::ElementShock: {
                const float zap = sine(t, 1480.0f + 820.0f * envExp(t, 45.0f)) * envExp(t, 34.0f);
                const float tick = pulse(t, 92.0f + 180.0f * envExp(t, 24.0f), 0.18f) * envExp(t, 21.0f);
                const float fizz = fastNoise(n * 41 + 5) * envExp(t, 55.0f);
                out = zap * 0.42f + tick * 0.20f + fizz * 0.26f;
                break;
            }
            case FeedbackEventType::ElementCryo: {
                const float ping = sine(t, 1160.0f) * envExp(t, 10.0f)
                                 + sine(t, 1720.0f) * envExp(t, 13.0f) * 0.55f;
                const float crack = fastNoise(n * 29 + 19) * envExp(std::max(0.0f, t - 0.055f), 58.0f);
                const float glass = sine(t, 620.0f - 180.0f * u) * envExp(t, 7.0f);
                out = ping * 0.34f + glass * 0.18f + crack * 0.18f;
                break;
            }
            case FeedbackEventType::ElementCorrode: {
                const float bubble = sine(t, 95.0f + 22.0f * std::sin(t * 71.0f)) * envExp(t, 8.0f);
                const float sizzle = fastNoise(n * 13 + 23) * envExp(t, 14.0f);
                const float scrape = saw(t, 210.0f + 80.0f * std::sin(t * 18.0f)) * envExp(t, 16.0f);
                out = bubble * 0.28f + softClip(sizzle * 1.6f, 1.8f) * 0.30f + scrape * 0.12f;
                break;
            }
            case FeedbackEventType::ElementCombo: {
                const float surge = sine(t, 78.0f + 210.0f * envExp(t, 6.0f)) * envExp(t, 5.0f);
                const float arc = sine(t, 680.0f + 560.0f * std::sin(t * 21.0f)) * envExp(t, 10.0f);
                const float burst = fastNoise(n * 31 + 7) * envExp(t, 18.0f);
                const float tail = sine(t, 1400.0f - 530.0f * u) * envExp(t, 7.0f);
                out = surge * 0.46f + arc * 0.22f + burst * 0.22f + tail * 0.13f;
                break;
            }
            case FeedbackEventType::ElementLeech: {
                const float pull = sine(t, 130.0f + 360.0f * u) * envExp(t, 5.5f);
                const float hollow = sine(t, 260.0f + 130.0f * std::sin(t * 10.0f)) * envExp(t, 7.0f);
                const float breath = fastNoise(n * 7 + 31) * envExp(t, 10.0f) * (0.35f + 0.65f * u);
                out = pull * 0.28f + hollow * 0.20f + breath * 0.16f;
                break;
            }
            default:
                out = 0.0f;
                break;
        }
        s.data[static_cast<size_t>(i)] = softClip(out * clickGuard, 1.8f);
    }
    return s;
}

std::array<Sample, 6> makeProceduralFeedbackBank() {
    return {
        makeProceduralFeedbackSample(FeedbackEventType::ElementBurn),
        makeProceduralFeedbackSample(FeedbackEventType::ElementShock),
        makeProceduralFeedbackSample(FeedbackEventType::ElementCryo),
        makeProceduralFeedbackSample(FeedbackEventType::ElementCorrode),
        makeProceduralFeedbackSample(FeedbackEventType::ElementCombo),
        makeProceduralFeedbackSample(FeedbackEventType::ElementLeech)
    };
}

const Sample* proceduralFeedbackSample(FeedbackEventType event) {
    const int idx = proceduralFeedbackIndex(event);
    if (idx < 0) return nullptr;
    static const std::array<Sample, 6> bank = makeProceduralFeedbackBank();
    return &bank[static_cast<size_t>(idx)];
}

std::string weaponEventKey(const std::string& weaponId, const char* eventName) {
    return weaponId + ":" + eventName;
}

std::string enemyEventKey(const std::string& enemyId, const char* eventName) {
    return enemyId + ":" + eventName;
}

bool isValidBankId(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool hasNumericVariationSuffix(const std::string& name) {
    const size_t lastUnderscore = name.find_last_of('_');
    if (lastUnderscore == std::string::npos || lastUnderscore + 1 >= name.size()) return false;
    for (size_t i = lastUnderscore + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

const char* musicLayerName(int layer) {
    switch (layer) {
        case 0: return "bed";
        case 1: return "bass";
        case 2: return "drums";
        case 3: return "pressure";
        case 4: return "boss";
        case 5: return "overpulse";
        case 6: return "duress";   // v4 (C1): per-biome near-death tension stem
    }
    return "unknown";
}

const char* musicBiomeFolder(int biome) {
    switch (biome) {
        case 0: return "foundry";
        case 1: return "furnace";
        case 2: return "reliquary";
    }
    return "foundry";
}

const char* musicStingerName(MusicStingerType type) {
    switch (type) {
        case MusicStingerType::RoomClear: return "room_clear";
        case MusicStingerType::Reward:    return "reward";
        case MusicStingerType::BossIntro: return "boss_intro";
        case MusicStingerType::Overpulse: return "overpulse";
        case MusicStingerType::RunWin:    return "run_win";
        case MusicStingerType::RunLose:   return "run_lose";
        case MusicStingerType::SectorFoundry:   return "sector_foundry";
        case MusicStingerType::SectorFurnace:   return "sector_furnace";
        case MusicStingerType::SectorReliquary: return "sector_reliquary";
        case MusicStingerType::BossPhase:       return "boss_phase";
        case MusicStingerType::BossEnrage:      return "boss_enrage";
        case MusicStingerType::Anticipation:    return "anticipation";
    }
    return "unknown";
}

MusicStingerType sectorStingerForBiome(int biome) {
    switch (std::clamp(biome, 0, kMusicBiomeCount - 1)) {
        case 1: return MusicStingerType::SectorFurnace;
        case 2: return MusicStingerType::SectorReliquary;
        default: return MusicStingerType::SectorFoundry;
    }
}

int musicStingerPriority(MusicStingerType type) {
    switch (type) {
        case MusicStingerType::RunWin:
        case MusicStingerType::RunLose: return 100;
        case MusicStingerType::BossIntro: return 90;
        case MusicStingerType::BossEnrage: return 88;   // v4: enrage crossing (just under BossIntro)
        case MusicStingerType::BossPhase: return 85;    // v4: phase crossing
        case MusicStingerType::Overpulse: return 60;
        case MusicStingerType::Anticipation: return 58; // v4 (C2): DoorsOpen riser
        case MusicStingerType::SectorFoundry:
        case MusicStingerType::SectorFurnace:
        case MusicStingerType::SectorReliquary: return 55;
        case MusicStingerType::RoomClear: return 35;
        case MusicStingerType::Reward: return 30;
    }
    return 0;
}

float smooth01(float edge0, float edge1, float x) {
    const float denom = std::max(0.0001f, edge1 - edge0);
    const float t = std::clamp((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Decode a 16-bit/float PCM WAV into interleaved float. Minimal RIFF chunk walk.
bool loadWavFile(const std::string& path, Sample& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char tag[4]; uint32_t sz = 0;
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4) != 0) { std::fclose(f); return false; }
    std::fread(&sz, 4, 1, f);
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4) != 0) { std::fclose(f); return false; }

    uint16_t audioFmt = 0, channels = 0, bits = 0; uint32_t rate = 0;
    std::vector<uint8_t> dataBytes; bool haveFmt = false, haveData = false;
    while (true) {
        char id[4]; if (std::fread(id, 1, 4, f) != 4) break;
        uint32_t csz = 0; if (std::fread(&csz, 4, 1, f) != 1) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint32_t byteRate = 0; uint16_t blockAlign = 0;
            std::fread(&audioFmt, 2, 1, f); std::fread(&channels, 2, 1, f); std::fread(&rate, 4, 1, f);
            std::fread(&byteRate, 4, 1, f); std::fread(&blockAlign, 2, 1, f); std::fread(&bits, 2, 1, f);
            if (csz > 16) std::fseek(f, static_cast<long>(csz - 16), SEEK_CUR);
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataBytes.resize(csz); std::fread(dataBytes.data(), 1, csz, f); haveData = true;
        } else {
            std::fseek(f, static_cast<long>((csz + 1) & ~1u), SEEK_CUR);   // chunks are word-aligned
        }
        if (haveFmt && haveData) break;
    }
    std::fclose(f);
    if (!haveFmt || !haveData || channels == 0) return false;
    out.channels = channels; out.rate = rate;
    if (audioFmt == 1 && bits == 16) {
        const uint32_t n = static_cast<uint32_t>(dataBytes.size() / 2);
        out.data.resize(n);
        const int16_t* s = reinterpret_cast<const int16_t*>(dataBytes.data());
        for (uint32_t i = 0; i < n; ++i) out.data[i] = s[i] / 32768.0f;
    } else if (audioFmt == 3 && bits == 32) {
        const uint32_t n = static_cast<uint32_t>(dataBytes.size() / 4);
        out.data.resize(n);
        std::memcpy(out.data.data(), dataBytes.data(), static_cast<size_t>(n) * 4);
    } else {
        return false;
    }
    return true;
}

// Decode an OGG Vorbis file into interleaved float via stb_vorbis.
bool loadOggFile(const std::string& path, Sample& out) {
    short* output = nullptr; int channels = 0, rate = 0;
    const int frames = stb_vorbis_decode_filename(path.c_str(), &channels, &rate, &output);
    if (frames < 0 || !output || channels <= 0) { if (output) std::free(output); return false; }
    out.channels = static_cast<uint32_t>(channels); out.rate = static_cast<uint32_t>(rate);
    const size_t n = static_cast<size_t>(frames) * channels;
    out.data.resize(n);
    for (size_t i = 0; i < n; ++i) out.data[i] = output[i] / 32768.0f;
    std::free(output);
    return true;
}

// Try .wav then .ogg for an asset stem (no extension); resolves up-tree.
bool loadSampleAny(const std::string& relStem, Sample& out) {
    if (loadWavFile(resolveAsset(relStem + ".wav"), out)) return true;
    if (loadOggFile(resolveAsset(relStem + ".ogg"), out)) return true;
    return false;
}

void toMono(Sample& s) {
    if (s.channels == 1) return;
    const uint32_t ch = s.channels, frames = s.frames();
    std::vector<float> mono(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        float acc = 0; for (uint32_t c = 0; c < ch; ++c) acc += s.data[i * ch + c];
        mono[i] = acc / static_cast<float>(ch);
    }
    s.data = std::move(mono); s.channels = 1;
}
void toStereo(Sample& s) {
    if (s.channels == 2) return;
    if (s.channels == 1) {
        const uint32_t frames = s.frames();
        std::vector<float> st(static_cast<size_t>(frames) * 2);
        for (uint32_t i = 0; i < frames; ++i) { st[i * 2] = s.data[i]; st[i * 2 + 1] = s.data[i]; }
        s.data = std::move(st); s.channels = 2;
    }
}

} // namespace

// Freeverb-style reverb for the shared SFX bus: 4 parallel damped feedback combs
// into 2 series allpass diffusers, per stereo channel (the right channel taps are
// offset by a stereo spread so the room has width). This is the "glue" that makes
// dry, mono-centred one-shots share one space instead of sounding detached.
struct ReverbState {
    static constexpr int kCombs = 4;
    static constexpr int kAllpass = 2;
    // Classic Freeverb tunings in samples at 44.1 kHz; scaled to the device rate.
    static constexpr int kCombTune[kCombs] = { 1116, 1188, 1277, 1356 };
    static constexpr int kAllpassTune[kAllpass] = { 556, 441 };
    static constexpr int kStereoSpread = 23;

    std::vector<float> combL[kCombs], combR[kCombs];
    int ciL[kCombs] = {}, ciR[kCombs] = {};
    float lpL[kCombs] = {}, lpR[kCombs] = {};
    std::vector<float> apL[kAllpass], apR[kAllpass];
    int aiL[kAllpass] = {}, aiR[kAllpass] = {};

    float feedback = 0.77f;   // room size: a tight shared space, not a hall
    float damp = 0.40f;       // high-frequency absorption in the tail
    float apFb = 0.5f;
    uint32_t builtRate = 0;

    void build(uint32_t rate) {
        const double scale = static_cast<double>(rate) / 44100.0;
        for (int i = 0; i < kCombs; ++i) {
            const int lenL = std::max(1, static_cast<int>(kCombTune[i] * scale));
            const int lenR = std::max(1, static_cast<int>((kCombTune[i] + kStereoSpread) * scale));
            combL[i].assign(static_cast<size_t>(lenL), 0.0f);
            combR[i].assign(static_cast<size_t>(lenR), 0.0f);
            ciL[i] = ciR[i] = 0; lpL[i] = lpR[i] = 0.0f;
        }
        for (int i = 0; i < kAllpass; ++i) {
            const int lenL = std::max(1, static_cast<int>(kAllpassTune[i] * scale));
            const int lenR = std::max(1, static_cast<int>((kAllpassTune[i] + kStereoSpread) * scale));
            apL[i].assign(static_cast<size_t>(lenL), 0.0f);
            apR[i].assign(static_cast<size_t>(lenR), 0.0f);
            aiL[i] = aiR[i] = 0;
        }
        builtRate = rate;
    }

    void clear() {
        for (int i = 0; i < kCombs; ++i) {
            std::fill(combL[i].begin(), combL[i].end(), 0.0f);
            std::fill(combR[i].begin(), combR[i].end(), 0.0f);
            lpL[i] = lpR[i] = 0.0f;
        }
        for (int i = 0; i < kAllpass; ++i) {
            std::fill(apL[i].begin(), apL[i].end(), 0.0f);
            std::fill(apR[i].begin(), apR[i].end(), 0.0f);
        }
    }

    void process(float in, float& outL, float& outR) {
        float accL = 0.0f, accR = 0.0f;
        for (int i = 0; i < kCombs; ++i) {
            float& cellL = combL[i][static_cast<size_t>(ciL[i])];
            const float yL = cellL;
            lpL[i] = yL * (1.0f - damp) + lpL[i] * damp;
            cellL = in + lpL[i] * feedback;
            if (++ciL[i] >= static_cast<int>(combL[i].size())) ciL[i] = 0;
            accL += yL;

            float& cellR = combR[i][static_cast<size_t>(ciR[i])];
            const float yR = cellR;
            lpR[i] = yR * (1.0f - damp) + lpR[i] * damp;
            cellR = in + lpR[i] * feedback;
            if (++ciR[i] >= static_cast<int>(combR[i].size())) ciR[i] = 0;
            accR += yR;
        }
        for (int i = 0; i < kAllpass; ++i) {
            float& cellL = apL[i][static_cast<size_t>(aiL[i])];
            const float boL = cellL;
            const float yL = -accL + boL;
            cellL = accL + boL * apFb;
            if (++aiL[i] >= static_cast<int>(apL[i].size())) aiL[i] = 0;
            accL = yL;

            float& cellR = apR[i][static_cast<size_t>(aiR[i])];
            const float boR = cellR;
            const float yR = -accR + boR;
            cellR = accR + boR * apFb;
            if (++aiR[i] >= static_cast<int>(apR[i].size())) aiR[i] = 0;
            accR = yR;
        }
        outL = accL;
        outR = accR;
    }
};

AudioSystem::AudioSystem(bool enableDevice) {
    enableDevice_ = enableDevice;
    musicStingerLastStart_.fill(-1.0e12);
    if (!enableDevice) return;
    loadBank();   // required sample assets; fails loud if missing (PROJECT_RULES)
    running_ = true;
    thread_ = std::thread(&AudioSystem::renderThread, this);
}

AudioSystem::~AudioSystem() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::vector<Sample> AudioSystem::loadVariantBank(const std::string& stem) {
    std::vector<Sample> bank;
    Sample s0;
    if (loadSampleAny(stem, s0)) { toMono(s0); bank.push_back(s0); }
    for (int k = 1; k < 32; ++k) {
        Sample sk;
        if (!loadSampleAny(stem + "_" + std::to_string(k), sk)) break;
        toMono(sk);
        bank.push_back(sk);
    }
    return bank;
}

void AudioSystem::playSampleBank(const std::vector<Sample>& bank, float volume, int sequenceIndex) {
    if (bank.empty()) return;
    const int count = static_cast<int>(bank.size());
    const Sample& s = bank[static_cast<size_t>(((sequenceIndex % count) + count) % count)];
    // Authored banks already round-robin; add subtle per-trigger detune/level so a
    // sustained spray reads as a weapon, not a looped sample.
    enqueueVoice(s, volume, 0.55f, 1.5f);
}

void AudioSystem::playSampleBankSpatial(const std::vector<Sample>& bank, float volume, int sequenceIndex,
                                        float ex, float ey) {
    if (bank.empty()) return;
    const int count = static_cast<int>(bank.size());
    const Sample& s = bank[static_cast<size_t>(((sequenceIndex % count) + count) % count)];
    enqueueVoiceEx(s, volume, 0.55f, 1.5f, true, ex, ey);
}

float AudioSystem::nextJitter() {
    uint32_t x = rngState_;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rngState_ = x;
    return (static_cast<float>(x & 0xFFFFFFu) / 8388607.5f) - 1.0f;   // [-1, 1]
}

void AudioSystem::enqueueVoice(const Sample& s, float volume, float pitchSemis, float gainDb) {
    enqueueVoiceEx(s, volume, pitchSemis, gainDb, false, 0.0f, 0.0f);
}

void AudioSystem::enqueueVoiceEx(const Sample& s, float volume, float pitchSemis, float gainDb,
                                 bool spatial, float ex, float ey) {
    if (s.data.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const float pitch = std::pow(2.0f, (pitchSemis * nextJitter()) / 12.0f);
    const float gain  = volume * std::pow(10.0f, (gainDb * nextJitter()) / 20.0f);
    Voice v;
    v.sample = &s;
    v.pos = 0.0;
    v.step = static_cast<double>(s.rate) / static_cast<double>(sampleRate_ ? sampleRate_ : 48000)
           * static_cast<double>(pitch);
    v.volume = gain;
    v.active = true;
    if (spatial) {
        // Bake pan + distance attenuation + distance low-pass from the listener pose.
        const float dx = ex - listenerX_;
        const float dy = ey - listenerY_;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const float invd = dist > 1e-4f ? 1.0f / dist : 0.0f;
        constexpr float kRefDist = 6.0f;     // full level within this radius
        // Gentle, floored falloff: distant threats stay audible for gameplay clarity.
        float att = kRefDist / (kRefDist + 0.5f * std::max(0.0f, dist - kRefDist));
        att = std::clamp(att, 0.18f, 1.0f);
        // Lateral component along the listener's right = stereo pan (sine of bearing).
        const float lateral = std::clamp((dx * listenerRightX_ + dy * listenerRightY_) * invd, -1.0f, 1.0f);
        // Front/back: behind the listener is a touch quieter (stereo alone cannot place it).
        const float frontness = (dx * listenerFwdX_ + dy * listenerFwdY_) * invd;
        const float rearAtt = frontness >= 0.0f ? 1.0f : (0.78f + 0.22f * (1.0f + frontness));
        const float gL = std::sqrt(0.5f * (1.0f - lateral));
        const float gR = std::sqrt(0.5f * (1.0f + lateral));
        v.spatial = true;
        v.gainL = gL * att * rearAtt;
        v.gainR = gR * att * rearAtt;
        float fc = 16000.0f;
        if (dist > kRefDist) fc = std::clamp(16000.0f * std::pow(kRefDist / dist, 0.7f), 2500.0f, 16000.0f);
        const float sr = static_cast<float>(sampleRate_ ? sampleRate_ : 48000);
        v.lpCoef = 1.0f - std::exp(-2.0f * kPi * fc / sr);
    }
    if (voices_.size() < 128) voices_.push_back(v);
}

void AudioSystem::setListener(float posX, float posY, float fwdX, float fwdY, float rightX, float rightY) {
    std::lock_guard<std::mutex> lock(mutex_);
    listenerX_ = posX; listenerY_ = posY;
    listenerFwdX_ = fwdX; listenerFwdY_ = fwdY;
    listenerRightX_ = rightX; listenerRightY_ = rightY;
}

void AudioSystem::ensureReverb() {
    const uint32_t rate = sampleRate_ ? sampleRate_ : 48000;
    if (!reverb_) reverb_ = std::make_unique<ReverbState>();
    if (reverb_->builtRate != rate) reverb_->build(rate);
}

void AudioSystem::clearMixState() {
    if (reverb_) reverb_->clear();
    sfxEnv_ = 0.0f;
    duck_ = 0.0f;
    rngState_ = 0x2545F491u;   // deterministic offline captures
    for (MusicStingerVoice& v : musicStingerVoices_) v = {};
    musicStingerLastStart_.fill(-1.0e12);
    // v4 reactive-tension state: reset inputs, the quantized-transition machine, and all the new
    // mixer-thread DSP filter states so offline captures are deterministic and v3 renders byte-match.
    musicV4_ = false;
    musicDuress_ = 0.0f;
    musicBossEscalation_ = 0.0f;
    musicRequestedState_ = musicState_;
    musicPendingApplyFrame_ = -1.0;
    musicDuressSm_ = 0.0f;
    musDuressLpL_ = musDuressLpR_ = 0.0f;
    duckSplitLpL_ = duckSplitLpR_ = 0.0f;
}

void AudioSystem::loadBank() {
    for (int i = 0; i < kSoundEventCount; ++i) {
        const SoundEventType t = static_cast<SoundEventType>(i);
        const std::string stem = std::string("assets/audio/sfx_") + sfxName(t);
        if (!loadSampleAny(stem, sfx_[i]))
            throw std::runtime_error("Required audio sample missing: " + stem + ".wav (run --bake-audio)");
        toMono(sfx_[i]);
    }
    // Fire banks, played as a consistent ordered series. Per-weapon banks live in
    // sfx_fire_<id>.wav (+ _1, _2, ...) and are auto-discovered here, keyed by <id>.
    // Named weapons do not fall back to the generic bank; validation must catch a
    // missing shipped-gun bank before a build is accepted.
    fireVariations_ = loadVariantBank("assets/audio/sfx_fire");
    namedFire_.clear();
    namedWeaponEvents_.clear();
    namedEnemyEvents_.clear();
    for (const std::filesystem::path& dir : assetDirectoryCandidates("assets/audio")) {
        std::error_code dec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, dec)) {
            if (dec) break;
            const std::string fn = entry.path().filename().string();
            const std::string pre = "sfx_fire_", suf = ".wav";
            if (fn.size() <= pre.size() + suf.size()) continue;
            if (fn.compare(0, pre.size(), pre) != 0) continue;
            if (fn.compare(fn.size() - suf.size(), suf.size(), suf) != 0) continue;
            const std::string name = fn.substr(pre.size(), fn.size() - pre.size() - suf.size());
            if (isValidBankId(name) && namedFire_.find(name) == namedFire_.end())
            {
                if (hasNumericVariationSuffix(name)) continue;
                const std::filesystem::path stem = entry.path().parent_path() / ("sfx_fire_" + name);
                namedFire_[name] = loadVariantBank(stem.string());
            }
        }
    }
    constexpr std::array<const char*, 8> kWeaponBankEvents = {
        "dry", "reload_start", "reload_end", "mag_out", "mag_in", "bolt", "shell", "equip"
    };
    for (const std::filesystem::path& dir : assetDirectoryCandidates("assets/audio")) {
        std::error_code dec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, dec)) {
            if (dec) break;
            const std::string fn = entry.path().filename().string();
            const std::string pre = "sfx_weapon_", suf = ".wav";
            if (fn.size() <= pre.size() + suf.size()) continue;
            if (fn.compare(0, pre.size(), pre) != 0) continue;
            if (fn.compare(fn.size() - suf.size(), suf.size(), suf) != 0) continue;
            std::string name = fn.substr(pre.size(), fn.size() - pre.size() - suf.size());
            if (hasNumericVariationSuffix(name)) continue;
            for (const char* eventName : kWeaponBankEvents) {
                const std::string eventSuffix = std::string("_") + eventName;
                if (name.size() <= eventSuffix.size()) continue;
                if (name.compare(name.size() - eventSuffix.size(), eventSuffix.size(), eventSuffix) != 0) continue;
                const std::string weaponId = name.substr(0, name.size() - eventSuffix.size());
                if (!isValidBankId(weaponId)) continue;
                const std::string key = weaponEventKey(weaponId, eventName);
                if (namedWeaponEvents_.find(key) != namedWeaponEvents_.end()) continue;
                const std::filesystem::path stem = entry.path().parent_path() / ("sfx_weapon_" + weaponId + "_" + eventName);
                namedWeaponEvents_[key] = loadVariantBank(stem.string());
                break;
            }
        }
    }
    constexpr std::array<const char*, 9> kEnemyBankEvents = {
        "telegraph", "shot", "impact", "beam", "lunge", "melee_hit", "hurt", "death", "boss_burst"
    };
    for (const std::filesystem::path& dir : assetDirectoryCandidates("assets/audio")) {
        std::error_code dec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, dec)) {
            if (dec) break;
            const std::string fn = entry.path().filename().string();
            const std::string pre = "sfx_enemy_", suf = ".wav";
            if (fn.size() <= pre.size() + suf.size()) continue;
            if (fn.compare(0, pre.size(), pre) != 0) continue;
            if (fn.compare(fn.size() - suf.size(), suf.size(), suf) != 0) continue;
            std::string name = fn.substr(pre.size(), fn.size() - pre.size() - suf.size());
            if (hasNumericVariationSuffix(name)) continue;
            for (const char* eventName : kEnemyBankEvents) {
                const std::string eventSuffix = std::string("_") + eventName;
                if (name.size() <= eventSuffix.size()) continue;
                if (name.compare(name.size() - eventSuffix.size(), eventSuffix.size(), eventSuffix) != 0) continue;
                const std::string enemyId = name.substr(0, name.size() - eventSuffix.size());
                if (!isValidBankId(enemyId)) continue;
                const std::string key = enemyEventKey(enemyId, eventName);
                if (namedEnemyEvents_.find(key) != namedEnemyEvents_.end()) continue;
                const std::filesystem::path stem = entry.path().parent_path() / ("sfx_enemy_" + enemyId + "_" + eventName);
                namedEnemyEvents_[key] = loadVariantBank(stem.string());
                break;
            }
        }
    }
    // Player-feedback banks (sfx_fb_<event>[_N].wav): one round-robin bank per event.
    // Soft-required: a missing bank degrades to a one-shot SFX in playFeedback rather
    // than failing the load, so the engine still runs before the producer has been run.
    namedFeedback_.clear();
    for (int i = 0; i < kFeedbackEventCount; ++i) {
        const std::string name = feedbackEventName(static_cast<FeedbackEventType>(i));
        namedFeedback_[name] = loadVariantBank(std::string("assets/audio/sfx_fb_") + name);
    }

    for (int biome = 0; biome < kMusicBiomeCount; ++biome) {
        for (int layer = 0; layer < kMusicLayerCount; ++layer) {
            const std::string stem = std::string("assets/audio/music/") + musicBiomeFolder(biome) + "_" + musicLayerName(layer);
            Sample& sample = musicLayers_[static_cast<size_t>(biome)][static_cast<size_t>(layer)];
            if (!loadSampleAny(stem, sample))
                throw std::runtime_error("Required adaptive music layer missing: " + stem + ".wav (run tools/reaper/render_pulse_music_headless.ps1)");
            toStereo(sample);
        }
    }
    {
        const std::string stem = "assets/audio/music/hub_bed";
        if (!loadSampleAny(stem, hubMusicBed_))
            throw std::runtime_error("Required hub music layer missing: " + stem + ".wav (run tools/reaper/render_pulse_music_headless.ps1)");
        toStereo(hubMusicBed_);
    }
    for (int i = 0; i < kMusicStingerCount; ++i) {
        const MusicStingerType type = static_cast<MusicStingerType>(i);
        const std::string stem = std::string("assets/audio/music/stinger_") + musicStingerName(type);
        if (!loadSampleAny(stem, musicStingers_[static_cast<size_t>(i)]))
            throw std::runtime_error("Required music stinger missing: " + stem + ".wav (run tools/reaper/render_pulse_music_headless.ps1)");
        toStereo(musicStingers_[static_cast<size_t>(i)]);
    }

    // Per-biome ambient beds (spec biome.audio). Soft assets: a missing bed just stays silent,
    // so the engine runs before tools/gen_ambient.py has been run. Kept MONO (read by readMono).
    {
        const char* bedStems[3] = {
            "assets/audio/sfx_ambient_foundry",
            "assets/audio/sfx_ambient_furnace",
            "assets/audio/sfx_ambient_reliquary",
        };
        for (int i = 0; i < 3; ++i) loadSampleAny(bedStems[i], ambientBeds_[static_cast<size_t>(i)]);
    }
}

void AudioSystem::play(SoundEventType type, float volume) {
    const int idx = static_cast<int>(type);
    if (idx < 0 || idx >= kSoundEventCount || sfx_[idx].data.empty()) return;
    // Fallback one-shots: lighter humanization than authored banks. enqueueVoice caps
    // concurrent voices so a storm of events cannot grow unbounded.
    enqueueVoice(sfx_[idx], volume, 0.45f, 1.2f);
}

void AudioSystem::playWeaponEvent(const std::string& weaponId, WeaponEventType event, float volume, int sequenceIndex) {
    switch (event) {
        case WeaponEventType::Fire:
            playFire(weaponId, volume, sequenceIndex);
            return;
        default:
            break;
    }

    if (!weaponId.empty()) {
        const char* eventName = weaponEventBankName(event);
        if (eventName[0] != '\0') {
            const auto it = namedWeaponEvents_.find(weaponEventKey(weaponId, eventName));
            if (it != namedWeaponEvents_.end() && !it->second.empty()) {
                playSampleBank(it->second, volume, sequenceIndex);
                return;
            }
        }
        const char* fallbackEvent = "";
        switch (event) {
            case WeaponEventType::MagOut:
            case WeaponEventType::Equip:
                fallbackEvent = "reload_start";
                break;
            case WeaponEventType::Bolt:
            case WeaponEventType::Shell:
                fallbackEvent = "reload_end";
                break;
            default:
                break;
        }
        if (fallbackEvent[0] != '\0') {
            const auto it = namedWeaponEvents_.find(weaponEventKey(weaponId, fallbackEvent));
            if (it != namedWeaponEvents_.end() && !it->second.empty()) {
                playSampleBank(it->second, volume, sequenceIndex);
                return;
            }
        }
    }

    switch (event) {
        case WeaponEventType::ReloadStart:
        case WeaponEventType::MagOut:
        case WeaponEventType::Equip:
            play(SoundEventType::ReloadStart, volume);
            return;
        case WeaponEventType::ReloadEnd:
        case WeaponEventType::MagIn:
        case WeaponEventType::Bolt:
        case WeaponEventType::Shell:
            play(SoundEventType::ReloadEnd, volume);
            return;
        default:
            return;
    }
}

void AudioSystem::playEnemyEvent(const std::string& enemyId, EnemyEventType event, float volume, int sequenceIndex) {
    playEnemyEventImpl(enemyId, event, volume, sequenceIndex, false, 0.0f, 0.0f);
}

void AudioSystem::playEnemyEvent(const std::string& enemyId, EnemyEventType event, float volume, int sequenceIndex,
                                 float emitterX, float emitterY) {
    playEnemyEventImpl(enemyId, event, volume, sequenceIndex, true, emitterX, emitterY);
}

void AudioSystem::playEnemyEventImpl(const std::string& enemyId, EnemyEventType event, float volume,
                                     int sequenceIndex, bool spatial, float ex, float ey) {
    if (!enemyId.empty()) {
        const char* eventName = enemyEventBankName(event);
        if (eventName[0] != '\0') {
            const auto it = namedEnemyEvents_.find(enemyEventKey(enemyId, eventName));
            if (it != namedEnemyEvents_.end() && !it->second.empty()) {
                if (spatial) playSampleBankSpatial(it->second, volume, sequenceIndex, ex, ey);
                else         playSampleBank(it->second, volume, sequenceIndex);
                return;
            }
            if (enemyId == "boss") {
                const auto tankIt = namedEnemyEvents_.find(enemyEventKey("tank", eventName));
                if (tankIt != namedEnemyEvents_.end() && !tankIt->second.empty()) {
                    if (spatial) playSampleBankSpatial(tankIt->second, volume * 0.85f, sequenceIndex, ex, ey);
                    else         playSampleBank(tankIt->second, volume * 0.85f, sequenceIndex);
                    return;
                }
            }
        }
    }

    switch (event) {
        case EnemyEventType::Telegraph:
        case EnemyEventType::Lunge:
            play(SoundEventType::Dash, volume * 0.65f);
            return;
        case EnemyEventType::Shot:
        case EnemyEventType::Impact:
        case EnemyEventType::Beam:
            play(SoundEventType::Hit, volume * 0.55f);
            return;
        case EnemyEventType::MeleeHit:
            play(SoundEventType::Hurt, volume * 0.60f);
            return;
        case EnemyEventType::Hurt:
            play(SoundEventType::Hit, volume * 0.55f);
            return;
        case EnemyEventType::Death:
        case EnemyEventType::BossBurst:
            play(SoundEventType::Kill, volume * 0.70f);
            return;
    }
}

void AudioSystem::playFeedback(FeedbackEventType event, float volume, int sequenceIndex) {
    const char* name = feedbackEventName(event);
    if (name[0] != '\0') {
        const auto it = namedFeedback_.find(name);
        if (it != namedFeedback_.end() && !it->second.empty()) {
            playSampleBank(it->second, volume, sequenceIndex);
            return;
        }
    }
    if (const Sample* s = proceduralFeedbackSample(event)) {
        enqueueVoice(*s, volume, 0.35f, 0.9f);
        return;
    }
    play(feedbackFallback(event), volume);
}

void AudioSystem::playFire(const std::string& bankId, float volume, int burstIndex) {
    const std::vector<Sample>* bank = &fireVariations_;
    if (!bankId.empty()) {
        const auto it = namedFire_.find(bankId);
        if (it == namedFire_.end() || it->second.empty()) return;
        bank = &it->second;
    }
    if (bank->empty()) return;
    playSampleBank(*bank, volume, burstIndex);
}

void AudioSystem::beginOfflineCapture(uint32_t sampleRate) {
    if (sampleRate < 8000) sampleRate = 48000;
    if (sfx_[0].data.empty()) loadBank();
    sampleRate_ = sampleRate;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        musicEnabled_ = false;
        musicVolume_ = 0.0f;
        musicIntensity_ = 0.0f;
        musicState_ = MusicState::Silent;
        musicOverpulseActive_ = false;
        musicBiome_ = MusicBiome::Foundry;
        musicCurBiome_ = 0;
        musicPrevBiome_ = -1;
        musicBiomeFade_ = 1.0f;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    offlineFrameCarry_ = 0.0;
    offlineStereo_.clear();
    offlineCapture_ = true;
}

void AudioSystem::advanceOfflineCapture(float seconds) {
    if (!offlineCapture_ || seconds <= 0.0f) return;
    const double wanted = static_cast<double>(seconds) * static_cast<double>(sampleRate_) + offlineFrameCarry_;
    const uint32_t frames = static_cast<uint32_t>(std::floor(wanted));
    offlineFrameCarry_ = wanted - static_cast<double>(frames);
    if (frames == 0) return;

    std::vector<float> scratch(static_cast<size_t>(frames) * 2u, 0.0f);
    mix(scratch.data(), frames);
    offlineStereo_.insert(offlineStereo_.end(), scratch.begin(), scratch.end());
}

bool AudioSystem::writeOfflineCaptureWav(const std::string& path) {
    if (!offlineCapture_) return false;
    if (offlineFrameCarry_ > 0.000001) {
        std::vector<float> scratch(2u, 0.0f);
        mix(scratch.data(), 1);
        offlineStereo_.insert(offlineStereo_.end(), scratch.begin(), scratch.end());
        offlineFrameCarry_ = 0.0;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t frames = static_cast<uint32_t>(offlineStereo_.size() / 2u);
    const uint32_t dataBytes = frames * 2u * 2u;
    writeWavHeaderN(f, sampleRate_, 2, dataBytes);
    for (float s : offlineStereo_) {
        const float c = std::clamp(s, -1.0f, 1.0f);
        const int16_t v = static_cast<int16_t>(std::lrint(c * 32767.0f));
        std::fwrite(&v, sizeof(v), 1, f);
    }
    std::fclose(f);
    return true;
}

void AudioSystem::setMusic(bool enabled, float bpm, float baseVolume, float intensity) {
    setMusicState(enabled, bpm, baseVolume, intensity, MusicState::Combat);
}

void AudioSystem::setMusicState(bool enabled, float bpm, float baseVolume, float intensity, MusicState state) {
    setMusicContext(enabled, bpm, baseVolume, intensity, state, MusicBiome::Foundry, false);
}

void AudioSystem::setMusicContext(bool enabled, float bpm, float baseVolume, float intensity,
                                  MusicState state, MusicBiome biome) {
    setMusicContext(enabled, bpm, baseVolume, intensity, state, biome, false);
}

void AudioSystem::setMusicContext(bool enabled, float bpm, float baseVolume, float intensity,
                                  MusicState state, MusicBiome biome, bool overpulseActive) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Legacy (v3) path: duress=0, bossEscalation=(Boss?1:0), immediate transitions, full-band duck.
    const float legacyEsc = (state == MusicState::Boss) ? 1.0f : 0.0f;
    updateMusicContextLocked(enabled, bpm, baseVolume, intensity, state, biome, overpulseActive,
                             0.0f, legacyEsc, false);
}

void AudioSystem::setMusicContext(const MusicContext& ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    updateMusicContextLocked(ctx.enabled, ctx.bpm, ctx.baseVolume, ctx.intensity, ctx.state, ctx.biome,
                             ctx.overpulseActive, ctx.duress, ctx.bossEscalation, true);
}

void AudioSystem::setMixOptions(float musicDuckDepth, float readabilityBoost,
                                bool reducedIntensity, bool monoDownmix) {
    musicDuckDepth_.store(std::clamp(musicDuckDepth, 0.0f, 1.0f), std::memory_order_relaxed);
    readabilityBoost_.store(readabilityBoost < 0.0f ? 0.0f : readabilityBoost, std::memory_order_relaxed);
    reducedIntensity_.store(reducedIntensity ? 1 : 0, std::memory_order_relaxed);
    monoDownmix_.store(monoDownmix ? 1 : 0, std::memory_order_relaxed);
}

void AudioSystem::updateMusicContextLocked(bool enabled, float bpm, float baseVolume, float intensity,
                                           MusicState state, MusicBiome biome, bool overpulseActive,
                                           float duress, float bossEscalation, bool v4) {
    const MusicState nextState = enabled ? state : MusicState::Silent;
    musicV4_ = v4;

    auto cullForRunOver = [&]() {
        constexpr int kRunPriority = 100;
        for (MusicStingerVoice& v : musicStingerVoices_) {
            if (!v.active) continue;
            const bool queued = musicPos_ + 0.5 < v.startFrame;
            if (queued || v.priority < kRunPriority) v.active = false;
        }
    };

    if (!v4) {
        // v3: structural transitions apply immediately (byte-identical legacy behavior).
        if (nextState != musicState_) {
            musicStateAgeFrames_ = 0.0;
            if (nextState == MusicState::RunOver) cullForRunOver();
        }
        musicState_ = nextState;
        musicRequestedState_ = nextState;
        musicPendingApplyFrame_ = -1.0;
    } else if (nextState != musicRequestedState_) {
        // v4 (M2): quantize the swap to the next musical boundary (beat for most, bar for ->Boss).
        // RunOver stays immediate; re-requesting the applied state cancels a pending swap.
        musicRequestedState_ = nextState;
        if (nextState == MusicState::RunOver) {
            if (nextState != musicState_) { musicStateAgeFrames_ = 0.0; cullForRunOver(); }
            musicState_ = nextState;
            musicPendingApplyFrame_ = -1.0;
        } else if (nextState == musicState_) {
            musicPendingApplyFrame_ = -1.0;
        } else {
            const double outRate = static_cast<double>(sampleRate_ ? sampleRate_ : 48000);
            const double beat = outRate * 60.0 / static_cast<double>(std::max(1.0f, bpm));
            const double boundary = (nextState == MusicState::Boss) ? beat * 4.0 : beat;
            double apply = musicPos_;
            if (boundary > 1.0) {
                apply = std::ceil((musicPos_ + 1.0) / boundary) * boundary;
                const double maxAhead = musicPos_ + beat * 4.0;   // never lag more than ~1 bar
                if (apply > maxAhead) apply = maxAhead;
            }
            musicPendingApplyFrame_ = apply;
        }
    }

    musicEnabled_ = enabled;
    musicBpm_ = bpm;
    musicVolume_ = enabled ? baseVolume : 0.0f;
    musicIntensity_ = intensity;
    musicOverpulseActive_ = enabled && overpulseActive;
    musicDuress_ = enabled ? std::clamp(duress, 0.0f, 1.0f) : 0.0f;   // continuous: slewed in mix()
    musicBossEscalation_ = std::clamp(bossEscalation, 0.0f, 1.0f);
    musicBiome_ = biome;
    const int nextBiome = std::clamp(static_cast<int>(biome), 0, kMusicBiomeCount - 1);
    if (nextBiome != musicCurBiome_) {
        musicPrevBiome_ = musicCurBiome_;
        musicCurBiome_ = nextBiome;
        musicBiomeFade_ = 0.0f;
        if (enabled && nextState != MusicState::Silent && nextState != MusicState::RunOver) {
            scheduleMusicStingerLocked(sectorStingerForBiome(nextBiome), 0.70f, true);
        }
    }
}

void AudioSystem::playMusicStinger(MusicStingerType type, float volume, bool quantizeToBar) {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduleMusicStingerLocked(type, volume, quantizeToBar);
}

bool AudioSystem::scheduleMusicStingerLocked(MusicStingerType type, float volume, bool quantizeToBar) {
    const int idx = static_cast<int>(type);
    if (idx < 0 || idx >= kMusicStingerCount) return false;
    const Sample& sample = musicStingers_[static_cast<size_t>(idx)];
    if (sample.data.empty() || volume <= 0.0f) return false;

    const double outRate = static_cast<double>(sampleRate_ ? sampleRate_ : 48000);
    if (type == MusicStingerType::RoomClear || type == MusicStingerType::Reward) {
        const double last = musicStingerLastStart_[static_cast<size_t>(idx)];
        if ((musicPos_ - last) < outRate * 2.0) return false;
    }

    const int priority = musicStingerPriority(type);
    for (MusicStingerVoice& v : musicStingerVoices_) {
        if (!v.active) continue;
        if (v.priority < priority) v.active = false;
    }

    double startFrame = musicPos_;
    if (quantizeToBar) {
        const double bpm = std::max(1.0f, musicBpm_);
        const double barFrames = outRate * (60.0 / bpm) * 4.0;
        if (barFrames > 1.0) {
            const double bars = std::ceil(musicPos_ / barFrames);
            startFrame = bars * barFrames;
        }
    }

    MusicStingerVoice* slot = nullptr;
    for (MusicStingerVoice& v : musicStingerVoices_) {
        if (!v.active) { slot = &v; break; }
    }
    if (!slot) {
        for (MusicStingerVoice& v : musicStingerVoices_) {
            if (!slot || v.priority < slot->priority) slot = &v;
        }
        if (!slot || slot->priority > priority) return false;
    }
    slot->sample = &sample;
    slot->pos = 0.0;
    slot->startFrame = startFrame;
    slot->volume = volume;
    slot->priority = priority;
    slot->type = type;
    slot->active = true;
    musicStingerLastStart_[static_cast<size_t>(idx)] = startFrame;
    return true;
}

void AudioSystem::mix(float* out, uint32_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t ch = channels_ < 1 ? 1 : channels_;
    ensureReverb();

    // Linear-interpolated mono read of a sample at a fractional frame position.
    auto readMono = [](const Sample& s, double pos) -> float {
        const uint32_t n = s.frames(); if (n == 0) return 0.0f;
        const uint32_t i0 = static_cast<uint32_t>(pos);
        const uint32_t i1 = i0 + 1 < n ? i0 + 1 : n - 1;
        const float f = static_cast<float>(pos - i0);
        return s.data[i0] * (1.0f - f) + s.data[i1] * f;
    };
    auto readStereoLoop = [](const Sample& s, double pos, float& l, float& r) {
        const uint32_t n = s.frames(); if (n == 0) { l = r = 0; return; }
        pos = std::fmod(std::max(0.0, pos), static_cast<double>(n));
        const uint32_t i0 = static_cast<uint32_t>(pos);
        const uint32_t i1 = i0 + 1 < n ? i0 + 1 : 0;
        const float f = static_cast<float>(pos - i0);
        l = s.data[i0 * 2]     * (1.0f - f) + s.data[i1 * 2]     * f;
        r = s.data[i0 * 2 + 1] * (1.0f - f) + s.data[i1 * 2 + 1] * f;
    };
    auto readStereoOneShot = [](const Sample& s, double pos, float& l, float& r) {
        const uint32_t n = s.frames(); if (n == 0 || pos < 0.0 || pos >= static_cast<double>(n)) { l = r = 0; return; }
        const uint32_t i0 = static_cast<uint32_t>(pos);
        const uint32_t i1 = i0 + 1 < n ? i0 + 1 : n - 1;
        const float f = static_cast<float>(pos - i0);
        l = s.data[i0 * 2]     * (1.0f - f) + s.data[i1 * 2]     * f;
        r = s.data[i0 * 2 + 1] * (1.0f - f) + s.data[i1 * 2 + 1] * f;
    };

    const float intensity = std::clamp(musicIntensity_, 0.0f, 1.0f);
    const float bossEsc = std::clamp(musicBossEscalation_, 0.0f, 1.0f);   // v4 (M3): 0 phase-1 .. 1 enrage
    const float duressInput = std::clamp(musicDuress_, 0.0f, 1.0f);       // v4 (M1): 0 healthy .. 1 near-death
    const double outRate = static_cast<double>(sampleRate_ ? sampleRate_ : 48000);
    auto computeMusicTargets = [&](double stateAgeFrames) {
        std::array<float, kMusicLayerCount> targets{};
        if (!musicEnabled_) return targets;
        const float stateAgeSec = static_cast<float>(stateAgeFrames / std::max(1.0, outRate));
        // v4 (C1): composed duress stem (idx 6) opens with near-death tension, in combat states
        // only. duress is 0 outside combat and always 0 in v3, so this stays silent for legacy.
        const float duressLayer = smooth01(0.12f, 0.85f, duressInput);
        switch (musicState_) {
            case MusicState::Hub:
                targets = { 0.36f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f };
                break;
            case MusicState::Reward: {
                const float breath = smooth01(0.60f, 1.05f, stateAgeSec);
                targets = { 0.58f * breath, 0.14f * breath, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f };
                break;
            }
            case MusicState::Boss:
                if (musicV4_) {
                    // v4 (M3): layers open with escalation instead of being hard-full; >0.8 is the
                    // enrage band (overpulse opens). The overpulse floor keeps the v3 gate behavior.
                    const float over = std::max(musicOverpulseActive_ ? 0.72f : 0.00f,
                                                smooth01(0.78f, 1.00f, bossEsc));
                    targets = {
                        1.00f, 0.95f,
                        0.80f + 0.20f * bossEsc,   // drums escalate
                        0.45f + 0.55f * bossEsc,   // pressure opens with escalation
                        0.70f + 0.30f * bossEsc,   // boss layer swells
                        over,
                        duressLayer
                    };
                } else {
                    // v3: all boss layers hard-full (byte-identical legacy behavior).
                    targets = { 1.00f, 0.95f, 0.92f, 0.82f, 1.00f, musicOverpulseActive_ ? 0.72f : 0.00f, 0.00f };
                }
                break;
            case MusicState::Combat:
                targets = {
                    1.00f,
                    smooth01(0.12f, 0.44f, intensity),
                    0.18f + 0.82f * smooth01(0.34f, 0.72f, intensity),
                    smooth01(0.58f, 0.94f, intensity),
                    0.00f,
                    musicOverpulseActive_ ? 0.78f * smooth01(0.62f, 0.96f, intensity) : 0.00f,
                    duressLayer
                };
                break;
            case MusicState::RunOver:
            case MusicState::Silent:
                targets = {};
                break;
        }
        return targets;
    };
    const float gainSlew = static_cast<float>(1.0 - std::exp(-1.0 / (outRate * 0.22)));
    const float biomeFadeStep = static_cast<float>(1.0 / (outRate * 0.50));
    const float sfxGain = sfxGain_.load(std::memory_order_relaxed);   // settings SFX bus volume

    // SFX bus tuning. The summed one-shot bus is sent (quietly) to the shared reverb
    // for "glue" and width, and its envelope ducks the music so shots punch through
    // the bed instead of fighting it.
    constexpr float kSfxBusGain   = 0.6f;    // preserves the previous dry SFX level
    constexpr float kReverbInGain = 0.016f;  // Freeverb-style fixed input gain (subtle tail)
    constexpr float kDuckDepth    = 0.5f;    // max music attenuation (~-6 dB) under loud SFX
    constexpr float kDuckSens     = 2.5f;    // how readily the SFX bus triggers ducking
    const float duckAtk = static_cast<float>(1.0 - std::exp(-1.0 / (outRate * 0.008)));  // ~8 ms
    const float duckRel = static_cast<float>(1.0 - std::exp(-1.0 / (outRate * 0.30)));   // ~300 ms

    // v4 frequency-tilted duck (S1): split the music sum at ~120 Hz and duck the high band hard
    // (gunshot/confirm clarity) while the low band (kick/sub) stays solid. v3 keeps the full-band duck.
    const float aDuckSplit = static_cast<float>(1.0 - std::exp(-6.2831853 * 120.0 / outRate));
    constexpr float kDuckDepthLow  = 0.12f;   // ~-1.1 dB on the low band
    constexpr float kDuckDepthHigh = 0.55f;   // ~-6.9 dB on the high band (scaled by the duck-depth setting)
    const float duckDepthSetting = std::clamp(musicDuckDepth_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const bool  reduced = reducedIntensity_.load(std::memory_order_relaxed) != 0;   // caps submerge + enrage
    const bool  monoMix = monoDownmix_.load(std::memory_order_relaxed) != 0;        // sum L/R at output

    for (uint32_t i = 0; i < frames; ++i) {
        // v4 (M2): apply a beat/bar-quantized structural transition once its boundary frame is
        // reached, so the Reward breath / re-entry / boss swell land on the beat instead of slewing
        // mid-phrase. Continuous inputs (intensity/duress/escalation/overpulse) still apply smoothly.
        if (musicPendingApplyFrame_ >= 0.0 && musicPos_ >= musicPendingApplyFrame_) {
            if (musicState_ != musicRequestedState_) {
                musicState_ = musicRequestedState_;
                musicStateAgeFrames_ = 0.0;
            }
            musicPendingApplyFrame_ = -1.0;
        }
        musicDuressSm_ += (duressInput - musicDuressSm_) * gainSlew;   // slewed, never quantized
        const std::array<float, kMusicLayerCount> musicTargets = computeMusicTargets(musicStateAgeFrames_);
        float musL = 0.0f, musR = 0.0f;
        const float biomeFade = std::clamp(musicBiomeFade_, 0.0f, 1.0f);
        const float curBiomeGain = (musicPrevBiome_ >= 0 && biomeFade < 1.0f) ? std::sqrt(biomeFade) : 1.0f;
        const float prevBiomeGain = (musicPrevBiome_ >= 0 && biomeFade < 1.0f) ? std::sqrt(1.0f - biomeFade) : 0.0f;
        for (int layer = 0; layer < kMusicLayerCount; ++layer) {
            const size_t idx = static_cast<size_t>(layer);
            musicLayerGains_[idx] += (musicTargets[idx] - musicLayerGains_[idx]) * gainSlew;
            const float layerGain = musicLayerGains_[idx];
            if (layerGain > 0.0001f) {
                float ml = 0.0f, mr = 0.0f;
                if (musicState_ == MusicState::Hub && layer == 0 && hubMusicBed_.frames() > 0) {
                    const double pos = musicPos_ * static_cast<double>(hubMusicBed_.rate) / outRate;
                    readStereoLoop(hubMusicBed_, pos, ml, mr);
                } else if (musicState_ != MusicState::Hub && musicCurBiome_ >= 0 && musicCurBiome_ < kMusicBiomeCount) {
                    const Sample& cur = musicLayers_[static_cast<size_t>(musicCurBiome_)][idx];
                    if (cur.frames() > 0) {
                        float cl, cr;
                        const double pos = musicPos_ * static_cast<double>(cur.rate) / outRate;
                        readStereoLoop(cur, pos, cl, cr);
                        ml += cl * curBiomeGain;
                        mr += cr * curBiomeGain;
                    }
                    if (musicPrevBiome_ >= 0 && musicPrevBiome_ < kMusicBiomeCount && prevBiomeGain > 0.0f) {
                        const Sample& prev = musicLayers_[static_cast<size_t>(musicPrevBiome_)][idx];
                        if (prev.frames() > 0) {
                            float pl, pr;
                            const double pos = musicPos_ * static_cast<double>(prev.rate) / outRate;
                            readStereoLoop(prev, pos, pl, pr);
                            ml += pl * prevBiomeGain;
                            mr += pr * prevBiomeGain;
                        }
                    }
                }
                float layerVol = musicVolume_ * layerGain;
                // v4 (M1): trim the bright layers (pressure idx 3, overpulse idx 5) as duress rises,
                // so the world reads as submerged rather than just darker.
                if (musicV4_ && (layer == 3 || layer == 5)) layerVol *= (1.0f - 0.5f * musicDuressSm_);
                musL += ml * layerVol;
                musR += mr * layerVol;
            }
        }
        if (musicBiomeFade_ < 1.0f) {
            musicBiomeFade_ = std::min(1.0f, musicBiomeFade_ + biomeFadeStep);
            if (musicBiomeFade_ >= 1.0f) musicPrevBiome_ = -1;
        }
        bool anyStingerActive = false;
        for (MusicStingerVoice& v : musicStingerVoices_) {
            if (!v.active || !v.sample) continue;
            anyStingerActive = true;
            if (musicPos_ + 0.5 < v.startFrame) continue;
            float sl = 0.0f, sr = 0.0f;
            readStereoOneShot(*v.sample, v.pos, sl, sr);
            musL += sl * v.volume;
            musR += sr * v.volume;
            v.pos += static_cast<double>(v.sample->rate) / outRate;
            if (v.pos >= static_cast<double>(v.sample->frames())) v.active = false;
        }
        bool anyLayerTail = false;
        for (float g : musicLayerGains_) anyLayerTail = anyLayerTail || g > 0.0001f;
        if (musicEnabled_ || anyLayerTail || anyStingerActive) {
            musicPos_ += 1.0;
            musicStateAgeFrames_ += 1.0;
        }

        // v4 (M1): low-health bus treatment, applied to the whole music bus just before the duck.
        // A one-pole low-pass sweeps the world down toward ~800 Hz as duress rises, and a
        // deterministic sub "heartbeat" (~55 Hz, two-thump) fades in. Reversible: at duress 0 this
        // is a transparent pass-through, and the whole block is gated off in v3.
        if (musicV4_ && musicDuressSm_ > 0.0008f) {
            const float duressFloorHz = reduced ? 1800.0f : 800.0f;
            const float cutoff = 18000.0f * std::pow(duressFloorHz / 18000.0f,
                                                     std::clamp(musicDuressSm_, 0.0f, 1.0f));
            const float aDuress = static_cast<float>(1.0 - std::exp(-6.2831853 * cutoff / outRate));
            musDuressLpL_ += aDuress * (musL - musDuressLpL_);
            musDuressLpR_ += aDuress * (musR - musDuressLpR_);
            musL = musDuressLpL_;
            musR = musDuressLpR_;
            const float hbGain = (reduced ? 0.28f : 0.5f) * musicDuressSm_;
            const double period = outRate * 0.92;            // ~65 bpm heartbeat
            const double php = std::fmod(musicPos_, period) / std::max(1.0, period);
            const float lub = std::exp(-static_cast<float>((php - 0.04) * (php - 0.04)) / 0.0024f);
            const float dub = 0.6f * std::exp(-static_cast<float>((php - 0.20) * (php - 0.20)) / 0.0024f);
            const float hb = std::sin(static_cast<float>(6.2831853 * 55.0 * (musicPos_ / outRate)))
                           * (lub + dub) * hbGain;
            musL += hb;
            musR += hb;
        } else {
            musDuressLpL_ = musL;   // keep the filter state warm so re-engaging does not click
            musDuressLpR_ = musR;
        }

        float sfxL = 0.0f, sfxR = 0.0f;
        for (Voice& v : voices_) {
            if (!v.active) continue;
            float m = readMono(*v.sample, v.pos) * v.volume;
            if (v.spatial) {
                v.lpZ += v.lpCoef * (m - v.lpZ);                       // distance / air low-pass
                m = v.lpZ;
                sfxL += m * v.gainL;
                sfxR += m * v.gainR;
            } else {
                sfxL += m; sfxR += m;                                  // centred (player/UI bus)
            }
            v.pos += v.step;
            if (v.pos >= v.sample->frames()) v.active = false;
        }
        const float busGain = kSfxBusGain * sfxGain;
        sfxL *= busGain;
        sfxR *= busGain;
        const float sfxMonoFx = (sfxL + sfxR) * 0.5f;                  // reverb send + duck key are mono

        // Shared reverb send (kept subtle): one room for every cue.
        float wetL = 0.0f, wetR = 0.0f;
        reverb_->process(sfxMonoFx * kReverbInGain, wetL, wetR);

        // Sidechain: follow the SFX bus envelope and duck the music under it.
        const float lvl = std::fabs(sfxMonoFx);
        sfxEnv_ += (lvl - sfxEnv_) * (lvl > sfxEnv_ ? duckAtk : duckRel);
        const float duckTarget = std::min(1.0f, sfxEnv_ * kDuckSens);
        duck_ += (duckTarget - duck_) * (duckTarget > duck_ ? duckAtk : duckRel);
        const float musicGain = 1.0f - kDuckDepth * duck_;

        // Per-biome ambient bed: a steady looping pad (not ducked), crossfaded on biome change.
        float ambL = 0.0f, ambR = 0.0f;
        {
            const int tb = ambientBiome_.load(std::memory_order_relaxed);
            const float want = (ambientCurBiome_ == tb) ? ambientTargetGain_.load(std::memory_order_relaxed) : 0.0f;
            ambientGain_ += (want - ambientGain_) * gainSlew;
            if (ambientCurBiome_ != tb && ambientGain_ < 0.0015f) { ambientCurBiome_ = tb; ambientPos_ = 0.0; }
            if (ambientCurBiome_ >= 0 && ambientCurBiome_ < 3 && ambientGain_ > 0.0001f) {
                const Sample& bed = ambientBeds_[static_cast<size_t>(ambientCurBiome_)];
                if (bed.frames() > 0) {
                    const double posBed = std::fmod(ambientPos_ * static_cast<double>(bed.rate) / outRate,
                                                    static_cast<double>(bed.frames()));
                    const float m = readMono(bed, posBed) * ambientGain_;
                    ambL += m; ambR += m;
                    ambientPos_ += 1.0;
                }
            }
        }

        float L, R;
        if (musicV4_) {
            // Frequency-tilted duck: one-pole split at ~120 Hz, low band ducks gently, high band hard.
            duckSplitLpL_ += aDuckSplit * (musL - duckSplitLpL_);
            duckSplitLpR_ += aDuckSplit * (musR - duckSplitLpR_);
            const float lowL = duckSplitLpL_, highL = musL - lowL;
            const float lowR = duckSplitLpR_, highR = musR - lowR;
            const float enrageAmt = (musicState_ == MusicState::Boss) ? smooth01(0.80f, 1.00f, bossEsc) : 0.0f;
            const float gLow  = 1.0f - kDuckDepthLow * duck_;
            const float gHigh = std::max(0.0f, 1.0f - kDuckDepthHigh * duckDepthSetting
                                                       * (1.0f + 0.30f * enrageAmt) * duck_);  // enrage deepens the pump
            L = (lowL * gLow + highL * gHigh) + sfxL + wetL + ambL;
            R = (lowR * gLow + highR * gHigh) + sfxR + wetR + ambR;
        } else {
            L = musL * musicGain + sfxL + wetL + ambL;
            R = musR * musicGain + sfxR + wetR + ambR;
        }
        if (monoMix) { const float m = (L + R) * 0.5f; L = m; R = m; }

        constexpr float kMasterDrive = 0.82f;
        constexpr float kOutputCeiling = 0.92f;
        L = std::tanh(L * kMasterDrive) * kOutputCeiling;
        R = std::tanh(R * kMasterDrive) * kOutputCeiling;
        out[i * ch + 0] = L;
        if (ch > 1) out[i * ch + 1] = R;
        for (uint32_t c = 2; c < ch; ++c) out[i * ch + c] = 0.0f;     // stereo on front L/R
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                  [](const Voice& v) { return !v.active; }), voices_.end());
}

bool AudioSystem::initDevice() {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_)))) return false;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_))) return false;
    if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&client_)))) return false;

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix))) return false;
    sampleRate_ = mix->nSamplesPerSec;
    channels_ = mix->nChannels;
    const bool isFloat32 = (mix->wBitsPerSample == 32);   // shared-mode mix is float32

    const REFERENCE_TIME dur = 20 * 10000;   // 20 ms
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     dur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr) || !isFloat32) return false;   // only the float32 path is supported

    bufferEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!bufferEvent_ || FAILED(client_->SetEventHandle(static_cast<HANDLE>(bufferEvent_)))) return false;
    if (FAILED(client_->GetBufferSize(&bufferFrames_))) return false;
    if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient_)))) return false;

    return SUCCEEDED(client_->Start());
}

void AudioSystem::shutdownDevice() {
    if (client_) client_->Stop();
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (bufferEvent_) { CloseHandle(static_cast<HANDLE>(bufferEvent_)); bufferEvent_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
}

void AudioSystem::renderThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!initDevice()) {
        logWarn("audio: no WASAPI device; running silent");
        shutdownDevice();
        CoUninitialize();
        return;
    }
    logInfo("audio: WASAPI up (%u Hz, %u ch, %u-frame buffer)", sampleRate_, channels_, bufferFrames_);

    // Run the render thread under MMCSS "Pro Audio" + time-critical priority so the
    // GPU-bound game cannot starve it. Starvation underruns the buffer, which wipes
    // short transients (a gunshot crack) entirely while only nicking the continuous
    // music bed -- the cause of "only some shots are audible".
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<float> scratch(static_cast<size_t>(bufferFrames_) * channels_);

    while (running_) {
        if (WaitForSingleObject(static_cast<HANDLE>(bufferEvent_), 100) != WAIT_OBJECT_0) continue;
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) break;
        const UINT32 avail = bufferFrames_ - padding;
        if (avail == 0) continue;
        BYTE* data = nullptr;
        if (FAILED(renderClient_->GetBuffer(avail, &data))) break;
        mix(scratch.data(), avail);
        std::memcpy(data, scratch.data(), static_cast<size_t>(avail) * channels_ * sizeof(float));
        renderClient_->ReleaseBuffer(avail, 0);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    shutdownDevice();
    CoUninitialize();
}

bool AudioSystem::renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume, float intensity) {
    return renderMusicToWav(path, seconds, bpm, baseVolume, intensity, MusicState::Combat, MusicBiome::Foundry);
}

bool AudioSystem::renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                                   float intensity, MusicState state, MusicBiome biome) {
    return renderMusicToWav(path, seconds, bpm, baseVolume, intensity, state, biome, false);
}

bool AudioSystem::renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                                   float intensity, MusicState state, MusicBiome biome, bool overpulseActive) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(1.0f, seconds) * sr);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    if (sfx_[0].data.empty()) loadBank();
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        const int biomeIndex = std::clamp(static_cast<int>(biome), 0, kMusicBiomeCount - 1);
        musicBiome_ = biome;
        musicOverpulseActive_ = false;
        musicCurBiome_ = biomeIndex;
        musicPrevBiome_ = -1;
        musicBiomeFade_ = 1.0f;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    setMusicContext(true, bpm, baseVolume, intensity, state, biome, overpulseActive);

    constexpr uint32_t kChunk = 2048;
    std::vector<float> scratch(static_cast<size_t>(kChunk) * 2u, 0.0f);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kChunk, frames - done);
        std::fill(scratch.begin(), scratch.begin() + static_cast<size_t>(n) * 2u, 0.0f);
        mix(scratch.data(), n);
        for (uint32_t i = 0; i < n * 2u; ++i) {
            const float c = std::clamp(scratch[i], -1.0f, 1.0f);
            const int16_t v = static_cast<int16_t>(std::lrint(c * 32767.0f));
            std::fwrite(&v, sizeof(v), 1, f);
        }
        done += n;
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                                   float intensity, MusicState state, MusicBiome biome, bool overpulseActive,
                                   float duress, float bossEscalation) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(1.0f, seconds) * sr);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    if (sfx_[0].data.empty()) loadBank();
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        const int biomeIndex = std::clamp(static_cast<int>(biome), 0, kMusicBiomeCount - 1);
        musicBiome_ = biome;
        musicOverpulseActive_ = false;
        musicCurBiome_ = biomeIndex;
        musicPrevBiome_ = -1;
        musicBiomeFade_ = 1.0f;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    // v4 path: build a MusicContext so the duress submerge + heartbeat, quantized transitions, and
    // boss escalation are exercised (used by --music-duress / --music-boss-escalation and the report).
    MusicContext ctx;
    ctx.enabled = true;
    ctx.bpm = bpm;
    ctx.baseVolume = baseVolume;
    ctx.intensity = intensity;
    ctx.state = state;
    ctx.biome = biome;
    ctx.overpulseActive = overpulseActive;
    ctx.duress = duress;
    ctx.bossEscalation = bossEscalation;
    setMusicContext(ctx);

    constexpr uint32_t kChunk = 2048;
    std::vector<float> scratch(static_cast<size_t>(kChunk) * 2u, 0.0f);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kChunk, frames - done);
        std::fill(scratch.begin(), scratch.begin() + static_cast<size_t>(n) * 2u, 0.0f);
        mix(scratch.data(), n);
        for (uint32_t i = 0; i < n * 2u; ++i) {
            const float c = std::clamp(scratch[i], -1.0f, 1.0f);
            const int16_t v = static_cast<int16_t>(std::lrint(c * 32767.0f));
            std::fwrite(&v, sizeof(v), 1, f);
        }
        done += n;
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::renderMusicStingerToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                                          MusicStingerType type, MusicBiome biome, bool quantizeToBar) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(1.0f, seconds) * sr);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    if (sfx_[0].data.empty()) loadBank();
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        const int biomeIndex = std::clamp(static_cast<int>(biome), 0, kMusicBiomeCount - 1);
        musicBiome_ = biome;
        musicOverpulseActive_ = false;
        musicCurBiome_ = biomeIndex;
        musicPrevBiome_ = -1;
        musicBiomeFade_ = 1.0f;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    setMusicContext(true, bpm, baseVolume * 0.55f, 0.0f, MusicState::Reward, biome);
    playMusicStinger(type, 1.0f, quantizeToBar);

    constexpr uint32_t kChunk = 2048;
    std::vector<float> scratch(static_cast<size_t>(kChunk) * 2u, 0.0f);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kChunk, frames - done);
        std::fill(scratch.begin(), scratch.begin() + static_cast<size_t>(n) * 2u, 0.0f);
        mix(scratch.data(), n);
        for (uint32_t i = 0; i < n * 2u; ++i) {
            const float c = std::clamp(scratch[i], -1.0f, 1.0f);
            const int16_t v = static_cast<int16_t>(std::lrint(c * 32767.0f));
            std::fwrite(&v, sizeof(v), 1, f);
        }
        done += n;
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::renderShotsToWav(const std::string& path, float seconds, SoundEventType type, float fireRate, float volume,
                                   const std::string& fireBankId) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(0.5f, seconds) * sr);
    const uint32_t interval = static_cast<uint32_t>(sr / std::max(0.5f, fireRate));
    try {
        if (sfx_[0].data.empty()) loadBank();
    } catch (...) {
        return false;
    }
    if (type == SoundEventType::Fire && !fireBankId.empty()) {
        const auto it = namedFire_.find(fireBankId);
        if (it == namedFire_.end() || it->second.empty()) return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        musicEnabled_ = false;
        musicVolume_ = 0.0f;
        musicIntensity_ = 0.0f;
        musicState_ = MusicState::Silent;
        musicOverpulseActive_ = false;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    int shotIndex = 0;
    float stereo[2]{};
    for (uint32_t i = 0; i < frames; ++i) {
        if (i % interval == 0) {
            if (type == SoundEventType::Fire) playFire(fireBankId, volume, shotIndex++);
            else play(type, volume);
        }
        mix(stereo, 1);
        const int16_t l = static_cast<int16_t>(std::clamp(stereo[0], -1.0f, 1.0f) * 32767.0f);
        const int16_t r = static_cast<int16_t>(std::clamp(stereo[1], -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&l, 2, 1, f); std::fwrite(&r, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::renderWeaponEventToWav(const std::string& path, float seconds, const std::string& weaponId,
                                          WeaponEventType event, float eventRate, float volume) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(0.5f, seconds) * sr);
    const uint32_t interval = static_cast<uint32_t>(sr / std::max(0.5f, eventRate));
    try {
        if (sfx_[0].data.empty()) loadBank();
    } catch (...) {
        return false;
    }
    const char* eventName = weaponEventBankName(event);
    if (weaponId.empty() || eventName[0] == '\0') return false;
    const auto bankIt = namedWeaponEvents_.find(weaponEventKey(weaponId, eventName));
    if (bankIt == namedWeaponEvents_.end() || bankIt->second.empty()) return false;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        musicEnabled_ = false;
        musicVolume_ = 0.0f;
        musicIntensity_ = 0.0f;
        musicState_ = MusicState::Silent;
        musicOverpulseActive_ = false;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    int eventIndex = 0;
    float stereo[2]{};
    for (uint32_t i = 0; i < frames; ++i) {
        if (i % interval == 0) {
            playWeaponEvent(weaponId, event, volume, eventIndex++);
        }
        mix(stereo, 1);
        const int16_t l = static_cast<int16_t>(std::clamp(stereo[0], -1.0f, 1.0f) * 32767.0f);
        const int16_t r = static_cast<int16_t>(std::clamp(stereo[1], -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&l, 2, 1, f); std::fwrite(&r, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::renderEnemyEventToWav(const std::string& path, float seconds, const std::string& enemyId,
                                        EnemyEventType event, float eventRate, float volume,
                                        bool spatial, float emitterX, float emitterY) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(0.5f, seconds) * sr);
    const uint32_t interval = static_cast<uint32_t>(sr / std::max(0.5f, eventRate));
    try {
        if (sfx_[0].data.empty()) loadBank();
    } catch (...) {
        return false;
    }
    const char* eventName = enemyEventBankName(event);
    if (enemyId.empty() || eventName[0] == '\0') return false;
    const auto bankIt = namedEnemyEvents_.find(enemyEventKey(enemyId, eventName));
    if (bankIt == namedEnemyEvents_.end() || bankIt->second.empty()) return false;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        musicEnabled_ = false;
        musicVolume_ = 0.0f;
        musicIntensity_ = 0.0f;
        musicState_ = MusicState::Silent;
        musicOverpulseActive_ = false;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    // Spatial QA render: listener at origin facing +Y (right = rightFromForward = -X),
    // so --emitter places the threat off-centre and the L/R balance is measurable.
    if (spatial) setListener(0.0f, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f);
    int eventIndex = 0;
    float stereo[2]{};
    for (uint32_t i = 0; i < frames; ++i) {
        if (i % interval == 0) {
            if (spatial) playEnemyEvent(enemyId, event, volume, eventIndex++, emitterX, emitterY);
            else         playEnemyEvent(enemyId, event, volume, eventIndex++);
        }
        mix(stereo, 1);
        const int16_t l = static_cast<int16_t>(std::clamp(stereo[0], -1.0f, 1.0f) * 32767.0f);
        const int16_t r = static_cast<int16_t>(std::clamp(stereo[1], -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&l, 2, 1, f); std::fwrite(&r, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::renderFeedbackEventToWav(const std::string& path, float seconds, FeedbackEventType event,
                                           float eventRate, float volume) {
    const uint32_t sr = 48000;
    const uint32_t frames = static_cast<uint32_t>(std::max(0.5f, seconds) * sr);
    const uint32_t interval = static_cast<uint32_t>(sr / std::max(0.5f, eventRate));
    try {
        if (sfx_[0].data.empty()) loadBank();
    } catch (...) {
        return false;
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    writeWavHeader(f, sr, frames * 4);
    sampleRate_ = sr;
    channels_ = 2;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        musicEnabled_ = false;
        musicVolume_ = 0.0f;
        musicIntensity_ = 0.0f;
        musicState_ = MusicState::Silent;
        musicOverpulseActive_ = false;
        musicPos_ = 0.0;
        musicStateAgeFrames_ = 0.0;
        musicLayerGains_.fill(0.0f);
        clearMixState();
    }
    int eventIndex = 0;
    float stereo[2]{};
    for (uint32_t i = 0; i < frames; ++i) {
        if (i % interval == 0) {
            playFeedback(event, volume, eventIndex++);
        }
        mix(stereo, 1);
        const int16_t l = static_cast<int16_t>(std::clamp(stereo[0], -1.0f, 1.0f) * 32767.0f);
        const int16_t r = static_cast<int16_t>(std::clamp(stereo[1], -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&l, 2, 1, f); std::fwrite(&r, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

bool AudioSystem::bakeSamples(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const uint32_t sr = 48000;

    // One-shot mono SFX, one WAV per event type.
    for (int i = 0; i < kSoundEventCount; ++i) {
        const SoundEventType t = static_cast<SoundEventType>(i);
        const uint32_t len = voiceLengthSamples(t, sr);
        const std::string path = dir + "/sfx_" + sfxName(t) + ".wav";
        // Never clobber an existing sample: real/sourced drop-ins (e.g. the SKS fire
        // shots) take precedence; the synth bake only fills in missing files.
        if (std::filesystem::exists(path)) { logInfo("kept existing %s", path.c_str()); continue; }
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        writeWavHeaderN(f, sr, 1, len * 2);          // mono, 16-bit
        uint32_t seed = 0x9E3779B9u ^ (static_cast<uint32_t>(t) * 2654435761u);
        for (uint32_t s = 0; s < len; ++s) {
            const float v = std::tanh(voiceSample(t, s, len, seed, sr) * 1.1f);
            const int16_t q = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
            std::fwrite(&q, 2, 1, f);
        }
        std::fclose(f);
        logInfo("baked %s (%u samples)", path.c_str(), len);
    }

    // Adaptive music pack: short beat-aligned stems that share the same bar length
    // and start sample, so runtime can crossfade layers without musical drift.
    const float bpm = 140.0f;
    const int   bars = 8;
    const uint32_t frames = static_cast<uint32_t>(bars * 4 * (60.0f / bpm) * sr);
    for (int layer = 0; layer < kMusicLayerCount; ++layer) {
        const std::string path = dir + "/music_" + musicLayerName(layer) + ".wav";
        if (std::filesystem::exists(path)) { logInfo("kept existing %s", path.c_str()); continue; }
        std::FILE* mf = std::fopen(path.c_str(), "wb");
        if (!mf) return false;
        writeWavHeaderN(mf, sr, 2, frames * 4);          // stereo, 16-bit
        MusicSynth synth; synth.sr = sr; synth.bpm = bpm;
        for (uint32_t i = 0; i < frames; ++i) {
            float l = 0.0f, r = 0.0f;
            synth.renderLayerStereo(layer, l, r);
            const int16_t ql = static_cast<int16_t>(std::clamp(l * 0.92f, -1.0f, 1.0f) * 32767.0f);
            const int16_t qr = static_cast<int16_t>(std::clamp(r * 0.92f, -1.0f, 1.0f) * 32767.0f);
            std::fwrite(&ql, 2, 1, mf);
            std::fwrite(&qr, 2, 1, mf);
        }
        std::fclose(mf);
        logInfo("baked %s (%u frames, %d bars @ %.0f BPM)", path.c_str(), frames, bars, bpm);
    }
    return true;
}

} // namespace pulse
