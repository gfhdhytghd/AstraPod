/* Owned folders and checked output for Random Folder Advance. GPL-2.0+. */
#include "files/filetypes.h"
#include "plugins/phase3-file-io.h"

#define RFA_PLAYLIST "/folder_advance_shuffled.m3u8"
#define RFA_SELECTIONS 32

struct rfa_scan
{
    char path[MAX_PATH];
    struct rfa_dirs *dirs;
    size_t capacity;
    bool dirty;
};

struct rfa_selection
{
    char path[MAX_PATH];
    bool exclude;
};

struct rfa_frame
{
    unsigned entries;
    size_t end;
};

struct rfa_input
{
    int fd;
    char bytes[256];
    int at;
    int length;
};

static struct rfa_selection selections[RFA_SELECTIONS];
static size_t selection_count;
static struct rfa_frame frames[MAX_PATH / 2 + 1];

static bool valid_path(const char *path)
{
    if (path[0] != '/')
        return false;
    for (const char *p = path + 1; *p; )
    {
        const char *end = rb->strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : rb->strlen(p);
        if (!n || (p[0] == '.' && (n == 1 || (n == 2 && p[1] == '.'))))
            return false;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 0x20 || rb->strchr("\\:<>", p[i]))
                return false;
        if (!end)
            return true;
        p = end + 1;
        if (!*p)
            return false;
    }
    return true;
}

static bool in_folder(const char *path, const char *folder)
{
    size_t n = rb->strlen(folder);
    if (n == 1 && folder[0] == '/')
        return path[0] == '/';
    return !rb->strncasecmp(path, folder, n) &&
           (path[n] == '/' || path[n] == '\0');
}

static bool ignored(const char *path)
{
    for (size_t i = 0; i < selection_count; i++)
        if (selections[i].exclude && !rb->strcasecmp(path, selections[i].path))
            return true;
    return false;
}

static bool set_buffer(struct rfa_scan *scan)
{
    size_t size;
    scan->dirs = rb->plugin_get_buffer(&size);
    if (!scan->dirs || size < sizeof(*scan->dirs))
        return false;
    scan->capacity = (size - sizeof(*scan->dirs)) / MAX_PATH;
    scan->dirs->count = 0;
    return true;
}

static int load_dirs(struct rfa_scan *scan)
{
    if (!set_buffer(scan))
        return -1;
    int fd = rb->open(RFA_LIST_DAT, O_RDONLY);
    if (fd < 0)
        return -1;
    int count;
    bool ok = rb->read(fd, &count, sizeof(count)) == sizeof(count) &&
              count >= 0 && (size_t)count <= scan->capacity;
    if (ok)
    {
        size_t bytes = (size_t)count * MAX_PATH;
        ok = rb->ffilesize(fd) >= (off_t)(sizeof(count) + bytes) &&
             rb->read(fd, scan->dirs->dir, bytes) == (ssize_t)bytes;
    }
    if (rb->close(fd) < 0)
        ok = false;
    for (int i = 0; ok && i < count; i++)
    {
        const char *path = scan->dirs->dir[i];
        size_t n = 0;
        while (n < MAX_PATH && path[n])
            n++;
        ok = n < MAX_PATH && valid_path(path);
    }
    if (ok)
        scan->dirs->count = count;
    return ok ? 0 : -1;
}

static int save_dirs(struct rfa_scan *scan)
{
    char temp[MAX_PATH];
    int fd = begin_replace(RFA_LIST_DAT, temp);
    if (fd < 0)
        return -1;
    int count = 0;
    for (int i = 0; i < scan->dirs->count; i++)
        count += scan->dirs->dir[i][0] == '/';
    bool ok = rb->write(fd, &count, sizeof(count)) == sizeof(count);
    for (int i = 0; ok && i < scan->dirs->count; i++)
    {
        if (rb->action_userabort(TIMEOUT_NOBLOCK))
            ok = false;
        else if (scan->dirs->dir[i][0] == '/')
            ok = rb->write(fd, scan->dirs->dir[i], MAX_PATH) == MAX_PATH;
    }
    return end_replace(fd, temp, RFA_LIST_DAT, ok) ? 1 : -1;
}

static bool append_folder(struct rfa_scan *scan, const char *path)
{
    if (!valid_path(path) || (size_t)scan->dirs->count >= scan->capacity)
        return false;
    char *row = scan->dirs->dir[scan->dirs->count++];
    rb->memset(row, 0, MAX_PATH);
    rb->strlcpy(row, path, MAX_PATH);
    return true;
}

