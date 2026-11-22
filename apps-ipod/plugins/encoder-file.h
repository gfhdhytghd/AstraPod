/* Checked output for the foreground audio utilities. GPL version 2 or later. */
#ifndef ASTRAPOD_ENCODER_FILE_H
#define ASTRAPOD_ENCODER_FILE_H

struct encoder_file {
    int input;
    int output;
    char temp[MAX_PATH];
};

static bool encoder_confirm(const char *prompt)
{
    const char *lines[] = {prompt};
    const struct text_message message = {lines, ARRAYLEN(lines)};
    return rb->gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES;
}

static inline bool encoder_destination(char *output, const char *input,
                                const char *extension)
{
    size_t length = rb->strlen(input);
    if (length <= 4 || length >= MAX_PATH ||
        rb->strcasecmp(input + length - 4, ".wav"))
        return false;
    int size = rb->snprintf(output, MAX_PATH, "%.*s%s", (int)(length - 4),
                            input, extension);
    return size > 0 && size < MAX_PATH;
}

static bool encoder_open_output(struct encoder_file *file, const char *path)
{
    int length = rb->snprintf(file->temp, sizeof(file->temp), "%s.tmp", path);
    if (length <= 0 || (size_t)length >= sizeof(file->temp))
        return false;
    file->output = rb->open(file->temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (file->output < 0)
    {
        file->temp[0] = '\0';
        return false;
    }
    return true;
}

static void encoder_discard(void *context)
{
    struct encoder_file *file = context;
    if (file->input >= 0)
    {
        rb->close(file->input);
        file->input = -1;
    }
    if (file->output >= 0)
    {
        rb->close(file->output);
        file->output = -1;
    }
    if (file->temp[0])
    {
        rb->remove(file->temp);
        file->temp[0] = '\0';
    }
}

/* Release volume handles before the core enters USB storage or powers off. */
static bool encoder_cancel(struct encoder_file *file)
{
    int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        return true;
    return IS_SYSEVENT(action) &&
           rb->default_event_handler_ex(action, encoder_discard, file)
           == SYS_USB_CONNECTED;
}

static bool encoder_commit(struct encoder_file *file, const char *destination)
{
    bool ok = rb->file_sync(file->output) == 0;
    if (rb->close(file->output) < 0)
        ok = false;
    file->output = -1;
    if (rb->close(file->input) < 0)
        ok = false;
    file->input = -1;

    if (ok && rb->rename(file->temp, destination) == 0)
    {
        file->temp[0] = '\0';
        return true;
    }
    return false;
}

static enum plugin_status encoder_result(struct encoder_file *file,
                                         bool ok, bool cancelled,
                                         const char *success)
{
    encoder_discard(file);
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (!cancelled)
        rb->splash(2 * HZ, ok ? success : "File unchanged: operation failed");
    return (ok || cancelled) ? PLUGIN_OK : PLUGIN_ERROR;
}
#endif
