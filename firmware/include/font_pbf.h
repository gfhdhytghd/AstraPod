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
void font_pbf_set_batch_trace(void (*trace)(unsigned, uint32_t, unsigned));

/* Build and validate compact, sorted records for an independently held face.
 * Build runs on the prefetch worker. Bind runs on the drawing thread. */
#define FONT_PACK_CODES 768
size_t font_pbf_pack_build(int id, const char *path, const ucschar_t *codes,
                           unsigned count, unsigned char *out, size_t size,
                           bool (*cancelled)(void));
bool font_pbf_pack_valid(int id, const unsigned char *pack, size_t size);
void font_pbf_pack_bind(int id, const unsigned char *primary, const unsigned char *adjacent);
uint32_t font_pbf_pack_revision(int id);

#endif
