#!/usr/bin/env python3
"""Check the carousel's LCD submissions across motion, landing, idle and exit.

Usage: python settle_refresh.py [carousel.c]
"""
from pathlib import Path
import subprocess
import sys
import tempfile


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def check(source):
    # Compile the player loop's state dispatch and transfer branches. The
    # clock stub controls which sample is the last; the LCD records each call.
    loop = source[source.index('if (pf_bg_color != last_bg)'):]
    dispatch = between(loop[loop.index('if (drew_art)'):],
                       'if (drew_art)', '/* Whether the covers')
    send = between(loop, 'if (full_flush_owed)', '/* Set the art deadline')
    probe = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define USEC_TIMER 0
#define UI_PERF_ART 1
#define UI_PERF_CAPTION 2
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ui_perf_add_transfer(a,b,c) ((void)(b), (void)(c))

static enum { pf_idle, pf_scrolling } pf_state;
/* The ordinary carousel has no entry/exit picture to blend. */
static void *fade;
static uint32_t fade_start;
static unsigned leave_from, opacity;
#define FLOW_LEAVE_FADE_US 250000u
#define FLOW_MENU_FADE_US 187500u
#define flow_present_fade(p,a) (abort(), false)
#define lcd_update() abort()

static bool full_flush_owed, caption_owed, land_now, leaving;
static int captioned_index, samples, full, partial;
static uint8_t panel[LCD_HEIGHT][LCD_WIDTH], framebuffer[LCD_HEIGHT][LCD_WIDTH];

static int carousel_caption_index(void) { return 3; }
static void cover_rows(int *top, int *end) { *top = 40; *end = 168; }
static void update_scroll_animation(void)
{
    ++samples;
    if (land_now)
        pf_state = pf_idle;
}
static void lcd_update_rect(int x, int y, int w, int h)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > LCD_WIDTH || y + h > LCD_HEIGHT) abort();
    if (x == 0 && y == 0 && w == LCD_WIDTH && h == LCD_HEIGHT) ++full;
    else ++partial;
    for (int row = y; row < y + h; row++)
        memcpy(panel[row] + x, framebuffer[row] + x, w);
}

static void frame(bool drew_art, bool caption_flush_owed)
{
    bool drew_caption = false;
    int art_y = 0, art_h = 0;
    int trace_id = 0, *trace = &trace_id;
    int caption_flush_top = 175, caption_flush_end = LCD_HEIGHT;
    DISPATCH
    int dirty_top = drew_art ? art_y : LCD_HEIGHT;
    int dirty_end = drew_art ? art_y + art_h : 0;
    if (drew_caption)
        framebuffer[220][90] = 0x55;
    SEND
    (void)caption_rows;
    (void)parts;
}

static void expect(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
int main(void)
{
    for (int run = 1; run <= 2; run++) {
        pf_state = pf_scrolling;
        land_now = false;
        /* The second selection is known but still awaits painting. */
        captioned_index = run == 1 ? -1 : carousel_caption_index();
        caption_owed = run == 2;
        int prior_samples = samples, prior_full = full, prior_partial = partial;
        panel[232][27] = 0xcc;
        framebuffer[220][90] = 0;

        frame(false, false);         /* caption-only iteration */
        frame(true, false);
        frame(true, false);
        expect(samples == prior_samples + 2 && full == prior_full &&
               partial == prior_partial + 2, "moving frames changed transfer cadence");

        land_now = true;
        frame(true, true);           /* bottom-aligned caption still pending */
        expect(full == prior_full + 1 && partial == prior_partial + 2 &&
               memcmp(panel, framebuffer, sizeof(panel)) == 0,
               "landing did not publish the complete framebuffer in one transfer");
        expect(framebuffer[220][90] == 0x55 && !caption_owed,
               "landing did not paint and consume the pending caption");
        frame(true, false);          /* artwork loads after landing */
        frame(true, false);
        expect(samples == prior_samples + 3 && full == prior_full + 1,
               "idle repaint repeated the settle refresh");
    }
    leaving = true;
    pf_state = pf_scrolling;
    land_now = true;
    frame(true, false);
    expect(full == 2, "leave animation repeated the settle refresh");
    puts("two full-screen transfers for two settles; moving, idle and leave checks passed");
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'probe.c'
        binary = Path(tmp) / 'probe'
        src.write_text(probe.replace('DISPATCH', dispatch).replace('SEND', send))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', str(src),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    default = Path(__file__).resolve().parent.parent / 'carousel.c'
    check((Path(sys.argv[1]) if len(sys.argv) > 1 else default).read_text())
