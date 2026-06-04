#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "numpy>=2",
#     "scipy>=1.13",
# ]
# ///
"""
measure-splice — score the splice quality of a wav64-style loop.

Given a rendered WAV and (intro, period), wav64 will play samples
[0, intro+period) once and then wrap to sample `intro` forever. The
audible click at the wrap is determined by the step between the last
sample played (`intro + period - 1`) and the next sample played
(`intro`), plus the local waveform mismatch in a small window around
the splice.

This tool sweeps intro over a range at a fixed period (from MML
structural analysis) and reports:
  * point step:     abs(audio[intro] - audio[intro+period])   per channel
  * window RMS:     RMS over W samples comparing
                      audio[intro            : intro + W]
                    against
                      audio[intro + period   : intro + period + W]

Both are reported as fractions of full-scale (16-bit signed),
so 0.001 = -60 dBFS, 0.0001 = -80 dBFS. Below ~-60 dBFS a step is
inaudible against typical music.

Usage:
  ./measure-splice.py audio/raw/music_breeze.wav --period 30.72
                      [--intro-start 0] [--intro-end 60]
                      [--intro-step 0.04] [--window-ms 5]

The intro values it sweeps are rounded to the nearest WAV sample
(at 44100 Hz: every sample ~= 22.7 us). Best splice is reported at
the top, then the rest sorted by quality.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile


def load_wav(path: Path):
    rate, data = wavfile.read(path)
    if data.dtype != np.int16:
        raise SystemExit(f"expected int16 WAV, got {data.dtype}")
    if data.ndim == 1:
        data = data[:, None]
    return data.astype(np.float64) / 32768.0, int(rate)


def score(sig, intro_samp, period_samp, win_samp):
    """Return (point_step, window_rms) at this splice."""
    a_idx = intro_samp
    b_idx = intro_samp + period_samp
    n = sig.shape[0]
    if b_idx + win_samp >= n:
        return None
    point = float(np.max(np.abs(sig[a_idx] - sig[b_idx])))
    a = sig[a_idx : a_idx + win_samp]
    b = sig[b_idx : b_idx + win_samp]
    rms = float(np.sqrt(np.mean((a - b) ** 2)))
    return point, rms


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wav")
    p.add_argument("--period", type=float, required=True,
                   help="Loop period in seconds.")
    p.add_argument("--intro-start", type=float, default=0.0)
    p.add_argument("--intro-end",   type=float, default=60.0)
    p.add_argument("--intro-step",  type=float, default=0.04,
                   help="Sweep granularity in seconds (default: one 25 Hz "
                        "audio frame).")
    p.add_argument("--window-ms",   type=float, default=5.0,
                   help="RMS comparison window in milliseconds.")
    p.add_argument("--top", type=int, default=10,
                   help="Show this many best candidates.")
    args = p.parse_args()

    sig, rate = load_wav(Path(args.wav))
    n = sig.shape[0]
    period_samp = int(round(args.period * rate))
    win_samp    = max(1, int(round(args.window_ms * 0.001 * rate)))

    print(f"# wav={args.wav}  rate={rate}  duration={n/rate:.3f}s"
          f"  channels={sig.shape[1]}", file=sys.stderr)
    print(f"# period={args.period}s ({period_samp} samp)  "
          f"window={args.window_ms}ms ({win_samp} samp)", file=sys.stderr)

    intro_start_samp = int(round(args.intro_start * rate))
    intro_end_samp   = int(round(args.intro_end   * rate))
    intro_step_samp  = max(1, int(round(args.intro_step  * rate)))

    rows = []
    for s in range(intro_start_samp, intro_end_samp + 1, intro_step_samp):
        r = score(sig, s, period_samp, win_samp)
        if r is None:
            break
        point, rms = r
        rows.append((s, point, rms))

    if not rows:
        print("no scorable intro positions", file=sys.stderr)
        sys.exit(1)

    rows.sort(key=lambda x: x[2])  # rank by window RMS
    print(f"# best {min(args.top, len(rows))} splice candidates (by window RMS):",
          file=sys.stderr)
    print(f"#   intro_s    intro_samp    point_step (dBFS)   rms (dBFS)",
          file=sys.stderr)
    for s, point, rms in rows[: args.top]:
        intro_s  = s / rate
        point_db = 20 * np.log10(point + 1e-12)
        rms_db   = 20 * np.log10(rms   + 1e-12)
        print(f"#   {intro_s:8.4f}   {s:10d}     "
              f"{point:.6f} ({point_db:+6.1f})   "
              f"{rms:.6f} ({rms_db:+6.1f})", file=sys.stderr)

    s, point, rms = rows[0]
    print(f"intro_seconds={s/rate:.6f}")
    print(f"intro_samples@{rate}={s}")
    print(f"period_seconds={args.period:.6f}")
    print(f"period_samples@{rate}={period_samp}")
    print(f"point_step={point:.6g}")
    print(f"window_rms={rms:.6g}")


if __name__ == "__main__":
    main()
