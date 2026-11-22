/* PictureFlow's transient album and track indexes. GPL-2.0-or-later. */
static const char *get_album_name(int index)
{
    return pf_idx.album_index[index].name;
}

static const char *get_album_artist(int index)
{
    return pf_idx.album_index[index].artist;
}

static const char *get_album_name_idx(int index, int *seek)
{
    *seek = pf_idx.album_index[index].row->seek;
    return get_album_name(index);
}

static const char *get_slide_name(int index, bool artist)
{
    return artist ? get_album_artist(index) : get_album_name(index);
}

static int compare_albums(const void *lhs, const void *rhs)
{
    const struct pf_album *a = lhs, *b = rhs;
    int artist = rb->strcasecmp(a->artist, b->artist);
    int album = rb->strcasecmp(a->name, b->name);
    int year = a->row->year - b->row->year;
    if (pf_cfg.year_sort_order == DESCENDING)
        year = -year;

    switch (pf_cfg.sort_albums_by)
    {
        case SORT_BY_ARTIST_AND_YEAR:
            if (!artist && year)
                return year;
            break;
        case SORT_BY_YEAR:
            if (year)
                return year;
            break;
        case SORT_BY_NAME:
            if (album)
                return album;
            break;
    }
    if (artist)
        return artist;
    if (album)
        return album;
    /* Use the native tag identities to break case-insensitive ties. */
    if (a->row->artist_seek != b->row->artist_seek)
        return a->row->artist_seek < b->row->artist_seek ? -1 : 1;
    return a->row->seek < b->row->seek ? -1 : a->row->seek != b->row->seek;
}

static int create_album_index(void *buffer, size_t bytes)
{
    int ret = rb->rock_pf_db_open(&pf_idx.summary, buffer, bytes);
    if (ret < 0)
        return ret;
    int count = pf_idx.summary.album_ct;
    size_t left = pf_idx.summary.buf_sz;
    void *next = pf_idx.summary.buf;
    ALIGN_BUFFER(next, left, __alignof__(struct pf_album));
    /* The stock animation uses a signed 16.16 slide coordinate. */
    if (count >= INT16_MAX || (size_t)count > left / sizeof(struct pf_album))
        return ERROR_BUFFER_FULL;

    pf_idx.album_index = next;
    char *name = (char *)(pf_idx.album_index + count);
    left -= count * sizeof(struct pf_album);

    for (int i = 0; i < count; i++)
    {
        struct pf_album *album = &pf_idx.album_index[i];
        album->row = &pf_idx.summary.album_index[i];
        if (i && album->row->artist_seek == album[-1].row->artist_seek)
            album->artist = album[-1].artist;
        else
        {
            char text[TAGCACHE_BUFSZ];
            rb->rock_pf_db_name(&pf_idx.summary, tag_albumartist,
                                album->row->artist_seek, text, sizeof(text));
            size_t length = rb->strlen(text) + 1;
            if (length > left)
                return ERROR_BUFFER_FULL;
            album->artist = name;
            rb->memcpy(name, text, length);
            name += length;
            left -= length;
        }

        char text[TAGCACHE_BUFSZ];
        rb->rock_pf_db_name(&pf_idx.summary, tag_album, album->row->seek,
                            text, sizeof(text));
        size_t length = rb->strlen(text) + 1;
        if (length > left)
            return ERROR_BUFFER_FULL;
        album->name = name;
        rb->memcpy(name, text, length);
        name += length;
        left -= length;
    }

    if (!rb->rock_pf_db_current(&pf_idx.summary))
        return ROCK_PF_DB_BUSY;
    pf_idx.album_ct = count;
    pf_idx.album_untagged_idx = pf_idx.summary.album_untagged_seek;
    pf_idx.buf = name;
    pf_idx.buf_sz = left;
    rb->qsort(pf_idx.album_index, count, sizeof(struct pf_album), compare_albums);
    return SUCCESS;
}

static int compare_tracks(const void *lhs, const void *rhs)
{
    const struct track_data *a = lhs, *b = rhs;
    if (a->disc != b->disc)
        return a->disc < b->disc ? -1 : 1;
    if (a->track != b->track)
        return a->track < b->track ? -1 : 1;
    return a->order - b->order;
}

static char *get_track_name(int index)
{
    return pf_tracks.names + pf_tracks.index[index].name_idx;
}

static char *get_track_filename(int index)
{
    return pf_tracks.names + pf_tracks.index[index].filename_idx;
}

static void clear_track_index(void)
{
    pf_tracks.cur_idx = -1;
    pf_tracks.count = 0;
}

static void create_track_index(int index)
{
    if (pf_tracks.cur_idx == index)
        return;

    clear_track_index();
    struct tagcache_search search;
    if (!rb->rock_pf_tracks_open(&pf_idx.summary, pf_idx.album_index[index].row,
                                 &search))
        return;

    struct rock_pf_track track;
    char *text = pf_tracks.names;
    struct track_data *rows = (void *)(text + pf_track_bytes);
    bool fits = true;
    while (rb->rock_pf_track_next(&search, &track))
    {
        size_t title_bytes = rb->strlen(track.title) + 32;
        size_t path_bytes = rb->strlen(track.path) + 1;
        if ((char *)rows - text <
            (ptrdiff_t)(sizeof(*rows) + title_bytes + path_bytes))
        {
            fits = false;
            break;
        }

        struct track_data *row = --rows;
        row->disc = MAX(0, track.disc);
        row->track = MAX(0, track.track);
        row->order = pf_tracks.count++;
        row->name_idx = text - pf_tracks.names;
        int len;
        if (row->disc && row->track)
            len = rb->snprintf(text, title_bytes, "%d.%02d: %s", row->disc,
                               row->track, track.title);
        else if (row->track)
            len = rb->snprintf(text, title_bytes, "%d: %s", row->track,
                               track.title);
        else
            len = rb->snprintf(text, title_bytes, "%s", track.title);
        text += len + 1;
        row->filename_idx = text - pf_tracks.names;
        rb->memcpy(text, track.path, path_bytes);
        text += path_bytes;
    }
    rb->tagcache_search_finish(&search);
    if (!fits)
    {
        clear_track_index();
        rb->splash(HZ, "Album track list exceeds PictureFlow memory");
        return;
    }

    pf_tracks.index = rows;
    rb->qsort(rows, pf_tracks.count, sizeof(*rows), compare_tracks);
    pf_tracks.cur_idx = index;
}
