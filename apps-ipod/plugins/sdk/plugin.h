/* AstraPod plugin SDK. GNU General Public License, version 2 or later. */
#ifndef ASTRAPOD_PLUGIN_H
#define ASTRAPOD_PLUGIN_H

#ifdef PLUGIN
#define NO_REDEFINES_PLEASE
#endif

#include "config.h"
#include "plugins/rock_pitch.h"
#include "plugins/rock_stream.h"
#include "plugins/rock_h264_audio.h"
#include "plugins/rock_execution.h"
#include "plugins/rock_mpeg.h"
#include "plugins/rock_h264.h"
#include "plugins/rock_pictureflow.h"
#ifdef ROCK_FINGERPRINT_PROBE
#include "database/db_summary.h"
#endif
#include "plugins/rock_hid.h"
#include "plugins/rock_game_io.h"
#include "plugins/rock_overlay.h"
#include "plugins/rock_image_overlay.h"
#include "plugins/rock_resident.h"
#include "plugins/rock_tsr.h"
#include "plugins/rock_phase3_settings.h"
#include "plugins/rock_specialist.h"
#include "bidi.h"
#include <ctype.h>
#include <stdio.h>
#include "plugins/rock_podlink.h"
#include "rock_tray.h"

#ifdef PLUGIN
/* Upstream attributes retain their DRAM behavior in every module. */
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
#include "diacritic.h"
#include "strcasestr.h"
#include "vuprintf.h"
#include "file.h"
#include "dir.h"
#include "load_code.h"
#include "lcd.h"
#include "font.h"
#include "audio.h"
#include "sound.h"
#include "pcm_mixer.h"
#include "pcm_waveform.h"
#ifdef USB_ENABLE_AUDIO
#include "usbstack/usb_audio.h"
#endif
#include "playback.h"
#include "playlist.h"
#include "action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/scrollbar.h"
#include "screens/browse/browser.h"
#include "system/volume.h"
#include "settings.h"
#include "settings/settings_list.h"
#include "database/tagcache.h"
#include "speech/language.h"
#include "speech/talk.h"
#include "system/app_util.h"
#include "system/format_time.h"
#include "system/appevents.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "powermgmt.h"
#include "timefuncs.h"
#include "version.h"
#include "lang_enum.h"
#include "rbpaths.h"
#include "ui/statusbar.h"
#include "widgets/menu.h"
#include "widgets/keyboard.h"
#include "widgets/list.h"
#include "widgets/yesno.h"
#include "widgets/option_select.h"
#include "widgets/splash.h"

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
#if defined(ROCK_IMAGE_DECODER)
#define ROCK_IMAGE_MAGIC ROCK_IMAGE_DECODER_MAGIC
#elif defined(ROCK_OVERLAY_IMAGE)
#define ROCK_IMAGE_MAGIC ROCK_OVERLAY_MAGIC
#elif defined(ROCK_TSR_IMAGE)
#define ROCK_IMAGE_MAGIC ROCK_TSR_MAGIC
#else
#define ROCK_IMAGE_MAGIC PLUGIN_MAGIC
#endif
#define PLUGIN_API_VERSION 2
#define ROCK_CLICK_TICK_HZ 500
#define INTERNAL_PLUGIN_RETVAL_START 0x100
enum plugin_status {
    PLUGIN_OK = 0,
    PLUGIN_USB_CONNECTED = INTERNAL_PLUGIN_RETVAL_START,
    PLUGIN_POWEROFF,
    PLUGIN_GOTO_WPS,
    PLUGIN_GOTO_PLUGIN,
    PLUGIN_GOTO_ROOT,
    PLUGIN_GOTO_FILEBROWSER,
    PLUGIN_CHAIN_OVERLAY,
    PLUGIN_ERROR = -1,
};

