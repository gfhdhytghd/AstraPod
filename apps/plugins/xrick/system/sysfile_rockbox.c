 /***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Port of xrick, a Rick Dangerous clone, to Rockbox.
 * See http://www.bigorno.net/xrick/
 *
 * Copyright (C) 2008-2014 Pierluigi Vicinanza
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "xrick/system/system.h"

#include "xrick/config.h"
#include "xrick/util.h"

#include "plugin.h"
#include <limits.h>

#define XRICK_GAME_DIR ROCKBOX_DIR "/xrick"

/*
 * Global variables
 */
const char *sysfile_defaultPath = XRICK_GAME_DIR;

/*
 * Local variables
 */
static char *rootPath = NULL;

/*
 *
 */
bool sysfile_setRootPath(const char *name)
{
    rootPath = u_strdup(name);
    return (rootPath != NULL);
}

/*
 *
 */
void sysfile_clearRootPath()
{
    sysmem_pop(rootPath);
    rootPath = NULL;
}

/*
 * Open a data file.
 */
file_t sysfile_open(const char *name)
{
    char path[MAX_PATH];
    int length = rb->snprintf(path, sizeof(path), "%s/%s", rootPath, name);
    if (length < 0 || (size_t)length >= sizeof(path))
        return NULL;

    int fd = rb->open(path, O_RDONLY);
    return fd < 0 ? NULL : (file_t)((intptr_t)fd + 1);
}

/*
 * Read a file within a data archive.
 */
int sysfile_read(file_t file, void *buf, size_t size, size_t count)
{
    int fd = (intptr_t)file - 1;
    if (!size || count > INT_MAX / size)
        return 0;
    ssize_t read = rb->read(fd, buf, size * count);
    return read < 0 ? 0 : read / (ssize_t)size;
}

/*
 * Seek.
 */
int sysfile_seek(file_t file, long offset, int origin)
{
    int fd = (intptr_t)file - 1;
    return rb->lseek(fd, offset, origin);
}

int sysfile_tell(file_t file)
{
    return sysfile_seek(file, 0, SEEK_CUR);
}

off_t sysfile_size(file_t file)
{
#ifdef ASTRAPOD_XRICK
    return rb->ffilesize((intptr_t)file - 1);
#else
    return rb->filesize((intptr_t)file - 1);
#endif
}

/*
 * Close a file within a data archive.
 */
void sysfile_close(file_t file)
{
    int fd = (intptr_t)file - 1;
    rb->close(fd);
}

/* eof */
