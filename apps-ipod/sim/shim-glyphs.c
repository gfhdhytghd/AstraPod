/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A slow disk for glyph batches, for the tests. With PODBOX_GLYPH_DELAY_MS
 * set, each batch of glyphs the font prefetch reads reaches the font that
 * many milliseconds late, as on the player, where the simulator's host
 * disk would otherwise load a page's glyphs before anything shows them
 * missing. The delay sleeps in the background thread that reads the batch.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

#include <stdint.h>
#include <stdlib.h>
#include "kernel.h"
#include "font_pbf.h"

static long delay_ticks;

static void late_batch(unsigned height, uint32_t elapsed, unsigned count)
{
    (void)height;
    (void)elapsed;
    (void)count;
    sleep(delay_ticks);
}

static void __attribute__((constructor)) glyph_delay_init(void)
{
    const char *ms = getenv("PODBOX_GLYPH_DELAY_MS");
    delay_ticks = ms ? atol(ms) * HZ / 1000 : 0;
    if (delay_ticks > 0)
        font_pbf_set_batch_trace(late_batch);
}

#endif /* SIMULATOR */
