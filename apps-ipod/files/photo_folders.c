/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Loose pictures for All Photos and the one-directory-at-a-time browser.
 *
 * Every folder that directly holds a picture is an album named after it; two
 * albums of the same name are shown as "Parent/Name". The walk leaves out
 * dot and hidden entries, /iPod_Control (iTunes' own), /Artwork (the art
 * cache's sources), /Photos/Thumbs, and /Photos/Full Resolution beside a
 * Photo Database, whose originals it holds. A folder that also holds a song
 * is a music album and its pictures are its art, so it is no photo album, and
 * cover.* and folder.* are art wherever they are. The root's pictures are in
 * All Photos without an album of their own. Albums and the pictures in each
 * are in natural, case-insensitive name order.
 *
 * Each folder is read twice: once for its pictures, which are then kept or
 * dropped whole, and once for the folders under it. The pictures of one
 * folder are therefore contiguous, and dropping a music folder's costs
 * nothing.
 *
 * In the caller's buffer the folder table comes first, file names grow up
 * after it and file records grow down from the end; the records move down
 * to the names when the walk is done.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "dir.h"
#include "file.h"
#include "fs_attr.h"
#include "strnatcmp.h"
#include "files/filetypes.h"
#include "photo_folders.h"

#define WALK_MAX_DEPTH 12
#define ROOT_FOLDER 0xffff

struct walk {
    struct photo_folders *pf;
    int folder_count;
    size_t names_used, names_size;
    struct photo_file *top;     /* file i is top[-1 - i] */
    bool (*checkpoint)(void);
    bool skip_full;
};

static char walk_path[MAX_PATH];

static bool is_picture(const char *name)
{
    static const char * const exts[] = {
        "jpg", "jpe", "jpeg", "png", "bmp", "gif", "webp",
    };
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return false;
    size_t stem = dot - name;
    if ((stem == 5 && !strncasecmp(name, "cover", 5)) ||
        (stem == 6 && !strncasecmp(name, "folder", 6)))
        return false;
    for (unsigned i = 0; i < ARRAYLEN(exts); i++)
        if (!strcasecmp(dot + 1, exts[i]))
            return true;
    return false;
}

static bool skipped_entry(const char *name, const struct dirinfo *info)
{
    return name[0] == '.' || (info->attribute & (ATTR_HIDDEN | ATTR_SYSTEM));
}

static bool skipped_folder(const struct walk *w, const char *name)
{
    if (!strcmp(walk_path, "/"))
        return !strcasecmp(name, "iPod_Control") ||
               !strcasecmp(name, "Artwork");
    if (!strcasecmp(walk_path, "/Photos"))
        return !strcasecmp(name, "Thumbs") ||
               (w->skip_full && !strcasecmp(name, "Full Resolution"));
    return false;
}

static uint32_t keep_name(struct walk *w, const char *name)
{
    size_t len = strlen(name) + 1;
    size_t files = (size_t)(w->pf->file_count + 1) * sizeof(struct photo_file);
    if (ALIGN_UP(w->names_used + len, sizeof(uint32_t)) + files >
        w->names_size)
        return UINT32_MAX;
    uint32_t at = w->names_used;
    memcpy(w->pf->names + at, name, len);
    w->names_used += len;
    return at;
}

/* This folder's own pictures, kept as an album unless it holds a song */
static bool read_pictures(struct walk *w, bool *subfolders)
{
    struct photo_folders *pf = w->pf;
    int first = pf->file_count;
    size_t names_mark = w->names_used;
    bool audio = false;
    DIR *dir = opendir(walk_path);
    if (!dir)
        return true;
    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        struct dirinfo info = dir_get_info(dir, entry);
        const char *name = (const char *)entry->d_name;
        if (!w->checkpoint())
        {
            closedir(dir);
            return false;
        }
        if (skipped_entry(name, &info))
            continue;
        if (info.attribute & ATTR_DIRECTORY)
        {
            if (skipped_folder(w, name))
                continue;
            *subfolders = true;
            if (pf->directory && w->folder_count < PHOTO_FOLDER_LIMIT)
            {
                uint32_t at = keep_name(w, name);
                if (at != UINT32_MAX)
                {
                    struct photo_folder *folder =
                        &pf->folders[w->folder_count++];
                    *folder = (struct photo_folder){.path = at, .label = at};
                }
            }
            continue;
        }
        if (!audio && filetype_get_attr(name) == FILE_ATTR_AUDIO)
            audio = true;
        if (audio || !is_picture(name))
            continue;
        uint32_t at = keep_name(w, name);
        if (at == UINT32_MAX)
            continue;
        struct photo_file *f = &w->top[-1 - pf->file_count++];
        f->name = at;
        f->size = info.size;
        f->mtime = info.mtime;
        f->folder = ROOT_FOLDER;
        f->thumb_w = f->thumb_h = 0;
        f->thumb = -1;
        f->ram = -1;
        f->pad = 0;
    }
    closedir(dir);

    if (pf->directory)
    {
        if (audio)
            pf->file_count = first;
        return true;
    }
    bool root = !strcmp(walk_path, "/");
    if (pf->file_count == first)
        return true;
    if (audio || (!root && w->folder_count >= PHOTO_FOLDER_LIMIT))
    {
        pf->file_count = first;
        w->names_used = names_mark;
        return true;
    }
    if (root)
        return true;
    uint32_t path = keep_name(w, walk_path);
    if (path == UINT32_MAX)
    {
        pf->file_count = first;
        w->names_used = names_mark;
        return true;
    }
    struct photo_folder *folder = &pf->folders[w->folder_count++];
    folder->path = path;
    folder->label = path + (strrchr(walk_path, '/') + 1 - walk_path);
    folder->first = first;
    folder->count = pf->file_count - first;
    return true;
}

