/* SPDX-License-Identifier: GPL-2.0-or-later
 * Album palette extraction adapted from RockPod. Copyright (C) 2026 RockPod contributors.
 */
#include "config.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"

#include <stdlib.h>
#include <string.h>
#include "lcd.h"
#include "settings/settings.h"
#include "kernel.h"
#include "system.h"
#include "audio/playback.h"
#include "system/appevents.h"
#include "core_alloc.h"
#include "file.h"
#include "storage.h"
#include "usb.h"
#include "system/bg_task.h"
#include "ata_idle_notify.h"
#include "string-extra.h"
#include "metadata/art_cache.h"
#include "metadata/art_name.h"
#include "playlist/playlist.h"
#include "draw/bmp.h"
#include "draw/color.h"
#include "draw/img_filter.h"
#include "ui/native_wps.h"
#include "album_palette.h"

#define PALETTE_MAX_DIM 128
#define HISTOGRAM_BUCKETS 4096
#define SAMPLE_TARGET (PALETTE_MAX_DIM * PALETTE_MAX_DIM)
#define MIN_RATIO 600
#define SATURATION_BASE 8
#define ACCENT_MIN_RATIO 300
#define ART_MIN_RATIO 300
#define PALETTE_EMBEDDED_WAIT HZ
#define PALETTE_REPAINT_WINDOW (HZ * 12 / 60)

/* Shared by the UI, audio and background prefetch under palette_lock.
 * The large RGB565 area is also reused as the backdrop filter's workspace. */
struct palette_workspace {
    fb_data pixels[PALETTE_MAX_DIM * PALETTE_MAX_DIM];
    uint16_t histogram[HISTOGRAM_BUCKETS];
    int8_t sector_of[HISTOGRAM_BUCKETS];
    char cache_path[MAX_PATH];
#ifdef HAVE_USB_PODLINK
    char remote_key[65];       /* observed by the UI thread only */
#endif
    struct mp3entry idle_track; /* used only by the background task */
};

static int workspace_handle;
static struct mutex palette_lock;
static volatile unsigned track_serial;
static unsigned observed_track_serial;
static volatile unsigned art_generation = 1;
static unsigned observed_art_generation = 1;
static unsigned palette_generation;
static bool needs_extraction;
static long change_tick, request_tick;

void album_palette_alloc(void)
{
    workspace_handle = core_alloc_free_space(sizeof(struct palette_workspace));
}

