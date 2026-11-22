/* Files owned by the Lua viewer. GNU GPL version 2 or later. */
#include "plugin.h"
#include "rock-lua-file.h"

#define GAMES PLUGIN_GAMES_DATA_DIR "/"
#define PUZZLES GAMES ".picross/"
#define WRITE_SLOTS 2

static struct lua_write {
    int fd;
    bool used;
    bool failed;
    char path[MAX_PATH];
    char temp[MAX_PATH];
} writes[WRITE_SLOTS];

static bool ascii_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

bool lua_puzzle_path(const char *path)
{
    if (rb->strncmp(path, PUZZLES, sizeof(PUZZLES) - 1))
        return false;

    const char *base = path + sizeof(PUZZLES) - 1;
    const char *ext = rb->strrchr(base, '.');
    if (!ext || ext == base || rb->strcmp(ext, ".picross"))
        return false;
    for (const char *p = base; p != ext; p++)
        if (!ascii_alnum(*p) && *p != '_' && *p != '-' && *p != ' ')
            return false;
    return true;
}

static bool writable(const char *path)
{
    return !rb->strcmp(path, GAMES "picross.sav") ||
           !rb->strcmp(path, GAMES "pixel-painter.save") ||
           !rb->strcmp(path, GAMES "pixel-painter.score") ||
           (lua_puzzle_path(path) && !rb->strncmp(path, PUZZLES "user_freedraw_",
                  sizeof(PUZZLES "user_freedraw_") - 1));
}

static struct lua_write *find_write(int fd)
{
    for (size_t i = 0; i < ARRAYLEN(writes); i++)
        if (writes[i].used && writes[i].fd == fd)
            return &writes[i];
    return NULL;
}

int lua_file_open(const char *path, int flags, ...)
{
    if (flags == O_RDONLY)
    {
        /* Game data is text. Bound allocations on malformed saves or puzzles. */
        if (!writable(path) && !lua_puzzle_path(path))
            return -1;
        int fd = rb->open(path, flags);
        if (fd < 0)
            return fd;
        off_t size = rb->ffilesize(fd);
        if (size > 0 && size <= 16 * 1024)
            return fd;
        rb->close(fd);
        return -1;
    }
    if (flags != (O_WRONLY | O_CREAT | O_TRUNC) || !writable(path))
        return -1;

    for (size_t i = 0; i < ARRAYLEN(writes); i++)
    {
        struct lua_write *slot = &writes[i];
        if (slot->used)
            continue;
        int n = rb->snprintf(slot->temp, sizeof(slot->temp), "%s.tmp", path);
        if (n < 0 || (size_t)n >= sizeof(slot->temp))
            return -1;
        int fd = rb->open(slot->temp, O_WRONLY | O_CREAT | O_EXCL);
        if (fd >= 0)
        {
            rb->strlcpy(slot->path, path, sizeof(slot->path));
            slot->used = true;
            slot->failed = false;
            slot->fd = fd;
        }
        return fd;
    }
    return -1;
}

int lua_file_close(int fd, bool commit)
{
    struct lua_write *slot = find_write(fd);
    if (!slot)
        return rb->close(fd);

    bool ok = commit && !slot->failed && !rb->plugin_usb_taken() &&
              rb->file_sync(fd) == 0;
    if (rb->close(fd) < 0)
        ok = false;
    if (ok)
        ok = rb->rename(slot->temp, slot->path) == 0;
    if (!ok)
        rb->remove(slot->temp);
    slot->used = false;
    return ok ? 0 : -1;
}

void lua_file_failed(int fd)
{
    struct lua_write *slot = find_write(fd);
    if (slot)
        slot->failed = true;
}

void lua_file_cleanup(void)
{
    for (size_t i = 0; i < ARRAYLEN(writes); i++)
        if (writes[i].used)
            lua_file_close(writes[i].fd, false);
}

int lua_file_remove(const char *path)
{
    return writable(path) ? rb->remove(path) : -1;
}
