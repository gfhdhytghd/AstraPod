/* On-demand SM1 test. Built only in the isolated research firmware. */
#include "s5l87xx.h"
#include "clocking-s5l8702.h"

#define SM1_REG(offset) (*(volatile uint32_t *)(0x38500000u + (offset)))
#define SM1_TEST_DIR ROCKBOX_DIR "/sm1"
#define SM1_IRQ (1u << 18)
#define SM1_SRAM(type, addr) ((volatile type *)S5L8702_UNCACHED_ADDR(addr))
#define SM1_Y SM1_SRAM(uint16_t, 0x22038000u)
#define SM1_PARAMS SM1_SRAM(uint16_t, 0x2203ff00u)
#define SM1_PACKET_MAX 65535

static uint32_t sm1_test_desc[4] __attribute__((aligned(32)));
/* Bounded storage for the isolated profiling firmware. */
#define SM1_BATCH_FRAMES 8
static uint8_t sm1_packet[SM1_PACKET_MAX + 32] __attribute__((aligned(32)));
#ifdef PODBOX_SM1_DMA_PROBE
#include "sm1_dma_probe.inc"
#else
static int16_t sm1_output[9216] __attribute__((aligned(32)));
#endif
#include "crc32.h"
#include "codecs.h"
#include "sm1_flac_frame.h"
volatile uint8_t sm1_test_remote_status;
volatile bool sm1_stop_requested;
/* Both pinned native images have a single halt instruction for pass 0.
 * After the first cold init, another track only needs its pass-0 data
 * restored and the normal frame rearm. */
static bool sm1_rearm_ready;

static bool sm1_test_wait(volatile uint32_t *reg, uint32_t mask, uint32_t value,
                          unsigned usec)
{
    uint32_t start = USEC_TIMER;
    do {
        if ((*reg & mask) == value)
            return true;
    } while ((uint32_t)(USEC_TIMER - start) < usec);
    return false;
}

static bool sm1_test_claim(void)
{
    SM1_REG(0x400) = 1;
    if (!sm1_test_wait(&SM1_REG(0x400), 3, 1, 10000))
        return false;
    SM1_REG(0x1008) = 1;
    return true;
}

struct sm1_test_section {
    uint8_t kind, pass;
    uint16_t reserved;
    uint32_t address, count;
};

/* Preflight the whole image before touching the DSP address space. */
static bool sm1_test_image(int fd, int pass)
{
    uint32_t header[2];
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, header, sizeof(header)) != sizeof(header) ||
        header[0] != 0x49373158 || (header[1] & 0xffff) != 1)
        return false;

    unsigned sections = header[1] >> 16;
    if (!sections || sections > 64)
        return false;

    for (unsigned i = 0; i < sections; ++i) {
        struct sm1_test_section s;
        if (read(fd, &s, sizeof(s)) != sizeof(s) || s.reserved ||
            (s.pass != 0 && s.pass != 255))
            return false;

        unsigned limit;
        switch (s.kind) {
        case 1: limit = 4096; break;
        case 2: limit = 0x1c00; break;
        case 3: limit = 0x2e80; break;
        case 4: limit = 0x80; break;
        case 5:
            if (s.address || s.count)
                return false;
            continue;
        default:
            return false;
        }
        if (s.kind == 4 && s.address >= 0xfd80)
            s.address -= 0xfd80;
        if (s.address > limit || s.count > limit - s.address)
            return false;

        for (unsigned j = 0; j < s.count; ++j) {
            uint32_t words[4];
            size_t size = s.kind == 1 ? sizeof(words) : sizeof(words[0]);
            if (read(fd, words, size) != (ssize_t)size)
                return false;
            if (s.pass != pass)
                continue;

            unsigned address = s.address + j;
            if (s.kind == 1) {
                for (unsigned lane = 0; lane < 4; ++lane) {
                    volatile uint32_t *program =
                        SM1_SRAM(uint32_t, 0x22020000u + lane * 0x4000u);
                    program[address] = words[lane];
                }
            } else if (s.kind == 2) {
                SM1_SRAM(uint32_t, 0x22030000u)[address] = words[0];
            } else if (s.kind == 3) {
                SM1_Y[address] = words[0];
            } else {
                SM1_PARAMS[address] = words[0];
            }
        }
    }

    unsigned char trailing;
    return read(fd, &trailing, 1) == 0;
}

enum sm1_trace_stage { TRACE_PRE_ARM, TRACE_AFTER_FEED, TRACE_EVENT, TRACE_COUNT };
static struct {
    uint32_t event, pc, status, entry, arm;
} sm1_trace[2][TRACE_COUNT];

static void sm1_test_trace(unsigned frame, enum sm1_trace_stage stage)
{
    if (frame >= ARRAYLEN(sm1_trace))
        return;
    sm1_trace[frame][stage].event = SM1_REG(0xa98);
    sm1_trace[frame][stage].pc = SM1_REG(0xc10);
    sm1_trace[frame][stage].status = SM1_REG(0x114);
    sm1_trace[frame][stage].entry = SM1_REG(0x988);
    sm1_trace[frame][stage].arm = SM1_REG(0x98c);
}

