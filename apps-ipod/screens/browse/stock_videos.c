/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * iTunes video categories and playlists. Names and locations come from the
 * stock library and all playback uses the same viewer as loose video files.
 ****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "draw/bmp.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "widgets/splash.h"
#include "root_menu.h"
#include "settings/settings.h"
#include "files/filetypes.h"
#include "plugins/rock_compat.h"
#include "input/action.h"
#include "ui/scene.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "browser_flat.h"
#include "stock_media.h"

#define VIDEO_MASK (STOCK_MOVIE | STOCK_TV_SHOW | STOCK_MUSIC_VIDEO)
#define MEDIA_LIMIT 32768
#define MAX_PLAYLISTS 2048
#define COVER_SIZE 56
#define COVER_SOURCE 128

struct video_track {
    uint32_t id;
    uint32_t media;
    off_t title;
    off_t path;
    off_t artist;
    off_t show;
    off_t artwork;
    uint32_t artwork_id;
    uint64_t dbid;
    uint32_t season;
    uint32_t episode;
};

struct videos {
    struct stock_db db;
    struct stock_db art;
    int cover_handle;
    fb_data *cover;
    int cached_cover;
    int source_edge;
    struct bitmap bitmap;
    struct video_track *tracks;
    int count;
    int *rows;
    int row_count;
    int row_capacity;
    off_t *playlists;
    int playlist_count;
    bool loose;
};

static bool text_at(const struct videos *v, off_t at, char *buf, size_t n)
{
    struct stock_record r;
    return at && stock_record_read(&v->db, at, v->db.end, "mhod", &r) &&
           stock_db_music_text(&v->db, &r, buf, n);
}

static int compare_id(const void *left, const void *right)
{
    uint32_t a = ((const struct video_track *)left)->id;
    uint32_t b = ((const struct video_track *)right)->id;
    return (a > b) - (a < b);
}

static bool read_dbid(const struct stock_db *db, off_t at, uint64_t *id)
{
    uint32_t low, high;
    if (!stock_db_u32(db, at, &low) || !stock_db_u32(db, at + 4, &high))
        return false;
    *id = ((uint64_t)high << 32) | low;
    return true;
}

static int compare_art_id(const void *left, const void *right)
{
    const struct video_track *a = left;
    const struct video_track *b = right;
    uint64_t id_a = a->artwork_id ? a->artwork_id : a->dbid;
    uint64_t id_b = b->artwork_id ? b->artwork_id : b->dbid;
    return (id_a > id_b) - (id_a < id_b);
}

static void read_artwork(struct videos *v)
{
    struct stock_record list, image;
    if (!v->count || !stock_artwork_open(&v->art) ||
        !stock_db_list(&v->art, 1, "mhli", &list))
        return;

    v->cover_handle = core_alloc((COVER_SOURCE * COVER_SOURCE + COVER_SIZE * COVER_SIZE) *
                                 sizeof(fb_data));
    if (v->cover_handle <= 0)
        return;
    v->cover = core_get_data_pinned(v->cover_handle);

    qsort(v->tracks, v->count, sizeof(*v->tracks), compare_art_id);
    for (off_t at = list.body; at < list.end; at = image.end)
    {
        struct video_track key = {0};
        if (!stock_record_read(&v->art, at, list.end, "mhii", &image))
            break;
        if (image.body - at < 28 ||
            !stock_db_u32(&v->art, at + 16, &key.artwork_id) ||
            !read_dbid(&v->art, at + 20, &key.dbid))
            continue;
        for (int i = 0; i < 2; i++)
        {
            int low = 0, high = v->count;
            while (low < high)
            {
                int mid = low + (high - low) / 2;
                if (compare_art_id(&v->tracks[mid], &key) < 0)
                    low = mid + 1;
                else
                    high = mid;
            }
            while (low < v->count && !compare_art_id(&v->tracks[low], &key))
                v->tracks[low++].artwork = at;
            key.artwork_id = 0;
        }
        yield();
    }
}

