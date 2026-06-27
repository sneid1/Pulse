#!/usr/bin/env python3
"""Correlate a PULSE music-context trace CSV with the recorded run audio.

PULSE Music v4, feature S3: a time-series playtest audio analyzer. The game
writes a per-frame music-context CSV during a bot run; this tool lines that CSV
up against the recorded stereo 48 kHz WAV of the same run and emits a markdown
report plus a timeline PNG that quantify whether the v4 systems behaved:

  - state-swap quantization to the beat grid (transition cadence + beat error)
  - short-term loudness over the run
  - the duress spectral submerge (mix darkens toward 800 Hz) and the 55 Hz
    heartbeat that fades in as duress rises
  - the per-band tilted SFX duck (presence attenuated hard, low gently)
  - seam-click scan at every state transition

Nothing here asserts pass/fail; it reports the measured numbers so a human can
judge the run. numpy and scipy are hard deps; matplotlib is optional.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile


ROOT = Path(__file__).resolve().parents[2]
BIOMES = ("foundry", "furnace", "reliquary")
MUSIC_STATES = ("silent", "hub", "combat", "reward", "boss", "runover")

CSV_COLUMNS = (
    "time_s",
    "bpm",
    "phase",
    "music_state",
    "biome",
    "intensity",
    "duress",
    "boss_escalation",
    "overpulse",
    "pulse_tier",
)

# Short-term loudness / band-analysis windowing.
WIN_MS = 400.0
HOP_MS = 100.0

# Bands for the duress submerge check (Hz).
HIGH_BAND = (2000.0, 8000.0)
SUB_BAND = (40.0, 70.0)

# Bands for the SFX duck check (Hz).
DUCK_LOW_BAND = (20.0, 120.0)
DUCK_PRESENCE_BAND = (1000.0, 4000.0)

# Transient detection parameters.
ENV_MS = 5.0
TRANSIENT_REFRACTORY_MS = 80.0
TRANSIENT_POST_MS = 200.0
MAX_TRANSIENTS = 200

# Seam-click scan parameters.
SEAM_HALF_MS = 30.0
SEAM_CLICK_DB = -34.0


def amp_to_db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


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


def to_mono(x: np.ndarray) -> np.ndarray:
    if x.ndim == 1:
        return x
    return np.mean(x, axis=1)


def band_db(x: np.ndarray, sr: int, lo: float, hi: float) -> float:
    """RMS energy of a butterworth bandpass of the mono mix, in dB.

    Mirrors band_db in pulse_music_report.py; accepts either a stereo array or
    a mono vector and clamps the upper edge below Nyquist so sosfilt is stable.
    """
    if not len(x):
        return -240.0
    mono = to_mono(x)
    nyq = 0.5 * sr
    hi = min(hi, nyq * 0.999)
    if lo >= hi:
        return -240.0
    sos = signal.butter(2, [lo, hi], "bandpass", fs=sr, output="sos")
    y = signal.sosfilt(sos, mono)
    return amp_to_db(float(np.sqrt(np.mean(y * y))))


def rms_db(segment: np.ndarray) -> float:
    if not len(segment):
        return -240.0
    return amp_to_db(float(np.sqrt(np.mean(segment * segment))))


def pearson(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) < 2 or len(b) < 2:
        return float("nan")
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    sa = float(np.std(a))
    sb = float(np.std(b))
    if sa < 1e-12 or sb < 1e-12:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


def fmt(value: float, digits: int = 4) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "nan"
    return f"{value:.{digits}f}"


# ----------------------------------------------------------------------------
# CSV loading.
# ----------------------------------------------------------------------------

def parse_float(token: str, default: float = 0.0) -> float:
    try:
        return float(token)
    except (TypeError, ValueError):
        return default


def parse_int(token: str, default: int = 0) -> int:
    try:
        return int(float(token))
    except (TypeError, ValueError):
        return default


def load_trace(path: Path) -> list[dict[str, object]]:
    """Read the music-context CSV into a list of row dicts.

    Tolerates blank lines, a missing/short file, and rows with fewer columns
    than the header (those are skipped with a warning). The expected header is
    CSV_COLUMNS; if a header is present we map by name, otherwise we fall back
    to positional order.
    """
    if not path.exists():
        raise FileNotFoundError(f"trace CSV not found: {path}")

    with path.open("r", newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        records = [row for row in reader]

    if not records:
        return []

    header = [c.strip().lower() for c in records[0]]
    has_header = "time_s" in header and "music_state" in header
    if has_header:
        index = {name: header.index(name) for name in CSV_COLUMNS if name in header}
        data_rows = records[1:]
    else:
        index = {name: i for i, name in enumerate(CSV_COLUMNS)}
        data_rows = records

    needed = max(index.get("time_s", 0), index.get("music_state", 3)) + 1

    rows: list[dict[str, object]] = []
    skipped = 0
    for raw in data_rows:
        if not raw or all((cell.strip() == "" for cell in raw)):
            continue
        if len(raw) < needed:
            skipped += 1
            continue

        def cell(name: str) -> str:
            i = index.get(name, -1)
            if i < 0 or i >= len(raw):
                return ""
            return raw[i].strip()

        rows.append(
            {
                "time_s": parse_float(cell("time_s")),
                "bpm": parse_float(cell("bpm"), 140.0),
                "phase": cell("phase"),
                "music_state": cell("music_state").lower(),
                "biome": cell("biome").lower(),
                "intensity": parse_float(cell("intensity")),
                "duress": parse_float(cell("duress")),
                "boss_escalation": parse_float(cell("boss_escalation")),
                "overpulse": parse_int(cell("overpulse")),
                "pulse_tier": parse_int(cell("pulse_tier")),
            }
        )

    if skipped:
        print(f"[music-trace] warning: skipped {skipped} short/malformed CSV row(s)")

    # Keep rows monotonic in time; the trace clock should be non-decreasing.
    rows.sort(key=lambda r: float(r["time_s"]))
    return rows


# ----------------------------------------------------------------------------
# Window-level WAV analysis (short-term loudness + bands).
# ----------------------------------------------------------------------------

def analyze_windows(mono: np.ndarray, sr: int) -> dict[str, np.ndarray]:
    """Compute per-window center times plus loudness and band levels.

    Bandpasses are computed once over the whole mono mix, then windowed RMS is
    taken on each filtered signal so every series shares the same window grid.
    """
    win = max(1, int(round(WIN_MS * 1e-3 * sr)))
    hop = max(1, int(round(HOP_MS * 1e-3 * sr)))
    n = len(mono)

    nyq = 0.5 * sr

    def bandpass(lo: float, hi: float) -> np.ndarray:
        hi = min(hi, nyq * 0.999)
        if lo >= hi or n == 0:
            return np.zeros(n, dtype=np.float64)
        sos = signal.butter(2, [lo, hi], "bandpass", fs=sr, output="sos")
        return signal.sosfilt(sos, mono)

    high = bandpass(*HIGH_BAND)
    sub = bandpass(*SUB_BAND)

    centers: list[float] = []
    loud: list[float] = []
    high_db: list[float] = []
    sub_db: list[float] = []

    start = 0
    while start < n:
        end = min(start + win, n)
        seg = mono[start:end]
        if len(seg):
            centers.append((start + end) * 0.5 / float(sr))
            loud.append(rms_db(seg))
            high_db.append(rms_db(high[start:end]))
            sub_db.append(rms_db(sub[start:end]))
        if end >= n:
            break
        start += hop

    return {
        "center_s": np.asarray(centers, dtype=np.float64),
        "loud_db": np.asarray(loud, dtype=np.float64),
        "high_db": np.asarray(high_db, dtype=np.float64),
        "sub_db": np.asarray(sub_db, dtype=np.float64),
    }


def nearest_sample(times: np.ndarray, values: np.ndarray, query: np.ndarray) -> np.ndarray:
    """Nearest-neighbour resample of (times, values) at the query times."""
    if len(times) == 0 or len(query) == 0:
        return np.zeros(len(query), dtype=np.float64)
    idx = np.searchsorted(times, query)
    idx = np.clip(idx, 0, len(times) - 1)
    out = np.empty(len(query), dtype=np.float64)
    for k, q in enumerate(query):
        i = idx[k]
        if i > 0 and abs(times[i - 1] - q) <= abs(times[i] - q):
            i = i - 1
        out[k] = values[i]
    return out


# ----------------------------------------------------------------------------
# Transition detection + beat alignment.
# ----------------------------------------------------------------------------

def find_transitions(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    """Every consecutive-row music_state change, with beat-alignment error.

    err_beats = |(t / beat_period) - round(t / beat_period)|, where the beat
    grid is anchored at t=0 and beat_period = 60 / bpm at the transition row.
    """
    transitions: list[dict[str, object]] = []
    for i in range(1, len(rows)):
        prev = str(rows[i - 1]["music_state"])
        cur = str(rows[i]["music_state"])
        if cur == prev:
            continue
        t = float(rows[i]["time_s"])
        bpm = float(rows[i]["bpm"])
        beat_period = 60.0 / bpm if bpm > 1e-6 else 0.0
        if beat_period > 0.0:
            ticks = t / beat_period
            err_beats = abs(ticks - round(ticks))
        else:
            err_beats = float("nan")
        transitions.append(
            {
                "time_s": t,
                "from": prev,
                "to": cur,
                "bpm": bpm,
                "err_beats": err_beats,
            }
        )
    return transitions


# ----------------------------------------------------------------------------
# Transient detection.
# ----------------------------------------------------------------------------

def detect_transients(mono: np.ndarray, sr: int) -> np.ndarray:
    """Return onset times (seconds) of large amplitude jumps in the mix.

    A fast envelope (moving-max of |mono| over ~ENV_MS) is compared to a
    rolling baseline; samples whose envelope jumps well above baseline are
    peak-picked with a refractory period, capped at MAX_TRANSIENTS.
    """
    n = len(mono)
    if n < 4:
        return np.asarray([], dtype=np.float64)

    env_win = max(1, int(round(ENV_MS * 1e-3 * sr)))
    if env_win % 2 == 0:
        env_win += 1   # order_filter domain must have an odd number of elements
    a = np.abs(mono)
    # Moving max via a sliding window maximum filter.
    env = signal.order_filter(a, np.ones(env_win), env_win - 1) if env_win > 1 else a

    # Rolling baseline: a long moving average of the envelope.
    base_win = max(env_win * 8, int(round(0.1 * sr)))
    kernel = np.ones(base_win, dtype=np.float64) / float(base_win)
    baseline = np.convolve(env, kernel, mode="same")
    baseline = np.maximum(baseline, 1e-6)

    ratio = env / baseline
    thresh = 3.0
    refractory = max(1, int(round(TRANSIENT_REFRACTORY_MS * 1e-3 * sr)))

    onsets: list[int] = []
    last = -refractory - 1
    i = 1
    while i < n - 1:
        if ratio[i] >= thresh and ratio[i] >= ratio[i - 1] and ratio[i] >= ratio[i + 1]:
            if i - last >= refractory:
                onsets.append(i)
                last = i
                if len(onsets) >= MAX_TRANSIENTS:
                    break
                i += refractory
                continue
        i += 1

    return np.asarray(onsets, dtype=np.float64) / float(sr)


def post_window_band_db(mono: np.ndarray, sr: int, t: float, lo: float, hi: float) -> float:
    start = int(round(t * sr))
    end = int(round((t + TRANSIENT_POST_MS * 1e-3) * sr))
    start = max(0, min(start, len(mono)))
    end = max(start, min(end, len(mono)))
    seg = mono[start:end]
    if len(seg) < 8:
        return -240.0
    return band_db(seg, sr, lo, hi)


# ----------------------------------------------------------------------------
# Seam-click scan.
# ----------------------------------------------------------------------------

def seam_delta_db(mono: np.ndarray, sr: int, t: float) -> float:
    half = int(round(SEAM_HALF_MS * 1e-3 * sr))
    center = int(round(t * sr))
    start = max(0, center - half)
    end = min(len(mono), center + half)
    seg = mono[start:end]
    if len(seg) < 2:
        return -240.0
    return amp_to_db(float(np.max(np.abs(np.diff(seg)))))


# ----------------------------------------------------------------------------
# Markdown report.
# ----------------------------------------------------------------------------

def write_markdown(
    md_path: Path,
    trace_csv: Path,
    audio_wav: Path,
    rows: list[dict[str, object]],
    csv_duration: float,
    wav_duration: float,
    sr: int,
    mono: np.ndarray,
    windows: dict[str, np.ndarray],
    transitions: list[dict[str, object]],
    duress_at_windows: np.ndarray,
    transients: np.ndarray,
    summary: dict[str, object],
) -> None:
    lines: list[str] = []
    lines.append("# PULSE Music v4 trace report")
    lines.append("")
    lines.append(f"- trace CSV: `{trace_csv}`")
    lines.append(f"- audio WAV: `{audio_wav}`")
    lines.append("")

    # 1. Run summary.
    frame_count = len(rows)
    if frame_count >= 2:
        times = np.asarray([float(r["time_s"]) for r in rows])
        mean_dt = float(np.mean(np.diff(times)))
    else:
        mean_dt = 0.0
    biomes_seen = sorted({str(r["biome"]) for r in rows if str(r["biome"])})

    state_frac: dict[str, float] = {}
    if frame_count >= 2:
        times = np.asarray([float(r["time_s"]) for r in rows])
        dts = np.diff(times)
        total = float(np.sum(dts))
        for i in range(frame_count - 1):
            st = str(rows[i]["music_state"]) or "(blank)"
            state_frac[st] = state_frac.get(st, 0.0) + float(dts[i])
        if total > 0:
            for st in list(state_frac.keys()):
                state_frac[st] /= total

    lines.append("## 1. Run summary")
    lines.append("")
    lines.append(f"- CSV duration: {fmt(csv_duration, 3)} s")
    lines.append(f"- WAV duration: {fmt(wav_duration, 3)} s")
    lines.append(f"- frame count: {frame_count}")
    lines.append(f"- mean frame dt: {fmt(mean_dt, 5)} s")
    lines.append(f"- biomes seen: {', '.join(biomes_seen) if biomes_seen else '(none)'}")
    lines.append("")
    lines.append("Fraction of time in each music_state:")
    lines.append("")
    if state_frac:
        lines.append("| music_state | fraction |")
        lines.append("| --- | --- |")
        for st in sorted(state_frac, key=lambda k: -state_frac[k]):
            lines.append(f"| {st} | {fmt(state_frac[st], 3)} |")
    else:
        lines.append("(insufficient rows to compute state fractions)")
    lines.append("")

    # 2. Transition cadence + beat alignment.
    lines.append("## 2. Transition cadence and beat alignment")
    lines.append("")
    n_trans = len(transitions)
    if csv_duration > 0:
        per_min = n_trans / (csv_duration / 60.0)
    else:
        per_min = 0.0
    errs = np.asarray(
        [float(t["err_beats"]) for t in transitions if not math.isnan(float(t["err_beats"]))]
    )
    mean_err = float(np.mean(errs)) if len(errs) else float("nan")
    max_err = float(np.max(errs)) if len(errs) else float("nan")
    lines.append(f"- transitions: {n_trans}")
    lines.append(f"- transitions per minute: {fmt(per_min, 3)}")
    lines.append(f"- mean beat-alignment error: {fmt(mean_err)} beats")
    lines.append(f"- max beat-alignment error: {fmt(max_err)} beats")
    lines.append("")
    lines.append(
        "Interpretation: v4 quantizes state swaps to the beat grid, so a "
        "well-behaved run has small mean err_beats; an unquantized run scatters."
    )
    lines.append("")
    if transitions:
        lines.append("| time_s | from | to | err_beats |")
        lines.append("| --- | --- | --- | --- |")
        for t in transitions:
            lines.append(
                f"| {fmt(float(t['time_s']), 3)} | {t['from'] or '(blank)'} | "
                f"{t['to'] or '(blank)'} | {fmt(float(t['err_beats']))} |"
            )
    else:
        lines.append("(no music_state transitions in this run)")
    lines.append("")

    # 3. Loudness over time.
    lines.append("## 3. Loudness over time")
    lines.append("")
    loud = windows["loud_db"]
    if len(loud):
        lines.append(f"- short-term window: {fmt(WIN_MS, 0)} ms, hop {fmt(HOP_MS, 0)} ms")
        lines.append(f"- min short-term loudness: {fmt(float(np.min(loud)), 2)} dBFS")
        lines.append(f"- mean short-term loudness: {fmt(float(np.mean(loud)), 2)} dBFS")
        lines.append(f"- max short-term loudness: {fmt(float(np.max(loud)), 2)} dBFS")
    else:
        lines.append("(no analysis windows; WAV too short)")
    lines.append("")

    # 4. Duress vs spectral submerge correlation.
    lines.append("## 4. Duress vs spectral submerge correlation")
    lines.append("")
    lines.append(
        "v4 low-pass-sweeps the mix down toward ~800 Hz and fades in a ~55 Hz "
        "heartbeat as duress rises. Expected: high-band correlates NEGATIVE "
        "with duress (darker), sub-band correlates POSITIVE (heartbeat)."
    )
    lines.append("")
    high = windows["high_db"]
    sub = windows["sub_db"]
    duress_var = float(np.var(duress_at_windows)) if len(duress_at_windows) else 0.0
    if len(duress_at_windows) < 2 or duress_var < 1e-6:
        lines.append(
            "Note: the run had little duress variation (duress is ~constant); "
            "skipping correlation numbers."
        )
        summary["high_vs_duress_r"] = float("nan")
        summary["sub_vs_duress_r"] = float("nan")
    else:
        r_high = pearson(duress_at_windows, high)
        r_sub = pearson(duress_at_windows, sub)
        summary["high_vs_duress_r"] = r_high
        summary["sub_vs_duress_r"] = r_sub
        lines.append(f"- Pearson(duress, high_band_db) [2-8 kHz]: {fmt(r_high)}")
        lines.append(f"- Pearson(duress, sub_band_db) [40-70 Hz]: {fmt(r_sub)}")
        lines.append("")
        order = np.argsort(duress_at_windows)
        q = max(1, len(order) // 4)
        lowq = order[:q]
        highq = order[-q:]
        lo_high = float(np.mean(high[lowq]))
        hi_high = float(np.mean(high[highq]))
        lo_sub = float(np.mean(sub[lowq]))
        hi_sub = float(np.mean(sub[highq]))
        lines.append(
            "| metric | lowest-duress quartile | highest-duress quartile | delta (dB) |"
        )
        lines.append("| --- | --- | --- | --- |")
        lines.append(
            f"| high_band_db (2-8 kHz) | {fmt(lo_high, 2)} | {fmt(hi_high, 2)} | "
            f"{fmt(hi_high - lo_high, 2)} |"
        )
        lines.append(
            f"| sub_band_db (40-70 Hz) | {fmt(lo_sub, 2)} | {fmt(hi_sub, 2)} | "
            f"{fmt(hi_sub - lo_sub, 2)} |"
        )
    lines.append("")

    # 5. Per-band duck at SFX transients.
    lines.append("## 5. Per-band duck at SFX transients")
    lines.append("")
    lines.append(
        "v4's tilted duck attenuates the presence band hard and the low band "
        "gently at SFX transients. Post-transient window is "
        f"{fmt(TRANSIENT_POST_MS, 0)} ms; baseline is the global median band level."
    )
    lines.append("")
    n_trans_det = len(transients)
    if n_trans_det == 0:
        lines.append("No transients detected; skipping the duck measurement.")
        summary["presence_duck_db"] = float("nan")
        summary["low_duck_db"] = float("nan")
    else:
        post_low = np.asarray(
            [post_window_band_db(mono, sr, float(t), *DUCK_LOW_BAND) for t in transients]
        )
        post_pres = np.asarray(
            [post_window_band_db(mono, sr, float(t), *DUCK_PRESENCE_BAND) for t in transients]
        )
        base_low = band_db(mono, sr, *DUCK_LOW_BAND)
        base_pres = band_db(mono, sr, *DUCK_PRESENCE_BAND)
        mean_post_low = float(np.mean(post_low))
        mean_post_pres = float(np.mean(post_pres))
        low_drop = base_low - mean_post_low
        pres_drop = base_pres - mean_post_pres
        summary["presence_duck_db"] = pres_drop
        summary["low_duck_db"] = low_drop
        lines.append(f"- transients detected: {n_trans_det}")
        lines.append(f"- baseline low-band (<120 Hz): {fmt(base_low, 2)} dB")
        lines.append(f"- baseline presence-band (1-4 kHz): {fmt(base_pres, 2)} dB")
        lines.append(f"- mean post-transient low-band: {fmt(mean_post_low, 2)} dB")
        lines.append(f"- mean post-transient presence-band: {fmt(mean_post_pres, 2)} dB")
        lines.append(f"- average low-band drop vs baseline: {fmt(low_drop, 2)} dB")
        lines.append(f"- average presence-band drop vs baseline: {fmt(pres_drop, 2)} dB")
    lines.append("")

    # 6. Seam-click scan at transitions.
    lines.append("## 6. Seam-click scan at transitions")
    lines.append("")
    lines.append(
        f"For each transition, the max sample-to-sample delta within +/- "
        f"{fmt(SEAM_HALF_MS, 0)} ms is measured in dB; deltas above "
        f"{fmt(SEAM_CLICK_DB, 0)} dB are flagged as potential clicks."
    )
    lines.append("")
    flagged: list[tuple[float, str, str, float]] = []
    for t in transitions:
        d = seam_delta_db(mono, sr, float(t["time_s"]))
        if d > SEAM_CLICK_DB:
            flagged.append((float(t["time_s"]), str(t["from"]), str(t["to"]), d))
    summary["seam_clicks"] = len(flagged)
    if not transitions:
        lines.append("(no transitions to scan)")
    elif not flagged:
        lines.append(f"no seam clicks > {fmt(SEAM_CLICK_DB, 0)} dB at transitions")
    else:
        lines.append("| time_s | from | to | seam_delta_db |")
        lines.append("| --- | --- | --- | --- |")
        for ts, fr, to, d in flagged:
            lines.append(
                f"| {fmt(ts, 3)} | {fr or '(blank)'} | {to or '(blank)'} | {fmt(d, 2)} |"
            )
    lines.append("")

    # Machine-readable summary line.
    line = (
        f"[music-trace] transitions={summary.get('transitions', 0)} "
        f"mean_beat_err={fmt(float(summary.get('mean_beat_err', float('nan'))))} "
        f"high_vs_duress_r={fmt(float(summary.get('high_vs_duress_r', float('nan'))))} "
        f"presence_duck_db={fmt(float(summary.get('presence_duck_db', float('nan'))), 2)} "
        f"seam_clicks={summary.get('seam_clicks', 0)}"
    )
    lines.append(line)
    lines.append("")

    md_path.write_text("\n".join(lines), encoding="utf-8")
    summary["machine_line"] = line


# ----------------------------------------------------------------------------
# Timeline PNG.
# ----------------------------------------------------------------------------

def render_timeline(
    png_path: Path,
    rows: list[dict[str, object]],
    windows: dict[str, np.ndarray],
    transitions: list[dict[str, object]],
) -> bool:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as exc:
        print(f"[warn] matplotlib unavailable; skipping PNG plots ({exc})")
        return False

    times = np.asarray([float(r["time_s"]) for r in rows])
    intensity = np.asarray([float(r["intensity"]) for r in rows])
    duress = np.asarray([float(r["duress"]) for r in rows])
    boss = np.asarray([float(r["boss_escalation"]) for r in rows])

    fig, axes = plt.subplots(3, 1, figsize=(12, 9), dpi=120, sharex=True)

    ax0 = axes[0]
    ax0.plot(times, intensity, linewidth=1.0, color="#2a6f97", label="intensity")
    ax0.plot(times, duress, linewidth=1.0, color="#bb3e03", label="duress")
    ax0.plot(times, boss, linewidth=1.0, color="#8338ec", label="boss_escalation")
    ax0.set_ylim(-0.05, 1.05)
    ax0.set_ylabel("0..1")
    ax0.set_title("v4 music context")
    ax0.grid(True, alpha=0.18)
    ax0.legend(loc="upper right", fontsize=8)
    for t in transitions:
        tt = float(t["time_s"])
        ax0.axvline(tt, color="#555555", linewidth=0.5, alpha=0.6)
        ax0.text(tt, 1.02, str(t["to"]), rotation=90, fontsize=6, va="bottom", ha="center")

    ax1 = axes[1]
    if len(windows["center_s"]):
        ax1.plot(windows["center_s"], windows["loud_db"], linewidth=1.0, color="#005f73")
    ax1.set_ylabel("loudness dBFS")
    ax1.set_title("short-term loudness")
    ax1.grid(True, alpha=0.18)

    ax2 = axes[2]
    if len(windows["center_s"]):
        ax2.plot(
            windows["center_s"], windows["high_db"], linewidth=1.0,
            color="#ee9b00", label="high 2-8 kHz",
        )
        ax2.plot(
            windows["center_s"], windows["sub_db"], linewidth=1.0,
            color="#9b2226", label="sub 40-70 Hz",
        )
    ax2.set_ylabel("band dB")
    ax2.set_xlabel("seconds")
    ax2.set_title("duress submerge: high-band darken + sub-band heartbeat")
    ax2.grid(True, alpha=0.18)
    ax2.legend(loc="upper right", fontsize=8)

    fig.tight_layout()
    fig.savefig(png_path)
    plt.close(fig)
    return True


# ----------------------------------------------------------------------------
# Main.
# ----------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Correlate a PULSE music-context trace CSV with the run audio (v4 S3)"
    )
    ap.add_argument("trace_csv", help="per-frame music-context CSV written by the game")
    ap.add_argument("audio_wav", help="recorded stereo 16-bit 48 kHz WAV of the same run")
    ap.add_argument("output_dir", help="directory to write the report and timeline into")
    ap.add_argument("--no-png", action="store_true", help="skip the timeline PNG")
    args = ap.parse_args()

    trace_csv = Path(args.trace_csv)
    audio_wav = Path(args.audio_wav)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_trace(trace_csv)
    if not rows:
        print("[music-trace] FAIL: no usable rows")
        return 1

    sr, x = read_wav(audio_wav)
    mono = to_mono(x)
    wav_duration = len(mono) / float(sr) if sr else 0.0
    csv_duration = float(rows[-1]["time_s"]) if rows else 0.0

    if csv_duration > 0 and wav_duration > 0:
        ratio = abs(wav_duration - csv_duration) / max(csv_duration, wav_duration)
        if ratio > 0.25:
            print(
                f"[music-trace] warning: CSV duration {csv_duration:.2f}s and WAV "
                f"duration {wav_duration:.2f}s differ by more than 25%; proceeding on "
                "each own clock"
            )

    windows = analyze_windows(mono, sr)
    transitions = find_transitions(rows)
    transients = detect_transients(mono, sr)

    # Align the CSV duress to the window center times (nearest sample).
    trace_times = np.asarray([float(r["time_s"]) for r in rows])
    trace_duress = np.asarray([float(r["duress"]) for r in rows])
    duress_at_windows = nearest_sample(trace_times, trace_duress, windows["center_s"])

    errs = np.asarray(
        [float(t["err_beats"]) for t in transitions if not math.isnan(float(t["err_beats"]))]
    )
    summary: dict[str, object] = {
        "transitions": len(transitions),
        "mean_beat_err": float(np.mean(errs)) if len(errs) else float("nan"),
    }

    md_path = out_dir / "music_v4_trace.md"
    write_markdown(
        md_path,
        trace_csv,
        audio_wav,
        rows,
        csv_duration,
        wav_duration,
        sr,
        mono,
        windows,
        transitions,
        duress_at_windows,
        transients,
        summary,
    )

    png_written = False
    if not args.no_png:
        png_path = out_dir / "music_v4_trace_timeline.png"
        png_written = render_timeline(png_path, rows, windows, transitions)

    # Concise human summary to stdout, mirroring report.py.
    loud = windows["loud_db"]
    mean_loud = float(np.mean(loud)) if len(loud) else float("nan")
    print(f"[music-trace] wrote {md_path}")
    if png_written:
        print(f"[music-trace] wrote {out_dir / 'music_v4_trace_timeline.png'}")
    print(
        f"[music-trace] frames={len(rows)} csv_dur={csv_duration:.2f}s "
        f"wav_dur={wav_duration:.2f}s transitions={summary['transitions']}"
    )
    print(
        f"[music-trace] mean_beat_err={fmt(float(summary['mean_beat_err']))} "
        f"mean_loud={fmt(mean_loud, 2)}dBFS transients={len(transients)}"
    )
    print(
        f"[music-trace] high_vs_duress_r={fmt(float(summary.get('high_vs_duress_r', float('nan'))))} "
        f"sub_vs_duress_r={fmt(float(summary.get('sub_vs_duress_r', float('nan'))))} "
        f"presence_duck_db={fmt(float(summary.get('presence_duck_db', float('nan'))), 2)} "
        f"seam_clicks={summary.get('seam_clicks', 0)}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"[music-trace] FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
