#!/usr/bin/env python3
"""Exercise Cover Flow requests and loading cards with a cold host window."""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"(?m)^(?:static )?(?:void|int|bool|unsigned int) " + name +
                      r"\([^;{}]*\)\n\{", source)
    start = match.start()
    return source[start:source.index("\n}", match.end()) + 2]


FIXTURE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TIME_BEFORE(a, b) ((a) < (b))
#define HZ 100
#define ALBUM_COVERS_NUM_SLIDES 6
#define CIDER_COVER_LEAD 4
#define CIDER_COVER_DIM 128
#define CIDER_COVER_SCRATCH_BYTES (CIDER_COVER_DIM * CIDER_COVER_DIM * sizeof(fb_data))
#define LCD_RGBPACK(r, g, b) (((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3))
enum { FORMAT_NATIVE = 1, CAROUSEL_VIEW_FLAT = 1, PP_ART_RAM = 0, CS_M = 0 };

typedef uint16_t fb_data;
struct bitmap { int width, height, format; unsigned char *data; };
struct cider_row { char art[65], id[65]; uint32_t bg; };
static fb_data surface[CIDER_COVER_DIM * CIDER_COVER_DIM];
static struct { int album_covers_view_mode; } global_settings;
static uint32_t album_bg;
static int center_index = 40, previous_center = 37, travel = 1;
static long current_tick = 100, moved_at = 97, prefetch_at = 100;
static bool flicking, lane_busy, preview_ready;
static unsigned revision, single_count, batch_count;
static int single_album, batch[3];

static int count(void) { return 200; }
static unsigned cider_art_revision(void) { return 0; }
static bool storage_disk_is_active(void) { return true; }
static void carousel_provider_changed(void) { assert(false); }
static bool cider_page_peek(int index, struct cider_row *row)
{
    assert(index >= 0 && index < count());
    snprintf(row->art, sizeof(row->art), "%064x", index);
    row->bg = album_bg;
    return true;
}
static bool cider_page_row(int index, struct cider_row *row)
{
    return cider_page_peek(index, row);
}
static int album(const char *key)
{
    return (int)strtol(key + 60, NULL, 16);
}
static bool cider_has_art(const char *key, const char *tier)
{
    return preview_ready && album(key) == center_index && !strcmp(tier, "XS");
}
static bool cider_art_local(const char *key, const char *tier)
{
    return cider_has_art(key, tier);
}
static bool cider_cover_preview_local(const char *key)
{
    return cider_has_art(key, "XS");
}
static bool cider_copy_art(const char *key, const char *tier, void *out, unsigned n)
{
    assert(key && tier && out && n == sizeof(surface));
    return false;
}
static bool cider_copy_cover_preview(const char *key, uint16_t *out)
{
    if (!cider_has_art(key, "XS"))
        return false;
    memset(out, 0, 32 * 32 * sizeof(*out));
    return true;
}
static void panel_perf_record(int event, int elapsed, unsigned version, int tier)
{
    (void)event; (void)elapsed; (void)version; (void)tier;
    assert(false);
}
static unsigned fnv1a_str(const char *key)
{
    unsigned hash = 2166136261u;
    for (; *key; key++)
        hash = (hash ^ (unsigned char)*key) * 16777619u;
    return hash;
}
static unsigned cider_read_ahead_count(unsigned speed, unsigned visible)
{
    assert(speed && visible);
    return 4;
}
static void cider_cover_read_ahead(const char *const *keys, unsigned n, unsigned full)
{
    assert(n >= full && album(keys[0]) == center_index);
}
static bool cider_prefetch_art(const char *key, const char *tier)
{
    if (lane_busy)
        return false;
    assert(!strcmp(tier, "XS"));
    single_count++;
    single_album = album(key);
    lane_busy = true;
    return true;
}
static void cider_request_xs(const char *const *keys, unsigned n)
{
    assert(n <= ARRAYLEN(batch) && batch_count == 0);
    for (unsigned i = 0; i < n; i++)
        batch[i] = album(keys[i]);
    batch_count = n;
}
static void cider_warm_art(const char *const *keys, unsigned n, const char *tier)
{
    assert(keys && n && !strcmp(tier, "XS"));
}
"""

CASES = r"""
static void reset(int previous, bool busy, bool ready)
{
    previous_center = previous;
    travel = 1;
    current_tick = 100;
    moved_at = 97;
    prefetch_at = 100;
    flicking = false;
    lane_busy = busy;
    preview_ready = ready;
    single_count = batch_count = 0;
}

int main(void)
{
    reset(37, false, false);
    tick();
    assert(flicking && single_count == 1 && single_album == 40);
    assert(batch_count == 3 && batch[0] == 41 && batch[2] == 43);

    reset(43, true, false);
    tick();
    assert(flicking && single_count == 0);
    assert(batch_count == 3 && batch[0] == 40 && batch[1] == 39 && batch[2] == 38);

    reset(40, false, false);
    tick();
    assert(!flicking && single_count == 1 && single_album == 40 && batch_count == 0);

    reset(40, true, true);
    tick();
    assert(!flicking && single_count == 0 && batch_count == 3 && batch[0] == 41);

    struct bitmap bitmap;
    unsigned version;
    preview_ready = false;
    album_bg = 0x181818;
    assert(art(center_index, &bitmap, &version));
    assert((version & 3) == 1 && surface[0] == LCD_RGBPACK(56, 56, 56));
    album_bg = 0x214264;
    assert(art(center_index, &bitmap, &version));
    assert((version & 3) == 1 && surface[0] == LCD_RGBPACK(0x21, 0x42, 0x64));
    preview_ready = true;
    assert(art(center_index, &bitmap, &version));
    assert((version & 3) == 2 && surface[0] == 0);

    puts("PASS: cold previews in fast scroll, reversal, centred entry, and loading colors");
    return 0;
}
"""

with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / "cover_batch"
    provider = "apps-ipod/screens/covers/cider_covers.c"
    production = [function("apps-ipod/screens/playback/cider_model.c", "cider_window")]
    production.extend(function(provider, name) for name in ("cover_version", "art", "tick"))
    source = FIXTURE + "\n".join(production) + CASES
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", exe], input=source, text=True, check=True)
    subprocess.run([exe], cwd=directory, check=True)