static int by_id(const struct videos *v, uint32_t id)
{
    int low = 0, high = v->count;
    while (low < high)
    {
        int mid = low + (high - low) / 2;
        if (id == v->tracks[mid].id)
            return mid;
        if (id < v->tracks[mid].id)
            high = mid;
        else
            low = mid + 1;
    }
    return -1;
}

static void read_tracks(struct videos *v, const struct stock_record *list,
                        unsigned capacity)
{
    struct stock_record track, child;
    for (off_t at = list->body; at < list->end && (unsigned)v->count < capacity;
         at = track.end)
    {
        struct video_track item = {0};
        if (!stock_record_read(&v->db, at, list->end, "mhit", &track))
            break;
        if (track.body - at < 212 || !stock_db_u32(&v->db, at + 208, &item.media) ||
            !(item.media & VIDEO_MASK) || (item.media & (4 | 0x8000)) ||
            !stock_db_u32(&v->db, at + 16, &item.id))
            continue;
        read_dbid(&v->db, at + 112, &item.dbid);
        if (track.body - at >= 220)
        {
            stock_db_u32(&v->db, at + 212, &item.season);
            stock_db_u32(&v->db, at + 216, &item.episode);
        }
        if (track.body - at >= 356)
            stock_db_u32(&v->db, at + 352, &item.artwork_id);
        for (off_t p = track.body; p < track.end; p = child.end)
        {
            uint32_t type;
            if (!stock_record_read(&v->db, p, track.end, "mhod", &child) ||
                child.body - p < 16 || !stock_db_u32(&v->db, p + 12, &type))
                break;
            if (type == 1)
                item.title = p;
            else if (type == 2)
                item.path = p;
            else if (type == 4)
                item.artist = p;
            else if (type == 19)
                item.show = p;
        }
        char path[MAX_PATH];
        if (text_at(v, item.path, path, sizeof(path)) && stock_video_path(path))
            v->tracks[v->count++] = item;
        yield();
    }
}

static int playlist_members(struct videos *v, const struct stock_record *playlist,
                            bool stop_at_first)
{
    struct stock_record r;
    uint32_t metadata, count;
    if (playlist->body - playlist->at < 21 ||
        !stock_db_u32(&v->db, playlist->at + 12, &metadata) ||
        !stock_db_u32(&v->db, playlist->at + 16, &count))
        return 0;

    off_t p = playlist->body;
    while (metadata--)
    {
        if (!stock_record_read(&v->db, p, playlist->end, "mhod", &r))
            return 0;
        p = r.end;
    }
    int shown = 0;
    while (count-- && shown < v->row_capacity)
    {
        uint32_t id;
        if (!stock_record_read(&v->db, p, playlist->end, "mhip", &r) ||
            r.body - p < 28 || !stock_db_u32(&v->db, p + 24, &id))
            break;
        int row = by_id(v, id);
        if (row >= 0)
        {
            if (stop_at_first)
                return 1;
            v->rows[shown++] = row;
        }
        p = r.end;
        if (r.body == r.end)
        {
            /* Older writers leave the member's MHODs outside its length. */
            while (stock_record_read(&v->db, p, playlist->end, "mhod", &r))
                p = r.end;
        }
    }
    return shown;
}

static void read_playlists(struct videos *v)
{
    struct stock_record list, playlist;
    if (!stock_db_list(&v->db, 3, "mhlp", &list) &&
        !stock_db_list(&v->db, 2, "mhlp", &list))
        return;
    for (off_t at = list.body; at < list.end && v->playlist_count < MAX_PLAYLISTS;
         at = playlist.end)
    {
        uint32_t flags;
        if (!stock_record_read(&v->db, at, list.end, "mhyp", &playlist) ||
            playlist.body - at < 24 || !stock_db_u32(&v->db, at + 20, &flags))
            break;
        if (!(flags & 0xff) && playlist_members(v, &playlist, true))
            v->playlists[v->playlist_count++] = at;
    }
}

