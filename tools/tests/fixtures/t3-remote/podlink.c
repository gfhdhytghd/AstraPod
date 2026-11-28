/* Headless simulator fixture: one datagram is one PodLink TEXT message. */
#include "config.h"
#include "kernel.h"
#include "podlink.h"
#include "root_menu.h"
#include "crc32.h"
#include "podlink_profile.h"
#include "playlist/playlist.h"
#include "usb_core.h"
#include "button.h"
#include "audio.h"
#include "input/action.h"
#include "ui/perf.h"
#include "metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

void t3_test_observe(const char *state);

static int wire = -1;
static bool foreground;
static bool connected = true;
static bool busy;
static uint32_t generation = 1;
static void (*background_receive)(const struct podlink_message *);
static const struct podlink_resident *resident;

int t3_test_start(void)
{
    wire = atoi(getenv("T3_TEST_FD"));
    assert(playlist_create("/", NULL) >= 0);
    assert(playlist_insert_track(NULL, "/one.wav", PLAYLIST_INSERT_LAST, false, true) >= 0);
    assert(playlist_insert_track(NULL, "/two.wav", PLAYLIST_INSERT_LAST, false, true) >= 0);
    playlist_start(0, 0, 0);
    const char *start = getenv("T3_TEST_START");
    return start && !strcmp(start, "menu") ? GO_TO_ROOT : GO_TO_AGENTS;
}

bool podlink_host_connected(void) { return wire >= 0 && connected; }
uint32_t podlink_generation(void) { return generation; }
enum podlink_connection podlink_state(void)
{ return podlink_host_connected() ? PODLINK_CONNECTED : PODLINK_WAIT_USB; }
bool podlink_open(void) { return wire >= 0; }
bool podlink_open_agents(void (*cb)(const struct podlink_message *))
{
    background_receive = cb;
    foreground = podlink_open();
    return foreground;
}
void podlink_init(void) {}
void podlink_close(void) { foreground = false; }
void podlink_shutdown(void) {}
void podlink_wait(int ticks) { sleep(ticks); }
unsigned int podlink_max_message(void) { return PODLINK_MAX_MESSAGE; }

int podlink_send(unsigned int type, const void *data, unsigned int length)
{
    if (wire < 0 || type != PODLINK_TEXT)
        return PODLINK_INVALID;
    if (!connected || busy)
        return PODLINK_BUSY;
    return send(wire, data, length, MSG_DONTWAIT | MSG_NOSIGNAL) == (int)length ?
        PODLINK_SENT : PODLINK_BUSY;
}

int podlink_poll(struct podlink_message *m)
{
    static unsigned char text[PODLINK_MAX_MESSAGE];
    if (wire < 0)
        return 0;
    int size = recv(wire, text, sizeof text, MSG_DONTWAIT);
    if (size <= 0)
        return 0;
    static unsigned perf;
    if (size > 2 && size < 32 && text[0] == '!' && text[1] == 'p')
    {
        text[size] = '\0';
        perf = ui_perf_begin((const char *)text + 2);
        t3_test_observe(perf ? "perf\tstarted" : "perf\tunavailable");
        return 0;
    }
    if (size == 2 && text[0] == '!')
    {
        switch (text[1])
        {
            case 'q': ui_perf_end(perf); perf = 0; break;
            case 'b': busy = true; break;
            case 'd': connected = false; generation++; break;
            case 'r': connected = true; busy = false; generation++; break;
        }
        return 0;
    }
    *m = (struct podlink_message){.type = PODLINK_TEXT, .length = size, .data = text};
    return 1;
}

void podlink_set_resident(const struct podlink_resident *r) { resident = r; }
void podlink_set_apl_consumer(const struct podlink_apl_consumer *c) { (void)c; }
bool podlink_take_time(char *r, unsigned long *t)
{
    (void)r;
    (void)t;
    struct podlink_message message;
    if (!foreground && podlink_poll(&message) &&
        (!resident || !resident->receive(&message)) && background_receive)
        background_receive(&message);
    return false;
}
bool usb_podlink_active(void) { return false; }
void usb_confirm_reopen(void) {}
bool usb_disk_take_warning(void) { return false; }
void podlink_wake(void) {}
uint32_t podlink_crc32(const unsigned char *bytes, unsigned int length)
{ return crc_32r(bytes, length, 0xffffffffu) ^ 0xffffffffu; }
uint32_t podlink_bulk_crc(const unsigned char *data, unsigned int length,
                          uint32_t previous, uint32_t *raw)
{
    *raw = crc_32r(data, length, 0);
    return crc_32r(data, length, previous);
}
uint32_t podlink_profile_now(void) { return 0; }
void podlink_profile_add(enum podlink_stage stage, uint32_t start, unsigned bytes)
{ (void)stage; (void)start; (void)bytes; }
bool podlink_profile_start(bool fast) { (void)fast; return false; }
void podlink_profile_stop(struct podlink_stage_sample *out, uint32_t *idle_us,
                          uint32_t *wall_us)
{
    memset(out, 0, PL_STAGE_COUNT * sizeof(*out));
    *idle_us = *wall_us = 0;
}

void t3_test_observe(const char *state)
{
    char line[512];
    int size = snprintf(line, sizeof line, ":%s", state);
    assert(size > 0 && size < (int)sizeof line);
    assert(send(wire, line, size, MSG_DONTWAIT | MSG_NOSIGNAL) == size);
}

void t3_test_media(long action)
{
    switch (action)
    {
        case BUTTON_MULTIMEDIA_PREV: t3_test_observe("playback\tprev"); break;
        case BUTTON_MULTIMEDIA_NEXT: t3_test_observe("playback\tnext"); break;
        case BUTTON_MULTIMEDIA_REW: t3_test_observe("playback\trew"); break;
        case BUTTON_MULTIMEDIA_FFWD: t3_test_observe("playback\tffwd"); break;
        case BUTTON_MULTIMEDIA_PLAYPAUSE: t3_test_observe("playback\tplay"); break;
    }
}

void usb_core_control_response(enum usb_control_response response, const void *data, size_t size)
{ (void)response; (void)data; (void)size; }

void t3_test_transport(int action)
{
    switch (action)
    {
        case ACTION_WPS_SKIPPREV: t3_test_observe("transport\tprev"); break;
        case ACTION_WPS_SKIPNEXT: t3_test_observe("transport\tnext"); break;
        case ACTION_WPS_PLAY: t3_test_observe("transport\tplay"); break;
    }
}

void t3_test_seek(long time)
{
    const struct mp3entry *track = audio_current_track();
    if (track)
        t3_test_observe(time > (long)track->elapsed ? "seek\tffwd" : "seek\trew");
}

/* The synthetic transport does not expose the hardware LCD shell. */
void shell_lcd_dispatch(void) {}

/* Real Cider runs in this build. The simulator cover trace is optional. */
void sim_cider_visible(int index, unsigned quality, unsigned pixel)
{
    (void)index;
    (void)quality;
    (void)pixel;
}
