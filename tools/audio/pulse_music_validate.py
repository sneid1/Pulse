#!/usr/bin/env python3
"""Validate the PULSE adaptive music v4 bank.

The runtime contract is intentionally strict: every looping stem must be a
stereo 48 kHz WAV with the same exact frame count and downbeat, while stingers
must be clean one-shots routed through the music bus.
"""

from __future__ import annotations

import argparse
import math
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile


ROOT = Path(__file__).resolve().parents[2]
MUSIC = ROOT / "assets" / "audio" / "music"
SR = 48_000
BPM = 140.0
BARS = 16
EXPECTED_LOOP_FRAMES = int(round(BARS * 4.0 * (60.0 / BPM) * SR))
BIOMES = ("foundry", "furnace", "reliquary")
STEMS = ("bed", "bass", "drums", "pressure", "boss", "overpulse", "duress")
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
# v4 (C1): the duress tension layer must sit at least this far below the bed (RMS) so it never
# fights the foreground when the engine opens it under near-death.
DURESS_BELOW_BED_DB = 3.0
CLIP_EPS = 2.0 / 32768.0


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def required_loop_names() -> list[str]:
    return [f"{biome}_{stem}.wav" for biome in BIOMES for stem in STEMS] + ["hub_bed.wav"]


def required_stinger_names() -> list[str]:
    return [f"stinger_{name}.wav" for name in STINGERS]


def read_wav(path: Path) -> tuple[int, int, np.ndarray]:
    with wave.open(str(path), "rb") as f:
        channels = f.getnchannels()
        width = f.getsampwidth()
    sr, data = wavfile.read(path)
    if data.dtype == np.int16:
        x = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        x = data.astype(np.float64) / 2147483648.0
    elif data.dtype.kind == "f":
        x = data.astype(np.float64)
    else:
        raise RuntimeError(f"unsupported WAV dtype {data.dtype}")
    return int(sr), int(width), x


def true_peak_db(x: np.ndarray) -> float:
    chans = [x[:, c] for c in range(x.shape[1])] if x.ndim > 1 else [x]
    peak = 0.0
    for chan in chans:
        if len(chan) >= 8:
            up = signal.resample_poly(chan, 4, 1)
            peak = max(peak, float(np.max(np.abs(up))))
        elif len(chan):
            peak = max(peak, float(np.max(np.abs(chan))))
    return amp_to_db(peak)


def loop_seam_db(x: np.ndarray) -> float:
    if len(x) < 2:
        return 0.0
    first = x[0]
    last = x[-1]
    return amp_to_db(float(np.max(np.abs(first - last))))


