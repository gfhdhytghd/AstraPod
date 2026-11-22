/* Foreground Quake file I/O and PACK validation. GPL-2.0-or-later. */
#include "quakedef.h"

#define QUAKE_OPEN_FILES 12
#define IO_CHUNK 4096
#define PACK_RECORD 64
#define PACK_NAME 56

struct quake_file {
    int handle;
    size_t remaining;
    bool used;
    bool eof;
};

static struct quake_file files[QUAKE_OPEN_FILES];

static bool game_path(const char *path)
{
    /* Engine reads and writes stay under id1. */
    return !strncmp(path, QUAKE_DATA, sizeof(QUAKE_DATA) - 1) &&
           !strstr(path, "..") && !strchr(path, '\\');
}

int quake_unlink(const char *path)
{
    return game_path(path) ? rb->remove(path) : -1;
}

struct quake_file *quake_fopen(const char *path, const char *mode)
{
    quake_check_events();
    if (!game_path(path))
        return NULL;

    int flags = mode[0] == 'r' ? O_RDONLY :
        O_WRONLY | O_CREAT | (mode[0] == 'a' ? O_APPEND : O_TRUNC);
    for (size_t i = 0; i < ARRAYLEN(files); i++)
    {
        if (files[i].used)
            continue;
        int fd = rb->open(path, flags);
        if (fd < 0)
            return NULL;
        files[i] = (struct quake_file){ .handle = fd, .used = true,
                                      .remaining = SIZE_MAX };
        return &files[i];
    }
    return NULL;
}

int quake_fclose(struct quake_file *file)
{
    if (!file || !file->used)
        return -1;
    file->used = false;
    return rb->close(file->handle);
}

int quake_fflush(struct quake_file *file)
{
    return file && file->used ? rb->file_sync(file->handle) : -1;
}

void quake_close_files(void)
{
    for (size_t i = 0; i < ARRAYLEN(files); i++)
        if (files[i].used)
            quake_fclose(&files[i]);
}

int quake_fseek(struct quake_file *file, long offset, int whence)
{
    if (!file || rb->lseek(file->handle, offset, whence) < 0)
        return -1;
    file->eof = false;
    return 0;
}

bool quake_fslice(struct quake_file *file, long offset, size_t bytes)
{
    if (quake_fseek(file, offset, SEEK_SET))
        return false;
    file->remaining = bytes;
    return true;
}

size_t quake_fread(void *ptr, size_t size, size_t count, struct quake_file *file)
{
    if (!file || !size || count > SIZE_MAX / size)
        return 0;

    size_t total = MIN(size * count, file->remaining);
    size_t done = 0;
    while (done < total)
    {
        quake_check_events();
        ssize_t n = rb->read(file->handle, (byte *)ptr + done,
                             MIN(IO_CHUNK, total - done));
        if (n <= 0)
        {
            file->eof = true;
            break;
        }
        done += n;
    }
    if (file->remaining != SIZE_MAX)
        file->remaining -= done;
    if (done < size * count)
        file->eof = true;
    return done / size;
}

size_t quake_fwrite(const void *ptr, size_t size, size_t count,
                    struct quake_file *file)
{
    if (!file || !size || count > SIZE_MAX / size)
        return 0;

    size_t total = size * count;
    size_t done = 0;
    while (done < total)
    {
        quake_check_events();
        ssize_t n = rb->write(file->handle, (const byte *)ptr + done,
                              MIN(IO_CHUNK, total - done));
        if (n <= 0)
            break;
        done += n;
    }
    return done / size;
}

int quake_fgetc(struct quake_file *file)
{
    byte c;
    return quake_fread(&c, 1, 1, file) == 1 ? c : EOF;
}

int quake_feof(struct quake_file *file)
{
    return file->eof;
}

int quake_fprintf(struct quake_file *file, const char *fmt, ...)
{
    char text[2048];
    va_list args;
    va_start(args, fmt);
    int n = quake_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(text))
        return -1;
    return quake_fwrite(text, 1, n, file) == (size_t)n ? n : -1;
}

/* Stock saves store one value per line. */
int quake_scan_line(struct quake_file *file, const char *fmt, void *out,
                    size_t size)
{
    char number[80];
    char *line = fmt[1] == 's' ? out : number;
    size_t limit = fmt[1] == 's' ? size : sizeof(number);
    size_t used = 0;
    int c;

    while ((c = quake_fgetc(file)) != EOF && c != '\n')
    {
        if (used + 1 == limit)
            Sys_Error("Invalid Quake save line");
        line[used++] = c;
    }
    if (!used && c == EOF)
        return 0;
    if (used && line[used - 1] == '\r')
        used--;
    line[used] = 0;

    if (fmt[1] == 'i')
        *(int *)out = Q_atoi(line);
    else if (fmt[1] == 'f')
        *(float *)out = Q_atof(line);
    return 1;
}

