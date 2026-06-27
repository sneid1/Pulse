#!/usr/bin/env python3
"""Offline enemy combat/attack SFX production for PULSE.

Runtime contract:
  sfx_enemy_<kind>_<event>.wav, sfx_enemy_<kind>_<event>_1.wav, ...

These banks are intentionally separate from player hit/kill confirmation. Enemy
audio is the body/threat layer: attack tells, projectile releases, impacts,
melee contact, hurt bodies, death collapses, and boss burst releases.
"""

from __future__ import annotations

import math
import wave
import zlib
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy import signal


ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "assets" / "audio"
LOG_PATH = AUDIO / "PULSE_enemy_sfx_producer_log.txt"

WORK_SR = 96_000
OUT_SR = 48_000
EVENTS = ("telegraph", "shot", "impact", "beam", "lunge", "melee_hit", "hurt", "death", "boss_burst")


@dataclass(frozen=True)
class EnemySpec:
    kind: str
    family: str
    mass: float
    bright: float
    menace: float
    base_hz: float
    peak_db: float
    variants: int = 4
    pitches: tuple[float, ...] = (1.000, 0.983, 1.017, 0.992)


# Quaternius enemies are MACHINES (Raptor walker / GunDrone / QuadShell mech / EyeDrone), so the
# families are mechanical-electronic, not organic: servo (legged predator), laser (gun drone),
# mech (heavy quadruped), scanner (eye drone), titan (boss). The renderers + finish() EQ key off
# these names to colour each toward servos / electronics / metal.
ENEMIES = (
    EnemySpec("rusher", "servo", 0.86, 0.92, 0.82, 110.0, -8.0, 4, (1.000, 0.988, 1.014, 0.976)),
    EnemySpec("ranged", "laser", 0.55, 1.60, 0.74, 240.0, -8.6, 4, (1.000, 1.022, 0.986, 1.011)),
    EnemySpec("tank", "mech", 1.50, 0.66, 1.10, 58.0, -7.0, 4, (1.000, 0.972, 0.988, 1.009)),
    EnemySpec("stalker", "scanner", 0.66, 1.35, 0.90, 200.0, -8.8, 4, (1.000, 1.028, 0.978, 1.013)),
    EnemySpec("boss", "titan", 1.90, 0.80, 1.35, 46.0, -6.4, 4, (1.000, 0.968, 0.986, 1.006)),
)


def db_to_amp(db: float) -> float:
    return 10.0 ** (db / 20.0)


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def seed_for(*parts: object) -> int:
    return zlib.crc32("|".join(str(p) for p in parts).encode("utf-8")) & 0xFFFFFFFF


def sos_filter(x: np.ndarray, sos: np.ndarray) -> np.ndarray:
    return signal.sosfilt(sos, x)


def highpass(x: np.ndarray, hz: float, order: int = 2) -> np.ndarray:
    return sos_filter(x, signal.butter(order, hz, "highpass", fs=WORK_SR, output="sos"))


def lowpass(x: np.ndarray, hz: float, order: int = 2) -> np.ndarray:
    return sos_filter(x, signal.butter(order, hz, "lowpass", fs=WORK_SR, output="sos"))


def bandpass(x: np.ndarray, lo: float, hi: float, order: int = 2) -> np.ndarray:
    hi = min(hi, WORK_SR * 0.46)
    lo = max(20.0, min(lo, hi * 0.82))
    return sos_filter(x, signal.butter(order, [lo, hi], "bandpass", fs=WORK_SR, output="sos"))


def peaking(x: np.ndarray, hz: float, q: float, gain_db: float) -> np.ndarray:
    a = db_to_amp(gain_db)
    w0 = 2.0 * math.pi * hz / WORK_SR
    alpha = math.sin(w0) / (2.0 * q)
    cw = math.cos(w0)
    b = np.array([1.0 + alpha * a, -2.0 * cw, 1.0 - alpha * a])
    acoef = np.array([1.0 + alpha / a, -2.0 * cw, 1.0 - alpha / a])
    b /= acoef[0]
    acoef /= acoef[0]
    return signal.lfilter(b, acoef, x)


