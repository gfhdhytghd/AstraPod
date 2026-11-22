/* Exercise asynchronous foreground resources across the loader boundary. */
#include "plugin.h"
#include "timer.h"
#include "usb.h"

static volatile unsigned int laps[2];
static unsigned int ticks;

static void worker(void *arg)
{
    volatile unsigned int *lap = arg;
    while (!rb->rock_worker_stopping())
    {
        (*lap)++;
        rb->sleep(1);
    }
}

static void tick(void *arg)
{
    unsigned int *count = arg;
    (*count)++;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t size;
    size_t stack_bytes = 8 * DEFAULT_STACK_SIZE;
    char *stack = rb->plugin_claim_scratch(2 * stack_bytes,
                                           2 * stack_bytes, &size);
    if (!stack || size != 2 * stack_bytes ||
        rb->plugin_claim_scratch(SIZE_MAX, SIZE_MAX, &size) || size)
        return PLUGIN_ERROR;
    if (!rb->rock_worker_start(worker, (void *)&laps[0], stack, stack_bytes) ||
        !rb->rock_worker_start(worker, (void *)&laps[1], stack + stack_bytes,
                               stack_bytes) ||
        rb->rock_worker_start(worker, NULL, stack, stack_bytes) ||
        !rb->rock_timer_start(TIMER_FREQ / 100, tick, &ticks))
        return PLUGIN_ERROR;

    rb->sleep(HZ / 5);
    if (!laps[0] || !laps[1] || rb->rock_timer_dispatch(2) != 2 || ticks != 2)
        return PLUGIN_ERROR;

    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "Execution probe ready");
    rb->lcd_update();
    while (1)
    {
        long button = rb->button_get_w_tmo(1);
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (button == (BUTTON_MENU | BUTTON_REL))
            return PLUGIN_OK;
        if (button == (BUTTON_SELECT | BUTTON_REL))
            return PLUGIN_ERROR;
    }
}
