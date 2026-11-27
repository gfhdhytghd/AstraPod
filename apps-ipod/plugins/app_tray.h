/* AstraPod app reminder store and UI snapshot. GPL version 2 or later. */
#ifndef ASTRAPOD_APP_TRAY_H
#define ASTRAPOD_APP_TRAY_H

#include <stdbool.h>
#include <time.h>
#include "plugins/sdk/rock_tray.h"

#define APP_TRAY_VISIBLE 2

/* No padding, so a view compares with memcmp(). */
struct app_tray_view {
    struct rock_tray_icon12 icons[APP_TRAY_VISIBLE];
    uint16_t count;
};

void app_tray_init(void);
void app_tray_snapshot(const struct tm *now, struct app_tray_view *view);

/* The loader serializes writes on the foreground module owner thread. */
bool app_tray_register(const char *path, const struct rock_tray_icon12 *frames,
                       unsigned count);
/* The same for 8 x 8 frames, kept centred in 12 x 12. */
bool app_tray_register8(const char *path, const struct rock_tray_bitmap *frames,
                        unsigned count);
bool app_tray_set(const char *path, unsigned frame);
int app_tray_get(const char *path, const struct tm *now);
bool app_background_schedule(const char *path,
                             const struct rock_tray_rule *rules,
                             unsigned count, const struct tm *now);

#endif
