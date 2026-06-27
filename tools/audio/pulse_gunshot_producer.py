#!/usr/bin/env python3
"""Offline layered gunshot production for PULSE weapon fire banks.

The runtime engine wants short mono WAV banks named:
  sfx_fire_<weapon>.wav, sfx_fire_<weapon>_1.wav, ...

This producer keeps the game-facing contract simple while doing the heavier work
offline: source-shot detection, pitch variation, body/crack/sub layering, EQ,
short tails, peak targeting, and dithered 48 kHz export.
"""

from __future__ import annotations

import math
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile


ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "assets" / "audio"
SOURCE = ROOT / "assets" / "external"
LOG_PATH = AUDIO / "PULSE_gunshot_producer_log.txt"

WORK_SR = 96_000
OUT_SR = 48_000


@dataclass(frozen=True)
class SourceSpec:
    key: str
    path: Path
    channel: int | None = None
    min_gap_s: float = 0.10
    threshold: float = 0.22


SOURCES = {
    "ak_close": SourceSpec(
        "ak_close",
        SOURCE / "sonniss" / "gdc2020_ak74" / "AKSD0813_AK74_spot_A_close_burst_03_rounds.wav",
        None,
        0.080,
        0.24,
    ),
    "ak_far": SourceSpec(
        "ak_far",
        SOURCE / "sonniss" / "gdc2020_ak74" / "AKSD0854_AK74_spot_D_far_burst_03_rounds.wav",
        None,
        0.080,
        0.20,
    ),
    "ak_rear": SourceSpec(
        "ak_rear",
        SOURCE / "sonniss" / "gdc2020_ak74" / "AK0405_AK74_rear_long_distance_burst_05_rounds.wav",
        0,
        0.080,
        0.18,
    ),
    "m3": SourceSpec(
        "m3",
        SOURCE / "sonniss" / "gdc2020_weapons" / "STSRC_WoWW2_Wep_SMG_M3_GreaseGun_Submachine_Gun_Single_Shot_X10_Comp_Full.wav",
        0,
        0.70,
        0.20,
    ),
    "luger": SourceSpec(
        "luger",
        SOURCE / "sonniss" / "gdc2020_weapons" / "STDSGN_WoWW2_Wep_Pistol_Luger_Shot_X5.wav",
        0,
        0.85,
        0.18,
    ),
    "future": SourceSpec(
        "future",
        SOURCE / "sonniss" / "gdc2020_weapons" / "PM_SFG_VOL1_WEAPON_4_4_GUN_GUNSHOT_FUTURISTIC.wav",
        1,
        0.10,
        0.18,
    ),
    "sks": SourceSpec(
        "sks",
        SOURCE / "gunshots" / "extracted" / "sounds" / "sks.wav",
        None,
        0.18,
        0.20,
    ),
    "mosin": SourceSpec(
        "mosin",
        SOURCE / "gunshots" / "extracted" / "sounds" / "mosin.wav",
        None,
        0.45,
        0.18,
    ),
    "shotty": SourceSpec(
        "shotty",
        SOURCE / "gunshots" / "extracted" / "sounds" / "shotty.wav",
        None,
        0.12,
        0.16,
    ),
}


@dataclass(frozen=True)
class BankSpec:
    stem: str
    variants: int
    length_s: float
    fade_s: float
    peak_db: float
    pitches: tuple[float, ...]


BANKS = {
    "default": BankSpec("sfx_fire", 8, 0.142, 0.096, -5.5, (1.000, 0.996, 1.004, 0.991, 1.007, 0.994, 1.002, 0.989)),
    "ak47": BankSpec("sfx_fire_ak47", 8, 0.145, 0.098, -5.2, (1.000, 0.997, 1.004, 0.992, 1.006, 0.995, 1.002, 0.990)),
    "carbine": BankSpec("sfx_fire_carbine", 8, 0.132, 0.082, -7.0, (1.000, 1.006, 0.996, 1.010, 0.992, 1.004, 0.989, 1.008)),
    "pistol": BankSpec("sfx_fire_pistol", 6, 0.168, 0.108, -6.0, (1.000, 0.992, 1.006, 0.988, 1.004, 0.996)),
    "pulse_smg": BankSpec("sfx_fire_pulse_smg", 8, 0.092, 0.055, -9.0, (1.000, 1.011, 0.992, 1.018, 0.985, 1.007, 0.996, 1.014)),
    "machine_pistol": BankSpec("sfx_fire_machine_pistol", 8, 0.094, 0.056, -9.5, (1.000, 1.012, 0.990, 1.020, 0.985, 1.008, 0.995, 1.016)),
    "marksman": BankSpec("sfx_fire_marksman", 5, 0.360, 0.255, -11.5, (1.000, 0.994, 1.006, 0.989, 1.011)),
    "scattergun": BankSpec("sfx_fire_scattergun", 8, 0.420, 0.310, -13.0, (1.000, 0.992, 1.006, 0.985, 1.011, 0.996, 1.004, 0.989)),
    "railbolt": BankSpec("sfx_fire_railbolt", 5, 0.300, 0.205, -9.0, (1.000, 0.994, 1.008, 0.989, 1.012)),
}