static int compute_luminance(int r8, int g8, int b8)
{
    return (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
}

static int color_luminance(unsigned int c)
{
    return compute_luminance(RGB_UNPACK_RED(c), RGB_UNPACK_GREEN(c),
                             RGB_UNPACK_BLUE(c));
}

/* WCAG relative luminance, 0..LUM_MAX.
 *
 * sRGB is linearised with a gamma of 2.0 rather than the exact piecewise 2.4
 * curve: a multiply instead of a lookup table, and well inside the tolerance of
 * a pass/fail threshold. */
#define LUM_MAX  65025          /* 255 * 255 */
#define LUM_5PCT  3251          /* the 0.05 term in the WCAG ratio */

static int32_t rel_luminance(int r8, int g8, int b8)
{
    int32_t r = (int32_t)r8 * r8;
    int32_t g = (int32_t)g8 * g8;
    int32_t b = (int32_t)b8 * b8;

    return (r * 2126 + g * 7152 + b * 722) / 10000;
}

/* Contrast between two relative luminances, in hundredths -- 450 is WCAG's
 * 4.5:1 for body text. A ratio, not a difference: the same gap between two dark
 * colours reads far weaker than between two light ones, which is exactly where
 * album art tends to sit. */
static int contrast_ratio(int32_t lum_a, int32_t lum_b)
{
    int32_t hi = MAX(lum_a, lum_b) + LUM_5PCT;
    int32_t lo = MIN(lum_a, lum_b) + LUM_5PCT;

    return (int)((hi * 100) / lo);
}

/* Mean full-precision colour of the sampled pixels a quantised bucket caught.
 * The histogram is 4 bits a channel, so the bucket index alone is not a colour
 * worth putting on screen. */
static void average_bucket(const fb_data *pixels, int total_pixels, int stride,
                           int bucket, int *out_r, int *out_g, int *out_b)
{
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int count = 0;
    int i;

    for (i = 0; i < total_pixels; i += stride)
    {
        unsigned short px = (unsigned short)pixels[i];
        int r4 = (px >> 12) & 0xF;
        int g4 = (px >> 7) & 0xF;
        int b4 = (px >> 1) & 0xF;

        if (((r4 << 8) | (g4 << 4) | b4) == bucket)
        {
            sum_r += RGB_UNPACK_RED(px);
            sum_g += RGB_UNPACK_GREEN(px);
            sum_b += RGB_UNPACK_BLUE(px);
            count++;
        }
    }

    *out_r = count ? (int)(sum_r / count) : 0;
    *out_g = count ? (int)(sum_g / count) : 0;
    *out_b = count ? (int)(sum_b / count) : 0;
}

/* Hues for the accent, pooled rather than taken a bucket at a time.
 *
 * A soft gradient -- an iridescent disc, a sky -- spreads one hue over dozens
 * of neighbouring buckets with a handful of pixels each, so no single bucket
 * outscores a flat patch a fraction of its size. Hues are pooled into 30-degree
 * sectors and each sector is judged with its two neighbours. */
#define HUE_SECTORS    12
#define HUE_MIN_SPREAD 2        /* 4-bit steps between channels to be a hue */
#define HUE_MIN_SHARE  32       /* a run needs 1/32 of the samples */
#ifndef HUE_ACCENT_SAT
#define HUE_ACCENT_SAT 160      /* 0..255: the least an accent taken this way has */
#endif

/* The bucket's hue sector, or -1 for a grey, dark or near-white one.
 *
 * Grey is judged by the channels' spread in 4-bit steps, not by saturation: a
 * single step is quantisation, and near black it reads as 20% saturation or
 * more, which would make the specks of print on a pale cover count as a
 * colour. Two steps keeps a pale pastel such as fdd. */
static int bucket_sector(int i)
{
    int r4 = (i >> 8) & 0xF;
    int g4 = (i >> 4) & 0xF;
    int b4 = i & 0xF;
    int h, s, v;

    if ((r4 < 4 && g4 < 4 && b4 < 4) || (r4 > 13 && g4 > 13 && b4 > 13))
        return -1;
    if (MAX(MAX(r4, g4), b4) - MIN(MIN(r4, g4), b4) < HUE_MIN_SPREAD)
        return -1;

    color_get_hsv(LCD_RGBPACK(r4 * 17, g4 * 17, b4 * 17), &h, &s, &v);
    return h * HUE_SECTORS / 360;
}

/* The bucket's sample count, weighted toward saturated colours. */
static unsigned int bucket_score(const uint16_t *histogram, int i)
{
    int r4 = (i >> 8) & 0xF;
    int g4 = (i >> 4) & 0xF;
    int b4 = i & 0xF;
    int max_c = MAX(MAX(r4, g4), b4);
    int min_c = MIN(MIN(r4, g4), b4);
    int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;

    return (unsigned int)histogram[i]
           * (sat + SATURATION_BASE) / SATURATION_BASE;
}

/* The picture's strongest run of one hue other than the dominant's, as its
 * best-scored bucket, or -1 when no run covers HUE_MIN_SHARE of the picture.
 * A run centred within a sector of a coloured dominant does not count.
 * *run_count is the winning run's sample count. */
static int strongest_hue_bucket(struct palette_workspace *work, int dom_bucket,
                                 unsigned int *run_count)
{
    const uint16_t *histogram = work->histogram;
    int8_t *sector_of = work->sector_of;
    unsigned int sector_count[HUE_SECTORS] = { 0 };
    unsigned int samples = 0;
    int dom_sector = -1;
    int best_run = -1;
    unsigned int best_run_count = 0;
    int i;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        samples += histogram[i];
        sector_of[i] = histogram[i] ? bucket_sector(i) : -1;
        if (sector_of[i] >= 0)
            sector_count[sector_of[i]] += histogram[i];
    }

    if (dom_bucket >= 0)
        dom_sector = sector_of[dom_bucket];

    for (i = 0; i < HUE_SECTORS; i++)
    {
        unsigned int run = sector_count[(i + HUE_SECTORS - 1) % HUE_SECTORS]
                         + sector_count[i]
                         + sector_count[(i + 1) % HUE_SECTORS];
        int apart = abs(i - dom_sector);

        if (dom_sector >= 0 && MIN(apart, HUE_SECTORS - apart) <= 1)
            continue;
        if (run > best_run_count)
        {
            best_run_count = run;
            best_run = i;
        }
    }

    *run_count = best_run_count;
    if (best_run < 0 || best_run_count * HUE_MIN_SHARE < samples)
        return -1;

    int bucket = -1;
    unsigned int best_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        int d = abs(sector_of[i] - best_run);

        if (sector_of[i] < 0 || i == dom_bucket || MIN(d, HUE_SECTORS - d) > 1)
            continue;

        unsigned int score = bucket_score(histogram, i);

        if (score > best_score)
        {
            best_score = score;
            bucket = i;
        }
    }

    return bucket;
}

/* The accent against a given dominant: the best-scored bucket clearing
 * MIN_RATIO against it, brought up to that ratio if nothing clears it.
 *
 * Unless that bucket is a grey, or there is none, and one hue covers more of
 * the picture than it does. On a light or pastel cover every colour is too
 * close to the dominant to be text on it, so the only buckets clearing the bar
 * are a few specks of black print; the hue the cover is actually made of is
 * then darkened or lightened until it reads instead. A white title on a dark
 * cover keeps its white unless one hue covers more of the cover than it does.
 *
 * A dark hue must also read against the picture as a whole ('art_rl'), which
 * black print does and a mid-dark hue may not; failing that sends
 * compute_palette() into its dark-accent rescue, which turns the palette over. */
