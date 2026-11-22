/* Foreground USB HID profile. GNU GPL version 2 or later. */
#include "system.h"
#include "usb.h"
#include "usb_core.h"
#include "usb_hid.h"
#include "usb_plugin_hid.h"

enum hid_lease {
    HID_OFF,
    HID_ARMED,
    HID_ACTIVE,
    HID_CABLE_HELD,
};

static enum hid_lease lease;
static struct {
    usage_page_t page;
    int id;
} request;

/* The USB policy caller holds interrupts while it checks the cable. */
bool usb_plugin_hid_arm(void)
{
    if (lease != HID_OFF)
        return false;
    lease = HID_ARMED;
    return true;
}

void usb_plugin_hid_close(void)
{
    int level = disable_irq_save();
    if (lease == HID_ARMED)
        lease = HID_OFF;
    else if (lease == HID_ACTIVE)
        lease = HID_CABLE_HELD;
    request.id = 0;
    restore_irq(level);
}

void usb_plugin_hid_insert(void)
{
    if (lease == HID_ARMED)
        lease = HID_ACTIVE;
}

void usb_plugin_hid_detach(void)
{
    if (usb_plugin_hid_latched())
        lease = HID_OFF;
    request.id = 0;
}

bool usb_plugin_hid_reserved(void)
{
    return lease != HID_OFF;
}

bool usb_plugin_hid_latched(void)
{
    return lease >= HID_ACTIVE;
}

static bool can_send(void)
{
    return lease == HID_ACTIVE && usb_detect() == USB_INSERTED &&
           usb_core_plugin_hid_ready() && usb_hid_plugin_idle();
}

bool usb_plugin_hid_send(usage_page_t page, int id)
{
    if ((page != HID_USAGE_PAGE_KEYBOARD_KEYPAD &&
         page != HID_USAGE_PAGE_CONSUMER) || id <= 0 || id > UINT8_MAX)
        return false;

    int level = disable_irq_save();
    bool accepted = can_send() && request.id == 0;
    if (accepted)
    {
        request.page = page;
        request.id = id;
    }
    restore_irq(level);
    if (accepted)
        usb_signal_class_notify(USB_DRIVER_HID, 0);
    return accepted;
}

void usb_plugin_hid_deliver(void)
{
    int level = disable_irq_save();
    int id = request.id;
    request.id = 0;
    if (id && can_send())
        usb_hid_plugin_send(request.page, id);
    restore_irq(level);
}

void usb_plugin_hid_flush(void)
{
    request.id = 0;
}
