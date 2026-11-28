/* Check the actual MP4 playback loop with a synchronous decoder double. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "plugin.h"
#include "mp4_demux.h"
#include "video_playback.h"
#include "video_audio.h"
#include "plugins/rock_h264.h"

int video_h264_probe(const char *path, void *buffer, size_t size);

enum mode { FINISH, SEEK_MENU, SCRUB_MENU, USB_EXIT, DECODE_ERROR, SEEK_INIT_ERROR };
static enum mode mode;
static volatile long current_tick;
static unsigned int thread_id = 1;
static unsigned int opens, closes, active, configured, decoded, drawn, seeks;
static int open_files, boosted, backlight;
static bool sent_menu, sent_seek;
static unsigned scrub_actions;
static uint8_t picture[16 * 16 * 3 / 2];

static unsigned audio_opens, audio_closes, audio_seeks;
static bool audio_active;
static uint32_t audio_ms;
static long audio_tick;

size_t video_audio_workspace_size(const struct mp4v_demux_res *demux)
{
    assert(demux->audio_sample_rate == 44100);
    return 65536;
}

int video_audio_init(const char *path, const struct mp4v_demux_res *demux,
                     void *workspace, size_t length, void *buffer, size_t bytes)
{
    assert(path && demux->audio_num_samples > 0 && workspace && length == 65536);
    assert(buffer && bytes >= 65536);
    audio_opens++;
    audio_ms = 0;
    return 0;
}

void video_audio_play(void) { audio_active = true; audio_tick = current_tick; }
void video_audio_pause(void) { assert(audio_active); }
void video_audio_resume(void) { assert(audio_active); }
void video_audio_seek(uint32_t ms)
{
    assert(audio_active && ms > 0);
    audio_seeks++;
    audio_ms = ms;
    audio_tick = current_tick;
}
void video_audio_stop(void) { audio_closes++; audio_active = false; }
bool video_audio_is_active(void) { return audio_active; }
bool video_audio_failed(void) { return false; }
static uint32_t audio_clock(void)
{
    assert(audio_active);
    return audio_ms + (current_tick - audio_tick) * 1000 / HZ;
}
static uint32_t audio_buffered(void) { return 0; }
static const struct rock_h264_audio_api audio_api = {
    .clock_ms = audio_clock,
    .buffered = audio_buffered,
};

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
    int permissions = 0;
    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        permissions = va_arg(args, int);
        va_end(args);
    }
    int fd = open(path, flags, permissions);
    if (fd >= 0)
        open_files++;
    return fd;
}

static int make_dir(const char *path) { return mkdir(path, 0700); }

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
    assert(context == CONTEXT_VIDEO_PLAYBACK && timeout == TIMEOUT_NOBLOCK);
    if (decoded < 4)
        return ACTION_NONE;
    if ((mode == SEEK_MENU || mode == SEEK_INIT_ERROR) && !sent_seek)
    {
        seeks++;
        sent_seek = true;
        return ACTION_WPS_SKIPNEXT;
    }
    if (mode == SCRUB_MENU && scrub_actions < 3)
    {
        /* The wheel must seek only after SELECT selects the progress bar. */
        unsigned step = scrub_actions++;
        if (step == 1)
            return ACTION_WPS_BROWSE;
        assert(opens == 1);
        if (step == 2)
            seeks++;
        return ACTION_WPS_VOLUP;
    }
    if ((mode == SEEK_MENU || mode == SCRUB_MENU) && decoded >= 8 && !sent_menu)
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

static void adjust_volume(int delta) { (void)delta; }

static const struct settings settings = {
    .backlight_timeout = 17, .video_fit = true,
};
static const struct status status;

static const struct plugin_api api = {
    .hw_h264 = &rock_h264_api,
    .h264_audio = &audio_api,
    .global_settings = &settings,
    .global_status = &status,
    .current_tick = &current_tick,
    .open = open_file,
    .close = close_file,
    .fdprintf = dprintf,
    .ffilesize = file_size,
    .lseek = lseek,
    .read = read,
    .write = write,
    .mkdir = make_dir,
    .rename = rename,
    .remove = remove,
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
    .lcd_set_background = color,
    .adjust_volume = adjust_volume,
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
    assert(argc == 4);
    reset(FINISH);
    assert(video_h264_probe(argv[3], scratch, sizeof(scratch)) == 0);
    assert(opens == 1 && configured == 1 && closes == 1 && decoded == 8);
    assert(!drawn && !audio_opens && access("resume", F_OK) < 0);
    FILE *log = fopen("video-probe.log", "r");
    char report[4096];
    assert(log);
    size_t bytes = fread(report, 1, sizeof(report) - 1, log);
    report[bytes] = '\0';
    fclose(log);
    assert(strstr(report, "stage=decoder-configure code=0"));
    assert(strstr(report, "sample=7") && strstr(report, "decode=1"));

    reset(FINISH);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 0);
    assert(opens == 1 && configured == 1 && closes == 1 && decoded == 336);
    assert(drawn >= decoded && backlight == settings.backlight_timeout);

    reset(SEEK_MENU);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 1);
    assert(opens == 2 && configured == 2 && closes == 2 && seeks == 1);

    reset(FINISH);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 0);
    assert(decoded == 96 && drawn == 93);

    reset(FINISH);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 0);
    assert(decoded == 336);

    reset(SCRUB_MENU);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 1);
    assert(opens == 2 && closes == 2 && seeks == 1);

    reset(USB_EXIT);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == 2);
    assert(opens == 1 && closes == 1 && !active);

    reset(DECODE_ERROR);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) < 0);
    assert(opens == 1 && closes == 1 && decoded == 3);
    log = fopen("video-probe.log", "r");
    assert(log);
    bytes = fread(report, 1, sizeof(report) - 1, log);
    report[bytes] = '\0';
    fclose(log);
    assert(strstr(report, "stage=sample-decode code=-1"));

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
    assert(audio_opens == 0);

    reset(SEEK_MENU);
    assert(video_h264_play(argv[3], scratch, sizeof(scratch)) == 1);
    assert(audio_opens == 1 && audio_seeks == 1 && audio_closes == 1 && !audio_active);
    reset(USB_EXIT);
    assert(video_h264_play(argv[3], scratch, sizeof(scratch)) == 2);
    assert(audio_opens == 2 && audio_closes == 2 && !audio_active);

    rock_h264_end();
    puts("PASS H.264 demux, complete playback, seek, resume, Menu, USB, errors, workspace");
}
