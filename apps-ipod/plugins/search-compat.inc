/* Native playlist search for upstream search. GPL version 2 or later. */

#define BLOCK_SIZE 4096
#define QUERY_SIZE 60

enum search_state {
    SEARCH_RUNNING, SEARCH_DONE, SEARCH_CANCELLED, SEARCH_USB, SEARCH_FAILED,
};

static struct {
    int input;
    int output;
    unsigned int matches;
    char result[MAX_PATH];
    char temp[MAX_PATH];
    bool owns_temp;
} search;

static char query[QUERY_SIZE];
static unsigned char block[BLOCK_SIZE];
/* Space for a path and one LF when it is written. */
static char line[MAX_PATH + 1];

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static bool matches(const char *path, size_t length)
{
    size_t term = rb->strlen(query);
    for (size_t i = 0; i + term <= length; i++)
    {
        size_t j = 0;
        while (j < term && ascii_lower(path[i + j]) == ascii_lower(query[j]))
            j++;
        if (j == term)
            return true;
    }
    return false;
}

static void discard_search(void *unused)
{
    (void)unused;
    if (search.input >= 0)
    {
        rb->close(search.input);
        search.input = -1;
    }
    if (search.output >= 0)
    {
        rb->close(search.output);
        search.output = -1;
    }
    if (search.owns_temp)
    {
        rb->remove(search.temp);
        search.owns_temp = false;
    }
}

static enum search_state poll_search(void)
{
    int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
        return SEARCH_CANCELLED;
    if (rb->default_event_handler_ex(action, discard_search, NULL) == SYS_USB_CONNECTED)
        return SEARCH_USB;
    rb->yield();
    return SEARCH_RUNNING;
}

static bool open_result(const char *filename, const char *ext)
{
    /* Sibling files use the same base for relative playlist entries. */
    int directory = rb->strrchr(filename, '/') - filename + 1;
    for (unsigned int index = 1; index <= 9999; index++)
    {
        int n = rb->snprintf(search.result, sizeof(search.result),
                             "%.*ssearch_result-%u%s",
                             directory, filename, index, ext);
        if (n < 0 || (size_t)n >= sizeof(search.result))
            return false;
        n = rb->snprintf(search.temp, sizeof(search.temp),
                         "%.*s.search_result-%u.tmp",
                         directory, filename, index);
        if (n < 0 || (size_t)n >= sizeof(search.temp))
            return false;
        if (rb->file_exists(search.result) || rb->file_exists(search.temp))
            continue;

        search.output = rb->open(search.temp, O_WRONLY | O_CREAT | O_EXCL);
        search.owns_temp = search.output >= 0;
        return search.owns_temp;
    }
    return false;
}

static bool write_match(size_t length)
{
    if (!length || !matches(line, length))
        return true;

    line[length++] = '\n';
    if (rb->write(search.output, line, length) != (ssize_t)length)
        return false;
    search.matches++;
    return true;
}

static enum search_state scan_playlist(off_t size)
{
    size_t used = 0;
    bool comment = false;
    off_t bytes = rb->lseek(search.input, 0, SEEK_CUR);
    if (bytes < 0 || bytes > size)
        return SEARCH_FAILED;

    for (;;)
    {
        enum search_state result = poll_search();
        if (result != SEARCH_RUNNING)
            return result;

        unsigned int percent = size ? (uint64_t)bytes * 100 / size : 100;
        rb->splash_progress(percent, 100, "Searching playlist - Menu to cancel\n"
                            "%u matches", search.matches);
        ssize_t n = rb->read(search.input, block, sizeof(block));
        if (n < 0 || n > size - bytes)
            return SEARCH_FAILED;
        if (!n)
            break;

        bytes += n;
        for (ssize_t i = 0; i < n; i++)
        {
            unsigned char c = block[i];
            if (c == '\r' || c == '\n')
            {
                if (!comment && !write_match(used))
                    return SEARCH_FAILED;
                used = 0;
                comment = false;
            }
            else if (!comment)
            {
                if (used == 0 && c == '#')
                    comment = true;
                else if (c == '\0' || used == MAX_PATH - 1)
                    return SEARCH_FAILED;
                else
                    line[used++] = c;
            }
        }
    }

    return bytes == size && (comment || write_match(used))
           ? SEARCH_DONE : SEARCH_FAILED;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *filename = parameter;
    if (!filename)
    {
        rb->splash(3 * HZ, "In Files, hold Select on an M3U or M3U8 "
                           "and choose Search in Playlist.");
        return PLUGIN_OK;
    }

    const char *ext = rb->strrchr(filename, '.');
    if (filename[0] != '/' || !ext ||
        (rb->strcasecmp(ext, ".m3u") && rb->strcasecmp(ext, ".m3u8")))
        return PLUGIN_ERROR;

    query[0] = '\0';
    if (rb->kbd_input(query, sizeof(query), NULL) || !query[0])
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;

    rb->memset(&search, 0, sizeof(search));
    search.output = -1;
    search.input = rb->open(filename, O_RDONLY);
    enum search_state result = SEARCH_FAILED;
    if (search.input < 0)
        goto finish;

    off_t size = rb->ffilesize(search.input);
    static const unsigned char bom[] = {0xef, 0xbb, 0xbf};
    unsigned char prefix[sizeof(bom)];
    size_t head = size >= (off_t)sizeof(bom) ? sizeof(bom) : 0;
    if (size < 0 || (head && rb->read(search.input, prefix, head) != (ssize_t)head))
        goto finish;
    bool has_bom = head && !rb->memcmp(prefix, bom, sizeof(bom));
    if (!has_bom && rb->lseek(search.input, 0, SEEK_SET) != 0)
        goto finish;

    if (!open_result(filename, ext))
        goto finish;
    if (has_bom && rb->write(search.output, bom, sizeof(bom)) != sizeof(bom))
        goto finish;

    rb->splash_progress_set_delay(0);
    result = scan_playlist(size);
    if (result != SEARCH_DONE)
        goto finish;

    if (search.matches)
    {
        int synced = rb->file_sync(search.output);
        int closed = rb->close(search.output);
        search.output = -1;
        if (synced || closed)
        {
            result = SEARCH_FAILED;
            goto finish;
        }
        enum search_state pending = poll_search();
        if (pending != SEARCH_RUNNING)
        {
            result = pending;
            goto finish;
        }
        /* The foreground owns storage until it processes a USB event.
         * Publish only to a new name, including after a successful retry. */
        if (rb->file_exists(search.result) || rb->rename(search.temp, search.result))
        {
            result = SEARCH_FAILED;
            goto finish;
        }
        search.owns_temp = false;
    }

finish:
    discard_search(NULL);
    if (result == SEARCH_FAILED)
        rb->splash(3 * HZ, "Search could not read or save the playlist");
    else if (result == SEARCH_DONE)
    {
        if (search.matches)
            rb->splashf(3 * HZ, "%u matches saved to\n%s", search.matches,
                        rb->strrchr(search.result, '/') + 1);
        else
            rb->splash(2 * HZ, "No matching tracks");
    }
    return result == SEARCH_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}
