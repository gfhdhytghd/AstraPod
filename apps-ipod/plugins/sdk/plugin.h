/* AstraPod plugin SDK. GNU General Public License, version 2 or later. */
#ifndef ASTRAPOD_PLUGIN_H
#define ASTRAPOD_PLUGIN_H

#ifdef PLUGIN
#define NO_REDEFINES_PLEASE
#endif

#include "config.h"

#ifdef PLUGIN
/* The phase 1 arena is in DRAM. Codec IRAM remains available to playback. */
#undef USE_IRAM
#undef ICODE_ATTR
#undef IDATA_ATTR
#undef ICONST_ATTR
#undef IBSS_ATTR
#define ICODE_ATTR
#define IDATA_ATTR
#define ICONST_ATTR
#define IBSS_ATTR
#endif

#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "audio/beep.h"
#include "rbunicode.h"
#include "file.h"
#include "dir.h"
#include "load_code.h"
#include "lcd.h"
#include "font.h"
#include "audio.h"
#include "playback.h"
#include "playlist.h"
#include "action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "settings.h"
#include "settings/settings_list.h"
#include "speech/talk.h"
#include "system/appevents.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "powermgmt.h"
#include "timefuncs.h"
#include "lang_enum.h"
#include "rbpaths.h"
#include "ui/statusbar.h"
#include "widgets/menu.h"
#include "widgets/keyboard.h"
#include "widgets/list.h"
#include "widgets/yesno.h"
#include "widgets/option_select.h"

#if __has_include("rock_compat_tables.h")
#include "rock_compat_tables.h"
#include "rock_compat_input.h"
#define ROCK_SETTINGS_TYPE struct rock_compat_settings
#define ROCK_COMPAT_GENERATED 1
#else
/* Phase 1 fallback until the generated tables are present. The enabled
 * consumers only access native settings members and language IDs. */
#define ROCK_SETTINGS_TYPE struct user_settings
#endif

#define PLUGIN_MAGIC 0x41535452UL
#define PLUGIN_API_VERSION 1
#define INTERNAL_PLUGIN_RETVAL_START 0x100
enum plugin_status {
    PLUGIN_OK = 0,
    PLUGIN_USB_CONNECTED = INTERNAL_PLUGIN_RETVAL_START,
    PLUGIN_POWEROFF,
    PLUGIN_GOTO_WPS,
    PLUGIN_GOTO_PLUGIN,
    PLUGIN_GOTO_ROOT,
    PLUGIN_ERROR = -1,
};

/* Hosted filesystem headers alias these names to the simulator backend. */
#undef strrchr
#undef strchr
#undef strcmp
#undef strncmp
#undef open
#undef creat
#undef close
#undef read
#undef write
#undef lseek
#undef ffilesize
#undef file_exists
#undef strrchr
#undef strchr
#undef strcmp
#undef strncmp
#undef opendir
#undef readdir
#undef closedir
#undef dir_get_info

struct plugin_api {
#define ROCK_DIRECT(name) __typeof__(name) *name;
#define ROCK_SLOT(decl, name, binding) decl;
#include "rock_api.def"
#undef ROCK_SLOT
#undef ROCK_DIRECT
};

struct plugin_header {
    struct lc_header lc_hdr;
    uint32_t platform;
    uint32_t header_size;
    size_t api_size;
    unsigned char fingerprint[32];
    enum plugin_status (*entry_point)(const void *);
    const struct plugin_api **api;
    const unsigned char *file_end;
    const unsigned char *bss_start;
};

extern const struct plugin_api *rb;
enum plugin_status plugin_start(const void *parameter);
enum plugin_status plugin__start(const void *parameter) NO_PROF_ATTR;

#ifdef PLUGIN
#ifndef ROCK_FINGERPRINT_PROBE
#include "rock_fingerprint.h"
#endif
#undef str
#define str(id) language_strings[(id)]
#define splash(ticks, msg) splashf((ticks), (msg))
#undef DEBUGF
#undef LDEBUGF
#define DEBUGF(...) do { } while (0)
#define LDEBUGF(...) do { } while (0)

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
extern unsigned char plugin_start_addr[], plugin_end_addr[];
extern unsigned char plugin_file_end[], plugin_bss_start[];
#define ROCK_IMAGE_BOUNDS plugin_start_addr, plugin_end_addr
#define ROCK_FILE_END plugin_file_end
#define ROCK_BSS_START plugin_bss_start
#define ROCK_HEADER_EXPORT __attribute__((section(".header"), used))
#define ROCK_API_STORAGE __attribute__((section(".data.rock_api")))
#else
#define ROCK_IMAGE_BOUNDS NULL, NULL
#define ROCK_FILE_END NULL
#define ROCK_BSS_START NULL
#ifdef _WIN32
#define ROCK_HEADER_EXPORT __declspec(dllexport) __attribute__((used))
#else
#define ROCK_HEADER_EXPORT __attribute__((visibility("default"), used))
#endif
#define ROCK_API_STORAGE
#endif

#define PLUGIN_HEADER \
    const struct plugin_api *rb ROCK_API_STORAGE = NULL; \
    ROCK_HEADER_EXPORT const struct plugin_header __header = { \
        .lc_hdr = { PLUGIN_MAGIC, TARGET_ID, PLUGIN_API_VERSION, ROCK_IMAGE_BOUNDS }, \
        .platform = CONFIG_PLATFORM, \
        .header_size = sizeof(struct plugin_header), \
        .api_size = sizeof(struct plugin_api), \
        .fingerprint = ROCK_FINGERPRINT, \
        .entry_point = plugin__start, \
        .api = &rb, \
        .file_end = ROCK_FILE_END, .bss_start = ROCK_BSS_START \
    };
#endif
#endif