def soft_clip(x: np.ndarray, drive: float) -> np.ndarray:
    return np.tanh(x * drive) / math.tanh(drive)


def add(out: np.ndarray, start_s: float, layer: np.ndarray) -> None:
    start = int(start_s * WORK_SR)
    if start >= len(out):
        return
    n = min(len(layer), len(out) - start)
    if n > 0:
        out[start:start + n] += layer[:n]


def env_asr(frames: int, attack: float, release: float, hold: float = 0.0) -> np.ndarray:
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    env = 1.0 - np.exp(-t / max(0.0005, attack))
    env *= np.exp(-np.maximum(0.0, t - hold) / max(0.001, release))
    return env


def impulse(frames: int, seed: int, amp: float, lo: float, hi: float, body_hz: float, mass: float,
            click_decay: float = 150.0, body_decay: float = 42.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    click = bandpass(rng.uniform(-1.0, 1.0, frames), lo, hi)
    click *= np.exp(-t * click_decay) * (0.26 + 0.10 * mass)
    body = np.sin(2.0 * np.pi * body_hz * t) * np.exp(-t * body_decay) * (0.34 + 0.12 * mass)
    body += np.sin(2.0 * np.pi * body_hz * 1.93 * t) * np.exp(-t * body_decay * 1.35) * 0.14
    return soft_clip((click + body) * amp, 1.35)


def noise_tail(frames: int, seed: int, amp: float, lo: float, hi: float, attack: float, release: float,
               hold: float = 0.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = bandpass(rng.uniform(-1.0, 1.0, frames), lo, hi)
    return n * env_asr(frames, attack, release, hold) * amp


def sine_sweep(frames: int, start_hz: float, end_hz: float, amp: float, attack: float, release: float,
               hold: float = 0.0, curve: float = 1.0, phase_jitter: float = 0.0) -> np.ndarray:
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    p = np.clip(t / max(0.001, (frames / WORK_SR) * 0.9), 0.0, 1.0) ** curve
    freq = start_hz + (end_hz - start_hz) * p
    if phase_jitter > 0.0:
        freq += np.sin(2.0 * np.pi * (5.0 + phase_jitter) * t) * phase_jitter
    phase = 2.0 * np.pi * np.cumsum(freq) / WORK_SR
    return np.sin(phase) * env_asr(frames, attack, release, hold) * amp


def resonant_voice(frames: int, seed: int, spec: EnemySpec, pitch: float, amp: float,
                   up: bool = False, short: bool = False) -> np.ndarray:
    # Mechanical/electronic "voice" (the machines' servo + electronics), NOT an organic growl.
    # A buzzy pulse drive with motor flutter + ring-modulated inharmonic metal partials + a thin
    # band of digital grit. Used by telegraph/death/lunge/hurt, so those read as servos powering
    # up/down and metal, never breath or vocal cords.
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    dur = frames / WORK_SR
    sweep = np.clip(t / max(0.001, dur), 0.0, 1.0)
    direction = 1.0 if up else -1.0
    base = spec.base_hz * pitch * (1.0 + direction * 0.38 * sweep)
    phase = 2.0 * np.pi * np.cumsum(base) / WORK_SR
    motor = 1.0 + 0.05 * np.sin(2.0 * np.pi * (34.0 + spec.bright * 26.0) * t)   # motor/servo flutter
    servo = signal.square(phase * motor, duty=0.42)                             # buzzy pulse drive
    metal = np.sin(phase * 2.41) + 0.6 * np.sin(phase * 3.17) + 0.4 * np.sin(phase * 5.07)  # inharmonic metal
    ringmod = 0.5 + 0.5 * np.sin(2.0 * np.pi * spec.base_hz * 1.7 * pitch * t)   # ring-mod = robotic warble
    grit = bandpass(rng.uniform(-1.0, 1.0, frames), 1600.0, 9000.0 + spec.bright * 3200.0) * (0.05 + spec.bright * 0.025)
    env = env_asr(frames, 0.006 if short else 0.022, 0.055 if short else 0.17, dur * (0.18 if short else 0.52))
    voice = soft_clip(servo * (0.42 + spec.menace * 0.06) + metal * 0.16 * ringmod, 1.6) + grit
    if spec.family == "laser":
        voice = peaking(voice, 3200.0, 0.80, 3.2)
        voice = peaking(voice, 8000.0, 0.90, 2.8)
    elif spec.family == "scanner":
        voice = peaking(voice, 2400.0, 0.80, 2.6)
        voice = peaking(voice, 5600.0, 0.90, 2.0)
    elif spec.family in ("mech", "titan"):
        voice = peaking(voice, 110.0, 0.70, 2.6)
        voice = peaking(voice, 320.0, 0.75, 1.8)
        voice = peaking(voice, 4200.0, 0.90, -1.0)
    else:  # servo (rusher walker)
        voice = peaking(voice, 900.0, 0.75, 2.0)
        voice = peaking(voice, 3400.0, 0.90, 1.6)
    return voice * env * amp


def shimmer(frames: int, seed: int, amp: float, base_hz: float, bright: float, down: bool = False) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    dur = frames / WORK_SR
    p = np.clip(t / max(0.001, dur), 0.0, 1.0)
    freq = base_hz * (1.0 + (0.80 * (1.0 - p) if down else 1.05 * p))
    phase = 2.0 * np.pi * np.cumsum(freq) / WORK_SR
    tone = np.sin(phase) + 0.35 * np.sin(phase * 2.003)
    grit = bandpass(rng.uniform(-1.0, 1.0, frames), 2400.0, 14500.0) * (0.08 + bright * 0.03)
    return (tone * (0.08 + bright * 0.035) + grit) * env_asr(frames, 0.006, 0.08, dur * 0.55) * amp


def event_length(spec: EnemySpec, event: str) -> float:
    scale = 1.0 + (spec.mass - 1.0) * 0.18
    lengths = {
        "telegraph": 0.42,
        "shot": 0.28,
        "impact": 0.34,
        "beam": 0.38,
        "lunge": 0.30,
        "melee_hit": 0.30,
        "hurt": 0.22,
        "death": 0.62,
        "boss_burst": 0.72,
    }
    base = lengths[event] * scale
    if spec.kind == "boss":
        base *= 1.20 if event in ("death", "boss_burst", "telegraph") else 1.08
    return max(0.16, base)


def render_telegraph(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "telegraph") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "telegraph", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, resonant_voice(frames, seed + 1, spec, pitch, 0.66, up=True))
    add(out, 0.030, sine_sweep(frames, spec.base_hz * 1.6 * pitch, spec.base_hz * (3.4 + spec.bright) * pitch,
                               0.22 + spec.bright * 0.05, 0.030, 0.19, frames / WORK_SR * 0.55, 0.65, 2.5))
    add(out, 0.010, noise_tail(frames, seed + 2, 0.11 + spec.menace * 0.035, 900.0, 9500.0 + spec.bright * 3200.0,
                               0.018, 0.16, frames / WORK_SR * 0.42))
    for k, at in enumerate((0.035, 0.145, 0.255)):
        add(out, at, impulse(int(0.085 * WORK_SR), seed + 20 + k, 0.30, 1200.0, 11000.0,
                             spec.base_hz * (2.2 + k * 0.5) * pitch, spec.mass, 190.0, 60.0))
    return finish(out, spec, "telegraph")


def render_shot(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "shot") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "shot", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    # Electronic laser/plasma zap: a fast descending pitch "pew" + a bright transient click + a thin
    # electric shimmer tail. The machines fire energy, not slugs.
    add(out, 0.000, sine_sweep(int(0.140 * WORK_SR), spec.base_hz * 9.0 * pitch, spec.base_hz * 2.2 * pitch,
                               0.66, 0.001, 0.060, 0.006, 0.5, 1.5))
    add(out, 0.000, impulse(int(0.060 * WORK_SR), seed + 1, 0.50, 2600.0, 17000.0,
                            spec.base_hz * 3.0 * pitch, spec.mass, 320.0, 110.0))
    add(out, 0.006, shimmer(int(0.180 * WORK_SR), seed + 2, 0.52, spec.base_hz * (7.0 + spec.bright) * pitch, spec.bright))
    if spec.family in ("mech", "titan"):
        # Heavy electro-mechanical cannon: a low body thump under the zap.
        add(out, 0.004, sine_sweep(int(0.170 * WORK_SR), 120.0 * pitch, 52.0 * pitch, 0.26 * spec.mass,
                                   0.003, 0.080, 0.014, 0.40))
    return finish(out, spec, "shot")


def render_impact(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "impact") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "impact", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, impulse(int(0.145 * WORK_SR), seed + 1, 1.10, 900.0, 15000.0,
                            spec.base_hz * 1.25 * pitch, spec.mass, 210.0, 38.0))
    add(out, 0.014, noise_tail(int(0.260 * WORK_SR), seed + 2, 0.30, 140.0, 5200.0 + spec.bright * 2200.0,
                               0.004, 0.090))
    add(out, 0.038, shimmer(int(0.185 * WORK_SR), seed + 3, 0.38, spec.base_hz * 7.5 * pitch, spec.bright, down=True))
    if spec.family in ("brute", "warden"):
        add(out, 0.000, sine_sweep(int(0.230 * WORK_SR), 70.0 * pitch, 38.0 * pitch, 0.26 * spec.mass,
                                   0.002, 0.110, 0.010, 0.40))
    return finish(out, spec, "impact")


