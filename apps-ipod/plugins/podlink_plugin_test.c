/* PodLink Shell plugin command dispatch. GNU GPL version 2 or later. */
#include "podlink-plugin-test.h"
#include "button.h"
#include "kernel.h"
#include "system.h"
#include "plugins/rock_compat.h"
#include "plugins/sdk/plugin.h"
#include "rbpaths.h"
#include "sm1-test-control.h"
#include "button_usb_remote.h"
#include "usb.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include <string.h>

enum { PLUGIN_IDLE, PLUGIN_QUEUED, PLUGIN_RUNNING };
enum { EXIT_NONE, EXIT_NORMAL, EXIT_USB, EXIT_ERROR };

/* The ipod6g is little-endian. IRQ exclusion serializes the USB and UI threads. */
static struct
{
    uint32_t version;
    uint32_t generation;
    uint32_t state;
    uint32_t reason;
    int32_t result;
    char name[32];
} status = { .version = 1 };

_Static_assert(sizeof(status) == 52, "plugin status wire size");

static const char *const paths[] = {
#include "rock-compat/rock_test_paths.h"
};

static const char *pending_path;
static bool foreground;
static bool test_keys;

static void set_result(int result)
{
    status.result = result;
    status.reason = result == PLUGIN_USB_CONNECTED ? EXIT_USB :
                    result < 0 ? EXIT_ERROR : EXIT_NORMAL;
}

bool podlink_plugin_test_matches(const struct usb_ctrlrequest *req)
{
    if (req->bRequest != SM1_TEST_USB_REQUEST || req->wIndex != SM1_TEST_USB_INDEX)
        return false;
    return req->bRequestType & USB_DIR_IN
        ? req->wValue == 3 || req->wValue == 4 || req->wValue >= PODLINK_TEST_CHUNK_BASE
        : req->wValue >= 9 && req->wValue <= 11;
}

static bool queue_launch(const uint8_t *name, unsigned length)
{
    if (!length || length >= sizeof(status.name))
        return false;

    const char *path = NULL;
    for (unsigned i = 0; i < ARRAYLEN(paths); i++)
    {
        const char *basename = strrchr(paths[i], '/') + 1;
        if (strlen(basename) == length + sizeof(".rock") - 1 &&
            !memcmp(basename, name, length))
        {
            path = paths[i];
            break;
        }
    }
    if (!path)
        return false;

    int level = disable_irq_save();
    bool accepted = !foreground && status.state == PLUGIN_IDLE &&
        button_queue_try_post(SYS_PODLINK_PLUGIN_TEST, 0);
    if (accepted)
    {
        pending_path = path;
        memset(status.name, 0, sizeof(status.name));
        memcpy(status.name, name, length);
        status.generation++;
        status.state = PLUGIN_QUEUED;
        status.reason = EXIT_NONE;
        status.result = 0;
    }
    restore_irq(level);
    return accepted;
}

void podlink_plugin_test_request(const struct usb_ctrlrequest *req,
                                uint8_t *data, size_t capacity, bool allowed)
{
    bool accepted = false;
    size_t reply = 0;
    if (allowed && req->wLength <= capacity)
    {
        if (req->bRequestType == 0xc0)
        {
            if (req->wValue == 3 && req->wLength == sizeof(status))
            {
                int level = disable_irq_save();
                memcpy(data, &status, sizeof(status));
                restore_irq(level);
                reply = sizeof(status);
            }
            else
                reply = podlink_plugin_test_lcd_read(req->wValue, req->wLength, data);
            accepted = reply != 0;
        }
        else if (req->bRequestType == 0x40)
        {
            if (req->wValue == 9)
                accepted = queue_launch(data, req->wLength);
            else if (req->wLength == 0 && req->wValue == 10)
            {
                int level = disable_irq_save();
                if (podlink_plugin_test_keys_active())
                {
                    accepted = podlink_plugin_test_key(USB_REMOTE_KEY_MENU, 1);
                    podlink_plugin_test_key(USB_REMOTE_KEY_MENU, 0);
                }
                restore_irq(level);
            }
            else if (req->wLength == 0 && req->wValue == 11)
                accepted = podlink_plugin_test_lcd_begin();
        }
    }
    usb_core_control_response(accepted ? USB_CONTROL_ACK : USB_CONTROL_STALL,
                              reply ? data : NULL, reply);
}

bool podlink_plugin_test_claim(bool shell)
{
    int level = disable_irq_save();
    bool claimed = !foreground;
    if (claimed)
    {
        foreground = true;
        /* A physical launch can already be in flight when EP0 queues its
         * event. Resolve that event before the new plugin can clear the queue. */
        if (!shell && pending_path)
        {
            pending_path = NULL;
            set_result(PLUGIN_ERROR);
            status.state = PLUGIN_IDLE;
        }
    }
    restore_irq(level);
    return claimed;
}

void podlink_plugin_test_release(void)
{
    int level = disable_irq_save();
    foreground = false;
    restore_irq(level);
}

void podlink_plugin_test_started(void)
{
    int level = disable_irq_save();
    status.state = PLUGIN_RUNNING;
    test_keys = true;
    restore_irq(level);
}

void podlink_plugin_test_stopped(void)
{
    int level = disable_irq_save();
    test_keys = false;
    podlink_plugin_test_clear_keys();
    restore_irq(level);
}

void podlink_plugin_test_usb_taken(void)
{
    int level = disable_irq_save();
    if (status.state == PLUGIN_RUNNING)
        set_result(PLUGIN_USB_CONNECTED);
    podlink_plugin_test_stopped();
    restore_irq(level);
}

bool podlink_plugin_test_keys_active(void)
{
    return test_keys;
}

bool podlink_plugin_test_launch(void)
{
    int level = disable_irq_save();
    const char *path = pending_path;
    pending_path = NULL;
    bool busy = foreground;
    restore_irq(level);
    if (!path)
        return false;

    int result = PLUGIN_ERROR;
    if (!busy)
    {
        if (usb_exclusive_storage())
            result = PLUGIN_USB_CONNECTED;
        else if (usb_podlink_active())
            result = rock_run_from_shell(path);
    }

    level = disable_irq_save();
    set_result(result);
    status.state = PLUGIN_IDLE;
    restore_irq(level);
    return result == PLUGIN_USB_CONNECTED;
}
