/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Breadcrumbs that survive a hard reset, and tick-time guards on the
 * memory a stray write would turn into a RESTART (FU51).
 *
 * Two copies of the ring (crumb_ring.h):
 *  - DRAM, in the NOLOAD .persist section before .bss (app.lds). crt0
 *    clears only .bss/.ibss; the bootloader loads rockbox.ipod at the start
 *    of DRAM (a few MiB) and keeps its own .bss from DRAM + 8 MiB on, and the
 *    linker script asserts .persist lies between the two.
 *  - IRAM1, its top 2 KiB. The bootloader runs from IRAM1's start (about
 *    0x2203c600 at most today) and the ROM is not known to use the top. The
 *    firmware never used IRAM1 (its clock was gated off); it stays on now.
 * Crash text uses a separate .persist record and the adjacent 512 bytes of
 * IRAM1, below the ring. Both records include a checksum and a boot number.
 * The bootloader link script protects the reserved tail of IRAM1.
 * Both are written through the uncached alias (+0x40000000), so nothing
 * waits in the D-cache when the reset comes.
 *
 * Guards, from the tick: the vector page and its literals (0x00-0x5f, which
 * the null-call trap also occupies) word for word; the IRAM code a block at
 * a time against checksums taken at boot; the SVC bank, which nothing uses
 * after crt0 (sp = _irqstackend, lr = crt0_boot_lr) and whose change is
 * exactly what the RESTART report finds afterwards; the ABT/UND stacks, the
 * same address. The IRAM code pages are also mapped read-only, so a CPU
 * store there data-aborts and the abort screen names the storing pc; a DMA
 * write is seen by the checksums only.
 ****************************************************************************/
#include <stdio.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "file.h"
#include "panic.h"
#include "rbversion.h"
#include "string-extra.h"
#include "s5l87xx.h"
#include "clocking-s5l8702.h"
#include "usb-designware.h"
#include "crumbs.h"
#include "crumb_ring.h"
#include "crumb_crash.h"

/* The aligned tables in sections of their own, hivec then l2tab: in this
 * file's .bss they page-aligned all of it and left about 4 KiB of holes
 * (FU55). */
#define TABLES_ATTR(n) __attribute__((section(".bss.crumbs_tables" #n)))
/* Coarse tables: [0] VA 0-1 MiB (IRAM), [1] VA 0xfff00000-1 MiB (vectors) */
static uint32_t l2tab[2][256] TABLES_ATTR(1) __attribute__((aligned(1024)));
/* The exception vectors from FU51 on: a DRAM page seen read-only at
 * 0xffff0000 (c1.V), a copy of what crt0 and system_init put at 0. */
static uint32_t hivec[1024] TABLES_ATTR(0) __attribute__((aligned(4096)));

#define UNCACHED(p) ((void *)((uintptr_t)(p) + 0x40000000))

static struct crumb_ring dram_ring __attribute__((section(".persist"),
                                                   aligned(32)));
#define IRAM1_RING_ADDR 0x2203f800
_Static_assert(sizeof(struct crumb_ring) <= 2048, "crumb ring size");

static struct crumb_crash dram_crash __attribute__((section(".persist"), aligned(32)));
#define IRAM1_CRASH_ADDR (IRAM1_RING_ADDR - 512)
_Static_assert(sizeof(struct crumb_crash) <= 512, "crash record size");

#define CRASH_A ((volatile struct crumb_crash *)UNCACHED(&dram_crash))
#define CRASH_B ((volatile struct crumb_crash *)UNCACHED(IRAM1_CRASH_ADDR))
#define RING_A ((volatile struct crumb_ring *)UNCACHED(&dram_ring))
#define RING_B ((volatile struct crumb_ring *)UNCACHED(IRAM1_RING_ADDR))
volatile uint32_t *const crumb_status_a =
    ((volatile struct crumb_ring *)UNCACHED(&dram_ring))->status;
volatile uint32_t *const crumb_status_b =
    ((volatile struct crumb_ring *)UNCACHED(&dram_ring))->status;

volatile uint32_t *const crumb_status_extra_a = RING_A->status_extra;
volatile uint32_t *const crumb_status_extra_b = RING_B->status_extra;
_Static_assert(CS_COUNT == CRUMB_STATUS, "crumb status count");

