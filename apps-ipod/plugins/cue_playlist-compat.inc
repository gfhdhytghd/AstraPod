/* Foreground CUE export. GPL version 2 or later. */
static struct {
    char record[4096];
    size_t used;
    bool overflow;
    int output;
    struct line_reader input;
    char destination[MAX_PATH];
    char temporary[MAX_PATH];
    bool owns_temporary;
    int tracks;
} cue;

static int cue_char(void *unused, int letter)
{
    (void)unused;
    if (cue.used == sizeof(cue.record))
    {
        cue.overflow = true;
        return -1;
    }
    cue.record[cue.used++] = letter;
    return 1;
}

static void cps_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rb->vuprintf(cue_char, NULL, fmt, ap);
    va_end(ap);
}

static void cue_quoted(const char *s)
{
    cue_char(NULL, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        int c = *p;
        if (c < 32 || c == 127)
            c = ' ';
        else if (c == '"')
            c = '\'';
        if (cue_char(NULL, c) < 0)
            break;
    }
    cue_char(NULL, '"');
}

static bool cue_flush(void)
{
    bool ok = !cue.overflow &&
              rb->write(cue.output, cue.record, cue.used) == (ssize_t)cue.used;
    cue.used = 0;
    return ok;
}

@UPSTREAM_METADATA@

static bool cue_append(const char *path, int attr, int index)
{
    /* FAT paths cannot contain these characters. A simulator can present
     * such a path, but rewriting it would break the FILE reference. */
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        if (*p < 32 || *p == 127 || *p == '"')
            return false;

    static struct mp3entry id3;
    if (!rb->get_metadata(&id3, -1, path))
        return false;

    int track = cue.tracks + 1;
    cps_printf("REM RB_ENTRY_DATA \"DISPLAY_INDEX %012u PLAYLIST_INDEX %012u ",
               (unsigned)track, (unsigned)index);
    size_t fields = cue.used;
    cps_printf("SIZE %012u TAGS %012u\"\n", 0u, 0u);
    cps_printf("FILE ");
    cue_quoted(path);
    const char *ext = rb->strrchr(path, '.');
    const char *type = ext && !rb->strcasecmp(ext, ".mp3") ? "MP3" : "WAVE";
    cps_printf(" %s\n TRACK %02d AUDIO\n", type, track);
    if (attr & PLAYLIST_ATTR_SKIPPED)
        cps_printf(TDINDENT "REM SKIPPED\n");
    if (attr & PLAYLIST_ATTR_QUEUED)
        cps_printf(TDINDENT "REM QUEUED\n");
    uint32_t tags = write_metadata_tags(&id3);
    size_t length = cue.used;
    if (cue.overflow)
        return false;
    cue.used = fields;
    cps_printf("SIZE %012u TAGS %012u", (unsigned)length, (unsigned)tags);
    cue.used = length;
    if (!cue_flush())
        return false;

    cue.tracks++;
    return true;
}

static void cue_discard(void *unused)
{
    (void)unused;
    if (cue.input.fd >= 0)
    {
        rb->close(cue.input.fd);
        cue.input.fd = -1;
    }
    if (cue.output >= 0)
    {
        rb->close(cue.output);
        cue.output = -1;
    }
    if (cue.owns_temporary)
    {
        rb->remove(cue.temporary);
        cue.owns_temporary = false;
    }
}

enum cue_result {
    CUE_CONTINUE, CUE_SUCCESS, CUE_CANCELLED, CUE_USB, CUE_FAILED,
};

static enum cue_result cue_poll(void)
{
    int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (rb->default_event_handler_ex(action, cue_discard, NULL) ==
        SYS_USB_CONNECTED)
        return CUE_USB;
    if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
        return CUE_CANCELLED;
    rb->yield();
    return CUE_CONTINUE;
}

static bool cue_names(const char *playlist)
{
    const char *stem = "/current";
    int length = rb->strlen(stem);
    if (playlist)
    {
        const char *ext = rb->strrchr(playlist, '.');
        if (!ext || *playlist != '/' ||
            (rb->strcasecmp(ext, ".m3u") && rb->strcasecmp(ext, ".m3u8")))
            return false;
        stem = playlist;
        length = ext - playlist;
    }

    for (unsigned int n = 1; n <= 9999; n++)
    {
        int result = n == 1
            ? rb->snprintf(cue.destination, sizeof(cue.destination),
                           "%.*s.cue", length, stem)
            : rb->snprintf(cue.destination, sizeof(cue.destination),
                           "%.*s-%u.cue", length, stem, n);
        if (result <= 0 || (size_t)result >= sizeof(cue.destination))
            return false;
        result = rb->snprintf(cue.temporary, sizeof(cue.temporary),
                              "%s.tmp", cue.destination);
        if (result <= 0 || (size_t)result >= sizeof(cue.temporary))
            return false;

        if (rb->file_exists(cue.destination) || rb->file_exists(cue.temporary))
            continue;
        cue.output = rb->open(cue.temporary, O_WRONLY | O_CREAT | O_EXCL, 0666);
        cue.owns_temporary = cue.output >= 0;
        return cue.owns_temporary;
    }
    return false;
}

