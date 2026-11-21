/* Native list presentation for upstream rb_info. GPL version 2 or later. */

enum info_page {
    ROOT, PATHS, BUFFERS, BUTTONS, BUTTON_TEST, ACTIONS, CONTEXTS,
    ACTION_TEST, ACTION_CONTEXT, PLUGINS, PRINTCELL, PRINTCELL_COLUMNS, QUIT,
};

static const char *const titles[] = {
    [ROOT] = "Rockbox Info Plugin",
    [PATHS] = ID2P(LANG_SHOW_PATH),
    [BUFFERS] = ID2P(LANG_BUFFER_STAT),
    [BUTTONS] = "Buttons",
    [BUTTON_TEST] = "Button test",
    [ACTIONS] = "Actions",
    [CONTEXTS] = "Contexts",
    [ACTION_TEST] = "Action test",
    [ACTION_CONTEXT] = "Select context",
    [PLUGINS] = ID2P(LANG_PLUGINS),
    [PRINTCELL] = "Printcell test",
    [PRINTCELL_COLUMNS] = "Printcell columns",
    [QUIT] = ID2P(LANG_MENU_QUIT),
};

/* The root has no entry for the drill-down page. */
static const enum info_page root_items[] = {
    PATHS, BUFFERS, BUTTONS, BUTTON_TEST, ACTIONS, CONTEXTS, ACTION_TEST,
    PLUGINS, PRINTCELL, QUIT,
};

/* The upstream table has 36 data rows and eight headed columns. */
#define SAMPLE_ROWS 36
#define SAMPLE_COLUMNS 8

static struct {
    struct gui_synclist list;
    enum info_page page;
    int root_selection;
    int row_count;
    int sample_row;
    int context;
    unsigned int input;
    bool listening;
    char sample_title[32];
} info;

static const char *action_label(unsigned int action)
{
    if (action < ARRAYLEN(action_names) && action_names[action])
        return action_names[action];
    return "Unlisted action";
}

static void button_result(unsigned int value, char *buf, size_t size)
{
    int used = 0;
    for (size_t i = 0; i < ARRAYLEN(buttons) && (size_t)used < size; i++)
    {
        unsigned int mask = buttons[i].value;
        if (mask ? (value & mask) == mask : value == 0)
            used += rb->snprintf(buf + used, size - used, "%s%s",
                                 used ? " | " : "", buttons[i].name);
    }
    if (used == 0)
        rb->snprintf(buf, size, "Unlisted button");
}

static const char *list_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    if (info.page != ROOT && row == info.row_count - 1)
        return ID2P(LANG_BACK);

    switch (info.page)
    {
        case ROOT:
            return titles[root_items[row]];
        case PATHS:
            rb->snprintf(buf, size, "%s: [%s]", paths[row].name, paths[row].path);
            break;
        case BUFFERS:
#ifndef CACHE_SIZE
            if (row == ARRAYLEN(m_buffer))
                return "cache: size unavailable";
#endif
            rb->snprintf(buf, size, "%s: [%lu bytes]", m_buffer[row].name,
                         (unsigned long)m_buffer[row].size);
            break;
        case BUTTONS:
            rb->snprintf(buf, size, "%s: [0x%X]", buttons[row].name,
                         buttons[row].value);
            break;
        case BUTTON_TEST:
        case ACTION_TEST:
            if (row == 0)
                return info.listening ? "Listening (4 s idle timeout)" : "Start test";
            if (info.page == BUTTON_TEST)
            {
                unsigned int flags = info.input & (BUTTON_REL | BUTTON_REPEAT);
                if (row == 1)
                    button_result(info.input & ~flags, buf, size);
                else if (row == 2)
                {
                    if (!flags)
                        return "No release/repeat flags";
                    button_result(flags, buf, size);
                }
                else
                    rb->snprintf(buf, size, "0x%X", info.input);
            }
            else if (row == 1)
                rb->snprintf(buf, size, "Context: %s",
                             context_names[info.context]);
            else if (row == 2)
                return action_label(info.input);
            else
                rb->snprintf(buf, size, "%u / 0x%X", info.input, info.input);
            break;
        case ACTIONS:
            rb->snprintf(buf, size, "%s [%d / 0x%X]", action_label(row), row, row);
            break;
        case CONTEXTS:
        case ACTION_CONTEXT:
            rb->snprintf(buf, size, "%s [%d / 0x%X]", context_names[row], row, row);
            break;
        case PLUGINS:
            switch (row)
            {
                case 0:
                    rb->snprintf(buf, size, "plugin_api: [%lu bytes]",
                                 (unsigned long)sizeof(struct plugin_api));
                    break;
                case 1:
                    rb->snprintf(buf, size, "plugin buffer: [%lu bytes]",
                                 (unsigned long)PLUGIN_BUFFER_SIZE);
                    break;
                case 2:
                    rb->snprintf(buf, size, "frame_buffer: [%lu bytes]",
                                 (unsigned long)FRAMEBUFFER_SIZE);
                    break;
                case 3:
                    rb->snprintf(buf, size, "LCD: [W: %d H: %d]",
                                 LCD_WIDTH, LCD_HEIGHT);
                    break;
                case 4:
                    rb->snprintf(buf, size, "fb_data: [%d bits]",
                                 (int)(sizeof(fb_data) * CHAR_BIT));
                    break;
            }
            break;
        case PRINTCELL:
            rb->snprintf(buf, size, "put_line item: [ %d ]", row);
            break;
        case PRINTCELL_COLUMNS:
            switch (row)
            {
                case 0:
                    rb->snprintf(buf, size, "col1: put_line item: [ %d ]",
                                 info.sample_row);
                    break;
                case 1:
                    return "col2: Text 1";
                case 2:
                    return "col3: Text LONGER TEST text 2";
                default:
                    rb->snprintf(buf, size, "col%d: %d", row + 1, row + 1);
                    break;
            }
            break;
        case QUIT:
            break;
    }
    return buf;
}