#ifdef HAVE_IPOD_CRASH_RECOVERY
#include "sm1_recovery.h"
static struct recovery_record recovery_saved __attribute__((section(".persist"), aligned(32)));
#define REC_SAVED ((volatile struct recovery_record *)UNCACHED(&recovery_saved))
static bool recovery_started;

bool crumbs_recovery_disk(void)
{
    return recovery_started && REC_SAVED->phase == REC_DISK;
}

bool crumbs_recovery_hold(void)
{
    return recovery_started && REC_SAVED->phase == REC_HOLD;
}
#endif

static const char *const crumb_names[CR_COUNT] =
{
    [CR_NONE] = "-", [CR_BOOT] = "boot", [CR_PANIC] = "PANIC",
    [CR_PANIC_DRAWN] = "panic-drawn", [CR_HANG] = "HANG",
    [CR_HANG_DRAWN] = "hang-drawn", [CR_FREEZE] = "FREEZE",
    [CR_FREEZE_DRAWN] = "freeze-drawn", [CR_RESTART] = "RESTART",
    [CR_RUST_PANIC] = "rust-panic", [CR_AGENTS_IN] = "agents-in",
    [CR_AGENTS_STEP] = "agents-step", [CR_AGENTS_OUT] = "agents-out",
    [CR_CIDER_IN] = "cider-in", [CR_CIDER_OUT] = "cider-out",
    [CR_PL_API] = "pl-api", [CR_PL_STATE] = "pl-state",
    [CR_PL_WORKER_START] = "pl-worker", [CR_USB_EVENT] = "usb-ev",
    [CR_USB_RESET] = "usb-reset", [CR_USB_ENUM] = "usb-enum",
    [CR_USB_SETUP] = "usb-setup", [CR_USB_TIMEOUT] = "USB-TIMEOUT",
    [CR_USB_DMA_BAD] = "USB-DMA-BAD", [CR_DMA_STOP] = "dma-stop",
    [CR_DMA_BAD] = "DMA-BAD", [CR_VECTOR_BAD] = "VECTOR-BAD",
    [CR_ICODE_BAD] = "ICODE-BAD", [CR_BANK_BAD] = "BANK-BAD",
    [CR_SELFTEST] = "selftest", [CR_SCREEN] = "screen",
    [CR_TTB_BAD] = "TTB-BAD",
    [CR_CODEC_LOAD] = "codec-load", [CR_CODEC_RUN] = "codec-run",
    [CR_CODEC_DONE] = "codec-done", [CR_CODEC_CONFIG] = "codec-cfg",
    [CR_CODEC_STRIP] = "codec-strip", [CR_AUDIO_FORMAT] = "audio-fmt",
    [CR_AUDIO_ROUTE] = "audio-route", [CR_AUDIO_ERROR] = "audio-error",
    [CR_BUF_OPEN] = "buf-open", [CR_BUF_OPEN_AT] = "buf-open-at",
    [CR_BUF_REBUFFER] = "buf-rebuf",
    [CR_BUF_DAMAGE] = "buf-damage", [CR_SEEK_REQ] = "seek",
    [CR_SEEK_NOW] = "seek-now", [CR_SEEK_DONE] = "seek-done",
    [CR_LYRICS_SEEK] = "lyrics-seek", [CR_LYRICS_REDRAW] = "lyrics-draw",
    [CR_MP4_MOOV] = "mp4-moov", [CR_MP4_MDAT] = "mp4-mdat",
    [CR_FREEZE_CONTEXT] = "freeze-ctx",
};

static const char *const status_names[CRUMB_STATUS] =
{
    "tick", "irq-in", "irq-out", "thread", "usb-stage", "usb-gint",
    "usb-dmaout", "usb-dmain", "usb-thr", "lcd", "pl-lock", "pl-worker",
    "pl-irq", "dma-q", "dma-src", "dma-dst",
    "seek-step", "codec-op", "codec-arg", "buf-op", "buf-pos", "buf-ptr",
    "buf-fill", "lyrics",
};

/* The previous boot, taken before this one starts writing */
static struct crumb_snapshot crumbs_prev;
static int crumbs_prev_copies;             /* 1: DRAM header valid, 2: IRAM1 */

