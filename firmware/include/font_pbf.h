/* Pixel-native PodBox bitmap font reader. GPL v2+. */
#ifndef FONT_PBF_H
#define FONT_PBF_H

#include "font.h"

bool font_pbf_header(int fd, struct font *font);
size_t font_pbf_buffer_size(const struct font *font, size_t limit, int glyphs);
bool font_pbf_open(struct font *font);
int font_pbf_run(struct font *font, struct font_run *run, ucschar_t cp,
                 struct font_glyph *out);

#endif