static void pick_accent(struct palette_workspace *work, const fb_data *pixels,
                        int total_pixels, int stride,
                        int dom_bucket, int32_t dom_rl, int dom_lum,
                        int32_t art_rl, int *out_r, int *out_g, int *out_b)
{
    const uint16_t *histogram = work->histogram;
    int i;

    int accent_bucket = -1;
    int hue_bucket = -1;
    unsigned int accent_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0 || i == dom_bucket)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;

        /* Approximate 8-bit values from 4-bit */
        int r8 = (r4 << 4) | r4;
        int g8 = (g4 << 4) | g4;
        int b8 = (b4 << 4) | b4;
        if (contrast_ratio(dom_rl, rel_luminance(r8, g8, b8)) >= MIN_RATIO)
        {
            unsigned int score = bucket_score(histogram, i);

            if (score > accent_score)
            {
                accent_score = score;
                accent_bucket = i;
            }
        }
    }

    unsigned int run_count = 0;

    if (accent_bucket < 0 || bucket_sector(accent_bucket) < 0)
    {
        hue_bucket = strongest_hue_bucket(work, dom_bucket, &run_count);
        if (accent_bucket >= 0 && run_count <= histogram[accent_bucket])
            hue_bucket = -1;
    }

    int acc_r, acc_g, acc_b;
    if (hue_bucket >= 0)
    {
        int dom_r, dom_g, dom_b;
        unsigned int fitted;

        average_bucket(pixels, total_pixels, stride, dom_bucket,
                       &dom_r, &dom_g, &dom_b);
        average_bucket(pixels, total_pixels, stride, hue_bucket,
                       &acc_r, &acc_g, &acc_b);

        /* A pastel darkened to read comes out a dull brown-grey, where the
         * eye expects the colour the pastel is a pale version of. Only where
         * that still reads, though: a saturated colour cannot get as light
         * or as dark, and one the fit gives up on for black or white is
         * worse than the paler hue it replaced. */
        unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
        unsigned int hued = LCD_RGBPACK(acc_r, acc_g, acc_b);
        unsigned int white = LCD_RGBPACK(255, 255, 255);
        unsigned int black = LCD_RGBPACK(0, 0, 0);
        int h, s, v;

        color_get_hsv(hued, &h, &s, &v);
        fitted = white;
        if (s < HUE_ACCENT_SAT)
            fitted = color_fit_contrast(color_from_hsv(h, HUE_ACCENT_SAT, v),
                                        dominant, MIN_RATIO);
        if (fitted == white || fitted == black)
            fitted = color_fit_contrast(hued, dominant, MIN_RATIO);

        /* A hue that cannot read at any brightness has lost to the accent
         * it was replacing, which is often an off-white or a near-black with
         * some warmth in it rather than the plain one the fit falls back to. */
        if ((fitted == white || fitted == black) && accent_bucket >= 0)
            hue_bucket = -1;
        acc_r = RGB_UNPACK_RED(fitted);
        acc_g = RGB_UNPACK_GREEN(fitted);
        acc_b = RGB_UNPACK_BLUE(fitted);

        if (compute_luminance(acc_r, acc_g, acc_b) < 128 &&
            contrast_ratio(art_rl, rel_luminance(acc_r, acc_g, acc_b))
                < ART_MIN_RATIO)
            hue_bucket = -1;
    }

    if (hue_bucket < 0 && accent_bucket >= 0)
    {
        average_bucket(pixels, total_pixels, stride, accent_bucket,
                       &acc_r, &acc_g, &acc_b);
    }
    else if (hue_bucket < 0)
    {
        /* Hard fallback: dark dominant -> white text, light -> black */
        acc_r = acc_g = acc_b = (dom_lum < 128) ? 255 : 0;
    }

    /* Readability enforcement. First try to keep the accent's hue by scaling
     * its channels toward a target luminance; then check the result, because
     * that scaling can fail silently -- each channel clamps at 255
     * independently, so a saturated one leaves the colour short of the target
     * and shifts its hue. If it is still short, legibility wins over hue. */
    if (contrast_ratio(dom_rl, rel_luminance(acc_r, acc_g, acc_b)) < MIN_RATIO)
    {
        int acc_lum = compute_luminance(acc_r, acc_g, acc_b);
        int target_lum = (dom_lum < 128) ? MIN(dom_lum + 128, 255)
                                         : MAX(dom_lum - 128, 0);

        if (acc_lum > 0)
        {
            int scale = (target_lum * 256) / acc_lum;
            acc_r = MIN((acc_r * scale) >> 8, 255);
            acc_g = MIN((acc_g * scale) >> 8, 255);
            acc_b = MIN((acc_b * scale) >> 8, 255);
        }
        else
        {
            acc_r = acc_g = acc_b = target_lum;
        }

        if (contrast_ratio(dom_rl, rel_luminance(acc_r, acc_g, acc_b)) < MIN_RATIO)
            acc_r = acc_g = acc_b = (dom_lum < 128) ? 255 : 0;
    }

    *out_r = acc_r;
    *out_g = acc_g;
    *out_b = acc_b;
}

/* The album's most colourful colour, for the `vivid` palette word: the bucket
 * scoring highest on count times OKLab chroma squared, among those at least
 * VIVID_CHROMA_MIN colourful whose hue -- the bucket's sector and both
 * neighbours, pooled as strongest_hue_bucket() pools them -- covers
 * 1/VIVID_SHARE of the picture. False when no bucket qualifies, as on a
 * black-and-white or sepia sleeve.
 *
 * Chroma, not HSV saturation: a dark brown is highly saturated in HSV, and
 * scored that way the wood and skin in a photograph win over the one red
 * jacket the eye goes to. */
#define VIVID_CHROMA_MIN 4588           /* 0.07 in color_chroma()'s units */
#define VIVID_SHARE      64

static bool pick_vivid(const uint16_t *histogram, const fb_data *pixels,
                       int total_pixels, int stride,
                       unsigned int *out)
{
    unsigned int sector_count[HUE_SECTORS] = { 0 };
    unsigned int samples = 0, best_score = 0;
    int best = -1, i, r, g, b;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        int sector = histogram[i] ? bucket_sector(i) : -1;

        samples += histogram[i];
        if (sector >= 0)
            sector_count[sector] += histogram[i];
    }

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        int sector = histogram[i] ? bucket_sector(i) : -1;
        unsigned int run, score;
        int chroma;

        if (sector < 0)
            continue;
        run = sector_count[(sector + HUE_SECTORS - 1) % HUE_SECTORS]
            + sector_count[sector]
            + sector_count[(sector + 1) % HUE_SECTORS];
        if (run * VIVID_SHARE < samples)
            continue;
        chroma = color_chroma(LCD_RGBPACK(((i >> 8) & 0xF) * 17,
                                          ((i >> 4) & 0xF) * 17,
                                          (i & 0xF) * 17));
        if (chroma < VIVID_CHROMA_MIN)
            continue;
        chroma >>= 6;     /* the square times a count of 16384 fits 32 bits */
        score = histogram[i] * (unsigned int)(chroma * chroma);
        if (score > best_score)
        {
            best_score = score;
            best = i;
        }
    }

    if (best < 0)
        return false;
    average_bucket(pixels, total_pixels, stride, best, &r, &g, &b);
    *out = LCD_RGBPACK(r, g, b);
    return true;
}

