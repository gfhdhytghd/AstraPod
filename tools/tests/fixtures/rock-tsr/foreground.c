/* Overwrite foreground scratch while another module remains resident. */
#include "plugin.h"

static void unexpected_tick(enum rock_tsr_event event)
{
    (void)event;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t bytes;
    void *scratch = rb->plugin_get_buffer(&bytes);
    if (!scratch || !bytes || rb->rock_tsr_register(unexpected_tick, 1))
        return PLUGIN_ERROR;
    rb->memset(scratch, 0x5a, bytes);

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "Foreground overwrite ready");
    rb->lcd_update();
#if CONFIG_BINFMT == BINFMT_DLOPEN && !defined(_WIN32)
    fprintf(stderr, "TSR foreground overwritten\n");
#endif
    while (true)
    {
        long button = rb->button_get(true);
        long event = rb->default_event_handler(button);
        if (event == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (event == SYS_POWEROFF || event == SYS_REBOOT)
            return PLUGIN_POWEROFF;
        if (button == (BUTTON_MENU | BUTTON_REL))
        {
#if CONFIG_BINFMT == BINFMT_DLOPEN && !defined(_WIN32)
            fprintf(stderr, "TSR foreground exit\n");
#endif
            return PLUGIN_OK;
        }
    }
}
