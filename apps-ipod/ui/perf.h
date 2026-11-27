/* Optional frame-time capture for native motion. GPL version 2+. */
#ifndef PODBOX_UI_PERF_H
#define PODBOX_UI_PERF_H

#include <stdint.h>
#include <stdbool.h>

/* Check /.rockbox/ui-perf.enable after mounting storage. Each capture writes
 * /.rockbox/ui-perf.txt on exit. Route names are short string literals. */
void ui_perf_init(void);
bool ui_perf_enabled(void);
struct ui_rect;
void ui_perf_wps_target(char route, const struct ui_rect *rect,
                        const char *reason, const char *bitmap,
                        uint32_t handoff_us, uint32_t playlist_us,
                        uint32_t wps_setup_us);
/* Which way Cover Flow was entered and left, and why. */
void ui_perf_flow_path(const char *enter, const char *enter_why,
                       const char *leave, const char *leave_why);
unsigned ui_perf_begin(const char *route);
/* Ancillary animation. Foreground motion can claim the recorder immediately
 * and discard an overlapping passive trace without writing on its hot path. */
unsigned ui_perf_begin_passive(const char *route);
void ui_perf_end(unsigned capture);

enum ui_cover_stage {
    UI_COVER_MOTION, UI_COVER_PROJECT, UI_COVER_COLUMNS, UI_COVER_BLIT,
    UI_COVER_CLEAR, UI_COVER_OTHER, UI_COVER_STAGE_COUNT
};
/* Non-overlapping render stages accumulate until the next art submission. */
void ui_perf_note_cover_work(unsigned changes, uint32_t cache_wait_us,
                             uint32_t caption_us, const uint32_t stages[UI_COVER_STAGE_COUNT]);
void ui_perf_note_render(uint32_t elapsed_clock);
void ui_perf_note_submit(uint32_t elapsed_clock);

/* For a frame with multiple driver calls, sample each immediately on return.
 * Zero-initialize the accumulator before the first call. */
struct ui_perf_transfers {
    uint32_t primary_start, last, calls_us, copy_us, dma_phase;
    uint32_t caption_calls_us, caption_copy_us;
    bool sampled, async;
};
void ui_perf_add_transfer(struct ui_perf_transfers *timing, uint32_t start, unsigned part);
void ui_perf_note_transfers(const struct ui_perf_transfers *timing);
/* What the next submission sends: UI_PERF_* parts, the art rect's rows and
 * the caption's rows. Both parts can share a rect or use two transfers. */
#define UI_PERF_ART 1u
#define UI_PERF_CAPTION 2u
void ui_perf_note_rects(unsigned parts, unsigned art_rows,
                        unsigned caption_rows);
/* The next submission is a hardware-scroll frame: the panel's scroll
 * offset after it and the rows of pixels its bands carry. */
void ui_perf_note_hwscroll(int offset, unsigned rows);

#endif