void crumb(enum crumb_id id, uint32_t value)
{
    int level = disable_interrupt_save(IRQ_FIQ_STATUS);
    crumb_ring_put(RING_A, current_tick, id, value);
    restore_interrupt(level);
}

extern const char *thread_current_name(void);

static uint32_t crash_save(const char *message,
                           const struct crumb_crash_context *context)
{
    uint32_t boot = crumb_ring_head_ok(RING_A) ? RING_A->boot : 0;
    struct crumb_crash_context saved = *context;
    if (!saved.thread[0])
        strlcpy(saved.thread, thread_current_name(), sizeof saved.thread);
    crumb_crash_store(CRASH_A, boot, RBVERSION, message, &saved);
    return boot;
}

void crumbs_crash_save(const char *message, const struct crumb_crash_context *context)
{
    crash_save(message, context);
}

void crumbs_crash_record(const char *message, const struct crumb_crash_context *context)
{
    uint32_t boot = crash_save(message, context);
#ifdef HAVE_IPOD_CRASH_RECOVERY
    /* No disk, scheduler, I2C or LCD from a fatal context. A recovery
     * fault drops through to the original diagnostic screen and halt. */
    if (recovery_started && recovery_fault(REC_SAVED, boot, (uint32_t)current_tick < 30u * HZ)) {
        uint32_t zero = 0;
        asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(zero) : "memory");
        system_reboot();
    }
#else
    (void)boot;
#endif
}

void crumbs_boot(void)
{
    crumbs_prev_copies = crumb_snapshot_take(RING_A, RING_A, &crumbs_prev);
    uint32_t boot = crumbs_prev.valid ? crumbs_prev.boot + 1 : 1;
#ifdef HAVE_IPOD_CRASH_RECOVERY
    recovery_boot(REC_SAVED, boot, crumbs_prev.valid && !crumbs_prev.clean);
    recovery_started = true;
#endif
    crumb_ring_start(RING_A, boot);
    crumb(CR_BOOT, boot);
}

void crumbs_clean(void)
{
    RING_A->clean = CRUMB_CLEAN;
}

static int format_lines(char (*lines)[48], int max)
{
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(lines[n++], 48, __VA_ARGS__); } while (0)
    if (!crumbs_prev.valid)
    {
        LINE("no breadcrumbs (copies %d)", crumbs_prev_copies);
        return n;
    }
    LINE("boot %lu %s copies:%s%s", (unsigned long)crumbs_prev.boot,
         crumbs_prev.clean ? "clean" : "UNCLEAN",
         crumbs_prev_copies & 1 ? " DRAM" : "", crumbs_prev_copies & 2 ? " IRAM1" : "");
    for (int i = 0; i < CRUMB_STATUS; i += 2)
        LINE("%-9.9s%08lx %-9.9s%08lx", status_names[i],
             (unsigned long)crumbs_prev.status[i], status_names[i + 1],
             (unsigned long)crumbs_prev.status[i + 1]);
    for (int i = 0; i < crumbs_prev.n; i++)
    {
        const struct crumb_entry *e = &crumbs_prev.e[i];
        unsigned id = e->id_rep >> 16, rep = e->id_rep & 0xffff;
        const char *name = id < CR_COUNT && crumb_names[id] ?
                           crumb_names[id] : "?";
        if (rep)
            LINE("%5lu %-11.11s %08lx x%u", (unsigned long)e->tick, name,
                 (unsigned long)e->value, rep + 1);
        else
            LINE("%5lu %-11.11s %08lx", (unsigned long)e->tick, name,
                 (unsigned long)e->value);
    }
#undef LINE
    return n;
}

#define REPORT_LINES (1 + CRUMB_STATUS / 2 + 32)
static char report[REPORT_LINES][48];
static int report_n;

/* The debug menu's view shares the boot report's buffer: same lines */
int crumbs_lines(char (**lines)[48])
{
    *lines = report;
    return format_lines(report, REPORT_LINES);
}