/* A palette as extracted, before anything is applied. */
struct found_palette {
    unsigned int accent;
    unsigned int dominant;
    unsigned int vivid;
    bool has_vivid;
};

static bool compute_palette(struct palette_workspace *work, const struct bitmap *bmp,
                            struct found_palette *out)
{
    if (!bmp->data || bmp->width <= 0 || bmp->height <= 0)
        return false;

    fb_data *pixels = (fb_data *)bmp->data;
    int width = bmp->width;
    int height = bmp->height;
    int total_pixels = width * height;
    int stride = MAX(total_pixels / SAMPLE_TARGET, 1);
    long sum_lum = 0;
    int nsamples = 0;
    int i;

    uint16_t *histogram = work->histogram;
    memset(histogram, 0, sizeof(work->histogram));

    /* Pass 1: build quantized histogram */
    for (i = 0; i < total_pixels; i += stride)
    {
        unsigned short px = (unsigned short)pixels[i];
        int r4 = (px >> 12) & 0xF;
        int g4 = (px >> 7) & 0xF;
        int b4 = (px >> 1) & 0xF;
        int bucket = (r4 << 8) | (g4 << 4) | b4;
        if (histogram[bucket] < UINT16_MAX)
            histogram[bucket]++;
        sum_lum += r4 * 77 + g4 * 150 + b4 * 29;
        nsamples++;
    }

    /* The picture's own mean luminance, on the 0..255 scale that
     * compute_luminance() uses. The running total weights 4-bit channels, and
     * 17 is what widens a 4-bit channel to 8. */
    int art_lum = nsamples ? (int)(((sum_lum / nsamples) * 17) >> 8) : 0;
    int32_t art_rl = rel_luminance(art_lum, art_lum, art_lum);

    /* Find dominant bucket (skip near-black/near-white, prefer saturated) */
    int best_bucket = -1;
    unsigned int best_score = 0;
    int fallback_bucket = -1;
    uint16_t fallback_count = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;

        /* Track unfiltered best as fallback */
        if (histogram[i] > fallback_count)
        {
            fallback_count = histogram[i];
            fallback_bucket = i;
        }

        /* Skip near-black and near-white */
        if (r4 < 2 && g4 < 2 && b4 < 2)
            continue;
        if (r4 > 13 && g4 > 13 && b4 > 13)
            continue;
        /* Skip very dark colors regardless of hue */
        int lum4 = (r4 * 5 + g4 * 9 + b4 * 2) >> 4;
        if (lum4 < 2)
            continue;

        unsigned int score = bucket_score(histogram, i);

        if (score > best_score)
        {
            best_score = score;
            best_bucket = i;
        }
    }

    if (best_bucket < 0)
        best_bucket = fallback_bucket;
    if (best_bucket < 0)
        return false; /* empty image? */

    /* Pass 2: average full-precision RGB for dominant bucket */
    int dom_r, dom_g, dom_b;

    average_bucket(pixels, total_pixels, stride, best_bucket,
                   &dom_r, &dom_g, &dom_b);
    unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
    int dom_lum = compute_luminance(dom_r, dom_g, dom_b);
    int32_t dom_rl = rel_luminance(dom_r, dom_g, dom_b);

    int acc_r, acc_g, acc_b;

    pick_accent(work, pixels, total_pixels, stride, best_bucket, dom_rl, dom_lum,
                art_rl, &acc_r, &acc_g, &acc_b);
    unsigned int accent = LCD_RGBPACK(acc_r, acc_g, acc_b);

    /* The dominant is not the only thing the accent is read against: a theme
     * that paints over the artwork puts the accent straight on the picture,
     * and the pair chosen above says nothing about that. The accent is picked
     * by count, so a cover that is mostly one flat expanse hands its own
     * background back as the text colour.
     *
     * Only a dark accent needs catching. A light one over light art is the
     * `scrim` filter's job -- it darkens the picture to make room -- but it
     * stands down below half scale, reading a dark accent as evidence that
     * the artwork is already light, which is what a dark expanse disproves.
     * The replacement settles for ACCENT_MIN_RATIO against the dominant
     * rather than MIN_RATIO, because nothing clears 6:1 above a mid-luminance
     * dominant: the pair that produced a dark accent is the pair with no
     * light answer.
     *
     * A dominant too bright for even white to clear that bar -- a light logo
     * on a black sleeve, since the dominant search skips near-black -- is
     * replaced instead, by the picture's most populous bucket no brighter than
     * its mean. Trap: searching the old dominant for a rescue there finds only
     * a mid-grey between it and the art, and a panel blended from the two
     * lands on that grey. */
    bool dark_unread = compute_luminance(acc_r, acc_g, acc_b) < 128 &&
        contrast_ratio(art_rl, rel_luminance(acc_r, acc_g, acc_b))
            < ART_MIN_RATIO;

    if (dark_unread && contrast_ratio(dom_rl, rel_luminance(255, 255, 255))
                           < ACCENT_MIN_RATIO)
    {
        int dark_bucket = -1;
        uint16_t dark_count = 0;

        for (i = 0; i < HISTOGRAM_BUCKETS; i++)
        {
            int r4 = (i >> 8) & 0xF;
            int g4 = (i >> 4) & 0xF;
            int b4 = i & 0xF;

            if (histogram[i] > dark_count &&
                compute_luminance(r4 * 17, g4 * 17, b4 * 17) <= art_lum)
            {
                dark_count = histogram[i];
                dark_bucket = i;
            }
        }

        if (dark_bucket >= 0)
        {
            average_bucket(pixels, total_pixels, stride, dark_bucket,
                           &dom_r, &dom_g, &dom_b);
            dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
            dom_lum = compute_luminance(dom_r, dom_g, dom_b);
            dom_rl = rel_luminance(dom_r, dom_g, dom_b);
            pick_accent(work, pixels, total_pixels, stride, dark_bucket, dom_rl,
                        dom_lum, art_rl, &acc_r, &acc_g, &acc_b);
            accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
        }
    }
    else if (dark_unread)
    {
        int rescue_bucket = -1;
        unsigned int rescue_score = 0;

        for (i = 0; i < HISTOGRAM_BUCKETS; i++)
        {
            if (histogram[i] == 0 || i == best_bucket)
                continue;

            int r4 = (i >> 8) & 0xF;
            int g4 = (i >> 4) & 0xF;
            int b4 = i & 0xF;
            int r8 = (r4 << 4) | r4;
            int g8 = (g4 << 4) | g4;
            int b8 = (b4 << 4) | b4;
            int32_t cand_rl = rel_luminance(r8, g8, b8);

            if (contrast_ratio(dom_rl, cand_rl) < ACCENT_MIN_RATIO)
                continue;
            if (contrast_ratio(art_rl, cand_rl) < ART_MIN_RATIO)
                continue;

            unsigned int score = bucket_score(histogram, i);

            if (score > rescue_score)
            {
                rescue_score = score;
                rescue_bucket = i;
            }
        }

        if (rescue_bucket >= 0)
        {
            average_bucket(pixels, total_pixels, stride, rescue_bucket,
                           &acc_r, &acc_g, &acc_b);
            accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
        }
        else if (contrast_ratio(art_rl, rel_luminance(255, 255, 255))
                     >= ART_MIN_RATIO)
        {
            accent = LCD_RGBPACK(255, 255, 255);
        }
    }

    out->accent = accent;
    out->dominant = dominant;
    out->has_vivid = pick_vivid(histogram, pixels, total_pixels, stride, &out->vivid);
    return true;
}

