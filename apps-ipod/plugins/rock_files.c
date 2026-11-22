/* Owned plugin filesystem operations. GNU GPL version 2 or later. */
#include <errno.h>
#include <string.h>
#include "file.h"
#include "rock_compat.h"
#include "rock_files.h"
#include "draw/bmp.h"
#include "system/strutil.h"
#include "vuprintf.h"

#define ROCK_OPEN_MAX 16
#define ROCK_DIR_MAX 4

/* Zero means unused. The stored descriptor is one greater than the OS value. */
static int open_files[ROCK_OPEN_MAX];
static DIR *open_dirs[ROCK_DIR_MAX];

static int file_slot(int fd)
{
    if (fd >= 0)
        for (int i = 0; i < ROCK_OPEN_MAX; ++i)
            if (open_files[i] && open_files[i] - 1 == fd)
                return i;
    return -1;
}

int rock_read_bmp_fd(int fd, struct bitmap *bm, int maxsize, int format,
                     const struct custom_format *cformat)
{
    if (rock_usb_taken() || file_slot(fd) < 0 || !bm || maxsize < 0)
        return -1;
    return read_bmp_fd(fd, bm, maxsize, format, cformat);
}

bool rock_get_metadata(struct mp3entry *id3, int fd, const char *path)
{
    if (rock_usb_taken() || !id3 || !path || !*path
        || (fd >= 0 && file_slot(fd) < 0))
        return false;

    /* The parser closes an internally opened fd and rewinds a supplied fd. */
    return get_metadata(id3, fd, path);
}

int rock_open(const char *path, int flags, ...)
{
    if (rock_usb_taken())
        return -1;
    for (int i = 0; i < ROCK_OPEN_MAX; ++i)
    {
        if (open_files[i])
            continue;
        /* Rockbox ignores permission bits. The sim still needs a mode. */
        int fd = open(path, flags, 0666);
        if (fd >= 0)
            open_files[i] = fd + 1;
        return fd;
    }
    return -1;
}

int rock_creat(const char *path, mode_t mode)
{
    (void)mode;
    return rock_open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

int rock_close(int fd)
{
    int slot = file_slot(fd);
    if (slot < 0)
        return -1;
    open_files[slot] = 0;
    return close(fd);
}

ssize_t rock_read(int fd, void *buf, size_t size)
{
    return file_slot(fd) >= 0 ? read(fd, buf, size) : -1;
}

ssize_t rock_write(int fd, const void *buf, size_t size)
{
    return file_slot(fd) >= 0 ? write(fd, buf, size) : -1;
}

int rock_file_sync(int fd)
{
    return file_slot(fd) >= 0 ? fsync(fd) : -1;
}

off_t rock_lseek(int fd, off_t offset, int whence)
{
    return file_slot(fd) >= 0 ? lseek(fd, offset, whence) : -1;
}

off_t rock_ffilesize(int fd)
{
    return file_slot(fd) >= 0 ? ffilesize(fd) : -1;
}

int rock_read_line(int fd, char *buf, int size)
{
    return file_slot(fd) >= 0 ? read_line(fd, buf, size) : -1;
}

struct rock_file_print {
    int fd;
    bool failed;
};

static int write_char(void *opaque, int ch)
{
    struct rock_file_print *print = opaque;
    unsigned char byte = ch;
    print->failed = write(print->fd, &byte, 1) != 1;
    return print->failed ? -1 : 1;
}

int rock_fdprintf(int fd, const char *fmt, ...)
{
    if (file_slot(fd) < 0)
        return -1;

    va_list args;
    va_start(args, fmt);
    struct rock_file_print print = { .fd = fd };
    int result = vuprintf(write_char, &print, fmt, args);
    va_end(args);
    return print.failed ? -1 : result;
}

bool rock_file_exists(const char *path)
{
    return file_exists(path);
}

int rock_remove(const char *path)
{
    return rock_usb_taken() ? -1 : remove(path);
}

int rock_rename(const char *oldpath, const char *newpath)
{
    return rock_usb_taken() ? -1 : rename(oldpath, newpath);
}

static int dir_slot(DIR *dir)
{
    if (dir)
        for (int i = 0; i < ROCK_DIR_MAX; ++i)
            if (open_dirs[i] == dir)
                return i;
    return -1;
}

DIR *rock_opendir(const char *path)
{
    if (rock_usb_taken())
        return NULL;
    for (int i = 0; i < ROCK_DIR_MAX; ++i)
        if (!open_dirs[i])
            return open_dirs[i] = opendir(path);
    return NULL;
}

struct DIRENT *rock_readdir(DIR *dir)
{
    return dir_slot(dir) >= 0 ? readdir(dir) : NULL;
}

int rock_closedir(DIR *dir)
{
    int slot = dir_slot(dir);
    if (slot < 0)
        return -1;
    open_dirs[slot] = NULL;
    return closedir(dir);
}

struct dirinfo rock_dir_get_info(DIR *dir, struct DIRENT *entry)
{
    if (dir_slot(dir) < 0 || !entry)
        return (struct dirinfo){0};
    return dir_get_info(dir, entry);
}

/* Distinguish an I/O error from end-of-directory without exporting errno. */
int rock_dir_next(DIR *dir, struct DIRENT **entry, struct dirinfo *info)
{
    if (dir_slot(dir) < 0)
        return -1;

    do
    {
        errno = 0;
        *entry = readdir(dir);
        if (!*entry)
            return errno ? -1 : 0;
    } while (!strcmp((*entry)->d_name, ".") || !strcmp((*entry)->d_name, ".."));

    errno = 0;
#ifdef SIMULATOR
    *info = sim_dir_get_info_nofollow(dir, *entry);
#else
    *info = dir_get_info(dir, *entry);
#endif
    return errno ? -1 : 1;
}

void rock_close_files(void)
{
    for (int i = 0; i < ROCK_OPEN_MAX; ++i)
        if (open_files[i])
            rock_close(open_files[i] - 1);
    for (int i = 0; i < ROCK_DIR_MAX; ++i)
        if (open_dirs[i])
            rock_closedir(open_dirs[i]);
}

bool rock_dir_exists(const char *path)
{
    return !rock_usb_taken() && path && dir_exists(path);
}

int rock_mkdir(const char *path)
{
    return rock_usb_taken() ? -1 : mkdir(path);
}

int rock_rmdir(const char *path)
{
    return rock_usb_taken() ? -1 : rmdir(path);
}

int rock_ftruncate(int fd, off_t length)
{
    return file_slot(fd) >= 0 ? ftruncate(fd, length) : -1;
}
