/* Standalone entry points for native services. GPL version 2 or later. */
#if defined(ROCK_SPECIALIST_PROPERTIES)
enum plugin_status plugin_start(const void *parameter)
{
    const char *file = parameter;
    char path[MAX_PATH];
    if (!file)
    {
        int picked = rb->pick_file(path, sizeof(path), "/", "Choose a file",
                                   NULL, 0, false);
        if (picked <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        file = path;
    }
    return rb->properties(file);
}
#elif defined(ROCK_SPECIALIST_MAIN_MENU_CONFIG)
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    return rb->main_menu_config();
}
#elif defined(ROCK_SPECIALIST_THEME_REMOVE)
enum plugin_status plugin_start(const void *parameter)
{
    if (parameter)
        return PLUGIN_ERROR;

    MENUITEM_STRINGLIST(menu, "Remove Theme", NULL, "Remove skin preset",
                        "Remove native theme");
    int selected = 0;
    int mode = rb->do_menu(&menu, &selected, NULL, false);
    if (mode < 0)
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;

    char path[MAX_PATH];
    int picked = rb->pick_file(path, sizeof(path),
                               mode == 0 ? THEME_DIR :
                               ROCKBOX_DIR "/native-themes",
                               "Choose a theme",
                               mode == 0 ? ".cfg" : ".podtheme", 0, true);
    if (picked <= 0)
    {
        if (rb->plugin_usb_taken())
            return PLUGIN_USB_CONNECTED;
        if (picked < 0)
            rb->splash(HZ, "No theme directory");
        return PLUGIN_OK;
    }

    const char *lines[] = { "Remove this theme file?", path };
    const struct text_message prompt = { lines, ARRAYLEN(lines) };
    enum yesno_res answer = rb->gui_syncyesno_run(&prompt, NULL, NULL);
    if (answer == YESNO_USB || rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (answer == YESNO_YES)
        rb->splash(HZ, rb->remove_theme_file(path) == 0 ?
                      "Theme file removed" : "Theme file protected or unavailable");
    return PLUGIN_OK;
}
#elif defined(ROCK_SPECIALIST_PITCH_SCREEN)
enum plugin_status plugin_start(const void *parameter)
{
    if (parameter)
        return PLUGIN_ERROR;
    return rb->pitch_preview();
}
#endif
