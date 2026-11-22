/* Probe executable TSR residency. GPL version 2 or later. */
#include "plugin.h"

static volatile unsigned int ticks;
static volatile enum rock_tsr_event last_stop;
static unsigned int cookie = 0x386b;

#if CONFIG_BINFMT == BINFMT_DLOPEN && !defined(_WIN32)
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
static void __attribute__((destructor)) closed(void)
{
    TRACE("TSR image closed\n");
}
#else
#define TRACE(...) do { } while (0)
#endif

static void pulse(enum rock_tsr_event event)
{
    if (event == ROCK_TSR_TICK)
    {
        if (++ticks == 1)
        {
            TRACE("TSR tick enter\n");
            rb->sleep(HZ / 2);
            TRACE("TSR tick leave\n");
        }
        TRACE("TSR tick %u\n", ticks);
        return;
    }

    last_stop = event;
    TRACE("TSR stop %d ticks %u\n", event, ticks);
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    if (ticks || cookie != 0x386b || rb->rock_tsr_register(NULL, 1) ||
        rb->rock_tsr_register(pulse, 0) ||
        !rb->rock_tsr_register(pulse, HZ / 10) ||
        rb->rock_tsr_register(pulse, 1))
        return PLUGIN_ERROR;

    cookie = 0;
    TRACE("TSR entry\n");
    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "TSR probe ready");
    rb->lcd_update();
    while (true)
    {
        long button = rb->button_get(true);
        long event = rb->default_event_handler(button);
        if (event == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (event == SYS_POWEROFF || event == SYS_REBOOT)
            return PLUGIN_POWEROFF;
        if (button == (BUTTON_MENU | BUTTON_REL))
            return PLUGIN_OK;
        if (button == (BUTTON_SELECT | BUTTON_REL))
            return PLUGIN_ERROR;
    }
}
