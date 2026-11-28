/***************************************************************************
 * GNU General Public License (version 2+)
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "button.h"
#include "fixedpoint.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "piezo.h"
#include "piezo-pcm.h"

/* ECLK/2 is 6 MHz. Timer A's 240 ticks give a 25 kHz PWM carrier.
 * TADATA0 latches at the next period match (S5L8700X DS, 11-18). */
#define PWM_PERIOD 240
#define SAMPLE_RATE ((TIMER_FREQ / 2) / PWM_PERIOD / 2)
#define SAMPLE_US (1000000 / SAMPLE_RATE)
#define TIMER_MATCH1_EN (1 << 13)
#define TIMER_ECLK_DIV2 (1 << 6)
#define TIMER_PWM (1 << 4)
#define TIMER_ACK_ALL (7 << 16)
#define PWM_MID (PWM_PERIOD / 2)
#define PWM_SWING (PWM_MID - 8)
#define GAIN_BITS 12

static volatile bool enabled;
static volatile bool active;
static volatile bool locked;
static volatile int gain = 1 << GAIN_BITS;
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
    /* Match the codec's mute and use its default -25 dB as piezo unity. */
    int db = MIN(tenth_db + 250, 60);
    gain = tenth_db <= -600 ? 0 :
           fp_factor(db * (1 << GAIN_BITS) / 10, GAIN_BITS);
}

void piezo_pcm_set_rate(unsigned int hz)
{
    /* Two one-pole low-pass stages. Approximate 1-exp(-2*pi*3500/hz). */
    unsigned int coeff = (22000u << 8) / (hz + 11000u);
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

    TACMD = 0;
    TACON = TIMER_ECLK_DIV2 | TIMER_PWM | TIMER_MATCH1_EN | TIMER_ACK_ALL;
    TAPRE = 0;
    TADATA0 = PWM_MID;
    TADATA1 = PWM_PERIOD;
    TACMD = 2; /* Clear and load the compare buffers */
    PCON0 = (PCON0 & 0x00ffffff) | 0x53000000;
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

    /* Remove low bass and DC, then limit the duty away from the endpoints. */
    dc += (low2 - dc) >> 3;
    int sample = ((low2 - dc) >> 4) * gain >> GAIN_BITS;
    sample = (sample * PWM_SWING) >> 15;
    TADATA0 = selected ? PWM_MID + MIN(MAX(sample, -PWM_SWING), PWM_SWING)
                       : PWM_MID;
}