static struct found_palette current;
static bool palette_valid;

/* The album palette changes in one step, with a brief redraw window for
 * views that are waiting for playback or input events. */
static void apply_found(const struct found_palette *next, unsigned hash)
{
    bool valid = next != NULL;

    bool changed = valid != palette_valid ||
                   (valid && (next->dominant != current.dominant ||
                              next->accent != current.accent ||
                              next->has_vivid != current.has_vivid ||
                              (next->has_vivid && next->vivid != current.vivid)));
    if (changed)
    {
        ++palette_generation;
        change_tick = current_tick;
    }
    if (valid)
        current = *next;
    palette_valid = valid;
    global_status.resume_art_hash = hash;
}

/* Palettes worked out ahead of their track, while the disk was already
 * running, so the track change can apply one without reading anything. One
 * per upcoming track that may be from a new album, as on shuffle, up to the
 * number the prefetch looks ahead. A slot is used once, so a cover the
 * art pass replaces is read afresh next time. */
#define PREFETCH_SLOTS 8
static struct {
    unsigned int hash;
    struct found_palette p;
} prefetched[PREFETCH_SLOTS];
static int prefetch_next;
/* Folders the prefetch found no thumbnail for, keyed as palette_miss is, so
 * each upcoming track's absent file is opened once rather than at every track
 * start. Oldest goes first; a folder the art pass reaches since is still read
 * by the screen at its track change. */
#define PREFETCH_MISSES 16
static unsigned int prefetch_missed[PREFETCH_MISSES];
static int prefetch_miss_next;
static int palette_size_idx = -2;      /* -2 not looked up, -1 no such size */

/* The folder last found to have no cached thumbnail. Without this, a track
 * whose album is not in the cache reopens a file that is not there on every
 * render pass while the timeout below runs down; with it, once per track. */
static unsigned int palette_miss;

/* The cache key of a track's artist's portrait: the name the database files
 * it under, its album artist or else its artist */
static unsigned int id3_artist_key(const struct mp3entry *id3)
{
    return art_name_hash(id3->albumartist && id3->albumartist[0]
                         ? id3->albumartist : id3->artist);
}

/* The edge of the thumbnails the palette is taken from, or 0 if this build
 * caches no such size or caches it larger than the buffer above. */
static int palette_dim(void)
{
    int dim;

    if (palette_size_idx == -2)
        palette_size_idx = art_cache_size_index("coverflow");
    if (palette_size_idx < 0)
        return 0;

    dim = art_cache_size_dim(palette_size_idx);
    return (dim > 0 && dim <= PALETTE_MAX_DIM) ? dim : 0;
}

/* Read one folder's cached thumbnail and work out its palette. False means
 * there is none to read: no database yet, a folder the caching pass has not
 * reached, or one with no art at all. */
static bool read_folder_palette(unsigned int hash, int dim,
                                struct found_palette *out)
{
    struct art_cache_header hdr;
    struct bitmap bmp;
    size_t bytes;
    bool got = false;
    int fd;

    if (hash == 0 || workspace_handle <= 0)
        return false;

