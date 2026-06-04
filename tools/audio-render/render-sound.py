#!/usr/bin/env python3
"""render-sound.py — drive one audio pipeline entry.

Internal helper invoked by cmake/audio-payload.cmake, one process per row
of sounds.txt. Pipeline:

  audio-render (host C++) --> raw WAV (44100 Hz, stereo)
       |
       v   [music_loop only: ffmpeg trims to sample-exact intro+loop]
       |
  audioconv64 --> VADPCM wav64 (22050 Hz; --wav-loop-offset for music_loop)

Direct invocation works too — pass --kind / --intro / --loop / --duration
matching one sounds.txt row.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def run(cmd):
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--render-bin", required=True)
    p.add_argument("--audioconv",  required=True)
    p.add_argument("--ffmpeg",     required=True)
    p.add_argument("--roms",       required=True)
    p.add_argument("--out-wav",    required=True,
                   help="raw 44100 Hz render destination")
    p.add_argument("--out-wav64",  required=True,
                   help="final VADPCM wav64 destination")
    p.add_argument("--id",         required=True,
                   help="hex sound id (e.g. 0x81)")
    p.add_argument("--kind",       required=True,
                   choices=["music_loop", "music_oneshot", "fx"])
    p.add_argument("--intro",      type=float,
                   help="music_loop only: intro length in seconds")
    p.add_argument("--loop",       type=float,
                   help="music_loop only: loop period in seconds")
    p.add_argument("--duration",   type=float,
                   help="music_oneshot / fx: total render length in seconds")
    p.add_argument("--render-rate", type=int, default=44100)
    p.add_argument("--wav64-rate",  type=int, default=22050)
    args = p.parse_args()

    os.makedirs(os.path.dirname(args.out_wav)   or ".", exist_ok=True)
    os.makedirs(os.path.dirname(args.out_wav64) or ".", exist_ok=True)

    if args.kind == "music_loop":
        if args.intro is None or args.loop is None:
            sys.exit("music_loop requires --intro and --loop")
        total = args.intro + args.loop
        # Render slightly long so ffmpeg has audio at the exact splice point.
        render_dur = total + 0.2
        render_extra = []
    else:
        if args.duration is None:
            sys.exit(f"{args.kind} requires --duration")
        render_dur = args.duration
        render_extra = ["--no-pcm"] if args.kind == "fx" else []

    run([
        args.render_bin, args.id, f"{render_dur:.6f}", args.out_wav,
        "--rom-path", args.roms,
        "--rate",     str(args.render_rate),
    ] + render_extra)

    out_wav64_dir = os.path.dirname(args.out_wav64) or "."
    expected_basename = os.path.splitext(os.path.basename(args.out_wav64))[0]

    # audioconv64 names its output after the input basename, so we feed it
    # an input whose stem matches the desired wav64 name. For music_loop we
    # also need a sample-exact trim, so do both in a temp dir.
    with tempfile.TemporaryDirectory() as td:
        if args.kind == "music_loop":
            encoder_input = os.path.join(td, expected_basename + ".wav")
            total = args.intro + args.loop
            run([
                args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                "-i", args.out_wav,
                "-t", f"{total:.6f}",
                "-c:a", "pcm_s16le",
                encoder_input,
            ])
        else:
            # One-shots: skip the trim. Stage a symlink/copy under the
            # expected name so audioconv64 produces the right .wav64.
            encoder_input = os.path.join(td, expected_basename + ".wav")
            try:
                os.symlink(os.path.abspath(args.out_wav), encoder_input)
            except (OSError, NotImplementedError):
                shutil.copyfile(args.out_wav, encoder_input)

        cmd = [
            args.audioconv, "-v",
            "-o", out_wav64_dir,
            "--wav-resample", str(args.wav64_rate),
            "--wav-compress", "1",
        ]
        if args.kind == "music_loop":
            loop_off = int(round(args.intro * args.wav64_rate))
            cmd += ["--wav-loop", "true", "--wav-loop-offset", str(loop_off)]
        cmd.append(encoder_input)
        run(cmd)


if __name__ == "__main__":
    main()
