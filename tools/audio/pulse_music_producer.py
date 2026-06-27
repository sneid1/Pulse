#!/usr/bin/env python3
"""Offline no-composer music production for PULSE adaptive music v3 stems.

This is intentionally heavier than the in-engine placeholder. It writes both the
runtime stems in assets/audio and source clips used by the REAPER session.
"""

from __future__ import annotations

import math
import time
import uuid
import wave
from pathlib import Path

import numpy as np
from scipy import signal


SR = 48_000
BPM = 140.0
BARS = 16
BEAT = 60.0 / BPM
SIX = BEAT * 0.25
BAR = BEAT * 4.0
DURATION = BARS * BAR
FRAMES = int(round(DURATION * SR))
ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "assets" / "audio"
MUSIC = AUDIO / "music"
SOURCE = AUDIO / "reaper_source"

STEMS = ("bed", "bass", "drums", "pressure", "boss", "overpulse", "duress")
BIOMES = ("foundry", "furnace", "reliquary")
STINGERS = (
    "room_clear",
    "reward",
    "boss_intro",
    "overpulse",
    "run_win",
    "run_lose",
    "sector_foundry",
    "sector_furnace",
    "sector_reliquary",
    "boss_phase",
    "boss_enrage",
    "anticipation",
)
PROJECT_PATH = SOURCE / "PULSE_adaptive_music.rpp"
ROOTS = np.array([55.00, 55.00, 43.65, 49.00, 55.00, 65.41, 49.00, 41.20])
CHORDS = [
    (0, 3, 7, 10), (0, 3, 7, 12), (0, 4, 7, 10), (0, 4, 7, 10),
    (0, 3, 7, 10), (0, 4, 7, 12), (0, 4, 7, 10), (0, 4, 7, 10),
]
REST = 99
BASS = [
    [REST,0,0,3, REST,0,7,0, REST,0,3,0, REST,7,5,-2],
    [REST,0,3,0, REST,7,0,10, REST,0,3,7, REST,12,10,7],
    [REST,0,0,4, REST,0,7,0, REST,0,4,7, REST,10,7,5],
    [REST,0,2,4, REST,7,4,2, REST,0,7,10, REST,12,10,7],
    [REST,0,0,3, REST,7,0,3, REST,10,7,3, REST,12,10,7],
    [REST,0,4,7, REST,12,7,4, REST,0,4,7, REST,12,10,7],
    [REST,0,2,4, REST,7,4,2, REST,0,7,10, REST,14,12,10],
    [REST,0,4,7, REST,10,7,4, REST,0,4,7, 10,7,4,-1],
]
LEAD = [
    [REST,0,REST,3, 7,REST,10,7, REST,12,10,7, REST,3,5,REST],
    [REST,0,REST,3, 7,REST,10,12, REST,15,12,10, 7,REST,5,3],
    [REST,7,REST,5, 4,REST,7,12, REST,10,7,5, 4,REST,5,7],
    [REST,0,REST,2, 7,REST,10,12, 14,REST,12,10, 7,5,3,2],
    [REST,12,REST,10, 7,REST,3,5, REST,7,10,12, REST,15,12,10],
    [REST,7,REST,4, 0,REST,4,7, REST,12,11,7, 4,REST,7,12],
    [REST,14,REST,12, 10,REST,7,4, REST,7,10,12, 14,REST,12,10],
    [REST,12,10,7, 4,REST,0,4, 7,10,12,10, 7,4,3,-1],
]
BOSS = [
    [0,REST,6,REST, 7,REST,3,REST, 0,REST,-1,REST, 3,REST,6,7],
    [0,REST,6,REST, 10,REST,7,REST, 3,REST,0,REST, 7,REST,10,12],
    [0,REST,5,REST, 7,REST,4,REST, 0,REST,-2,REST, 4,REST,7,10],
    [0,REST,6,REST, 7,REST,10,REST, 12,REST,10,7, 6,3,1,0],
    [12,REST,10,REST, 7,REST,6,REST, 3,REST,0,REST, 6,REST,7,10],
    [0,REST,4,REST, 7,REST,11,REST, 12,REST,11,REST, 7,REST,4,0],
    [0,REST,6,REST, 10,REST,12,REST, 14,REST,12,10, 7,6,3,1],
    [0,4,7,10, 12,10,7,4, 0,4,7,10, 12,10,7,-1],
]


# Per-biome compositional identity (C3). The bed/drums/boss synthesis is shared (the foundation
# and the antagonist theme stay consistent), but the bass and lead (pressure) note tables fork per
# biome so the sectors are distinct compositions, not just EQ variants. Transforms are deterministic
# and stay in the same scale/grid, so frame parity (140 BPM, 16 bars) is preserved automatically.
def _xpose(tbl: list[list[int]], semis: int) -> list[list[int]]:
    return [[n if n == REST else n + semis for n in row] for row in tbl]


def _densify(tbl: list[list[int]], fill_steps: tuple[int, ...]) -> list[list[int]]:
    out = []
    for row in tbl:
        r = list(row)
        for s in fill_steps:
            if r[s] == REST:
                r[s] = 0
        out.append(r)
    return out


def _sparsify(tbl: list[list[int]], drop_steps: tuple[int, ...]) -> list[list[int]]:
    out = []
    for row in tbl:
        r = list(row)
        for s in drop_steps:
            r[s] = REST
        out.append(r)
    return out


def biome_bass(biome: str) -> list[list[int]]:
    if biome == "furnace":
        return _densify(BASS, (2, 10))            # driving offbeat roots
    if biome == "reliquary":
        return _sparsify(BASS, (2, 3, 10, 11))    # leave cold space
    return BASS


def biome_lead(biome: str) -> list[list[int]]:
    if biome == "furnace":
        return _xpose(LEAD, -12)                  # heavier, lower lead
    if biome == "reliquary":
        return _xpose(LEAD, 12)                   # airy, high lead
    return LEAD


def semitone(st: float) -> float:
    return 2.0 ** (st / 12.0)


def saw(t: np.ndarray, hz: float) -> np.ndarray:
    return ((t * hz) % 1.0) * 2.0 - 1.0


def pulse(t: np.ndarray, hz: float, width: float) -> np.ndarray:
    return np.where(((t * hz) % 1.0) < width, 1.0, -1.0)


def softclip(x: np.ndarray, drive: float = 1.0) -> np.ndarray:
    return np.tanh(x * drive)


