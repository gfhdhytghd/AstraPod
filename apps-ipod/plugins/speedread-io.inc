/* Speedread documents, bookmarks and fonts. GPL version 2 or later. */
#include <limits.h>
#include "plugins/phase3-file-io.h"

#define BOOKMARK_LINE (MAX_PATH + 40)

static bool bookmark_line(char *record, const char *path, off_t file_size,
                          off_t *offset, int *word, int *wpm)
{
    char *end;
    char *pos = rb->strtok_r(record, " ", &end);
    char *index = rb->strtok_r(NULL, " ", &end);
    char *rate = rb->strtok_r(NULL, " ", &end);
    if (!end || rb->strcmp(end, path))
        return false;

    uintmax_t parsed_offset, parsed_word, parsed_rate;
    if (!parse_unsigned(pos, file_size, &parsed_offset)
        || parsed_offset < (uintmax_t)begin_offs
        || !parse_unsigned(index, LINE_LEN - 1, &parsed_word)
        || !parse_unsigned(rate, MAX_WPM, &parsed_rate)
        || parsed_rate < MIN_WPM)
        return false;

    *offset = parsed_offset;
    *word = parsed_word;
    *wpm = parsed_rate;
    return true;
}

static bool save_bookmark(const char *fname, int wpm)
{
    if (rb->strchr(fname, '\r') || rb->strchr(fname, '\n'))
        return false;
    char temp[MAX_PATH];
    int out = begin_replace(BOOKMARK_FILE, temp);
    if (out < 0)
        return false;

    int previous = rb->open(BOOKMARK_FILE, O_RDONLY);
    bool ok = previous >= 0 || !rb->file_exists(BOOKMARK_FILE);
    if (previous >= 0)
    {
        static char line[BOOKMARK_LINE];
        static char copy[BOOKMARK_LINE];
        int len = 0;
        while (ok && (len = rb->read_line(previous, line, sizeof(line))) > 0)
        {
            if ((size_t)len >= sizeof(line) - 1)
            {
                ok = false;
                break;
            }
            rb->strlcpy(copy, line, sizeof(copy));
            char *end;
            rb->strtok_r(copy, " ", &end);
            rb->strtok_r(NULL, " ", &end);
            rb->strtok_r(NULL, " ", &end);
            if (end && !rb->strcmp(end, fname))
                continue;
            size_t size = rb->strlen(line);
            ok = rb->write(out, line, size) == (ssize_t)size
                 && rb->write(out, "\n", 1) == 1;
        }
        if (rb->close(previous) < 0 || len < 0)
            ok = false;
    }
    if (ok)
        ok = rb->fdprintf(out, "%jd %d %d %s\n",
                         (intmax_t)line_offs, word_num, wpm, fname) > 0;
    return end_replace(out, temp, BOOKMARK_FILE, ok);
}

static bool load_bookmark(const char *fname, off_t file_size, int *wpm)
{
    int bookmark = rb->open(BOOKMARK_FILE, O_RDONLY);
    if (bookmark < 0)
        return false;
    char line[BOOKMARK_LINE];
    bool found = false;
    int len;
    while ((len = rb->read_line(bookmark, line, sizeof(line))) > 0)
    {
        if ((size_t)len >= sizeof(line) - 1)
            break;
        off_t offset;
        int word, rate;
        if (!bookmark_line(line, fname, file_size, &offset, &word, &rate))
            continue;
        if (rb->lseek(fd, offset, SEEK_SET) < 0)
            break;
        line_len = -1;
        for (int i = 0; i <= word; i++)
        {
            if (!get_next_word() || line_offs != offset)
                break;
            if (i == word)
            {
                *wpm = rate;
                found = true;
            }
        }
        if (!found)
        {
            rb->lseek(fd, begin_offs, SEEK_SET);
            last_word = NULL;
            line_len = -1;
        }
        break;
    }
    rb->close(bookmark);
    return found;
}

static bool new_font(const char *path)
{
    if (custom_font != FONT_UI)
        rb->font_unload(custom_font);
    custom_font = rb->font_load(path);
    if (custom_font < 0)
    {
        custom_font = FONT_UI;
        return false;
    }
    return true;
}

static void load_font(void)
{
    int config = rb->open(CONFIG_FILE, O_RDONLY);
    if (config < 0)
        return;
    char path[MAX_PATH];
    off_t size = rb->ffilesize(config);
    if (size > 0 && size < MAX_PATH && rb->read(config, path, size) == size)
    {
        path[size] = '\0';
        if (rb->strlen(path) == (size_t)size)
            new_font(path);
    }
    rb->close(config);
}

static void font_menu(void)
{
    char path[MAX_PATH];
    if (rb->pick_file(path, sizeof(path), FONT_DIR, "Choose a font",
                      ".fnt.pbf", 0, false) <= 0)
        return;
    if (!new_font(path))
    {
        rb->splash(HZ, "Unable to load font");
        return;
    }
    char temp[MAX_PATH];
    int config = begin_replace(CONFIG_FILE, temp);
    size_t size = rb->strlen(path);
    if (config < 0 || !end_replace(config, temp, CONFIG_FILE,
                         rb->write(config, path, size) == (ssize_t)size))
        rb->splash(HZ, "Font choice not saved");
}
