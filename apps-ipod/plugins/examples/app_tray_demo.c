/* AstraPod app reminder demo. GPL version 2 or later. */
#include "plugin.h"

static const struct rock_tray_bitmap frames[] = {
    {{0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00}},
    {{0x00, 0x3c, 0x42, 0x5a, 0x5a, 0x42, 0x3c, 0x00}},
    {{0x00, 0x3c, 0x7e, 0x7e, 0x7e, 0x7e, 0x3c, 0x00}},
};

static const struct rock_tray_rule schedule[] = {
    {60, 1}, {120, 2}, {180, ROCK_TRAY_HIDDEN},
};

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    int frame = rb->app_tray_get();
    if (frame >= 0 && frame != ROCK_TRAY_HIDDEN)
    {
        if (!rb->app_tray_register(NULL, 0))
            goto error;
        rb->splash(HZ, "Tray demo cleared");
        return PLUGIN_OK;
    }

    if (!rb->app_tray_register(frames, ARRAYLEN(frames)) ||
        !rb->app_tray_set(0))
        goto error;
    if (!rb->app_background_schedule(schedule, ARRAYLEN(schedule)))
    {
        rb->app_tray_register(NULL, 0);
        goto error;
    }
    rb->splash(HZ, "Tray demo started");
    return PLUGIN_OK;

error:
    rb->splash(HZ, "Tray demo failed");
    return PLUGIN_ERROR;
}
