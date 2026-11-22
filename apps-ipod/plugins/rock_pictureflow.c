/* Native PictureFlow query, artwork and playback adapters. GPL-2.0-or-later. */
#include "plugins/rock_pictureflow.h"

#include <stdio.h>
#include <string.h>

#include "database/db_summary.h"
#include "string-extra.h"
#include "draw/image_load.h"
#include "metadata/albumart.h"
#include "playlist/catalog.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_widgets.h"
#include "screens/playback/track_info.h"
#include "settings/settings.h"
#include "system/app_util.h"

int rock_pf_db_open(struct db_summary_t *summary, void *buffer, size_t bytes)
{
    if (rock_usb_taken() || !tagcache_search_ready() || !tagcache_is_in_ram())
        return ROCK_PF_DB_BUSY;
    if (!buffer || (uintptr_t)buffer % sizeof(long) || bytes < sizeof(long))
        return ERROR_BUFFER_FULL;

    int result = db_summary_build_into(summary, buffer, bytes);
    if (result == SUCCESS && !db_summary_current(summary))
        return ROCK_PF_DB_BUSY;
    return result;
}

bool rock_pf_db_current(const struct db_summary_t *summary)
{
    return !rock_usb_taken() && db_summary_current(summary);
}

char *rock_pf_db_name(const struct db_summary_t *summary, int tag, long seek,
                      char *buffer, size_t bytes)
{
    return db_summary_name(summary, tag, seek, buffer, bytes);
}

bool rock_pf_tracks_open(const struct db_summary_t *summary,
                         const struct album_data *album,
                         struct tagcache_search *search)
{
    if (!rock_pf_db_current(summary) || !tagcache_search_ready())
        return false;

    struct tagcache_album row;
    int slot = tagcache_album_find(album->seek, album->artist_seek);
    if (!tagcache_album_get(slot, &row) || !tagcache_search(search, tag_filename))
        return false;

    /* Opening may yield while the native spoken-word tables refresh. */
    if (!rock_pf_db_current(summary) ||
        !tagcache_search_add_filter(search, tag_album, row.album_seek) ||
        !tagcache_search_add_filter(search, tag_albumartist, row.artist_seek))
    {
        tagcache_search_finish(search);
        return false;
    }

    tagcache_search_set_range(search, row.first, row.last);
    return true;
}

bool rock_pf_track_next(struct tagcache_search *search,
                        struct rock_pf_track *track)
{
    if (!tagcache_get_next(search, track->path, sizeof(track->path)))
        return false;

    if (!tagcache_entry_string(search->idx_id, tag_title,
                               track->title, sizeof(track->title)))
    {
        const char *base = strrchr(track->path, '/');
        strlcpy(track->title, base ? base + 1 : track->path,
                sizeof(track->title));
    }
    track->disc = tagcache_get_numeric(search, tag_discnumber);
    track->track = tagcache_get_numeric(search, tag_tracknumber);
    return true;
}

static int decode(const char *path, const struct mp3_albumart *embedded,
                  struct bitmap *bitmap, int bytes, int format,
                  const struct custom_format *output)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct image_src source = { .fd = fd, .len = -1 };
    if (embedded)
    {
        source.pos = embedded->pos;
        source.len = embedded->size;
        source.flags = embedded->type & ~AA_CLEAR_FLAGS_MASK;
        off_t size = ffilesize(fd);
        if (source.pos < 0 || source.pos >= size ||
            source.len <= 0 || source.len > size - source.pos)
        {
            close(fd);
            return -1;
        }
    }
    int ret = image_load(&source, bitmap, bytes, format, output);
    close(fd);
    return ret;
}