def render_beam(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "beam") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "beam", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, impulse(int(0.090 * WORK_SR), seed + 1, 0.74, 2600.0, 18000.0,
                            spec.base_hz * 3.0 * pitch, spec.mass, 280.0, 82.0))
    add(out, 0.010, shimmer(int(0.310 * WORK_SR), seed + 2, 1.10, spec.base_hz * 9.5 * pitch, spec.bright))
    add(out, 0.024, noise_tail(int(0.300 * WORK_SR), seed + 3, 0.18, 2400.0, 16500.0, 0.003, 0.12, 0.10))
    add(out, 0.220, impulse(int(0.090 * WORK_SR), seed + 4, 0.44, 1800.0, 12000.0,
                            spec.base_hz * 2.2 * pitch, spec.mass, 240.0, 88.0))
    return finish(out, spec, "beam")


def render_lunge(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "lunge") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "lunge", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, noise_tail(frames, seed + 1, 0.54 + spec.mass * 0.10, 90.0, 4200.0 + spec.bright * 3000.0,
                               0.018, 0.085, 0.050))
    add(out, 0.018, resonant_voice(int(0.220 * WORK_SR), seed + 2, spec, pitch, 0.44, up=False, short=True))
    add(out, 0.052, shimmer(int(0.145 * WORK_SR), seed + 3, 0.36, spec.base_hz * 4.4 * pitch, spec.bright))
    return finish(out, spec, "lunge")