static void crash_report(void)
{
    const volatile struct crumb_crash *crash = crumb_crash_latest(CRASH_A, CRASH_A);
    if (!crash)
        return;

    static const char path[] = ROCKBOX_DIR "/crash-native.txt";
    static const char temp[] = ROCKBOX_DIR "/crash-native.tmp";
    static const char *const kinds[] = {
        [CRUMB_CRASH_PANIC] = "Panic",
        [CRUMB_CRASH_EXCEPTION] = "ARM exception",
        [CRUMB_CRASH_HANG] = "Hang",
        [CRUMB_CRASH_FREEZE] = "Freeze",
    };
    struct crumb_crash_context context = crash->data.context;
    char output[640];
    int length = snprintf(output, sizeof(output),
                          "AstraPod native crash\nFirmware: %s\nBoot: %lu\n"
                          "%s: %s\n"
                          "pc: %08lx  sp: %08lx  lr: %08lx  status: %08lx\n"
                          "exception: %lu  fsr: %08lx  far: %08lx\n"
                          "thread: %.16s\n"
                          "ui lr: %08lx  wait: %08lx  caller: %08lx\n"
                          "stack candidates: %08lx %08lx %08lx %08lx\n",
                          (const char *)crash->data.version,
                          (unsigned long)crash->data.boot,
                          kinds[context.kind],
                          (const char *)crash->data.message,
                          (unsigned long)context.pc, (unsigned long)context.sp,
                          (unsigned long)context.lr, (unsigned long)context.status,
                          (unsigned long)context.exception, (unsigned long)context.fault_status,
                          (unsigned long)context.fault_address, context.thread,
                          (unsigned long)context.ui_lr,
                          (unsigned long)context.ui_wait,
                          (unsigned long)context.ui_caller,
                          (unsigned long)context.calls[0],
                          (unsigned long)context.calls[1],
                          (unsigned long)context.calls[2],
                          (unsigned long)context.calls[3]);
    if (length <= 0 || (size_t)length >= sizeof(output))
        return;

    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    bool ok = write(fd, output, length) == length && fsync(fd) == 0;
    if (close(fd) < 0)
        ok = false;
    /* Keep the previous file and the pending RAM copies on write failure. */
    if (ok && rename(temp, path) == 0)
    {
        CRASH_A->check = 0;
    }
    else
        remove(temp);
}

void crumbs_report(void)
{
    crash_report();
    if (!crumbs_prev.valid || crumbs_prev.clean)
        return;
    int n = report_n = format_lines(report, REPORT_LINES);

    int fd = open(ROCKBOX_DIR "/crumbs-last.txt", O_WRONLY | O_CREAT | O_TRUNC,
                  0666);
    if (fd >= 0)
    {
        fdprintf(fd, "# AstraPod breadcrumbs of the boot before %s\n",
                 RBVERSION);
        for (int i = 0; i < n; i++)
            fdprintf(fd, "%s\n", report[i]);
        close(fd);
    }
}

/* Once boot is done, so USB and the dock's iAP are already up and nothing
 * waits on it (FU55). Summary, status words, then the newest entries that
 * fit, until a key or any other event, which is returned for the caller to
 * handle. */
long crumbs_report_show(void)
{
    int n = report_n;
    if (!n)
        return BUTTON_NONE;
    int font = lcd_getfont();
    lcd_set_viewport(NULL);
    lcd_setfont(FONT_SYSFIXED);
    lcd_clear_display();
    int rows = LCD_HEIGHT / 8, y = 0, head = 1 + CRUMB_STATUS / 2;
    lcd_puts(0, y++, (const unsigned char *)"Last boot did not shut down:");
    for (int i = 0; i < head && i < n; i++)
        lcd_puts(0, y++, (const unsigned char *)report[i]);
    int first = n - (rows - y);
    if (first < head)
        first = head;
    for (int i = first; i < n && y < rows; i++)
        lcd_puts(0, y++, (const unsigned char *)report[i]);
    lcd_update();
    button_clear_queue();
    long ev = button_get_w_tmo(60 * HZ);
    lcd_setfont(font);
    lcd_clear_display();
    lcd_update();
    return ev;
}

/*** Self-tests of the two watchdogs, from the debug menu ***/
static void spin_in_irq(void)
{
    while (1);              /* in the tick IRQ: IRQs masked, FIQ not */
}