int rock_pf_art(const struct db_summary_t *summary,
                const struct album_data *album, struct bitmap *bitmap,
                int bytes, int format, const struct custom_format *output)
{
    struct tagcache_search search;
    char path[MAX_PATH];
    if (!rock_pf_tracks_open(summary, album, &search))
        return -1;
    bool found = tagcache_get_next(&search, path, sizeof(path));
    tagcache_search_finish(&search);
    if (!found || !image_progress(output, 0, 0))
        return -1;

    struct mp3entry id3;
    char art[MAX_PATH];
    bool have_tags = get_metadata(&id3, -1, path);
    bool embedded = have_tags && id3.has_embedded_albumart &&
                    image_embedded_usable(id3.albumart.type);
    int width = bitmap->width, height = bitmap->height;
    int result;

    if (embedded && global_settings.album_art == AA_PREFER_EMBEDDED)
    {
        result = decode(path, &id3.albumart, bitmap, bytes, format, output);
        if (result > 0)
            return result;
        bitmap->width = width;
        bitmap->height = height;
        embedded = false;
    }

    if (!image_progress(output, 0, 0))
        return -1;
    if (have_tags ? art_cover_file(&id3, art, sizeof(art))
                  : art_folder_file(path, art, sizeof(art)))
    {
        result = decode(art, NULL, bitmap, bytes, format, output);
        if (result > 0)
            return result;
        bitmap->width = width;
        bitmap->height = height;
    }

    if (embedded && global_settings.album_art == AA_PREFER_IMAGE_FILE &&
        image_progress(output, 0, 0))
        return decode(path, &id3.albumart, bitmap, bytes, format, output);
    return -1;
}

/* The slide worker owns each descriptor through a synchronous call.
 * The foreground's SDK descriptor table is independent of these opens. */
static bool cache_path(char *path, size_t capacity, const char *name)
{
    return !rock_usb_taken() && name[0] && !strchr(name, '/') &&
           !strchr(name, '.') && !strchr(name, '\\') &&
           snprintf(path, capacity, ROCK_PF_CACHE_DIR "/%s.pfraw", name)
               < (int)capacity;
}

ssize_t rock_pf_cache_read(const char *name, struct dim *size,
                           void *pixels, size_t capacity)
{
    char path[MAX_PATH];
    if (!cache_path(path, sizeof(path), name))
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    off_t extent = ffilesize(fd);
    ssize_t bytes = -1;
    if (extent > (off_t)sizeof(*size) &&
        extent - (off_t)sizeof(*size) <= (off_t)capacity &&
        read(fd, size, sizeof(*size)) == sizeof(*size))
    {
        size_t count = extent - sizeof(*size);
        if (read(fd, pixels, count) == (ssize_t)count)
            bytes = count;
    }
    close(fd);
    return bytes;
}

void rock_pf_cache_write(const char *name, const struct dim *size,
                         const void *pixels, size_t bytes)
{
    char path[MAX_PATH], temp[MAX_PATH];
    if (!cache_path(path, sizeof(path), name) ||
        snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return;

    /* Only one PictureFlow worker writes here. A sibling from a prior
     * interrupted write can be replaced without touching the last cover. */
    remove(temp);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return;

    bool ok = write(fd, size, sizeof(*size)) == sizeof(*size) &&
              write(fd, pixels, bytes) == (ssize_t)bytes && fsync(fd) == 0;
    if (close(fd) < 0 || !ok || rename(temp, path) < 0)
        remove(temp);
}

bool rock_pf_confirm_play(void)
{
    struct rock_native_scope scope = rock_modal_begin();
    bool result = warn_on_pl_erase();
    rock_modal_end(&scope);
    return result && !rock_usb_taken();
}

bool rock_pf_track_info(struct mp3entry *id3)
{
    struct rock_native_scope scope = rock_modal_begin();
    bool usb = browse_id3(id3, 0, 0, NULL, 1, &view_text);
    rock_modal_end(&scope);
    return usb || rock_usb_taken();
}

bool rock_pf_catalog(const char *path, int attr, bool new_playlist,
                     int (*append)(const char *, bool))
{
    struct rock_native_scope scope = rock_modal_begin();
    catalog_add_to_a_playlist(path, attr, new_playlist, NULL, append);
    rock_modal_end(&scope);
    return rock_usb_taken();
}
