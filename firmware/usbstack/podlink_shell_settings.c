/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Queue settings work on the UI thread and serve bounded EP0 reply pages.
 ****************************************************************************/
#include "system.h"
#include "string.h"
#include "button.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "sm1-test-control.h"
#include "podlink-shell-settings.h"

static unsigned operation;
static uint32_t request_cookie;
static char body[SHELL_SETTINGS_OUT_SIZE - 4 + 1];

static uint32_t reply_cookie;
static size_t reply_length;
static char reply[SHELL_SETTINGS_REPLY_SIZE];

unsigned shell_settings_operation(void)
{
    return operation;
}

char *shell_settings_body(void)
{
    return body;
}

unsigned shell_settings_ordinal(void)
{
    return (unsigned char)body[0] | (unsigned char)body[1] << 8;
}

void shell_settings_publish(const char *json, size_t length)
{
    if (length > sizeof(reply))
    {
        json = "{\"error\":\"Settings reply is too large\"}";
        length = strlen(json);
    }
    /* The USB thread sees no cookie until the entire reply is available. */
    memcpy(reply, json, length);
    int oldlevel = disable_irq_save();
    reply_length = length;
    reply_cookie = request_cookie;
    restore_irq(oldlevel);
}

static void read_page(unsigned page, uint8_t *buffer)
{
    const size_t page_size = SHELL_SETTINGS_PAGE_SIZE -
                             SHELL_SETTINGS_PAGE_HEADER;
    size_t offset = page * page_size;

    memset(buffer, 0, SHELL_SETTINGS_PAGE_SIZE);
    int oldlevel = disable_irq_save();
    uint32_t cookie = reply_cookie;
    if (cookie)
    {
        buffer[0] = cookie;
        buffer[1] = cookie >> 8;
        buffer[2] = cookie >> 16;
        buffer[3] = cookie >> 24;
        buffer[4] = reply_length;
        buffer[5] = reply_length >> 8;
        if (offset < reply_length)
            memcpy(buffer + SHELL_SETTINGS_PAGE_HEADER, reply + offset,
                   MIN(page_size, reply_length - offset));
    }
    restore_irq(oldlevel);
    usb_core_control_response(USB_CONTROL_ACK, buffer, SHELL_SETTINGS_PAGE_SIZE);
}

static bool submit(unsigned op, const uint8_t *data, size_t size)
{
    if (size <= 4 || size > SHELL_SETTINGS_OUT_SIZE ||
        op < SHELL_SETTINGS_LIST || op > SHELL_SETTINGS_ACTION ||
        sm1_test_submitted != sm1_test_completed || sm1_test_record_active())
        return false;

    uint32_t cookie = data[0] | (uint32_t)data[1] << 8 |
                      (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    data += 4;
    size -= 4;
    if (!cookie || cookie == reply_cookie ||
        (op == SHELL_SETTINGS_LIST && size != 2) ||
        (op != SHELL_SETTINGS_LIST && (memchr(data, '\0', size) ||
                                       memchr(data, '\r', size) ||
                                       memchr(data, '\n', size))) ||
        (op == SHELL_SETTINGS_ACTION && (size < 2 || data[0] != 1)))
        return false;

    memcpy(body, data, size);
    body[size] = '\0';
    request_cookie = cookie;
    operation = op;
    uint32_t previous = reply_cookie;
    reply_cookie = 0;
    ++sm1_test_submitted;
    if (button_queue_try_post(SYS_SM1_TEST_CONTROL, SHELL_SETTINGS_EVENT))
        return true;

    --sm1_test_submitted;
    reply_cookie = previous;
    return false;
}

void shell_settings_request(const struct usb_ctrlrequest *req,
                            uint8_t *buffer, size_t capacity, bool allowed)
{
    if (allowed && req->wLength <= capacity)
    {
        if (req->bRequestType == 0xc0 && req->wLength == SHELL_SETTINGS_PAGE_SIZE)
        {
            read_page(req->wValue, buffer);
            return;
        }
        if (req->bRequestType == 0x40 &&
            submit(req->wValue, buffer, req->wLength))
        {
            usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
            return;
        }
    }
    usb_core_control_response(USB_CONTROL_STALL, NULL, 0);
}
