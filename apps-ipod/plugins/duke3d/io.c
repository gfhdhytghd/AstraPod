/* Duke3D file namespace and save publication. GNU GPL version 2 or later. */
#include "duke-build.h"

static FILE *writer;
static char destination[MAX_PATH];
static char temporary[MAX_PATH];

static bool duke_path(char *path, const char *name, bool write)
{
    if (!name)
        return false;
    const char prefix[] = DUKE_DIR "/";
    if (!rb->strncmp(name, prefix, sizeof(prefix) - 1))
        name += sizeof(prefix) - 1;
    if (!rb->strncmp(name, "saves/", 6))
        name += 6;

    if (!*name || !rb->strcmp(name, ".") || !rb->strcmp(name, "..") ||
        rb->strchr(name, '/') || rb->strchr(name, '\\'))
        return false;

    const char *extension = rb->strrchr(name, '.');
    bool save = extension && (!rb->strcasecmp(extension, ".cfg") ||
                              !rb->strcasecmp(extension, ".sav") ||
                              !rb->strcasecmp(extension, ".esv"));
    int n = rb->snprintf(path, MAX_PATH, "%s/%s",
                         write || save ? DUKE_SAVES : DUKE_DIR, name);
    return n > 0 && n < MAX_PATH;
}

int duke_open(const char *name, int flags, ...)
{
    char path[MAX_PATH];
    if (!duke_path(path, name, flags != O_RDONLY))
        return -1;
    return rb->open(path, flags, 0666);
}

int duke_remove(const char *name)
{
    char path[MAX_PATH];
    return duke_path(path, name, true) ? rb->remove(path) : -1;
}

FILE *duke_fopen(const char *name, const char *mode)
{
    if (*mode == 'r')
    {
        char path[MAX_PATH];
        return duke_path(path, name, false) ? _fopen_(path, mode) : NULL;
    }
    if (*mode != 'w' || writer || !duke_path(destination, name, true))
        return NULL;

    int n = rb->snprintf(temporary, sizeof(temporary), "%s.tmp", destination);
    if (n < 0 || n >= (int)sizeof(temporary))
        return NULL;
    writer = _fopen_(temporary, mode);
    return writer;
}

int duke_fclose(FILE *file)
{
    if (!file)
        return -1;
    if (file != writer)
        return _fclose_(file);

    bool ok = !_ferror_(file) && rb->file_sync(file->fd) == 0;
    ok = _fclose_(file) == 0 && ok;
    writer = NULL;
    if (ok && rb->rename(temporary, destination) == 0)
        return 0;

    rb->remove(temporary);
    return -1;
}

void duke_abort_files(void)
{
    if (writer)
    {
        _fclose_(writer);
        writer = NULL;
        rb->remove(temporary);
    }
}

bool duke_file_exists(const char *name)
{
    char path[MAX_PATH];
    return duke_path(path, name, false) && rb->file_exists(path);
}
