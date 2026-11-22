/* File menu entries for foreground audio utilities. GPL version 2 or later. */
static int run_audio_utility(const char *module)
{
    int result = rock_open_from_browser(module, selected_file.path);
    context_menu_result = result == GO_TO_ROOT ? ONPLAY_MAINMENU
                                               : ONPLAY_RELOAD_DIR;
    return result;
}

static bool offer_audio_utility(const char *extension, const char *module)
{
    if (selected_file.context != CONTEXT_TREE || !selected_file.path ||
        (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        return false;
    const char *ext = strrchr(selected_file.path, '.');
    return ext && !strcasecmp(ext, extension) && file_exists(module);
}

#define AUDIO_UTILITY(action_name, module, extension, label)                  \
    static int action_name(void)                                             \
    {                                                                        \
        return run_audio_utility(VIEWERS_DIR "/" module ".rock");             \
    }                                                                        \
    static int action_name##_callback(int action,                             \
                                      const struct menu_item_ex *item,        \
                                      struct gui_synclist *list)              \
    {                                                                        \
        (void)item;                                                          \
        (void)list;                                                          \
        if (action == ACTION_REQUEST_MENUITEM &&                              \
            !offer_audio_utility(extension, VIEWERS_DIR "/" module ".rock")) \
            return ACTION_EXIT_MENUITEM;                                     \
        return action;                                                       \
    }                                                                        \
    MENUITEM_FUNCTION(action_name##_item, MENU_FUNC_CHECK_RETVAL, label,      \
                      action_name, action_name##_callback, Icon_NOICON)

AUDIO_UTILITY(encode_mp3, "mp3_encoder", ".wav", "Encode MP3");
AUDIO_UTILITY(encode_wavpack, "wav2wv", ".wav", "Encode WavPack");
AUDIO_UTILITY(repair_vbr, "vbrfix", ".mp3", "Repair VBR seek table");
#undef AUDIO_UTILITY