def analyze(path: Path, is_loop: bool, ceiling_db: float) -> dict[str, object]:
    sr, width, x = read_wav(path)
    frames = int(x.shape[0])
    channels = 1 if x.ndim == 1 else int(x.shape[1])
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    rms = float(np.sqrt(np.mean(x * x))) if x.size else 0.0
    dc = float(np.mean(x)) if x.size else 0.0
    true_peak = true_peak_db(x) if x.size else -240.0
    seam = loop_seam_db(x) if is_loop and x.size else -240.0
    fails: list[str] = []

    if not path.exists():
        fails.append("missing")
    if sr != SR:
        fails.append(f"sr={sr}!={SR}")
    if width != 2:
        fails.append(f"sampwidth={width}!={2}")
    if channels != 2:
        fails.append(f"channels={channels}!=2")
    if frames <= 0:
        fails.append("empty")
    if is_loop and frames != EXPECTED_LOOP_FRAMES:
        fails.append(f"frames={frames}!={EXPECTED_LOOP_FRAMES}")
    if peak <= 1e-5 or rms <= 1e-6:
        fails.append("silent")
    if amp_to_db(rms) < -60.0:
        fails.append(f"rms={amp_to_db(rms):.2f}dB too low")
    if amp_to_db(rms) > -4.0:
        fails.append(f"rms={amp_to_db(rms):.2f}dB too hot")
    if true_peak > ceiling_db:
        fails.append(f"true_peak={true_peak:.2f}>{ceiling_db:.2f}")
    if abs(dc) > 0.02:
        fails.append(f"dc={dc:+.4f}")
    if np.count_nonzero(np.abs(x) >= (1.0 - CLIP_EPS)) > 0:
        fails.append("pinned-samples")
    if is_loop and seam > -34.0:
        fails.append(f"seam={seam:.2f}dB")

    return {
        "path": path,
        "ok": not fails,
        "fails": fails,
        "sr": sr,
        "channels": channels,
        "width": width,
        "frames": frames,
        "seconds": frames / float(sr) if sr else 0.0,
        "peak_db": amp_to_db(peak),
        "rms_db": amp_to_db(rms),
        "true_peak_db": true_peak,
        "dc": dc,
        "seam_db": seam,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate PULSE adaptive music v3 assets")
    ap.add_argument("music_dir", nargs="?", default=str(MUSIC), help="music asset directory")
    ap.add_argument("--ceiling-db", type=float, default=-0.3, help="true-peak ceiling in dBFS")
    ap.add_argument("--quiet", action="store_true", help="only print failures and summary")
    args = ap.parse_args()

    music_dir = Path(args.music_dir)
    loop_files = [music_dir / name for name in required_loop_names()]
    stinger_files = [music_dir / name for name in required_stinger_names()]
    missing = [p for p in loop_files + stinger_files if not p.exists()]
    if missing:
        for path in missing:
            print(f"[FAIL] {path}: missing")
        return 1

    results: list[dict[str, object]] = []
    for path in loop_files:
        results.append(analyze(path, True, args.ceiling_db))
    for path in stinger_files:
        results.append(analyze(path, False, args.ceiling_db))

    loop_frames = {int(r["frames"]) for r in results[: len(loop_files)]}
    if len(loop_frames) != 1:
        for r in results[: len(loop_files)]:
            r["ok"] = False
            fails = r["fails"]
            assert isinstance(fails, list)
            fails.append("loop-frame-parity")

    # v4 (C1): the duress layer must sit at least DURESS_BELOW_BED_DB below the bed (RMS), per biome.
    rms_by_name = {Path(r["path"]).name: float(r["rms_db"]) for r in results}
    for biome in BIOMES:
        bed = rms_by_name.get(f"{biome}_bed.wav")
        duress = rms_by_name.get(f"{biome}_duress.wav")
        if bed is not None and duress is not None and duress > bed - DURESS_BELOW_BED_DB:
            for r in results:
                if Path(r["path"]).name == f"{biome}_duress.wav":
                    r["ok"] = False
                    fails = r["fails"]
                    assert isinstance(fails, list)
                    fails.append(f"duress_rms={duress:.2f}>bed-{DURESS_BELOW_BED_DB:.0f}={bed - DURESS_BELOW_BED_DB:.2f}")

    failures = [r for r in results if not r["ok"]]
    worst_tp = max(float(r["true_peak_db"]) for r in results)
    for r in results:
        if args.quiet and r["ok"]:
            continue
        flag = "OK  " if r["ok"] else "FAIL"
        print(
            f"[{flag}] {Path(r['path']).name:<28} "
            f"frames={int(r['frames']):>8} peak={float(r['peak_db']):>7.2f} "
            f"rms={float(r['rms_db']):>7.2f} tp={float(r['true_peak_db']):>7.2f} "
            f"dc={float(r['dc']):>+.4f} seam={float(r['seam_db']):>7.2f} "
            + (" ".join(str(f) for f in r["fails"]) if r["fails"] else "")
        )

    print(
        f"\n[music-validate] loops={len(loop_files)} stingers={len(stinger_files)} "
        f"failures={len(failures)} worst_true_peak={worst_tp:.2f}dBFS "
        f"expected_loop_frames={EXPECTED_LOOP_FRAMES}"
    )
    if failures:
        return 1
    print("[music-validate] PASS: adaptive music bank matches the v4 contract")
    return 0


if __name__ == "__main__":
    sys.exit(main())
