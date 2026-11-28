/* Host contracts for the staged video player and stock MP4 demuxer. */
#ifndef H264_TEST_PLUGIN_H
#define H264_TEST_PLUGIN_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "hw_h264.h"
#include "plugins/rock_h264_audio.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TIME_BEFORE(a, b) ((a) < (b))
#define MAX_PATH 260
#define PLUGIN_APPS_DATA_DIR "resume"
#define ROCKBOX_DIR "."
#define CACHEALIGN_SIZE 32
#define HZ 100
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_BLACK 0
#define FONT_SYSFIXED 0
#define SOUND_VOLUME 0
#define CONTEXT_VIDEO_PLAYBACK 27
#define TIMEOUT_NOBLOCK 0
#define SYS_USB_CONNECTED 0x100
enum {
    ACTION_NONE, ACTION_WPS_MENU, ACTION_STD_CANCEL, ACTION_WPS_STOP,
    ACTION_WPS_SKIPPREV, ACTION_WPS_SEEKBACK, ACTION_WPS_SKIPNEXT,
    ACTION_WPS_SEEKFWD, ACTION_WPS_PLAY, ACTION_WPS_BROWSE,
    ACTION_WPS_VOLUP, ACTION_WPS_VOLDOWN, ACTION_WPS_STOPSEEK
};
struct font { unsigned int depth, height; };
struct settings { int backlight_timeout; bool video_fit; };
struct status { int volume; };
struct plugin_api {
    const struct hw_h264_api *hw_h264;
    const struct rock_h264_audio_api *h264_audio;
    const struct settings *global_settings;
    const struct status *global_status;
    volatile long *current_tick;
    int (*open)(const char *, int, ...);
    int (*close)(int);
    int (*fdprintf)(int, const char *, ...);
    off_t (*ffilesize)(int);
    off_t (*lseek)(int, off_t, int);
    ssize_t (*read)(int, void *, size_t);
    ssize_t (*write)(int, const void *, size_t);
    int (*mkdir)(const char *);
    int (*rename)(const char *, const char *);
    int (*remove)(const char *);
    void *(*memset)(void *, int, size_t);
    void (*cpu_boost)(bool);
    int (*get_action)(int, int);
    long (*default_event_handler)(long);
    void (*sleep)(long);
    void (*backlight_on)(void);
    void (*backlight_set_timeout)(int);
    void (*lcd_clear_display)(void);
    void (*lcd_update)(void);
    void (*lcd_set_foreground)(unsigned int);
    void (*lcd_set_background)(unsigned int);
    void (*adjust_volume)(int);
    void (*lcd_blit_yuv)(unsigned char * const[3], int, int, int,
                         int, int, int, int);
    struct font *(*font_get)(int);
    int (*font_get_width)(struct font *, unsigned int);
    const unsigned char *(*font_get_bits)(struct font *, unsigned int);
    int (*snprintf)(char *, size_t, const char *, ...);
    int (*sound_min)(int);
    int (*sound_val2phys)(int, int);
    const char *(*sound_unit)(int);
    void (*splash)(int, const char *);
};
extern const struct plugin_api *rb;
#endif
