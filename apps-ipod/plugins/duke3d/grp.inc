/* Bounded GRP index and streaming reads. GNU GPL version 2 or later. */
#define DUKE_GRP_MAX_BYTES (64 * 1024 * 1024)
#define DUKE_GRP_MAX_ENTRIES 4096
#define DUKE_FILES 64

struct duke_grp_entry {
    char name[13];
    int32_t start;
    int32_t length;
};

struct duke_grp {
    struct duke_grp_entry *entries;
    int count;
    int fd;
};

static struct {
    struct duke_grp archives[MAXGROUPFILES];
    int num;
} grpSet;

static struct {
    bool used;
    int fd;
    int32_t cursor;
    const struct duke_grp_entry *entry;
} duke_files[DUKE_FILES];

static uint32_t duke_le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

int32_t filelength(int32_t fd)
{
    off_t bytes = rb->ffilesize(fd);
    return bytes >= 0 && bytes <= INT32_MAX ? bytes : -1;
}

int32_t initgroupfile(const char *name)
{
    if (grpSet.num >= MAXGROUPFILES)
        return -1;

    int fd = duke_open(name, O_RDONLY);
    if (fd < 0)
        return -1;

    struct duke_grp_entry *entries = NULL;
    uint8_t record[16];
    int32_t bytes = filelength(fd);
    if (bytes < (int32_t)sizeof(record) || bytes > DUKE_GRP_MAX_BYTES ||
        read(fd, record, sizeof(record)) != sizeof(record) ||
        memcmp(record, "KenSilverman", 12))
        goto invalid;

    uint32_t count = duke_le32(record + 12);
    if (!count || count > DUKE_GRP_MAX_ENTRIES ||
        count > (uint32_t)(bytes - 16) / 16)
        goto invalid;

    entries = malloc(count * sizeof(*entries));
    if (!entries)
        goto invalid;

    int32_t cursor = 16 + 16 * count;
    for (uint32_t i = 0; i < count; i++)
    {
        if (read(fd, record, sizeof(record)) != sizeof(record))
            goto invalid;
        uint32_t length = duke_le32(record + 12);
        if (length > (uint32_t)(bytes - cursor))
            goto invalid;
        memcpy(entries[i].name, record, 12);
        entries[i].name[12] = '\0';
        entries[i].start = cursor;
        entries[i].length = length;
        cursor += length;
        if ((i & 31) == 0)
            duke_service();
    }
    if (cursor != bytes || lseek(fd, 0, SEEK_SET) != 0)
        goto invalid;

    uint8_t block[4096];
    uint32_t crc = 0;
    for (int32_t left = bytes; left > 0;)
    {
        int32_t chunk = MIN(left, (int32_t)sizeof(block));
        if (read(fd, block, chunk) != chunk)
            goto invalid;
        crc = crc32_update(block, chunk, crc);
        left -= chunk;
        duke_service();
    }

    int slot = grpSet.num++;
    grpSet.archives[slot] = (struct duke_grp){entries, count, fd};
    groupefil_crc32[slot] = crc;
    return slot;

invalid:
    free(entries);
    close(fd);
    return -1;
}

static bool duke_valid_file(int handle)
{
    return (unsigned)handle < DUKE_FILES && duke_files[handle].used;
}

void kclose(int32_t handle)
{
    if (!duke_valid_file(handle))
        return;
    if (!duke_files[handle].entry)
        close(duke_files[handle].fd);
    duke_files[handle].used = false;
}

void uninitgroupfile(void)
{
    for (int i = 0; i < DUKE_FILES; i++)
        kclose(i);

    while (grpSet.num)
    {
        int slot = --grpSet.num;
        close(grpSet.archives[slot].fd);
        free(grpSet.archives[slot].entries);
        groupefil_crc32[slot] = 0;
    }
}

int32_t kopen4load(const char *name, int only_grp)
{
    int slot = 0;
    while (slot < DUKE_FILES && duke_files[slot].used)
        slot++;
    if (slot == DUKE_FILES)
        return -1;

    int fd = only_grp ? -1 : duke_open(name, O_RDONLY);
    if (fd >= 0)
    {
        duke_files[slot] = (typeof(duke_files[0])){.used = true, .fd = fd};
        return slot;
    }

    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!base[0] || strlen(base) > 12)
        return -1;

    for (int g = grpSet.num - 1; g >= 0; g--)
    {
        const struct duke_grp *grp = &grpSet.archives[g];
        for (int i = grp->count - 1; i >= 0; i--)
        {
            const struct duke_grp_entry *entry = &grp->entries[i];
            if (!strcasecmp(entry->name, base))
            {
                duke_files[slot] = (typeof(duke_files[0])){
                    .used = true, .fd = grp->fd, .entry = entry,
                };
                return slot;
            }
        }
    }
    return -1;
}

int32_t kread(int32_t handle, void *buffer, int32_t length)
{
    if (!duke_valid_file(handle) || length < 0)
        return -1;
    typeof(duke_files[0]) *file = &duke_files[handle];
    if (file->entry)
    {
        length = MIN(length, file->entry->length - file->cursor);
        if (lseek(file->fd, file->entry->start + file->cursor, SEEK_SET) < 0)
            return -1;
    }
    int32_t result = read(file->fd, buffer, length);
    if (file->entry && result > 0)
        file->cursor += result;
    return result;
}

int32_t klseek(int32_t handle, int32_t offset, int whence)
{
    if (!duke_valid_file(handle))
        return -1;
    typeof(duke_files[0]) *file = &duke_files[handle];
    if (!file->entry)
        return lseek(file->fd, offset, whence);

    int64_t pos = offset;
    switch (whence)
    {
        case SEEK_CUR: pos += file->cursor; break;
        case SEEK_END: pos += file->entry->length; break;
        case SEEK_SET: break;
        default: return -1;
    }

    if (pos < 0 || pos > file->entry->length)
        return -1;
    return file->cursor = pos;
}

int32_t kfilelength(int32_t handle)
{
    if (!duke_valid_file(handle))
        return -1;
    const typeof(duke_files[0]) *file = &duke_files[handle];
    return file->entry ? file->entry->length : filelength(file->fd);
}
