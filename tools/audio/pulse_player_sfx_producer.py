#!/usr/bin/env python3
"""Offline player-feedback / ability / UI SFX production for PULSE.

Runtime contract (one round-robin bank per feedback event):
  sfx_fb_<event>.wav, sfx_fb_<event>_1.wav, sfx_fb_<event>_2.wav, ...

These are the PLAYER bus: short, tactile, non-musical cues the player reads to
confirm their own actions and state. They are deliberately distinct from the
enemy/weapon/world banks, but they avoid arcade coin/chime language: connects,
crits, kills, movement, abilities, pickups, shield events, and menu moves are
built from filtered transients, body, air, servo, metal, and shield-fracture
layers.

Everything here is pure synthesis (no external recordings), so the whole bank is
reproducible from this script. Variants give deterministic round-robin variation
for the frequent cues; every file is peak-targeted with no clipping.
"""

from __future__ import annotations

import math
import wave
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from scipy import signal


ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "assets" / "audio"
LOG_PATH = AUDIO / "PULSE_player_sfx_producer_log.txt"

WORK_SR = 96_000
OUT_SR = 48_000


def db_to_amp(db: float) -> float:
    return 10.0 ** (db / 20.0)


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def seed_for(*parts: object) -> int:
    return zlib.crc32("|".join(str(p) for p in parts).encode("utf-8")) & 0xFFFFFFFF


def n_of(dur_s: float) -> int:
    return max(1, int(dur_s * WORK_SR))


def tarr(frames: int) -> np.ndarray:
    return np.arange(frames, dtype=np.float64) / WORK_SR


def sine(t: np.ndarray, hz: float) -> np.ndarray:
    return np.sin(2.0 * np.pi * hz * t)


def tri(t: np.ndarray, hz: float) -> np.ndarray:
    p = (t * hz) % 1.0
    return 4.0 * np.abs(p - 0.5) - 1.0


def env_exp(t: np.ndarray, k: float) -> np.ndarray:
    return np.exp(-np.maximum(t, 0.0) * k)


def env_ar(frames: int, attack_s: float, release_s: float) -> np.ndarray:
    t = tarr(frames)
    a = 1.0 - np.exp(-t / max(1e-4, attack_s))
    r = np.exp(-t / max(1e-4, release_s))
    return a * r


def soft_clip(x: np.ndarray, drive: float) -> np.ndarray:
    return np.tanh(x * drive) / math.tanh(drive)


def sos(x: np.ndarray, mode: str, freq) -> np.ndarray:
    return signal.sosfilt(signal.butter(2, freq, mode, fs=WORK_SR, output="sos"), x)


