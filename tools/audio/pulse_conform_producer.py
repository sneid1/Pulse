#!/usr/bin/env python3
"""Conform arbitrary source audio (library or AI-generated) into a PULSE bank.

This is the missing "ingest" step. The authored gun banks sound good because raw
Sonniss recordings are run through layering + EQ + peak-targeting before shipping;
dropped-in files sound "wrong" only because they skip that conform. This tool runs
the same conform on ANY source so it lands at the right level, tone, and format
next to the existing banks.

It produces the runtime contract the engine loads (see src/Engine/Audio.cpp
loadVariantBank): mono 48 kHz / 16-bit WAVs named
  <stem>.wav, <stem>_1.wav, <stem>_2.wav, ...
where <stem> is e.g. sfx_fb_jump, sfx_enemy_rusher_death, sfx_fire_pistol.

Each input file becomes one round-robin variant (first input -> <stem>.wav). With
--variants N greater than the input count, extra variants are synthesized by
deterministic micro-variation (slight pitch/level/trim) so a repeated cue does not
read as one identical sample.

Per source the conform is: mono -> 96 kHz work rate -> DC/rumble highpass ->
optional lowpass/tilt -> silence trim -> de-click fades -> optional length cap ->
pre-gain -> peak-target -> 48 kHz / 16-bit dithered WAV. Output is then handed to
pulse_audio_validate.py (the zero-clip gate) unless --no-validate.

Examples:
  python pulse_conform_producer.py --stem sfx_fb_jump --inputs jump.wav
  python pulse_conform_producer.py --stem sfx_enemy_rusher_death \\
      --inputs growl_a.wav growl_b.wav growl_c.wav --max-len 0.9 --peak-db -1.5
  python pulse_conform_producer.py --stem sfx_fb_ui_move --inputs tick.wav \\
      --variants 4 --max-len 0.18
"""

from __future__ import annotations

import argparse
import math
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "assets" / "audio"
VALIDATE = Path(__file__).resolve().parent / "pulse_audio_validate.py"

WORK_SR = 96_000
OUT_SR = 48_000


def db_to_amp(db: float) -> float:
    return 10.0 ** (db / 20.0)


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def normalize_dtype(data: np.ndarray) -> np.ndarray:
    if data.dtype.kind == "f":
        return data.astype(np.float64)
    if data.dtype == np.uint8:                       # WAV 8-bit is unsigned
        return (data.astype(np.float64) - 128.0) / 128.0
    if data.dtype == np.int16:
        return data.astype(np.float64) / 32768.0
    if data.dtype == np.int32:
        return data.astype(np.float64) / 2147483648.0
    raise RuntimeError(f"Unsupported WAV sample dtype: {data.dtype}")


