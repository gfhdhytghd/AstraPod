/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PODBOX_PANEL_CACHE_H
#define PODBOX_PANEL_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "database/db_summary.h"

#define CAROUSEL_TRACK_TEXT 160
#define PANEL_PAGE_ROWS 32
#define PANEL_WINDOW 3

struct panel_request {
    char title[CAROUSEL_TRACK_TEXT], artist[CAROUSEL_TRACK_TEXT];
    struct album_data album;
    uint64_t key;
    uint32_t revision, generation, locale;
    int first;
    bool remote, legacy_labels;
    char route[320];
};

struct panel_cache_row {
    uint64_t playing_key;
    uint32_t duration, order;
    int32_t id;
    char title[CAROUSEL_TRACK_TEXT], route[320], remote_id[80];
};

struct panel_cached;
enum panel_face { PANEL_ALBUM, PANEL_BODY, PANEL_SMALL, PANEL_FACES };

void panel_cache_init(void);
void panel_cache_watch_playback(void);
uint64_t panel_cache_playing_key(void);
uint64_t panel_cache_identity(const char *category, const char *a, const char *b);
bool panel_cache_same(const struct panel_request *a, const struct panel_request *b);
void panel_cache_window(const struct panel_request *requests, unsigned count);
bool panel_cache_pump(void);
const struct panel_cached *panel_cache_acquire(const struct panel_request *request, int first);
void panel_cache_release(const struct panel_cached *data);
int panel_cache_total(const struct panel_cached *data);
int panel_cache_discs(const struct panel_cached *data);
int panel_cache_disc(const struct panel_cached *data, int n);
unsigned panel_cache_disc_number(const struct panel_cached *data, int n);
const char *panel_cache_artist(const struct panel_cached *data);
const struct panel_cache_row *panel_cache_row(const struct panel_cached *data, int index);
int panel_cache_font(enum panel_face face);
void panel_cache_bind(const struct panel_cached *primary, const struct panel_cached *adjacent);

#endif