static int list_voice(int row, void *data)
{
    if (!rb->global_settings->talk_menu)
        return 0;
    char buf[256];
    const char *name = list_name(row, data, buf, sizeof(buf));
    long id = P2ID((const unsigned char *)name);
    if (id >= 0)
        rb->talk_id(id, true);
    else
        rb->talk_spell(name, true);
    return 0;
}

static void show_page(enum info_page page, int selection)
{
    static const int counts[] = {
        [ROOT] = ARRAYLEN(root_items),
        [PATHS] = ARRAYLEN(paths),
#ifdef CACHE_SIZE
        [BUFFERS] = ARRAYLEN(m_buffer),
#else
        [BUFFERS] = ARRAYLEN(m_buffer) + 1,
#endif
        [BUTTONS] = ARRAYLEN(buttons),
        [BUTTON_TEST] = 4,
        [ACTIONS] = ARRAYLEN(action_names),
        [CONTEXTS] = ARRAYLEN(context_names),
        [ACTION_TEST] = 4,
        [ACTION_CONTEXT] = ARRAYLEN(context_names),
        [PLUGINS] = 5,
        [PRINTCELL] = SAMPLE_ROWS,
        [PRINTCELL_COLUMNS] = SAMPLE_COLUMNS,
    };
    info.page = page;
    info.row_count = counts[page] + (page != ROOT);
    rb->gui_synclist_init(&info.list, list_name, NULL, false, 1, NULL);
    rb->gui_synclist_set_title(&info.list,
        page == PRINTCELL_COLUMNS ? info.sample_title : titles[page], Icon_NOICON);
    rb->gui_synclist_set_voice_callback(&info.list, list_voice);
    rb->gui_synclist_set_nb_items(&info.list, info.row_count);
    rb->gui_synclist_select_item(&info.list, selection);
    rb->gui_synclist_draw(&info.list);
    rb->gui_synclist_speak_item(&info.list);
}

static enum plugin_status capture_input(bool actions)
{
    info.listening = true;
    info.input = 0;
    rb->gui_synclist_draw(&info.list);

    long deadline = *rb->current_tick + 4 * HZ;
    while (TIME_BEFORE(*rb->current_tick, deadline))
    {
        long event = actions ? rb->get_action(info.context, HZ / 10)
                             : rb->button_get_w_tmo(HZ / 10);
        if (IS_SYSEVENT(event))
        {
            if (rb->default_event_handler(event) == SYS_USB_CONNECTED)
                return PLUGIN_USB_CONNECTED;
        }
        else if (event == (actions ? ACTION_REDRAW : BUTTON_REDRAW))
            rb->gui_synclist_draw(&info.list);
        else if (event != BUTTON_NONE)
        {
            info.input = event;
            deadline = *rb->current_tick + 4 * HZ;
            rb->gui_synclist_draw(&info.list);
        }
    }
    info.listening = false;
    rb->gui_synclist_draw(&info.list);
    return PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    rb->memset(&info, 0, sizeof(info));
    show_page(ROOT, 0);

    for (;;)
    {
        int action = rb->get_action(CONTEXT_LIST, HZ / 10);
        if (IS_SYSEVENT(action) &&
            rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (rb->gui_synclist_do_button(&info.list, &action))
            continue;

        int row = rb->gui_synclist_get_sel_pos(&info.list);
        bool back = info.page != ROOT && row == info.row_count - 1;
        if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL ||
            (action == ACTION_STD_OK && back))
        {
            if (info.page == ROOT)
                return PLUGIN_OK;
            if (info.page == ACTION_CONTEXT)
                show_page(ACTION_TEST, 1);
            else if (info.page == PRINTCELL_COLUMNS)
                show_page(PRINTCELL, info.sample_row);
            else
                show_page(ROOT, info.root_selection);
        }
        else if (action == ACTION_STD_OK)
        {
            if (info.page == ROOT)
            {
                enum info_page page = root_items[row];
                if (page == QUIT)
                    return PLUGIN_OK;
                info.root_selection = row;
                info.context = CONTEXT_STD;
                info.input = 0;
                show_page(page, 0);
            }
            else if (info.page == PRINTCELL)
            {
                info.sample_row = row;
                rb->snprintf(info.sample_title, sizeof(info.sample_title),
                             "Printcell row %d", row);
                show_page(PRINTCELL_COLUMNS, 0);
            }
            else if (info.page == ACTION_TEST && row == 1)
            {
                show_page(ACTION_CONTEXT, info.context);
            }
            else if (info.page == ACTION_CONTEXT)
            {
                info.context = row;
                info.input = ACTION_NONE;
                show_page(ACTION_TEST, 1);
            }
            else if ((info.page == BUTTON_TEST || info.page == ACTION_TEST) && row == 0)
            {
                enum plugin_status status = capture_input(info.page == ACTION_TEST);
                if (status != PLUGIN_OK)
                    return status;
            }
        }
    }
}
