/* Volume-relative Windows shortcuts. GNU GPL version 2 or later. */

static uint32_t read_le(const unsigned char *bytes, unsigned count)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint32_t)bytes[i] << (8 * i);
    return value;
}

static bool skip_bytes(int fd, uint32_t count, off_t size)
{
    off_t offset = rb->lseek(fd, 0, SEEK_CUR);
    return offset >= 0 && offset <= size && count <= (uint64_t)(size - offset)
           && rb->lseek(fd, offset + count, SEEK_SET) >= 0;
}

static bool extract_link_destination(int fd, char *path, size_t capacity,
                                     bool *directory)
{
    /* Header size and Shell Link class GUID in wire byte order. */
    static const unsigned char prefix[] = {
        0x4c, 0, 0, 0, 1, 0x14, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46
    };
    unsigned char header[76], count[4], raw[MAX_PATH * 2];
    if (rb->read(fd, header, sizeof(header)) != sizeof(header) ||
        rb->memcmp(header, prefix, sizeof(prefix)))
        return false;

    uint32_t flags = read_le(header + 20, 4);
    off_t size = rb->ffilesize(fd);
    if (!(flags & HAS_RELATIVE_PATH) || size < (off_t)sizeof(header))
        return false;

    if ((flags & HAS_LINK_TARGET_ID_LIST) &&
        (rb->read(fd, count, 2) != 2 ||
         !skip_bytes(fd, read_le(count, 2), size)))
        return false;
    if (flags & HAS_LINK_INFO)
    {
        if (rb->read(fd, count, 4) != 4)
            return false;
        uint32_t info_size = read_le(count, 4);
        if (info_size < 0x1c || !skip_bytes(fd, info_size - 4, size))
            return false;
    }

    unsigned width = (flags & IS_UNICODE) ? 2 : 1;
    if ((flags & HAS_NAME) &&
        (rb->read(fd, count, 2) != 2 ||
         !skip_bytes(fd, width * read_le(count, 2), size)))
        return false;
    if (rb->read(fd, count, 2) != 2)
        return false;

    unsigned units = read_le(count, 2);
    unsigned bytes = units * width;
    if (!units || units >= capacity || bytes > sizeof(raw) ||
        rb->read(fd, raw, bytes) != (ssize_t)bytes)
        return false;

    size_t used = 0;
    for (unsigned i = 0; i < bytes; i += width)
    {
        uint32_t code = read_le(raw + i, width);
        unsigned char encoded[4];
        size_t length = 1;
        if (code < 0x20 || code == 0x7f)
            return false;
        if (width == 1)
        {
            /* Shell Links do not record the legacy Windows code page. */
            if (code > 0x7e)
                return false;
            encoded[0] = code;
        }
        else
        {
            unsigned words = 1;
            if (code >= 0xd800 && code <= 0xdfff)
            {
                if (code >= 0xdc00 || i + 2 >= bytes)
                    return false;
                uint32_t second = read_le(raw + i + 2, 2);
                if (second < 0xdc00 || second > 0xdfff)
                    return false;
                words = 2;
            }
            length = rb->utf16LEdecode(raw + i, encoded, words) - encoded;
            i += (words - 1) * 2;
        }
        if (length >= capacity - used)
            return false;
        rb->memcpy(path + used, encoded, length);
        used += length;
    }
    path[used] = '\0';
    *directory = (read_le(header + 24, 4) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

static bool resolve_link(const char *source, char *relative,
                         char *resolved, size_t capacity)
{
    for (char *p = relative; *p; p++)
    {
        if (*p == '\\')
            *p = '/';
        if (*p == ':' || *p == '<' || *p == '>')
            return false;
    }
    if (*relative == '/' || source[0] != '/')
        return false;

    size_t root = 1;
    if (source[1] == '<')
    {
        static const char volume[] = "/<HDD0>/";
        if (rb->strncmp(source, volume, sizeof(volume) - 1))
            return false;
        root = sizeof(volume) - 1;
    }
    const char *slash = rb->strrchr(source, '/');
    size_t length = slash - source + 1;
    if (length < root || length >= capacity)
        return false;
    rb->memcpy(resolved, source, length);

    for (const char *part = relative; *part; )
    {
        const char *end = rb->strchr(part, '/');
        size_t count = end ? (size_t)(end - part) : rb->strlen(part);
        if (count == 2 && part[0] == '.' && part[1] == '.')
        {
            if (length <= root)
                return false;
            do
                length--;
            while (length > root && resolved[length - 1] != '/');
        }
        else if (count && !(count == 1 && part[0] == '.'))
        {
            if (count >= capacity - length)
                return false;
            rb->memcpy(resolved + length, part, count);
            length += count;
            resolved[length++] = '/';
        }

        part += count + (end != NULL);
    }

    resolved[length > root ? length - 1 : root] = '\0';
    return true;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *filename = parameter;
    char relative[MAX_PATH], target[MAX_PATH];
    bool directory = false;
    if (!filename)
    {
        rb->splash(HZ * 2, "Open a .lnk file from Files");
        return PLUGIN_OK;
    }

    int fd = rb->open(filename, O_RDONLY);
    if (fd < 0)
    {
        rb->splash(HZ * 2, "Cannot open shortcut");
        return PLUGIN_OK;
    }
    bool valid = extract_link_destination(fd, relative, sizeof(relative),
                                          &directory);
    if (rb->close(fd) < 0 || !valid ||
        !resolve_link(filename, relative, target, sizeof(target)))
    {
        rb->splash(HZ * 2, "Shortcut needs a path inside this volume");
        return PLUGIN_OK;
    }
    if (!rb->reveal_file(target, directory))
    {
        rb->splashf(HZ * 2, "Target unavailable: %s", target);
        return PLUGIN_OK;
    }
    return PLUGIN_GOTO_FILEBROWSER;
}
