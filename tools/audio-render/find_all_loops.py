#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "numpy>=2",
#     "scipy>=1.13",
# ]
# ///
"""
find-all-loops — list every candidate (intro, period) pair where the audio
makes a clean loop. Unlike find-loop.py, which picks ONE best, this enumerates
all peaks above a configurable threshold so the user can pick the one that
matches the song's perceived structure.

Algorithm:
  1. Mono-sum, downsample to 4 kHz for fast autocorrelation.
  2. FFT-based unbiased autocorrelation, normalised so ACF(0) = 1.
  3. Find ALL local-maximum peaks with ACF >= ac-threshold AND lag in
     [min-period, max-period].
  4. For each peak period P, sweep intro positions and for each intro
     measure:
       - splice_step:    abs(audio[I] - audio[I+P])     (point-step)
       - splice_window:  RMS over W ms comparing audio[I..I+W] vs
                          audio[I+P..I+P+W]             (local window match)
     Pick the intro with smallest splice_window for that P.
  5. Output table sorted by period, including splice quality.

The user picks based on:
  - Period: which matches the song's natural loop length
  - AC value: how clean the long-range similarity is
  - Splice RMS (dBFS): how audible the wrap will be (-60 dB or better is inaudible)

Usage:
  find-all-loops <wav>
      [--intro I]           # fixed intro; only sweep period (default: best per P)
      [--min-period S]      # default 30
      [--max-period S]      # default 600
      [--min-ac F]          # default 0.5
      [--window-ms F]       # splice window for RMS (default 50)
      [--max-intro S]       # ceiling on intro sweep when --intro not given (default 60)
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import find_peaks


def load_wav_mono(path: Path):
    rate, data = wavfile.read(path)
    if data.dtype == np.int16:
        sig = data.astype(np.float32) * (1.0 / 32768.0)
    elif data.dtype == np.int32:
        sig = data.astype(np.float32) * (1.0 / 2147483648.0)
    elif data.dtype == np.float32:
        sig = data
    else:
        sig = data.astype(np.float32)
    if sig.ndim == 2:
        sig_mono = sig.mean(axis=1)
    else:
        sig_mono = sig
    return sig_mono, sig, int(rate)


def downsample(sig: np.ndarray, rate: int, target_rate: int):
    factor = max(1, int(round(rate / target_rate)))
    new_rate = rate // factor
    n = (len(sig) // factor) * factor
    return sig[:n].reshape(-1, factor).mean(axis=1), new_rate


def autocorr_unbiased(sig: np.ndarray) -> np.ndarray:
    n = len(sig)
    x = sig - sig.mean()
    f = np.fft.rfft(x, n=2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    counts = np.arange(n, 0, -1).astype(np.float64)
    ac = ac / counts
    if ac[0] != 0:
        ac = ac / ac[0]
    return ac


def find_all_peaks(ac, rate, min_lag_s, max_lag_s, min_ac):
    lo = int(min_lag_s * rate)
    hi = min(int(max_lag_s * rate), len(ac) - 1)
    region = ac[lo:hi]
    distance = max(1, int(0.25 * rate))
    peaks, _ = find_peaks(region, distance=distance, height=min_ac)
    if len(peaks) == 0:
        return []
    heights = region[peaks]
    return sorted(
        [(int(p + lo), float(h)) for p, h in zip(peaks, heights)],
        key=lambda x: x[0],
    )


def best_intro_for_period(sig_full_rate, src_rate, period_samples_src,
                          window_samples, max_intro_samples, stride):
    """For a given period, sweep intros and return the (intro_samples,
    point_step, window_rms) with smallest window_rms."""
    n = sig_full_rate.shape[0] if sig_full_rate.ndim > 1 else len(sig_full_rate)
    max_t = min(max_intro_samples, n - period_samples_src - window_samples)
    if max_t <= 0:
        return None

    if sig_full_rate.ndim == 2:
        s = sig_full_rate
    else:
        s = sig_full_rate[:, None]

    best = None
    for t in range(0, max_t, stride):
        a = s[t : t + window_samples]
        b = s[t + period_samples_src : t + period_samples_src + window_samples]
        if len(a) != len(b) or len(a) == 0:
            continue
        point = float(np.max(np.abs(s[t] - s[t + period_samples_src])))
        rms = float(np.sqrt(np.mean((a - b) ** 2)))
        if best is None or rms < best[2]:
            best = (t, point, rms)
    return best


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wav")
    p.add_argument("--intro", type=float, default=None,
                   help="Fix intro (seconds). If omitted, the script picks the best "
                        "intro per period in [0, --max-intro].")
    p.add_argument("--min-period", type=float, default=30.0)
    p.add_argument("--max-period", type=float, default=600.0)
    p.add_argument("--min-ac", type=float, default=0.5,
                   help="Only list periods whose autocorrelation peak is >= this.")
    p.add_argument("--window-ms", type=float, default=50.0)
    p.add_argument("--max-intro", type=float, default=60.0,
                   help="Ceiling on intro sweep when --intro not given.")
    p.add_argument("--intro-stride-ms", type=float, default=40.0)
    p.add_argument("--search-rate", type=int, default=4000)
    args = p.parse_args()

    sig_mono, sig_full, rate = load_wav_mono(Path(args.wav))
    duration = len(sig_mono) / rate
    print(f"# loaded {args.wav}  rate={rate}  duration={duration:.3f}s",
          file=sys.stderr)

    search_sig, search_rate = downsample(sig_mono, rate, args.search_rate)
    ac = autocorr_unbiased(search_sig)
    max_lag = min(args.max_period, duration / 2 - 0.1)
    peaks = find_all_peaks(ac, search_rate, args.min_period, max_lag, args.min_ac)
    if not peaks:
        print(f"no AC peaks >= {args.min_ac} in [{args.min_period},{max_lag}]s",
              file=sys.stderr)
        sys.exit(1)
    print(f"# {len(peaks)} candidate periods with AC >= {args.min_ac}",
          file=sys.stderr)

    win_samp = max(1, int(round(args.window_ms * 0.001 * rate)))
    stride_samp = max(1, int(round(args.intro_stride_ms * 0.001 * rate)))

    if args.intro is not None:
        max_intro_samp = int(args.intro * rate) + 1
        intro_lo = int(args.intro * rate)
    else:
        max_intro_samp = int(args.max_intro * rate)
        intro_lo = 0

    print()
    print(f"{'rank':>4}  {'period_s':>10}  {'AC':>7}  "
          f"{'intro_s':>8}  {'point(dBFS)':>11}  {'rms(dBFS)':>10}  "
          f"{'total_s':>9}")
    print("-" * 72)

    rows = []
    for lag_search, ac_val in peaks:
        period_s = lag_search / search_rate
        period_samp = int(round(period_s * rate))

        if args.intro is not None:
            t = int(args.intro * rate)
            a = sig_full[t : t + win_samp]
            b = sig_full[t + period_samp : t + period_samp + win_samp]
            if len(a) != len(b) or len(a) == 0:
                continue
            point = float(np.max(np.abs(sig_full[t] - sig_full[t + period_samp])))
            rms = float(np.sqrt(np.mean((a - b) ** 2)))
            best = (t, point, rms)
        else:
            best = best_intro_for_period(sig_full, rate, period_samp,
                                         win_samp, max_intro_samp, stride_samp)
        if best is None:
            continue
        t, point, rms = best
        rows.append((period_s, ac_val, t / rate, point, rms))

    rows.sort(key=lambda r: r[0])  # sort by period ascending
    for i, (per, ac_val, intro_s, point, rms) in enumerate(rows):
        point_db = 20 * np.log10(point + 1e-12)
        rms_db   = 20 * np.log10(rms   + 1e-12)
        print(f"{i:>4}  {per:>10.4f}  {ac_val:>+6.3f}  "
              f"{intro_s:>8.4f}  {point_db:>+11.1f}  {rms_db:>+10.1f}  "
              f"{intro_s + per:>9.2f}")


if __name__ == "__main__":
    main()
