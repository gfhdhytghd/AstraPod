/* Disk Tidy filesystem and display adapters. GPL-2.0-or-later. */

static bool finish_file(int fd, const char *temp, const char *path, bool ok)
{
    if (ok && rb->file_sync(fd) < 0)
        ok = false;
    if (rb->close(fd) < 0)
        ok = false;
    if (ok && rb->rename(temp, path) == 0)
        return true;
    rb->remove(temp);
    return false;
}

static bool save_run_stats(void)
{
    const char *temp = LAST_RUN_STATS_FILE ".tmp";
    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return false;

    bool ok = rb->write(fd, &run_stats, sizeof(run_stats)) ==
              sizeof(run_stats);
    return finish_file(fd, temp, LAST_RUN_STATS_FILE, ok);
}

static bool load_run_stats(void)
{
    int fd = rb->open(LAST_RUN_STATS_FILE, O_RDONLY);
    if (fd < 0)
        return false;

    bool ok = rb->ffilesize(fd) == sizeof(run_stats) &&
              rb->read(fd, &run_stats, sizeof(run_stats)) ==
              sizeof(run_stats);
    if (rb->close(fd) < 0)
        ok = false;
    return ok && run_stats.files_removed >= 0 &&
           run_stats.dirs_removed >= 0 && run_stats.run_duration >= 0 &&
           run_stats.removed_size >= 0 && run_stats.removed_size < 0x1p64;
}

static enum plugin_status display_run_stats(void)
{
    if (!load_run_stats())
    {
        rb->splash(HZ * 2, "Unable to load last run stats");
        return PLUGIN_OK;
    }

    char date[48] = "";
#if CONFIG_RTC
    const struct tm *tm = &run_stats.last_run_time;
    if (tm->tm_year >= 0 && tm->tm_year <= 8099 &&
        tm->tm_mon >= 0 && tm->tm_mon < 12 &&
        tm->tm_mday >= 1 && tm->tm_mday <= 31 &&
        tm->tm_hour >= 0 && tm->tm_hour < 24 &&
        tm->tm_min >= 0 && tm->tm_min < 60)
        rb->snprintf(date, sizeof(date), "%04d-%02d-%02d %02d:%02d\n\n",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min);
#endif
    char summary[192];
    rb->snprintf(summary, sizeof(summary),
                 "%s%d files removed\n%d directories removed\n"
                 "%llu bytes removed\n%d seconds",
                 date, run_stats.files_removed, run_stats.dirs_removed,
                 (unsigned long long)run_stats.removed_size,
                 run_stats.run_duration);
    return rb->view_text("Last Run Stats", summary);
}

/* Directory scan. */

struct tidy_frame {
    int path_length;
    unsigned int visited;
    bool remove;
};

/* An absolute path uses a slash and at least one byte per nested component. */
static struct tidy_frame frames[MAX_PATH / 2 + 1];

static bool protected_path(const char *path)
{
    return !rb->strcasecmp(path, ROCKBOX_DIR) ||
           !rb->strcasecmp(path, "/rockbox.ipod");
}

static enum plugin_status tidy_clean(char *path)
{
    int depth = 0;
    DIR *dir = NULL;
    unsigned int replay = 0;
    enum plugin_status status = PLUGIN_ERROR;
    long next_tick = *rb->current_tick;
    frames[0] = (struct tidy_frame){ .path_length = 1 };

    for (;;)
    {
        int key = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
        if (key == ACTION_STD_CANCEL || key == ACTION_STD_MENU)
        {
            user_abort = true;
            status = PLUGIN_OK;
            break;
        }
        if (rb->default_event_handler(key) == SYS_USB_CONNECTED)
        {
            status = PLUGIN_USB_CONNECTED;
            break;
        }
        rb->yield();

        if (!TIME_BEFORE(*rb->current_tick, next_tick))
        {
            rb->splash_progress(0, 1, "Cleaning - Menu to cancel\n%s", path);
            next_tick = *rb->current_tick + HZ / 8;
        }

        struct tidy_frame *frame = &frames[depth];
        if (!dir)
        {
            dir = rb->opendir(path);
            if (!dir)
                break;
            replay = frame->visited;
        }

        struct DIRENT *entry;
        struct dirinfo info;
        int result = rb->dir_next(dir, &entry, &info);
        if (result < 0 || (result == 0 && replay))
            break;
        if (replay)
        {
            --replay;
        }
        else if (result == 0)
        {
            int closed = rb->closedir(dir);
            dir = NULL;
            if (closed < 0)
                break;
            if (frame->remove)
            {
                if (run_stats.dirs_removed == INT_MAX ||
                    rb->rmdir(path) < 0)
                    break;
                run_stats.dirs_removed++;
            }
            if (!depth)
                return PLUGIN_OK;
            path[frames[--depth].path_length] = '\0';
        }
        else if (info.attribute & (ATTR_VOLUME | ATTR_LINK))
        {
            frame->visited++;
        }
        else
        {
            bool is_dir = (info.attribute & ATTR_DIRECTORY) != 0;
            bool remove = frame->remove ||
                          tidy_remove_item(entry->d_name, info.attribute);
            int length = frame->path_length;
            if (!is_dir && !remove)
            {
                frame->visited++;
                continue;
            }

            int written = rb->snprintf(path + length, MAX_PATH - length,
                                       "%s%s", length == 1 ? "" : "/",
                                       entry->d_name);
            if (written < 0 || written >= MAX_PATH - length)
            {
                path[length] = '\0';
                break;
            }

            if (protected_path(path))
            {
                frame->visited++;
                path[length] = '\0';
            }
            else if (is_dir)
            {
                if (depth + 1 >= (int)ARRAYLEN(frames))
                    break;

                /* Reopen the parent after the child, skipping entries still
                 * present. The removed child consumes no replay position. */
                if (!remove)
                    frame->visited++;
                int closed = rb->closedir(dir);
                dir = NULL;
                if (closed < 0)
                    break;
                frames[++depth] = (struct tidy_frame) {
                    .path_length = length + written, .remove = remove
                };
            }
            else
            {
                if (run_stats.files_removed == INT_MAX ||
                    rb->remove(path) < 0)
                    break;
                run_stats.files_removed++;
                run_stats.removed_size += sizeof(info.size) == sizeof(uint32_t)
                                        ? (uint32_t)info.size
                                        : (uint64_t)info.size;
                path[length] = '\0';
            }
        }
    }

    if (dir && status != PLUGIN_USB_CONNECTED)
        rb->closedir(dir);
    return status;
}
