#!/usr/bin/env python3
"""Generate ten-second AVC compatibility clips from a supplied film."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess


def cases():
    yield "control", {}
    for refs in range(1, 17):
        yield f"refs{refs:02d}", {"refs": refs}
    for refs in (6, 7, 16):
        yield f"vga-refs{refs:02d}", {"refs": refs, "size": "640:480"}
    for refs in (15, 16):
        yield f"small-refs{refs:02d}", {"refs": refs, "size": "320:240"}
    for preset in ("ultrafast", "superfast", "veryfast", "faster", "fast",
                   "medium", "slow", "slower", "veryslow"):
        yield f"preset-{preset}", {"preset": preset, "refs": None}
    for w in (560, 564, 570, 574, 640, 642):
        yield f"width{w}", {"size": f"{w}:240"}
    for h in (222, 238, 256, 360, 480, 482):
        yield f"height{h}", {"size": f"576:{h}"}
    for name, crop in (("left", "2,0,0,0"), ("right", "0,0,2,0"),
                       ("top", "0,2,0,0"), ("bottom", "0,0,0,2"),
                       ("all", "2,2,2,2")):
        yield f"crop-{name}", {"params": f"crop-rect={crop}"}
    for level in ("1.3", "2.0", "2.1", "2.2", "3.0"):
        yield f"level-{level}", {"level": level, "size": "352:240"}
    for keyint in (16, 32, 64, 250, 512):
        yield f"keyint{keyint}", {"params": f"keyint={keyint}"}
    for slices in (1, 2, 4, 10, 11):
        yield f"slices{slices:02d}", {"params": f"slices={slices}"}
    for kbps in (500, 1000, 1500, 2000, 2500):
        yield f"rate{kbps}", {"rate": f"{kbps}k"}
    yield "nvenc", {"codec": "h264_nvenc"}
    yield "vga", {"size": "640:480"}


def generate(source, directory, name, *, refs=3, size="576:240",
             preset="medium", level="3.0", params="", rate="",
             codec="libx264"):
    dst = directory / f"{name}.mp4"
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning",
               "-y", "-ss", "180", "-i", source, "-t", "10", "-an",
               "-vf", f"scale={size},fps=24,setsar=1", "-pix_fmt", "yuv420p",
               "-c:v", codec, "-profile:v", "baseline", "-level:v", level,
               "-preset", preset if codec == "libx264" else "p4",
               "-threads", "3", "-bf", "0"]
    if refs is not None:
        command += ["-refs", str(refs)]
    if params:
        command += ["-x264-params", params]
    if rate:
        command += ["-b:v", rate, "-minrate", rate, "-maxrate", rate,
                    "-bufsize", rate, "-x264-params", "nal-hrd=cbr"]
    with (directory / f"{name}.encode.log").open("w") as log:
        subprocess.run([*command, "-movflags", "+faststart", dst],
                       stderr=log, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = [(name, pool.submit(generate, args.source, args.directory,
                                      name, **options))
                   for name, options in cases()]
        for name, result in results:
            try:
                result.result()
            except subprocess.CalledProcessError:
                print(f"{name}: encode failed; see encode log", flush=True)
    for variant in ("crop240", "crop576"):
        source = args.source.with_name(
            f"{args.source.stem.split('.')[0]}.{variant}.m4v")
        if source.exists():
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y",
                            "-i", source, "-t", "10", "-map", "0:v:0",
                            "-c", "copy", args.directory / f"original-{variant}.mp4"],
                           check=True)


if __name__ == "__main__":
    main()
