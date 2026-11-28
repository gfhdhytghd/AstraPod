/* AstraPod: the movie player's side of the stock overlay, for Classic and
 * Fruit Company. stage-h264.py includes it in video_playback.c after the
 * overlay's types. GPL version 2 or later. */
#ifndef ASTRAPOD_VIDEO_STOCK_INPUT_H
#define ASTRAPOD_VIDEO_STOCK_INPUT_H

#include "plugins/video_stock_osd.h"

/* A band was sliding at the last present: present again while paused. */
static bool video_stock_moving;

/* SELECT steps Progress, Scrub, Brightness and Picture, then hides. SELECT
 * after a scrub commits it and hides, and SELECT from Volume hides. */
static enum video_input_action video_stock_select(struct video_osd *osd)
{
    enum video_overlay mode = VIDEO_OVERLAY_HIDDEN;
    switch (osd->mode)
    {
    case VIDEO_OVERLAY_HIDDEN:
        mode = VIDEO_OVERLAY_PROGRESS;
        break;
    case VIDEO_OVERLAY_PROGRESS:
        mode = VIDEO_OVERLAY_SCRUB;
        break;
    case VIDEO_OVERLAY_SCRUB:
        if (!osd->pending_seek)
            mode = VIDEO_OVERLAY_BRIGHTNESS;
        break;
    case VIDEO_OVERLAY_BRIGHTNESS:
        if (osd->options->picture_mode)
            mode = VIDEO_OVERLAY_PICTURE;
        break;
    default:
        break;
    }
    video_osd_show(osd, mode);
    return osd->pending_seek ? VIDEO_INPUT_SEEK : VIDEO_INPUT_NONE;
}

static void video_stock_present(unsigned char *const planes[3], bool paused,
                                uint32_t position_ms, uint32_t duration_ms,
                                const struct video_osd *osd,
                                struct h264_lcd_timing *timing)
{
    const struct video_stock_osd *stock = osd->options->stock_osd;
    static const unsigned char modes[] = {
        [VIDEO_OVERLAY_HIDDEN] = VIDEO_STOCK_HIDDEN,
        [VIDEO_OVERLAY_PROGRESS] = VIDEO_STOCK_PROGRESS,
        [VIDEO_OVERLAY_SCRUB] = VIDEO_STOCK_SCRUB,
        [VIDEO_OVERLAY_VOLUME] = VIDEO_STOCK_VOLUME,
        [VIDEO_OVERLAY_BRIGHTNESS] = VIDEO_STOCK_BRIGHTNESS,
        [VIDEO_OVERLAY_PICTURE] = VIDEO_STOCK_PICTURE,
    };
    struct video_stock_state state = {
        .title = osd->title,
        .picture = video_picture_label(osd),
        .position_ms = osd->pending_seek ? osd->seek_ms : position_ms,
        .duration_ms = duration_ms,
        .mode = modes[osd->mode],
        .paused = paused,
    };
    if (osd->mode == VIDEO_OVERLAY_VOLUME)
    {
        int minimum = rb->sound_min(SOUND_VOLUME);
        state.level = rb->global_status->volume - minimum;
        state.level_max = rb->sound_max(SOUND_VOLUME) - minimum;
    }
    else if (osd->mode == VIDEO_OVERLAY_BRIGHTNESS)
    {
        state.level = stock->brightness(0);
        state.level_max = stock->brightness_max;
    }
    video_stock_moving = stock->present(planes, &state, timing);
}

#endif