def env(t: np.ndarray, speed: float) -> np.ndarray:
    return np.exp(-np.maximum(t, 0.0) * speed)


def add_pan(buf: np.ndarray, mono: np.ndarray, pan: float) -> None:
    lg = math.sqrt(0.5 * (1.0 - pan))
    rg = math.sqrt(0.5 * (1.0 + pan))
    buf[0, : len(mono)] += mono * lg
    buf[1, : len(mono)] += mono * rg


def add_event(dst: np.ndarray, start: int, mono: np.ndarray, pan: float = 0.0) -> None:
    if start >= FRAMES:
        return
    end = min(FRAMES, start + len(mono))
    view = dst[:, start:end]
    add_pan(view, mono[: end - start], pan)


def add_stereo_event(dst: np.ndarray, start: int, stereo: np.ndarray) -> None:
    if start >= FRAMES:
        return
    end = min(FRAMES, start + stereo.shape[1])
    dst[:, start:end] += stereo[:, : end - start]


def noise(n: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.uniform(-1.0, 1.0, n)


def bp_noise(n: int, seed: int, hp: float, lp: float) -> np.ndarray:
    x = noise(n, seed)
    sos = signal.butter(2, [hp, lp], "bandpass", fs=SR, output="sos")
    return signal.sosfilt(sos, x)


def step_time(bar_i: int, step_i: int) -> float:
    return bar_i * BAR + step_i * SIX


def kick(length: float = 0.46, drive: float = 2.4) -> np.ndarray:
    n = int(length * SR)
    t = np.arange(n) / SR
    freq = 38.0 + 150.0 * np.exp(-t * 35.0)
    phase = 2.0 * np.pi * np.cumsum(freq) / SR
    body = np.sin(phase) * np.exp(-t * 9.0)
    sub = np.sin(2.0 * np.pi * 48.0 * t) * np.exp(-t * 6.5)
    click = bp_noise(n, 1001, 2300.0, 9500.0) * np.exp(-t * 260.0)
    return softclip(body * 1.25 + sub * 0.35 + click * 0.12, drive)


def ghost_kick() -> np.ndarray:
    return kick(0.22, 2.0) * 0.38


def clap(seed: int) -> np.ndarray:
    n = int(0.32 * SR)
    t = np.arange(n) / SR
    bursts = (
        np.exp(-t * 24.0)
        + np.where(t > 0.012, np.exp(-(t - 0.012) * 31.0), 0.0) * 0.8
        + np.where(t > 0.027, np.exp(-(t - 0.027) * 36.0), 0.0) * 0.55
    )
    body = np.sin(2.0 * np.pi * 185.0 * t) * np.exp(-t * 22.0) * 0.08
    return softclip(bp_noise(n, seed, 650.0, 7200.0) * bursts * 0.42 + body, 1.25)


def hat(seed: int, open_hat: bool) -> np.ndarray:
    n = int((0.19 if open_hat else 0.085) * SR)
    t = np.arange(n) / SR
    decay = np.exp(-t * (16.0 if open_hat else 78.0))
    tone = np.sin(2.0 * np.pi * (7800.0 if open_hat else 10200.0) * t) * 0.08
    return bp_noise(n, seed, 5200.0, 15000.0) * decay * 0.22 + tone * decay


def rim(seed: int) -> np.ndarray:
    n = int(0.09 * SR)
    t = np.arange(n) / SR
    return softclip(
        np.sin(2.0 * np.pi * 740.0 * t) * np.exp(-t * 70.0)
        + bp_noise(n, seed, 900.0, 6800.0) * np.exp(-t * 90.0) * 0.18,
        1.6,
    ) * 0.28


def acid_note(freq: float, length: float, drive: float = 4.2, bright: float = 1.0) -> np.ndarray:
    n = int(length * SR)
    t = np.arange(n) / SR
    e = np.exp(-t * 8.0)
    cutoff = np.exp(-t * 22.0)
    wave = saw(t, freq * 0.997) * 0.44 + pulse(t, freq * 1.003, 0.35) * 0.30
    wave += np.sin(2.0 * np.pi * freq * 0.5 * t) * 0.52
    click = bp_noise(n, int(freq * 13) % 10000, 1600.0, 8500.0) * np.exp(-t * 140.0) * 0.025
    return softclip((wave * (0.78 + cutoff * 0.36 * bright) + click) * e, drive)


def lead_note(freq: float, length: float, seed: int, bright: float) -> np.ndarray:
    n = int(length * SR)
    t = np.arange(n) / SR
    vib = 1.0 + 0.012 * np.sin(2.0 * np.pi * 6.1 * t) + 0.004 * np.sin(2.0 * np.pi * 10.7 * t)
    left = saw(t, freq * vib * 0.996) * 0.42 + pulse(t, freq * 1.002, 0.30) * 0.25
    right = saw(t, freq * vib * 1.007) * 0.40 + pulse(t, freq * 0.991, 0.36) * 0.27
    e = np.exp(-t * 5.8)
    grit = bp_noise(n, seed, 900.0, 6800.0) * np.exp(-t * 22.0) * 0.028
    stereo = np.vstack([softclip(left * e + grit, 3.7), softclip(right * e - grit, 3.7)])
    return stereo * bright


def sidechain_curve() -> np.ndarray:
    t = np.arange(FRAMES) / SR
    beat_phase = t % BEAT
    return 0.22 + 0.78 * (1.0 - np.exp(-beat_phase * 7.0))


def loop_guard() -> np.ndarray:
    t = np.arange(FRAMES) / SR
    fade = 0.006
    return np.minimum(np.clip(t / fade, 0.0, 1.0), np.clip((DURATION - t) / (fade * 0.75), 0.0, 1.0))


def produce(biome: str = "foundry") -> dict[str, np.ndarray]:
    stems = {name: np.zeros((2, FRAMES), dtype=np.float32) for name in STEMS}
    pump = sidechain_curve()
    guard = loop_guard()
    whole_t = np.arange(FRAMES) / SR

    # C3: bass and lead fork per biome for compositional identity; bed/drums/boss stay shared.
    bass_tbl = biome_bass(biome)
    lead_tbl = biome_lead(biome)
    extra_ghost = (3, 9) if biome == "furnace" else ()   # furnace: denser low drive
    skip_clap = (12,) if biome == "reliquary" else ()     # reliquary: sparser, colder

    # Bed: kick + harmonic pressure bed.
    k = kick()
    for b in range(BARS * 4):
        add_event(stems["bed"], int(round(b * BEAT * SR)), k, 0.0)

    for bar_i in range(BARS):
        motif = bar_i % 8
        section = bar_i // 8
        start = int(round(bar_i * BAR * SR))
        end = int(round((bar_i + 1) * BAR * SR))
        t = whole_t[start:end]
        local = t - t[0]
        root = ROOTS[motif]
        chord = CHORDS[motif]
        pad_l = np.zeros_like(t)
        pad_r = np.zeros_like(t)
        for idx, degree in enumerate(chord):
            octave = 0.5 if idx == 0 else 1.0
            h = root * octave * semitone(degree)
            breath = 0.84 + 0.16 * np.sin(2.0 * np.pi * (0.06 + idx * 0.013) * t)
            pad_l += softclip(saw(t, h * (0.995 - idx * 0.0015)) * 0.42 + np.sin(2.0 * np.pi * h * 0.5 * t) * 0.15, 1.4) * breath
            pad_r += softclip(saw(t, h * (1.004 + idx * 0.0012)) * 0.40 + np.sin(2.0 * np.pi * h * 0.5 * t) * 0.15, 1.4) * breath
        off = np.exp(-np.maximum((local % BEAT) - BEAT * 0.5, 0.0) * 7.5)
        pad_env = (0.35 + 0.34 * pump[start:end] + 0.13 * off) * guard[start:end] * (1.12 if section else 0.96)
        stems["bed"][0, start:end] += pad_l * 0.060 * pad_env
        stems["bed"][1, start:end] += pad_r * 0.060 * pad_env
        if section == 1:
            shimmer = np.sin(2.0 * np.pi * root * semitone(chord[-1]) * 8.0 * t) * 0.010
            stems["bed"][:, start:end] += shimmer * pad_env

    # Bass, drums, pressure, boss.
    for bar_i in range(BARS):
        motif = bar_i % 8
        section = bar_i // 8
        root = ROOTS[motif]
        for step in range(16):
            start = int(round(step_time(bar_i, step) * SR))
            note = bass_tbl[motif][step]
            if note != REST:
                if section and step in (6, 14):
                    note += 12
                f = root * 2.0 * semitone(note)
                length = SIX * (0.72 if step in (14, 15) else 0.88)
                mono = acid_note(f, length, 4.3, 1.15 if section else 1.0)
                amp = (1.05 if step in (1, 9) else 0.84) * (1.08 if section else 1.0)
                add_event(stems["bass"], start, mono * amp * 0.46, 0.0)

            open_hat = step in (2, 6, 10, 14)
            add_event(stems["drums"], start, hat(3000 + bar_i * 19 + step, open_hat) * (0.72 if open_hat else 0.52), -0.48 if step % 2 else 0.46)
            if step in (4, 12) and step not in skip_clap:
                c = clap(4000 + bar_i)
                add_event(stems["drums"], start, c * (0.78 if section else 0.68), -0.13)
                add_event(stems["drums"], start, c * 0.56, 0.24)
            if step in (7, 11, 14) or (motif == 7 and step >= 12) or step in extra_ghost:
                add_event(stems["drums"], start, ghost_kick() * (0.24 if not (motif == 7 and step >= 12) else 0.18), 0.0)
            if section and step in (3, 5, 13, 15):
                add_event(stems["drums"], start, rim(5000 + bar_i * 17 + step), -0.32 if step % 2 else 0.32)

            lead = lead_tbl[motif][step]
            if lead != REST:
                if section and step in (10, 14):
                    lead += 12
                f = root * 4.0 * semitone(lead)
                phrase = (1.12 if motif >= 4 else 0.95) * (1.17 if section else 1.0)
                stereo = lead_note(f, SIX * (0.72 if motif == 7 else 0.58), 6000 + bar_i * 23 + step, phrase)
                add_stereo_event(stems["pressure"], start, stereo * 0.125)

            boss_note = BOSS[motif][step]
            if boss_note != REST:
                if section and step in (8, 12, 15):
                    boss_note += 12
                f = root * 2.0 * semitone(boss_note)
                stereo = lead_note(f, SIX * 0.80, 7000 + bar_i * 31 + step, 0.95)
                add_stereo_event(stems["boss"], start, stereo * (0.16 if section else 0.13))

        if motif in (3, 7):
            rs = int(round((bar_i * BAR + BAR * 0.5) * SR))
            n = int(round(BAR * 0.5 * SR))
            tr = np.arange(n) / SR
            rise = np.linspace(0.0, 1.0, n) ** 2
            riser = bp_noise(n, 8000 + bar_i, 700.0, 12000.0) * rise * 0.055
            riser += np.sin(2.0 * np.pi * (320.0 * tr + 2400.0 * tr * tr)) * rise * 0.050
            add_event(stems["pressure"], rs, riser, 0.45)
            add_event(stems["pressure"], rs, riser * 0.68, -0.38)

        start = int(round(bar_i * BAR * SR))
        end = int(round((bar_i + 1) * BAR * SR))
        t = whole_t[start:end]
        drone = softclip(saw(t, root * 0.50 * (1.0 + 0.006 * np.sin(2.0 * np.pi * 0.19 * t))) * 0.28
                         + np.sin(2.0 * np.pi * root * 0.25 * t) * 0.22
                         + saw(t, root * 0.75) * 0.10, 3.0)
        stems["boss"][:, start:end] += drone * (0.16 if section else 0.13) * pump[start:end] * guard[start:end]

    # Duress layer (C1): the near-death tension stem the engine opens with the duress input, distinct
    # per biome (foundry alarm-relay pulse, furnace pressure groan, reliquary cold high drone). Kept
    # moderate here; main() bounds its RMS below the bed so it never fights the foreground.
    t = whole_t
    if biome == "foundry":
        gate = 0.35 + 0.65 * (np.sin(2.0 * np.pi * 1.0 * t) > 0.0)
        relay = np.sin(2.0 * np.pi * 330.0 * t) * np.sin(2.0 * np.pi * 2.0 * t) * gate
        arc = bp_noise(FRAMES, 17001, 1800.0, 9000.0) * (0.4 + 0.6 * gate)
        body = (relay * 0.5 + arc * 0.22).astype(np.float32)
        stems["duress"] += np.vstack([body, np.roll(body, int(0.008 * SR)) * 0.9]) * 0.5
    elif biome == "furnace":
        swell = 0.5 + 0.5 * np.sin(2.0 * np.pi * 0.18 * t)
        groan = np.sin(2.0 * np.pi * 55.0 * t * (1.0 + 0.01 * np.sin(2.0 * np.pi * 0.07 * t)))
        groan += np.sin(2.0 * np.pi * 82.5 * t) * 0.4
        grit = bp_noise(FRAMES, 18001, 60.0, 1400.0) * swell
        body = softclip(groan * 0.6 * swell + grit * 0.4, 2.0).astype(np.float32)
        stems["duress"] += np.vstack([body, np.roll(body, int(0.012 * SR)) * 0.92]) * 0.5
    else:
        cold = np.zeros(FRAMES, dtype=np.float32)
        for i, fhz in enumerate((784.0, 1046.5, 1568.0, 2093.0)):
            drift = 1.0 + 0.003 * np.sin(2.0 * np.pi * (0.02 + i * 0.006) * t)
            cold += np.sin(2.0 * np.pi * fhz * drift * t + i * 0.6) * (0.5 / (1.0 + i))
        air = bp_noise(FRAMES, 19001, 2400.0, 12000.0) * (0.3 + 0.3 * np.sin(2.0 * np.pi * 0.09 * t))
        body = (cold * 0.5 + air * 0.3).astype(np.float32)
        stems["duress"] += np.vstack([body, np.roll(body, int(0.05 * SR)) * 0.9]) * 0.5

    stems["bed"] *= guard
    stems["boss"] *= guard
    stems["duress"] *= guard
    return stems


def add_continuous(dst: np.ndarray, freq: float, amp: float, seed: int, wobble: float = 0.0) -> None:
    t = np.arange(dst.shape[1]) / SR
    drift = 1.0 + wobble * np.sin(2.0 * np.pi * 0.047 * t + seed * 0.01)
    tone = np.sin(2.0 * np.pi * freq * drift * t)
    grit = bp_noise(dst.shape[1], seed, 90.0, 1800.0)
    stereo = np.vstack([tone + grit * 0.16, tone * 0.92 - grit * 0.12])
    dst += stereo.astype(np.float32) * amp


def add_sparse_hits(dst: np.ndarray, every_steps: int, seed: int, amp: float, hp: float, lp: float) -> None:
    for step in range(0, BARS * 16, every_steps):
        start = int(round(step * SIX * SR))
        n = int(round(0.32 * SR))
        t = np.arange(n) / SR
        hit = bp_noise(n, seed + step * 13, hp, lp) * np.exp(-t * 18.0)
        hit += np.sin(2.0 * np.pi * (180.0 + (step % 7) * 31.0) * t) * np.exp(-t * 24.0) * 0.20
        add_event(dst, start, softclip(hit * amp, 1.8), -0.35 if (step // every_steps) % 2 else 0.35)


def add_biome_overpulse(stems: dict[str, np.ndarray], biome: str) -> None:
    t = np.arange(FRAMES) / SR
    guard = loop_guard()
    pump = sidechain_curve()
    stems["overpulse"] += stems["pressure"] * 0.38 + stems["boss"] * 0.30

    if biome == "foundry":
        six_phase = (t % SIX) / SIX
        chop = np.exp(-six_phase * 7.0) * (0.45 + 0.55 * (((np.arange(FRAMES) // int(SIX * SR)) % 4) != 0))
        arcs = bp_noise(FRAMES, 14101, 2400.0, 16000.0)
        relay = np.sin(2.0 * np.pi * 29.0 * t) * np.sin(2.0 * np.pi * 173.0 * t)
        stereo = np.vstack([arcs + relay * 0.20, -np.roll(arcs, int(0.009 * SR)) + relay * 0.15])
        stems["overpulse"] += stereo * (0.010 + 0.030 * chop) * guard
        stems["boss"] += stereo * 0.0045 * guard
        for step in range(3, BARS * 16, 7):
            n = int(round(0.15 * SR))
            tt = np.arange(n) / SR
            arc = bp_noise(n, 14200 + step, 1900.0, 15000.0) * np.exp(-tt * 19.0)
            chirp = np.sin(2.0 * np.pi * (1200.0 * tt + 4200.0 * tt * tt)) * np.exp(-tt * 15.0)
            add_event(stems["overpulse"], int(round(step * SIX * SR)), softclip(arc * 0.090 + chirp * 0.050, 1.6), -0.55 if step % 2 else 0.55)
    elif biome == "furnace":
        flame = bp_noise(FRAMES, 15101, 70.0, 1700.0)
        sub = (
            np.sin(2.0 * np.pi * 42.0 * t * (1.0 + 0.010 * np.sin(2.0 * np.pi * 0.09 * t)))
            + np.sin(2.0 * np.pi * 63.0 * t) * 0.55
        )
        pressure = softclip(sub * 0.62 + flame * 0.34, 2.5)
        stereo = np.vstack([pressure, np.roll(pressure, int(0.013 * SR)) * 0.92])
        stems["overpulse"] += stereo * (0.040 + 0.044 * pump) * guard
        stems["boss"] += stereo * 0.010 * guard
        for step in range(0, BARS * 16, 9):
            n = int(round(0.46 * SR))
            tt = np.arange(n) / SR
            vent = bp_noise(n, 15200 + step, 260.0, 5600.0) * np.exp(-tt * 5.2)
            vent += np.sin(2.0 * np.pi * 92.0 * tt) * np.exp(-tt * 6.5) * 0.38
            add_event(stems["overpulse"], int(round((step + 0.5) * SIX * SR)), softclip(vent * 0.105, 2.0), -0.38 if step % 3 else 0.38)
    else:
        shimmer = np.zeros(FRAMES, dtype=np.float32)
        for i, f in enumerate((705.0, 941.0, 1274.0, 1760.0, 2489.0)):
            drift = 1.0 + 0.0035 * np.sin(2.0 * np.pi * (0.031 + i * 0.007) * t)
            shimmer += np.sin(2.0 * np.pi * f * drift * t + i * 0.73) * (0.018 / (1.0 + i * 0.25))
        air = bp_noise(FRAMES, 16101, 1800.0, 15000.0)
        cold = shimmer + air * (0.006 + 0.014 * np.maximum(0.0, np.sin(2.0 * np.pi * 0.12 * t)))
        stereo = np.vstack([cold, np.roll(cold, int(0.043 * SR)) * 0.90])
        stems["overpulse"] += stereo * (0.75 + 0.25 * pump) * guard
        stems["boss"] += stereo * 0.20 * guard
        for bar_i in range(0, BARS, 2):
            n = int(round(1.05 * SR))
            tt = np.arange(n) / SR
            f = 523.25 * semitone((bar_i % 8) - 5)
            bell = np.sin(2.0 * np.pi * f * tt) * np.exp(-tt * 2.3)
            bell += np.sin(2.0 * np.pi * f * 2.01 * tt) * np.exp(-tt * 3.7) * 0.36
            add_event(stems["overpulse"], int(round((bar_i * BAR + BEAT * 2.5) * SR)), bell * 0.044, -0.62 if bar_i % 4 else 0.62)


def biome_raw(biome: str) -> dict[str, np.ndarray]:
    stems = produce(biome)   # C3: per-biome bass/lead + duress are baked here, then flavored below
    t = np.arange(FRAMES) / SR
    guard = loop_guard()
    if biome == "foundry":
        add_continuous(stems["bed"], 58.0, 0.020, 11001, 0.004)
        add_continuous(stems["bed"], 119.5, 0.009, 11002, 0.002)
        stems["bass"] *= 0.94
        stems["drums"] *= 1.08
        stems["pressure"] *= 1.10
        relay = bp_noise(FRAMES, 11003, 2200.0, 9800.0) * (0.5 + 0.5 * np.sin(2.0 * np.pi * 0.5 * t))
        stems["pressure"] += np.vstack([relay, -relay]) * 0.006 * guard
        add_sparse_hits(stems["drums"], 19, 11100, 0.055, 900.0, 7600.0)
    elif biome == "furnace":
        add_continuous(stems["bed"], 42.0, 0.034, 12001, 0.010)
        stems["bed"] += np.vstack([
            bp_noise(FRAMES, 12002, 120.0, 760.0),
            bp_noise(FRAMES, 12003, 130.0, 690.0),
        ]) * 0.018 * guard
        stems["bass"] *= 1.12
        stems["drums"] *= 0.96
        stems["pressure"] *= 0.93
        stems["boss"] *= 1.08
        add_sparse_hits(stems["drums"], 11, 12100, 0.075, 280.0, 4200.0)
        ember = bp_noise(FRAMES, 12101, 4200.0, 15000.0) * np.maximum(0.0, np.sin(2.0 * np.pi * 0.21 * t))
        stems["pressure"] += np.vstack([ember * 0.004, ember * 0.007]) * guard
    else:
        wind = bp_noise(FRAMES, 13001, 55.0, 900.0)
        add_continuous(stems["bed"], 36.7, 0.022, 13002, 0.013)
        stems["bed"] += np.vstack([wind * 0.024, np.roll(wind, int(0.071 * SR)) * 0.021]) * guard
        stems["bass"] *= 0.76
        stems["drums"] *= 0.70
        stems["pressure"] *= 0.88
        stems["boss"] *= 0.92
        for bar_i in range(0, BARS, 2):
            start = int(round((bar_i * BAR + BEAT * 3.0) * SR))
            n = int(round(1.35 * SR))
            tt = np.arange(n) / SR
            bell = np.sin(2.0 * np.pi * (880.0 + 7.0 * (bar_i % 4)) * tt) * np.exp(-tt * 2.6)
            bell += np.sin(2.0 * np.pi * 1320.0 * tt) * np.exp(-tt * 3.8) * 0.40
            add_event(stems["bed"], start, bell * 0.020, -0.55 if bar_i % 4 else 0.55)
    add_biome_overpulse(stems, biome)
    return stems


def post(stems: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    def filt(buf: np.ndarray, mode: str, freq) -> np.ndarray:
        sos = signal.butter(2, freq, mode, fs=SR, output="sos")
        return signal.sosfilt(sos, buf, axis=1)

    stems["bed"] = filt(stems["bed"], "lowpass", 9500.0)
    stems["bass"] = filt(stems["bass"], "lowpass", 4200.0)
    stems["drums"] = filt(stems["drums"], "highpass", 28.0)
    stems["pressure"] = filt(stems["pressure"], "highpass", 150.0)
    stems["boss"] = filt(stems["boss"], "highpass", 42.0)
    stems["overpulse"] = filt(stems["overpulse"], "highpass", 48.0)
    stems["duress"] = filt(stems["duress"], "highpass", 32.0)   # v4 (C1) tension layer

    def delay(name: str, delay_s: float, mix: float) -> None:
        d = int(delay_s * SR)
        b = stems[name]
        wet_l = np.zeros(FRAMES, dtype=np.float32)
        wet_r = np.zeros(FRAMES, dtype=np.float32)
        wet_l[d:] = b[1, :-d] * mix
        wet_r[d:] = b[0, :-d] * mix
        b[0] += wet_l
        b[1] += wet_r

    delay("drums", SIX * 0.75, 0.04)
    delay("pressure", SIX * 3.0, 0.17)
    delay("pressure", SIX * 5.0, 0.075)
    delay("boss", SIX * 2.0, 0.10)
    delay("overpulse", SIX * 1.5, 0.08)

    widths = {"bed": 1.08, "bass": 0.55, "drums": 1.22, "pressure": 1.34, "boss": 1.18, "overpulse": 1.30, "duress": 1.16}
    targets = {"bed": 0.82, "bass": 0.74, "drums": 0.76, "pressure": 0.72, "boss": 0.78, "overpulse": 0.70, "duress": 0.46}
    guard = loop_guard()
    for name, b in stems.items():
        mid = (b[0] + b[1]) * 0.5
        side = (b[0] - b[1]) * 0.5 * widths[name]
        b[0] = mid + side
        b[1] = mid - side
        peak = max(float(np.max(np.abs(b))), 1e-6)
        gain = min(targets[name] / peak, 1.65)
        stems[name] = np.tanh(b * gain * 1.02).astype(np.float32)
        stems[name] *= guard
    return stems


def stinger_duration(name: str) -> float:
    return {
        "room_clear": 1.05,
        "reward": 1.18,
        "boss_intro": 2.25,
        "overpulse": 1.35,
        "run_win": 2.45,
        "run_lose": 1.85,
        "sector_foundry": 1.18,
        "sector_furnace": 1.28,
        "sector_reliquary": 1.34,
        "boss_phase": 1.80,
        "boss_enrage": 2.00,
        "anticipation": 1.60,
    }[name]


def make_stinger(name: str) -> np.ndarray:
    dur = stinger_duration(name)
    n = int(round(dur * SR))
    t = np.arange(n) / SR
    out = np.zeros((2, n), dtype=np.float32)

    def add_mono(start_s: float, mono: np.ndarray, pan: float = 0.0) -> None:
        start = int(round(start_s * SR))
        if start >= n:
            return
        end = min(n, start + len(mono))
        add_pan(out[:, start:end], mono[: end - start], pan)

    if name == "room_clear":
        hit = softclip(bp_noise(int(0.38 * SR), 21001, 180.0, 6200.0) * env(np.arange(int(0.38 * SR)) / SR, 9.0), 1.5)
        add_mono(0.0, hit * 0.34, 0.0)
        add_mono(0.18, acid_note(220.0, 0.48, 3.0, 0.8) * 0.18, -0.18)
        add_mono(0.34, acid_note(330.0, 0.44, 3.0, 0.7) * 0.12, 0.22)
    elif name == "reward":
        for i, f in enumerate((330.0, 440.0, 660.0)):
            add_mono(i * 0.10, acid_note(f, 0.62, 2.6, 0.72) * (0.12 - i * 0.018), -0.25 + i * 0.25)
        shine = bp_noise(n, 22001, 4200.0, 13000.0) * np.exp(-t * 2.7) * 0.025
        out += np.vstack([shine, np.roll(shine, int(0.021 * SR))]) 
    elif name == "boss_intro":
        rise = (np.linspace(0.0, 1.0, n) ** 2.2)
        siren = np.sin(2.0 * np.pi * (72.0 * t + 28.0 * t * t)) * rise
        grit = bp_noise(n, 23001, 90.0, 2400.0) * rise
        out += np.vstack([siren + grit * 0.20, siren * 0.94 - grit * 0.18]) * 0.15
        add_mono(dur - 0.42, kick(0.48, 3.1) * 0.42, 0.0)
        add_mono(dur - 0.30, acid_note(110.0, 0.60, 4.6, 1.1) * 0.22, 0.0)
    elif name == "overpulse":
        burst = bp_noise(n, 24001, 700.0, 15000.0) * np.exp(-t * 3.5)
        tone = np.sin(2.0 * np.pi * (880.0 + 220.0 * np.exp(-t * 5.0)) * t) * np.exp(-t * 2.4)
        out += np.vstack([burst * 0.045 + tone * 0.12, -burst * 0.035 + tone * 0.11])
        for off, f in ((0.0, 220.0), (0.12, 330.0), (0.24, 440.0)):
            add_mono(off, acid_note(f, 0.38, 5.0, 1.2) * 0.15, 0.0)
    elif name == "sector_foundry":
        arc = bp_noise(n, 27001, 1700.0, 15000.0) * np.exp(-t * 5.0)
        relay = np.sin(2.0 * np.pi * (185.0 + 740.0 * np.exp(-t * 4.0)) * t) * np.exp(-t * 3.6)
        out += np.vstack([arc * 0.050 + relay * 0.13, -arc * 0.043 + relay * 0.10])
        add_mono(0.08, rim(27002) * 0.80, -0.38)
        add_mono(0.27, rim(27003) * 0.55, 0.42)
    elif name == "sector_furnace":
        add_mono(0.0, kick(0.48, 3.0) * 0.30, 0.0)
        roar = bp_noise(n, 27101, 80.0, 4200.0) * np.exp(-t * 2.8)
        tone = np.sin(2.0 * np.pi * (70.0 - 12.0 * np.minimum(t, 1.0)) * t) * np.exp(-t * 2.1)
        out += np.vstack([roar * 0.055 + tone * 0.14, np.roll(roar, int(0.017 * SR)) * 0.050 + tone * 0.12])
        add_mono(0.24, clap(27102) * 0.42, 0.20)
    elif name == "sector_reliquary":
        wind = bp_noise(n, 27201, 240.0, 9500.0) * np.exp(-t * 2.4)
        bell = np.sin(2.0 * np.pi * 523.25 * t) * np.exp(-t * 2.0)
        bell += np.sin(2.0 * np.pi * 784.0 * t) * np.exp(-t * 2.9) * 0.46
        out += np.vstack([wind * 0.022 + bell * 0.12, np.roll(wind, int(0.041 * SR)) * 0.025 + bell * 0.095])
        add_mono(0.38, acid_note(261.63, 0.64, 2.4, 0.55) * 0.050, -0.55)
    elif name == "run_win":
        add_mono(0.0, kick(0.44, 2.7) * 0.32, 0.0)
        for i, f in enumerate((220.0, 330.0, 440.0, 660.0)):
            add_mono(0.18 + i * 0.18, acid_note(f, 0.72, 3.1, 0.95) * (0.16 - i * 0.014), -0.35 + i * 0.23)
        air = bp_noise(n, 25001, 5000.0, 16000.0) * np.exp(-t * 1.8) * 0.020
        out += np.vstack([air, np.roll(air, int(0.037 * SR))])
    elif name == "run_lose":
        add_mono(0.0, kick(0.55, 2.1) * 0.22, 0.0)
        fall = np.sin(2.0 * np.pi * (140.0 - 42.0 * np.minimum(t, 1.2)) * t) * np.exp(-t * 1.4)
        grit = bp_noise(n, 26001, 60.0, 1200.0) * np.exp(-t * 1.9)
        out += np.vstack([fall + grit * 0.20, fall * 0.88 - grit * 0.16]) * 0.13
    elif name == "boss_phase":
        # v4 (M3): descending detuned gong + drum fill + sub drop (transformed antagonist motif).
        gong = np.sin(2.0 * np.pi * (220.0 - 60.0 * np.minimum(t, 1.6)) * t) * np.exp(-t * 1.6)
        gong += np.sin(2.0 * np.pi * (221.5 - 60.0 * np.minimum(t, 1.6)) * t) * np.exp(-t * 1.7) * 0.8
        grit = bp_noise(n, 28001, 80.0, 3200.0) * np.exp(-t * 2.2)
        out += np.vstack([gong * 0.12 + grit * 0.06, gong * 0.11 - grit * 0.05])
        for off in (0.0, 0.16, 0.30):
            add_mono(off, kick(0.34, 2.6) * 0.30, 0.0)
        sub = np.sin(2.0 * np.pi * (70.0 - 28.0 * np.minimum(t, 1.2)) * t) * np.exp(-t * 1.5)
        out += np.vstack([sub, sub]) * 0.16
        add_mono(0.40, acid_note(110.0, 0.6, 4.2, 1.0) * 0.16, 0.0)
    elif name == "boss_enrage":
        # v4 (M3): aggressive noise riser + distorted impact + the boss motif a fifth up.
        rise = np.linspace(0.0, 1.0, n) ** 2.0
        riser = bp_noise(n, 29001, 400.0, 14000.0) * rise * 0.10
        riser += np.sin(2.0 * np.pi * (180.0 * t + 3200.0 * t * t)) * rise * 0.07
        out += np.vstack([riser, np.roll(riser, int(0.006 * SR)) * 0.92])
        add_mono(dur - 0.55, kick(0.52, 3.4) * 0.46, 0.0)
        for i, st in enumerate((7, 14, 19)):
            add_mono(dur - 0.50 + i * 0.10, acid_note(110.0 * semitone(st), 0.40, 5.2, 1.3) * (0.16 - i * 0.02), -0.4 + i * 0.4)
    elif name == "anticipation":
        # v4 (C2): rising tension sweep into a soft downbeat, distinct from the reward breath; fired
        # on DoorsOpen and quantized into combat re-entry.
        rise = np.linspace(0.0, 1.0, n) ** 1.6
        sweep = np.sin(2.0 * np.pi * (140.0 * t + 900.0 * t * t)) * rise
        air = bp_noise(n, 30001, 1200.0, 11000.0) * rise * 0.05
        out += np.vstack([sweep * 0.10 + air, -sweep * 0.085 + np.roll(air, int(0.01 * SR))])
        add_mono(dur - 0.20, kick(0.40, 2.4) * 0.26, 0.0)
        add_mono(dur - 0.16, acid_note(165.0, 0.42, 3.4, 0.9) * 0.10, 0.0)
    else:
        raise ValueError(f"unknown stinger {name}")

    fade = np.minimum(np.clip(t / 0.006, 0.0, 1.0), np.clip((dur - t) / 0.030, 0.0, 1.0))
    out *= fade
    peak = max(float(np.max(np.abs(out))), 1e-6)
    out = np.tanh(out * min(0.74 / peak, 2.4)).astype(np.float32)
    return out


def write_wav(path: Path, stereo: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pcm = np.clip(stereo.T, -1.0, 1.0)
    pcm16 = (pcm * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes(pcm16.tobytes())


def project_guid(name: str) -> str:
    return "{" + str(uuid.uuid5(uuid.NAMESPACE_URL, f"pulse-adaptive-music/{name}")).upper() + "}"


def fmt_time(value: float) -> str:
    return f"{value:.14f}".rstrip("0").rstrip(".")


def write_reaper_project(tracks: list[dict[str, object]]) -> None:
    """Write a REAPER-editable source session without launching REAPER."""
    loop_len = fmt_time(DURATION)
    bar_len = BAR
    lines: list[str] = [
        f'<REAPER_PROJECT 0.1 "7.74/win64" {int(time.time())} 0',
        "  <NOTES 0 2",
        "    |PULSE adaptive music v4 source. Three biome stem banks (incl. a per-biome duress tension layer), overpulse layers, hub bed, and event stingers (incl. boss_phase / boss_enrage / anticipation) are produced headlessly by tools/audio/pulse_music_producer.py.",
        "    |Runtime loops live in assets/audio/music, are stereo 48 kHz WAV, 140 BPM, 16 bars, frame-matched, and share the same downbeat. Bass and lead fork per biome for compositional identity.",
        "  >",
        "  RIPPLE 0 0",
        "  AUTOXFADE 129",
        "  ENVATTACH 3",
        "  PEAKGAIN 1",
        "  FEEDBACK 0",
        "  PANLAW 1",
        "  PROJOFFS 0 0 0",
        "  MAXPROJLEN 0 0",
        "  GRID 3199 8 1 8 1 0 0 0",
        "  TIMEMODE 1 5 -1 30 0 0 -1 0",
        "  PANMODE 3",
        f"  CURSOR {loop_len}",
        "  ZOOM 100 1291 0",
        "  LOOP 0",
        "  LOOPGRAN 0 4",
        '  RECORD_PATH "Media" ""',
        "  TIMELOCKMODE 1",
        "  TEMPOENVLOCKMODE 1",
        "  ITEMMIX 1",
        "  DEFPITCHMODE 589824 0",
        f'  RENDER_FILE "{AUDIO}"',
        "  RENDER_PATTERN $track",
        "  RENDER_FMT 0 2 48000",
        "  RENDER_1X 0",
        f"  RENDER_RANGE 0 0 {loop_len} 0 0",
        "  RENDER_RESAMPLE 3 0 1",
        "  RENDER_ADDTOPROJ 0",
        "  RENDER_STEMS 128",
        "  RENDER_DITHER 0",
        "  SAMPLERATE 48000 1 0",
        "  <RENDER_CFG",
        "    d2F2ZQ==",
        "  >",
        "  TEMPO 140 4 4 0",
        "  PLAYRATE 1 0 0.25 4",
        "  SELECTION 0 0",
        "  SELECTION2 0 0",
        "  MASTERTRACKHEIGHT 0 0",
        "  MASTER_NCH 2 2",
        "  MASTER_VOLUME 1 0 -1 -1 1",
        "  MASTER_PANMODE 3",
        "  MASTER_FX 1",
        "  <TEMPOENVEX",
        f"    EGUID {project_guid('tempo-env')}",
        "    ACT 1 -1",
        "    VIS 1 0 1",
        "    LANEHEIGHT 0 0",
        "    ARM 0",
        "    DEFSHAPE 1 -1 -1",
        '    PT 0.000000000000 140.0000000000 1 262148 0 1 0 "" 0 169 0 ABBB',
        "  >",
        f"  MARKER 1 0 PULSE_16_BAR_LOOP 1 0 {loop_len} R {project_guid('loop-region')} 0 1",
    ]

    for bar_i in range(BARS):
        lines.append(
            f"  MARKER {bar_i + 2} {fmt_time(bar_i * bar_len)} bar_{bar_i + 1} 0 0 1 B {project_guid(f'bar-{bar_i + 1}')} 0 2"
        )

    for idx, track in enumerate(tracks, start=1):
        short_name = str(track["short_name"])
        track_name = str(track["track_name"])
        source_name = str(track["source_name"])
        length = fmt_time(float(track["length"]))
        volume = float(track["volume"])
        color = int(track["color"])
        track_id = project_guid(f"track-{short_name}")
        item_id = project_guid(f"item-{short_name}")
        take_id = project_guid(f"take-{short_name}")
        lines.extend(
            [
                f"  <TRACK {track_id}",
                f"    NAME {track_name}",
                f"    PEAKCOL {color}",
                "    BEAT -1",
                "    AUTOMODE 0",
                "    PANLAWFLAGS 3",
                f"    VOLPAN {volume:.2f} 0 -1 -1 1",
                "    MUTESOLO 0 0 0",
                "    IPHASE 0",
                "    PLAYOFFS 0 1",
                "    ISBUS 0 0",
                "    BUSCOMP 0 0 0 0 0",
                "    SHOWINMIX 1 0.6667 0.5 1 0.5 0 0 0 0",
                "    FIXEDLANES 9 0 0 0 0",
                "    SEL 0",
                "    REC 0 0 1 0 0 0 0 0",
                "    VU 64",
                "    NCHAN 2",
                "    FX 1",
                f"    TRACKID {track_id}",
                "    PERF 0",
                "    MIDIOUT -1",
                "    MAINSEND 1 0",
                "    <ITEM",
                "      POSITION 0",
                "      SNAPOFFS 0",
                f"      LENGTH {length}",
                "      LOOP 0",
                "      ALLTAKES 0",
                "      FADEIN 1 0 0 1 0 0 0",
                "      FADEOUT 1 0 0 1 0 0 0",
                "      MUTE 0 0",
                "      SEL 0",
                f"      IGUID {item_id}",
                f"      IID {idx}",
                f"      NAME {source_name}",
                "      VOLPAN 1 0 1 -1",
                "      SOFFS 0",
                "      PLAYRATE 1 1 0 -1 0 0.0025",
                "      CHANMODE 0",
                f"      GUID {take_id}",
                "      <SOURCE WAVE",
                f'        FILE "{source_name}"',
                "      >",
                "    >",
                "  >",
            ]
        )

    lines.append(">")
    PROJECT_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    SOURCE.mkdir(parents=True, exist_ok=True)
    MUSIC.mkdir(parents=True, exist_ok=True)
    tracks: list[dict[str, object]] = []
    log_lines = [
        "PULSE adaptive music v4 offline producer",
        f"bpm={BPM} bars={BARS} sr={SR} frames={FRAMES}",
    ]

    loop_colors = {
        "foundry": 30576217,
        "furnace": 26328640,
        "reliquary": 21537000,
        "hub": 31222212,
    }
    stem_volume = {
        "bed": 0.92,
        "bass": 0.88,
        "drums": 0.84,
        "pressure": 0.82,
        "boss": 0.78,
        "overpulse": 0.80,
        "duress": 0.72,
    }

    foundry_bed: np.ndarray | None = None
    for biome in BIOMES:
        stems = post(biome_raw(biome))
        # v4 (C1): keep the duress layer comfortably below the bed so it never fights the foreground
        # (the validator enforces >= 3 dB headroom). Bound by RMS, which is robust to crest factor.
        bed_rms = float(np.sqrt(np.mean(stems["bed"] ** 2)))
        dur_rms = float(np.sqrt(np.mean(stems["duress"] ** 2)))
        if dur_rms > 1e-9 and bed_rms > 1e-9:
            target_rms = bed_rms * (10.0 ** (-5.0 / 20.0))   # 5 dB below the bed
            if dur_rms > target_rms:
                stems["duress"] = (stems["duress"] * (target_rms / dur_rms)).astype(np.float32)
        if biome == "foundry":
            foundry_bed = stems["bed"].copy()
        for name in STEMS:
            stereo = stems[name]
            short = f"{biome}_{name}"
            src = SOURCE / f"source_{short}.wav"
            dst = MUSIC / f"{short}.wav"
            write_wav(src, stereo)
            write_wav(dst, stereo)
            peak = float(np.max(np.abs(stereo)))
            rms = float(np.sqrt(np.mean(stereo**2)))
            log_lines.append(f"{short}: peak={peak:.4f} rms={rms:.4f} src={src} dst={dst}")
            tracks.append(
                {
                    "short_name": short,
                    "track_name": f"music_{short}",
                    "source_name": src.name,
                    "length": DURATION,
                    "volume": stem_volume[name],
                    "color": loop_colors[biome],
                }
            )

    if foundry_bed is None:
        raise RuntimeError("foundry bed was not produced")

    hub = foundry_bed * 0.62
    add_continuous(hub, 48.5, 0.010, 14001, 0.008)
    hub = signal.sosfilt(signal.butter(2, 7600.0, "lowpass", fs=SR, output="sos"), hub, axis=1).astype(np.float32)
    hub *= loop_guard()
    peak = max(float(np.max(np.abs(hub))), 1e-6)
    hub = np.tanh(hub * min(0.56 / peak, 1.3)).astype(np.float32)
    src = SOURCE / "source_hub_bed.wav"
    dst = MUSIC / "hub_bed.wav"
    write_wav(src, hub)
    write_wav(dst, hub)
    log_lines.append(
        f"hub_bed: peak={float(np.max(np.abs(hub))):.4f} rms={float(np.sqrt(np.mean(hub**2))):.4f} src={src} dst={dst}"
    )
    tracks.append(
        {
            "short_name": "hub_bed",
            "track_name": "music_hub_bed",
            "source_name": src.name,
            "length": DURATION,
            "volume": 0.74,
            "color": loop_colors["hub"],
        }
    )

    stinger_color = 22040801
    for name in STINGERS:
        stereo = make_stinger(name)
        short = f"stinger_{name}"
        src = SOURCE / f"source_{short}.wav"
        dst = MUSIC / f"{short}.wav"
        write_wav(src, stereo)
        write_wav(dst, stereo)
        peak = float(np.max(np.abs(stereo)))
        rms = float(np.sqrt(np.mean(stereo**2)))
        log_lines.append(f"{short}: peak={peak:.4f} rms={rms:.4f} src={src} dst={dst}")
        tracks.append(
            {
                "short_name": short,
                "track_name": f"music_{short}",
                "source_name": src.name,
                "length": stinger_duration(name),
                "volume": 0.86,
                "color": stinger_color,
            }
        )

    write_reaper_project(tracks)
    log_lines.append(f"reaper_project: {PROJECT_PATH}")
    (SOURCE / "PULSE_offline_producer_log.txt").write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    print("\n".join(log_lines))


if __name__ == "__main__":
    main()
