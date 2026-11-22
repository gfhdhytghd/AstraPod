/* AstraPod shopping list writer. GNU GPL version 2 or later. */
static bool save_changes(void)
{
    char temp[MAX_PATH];
    int length = rb->snprintf(temp, sizeof(temp), "%s.tmp", filename);
    int fd = length > 0 && (size_t)length < sizeof(temp)
           ? rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666) : -1;
    bool ok = fd >= 0;
    if (ok)
    {
        static const char marker[] = { ' ', '!', '#' };
        char line[DESC_SIZE + 2];
        for (int i = 0; ok && i < total_item_count; i++)
        {
            const struct items_list_s *item = &items_list[i];
            length = rb->snprintf(line, sizeof(line), "%c%s\n",
                                  marker[item->flag], item->desc);
            ok = rb->write(fd, line, length) == length;
        }
        if (ok)
        {
            length = rb->snprintf(line, sizeof(line), "$%d%d\n", view, show_categories);
            ok = rb->write(fd, line, length) == length && rb->file_sync(fd) == 0;
        }
        if (rb->close(fd) < 0)
            ok = false;
        if (ok)
            ok = rb->rename(temp, filename) == 0;
        if (!ok)
            rb->remove(temp);
    }
    if (!ok)
        rb->splash(HZ * 2, "Changes not saved");
    return ok;
}
