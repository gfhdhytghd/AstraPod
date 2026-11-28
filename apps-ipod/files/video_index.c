/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The video index: the Videos category, title, artist, show, episode,
 * length and cover location of every video file copied to the player
 * outside the iTunes library, for the Videos lists and their artwork.
 *
 * The documents walk (file_index.c) hands each video here, and its MP4 tags
 * are read once per walk: a video's metadata sits at the end of its 'moov'
 * and is reached by seeking past the sample tables, a handful of small
 * reads. The category is the 'stik' media kind iTunes writes: 6 is a music
 * video, 10 a TV show, and anything else, or none, a movie. One entry per
 * video in a libfile of entries that each say their length.
 *
 * Parts, in order:
 *   - the record, and reading the file
 *   - the MP4 tags
 *   - the walk's side: begin, each video, end
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "string-extra.h"
#include "database/libfile.h"
#include "database/path_key.h"
#include "system/library_files.h"
#include "files/stock_db.h"
#include "video_index.h"

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

/* ------------------------------------------------------------------ *
 * the record, and reading the file                                   *
 * ------------------------------------------------------------------ */

/* One video; its path, title, artist and show follow it, unterminated */
struct video_rec
{
    uint64_t art_key;
    uint32_t duration_ms;
    int32_t  cover_at;
    uint32_t cover_size;
    uint16_t season, episode;
    uint8_t  kind;
    uint8_t  pad;
    uint16_t path_len, title_len, artist_len, show_len;
};

bool video_index_stale(void)
{
    struct libfile_header h;
    return !libfile_peek(LIB_VIDEO_INDEX_FILE, LIB_VIDEO_INDEX_MAGIC,
                         LIB_VIDEO_INDEX_VERSION, &h);
}

void video_index_each(video_index_fn fn, void *data,
                      struct video_index_text *text)
{
    struct video_entry e = { .path = text->path, .title = text->title,
                             .artist = text->artist, .show = text->show };
    struct libfile_header h;
    struct video_rec r;
    int fd = libfile_open(LIB_VIDEO_INDEX_FILE, LIB_VIDEO_INDEX_MAGIC,
                          LIB_VIDEO_INDEX_VERSION, 1, &h, NULL);

    if (fd < 0)
        return;
    while (read(fd, &r, sizeof(r)) == (ssize_t)sizeof(r))
    {
        if (r.path_len >= sizeof(text->path)
            || r.title_len >= sizeof(text->title)
            || r.artist_len >= sizeof(text->artist)
            || r.show_len >= sizeof(text->show)
            || read(fd, text->path, r.path_len) != r.path_len
            || read(fd, text->title, r.title_len) != r.title_len
            || read(fd, text->artist, r.artist_len) != r.artist_len
            || read(fd, text->show, r.show_len) != r.show_len)
            break;
        text->path[r.path_len] = '\0';
        text->title[r.title_len] = '\0';
        text->artist[r.artist_len] = '\0';
        text->show[r.show_len] = '\0';
        e.art_key = r.art_key;
        e.kind = r.kind;
        e.duration_ms = r.duration_ms;
        e.season = r.season;
        e.episode = r.episode;
        e.cover_at = r.cover_at;
        e.cover_size = r.cover_size;
        if (!fn(&e, data))
            break;
    }
    close(fd);
}

/* ------------------------------------------------------------------ *
 * the MP4 tags                                                       *
 * ------------------------------------------------------------------ */

struct video_tags
{
    struct video_rec r;
    unsigned stik;
    char title[VIDEO_INDEX_TITLE];
    char artist[VIDEO_INDEX_NAME];
    char show[VIDEO_INDEX_NAME];
};

static bool read_u32(int fd, uint32_t *v)
{
    unsigned char b[4];
    if (read(fd, b, 4) != 4)
        return false;
    *v = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    return true;
}

/* The atom at `at`, which must end by `end`: its type, and where its body
 * starts and it ends */