static int read_path_line(struct rfa_input *in, char *line)
{
    size_t used = 0;
    for (;;)
    {
        if (in->at == in->length)
        {
            in->length = rb->read(in->fd, in->bytes, sizeof(in->bytes));
            in->at = 0;
            if (in->length < 0)
                return -1;
            if (!in->length)
                break;
        }
        char c = in->bytes[in->at++];
        if (c == '\n')
            break;
        if (!c || used >= MAX_PATH)
            return -1;
        line[used++] = c;
    }
    if (used && line[used - 1] == '\r')
        used--;
    if (used >= MAX_PATH)
        return -1;
    line[used] = '\0';
    return in->length || used ? 1 : 0;
}

static bool load_selection(void)
{
    selection_count = 0;
    if (!rb->file_exists(RFA_CSTM_DIR))
        return true;
    int fd = rb->open(RFA_CSTM_DIR, O_RDONLY);
    if (fd < 0)
        return false;
    struct rfa_input in = { .fd = fd };
    char line[MAX_PATH + 1];
    int status;
    while ((status = read_path_line(&in, line)) > 0)
    {
        if (!*line)
            continue;
        bool exclude = *line == '-';
        const char *path = line + exclude;
        if ((exclude && *path != '/') ||
            selection_count == ARRAYLEN(selections))
        {
            status = -1;
            break;
        }
        while (*path == '/')
            path++;
        struct rfa_selection *row = &selections[selection_count];
        int length = rb->snprintf(row->path, sizeof(row->path), "/%s", path);
        if (length < 1 || (size_t)length >= sizeof(row->path) ||
            !valid_path(row->path))
        {
            status = -1;
            break;
        }
        row->exclude = exclude;
        selection_count++;
    }
    return rb->close(fd) == 0 && status == 0;
}

static bool walk_root(struct rfa_scan *scan, const char *root)
{
    if (!ignored(root) && !append_folder(scan, root))
        return false;
    rb->strlcpy(scan->path, root, sizeof(scan->path));
    frames[0] = (struct rfa_frame){0, rb->strlen(root)};
    size_t depth = 0;
    unsigned replay = 0;
    DIR *dir = NULL;
    bool ok = false;

    /* Reopen each parent at its saved ordinal so only one stream is live. */
    while (!rb->action_userabort(TIMEOUT_NOBLOCK))
    {
        rb->yield();
        struct rfa_frame *frame = &frames[depth];
        if (!dir)
        {
            dir = rb->opendir(scan->path);
            if (!dir)
                break;
            replay = frame->entries;
        }

        struct DIRENT *entry;
        struct dirinfo info;
        int status = rb->dir_next(dir, &entry, &info);
        if (status < 0 || (!status && replay))
            break;
        if (!status)
        {
            int closed = rb->closedir(dir);
            dir = NULL;
            if (closed < 0)
                break;
            if (!depth)
            {
                ok = true;
                break;
            }
            scan->path[frames[--depth].end] = '\0';
        }
        else if (replay)
            replay--;
        else
        {
            frame->entries++;
            if (!(info.attribute & ATTR_DIRECTORY) ||
                (info.attribute & (ATTR_LINK | ATTR_VOLUME)) ||
                entry->d_name[0] == '.')
                continue;
            size_t length = frame->end;
            int n = rb->snprintf(scan->path + length, MAX_PATH - length,
                                 "%s%s", length == 1 ? "" : "/", entry->d_name);
            if (n < 1 || (size_t)n >= MAX_PATH - length ||
                depth + 1 >= ARRAYLEN(frames))
                break;
            if (ignored(scan->path))
            {
                scan->path[length] = '\0';
                continue;
            }
            if (!append_folder(scan, scan->path))
                break;
            int closed = rb->closedir(dir);
            dir = NULL;
            if (closed < 0)
                break;
            frames[++depth] = (struct rfa_frame){0, length + n};
            rb->splash_progress(scan->dirs->count, scan->capacity,
                                "Scanning folders: %d", scan->dirs->count);
        }
    }
    if (dir)
        rb->closedir(dir);
    return ok;
}

