/* AstraPod foreground plugin integration. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_COMPAT_H
#define ASTRAPOD_ROCK_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#ifdef ASTRAPOD_ROCK_COMPAT
int rock_browser(void *unused);
/* The selected input path is borrowed until return. NULL starts the module. */
int rock_open_from_browser(const char *path, const char *file);
/* Like rock_open_from_browser(), without Rockbox's error splash; sets *failed
 * when the module could not be loaded or returned PLUGIN_ERROR. */
int rock_open_from_browser_quiet(const char *path, const char *file,
                                 bool *failed);

enum rock_video_result {
    ROCK_VIDEO_FAILED = -1,
    ROCK_VIDEO_FINISHED,
    ROCK_VIDEO_EXIT,
    ROCK_VIDEO_USB,
    ROCK_VIDEO_PREVIOUS,
    ROCK_VIDEO_NEXT,
};

/* Appearance flags above the RGB565 accent. */
#define ROCK_VIDEO_COMPACT_CONTROLS (1u << 16)

/* Borrowed for one video invocation. */
struct rock_video_request {
    const char *path;
    const char *title;
    int (*battery_level)(void);
    const uint32_t *chapters;
    size_t chapter_count;
    bool has_previous;
    bool probe;
    int result;
    bool themed_controls;
    unsigned accent;
    int (*resume_menu)(void);
    bool (*control_centre)(void); /* False on USB takeover. */
    int (*picture_mode)(int step); /* Zero reads without changing the mode. */
};
#ifdef HAVE_HW_H264
int rock_probe_video(const char *path);
#endif
int rock_open_video(const char *path, const char *title, bool has_previous);
int rock_run_from_shell(const char *path);
bool rock_ui_raw(void);
bool rock_ui_native(void);
bool rock_usb_taken(void);
void rock_usb_takeover(void);
void rock_media_shutdown(void);
void rock_playback_takeover(void);
void rock_action_reset(void);
void rock_action_handoff_wait_for_release(void);
const unsigned char *rock_widget_text(const unsigned char *text);
#else
static inline void rock_usb_takeover(void) {}
static inline void rock_media_shutdown(void) {}
static inline void rock_playback_takeover(void) {}
static inline bool rock_ui_raw(void) { return false; }
static inline bool rock_ui_native(void) { return false; }
#endif

#endif