#include "sm1-irq-6g.h"

static bool sm1_test_complete(unsigned timeout, unsigned frame)
{
    bool signaled = sm1_irq_wait(timeout);
    sm1_test_trace(frame, TRACE_EVENT);
    if (!signaled || !sm1_test_claim())
        return false;
    SM1_REG(0xc48) = 4;
    return sm1_test_wait(&SM1_REG(0xa98), 4, 0, 10000);
}

static struct {
    bool irq, g0, g1, gsm, owned, warm;
    unsigned divider, signature, count;
    const char *fault;
} hw;

static bool sm1_begin(int image)
{
#ifdef PODBOX_SM1_DMA_PROBE
    if (probe_quarantined) return false;
#endif
    bool irq_enabled = (VIC0INTENABLE & SM1_IRQ) != 0;
    bool gate0 = (PWRCON(0) & (1u << 2)) == 0;
    bool gate1 = (PWRCON(1) & (1u << 3)) == 0;
    bool gate_sm1 = (PWRCON(0) & (1u << CLOCKGATE_SM1)) == 0;
    sm1_irq_begin();
    clockgate_enable(2, true);
    clockgate_enable(CLOCKGATE_SM1, true);
    clockgate_enable(35, true);
    uint32_t signature = SM1_REG(0xc80);
    uint32_t divider = SM1_REG(0x1000);
    uint32_t pending;
    const bool warm = sm1_rearm_ready;
    sm1_rearm_ready = false;
    hw.irq = irq_enabled; hw.g0 = gate0; hw.g1 = gate1; hw.gsm = gate_sm1;
    hw.divider = divider; hw.signature = signature; hw.warm = warm;
    hw.owned = true; hw.count = 0; hw.fault = NULL;
    if (signature != 0x004e7309) {
        hw.fault = "SM1 signature mismatch";
        return false;
    }
    SM1_REG(0x1000) = 3;
    SM1_REG(0x99c) = 3;
    pending = SM1_REG(0x860);
    SM1_REG(0x860) = pending;
    SM1_REG(0x858) = 7;
    SM1_REG(0xc48) = 4;
    SM1_REG(0x100c) = 0;
    if ((SM1_REG(0x114) & 7) != 0 || (SM1_REG(0xa98) & (1u << 26))) {
        hw.fault = "SM1 was active before loading";
        return false;
    }
    /* Cold state has READY=0. Verify idle SRAM access independently of
     * the post-completion READY handshake before loading an image. */
    uint16_t saved_y = SM1_Y[0x600];
    SM1_Y[0x600] = 0xa55a;
    bool first_pattern = SM1_Y[0x600] == 0xa55a;
    SM1_Y[0x600] = 0x5aa5;
    bool second_pattern = SM1_Y[0x600] == 0x5aa5;
    SM1_Y[0x600] = saved_y;
    if (!first_pattern || !second_pattern) {
        hw.fault = "SM1 idle SRAM is not writable";
        return false;
    }
    if (!sm1_test_wait(&VIC0RAWINTR, SM1_IRQ, 0, 10000) ||
        !sm1_test_wait(&SM1_REG(0xa98), 4, 0, 10000)) {
        hw.fault = "Initial SM1 completion remained asserted";
        return false;
    }

    static const uint32_t mode0[18] = {
        [2] = 0x84006e00, [10] = 0xc600b800,
        [11] = 0x848cba5d, [12] = 0xc013f7fb,
    };
    for (unsigned i = 0; i < 18; ++i)
        SM1_REG(0x938 + i * 4) = mode0[i];

    /* At session end the final completion was already claimed and
     * acknowledged. The idle SRAM readback above establishes access. */
    if (!sm1_test_image(image, 0)) {
        hw.fault = "Unable to load SM1 init";
        return false;
    }

    if (!warm)
    {
        sm1_irq_prepare();
        SM1_REG(0x984) = 0;
    }
    if (!warm && !sm1_test_complete(50000, ARRAYLEN(sm1_trace))) {
        hw.fault = "SM1 init did not complete";
        return false;
    }
    if (!sm1_test_image(image, 255)) {
        hw.fault = "Unable to load SM1 decoder";
        return false;
    }

    memset(sm1_trace, 0, sizeof(sm1_trace));
    return true;
}

