/* Bounded Audioscrobbler and playback log input. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_LASTFM_LOG_COMPAT_H
#define ASTRAPOD_LASTFM_LOG_COMPAT_H

#define LASTFM_EXPORT "/lastfm-export.scrobbler"
#define LASTFM_LOG "/.scrobbler.log"
#define LASTFM_LINE_MAX (MAX_PATH * 4)

enum lastfm_read {
    LASTFM_EOF = 0,
    LASTFM_RECORD = 1,
    LASTFM_INVALID = 2,
    LASTFM_ERROR = -1,
};

struct lastfm_log {
    int fd;
    off_t consumed;
    off_t length;
    size_t next;
    size_t end;
    unsigned char block[4096];
};

static void lastfm_close(struct lastfm_log *log)
{
    if (log->fd >= 0)
    {
        rb->close(log->fd);
        log->fd = -1;
    }
}

static bool lastfm_open(struct lastfm_log *log, const char *path)
{
    log->fd = rb->open(path, O_RDONLY);
    if (log->fd < 0)
        return false;
    log->length = rb->ffilesize(log->fd);
    log->consumed = 0;
    log->next = 0;
    log->end = 0;
    if (log->length >= 0)
        return true;
    lastfm_close(log);
    return false;
}

/* Ignore an incomplete trailing line. Writers may be appending to the log. */
static enum lastfm_read lastfm_line(struct lastfm_log *log, char *line)
{
    bool invalid = false;
    size_t used = 0;
    while (log->consumed < log->length)
    {
        if (log->next == log->end)
        {
            size_t want = MIN((off_t)sizeof(log->block),
                              log->length - log->consumed);
            ssize_t count = rb->read(log->fd, log->block, want);
            if (count <= 0)
                return LASTFM_ERROR;
            log->end = count;
            log->next = 0;
            if (rb->plugin_usb_taken())
                return LASTFM_ERROR;
        }
        unsigned char ch = log->block[log->next++];
        log->consumed++;
        if (ch == '\n')
        {
            if (used && line[used - 1] == '\r')
                used--;
            line[used] = '\0';
            return invalid ? LASTFM_INVALID : LASTFM_RECORD;
        }
        if (used == LASTFM_LINE_MAX - 1)
            return LASTFM_ERROR;
        if (ch == 0)
            invalid = true;
        line[used++] = ch;
    }
    return LASTFM_EOF;
}

static bool lastfm_number(char **line, char delimiter, unsigned long *number)
{
    char *first = *line;
    char *p = first;
    unsigned long value = 0;
    while (*p >= '0' && *p <= '9')
    {
        unsigned int digit = *p++ - '0';
        if (value > (ULONG_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (p == first || *p != delimiter)
        return false;
    *line = delimiter ? p + 1 : p;
    *number = value;
    return true;
}

#endif
