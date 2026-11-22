/* Check the actual MP4 playback loop with a synchronous decoder double. */
#include <assert.h>
#include <sys/stat.h>
#include "plugin.h"
#include "mp4_demux.h"
#include "video_playback.h"
#include "plugins/rock_h264.h"

enum mode { FINISH, SEEK_MENU, USB_EXIT, DECODE_ERROR, SEEK_INIT_ERROR };
static enum mode mode;
static volatile long current_tick;
static unsigned int thread_id = 1;
static unsigned int opens, closes, active, configured, decoded, drawn, seeks;
static int open_files, boosted, backlight;
static bool sent_menu, sent_seek;
static uint8_t picture[16 * 16 * 3 / 2];

unsigned int thread_self(void) { return thread_id; }

static size_t size_for(int w, int h)
{
    return (size_t)w * h;
}

static struct vpu_h264 *open_video(void *buffer, size_t size, int w, int h)
{
    assert(!active && buffer && size >= size_for(w, h));
    assert(w == 320 && h == 240);
    opens++;
    active = 1;
    return buffer;
}

static int configure(struct vpu_h264 *decoder, const uint8_t *avcc, int size)
{
    assert(active && decoder && avcc[0] == 1 && avcc[1] == 66 && size >= 7);
    configured++;
    return mode == SEEK_INIT_ERROR && opens == 2 ? -1 : 0;
}

static int decode(struct vpu_h264 *decoder, const uint8_t *sample,
                  int size, int nalu_len_size)
{
    assert(active && decoder && sample && size > nalu_len_size);
    decoded++;
    return mode == DECODE_ERROR && decoded == 3 ? -1 : 1;
}

static void frame(const struct vpu_h264 *decoder, const uint8_t **y,
                  const uint8_t **cb, const uint8_t **cr, int *w,
                  int *h, int *stride)
{
    assert(active && decoder);
    *y = picture;
    *cb = &picture[256];
    *cr = &picture[320];
    *w = *h = *stride = 16;
}

static void close_video(struct vpu_h264 *v)
{
    assert(v && active);
    closes++;
    active = 0;
}

const struct hw_h264_api target_hw_h264_api = {
    size_for, open_video, configure, decode, frame, close_video
};

static int open_file(const char *path, int flags, ...)
{
    int fd = open(path, flags);
    if (fd >= 0)
        open_files++;
    return fd;
}

static int close_file(int fd)
{
    assert(open_files > 0);
    open_files--;
    return close(fd);
}

static off_t file_size(int fd)
{
    struct stat stat;
    return fstat(fd, &stat) < 0 ? -1 : stat.st_size;
}

static int action(int context, int timeout)
{
    assert(context == CONTEXT_WPS && timeout == TIMEOUT_NOBLOCK);
    if (decoded < 4)
        return ACTION_NONE;
    if ((mode == SEEK_MENU || mode == SEEK_INIT_ERROR) && !sent_seek)
    {
        seeks++;
        sent_seek = true;
        return ACTION_WPS_SKIPNEXT;
    }
    if (mode == SEEK_MENU && decoded >= 8 && !sent_menu)
    {
        sent_menu = true;
        return ACTION_WPS_MENU;
    }
    if (mode == USB_EXIT)
        return SYS_USB_CONNECTED;
    return ACTION_NONE;
}

static long event(long code)
{
    if (code == SYS_USB_CONNECTED)
    {
        assert(active);
        rock_h264_end();
        assert(!active);
    }
    return code;
}

static void boost(bool state) { boosted = state; }
static void timeout(int value) { backlight = value; }
static void wait_ticks(long ticks) { current_tick += ticks; }
static void no_op(void) {}
static void color(unsigned int value) { (void)value; }
static void splash(int ticks, const char *label) { (void)ticks; (void)label; }
static struct font *font(int id) { (void)id; return NULL; }
static int val2phys(int setting, int value) { (void)setting; return value; }
static int minimum(int setting) { (void)setting; return -80; }
static const char *unit(int setting) { (void)setting; return "dB"; }

static void draw(unsigned char * const src[3], int sx, int sy, int stride,
                 int x, int y, int w, int h)
{
    assert(active && src[0] && src[1] && src[2]);
    assert(!x && !y && !sx && !sy &&
           stride == LCD_WIDTH && w == LCD_WIDTH && h == LCD_HEIGHT);
    drawn++;
}

static const struct settings settings = { .backlight_timeout = 17 };
static const struct status status;

static const struct plugin_api api = {
    .hw_h264 = &rock_h264_api,
    .global_settings = &settings,
    .global_status = &status,
    .current_tick = &current_tick,
    .open = open_file,
    .close = close_file,
    .ffilesize = file_size,
    .lseek = lseek,
    .read = read,
    .memset = memset,
    .cpu_boost = boost,
    .get_action = action,
    .default_event_handler = event,
    .sleep = wait_ticks,
    .backlight_on = no_op,
    .backlight_set_timeout = timeout,
    .lcd_clear_display = no_op,
    .lcd_update = no_op,
    .lcd_set_foreground = color,
    .lcd_blit_yuv = draw,
    .font_get = font,
    .snprintf = snprintf,
    .sound_min = minimum,
    .sound_val2phys = val2phys,
    .sound_unit = unit,
    .splash = splash,
};

const struct plugin_api *rb = &api;
static uint8_t scratch[2 * 1024 * 1024] __attribute__((aligned(32)));

static void reset(enum mode next)
{
    assert(!active && !open_files && !boosted);
    opens = closes = configured = decoded = drawn = seeks = 0;
    current_tick = 0;
    sent_menu = sent_seek = false;
    mode = next;
    rock_h264_begin(scratch, sizeof(scratch));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    reset(FINISH);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 0);
    assert(opens == 1 && configured == 1 && closes == 1 && decoded == 336);
    assert(drawn >= decoded && backlight == settings.backlight_timeout);

    reset(SEEK_MENU);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 1);
    assert(opens == 2 && configured == 2 && closes == 2 && seeks == 1);

    reset(USB_EXIT);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 2);
    assert(opens == 1 && closes == 1 && !active);

    reset(DECODE_ERROR);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) < 0);
    assert(opens == 1 && closes == 1 && decoded == 3);

    reset(SEEK_INIT_ERROR);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) < 0);
    assert(opens == 2 && closes == 2 && seeks == 1);

    reset(FINISH);
    assert(video_h264_play(argv[2], scratch, sizeof(scratch)) < 0);
    assert(!opens);
    assert(video_h264_play(argv[1], scratch, 1024) < 0);
    assert(!opens);
    assert(!rock_h264_api.open(scratch + sizeof(scratch) - 32, 64, 320, 240));
    assert(!rock_h264_api.open(scratch + 1, 100000, 320, 240));
    thread_id = 2;
    assert(!rock_h264_api.open(scratch, sizeof(scratch), 320, 240));
    thread_id = 1;
    assert(!active && !open_files && !boosted);
    rock_h264_end();
    puts("PASS H.264 demux, complete playback, seek, Menu, USB, errors, workspace");
}
