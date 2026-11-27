/* Idle screen scheduling for the iPod Classic. GPL version 2+. */
#include "config.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"
#include "plugins/rock_compat.h"
#include "settings/settings.h"
#include "ui/animation.h"
#include "ui/frame.h"
#include "ui/idle.h"

void ui_idle_list_present(void)
{
#ifdef IPOD_6G
    if (!rock_ui_raw())
    {
        ui_flush_dirty();
        return;
    }
#endif
    lcd_update();
}

int ui_idle_status_timeout(uint32_t start, unsigned frames)
{
    uint32_t elapsed = USEC_TIMER - start;
    int ticks = ui_clock_poll_timeout(elapsed, frames, HZ);
#ifdef IPOD_6G
    if (ticks == TIMEOUT_NOBLOCK)
    {
        /* The action loop has already sampled the current frame. A partial
         * tick must wait for the next pose instead of polling at full speed. */
        uint32_t next = start + ui_frame_clock(ui_clock_frame(elapsed) + 1);
        ticks = ui_clock_wait_until(next);
        ticks = MAX(1, ticks);
    }
#endif
    return ticks;
}

int ui_idle_wps_timeout(bool remote, bool settled, int meter_ticks)
{
    if (!lcd_active())
    {
#ifdef IPOD_6G
        if (!remote && !global_settings.caption_backlight)
            return HZ / 2;
#endif
        return HZ / 5;
    }
#ifdef IPOD_6G
    if (!remote && settled && !global_settings.caption_backlight)
    {
        /* Steps of a steady progress bar need at most one panel slot.
         * Keep the spectrum sampler's existing tick deadline. */
        uint32_t period = ui_frame_period();
        if (meter_ticks > 0 && (unsigned)meter_ticks <= period / (1000000 / HZ))
            return meter_ticks;
        int ticks = ui_clock_wait_until(USEC_TIMER + period);
        return MAX(1, ticks);
    }
#else
    (void)remote;
    (void)settled;
    (void)meter_ticks;
#endif
    return 1;
}

int ui_idle_cover_boost_tail(void)
{
#ifdef IPOD_6G
    /* Bridge bursts of completed art loads after the last moving frame. */
    return HZ / 8;
#else
    return HZ;
#endif
}

int ui_idle_cover_timeout(int timeout, long boost_until, bool boosted)
{
#ifdef IPOD_6G
    if (boosted)
    {
        long left = boost_until - current_tick;
        return MIN(timeout, MAX(0, left));
    }
#else
    (void)boost_until;
    (void)boosted;
#endif
    return timeout;
}
