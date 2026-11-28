/* AstraPod: the stock movie overlay of Classic and Fruit Company.
 * GPL version 2 or later. */
#ifndef ASTRAPOD_VIDEO_STOCK_OSD_H
#define ASTRAPOD_VIDEO_STOCK_OSD_H

#include <stdbool.h>
#include <stdint.h>

enum video_stock_mode {
    VIDEO_STOCK_HIDDEN,
    VIDEO_STOCK_PROGRESS,
    VIDEO_STOCK_SCRUB,
    VIDEO_STOCK_VOLUME,
    VIDEO_STOCK_BRIGHTNESS,
    VIDEO_STOCK_PICTURE,
};

/* What the overlay shows for one frame. */
struct video_stock_state {
    const char *title;
    int picture;            /* PICTURE: enum video_picture_mode */
    int picture_pull;       /* PICTURE: wheel events towards the next */
    uint32_t position_ms;   /* SCRUB: where the marker stands */
    uint32_t duration_ms;
    int level, level_max;   /* VOLUME and BRIGHTNESS */
    unsigned char mode;     /* enum video_stock_mode */
    bool paused;            /* shows the top band while the bottom is hidden */
};

struct h264_lcd_timing;

struct video_stock_osd {
    /* Present the 320x240 YUV 4:2:0 picture in `planes`, stride 320, with
     * the overlay over it. Returns true while a band is sliding, when the
     * caller must present again soon even with the picture still. */
    bool (*present)(unsigned char *const planes[3],
                    const struct video_stock_state *state,
                    struct h264_lcd_timing *timing);
    /* Step the backlight brightness and return its level, 0 to
     * brightness_max; a step of zero reads it. */
    int (*brightness)(int step);
    int brightness_max;
};

#ifndef PLUGIN
extern const struct video_stock_osd video_stock_osd;
/* Free what the overlay holds and save a changed brightness. */
void video_stock_osd_release(void);
#endif

#endif
