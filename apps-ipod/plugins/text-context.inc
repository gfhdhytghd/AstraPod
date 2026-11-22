/* File menu entries for text utilities. GPL version 2 or later. */
static int run_text_utility(void *module)
{
    int result = rock_open_from_browser(module, selected_file.path);
    context_menu_result = result == GO_TO_ROOT ? ONPLAY_MAINMENU
                                               : ONPLAY_RELOAD_DIR;
    return result;
}

static int text_utility_callback(int action, const struct menu_item_ex *item,
                                 struct gui_synclist *list)
{
    (void)list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    if (selected_file.context != CONTEXT_TREE || !selected_file.path ||
        (selected_file.attr & ATTR_DIRECTORY))
        return ACTION_EXIT_MENUITEM;

    const char *name = strrchr(selected_file.path, '/');
    const char *ext = strrchr(name ? name + 1 : selected_file.path, '.');
    if (!ext || (strcasecmp(ext, ".txt") && strcasecmp(ext, ".md")))
        return ACTION_EXIT_MENUITEM;
    return file_exists(item->function_param->param) ? action
                                                    : ACTION_EXIT_MENUITEM;
}

MENUITEM_FUNCTION_W_PARAM(rock_edit_text_item, MENU_FUNC_CHECK_RETVAL,
                          "Edit Text", run_text_utility,
                          PLUGIN_APPS_DIR "/text_editor.rock",
                          text_utility_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(rock_sort_text_item, MENU_FUNC_CHECK_RETVAL,
                          "Sort Text Lines", run_text_utility,
                          PLUGIN_APPS_DIR "/sort.rock",
                          text_utility_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(rock_speedread_item, MENU_FUNC_CHECK_RETVAL,
                          "Speedread", run_text_utility,
                          PLUGIN_APPS_DIR "/speedread.rock",
                          text_utility_callback, Icon_NOICON);
