/* Exercise the target probe against a deterministic PL080 register model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dma-s5l8702.h"
struct dmac s5l8702_dmac0;
static uint32_t cfg, control, link_reg, global_cfg, enabled, tc, err, tcclr, errclr;
static void *src, *dst;
static uint16_t y[0x80 + 9216];
static unsigned segments, model, micros = UINT32_MAX - 400;
static uint32_t clock_read(void);
#undef DMACCxCONFIG
#undef DMACCxCONTROL
#undef DMACCxLINK
#undef DMACCxSRCADDR
#undef DMACCxDESTADDR
#undef DMACCONFIG
#undef DMACENABLEDCHANS
#undef DMACRAWINTTCSTS
#undef DMACRAWINTERRSTS
#undef DMACINTTCCLR
#undef DMACINTERRCLR
#define DMACCxCONFIG(x) cfg
#define DMACCxCONTROL(x) control
#define DMACCxLINK(x) link_reg
#define DMACCxSRCADDR(x) src
#define DMACCxDESTADDR(x) dst
#define DMACCONFIG(x) global_cfg
#define DMACENABLEDCHANS(x) enabled
#define DMACRAWINTTCSTS(x) tc
#define DMACRAWINTERRSTS(x) err
#define DMACINTTCCLR(x) tcclr
#define DMACINTERRCLR(x) errclr
#define SM1_Y y
#define DRAM_ORIG ((uintptr_t)&probe_area)
#define DRAM_SIZE sizeof(probe_area)
#define USEC_TIMER clock_read()
#define SM1_DMA_IRQ_FN
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define disable_irq_save() 0
#define restore_irq(x) ((void)(x))
#define commit_discard_dcache_range(p,s) ((void)0)
#define discard_dcache_range(p,s) ((void)0)
#define debug_log(...) ((void)0)
#include "../../apps-ipod/screens/system/sm1_dma_probe.inc"

static uint32_t clock_read(void)
{
    tc &= ~tcclr; err &= ~errclr; tcclr = errclr = 0;
    if (cfg == DMACCxCONFIG_E_BIT) {
        unsigned pos = ((uintptr_t)src - 0x22038100u)/2;
        unsigned n = control & 4095;
        assert(n && n <= 4094 && !(n & 1) && pos+n <= 9216 && !link_reg);
        assert(dst == &sm1_output[pos]);
        segments++;
        if (model == 3 || model == 4) {
            cfg |= DMACCxCONFIG_A_BIT;
            enabled = PROBE_BIT;
        } else if (model == 2 && segments == 2) {
            err = PROBE_BIT;
            cfg = 0;
        } else {
            memcpy(dst, &y[0x80+pos], n*2);
            if (model == 5) sm1_output[pos] ^= 1;
            cfg = 0;
            tc = PROBE_BIT;
        }
    }
    if ((cfg & DMACCxCONFIG_H_BIT) && model == 3)
        cfg &= ~DMACCxCONFIG_A_BIT;
    if (!cfg) enabled = 0;
    return micros += 100;
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    model = atoi(argv[1]);
    unsigned words = atoi(argv[2]);
    for (unsigned i=0; i<words; i++) y[0x80+i] = 131u*i;
    if (model == 1) s5l8702_dmac0.ch_run_status = PROBE_BIT;
    int result = probe_copy(words);
    assert(result == (model == 0 ? 1 : model == 4 ? -1 : 0));
    assert(probe_quarantined == (model == 4));
    assert((s5l8702_dmac0.ch_l[7] != NULL) == probe_quarantined);
    assert(s5l8702_dmac0.ch_run_status == (model == 1 || model == 4 ? PROBE_BIT : 0));
    if (!model) assert(segments == (words+4093)/4094);
    if (model == 1) assert(!segments);
    if (model == 2) assert(segments == 2);
    if (model == 4) assert(cfg & DMACCxCONFIG_H_BIT);
    (void)probe_playback_frame;
    printf("model=%u words=%u segments=%u result=%d\n", model, words, segments, result);
}
