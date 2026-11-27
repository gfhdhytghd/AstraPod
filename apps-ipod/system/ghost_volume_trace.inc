/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ghost-volume-trace.h"
#include "powermgmt.h"

#define GHOST_TRACE_SIZE 128

static struct ghost_volume_record {
    unsigned long tick;
    enum ghost_volume_event event;
    uint32_t a, b, c;
} ghost_records[GHOST_TRACE_SIZE];
static unsigned long ghost_head, ghost_written;

void ghost_volume_trace(enum ghost_volume_event event,
                        uint32_t a, uint32_t b, uint32_t c)
{
    if (!global_settings.debug_log_usb)
        return;

    int level = disable_irq_save();
    ghost_records[ghost_head++ % GHOST_TRACE_SIZE] =
        (struct ghost_volume_record){ current_tick, event, a, b, c };
    restore_irq(level);
}

void ghost_volume_trace_flush(void)
{
    static const char * const names[GHOST_VOLUME_EVENT_COUNT] = {
        [GHOST_BUTTON_SOURCES] = "sources wheel serial mikey",
        [GHOST_MIKEY_VOLUME_EDGE] = "mikey raw-btn raw-event decoded",
        [GHOST_VOLUME_EVENT] = "volume-event key action before-db",
        [GHOST_VOLUME_SET] = "volume-set stored-db applied-db caller",
        [GHOST_PLAY_POWEROFF] = "play-poweroff key repeat",
        [GHOST_INLINE_POWEROFF_BLOCKED] = "inline-poweroff-blocked key repeat",
        [GHOST_BATTERY_POWEROFF_DISK] = "battery-poweroff-disk mv percent",
        [GHOST_BATTERY_POWEROFF] = "battery-poweroff mv percent",
        [GHOST_IDLE_POWEROFF] = "idle-poweroff audio-status",
        [GHOST_SLEEP_POWEROFF] = "sleep-poweroff",
        [GHOST_POWEROFF_REQUEST] = "poweroff-request caller",
        [GHOST_REBOOT_REQUEST] = "reboot-request caller",
        [GHOST_CLEAN_SHUTDOWN] = "clean-shutdown type mv percent",
        [GHOST_FORCED_POWEROFF] = "forced-poweroff",
    };

    if (!global_settings.debug_log_usb || !battery_level_safe())
        return;
    int level = disable_irq_save();
    bool pending = ghost_written != ghost_head;
    restore_irq(level);
    if (!pending)
        return;

    int fd = debug_log_open(DEBUG_LOG_USB);
    if (fd < 0)
        return;

    level = disable_irq_save();
    unsigned long end = ghost_head;
    while (ghost_written != end)
    {
        if (ghost_head - ghost_written > GHOST_TRACE_SIZE)
        {
            unsigned long lost = ghost_head - ghost_written - GHOST_TRACE_SIZE;
            ghost_written += lost;
            end = ghost_head;
            restore_irq(level);
            debug_log_lost(DEBUG_LOG_USB, fd, lost, "volume trace overflow");
            level = disable_irq_save();
            continue;
        }
        struct ghost_volume_record r =
            ghost_records[ghost_written % GHOST_TRACE_SIZE];
        restore_irq(level);
        if (logfile_printf(fd, "[%8lu] GHOST %s %08lx %08lx %08lx\n",
                           r.tick, names[r.event], (unsigned long)r.a,
                           (unsigned long)r.b, (unsigned long)r.c) < 0)
        {
            debug_log_close(DEBUG_LOG_USB, fd);
            return;
        }
        level = disable_irq_save();
        ghost_written++;
    }
    restore_irq(level);
    debug_log_close(DEBUG_LOG_USB, fd);
}
