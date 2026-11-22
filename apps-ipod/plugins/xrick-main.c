/* XRick entry with a missing-data screen. GNU GPL version 2 or later. */
#include "plugin.h"
#include "xrick/game.h"
#include "xrick/system/system.h"

#define XRICK_FILELIST ROCKBOX_DIR "/xrick/filelist.dat"

static enum plugin_status missing_data(void)
{
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_clear_display();
    rb->lcd_setfont(FONT_UI);
    rb->lcd_puts(0, 0, "Missing XRick data");
    rb->lcd_puts(0, 2, "Copy xrick-data.zip contents");
    rb->lcd_puts(0, 3, "to /.rockbox/xrick/");
    rb->lcd_puts(0, 5, "Menu: return");
    rb->lcd_update();
    while (true)
    {
        int action = rb->get_action(CONTEXT_STD, HZ / 10);
        if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            return PLUGIN_OK;
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    if (!rb->file_exists(XRICK_FILELIST))
        return missing_data();

    /* The PCM stream needs the built-in sink, like upstream's audio buffer. */
    if (rb->audio_status())
        rb->audio_stop();

    char *data = NULL;
    bool success = sys_init(1, &data) && game_run();
    sys_shutdown();
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    return success ? PLUGIN_OK : PLUGIN_ERROR;
}