void crumbs_freeze_test(int kind)
{
    crumb(CR_SELFTEST, kind);
    switch (kind)
    {
        case 1:             /* an IRQ handler spins: *FREEZE* in 3 s */
            tick_add_task(spin_in_irq);
            break;
        case 2:             /* a thread spins with IRQs masked: *FREEZE* */
            disable_irq();
            while (1);
        case 3:             /* a thread spins with IRQs on: *HANG* in 10 s */
            while (1);
        case 4:             /* IRQ+FIQ masked: no screen; hard reset, and the
                             * next boot must show this test's crumbs */
            disable_interrupt(IRQ_FIQ_STATUS);
            while (1);
    }
}

/*** Guards ***/
extern const char crt0_boot_lr[], _irqstackend[];
extern char _icodestart[], _icodeend[];

#define VEC_WORDS (0x60 / 4)
static uint32_t vec_ref[VEC_WORDS];
#define ICODE_BLOCK 256
#define ICODE_BLOCKS (48 * 1024 / ICODE_BLOCK)
static uint32_t icode_sum[ICODE_BLOCKS];
static unsigned icode_blocks, icode_next;
/* The L1 page table, at the very end of DRAM right after the plugin buffer:
 * a stray write there remaps the CPU's view (vectors, code) at the next TLB
 * miss. Read uncached, as the table walk sees it. */
#define TTB_BLOCKS (TTB_SIZE / ICODE_BLOCK)
static uint32_t ttb_sum[TTB_BLOCKS];
static unsigned ttb_next;
static bool guard_on;

