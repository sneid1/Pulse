#!/usr/bin/env python3
"""PULSE audio QA: peak / RMS / true-peak / clipping measurement and spectrograms.

This is the repeatable mix gate. It measures every WAV it is pointed at and FAILS
(non-zero exit) if any file clips, so "zero clipping" is verifiable in CI/build,
not just by ear. It can also emit PNG spectrograms for representative captures.

Clipping here means digital clipping: runs of samples pinned at (or within a hair
of) full scale. A single isolated full-scale sample is reported but not failed; a
sustained run (the audible artifact) fails. Inter-sample / true peak is estimated
by 4x oversampling and reported against a ceiling so masters that round-trip
through a DAC stay clean.

Usage:
  python pulse_audio_validate.py [paths-or-globs ...]
      [--spectrogram OUTDIR] [--ceiling-db -0.3] [--clip-run N] [--json OUT.json]

With no paths it scans assets/audio/*.wav. Examples:
  python pulse_audio_validate.py assets/audio/sfx_fb_*.wav
  python pulse_audio_validate.py build/audio_mix_test.wav --spectrogram build/spectro
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import sys
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile


ROOT = Path(__file__).resolve().parents[2]
FULL_SCALE_16 = 32767.0
# A sample within this of full scale counts as "pinned" (clipped) for 16-bit content.
CLIP_EPS = 2.0 / 32768.0


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def to_float(data: np.ndarray) -> np.ndarray:
    if data.dtype.kind == "f":
        return data.astype(np.float64)
    if data.dtype == np.int16:
        return data.astype(np.float64) / 32768.0
    if data.dtype == np.int32:
        return data.astype(np.float64) / 2147483648.0
    if data.dtype == np.uint8:
        return (data.astype(np.float64) - 128.0) / 128.0
    raise RuntimeError(f"unsupported dtype {data.dtype}")


def max_clip_run(mono: np.ndarray) -> int:
    pinned = np.abs(mono) >= (1.0 - CLIP_EPS)
    if not pinned.any():
        return 0
    best = run = 0
    for p in pinned:
        run = run + 1 if p else 0
        if run > best:
            best = run
    return best


def true_peak_db(mono: np.ndarray, sr: int) -> float:
    if len(mono) < 8:
        return amp_to_db(float(np.max(np.abs(mono))) if len(mono) else 1e-12)
    up = signal.resample_poly(mono, 4, 1)
    return amp_to_db(float(np.max(np.abs(up))))


def analyze(path: Path, ceiling_db: float, clip_run_fail: int) -> dict:
    sr, data = wavfile.read(path)
    x = to_float(data)
    if x.ndim > 1:
        chans = [x[:, c] for c in range(x.shape[1])]
    else:
        chans = [x]
    mono = np.mean(np.column_stack(chans), axis=1) if len(chans) > 1 else chans[0]

    peak = float(np.max(np.abs(x))) if x.size else 0.0
    rms = float(np.sqrt(np.mean(x * x))) if x.size else 0.0
    dc = float(np.mean(x)) if x.size else 0.0
    clip_samples = int(np.count_nonzero(np.abs(x) >= (1.0 - CLIP_EPS)))
    clip_run = max(max_clip_run(c) for c in chans)
    tp = max(true_peak_db(c, sr) for c in chans)

    fails = []
    if clip_run >= clip_run_fail:
        fails.append(f"clip-run={clip_run}")
    if tp > ceiling_db:
        fails.append(f"true-peak={tp:.2f}>{ceiling_db:.2f}")
    return {
        "path": str(path),
        "sr": int(sr),
        "channels": len(chans),
        "frames": int(len(mono)),
        "seconds": round(len(mono) / float(sr), 3) if sr else 0.0,
        "peak_db": round(amp_to_db(peak), 2),
        "rms_db": round(amp_to_db(rms), 2),
        "true_peak_db": round(tp, 2),
        "dc_offset": round(dc, 5),
        "clip_samples": clip_samples,
        "clip_run": clip_run,
        "ok": not fails,
        "fails": fails,
    }


def write_spectrogram(path: Path, out_dir: Path) -> str | None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as exc:  # matplotlib optional
        return f"(spectrogram skipped, matplotlib unavailable: {exc})"
    sr, data = wavfile.read(path)
    x = to_float(data)
    mono = np.mean(x, axis=1) if x.ndim > 1 else x
    if len(mono) < 256:
        return None
    out_dir.mkdir(parents=True, exist_ok=True)
    nperseg = 1024 if len(mono) > 4096 else 256
    f, t, sxx = signal.spectrogram(mono, fs=sr, nperseg=nperseg,
                                   noverlap=nperseg * 3 // 4, scaling="spectrum")
    db = 10.0 * np.log10(sxx + 1e-12)
    fig, ax = plt.subplots(figsize=(9, 4), dpi=110)
    im = ax.pcolormesh(t, f, db, shading="auto", cmap="magma", vmin=db.max() - 90.0, vmax=db.max())
    ax.set_ylim(0, min(20000.0, sr / 2.0))
    ax.set_xlabel("time (s)")
    ax.set_ylabel("frequency (Hz)")
    ax.set_title(path.name)
    fig.colorbar(im, ax=ax, label="dB")
    fig.tight_layout()
    out = out_dir / (path.stem + ".png")
    fig.savefig(out)
    plt.close(fig)
    return str(out)


def expand(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        matches = glob.glob(p)
        if matches:
            out.extend(Path(m) for m in matches if m.lower().endswith(".wav"))
        elif Path(p).is_dir():
            out.extend(sorted(Path(p).glob("*.wav")))
        elif Path(p).exists():
            out.append(Path(p))
    # stable, de-duplicated order
    seen, uniq = set(), []
    for p in sorted(out):
        if p not in seen:
            seen.add(p)
            uniq.append(p)
    return uniq


def main() -> int:
    ap = argparse.ArgumentParser(description="PULSE audio QA: levels, clipping, spectrograms")
    ap.add_argument("paths", nargs="*", help="WAV files / globs / dirs (default: assets/audio/*.wav)")
    ap.add_argument("--spectrogram", metavar="OUTDIR", help="also write PNG spectrograms here")
    ap.add_argument("--ceiling-db", type=float, default=-0.3, help="true-peak ceiling (default -0.3 dBFS)")
    ap.add_argument("--clip-run", type=int, default=3, help="consecutive pinned samples that fail (default 3)")
    ap.add_argument("--json", metavar="OUT", help="write full results as JSON")
    ap.add_argument("--quiet", action="store_true", help="only print failures and the summary")
    args = ap.parse_args()

    paths = expand(args.paths) if args.paths else expand([str(ROOT / "assets" / "audio" / "*.wav")])
    if not paths:
        print("[audio-validate] no WAV files matched")
        return 2

    results = []
    failures = []
    worst_peak = -999.0
    for p in paths:
        try:
            r = analyze(p, args.ceiling_db, args.clip_run)
        except Exception as exc:
            print(f"[audio-validate] ERROR reading {p}: {exc}")
            failures.append(str(p))
            continue
        results.append(r)
        worst_peak = max(worst_peak, r["true_peak_db"])
        if not r["ok"]:
            failures.append(r["path"])
        if not args.quiet or not r["ok"]:
            flag = "OK  " if r["ok"] else "FAIL"
            print(f"[{flag}] {Path(r['path']).name:<34} "
                  f"peak={r['peak_db']:>7.2f} rms={r['rms_db']:>7.2f} "
                  f"tp={r['true_peak_db']:>7.2f} dc={r['dc_offset']:>+.4f} "
                  f"clip={r['clip_samples']}/{r['clip_run']} "
                  + (" ".join(r["fails"]) if r["fails"] else ""))

    spectros = []
    if args.spectrogram:
        out_dir = Path(args.spectrogram)
        for p in paths:
            s = write_spectrogram(p, out_dir)
            if s:
                spectros.append(s)
                print(f"[spectro] {s}")

    if args.json:
        Path(args.json).write_text(json.dumps({"results": results, "spectrograms": spectros}, indent=2),
                                   encoding="utf-8")

    print(f"\n[audio-validate] files={len(results)} failures={len(failures)} "
          f"worst_true_peak={worst_peak:.2f}dBFS ceiling={args.ceiling_db:.2f}dBFS")
    if failures:
        print("[audio-validate] CLIPPING/OVER-CEILING DETECTED:")
        for f in failures:
            print(f"    {f}")
        return 1
    print("[audio-validate] PASS: zero clipping, all within ceiling")
    return 0


if __name__ == "__main__":
    sys.exit(main())