static enum cue_result cue_scan_file(const char *playlist)
{
    cue.input.fd = rb->open(playlist, O_RDONLY);
    if (cue.input.fd < 0)
        return CUE_FAILED;
    cue.input.size = rb->ffilesize(cue.input.fd);
    if (cue.input.size < 0)
        return CUE_FAILED;

    char line[MAX_PATH];
    char path[MAX_PATH];
    bool first = true;
    for (;;)
    {
        enum cue_result result = cue_poll();
        if (result != CUE_CONTINUE)
            return result;

        size_t count;
        int next = input_line(&cue.input, (unsigned char *)line, sizeof(line),
                              &count, 1, false);
        if (next <= 0)
            return next == 0 && rb->ffilesize(cue.input.fd) == cue.input.size
                   ? CUE_SUCCESS : CUE_FAILED;
        line[count] = '\0';
        if (first && count >= 3 && !rb->memcmp(line, "\xef\xbb\xbf", 3))
            rb->memmove(line, line + 3, count - 2);
        first = false;
        if (!line[0] || line[0] == '#')
            continue;
        if (rb->playlist_line_path(playlist, line, path, sizeof(path)) <= 0 ||
            !cue_append(path, 0, cue.tracks))
            return CUE_FAILED;
        rb->splash_progress(cue.input.position / 1024,
                            cue.input.size / 1024 + 1,
                            "Exporting CUE\n%d tracks", cue.tracks);
    }
}

static enum cue_result cue_scan_current(void)
{
    int total = rb->playlist_amount();
    int index = rb->playlist_get_first_index(NULL);
    for (int i = 0; i < total; i++)
    {
        enum cue_result result = cue_poll();
        if (result != CUE_CONTINUE)
            return result;
        struct playlist_track_info info;
        if (rb->playlist_amount() != total ||
            rb->playlist_get_track_info(NULL, index, &info) < 0 ||
            info.display_index != i + 1 || !cue_append(info.filename, info.attr,
                                                       info.index))
            return CUE_FAILED;
        index = (index + 1) % total;
        rb->splash_progress(i + 1, total, "Exporting CUE");
    }
    return cue.tracks == total ? CUE_SUCCESS : CUE_FAILED;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *playlist = parameter;
    cue.input.fd = cue.output = -1;
    enum cue_result result = CUE_FAILED;

    if (!cue_names(playlist))
        goto finish;
    cps_printf("\xef\xbb\xbf" "REM COMMENT ");
    cue_quoted(rb->rbversion);
    cps_printf("\nTITLE ");
    cue_quoted(playlist ? playlist : "Current playlist");
    cps_printf("\n");
    if (!cue_flush())
        goto finish;

    rb->splash_progress_set_delay(0);
    result = playlist ? cue_scan_file(playlist) : cue_scan_current();
    if (result != CUE_SUCCESS)
        goto finish;
    if (!cue.tracks)
    {
        cue_discard(NULL);
        rb->splash(HZ * 2, "No playlist tracks");
        return PLUGIN_OK;
    }

    int synced = rb->file_sync(cue.output);
    int closed = rb->close(cue.output);
    cue.output = -1;
    result = cue_poll();
    if (result != CUE_CONTINUE)
        goto finish;
    if (synced || closed || rb->file_exists(cue.destination) ||
        rb->rename(cue.temporary, cue.destination))
    {
        result = CUE_FAILED;
        goto finish;
    }
    cue.owns_temporary = false;
    result = CUE_SUCCESS;

finish:
    cue_discard(NULL);
    if (result == CUE_SUCCESS)
        rb->splashf(HZ * 2, "Saved %d tracks\n%s", cue.tracks, cue.destination);
    else if (result == CUE_FAILED)
        rb->splash(HZ * 2, "CUE export failed");
    return result == CUE_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}
