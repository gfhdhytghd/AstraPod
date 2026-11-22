/* Foreground MPEGPlayer controls. GNU GPL version 2 or later. */
#include "plugin.h"
#include "mpegplayer.h"
#include "usb.h"

void mpeg_yuv_blit(uint8_t *const *planes, int sx, int sy, int stride,
                   int x, int y, int width, int height)
{
#ifdef HAVE_HW_H264
    rb->lcd_blit_yuv(planes, sx, sy, stride, x, y, width, height);
#else
    /* SDL has no target YUV blitter. Convert into a bounded RGB565 row. */
    fb_data row[LCD_WIDTH];
    for (int j = 0; j < height; ++j)
    {
        int luma = (sy + j) * stride;
        int chroma = ((sy + j) / 2) * (stride / 2);
        for (int i = 0; i < width; ++i)
        {
            int s = sx + i;
            int yy = MAX(planes[0][luma + s] - 16, 0) * 298;
            int u = planes[1][chroma + s / 2] - 128;
            int v = planes[2][chroma + s / 2] - 128;
            row[i] = LCD_RGBPACK(MIN(MAX((yy + 409*v + 128) >> 8, 0), 255),
                                MIN(MAX((yy - 100*u - 208*v + 128) >> 8, 0), 255),
                                MIN(MAX((yy + 516*u + 128) >> 8, 0), 255));
        }
        rb->lcd_bitmap_part(row, 0, 0, width, x, y + j, width, 1);
    }
    rb->lcd_update_rect(x, y, width, height);
#endif
}

static long play(void)
{
    rb->backlight_set_timeout(0);
    rb->backlight_set_timeout_plugged(0);
    rb->backlight_on();
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    rb->lcd_update();
    stream_show_vo(true);
    if (stream_play() < STREAM_OK)
        return BUTTON_NONE;

    while (stream_status() != STREAM_STOPPED && mpeg_pcm_pump())
    {
        mpeg_stream_poll();
        long button = rb->button_get_w_tmo(1);
        switch (button)
        {
        case BUTTON_MENU:
            return BUTTON_NONE;
        case BUTTON_PLAY | BUTTON_REL:
            if (stream_status() == STREAM_PLAYING)
                stream_pause();
            else
                stream_resume();
            break;
        case BUTTON_LEFT:
        case BUTTON_RIGHT:
        case BUTTON_LEFT | BUTTON_REPEAT:
        case BUTTON_RIGHT | BUTTON_REPEAT:
            if (stream_can_seek())
            {
                int step = (button & BUTTON_LEFT) ? -5 : 5;
                if (stream_seek(step * TS_SECOND, SEEK_CUR) >= STREAM_OK &&
                    stream_status() == STREAM_PAUSED)
                    stream_draw_frame(false);
            }
            break;
        case BUTTON_MULTIMEDIA_VOLUME_DOWN:
#ifdef BUTTON_RC_VOL_DOWN
        case BUTTON_RC_VOL_DOWN:
        case BUTTON_RC_VOL_DOWN | BUTTON_REPEAT:
#endif
        case BUTTON_SCROLL_BACK:
        case BUTTON_SCROLL_BACK | BUTTON_REPEAT:
            rb->adjust_volume(-1);
            break;
        case BUTTON_MULTIMEDIA_VOLUME_UP:
#ifdef BUTTON_RC_VOL_UP
        case BUTTON_RC_VOL_UP:
        case BUTTON_RC_VOL_UP | BUTTON_REPEAT:
#endif
        case BUTTON_SCROLL_FWD:
        case BUTTON_SCROLL_FWD | BUTTON_REPEAT:
            rb->adjust_volume(1);
            break;
        case SYS_USB_CONNECTED:
#ifdef HAVE_USB_CONFIRM
        case SYS_USB_CONFIRM:
#endif
        case SYS_POWEROFF:
        case SYS_REBOOT:
            return button;
        }
        rb->reset_poweroff_timer();
    }
    return BUTTON_NONE;
}

enum plugin_status plugin_start(const void *parameter)
{
    char path[MAX_PATH];
    if (parameter)
    {
        if (rb->strlcpy(path, parameter, sizeof(path)) >= sizeof(path))
            return PLUGIN_ERROR;
    }
    else
    {
        int picked = rb->pick_file(path, sizeof(path), "/", "Choose an MPEG video",
                                   ".mpg,.mpeg,.m2v,.mpv", 0, false);
        if (picked <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
    }

    if (rb->audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        rb->audio_stop();
    if (!rb->mpeg->set_drain(stream_exit))
        return PLUGIN_ERROR;

    int result = stream_init();
    long event = BUTTON_NONE;
    if (result >= STREAM_OK)
    {
        result = stream_open(path);
        if (result >= STREAM_OK)
            event = play();
    }

    stream_exit();
    rb->mpeg->set_drain(NULL);

    if (event != BUTTON_NONE)
        return rb->default_event_handler(event) == SYS_USB_CONNECTED ?
            PLUGIN_USB_CONNECTED : PLUGIN_OK;
    if (result < STREAM_OK)
        rb->splash(HZ * 2, result == STREAM_UNSUPPORTED ?
                   "Unsupported MPEG video" : "Could not open MPEG video");
    return result < STREAM_OK ? PLUGIN_ERROR : PLUGIN_OK;
}