    /* Ask where the thumbnail would be and open it, rather than asking
     * whether it exists first -- the open is the existence test. A folder
     * with no art has no file here, which is what keeps the shared "no art"
     * placeholder from becoming somebody's palette. */
    mutex_lock(&palette_lock);
    struct palette_workspace *work = core_get_data_pinned(workspace_handle);
    art_cache_thumb_path(hash, palette_size_idx, work->cache_path, sizeof(work->cache_path));
    fd = open(work->cache_path, O_RDONLY);
    if (fd >= 0)
    {
        bytes = (size_t)dim * dim * sizeof(fb_data);
        if (read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
            hdr.magic == ART_CACHE_MAGIC &&
            hdr.version == ART_CACHE_FORMAT_VERSION &&
            hdr.width == dim && hdr.height == dim &&
            read(fd, work->pixels, bytes) == (ssize_t)bytes)
        {
            /* Pixel order is not checked because a histogram does not care:
             * this size is stored column-major for the carousel, and every
             * pixel is counted whichever way round they sit. */
            bmp.width = dim;
            bmp.height = dim;
            bmp.format = FORMAT_NATIVE;
            bmp.data = (unsigned char *)work->pixels;
            got = compute_palette(work, &bmp, out);
        }
        close(fd);
    }
    core_put_data_pinned(work);
    mutex_unlock(&palette_lock);
    return got;
}

static bool prefetched_find(unsigned int hash, struct found_palette *out)
{
    bool found = false;
    mutex_lock(&palette_lock);
    for (int i = 0; i < PREFETCH_SLOTS; i++)
    {
        if (prefetched[i].hash == hash)
        {
            if (out)
            {
                *out = prefetched[i].p;
                prefetched[i].hash = 0;
            }
            found = true;
            break;
        }
    }
    mutex_unlock(&palette_lock);
    return found;
}

#ifdef HAVE_USB_PODLINK
static bool remote_key_changed(const char key[65])
{
    static bool have_key;
    struct palette_workspace *work = core_get_data_pinned(workspace_handle);
    bool changed = !have_key || strcmp(work->remote_key, key);
    if (changed)
    {
        strlcpy(work->remote_key, key, sizeof(work->remote_key));
        have_key = true;
    }
    core_put_data_pinned(work);
    return changed;
}

/* P matches the unfiltered local 128px thumbnail used for extraction. */
static bool palette_from_remote(const char *key, struct found_palette *out)
{
    bool got = false;

    if (workspace_handle <= 0)
        return false;
    if (!cider_has_art(key, "P"))
    {
        cider_request_art(key, "P");
        return false;
    }

    mutex_lock(&palette_lock);
    struct palette_workspace *work = core_get_data_pinned(workspace_handle);
    struct bitmap bmp = {
        .width = PALETTE_MAX_DIM, .height = PALETTE_MAX_DIM,
        .format = FORMAT_NATIVE, .data = (unsigned char *)work->pixels,
    };
    got = cider_copy_art(key, "P", bmp.data, sizeof(work->pixels)) &&
          compute_palette(work, &bmp, out);
    core_put_data_pinned(work);
    mutex_unlock(&palette_lock);
    return got;
}
#endif

static int palette_candidates(const char *path, unsigned int artist,
                              unsigned int cand[2])
{
    int n = 0;

    if (global_settings.wps_art_source == WPS_ART_ARTIST)
        cand[n++] = artist;
    else
    {
        if (global_settings.wps_art_source == WPS_ART_AUTO &&
            playlist_is_from_artist())
        {
            cand[n++] = artist;
        }
        cand[n++] = art_cache_track_key(path);
    }
    return n;
}

static bool palette_from_cache(struct found_palette *out, unsigned *hash)
{
    unsigned int cand[2];
    char path[MAX_PATH];
    struct mp3entry *id3;
    unsigned int miss_key;
    int n, i, dim;

    dim = palette_dim();
    if (dim == 0)
        return false;

    /* audio_current_track() hands back the engine's live entry, not a copy,
     * so take what is needed out of it before doing anything else. */
    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return false;
    strmemccpy(path, id3->path, sizeof(path));
    n = palette_candidates(path, id3_artist_key(id3), cand);

    /* The negative cache is keyed on the album row whichever candidate was
     * tried, because that is the one key every setting can compute: it only has
     * to name this track's folder set, and it is read back before any of them are
     * opened. Set only when they all failed, or a fallback would be skipped the
     * moment the portrait was missing. */
    miss_key = art_cache_track_key(path);
    if (miss_key != 0 && miss_key == palette_miss)
        return false;

    /* A candidate whose palette is already the one in use is not read again:
     * by the time a track changes the disk is usually asleep, and the next
     * track of the same album would otherwise spin it up for nothing. */
    for (i = 0; i < n; i++)
    {

        if (cand[i] == 0)
            continue;
        if (palette_valid && cand[i] == global_status.resume_art_hash)
            *out = current;
        else if (!prefetched_find(cand[i], out) &&
                 !read_folder_palette(cand[i], dim, out))
            continue;
        *hash = cand[i];
        return true;
    }

    palette_miss = miss_key;
    return false;
}

/* A track about to play: work out its palette now if the disk is running,
 * rather than at the track change -- with the screen dark that read would land
 * on the first render after it wakes and hold the screen up for a spin-up.
 * Runs on the audio and background threads, so a disk that is not running is left
 * alone: a spin-up here stalls playback's queue. The first candidate that
 * reads is the one palette_from_cache() would settle on too. */