static uint32_t little32(const byte *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool pack_valid(struct quake_file *file, bool base)
{
    byte header[12];
    off_t bytes = rb->ffilesize(file->handle);
    if (bytes < (off_t)sizeof(header) || bytes > INT_MAX ||
        fread(header, sizeof(header), 1, file) != 1 ||
        memcmp(header, "PACK", 4))
        return false;

    uint32_t pos = little32(header + 4);
    uint32_t length = little32(header + 8);
    if (pos < sizeof(header) || pos > (uint32_t)bytes ||
        length > (uint32_t)bytes - pos || length % PACK_RECORD ||
        !length || length / PACK_RECORD > QUAKE_PACK_FILES ||
        fseek(file, pos, SEEK_SET))
        return false;

    unsigned int required = 0;
    for (uint32_t i = 0; i < length / PACK_RECORD; i++)
    {
        byte record[PACK_RECORD];
        if (fread(record, sizeof(record), 1, file) != 1)
            return false;
        uint32_t start = little32(record + PACK_NAME);
        uint32_t size = little32(record + PACK_NAME + 4);
        if (start < sizeof(header) || start > (uint32_t)bytes ||
            size > (uint32_t)bytes - start ||
            (start < pos + length && start + size > pos) ||
            !record[0] || !memchr(record, 0, PACK_NAME) || size > QUAKE_MAX_FILE)
            return false;

        char name[PACK_NAME + 1];
        memcpy(name, record, PACK_NAME);
        name[PACK_NAME] = 0;
        if (name[0] == '/' || strstr(name, "..") || strchr(name, '\\'))
            return false;
        if (!strcmp(name, "gfx.wad") && size >= 12)
            required |= 1;
        else if (!strcmp(name, "gfx/palette.lmp") && size == 768)
            required |= 2;
        else if (!strcmp(name, "gfx/colormap.lmp") && size >= 8196)
            required |= 4;
        else if (!strcmp(name, "progs.dat") && size >= 60)
            required |= 8;
    }
    return !base || required == 15;
}

enum quake_data quake_check_data(void)
{
    for (int i = 0; i < QUAKE_OPEN_FILES - 2; i++)
    {
        char path[MAX_OSPATH];
        snprintf(path, sizeof(path), QUAKE_DATA "pak%d.pak", i);
        struct quake_file *file = fopen(path, "rb");
        if (!file)
            return i > 0 ? QUAKE_DATA_OK : QUAKE_DATA_MISSING;
        bool valid = pack_valid(file, i == 0);
        fclose(file);
        if (!valid)
            return QUAKE_DATA_INVALID;
    }
    return QUAKE_DATA_INVALID;
}

static struct quake_file *sys_file(int handle)
{
    return handle > 0 && handle <= QUAKE_OPEN_FILES && files[handle - 1].used ?
        &files[handle - 1] : NULL;
}

int Sys_FileOpenRead(char *path, int *handle)
{
    *handle = -1;
    struct quake_file *file = fopen(path, "rb");
    if (!file)
        return -1;
    off_t size = rb->ffilesize(file->handle);
    if (size < 0 || size > INT_MAX)
    {
        fclose(file);
        return -1;
    }
    *handle = file - files + 1;
    return size;
}

int Sys_FileOpenWrite(char *path)
{
    struct quake_file *file = fopen(path, "wb");
    if (!file)
        Sys_Error("Couldn't open %s", path);
    return file - files + 1;
}

void Sys_FileClose(int handle)
{
    fclose(sys_file(handle));
}

void Sys_FileSeek(int handle, int position)
{
    if (fseek(sys_file(handle), position, SEEK_SET))
        Sys_Error("Quake file seek failed");
}

int Sys_FileRead(int handle, void *dst, int count)
{
    return count > 0 ? fread(dst, 1, count, sys_file(handle)) : 0;
}

int Sys_FileWrite(int handle, void *src, int count)
{
    return count > 0 ? fwrite(src, 1, count, sys_file(handle)) : 0;
}

int Sys_FileTime(char *path)
{
    struct quake_file *file = fopen(path, "rb");
    if (!file)
        return -1;
    fclose(file);
    return 1;
}

void Sys_mkdir(char *path)
{
    if (!strcmp(path, QUAKE_BASE "/id1") || game_path(path))
        mkdir(path);
}

void Sys_Shutdown(void)
{
    quake_close_files();
}