def db_to_amp(db: float) -> float:
    return 10.0 ** (db / 20.0)


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def normalize_dtype(data: np.ndarray) -> np.ndarray:
    if data.dtype.kind == "f":
        return data.astype(np.float64)
    if data.dtype == np.int16:
        return data.astype(np.float64) / 32768.0
    if data.dtype == np.int32:
        return data.astype(np.float64) / 2147483648.0
    raise RuntimeError(f"Unsupported WAV dtype: {data.dtype}")


def read_source(spec: SourceSpec) -> np.ndarray:
    if not spec.path.exists():
        raise FileNotFoundError(spec.path)
    sr, data = wavfile.read(spec.path)
    x = normalize_dtype(data)
    if x.ndim > 1:
        if spec.channel is None:
            x = x.mean(axis=1)
        else:
            x = x[:, spec.channel]
    if sr != WORK_SR:
        x = signal.resample_poly(x, WORK_SR, sr)
    return np.asarray(x, dtype=np.float64)


def sos_filter(x: np.ndarray, sos: np.ndarray) -> np.ndarray:
    return signal.sosfilt(sos, x)


def highpass(x: np.ndarray, hz: float, order: int = 2) -> np.ndarray:
    return sos_filter(x, signal.butter(order, hz, "highpass", fs=WORK_SR, output="sos"))


def lowpass(x: np.ndarray, hz: float, order: int = 2) -> np.ndarray:
    return sos_filter(x, signal.butter(order, hz, "lowpass", fs=WORK_SR, output="sos"))


def bandpass(x: np.ndarray, lo: float, hi: float, order: int = 2) -> np.ndarray:
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


def detect_shots(x: np.ndarray, min_gap_s: float, threshold_factor: float) -> list[int]:
    probe = np.abs(highpass(x, 80.0))
    win = max(1, int(0.0015 * WORK_SR))
    env = np.convolve(probe, np.ones(win) / win, mode="same")
    threshold = max(float(np.max(env)) * threshold_factor, 0.0015)
    min_gap = int(min_gap_s * WORK_SR)
    peaks: list[int] = []
    last = -min_gap
    for i, value in enumerate(env):
        if value < threshold or i - last < min_gap:
            continue
        lo = max(0, i - int(0.006 * WORK_SR))
        hi = min(len(x), i + int(0.028 * WORK_SR))
        peak = lo + int(np.argmax(np.abs(x[lo:hi])))
        peaks.append(peak)
        last = peak
    return peaks


def extract(x: np.ndarray, peak: int, frames: int, pre_s: float = 0.0018) -> np.ndarray:
    start = max(0, peak - int(pre_s * WORK_SR))
    seg = x[start:start + frames].copy()
    if len(seg) < frames:
        seg = np.pad(seg, (0, frames - len(seg)))
    ramp = int(0.00018 * WORK_SR)
    if ramp > 1:
        seg[:ramp] *= np.linspace(0.0, 1.0, ramp)
    return seg


def pitch_window(x: np.ndarray, ratio: float, frames: int) -> np.ndarray:
    idx = np.arange(frames, dtype=np.float64) * ratio
    src = np.arange(len(x), dtype=np.float64)
    return np.interp(idx, src, x, left=0.0, right=0.0)


def layer(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], key: str, slot: int, frames: int, pitch: float, pre_s: float = 0.0018) -> np.ndarray:
    source = pool[key]
    found = peaks[key]
    return layer_from_peak(source, found[slot % len(found)], frames, pitch, pre_s)


def layer_from_peak(source: np.ndarray, peak: int, frames: int, pitch: float, pre_s: float = 0.0018) -> np.ndarray:
    raw_frames = int(frames * max(1.0, pitch) + 0.040 * WORK_SR)
    return pitch_window(extract(source, peak, raw_frames, pre_s), pitch, frames)


