/* Remote Control SDK binding. GNU GPL version 2 or later. */
#include "config.h"
#include "kernel.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_hid.h"
#include "plugins/rock_widgets.h"
#include "usbstack/usb_plugin_hid.h"
#include "widgets/splash.h"

int rock_hid_open(void)
{
#ifdef HAVE_USB_PLUGIN_HID
    if (!rock_usb_taken() && usb_plugin_hid_open())
        return ROCK_HID_ARMED;
    return ROCK_HID_CABLE_BUSY;
#else
    return ROCK_HID_UNAVAILABLE;
#endif
}

void rock_hid_close(void)
{
#ifdef HAVE_USB_PLUGIN_HID
    usb_plugin_hid_close();
#endif
}

void rock_hid_send(usage_page_t page, int id)
{
    if (rock_usb_taken())
        return;

#ifdef HAVE_USB_PLUGIN_HID
    if (usb_plugin_hid_send(page, id))
        return;
#else
    (void)page;
    (void)id;
#endif
    struct rock_native_scope scope = rock_modal_begin();
    splash(HZ, "USB HID is disconnected or unavailable");
    rock_modal_end(&scope);
}
