/* Bounded Keybox storage. GNU GPL version 2 or later. */
static bool read_field(char field[FIELD_LEN], const char **cursor,
                       const char *end)
{
    size_t available = end - *cursor;
    size_t limit = available < FIELD_LEN ? available : FIELD_LEN;
    size_t length = 0;
    while (length < limit && (*cursor)[length])
        ++length;
    if (length == limit)
        return false;
    rb->memcpy(field, *cursor, length + 1);
    *cursor += length + 1;
    return true;
}

static int parse_buffer(void)
{
    rb->memcpy(&salt, buffer, sizeof(salt));
    make_key();
    decrypt_buffer(buffer + BLOCK_SIZE, bytes_read - BLOCK_SIZE, key.words);

    if (rb->memcmp(buffer + BLOCK_SIZE, &pwhash, sizeof(pwhash)))
    {
        rb->splash(HZ * 2, "Wrong password");
        return WRONG_PASSWORD;
    }

    const char *cursor = buffer + HEADER_LEN;
    const char *end = buffer + bytes_read;
    struct pw_entry *last = &pw_list.first;
    int count = 0;
    while (cursor < end && *cursor && count < MAX_ENTRIES)
    {
        struct pw_entry *entry = &pw_list.entries[count];
        if (!read_field(entry->title, &cursor, end) ||
            !read_field(entry->name, &cursor, end) ||
            !read_field(entry->password, &cursor, end))
            return FILE_OPEN_ERROR;
        entry->used = true;
        last->next = entry;
        last = entry;
        ++count;
    }
    /* Legacy writers leave unspecified padding after the end marker. */
    if (cursor == end || *cursor ||
        ALIGN_UP(cursor - buffer + 1, BLOCK_SIZE) != bytes_read)
        return FILE_OPEN_ERROR;
    pw_list.num_entries = count;
    rb->gui_synclist_set_nb_items(&kb_list, count);
    return 0;
}

static bool read_keybox(void)
{
    int fd = rb->open(KEYBOX_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    off_t length = rb->ffilesize(fd);
    bool ok = length > HEADER_LEN && length <= (off_t)sizeof(buffer) &&
              length % BLOCK_SIZE == 0;
    if (ok)
        ok = rb->read(fd, buffer, length) == length;
    if (rb->close(fd) != 0)
        ok = false;
    if (ok)
        bytes_read = length;
    return ok;
}

static bool save_keybox(void)
{
    rb->memset(buffer, 0, sizeof(buffer));
    rb->memcpy(buffer + BLOCK_SIZE, &pwhash, sizeof(pwhash));
    char *position = buffer + HEADER_LEN;
    for (struct pw_entry *entry = pw_list.first.next; entry; entry = entry->next)
    {
        const char *fields[] = {entry->title, entry->name, entry->password};
        for (size_t i = 0; i < ARRAYLEN(fields); ++i)
        {
            size_t length = rb->strlen(fields[i]) + 1;
            rb->memcpy(position, fields[i], length);
            position += length;
        }
    }

    size_t size = ALIGN_UP(position - buffer + 1, BLOCK_SIZE);
    salt = rb->rand();
    make_key();
    encrypt_buffer(buffer + BLOCK_SIZE, size - BLOCK_SIZE, key.words);
    rb->memcpy(buffer, &salt, sizeof(salt));

    int fd = rb->open(KEYBOX_FILE ".tmp", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    bool ok = rb->write(fd, buffer, size) == (ssize_t)size &&
              rb->file_sync(fd) == 0;
    if (rb->close(fd) != 0)
        ok = false;
    if (ok)
        ok = rb->rename(KEYBOX_FILE ".tmp", KEYBOX_FILE) == 0;
    if (!ok)
        rb->remove(KEYBOX_FILE ".tmp");
    return ok;
}