static const char *video_title(int row, void *data, char *buf, size_t len)
{
    const struct videos *v = data;
    if (row == v->row_count)
        return "Loose video files";
    const struct video_track *item = &v->tracks[v->rows[row]];
    if (text_at(v, item->title, buf, len))
        return buf;
    if (text_at(v, item->path, buf, len))
    {
        const char *leaf = strrchr(buf, ':');
        return leaf ? leaf + 1 : buf;
    }
    return "";
}

static const char *playlist_title(int row, void *data, char *buf, size_t len)
{
    const struct videos *v = data;
    struct stock_record r;
    return stock_record_read(&v->db, v->playlists[row], v->db.end, "mhyp", &r) &&
           text_at(v, r.body, buf, len) ? buf : "Video playlist";
}

static enum themable_icons video_icon(int row, void *data)
{
    (void)row;
    (void)data;
    return filetype_get_icon(FILE_ATTR_VIDEO);
}

static const struct bitmap *video_cover(int row, void *data, struct dim *size)
{
    struct videos *v = data;
    if (row >= v->row_count || !v->cover)
        return NULL;
    int item = v->rows[row];
    struct video_track *track = &v->tracks[item];
    if (!track->artwork)
        return NULL;
    if (v->cached_cover != item)
    {
        v->source_edge = stock_artwork_pixels(&v->art, track->artwork, v->cover);
        v->cached_cover = -1;
        if (!v->source_edge)
            return NULL;
        v->cached_cover = item;
        v->bitmap.width = 0;
    }
    int edge = MIN(COVER_SIZE, MIN(size->width, size->height));
    if (edge <= 0)
        return NULL;
    if (v->bitmap.width != edge)
    {
        fb_data *scaled = v->cover + COVER_SOURCE * COVER_SOURCE;
        for (int y = 0; y < edge; y++)
            for (int x = 0; x < edge; x++)
                scaled[y * edge + x] =
                    v->cover[(y * v->source_edge / edge) * v->source_edge +
                             x * v->source_edge / edge];
        v->bitmap = (struct bitmap){.width = edge, .height = edge,
                                   .format = FORMAT_NATIVE, .data = (void *)scaled};
    }
    return &v->bitmap;
}

static int show_tracks(struct videos *v, char *title)
{
    if (!v->row_count && !v->loose)
        return 0;
    struct simplelist_info info;
    simplelist_info_init(&info, title, v->row_count + v->loose, v);
    info.get_name = video_title;
    info.action_callback = stock_media_list_action;
    info.get_icon = video_icon;
    info.get_albumart = video_cover;
    info.row_height = 54;
    info.selection = 0;
    while (true)
    {
        if (simplelist_show_list(&info))
            return MENU_ATTACHED_USB;
        int sel = info.selection;
        if (sel < 0)
            return 0;
        if (v->loose && sel == v->row_count)
        {
            if (browser_flat_loose_videos() == GO_TO_ROOT)
                return MENU_ATTACHED_USB;
            continue;
        }
#ifdef HAVE_VIDEO_PLAYER
        while (sel >= 0 && sel < v->row_count)
        {
            char path[MAX_PATH], label[MAX_PATH];
            const struct video_track *track = &v->tracks[v->rows[sel]];
            if (!text_at(v, track->path, path, sizeof(path)) ||
                !stock_video_path(path) || !file_exists(path))
            {
                splash(HZ, ID2P(LANG_FILE_NOT_FOUND));
                break;
            }
            const char *name = text_at(v, track->title, label, sizeof(label)) ?
                               label : NULL;
            info.selection = sel;
            int result = rock_open_video(path, name, sel > 0);
            if (result == ROCK_VIDEO_USB)
                return MENU_ATTACHED_USB;
            if (result == ROCK_VIDEO_PREVIOUS)
                sel--;
            else if (result == ROCK_VIDEO_FINISHED || result == ROCK_VIDEO_NEXT)
                sel++;
            else
                break;
        }
#endif
    }
}

