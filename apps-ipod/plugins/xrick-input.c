/* XRick foreground input and USB handoff. GNU GPL version 2 or later. */
#include "plugin.h"
#include "xrick-compat.h"
#include "xrick/control.h"
#include "xrick/draw.h"
#include "xrick/system/sysmenu_rockbox.h"

static void close_stream(void *unused)
{
    (void)unused;
    syssnd_shutdown();
}

bool xrick_usb_event(long event)
{
    return rb->default_event_handler_ex(event, close_stream, NULL) ==
           SYS_USB_CONNECTED;
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

/* The wheel only posts pulses, so each pulse holds Up or Down briefly. */
static long up_until, down_until;

void sysevt_poll(void)
{
    for (unsigned i = 0; i < 8; i++)
    {
        long event = rb->button_get(false);
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
        else if (event == (BUTTON_MENU | BUTTON_REL))
            sysmenu_exec();
        else if (event == (BUTTON_PLAY | BUTTON_REL))
            control_set(Control_PAUSE); /* Present a press for one frame. */
        else if ((event & ~BUTTON_REPEAT) == BUTTON_SCROLL_BACK)
            up_until = *rb->current_tick + HZ / 6;
        else if ((event & ~BUTTON_REPEAT) == BUTTON_SCROLL_FWD)
            down_until = *rb->current_tick + HZ / 6;
        if (control_test(Control_EXIT))
            return;
    }

    long now = *rb->current_tick;
    int held = rb->button_status();
    control_clear(Control_UP | Control_DOWN | Control_LEFT | Control_RIGHT |
                  Control_FIRE);
    if (TIME_BEFORE(now, up_until))
        control_set(Control_UP);
    if (TIME_BEFORE(now, down_until))
        control_set(Control_DOWN);
    if (held & BUTTON_LEFT)
        control_set(Control_LEFT);
    if (held & BUTTON_RIGHT)
        control_set(Control_RIGHT);
    if (held & BUTTON_SELECT)
        control_set(Control_FIRE);
}

bool xrick_load_interrupted(void)
{
    long event = rb->button_get(false);
    if (event == BUTTON_NONE || remote_volume(event))
        return false;
    return xrick_usb_event(event);
}
