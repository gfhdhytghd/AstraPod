#!/usr/bin/env python3
"""Run the iPod6G AVC parser and decoder preparation with a mock VPU."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = "firmware/target/arm/s5l8702/ipod6g/vpu-h264-6g.c"

STUB = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#define ALIGN_UP(x,a) (((uintptr_t)(x) + (a) - 1) & ~(uintptr_t)((a) - 1))
#define CACHEALIGN_UP(x) ALIGN_UP(x,32)
#define S5L8702_PHYSICAL_ADDR(x) (x)
#define S5L8702_UNCACHED_ADDR(x) (x)
#define USEC_TIMER 0
static void commit_dcache(void) {}
static void commit_discard_dcache(void) {}
static void vpub_power_on(void) {}
static void vpub_power_off(void) {}
"""

MAIN = r"""
int main(int argc, char **argv)
{
    unsigned char avcc[65536];
    size_t avcc_bytes = (size_t)atoi(argv[3]);
    if (avcc_bytes > sizeof(avcc))
        return 2;
    int w = atoi(argv[1]), h = atoi(argv[2]);
    size_t bytes = vpu_h264_buf_size(w, h);
    void *buf = malloc(bytes);
    struct vpu_h264 *v = vpu_h264_open(buf, bytes, w, h);
    size_t len = fread(avcc, 1, avcc_bytes, stdin);
    int code = v ? vpu_h264_configure(v, avcc, len) : -1;
    if (!v)
    {
        puts("open rejected coded dimensions");
        free(buf);
        return 0;
    }
    if (len != avcc_bytes || len < 7) return 2;
    printf("configure=%d %s coded=%dx%d crop=%d,%d,%d,%d level=%d "
           "refs=%d pps_refs=%d frame_num_bits=%d poc=%d\n",
           code, rejection, (v->sps.pic_width_in_mbs_minus1 + 1) * 16,
           (v->sps.pic_height_in_map_units_minus1 + 1) * 16,
           v->sps.crop_left, v->sps.crop_right, v->sps.crop_top,
           v->sps.crop_bottom, v->sps.level_idc, v->sps.max_num_ref_frames,
           v->have_pps ? v->pps.num_ref_idx_l0_default_active_minus1 + 1 : 0,
           v->sps.log2_max_frame_num_minus4 + 4, v->sps.pic_order_cnt_type);
    if (code == 0 && argc > 4)
    {
        FILE *movie = fopen(argv[4], "rb");
        unsigned pos, size, frames = 0, samples = 0;
        if (!movie) return 2;
        while (scanf("%u %u", &pos, &size) == 2)
        {
            unsigned char *packet = malloc(size);
            if (!packet || fseek(movie, pos, SEEK_SET) ||
                fread(packet, 1, size, movie) != size) return 2;
            rejection = "";
            code = vpu_h264_decode_sample(v, packet, size, (avcc[4] & 3) + 1);
            free(packet);
            if (code < 0) break;
            samples++;
            frames += code > 0;
        }
        printf("samples=%u frames=%u decode=%d %s\n",
               samples, frames, code, rejection);
        fclose(movie);
    }
    free(buf);
    return 0;
}
"""


def compile_driver(revision, target):
    if revision:
        source = subprocess.check_output(
            ["git", "-C", ROOT, "show", f"{revision}:{DRIVER}"], text=True)
    else:
        source = (ROOT / DRIVER).read_text()
    start = source.index("/* ---- VPU-B power management")
    end = source.index("/* ---- Internal context structure")
    signature = re.search(r"static int vpub_decode\([^{}]+\)", source[start:end])
    source = source[:start] + signature[0] + " { return 0; }\n" + source[end:]
    source = re.sub(r'^#include "(?:config|system|kernel|s5l87xx|vpu-6g)\.h"\n',
                    "", source, flags=re.M)
    # Keep the attempted SPS visible even when validation rejects it.
    source = source.replace("    parse_sps(&sps, b);",
                            "    parse_sps(&sps, b);\n    probe_sps = sps;")
    if "probe_sps = sps;" in source:
        start = source.index("static int configure_sps")
        source = source[:start] + "static struct sps probe_sps;\n" + source[start:]
        main = MAIN.replace("v->sps.", "probe_sps.")
    else:
        main = MAIN
    # Report the baseline guard directly, without changing its condition.
    source = source.replace("    if (v->sps.profile_idc != 66 ||",
        '    if (v->sps.max_num_ref_frames >= MAX_DPB_FRAMES)\n'
        '        rejection = "max_num_ref_frames >= MAX_DPB_FRAMES";\n'
        '    if (v->sps.profile_idc != 66 ||')
    source = source.replace("    if (b.error ||\n        (slice->header.slice_type",
        '    if (slice->header.ref_list_modification)\n'
        '        rejection = "ref_pic_list_modification";\n'
        '    if (slice->header.adaptive_ref_marking)\n'
        '        rejection = "adaptive_ref_pic_marking_mode_flag";\n'
        '    if (b.error ||\n        (slice->header.slice_type')
    subprocess.run(["cc", "-O2", "-I", ROOT / "firmware/export", "-I",
                    ROOT / DRIVER.rsplit("/", 1)[0], "-x", "c", "-", "-o", target],
                   input=STUB + 'static const char *rejection = "";\n' +
                   source + main, text=True, check=True)


def probe(path, runner, samples):
    info = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,extradata" +
        (":packet=pos,size" if samples else ""),
        "-show_data", *(["-show_packets"] if samples else
                        ["-read_intervals", "0%+#1"]), "-of", "json", path]))
    stream = info["streams"][0]
    avcc = bytes.fromhex("".join(
        line.split(":", 1)[1].split("  ", 1)[0].strip()
        for line in stream["extradata"].strip().splitlines()))
    dimensions = [str((stream[key] + 15) & ~15) for key in ("width", "height")]
    print(path.name, flush=True)
    packets = "".join(f"{p['pos']} {p['size']}\n" for p in info.get("packets", []))
    subprocess.run([runner, *dimensions, str(len(avcc)), *([path] if samples else [])],
                   input=avcc + packets.encode(), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision")
    parser.add_argument("--samples", action="store_true")
    parser.add_argument("movie", type=Path, nargs="+")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        runner = Path(temp) / "probe"
        compile_driver(args.revision, runner)
        for movie in args.movie:
            probe(movie, runner, args.samples)


if __name__ == "__main__":
    main()
