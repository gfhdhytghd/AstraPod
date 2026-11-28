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

enum mode {
    FINISH, SEEK_MENU, SCRUB_MENU, USB_EXIT, DECODE_ERROR, SEEK_INIT_ERROR,
    COMPACT_SCRUB_TIMED, COMPACT_SCRUB_SELECT,
};
enum control_style { NO_CONTROLS, THEMED_CONTROLS, COMPACT_CONTROLS };
static enum mode mode;
static volatile long current_tick;
static unsigned int thread_id = 1;
static unsigned int opens, closes, active, configured, decoded, drawn, seeks;
static int open_files, boosted, backlight;
static bool sent_menu, sent_seek;
static unsigned scrub_actions;
static uint8_t picture[16 * 16 * 3 / 2];
static unsigned control_action, resume_calls;
static enum control_style controls;
static int resume_choice;
static long paused_at;
static bool saw_timeout;
static unsigned control_centre_calls, picture_changes;
static unsigned volume_changes;
static int picture_mode;
static long scrub_at, seek_at;
static bool scrub_cleared;

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
    if (opens == 2)
        seek_at = current_tick;
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
    *w = *stride = 16;
    *h = 12;
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
    if (mode == COMPACT_SCRUB_TIMED || mode == COMPACT_SCRUB_SELECT)
    {
        switch (control_action)
        {
        case 0:
        case 1:
            control_action++;
            return ACTION_WPS_BROWSE;
        case 2:
            scrub_at = current_tick;
            control_action++;
            return ACTION_WPS_VOLUP;
        case 3:
        {
            long delay = mode == COMPACT_SCRUB_TIMED ? 30 : 10;
            if (current_tick < scrub_at + delay)
                return ACTION_NONE;
            control_action++;
            if (mode == COMPACT_SCRUB_SELECT)
                return ACTION_WPS_BROWSE;
            scrub_at = current_tick;
            return ACTION_WPS_VOLUP;
        }
        default:
            return opens == 2 ? ACTION_WPS_MENU : ACTION_NONE;
        }
    }
    if (controls == COMPACT_CONTROLS)
    {
        switch (control_action)
        {
        case 0: control_action++; return ACTION_WPS_BROWSE;
        case 1: control_action++; return ACTION_WPS_QUICKSCREEN;
        case 2: control_action++; return ACTION_WPS_BROWSE;
        case 3: control_action++; return ACTION_WPS_BROWSE;
        case 4:
        case 5:
        case 6: control_action++; return ACTION_WPS_VOLUP;
        case 7: control_action++; return ACTION_WPS_BROWSE;
        case 8: control_action++; return ACTION_WPS_VOLUP;
        case 9: control_action++; return ACTION_WPS_BROWSE;
        case 10: control_action++; return ACTION_WPS_VOLDOWN;
        case 11:
            paused_at = current_tick;
            control_action++;
            return ACTION_WPS_PLAY;
        case 12:
            if (current_tick > paused_at + 4 * HZ)
                return ACTION_WPS_MENU;
            break;
        }
    }
    else if (controls == THEMED_CONTROLS)
    {
        switch (control_action)
        {
        case 0: control_action++; return ACTION_WPS_BROWSE;
        case 1: control_action++; return ACTION_WPS_BROWSE;
        case 2: control_action++; return ACTION_WPS_BROWSE;
        case 3: control_action++; return ACTION_WPS_VOLUP;
        case 4:
            paused_at = current_tick;
            control_action++;
            return ACTION_WPS_PLAY;
        case 5:
            if (current_tick > paused_at + 4 * HZ)
                return ACTION_WPS_MENU;
            break;
        }
    }
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
static int battery_level(void) { return 62; }

static void check_compact_controls(const unsigned char *plane, int stride)
{
    int footer_left = plane[213 * stride + 11];
    int footer_right = plane[213 * stride + 309];
    int middle = plane[213 * stride + 210];
    int top = plane[10 * stride + 12];
    int battery = plane[10 * stride + 290];
    int handle_pixels = 0;
    for (int col = 10; col < 310; col++)
        handle_pixels += plane[217 * stride + col] > 225;

    assert(plane[10 * stride + 244] == 112);
    switch (control_action)
    {
    case 0:
        assert(footer_left == 112 && footer_right == 112);
        assert(top == 112 && battery == 112);
        break;
    case 1:
    case 2:
    case 8:
    case 10:
        assert(footer_left != 112 && footer_right != 112 && !handle_pixels);
        assert(top != 112 && battery != 112);
        break;
    case 3:
        assert(footer_left != 112 && footer_right != 112 && handle_pixels == 4);
        break;
    case 4:
    case 5:
    case 6:
    case 7:
        assert(footer_left == 112 && footer_right != 112 && middle == 112);
        break;
    case 9:
    case 11:
        assert(footer_left != 112 && footer_right == 112 && middle == 112);
        assert(top != 112 && battery != 112);
        break;
    case 12:
        assert(top != 112);
        if (current_tick == paused_at + 4 * HZ)
        {
            assert(footer_left == 112 && footer_right == 112 && battery == 112);
            saw_timeout = true;
        }
        else
            assert(footer_left != 112 && footer_right == 112 && battery != 112);
        break;
    }
}