static uint32_t block_sum(const uint32_t *p)
{
    uint32_t h = 0;
    for (int i = 0; i < ICODE_BLOCK / 4; i++)
        h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* Banked sp and lr of a mode, read with IRQ+FIQ masked */
static void bank(uint32_t mode, uint32_t *sp, uint32_t *lr)
{
    uint32_t s, l, saved;
    asm volatile ("mrs   %2, cpsr        \n"
                  "msr   cpsr_c, %3      \n"
                  "mov   %0, sp          \n"
                  "mov   %1, lr          \n"
                  "msr   cpsr_c, %2      \n"
                  : "=&r" (s), "=&r" (l), "=&r" (saved)
                  : "r" (mode | 0xc0));
    *sp = s;
    *lr = l;
}

static bool gate_on(int gate)
{
    return !(PWRCON(gate >> 5) & (1u << (gate & 31)));
}

/* What the DMA masters were last told, for a guard's panic */
static void dma_text(char *buf, size_t size)
{
    size_t n = 0;
    if (gate_on(CLOCKGATE_USBOTG))
        n += snprintf(buf + n, size - n, " usb o:%lx,%lx,%lx i:%lx,%lx",
                      (unsigned long)DWC_DOEPDMA(0),
                      (unsigned long)DWC_DOEPDMA(1),
                      (unsigned long)DWC_DOEPDMA(2),
                      (unsigned long)DWC_DIEPDMA(0),
                      (unsigned long)DWC_DIEPDMA(1));
    for (int ch = 0; ch < 8 && n < size; ch++)
    {
        volatile uint32_t *c = (volatile uint32_t *)(DMA0_BASE + 0x100 +
                                                     ch * 0x20);
        if (c[4] & 1)       /* enabled channel: src, dst */
            n += snprintf(buf + n, size - n, " d%d:%lx>%lx", ch,
                          (unsigned long)c[0], (unsigned long)c[1]);
    }
    if (n < size)
        snprintf(buf + n, size - n, " q:%lx s:%lx d:%lx",
                 (unsigned long)crumb_status_a[CS_DMA_Q],
                 (unsigned long)crumb_status_a[CS_DMA_SRC],
                 (unsigned long)crumb_status_a[CS_DMA_DST]);
}

static char guard_buf[160];
#define L2_BLOCKS (sizeof l2tab / ICODE_BLOCK)
static uint32_t l2_sum[L2_BLOCKS];
#define HIGH_VECTORS 0xffff0000u
static uint32_t low_sect;       /* L1[0] as memory_init set it */
static uint32_t low_ref[VEC_WORDS];

/* FU53: VA page 0 maps, read-only, the old low vector page (PA 0), which the
 * high vectors leave unused. It is zeroed, so a NULL read returns 0 rather
 * than a fault (FU51) or a vector word (upstream), and a store still aborts.
 * ARMv5 has no execute-never: a call into the page runs its zero words, each
 * "andeq r0, r0, r0", which changes nothing, up to the last two -- a jump to
 * null_call_trap, which reports "NULL call" with the caller's lr intact. */
#define LOW_WORDS 1024
#define LOW_TRAP_INSN 0xe51ff004u       /* ldr pc, [pc, #-4]: the next word */
extern void null_call_trap(void);
static uint32_t low_word(unsigned i)
{
    return i == LOW_WORDS - 2 ? LOW_TRAP_INSN :
           i == LOW_WORDS - 1 ? (uint32_t)null_call_trap : 0;
}
uint32_t crumbs_ro_end;         /* first writable byte (for the ARM check) */

static void guard_tick(void)
{
    crumb_status(CS_TICK, current_tick);
    if (!guard_on)
        return;

    /* The live vectors (high page, CPU view), and the old low page through
     * its uncached alias: no CPU store reaches it any more (read-only), so
     * a change there is a device DMA aimed at address 0. */
    const volatile uint32_t *vec = (const volatile uint32_t *)
        (crumbs_ro_end ? HIGH_VECTORS : 0x40000000u);
    const volatile uint32_t *low = (const volatile uint32_t *)0x40000000u;
    for (int i = 0; i < VEC_WORDS; i++)
        if (vec[i] != vec_ref[i] || low[i] != low_ref[i])
        {
            bool hi = vec[i] != vec_ref[i];
            guard_on = false;
            crumb(CR_VECTOR_BAD, i * 4 | (hi ? 0 : 0x80000000u));
            dma_text(guard_buf, sizeof guard_buf);
            if (hi)
                panicf("VECTOR +%02x %08lx->%08lx%s", i * 4,
                       (unsigned long)vec_ref[i], (unsigned long)vec[i],
                       guard_buf);
            panicf("VECTOR PA0 +%02x %08lx->%08lx%s", i * 4,
                   (unsigned long)low_ref[i], (unsigned long)low[i],
                   guard_buf);
        }
    if (crumbs_ro_end)
    {
        uint32_t c1;
        asm volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (c1));
        if (!(c1 & (1u << 13)))
        {
            guard_on = false;
            crumb(CR_VECTOR_BAD, 0x40000000u);
            panicf("VECTOR high vectors off c1:%lx", (unsigned long)c1);
        }
    }

    uint32_t sp, lr;
    bank(0x13, &sp, &lr);
    bool svc = sp == (uint32_t)_irqstackend && lr == (uint32_t)crt0_boot_lr;
    uint32_t asp, alr, usp, ulr;
    bank(0x17, &asp, &alr);
    bank(0x1b, &usp, &ulr);
    if (!svc || asp != (uint32_t)_irqstackend || usp != (uint32_t)_irqstackend)
    {
        guard_on = false;
        crumb(CR_BANK_BAD, svc ? asp : sp);
        panicf("BANK svc:%lx,%lx abt:%lx,%lx und:%lx,%lx",
               (unsigned long)sp, (unsigned long)lr, (unsigned long)asp,
               (unsigned long)alr, (unsigned long)usp, (unsigned long)ulr);
    }

    if (icode_blocks)
    {
        unsigned b = icode_next;
        const uint32_t *p = (const uint32_t *)(_icodestart + b * ICODE_BLOCK);
        if (block_sum(p) != icode_sum[b])
        {
            guard_on = false;
            crumb(CR_ICODE_BAD, (uint32_t)p);
            dma_text(guard_buf, sizeof guard_buf);
            panicf("ICODE %08lx..+%x changed%s", (unsigned long)p,
                   ICODE_BLOCK, guard_buf);
        }
        icode_next = b + 1 < icode_blocks ? b + 1 : 0;
    }

    unsigned t = ttb_next;
    const uint32_t *q = (const uint32_t *)UNCACHED(TTB_BASE_ADDR +
                                                   t * ICODE_BLOCK);
    if (block_sum(q) != ttb_sum[t])
    {
        guard_on = false;
        crumb(CR_TTB_BAD, (uint32_t)TTB_BASE_ADDR + t * ICODE_BLOCK);
        uint32_t va = t * (ICODE_BLOCK / 4) << 20;
        dma_text(guard_buf, sizeof guard_buf);
        panicf("TTB VA %08lx..+%dM changed%s", (unsigned long)va,
               ICODE_BLOCK / 4, guard_buf);
    }
    ttb_next = t + 1 < TTB_BLOCKS ? t + 1 : 0;

    if (crumbs_ro_end)
    {
        /* The rest of the zero page, a block per tick: what NULL reads see */
        unsigned w = t % (LOW_WORDS * 4 / ICODE_BLOCK) * (ICODE_BLOCK / 4);
        for (unsigned i = w; i < w + ICODE_BLOCK / 4; i++)
            if (low[i] != low_word(i))
            {
                guard_on = false;
                crumb(CR_VECTOR_BAD, i * 4 | 0x80000000u);
                dma_text(guard_buf, sizeof guard_buf);
                panicf("VECTOR PA0 +%02x %08lx->%08lx%s", i * 4,
                       (unsigned long)low_word(i), (unsigned long)low[i],
                       guard_buf);
            }

        unsigned k = t % L2_BLOCKS;
        const uint32_t *l2 = (const uint32_t *)UNCACHED(l2tab) +
                             k * (ICODE_BLOCK / 4);
        if (block_sum(l2) != l2_sum[k])
        {
            guard_on = false;
            crumb(CR_TTB_BAD, (uint32_t)l2);
            panicf("L2 table %08lx changed", (unsigned long)l2);
        }
    }
}

