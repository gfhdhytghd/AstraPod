/* Themed OTP account list and code display. GPL version 2 or later. */
static const char *account_row(int selection, void *data, char *text, size_t size)
{
    (void)data;
    rb->strlcpy(text, accounts[selection].name, size);
    return text;
}

static void choose_account(const char *title, void (*activate)(int))
{
    if (!next_slot)
    {
        rb->splash(HZ, "No accounts configured");
        return;
    }

    struct gui_synclist list;
    rb->gui_synclist_init(&list, account_row, NULL, false, 1, NULL);
    rb->gui_synclist_set_title(&list, title, Icon_Plugin);
    rb->gui_synclist_set_nb_items(&list, next_slot);
    while (next_slot && !save_failed && !rb->plugin_usb_taken())
    {
        rb->gui_synclist_draw(&list);
        int action = rb->get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (IS_SYSEVENT(action) &&
            rb->default_event_handler(action) == SYS_USB_CONNECTED)
            break;
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        if (action == ACTION_STD_OK)
        {
            int selected = rb->gui_synclist_get_sel_pos(&list);
            rb->gui_synclist_scroll_stop(&list);
            if (selected >= 0 && selected < next_slot)
                activate(selected);
            rb->gui_synclist_set_nb_items(&list, next_slot);
        }
    }
    rb->gui_synclist_scroll_stop(&list);
}

static void show_code(int selected)
{
    struct account_t *account = &accounts[selected];
    char text[64];
    if (!account->is_totp)
    {
        if (account->hotp_counter == UINT64_MAX)
        {
            rb->splash(HZ, "HOTP counter exhausted");
            return;
        }
        int code = HOTP(account->secret, account->sec_len,
                        account->hotp_counter++, account->digits);
        if (!save_accts())
            return;
        rb->snprintf(text, sizeof(text), "%0*d", account->digits, code);
    }
#if CONFIG_RTC
    else
    {
        time_t now = get_utc();
        if (now < 0)
        {
            rb->splash(HZ, "Set the player clock");
            return;
        }
        unsigned long left = account->totp_period
                             - (uint64_t)now % account->totp_period;
        int code = TOTP(account->secret, account->sec_len,
                        account->totp_period, account->digits);
        rb->snprintf(text, sizeof(text), "%0*d\n%lu seconds left",
                     account->digits, code, left);
    }
#else
    else
    {
        rb->splash(HZ, "TOTP requires a real-time clock");
        return;
    }
#endif
    rb->view_text(account->name, text);
}

static void gen_codes(void)
{
    choose_account("Generate Code", show_code);
}
