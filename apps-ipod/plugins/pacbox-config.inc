/* Pacbox preferences use the stock file format. GNU GPL version 2 or later. */
#define PACBOX_SETTINGS PLUGIN_GAMES_DIR "/pacbox.cfg"
#define PACBOX_SETTINGS_TEMP PACBOX_SETTINGS ".tmp"

static void pacbox_load_settings(void)
{
    int fd = rb->open(PACBOX_SETTINGS, O_RDONLY);
    if (fd < 0)
        return;
    off_t size = rb->ffilesize(fd);
    if (size < 0 || size > 4096)
    {
        rb->close(fd);
        return;
    }

    char line[128];
    struct pacman_settings defaults = settings;
    int version = -1;
    int length;
    while ((length = rb->read_line(fd, line, sizeof(line))) > 0)
    {
        char *key;
        char *value;
        if (!rb->settings_parseline(line, &key, &value))
            continue;
        if (!rb->strcmp(key, "file version"))
            version = rb->atoi(value);
        for (size_t item = 0; item < ARRAYLEN(config); ++item)
        {
            if (rb->strcmp(key, config[item].name))
                continue;
            for (int choice = 0; choice < config[item].max; ++choice)
                if (!rb->strcmp(value, config[item].values[choice]))
                    *config[item].int_p = choice;
            break;
        }
    }

    if (rb->close(fd) < 0 || length < 0 || version != SETTINGS_VERSION)
        settings = defaults;
}

static bool pacbox_save_settings(void)
{
    int fd = rb->open(PACBOX_SETTINGS_TEMP, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return false;

    bool ok = rb->fdprintf(fd, "file version: %d\n", SETTINGS_VERSION) > 0;
    for (size_t item = 0; ok && item < ARRAYLEN(config); ++item)
    {
        const char *value = config[item].values[*config[item].int_p];
        ok = rb->fdprintf(fd, "%s: %s\n", config[item].name, value) > 0;
    }

    ok = ok && rb->file_sync(fd) == 0;
    if (rb->close(fd) < 0)
        ok = false;
    if (ok && rb->rename(PACBOX_SETTINGS_TEMP, PACBOX_SETTINGS) == 0)
        return true;

    rb->remove(PACBOX_SETTINGS_TEMP);
    return false;
}