/* Two coarse tables in domain 1 (client); domain 0, every other section,
 * stays manager.
 * - VA 0-1 MiB (IRAM): page 0 read-only and zeroed (see low_word(): NULL
 *   reads return 0, NULL stores abort with the culprit's pc, NULL calls end
 *   in null_call_trap), the IRAM code pages read-only (AP=00 with c1.R), the
 *   rest privileged read/write as before.
 * - VA 0xffff0000: the vector page, read-only; c1.V makes it the live one.
 * crumbs_guard_off() undoes it for ROLO. */
static void guard_mmu(void)
{
    uint32_t ro_end = (uint32_t)_icodeend & ~0xfffu;
    volatile uint32_t *l1 = (volatile uint32_t *)TTB_BASE_ADDR;
    uint32_t sect = l1[0], dram = l1[(uint32_t)hivec >> 20];

    /* Only over the mapping this firmware set: a section VA 0 -> PA 0, IRAM
     * code from page 1 on (app.lds), nothing at the top section */
    if ((sect & 3) != 2 || (sect >> 20) != 0 || (dram & 3) != 2 ||
        (uint32_t)_icodestart < 0x1000 || ro_end <= 0x1000 ||
        (l1[0xfff] & 3) != 0)
        return;
    for (unsigned i = 0; i < 256; i++)
        l2tab[0][i] = (i << 12) | (sect & 0xc) |
                      (i * 4096 < ro_end ? 0x000 : 0x550) | 2;
    for (int i = 0; i < VEC_WORDS; i++)
        hivec[i] = vec_ref[i];
    l2tab[1][(HIGH_VECTORS >> 12) & 0xff] =
        (uint32_t)hivec | (dram & 0xc) | 0x000 | 2;     /* read-only */
    commit_dcache_range(hivec, sizeof hivec);
    commit_dcache_range(l2tab, sizeof l2tab);
    for (unsigned b = 0; b < L2_BLOCKS; b++)
        l2_sum[b] = block_sum((const uint32_t *)l2tab + b * (ICODE_BLOCK / 4));

    int level = disable_interrupt_save(IRQ_FIQ_STATUS);
    uint32_t c3, c1;
    asm volatile ("mrc p15, 0, %0, c3, c0, 0" : "=r" (c3));
    c3 = (c3 & ~0xcu) | 0x4u;                   /* domain 1: client */
    asm volatile ("mcr p15, 0, %0, c3, c0, 0" : : "r" (c3));
    asm volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (c1));
    c1 |= 1u << 9;                              /* R: AP=00 reads */
    asm volatile ("mcr p15, 0, %0, c1, c0, 0" : : "r" (c1));
    low_sect = sect;
    l1[0xfff] = (uint32_t)l2tab[1] | (1u << 5) | (1u << 4) | 1u;
    commit_dcache_range((const void *)&l1[0xfff], 4);
    asm volatile ("mcr p15, 0, %0, c8, c7, 0  \n"  /* invalidate TLBs */
                  "mcr p15, 0, %0, c7, c10, 4 \n"  /* drain write buffer */
                  : : "r" (0));
    c1 |= 1u << 13;                             /* V: high vectors */
    asm volatile ("mcr p15, 0, %0, c1, c0, 0" : : "r" (c1));
    /* The low vectors are dead from here: page 0 becomes the zero page,
     * written while VA 0 is still the writable section */
    volatile uint32_t *page0;
    asm volatile ("mov %0, #0" : "=r" (page0)); /* not a C null pointer */
    for (unsigned i = 0; i < LOW_WORDS; i++)
        page0[i] = low_word(i);
    for (int i = 0; i < VEC_WORDS; i++)
        low_ref[i] = low_word(i);
    commit_dcache_range((const void *)page0, LOW_WORDS * 4);
    l1[0] = (uint32_t)l2tab[0] | (1u << 5) | (1u << 4) | 1u;
    commit_dcache_range((const void *)l1, 4);
    asm volatile ("mcr p15, 0, %0, c8, c7, 0  \n"
                  "mcr p15, 0, %0, c7, c10, 4 \n"
                  "mcr p15, 0, %0, c7, c5, 0  \n"  /* invalidate I-cache */
                  : : "r" (0));
    crumbs_ro_end = ro_end;
    restore_interrupt(level);
}

