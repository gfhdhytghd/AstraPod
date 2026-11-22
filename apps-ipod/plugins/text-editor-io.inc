/* Text editor input and checked replacement. GPL version 2 or later. */
#include "plugins/phase3-file-io.h"
#include "widgets/edit_line.h"

static off_t bom;

static int edit_text(char *line, int size)
{
    if (rb->utf8length((const unsigned char *)line) >= EDIT_LINE_MAX)
    {
        rb->splash(HZ, "Line exceeds keyboard capacity");
        return -1;
    }
    return rb->kbd_input_raw(line, size);
}

static bool load_document(const char *path)
{
    int fd = open_text(path, &bom);
    if (fd < 0)
        return false;
    off_t length = rb->ffilesize(fd);
    bool ok = length >= bom && (uintmax_t)(length - bom) < buffer_size;
    size_t bytes = ok ? (size_t)(length - bom) : 0;
    if (ok)
        ok = rb->read(fd, buffer, bytes) == (ssize_t)bytes;
    if (rb->close(fd) < 0)
        ok = false;
    if (!ok)
        return false;

    eol[0] = '\0';
    line_count = bytes != 0;
    size_t start = 0, used = 0;
    for (size_t i = 0; i < bytes; i++)
    {
        char ch = buffer[i];
        if (!ch)
            return false;
        if (ch == '\r' || ch == '\n')
        {
            bool crlf = ch == '\r' && i + 1 < bytes && buffer[i + 1] == '\n';
            if (!eol[0])
                rb->strcpy(eol, crlf ? "\r\n" : (ch == '\r' ? "\r" : "\n"));
            buffer[used++] = '\0';
            start = used;
            line_count++;
            i += crlf;
        }
        else
        {
            if (used - start == MAX_LINE_LEN - 1)
                return false;
            buffer[used++] = ch;
        }
    }
    if (!eol[0])
        rb->strcpy(eol, "\n");
    buffer[used] = '\0';
    char_count = bytes ? used + 1 : 0;
    return true;
}

static bool save_changes(int overwrite)
{
    char target[MAX_PATH];
    rb->strlcpy(target, filename, sizeof(target));
    if (newfile || !overwrite)
    {
        if (rb->kbd_input(target, sizeof(target), NULL) < 0)
            return false;
        size_t length = rb->strlen(target);
        if (length < 2 || target[0] != '/' || target[length - 1] == '/')
        {
            rb->splash(HZ * 2, "Enter an absolute file path");
            return false;
        }
        if (rb->file_exists(target))
        {
            const char *lines[] = { "Replace the existing file?", target };
            const struct text_message message = { lines, ARRAYLEN(lines) };
            if (rb->gui_syncyesno_run(&message, NULL, NULL) != YESNO_YES)
                return false;
        }
    }

    char temp[MAX_PATH];
    int fd = begin_replace(target, temp);
    bool ok = false;
    if (fd >= 0)
    {
        ok = !bom || rb->write(fd, utf8_bom, sizeof(utf8_bom)) == bom;
        size_t eol_size = rb->strlen(eol);
        for (int i = 0; i < line_count && ok; i++)
        {
            const char *line = do_action(ACTION_GET, NULL, i);
            size_t length = rb->strlen(line);
            ok = !rb->action_userabort(TIMEOUT_NOBLOCK)
                 && rb->write(fd, line, length) == (ssize_t)length;
            if (ok && i + 1 < line_count)
                ok = rb->write(fd, eol, eol_size) == (ssize_t)eol_size;
        }
        ok = end_replace(fd, temp, target, ok);
    }
    if (ok)
    {
        rb->strlcpy(filename, target, sizeof(filename));
        newfile = false;
    }
    else if (!rb->plugin_usb_taken())
        rb->splash(HZ * 2, "Changes not saved");
    return ok;
}
