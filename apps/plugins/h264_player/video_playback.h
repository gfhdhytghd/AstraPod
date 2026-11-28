/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025-2026 David Cormier
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
#ifndef VIDEO_PLAYBACK_H
#define VIDEO_PLAYBACK_H

/*
 * Hardware H.264 player implementation.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum video_result
{
    VIDEO_FAILED = -1,
    VIDEO_FINISHED,
    VIDEO_EXIT,
    VIDEO_USB,
    VIDEO_PREVIOUS,
    VIDEO_NEXT,
};

struct video_play_options
{
    const char *title;
    int (*battery_level)(void);
    const uint32_t *chapters;
    size_t chapter_count;
    int max_volume;
    bool has_previous;
    bool themed_controls;
    unsigned accent; /* RGB565 theme accent. */
    /* Standard menu result: 0 resumes, 1 restarts, MENU_ATTACHED_USB exits. */
    int (*resume_menu)(void);
    bool (*control_centre)(void); /* False on USB takeover. */
    int (*picture_mode)(int step); /* Zero reads without changing the mode. */
};

int video_h264_play_named(const char *path, void *buffer, size_t size,
                          const struct video_play_options *options);

/* Size the playback arena from the container header. Returns 0 on error. */
size_t video_h264_scratch_size(const char *path);

/* Play a validated H.264 Baseline/AAC-LC MP4 from caller-owned memory.
 * The caller must retain the playback buffer until this function returns.
 * Returns 0 at end of stream, 1 on user exit, 2 for USB, or -1 on failure. */
int video_h264_play(const char *filepath, void *buffer, size_t buffer_size);

#endif /* VIDEO_PLAYBACK_H */
