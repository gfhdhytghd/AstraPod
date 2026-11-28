/* Defer optional Classic rescans without holding the filesystem lock.
 * GPL version 2+. */
#ifdef HAVE_MOTION_SCHED
static bool dircache_defer_motion(const struct queue_event *ev)
{
    if (!motion_sched_active())
        return false;

    queue_post(&dircache_queue, ev->id, ev->data);
    dircache_unlock();
    sleep(1);
    dircache_lock();
    return true;
}
#endif
