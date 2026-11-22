/* Foreground shortcuts to bundled plugins. GNU GPL version 2 or later. */
#define SHORTCUTS ROCKBOX_DIR "/plugin_shortcuts.txt"
#define SHORTCUTS_TEMP SHORTCUTS ".tmp"
#define MAX_SHORTCUTS 32

static const char *shortcuts[MAX_SHORTCUTS];
static int count;

static const char *allowlisted_path(const char *path)
{
    const char *candidate;
    for (unsigned int i = 0; (candidate = rb->plugin_path(i)); ++i)
        if (!rb->strcmp(candidate, path))
            return candidate;
    return NULL;
}

static int find_path(const char *path)
{
    for (int i = 0; i < count; ++i)
        if (shortcuts[i] == path)
            return i;
    return -1;
}

static void load_shortcuts(void)
{
    static char input[MAX_SHORTCUTS * MAX_PATH + 1];
    int fd = rb->open(SHORTCUTS, O_RDONLY);
    if (fd < 0)
        return;
    off_t length = rb->ffilesize(fd);
    bool read_ok = length > 0 && (size_t)length < sizeof(input) &&
                   rb->read(fd, input, length) == length;
    rb->close(fd);
    if (!read_ok)
        return;
    input[length] = '\0';
    if (rb->strlen(input) != (size_t)length)
        return;
    char *next = NULL;
    for (char *line = rb->strtok_r(input, "\r\n", &next);
         line && count < MAX_SHORTCUTS;
         line = rb->strtok_r(NULL, "\r\n", &next))
    {
        const char *path = allowlisted_path(line);
        if (path && find_path(path) < 0)
            shortcuts[count++] = path;
    }
}

static bool save_shortcuts(void)
{
    int fd = rb->open(SHORTCUTS_TEMP, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return false;
    bool saved = true;
    for (int i = 0; saved && i < count; ++i)
        saved = rb->fdprintf(fd, "%s\n", shortcuts[i]) >= 0;
    if (saved)
        saved = rb->file_sync(fd) == 0;
    if (rb->close(fd) || !saved || rb->rename(SHORTCUTS_TEMP, SHORTCUTS))
    {
        rb->remove(SHORTCUTS_TEMP);
        rb->splash(HZ, "Could not save plugin shortcuts");
        return false;
    }
    return true;
}

static const char *short_name(const char *path, char *buf, size_t size)
{
    const char *name = rb->strrchr(path, '/') + 1;
    const char *ext = rb->strrchr(name, '.');
    rb->snprintf(buf, size, "%.*s", (int)(ext - name), name);
    return buf;
}

static const char *shortcut_name(int row, void *arg, char *buf, size_t size)
{
    (void)arg;
    if (row == count)
        return "Add shortcut";
    return short_name(shortcuts[row], buf, size);
}

struct picker {
    const char **paths;
    int count;
    int selected;
};

static const char *picker_name(int row, void *arg, char *buf, size_t size)
{
    struct picker *picker = arg;
    return short_name(picker->paths[row], buf, size);
}

static int picker_action(int action, struct gui_synclist *list)
{
    struct picker *picker = list->data;
    if (action == ACTION_STD_OK)
    {
        picker->selected = rb->gui_synclist_get_sel_pos(list);
        return ACTION_STD_CANCEL;
    }
    return action;
}

static int add_shortcut(void)
{
    if (count == MAX_SHORTCUTS)
    {
        rb->splash(HZ, "Shortcut list is full");
        return PLUGIN_OK;
    }

    size_t bytes;
    const char **paths = rb->plugin_get_buffer(&bytes);
    if (!paths)
        return PLUGIN_ERROR;
    struct picker picker = {paths, 0, -1};
    for (unsigned int i = 0; (size_t)picker.count < bytes / sizeof(*paths);
         ++i)
    {
        const char *path = rb->plugin_path(i);
        if (!path)
            break;
        if (rb->strcmp(path, rb->plugin_get_current_filename()) &&
            find_path(path) < 0 && rb->file_exists(path))
            picker.paths[picker.count++] = path;
    }
    if (!picker.count)
    {
        rb->splash(HZ, "No more bundled plugins");
        return PLUGIN_OK;
    }

    struct simplelist_info list;
    rb->simplelist_info_init(&list, "Choose a plugin", picker.count, &picker);
    list.get_name = picker_name;
    list.action_callback = picker_action;
    if (rb->simplelist_show_list(&list))
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
    if (picker.selected >= 0)
    {
        shortcuts[count++] = picker.paths[picker.selected];
        if (!save_shortcuts())
            --count;
    }
    return PLUGIN_OK;
}

static int remove_shortcut(int row)
{
    static const char *lines[] = {"Remove selected shortcut?"};
    static const struct text_message request = {lines, ARRAYLEN(lines)};
    int result = rb->gui_syncyesno_run(&request, NULL, NULL);
    if (result != YESNO_YES)
        return result == YESNO_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;

    const char *path = shortcuts[row];
    --count;
    size_t bytes = (count - row) * sizeof(*shortcuts);
    rb->memmove(shortcuts + row, shortcuts + row + 1, bytes);
    if (!save_shortcuts())
    {
        rb->memmove(shortcuts + row + 1, shortcuts + row, bytes);
        shortcuts[row] = path;
        ++count;
    }
    return PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    count = 0;
    load_shortcuts();

    static struct gui_synclist list;
    rb->gui_synclist_init(&list, shortcut_name, NULL, false, 1, NULL);
    rb->gui_synclist_set_title(&list, "Open Plugins", Icon_Plugin);
    for (;;)
    {
        rb->gui_synclist_set_nb_items(&list, count + 1);
        rb->gui_synclist_draw(&list);
        int action = rb->get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        int row = rb->gui_synclist_get_sel_pos(&list);
        int result = PLUGIN_OK;
        if (action == ACTION_STD_OK)
        {
            if (row == count)
                result = add_shortcut();
            else
            {
                const char *path = shortcuts[row];
                if (rb->file_exists(path))
                    result = rb->queue_handoff(path, NULL);
                if (result != PLUGIN_GOTO_PLUGIN)
                {
                    rb->splash(HZ, "Plugin unavailable");
                    result = PLUGIN_OK;
                }
            }
        }
        else if (action == ACTION_STD_CONTEXT && row < count)
            result = remove_shortcut(row);
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            return PLUGIN_OK;
        else if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (result != PLUGIN_OK)
            return result;
    }
}
