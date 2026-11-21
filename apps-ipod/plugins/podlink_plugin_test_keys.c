/* Click wheel input for shell-launched plugins. GNU GPL version 2 or later. */
#include "podlink-plugin-test.h"
#include "button.h"
#include "button_usb_remote.h"
#include "kernel.h"
#include "system.h"

static const int keys[] = {
    [USB_REMOTE_KEY_MENU] = BUTTON_MENU,
    [USB_REMOTE_KEY_SELECT] = BUTTON_SELECT,
    [USB_REMOTE_KEY_LEFT] = BUTTON_LEFT,
    [USB_REMOTE_KEY_RIGHT] = BUTTON_RIGHT,
    [USB_REMOTE_KEY_PLAY] = BUTTON_PLAY,
};

static struct
{
    bool held;
    unsigned long pressed_at;
    unsigned long until;
} press[ARRAYLEN(keys)];

/* Callers serialize this state with IRQs off, including the button tick. */
bool podlink_plugin_test_key(unsigned key, unsigned edge)
{
    if (key == USB_REMOTE_KEY_VOL_UP || key == USB_REMOTE_KEY_VOL_DOWN)
    {
        int wheel = key == USB_REMOTE_KEY_VOL_UP ? BUTTON_SCROLL_BACK : BUTTON_SCROLL_FWD;
        /* One click-wheel detent, with no acceleration. Volume uses 1/2. */
        return edge == 2 || button_queue_try_post(wheel, (1u << 31) | (1u << 24));
    }
    if (key >= ARRAYLEN(keys) || !keys[key])
        return false;

    unsigned long now = current_tick;
    if (edge)
    {
        if (!press[key].held || !TIME_BEFORE(now, press[key].until))
            press[key].pressed_at = now;
        press[key].held = true;
        press[key].until = now + HZ;
    }
    else if (press[key].held)
    {
        /* Ensure even consecutive OUT transfers pass the two-sample debounce. */
        unsigned long minimum = press[key].pressed_at + (HZ + 19) / 20;
        press[key].until = TIME_BEFORE(now, minimum) ? minimum : now;
    }
    return true;
}

int podlink_plugin_test_buttons(void)
{
    int buttons = 0;
    for (unsigned key = 0; key < ARRAYLEN(keys); key++)
    {
        if (press[key].held && TIME_BEFORE(current_tick, press[key].until))
            buttons |= keys[key];
        else
            press[key].held = false;
    }
    return buttons;
}

void podlink_plugin_test_clear_keys(void)
{
    int level = disable_irq_save();
    for (unsigned key = 0; key < ARRAYLEN(keys); key++)
        press[key].held = false;
    restore_irq(level);
}
