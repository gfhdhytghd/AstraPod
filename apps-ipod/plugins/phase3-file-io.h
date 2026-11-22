/* Checked file helpers for the foreground utilities. GPL version 2 or later. */
#ifndef ASTRAPOD_PHASE3_FILE_IO_H
#define ASTRAPOD_PHASE3_FILE_IO_H

static const unsigned char utf8_bom[] = { 0xef, 0xbb, 0xbf };

static inline int open_text(const char *path, off_t *bom)
{
    *bom = 0;
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    unsigned char header[sizeof(utf8_bom)];
    size_t got = 0;
    while (got < sizeof(header))
    {
        ssize_t n = rb->read(fd, header + got, sizeof(header) - got);
        if (n < 0)
        {
            rb->close(fd);
            return -1;
        }
        if (!n)
            break;
        got += n;
    }
    if (got == sizeof(header) && !rb->memcmp(header, utf8_bom, sizeof(header)))
        *bom = sizeof(header);
    else if (rb->lseek(fd, 0, SEEK_SET) < 0)
    {
        rb->close(fd);
        return -1;
    }
    return fd;
}

static inline int begin_replace(const char *path, char temp[MAX_PATH])
{
    int n = rb->snprintf(temp, MAX_PATH, "%s.tmp", path);
    if (n < 0 || n >= MAX_PATH)
        return -1;
    return rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
}

static inline bool end_replace(int fd, const char *temp,
                               const char *path, bool ok)
{
    if (ok)
        ok = rb->file_sync(fd) == 0;
    if (rb->close(fd) < 0)
        ok = false;
    if (ok)
        ok = rb->rename(temp, path) == 0;
    if (!ok)
        rb->remove(temp);
    return ok;
}

static inline bool parse_unsigned(const char *text, uintmax_t max,
                                  uintmax_t *value)
{
    if (!text || !*text)
        return false;
    uintmax_t number = 0;
    for (; *text; text++)
    {
        unsigned int digit = (unsigned char)*text - '0';
        if (digit > 9 || digit > max || number > (max - digit) / 10)
            return false;
        number = 10 * number + digit;
    }
    *value = number;
    return true;
}

#endif
