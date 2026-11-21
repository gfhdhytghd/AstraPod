/* Native directory statistics for upstream stats. GPL version 2 or later. */
#include "files/filetypes.h"
#include "lib/mul_id3.h"

struct scan_frame {
    unsigned int entries;
    unsigned int files;
};

/* Each nested component consumes at least two bytes in an absolute path. */
static struct scan_frame frames[MAX_PATH / 2 + 1];
static struct dir_stats stats;

static bool has_extension(const char *ext, const char *const names[], size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!rb->strcasecmp(ext, names[i]))
            return true;
    return false;
}

static void count_file(const char *name, struct dirinfo info)
{
    stats.file_count++;
    stats.byte_count += sizeof(info.size) == sizeof(uint32_t)
                      ? (uint32_t)info.size : (uint64_t)info.size;

    int attr = rb->filetype_get_attr(name);
    const char *ext = rb->strrchr(name, '.');
    if (attr == FILE_ATTR_AUDIO)
        stats.audio_file_count++;
    else if (attr == FILE_ATTR_M3U)
        stats.m3u_file_count++;
    else if (ext && has_extension(ext + 1, image_exts, ARRAYLEN(image_exts)))
        stats.img_file_count++;
    else if (ext && has_extension(ext + 1, video_exts, ARRAYLEN(video_exts)))
        stats.vid_file_count++;
}

/* Reopen the parent after each child and replay to its saved entry.
 * This keeps one directory lease regardless of path depth. */
static enum plugin_status scan(void)
{
    unsigned int depth = 0;
    unsigned int replay = 0;
    DIR *dir = NULL;
    enum plugin_status result = PLUGIN_ERROR;
    long next_update = *rb->current_tick;

    for (;;)
    {
        if (!TIME_BEFORE(*rb->current_tick, next_update))
        {
            rb->splash_progress(0, 1, "Scanning files - Menu to cancel\n"
                                "%u files, %u directories",
                                stats.file_count, stats.dir_count);
            next_update = *rb->current_tick + HZ / 8;
        }

        int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            stats.canceled = true;
            result = PLUGIN_OK;
            break;
        }
        if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = PLUGIN_USB_CONNECTED;
            break;
        }

        struct scan_frame *frame = &frames[depth];
        if (!dir)
        {
            dir = rb->opendir(stats.dirname);
            if (!dir)
                break;
            replay = frame->entries;
        }

        struct DIRENT *entry;
        struct dirinfo info;
        int status = rb->dir_next(dir, &entry, &info);
        if (status < 0 || (status == 0 && replay))
            break;

        if (status == 0)
        {
            int closed = rb->closedir(dir);
            dir = NULL;
            if (closed < 0)
                break;
            if (frame->files > stats.max_files_in_dir)
                stats.max_files_in_dir = frame->files;
            if (!depth)
            {
                result = PLUGIN_OK;
                break;
            }
            depth--;
            char *slash = rb->strrchr(stats.dirname, '/');
            slash[slash == stats.dirname ? 1 : 0] = '\0';
        }
        else if (replay)
            replay--;
        else
        {
            frame->entries++;
            if (info.attribute & ATTR_DIRECTORY)
            {
                size_t end = rb->strlen(stats.dirname);
                const char *sep = end == 1 ? "" : "/";
                int n = rb->snprintf(stats.dirname + end,
                                     sizeof(stats.dirname) - end,
                                     "%s%s", sep, entry->d_name);
                if (n < 0 || (size_t)n >= sizeof(stats.dirname) - end ||
                    depth + 1 >= ARRAYLEN(frames))
                {
                    stats.dirname[end] = '\0';
                    break;
                }

                int closed = rb->closedir(dir);
                dir = NULL;
                if (closed < 0)
                    break;
                stats.dir_count++;
                frames[++depth] = (struct scan_frame){0};
            }
            else
            {
                frame->files++;
                count_file(entry->d_name, info);
            }
        }
        rb->yield();
    }

    if (dir && result != PLUGIN_USB_CONNECTED)
        rb->closedir(dir);
    return result;
}

static const char *stat_row(int row, void *data, char *buf, size_t size)
{
    const struct dir_stats *s = data;
    const struct {
        const char *label;
        unsigned long long count;
    } rows[] = {
        { "Files", s->file_count },
        { "Audio", s->audio_file_count },
        { "Playlists", s->m3u_file_count },
        { "Images", s->img_file_count },
        { "Videos", s->vid_file_count },
        { "Directories", s->dir_count },
        { "Max files in a directory", s->max_files_in_dir },
        { "Bytes", s->byte_count },
    };
    rb->snprintf(buf, size, "%s: %llu", rows[row].label, rows[row].count);
    return buf;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->memset(&stats, 0, sizeof(stats));
    frames[0] = (struct scan_frame){0};
    rb->strcpy(stats.dirname, "/");
    rb->splash_progress_set_delay(0);

    enum plugin_status result = scan();
    if (result == PLUGIN_USB_CONNECTED || stats.canceled)
        return result;
    if (result == PLUGIN_ERROR)
    {
        rb->splashf(3 * HZ, "Scan incomplete near %s", stats.dirname);
        return PLUGIN_OK;
    }

    struct simplelist_info list;
    rb->simplelist_info_init(&list, "Storage statistics", 8, &stats);
    list.get_name = stat_row;
    return rb->simplelist_show_list(&list) ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}
