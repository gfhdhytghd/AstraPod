/* Production shell request, dispatch and recording loop with RAM-only ports. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sm1-test-control.h"
#include "usb_ch9.h"

#define IPOD_6G
#define HAVE_USB_PODLINK
#define USB_ENABLE_AUDIO
#define ROCKBOX_DIR "/.rockbox"
#define REC_BASE_DIR "/Recordings"
#define WAV_PATH REC_BASE_DIR "/PodLink-0001.wav"
#define MAX_PATH 260
#define IF_CNFN_NUM_(...)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define HZ 100
#define SYS_SM1_TEST_CONTROL ((int)0xd0000008u)

enum {
    ACTION_NONE, ACTION_STD_CANCEL, SYS_USB_CONFIRM, SYS_USB_CONNECTED,
    SYS_USB_DISCONNECTED, SYS_POWEROFF, SYS_REBOOT, CONTEXT_RECSCREEN,
    CONFIGURED, PODLINK_CLOSED, SHUTDOWN_REBOOT, PLAYLIST_INSERT_LAST,
    BUTTON_REDRAW
};
enum usb_control_response { USB_CONTROL_ACK, USB_CONTROL_STALL };
enum { AUDIO_STATUS_PLAY = 1, AUDIO_STATUS_RECORD = 4, AUDIO_STATUS_ERROR = 8 };
enum { AUDIO_SRC_PLAYBACK, AUDIO_SRC_MIC, AUDIO_SRC_LINEIN };
enum { AUDIO_GAIN_MIC, AUDIO_GAIN_LINEIN, REC_FORMAT_PCM_WAV, SRCF_PLAYBACK };
enum { REC_FREQ_44 = 44100, START_TICK = 123 };

struct audio_recording_options {
    int rec_source, rec_frequency, rec_channels, rec_mono_mode;
    struct { int rec_format; } enc_config;
};
static struct {
    int keyclick, rec_mic_gain, rec_left_gain, rec_right_gain;
} global_settings = { .keyclick = 1 };

volatile unsigned sm1_test_submitted, sm1_test_completed, sm1_test_dispatch_tick;
static long current_tick = START_TICK;
static int usb_state = CONFIGURED;
static bool cable = true, talking_disabled, capture, file_saved, closed;
static bool stop_in_cleanup, save_error;
static int queued_event, queued_data, button_data;
static unsigned polls, stop_calls;
static int response;
static size_t reply_size;
static const char *scenario;

static bool is_case(const char *name) { return strcmp(scenario, name) == 0; }
static int disable_irq_save(void) { return 0; }
static void restore_irq(int irq) { assert(irq == 0); }
static bool mikey_supported(void) { return true; }
static bool in_recording_screen(void) { return false; }
static bool usb_audio_get_active(void) { return false; }
static bool usb_audio_get_playing(void) { return false; }
static bool usb_inserted(void) { return cable; }
static bool usb_podlink_active(void) { return cable; }
static bool usb_exclusive_storage(void) { return false; }
static int podlink_state(void) { return PODLINK_CLOSED; }
static bool dir_exists(const char *path) { return strcmp(path, REC_BASE_DIR) == 0; }
static int mkdir(const char *path) { (void)path; assert(0); return -1; }
static char *create_numbered_filename(char *path, const char *dir,
                                      const char *prefix, const char *ext, int digits)
{
    assert(strcmp(dir, REC_BASE_DIR) == 0 && digits == 4);
    snprintf(path, MAX_PATH, "%s/%s0001%s", dir, prefix, ext);
    return path;
}
static bool file_exists(const char *path)
{
    return strcmp(path, WAV_PATH) == 0 && file_saved;
}
static int audio_status(void) { return capture ? AUDIO_STATUS_RECORD : 0; }
uint32_t sm1_test_audio_status(void) { return audio_status(); }
static void audio_error_clear(void) { assert(0); }
static unsigned pcm_rec_get_errors(void) { return 0; }
static unsigned pcm_rec_get_dma_blocks(void) { return capture || file_saved; }
static void talk_disable(bool disabled) { talking_disabled = disabled; }
static bool audio_try_init_recording(void) { return !capture; }
static void rec_set_recording_options(struct audio_recording_options *options)
{
    assert(options->rec_source == AUDIO_SRC_MIC && options->rec_channels == 1);
    assert(options->rec_frequency == REC_FREQ_44);
    assert(options->enc_config.rec_format == REC_FORMAT_PCM_WAV);
}
static void audio_set_recording_gain(int left, int right, int type)
{
    assert(left == 0 && right == 0 && type == AUDIO_GAIN_MIC);
}
static void audio_record(const char *path)
{
    assert(strcmp(path, WAV_PATH) == 0 && talking_disabled && !global_settings.keyclick);
    assert(sm1_test_submitted == sm1_test_completed + 1);
    capture = true;
}
static void audio_stop_recording_sync(void);
static void audio_close_recording(void) { closed = true; }
static void rec_set_source(int source, int flags)
{
    assert(closed && source == AUDIO_SRC_PLAYBACK && flags == SRCF_PLAYBACK);
}
static void sound_settings_apply(void) {}
static size_t copy_string(char *dest, const char *source, size_t size)
{
    snprintf(dest, size, "%s", source);
    return strlen(source);
}
static int get_action(int context, int timeout);
static long default_event_handler(long event);

#define strlcpy copy_string
#include "recording.inc"
#undef strlcpy

const char *sm1_test_debug_name(unsigned index) { (void)index; return NULL; }
void sm1_test_debug_run(unsigned index) { (void)index; assert(0); }
static bool button_queue_try_post(long event, int data)
{
    assert(event == SYS_SM1_TEST_CONTROL);
    if (queued_event)
        return false;
    queued_event = event;
    queued_data = data;
    return true;
}
static void usb_core_control_response(enum usb_control_response result,
                                      const void *payload, size_t size)
{
    assert(!size || payload);
    response = result;
    reply_size = size;
}
#include "ep0.inc"

static uint8_t *request(int type, int value, size_t length, int expected)
{
    static uint8_t data[SM1_TEST_RECORD_STATUS_SIZE];
    struct usb_ctrlrequest setup = {
        .bRequestType = type, .bRequest = SM1_TEST_USB_REQUEST,
        .wValue = value, .wIndex = SM1_TEST_USB_INDEX, .wLength = length,
    };
    memset(data, 0xff, sizeof(data));
    response = -1;
    usb_core_control_request_handler(&setup, data, sizeof(data));
    if (response != expected)
        fprintf(stderr, "%s: command %d EP0 response %d, expected %d\n",
                scenario, value, response, expected);
    assert(response == expected && reply_size == length);
    return data;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static void check_record_status(unsigned state, const char *path)
{
    const uint8_t *reply = request(0xc0, 1, 96, USB_CONTROL_ACK);
    assert(read_le32(reply) == 1);
    assert(read_le32(reply + 4) == state);
    assert(strcmp((const char *)reply + 8, path) == 0);
}

static uint32_t check_shell_counts(unsigned submitted, unsigned completed)
{
    const uint8_t *reply = request(0xc0, 0, 16, USB_CONTROL_ACK);
    assert(read_le32(reply + 4) == submitted);
    if (read_le32(reply + 8) != completed)
        fprintf(stderr, "%s: STOP completed while record-info still reports recording\n",
                scenario);
    assert(read_le32(reply + 8) == completed);
    return read_le32(reply + 12);
}

static void submit_stop(void)
{
    request(0x40, 8, 0, USB_CONTROL_ACK);
    request(0x40, 8, 0, USB_CONTROL_STALL);
}

static void audio_stop_recording_sync(void)
{
    assert(capture && !closed);
    ++stop_calls;
    if (stop_in_cleanup)
        submit_stop();
    if (cable)
    {
        check_record_status(SM1_RECORD_ACTIVE, "");
        assert(read_le32(request(0xc0, SM1_TEST_AUDIO_STATUS_VALUE, 4,
                                 USB_CONTROL_ACK)) == AUDIO_STATUS_RECORD);
        check_shell_counts(2, 1);
    }
    current_tick += HZ;
    capture = false;
    file_saved = !save_error;
}

static intptr_t button_get_data(void) { return button_data; }
static int take_button(void)
{
    int event = queued_event;
    button_data = queued_data;
    queued_event = 0;
    return event;
}

static bool clean_shutdown(int mode, void (*callback)(void *), void *parameter)
{
    (void)mode; (void)callback; (void)parameter; assert(0); return false;
}
static void complete_shell_command(void) { ++sm1_test_completed; }
static int podlink_screen_with_ready(void (*ready)(void))
{
    (void)ready; assert(0); return 0;
}
static void button_queue_post(int button, int data)
{
    assert(button == BUTTON_REDRAW && data == 0 && closed);
}
static void audio_stop(void) { assert(0); }
static void audio_pre_ff_rewind(void) { assert(0); }
static void audio_ff_rewind(int ms) { (void)ms; assert(0); }
static int playlist_create(void *dir, void *file)
{
    (void)dir; (void)file; assert(0); return -1;
}
static int playlist_insert_track(void *list, const char *file, int position,
                                 bool queue, bool sync)
{
    (void)list; (void)file; (void)position; (void)queue; (void)sync;
    assert(0); return -1;
}
static void playlist_start(int index, int elapsed, int offset)
{
    (void)index; (void)elapsed; (void)offset; assert(0);
}

static long default_event_handler(long event)
{
    void (*callback)(void *) = NULL;
    void *parameter = NULL;
    switch (event)
    {
#include "dispatch.inc"
        default:
            assert(event == ACTION_NONE);
    }
    return event;
}

static int get_action(int context, int timeout)
{
    assert(context == CONTEXT_RECSCREEN && timeout == HZ / 10);
    assert(++polls <= 3);
    current_tick += timeout;
    if (polls == 1)
    {
        check_shell_counts(1, 1);
        check_record_status(SM1_RECORD_ACTIVE, "");
        request(0x40, 7, 0, USB_CONTROL_STALL);
        if (is_case("unplug"))
        {
            cable = false;
            return ACTION_NONE;
        }
        if (stop_in_cleanup)
            return ACTION_STD_CANCEL;
        if (is_case("busy"))
            queued_event = SYS_USB_DISCONNECTED; /* Queue occupied during EP0. */
        submit_stop();
        if (is_case("busy"))
            queued_event = 0;
        if (is_case("nested") && queued_event)
        {
            /* An inner input pump delivers the event to the default UI
             * handler, so the recorder's get_action does not return it. */
            default_event_handler(take_button());
            check_record_status(SM1_RECORD_ACTIVE, "");
            check_shell_counts(2, 1);
        }
    }
    return take_button();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    save_error = is_case("save-error");
    stop_in_cleanup = is_case("late");
    check_record_status(SM1_RECORD_IDLE, "");
    request(0x40, 8, 0, USB_CONTROL_STALL);
    request(0x40, 7, 0, USB_CONTROL_ACK);
    request(0x40, 8, 0, USB_CONTROL_STALL);
    default_event_handler(take_button());
    assert(stop_calls == 1 && closed && !capture);
    assert(!talking_disabled && global_settings.keyclick == 1);
    cable = true;
    unsigned commands = is_case("unplug") ? 1 : 2;
    uint32_t dispatch_tick = check_shell_counts(commands, commands);
    if (is_case("unplug"))
        assert(dispatch_tick == START_TICK);
    else
        assert(dispatch_tick ==
               START_TICK + HZ / 10 + (stop_in_cleanup ? HZ : 0));
    check_record_status(save_error ? SM1_RECORD_ERROR : SM1_RECORD_READY,
                        save_error ? "" : WAV_PATH);
    if (save_error)
    {
        const uint8_t *reply = request(0xc0, 2, 20, USB_CONTROL_ACK);
        assert(read_le32(reply) == SM1_RECORD_STAGE_SAVE);
    }
    request(0x40, 8, 0, USB_CONTROL_STALL);
    request(0x40, 7, 0, USB_CONTROL_ACK);
    printf("PASS shell record start/stop: %s\n", scenario);
}
