#!/usr/bin/env python3
# Generate per-biome ambient bed loops for Pulse (spec biome.audio):
#   Foundry   - deep electrical hum + transformer buzz + faint air
#   Furnace   - roaring furnace rumble + hiss, slow surge
#   Reliquary - deep silent vault drone + sparse wind swells (lots of space)
# Seamless mono 16-bit WAV loops written to assets/audio/. Pure stdlib (no numpy).
# These are low beds; the mixer plays them at a low gain under the gameplay SFX.
# ASCII only (see CLAUDE.md).

import math
import os
import random
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "assets", "audio")
RATE = 48000
SECONDS = 6.0
N = int(RATE * SECONDS)
XF = int(0.5 * RATE)   # crossfade length for seamless noise loops


def one_pole_lowpass(samples, coef):
    """coef in (0,1): smaller = darker. y += coef*(x-y)."""
    out = [0.0] * len(samples)
    y = 0.0
    for i, x in enumerate(samples):
        y += coef * (x - y)
        out[i] = y
    return out


def seamless(buf):
    """Crossfade the tail into the head so the loop has no seam."""
    n = len(buf)
    out = list(buf)
    for k in range(XF):
        t = k / XF
        out[k] = buf[k] * t + buf[n - XF + k] * (1.0 - t)
    return out[:n - XF]   # drop the now-redundant tail region


def norm(buf, peak):
    m = max(1e-9, max(abs(v) for v in buf))
    g = peak / m
    return [v * g for v in buf]


def write_wav(name, buf):
    path = os.path.join(OUT, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for v in buf:
            s = int(max(-1.0, min(1.0, v)) * 32767.0)
            frames += struct.pack("<h", s)
        w.writeframes(bytes(frames))
    print("wrote {} ({} frames, {:.1f}s)".format(path, len(buf), len(buf) / RATE))


def foundry():
    rng = random.Random(101)
    buf = [0.0] * N
    for i in range(N):
        t = i / RATE
        v = (0.20 * math.sin(2 * math.pi * 50.0 * t)
             + 0.11 * math.sin(2 * math.pi * 100.0 * t)
             + 0.05 * math.sin(2 * math.pi * 150.0 * t)
             + 0.045 * math.sin(2 * math.pi * 120.0 * t) * (0.6 + 0.4 * math.sin(2 * math.pi * 0.33 * t))  # transformer buzz, slow tremolo
             )
        buf[i] = v
    air = one_pole_lowpass([rng.uniform(-1, 1) for _ in range(N)], 0.06)
    buf = [buf[i] + 0.06 * air[i] for i in range(N)]
    return norm(seamless(buf), 0.5)


def furnace():
    rng = random.Random(202)
    rumble = one_pole_lowpass([rng.uniform(-1, 1) for _ in range(N)], 0.010)   # very low roar
    hiss = one_pole_lowpass([rng.uniform(-1, 1) for _ in range(N)], 0.5)       # broadband vent hiss
    buf = [0.0] * N
    for i in range(N):
        t = i / RATE
        surge = 0.7 + 0.3 * math.sin(2 * math.pi * 0.18 * t)   # slow furnace surge
        buf[i] = (0.85 * rumble[i] * surge
                  + 0.12 * math.sin(2 * math.pi * 40.0 * t)
                  + 0.05 * hiss[i] * surge)
    return norm(seamless(buf), 0.5)


def reliquary():
    rng = random.Random(303)
    wind = one_pole_lowpass([rng.uniform(-1, 1) for _ in range(N)], 0.02)
    buf = [0.0] * N
    for i in range(N):
        t = i / RATE
        drone = 0.10 * math.sin(2 * math.pi * 55.0 * t) + 0.06 * math.sin(2 * math.pi * 82.5 * t)  # low fifth
        swell = max(0.0, math.sin(2 * math.pi * 0.07 * t)) ** 2   # sparse, mostly-quiet swells
        buf[i] = drone * 0.5 + 0.5 * wind[i] * swell
    return norm(seamless(buf), 0.42)   # quieter: "deep silent reverb bed"


def main():
    if not os.path.isdir(OUT):
        raise SystemExit("missing assets/audio dir: " + OUT)
    write_wav("sfx_ambient_foundry.wav", foundry())
    write_wav("sfx_ambient_furnace.wav", furnace())
    write_wav("sfx_ambient_reliquary.wav", reliquary())


if __name__ == "__main__":
    main()