static void draw(unsigned char * const src[3], int sx, int sy, int stride,
                 int x, int y, int w, int h)
{
    assert(active && src[0] && src[1] && src[2]);
    assert(!x && !y && !sx && !sy &&
           stride == LCD_WIDTH && w == LCD_WIDTH && h == LCD_HEIGHT);
    if (mode == COMPACT_SCRUB_SELECT && control_action == 4)
    {
        assert(src[0][213 * stride + 11] == 112);
        assert(src[0][213 * stride + 309] == 112);
        scrub_cleared = true;
    }
    if (controls == COMPACT_CONTROLS)
        check_compact_controls(src[0], stride);
    else if (controls == THEMED_CONTROLS)
    {
        /* The uniform source remains visible through the inset panels. */
        int rail = src[0][208 * stride + 300];
        int header = src[0][10 * stride + 12];
        int handle_pixels = 0;
        for (int col = 14; col < 306; col++)
            handle_pixels += src[0][204 * stride + col] == 235;
        int badge = src[0][18 * stride + 292];
        switch (control_action)
        {
        case 0:
        case 3: assert(rail == 112 && header == 112); break;
        case 1: assert(rail != 112 && header == 40 && handle_pixels == 0); break;
        case 2: assert(rail != 112 && header == 40 && handle_pixels == 4); break;
        case 4: assert(rail != 112 && header == 112); break;
        case 5:
            if (current_tick == paused_at + 4 * HZ)
            {
                assert(rail == 112 && header == 112 && badge == 40);
                saw_timeout = true;
            }
            else
                assert(rail != 112 && header == 40);
            break;
        }
    }
    drawn++;
}

static int resume_menu(void)
{
    resume_calls++;
    return resume_choice;
}

static bool control_centre(void)
{
    control_centre_calls++;
    current_tick += 42;
    return true;
}

static int scale_picture(int step)
{
    if (step)
    {
        picture_changes++;
        picture_mode = (picture_mode + step + 3) % 3;
    }
    return picture_mode;
}

static void adjust_volume(int delta) { if (delta) volume_changes++; }

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
    control_action = 0;
    controls = NO_CONTROLS;
    saw_timeout = false;
    control_centre_calls = picture_changes = 0;
    volume_changes = 0;
    picture_mode = 0;
    scrub_at = seek_at = 0;
    scrub_cleared = false;
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

    memset(picture, 112, 256);
    memset(picture + 256, 128, sizeof(picture) - 256);
    const struct video_play_options options = {
        .title = "Test clip",
        .themed_controls = true,
        .accent = 0x041f,
        .resume_menu = resume_menu,
    };
    reset(FINISH);
    assert(video_h264_play(argv[1], scratch, sizeof(scratch)) == VIDEO_FINISHED);
    reset(FINISH);
    controls = THEMED_CONTROLS;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &options) ==
           VIDEO_EXIT);
    assert(!resume_calls && saw_timeout);

    reset(FINISH);
    resume_choice = -1;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &options) ==
           VIDEO_EXIT);
    assert(resume_calls == 1 && !opens);

    reset(FINISH);
    resume_choice = MENU_ATTACHED_USB;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &options) ==
           VIDEO_USB);
    assert(resume_calls == 2 && !opens);

    reset(FINISH);
    resume_choice = 1;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &options) ==
           VIDEO_FINISHED);
    assert(resume_calls == 3 && decoded == 336);

    reset(FINISH);
    controls = THEMED_CONTROLS;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &options) ==
           VIDEO_EXIT);
    assert(resume_calls == 3);
    reset(FINISH);
    resume_choice = 0;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &options) ==
           VIDEO_FINISHED);
    assert(resume_calls == 4 && drawn < 336);

    reset(FINISH);
    controls = COMPACT_CONTROLS;
    struct video_play_options compact = options;
    compact.compact_controls = true;
    compact.control_centre = control_centre;
    compact.picture_mode = scale_picture;
    compact.battery_level = battery_level;
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &compact) ==
           VIDEO_EXIT);
    assert(saw_timeout && control_centre_calls == 1);
    assert(picture_changes == 3 && picture_mode == 0 && volume_changes == 2);

    compact.resume_menu = NULL;
    reset(COMPACT_SCRUB_TIMED);
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &compact) ==
           VIDEO_EXIT);
    assert(seek_at == scrub_at + HZ / 2 && volume_changes == 0);

    reset(COMPACT_SCRUB_SELECT);
    assert(video_h264_play_named(argv[1], scratch, sizeof(scratch), &compact) ==
           VIDEO_EXIT);
    assert(seek_at == scrub_at + 10 && scrub_cleared && picture_changes == 0);

    rock_h264_end();
    puts("PASS H.264 lifecycle, timed video controls, resume choices and USB");
}