def render_melee_hit(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "melee_hit") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "melee_hit", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, impulse(int(0.155 * WORK_SR), seed + 1, 1.12, 260.0, 7600.0,
                            spec.base_hz * 0.95 * pitch, spec.mass, 150.0, 32.0))
    add(out, 0.006, noise_tail(int(0.180 * WORK_SR), seed + 2, 0.24 + spec.mass * 0.05, 120.0, 2800.0,
                               0.002, 0.060))
    add(out, 0.048, noise_tail(int(0.180 * WORK_SR), seed + 3, 0.18, 1400.0, 12500.0, 0.002, 0.050))
    if spec.kind in ("tank", "boss"):
        add(out, 0.000, sine_sweep(int(0.210 * WORK_SR), 58.0 * pitch, 34.0 * pitch, 0.36,
                                   0.002, 0.110, 0.012, 0.35))
    return finish(out, spec, "melee_hit")


def render_hurt(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "hurt") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "hurt", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, impulse(int(0.105 * WORK_SR), seed + 1, 0.58, 700.0, 12500.0,
                            spec.base_hz * 1.55 * pitch, spec.mass, 210.0, 72.0))
    add(out, 0.014, resonant_voice(int(0.160 * WORK_SR), seed + 2, spec, pitch, 0.30, up=False, short=True))
    add(out, 0.036, noise_tail(int(0.130 * WORK_SR), seed + 3, 0.10, 1700.0, 9600.0, 0.003, 0.045))
    return finish(out, spec, "hurt")