static void generate(struct rfa_scan *scan)
{
    bool ok = set_buffer(scan) && load_selection();
    int roots = 0;
    rb->splash_progress_set_delay(HZ / 4);
    for (size_t i = 0; ok && i < selection_count; i++)
        if (!selections[i].exclude)
        {
            roots++;
            ok = walk_root(scan, selections[i].path);
        }
    if (ok && !roots)
        ok = walk_root(scan, "/");
    if (ok)
        ok = save_dirs(scan) >= 0;
    scan->dirs = NULL;
    if (!rb->plugin_usb_taken())
        rb->splash(HZ, ok ? "Folder list saved" : "Scan stopped");
}

static int export(struct rfa_scan *scan)
{
    if (load_dirs(scan) < 0)
        return -1;
    char temp[MAX_PATH];
    int fd = begin_replace(RFA_LIST_TXT, temp);
    if (fd < 0)
        return -1;

    bool ok = true;
    for (int i = 0; ok && i < scan->dirs->count; i++)
        ok = !rb->action_userabort(TIMEOUT_NOBLOCK) &&
             rb->fdprintf(fd, "%s\n", scan->dirs->dir[i]) >= 0;
    return end_replace(fd, temp, RFA_LIST_TXT, ok) ? 1 : -1;
}

static int import(struct rfa_scan *scan)
{
    if (!set_buffer(scan))
        return -1;
    int fd = rb->open(RFA_LIST_TXT, O_RDONLY);
    if (fd < 0)
        return -1;
    int status;
    struct rfa_input in = { .fd = fd };
    char line[MAX_PATH + 1];
    while ((status = read_path_line(&in, line)) > 0)
    {
        if (rb->action_userabort(TIMEOUT_NOBLOCK) ||
            (*line && !append_folder(scan, line)))
        {
            status = -1;
            break;
        }
    }
    if (rb->close(fd) < 0 || status < 0)
    {
        scan->dirs = NULL;
        return -1;
    }

    int result = save_dirs(scan);
    scan->dirs = NULL;
    return result;
}

static bool append_tracks(int fd, const char *path, size_t *tracks)
{
    DIR *dir = rb->opendir(path);
    if (!dir)
        return false;
    struct DIRENT *entry;
    struct dirinfo info;
    bool ok = true;
    int status = 0;
    while (ok && (status = rb->dir_next(dir, &entry, &info)) > 0)
    {
        rb->yield();
        if (rb->action_userabort(TIMEOUT_NOBLOCK))
        {
            ok = false;
            break;
        }
        if ((info.attribute & (ATTR_DIRECTORY | ATTR_LINK | ATTR_VOLUME)) ||
            rb->filetype_get_attr(entry->d_name) != FILE_ATTR_AUDIO)
            continue;
        char track[MAX_PATH];
        int n = rb->snprintf(track, sizeof(track), "%s%s%s",
                             path, path[1] ? "/" : "", entry->d_name);
        ok = n > 0 && (size_t)n < sizeof(track) && valid_path(track) &&
             *tracks < (size_t)rb->global_settings->max_files_in_playlist &&
             rb->fdprintf(fd, "%s\n", track) >= 0;
        if (ok)
            ++*tracks;
    }
    return rb->closedir(dir) == 0 && ok && status == 0;
}

static int save_shuffled(struct rfa_scan *scan)
{
    if (load_dirs(scan) < 0 || !scan->dirs->count)
        return -1;
    size_t bytes;
    unsigned char *buffer = rb->plugin_get_buffer(&bytes);
    size_t offset = sizeof(*scan->dirs) + MAX_PATH * (size_t)scan->dirs->count;
    offset = (offset + sizeof(int) - 1) & ~(sizeof(int) - 1);
    size_t count = scan->dirs->count;
    if (offset > bytes || count > (bytes - offset) / sizeof(int))
        return -1;
    int *order = (int *)(buffer + offset);
    for (size_t i = 0; i < count; i++)
        order[i] = i;
    for (size_t i = count - 1; i > 0; i--)
    {
        size_t j = rb->rand() % (i + 1);
        int slot = order[j];
        order[j] = order[i];
        order[i] = slot;
    }
    char temp[MAX_PATH];
    int fd = begin_replace(RFA_PLAYLIST, temp);
    if (fd < 0)
        return -1;
    size_t tracks = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++)
        ok = append_tracks(fd, scan->dirs->dir[order[i]], &tracks);
    return end_replace(fd, temp, RFA_PLAYLIST, ok && tracks) ? 1 : -1;
}
