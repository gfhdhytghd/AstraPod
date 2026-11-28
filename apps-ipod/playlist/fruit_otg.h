/* Copyright (C) 2026 AstraPod contributors
 * GPL version 2 or later. Fruit Company On-The-Go and Genius screens. */
#ifndef FRUIT_OTG_H
#define FRUIT_OTG_H

#include <stdbool.h>
#include <stddef.h>

struct gui_synclist;

bool fruit_otg_add(const char *path, bool db_group);
void fruit_otg_refresh(void);
int fruit_otg_count(void);
int fruit_playlist_count(const char *path, unsigned mtime);
const char *fruit_otg_count_text(char *buf, size_t size, int count, bool remove);
int fruit_otg_show(char *saved_path, size_t size, int *track_selection);
int fruit_genius_show(void);
int fruit_genius_unavailable(void);
int fruit_list_action(int action, struct gui_synclist *list);

#endif
