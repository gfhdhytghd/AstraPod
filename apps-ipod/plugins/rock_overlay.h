/* Foreground overlay handoff. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_OVERLAY_H
#define ASTRAPOD_ROCK_OVERLAY_H

#include <stdbool.h>
#include "fs_defines.h"
#include "rbpaths.h"

#define ROCK_OVERLAY_MAGIC 0x4153544fUL
#define ROCK_OVERLAY_DIR PLUGIN_DIR "/overlays"
#define ROCK_OVERLAY_SUFFIX ".ovl"

struct rock_overlay_launch {
    char path[MAX_PATH];
    char parameter[MAX_PATH];
    bool has_parameter;
};

void rock_overlay_begin(void);
int rock_overlay_request(const char *name, const char *parameter);
bool rock_overlay_take(struct rock_overlay_launch *launch);
void rock_overlay_end(void);

#endif