def bandpass(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    hi = min(hi, WORK_SR * 0.46)
    lo = max(20.0, min(lo, hi * 0.85))
    return sos(x, "bandpass", [lo, hi])


def noise(frames: int, seed: int) -> np.ndarray:
    return np.random.default_rng(seed).uniform(-1.0, 1.0, frames)


def add(out: np.ndarray, start_s: float, layer: np.ndarray) -> None:
    start = int(start_s * WORK_SR)
    if start >= len(out) or len(layer) == 0:
        return
    m = min(len(layer), len(out) - start)
    if m > 0:
        out[start:start + m] += layer[:m]


def click(frames: int, seed: int, lo: float, hi: float, decay: float, amp: float) -> np.ndarray:
    t = tarr(frames)
    return bandpass(noise(frames, seed), lo, hi) * np.exp(-t * decay) * amp


def blip(frames: int, hz: float, decay: float, amp: float, harmonic: float = 0.0) -> np.ndarray:
    t = tarr(frames)
    tone = sine(t, hz)
    if harmonic > 0.0:
        tone = tone + harmonic * sine(t, hz * 2.0)
    return tone * np.exp(-t * decay) * amp


def chime(frames: int, hz: float, decay: float, amp: float) -> np.ndarray:
    # A glassy struck tone: fundamental + slightly inharmonic partials.
    t = tarr(frames)
    tone = (sine(t, hz) + 0.5 * sine(t, hz * 2.01) + 0.25 * sine(t, hz * 3.02)
            + 0.12 * sine(t, hz * 4.04))
    return tone * np.exp(-t * decay) * amp


def sweep(frames: int, f0: float, f1: float, amp: float, curve: float = 1.0,
          attack_s: float = 0.004, release_k: float = 0.0) -> np.ndarray:
    t = tarr(frames)
    span = max(1e-4, frames / WORK_SR)
    p = np.clip(t / span, 0.0, 1.0) ** curve
    freq = f0 + (f1 - f0) * p
    phase = 2.0 * np.pi * np.cumsum(freq) / WORK_SR
    a = 1.0 - np.exp(-t / max(1e-4, attack_s))
    r = env_exp(t, release_k) if release_k > 0.0 else (1.0 - p * 0.0 + 0.0)
    if release_k <= 0.0:
        r = np.sin(np.clip(t / span, 0.0, 1.0) * math.pi)  # gentle hump
    return np.sin(phase) * a * r * amp


def arp(frames: int, freqs, step_s: float, decay: float, amp: float) -> np.ndarray:
    out = np.zeros(frames, dtype=np.float64)
    for i, hz in enumerate(freqs):
        note = blip(n_of(step_s * 2.6), hz, decay, amp, harmonic=0.4)
        add(out, i * step_s, note)
    return out


def resonant_partials(frames: int, base_hz: float, amp: float, decay: float,
                      ratios=(1.0, 1.61, 2.37), detune: float = 0.0) -> np.ndarray:
    t = tarr(frames)
    out = np.zeros(frames, dtype=np.float64)
    for i, ratio in enumerate(ratios):
        hz = base_hz * ratio * (1.0 + detune * (i - 1) * 0.006)
        out += sine(t, hz) * (1.0 / (1.0 + i * 0.85))
    return out * np.exp(-t * decay) * amp


def body_thud(frames: int, seed: int, amp: float = 0.35, hz: float = 92.0,
              decay: float = 18.0, noise_amp: float = 0.10) -> np.ndarray:
    t = tarr(frames)
    pitch_drop = hz * (0.60 + 0.40 * np.exp(-t * 30.0))
    phase = 2.0 * np.pi * np.cumsum(pitch_drop) / WORK_SR
    body = np.sin(phase) * np.exp(-t * decay) * amp
    shell = bandpass(noise(frames, seed), 55.0, 520.0) * np.exp(-t * (decay * 0.75)) * noise_amp
    return body + shell


def metal_tick(frames: int, seed: int, amp: float = 0.30, body_hz: float = 1450.0,
               bright: float = 0.7, decay: float = 58.0) -> np.ndarray:
    t = tarr(frames)
    strike = bandpass(noise(frames, seed), 900.0, 12500.0) * np.exp(-t * 170.0) * amp
    ring = resonant_partials(frames, body_hz, amp * 0.18 * bright, decay,
                             ratios=(1.0, 1.48, 2.19, 3.07), detune=bright)
    low = bandpass(noise(frames, seed + 17), 180.0, 900.0) * np.exp(-t * 80.0) * amp * 0.18
    return strike + ring + low


def cloth_air(frames: int, seed: int, amp: float = 0.28, lo: float = 450.0,
              hi: float = 9000.0, attack_s: float = 0.006,
              release_s: float = 0.075) -> np.ndarray:
    return bandpass(noise(frames, seed), lo, hi) * env_ar(frames, attack_s, release_s) * amp


def servo(frames: int, seed: int, amp: float = 0.18, f0: float = 180.0,
          f1: float = 520.0, attack_s: float = 0.006,
          release_s: float = 0.085) -> np.ndarray:
    t = tarr(frames)
    p = np.clip(t / max(1e-4, frames / WORK_SR), 0.0, 1.0)
    rng = np.random.default_rng(seed)
    jitter = signal.sosfilt(
        signal.butter(2, 18.0, "lowpass", fs=WORK_SR, output="sos"),
        rng.uniform(-1.0, 1.0, frames),
    )
    freq = (f0 + (f1 - f0) * (p ** 0.72)) * (1.0 + jitter * 0.012)
    phase = 2.0 * np.pi * np.cumsum(freq) / WORK_SR
    tone = np.sin(phase) + 0.28 * tri(t, freq.mean() * 1.85)
    return bandpass(tone, 120.0, 2200.0) * env_ar(frames, attack_s, release_s) * amp


def shield_grit(frames: int, seed: int, amp: float = 0.24, decay: float = 52.0) -> np.ndarray:
    t = tarr(frames)
    grain = bandpass(noise(frames, seed), 1800.0, 13000.0) * np.exp(-t * decay) * amp
    ring = resonant_partials(frames, 1850.0, amp * 0.20, decay * 0.8,
                             ratios=(1.0, 1.29, 1.93, 2.71), detune=1.2)
    return grain + ring


def scatter_ticks(out: np.ndarray, start_s: float, count: int, seed: int,
                  lo_s: float, hi_s: float, amp: float, base_hz: float) -> None:
    rng = np.random.default_rng(seed)
    for _ in range(count):
        at = start_s + float(rng.uniform(lo_s, hi_s))
        hz = base_hz * float(rng.uniform(0.72, 1.55))
        length = float(rng.uniform(0.024, 0.070))
        add(out, at, metal_tick(n_of(length), seed + int(at * 100000), amp, hz,
                                bright=float(rng.uniform(0.35, 0.9)),
                                decay=float(rng.uniform(38.0, 88.0))))


@dataclass(frozen=True)
class FbSpec:
    name: str
    length_s: float
    peak_db: float
    variants: int = 3
    pitches: tuple[float, ...] = (1.000, 1.018, 0.985, 1.010)
    fade_s: float = 0.045


# The full player-feedback event roster. Lengths/peaks are tuned so combat cues
# stay readable above music while ambient/UI cues sit politely under it.
SPECS = (
    FbSpec("hitmarker", 0.080, -5.8, 4),
    FbSpec("hit_crit", 0.120, -5.0, 4),
    FbSpec("kill", 0.220, -5.2, 4),
    FbSpec("kill_elite", 0.340, -4.7, 3),
    FbSpec("dash", 0.180, -7.2, 3),
    FbSpec("jump", 0.155, -9.2, 3),
    FbSpec("ability_tactical", 0.250, -5.7, 3),
    FbSpec("ability_ultimate", 0.600, -4.4, 3),
    FbSpec("charge_ready", 0.260, -6.7, 3),
    FbSpec("explosion", 0.480, -4.5, 4),
    FbSpec("shield_absorb", 0.170, -7.0, 3),
    FbSpec("shield_break", 0.340, -5.6, 3),
    FbSpec("low_health", 0.440, -9.0, 2),
    FbSpec("pickup_health", 0.210, -7.1, 3),
    FbSpec("pickup_shield", 0.220, -7.0, 3),
    FbSpec("pickup_ammo", 0.170, -8.0, 3),
    FbSpec("pickup_scrap", 0.160, -8.3, 3),
    FbSpec("pickup_powerup", 0.380, -6.0, 3),
    FbSpec("ui_move", 0.060, -13.0, 2),
    FbSpec("ui_confirm", 0.145, -8.0, 2),
    FbSpec("ui_cancel", 0.155, -8.8, 2),
    FbSpec("ui_reward", 0.420, -5.8, 3),
    FbSpec("run_win", 0.950, -4.8, 2),
    FbSpec("run_lose", 0.780, -5.8, 2),
)


def r_hitmarker(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.044), seed + 1, 0.34, 1600.0 * pitch, 0.45, 76.0))
    add(out, 0.002, body_thud(n_of(0.058), seed + 2, 0.16, 118.0 * pitch, 30.0, 0.04))
    add(out, 0.006, cloth_air(n_of(0.050), seed + 3, 0.08, 1100.0, 7600.0, 0.002, 0.026))
    return soft_clip(out, 1.25)


