#!/usr/bin/env python3
"""render-sound.py — drive one audio pipeline entry.

Invoked by cmake/audio-payload.cmake (one process per row of sounds.txt)
and also runnable standalone for ad-hoc renders. Pipeline:

  audio-render (host C++) --> raw WAV (44100 Hz, stereo)
       |
       v   [music_loop only: ffmpeg trims to sample-exact intro+loop; with
       |    --fade the file tail is crossfaded into the pre-loop-point
       |    audio so the wav64 wrap is sample-continuous]
       |
  audioconv64 --> VADPCM wav64 (22050 Hz; --wav-loop-offset for music_loop)

External-tool defaults when flags are omitted:
  --render-bin  build-host/audio-render/audio-render   (this repo)
  --audioconv   $N64_INST/bin/audioconv64              (libdragon install)
  --ffmpeg      ffmpeg                                 (from PATH)
  --roms        roms/                                  (this repo)
"""

import argparse
import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave


def run(cmd):
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}")


def default_audioconv():
    n64_inst = os.environ.get("N64_INST")
    if n64_inst:
        path = os.path.join(n64_inst, "bin", "audioconv64")
        if os.path.exists(path):
            return path
    found = shutil.which("audioconv64")
    if found:
        return found
    sys.exit("audioconv64 not found: set $N64_INST or pass --audioconv")


def default_ffmpeg():
    found = shutil.which("ffmpeg")
    if found:
        return found
    sys.exit("ffmpeg not found: install it or pass --ffmpeg")


def default_render_bin():
    # Relative to the repo root, where cmake builds the host tool.
    candidates = [
        "build-host/audio-render/audio-render",
        os.path.join(os.path.dirname(__file__),
                     "../../build-host/audio-render/audio-render"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    sys.exit("audio-render not found: build with "
             "`cmake -S tools/audio-render -B build-host/audio-render && "
             "cmake --build build-host/audio-render` or pass --render-bin")


def crossfade_loop_tail(path, intro, fade, render_rate, wav64_rate):
    """Blend the last `fade` seconds of `path` into the audio that precedes
    the loop point, so the final sample lands exactly on the sample the
    wav64 wrap jumps to.

    Playback loops from the file end back to the loop offset S. The material
    at both points is the same musical phrase, but the chips render it from
    different internal state, so the raw seam has a discontinuity. Rewriting
    the tail as a raised-cosine blend toward orig[S-fade .. S) makes the wrap
    sample-continuous and reproduces the intro->loop transition the listener
    heard on the first pass. Fading here, at the render rate, keeps the
    continuity through audioconv64's resample to the wav64 rate.
    """
    with wave.open(path, "rb") as w:
        n_ch = w.getnchannels()
        if w.getsampwidth() != 2 or w.getframerate() != render_rate:
            sys.exit(f"crossfade: {path} is not 16-bit {render_rate} Hz")
        n_frames = w.getnframes()
        data = array.array("h")
        data.frombytes(w.readframes(n_frames))

    # The wav64 loop offset is quantised to the wav64 rate; target the same
    # instant here so the fade lands on the exact post-resample jump target.
    if render_rate % wav64_rate != 0:
        sys.exit("crossfade: render rate must be a multiple of the wav64 rate")
    step = render_rate // wav64_rate
    loop_frame = int(round(intro * wav64_rate)) * step
    fade_frames = int(fade * render_rate)
    if loop_frame < fade_frames:
        sys.exit(f"crossfade: loop point {loop_frame} inside fade window "
                 f"{fade_frames} — intro too short")

    for i in range(fade_frames):
        # Raised-cosine, amplitude-complementary (right choice for the
        # highly-correlated material on both sides of the blend). At the
        # last frame wf == 1, so the file ends on orig[loop_frame - 1].
        wf = math.sin(math.pi / 2 * (i + 1) / fade_frames) ** 2
        dst = (n_frames - fade_frames + i) * n_ch
        src = (loop_frame - fade_frames + i) * n_ch
        for c in range(n_ch):
            a = data[dst + c]
            data[dst + c] = int(round(a + (data[src + c] - a) * wf))

    with wave.open(path, "wb") as w:
        w.setnchannels(n_ch)
        w.setsampwidth(2)
        w.setframerate(render_rate)
        w.writeframes(data.tobytes())


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--render-bin",
                   help="path to host audio-render binary (default: "
                        "build-host/audio-render/audio-render)")
    p.add_argument("--audioconv",
                   help="path to audioconv64 (default: $N64_INST/bin/audioconv64)")
    p.add_argument("--ffmpeg",
                   help="path to ffmpeg (default: from $PATH)")
    p.add_argument("--roms",       default="roms/",
                   help="path to OutRun ROM directory (default: roms/)")
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
    p.add_argument("--fade",       type=float, default=0.0,
                   help="music_loop only: loop-wrap crossfade length in "
                        "seconds (0 = bare wrap)")
    p.add_argument("--duration",   type=float,
                   help="music_oneshot / fx: total render length in seconds")
    p.add_argument("--render-rate", type=int, default=44100)
    p.add_argument("--wav64-rate",  type=int, default=22050)
    args = p.parse_args()

    if not args.render_bin:
        args.render_bin = default_render_bin()
    if not args.audioconv:
        args.audioconv  = default_audioconv()
    if not args.ffmpeg:
        args.ffmpeg     = default_ffmpeg()

    os.makedirs(os.path.dirname(args.out_wav)   or ".", exist_ok=True)
    os.makedirs(os.path.dirname(args.out_wav64) or ".", exist_ok=True)

    if args.kind == "music_loop":
        if args.intro is None or args.loop is None:
            sys.exit("music_loop requires --intro and --loop")
        # The tail crossfade blends toward the audio just before the loop
        # point, so the loop point needs at least `fade` seconds of lead-in.
        # For a shorter intro, shift the loop point past the fade window:
        # the period is unchanged and the wrap replays the same phrase
        # either way, so this is musically identical.
        args.intro = max(args.intro, args.fade)
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
            if args.fade > 0:
                crossfade_loop_tail(encoder_input, args.intro, args.fade,
                                    args.render_rate, args.wav64_rate)
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
            # audioconv64 interprets --wav-loop-offset in input-file samples
            # and scales it by the resample ratio, so pass it at the render
            # rate. Quantise to the wav64 rate first so the scaled offset is
            # exact and matches the crossfade target in crossfade_loop_tail().
            step = args.render_rate // args.wav64_rate
            loop_off = int(round(args.intro * args.wav64_rate)) * step
            cmd += ["--wav-loop", "true", "--wav-loop-offset", str(loop_off)]
        # Pause-menu resume calls wav64_seek; VADPCM can only land on encoded
        # skip points, so without these the music restarts from 0. Music kinds
        # get a dense table (every 0.25s); fx oneshots are too short to matter.
        if args.kind in ("music_loop", "music_oneshot"):
            cmd += ["--wav-seek", "0.25"]
        cmd.append(encoder_input)
        run(cmd)


if __name__ == "__main__":
    main()
