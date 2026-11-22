/* Checked save transport for the upstream puzzle mid-end. GPL-2.0-or-later. */
#define PUZZLE_SAVE_MAX (256 * 1024)

static bool load_game(void)
{
    char fname[MAX_PATH];
    save_fname(fname);

    int fd = rb->open(fname, O_RDONLY);
    if (fd < 0)
        return false;

    off_t size = rb->ffilesize(fd);
    char *game = NULL;
    const char *error = NULL;
    bool loaded = false;

    if (size > 0 && size <= PUZZLE_SAVE_MAX)
    {
        rb->splash(0, "Loading...");
        error = identify_game(&game, read_wrapper, (void *)(intptr_t)fd);
        if (!error && game && !strcmp(game, thegame.name) &&
            rb->lseek(fd, 0, SEEK_SET) == 0)
        {
            error = init_for_game(&thegame, fd);
            loaded = !error;
            if (!loaded && me)
            {
                midend_free(me);
                me = NULL;
            }
        }
    }
    sfree(game);
    rb->close(fd);
    if (!loaded)
        rb->splashf(HZ, "%s", error ? error : "Cannot load saved game");
    return loaded;
}

struct puzzle_output {
    int fd;
    int remaining;
    bool ok;
};

static void write_wrapper(void *context, const void *buf, int len)
{
    struct puzzle_output *output = context;
    if (!output->ok)
        return;
    if (len < 0 || len > output->remaining ||
        rb->write(output->fd, buf, len) != len)
        output->ok = false;
    else
        output->remaining -= len;
}

static void save_game(void)
{
    rb->splash(0, "Saving...");

    char fname[MAX_PATH], temp[MAX_PATH];
    save_fname(fname);
    int len = rb->snprintf(temp, sizeof(temp), "%s.tmp", fname);
    if (len < 0 || (size_t)len >= sizeof(temp))
    {
        rb->splash(HZ, "Save path is too long");
        return;
    }

    struct puzzle_output output = {
        .fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666),
        .remaining = PUZZLE_SAVE_MAX,
        .ok = true,
    };
    if (output.fd < 0)
    {
        rb->splash(HZ, "Cannot open temporary save");
        return;
    }

    midend_serialize(me, write_wrapper, &output);
    output.ok = output.ok && rb->file_sync(output.fd) == 0;
    if (rb->close(output.fd) < 0)
        output.ok = false;
    if (!output.ok || rb->rename(temp, fname) < 0)
    {
        rb->remove(temp);
        rb->splash(HZ, "Cannot save game");
    }
    rb->lcd_update();
}
