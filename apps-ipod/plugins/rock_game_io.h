/* Foreground image and font adapters. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_GAME_IO_H
#define ASTRAPOD_ROCK_GAME_IO_H

#include <stdbool.h>
#include <stddef.h>

struct bitmap;

int rock_read_plugin_bmp(const char *path, struct bitmap *bm, size_t size,
                         bool resize);
int rock_load_plugin_art(struct bitmap *bm, size_t size);

int rock_load_plugin_font(const char *path);
void rock_unload_plugin_font(int id);
void rock_release_plugin_font(void);

#endif
