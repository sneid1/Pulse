#!/usr/bin/env python3
"""Render and analyze the PULSE adaptive music v4 matrix (incl. duress + boss-escalation sweeps)."""

from __future__ import annotations

import argparse
import csv
import math
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile


ROOT = Path(__file__).resolve().parents[2]
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
BANDS = {
    "sub_db": (20.0, 90.0),
    "low_db": (90.0, 250.0),
    "body_db": (250.0, 900.0),
    "presence_db": (900.0, 4000.0),
    "air_db": (4000.0, 14000.0),
}


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def find_exe() -> Path:
    candidates = [
        ROOT / "build" / "pulse.exe",
        ROOT / "build" / "Debug" / "pulse.exe",
        ROOT / "build" / "Release" / "pulse.exe",
        ROOT / "out" / "build" / "x64-debug" / "pulse.exe",
    ]
    for exe in candidates:
        if exe.exists():
            return exe
    raise FileNotFoundError("pulse.exe was not found; run Build.bat first")


def run_render(cmd: list[str]) -> None:
    print("[render]", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.stdout:
        print(proc.stdout.strip())
    if proc.returncode != 0:
        raise RuntimeError(f"render failed with exit code {proc.returncode}")


def read_wav(path: Path) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as f:
        channels = f.getnchannels()
    sr, data = wavfile.read(path)
    if data.dtype == np.int16:
        x = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        x = data.astype(np.float64) / 2147483648.0
    elif data.dtype.kind == "f":
        x = data.astype(np.float64)
    else:
        raise RuntimeError(f"unsupported WAV dtype {data.dtype}: {path}")
    if x.ndim == 1:
        x = np.repeat(x[:, None], 2, axis=1)
    if x.shape[1] != channels:
        raise RuntimeError(f"channel metadata mismatch for {path}")
    return int(sr), x


def true_peak_db(x: np.ndarray) -> float:
    peak = 0.0
    for c in range(x.shape[1]):
        chan = x[:, c]
        if len(chan) >= 8:
            up = signal.resample_poly(chan, 4, 1)
            peak = max(peak, float(np.max(np.abs(up))))
        elif len(chan):
            peak = max(peak, float(np.max(np.abs(chan))))
    return amp_to_db(peak)


def seam_db(x: np.ndarray) -> float:
    if len(x) < 2:
        return -240.0
    return amp_to_db(float(np.max(np.abs(x[0] - x[-1]))))


def band_db(x: np.ndarray, sr: int, lo: float, hi: float) -> float:
    if not len(x):
        return -240.0
    mono = np.mean(x, axis=1)
    sos = signal.butter(2, [lo, hi], "bandpass", fs=sr, output="sos")
    y = signal.sosfilt(sos, mono)
    return amp_to_db(float(np.sqrt(np.mean(y * y))))


def analyze(path: Path, loop: bool) -> dict[str, object]:
    sr, x = read_wav(path)
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    rms = float(np.sqrt(np.mean(x * x))) if x.size else 0.0
    row: dict[str, object] = {
        "file": path.name,
        "seconds": x.shape[0] / float(sr) if sr else 0.0,
        "frames": x.shape[0],
        "sample_rate": sr,
        "peak_db": amp_to_db(peak),
        "rms_db": amp_to_db(rms),
        "true_peak_db": true_peak_db(x),
        "dc": float(np.mean(x)) if x.size else 0.0,
        "seam_db": seam_db(x) if loop else -240.0,
    }
    for name, (lo, hi) in BANDS.items():
        row[name] = band_db(x, sr, lo, hi)
    return row


def render_plots(path: Path, out_dir: Path) -> bool:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as exc:
        print(f"[warn] matplotlib unavailable; skipping PNG plots ({exc})")
        return False

    sr, x = read_wav(path)
    mono = np.mean(x, axis=1)
    t = np.arange(len(mono)) / float(sr)

    fig, ax = plt.subplots(figsize=(12, 3.2), dpi=120)
    ax.plot(t, mono, linewidth=0.55, color="#2a6f97")
    ax.set_title(path.stem)
    ax.set_xlabel("seconds")
    ax.set_ylabel("amplitude")
    ax.set_ylim(-1.05, 1.05)
    ax.grid(True, alpha=0.18)
    fig.tight_layout()
    fig.savefig(out_dir / f"{path.stem}_waveform.png")
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(12, 4.0), dpi=120)
    f, tt, spec = signal.spectrogram(mono, fs=sr, nperseg=2048, noverlap=1536)
    spec_db = 10.0 * np.log10(np.maximum(spec, 1e-12))
    im = ax.pcolormesh(tt, f, spec_db, shading="auto", cmap="magma", vmin=-110, vmax=-25)
    ax.set_ylim(20.0, 16000.0)
    ax.set_yscale("log")
    ax.set_title(path.stem)
    ax.set_xlabel("seconds")
    ax.set_ylabel("Hz")
    fig.colorbar(im, ax=ax, label="dB")
    fig.tight_layout()
    fig.savefig(out_dir / f"{path.stem}_spectrogram.png")
    plt.close(fig)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="Render and analyze the PULSE music v3 matrix")
    ap.add_argument("output_dir", nargs="?", default=str(ROOT / "build" / "music_v4_report"))
    ap.add_argument("--seconds", type=float, default=16.0, help="seconds for loop state renders")
    ap.add_argument("--stinger-seconds", type=float, default=4.0, help="seconds for stinger renders")
    ap.add_argument("--no-png", action="store_true", help="skip waveform and spectrogram PNGs")
    args = ap.parse_args()

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = find_exe()

    renders: list[tuple[Path, bool]] = []
    # (label, state, intensity, overpulse, duress, boss_escalation). A non-None duress or
    # boss_escalation forces the v4 render path (so duress=0.0 still exercises v4); the other rows
    # stay on the v3 matrix. The duress sweep and boss-escalation sweep are v4 (S3 / M1 / M3).
    states = [
        ("combat_low", "combat", 0.22, False, None, None),
        ("combat_high", "combat", 0.92, False, None, None),
        ("combat_overpulse", "combat", 0.98, True, None, None),
        ("combat_duress_low", "combat", 0.55, False, 0.0, None),
        ("combat_duress_mid", "combat", 0.55, False, 0.5, None),
        ("combat_duress_high", "combat", 0.55, False, 0.9, None),
        ("reward", "reward", 0.35, False, None, None),
        ("boss", "boss", 1.0, False, None, None),
        ("boss_esc_low", "boss", 1.0, False, None, 0.0),
        ("boss_esc_mid", "boss", 1.0, False, None, 0.5),
        ("boss_esc_high", "boss", 1.0, False, None, 0.95),
        ("runover", "runover", 0.0, False, None, None),
    ]
    for biome in BIOMES:
        for label, state, intensity, overpulse, duress, boss_esc in states:
            wav = out_dir / f"{biome}_{label}.wav"
            cmd = [
                str(exe),
                "--render-music",
                str(wav),
                str(args.seconds),
                "--music-state",
                state,
                "--music-biome",
                biome,
                "--music-intensity",
                f"{intensity:.3f}",
            ]
            if overpulse:
                cmd.append("--music-overpulse")
            if duress is not None:
                cmd += ["--music-duress", f"{duress:.3f}"]
            if boss_esc is not None:
                cmd += ["--music-boss-escalation", f"{boss_esc:.3f}"]
            run_render(cmd)
            renders.append((wav, state != "runover"))

    for stinger in STINGERS:
        wav = out_dir / f"stinger_{stinger}.wav"
        run_render([str(exe), "--render-music-stinger", stinger, str(wav), str(args.stinger_seconds)])
        renders.append((wav, False))

    rows = [analyze(path, loop) for path, loop in renders]
    csv_path = out_dir / "music_report.csv"
    fieldnames = list(rows[0].keys()) if rows else []
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    if not args.no_png:
        plotted = False
        for path, _ in renders:
            plotted = render_plots(path, out_dir) or plotted
            if not plotted:
                break

    worst_peak = max(float(row["true_peak_db"]) for row in rows) if rows else -240.0
    print(f"[music-report] wrote {csv_path}")
    print(f"[music-report] renders={len(rows)} worst_true_peak={worst_peak:.2f}dBFS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"[music-report] FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
