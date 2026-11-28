/* AstraPod movie controls for Classic and Fruit Company. GPL version 2+. */
#ifndef ASTRAPOD_VIDEO_CONTROLS_H
#define ASTRAPOD_VIDEO_CONTROLS_H

static void video_draw_compact_header(uint8_t * const *planes, bool paused,
                                      const struct video_osd *osd)
{
    const int left = 10, right = LCD_WIDTH - left;
    const bool visible = osd->mode != VIDEO_OVERLAY_HIDDEN;
    if (!visible && !paused)
        return;

    int width = visible ? MIN(228, video_yuv_text_width(osd->title) + 34) : 22;
    video_yuv_rect(planes, left, 8, width, 18, 20, 28, 34);
    video_draw_status_icon(planes, left + 7, 12, paused);
    if (!visible)
        return;

    video_yuv_text(planes, left + 23, 13, left + width - 8, osd->title,
                   248, 250, 252);
    int battery = osd->options->battery_level ?
                  osd->options->battery_level() : -1;
    if (battery < 0)
        return;

    video_yuv_rect(planes, right - 30, 8, 30, 18, 20, 28, 34);
    video_yuv_rect(planes, right - 24, 13, 16, 8, 150, 163, 175);
    video_yuv_rect(planes, right - 8, 15, 2, 4, 150, 163, 175);
    video_yuv_rect(planes, right - 23, 14, 14, 6, 20, 28, 34);
    video_yuv_rect(planes, right - 23, 14, MIN(battery, 100) * 14 / 100, 6,
                   248, 250, 252);
}

static void video_draw_compact_controls(uint8_t * const *planes, bool paused,
                                        uint32_t position, uint32_t duration,
                                        const struct video_osd *osd)
{
    const int left = 10, right = LCD_WIDTH - left;
    const int top = LCD_HEIGHT - 28, height = 20;
    const int text_y = top + 6, mid_y = top + height / 2;
    unsigned accent = osd->options->accent;
    int red = 63 + ((accent >> 11) & 31) * 192 / 31;
    int green = 63 + ((accent >> 5) & 63) * 192 / 63;
    int blue = 63 + (accent & 31) * 192 / 31;

    video_draw_compact_header(planes, paused, osd);

    switch (osd->mode)
    {
    case VIDEO_OVERLAY_HIDDEN:
        return;
    case VIDEO_OVERLAY_VOLUME:
    {
        const int width = 144, meter = left + 54;
        int min = rb->sound_min(SOUND_VOLUME);
        int max = osd->options->max_volume;
        int volume = MAX(min, MIN(max, rb->global_status->volume));
        int fill = max > min ? (volume - min) * 10 / (max - min) : 0;

        video_yuv_rect(planes, left, top, width, height, 20, 28, 34);
        video_yuv_text(planes, left + 8, text_y, meter, "Volume",
                       248, 250, 252);
        for (int i = 0; i < 10; i++)
        {
            int x = meter + i * 8;
            if (i < fill)
                video_yuv_rect(planes, x, mid_y - 3, 6, 6, red, green, blue);
            else
                video_yuv_rect(planes, x, mid_y - 3, 6, 6, 86, 101, 112);
        }
        return;
    }
    case VIDEO_OVERLAY_PICTURE:
    {
        const char *label = video_picture_label(osd);
        int width = video_yuv_text_width(label) + 26;
        int x = right - width;

        video_yuv_rect(planes, x, top, width, height, 20, 28, 34);
        video_yuv_rect(planes, x + 6, mid_y - 4, 4, 8, red, green, blue);
        video_yuv_text(planes, x + 16, text_y, right - 8, label,
                       248, 250, 252);
        return;
    }
    case VIDEO_OVERLAY_PROGRESS:
    case VIDEO_OVERLAY_SCRUB:
        break;
    }

    char elapsed[20], remaining[20];
    position = MIN(osd->pending_seek ? osd->seek_ms : position, duration);
    video_time_text(position, elapsed, sizeof(elapsed));
    remaining[0] = '-';
    video_time_text(duration, remaining + 1, sizeof(remaining) - 1);
    const int time_left = left + 8;
    int rail_left = time_left + video_yuv_text_width(remaining + 1) + 10;
    int rail_right = right - 18 - video_yuv_text_width(remaining);
    video_time_text(duration - position, remaining + 1, sizeof(remaining) - 1);
    int time_right = right - 8 - video_yuv_text_width(remaining);
    int rail_width = rail_right - rail_left;
    int fill = duration ? (uint64_t)position * rail_width / duration : 0;
    int thumb = rail_left + fill;
    bool scrub = osd->mode == VIDEO_OVERLAY_SCRUB;

    video_yuv_rect(planes, left, top, right - left, height, 20, 28, 34);
    video_yuv_text(planes, time_left, text_y, rail_left, elapsed,
                   248, 250, 252);
    video_yuv_text(planes, time_right, text_y, right, remaining,
                   248, 250, 252);
    video_yuv_rect(planes, rail_left, mid_y - 1, rail_width, 2, 86, 101, 112);
    video_yuv_rect(planes, rail_left, mid_y - 1, fill, 2, red, green, blue);
    video_yuv_rect(planes, thumb - 2, mid_y - (scrub ? 5 : 3),
                   4, scrub ? 10 : 6, 248, 250, 252);
}

#endif
