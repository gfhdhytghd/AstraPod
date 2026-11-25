/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <string.h>
#include "button.h"
#include "lcd-te-s5l8702.h"
#include "podlink-shell-lcd.h"
#include "screens/system/lcd_te_debug.h"
#include "sm1-test-control.h"
#include "system.h"
#include "ui/pace.h"
#include "usb.h"
#include "usb_ch9.h"
#include "usb_core.h"

enum shell_lcd_op {
    SHELL_LCD_TIMING = 1,
    SHELL_LCD_SYNC,
    SHELL_LCD_PACING,
    SHELL_LCD_DIAG,
    /* 5 to 9 are the LCD throughput experiments' ops; keep them free. */
    SHELL_LCD_PACE_STATUS = 10,
    SHELL_LCD_ASYNC,
};

/* The ui_pace_status field SHELL_LCD_PACE_STATUS returns. */
enum shell_lcd_pace_field {
    PACE_PERIOD,
    PACE_DIVISOR,
    PACE_SHOWN,
    PACE_MISSED,
    PACE_OFFSET,
    PACE_COST,
    PACE_DMA,
    PACE_FLAGS,
    PACE_FIELDS,
};

enum { LCD_OFF, LCD_ON, LCD_GET };

static uint32_t cookie;
static unsigned int operation, argument;
static uint32_t reply[4] = {1};

static bool submit(unsigned int op, const uint8_t *data)
{
    unsigned int arg = data[4];
    if (op < SHELL_LCD_TIMING ||
        (op > SHELL_LCD_DIAG && op != SHELL_LCD_PACE_STATUS &&
         op != SHELL_LCD_ASYNC) ||
        (op == SHELL_LCD_TIMING && arg && !lcd_te_debug_valid_timing(arg)) ||
        ((op == SHELL_LCD_SYNC || op == SHELL_LCD_PACING ||
          op == SHELL_LCD_ASYNC) &&
         arg > LCD_GET) ||
        (op == SHELL_LCD_DIAG && arg > 1) ||
        (op == SHELL_LCD_PACE_STATUS && arg >= PACE_FIELDS) ||
        sm1_test_submitted != sm1_test_completed || sm1_test_record_active())
        return false;

    memcpy(&cookie, data, sizeof(cookie));
    if (!cookie)
        return false;
    operation = op;
    argument = arg;
    ++sm1_test_submitted;
    if (button_queue_try_post(SYS_SM1_TEST_CONTROL, SHELL_LCD_EVENT))
        return true;

    --sm1_test_submitted;
    return false;
}

void shell_lcd_request(const struct usb_ctrlrequest *req, uint8_t *buffer,
                       bool allowed)
{
    if (allowed && req->bRequestType == 0xc0 && req->wValue == 0 &&
        req->wLength == sizeof(reply))
    {
        int oldlevel = disable_irq_save();
        memcpy(buffer, reply, sizeof(reply));
        restore_irq(oldlevel);
        usb_core_control_response(USB_CONTROL_ACK, buffer, sizeof(reply));
        return;
    }
    if (allowed && req->bRequestType == 0x40 && req->wLength == 5 &&
        submit(req->wValue, buffer))
    {
        usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
        return;
    }
    usb_core_control_response(USB_CONTROL_STALL, NULL, 0);
}

static unsigned int pace_field(unsigned int field)
{
    struct ui_pace_status status;
    ui_pace_get_status(&status);
    switch (field)
    {
        case PACE_PERIOD:
            return status.period;
        case PACE_DIVISOR:
            return status.divisor;
        case PACE_SHOWN:
            return status.shown;
        case PACE_MISSED:
            return status.missed;
        case PACE_OFFSET:
            return status.offset;
        case PACE_COST:
            return status.cost;
        case PACE_DMA:
            return status.dma;
    }
    return status.enabled | status.measured << 1 | status.phased << 2;
}

static int dispatch(unsigned int *value)
{
    if (!usb_podlink_active() || usb_exclusive_storage())
        return ENOTCONN;

    /* Pacing falls back to the nominal refresh on any LCD. */
    switch (operation)
    {
        case SHELL_LCD_PACING:
            if (argument != LCD_GET)
                ui_te_pacing = argument == LCD_ON;
            *value = ui_te_pacing;
            return 0;
        case SHELL_LCD_PACE_STATUS:
            *value = pace_field(argument);
            return 0;
    }

    if (!lcd_te_supported())
        return ENOTSUP;

    switch (operation)
    {
        case SHELL_LCD_TIMING:
            if (argument && !lcd_te_set_bus_timing(argument))
                return EBUSY;
            *value = lcd_te_bus_timing();
            return 0;
        case SHELL_LCD_SYNC:
            if (argument != LCD_GET)
                lcd_te_set_enabled(argument == LCD_ON);
            *value = lcd_te_enabled();
            return argument == LCD_ON && !*value ? ENODATA : 0;
        case SHELL_LCD_ASYNC:
            if (argument != LCD_GET)
                lcd_te_set_async_enabled(argument == LCD_ON);
            *value = lcd_te_async_enabled();
            return 0;
        case SHELL_LCD_DIAG:
            return lcd_te_debug_save_report(argument);
    }
    return EINVAL;
}

void shell_lcd_dispatch(void)
{
    unsigned int value = 0;
    int error = dispatch(&value);

    int oldlevel = disable_irq_save();
    reply[1] = cookie;
    reply[2] = error;
    reply[3] = value;
    ++sm1_test_completed;
    restore_irq(oldlevel);
}
