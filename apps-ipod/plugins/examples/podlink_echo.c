/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin.h"

#define CHANNEL "astrapod.echo"
#define OBJECT "from-ipod.bin"
#define SOURCE ROCKBOX_DIR "/podlink-echo.in"
#define TARGET ROCKBOX_DIR "/podlink-echo.out"
#define FIXTURE_SIZE 5000u
#define FIXTURE_CRC 0xc1607408u

static const char text[] = "AstraPod echo";
static const unsigned char binary[] = {0x00, 0x01, 0xff, 0x80, 0x00, 0x7f};
static struct rock_podlink_event event;
static struct gui_synclist list;
static char detail[64];

enum step { CONNECT, ECHO_TEXT, ECHO_BINARY, PUSH_FILE, PULL_FILE, SUCCESS, FAILURE };
static enum step step;

static const char *row(int index, void *data, char *buffer, size_t size)
{
    (void)data; (void)buffer; (void)size;
    static const char *const labels[] = {
        "Opening astrapod.echo", "Text echo", "Binary echo", "Pushing fixture",
        "Pulling fixture", "All checks passed", "Check failed",
    };
    if (index == 0)
        return labels[step];
    if (index == 1)
        return detail;
    return step < SUCCESS ? "Menu: cancel and exit" : "Menu: exit";
}

static void show(enum step next)
{
    step = next;
    detail[0] = '\0';
    rb->gui_synclist_draw(&list);
}

static void fail(int reason)
{
    static const char *const messages[] = {
        [ROCK_PODLINK_NO_COMPANION] = "No desktop companion",
        [ROCK_PODLINK_BUSY] = "Channel or transfer busy",
        [ROCK_PODLINK_INVALID] = "Reply or fixture check failed",
        [ROCK_PODLINK_IO] = "Check fixture and free space",
        [ROCK_PODLINK_TIMEOUT] = "Desktop reply timed out",
        [ROCK_PODLINK_CANCELLED] = "Transfer cancelled",
        [ROCK_PODLINK_LINK_LOST] = "Connect to desktop PodLink",
        [ROCK_PODLINK_LIMIT] = "Message or file exceeds limit",
        [ROCK_PODLINK_UNSUPPORTED] = "No USB transport in simulator",
    };
    rb->podlink_close();
    show(FAILURE);
    if (reason > 0 && (unsigned int)reason < ARRAYLEN(messages))
        rb->snprintf(detail, sizeof(detail), "%s", messages[reason]);
    else
        rb->snprintf(detail, sizeof(detail), "Result %d", reason);
    rb->gui_synclist_draw(&list);
}

static int transfer_event(int type)
{
    if (type == ROCK_PODLINK_PROGRESS)
    {
        rb->snprintf(detail, sizeof(detail), "%lu / %lu bytes",
                     (unsigned long)event.bytes, (unsigned long)event.total);
        rb->gui_synclist_draw(&list);
        return 0;
    }

    switch (step)
    {
        case CONNECT:
            if (type != ROCK_PODLINK_OPENED)
                break;
            show(ECHO_TEXT);
            return rb->podlink_send(false, text, sizeof(text) - 1);
        case ECHO_TEXT:
            if (type != ROCK_PODLINK_TEXT || event.length != sizeof(text) - 1 ||
                rb->memcmp(event.data, text, event.length))
                break;
            show(ECHO_BINARY);
            return rb->podlink_send(true, binary, sizeof(binary));
        case ECHO_BINARY:
            if (type != ROCK_PODLINK_BINARY || event.length != sizeof(binary) ||
                rb->memcmp(event.data, binary, event.length))
                break;
            show(PUSH_FILE);
            return rb->podlink_push(SOURCE, OBJECT);
        case PUSH_FILE:
        case PULL_FILE:
            if (type != ROCK_PODLINK_COMPLETE || event.bytes != FIXTURE_SIZE ||
                event.total != FIXTURE_SIZE || event.crc32 != FIXTURE_CRC)
                break;
            if (step == PUSH_FILE)
            {
                show(PULL_FILE);
                return rb->podlink_pull(TARGET, OBJECT);
            }
            rb->podlink_close();
            show(SUCCESS);
            rb->snprintf(detail, sizeof(detail), "5000 bytes, CRC c1607408");
            rb->gui_synclist_draw(&list);
            return 0;
        default:
            break;
    }
    return -ROCK_PODLINK_INVALID;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->gui_synclist_init(&list, row, NULL, false, 1, NULL);
    rb->gui_synclist_set_title(&list, "PodLink Echo", Icon_NOICON);
    rb->gui_synclist_set_nb_items(&list, 3);
    show(CONNECT);

    int rc = rb->podlink_open(CHANNEL);
    if (rc < 0)
        fail(-rc);
    while (true)
    {
        if (step < SUCCESS)
        {
            /* File operations have their own 10-second inactivity deadline. */
            rc = rb->podlink_poll(&event, step <= ECHO_BINARY ? 3000 : 0, NULL, NULL);
            if (rc == ROCK_PODLINK_CLOSED || (rc > 0 && event.result))
                fail(event.result ? event.result : ROCK_PODLINK_LINK_LOST);
            else if (rc < 0)
                fail(-rc);
            else if (rc > 0)
            {
                rc = transfer_event(rc);
                if (rc < 0)
                    fail(-rc);
            }
        }
        int action = rb->get_action(CONTEXT_LIST, HZ / 20);
        if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            rb->podlink_cancel();
            return PLUGIN_OK;
        }
        rb->gui_synclist_do_button(&list, &action);
    }
}
