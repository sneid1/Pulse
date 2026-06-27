#!/usr/bin/env python3
"""Offline weapon reload foley production for PULSE.

The runtime contract is:
  sfx_weapon_<weapon>_<event>.wav, sfx_weapon_<weapon>_<event>_1.wav, ...

Events currently used by gameplay are reload_start, mag_out, mag_in,
reload_end, and shell. The assets are procedural foley-style layers: short
metal/plastic impacts, magazine body handling, slides, springs, restrained
servo movement, hand cloth, and weapon-specific mass/brightness shaping.
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
LOG_PATH = AUDIO / "PULSE_reload_producer_log.txt"

WORK_SR = 96_000
OUT_SR = 48_000
EVENTS = ("dry", "equip", "reload_start", "mag_out", "mag_in", "reload_end", "bolt", "shell")


@dataclass(frozen=True)
class ReloadSpec:
    weapon: str
    family: str
    mass: float
    bright: float
    peak_db: float
    start_len: float
    insert_len: float
    end_len: float
    variants: int = 3
    pitches: tuple[float, ...] = (1.000, 0.985, 1.014)


WEAPONS = (
    ReloadSpec("pistol", "pistol", 0.72, 1.12, -7.2, 0.44, 0.34, 0.38, 3, (1.000, 0.988, 1.012)),
    ReloadSpec("ak47", "ak", 1.22, 0.92, -6.0, 0.58, 0.46, 0.50, 3, (1.000, 0.978, 1.009)),
    ReloadSpec("carbine", "carbine", 0.96, 1.03, -6.6, 0.50, 0.38, 0.42, 3, (1.000, 0.990, 1.010)),
    ReloadSpec("pulse_smg", "pulse", 0.66, 1.24, -7.8, 0.40, 0.32, 0.36, 3, (1.000, 1.018, 0.986)),
    ReloadSpec("machine_pistol", "machine_pistol", 0.58, 1.20, -7.8, 0.38, 0.30, 0.34, 3, (1.000, 1.016, 0.990)),
    ReloadSpec("scattergun", "scattergun", 1.30, 0.86, -5.8, 0.62, 0.34, 0.58, 3, (1.000, 0.976, 1.006)),
    ReloadSpec("marksman", "marksman", 1.36, 0.94, -6.2, 0.64, 0.42, 0.62, 3, (1.000, 0.982, 1.008)),
    ReloadSpec("railbolt", "railbolt", 0.92, 1.35, -7.2, 0.54, 0.42, 0.50, 3, (1.000, 1.012, 0.988)),
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
    lo = min(lo, hi * 0.80)
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


def envelope_ar(frames: int, attack_s: float, release_s: float) -> np.ndarray:
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    attack = 1.0 - np.exp(-t / max(1e-4, attack_s))
    release = np.exp(-t / max(1e-4, release_s))
    return attack * release


def resonant_hit(frames: int, hz: float, amp: float, decay: float, color: float) -> np.ndarray:
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    out = np.zeros(frames, dtype=np.float64)
    ratios = (1.0, 1.47, 2.11, 2.92)
    for i, ratio in enumerate(ratios):
        drift = 1.0 + (i - 1.5) * 0.004 * color
        out += np.sin(2.0 * np.pi * hz * ratio * drift * t) * np.exp(-t * decay * (1.0 + i * 0.22)) / (1.0 + i)
    return out * amp


def contact_cluster(frames: int, seed: int, amp: float, density: int, lo: float, hi: float,
                    span_s: float, decay: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    out = np.zeros(frames, dtype=np.float64)
    for i in range(density):
        start = int(rng.uniform(0.0, max(0.001, span_s)) * WORK_SR)
        if start >= frames:
            continue
        length = min(frames - start, int(rng.uniform(0.010, 0.040) * WORK_SR))
        if length <= 0:
            continue
        local_t = np.arange(length, dtype=np.float64) / WORK_SR
        grain = bandpass(rng.uniform(-1.0, 1.0, length), lo, hi)
        grain *= np.exp(-local_t * rng.uniform(decay * 0.75, decay * 1.45))
        out[start:start + length] += grain * amp * rng.uniform(0.32, 1.0)
    return out


def impact(frames: int, seed: int, amp: float, body_hz: float, bright: float, mass: float,
           noise_lo: float = 900.0, noise_hi: float = 13000.0, decay: float = 48.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    click = bandpass(rng.uniform(-1.0, 1.0, frames), noise_lo, noise_hi)
    click *= np.exp(-t * (130.0 + bright * 45.0)) * (0.36 + bright * 0.14)
    body_noise = lowpass(rng.uniform(-1.0, 1.0, frames), max(260.0, body_hz * 4.2))
    body_noise *= np.exp(-t * (decay * 0.78)) * (0.20 + mass * 0.13)
    pitch = body_hz * (0.74 + 0.26 * np.exp(-t * 36.0))
    phase = 2.0 * np.pi * np.cumsum(pitch) / WORK_SR
    body = np.sin(phase) * np.exp(-t * decay) * (0.16 + mass * 0.075)
    ring = resonant_hit(frames, body_hz * (2.4 + bright * 0.55), 0.035 + bright * 0.018, decay * 1.15, bright)
    grit = contact_cluster(frames, seed + 101, 0.075 + bright * 0.018, 4, noise_lo, noise_hi, 0.030, 180.0)
    return soft_clip((click + body_noise + body + ring + grit) * amp, 1.16)


def scrape(frames: int, seed: int, amp: float, dur_s: float, bright: float, mass: float,
           lo: float = 520.0, hi: float = 9000.0, tone_hz: float = 260.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    dur = max(0.01, dur_s)
    sweep = np.clip(t / dur, 0.0, 1.0)
    env = (1.0 - np.exp(-t * 45.0)) * np.exp(-sweep * 2.15)
    env[t > dur] *= np.exp(-(t[t > dur] - dur) * 55.0)
    rub = bandpass(rng.uniform(-1.0, 1.0, frames), lo, hi) * env * (0.20 + bright * 0.065)
    grind = lowpass(rng.uniform(-1.0, 1.0, frames), 850.0 + mass * 320.0) * env * (0.13 + mass * 0.055)
    rail = resonant_hit(frames, tone_hz * 0.72, 0.050 + mass * 0.018, 15.0, bright)
    rail *= env
    flecks = contact_cluster(frames, seed + 211, 0.055 + bright * 0.020, 10, lo * 0.85, hi, dur, 130.0)
    return soft_clip((rub + grind + rail + flecks) * amp, 1.12)


def spring(frames: int, seed: int, amp: float, bright: float, base_hz: float = 980.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    out = np.zeros(frames, dtype=np.float64)
    for k in range(5):
        d = int((0.012 + k * rng.uniform(0.012, 0.025) + rng.uniform(-0.004, 0.004)) * WORK_SR)
        if d >= frames:
            continue
        local = t[:frames - d]
        hz = base_hz * (1.0 + k * 0.19) * (0.985 + rng.random() * 0.03)
        chirp = resonant_hit(len(local), hz, 0.035 + bright * 0.013, 68.0 + k * 22.0, bright)
        rasp = bandpass(rng.uniform(-1.0, 1.0, len(local)), 1800.0, 14000.0)
        rasp *= np.exp(-local * (155.0 + k * 22.0)) * (0.035 + bright * 0.012)
        out[d:] += chirp + rasp
    return soft_clip(out * amp, 1.08)


def servo(frames: int, seed: int, amp: float, dur_s: float, base_hz: float, bright: float, down: bool = False) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    dur = max(0.02, dur_s)
    sweep = np.clip(t / dur, 0.0, 1.0)
    wobble = lowpass(rng.uniform(-1.0, 1.0, frames), 22.0)
    freq = base_hz * (1.0 + (0.72 * (1.0 - sweep) if down else 0.50 * sweep)) * (1.0 + wobble * 0.010)
    phase = 2.0 * np.pi * np.cumsum(freq) / WORK_SR
    env = np.sin(np.clip(t / dur, 0.0, 1.0) * math.pi)
    env[t > dur] = 0.0
    whine = np.sin(phase) * env * (0.035 + bright * 0.018)
    motor = bandpass(rng.uniform(-1.0, 1.0, frames), 120.0, 2600.0) * env * (0.070 + bright * 0.020)
    brush = bandpass(rng.uniform(-1.0, 1.0, frames), 1900.0, 11500.0) * env * 0.038
    return soft_clip((whine + motor + brush) * amp, 1.10)


def hand_cloth(frames: int, seed: int, amp: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    rub_a = bandpass(rng.uniform(-1.0, 1.0, frames), 120.0, 2600.0) * envelope_ar(frames, 0.018, 0.135)
    rub_b = bandpass(rng.uniform(-1.0, 1.0, frames), 700.0, 5200.0) * envelope_ar(frames, 0.045, 0.090) * 0.45
    palm = lowpass(rng.uniform(-1.0, 1.0, frames), 420.0) * np.exp(-t * 10.0) * 0.30
    return (rub_a + rub_b + palm) * amp


def event_length(spec: ReloadSpec, event: str) -> float:
    if event == "dry":
        return 0.18 + spec.mass * 0.045
    if event == "equip":
        return max(0.34, spec.start_len * 0.74)
    if event == "mag_out":
        return max(0.32, spec.start_len * 0.62)
    if event == "reload_start":
        return spec.start_len
    if event == "mag_in":
        return spec.insert_len
    if event == "shell":
        return max(0.30, spec.insert_len)
    if event == "bolt":
        return max(0.34, spec.end_len * 0.78)
    return spec.end_len


def render_reload_start(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(spec.start_len * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "start", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright

    if spec.family in ("pistol", "machine_pistol"):
        add(out, 0.010, impact(int(0.115 * WORK_SR), seed + 1, 0.70, 520.0 * pitch, b, m, 1800.0, 15000.0, 82.0))
        add(out, 0.060, scrape(int(0.180 * WORK_SR), seed + 2, 0.58, 0.100, b, m, 900.0, 12000.0, 360.0 * pitch))
        add(out, 0.175, impact(int(0.145 * WORK_SR), seed + 3, 0.46, 210.0 * pitch, b, m, 700.0, 7200.0, 36.0))
        add(out, 0.260, hand_cloth(int(0.120 * WORK_SR), seed + 4, 0.050))
    elif spec.family == "ak":
        add(out, 0.014, impact(int(0.130 * WORK_SR), seed + 5, 0.78, 250.0 * pitch, b, m, 650.0, 9800.0, 44.0))
        add(out, 0.082, scrape(int(0.290 * WORK_SR), seed + 6, 0.88, 0.210, b, m, 380.0, 7200.0, 185.0 * pitch))
        add(out, 0.255, impact(int(0.180 * WORK_SR), seed + 7, 0.62, 135.0 * pitch, b, m, 420.0, 6500.0, 28.0))
        add(out, 0.365, spring(int(0.115 * WORK_SR), seed + 8, 0.42, b, 760.0 * pitch))
    elif spec.family == "carbine":
        add(out, 0.012, impact(int(0.120 * WORK_SR), seed + 9, 0.62, 340.0 * pitch, b, m, 1100.0, 12000.0, 58.0))
        add(out, 0.072, scrape(int(0.210 * WORK_SR), seed + 10, 0.66, 0.145, b, m, 620.0, 8500.0, 270.0 * pitch))
        add(out, 0.215, impact(int(0.145 * WORK_SR), seed + 11, 0.42, 190.0 * pitch, b, m, 650.0, 7600.0, 32.0))
    elif spec.family == "pulse":
        add(out, 0.006, impact(int(0.100 * WORK_SR), seed + 12, 0.44, 680.0 * pitch, b, m, 2300.0, 17000.0, 92.0))
        add(out, 0.050, servo(int(0.220 * WORK_SR), seed + 13, 0.82, 0.185, 780.0 * pitch, b, True))
        add(out, 0.198, impact(int(0.110 * WORK_SR), seed + 14, 0.40, 290.0 * pitch, b, m, 1200.0, 11000.0, 52.0))
    elif spec.family == "scattergun":
        add(out, 0.012, impact(int(0.140 * WORK_SR), seed + 15, 0.66, 185.0 * pitch, b, m, 420.0, 8200.0, 34.0))
        add(out, 0.066, scrape(int(0.390 * WORK_SR), seed + 16, 1.05, 0.290, b, m, 240.0, 6500.0, 155.0 * pitch))
        add(out, 0.330, impact(int(0.170 * WORK_SR), seed + 17, 0.64, 122.0 * pitch, b, m, 360.0, 6200.0, 25.0))
        add(out, 0.430, spring(int(0.110 * WORK_SR), seed + 18, 0.32, b, 590.0 * pitch))
    elif spec.family == "marksman":
        add(out, 0.014, impact(int(0.150 * WORK_SR), seed + 19, 0.76, 210.0 * pitch, b, m, 520.0, 8700.0, 38.0))
        add(out, 0.090, scrape(int(0.390 * WORK_SR), seed + 20, 0.94, 0.300, b, m, 320.0, 7600.0, 175.0 * pitch))
        add(out, 0.380, impact(int(0.170 * WORK_SR), seed + 21, 0.70, 118.0 * pitch, b, m, 420.0, 6200.0, 24.0))
        add(out, 0.485, spring(int(0.110 * WORK_SR), seed + 22, 0.28, b, 720.0 * pitch))
    else:  # railbolt
        add(out, 0.010, impact(int(0.120 * WORK_SR), seed + 23, 0.54, 430.0 * pitch, b, m, 1300.0, 15000.0, 66.0))
        add(out, 0.072, servo(int(0.300 * WORK_SR), seed + 24, 1.00, 0.260, 640.0 * pitch, b, True))
        add(out, 0.300, impact(int(0.150 * WORK_SR), seed + 25, 0.48, 165.0 * pitch, b, m, 900.0, 9000.0, 34.0))
    return finish(out, spec, "reload_start")


def render_mag_in(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(spec.insert_len * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "insert", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright

    if spec.family in ("pistol", "machine_pistol"):
        add(out, 0.012, impact(int(0.150 * WORK_SR), seed + 30, 0.78, 188.0 * pitch, b, m, 700.0, 9800.0, 34.0))
        add(out, 0.100, impact(int(0.125 * WORK_SR), seed + 31, 0.62, 440.0 * pitch, b, m, 1500.0, 14500.0, 76.0))
        add(out, 0.162, spring(int(0.115 * WORK_SR), seed + 32, 0.30, b, 1080.0 * pitch))
    elif spec.family == "ak":
        add(out, 0.016, impact(int(0.160 * WORK_SR), seed + 33, 0.66, 160.0 * pitch, b, m, 450.0, 6500.0, 28.0))
        add(out, 0.098, scrape(int(0.185 * WORK_SR), seed + 34, 0.62, 0.130, b, m, 350.0, 6900.0, 170.0 * pitch))
        add(out, 0.210, impact(int(0.170 * WORK_SR), seed + 35, 0.86, 112.0 * pitch, b, m, 420.0, 7600.0, 24.0))
        add(out, 0.310, hand_cloth(int(0.100 * WORK_SR), seed + 36, 0.045))
    elif spec.family == "carbine":
        add(out, 0.010, scrape(int(0.130 * WORK_SR), seed + 37, 0.34, 0.090, b, m, 700.0, 7400.0, 250.0 * pitch))
        add(out, 0.070, impact(int(0.160 * WORK_SR), seed + 38, 0.78, 145.0 * pitch, b, m, 520.0, 9000.0, 30.0))
        add(out, 0.168, impact(int(0.115 * WORK_SR), seed + 39, 0.44, 520.0 * pitch, b, m, 1600.0, 14200.0, 86.0))
    elif spec.family == "pulse":
        add(out, 0.012, servo(int(0.185 * WORK_SR), seed + 40, 0.82, 0.145, 880.0 * pitch, b, False))
        add(out, 0.092, impact(int(0.135 * WORK_SR), seed + 41, 0.52, 260.0 * pitch, b, m, 1200.0, 12000.0, 44.0))
        add(out, 0.190, spring(int(0.100 * WORK_SR), seed + 42, 0.22, b, 1480.0 * pitch))
    elif spec.family == "scattergun":
        add(out, 0.010, impact(int(0.120 * WORK_SR), seed + 43, 0.70, 138.0 * pitch, b, m, 420.0, 5800.0, 30.0))
        add(out, 0.068, scrape(int(0.150 * WORK_SR), seed + 44, 0.48, 0.100, b, m, 360.0, 5600.0, 185.0 * pitch))
        add(out, 0.155, impact(int(0.120 * WORK_SR), seed + 45, 0.46, 310.0 * pitch, b, m, 900.0, 7800.0, 60.0))
    elif spec.family == "marksman":
        add(out, 0.018, impact(int(0.160 * WORK_SR), seed + 46, 0.66, 150.0 * pitch, b, m, 430.0, 6900.0, 28.0))
        add(out, 0.100, impact(int(0.145 * WORK_SR), seed + 47, 0.56, 360.0 * pitch, b, m, 1000.0, 10000.0, 58.0))
        add(out, 0.205, spring(int(0.125 * WORK_SR), seed + 48, 0.24, b, 880.0 * pitch))
    else:
        add(out, 0.012, servo(int(0.220 * WORK_SR), seed + 49, 0.92, 0.180, 820.0 * pitch, b, False))
        add(out, 0.150, impact(int(0.150 * WORK_SR), seed + 50, 0.60, 198.0 * pitch, b, m, 800.0, 11000.0, 32.0))
        add(out, 0.255, spring(int(0.115 * WORK_SR), seed + 51, 0.25, b, 1320.0 * pitch))
    return finish(out, spec, "mag_in")


def render_reload_end(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(spec.end_len * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "end", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright

    if spec.family in ("pistol", "machine_pistol"):
        add(out, 0.014, scrape(int(0.180 * WORK_SR), seed + 60, 0.72, 0.125, b, m, 820.0, 11800.0, 390.0 * pitch))
        add(out, 0.142, impact(int(0.145 * WORK_SR), seed + 61, 0.70, 235.0 * pitch, b, m, 850.0, 13000.0, 42.0))
        add(out, 0.220, spring(int(0.110 * WORK_SR), seed + 62, 0.22, b, 1120.0 * pitch))
    elif spec.family == "ak":
        add(out, 0.018, scrape(int(0.270 * WORK_SR), seed + 63, 0.98, 0.200, b, m, 360.0, 7600.0, 190.0 * pitch))
        add(out, 0.230, impact(int(0.180 * WORK_SR), seed + 64, 0.92, 105.0 * pitch, b, m, 360.0, 7200.0, 24.0))
        add(out, 0.330, spring(int(0.120 * WORK_SR), seed + 65, 0.30, b, 700.0 * pitch))
    elif spec.family == "carbine":
        add(out, 0.014, scrape(int(0.210 * WORK_SR), seed + 66, 0.70, 0.150, b, m, 520.0, 8500.0, 245.0 * pitch))
        add(out, 0.172, impact(int(0.150 * WORK_SR), seed + 67, 0.76, 135.0 * pitch, b, m, 540.0, 9000.0, 28.0))
        add(out, 0.258, spring(int(0.110 * WORK_SR), seed + 68, 0.25, b, 920.0 * pitch))
    elif spec.family == "pulse":
        add(out, 0.010, servo(int(0.230 * WORK_SR), seed + 69, 0.86, 0.185, 960.0 * pitch, b, False))
        add(out, 0.190, impact(int(0.130 * WORK_SR), seed + 70, 0.56, 230.0 * pitch, b, m, 1100.0, 15000.0, 46.0))
        add(out, 0.262, spring(int(0.110 * WORK_SR), seed + 71, 0.28, b, 1620.0 * pitch))
    elif spec.family == "scattergun":
        add(out, 0.014, scrape(int(0.400 * WORK_SR), seed + 72, 1.12, 0.315, b, m, 240.0, 6500.0, 145.0 * pitch))
        add(out, 0.315, impact(int(0.190 * WORK_SR), seed + 73, 1.02, 92.0 * pitch, b, m, 320.0, 6200.0, 22.0))
        add(out, 0.438, spring(int(0.110 * WORK_SR), seed + 74, 0.28, b, 560.0 * pitch))
    elif spec.family == "marksman":
        add(out, 0.016, scrape(int(0.430 * WORK_SR), seed + 75, 1.04, 0.330, b, m, 300.0, 7400.0, 165.0 * pitch))
        add(out, 0.345, impact(int(0.180 * WORK_SR), seed + 76, 0.84, 105.0 * pitch, b, m, 360.0, 6800.0, 23.0))
        add(out, 0.462, impact(int(0.135 * WORK_SR), seed + 77, 0.48, 390.0 * pitch, b, m, 1000.0, 9800.0, 62.0))
    else:
        add(out, 0.014, servo(int(0.300 * WORK_SR), seed + 78, 1.00, 0.255, 820.0 * pitch, b, False))
        add(out, 0.260, impact(int(0.170 * WORK_SR), seed + 79, 0.72, 150.0 * pitch, b, m, 760.0, 12000.0, 28.0))
        add(out, 0.365, spring(int(0.120 * WORK_SR), seed + 80, 0.32, b, 1500.0 * pitch))
    return finish(out, spec, "reload_end")


def render_dry(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "dry") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "dry", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright
    if spec.family in ("pulse", "railbolt"):
        add(out, 0.006, servo(int(0.110 * WORK_SR), seed + 1, 0.52, 0.082, 1280.0 * pitch, b, True))
        add(out, 0.052, impact(int(0.090 * WORK_SR), seed + 2, 0.28, 760.0 * pitch, b, m, 2800.0, 16000.0, 120.0))
        add(out, 0.105, spring(int(0.070 * WORK_SR), seed + 3, 0.16, b, 1720.0 * pitch))
    elif spec.family in ("scattergun", "marksman", "ak"):
        add(out, 0.008, impact(int(0.105 * WORK_SR), seed + 4, 0.50, 240.0 * pitch, b, m, 900.0, 9600.0, 70.0))
        add(out, 0.060, impact(int(0.090 * WORK_SR), seed + 5, 0.30, 660.0 * pitch, b, m, 1900.0, 13000.0, 112.0))
        add(out, 0.112, spring(int(0.070 * WORK_SR), seed + 6, 0.14, b, 840.0 * pitch))
    else:
        add(out, 0.006, impact(int(0.088 * WORK_SR), seed + 7, 0.44, 520.0 * pitch, b, m, 1700.0, 15000.0, 104.0))
        add(out, 0.056, impact(int(0.080 * WORK_SR), seed + 8, 0.26, 980.0 * pitch, b, m, 2800.0, 17000.0, 150.0))
        add(out, 0.104, spring(int(0.065 * WORK_SR), seed + 9, 0.12, b, 1320.0 * pitch))
    return finish(out, spec, "dry")


def render_equip(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "equip") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "equip", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright
    add(out, 0.000, hand_cloth(int(min(frames, 0.220 * WORK_SR)), seed + 10, 0.090 + m * 0.015))
    if spec.family in ("pulse", "railbolt"):
        add(out, 0.035, servo(int(0.190 * WORK_SR), seed + 11, 0.70, 0.150, 760.0 * pitch, b, False))
        add(out, 0.180, impact(int(0.125 * WORK_SR), seed + 12, 0.46, 260.0 * pitch, b, m, 1300.0, 14000.0, 54.0))
    elif spec.family in ("scattergun", "marksman"):
        add(out, 0.030, scrape(int(0.250 * WORK_SR), seed + 13, 0.76, 0.180, b, m, 360.0, 7200.0, 165.0 * pitch))
        add(out, 0.215, impact(int(0.140 * WORK_SR), seed + 14, 0.58, 126.0 * pitch, b, m, 420.0, 7200.0, 30.0))
    elif spec.family == "ak":
        add(out, 0.028, scrape(int(0.210 * WORK_SR), seed + 15, 0.68, 0.145, b, m, 400.0, 7600.0, 190.0 * pitch))
        add(out, 0.180, impact(int(0.130 * WORK_SR), seed + 16, 0.54, 150.0 * pitch, b, m, 520.0, 8400.0, 34.0))
    else:
        add(out, 0.028, scrape(int(0.165 * WORK_SR), seed + 17, 0.48, 0.105, b, m, 720.0, 10800.0, 310.0 * pitch))
        add(out, 0.145, impact(int(0.118 * WORK_SR), seed + 18, 0.44, 240.0 * pitch, b, m, 980.0, 13000.0, 52.0))
    return finish(out, spec, "equip")


def render_mag_out(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "mag_out") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "mag_out", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright
    add(out, 0.008, impact(int(0.115 * WORK_SR), seed + 20, 0.48 + m * 0.10, 260.0 * pitch, b, m, 700.0, 9500.0, 50.0))
    add(out, 0.060, scrape(int(0.190 * WORK_SR), seed + 21, 0.55 + m * 0.11, 0.135, b, m, 420.0, 8600.0, 210.0 * pitch))
    if spec.family in ("ak", "scattergun", "marksman"):
        add(out, 0.205, impact(int(0.110 * WORK_SR), seed + 22, 0.38, 120.0 * pitch, b, m, 380.0, 6500.0, 32.0))
    else:
        add(out, 0.178, impact(int(0.092 * WORK_SR), seed + 23, 0.28, 520.0 * pitch, b, m, 1500.0, 12500.0, 88.0))
    return finish(out, spec, "mag_out")


def render_shell(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "shell") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "shell", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright
    if spec.family == "scattergun":
        add(out, 0.012, impact(int(0.120 * WORK_SR), seed + 30, 0.72, 132.0 * pitch, b, m, 360.0, 6400.0, 28.0))
        add(out, 0.075, scrape(int(0.150 * WORK_SR), seed + 31, 0.50, 0.105, b, m, 380.0, 5800.0, 178.0 * pitch))
        add(out, 0.175, impact(int(0.120 * WORK_SR), seed + 32, 0.48, 320.0 * pitch, b, m, 900.0, 7800.0, 60.0))
    elif spec.family == "marksman":
        add(out, 0.012, impact(int(0.135 * WORK_SR), seed + 33, 0.58, 155.0 * pitch, b, m, 420.0, 7200.0, 32.0))
        add(out, 0.100, impact(int(0.115 * WORK_SR), seed + 34, 0.42, 390.0 * pitch, b, m, 900.0, 9000.0, 64.0))
    else:
        return render_mag_in(spec, slot)
    return finish(out, spec, "shell")


def render_bolt(spec: ReloadSpec, slot: int) -> np.ndarray:
    frames = int(event_length(spec, "bolt") * WORK_SR)
    out = np.zeros(frames, dtype=np.float64)
    seed = seed_for(spec.weapon, "bolt", slot)
    pitch = spec.pitches[slot % len(spec.pitches)]
    m = spec.mass
    b = spec.bright
    if spec.family in ("pulse", "railbolt"):
        add(out, 0.006, servo(int(0.210 * WORK_SR), seed + 40, 0.82, 0.170, 920.0 * pitch, b, False))
        add(out, 0.170, impact(int(0.120 * WORK_SR), seed + 41, 0.44, 240.0 * pitch, b, m, 1000.0, 14000.0, 52.0))
    elif spec.family in ("marksman", "scattergun"):
        add(out, 0.014, scrape(int(0.270 * WORK_SR), seed + 42, 0.98, 0.205, b, m, 260.0, 7000.0, 150.0 * pitch))
        add(out, 0.225, impact(int(0.150 * WORK_SR), seed + 43, 0.76, 100.0 * pitch, b, m, 340.0, 6400.0, 24.0))
        add(out, 0.315, spring(int(0.095 * WORK_SR), seed + 44, 0.20, b, 650.0 * pitch))
    elif spec.family == "ak":
        add(out, 0.012, scrape(int(0.210 * WORK_SR), seed + 45, 0.82, 0.160, b, m, 340.0, 7600.0, 185.0 * pitch))
        add(out, 0.175, impact(int(0.135 * WORK_SR), seed + 46, 0.66, 112.0 * pitch, b, m, 360.0, 7400.0, 28.0))
    else:
        add(out, 0.010, scrape(int(0.160 * WORK_SR), seed + 47, 0.58, 0.110, b, m, 760.0, 12000.0, 360.0 * pitch))
        add(out, 0.125, impact(int(0.110 * WORK_SR), seed + 48, 0.48, 220.0 * pitch, b, m, 900.0, 13000.0, 48.0))
    return finish(out, spec, "bolt")


def finish(x: np.ndarray, spec: ReloadSpec, event: str) -> np.ndarray:
    x = highpass(x, 36.0)
    if spec.family in ("pulse", "railbolt"):
        x = peaking(x, 2400.0, 0.85, 2.4)
        x = peaking(x, 7200.0, 0.90, 1.4)
    elif spec.family in ("ak", "scattergun", "marksman"):
        x = peaking(x, 120.0, 0.80, 1.8)
        x = peaking(x, 420.0, 0.85, -1.2)
        x = peaking(x, 6200.0, 0.90, 0.3)
    else:
        x = peaking(x, 180.0, 0.85, 0.8)
        x = peaking(x, 7600.0, 0.90, 0.7)
    x = peaking(x, 3100.0, 0.95, -1.1)
    x = soft_clip(x, 1.10 + spec.mass * 0.10)
    fade_start = int(max(0.0, len(x) / WORK_SR - 0.075) * WORK_SR)
    if fade_start < len(x):
        fade = np.linspace(1.0, 0.0, len(x) - fade_start) ** 1.45
        x[fade_start:] *= fade
    event_offset = {
        "dry": -2.8,
        "equip": -1.2,
        "reload_start": -1.5,
        "mag_out": -0.2,
        "mag_in": 0.0,
        "reload_end": -0.2,
        "bolt": -0.3,
        "shell": 0.0,
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
    low = bandpass(x, 80.0, 350.0)
    mid = bandpass(x, 500.0, 3500.0)
    high = bandpass(x, 4500.0, 14000.0)
    return (
        f"{name}: peak={amp_to_db(peak):.2f}dBFS rms={amp_to_db(rms):.2f}dBFS "
        f"low80-350={amp_to_db(float(np.sqrt(np.mean(low * low)))):.2f}dB "
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


def render_event(spec: ReloadSpec, event: str) -> list[str]:
    stem = f"sfx_weapon_{spec.weapon}_{event}"
    remove_old(stem)
    maker = {
        "dry": render_dry,
        "equip": render_equip,
        "reload_start": render_reload_start,
        "mag_out": render_mag_out,
        "mag_in": render_mag_in,
        "reload_end": render_reload_end,
        "bolt": render_bolt,
        "shell": render_shell,
    }[event]
    lines = [f"[{spec.weapon}:{event}] stem={stem} variants={spec.variants} length={event_length(spec, event):.3f}s targetPeak={spec.peak_db:.1f}dBFS"]
    for slot in range(spec.variants):
        x = maker(spec, slot)
        name = f"{stem}.wav" if slot == 0 else f"{stem}_{slot}.wav"
        write_wav(AUDIO / name, x, seed_for(spec.weapon, event, slot, "write"))
        lines.append(stat_line(name, x))
    return lines


def main() -> None:
    log_lines = ["PULSE weapon reload producer", f"work_sr={WORK_SR} out_sr={OUT_SR}"]
    for spec in WEAPONS:
        for event in EVENTS:
            log_lines.extend(render_event(spec, event))
    LOG_PATH.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    print("\n".join(log_lines))


if __name__ == "__main__":
    main()