def r_hit_crit(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.050), seed + 1, 0.38, 2150.0 * pitch, 0.65, 84.0))
    add(out, 0.026, metal_tick(n_of(0.052), seed + 2, 0.22, 3200.0 * pitch, 0.85, 92.0))
    add(out, 0.002, body_thud(n_of(0.078), seed + 3, 0.17, 142.0 * pitch, 27.0, 0.035))
    add(out, 0.018, shield_grit(n_of(0.085), seed + 4, 0.07, 75.0))
    return soft_clip(out, 1.28)


def r_kill(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, body_thud(n_of(0.170), seed + 1, 0.34, 86.0 * pitch, 18.0, 0.07))
    add(out, 0.000, metal_tick(n_of(0.060), seed + 2, 0.28, 980.0 * pitch, 0.42, 54.0))
    add(out, 0.045, metal_tick(n_of(0.075), seed + 3, 0.16, 620.0 * pitch, 0.22, 36.0))
    add(out, 0.018, servo(n_of(0.130), seed + 4, 0.13, 390.0 * pitch, 165.0 * pitch, 0.004, 0.055))
    return soft_clip(out, 1.32)


def r_kill_elite(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, body_thud(n_of(0.260), seed + 1, 0.46, 63.0 * pitch, 13.5, 0.10))
    add(out, 0.000, metal_tick(n_of(0.080), seed + 2, 0.38, 760.0 * pitch, 0.35, 43.0))
    add(out, 0.036, resonant_partials(n_of(0.260), 360.0 * pitch, 0.18, 10.0,
                                      ratios=(1.0, 1.42, 2.08, 2.89), detune=1.4))
    add(out, 0.060, cloth_air(n_of(0.210), seed + 3, 0.14, 260.0, 5200.0, 0.010, 0.110))
    scatter_ticks(out, 0.060, 5, seed + 4, 0.000, 0.190, 0.08, 1180.0 * pitch)
    return soft_clip(out, 1.34)


