/* SPDX-License-Identifier: GPL-2.0-or-later */

static volatile bool detent_mode;
static int detent_remainder;
static int touch_threshold, touch_sum;

void wheel_set_detent_mode(bool enabled)
{
    if (detent_mode == enabled)
        return;
    int level = disable_irq_save();
    detent_mode = enabled;
    touch_threshold = touch_sum = 0;
    accumulated_wheel_delta = wheel_velocity = wheel_repeat = 0;
    old_wheel_value = wheel_position;
    restore_irq(level);
}

/* One turn has 96 positions. Keep the signed division remainder across
 * reversals and touches. The first small movement takes up the touch band. */
static void post_wheel_detents(int position)
{
    if (!wheel_is_touched || wheel_position < 0)
    {
        touch_threshold = 6;
        touch_sum = 0;
        return;
    }

    int delta = position - wheel_position;
    if (delta > 72)
        delta -= WHEELCLICKS_PER_ROTATION;
    else if (delta < -72)
        delta += WHEELCLICKS_PER_ROTATION;
    if (abs(delta) < touch_threshold)
    {
        touch_sum += delta;
        if (abs(touch_sum) >= WHEEL_SENSITIVITY)
            touch_threshold = 0;
        return;
    }
    if (abs(delta) >= 36)
        return;

    touch_sum = 0;
    detent_remainder += delta;
    int steps = detent_remainder / WHEEL_SENSITIVITY;
    if (steps)
    {
        detent_remainder -= steps * WHEEL_SENSITIVITY;
        int key = steps > 0 ? BUTTON_SCROLL_FWD : BUTTON_SCROLL_BACK;
        /* The cover browser reads the physical count in bits 24..30.
         * No velocity component is supplied. */
        unsigned data = (1u << 31) | ((unsigned)abs(steps) << 24);
        button_queue_post(key, data);
    }
}

static inline bool wheel_handle_detents(int position, unsigned long usec)
{
    if (!(detent_mode && send_events))
        return false;

    post_wheel_detents(position);
    wheel_is_touched = true;
    wheel_position = position;
    last_wheel_usec = usec;
    return true;
}