static void prefetch_path(const char *track_path, unsigned int artist)
{
    /* Palette resolution can yield. Each caller owns its path while the
     * audio and background threads prefetch concurrently. */
    char path[MAX_PATH];
    struct found_palette p;
    unsigned int cand[2];
    unsigned int miss_key;
    int n, i, dim;

    if (audio_is_unrecorded() ||
        !track_path[0] || !storage_disk_is_active())
        return;
    dim = palette_dim();
    if (dim == 0)
        return;

    strmemccpy(path, track_path, sizeof(path));
    n = palette_candidates(path, artist, cand);
    miss_key = art_cache_track_key(path);
    bool already_missed = false;
    mutex_lock(&palette_lock);
    for (i = 0; miss_key != 0 && i < PREFETCH_MISSES; i++)
        if (prefetch_missed[i] == miss_key)
            already_missed = true;
    mutex_unlock(&palette_lock);
    if (already_missed)
        return;
    for (i = 0; i < n; i++)
    {
        if (cand[i] == 0)
            continue;
        if (cand[i] == global_status.resume_art_hash ||
            prefetched_find(cand[i], NULL))
            return;
        if (read_folder_palette(cand[i], dim, &p))
        {
            mutex_lock(&palette_lock);
            prefetched[prefetch_next].hash = cand[i];
            prefetched[prefetch_next].p = p;
            prefetch_next = (prefetch_next + 1) % PREFETCH_SLOTS;
            mutex_unlock(&palette_lock);
            return;
        }
    }
    mutex_lock(&palette_lock);
    prefetch_missed[prefetch_miss_next] = miss_key;
    prefetch_miss_next = (prefetch_miss_next + 1) % PREFETCH_MISSES;
    mutex_unlock(&palette_lock);
}

/* Asked when the next track's metadata loads, which is buffering, and when
 * its decoding starts. Decoding start also covers the tracks after it by
 * their playlist paths, buffered or not: a skip lands on one of those, and
 * hi-res files leave little of it buffered. The playlist can read its file,
 * so only while the disk runs. Trap: the starting track takes a slot too, so
 * looking a full PREFETCH_SLOTS ahead evicts the palette about to be shown. */
static void prefetch_cb(unsigned short id, void *param)
{
    struct mp3entry *id3 = ((struct track_event *)param)->id3;
    static char path[MAX_PATH]; /* only the audio thread asks */
    const char *p;

    if (id3)
        prefetch_path(id3->path, id3_artist_key(id3));
    if (id != PLAYBACK_EVENT_TRACK_BUFFER
        || palette_dim() == 0 || !storage_disk_is_active())
        return;
    for (int offset = 1; offset < PREFETCH_SLOTS; offset++)
    {
        p = playlist_peek(offset, path, sizeof(path));
        if (!p)
            break;
        prefetch_path(p, 0);    /* a path names no artist: no portrait */
    }
}

/* Best-effort idle prefetch runs on bgtask's decoder-sized stack. Palette
 * lookup and thumbnail validation stay off the storage stack. Recheck
 * the disk and USB here: the worker may not run until after ATA has slept
 * or a host has requested the disk. Never spin it up for palette prefetch. */
static void prefetch_idle_work(const struct queue_event *ev)
{
    (void)ev;
    if (usb_host_wants_storage() ||
        !storage_disk_is_active() || workspace_handle <= 0 || palette_dim() == 0)
        return;
    struct palette_workspace *work = core_get_data_pinned(workspace_handle);
    for (int offset = 1; offset < PREFETCH_SLOTS; offset++)
    {
        struct mp3entry *id3 = &work->idle_track;
        if (usb_host_wants_storage() || !storage_disk_is_active() ||
            !audio_peek_buffered_track(id3, offset))
            break;
        prefetch_path(id3->path, id3_artist_key(id3));
    }
    core_put_data_pinned(work);
}

/* Event-only task: no periodic tick, registration, allocation or new thread. */
#define PALETTE_EVENT_IDLE 3
static struct bg_task palette_idle_task = {
    .name = "palette prefetch",
    .handle_event = prefetch_idle_work,
};

/* Storage thread: post only. Do not inspect tracks, resolve owners, validate
 * thumbnails, wait for the worker or perform any filesystem I/O here. */
static void prefetch_idle_cb(unsigned short id, void *param)
{
    (void)id;
    (void)param;
    if (!bg_task_queued(&palette_idle_task, PALETTE_EVENT_IDLE))
        bg_task_post(&palette_idle_task, PALETTE_EVENT_IDLE);
}
static void track_changed(unsigned short id, void *data)
{
    (void)id;
    (void)data;
    if (!audio_is_unrecorded())
        ++track_serial;
}

void album_palette_init(void)
{
    mutex_init(&palette_lock);
    if (workspace_handle <= 0)
        return;

    add_event(PLAYBACK_EVENT_TRACK_BUFFER, prefetch_cb);
    add_event(PLAYBACK_EVENT_NEXTTRACKID3_AVAILABLE, prefetch_cb);
    add_event(DISK_EVENT_SPINUP, prefetch_idle_cb);
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
    add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_changed);
}

void album_palette_seed_resume(void)
{
    int dim = palette_dim();
    unsigned hash = global_status.resume_art_hash;
    struct found_palette next;

    if (dim > 0 && hash && global_status.resume_index != -1 &&
        read_folder_palette(hash, dim, &next))
    {
        apply_found(&next, hash);
        change_tick = current_tick - PALETTE_REPAINT_WINDOW;
    }
}

int album_art_current(void)
{
    int slot = native_wps_art_slot();
#ifdef HAVE_USB_PODLINK
    if (cider_active())
        return wps_art_handle(slot);
#endif
    return slot >= 0 ? wps_art_handle(slot) : -1;
}

unsigned album_art_generation(void)
{
    return art_generation;
}

void album_art_opened(int handle)
{
    if (handle > 0)
    {
        int level = disable_irq_save();
        ++art_generation;
        restore_irq(level);
    }
}

static bool palette_from_bitmap(int handle, struct found_palette *out)
{
    if (handle < 0 || !wps_art_pin(handle, true))
        return false;

    mutex_lock(&palette_lock);
    struct palette_workspace *work = core_get_data_pinned(workspace_handle);
    struct bitmap *bitmap;
    bool got = wps_art_bitmap(handle, &bitmap) && compute_palette(work, bitmap, out);
    core_put_data_pinned(work);
    mutex_unlock(&palette_lock);
    wps_art_pin(handle, false);
    return got;
}