def r_dash(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    p = np.linspace(0.0, 1.0, frames)
    out += bandpass(noise(frames, seed + 1), 380.0, 7800.0) * np.sin(p * math.pi) * 0.24
    add(out, 0.000, body_thud(n_of(0.095), seed + 2, 0.14, 96.0 * pitch, 22.0, 0.035))
    add(out, 0.018, servo(n_of(0.125), seed + 3, 0.13, 240.0 * pitch, 560.0 * pitch, 0.010, 0.070))
    add(out, 0.040, cloth_air(n_of(0.105), seed + 4, 0.12, 1100.0, 10500.0, 0.008, 0.052))
    return soft_clip(out, 1.18)


def r_jump(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, body_thud(n_of(0.090), seed + 1, 0.12, 102.0 * pitch, 24.0, 0.045))
    add(out, 0.006, cloth_air(n_of(0.130), seed + 2, 0.12, 430.0, 5600.0, 0.012, 0.058))
    add(out, 0.018, servo(n_of(0.090), seed + 3, 0.055, 150.0 * pitch, 280.0 * pitch, 0.012, 0.060))
    add(out, 0.032, metal_tick(n_of(0.036), seed + 4, 0.055, 820.0 * pitch, 0.18, 88.0))
    return soft_clip(sos(out, "lowpass", 7200.0), 1.12)


def r_ability_tactical(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, servo(n_of(0.130), seed + 1, 0.18, 260.0 * pitch, 870.0 * pitch, 0.010, 0.065))
    add(out, 0.086, metal_tick(n_of(0.060), seed + 2, 0.32, 1260.0 * pitch, 0.55, 70.0))
    add(out, 0.088, body_thud(n_of(0.150), seed + 3, 0.25, 112.0 * pitch, 18.0, 0.06))
    add(out, 0.095, cloth_air(n_of(0.140), seed + 4, 0.20, 520.0, 7000.0, 0.004, 0.060))
    return soft_clip(out, 1.25)


def r_ability_ultimate(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    n_charge = n_of(0.330)
    ramp = np.linspace(0.0, 1.0, n_charge) ** 1.8
    charge_noise = bandpass(noise(n_charge, seed + 1), 180.0, 8500.0) * ramp * 0.20
    charge_servo = servo(n_charge, seed + 2, 0.24, 95.0 * pitch, 720.0 * pitch, 0.060, 0.220) * ramp
    add(out, 0.000, charge_noise + charge_servo)
    add(out, 0.304, metal_tick(n_of(0.082), seed + 3, 0.48, 920.0 * pitch, 0.50, 54.0))
    add(out, 0.305, body_thud(n_of(0.290), seed + 4, 0.56, 58.0 * pitch, 10.0, 0.11))
    add(out, 0.318, shield_grit(n_of(0.240), seed + 5, 0.18, 34.0))
    scatter_ticks(out, 0.335, 6, seed + 6, 0.000, 0.185, 0.07, 1600.0 * pitch)
    return soft_clip(out, 1.32)


def r_charge_ready(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, servo(n_of(0.155), seed + 1, 0.12, 180.0 * pitch, 640.0 * pitch, 0.020, 0.085))
    add(out, 0.096, metal_tick(n_of(0.070), seed + 2, 0.22, 1900.0 * pitch, 0.55, 64.0))
    add(out, 0.102, shield_grit(n_of(0.120), seed + 3, 0.07, 52.0))
    return soft_clip(out, 1.18)


def r_explosion(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.052), seed + 1, 0.44, 640.0 * pitch, 0.45, 72.0))
    add(out, 0.000, body_thud(n_of(0.310), seed + 2, 0.58, 54.0 * pitch, 9.5, 0.13))
    out += bandpass(noise(frames, seed + 3), 85.0, 3400.0) * env_ar(frames, 0.003, 0.145) * 0.38
    add(out, 0.030, cloth_air(n_of(0.360), seed + 4, 0.20, 180.0, 5200.0, 0.014, 0.180))
    scatter_ticks(out, 0.020, 9, seed + 5, 0.000, 0.330, 0.055, 1180.0 * pitch)
    return soft_clip(out, 1.34)


