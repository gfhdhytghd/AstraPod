/* Minimal SDK for XWorld's host data checks. GPL-2.0-or-later. */
#ifndef XWORLD_TEST_PLUGIN_H
#define XWORLD_TEST_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <fcntl.h>
#include "gcc_extensions.h"

#define ROCKBOX_DIR "/.rockbox"
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#define LCD_WIDTH 320
#define LCD_HEIGHT 240

typedef uint16_t fb_data;

struct plugin_api {
    int (*snprintf)(char *, size_t, const char *, ...);
    size_t (*strlen)(const char *);
    void *(*memset)(void *, int, size_t);
    int (*open)(const char *, int, ...);
    int (*close)(int);
    ssize_t (*read)(int, void *, size_t);
    ssize_t (*write)(int, const void *, size_t);
    off_t (*lseek)(int, off_t, int);
    off_t (*ffilesize)(int);
    bool (*file_exists)(const char *);
    int (*remove)(const char *);
    void *(*plugin_claim_scratch)(size_t, size_t, size_t *);
    int (*audio_status)(void);
    void (*audio_stop)(void);
};
extern const struct plugin_api *rb;
#endif
