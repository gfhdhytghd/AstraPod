/* AstraPod playback, library refresh and entry for PictureFlow. GPL-2.0-or-later. */
static bool pf_append_current(int position)
{
    int first = insert_whole_album ? 0 : pf_tracks.sel;
    int end = insert_whole_album ? pf_tracks.count : first + 1;
    bool ok = true;
    for (int i = first; i < end; i++)
    {
        if (rb->playlist_insert_track(NULL, get_track_filename(i),
                                      position, false, false) < 0)
        {
            ok = false;
            break;
        }
        if (position == PLAYLIST_INSERT_FIRST)
            position = PLAYLIST_INSERT;
    }
    rb->playlist_sync(NULL);
    return ok;
}

static int pf_append_catalog(const char *path, bool new_playlist)
{
    int fd = rb->open(path, O_CREAT | O_WRONLY |
                     (new_playlist ? O_TRUNC : O_APPEND), 0666);
    if (fd < 0)
        return -1;

    int first = insert_whole_album ? 0 : pf_tracks.sel;
    int end = insert_whole_album ? pf_tracks.count : first + 1;
    bool ok = true;
    for (int i = first; i < end; i++)
        if (rb->fdprintf(fd, "%s\n", get_track_filename(i)) < 0)
        {
            ok = false;
            break;
        }

    return rb->close(fd) == 0 && ok ? 0 : -1;
}

static bool track_list_ready(void)
{
    if (pf_state != pf_show_tracks)
    {
        create_track_index(center_slide.slide_index);
        if (pf_tracks.count)
            reset_track_list();
    }
    return pf_tracks.count > 0;
}

static int context_menu(void)
{
    if (!track_list_ready())
        return 0;

    insert_whole_album = pf_state != pf_show_tracks || show_tracks_while_browsing;
    const char *path = get_track_filename(insert_whole_album ? 0 : pf_tracks.sel);
    MENUITEM_STRINGLIST(actions, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                        ID2P(LANG_PLAYING_NEXT), ID2P(LANG_ADD_TO_PL),
                        ID2P(LANG_CREATE_PLAYLIST), ID2P(LANG_MENU_SHOW_ID3_INFO));

    int action = rb->do_menu(&actions, NULL, NULL, false);
    int ret = PLUGIN_OK;
    switch (action)
    {
        case 0:
            if (!pf_append_current(PLAYLIST_INSERT_FIRST))
                rb->splash(HZ, "Could not queue PictureFlow selection");
            break;
        case 1:
        case 2:
        {
            char album[MAX_PATH];
            if (insert_whole_album)
                rb->snprintf(album, sizeof(album), "/%s", get_album_name(center_index));
            if (rb->rock_pf_catalog(insert_whole_album ? album : path,
                                    insert_whole_album ? ATTR_DIRECTORY : FILE_ATTR_AUDIO,
                                    action == 2, pf_append_catalog))
                ret = PLUGIN_USB_CONNECTED;
            break;
        }
        case 3:
            if (rb->get_metadata(&id3, -1, path) && rb->rock_pf_track_info(&id3))
                ret = PLUGIN_USB_CONNECTED;
            break;
        case MENU_ATTACHED_USB:
            ret = PLUGIN_USB_CONNECTED;
            break;
    }

    if (pf_state != pf_show_tracks)
        clear_track_index();
    return ret;
}

static bool start_playback(bool return_to_wps)
{
    (void)return_to_wps;
    if (!track_list_ready() || !rb->rock_pf_confirm_play() ||
        !rb->rock_pf_db_current(&pf_idx.summary))
        return false;

    insert_whole_album = true;
    if (rb->playlist_create(NULL, NULL) < 0 ||
        !pf_append_current(PLAYLIST_INSERT_LAST))
    {
        rb->splash(HZ, "Could not start PictureFlow album");
        return false;
    }

    int first = pf_tracks.sel;
    if (rb->global_settings->playlist_shuffle)
        first = rb->playlist_shuffle(0, first);
    rb->playlist_start(first, 0, 0);
    return true;
}
