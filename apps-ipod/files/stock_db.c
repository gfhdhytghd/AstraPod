/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Read the iPod Classic record layouts documented by libgpod's
 * db-itunes-parser.h, itdb_itunesdb.c and itdb_device.c.
 ****************************************************************************/
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "rbpaths.h"
#include "rbunicode.h"
#include "system.h"
#include "viewers/text_viewer/ts_io_core.h"
#include "stock_db.h"

#define ITUNES_DB "/iPod_Control/iTunes/iTunesDB"
#define ITUNES_CDB "/iPod_Control/iTunes/iTunesCDB"
#define CDB_SCRATCH ROCKBOX_DIR "/.stock-video-db.tmp"
#define CDB_LIMIT (128 * 1024 * 1024)

static uint32_t le32(const unsigned char *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 |
           (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static bool read_at(int fd, off_t pos, void *data, size_t size)
{
    return lseek(fd, pos, SEEK_SET) == pos &&
           read(fd, data, size) == (ssize_t)size;
}

bool stock_db_u32(const struct stock_db *db, off_t at, uint32_t *value)
{
    unsigned char b[4];

    if (at < db->start || at > db->end - 4 || !read_at(db->fd, at, b, 4))
        return false;
    *value = le32(b);
    return true;
}

static bool header(const struct stock_db *db, off_t at, off_t end,
                   const char *tag, bool list, struct stock_record *r)
{
    unsigned char b[12];
    if (at < 0 || at > end - 12 || !read_at(db->fd, at, b, sizeof(b)) ||
        memcmp(b, tag, 4))
        return false;
    uint32_t header_len = le32(b + 4);
    uint32_t field = le32(b + 8);
    off_t size = list ? end - at : field;
    if (header_len < sizeof(b) || size < (off_t)header_len || size > end - at)
        return false;

    *r = (struct stock_record){at, at + header_len, at + size, list ? field : 0};
    return true;
}

bool stock_record_read(const struct stock_db *db, off_t at, off_t end,
                       const char *tag, struct stock_record *r)
{
    return header(db, at, end, tag, false, r);
}

bool stock_db_list(const struct stock_db *db, unsigned section,
                   const char *tag, struct stock_record *list)
{
    struct stock_record r;
    for (off_t at = db->start; at < db->end; at = r.end)
    {
        uint32_t kind;
        if (!header(db, at, db->end, "mhsd", false, &r) ||
            r.body - at < 16 || !stock_db_u32(db, at + 12, &kind))
            return false;
        if (kind == section)
            return header(db, r.body, r.end, tag, true, list);
    }
    return false;
}

static bool inflate_cdb(struct stock_db *db)
{
    struct ts_core_file file;
    bs_io io;
    if (ts_io_core(&io, &file, ITUNES_CDB) != BS_OK)
        return false;
    db->fd = file.fd;

    unsigned char data[768];
    bool valid = false;
    if (!read_at(file.fd, 0, data, 0xa9) || memcmp(data, "mhbd", 4) ||
        data[0xa8] != 1)
        goto cleanup;
    uint32_t start = le32(data + 4);
    uint32_t end = le32(data + 8);
    if (start < 0xa9 || end <= start || end > (uint64_t)ffilesize(file.fd))
        goto cleanup;
    file.pos = 0xa9;

    int handle = core_alloc(64 * 1024);
    if (handle <= 0)
        goto cleanup;
    bs_arena arena = {core_get_data_pinned(handle), 64 * 1024, 0};
    bs_stream *raw = bs_file_stream(&arena, &io, start, end - start);
    bs_stream *inflated = raw ? bs_inflate_stream(&arena, raw, BS_INFLATE_ZLIB) : NULL;
    if (inflated)
    {
        int out = open(CDB_SCRATCH, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (out >= 0)
        {
            size_t got;
            db->temporary = true;
            while (bs_pull(inflated, data, sizeof(data), &got) == BS_OK)
            {
                if (!got)
                {
                    valid = db->end && bs_inflate_finished(inflated);
                    break;
                }
                if (db->end + (off_t)got > CDB_LIMIT ||
                    write(out, data, got) != (ssize_t)got)
                    break;
                db->end += got;
                yield();
            }
            close(file.fd);
            db->fd = out;
        }
    }
    core_put_data_pinned(arena.base);
    core_free(handle);
cleanup:
    if (!valid)
        stock_db_close(db);
    return valid;
}

bool stock_db_open(struct stock_db *db, bool photos)
{
    *db = (struct stock_db){.fd = -1};

    if (!photos && file_exists(ITUNES_CDB))
    {
        if (inflate_cdb(db))
            return true;
        *db = (struct stock_db){.fd = -1};
    }

    db->fd = open(photos ? STOCK_PHOTOS : ITUNES_DB, O_RDONLY);
    if (db->fd < 0)
        return false;
    struct stock_record root;
    if (!header(db, 0, ffilesize(db->fd), photos ? "mhfd" : "mhbd", false, &root))
    {
        stock_db_close(db);
        return false;
    }
    db->start = root.body;
    db->end = root.end;
    return true;
}

off_t stock_db_size(void)
{
    int fd = open(ITUNES_CDB, O_RDONLY);
    if (fd < 0)
        fd = open(ITUNES_DB, O_RDONLY);
    if (fd < 0)
        return 0;
    off_t size = ffilesize(fd);
    close(fd);
    return size;
}

bool stock_artwork_open(struct stock_db *db)
{
    *db = (struct stock_db){.fd = open("/iPod_Control/Artwork/ArtworkDB", O_RDONLY)};
    if (db->fd < 0)
        return false;
    struct stock_record root;
    if (!header(db, 0, ffilesize(db->fd), "mhfd", false, &root))
    {
        stock_db_close(db);
        return false;
    }
    db->start = root.body;
    db->end = root.end;
    return true;
}

void stock_db_close(struct stock_db *db)
{
    if (db->fd >= 0)
        close(db->fd);
    if (db->temporary)
        remove(CDB_SCRATCH);
    db->fd = -1;
}

static bool text(const struct stock_db *db, const struct stock_record *r,
                 uint32_t bytes, unsigned skip, bool wide, char *buf, size_t size)
{
    unsigned char raw[2 * MAX_PATH];
    if (r->end - r->body < (off_t)skip || bytes >= sizeof(raw) ||
        bytes > (uint64_t)(r->end - r->body - skip) ||
        (wide && (bytes & 1)) || (!wide && bytes >= size) ||
        !read_at(db->fd, r->body + skip, raw, bytes))
        return false;
    if (wide)
    {
        if (!bytes || (raw[bytes - 1] >= 0xd8 && raw[bytes - 1] <= 0xdb))
            return false;
        unsigned char *end = utf16decode(raw, (unsigned char *)buf, bytes / 2,
                                         size - 1, true);
        *end = 0;
    }
    else
    {
        memcpy(buf, raw, bytes);
        buf[bytes] = 0;
    }
    return buf[0];
}

bool stock_db_music_text(const struct stock_db *db, const struct stock_record *r,
                         char *buf, size_t size)
{
    uint32_t bytes;
    return r->end - r->body >= 16 && stock_db_u32(db, r->body + 4, &bytes) &&
           text(db, r, bytes, 16, true, buf, size);
}

bool stock_db_photo_text(const struct stock_db *db, const struct stock_record *r,
                         char *buf, size_t size)
{
    unsigned char data[8];
    return r->end - r->body >= 12 && read_at(db->fd, r->body, data, sizeof(data)) &&
           text(db, r, le32(data), 12, data[4] == 2, buf, size);
}

bool stock_music_path(char *path)
{
    for (char *p = path; *p; p++)
    {
        if (*p == ':')
            *p = '/';
        else if (*p == '/' || *p == '\\' || (unsigned char)*p < 32)
            return false;
    }
    size_t prefix = sizeof(STOCK_VIDEO_PATH) - 1;
    if (strncasecmp(path, STOCK_VIDEO_PATH, prefix))
        return false;
    const char *file = strchr(path + prefix, '/');
    if (!file || file - (path + prefix) != 3 ||
        (path[prefix] != 'F' && path[prefix] != 'f') ||
        path[prefix + 1] < '0' || path[prefix + 1] > '9' ||
        path[prefix + 2] < '0' || path[prefix + 2] > '9')
        return false;
    const char *extension = strrchr(file, '.');
    return extension && extension > file + 1 && !strchr(file + 1, '/');
}

bool stock_video_path(char *path)
{
    if (!stock_music_path(path))
        return false;
    const char *extension = strrchr(path, '.');
    return !strcasecmp(extension, ".m4v") || !strcasecmp(extension, ".mp4");
}

static bool thumbnail_pixels(const struct stock_db *db, off_t at,
                             unsigned format_id, unsigned size,
                             const char *dir, void *pixels)
{
    struct stock_record image, data, thumb, name;
    if (!header(db, at, db->end, "mhii", false, &image))
        return false;
    for (off_t p = image.body; p < image.end; p = data.end)
    {
        uint32_t type, format, offset, length;
        if (!header(db, p, image.end, "mhod", false, &data))
            return false;
        if (data.body - p < 16 || !stock_db_u32(db, p + 12, &type) ||
            (type & 0xffff) != 2 ||
            !header(db, data.body, data.end, "mhni", false, &thumb) ||
            thumb.body - thumb.at < 36 ||
            !stock_db_u32(db, thumb.at + 16, &format) || format != format_id ||
            !stock_db_u32(db, thumb.at + 20, &offset) ||
            !stock_db_u32(db, thumb.at + 24, &length) || length != size ||
            !header(db, thumb.body, thumb.end, "mhod", false, &name))
            continue;
        char location[64], path[64];
        if (!stock_db_photo_text(db, &name, location, sizeof(location)))
            return false;
        const char *leaf = strrchr(location, ':');
        unsigned id, shard;
        char basename[32];
        if (!leaf || sscanf(leaf + 1, "F%u_%u", &id, &shard) != 2 ||
            id != format_id || !shard)
            return false;
        snprintf(basename, sizeof(basename), "F%u_%u.ithmb", id, shard);
        if (strcasecmp(basename, leaf + 1))
            return false;
        snprintf(path, sizeof(path), "%s/%s", dir, basename);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return false;
        bool ok = (uint64_t)offset + size <= (uint64_t)ffilesize(fd) &&
                  read_at(fd, offset, pixels, size);
        close(fd);
        return ok;
    }
    return false;
}


bool stock_photo_pixels(const struct stock_db *db, off_t at, unsigned width,
                        unsigned height, void *pixels)
{
    return thumbnail_pixels(db, at, width == 64 ? 1066 : 1024,
                            width * height * 2, "/Photos/Thumbs", pixels);
}

int stock_artwork_pixels(const struct stock_db *db, off_t at, void *pixels)
{
    const char *dir = "/iPod_Control/Artwork";
    if (thumbnail_pixels(db, at, 1061, 56 * 56 * 2, dir, pixels))
        return 56;
    if (thumbnail_pixels(db, at, 1055, 128 * 128 * 2, dir, pixels))
        return 128;
    return 0;
}
