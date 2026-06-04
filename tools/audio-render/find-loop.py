#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "numpy>=2",
#     "scipy>=1.13",
# ]
# ///
"""
find-loop — locate (intro, loop_period) in a long music render by autocorrelating
the waveform.

Renders should contain at least two full loop periods of the suspected song
cycle so the candidate peak at the true period stands clearly above noise from
shorter sub-periods. Three or four periods is comfortable.

Algorithm:
  1. Mono-sum, downsample for fast autocorrelation.
  2. Unbiased FFT-based autocorrelation; surface the top candidate peaks above
     min-lag.
  3. For the chosen peak P, slide a length-P window and compute
     loop_mse(t) = mean((s[t:t+P] - s[t+P:t+2P])^2).
     The intro ends at the first t whose MSE drops near the floor.
  4. Snap intro_samples to the nearest zero-crossing within ±20 ms (masks any
     residual seam click at the loop boundary).

Outputs a key=value report on stdout. Diagnostics go to stderr.

Example:
  ./find-loop.py audio/raw/music_breeze.wav --max-period 100
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
        peak = float(np.abs(sig).max())
        if peak > 0:
            sig = sig / peak
    if sig.ndim == 2:
        sig = sig.mean(axis=1)
    return sig, int(rate)


def downsample(sig: np.ndarray, rate: int, target_rate: int):
    factor = max(1, int(round(rate / target_rate)))
    new_rate = rate // factor
    n = (len(sig) // factor) * factor
    return sig[:n].reshape(-1, factor).mean(axis=1), new_rate


def autocorr_unbiased(sig: np.ndarray) -> np.ndarray:
    """Unbiased autocorrelation, normalised so ac[0]=1."""
    n = len(sig)
    x = sig - sig.mean()
    f = np.fft.rfft(x, n=2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    counts = np.arange(n, 0, -1).astype(np.float64)
    ac = ac / counts
    if ac[0] != 0:
        ac = ac / ac[0]
    return ac


def find_period_candidates(ac: np.ndarray, rate: int,
                           min_lag_s: float, max_lag_s: float,
                           max_peaks: int = 8):
    lo = int(min_lag_s * rate)
    hi = min(int(max_lag_s * rate), len(ac) - 1)
    if hi <= lo:
        return []
    region = ac[lo:hi]
    # Spacing prevents picking neighbouring samples of the same peak.
    distance = max(1, int(0.25 * rate))
    peaks, _ = find_peaks(region, distance=distance)
    if len(peaks) == 0:
        return []
    heights = region[peaks]
    order = np.argsort(-heights)
    return [(int(peaks[i] + lo), float(heights[i])) for i in order[:max_peaks]]


def find_intro(sig: np.ndarray, period_samples: int,
               max_intro_samples: int, stride: int):
    n = len(sig)
    max_t = min(max_intro_samples, n - 2 * period_samples)
    if max_t <= 0:
        return 0, float("nan"), float("nan")

    ts = np.arange(0, max_t, stride)
    mses = np.empty(len(ts), dtype=np.float64)
    for i, t in enumerate(ts):
        a = sig[t : t + period_samples]
        b = sig[t + period_samples : t + 2 * period_samples]
        mses[i] = float(np.mean((a - b) ** 2))

    floor = float(np.percentile(mses, 5))
    threshold = floor * 1.3 + 1e-12
    mask = mses <= threshold
    intro_idx = int(np.argmax(mask)) if mask.any() else 0
    return int(ts[intro_idx]), float(mses[intro_idx]), floor


def snap_zero_crossing(sig: np.ndarray, t: int, search: int) -> int:
    lo = max(0, t - search)
    hi = min(len(sig) - 1, t + search)
    region = sig[lo : hi + 1]
    sgn = np.sign(region)
    sgn[sgn == 0] = 1.0
    changes = np.where(np.diff(sgn) != 0)[0]
    if len(changes) == 0:
        return t
    target = t - lo
    return lo + int(changes[np.argmin(np.abs(changes - target))])


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wav", help="Input WAV (mono or stereo, int16 expected).")
    p.add_argument("--min-period", type=float, default=5.0,
                   help="Minimum candidate loop period (seconds).")
    p.add_argument("--max-period", type=float, default=240.0,
                   help="Maximum candidate loop period (seconds).")
    p.add_argument("--max-intro", type=float, default=60.0,
                   help="Maximum intro length to consider (seconds).")
    p.add_argument("--search-rate", type=int, default=4000,
                   help="Downsample to this rate for the search (Hz).")
    p.add_argument("--intro-stride", type=float, default=0.02,
                   help="Intro MSE sliding-window stride (seconds).")
    p.add_argument("--peak", type=int, default=0,
                   help="Use the Nth-strongest peak instead of the strongest.")
    p.add_argument("--force-period", type=float, default=None,
                   help="Skip autocorrelation; use this exact period (seconds). "
                        "Use when the structural loop is known from MML analysis "
                        "and you only need an MSE-best intro.")
    p.add_argument("--no-snap", action="store_true",
                   help="Skip zero-crossing snap on the intro boundary.")
    args = p.parse_args()

    sig, rate = load_wav_mono(Path(args.wav))
    duration = len(sig) / rate
    print(f"# loaded {args.wav}  rate={rate}  duration={duration:.3f}s"
          f"  ({len(sig)} samples)", file=sys.stderr)

    search_sig, search_rate = downsample(sig, rate, args.search_rate)
    print(f"# search rate={search_rate} Hz", file=sys.stderr)

    if args.force_period is not None:
        chosen_period_s            = args.force_period
        chosen_lag_samples_search  = int(round(chosen_period_s * search_rate))
        chosen_peak                = float("nan")
        print(f"# forcing period={chosen_period_s:.6f}s "
              f"({chosen_lag_samples_search} search-rate samples)",
              file=sys.stderr)
    else:
        ac = autocorr_unbiased(search_sig)
        max_lag = min(args.max_period, duration / 2 - 0.1)
        candidates = find_period_candidates(ac, search_rate, args.min_period, max_lag)
        if not candidates:
            print(f"# no autocorrelation peak found in [{args.min_period},{max_lag}]s",
                  file=sys.stderr)
            sys.exit(1)

        print("# top period candidates:", file=sys.stderr)
        for i, (lag, val) in enumerate(candidates):
            marker = "  <-- chosen" if i == args.peak else ""
            print(f"#   [{i}]  period={lag / search_rate:8.4f}s  AC={val:+.4f}{marker}",
                  file=sys.stderr)

        chosen_lag_samples_search = candidates[args.peak][0]
        chosen_peak                = candidates[args.peak][1]
        chosen_period_s            = chosen_lag_samples_search / search_rate

    # Intro search at search rate, then map back to source rate.
    stride_search = max(1, int(args.intro_stride * search_rate))
    intro_search_samples, intro_mse, mse_floor = find_intro(
        search_sig, chosen_lag_samples_search,
        int(args.max_intro * search_rate),
        stride_search,
    )
    intro_s = intro_search_samples / search_rate

    intro_samples_src  = int(round(intro_s * rate))
    period_samples_src = int(round(chosen_period_s * rate))

    if not args.no_snap:
        snap_window = int(0.020 * rate)  # ±20 ms
        intro_samples_src = snap_zero_crossing(sig, intro_samples_src, snap_window)
        intro_s = intro_samples_src / rate

    print(f"# intro MSE at chosen t: {intro_mse:.6g} (floor {mse_floor:.6g})",
          file=sys.stderr)

    print(f"intro_seconds={intro_s:.6f}")
    print(f"period_seconds={chosen_period_s:.6f}")
    print(f"intro_samples@{rate}={intro_samples_src}")
    print(f"period_samples@{rate}={period_samples_src}")
    print(f"ac_peak_normalised={chosen_peak:.6f}")
    print(f"intro_mse={intro_mse:.6g}")
    print(f"mse_floor={mse_floor:.6g}")


if __name__ == "__main__":
    main()
