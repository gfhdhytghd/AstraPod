/* Foreground plugin support. GNU GPL version 2 or later. */

#ifdef ASTRAPOD_ROCK_COMPAT
static bool ticks_reserved;
static volatile unsigned int pending_ticks;
#endif

#ifdef ASTRAPOD_ROCK_COMPAT
static void count_tick(void)
{
    ++pending_ticks;
}

bool timer_reserve_ticks(long cycles)
{
    int irq = disable_irq_save();
    bool ok = false;
    if (!ticks_reserved && timer_prio == -1 && cycles > 0 &&
        timer_set(cycles, true))
    {
        pending_ticks = 0;
        pfn_timer = count_tick;
        ticks_reserved = timer_start();
        if (!ticks_reserved)
        {
            timer_stop();
            pfn_timer = NULL;
        }
        ok = ticks_reserved;
    }
    restore_irq(irq);
    return ok;
}

unsigned int timer_take_ticks(void)
{
    int irq = disable_irq_save();
    unsigned int ticks = pending_ticks;
    pending_ticks = 0;
    restore_irq(irq);
    return ticks;
}

void timer_release_ticks(void)
{
    int irq = disable_irq_save();
    if (ticks_reserved)
    {
        timer_stop();
        pfn_timer = NULL;
        pending_ticks = 0;
        ticks_reserved = false;
    }
    restore_irq(irq);
}
#endif
