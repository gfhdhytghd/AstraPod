/* Optional frame-time capture for native motion. GPL version 2+. */
#ifndef PODBOX_UI_PERF_H
#define PODBOX_UI_PERF_H

#include <stdint.h>
#include <stdbool.h>

/* Check /.rockbox/ui-perf.enable after mounting storage. Each capture writes
 * /.rockbox/ui-perf.txt on exit. Route names are short string literals. */
void ui_perf_init(void);
bool ui_perf_enabled(void);
unsigned ui_perf_begin(const char *route);
/* Ancillary animation. Foreground motion can claim the recorder immediately
 * and discard an overlapping passive trace without writing on its hot path. */
unsigned ui_perf_begin_passive(const char *route);
void ui_perf_end(unsigned capture);

/* Accumulate carousel diagnostics until the next artwork submission. */
void ui_perf_note_cover_work(unsigned changes, uint32_t cache_wait_us,
                             uint32_t caption_us);
void ui_perf_note_render(uint32_t elapsed_clock);
void ui_perf_note_submit(uint32_t elapsed_clock);

/* For a frame with multiple driver calls, sample each immediately on return.
 * Zero-initialize the accumulator before the first call. */
struct ui_perf_transfers {
    uint32_t first, last, calls_us, copy_us, dma_phase;
    bool sampled, async;
};
void ui_perf_add_transfer(struct ui_perf_transfers *timing, uint32_t start);
void ui_perf_note_transfers(const struct ui_perf_transfers *timing);
/* What the next submission sends: UI_PERF_* parts, the art rect's rows and
 * the caption's rows. Both parts can share a rect or use two transfers. */
#define UI_PERF_ART 1u
#define UI_PERF_CAPTION 2u
void ui_perf_note_rects(unsigned parts, unsigned art_rows,
                        unsigned caption_rows);

#endif
