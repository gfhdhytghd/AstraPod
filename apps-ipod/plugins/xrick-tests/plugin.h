/* Host services used by the XRick resource and PCM probe. GPL version 2+. */
#ifndef XRICK_TEST_PLUGIN_H
#define XRICK_TEST_PLUGIN_H
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rock_stream.h"

#define ROCKBOX_DIR "/.rockbox"
#define MAX_PATH 260
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct plugin_api {
    void *(*plugin_claim_scratch)(size_t, size_t, size_t *);
    void *(*memset)(void *, int, size_t);
    size_t (*strlen)(const char *);
    int (*snprintf)(char *, size_t, const char *, ...);
    int (*open)(const char *, int, ...);
    ssize_t (*read)(int, void *, size_t);
    int (*close)(int);
    off_t (*lseek)(int, off_t, int);
    off_t (*ffilesize)(int);
    unsigned (*rock_stream_open)(void);
    int (*rock_stream_pending)(void);
    bool (*rock_stream_submit)(const void *, size_t);
    bool (*rock_stream_pause)(bool);
    void (*rock_stream_close)(void);
};

extern const struct plugin_api *rb;
#endif
