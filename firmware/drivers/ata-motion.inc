/* Dispatch iPod Classic bulk ATA I/O at command boundaries. GPL version 2+. */
static int ata_transfer_sectors(uint64_t start, int count, void *buffer,
                                int write);

static void ata_motion_lock(void)
{
    for (;;)
    {
        while (motion_sched_defer_io())
            sleep(1);

        mutex_lock(&ata_mutex);
        if (!motion_sched_defer_io())
            return;

        mutex_unlock(&ata_mutex);
    }
}

static int ata_motion_bulk(uint64_t start, int count, void *buffer, int write)
{
    /* Keep partial-sector read/modify/write under one mutex acquisition.
     * Bound each background bulk command before allowing another waiter in. */
    if (!motion_sched_background_io() || count <= 0)
        return ata_transfer_sectors(start, count, buffer, write);

    int chunk = 128 * 1024 / log_sector_size;
    uint8_t *bytes = buffer;
    for (;;)
    {
        int sectors = MIN(chunk, count);
        int rc = ata_transfer_sectors(start, sectors, bytes, write);
        if (rc || count == sectors)
            return rc;

        start += sectors;
        count -= sectors;
        bytes += sectors * log_sector_size;
        mutex_unlock(&ata_mutex);
        ata_motion_lock();
    }
}
