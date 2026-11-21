/* Diagnostic only. Compare every transfer against the still-host-owned Y slot. */
#include "dma-s5l8702.h"
#define PROBE_CH 7u
#define PROBE_BIT (1u << PROBE_CH)
#define PROBE_BASE (s5l8702_dmac0.baddr)
#define PROBE_CHANNEL DMAC_CH_BASE(PROBE_BASE, PROBE_CH)
static struct dmac_ch probe_token;
static bool probe_keep_enabled, probe_quarantined, probe_disabled;
static bool probe_playback_frame;
static struct {
    uint16_t leading[16];
    int16_t samples[9216];
    uint16_t trailing[16];
} probe_area __attribute__((aligned(32)));
#define sm1_output probe_area.samples

static bool probe_busy(void)
{
    return (DMACCxCONFIG(PROBE_CHANNEL) & (DMACCxCONFIG_A_BIT | DMACCxCONFIG_E_BIT))
        || (DMACENABLEDCHANS(PROBE_BASE) & PROBE_BIT);
}

#ifndef SM1_DMA_IRQ_FN
#define SM1_DMA_IRQ_FN __attribute__((target("arm"), noinline))
#endif
static SM1_DMA_IRQ_FN bool probe_reserve(void)
{
    int flags = disable_irq_save();
    bool available = !s5l8702_dmac0.ch_l[PROBE_CH]
        && !(s5l8702_dmac0.ch_run_status & PROBE_BIT) && !probe_busy()
        && !((DMACRAWINTTCSTS(PROBE_BASE) | DMACRAWINTERRSTS(PROBE_BASE)) & PROBE_BIT);
    if (available) {
        probe_keep_enabled = !s5l8702_dmac0.ch_run_status
            && (DMACCONFIG(PROBE_BASE) & DMACCONFIG_E_BIT);
        s5l8702_dmac0.ch_l[PROBE_CH] = &probe_token;
        s5l8702_dmac0.ch_run_status |= PROBE_BIT;
        DMACCONFIG(PROBE_BASE) |= DMACCONFIG_E_BIT;
    }
    restore_irq(flags);
    return available;
}

static SM1_DMA_IRQ_FN void probe_release(void)
{
    int flags = disable_irq_save();
    DMACINTTCCLR(PROBE_BASE) = PROBE_BIT;
    DMACINTERRCLR(PROBE_BASE) = PROBE_BIT;
    s5l8702_dmac0.ch_l[PROBE_CH] = NULL;
    s5l8702_dmac0.ch_run_status &= ~PROBE_BIT;
    if (!s5l8702_dmac0.ch_run_status && !probe_keep_enabled)
        DMACCONFIG(PROBE_BASE) &= ~DMACCONFIG_E_BIT;
    restore_irq(flags);
}

static bool probe_segment(unsigned offset, unsigned words)
{
    DMACINTTCCLR(PROBE_BASE) = PROBE_BIT;
    DMACCxSRCADDR(PROBE_CHANNEL) = (void *)(uintptr_t)(0x22038100u + offset * 2);
    DMACCxDESTADDR(PROBE_CHANNEL) = &sm1_output[offset];
    DMACCxLINK(PROBE_CHANNEL) = 0;
    DMACCxCONTROL(PROBE_CHANNEL) = (1u << DMACCxCONTROL_I_POS)
        | (1u << DMACCxCONTROL_PROT_POS) | (1u << DMACCxCONTROL_DI_POS)
        | (1u << DMACCxCONTROL_SI_POS) | (1u << DMACCxCONTROL_DWIDTH_POS)
        | (1u << DMACCxCONTROL_SWIDTH_POS) | words;
    DMACCxCONFIG(PROBE_CHANNEL) = DMACCxCONFIG_E_BIT; /* masked IRQs, raw TC */
    uint32_t start = USEC_TIMER;
    do {
        if (DMACRAWINTERRSTS(PROBE_BASE) & PROBE_BIT)
            break;
        if ((DMACRAWINTTCSTS(PROBE_BASE) & PROBE_BIT) && !probe_busy())
            return true;
    } while ((uint32_t)(USEC_TIMER - start) < 10000);
    DMACCxCONFIG(PROBE_CHANNEL) |= DMACCxCONFIG_H_BIT;
    start = USEC_TIMER;
    while (DMACCxCONFIG(PROBE_CHANNEL) & DMACCxCONFIG_A_BIT) {
        if ((uint32_t)(USEC_TIMER - start) >= 1000) {
            probe_quarantined = true;
            return false;
        }
    }
    DMACCxCONFIG(PROBE_CHANNEL) = 0;
    start = USEC_TIMER;
    while (probe_busy() && (uint32_t)(USEC_TIMER - start) < 1000) {}
    probe_quarantined = probe_busy();
    return false;
}

/* -1 keeps both slot and buffer pinned until reset, 0 requests the CPU copy. */
static int probe_copy(unsigned words)
{
    static unsigned attempts;
    uintptr_t dest = (uintptr_t)&probe_area;
    if (probe_disabled || attempts >= 16 || !words || (words & 1)
        || words > ARRAYLEN(sm1_output) || dest < DRAM_ORIG
        || dest > DRAM_ORIG + DRAM_SIZE - sizeof(probe_area)
        || !probe_reserve())
        return 0;
    attempts++;
    memset(&probe_area, 0x5a, sizeof(probe_area));
    for (unsigned i = 0; i < words; i++)
        sm1_output[i] = ~SM1_Y[0x80+i];
    commit_discard_dcache_range(&probe_area, sizeof(probe_area));
    bool ok = true;
    for (unsigned offset = 0; offset < words && ok;) {
        unsigned span = MIN(words-offset, 4094u);
        ok = probe_segment(offset, span);
        offset += span;
    }
    if (probe_quarantined) {
        debug_log(DEBUG_LOG_PLAYBACK, "SM1 DMA channel active; slot held");
        return -1;
    }
    probe_release();
    discard_dcache_range(&probe_area, sizeof(probe_area));
    for (unsigned i = 0; i < words && ok; i++)
        ok = (uint16_t)sm1_output[i] == SM1_Y[0x80+i];
    for (unsigned i = words; i < ARRAYLEN(sm1_output) && ok; i++)
        ok = sm1_output[i] == 0x5a5a;
    for (unsigned i = 0; i < ARRAYLEN(probe_area.leading) && ok; i++)
        ok = probe_area.leading[i] == 0x5a5a && probe_area.trailing[i] == 0x5a5a;
    probe_disabled = !ok;
    debug_log(DEBUG_LOG_PLAYBACK, "SM1 DMA words=%u verified=%d", words, ok);
    return ok;
}