def render_death(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "death") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "death", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    # Power-DOWN (the rig's "TurnOff"): a descending servo/motor whine that slows + detunes as the
    # machine loses power, plus a metallic clatter as it crashes down.
    add(out, 0.000, impulse(int(0.150 * WORK_SR), seed + 1, 0.84, 420.0, 11500.0,
                            spec.base_hz * 1.2 * pitch, spec.mass, 200.0, 38.0))
    add(out, 0.010, resonant_voice(int(0.460 * WORK_SR), seed + 2, spec, pitch, 0.60, up=False))
    add(out, 0.080, sine_sweep(int(0.470 * WORK_SR), spec.base_hz * 3.4 * pitch, spec.base_hz * 0.28 * pitch,
                               0.34 * (0.8 + spec.menace * 0.2), 0.010, 0.230, 0.060, 0.70, 3.2))
    for k in range(4):   # metallic clatter on the way down
        add(out, 0.180 + k * 0.072, impulse(int(0.090 * WORK_SR), seed + 30 + k, 0.20 + 0.03 * spec.mass,
                                            900.0, 12500.0, spec.base_hz * (1.4 + 0.5 * k) * pitch,
                                            spec.mass, 240.0, 92.0))
    add(out, 0.200, noise_tail(int(0.300 * WORK_SR), seed + 4, 0.16, 160.0, 5200.0, 0.020, 0.150))
    return finish(out, spec, "death")


def render_boss_burst(spec: EnemySpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "boss_burst") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.kind, "boss_burst", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    add(out, 0.000, impulse(int(0.180 * WORK_SR), seed + 1, 1.10, 540.0, 13000.0,
                            spec.base_hz * 1.45 * pitch, spec.mass, 190.0, 30.0))
    add(out, 0.010, sine_sweep(int(0.520 * WORK_SR), spec.base_hz * 1.1 * pitch, spec.base_hz * 4.6 * pitch,
                               0.42 + spec.menace * 0.10, 0.008, 0.180, 0.080, 0.58, 2.0))
    add(out, 0.050, noise_tail(int(0.520 * WORK_SR), seed + 2, 0.34, 95.0, 6200.0 + spec.bright * 2700.0,
                               0.009, 0.190, 0.055))
    for k in range(4):
        add(out, 0.100 + k * 0.062, shimmer(int(0.175 * WORK_SR), seed + 20 + k,
                                            0.26, spec.base_hz * (5.5 + k * 0.8) * pitch, spec.bright))
    return finish(out, spec, "boss_burst")


