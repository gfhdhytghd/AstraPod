/* Foreground lyric input and checked write-back. GPL version 2 or later. */


static struct {
    off_t size;
    uint32_t digest;
    long offset;
    int width;
    bool little_endian;
} loaded_source;

static uint32_t lrc_digest(uint32_t digest, const unsigned char *data,
                           size_t size)
{
    while (size--)
        digest = (digest ^ *data++) * 16777619u;
    return digest;
}

static bool valid_utf16(const unsigned char *data, size_t size, bool le)
{
    bool high = false;
    for (size_t i = le ? 1 : 0; i < size; i += 2)
    {
        unsigned int byte = data[i];
        bool low = byte >= 0xdc && byte < 0xe0;
        if (high != low)
            return false;
        high = byte >= 0xd8 && byte < 0xdc;
    }
    return !high;
}

static bool valid_utf8(const unsigned char *data, const unsigned char *end)
{
    while (data < end)
    {
        ucschar_t ch;
        const unsigned char *next = rb->utf8decode(data, &ch);
        if (next <= data || next > end || ch == 0)
            return false;
        data = next;
    }
    return true;
}

static bool read_lrc_lines(struct line_reader *reader, int encoding,
                           bool (*parse)(char *, off_t))
{
    unsigned char head[3];
    size_t prefix = MIN(reader->size, (off_t)sizeof(head));
    if (rb->read(reader->fd, head, prefix) != (ssize_t)prefix ||
        rb->lseek(reader->fd, 0, SEEK_SET) != 0)
        return false;

    source_width = loaded_source.width = 1;
    if (prefix == 3 && !rb->memcmp(head, "\xef\xbb\xbf", 3))
        encoding = UTF_8;
    else if (prefix >= 2 && (!rb->memcmp(head, "\xff\xfe", 2) ||
                             !rb->memcmp(head, "\xfe\xff", 2)))
    {
        source_width = loaded_source.width = 2;
        loaded_source.little_endian = head[0] == 0xff;
        if (current.type == SNC || reader->size % 2)
            return false;
        prefix = 2;
    }
    else
        prefix = 0;

    source_encoding = encoding;
    for (size_t i = 0; i < prefix; i++)
        if (line_byte(reader) != head[i])
            return false;

    int result;
    do
    {
        unsigned char decoded[MAX_LINE_LEN * 3 + 4];
        off_t offset = reader->position;
        size_t size;
        result = input_line(reader, (unsigned char *)temp_buf,
                            MAX_LINE_LEN, &size, loaded_source.width,
                            loaded_source.little_endian);
        if (result <= 0)
            break;

        unsigned char *end;
        if (loaded_source.width == 2)
        {
            if (!valid_utf16((unsigned char *)temp_buf, size,
                             loaded_source.little_endian))
                return false;
            end = loaded_source.little_endian
                ? rb->utf16LEdecode(temp_buf, decoded, size / 2)
                : rb->utf16BEdecode(temp_buf, decoded, size / 2);
        }
        else
        {
            rb->memset(temp_buf + size, 0, MAX_LINE_LEN - size);
            if (current.type == SNC &&
                (size < 12 || rb->memcmp(temp_buf, "\xa2\xe2", 2) ||
                 rb->memcmp(temp_buf + 10, "\xa2\xd0", 2)))
                end = rb->iso_decode(temp_buf, decoded, encoding, size);
            else if (current.type == SNC)
            {
                for (int i = 2; i < 10; i++)
                    if (temp_buf[i] < '0' || temp_buf[i] > '9')
                        return false;
                end = decoded;
            }
            else
                end = rb->iso_decode(temp_buf, decoded, encoding, size);
        }
        *end = '\0';
        if (end - decoded >= MAX_LINE_LEN - 1 ||
            !valid_utf8(decoded, end) || !parse((char *)decoded, offset) ||
            current.nlrcline > LRC_LINE_LIMIT)
            return false;
        int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
        if (IS_SYSEVENT(action) &&
            rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return false;
        rb->yield();
    } while (!rb->plugin_usb_taken());

    return result == 0 && reader->position == reader->size;
}

static void load_lrc_file(void)
{
    bool (*parse)(char *, off_t) = NULL;
    int encoding = prefs.encoding;
    switch (current.type)
    {
        case LRC8:
            encoding = UTF_8;
            /* fall through */
        case LRC:
            parse = parse_lrc_line;
            break;
        case SNC:
            parse = parse_snc_line;
            break;
        case TXT:
            parse = parse_txt_line;
            break;
        default:
            return;
    }

    static struct line_reader reader;
    reader = (struct line_reader){
        .fd = rb->open(current.lrc_file, O_RDONLY),
        .digest = 2166136261u,
    };
    bool ok = false;
    if (reader.fd >= 0)
    {
        reader.size = rb->ffilesize(reader.fd);
        ok = reader.size >= 0 && reader.size <= LRC_BYTE_LIMIT &&
             read_lrc_lines(&reader, encoding, parse);
        if (rb->close(reader.fd))
            ok = false;
    }
    if (ok)
    {
        loaded_source.size = reader.size;
        loaded_source.digest = reader.digest;
        loaded_source.offset = current.offset;
        calc_brpos(NULL, 0);
        init_time_tag();
    }
    else
        reset_current_data();

    current.loaded_lrc = true;
}

static struct {
    int source;
    int output;
    char temporary[MAX_PATH];
    bool owns_temporary;
} lrc_write;

static void close_lrc_write(void *unused)
{
    (void)unused;
    if (lrc_write.source >= 0)
    {
        rb->close(lrc_write.source);
        lrc_write.source = -1;
    }
    if (lrc_write.output >= 0)
    {
        rb->close(lrc_write.output);
        lrc_write.output = -1;
    }
    if (lrc_write.owns_temporary)
    {
        rb->remove(lrc_write.temporary);
        lrc_write.owns_temporary = false;
    }
}

static bool continue_lrc_write(void)
{
    int button = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (rb->default_event_handler_ex(button, close_lrc_write, NULL) ==
        SYS_USB_CONNECTED)
        return false;
    rb->yield();
    return button != ACTION_STD_CANCEL && button != ACTION_STD_MENU;
}

static bool lrc_write_bytes(const void *data, size_t count)
{
    return rb->write(lrc_write.output, data, count) == (ssize_t)count;
}

static bool write_time_tag(long time, int width)
{
    char tag[32];
    int length;
    if (current.type == SNC)
        length = rb->snprintf(tag, sizeof(tag), "%02ld%02ld%02ld%02ld",
                              time / 3600000, (time / 60000) % 60,
                              (time / 1000) % 60, (time / 10) % 100);
    else
        length = rb->snprintf(tag, sizeof(tag), "[%02ld:%02ld.%02ld]",
                              time / 60000, (time / 1000) % 60,
                              (time / 10) % 100);
    if (length <= 0 || (size_t)length >= sizeof(tag))
        return false;
    if (width == 1)
        return lrc_write_bytes(tag, length);

    unsigned char utf16[sizeof(tag) * 2] = {0};
    for (int i = 0; i < length; i++)
        utf16[2 * i + !loaded_source.little_endian] = tag[i];
    return lrc_write_bytes(utf16, 2 * length);
}

static bool copy_lrc_source(off_t count, uint32_t *digest)
{
    unsigned char block[1024];
    while (count)
    {
        size_t size = MIN(count, (off_t)sizeof(block));
        if (!continue_lrc_write() ||
            rb->read(lrc_write.source, block, size) != (ssize_t)size ||
            !lrc_write_bytes(block, size))
            return false;
        *digest = lrc_digest(*digest, block, size);
        count -= size;
    }
    return true;
}

/* Rewrite only the edited timestamp bytes. Retain BOM, metadata, encoding,
 * offset tags, timing for individual words and original line endings. */
static bool rewrite_lrc(void)
{
    off_t position = 0;
    uint32_t digest = 2166136261u;
    int width = loaded_source.width;
    while (position < loaded_source.size)
    {
        off_t next = loaded_source.size;
        const struct lrc_line *edit = NULL;
        for (const struct lrc_line *line = current.ll_head; line;
             line = line->next)
        {
            if (line->old_time_start != line->time_start &&
                line->file_offset >= position && line->file_offset < next)
            {
                next = line->file_offset;
                edit = line;
            }
        }
        if (!copy_lrc_source(next - position, &digest))
            return false;
        position = next;
        if (!edit)
            break;

        unsigned char tag[32];
        size_t size = 0;
        bool complete = false;
        while (position + width <= loaded_source.size &&
               size + width <= sizeof(tag))
        {
            if (rb->read(lrc_write.source, tag + size, width) != width)
                return false;
            unsigned char ch = tag[size + (width == 2 &&
                                           !loaded_source.little_endian)];
            size += width;
            position += width;
            complete = current.type == SNC ? size == 8 : ch == ']';
            if (complete)
                break;
        }
        long time = MAX(0, edit->time_start + current.offset -
                           loaded_source.offset);
        long limit = current.type == SNC ? 360000000L : 6000000L;
        if (!complete || time >= limit || !write_time_tag(time, width))
            return false;
        digest = lrc_digest(digest, tag, size);
    }
    return digest == loaded_source.digest;
}

static bool export_text_lrc(void)
{
    static const char bom[] = "\xef\xbb\xbf";
    if (!lrc_write_bytes(bom, sizeof(bom) - 1))
        return false;
    for (struct lrc_line *line = current.ll_head; line; line = line->next)
    {
        const char *text = get_lrc_str(line);
        long time = get_time_start(line);
        if (!continue_lrc_write() || time >= 6000000L ||
            !write_time_tag(time, 1) ||
            !lrc_write_bytes(text, rb->strlen(text)) ||
            !lrc_write_bytes("\n", 1))
            return false;
    }
    return true;
}

static void save_changes(void)
{
    if (!current.changed_lrc || !current.ll_head)
        return;

    char destination[MAX_PATH];
    bool create = current.type == TXT;
    rb->strlcpy(destination, current.lrc_file, sizeof(destination));
    if (create)
    {
        char *extension = rb->strrchr(destination, '.');
        if (!extension || rb->strcasecmp(extension, ".txt"))
            return;
        rb->strcpy(extension, ".lrc");
        if (rb->file_exists(destination))
        {
            rb->splash(HZ * 2, "LRC file already exists");
            return;
        }
    }

    lrc_write.source = lrc_write.output = -1;
    int length = rb->snprintf(lrc_write.temporary, sizeof(lrc_write.temporary),
                              "%s.tmp", destination);
    bool ok = length > 0 && (size_t)length < sizeof(lrc_write.temporary);
    if (ok && !create)
    {
        lrc_write.source = rb->open(destination, O_RDONLY);
        ok = lrc_write.source >= 0 &&
             rb->ffilesize(lrc_write.source) == loaded_source.size;
    }
    if (ok)
    {
        lrc_write.output = rb->open(lrc_write.temporary,
                                    O_WRONLY | O_CREAT | O_EXCL, 0666);
        lrc_write.owns_temporary = lrc_write.output >= 0;
        ok = lrc_write.owns_temporary &&
             (create ? export_text_lrc() : rewrite_lrc()) &&
             rb->file_sync(lrc_write.output) == 0;
    }
    if (lrc_write.output >= 0)
    {
        if (rb->close(lrc_write.output))
            ok = false;
        lrc_write.output = -1;
    }
    if (lrc_write.source >= 0)
    {
        if (rb->close(lrc_write.source))
            ok = false;
        lrc_write.source = -1;
    }
    if (ok && continue_lrc_write() &&
        (!create || !rb->file_exists(destination)) &&
        rb->rename(lrc_write.temporary, destination) == 0)
    {
        lrc_write.owns_temporary = false;
        rb->strlcpy(current.lrc_file, destination, sizeof(current.lrc_file));
        if (create)
            current.type = LRC;
        reset_current_data();
    }
    else if (!rb->plugin_usb_taken())
        rb->splash(HZ * 2, "Could not save changes");
    close_lrc_write(NULL);
}

static bool lrc_integer(const char *text, int min, int max, int *value)
{
    bool negative = *text == '-';
    if (negative || *text == '+')
        text++;
    if (!isdigit((unsigned char)*text))
        return false;

    long number = 0;
    for (; isdigit((unsigned char)*text); text++)
    {
        if (number > 100000000L)
            return false;
        number = number * 10 + *text - '0';
    }
    if (negative)
        number = -number;
    if (*text || number < min || number > max)
        return false;
    *value = number;
    return true;
}

static bool lrc_config_path(char *path, const char *name)
{
    const char *module = rb->plugin_get_current_filename();
    const char *slash = module ? rb->strrchr(module, '/') : NULL;
    if (!slash)
        return false;
    int length = rb->snprintf(path, MAX_PATH, "%.*s/%s",
                              (int)(slash - module), module, name);
    return length > 0 && length < MAX_PATH;
}

static void lrc_config_load(const char *name, struct configdata *rows,
                            size_t count)
{
    char path[MAX_PATH];
    if (!lrc_config_path(path, name))
        return;
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return;

    char line[MAX_PATH];
    int size;
    while ((size = rb->read_line(fd, line, sizeof(line))) > 0 &&
           size < (int)sizeof(line) - 1)
    {
        char *key, *value;
        if (!rb->settings_parseline(line, &key, &value))
            continue;
        for (size_t i = 0; i < count; i++)
        {
            struct configdata *row = &rows[i];
            if (rb->strcmp(key, row->name))
                continue;
            int number;
            if (row->type == TYPE_STRING)
                rb->strlcpy(row->string, value, row->max);
            else if (lrc_integer(value, row->min, row->max, &number))
            {
                if (row->type == TYPE_BOOL)
                    *row->bool_p = number != 0;
                else
                    *row->int_p = number;
            }
            break;
        }
    }
    rb->close(fd);
}

static bool lrc_config_save(const char *name, const struct configdata *rows,
                            size_t count)
{
    char path[MAX_PATH], temporary[MAX_PATH];
    if (!lrc_config_path(path, name))
        return false;
    int length = rb->snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length <= 0 || (size_t)length >= sizeof(temporary))
        return false;

    int fd = rb->open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return false;

    bool ok = rb->fdprintf(fd, "file version: 0\n") >= 0;
    for (size_t i = 0; i < count && ok; i++)
    {
        const struct configdata *row = &rows[i];
        if (row->type == TYPE_STRING)
            ok = rb->fdprintf(fd, "%s: %s\n", row->name, row->string) >= 0;
        else
        {
            int value = row->type == TYPE_BOOL ? *row->bool_p : *row->int_p;
            ok = rb->fdprintf(fd, "%s: %d\n", row->name, value) >= 0;
        }
    }
    if (ok && rb->file_sync(fd))
        ok = false;
    if (rb->close(fd))
        ok = false;
    if (ok && rb->rename(temporary, path) == 0)
        return true;
    rb->remove(temporary);
    return false;
}