def r_shield_absorb(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, shield_grit(n_of(0.145), seed + 1, 0.28, 54.0))
    add(out, 0.004, metal_tick(n_of(0.040), seed + 2, 0.12, 2600.0 * pitch, 0.70, 95.0))
    add(out, 0.000, body_thud(n_of(0.090), seed + 3, 0.10, 130.0 * pitch, 28.0, 0.025))
    return soft_clip(out, 1.18)


def r_shield_break(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, shield_grit(n_of(0.270), seed + 1, 0.34, 28.0))
    add(out, 0.000, body_thud(n_of(0.180), seed + 2, 0.24, 92.0 * pitch, 17.0, 0.06))
    add(out, 0.000, metal_tick(n_of(0.060), seed + 3, 0.28, 1450.0 * pitch, 0.70, 74.0))
    scatter_ticks(out, 0.016, 8, seed + 4, 0.000, 0.250, 0.075, 2150.0 * pitch)
    return soft_clip(out, 1.22)


def r_low_health(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, body_thud(n_of(0.190), seed + 1, 0.30, 67.0 * pitch, 9.5, 0.035))
    add(out, 0.205, body_thud(n_of(0.210), seed + 2, 0.26, 62.0 * pitch, 8.5, 0.030))
    add(out, 0.016, bandpass(noise(n_of(0.330), seed + 3), 120.0, 780.0)
        * env_ar(n_of(0.330), 0.020, 0.160) * 0.055)
    return soft_clip(sos(out, "lowpass", 1800.0), 1.12)


def r_pickup_health(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.045), seed + 1, 0.18, 820.0 * pitch, 0.28, 58.0))
    add(out, 0.035, cloth_air(n_of(0.120), seed + 2, 0.14, 650.0, 5200.0, 0.008, 0.055))
    add(out, 0.058, servo(n_of(0.095), seed + 3, 0.08, 240.0 * pitch, 330.0 * pitch, 0.010, 0.052))
    add(out, 0.000, body_thud(n_of(0.100), seed + 4, 0.09, 124.0 * pitch, 24.0, 0.025))
    return soft_clip(out, 1.16)


def r_pickup_shield(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, shield_grit(n_of(0.150), seed + 1, 0.18, 45.0))
    add(out, 0.055, shield_grit(n_of(0.120), seed + 2, 0.12, 48.0))
    add(out, 0.030, servo(n_of(0.120), seed + 3, 0.07, 180.0 * pitch, 420.0 * pitch, 0.018, 0.065))
    return soft_clip(out, 1.14)


