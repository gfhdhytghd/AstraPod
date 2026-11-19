/* Simulator-only HerdRemote peer for capture-readme-gifs.py. */
#include "config.h"
#include "kernel.h"
#include "podlink.h"
#include "root_menu.h"
#include <stdlib.h>
#include <string.h>

static const char *const greeting[] = {
    "HR\t1\tFixture peer",
    "T\t0\t1",
    "W\t3\nw1\tf\t1\t0\t1\t0\tBeacon Studio\n"
    "w2\t-\t0\t1\t0\t0\tGlass Harbor\n"
    "w3\t-\t0\t0\t0\t2\tPaper Station",
    "S\t3\nw1\tw\td\nw2\tb\t-\nw3\ti\ti",
};
static const char usage[] =
    "U\t0\t1\n"
    "P\tcodex\tok\t28\t13245\t46\t242136\t840000\t2400\t\n"
    "P\tclaude\tok\t47\t9000\t33\t199380\t430000\t890\t\n"
    "P\tgrok\tok\t13\t5760\t27\t159240\t150000\t320\t\n"
    "P\tgemini\tok\t35\t11100\t15\t203760\t250000\t490\t\n"
    "N\t1670000\t4100";

static bool connected, usage_pending;
static unsigned int next = sizeof greeting / sizeof greeting[0];

int fu79_scene(int screen)
{
    return getenv("FU121_AGENTS") ? GO_TO_AGENTS : screen;
}

void podlink_set_resident(const struct podlink_resident *consumer) { (void)consumer; }
bool podlink_host_connected(void) { return connected; }
uint32_t podlink_generation(void) { return 1; }
enum podlink_connection podlink_state(void)
{
    return connected ? PODLINK_CONNECTED : PODLINK_CLOSED;
}
bool podlink_open(void) { return false; }
bool podlink_open_agents(void (*background)(const struct podlink_message *))
{
    (void)background;
    connected = true;
    return true;
}

void podlink_close(void) { connected = false; }
void podlink_shutdown(void) { podlink_close(); }

int podlink_send(unsigned int type, const void *data, unsigned int length)
{
    if (!connected || type != PODLINK_TEXT)
        return PODLINK_INVALID;
    if (length == 4 && !memcmp(data, "hr\t1", 4))
        next = 0;
    if (length == 2 && !memcmp(data, "us", 2))
        usage_pending = true;
    return PODLINK_SENT;
}

int podlink_poll(struct podlink_message *message)
{
    const char *text;
    if (next < sizeof greeting / sizeof greeting[0])
        text = greeting[next++];
    else if (usage_pending)
    {
        usage_pending = false;
        text = usage;
    }
    else
        return 0;
    *message = (struct podlink_message){
        .type = PODLINK_TEXT, .length = strlen(text), .data = (const unsigned char *)text,
    };
    return 1;
}

void podlink_wait(int ticks) { sleep(ticks); }
unsigned int podlink_max_message(void) { return PODLINK_MAX_MESSAGE; }

bool podlink_take_time(char *record, unsigned long *tick)
{
    (void)record;
    (void)tick;
    return false;
}

/* The host-only build cannot enter physical USB disk mode. */
bool usb_podlink_active(void) { return false; }
void usb_confirm_reopen(void) {}
bool usb_disk_take_warning(void) { return false; }

uint32_t podlink_crc32(const unsigned char *bytes, unsigned int length)
{
    uint32_t crc = ~0u;
    for (unsigned int n = 0; n < length; n++)
    {
        crc ^= bytes[n];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
}
