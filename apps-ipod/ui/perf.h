/* Optional frame-time capture for native motion. GPL version 2+. */
#ifndef PODBOX_UI_PERF_H
#define PODBOX_UI_PERF_H

#include <stdint.h>

/* Check /.rockbox/ui-perf.enable after mounting storage. Each capture writes
 * /.rockbox/ui-perf.txt on exit. Route names are short string literals. */
void ui_perf_init(void);
unsigned ui_perf_begin(const char *route);
/* Ancillary animation. Foreground motion can claim the recorder immediately
 * and discard an overlapping passive trace without writing on its hot path. */
unsigned ui_perf_begin_passive(const char *route);
void ui_perf_end(unsigned capture);

/* Call with USEC_TIMER differences. Multiple paint stages accumulate until
 * the next actual framebuffer submission. */
void ui_perf_note_render(uint32_t elapsed_clock);
void ui_perf_note_submit(uint32_t elapsed_clock);
/* What the next submission sends: UI_PERF_* parts, the art rect's rows and
 * the caption's rows. With both parts the caption lies inside the art rect. */
#define UI_PERF_ART 1u
#define UI_PERF_CAPTION 2u
void ui_perf_note_rects(unsigned parts, unsigned art_rows,
                        unsigned caption_rows);

#endif