def r_pickup_ammo(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.055), seed + 1, 0.28, 720.0 * pitch, 0.30, 52.0))
    add(out, 0.052, metal_tick(n_of(0.050), seed + 2, 0.20, 1160.0 * pitch, 0.42, 68.0))
    add(out, 0.000, body_thud(n_of(0.105), seed + 3, 0.11, 120.0 * pitch, 25.0, 0.025))
    return soft_clip(out, 1.18)


def r_pickup_scrap(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.046), seed + 1, 0.22, 980.0 * pitch, 0.38, 62.0))
    add(out, 0.034, metal_tick(n_of(0.044), seed + 2, 0.18, 1460.0 * pitch, 0.52, 76.0))
    add(out, 0.070, metal_tick(n_of(0.040), seed + 3, 0.12, 760.0 * pitch, 0.28, 58.0))
    return soft_clip(out, 1.16)


def r_pickup_powerup(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, body_thud(n_of(0.210), seed + 1, 0.22, 78.0 * pitch, 15.0, 0.055))
    add(out, 0.000, metal_tick(n_of(0.070), seed + 2, 0.28, 820.0 * pitch, 0.36, 52.0))
    add(out, 0.072, servo(n_of(0.190), seed + 3, 0.16, 190.0 * pitch, 520.0 * pitch, 0.020, 0.110))
    add(out, 0.118, shield_grit(n_of(0.180), seed + 4, 0.09, 36.0))
    return soft_clip(out, 1.20)


def r_ui_move(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.032), seed + 1, 0.16, 1250.0 * pitch, 0.25, 92.0))
    add(out, 0.004, body_thud(n_of(0.040), seed + 2, 0.035, 180.0 * pitch, 45.0, 0.012))
    return soft_clip(out, 1.10)


def r_ui_confirm(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.050), seed + 1, 0.20, 1050.0 * pitch, 0.30, 66.0))
    add(out, 0.046, metal_tick(n_of(0.060), seed + 2, 0.16, 1450.0 * pitch, 0.42, 70.0))
    add(out, 0.026, servo(n_of(0.070), seed + 3, 0.045, 210.0 * pitch, 300.0 * pitch, 0.012, 0.044))
    return soft_clip(out, 1.12)


def r_ui_cancel(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.055), seed + 1, 0.18, 760.0 * pitch, 0.22, 54.0))
    add(out, 0.052, body_thud(n_of(0.080), seed + 2, 0.10, 110.0 * pitch, 25.0, 0.020))
    add(out, 0.012, servo(n_of(0.090), seed + 3, 0.055, 320.0 * pitch, 170.0 * pitch, 0.008, 0.055))
    return soft_clip(out, 1.12)


def r_ui_reward(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.070), seed + 1, 0.28, 850.0 * pitch, 0.34, 52.0))
    add(out, 0.075, body_thud(n_of(0.190), seed + 2, 0.22, 74.0 * pitch, 15.0, 0.055))
    add(out, 0.080, resonant_partials(n_of(0.260), 315.0 * pitch, 0.12, 10.0,
                                      ratios=(1.0, 1.58, 2.27), detune=0.8))
    add(out, 0.160, shield_grit(n_of(0.160), seed + 3, 0.08, 38.0))
    return soft_clip(out, 1.18)


def r_run_win(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, metal_tick(n_of(0.085), seed + 1, 0.28, 720.0 * pitch, 0.32, 42.0))
    add(out, 0.120, metal_tick(n_of(0.080), seed + 2, 0.22, 980.0 * pitch, 0.38, 46.0))
    add(out, 0.215, body_thud(n_of(0.380), seed + 3, 0.26, 62.0 * pitch, 8.0, 0.060))
    add(out, 0.220, resonant_partials(n_of(0.580), 245.0 * pitch, 0.17, 4.6,
                                      ratios=(1.0, 1.50, 2.18, 3.01), detune=0.6))
    add(out, 0.420, shield_grit(n_of(0.350), seed + 4, 0.08, 18.0))
    return soft_clip(out, 1.20)