def ranked_layer(
    pool: dict[str, np.ndarray],
    peaks: dict[str, list[int]],
    key: str,
    slot: int,
    frames: int,
    pitch: float,
    pre_s: float = 0.0018,
    score_s: float = 0.120,
    top: int = 8,
) -> np.ndarray:
    source = pool[key]
    score_frames = int(score_s * WORK_SR)
    scored: list[tuple[float, int]] = []
    for peak in peaks[key]:
        seg = extract(source, peak, score_frames, pre_s)
        rms = float(np.sqrt(np.mean(seg * seg)))
        peak_amp = float(np.max(np.abs(seg)))
        scored.append((rms + peak_amp * 0.28, peak))
    ranked = [peak for _, peak in sorted(scored, reverse=True)]
    usable = ranked[: max(1, min(top, len(ranked)))]
    return layer_from_peak(source, usable[slot % len(usable)], frames, pitch, pre_s)


def synth_snap(frames: int, seed: int, hp_hz: float = 4200.0, decay: float = 125.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = rng.uniform(-1.0, 1.0, frames)
    snap = bandpass(n, hp_hz, 19000.0)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    return snap * np.exp(-t * decay)


def synth_sub(frames: int, base_hz: float, sweep_hz: float, decay: float, delay_s: float = 0.004) -> np.ndarray:
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    freq = base_hz + sweep_hz * np.exp(-t * 58.0)
    phase = 2.0 * np.pi * np.cumsum(freq) / WORK_SR
    tone = (np.sin(phase) * 0.78 + np.sin(phase * 2.0) * 0.24) * np.exp(-t * decay)
    delay = int(delay_s * WORK_SR)
    if delay > 0:
        tone = np.pad(tone[:-delay], (delay, 0))
    return tone


def synth_mech(frames: int, seed: int, start_s: float, dur_s: float, mass: float, bright: float, tone_hz: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    out = np.zeros(frames, dtype=np.float64)
    start = int(start_s * WORK_SR)
    n = min(frames - start, max(1, int(dur_s * WORK_SR)))
    if n <= 0:
        return out
    t = np.arange(n, dtype=np.float64) / WORK_SR
    env = np.exp(-t * (26.0 + bright * 18.0))
    scrape_env = (1.0 - np.exp(-t * 85.0)) * np.exp(-t * (18.0 + mass * 7.0))
    metal = bandpass(rng.uniform(-1.0, 1.0, n), 520.0, 11800.0) * env * (0.10 + bright * 0.055)
    body = np.sin(2.0 * np.pi * tone_hz * t) * np.exp(-t * (38.0 + mass * 9.0)) * (0.09 + mass * 0.040)
    scrape = bandpass(rng.uniform(-1.0, 1.0, n), 900.0, 7600.0) * scrape_env * 0.045
    out[start:start + n] += soft_clip(metal + body + scrape, 1.08)
    return out


def synth_room_tail(frames: int, seed: int, delay_s: float, decay: float, amp: float, lo_hz: float, hi_hz: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    out = np.zeros(frames, dtype=np.float64)
    start = int(delay_s * WORK_SR)
    n = frames - start
    if n <= 0:
        return out
    t = np.arange(n, dtype=np.float64) / WORK_SR
    noise = bandpass(rng.uniform(-1.0, 1.0, n), lo_hz, hi_hz)
    flutter = 0.65 + 0.35 * np.sin(2.0 * np.pi * (18.0 + (seed & 7)) * t)
    out[start:] += noise * np.exp(-t * decay) * flutter * amp
    for tap, gain in ((0.021, 0.42), (0.043, 0.30), (0.071, 0.22)):
        tap_start = start + int(tap * WORK_SR)
        if tap_start < frames:
            tap_n = frames - tap_start
            out[tap_start:] += noise[:tap_n] * gain * np.exp(-t[:tap_n] * (decay * 1.15)) * amp
    return out


def apply_tail_and_level(x: np.ndarray, spec: BankSpec, drive: float = 1.12) -> np.ndarray:
    frames = len(x)
    x = highpass(x, 28.0)
    x = soft_clip(x, drive)
    fade_start = int(spec.fade_s * WORK_SR)
    if fade_start < frames:
        fade = np.linspace(1.0, 0.0, frames - fade_start) ** 1.55
        x[fade_start:] *= fade
    x = highpass(x, 26.0)
    peak = max(float(np.max(np.abs(x))), 1e-9)
    return x * (db_to_amp(spec.peak_db) / peak)


def make_ak(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    close = layer(pool, peaks, "ak_close", slot, frames, pitch)
    far = layer(pool, peaks, "ak_far", slot, frames, pitch * 0.998)
    rear = layer(pool, peaks, "ak_rear", slot, frames, pitch * 0.995)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    direct = highpass(close, 40.0)
    direct = peaking(direct, 115.0, 0.80, 1.4)
    direct = peaking(direct, 340.0, 0.85, -2.0)
    direct = peaking(direct, 2550.0, 0.90, 2.6)
    direct = peaking(direct, 6200.0, 0.80, 4.4)
    direct = peaking(direct, 9800.0, 0.90, 3.0)
    direct = lowpass(direct, 18500.0)

    body = lowpass(close, 760.0)
    body = peaking(body, 78.0, 0.72, 2.8)
    body = peaking(body, 155.0, 0.82, 2.0)
    body = peaking(body, 520.0, 0.90, -3.4)
    body = soft_clip(body * 3.0, 1.08) * np.exp(-t * 10.0)

    crack = bandpass(close, 4200.0, 18500.0)
    crack += synth_snap(frames, 0xA47000 + slot, 5200.0, 150.0) * 0.18
    crack = peaking(crack, 7200.0, 0.80, 6.0)
    crack = peaking(crack, 10800.0, 0.90, 4.0)
    crack = soft_clip(crack * 2.0, 1.05) * np.exp(-t * 100.0)

    room = highpass(far * 0.72 + rear * 0.28, 80.0)
    room = lowpass(room, 5200.0)
    room = peaking(room, 260.0, 0.90, -3.5) * np.exp(-t * 8.0)

    sub = synth_sub(frames, 58.0, 26.0, 28.0, 0.004)
    mech = synth_mech(frames, 0xA4A100 + slot, 0.041, 0.078, 1.16, 0.88, 235.0)
    early = synth_room_tail(frames, 0xA4A200 + slot, 0.024, 15.0, 0.030, 260.0, 7200.0)
    return apply_tail_and_level(direct * 0.42 + body * 0.38 + crack * 0.62 + room * 0.10 + sub * 0.13 + mech * 0.24 + early, spec, 1.10)


def make_pistol(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    m3 = layer(pool, peaks, "m3", slot, frames, pitch)
    luger = layer(pool, peaks, "luger", slot, frames, pitch * 0.996)
    future = layer(pool, peaks, "future", slot, frames, 1.0)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    close = highpass(m3, 32.0)
    close = peaking(close, 120.0, 0.82, 1.4)
    close = peaking(close, 420.0, 0.85, -2.0)
    close = peaking(close, 3200.0, 0.90, 2.4)
    close = peaking(close, 6800.0, 0.82, 3.8)
    close = lowpass(close, 17800.0)

    body = lowpass(luger, 920.0)
    body = peaking(body, 78.0, 0.70, 2.8)
    body = peaking(body, 140.0, 0.85, 2.2)
    body = peaking(body, 560.0, 0.85, -3.2)
    body = soft_clip(body * 3.8, 1.12) * np.exp(-t * 9.2)

    crack = bandpass(m3, 4200.0, 18000.0) * 0.70
    crack += bandpass(luger, 5200.0, 18500.0) * 0.38
    crack += bandpass(future, 6400.0, 19000.0) * 0.16
    crack += synth_snap(frames, 0x510700 + slot, 5600.0, 135.0) * 0.12
    crack = peaking(crack, 8400.0, 0.85, 5.5)
    crack = soft_clip(crack * 1.9, 1.05) * np.exp(-t * 82.0)

    sub = synth_sub(frames, 66.0, 18.0, 24.0, 0.006)
    slide = synth_mech(frames, 0x515100 + slot, 0.032, 0.082, 0.66, 1.18, 520.0)
    early = synth_room_tail(frames, 0x515200 + slot, 0.020, 18.0, 0.024, 340.0, 8200.0)
    return apply_tail_and_level(close * 0.44 + body * 0.54 + crack * 0.58 + sub * 0.10 + slide * 0.20 + early, spec, 1.12)


def make_carbine(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    sks = layer(pool, peaks, "sks", slot, frames, pitch, 0.0012)
    ak = layer(pool, peaks, "ak_close", slot, frames, pitch * 1.004)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    direct = highpass(sks, 38.0)
    direct = peaking(direct, 105.0, 0.85, 1.5)
    direct = peaking(direct, 380.0, 0.85, -2.4)
    direct = peaking(direct, 2800.0, 0.88, 2.8)
    direct = peaking(direct, 7200.0, 0.82, 4.0)
    direct = lowpass(direct, 18000.0)

    body = lowpass(sks * 0.72 + ak * 0.28, 740.0)
    body = peaking(body, 82.0, 0.72, 1.8)
    body = peaking(body, 155.0, 0.82, 1.6)
    body = peaking(body, 500.0, 0.82, -3.8)
    body = soft_clip(body * 2.8, 1.05) * np.exp(-t * 12.0)

    crack = bandpass(ak, 4500.0, 18500.0) * 0.50 + bandpass(sks, 3800.0, 16500.0) * 0.52
    crack += synth_snap(frames, 0xCA8200 + slot, 6000.0, 170.0) * 0.10
    crack = peaking(crack, 7600.0, 0.82, 5.0)
    crack = soft_clip(crack * 1.75, 1.02) * np.exp(-t * 115.0)

    sub = synth_sub(frames, 62.0, 18.0, 34.0, 0.0035)
    mech = synth_mech(frames, 0xCA9100 + slot, 0.036, 0.070, 0.92, 1.04, 315.0)
    early = synth_room_tail(frames, 0xCA9200 + slot, 0.019, 19.0, 0.024, 300.0, 7600.0)
    return apply_tail_and_level(direct * 0.42 + body * 0.36 + crack * 0.55 + sub * 0.09 + mech * 0.20 + early, spec, 1.08)


def make_smg(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    m3 = layer(pool, peaks, "m3", slot, frames, pitch * 1.010, 0.0010)
    future = layer(pool, peaks, "future", slot, frames, pitch)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    direct = highpass(m3, 70.0)
    direct = peaking(direct, 170.0, 0.90, -1.2)
    direct = peaking(direct, 900.0, 0.90, -2.8)
    direct = peaking(direct, 3900.0, 0.85, 2.2)
    direct = peaking(direct, 8200.0, 0.82, 4.2)
    direct = lowpass(direct, 17000.0)

    body = lowpass(m3, 780.0)
    body = peaking(body, 92.0, 0.80, 1.2)
    body = peaking(body, 210.0, 0.95, -1.0)
    body = soft_clip(body * 2.2, 1.04) * np.exp(-t * 16.0)

    electric = bandpass(future, 1500.0, 12000.0)
    electric += np.sin(2.0 * np.pi * (1780.0 + slot * 27.0) * t) * np.exp(-t * 58.0) * 0.055
    electric += np.sin(2.0 * np.pi * (3920.0 + slot * 41.0) * t) * np.exp(-t * 82.0) * 0.025
    electric = peaking(electric, 4200.0, 0.85, 3.0)
    electric = peaking(electric, 8800.0, 0.82, 3.4)
    electric = soft_clip(electric * 1.65, 1.02) * np.exp(-t * 42.0)

    snap = synth_snap(frames, 0x5A1900 + slot, 6500.0, 210.0)
    sub = synth_sub(frames, 72.0, 10.0, 42.0, 0.0025)
    mech = synth_mech(frames, 0x5A1A00 + slot, 0.024, 0.050, 0.56, 1.28, 760.0)
    early = synth_room_tail(frames, 0x5A1B00 + slot, 0.014, 24.0, 0.016, 520.0, 9800.0)
    return apply_tail_and_level(direct * 0.33 + body * 0.25 + electric * 0.42 + snap * 0.13 + sub * 0.045 + mech * 0.18 + early, spec, 1.06)


def make_marksman(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    mosin = ranked_layer(pool, peaks, "mosin", slot, frames, pitch, 0.0020, 0.150, 5)
    sks = ranked_layer(pool, peaks, "sks", slot, frames, pitch * 1.002, 0.0014, 0.090, 8)
    ak = layer(pool, peaks, "ak_close", slot, frames, pitch * 0.996, 0.0014)
    rear = layer(pool, peaks, "ak_rear", slot, frames, pitch * 0.992, 0.0020)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    direct = highpass(mosin * 0.74 + sks * 0.26, 30.0)
    direct = peaking(direct, 86.0, 0.72, 2.0)
    direct = peaking(direct, 150.0, 0.80, 2.8)
    direct = peaking(direct, 420.0, 0.82, -2.2)
    direct = peaking(direct, 2700.0, 0.86, 2.8)
    direct = peaking(direct, 6200.0, 0.82, 4.5)
    direct = peaking(direct, 10800.0, 0.90, 3.0)
    direct = lowpass(direct, 18200.0)

    body = lowpass(mosin * 0.82 + sks * 0.18, 880.0)
    body = peaking(body, 62.0, 0.70, 2.8)
    body = peaking(body, 118.0, 0.76, 4.0)
    body = peaking(body, 245.0, 0.82, 1.0)
    body = peaking(body, 560.0, 0.82, -3.8)
    body = soft_clip(body * 3.8, 1.12) * np.exp(-t * 5.4)

    crack = bandpass(mosin, 3600.0, 17800.0) * 0.54
    crack += bandpass(ak, 4600.0, 18500.0) * 0.50
    crack += bandpass(sks, 4200.0, 16500.0) * 0.24
    crack += synth_snap(frames, 0x5A1F300 + slot, 5800.0, 105.0) * 0.12
    crack = peaking(crack, 7200.0, 0.82, 5.8)
    crack = peaking(crack, 11200.0, 0.90, 3.8)
    crack = soft_clip(crack * 1.75, 1.04) * np.exp(-t * 58.0)

    report = highpass(mosin * 0.55 + rear * 0.45, 62.0)
    report = lowpass(report, 5600.0)
    report = peaking(report, 220.0, 0.90, -1.0)
    report = peaking(report, 1350.0, 0.88, -1.4)
    report = report * np.exp(-t * 4.1)

    sub = synth_sub(frames, 49.0, 34.0, 17.0, 0.006)
    bolt = synth_mech(frames, 0x5A1F400 + slot, 0.205, 0.125, 1.36, 0.96, 155.0)
    early = synth_room_tail(frames, 0x5A1F500 + slot, 0.036, 9.0, 0.038, 180.0, 6200.0)
    mixed = direct * 0.36 + body * 0.55 + crack * 0.62 + report * 0.18 + sub * 0.14 + bolt * 0.34 + early
    return apply_tail_and_level(mixed, spec, 1.13)


def make_scattergun(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    shotty = ranked_layer(pool, peaks, "shotty", slot, frames, pitch, 0.0022, 0.180, 1)
    sks = ranked_layer(pool, peaks, "sks", slot, frames, pitch * 0.996, 0.0015, 0.100, 8)
    mosin = ranked_layer(pool, peaks, "mosin", slot, frames, pitch * 0.990, 0.0020, 0.130, 5)
    ak = layer(pool, peaks, "ak_close", slot, frames, pitch * 1.003, 0.0013)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    direct = highpass(shotty * 0.82 + sks * 0.18, 24.0)
    direct = peaking(direct, 68.0, 0.72, 2.0)
    direct = peaking(direct, 118.0, 0.80, 3.4)
    direct = peaking(direct, 270.0, 0.85, 1.4)
    direct = peaking(direct, 620.0, 0.82, -2.8)
    direct = peaking(direct, 2800.0, 0.88, 2.2)
    direct = peaking(direct, 6800.0, 0.82, 4.2)
    direct = lowpass(direct, 17800.0)

    body = lowpass(shotty * 0.70 + mosin * 0.30, 980.0)
    body = peaking(body, 54.0, 0.70, 3.0)
    body = peaking(body, 104.0, 0.78, 4.2)
    body = peaking(body, 190.0, 0.86, 2.0)
    body = peaking(body, 540.0, 0.82, -3.8)
    body = soft_clip(body * 3.9, 1.16) * np.exp(-t * 4.7)

    blast = bandpass(shotty, 520.0, 5200.0)
    blast = peaking(blast, 1250.0, 0.92, 2.2)
    blast = peaking(blast, 3300.0, 0.86, 2.8)
    blast = soft_clip(blast * 1.75, 1.08) * np.exp(-t * 8.0)

    crack = bandpass(shotty, 3600.0, 17000.0) * 0.48
    crack += bandpass(ak, 4700.0, 18500.0) * 0.45
    crack += synth_snap(frames, 0x5CA77E00 + slot, 5600.0, 120.0) * 0.13
    crack = peaking(crack, 7600.0, 0.84, 5.2)
    crack = peaking(crack, 11800.0, 0.92, 3.2)
    crack = soft_clip(crack * 1.85, 1.05) * np.exp(-t * 72.0)

    rng = np.random.default_rng(0xB00100 + slot)
    mechanical = np.zeros(frames, dtype=np.float64)
    start = int(0.168 * WORK_SR)
    mech_frames = min(frames - start, int(0.120 * WORK_SR))
    if mech_frames > 0:
        mt = np.arange(mech_frames, dtype=np.float64) / WORK_SR
        rack_noise = bandpass(rng.uniform(-1.0, 1.0, mech_frames), 650.0, 7800.0)
        rack = rack_noise * np.exp(-mt * 32.0) * 0.28
        rack += np.sin(2.0 * np.pi * 330.0 * mt) * np.exp(-mt * 42.0) * 0.12
        rack += np.sin(2.0 * np.pi * 1180.0 * mt) * np.exp(-mt * 70.0) * 0.07
        mechanical[start:start + mech_frames] += rack

    sub = synth_sub(frames, 43.0, 34.0, 15.0, 0.007)
    early = synth_room_tail(frames, 0x5CA77F00 + slot, 0.030, 8.8, 0.036, 150.0, 5600.0)
    mixed = direct * 0.34 + body * 0.62 + blast * 0.42 + crack * 0.54 + mechanical * 0.55 + sub * 0.17 + early
    return apply_tail_and_level(mixed, spec, 1.16)


def make_machine_pistol(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    # Panic full-auto sidearm: lighter, brighter, and shorter than the pistol or the
    # pulse SMG so a high-RPM spray reads as a frantic tin-can stutter, not a rifle.
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    m3 = layer(pool, peaks, "m3", slot, frames, pitch * 1.035, 0.0008)
    luger = layer(pool, peaks, "luger", slot, frames, pitch * 1.020, 0.0010)
    future = layer(pool, peaks, "future", slot, frames, pitch)
    t = np.arange(frames, dtype=np.float64) / WORK_SR

    direct = highpass(m3, 120.0)
    direct = peaking(direct, 230.0, 0.90, -2.2)
    direct = peaking(direct, 1100.0, 0.90, -1.6)
    direct = peaking(direct, 4200.0, 0.85, 2.8)
    direct = peaking(direct, 9000.0, 0.82, 4.4)
    direct = lowpass(direct, 17600.0)

    body = lowpass(m3 * 0.62 + luger * 0.38, 860.0)
    body = peaking(body, 150.0, 0.85, 0.8)
    body = peaking(body, 420.0, 0.88, -2.4)
    body = soft_clip(body * 2.4, 1.05) * np.exp(-t * 24.0)

    crack = bandpass(luger, 5000.0, 18500.0) * 0.50 + bandpass(m3, 4200.0, 17000.0) * 0.46
    crack += bandpass(future, 6000.0, 19000.0) * 0.12
    crack += synth_snap(frames, 0x3A91700 + slot, 6800.0, 230.0) * 0.10
    crack = peaking(crack, 8600.0, 0.85, 5.0)
    crack = soft_clip(crack * 1.85, 1.04) * np.exp(-t * 72.0)

    snap = synth_snap(frames, 0x3A91900 + slot, 7200.0, 260.0)
    sub = synth_sub(frames, 78.0, 8.0, 52.0, 0.0020)
    mech = synth_mech(frames, 0x3A91A00 + slot, 0.020, 0.040, 0.50, 1.32, 820.0)
    early = synth_room_tail(frames, 0x3A91B00 + slot, 0.012, 28.0, 0.012, 600.0, 9500.0)
    return apply_tail_and_level(direct * 0.34 + body * 0.20 + crack * 0.50 + snap * 0.12 + sub * 0.040 + mech * 0.16 + early, spec, 1.05)


def make_railbolt(pool: dict[str, np.ndarray], peaks: dict[str, list[int]], spec: BankSpec, slot: int) -> np.ndarray:
    # Charged splash launcher: a short rising coil whine snaps into a bright electric
    # discharge with a delayed sub boom and a controlled energy tail. Energy weapon, so
    # it is synth-led with a recorded sci-fi layer for organic grit.
    frames = int(spec.length_s * WORK_SR)
    pitch = spec.pitches[slot]
    future = layer(pool, peaks, "future", slot, frames, pitch)
    t = np.arange(frames, dtype=np.float64) / WORK_SR
    disc = 0.050                                   # discharge instant
    gate = (t >= disc).astype(np.float64)
    after = np.maximum(t - disc, 0.0)

    # Rising coil charge in the front window.
    charge_n = int(disc * WORK_SR)
    ct = np.arange(charge_n, dtype=np.float64) / WORK_SR
    cspan = max(1e-4, disc)
    cfreq = (320.0 + 1500.0 * (ct / cspan)) * pitch
    cphase = 2.0 * np.pi * np.cumsum(cfreq) / WORK_SR
    charge = np.zeros(frames, dtype=np.float64)
    charge[:charge_n] = (np.sin(cphase) + 0.40 * np.sin(2.0 * cphase)) * (ct / cspan) ** 1.6 * 0.16

    # Bright electric discharge (recorded sci-fi grit + synth snap), gated to the discharge.
    zap = bandpass(future, 1200.0, 16000.0)
    zap += synth_snap(frames, 0x5A11B00 + slot, 5200.0, 90.0) * 0.55
    zap = peaking(zap, 5200.0, 0.85, 3.4)
    zap = soft_clip(zap * 1.7, 1.05) * gate * np.exp(-after * 34.0)

    # Falling rail snap and punchy discharge body.
    snap_freq = (2600.0 - 2100.0 * np.clip(after / 0.045, 0.0, 1.0)) * pitch
    snap_phase = 2.0 * np.pi * np.cumsum(snap_freq) / WORK_SR
    rail = np.sin(snap_phase) * gate * np.exp(-after * 46.0) * 0.30
    bf = 150.0 * pitch
    body = (np.sin(2.0 * np.pi * bf * t) * 0.70 + np.sin(2.0 * np.pi * bf * 2.0 * t) * 0.22)
    body = soft_clip(body, 1.10) * gate * np.exp(-after * 16.0) * 0.52

    # Delayed sub boom (the splash charge leaving the rail) + energy tail.
    sub = synth_sub(frames, 52.0, 30.0, 13.0, disc + 0.004)
    sparkle = np.sin(2.0 * np.pi * (5200.0 + slot * 80.0) * t) * gate * np.exp(-after * 26.0) * 0.05
    early = synth_room_tail(frames, 0x5A11C00 + slot, disc + 0.020, 9.0, 0.030, 200.0, 6000.0)
    mixed = charge * 0.60 + zap * 0.52 + rail * 0.42 + body + sub * 0.55 + sparkle + early
    return apply_tail_and_level(mixed, spec, 1.10)


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
    front = x[: int(min(len(x), 0.070 * WORK_SR))]
    low = bandpass(front, 35.0, 180.0)
    mid = bandpass(front, 500.0, 3500.0)
    high = bandpass(front, 6000.0, 14000.0)
    return (
        f"{name}: peak={amp_to_db(peak):.2f}dBFS rms={amp_to_db(rms):.2f}dBFS "
        f"low35-180={amp_to_db(float(np.sqrt(np.mean(low * low)))):.2f}dB "
        f"mid0.5-3.5k={amp_to_db(float(np.sqrt(np.mean(mid * mid)))):.2f}dB "
        f"high6-14k={amp_to_db(float(np.sqrt(np.mean(high * high)))):.2f}dB"
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


def render_bank(label: str, pool: dict[str, np.ndarray], peaks: dict[str, list[int]]) -> list[str]:
    spec = BANKS[label]
    remove_old(spec.stem)
    maker = {
        "default": make_carbine,
        "ak47": make_ak,
        "carbine": make_carbine,
        "pistol": make_pistol,
        "pulse_smg": make_smg,
        "machine_pistol": make_machine_pistol,
        "marksman": make_marksman,
        "scattergun": make_scattergun,
        "railbolt": make_railbolt,
    }[label]
    lines = [f"[{label}] stem={spec.stem} variants={spec.variants} length={spec.length_s:.3f}s targetPeak={spec.peak_db:.1f}dBFS"]
    for slot in range(spec.variants):
        x = maker(pool, peaks, spec, slot)
        name = f"{spec.stem}.wav" if slot == 0 else f"{spec.stem}_{slot}.wav"
        write_wav(AUDIO / name, x, 0x600D5000 + slot + len(label) * 101)
        lines.append(stat_line(name, x))
    return lines


def main() -> None:
    pool: dict[str, np.ndarray] = {}
    peaks: dict[str, list[int]] = {}
    log_lines = ["PULSE layered gunshot producer", f"work_sr={WORK_SR} out_sr={OUT_SR}"]
    for key, spec in SOURCES.items():
        pool[key] = read_source(spec)
        found = detect_shots(pool[key], spec.min_gap_s, spec.threshold)
        if not found:
            raise RuntimeError(f"No shots detected in {key}: {spec.path}")
        peaks[key] = found
        log_lines.append(f"source {key}: shots={len(found)} first={[round(p / WORK_SR, 3) for p in found[:10]]} path={spec.path}")

    for label in ("default", "ak47", "carbine", "pistol", "pulse_smg", "machine_pistol",
                  "marksman", "scattergun", "railbolt"):
        log_lines.extend(render_bank(label, pool, peaks))

    LOG_PATH.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    print("\n".join(log_lines))


if __name__ == "__main__":
    main()
