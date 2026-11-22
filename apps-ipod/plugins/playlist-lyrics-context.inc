/* File browser routes for CUE export and Lrcplayer. GPL version 2 or later. */
static const char cue_playlist_path[] = PLUGIN_APPS_DIR "/cue_playlist.rock";
static const char lrcplayer_path[] = PLUGIN_APPS_DIR "/lrcplayer.rock";

static int open_playlist_or_lyrics_plugin(const char *module)
{
    int result = rock_open_from_browser(module, selected_file.path);
    context_menu_result = result == GO_TO_ROOT ? ONPLAY_MAINMENU
                                              : ONPLAY_RELOAD_DIR;
    return result;
}

static int export_playlist_cue(void)
{
    return open_playlist_or_lyrics_plugin(cue_playlist_path);
}

static int open_lrcplayer_file(void)
{
    return open_playlist_or_lyrics_plugin(lrcplayer_path);
}

static int export_playlist_cue_callback(int action,
                                        const struct menu_item_ex *item,
                                        struct gui_synclist *list)
{
    (void)item;
    (void)list;
    if (action == ACTION_REQUEST_MENUITEM &&
        (selected_file.context != CONTEXT_TREE || !selected_file.path ||
         (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_M3U ||
         !file_exists(cue_playlist_path)))
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int open_lrcplayer_file_callback(int action,
                                        const struct menu_item_ex *item,
                                        struct gui_synclist *list)
{
    (void)item;
    (void)list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    if (selected_file.context != CONTEXT_TREE || !selected_file.path ||
        (selected_file.attr & ATTR_DIRECTORY) ||
        !(audio_status() & AUDIO_STATUS_PLAY) || !file_exists(lrcplayer_path))
        return ACTION_EXIT_MENUITEM;
    const char *ext = strrchr(selected_file.path, '.');
    if (!ext || (strcasecmp(ext, ".lrc") && strcasecmp(ext, ".lrc8") &&
                 strcasecmp(ext, ".snc")))
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_FUNCTION(export_playlist_cue_item, MENU_FUNC_CHECK_RETVAL,
                  "Export CUE", export_playlist_cue,
                  export_playlist_cue_callback, Icon_NOICON);

MENUITEM_FUNCTION(open_lrcplayer_file_item, MENU_FUNC_CHECK_RETVAL,
                  "Open in Lrcplayer", open_lrcplayer_file,
                  open_lrcplayer_file_callback, Icon_NOICON);
