/* Exercise the foreground IRAM lease. GNU GPL version 2 or later. */
#include "plugin.h"

static volatile unsigned int seed ROCK_IRAM_DATA = 0x1234;
static volatile unsigned int count ROCK_IRAM_BSS;

static ROCK_IRAM_CODE unsigned int step(void)
{
    return ++count + seed++;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    if (step() != 0x1235 || step() != 0x1237 || rb->audio_status())
        return PLUGIN_ERROR;

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "IRAM probe ready");
    rb->lcd_puts(0, 1, "Menu: exit   Select: error");
    rb->lcd_puts(0, 2, "Play: try local playback");
    rb->lcd_update();
    for (;;)
    {
        long button = rb->button_get_w_tmo(HZ / 5);
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (button == BUTTON_MENU)
            return PLUGIN_OK;
        if (button == BUTTON_SELECT)
            return PLUGIN_ERROR;
        if (button == BUTTON_PLAY && rb->playlist_amount())
        {
            rb->playlist_resume_track(0, 0, 0, 0);
            if (rb->audio_status())
                return PLUGIN_ERROR;
            rb->lcd_puts(0, 3, "Playback blocked");
            rb->lcd_update();
        }
    }
}