/* Hosted filesystem headers alias these names to the simulator backend. */
#undef strrchr
#undef strchr
#undef strcmp
#undef strncmp
#undef open
#undef creat
#undef remove
#undef close
#undef read
#undef write
#undef lseek
#undef ffilesize
#undef remove
#undef rename
#undef file_exists
#undef strrchr
#undef strchr
#undef strcmp
#undef strncmp
#undef opendir
#undef readdir
#undef closedir
#undef dir_get_info
#undef dir_exists
#undef mkdir
#undef rmdir
#undef ftruncate

struct custom_format;

struct plugin_api {
#define ROCK_DIRECT(name) __typeof__(name) *name;
#define ROCK_SLOT(decl, name, binding) decl;
#include "rock_api.def"
#undef ROCK_SLOT
#undef ROCK_DIRECT
};

#define ROCK_IMAGE_LARGE_STACK (1u << 0)
#define ROCK_IMAGE_IRAM        (1u << 1)

struct plugin_header {
    struct lc_header lc_hdr;
    uint32_t platform;
    uint32_t header_size;
    uint32_t flags;
    size_t api_size;
    unsigned char fingerprint[32];
    enum plugin_status (*entry_point)(const void *);
    const struct plugin_api **api;
    const unsigned char *file_end;
    const unsigned char *bss_start;
    const unsigned char *rock_iram_source;
    const unsigned char *rock_iram_data_end;
    const unsigned char *rock_iram_end;
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
#undef splash
#define splash(ticks, msg) splashf((ticks), (msg))
#undef DEBUGF
#undef LDEBUGF
#undef LOGF
#define DEBUGF(...) do { } while (0)
#define LDEBUGF(...) do { } while (0)
#define LOGF(...) do { } while (0)

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

#if defined(ROCK_REQUEST_IRAM)
#if defined(ROCK_OVERLAY_IMAGE) || defined(ROCK_TSR_IMAGE) || \
    defined(ROCK_IMAGE_DECODER) || defined(ROCK_REQUEST_LARGE_STACK) || !defined(IPOD_6G)
#error IRAM requires a foreground ipod6g module
#endif
#define ROCK_BUILD_IMAGE_FLAGS ROCK_IMAGE_IRAM
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#define ROCK_IRAM_CODE __attribute__((section(".rock_iram.text"), noinline))
#define ROCK_IRAM_DATA __attribute__((section(".rock_iram.data")))
#define ROCK_IRAM_BSS  __attribute__((section(".rock_iram.bss")))
extern unsigned char rock_iram_source[], rock_iram_data_end[], rock_iram_end[];
#define ROCK_IRAM_BOUNDS \
        .rock_iram_source = rock_iram_source, \
        .rock_iram_data_end = rock_iram_data_end, \
        .rock_iram_end = rock_iram_end,
#else
#define ROCK_IRAM_CODE __attribute__((noinline))
#define ROCK_IRAM_DATA
#define ROCK_IRAM_BSS
#endif
#elif defined(ROCK_REQUEST_LARGE_STACK)
#define ROCK_BUILD_IMAGE_FLAGS ROCK_IMAGE_LARGE_STACK
#else
#define ROCK_BUILD_IMAGE_FLAGS 0
#endif
#ifndef ROCK_IRAM_BOUNDS
#define ROCK_IRAM_BOUNDS
#endif

#define PLUGIN_HEADER \
    const struct plugin_api *rb ROCK_API_STORAGE = NULL; \
    ROCK_HEADER_EXPORT const struct plugin_header __header = { \
        .lc_hdr = { ROCK_IMAGE_MAGIC, TARGET_ID, PLUGIN_API_VERSION, ROCK_IMAGE_BOUNDS }, \
        .platform = CONFIG_PLATFORM, \
        .header_size = sizeof(struct plugin_header), \
        .api_size = sizeof(struct plugin_api), \
        .flags = ROCK_BUILD_IMAGE_FLAGS, \
        .fingerprint = ROCK_FINGERPRINT, \
        .entry_point = plugin__start, \
        .api = &rb, \
        .file_end = ROCK_FILE_END, .bss_start = ROCK_BSS_START, \
        ROCK_IRAM_BOUNDS \
    };
#endif
#endif
