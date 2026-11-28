/***************************************************************************
 * GNU General Public License (version 2+)
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "button.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "piezo.h"
#include "piezo-pcm.h"

/* Timer A paces 1-bit samples. GPIO drives the same pin and mode as the
 * Classic bootloader's square-wave tones in piezo-6g.c. */
#define SAMPLE_RATE 12500
#define SAMPLE_US (1000000 / SAMPLE_RATE)
#define TIMER_PERIOD (TIMER_FREQ / 2 / SAMPLE_RATE)
#define TIMER_MATCH0_EN (1 << 12)
#define TIMER_ECLK_DIV2 (1 << 6)
#define TIMER_ACK_ALL (7 << 16)
#define PIEZO_GPIO_LOW 0x0060e
#define PIEZO_THRESHOLD (24 * 16) /* Q4, about -63 dBFS */
#define QUIET_SAMPLES (SAMPLE_RATE / 100)

static volatile bool enabled;
static volatile bool active;
static volatile bool locked;
static volatile bool muted;
static bool output_high;
static unsigned int quiet_samples;
static unsigned int step, alpha;
static unsigned int phase, next_sample;
#ifdef HAVE_HW_H264
static uint32_t played_bytes;

uint32_t piezo_pcm_played_bytes(void)
{
    return played_bytes;
}
#endif
static int low1, low2, dc;
static const int16_t *input, *end;

void piezo_pcm_enable(bool on)
{
    enabled = on;
}

bool piezo_pcm_selected(void)
{
    return enabled && !headphones_inserted();
}

bool piezo_pcm_active(void)
{
    return active;
}

void piezo_pcm_lock(bool lock)
{
    locked = lock;
}

void piezo_pcm_set_volume(int tenth_db)
{
    /* This path uses full GPIO swing at every non-muted volume. */
    muted = tenth_db <= -600;
}

void piezo_pcm_set_rate(unsigned int hz)
{
    /* Two low-pass stages retain the presence band before decimation. */
    unsigned int coeff = (30000u << 8) / (hz + 15000u);
    int irq = disable_irq_save();
    step = hz;
    alpha = MIN(coeff, 255u);
    restore_irq(irq);
}

static void set_input(const void *addr, size_t size)
{
    input = addr;
    end = input + size / sizeof(*input);
}

void piezo_pcm_start(const void *addr, size_t size)
{
    int irq = disable_irq_save();
    piezo_stop();
    active = true;
    set_input(addr, size);
    phase = SAMPLE_RATE - 1;
    low1 = low2 = dc = 0;
    quiet_samples = 0;
    output_high = false;
    GPIOCMD = PIEZO_GPIO_LOW;

    TACMD = 0;
    TACON = TIMER_ECLK_DIV2 | TIMER_MATCH0_EN | TIMER_ACK_ALL;
    TAPRE = 0;
    TADATA0 = TIMER_PERIOD;
    TACMD = 2; /* Clear and load the compare buffer */
    next_sample = USEC_TIMER + SAMPLE_US;
    TACMD = 1;
    restore_irq(irq);
}

void piezo_pcm_stop(void)
{
    if (!active)
        return;

    int irq = disable_irq_save();
    active = false;
    GPIOCMD = PIEZO_GPIO_LOW;
    piezo_stop();
    restore_irq(irq);
}

void piezo_pcm_irq(void) ICODE_ATTR;
void piezo_pcm_irq(void)
{
    if (locked)
        return;

    /* Count missed updates after brief IRQ or PCM locks. Bound catch-up
     * work so a long stall cannot keep the CPU in the interrupt handler. */
    int late = (int32_t)(USEC_TIMER - next_sample);
    if (late < 0)
        return;

    unsigned int periods = 1 + (unsigned int)late / SAMPLE_US;
    next_sample += periods * SAMPLE_US;
    phase += MIN(periods, 8u) * step;

    /* Switch at a PCM buffer boundary so each sink owns whole buffers. */
    bool selected = piezo_pcm_selected();
    while (phase >= SAMPLE_RATE)
    {
        if (input == end)
        {
            const void *addr;
            size_t size;
            if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
                return;

            if (!selected)
            {
                piezo_pcm_stop();
                piezo_pcm_resume_codec(addr, size);
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
                return;
            }

            set_input(addr, size);
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
        }

        /* Sum the two signed 16-bit channels, then keep four fraction bits
         * for the filter accumulators. Filter before dropping samples. */
        int mono = (input[0] + input[1]) * 8;
        input += 2;
        low1 += ((mono - low1) * (int)alpha) >> 8;
        low2 += ((low1 - low2) * (int)alpha) >> 8;
        phase -= SAMPLE_RATE;
#ifdef HAVE_HW_H264
        played_bytes += 4;
#endif
    }

    /* Favor speech and other midrange content before hard limiting. A
     * Schmitt trigger gives full swing for quiet signals and holds its
     * last level across zero until the signal crosses the other threshold. */
    dc += ((low2 - dc) * 13) >> 6;
    int signal = low2 - dc;
    bool high = output_high;

    if (!selected || muted)
        high = false;
    else if (signal > PIEZO_THRESHOLD || signal < -PIEZO_THRESHOLD)
    {
        quiet_samples = 0;
        high = signal > 0;
    }
    else if (quiet_samples < QUIET_SAMPLES && ++quiet_samples == QUIET_SAMPLES)
        high = false;

    if (high != output_high)
    {
        output_high = high;
        GPIOCMD = PIEZO_GPIO_LOW | high;
    }
}