void album_palette_refresh(void)
{
    if (workspace_handle <= 0)
        return;

    unsigned serial = track_serial;
    unsigned artwork = art_generation;
    bool remote = cider_active();
#ifdef HAVE_USB_PODLINK
    static bool was_remote, had_remote_palette;
    static unsigned observed_remote_generation;
    bool remote_palette = remote && cider_palette_supported();
    unsigned generation = remote && !remote_palette ? cider_wps_art_generation() : 0;
    char key[65] = "";
    if (remote_palette)
        cider_current_art(key);
    bool key_changed = remote_palette && remote_key_changed(key);
    if (remote != was_remote || remote_palette != had_remote_palette ||
        generation != observed_remote_generation || key_changed)
    {
        was_remote = remote;
        had_remote_palette = remote_palette;
        observed_remote_generation = generation;
        request_tick = current_tick;
        needs_extraction = true;
    }
#endif

    if (serial != observed_track_serial)
    {
        observed_track_serial = serial;
        palette_miss = 0;
        request_tick = current_tick;
        needs_extraction = true;
    }

    /* The audio thread announces speculative loads as well. Once a palette
     * is settled, opening those buffers must not restart extraction. */
    if (artwork != observed_art_generation)
    {
        observed_art_generation = artwork;
        if (!palette_valid && !remote)
            needs_extraction = true;
    }
    if (!needs_extraction)
        return;

    struct found_palette next;
    unsigned hash = 0;
    bool got;

#ifdef HAVE_USB_PODLINK
    if (remote_palette)
        got = *key && palette_from_remote(key, &next);
    else
#endif
        got = (!remote && palette_from_cache(&next, &hash)) ||
              palette_from_bitmap(album_art_current(), &next);

    /* Thumbnail I/O can yield across a track or source change. Do not
     * publish the resulting colors for the wrong source. */
    if (track_serial != serial || cider_active() != remote)
        return;
#ifdef HAVE_USB_PODLINK
    if (got && remote_palette)
    {
        char current_key[65];
        cider_current_art(current_key);
        if (strcmp(key, current_key))
            return;
    }
#endif
    if (got)
    {
        apply_found(&next, hash);
        needs_extraction = false;
    }
    else if (TIME_AFTER(current_tick, request_tick + PALETTE_EMBEDDED_WAIT))
    {
        if (remote || audio_status())
            apply_found(NULL, 0);
#ifdef HAVE_USB_PODLINK
        /* Continue checking the requested P tier on normal screen polls. */
        needs_extraction = remote_palette && *key;
#else
        needs_extraction = false;
#endif
    }
}

static uint32_t rgb888(unsigned packed)
{
    return (uint32_t)RGB_UNPACK_RED(packed) << 16 |
           (uint32_t)RGB_UNPACK_GREEN(packed) << 8 |
           (uint32_t)RGB_UNPACK_BLUE(packed);
}

bool album_palette_snapshot(struct ui_album_palette *out)
{
    *out = (struct ui_album_palette){0};
    if (!palette_valid)
        return false;

    out->dominant = rgb888(current.dominant);
    out->accent = rgb888(current.accent);
    out->vivid = current.has_vivid ? rgb888(current.vivid) : 0;
    out->has_dominant = out->has_accent = true;
    out->has_vivid = current.has_vivid;
    return true;
}

unsigned album_palette_generation(void)
{
    return palette_generation;
}

bool album_palette_needs_repaint(void)
{
    return palette_generation &&
           TIME_BEFORE(current_tick, change_tick + PALETTE_REPAINT_WINDOW);
}

bool album_palette_pending(void)
{
    return track_serial != observed_track_serial ||
           (needs_extraction &&
            !TIME_AFTER(current_tick, request_tick + PALETTE_EMBEDDED_WAIT));
}

bool album_art_render_background(fb_data *out, int width, int height, int handle,
                                 bool blur, unsigned text_color)
{
    if (!out || width <= 0 || height <= 0 || width > LCD_WIDTH ||
        height > LCD_HEIGHT || workspace_handle <= 0 || handle < 0 ||
        !wps_art_pin(handle, true))
        return false;

    mutex_lock(&palette_lock);
    struct palette_workspace *work = core_get_data_pinned(workspace_handle);
    struct bitmap *bitmap;
    struct img_filter filter;
    bool rendered = false;

    if (wps_art_bitmap(handle, &bitmap) && bitmap->data &&
        bitmap->width > 0 && bitmap->height > 0 &&
        img_filter_compile(blur ? "blur12+scrim+dither" : "scrim+dither",
                           IMG_CLASSES_ALL, &filter))
    {
        const fb_data *pixels = (const fb_data *)bitmap->data;
        int sw = bitmap->width, sh = bitmap->height;
        int crop_w = sw, crop_h = sh;
        if ((int64_t)sw * height > (int64_t)sh * width)
            crop_w = MAX(1, (int)((int64_t)sh * width / height));
        else
            crop_h = MAX(1, (int)((int64_t)sw * height / width));

        struct img_levels levels;
        img_filter_measure(pixels, sw, sh, &levels);
        img_filter_adapt(&filter, &levels, color_luminance(text_color));
        pixels += (sh - crop_h) / 2 * sw + (sw - crop_w) / 2;
        rendered = img_filter_render(out, width, height, pixels,
                                      crop_w, crop_h, sw, &filter,
                                      work->pixels, ARRAYLEN(work->pixels));
    }

    core_put_data_pinned(work);
    mutex_unlock(&palette_lock);
    wps_art_pin(handle, false);
    return rendered;
}