/* Before ROLO: the next image (any version) expects low vectors and VA 0
 * mapped as a section; its crt0 may not clear c1.V. */
void crumbs_guard_off(void)
{
    if (!crumbs_ro_end)
        return;
    int level = disable_interrupt_save(IRQ_FIQ_STATUS);
    volatile uint32_t *l1 = (volatile uint32_t *)TTB_BASE_ADDR;
    guard_on = false;
    l1[0] = low_sect;                           /* vectors at 0 again */
    commit_dcache_range((const void *)l1, 4);
    asm volatile ("mcr p15, 0, %0, c8, c7, 0  \n"
                  "mcr p15, 0, %0, c7, c10, 4 \n"
                  : : "r" (0));
    volatile uint32_t *page0;                   /* the zero page held them */
    asm volatile ("mov %0, #0" : "=r" (page0));
    for (int i = 0; i < VEC_WORDS; i++)
        page0[i] = low_ref[i] = vec_ref[i];
    commit_dcache_range((const void *)page0, VEC_WORDS * 4);
    asm volatile ("mcr p15, 0, %0, c7, c5, 0" : : "r" (0)); /* I-cache */
    uint32_t c1;
    asm volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (c1));
    c1 &= ~(1u << 13);
    asm volatile ("mcr p15, 0, %0, c1, c0, 0" : : "r" (c1));
    l1[0xfff] = 0;
    commit_dcache_range((const void *)&l1[0xfff], 4);
    asm volatile ("mcr p15, 0, %0, c8, c7, 0  \n"
                  "mcr p15, 0, %0, c7, c10, 4 \n"
                  : : "r" (0));
    crumbs_ro_end = 0;
    restore_interrupt(level);
}

void crumbs_guard_init(void)
{
    /* What crt0 and system_init (null-call trap) put at 0, read through the
     * uncached alias of PA 0 */
    const volatile uint32_t *low = (const volatile uint32_t *)0x40000000u;
    for (int i = 0; i < VEC_WORDS; i++)
        low_ref[i] = vec_ref[i] = low[i];
    unsigned blocks = (unsigned)(_icodeend - _icodestart) / ICODE_BLOCK;
    if (blocks > ICODE_BLOCKS)
        blocks = ICODE_BLOCKS;
    for (unsigned b = 0; b < blocks; b++)
        icode_sum[b] = block_sum((const uint32_t *)(_icodestart +
                                                    b * ICODE_BLOCK));
    icode_blocks = blocks;
    guard_mmu();
    for (unsigned t = 0; t < TTB_BLOCKS; t++)
        ttb_sum[t] = block_sum((const uint32_t *)UNCACHED(TTB_BASE_ADDR +
                                                          t * ICODE_BLOCK));
    guard_on = true;
    tick_add_task(guard_tick);
}