static bool atom_at(int fd, off_t at, off_t end, uint32_t *type,
                    off_t *body, off_t *stop)
{
    uint32_t size, high, low;
    if (end - at < 8 || lseek(fd, at, SEEK_SET) != at ||
        !read_u32(fd, &size) || !read_u32(fd, type))
        return false;
    *body = at + 8;
    if (size == 1)
    {
        if (!read_u32(fd, &high) || !read_u32(fd, &low) || high)
            return false;
        size = low;
        *body += 8;
    }
    else if (size == 0)
        size = end - at;
    if (size < *body - at || (off_t)size > end - at)
        return false;
    *stop = at + size;
    return true;
}

/* The first child of type `want` in [at, end) */
static bool find_atom(int fd, off_t at, off_t end, uint32_t want,
                      off_t *body, off_t *stop)
{
    uint32_t type;
    while (atom_at(fd, at, end, &type, body, stop))
    {
        if (type == want)
            return true;
        at = *stop;
    }
    return false;
}

static void read_duration(int fd, off_t body, off_t stop, struct video_rec *r)
{
    uint32_t vflags, scale, high = 0, low;
    if (lseek(fd, body, SEEK_SET) != body || !read_u32(fd, &vflags))
        return;
    bool wide = (vflags >> 24) == 1;
    if (stop - body < (wide ? 32 : 20) ||
        lseek(fd, body + (wide ? 20 : 12), SEEK_SET) < 0 ||
        !read_u32(fd, &scale) || (wide && !read_u32(fd, &high)) ||
        !read_u32(fd, &low) || !scale)
        return;
    uint64_t ms = (((uint64_t)high << 32) | low) * 1000 / scale;
    r->duration_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

/* One 'ilst' item's first 'data' atom: its type, and the payload's place */
static bool item_data(int fd, off_t body, off_t stop, uint32_t *type,
                      off_t *at, uint32_t *len)
{
    off_t data, end;
    uint32_t locale;
    if (!find_atom(fd, body, stop, FOURCC('d', 'a', 't', 'a'), &data, &end) ||
        end - data < 8 || !read_u32(fd, type) || !read_u32(fd, &locale))
        return false;
    *type &= 0xffffff;
    *at = data + 8;
    *len = end - *at;
    return true;
}

static void item_text(int fd, off_t at, uint32_t len, char *buf, size_t size)
{
    len = MIN(len, size - 1);
    if (lseek(fd, at, SEEK_SET) != at || read(fd, buf, len) != (ssize_t)len)
        len = 0;
    buf[len] = '\0';
}

static uint32_t item_number(int fd, off_t at, uint32_t len)
{
    unsigned char b[4] = {0};
    len = MIN(len, 4);
    if (lseek(fd, at, SEEK_SET) != at || read(fd, b, len) != (ssize_t)len)
        return 0;
    uint32_t v = 0;
    for (uint32_t i = 0; i < len; i++)
        v = v << 8 | b[i];
    return v;
}

static void read_items(int fd, off_t at, off_t end, struct video_tags *t)
{
    uint32_t item;
    off_t body, stop;
    while (atom_at(fd, at, end, &item, &body, &stop))
    {
        uint32_t type, len;
        off_t data;
        at = stop;
        if (!item_data(fd, body, stop, &type, &data, &len))
            continue;
        switch (item)
        {
            case FOURCC(0xa9, 'n', 'a', 'm'):
                item_text(fd, data, len, t->title, sizeof(t->title));
                break;
            case FOURCC(0xa9, 'A', 'R', 'T'):
                item_text(fd, data, len, t->artist, sizeof(t->artist));
                break;
            case FOURCC('t', 'v', 's', 'h'):
                item_text(fd, data, len, t->show, sizeof(t->show));
                break;
            case FOURCC('t', 'v', 's', 'n'):
                t->r.season = MIN(item_number(fd, data, len), UINT16_MAX);
                break;
            case FOURCC('t', 'v', 'e', 's'):
                t->r.episode = MIN(item_number(fd, data, len), UINT16_MAX);
                break;
            case FOURCC('s', 't', 'i', 'k'):
                t->stik = item_number(fd, data, len);
                break;
            case FOURCC('c', 'o', 'v', 'r'):
                /* 13 JPEG, 14 PNG */
                if ((type == 13 || type == 14) && len && t->r.cover_at < 0 &&
                    data <= INT32_MAX)
                {
                    t->r.cover_at = data;
                    t->r.cover_size = len;
                }
                break;
        }
    }
}

static void read_tags(const char *path, struct video_tags *t)
{
    int fd = open(path, O_RDONLY);
    off_t moov, moov_end, body, stop;
    if (fd < 0)
        return;
    if (find_atom(fd, 0, ffilesize(fd), FOURCC('m', 'o', 'o', 'v'),
                  &moov, &moov_end))
    {
        if (find_atom(fd, moov, moov_end, FOURCC('m', 'v', 'h', 'd'),
                      &body, &stop))
            read_duration(fd, body, stop, &t->r);
        off_t meta, meta_end, first, ilst, ilst_end;
        uint32_t type;
        if (find_atom(fd, moov, moov_end, FOURCC('u', 'd', 't', 'a'),
                      &body, &stop) &&
            find_atom(fd, body, stop, FOURCC('m', 'e', 't', 'a'),
                      &meta, &meta_end))
        {
            /* A full atom, but QuickTime writes it without the version */
            first = meta + 4;
            if (atom_at(fd, meta, meta_end, &type, &body, &stop) &&
                (type == FOURCC('h', 'd', 'l', 'r') ||
                 type == FOURCC('i', 'l', 's', 't')))
                first = meta;
            if (find_atom(fd, first, meta_end, FOURCC('i', 'l', 's', 't'),
                          &ilst, &ilst_end))
                read_items(fd, ilst, ilst_end, t);
        }
    }
    close(fd);
}

/* ------------------------------------------------------------------ *
 * the walk's side                                                    *
 * ------------------------------------------------------------------ */

static struct libfile_writer writer;
static bool writing;

void video_index_begin(void)
{
    writing = libfile_begin(&writer, LIB_VIDEO_INDEX_FILE,
                            LIB_VIDEO_INDEX_MAGIC, LIB_VIDEO_INDEX_VERSION,
                            1, NULL);
}

void video_index_found(const char *path, unsigned long size,
                       unsigned long mtime)
{
    static struct video_tags t;
    if (!writing ||
        !strncasecmp(path, STOCK_VIDEO_PATH, sizeof(STOCK_VIDEO_PATH) - 1))
        return;

    memset(&t, 0, sizeof(t));
    t.r.cover_at = -1;
    read_tags(path, &t);
    t.r.kind = t.stik == 6 ? STOCK_MUSIC_VIDEO :
               t.stik == 10 ? STOCK_TV_SHOW : STOCK_MOVIE;
    if (!t.title[0])
    {
        const char *leaf = strrchr(path, '/');
        strlcpy(t.title, leaf ? leaf + 1 : path, sizeof(t.title));
        char *dot = strrchr(t.title, '.');
        if (dot && dot != t.title)
            *dot = '\0';
    }
    /* The file's stamp is in the key, so a replaced file gets a new
     * thumbnail rather than its predecessor's */
    if (t.r.cover_at >= 0)
        t.r.art_key = (path_key(path) ^ ((uint64_t)size << 32 | mtime)) | 1;

    t.r.path_len = strlen(path);
    t.r.title_len = strlen(t.title);
    t.r.artist_len = strlen(t.artist);
    t.r.show_len = strlen(t.show);
    writing = libfile_write(&writer, &t.r, sizeof(t.r), sizeof(t.r)) &&
              libfile_write(&writer, path, t.r.path_len, t.r.path_len) &&
              libfile_write(&writer, t.title, t.r.title_len, t.r.title_len) &&
              libfile_write(&writer, t.artist, t.r.artist_len,
                            t.r.artist_len) &&
              libfile_write(&writer, t.show, t.r.show_len, t.r.show_len);
}

void video_index_end(bool completed)
{
    libfile_finish(&writer, writing && completed);
    writing = false;
}
