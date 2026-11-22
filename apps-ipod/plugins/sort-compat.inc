/* Bounded line storage and checked file replacement. GPL version 2 or later. */
#include "plugins/phase3-file-io.h"

static bool read_buffer(const char *path, char *buffer, size_t size,
                        const char **eol, bool *final_eol, off_t *bom)
{
    int fd = open_text(path, bom);
    if (fd < 0)
        return false;

    off_t length = rb->ffilesize(fd);
    bool ok = length >= *bom && (uintmax_t)(length - *bom) < size;
    size_t bytes = ok ? (size_t)(length - *bom) : 0;
    if (ok)
        ok = rb->read(fd, buffer, bytes) == (ssize_t)bytes;
    if (rb->close(fd) < 0)
        ok = false;
    if (!ok)
        return false;

    char *end = buffer + bytes;
    *end = '\0';
    uintptr_t top = (uintptr_t)(buffer + size);
    pointers = (char **)(top & ~((uintptr_t)_Alignof(char *) - 1));
    *eol = NULL;
    *final_eol = false;

    for (char *cursor = buffer; cursor < end; )
    {
        if ((char *)pointers - end <= (ptrdiff_t)sizeof(*pointers))
            return false;
        *--pointers = cursor;
        num_entries++;

        while (cursor < end && *cursor != '\r' && *cursor != '\n')
            if (!*cursor++)
                return false;

        *final_eol = cursor < end;
        if (*final_eol)
        {
            bool cr = *cursor == '\r';
            *cursor++ = '\0';
            bool crlf = cr && cursor < end && *cursor == '\n';
            if (crlf)
                cursor++;
            if (!*eol)
                *eol = crlf ? "\r\n" : (cr ? "\r" : "\n");
        }
    }
    if (!*eol)
        *eol = "\n";
    return true;
}

static bool write_file(const char *path, const char *eol, bool final_eol,
                       off_t bom)
{
    char temp[MAX_PATH];
    int fd = begin_replace(path, temp);
    if (fd < 0)
        return false;

    bool ok = !bom || rb->write(fd, utf8_bom, sizeof(utf8_bom)) == bom;
    size_t eol_size = rb->strlen(eol);
    for (int line = num_entries; line > 0 && ok; )
    {
        const char *value = pointers[--line];
        size_t bytes = rb->strlen(value);
        ok = !rb->action_userabort(TIMEOUT_NOBLOCK)
             && rb->write(fd, value, bytes) == (ssize_t)bytes;
        if (ok && (line || final_eol))
            ok = rb->write(fd, eol, eol_size) == (ssize_t)eol_size;
    }
    return end_replace(fd, temp, path, ok);
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    char selected[MAX_PATH];
    if (!path)
    {
        if (rb->pick_file(selected, sizeof(selected), "/", "Select text to sort",
                          ".txt.md", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = selected;
    }

    const char *lines[] = { "Sort the lines in this file?", path };
    const struct text_message message = { lines, ARRAYLEN(lines) };
    enum yesno_res answer = rb->gui_syncyesno_run(&message, NULL, NULL);
    if (answer != YESNO_YES)
        return answer == YESNO_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;

    size_t size;
    char *buffer = rb->plugin_get_buffer(&size);
    const char *eol;
    bool final_eol;
    off_t bom;
    num_entries = 0;
    if (!buffer || !size || !read_buffer(path, buffer, size, &eol, &final_eol,
                                         &bom))
    {
        rb->splash(HZ * 2, "Unreadable text or file exceeds plugin memory");
        return PLUGIN_ERROR;
    }

    if (num_entries < 2 || rb->action_userabort(TIMEOUT_NOBLOCK))
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
    rb->splash(0, "Sorting...");
    sort_buffer();

    if (rb->action_userabort(TIMEOUT_NOBLOCK))
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
    if (!write_file(path, eol, final_eol, bom))
    {
        if (rb->plugin_usb_taken())
            return PLUGIN_USB_CONNECTED;
        rb->splash(HZ * 2, "Sort not saved");
        return PLUGIN_ERROR;
    }

    rb->splash(HZ, "Sorted");
    return PLUGIN_OK;
}