static enum list_row_kind video_folder_kind(int row, void *data)
{
    (void)row;
    (void)data;
    return LIST_ROW_MENU;
}

static int show_playlists(struct videos *v, char *title)
{
    read_playlists(v);
    if (!v->playlist_count)
        return 0;
    struct simplelist_info info;
    simplelist_info_init(&info, title, v->playlist_count, v);
    info.get_name = playlist_title;
    info.get_kind = video_folder_kind;
    info.action_callback = stock_media_list_action;
    while (true)
    {
        if (simplelist_show_list(&info))
            return MENU_ATTACHED_USB;
        if (info.selection < 0)
            return 0;

        struct stock_record playlist;
        char name[96];
        int row = info.selection;
        if (!stock_record_read(&v->db, v->playlists[row], v->db.end, "mhyp", &playlist))
            return 0;
        uint32_t members;
        if (!stock_db_u32(&v->db, playlist.at + 16, &members))
            return 0;
        int row_handle = -1;
        int *previous_rows = v->rows;
        int previous_capacity = v->row_capacity;
        if (members > (unsigned)v->row_capacity)
        {
            unsigned capacity = MIN(members, MEDIA_LIMIT);
            row_handle = core_alloc(capacity * sizeof(*v->rows));
            if (row_handle > 0)
            {
                v->rows = core_get_data_pinned(row_handle);
                v->row_capacity = capacity;
            }
        }
        v->row_count = playlist_members(v, &playlist, false);
        const char *label = playlist_title(row, v, name, sizeof(name));
        if (label != name)
            snprintf(name, sizeof(name), "%s", label);
        int rc = show_tracks(v, name);
        if (row_handle > 0)
        {
            v->rows = previous_rows;
            v->row_capacity = previous_capacity;
            core_unpin(row_handle);
            core_free(row_handle);
        }
        if (rc)
            return rc;
    }
}

struct video_group_entry {
    char name[96];
    int track;
    uint32_t season;
    uint32_t episode;
};

struct video_groups {
    struct video_group_entry *entries;
    int *first;
    bool all;
};

static int compare_group(const void *left, const void *right)
{
    const struct video_group_entry *a = left;
    const struct video_group_entry *b = right;
    int order = strcasecmp(a->name, b->name);
    if (order)
        return order;
    if (a->season != b->season)
        return a->season < b->season ? -1 : 1;
    if (a->episode != b->episode)
        return a->episode < b->episode ? -1 : 1;
    return a->track - b->track;
}

static const char *group_title(int row, void *data, char *buf, size_t size)
{
    (void)buf;
    (void)size;
    const struct video_groups *groups = data;
    if (groups->all && row == 0)
        return str(LANG_ALL_ALBUMS);
    return groups->entries[groups->first[row - groups->all]].name;
}