static bool walk(struct walk *w, int depth)
{
    bool subfolders = false;
    if (!read_pictures(w, &subfolders))
        return false;
    if (!subfolders || depth >= WALK_MAX_DEPTH)
        return true;

    size_t len = strlen(walk_path);
    DIR *dir = opendir(walk_path);
    if (!dir)
        return true;
    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        struct dirinfo info = dir_get_info(dir, entry);
        const char *name = (const char *)entry->d_name;
        if (!w->checkpoint())
        {
            closedir(dir);
            return false;
        }
        if (!(info.attribute & ATTR_DIRECTORY) || skipped_entry(name, &info) ||
            skipped_folder(w, name) ||
            len + 1 + strlen(name) >= sizeof(walk_path))
            continue;
        strcpy(walk_path + len, len > 1 ? "/" : "");
        strcat(walk_path, name);
        if (!walk(w, depth + 1))
        {
            closedir(dir);
            return false;
        }
        walk_path[len] = '\0';
    }
    closedir(dir);
    return true;
}

/* qsort() has no context argument */
static const struct photo_folders *sorting;

static int compare_label(const void *left, const void *right)
{
    const struct photo_folder *a = left, *b = right;
    int order = strnatcasecmp(sorting->names + a->label,
                              sorting->names + b->label);
    return order ? order : strnatcasecmp(sorting->names + a->path,
                                         sorting->names + b->path);
}

static int compare_file(const void *left, const void *right)
{
    const struct photo_file *a = left, *b = right;
    int fa = a->folder == ROOT_FOLDER ? -1 : a->folder;
    int fb = b->folder == ROOT_FOLDER ? -1 : b->folder;
    if (fa != fb)
        return fa - fb;
    return strnatcasecmp(sorting->names + a->name, sorting->names + b->name);
}

/* "Parent/Name" for every album whose name another album shares */
static void tell_apart(struct photo_folders *pf)
{
    for (int i = 0; i < pf->album_count; )
    {
        int end = i + 1;
        while (end < pf->album_count &&
               !strcasecmp(photo_folders_label(pf, i),
                           photo_folders_label(pf, end)))
            end++;
        for (int k = i; end - i > 1 && k < end; k++)
        {
            struct photo_folder *f = &pf->folders[k];
            const char *path = pf->names + f->path;
            const char *slash = pf->names + f->label - 1;
            while (slash > path && slash[-1] != '/')
                slash--;
            if (slash > path)
                f->label = slash - pf->names;
        }
        i = end;
    }
}

size_t photo_folders_scan(struct photo_folders *pf, void *buf, size_t size,
                          const char *directory, bool skip_full_resolution,
                          bool (*checkpoint)(void))
{
    size_t table = PHOTO_FOLDER_LIMIT * sizeof(struct photo_folder);
    memset(pf, 0, sizeof(*pf));
    if (size < table + 1024)
        return 0;
    struct walk w = {
        .pf = pf,
        .top = (struct photo_file *)ALIGN_DOWN((uintptr_t)buf + size,
                                              sizeof(uint32_t)),
        .checkpoint = checkpoint,
        .skip_full = skip_full_resolution,
    };
    w.names_size = (char *)w.top - ((char *)buf + table);
    pf->directory = directory;
    pf->folders = buf;
    pf->names = (char *)buf + table;
    strlcpy(walk_path, directory ? directory : "/", sizeof(walk_path));
    bool subfolders = false;
    if (!(directory ? read_pictures(&w, &subfolders) : walk(&w, 0)))
        return 0;
    pf->album_count = w.folder_count;

    /* The records turned into walk order, then moved down to the names */
    int n = pf->file_count;
    struct photo_file *low = w.top - n;
    for (int i = 0; i < n / 2; i++)
    {
        struct photo_file t = low[i];
        low[i] = low[n - 1 - i];
        low[n - 1 - i] = t;
    }
    struct photo_file *files = (struct photo_file *)ALIGN_UP(
        (uintptr_t)pf->names + w.names_used, sizeof(uint32_t));
    memmove(files, low, n * sizeof(*files));
    pf->files = files;

    sorting = pf;
    qsort(pf->folders, pf->album_count, sizeof(*pf->folders), compare_label);
    if (!directory)
    {
        tell_apart(pf);
        qsort(pf->folders, pf->album_count, sizeof(*pf->folders),
              compare_label);
    }
    for (int k = 0; k < pf->album_count; k++)
    {
        const struct photo_folder *f = &pf->folders[k];
        for (int i = f->first; i < f->first + f->count; i++)
            files[i].folder = k;
    }
    qsort(files, pf->file_count, sizeof(*files), compare_file);
    for (int i = 0, k = -1; i < pf->file_count; i++)
    {
        if (files[i].folder == ROOT_FOLDER || files[i].folder == k)
            continue;
        k = files[i].folder;
        pf->folders[k].first = i;
    }
    return (char *)(files + pf->file_count) - (char *)buf;
}

bool photo_folders_path(const struct photo_folders *pf, int file, char *buf,
                        size_t size)
{
    const struct photo_file *f = &pf->files[file];
    const char *folder = "";
    if (pf->directory && strcmp(pf->directory, "/"))
        folder = pf->directory;
    else if (f->folder != ROOT_FOLDER)
        folder = pf->names + pf->folders[f->folder].path;
    return (size_t)snprintf(buf, size, "%s/%s", folder,
                            pf->names + f->name) < size;
}
