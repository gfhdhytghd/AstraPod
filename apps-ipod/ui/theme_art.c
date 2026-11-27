#include "theme_art.h"
#include "system.h"
#include "plastic_columns.h"
#include "kernel.h"
#include "powermgmt.h"
#include "system/battery_calibration.h"

bool ui_theme_art(const struct ui_theme *theme)
{
    return theme && theme->style[UI_STYLE_ART] == UI_ART_PLASTIC;
}

unsigned ui_art_battery_index(int percent)
{
    static unsigned frame;
    static unsigned long last_drop;

    percent = MIN(MAX(percent, 0), 100);
#if defined(IPOD_6G) && !defined(SIMULATOR)
    unsigned level = battery_calibration_icon_level(percent);
#else
    unsigned level = percent * 15 / 100;
#endif
    unsigned target = (level * 256 / 15) * 22 / 256;
    unsigned long now = current_tick;
    if (target >= frame)
        frame = target;
    else if (now - last_drop >= POWER_THREAD_STEP_TICKS)
    {
        frame--;
        last_drop = now;
    }
    return frame;
}

enum stock_art_id ui_art_battery(unsigned frame)
{
    static const enum stock_art_id level[] = {
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_0,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_1,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_2,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_3,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_4,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_5,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_6,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_7,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_8,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_9,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_10,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_11,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_12,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_13,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_14,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_15,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_16,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_17,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_18,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_19,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_20,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_21,
        STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_22,
    };
    return level[frame];
}

void ui_art_tile(enum stock_art_id id, int x, int y, int width, bool flush,
                 struct ui_rect clip)
{
    int tile = stock_art[id].width;
    int end = x + width;
    int right = MIN(clip.x + clip.width, end);
    clip.x = MAX(clip.x, x);
    if (flush && width >= tile)
    {
        clip.width = right - clip.x;
        stock_art_draw(id, end - tile, y, clip);
        end -= tile;
        right = MIN(right, end);
    }
    /* The tiles left of a flush one stop where it starts: its reflection
     * rows are translucent and must not be laid twice. */
    clip.width = right - clip.x;
    for (; x < end && clip.width > 0; x += tile)
        stock_art_draw(id, x, y, clip);
}

const uint32_t *ui_art_selection(int height)
{
    switch (height)
    {
    case ARRAYLEN(plastic_selection):
        return plastic_selection;
    case ARRAYLEN(plastic_selection_24):
        return plastic_selection_24;
    case ARRAYLEN(plastic_selection_44):
        return plastic_selection_44;
    }
    return NULL;
}