static int show_groups(struct videos *v, char *title, bool television)
{
    int count = v->row_count;
    int handle = core_alloc(count * sizeof(struct video_group_entry) +
                            (count + 1) * sizeof(int));
    if (handle <= 0)
        return show_tracks(v, title);
    struct video_groups groups = {.entries = core_get_data_pinned(handle),
                                  .all = !television};
    groups.first = (int *)(groups.entries + count);
    for (int i = 0; i < count; i++)
    {
        struct video_group_entry *entry = &groups.entries[i];
        const struct video_track *track = &v->tracks[v->rows[i]];
        if (!text_at(v, television ? track->show : track->artist,
                     entry->name, sizeof(entry->name)))
            snprintf(entry->name, sizeof(entry->name), "%s",
                     str(television ? LANG_VIDEO_UNKNOWN_SHOW :
                                      LANG_VIDEO_UNKNOWN_ARTIST));
        entry->track = v->rows[i];
        entry->season = television ? track->season : 0;
        entry->episode = television ? track->episode : 0;
    }
    qsort(groups.entries, count, sizeof(*groups.entries), compare_group);
    int ngroups = 0;
    for (int i = 0; i < count; i++)
        if (i == 0 || strcasecmp(groups.entries[i-1].name, groups.entries[i].name))
            groups.first[ngroups++] = i;
    groups.first[ngroups] = count;

    struct simplelist_info info;
    simplelist_info_init(&info, title, ngroups + groups.all, &groups);
    info.get_name = group_title;
    info.get_kind = video_folder_kind;
    info.action_callback = stock_media_list_action;
    int rc = 0;
    while (!rc)
    {
        if (simplelist_show_list(&info))
        {
            rc = MENU_ATTACHED_USB;
            break;
        }
        if (info.selection < 0)
            break;
        int group = info.selection - groups.all;
        int first = group < 0 ? 0 : groups.first[group];
        int end = group < 0 ? count : groups.first[group + 1];
        v->row_count = 0;
        for (int i = first; i < end; i++)
            v->rows[v->row_count++] = groups.entries[i].track;
        char name[96];
        snprintf(name, sizeof(name), "%s",
                 group_title(info.selection, &groups, NULL, 0));
        rc = show_tracks(v, name);
    }
    core_unpin(handle);
    core_free(handle);
    return rc;
}

int stock_videos_screen(enum stock_video_kind kind, int title)
{
    struct videos v = {.art.fd = -1, .cached_cover = -1,
                       .loose = kind == STOCK_MOVIE};
    struct stock_record list;
    bool menu_back = action_set_menu_back(true);
    int rc = 0, handle = -1;
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "videos-push");
    if (!stock_db_open(&v.db, false) ||
        !stock_db_list(&v.db, 1, "mhlt", &list) || !list.count)
        goto leave;

    unsigned count = MIN(list.count, MEDIA_LIMIT);
    handle = core_alloc(count * (sizeof(*v.tracks) + sizeof(*v.rows)) +
                        MAX_PLAYLISTS * sizeof(*v.playlists));
    if (handle <= 0)
        goto leave;
    v.tracks = core_get_data_pinned(handle);
    v.playlists = (off_t *)(v.tracks + count);
    v.rows = (int *)(v.playlists + MAX_PLAYLISTS);
    v.row_capacity = count;
    read_tracks(&v, &list, count);
    read_artwork(&v);
    qsort(v.tracks, v.count, sizeof(*v.tracks), compare_id);
    if (kind == STOCK_VIDEO_PLAYLISTS)
        rc = show_playlists(&v, str(title));
    else
    {
        for (int i = 0; i < v.count; i++)
        {
            uint32_t media = v.tracks[i].media;
            unsigned category = (media & STOCK_TV_SHOW) ? STOCK_TV_SHOW
                              : (media & STOCK_MUSIC_VIDEO) ? STOCK_MUSIC_VIDEO
                              : STOCK_MOVIE;
            if (category == kind)
                v.rows[v.row_count++] = i;
        }
        if (v.row_count && kind != STOCK_MOVIE)
            rc = show_groups(&v, str(title), kind == STOCK_TV_SHOW);
        else if (v.row_count)
            rc = show_tracks(&v, str(title));
    }

leave:
    if (v.loose && !v.row_count && browser_flat_loose_videos() == GO_TO_ROOT)
        rc = MENU_ATTACHED_USB;
    if (handle > 0)
    {
        core_unpin(handle);
        core_free(handle);
    }
    if (v.cover_handle > 0)
    {
        core_unpin(v.cover_handle);
        core_free(v.cover_handle);
    }
    stock_db_close(&v.art);
    stock_db_close(&v.db);
    ui_scene_close_modal(&scene, !rc, "videos-pop");
    action_set_menu_back(menu_back);
    return rc;
}
