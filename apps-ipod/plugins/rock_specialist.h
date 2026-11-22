/* Specialist plugin services. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_SPECIALIST_H
#define ASTRAPOD_ROCK_SPECIALIST_H

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "fs_defines.h"

/* >0 selected, 0 canceled, <0 error or storage takeover. */
int rock_pick_file(char *path, size_t size, const char *root, const char *title,
                   const char *extensions, unsigned int attr, bool hide_dirs);
int rock_properties(const char *path);
int rock_main_menu_config(void);
int rock_remove_theme_file(const char *path);

enum rock_handoff_kind {
    ROCK_HANDOFF_PLUGIN,
    ROCK_HANDOFF_IMAGE,
    ROCK_HANDOFF_TEXT,
    ROCK_HANDOFF_BROWSE,
};
struct rock_handoff {
    enum rock_handoff_kind kind;
    char module[MAX_PATH];
    char path[MAX_PATH];
};
int rock_queue_handoff(const char *module, const char *path);
int rock_queue_native_handoff(enum rock_handoff_kind kind, const char *path);
const struct rock_handoff *rock_take_handoff(void);
void rock_clear_handoff(void);

#ifdef HAVE_PITCHCONTROL
void rock_pitch_preview_init(void);
int rock_pitch_preview(void);
void rock_pitch_preview_revoke(void);
#else
static inline void rock_pitch_preview_init(void) {}
static inline void rock_pitch_preview_revoke(void) {}
#endif

#endif