def load_source(path: Path) -> np.ndarray:
    """Load any source file to mono float64 at WORK_SR. WAV via scipy; other
    formats (mp3/ogg/flac) via soundfile if it is installed."""
    if not path.exists():
        raise FileNotFoundError(path)
    if path.suffix.lower() == ".wav":
        from scipy.io import wavfile
        sr, data = wavfile.read(path)
        x = normalize_dtype(data)
    else:
        try:
            import soundfile as sf
        except ImportError as exc:
            raise RuntimeError(
                f"{path.name}: non-WAV input needs the 'soundfile' package "
                f"(pip install soundfile), or convert the source to WAV first."
            ) from exc
        x, sr = sf.read(str(path), always_2d=False)
        x = np.asarray(x, dtype=np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if sr != WORK_SR:
        x = signal.resample_poly(x, WORK_SR, sr)
    return np.asarray(x, dtype=np.float64)


def highpass(x: np.ndarray, hz: float, order: int = 2) -> np.ndarray:
    if hz <= 0.0:
        return x
    return signal.sosfilt(signal.butter(order, hz, "highpass", fs=WORK_SR, output="sos"), x)


def lowpass(x: np.ndarray, hz: float, order: int = 2) -> np.ndarray:
    if hz <= 0.0 or hz >= WORK_SR * 0.5:
        return x
    return signal.sosfilt(signal.butter(order, hz, "lowpass", fs=WORK_SR, output="sos"), x)


def peaking(x: np.ndarray, hz: float, q: float, gain_db: float) -> np.ndarray:
    if abs(gain_db) < 1e-3:
        return x
    a = db_to_amp(gain_db)
    w0 = 2.0 * math.pi * hz / WORK_SR
    alpha = math.sin(w0) / (2.0 * q)
    cos_w0 = math.cos(w0)
    b0 = 1.0 + alpha * a
    b1 = -2.0 * cos_w0
    b2 = 1.0 - alpha * a
    a0 = 1.0 + alpha / a
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha / a
    return signal.lfilter([b0 / a0, b1 / a0, b2 / a0], [1.0, a1 / a0, a2 / a0], x)


def trim_silence(x: np.ndarray, trim_db: float, pad_start_ms: float, pad_end_ms: float) -> np.ndarray:
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    if peak <= 0.0:
        return x
    thr = peak * db_to_amp(trim_db)
    loud = np.where(np.abs(x) > thr)[0]
    if loud.size == 0:
        return x
    pad_s = int(pad_start_ms * 1e-3 * WORK_SR)
    pad_e = int(pad_end_ms * 1e-3 * WORK_SR)
    start = max(0, int(loud[0]) - pad_s)
    end = min(len(x), int(loud[-1]) + pad_e)
    return x[start:end]


def apply_fades(x: np.ndarray, fade_in_ms: float, fade_out_ms: float) -> np.ndarray:
    n = len(x)
    fi = min(n, int(fade_in_ms * 1e-3 * WORK_SR))
    fo = min(n, int(fade_out_ms * 1e-3 * WORK_SR))
    y = x.copy()
    if fi > 1:
        y[:fi] *= np.linspace(0.0, 1.0, fi)
    if fo > 1:
        y[-fo:] *= np.linspace(1.0, 0.0, fo)
    return y


def conform_one(x: np.ndarray, args, variant: int) -> np.ndarray:
    y = x
    # Deterministic micro-variation for synthesized extra variants (variant > 0 only
    # matters when we ran out of distinct inputs): small pitch + level + offset.
    if variant > 0 and args.synth_variant:
        ratio = 1.0 + ((variant % 2) * 2 - 1) * 0.012 * ((variant + 1) // 2)  # +/-1.2% steps
        ratio = float(np.clip(ratio, 0.94, 1.06))
        y = signal.resample_poly(y, int(round(1000 * ratio)), 1000)
    y = highpass(y, args.hp)
    y = lowpass(y, args.lp)
    y = peaking(y, 9000.0, 0.5, args.tilt_db)
    y = trim_silence(y, args.trim_db, args.start_pad_ms, args.end_pad_ms)
    if args.max_len > 0.0:
        cap = int(args.max_len * WORK_SR)
        if len(y) > cap:
            y = y[:cap]
    y = apply_fades(y, 1.0, args.fade_ms)
    y = y * db_to_amp(args.gain_db)
    if variant > 0 and args.synth_variant:
        y = y * db_to_amp(((variant % 3) - 1) * 0.6)  # +/-0.6 dB level walk
    # Peak-target last so every variant sits at a known ceiling.
    peak = float(np.max(np.abs(y))) if y.size else 0.0
    if peak > 0.0:
        y = y * (db_to_amp(args.peak_db) / peak)
    return y


def write_wav(path: Path, x96: np.ndarray, seed: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    x48 = signal.resample_poly(x96, OUT_SR, WORK_SR)
    rng = np.random.default_rng(seed)
    dither = (rng.random(len(x48)) - rng.random(len(x48))) / 65536.0   # TPDF
    q = np.clip(x48 + dither, -0.999969, 0.999969)
    pcm = (q * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(OUT_SR)
        w.writeframes(pcm.tobytes())


def remove_old(outdir: Path, stem: str) -> None:
    base = outdir / f"{stem}.wav"
    if base.exists():
        base.unlink()
    for old in outdir.glob(f"{stem}_*.wav"):
        if old.stem[len(stem) + 1:].isdigit():
            old.unlink()


def variant_path(outdir: Path, stem: str, index: int) -> Path:
    return outdir / (f"{stem}.wav" if index == 0 else f"{stem}_{index}.wav")


def main() -> int:
    ap = argparse.ArgumentParser(description="Conform source audio into a PULSE bank.")
    ap.add_argument("--stem", required=True, help="bank stem, e.g. sfx_fb_jump")
    ap.add_argument("--inputs", required=True, nargs="+", help="source files (wav, or any soundfile format)")
    ap.add_argument("--outdir", default=str(AUDIO))
    ap.add_argument("--variants", type=int, default=0, help="total variants (default = number of inputs)")
    ap.add_argument("--peak-db", type=float, default=-1.0, help="target peak dBFS (default -1.0)")
    ap.add_argument("--trim-db", type=float, default=-50.0, help="silence trim threshold below peak (default -50)")
    ap.add_argument("--max-len", type=float, default=0.0, help="cap length in seconds (0 = no cap)")
    ap.add_argument("--fade-ms", type=float, default=6.0, help="out-fade ms to de-click (default 6)")
    ap.add_argument("--hp", type=float, default=35.0, help="highpass Hz for rumble/DC (default 35; 0=off)")
    ap.add_argument("--lp", type=float, default=0.0, help="lowpass Hz (default off)")
    ap.add_argument("--tilt-db", type=float, default=0.0, help="brightness shelf at ~9 kHz dB (default 0)")
    ap.add_argument("--gain-db", type=float, default=0.0, help="pre-gain dB before peak-target")
    ap.add_argument("--start-pad-ms", dest="start_pad_ms", type=float, default=5.0)
    ap.add_argument("--end-pad-ms", dest="end_pad_ms", type=float, default=40.0)
    ap.add_argument("--no-synth-variant", dest="synth_variant", action="store_false",
                    help="do not micro-vary synthesized extra variants")
    ap.add_argument("--no-validate", dest="validate", action="store_false")
    args = ap.parse_args()

    outdir = Path(args.outdir)
    inputs = [Path(p) for p in args.inputs]
    count = max(args.variants, len(inputs))

    sources = [load_source(p) for p in inputs]
    remove_old(outdir, args.stem)

    written: list[Path] = []
    for i in range(count):
        src = sources[i % len(sources)]
        y = conform_one(src, args, variant=i)
        path = variant_path(outdir, args.stem, i)
        write_wav(path, y, seed=0xC0FFEE ^ (i * 2654435761 & 0xFFFFFFFF))
        peak_db = amp_to_db(float(np.max(np.abs(y))) if y.size else 0.0)
        rms_db = amp_to_db(float(np.sqrt(np.mean(y * y))) if y.size else 0.0)
        dur = len(signal.resample_poly(y, OUT_SR, WORK_SR)) / OUT_SR
        print(f"  {path.name}: dur={dur:.3f}s peak={peak_db:.2f}dBFS rms={rms_db:.2f}dBFS "
              f"src={inputs[i % len(inputs)].name}")
        written.append(path)

    print(f"[conform] wrote {len(written)} file(s) for bank '{args.stem}'")

    if args.validate and written:
        try:
            r = subprocess.run([sys.executable, str(VALIDATE)] + [str(p) for p in written],
                               check=False)
            if r.returncode != 0:
                print("[conform] WARNING: pulse_audio_validate reported a problem", file=sys.stderr)
                return r.returncode
        except OSError as exc:
            print(f"[conform] could not run validator: {exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
