/* Checked image load and save for Rockpaint. GNU GPL version 2 or later. */
static int load_bitmap(const char *file)
{
    struct bitmap bm = {
        .width = COLS,
        .height = ROWS,
        .data = (unsigned char *)buffer->clipboard,
    };
    int result = rb->read_plugin_bmp(file, &bm, sizeof(buffer->clipboard),
                                     false);
    if (result <= 0)
        return -1;

    img_width = bm.width;
    img_height = bm.height;
    for (int i = 0; i < ROWS * COLS; i++)
        save_buffer[i] = rp_colors[bgdrawcolor];
    for (int i = 0; i < bm.height; i++)
        rb->memcpy(save_buffer + i * COLS,
                   buffer->clipboard + i * bm.width,
                   sizeof(fb_data) * bm.width);
    return result;
}

static int save_bitmap(char *file)
{
    struct bitmap bm = {
        .data = (unsigned char *)buffer->clipboard,
        .height = img_height,
        .width = img_width,
        .format = FORMAT_NATIVE,
    };
    char temp[MAX_PATH];
    int len = rb->snprintf(temp, sizeof(temp), "%s.tmp", file);
    if (file[0] != '/' || len <= 0 || len >= (int)sizeof(temp))
        return -1;

    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return -1;

    for (int i = 0; i < img_height; i++)
        rb->memcpy(buffer->clipboard + i * img_width, save_buffer + i * COLS,
                   sizeof(fb_data) * img_width);
    bool ok = write_bmp_fd(fd, &bm) > 0 && rb->file_sync(fd) == 0;
    if (rb->close(fd) < 0)
        ok = false;
    if (ok && rb->rename(temp, file) == 0)
        return 1;
    rb->remove(temp);
    return -1;
}
