#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "numpy>=2",
#     "scipy>=1.13",
#     "librosa>=0.10",
# ]
# ///
"""
find-song-structure — detect song-level periodicity using chromagram
autocorrelation rather than raw waveform autocorrelation.

The previous find-all-loops tool autocorrelates the raw audio. That picks
up short-cycle waveform similarity (chord pads, drums) but misses the
long-cycle MELODIC/HARMONIC structure of the song. A song's "meat" lives
in the chord progression and melodic phrasing, both of which are captured
by the chromagram (12-bin pitch class energy).

Algorithm:
  1. Load WAV.
  2. Compute chroma_cqt (constant-Q chromagram) at hop_length 4096 (~93ms
     at 44.1 kHz).
  3. Compute frame-by-frame autocorrelation of the chromagram (treating
     each frame as a 12-d vector). Sum across pitch classes to get a
     "structural similarity" curve as a function of lag.
  4. Find peaks in this curve above a threshold — these are the lags at
     which the song's harmonic structure repeats.
  5. Report the peaks in seconds.

Output is a table of (period_s, structural_AC) sorted by AC.

Usage:
  find-song-structure <wav> [--min-period S] [--max-period S]
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import find_peaks
import librosa


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
        sig = sig.mean(axis=1)
    return sig, int(rate)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wav")
    p.add_argument("--min-period", type=float, default=30.0)
    p.add_argument("--max-period", type=float, default=1800.0)
    p.add_argument("--hop-ms", type=float, default=93.0)
    p.add_argument("--min-ac", type=float, default=0.5)
    p.add_argument("--top", type=int, default=40)
    args = p.parse_args()

    print(f"# loading {args.wav}...", file=sys.stderr)
    sig, rate = load_wav_mono(Path(args.wav))
    duration = len(sig) / rate
    print(f"# duration {duration:.1f}s at {rate} Hz", file=sys.stderr)

    hop = int(rate * args.hop_ms / 1000.0)
    print(f"# computing chromagram with hop_length={hop}...", file=sys.stderr)
    chroma = librosa.feature.chroma_cqt(y=sig, sr=rate, hop_length=hop)
    # chroma shape: (12, n_frames)
    frame_rate = rate / hop
    n_frames = chroma.shape[1]
    print(f"# chromagram: 12 x {n_frames} frames @ {frame_rate:.2f} Hz", file=sys.stderr)

    # Normalise per frame so AC reflects shape, not loudness.
    norms = np.linalg.norm(chroma, axis=0) + 1e-9
    chroma_n = chroma / norms

    # Compute structural autocorrelation: for each lag L, average cosine
    # similarity between frame f and frame f+L over all valid f.
    min_lag = int(args.min_period * frame_rate)
    max_lag = min(int(args.max_period * frame_rate), n_frames - 1)
    print(f"# searching lags {min_lag}..{max_lag} frames "
          f"({args.min_period:.1f}..{args.max_period:.1f}s)", file=sys.stderr)

    lags = np.arange(min_lag, max_lag + 1)
    ac = np.empty(len(lags), dtype=np.float64)
    for i, L in enumerate(lags):
        a = chroma_n[:, :n_frames - L]
        b = chroma_n[:, L:]
        # Cosine similarity per frame pair = dot product (since unit-normalised)
        sim = np.sum(a * b, axis=0)
        ac[i] = float(np.mean(sim))

    # Find peaks
    distance = max(1, int(0.5 * frame_rate))  # at least 0.5s between peaks
    peaks, _ = find_peaks(ac, distance=distance, height=args.min_ac)
    if len(peaks) == 0:
        print(f"no peaks above {args.min_ac}", file=sys.stderr)
        return

    peak_lags = lags[peaks]
    peak_acs = ac[peaks]
    order = np.argsort(-peak_acs)

    print(f"\n# {len(peaks)} peaks. Top {min(args.top, len(peaks))} by structural AC:")
    print(f"{'rank':>4}  {'period_s':>10}  {'struct_AC':>10}")
    print("-" * 32)
    for i, idx in enumerate(order[:args.top]):
        period_s = peak_lags[idx] / frame_rate
        print(f"{i:>4}  {period_s:>10.4f}  {peak_acs[idx]:>+10.4f}")


if __name__ == "__main__":
    main()