def r_run_lose(spec, seed, slot, pitch):
    frames = n_of(spec.length_s)
    out = np.zeros(frames, dtype=np.float64)
    add(out, 0.000, body_thud(n_of(0.260), seed + 1, 0.30, 58.0 * pitch, 9.0, 0.070))
    add(out, 0.090, cloth_air(n_of(0.520), seed + 2, 0.16, 90.0, 1800.0, 0.030, 0.250))
    add(out, 0.150, servo(n_of(0.360), seed + 3, 0.12, 260.0 * pitch, 92.0 * pitch, 0.050, 0.220))
    add(out, 0.330, metal_tick(n_of(0.090), seed + 4, 0.12, 520.0 * pitch, 0.18, 30.0))
    return soft_clip(sos(out, "lowpass", 6200.0), 1.14)


RENDERERS = {
    "hitmarker": r_hitmarker, "hit_crit": r_hit_crit, "kill": r_kill, "kill_elite": r_kill_elite,
    "dash": r_dash, "jump": r_jump, "ability_tactical": r_ability_tactical,
    "ability_ultimate": r_ability_ultimate, "charge_ready": r_charge_ready, "explosion": r_explosion,
    "shield_absorb": r_shield_absorb, "shield_break": r_shield_break, "low_health": r_low_health,
    "pickup_health": r_pickup_health, "pickup_shield": r_pickup_shield, "pickup_ammo": r_pickup_ammo,
    "pickup_scrap": r_pickup_scrap, "pickup_powerup": r_pickup_powerup,
    "ui_move": r_ui_move, "ui_confirm": r_ui_confirm, "ui_cancel": r_ui_cancel, "ui_reward": r_ui_reward,
    "run_win": r_run_win, "run_lose": r_run_lose,
}


def finish(x: np.ndarray, spec: FbSpec) -> np.ndarray:
    x = sos(x, "highpass", 24.0)
    fade_start = int(max(0.0, len(x) / WORK_SR - spec.fade_s) * WORK_SR)
    if fade_start < len(x):
        fade = np.linspace(1.0, 0.0, len(x) - fade_start) ** 1.5
        x[fade_start:] *= fade
    peak = max(float(np.max(np.abs(x))), 1e-9)
    return x * (db_to_amp(spec.peak_db) / peak)


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
    return f"{name}: peak={amp_to_db(peak):.2f}dBFS rms={amp_to_db(rms):.2f}dBFS frames={len(x)}"


def remove_old(stem: str) -> None:
    base = AUDIO / f"{stem}.wav"
    if base.exists():
        base.unlink()
    for old in AUDIO.glob(f"{stem}_*.wav"):
        if old.stem[len(stem) + 1:].isdigit():
            old.unlink()


def render_event(spec: FbSpec) -> list[str]:
    stem = f"sfx_fb_{spec.name}"
    remove_old(stem)
    fn = RENDERERS[spec.name]
    lines = [f"[{spec.name}] stem={stem} variants={spec.variants} "
             f"length={spec.length_s:.3f}s targetPeak={spec.peak_db:.1f}dBFS"]
    for slot in range(spec.variants):
        seed = seed_for(spec.name, slot)
        pitch = spec.pitches[slot % len(spec.pitches)]
        x = finish(fn(spec, seed, slot, pitch), spec)
        name = f"{stem}.wav" if slot == 0 else f"{stem}_{slot}.wav"
        write_wav(AUDIO / name, x, seed_for(spec.name, slot, "write"))
        lines.append(stat_line(name, x))
    return lines


def main() -> None:
    log_lines = ["PULSE player-feedback SFX producer", f"work_sr={WORK_SR} out_sr={OUT_SR}"]
    total = 0
    for spec in SPECS:
        log_lines.extend(render_event(spec))
        total += spec.variants
    log_lines.append(f"total_events={len(SPECS)} total_files={total}")
    LOG_PATH.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    print("\n".join(log_lines))


if __name__ == "__main__":
    main()
