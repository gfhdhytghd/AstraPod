
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    MENUITEM_STRINGLIST(menu, ID2P(LANG_ANNOUNCE_STATUS), NULL,
                        "Announce now", "Track announcement",
                        ID2P(LANG_ANNOUNCEMENT_FMT), ID2P(LANG_TIMEOUT),
                        ID2P(LANG_GROUPING), ID2P(LANG_MENU_QUIT));

    struct rock_announce_status options;
    rb->rock_announce_status_get(&options);
    int selection = 0;
    for (;;)
    {
        switch (rb->do_menu(&menu, &selection, NULL, false))
        {
        case 0:
            if (!rb->rock_announce_status_speak())
                rb->splash(2 * HZ, "Speech unavailable");
            break;
        case 1:
            if (!rb->set_bool("Track announcement", &options.track_change))
                rb->rock_announce_status_set(&options);
            break;
        case 2:
            if (rb->kbd_input(options.format, sizeof(options.format),
                              NULL) >= 0)
                rb->rock_announce_status_set(&options);
            break;
        case 3:
            if (!rb->set_int(rb->str(LANG_TIMEOUT), "s", UNIT_SEC,
                             &options.interval, NULL, 1, 1, 360, NULL))
                rb->rock_announce_status_set(&options);
            break;
        case 4:
            if (!rb->set_int(rb->str(LANG_GROUPING), "", 1,
                             &options.grouping, NULL, 1, 0, 7, NULL))
                rb->rock_announce_status_set(&options);
            break;
        case MENU_ATTACHED_USB:
            return PLUGIN_USB_CONNECTED;
        default:
            return PLUGIN_OK;
        }
        if (rb->plugin_usb_taken())
            return PLUGIN_USB_CONNECTED;
        rb->rock_announce_status_get(&options);
    }
}