static unsigned sm1_last_us;
static bool sm1_packet_output(unsigned bytes, unsigned expected_words, unsigned alac_depth,
                              int32_t *left, int32_t *right)
{
#ifdef PODBOX_SM1_DMA_PROBE
    if (probe_quarantined) return false;
#endif
    unsigned frames = hw.count;
    const bool warm = hw.warm;
    uintptr_t packet = (uintptr_t)sm1_packet & ~0x40000000u;
    uintptr_t desc = (uintptr_t)sm1_test_desc & ~0x40000000u;
    sm1_test_desc[0] = 0;
    sm1_test_desc[1] = ((uint32_t)bytes << 16) | (0x1fu << 7);
    sm1_test_desc[2] = packet | 0x80000000u;
    sm1_test_desc[3] = 0;
    commit_dcache_range(sm1_packet, bytes);
    commit_dcache_range(sm1_test_desc, sizeof(sm1_test_desc));

    SM1_REG(0x100c) = 0;
    SM1_REG(0x1008) = 0;
    SM1_REG(0x400) = 0;
    if (!sm1_test_wait(&SM1_REG(0x400), 3, 0, 10000)) {
        hw.fault = "SM1 handoff timed out";
        return false;
    }

    if (!frames && !warm) {
        SM1_REG(0x988) = 0x100;
        SM1_REG(0x98c) = 1;
        SM1_REG(0x118) = 0;
        SM1_REG(0xa8c) = 4;
    }
    if (frames && (SM1_REG(0xa98) & 4)) {
        SM1_REG(0xc48) = 4;
        if (!sm1_test_wait(&SM1_REG(0xa98), 4, 0, 10000)) {
            hw.fault = "Previous completion remained asserted";
            return false;
        }
    }
    if (!sm1_test_wait(&SM1_REG(0x114), 7, 0, 10000)) {
        hw.fault = "SM1 input DMA is busy";
        return false;
    }

    sm1_test_trace(frames, TRACE_PRE_ARM);
    sm1_irq_prepare();
    uint32_t start = USEC_TIMER;
    SM1_REG(0x100) = desc | 1;
    if (frames || warm)
        SM1_REG(0x98c) = 1;
    sm1_test_trace(frames, TRACE_AFTER_FEED);
    if (!sm1_test_complete(850000, frames)) {
        hw.fault = "SM1 frame did not complete";
        return false;
    }
    unsigned elapsed = USEC_TIMER - start;
    sm1_last_us=elapsed;
    if (elapsed < 10) {
        hw.fault = "SM1 reported completion before frame could decode";
        return false;
    }
    unsigned output_ready = SM1_PARAMS[0x14];
    unsigned output_error = SM1_PARAMS[0x15];
    unsigned filled = SM1_Y[5];
    unsigned length = SM1_Y[6];
    if (!(output_ready & 1) || output_error != 0 || filled != 1 || length != expected_words) {
        hw.fault = "SM1 output slot or status mismatch";
        return false;
    }


    if (alac_depth)
    {
        for (unsigned i = 0; i < length / 2; i++)
        {
            /* ALAC's codec output is signed 29-bit planar PCM. Read both
             * parts under the same SRAM ownership before rearming the slot. */
            unsigned low = 0;
            if (alac_depth == 24)
                low = i < 2048 ? SM1_Y[0x2200 + i] :
                    SM1_SRAM(uint32_t, 0x22030000u)[0x1100 + i - 2048];
            left[i] = (int16_t)SM1_Y[0x80 + 2*i] * 8192 + ((low >> 8) & 255) * 32;
            right[i] = (int16_t)SM1_Y[0x81 + 2*i] * 8192 + (low & 255) * 32;
        }
    }
    else {
#ifdef PODBOX_SM1_DMA_PROBE
        int copied = 0;
        if (probe_playback_frame)
            copied = probe_copy(length);
        if (copied < 0) {
            hw.fault = "SM1 DMA slot quarantined";
            return false;
        }
        if (!copied)
#endif
        for (unsigned i = 0; i < length; ++i)
            sm1_output[i] = SM1_Y[0x80 + i];
    }
    SM1_Y[5] = 0;
    /* The DSP clears its per-frame command word on publishing PCM.
     * Stage the next command while the CPU owns the SRAM. */
    SM1_PARAMS[0] = 7;
    SM1_PARAMS[0x14] = 0;
    SM1_PARAMS[0x15] = 0;
    if (SM1_Y[5] != 0 || SM1_PARAMS[0] != 7) {
        hw.fault = "SM1 slot release did not read back";
        return false;
    }
    SM1_REG(0x100c) = 1;
    ++hw.count;
    return true;
}

static bool sm1_packet_decode(unsigned bytes, unsigned expected_words)
{
    return sm1_packet_output(bytes, expected_words, 0, NULL, NULL);
}

static void sm1_end(void)
{
    if (!hw.owned) return;
    sm1_irq_disarm();
#ifdef PODBOX_SM1_DMA_PROBE
    if (probe_quarantined) return;
#endif
    sm1_rearm_ready = hw.count && !hw.fault;
    if (hw.signature == 0x004e7309) {
        SM1_REG(0xc48) = 4;
        SM1_REG(0x1000) = hw.divider;
    }
    clockgate_enable(35, hw.g1);
    clockgate_enable(CLOCKGATE_SM1, hw.gsm);
    clockgate_enable(2, hw.g0);
    struct sm1_irq_stats irq_stats = sm1_irq_stats();
    debug_log(DEBUG_LOG_PLAYBACK, "SM1 frames=%u IRQ18=%u semaphore=%u tick=%u",
              hw.count, irq_stats.interrupts, irq_stats.semaphore_wakes, irq_stats.tick_wakes);
    if (hw.irq) VIC0INTENABLE = SM1_IRQ;
    hw.owned = false;
}

#include "sm1_codec_route.inc"
