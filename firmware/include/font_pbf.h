/* Pixel-native PodBox bitmap font reader. GPL v2+. */
#ifndef FONT_PBF_H
#define FONT_PBF_H

#include "font.h"

void font_pbf_reserve_hot(void);
bool font_pbf_header(int fd, struct font *font);
size_t font_pbf_buffer_size(const struct font *font, size_t limit, int glyphs);
bool font_pbf_open(struct font *font);
int font_pbf_run(struct font *font, struct font_run *run, ucschar_t cp,
                 struct font_glyph *out);
bool font_pbf_prefetch(struct font *font, int id, const char *path,
                       const ucschar_t *text);
void font_pbf_forget(struct font *font, int id);
void font_pbf_prefetch_suspend(bool suspend);
/* The application queues one work item on its existing background thread. */
void font_pbf_prefetch_init(void (*schedule)(void));
void font_pbf_prefetch_work(void);

#endif
