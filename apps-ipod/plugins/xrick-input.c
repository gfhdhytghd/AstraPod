/* XRick foreground input and USB handoff. GNU GPL version 2 or later. */
#include "plugin.h"
#include "xrick-compat.h"
#include "xrick/control.h"
#include "xrick/draw.h"
#include "xrick/system/rockboxcodes.h"
#include "xrick/system/sysmenu_rockbox.h"

static void close_stream(void *unused)
{
    (void)unused;
    syssnd_shutdown();
}

bool xrick_usb_event(long event)
{
    return rb->default_event_handler_ex(event, close_stream, NULL) == SYS_USB_CONNECTED;
}

void xrick_redraw(void)
{
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    sysvid_update(&draw_SCREENRECT);
    rb->lcd_update();
}

static bool remote_volume(long event)
{
#ifdef BUTTON_RC_VOL_UP
    long key = event & ~(BUTTON_REPEAT | BUTTON_REL);
    if (key == BUTTON_RC_VOL_UP || key == BUTTON_RC_VOL_DOWN)
    {
        if (!(event & BUTTON_REL) || (event & BUTTON_REPEAT))
            rb->adjust_volume(key == BUTTON_RC_VOL_UP ? 1 : -1);
        return true;
    }
#else
    (void)event;
#endif
    return false;
}

void sysevt_poll(void)
{
    for (unsigned i = 0; i < 8; i++)
    {
        long event = rb->button_get_with_remote_volume();
        if (event == BUTTON_NONE)
            break;
        if (xrick_usb_event(event))
        {
            control_set(Control_EXIT);
            return;
        }
        if (remote_volume(event))
            continue;
        if (event == BUTTON_REDRAW)
            xrick_redraw();
        else if (event == XRICK_BTN_MENU)
            sysmenu_exec();
        else if (event == XRICK_BTN_PAUSE)
        {
            /* The wheel only posts pulses. Present a press for one frame. */
            control_set(Control_PAUSE);
        }
        if (control_test(Control_EXIT))
            return;
    }

    int held = rb->button_status();
    control_clear(Control_UP | Control_DOWN | Control_LEFT | Control_RIGHT | Control_FIRE);
    if (held & XRICK_BTN_UP)
        control_set(Control_UP);
    if (held & XRICK_BTN_DOWN)
        control_set(Control_DOWN);
    if (held & XRICK_BTN_LEFT)
        control_set(Control_LEFT);
    if (held & XRICK_BTN_RIGHT)
        control_set(Control_RIGHT);
    if (held & XRICK_BTN_FIRE)
        control_set(Control_FIRE);
}

bool xrick_load_interrupted(void)
{
    long event = rb->button_get_with_remote_volume();
    if (event == BUTTON_NONE || remote_volume(event))
        return false;
    return xrick_usb_event(event);
}
