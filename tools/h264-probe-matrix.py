#!/usr/bin/env python3
"""Generate ten-second AVC compatibility clips from a supplied film."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import re


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
    # GOP 2 with B-frame capability makes x264 use POC 0, but emits I/P only.
    yield "poc0", {"params": "keyint=2:min-keyint=2:scenecut=0:"
                             "bframes=1:cabac=0:weightp=0:weightb=0:b-pyramid=none",
                   "profile": "main"}
    yield "poc2-control", {"params": "keyint=2:min-keyint=2:scenecut=0"}


def generate(source, directory, name, *, refs=3, size="576:240",
             preset="medium", level="3.0", params="", rate="",
             codec="libx264", profile="baseline"):
    dst = directory / f"{name}.mp4"
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning",
               "-y", "-ss", "180", "-i", source, "-t", "10", "-an",
               "-vf", f"scale={size},fps=24,setsar=1", "-pix_fmt", "yuv420p",
               "-c:v", codec, "-profile:v", profile, "-level:v", level,
               "-preset", preset if codec == "libx264" else "p4",
               "-threads", "3", "-bf", "0"]
    if refs is not None:
        command += ["-refs", str(refs)]
        if codec == "h264_nvenc":
            command += ["-dpb_size", str(refs)]
    if params:
        command += ["-x264-params", params]
    if rate:
        command += ["-b:v", rate, "-minrate", rate, "-maxrate", rate,
                    "-bufsize", rate, "-x264-params", "nal-hrd=cbr"]
    with (directory / f"{name}.encode.log").open("w") as log:
        subprocess.run([*command, "-movflags", "+faststart", dst],
                       stderr=log, check=True)
    if name == "poc0":
        frames = subprocess.check_output([
            "ffprobe", "-v", "error", "-select_streams", "v:0",
            "-show_entries", "frame=pict_type", "-of", "csv=p=0", dst
        ], text=True).split()
        if len(frames) != 240 or set(frames) != {"I", "P"}:
            raise ValueError("POC 0 test stream must contain only I and P frames")

        # Relabel the I/P, CAVLC stream as Constrained Baseline. The SPS
        # layout for these two profiles is identical.
        data = bytearray(dst.read_bytes())
        avcc = data.index(b"avcC\1\x4d") + 4
        sps = avcc + 8
        if data[avcc + 5] & 31 != 1 or data[sps:sps + 2] != b"\x67\x4d":
            raise ValueError("Expected exactly one Main-profile SPS")
        data[avcc + 1:avcc + 3] = data[sps + 1:sps + 3] = b"\x42\xc0"
        dst.write_bytes(data)


def frame_num_12(control, output):
    """Widen frame_num from 4 to 12 bits in the 16-frame GOP control."""
    command = ["ffmpeg", "-nostdin", "-v", "error"]
    stream = subprocess.check_output([
        *command, "-i", control, "-c:v", "copy", "-bsf:v", "h264_mp4toannexb",
        "-f", "h264", "-"
    ])
    def ue_end(bits, start):
        leading_zeroes = bits.index("1", start) - start
        return start + 2 * leading_zeroes + 1

    def rewrite(nalu):
        kind = nalu[0] & 31
        if kind not in (1, 5, 7):
            return nalu
        rbsp = nalu[1:].replace(b"\0\0\3", b"\0\0")
        bits = "".join(f"{value:08b}" for value in rbsp).rstrip("0")
        start = 24 if kind == 7 else 0
        for _ in range(1 if kind == 7 else 3):
            start = ue_end(bits, start)
        if kind == 7:
            if bits[start] != "1":
                raise ValueError("Expected a 4-bit frame_num")
            bits = bits[:start] + "0001001" + bits[start + 1:]
        else:
            bits = bits[:start] + "00000000" + bits[start:]
        bits += "0" * (-len(bits) % 8)
        rbsp = int(bits, 2).to_bytes(len(bits) // 8, "big")
        ebsp = bytearray()
        zeroes = 0
        for value in rbsp:
            if zeroes == 2 and value <= 3:
                ebsp.append(3)
                zeroes = 0
            ebsp.append(value)
            zeroes = zeroes + 1 if value == 0 else 0
        return nalu[:1] + ebsp

    units = re.split(rb"\x00\x00\x00?\x01", stream)
    changed = b"".join(b"\0\0\0\1" + rewrite(unit)
                       for unit in units if unit)
    subprocess.run([*command, "-y", "-r", "24", "-f", "h264", "-i", "-",
                    "-c:v", "copy", output], input=changed, check=True)


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
    frame_num_12(args.directory / "keyint16.mp4",
                 args.directory / "frame-num12.mp4")
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