def finish(x: np.ndarray, spec: EnemySpec, event: str) -> np.ndarray:
    if len(x) == 0:
        return x
    x = highpass(x, 30.0)
    if spec.family == "laser":            # gun drone: bright, thin, electric
        x = peaking(x, 2600.0, 0.72, 3.0)
        x = peaking(x, 8200.0, 0.85, 2.6)
        x = peaking(x, 130.0, 0.80, -2.0)
    elif spec.family == "scanner":        # eye drone: mid-high digital chirp
        x = peaking(x, 2200.0, 0.75, 2.4)
        x = peaking(x, 5400.0, 0.85, 2.0)
        x = peaking(x, 150.0, 0.80, -1.4)
    elif spec.family in ("mech", "titan"):  # heavy quadruped / boss: low + metallic mid
        x = peaking(x, 72.0, 0.70, 2.6)
        x = peaking(x, 185.0, 0.75, 2.2)
        x = peaking(x, 3600.0, 0.90, 1.2)   # metal clank presence
    else:                                  # servo (rusher walker): mid servo body
        x = peaking(x, 240.0, 0.80, 1.0)
        x = peaking(x, 1800.0, 0.85, 1.6)
        x = peaking(x, 4200.0, 0.90, 1.2)
    x = soft_clip(x, 1.12 + spec.menace * 0.16)
    fade_start = int(max(0.0, len(x) / WORK_SR - 0.080) * WORK_SR)
    if fade_start < len(x):
        fade = np.linspace(1.0, 0.0, len(x) - fade_start) ** 1.55
        x[fade_start:] *= fade
    event_offset = {
        "telegraph": -4.0,
        "shot": -0.7,
        "impact": -0.3,
        "beam": -0.8,
        "lunge": -1.3,
        "melee_hit": 0.0,
        "hurt": -3.2,
        "death": 0.0,
        "boss_burst": 0.4 if spec.kind == "boss" else -0.4,
    }[event]
    peak = max(float(np.max(np.abs(x))), 1e-9)
    return x * (db_to_amp(spec.peak_db + event_offset) / peak)


def write_wav(path: Path, x96: np.ndarray, seed: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    x48 = signal.resample_poly(x96, OUT_SR, WORK_SR)
    rng = np.random.default_rng(seed)
    dither = (rng.random(len(x48)) - rng.random(len(x48))) / 65536.0
    q = np.clip(x48 + dither, -0.999969, 0.999969)
    pcm = (q * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_SR)
        w.writeframes(pcm.tobytes())


def stat_line(name: str, x: np.ndarray) -> str:
    peak = float(np.max(np.abs(x)))
    rms = float(np.sqrt(np.mean(x * x)))
    low = bandpass(x, 60.0, 280.0)
    mid = bandpass(x, 500.0, 3500.0)
    high = bandpass(x, 4500.0, 14000.0)
    return (
        f"{name}: peak={amp_to_db(peak):.2f}dBFS rms={amp_to_db(rms):.2f}dBFS "
        f"low60-280={amp_to_db(float(np.sqrt(np.mean(low * low)))):.2f}dB "
        f"mid0.5-3.5k={amp_to_db(float(np.sqrt(np.mean(mid * mid)))):.2f}dB "
        f"high4.5-14k={amp_to_db(float(np.sqrt(np.mean(high * high)))):.2f}dB"
    )


def remove_old(stem: str) -> None:
    base = AUDIO / f"{stem}.wav"
    if base.exists():
        base.unlink()
    prefix = f"{stem}_"
    for old in AUDIO.glob(f"{prefix}*.wav"):
        suffix = old.stem[len(prefix):]
        if suffix.isdigit():
            old.unlink()


def render_event(spec: EnemySpec, event: str) -> list[str]:
    stem = f"sfx_enemy_{spec.kind}_{event}"
    remove_old(stem)
    maker = {
        "telegraph": render_telegraph,
        "shot": render_shot,
        "impact": render_impact,
        "beam": render_beam,
        "lunge": render_lunge,
        "melee_hit": render_melee_hit,
        "hurt": render_hurt,
        "death": render_death,
        "boss_burst": render_boss_burst,
    }[event]
    lines = [
        f"[{spec.kind}:{event}] stem={stem} variants={spec.variants} "
        f"length={event_length(spec, event):.3f}s targetPeak={spec.peak_db:.1f}dBFS"
    ]
    for slot in range(spec.variants):
        x = maker(spec, slot)
        name = f"{stem}.wav" if slot == 0 else f"{stem}_{slot}.wav"
        write_wav(AUDIO / name, x, seed_for(spec.kind, event, slot, "write"))
        lines.append(stat_line(name, x))
    return lines


def main() -> None:
    log_lines = ["PULSE enemy combat SFX producer", f"work_sr={WORK_SR} out_sr={OUT_SR}"]
    for spec in ENEMIES:
        for event in EVENTS:
            log_lines.extend(render_event(spec, event))
    LOG_PATH.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    print("\n".join(log_lines))


if __name__ == "__main__":
    main()
