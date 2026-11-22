/* Bitmap plugin actions in the Files context menu. GPL version 2 or later. */
static const char rockpaint_module[] = PLUGIN_APPS_DIR "/rockpaint.rock";
static const char sliding_puzzle_module[] =
    PLUGIN_GAMES_DIR "/sliding_puzzle.rock";

static int run_bitmap_plugin(const char *module, bool may_write)
{
    int result = rock_open_from_browser(module, selected_file.path);
    context_menu_result = result == GO_TO_ROOT ? ONPLAY_MAINMENU :
                          may_write ? ONPLAY_RELOAD_DIR : ONPLAY_OK;
    return result;
}

static int edit_bitmap(void)
{
    return run_bitmap_plugin(rockpaint_module, true);
}

static int play_bitmap_puzzle(void)
{
    return run_bitmap_plugin(sliding_puzzle_module, false);
}

static int bitmap_plugin_callback(int action, const char *module)
{
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    if (selected_file.context != CONTEXT_TREE || !selected_file.path ||
        (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_IMG)
        return ACTION_EXIT_MENUITEM;
    const char *extension = strrchr(selected_file.path, '.');
    if (!extension || strcasecmp(extension, ".bmp") || !file_exists(module))
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int edit_bitmap_callback(int action, const struct menu_item_ex *item,
                                struct gui_synclist *list)
{
    (void)item;
    (void)list;
    return bitmap_plugin_callback(action, rockpaint_module);
}

static int play_bitmap_puzzle_callback(int action,
                                        const struct menu_item_ex *item,
                                        struct gui_synclist *list)
{
    (void)item;
    (void)list;
    return bitmap_plugin_callback(action, sliding_puzzle_module);
}

MENUITEM_FUNCTION(edit_bitmap_item, MENU_FUNC_CHECK_RETVAL,
                  "Edit Bitmap", edit_bitmap, edit_bitmap_callback,
                  Icon_NOICON);
MENUITEM_FUNCTION(play_bitmap_puzzle_item, MENU_FUNC_CHECK_RETVAL,
                  "Play Sliding Puzzle", play_bitmap_puzzle,
                  play_bitmap_puzzle_callback, Icon_NOICON);
