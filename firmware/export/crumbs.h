/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Breadcrumbs that survive a hard reset (FU51). A freeze leaves no screen,
 * so the firmware writes, cheaply and as it goes, what it was doing; the next
 * boot shows the last of it when the previous one did not shut down cleanly,
 * and keeps it in /.rockbox/crumbs-last.txt. Ring format: crumb_ring.h.
 ****************************************************************************/
#ifndef CRUMBS_H
#define CRUMBS_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* Ring entries: rare events, in order. Names in crumbs.c (crumb_names). */
enum crumb_id
{
    CR_NONE = 0,
    CR_BOOT,            /* value: boot number */
    CR_PANIC,           /* value: caller pc */
    CR_PANIC_DRAWN,     /* the panic's first lines reached the LCD */
    CR_HANG,            /* tick watchdog fired; value: stuck pc */
    CR_HANG_DRAWN,
    CR_FREEZE,          /* FIQ watchdog fired; value: interrupted pc */
    CR_FREEZE_DRAWN,
    CR_RESTART,         /* null-call/RESTART trap; value: entry kind */
    CR_RUST_PANIC,
    CR_AGENTS_IN,
    CR_AGENTS_STEP,     /* value: loading step */
    CR_AGENTS_OUT,
    CR_CIDER_IN,
    CR_CIDER_OUT,
    CR_PL_API,          /* UI call into PodLink; value: caller */
    CR_PL_STATE,        /* value: client << 8 | session state */
    CR_PL_WORKER_START,
    CR_USB_EVENT,       /* usb_thread event; value: id */
    CR_USB_RESET,       /* bus reset (ISR) */
    CR_USB_ENUM,        /* enumeration done; value: speed */
    CR_USB_SETUP,       /* value: bmRequestType | bRequest << 8 | wLength << 16 */
    CR_USB_TIMEOUT,     /* bounded USB wait gave up; value: stage */
    CR_USB_DMA_BAD,     /* OUT DMA aimed outside RAM buffers; value: addr */
    CR_DMA_STOP,        /* pl080 channel stopped; value: prio */
    CR_DMA_BAD,         /* pl080 memory destination in core IRAM */
    CR_VECTOR_BAD,      /* vector page changed; value: offset */
    CR_ICODE_BAD,       /* IRAM code block changed; value: block address */
    CR_BANK_BAD,        /* SVC/IRQ banked sp/lr changed; value: which */
    CR_SELFTEST,        /* freeze self-test started; value: kind */
    CR_SCREEN,          /* root screen change; value: screen id */
    CR_TTB_BAD,         /* L1 page table changed; value: address */
    CR_CODEC_LOAD,      /* codec format (AFMT_*) */
    CR_CODEC_RUN,       /* audio handle ID */
    CR_CODEC_DONE,      /* signed decoder return code */
    CR_CODEC_CONFIG,    /* DSP setting in top 8 bits, argument in low 24 */
    CR_CODEC_STRIP,     /* requested logical file size */
    CR_AUDIO_FORMAT,   /* AFMT << 24 | source bits << 16 | (Hz / 10) */
    CR_AUDIO_ROUTE,    /* converted file or SM1 route decision */
    CR_AUDIO_ERROR,    /* signed playback diagnostic error */
    CR_BUF_OPEN,       /* type << 24 | signed handle/result in low 24 */
    CR_BUF_OPEN_AT,    /* requested file start offset */
    CR_BUF_REBUFFER,   /* requested file offset outside the buffer */
    CR_BUF_DAMAGE,     /* rejected list link address */
    CR_SEEK_REQ,       /* requested position in milliseconds */
    CR_SEEK_NOW,       /* seamless seek request, milliseconds */
    CR_SEEK_DONE,      /* resulting playing position in milliseconds */
    CR_LYRICS_SEEK,    /* seek from selected lyrics line, milliseconds */
    CR_LYRICS_REDRAW,  /* screen and seek redraw events */
    CR_MP4_MOOV,       /* moov payload offset */
    CR_MP4_MDAT,       /* nonempty mdat payload offset */
    CR_FREEZE_CONTEXT, /* watchdog kind << 8 | interrupted CPSR low byte */
    CR_HANG_SAMPLE,    /* overdue scheduler: PC interrupted by the tick */
    CR_VIDEO_PHASE,    /* video entry and teardown phases in rock_runtime.c */
    CR_COUNT
};

/* Status words: overwritten in place, read at the next boot */
enum crumb_status
{
    CS_TICK = 0,        /* current_tick, from the tick task */
    CS_IRQ_IN,          /* irq << 24 | entries */
    CS_IRQ_OUT,         /* irq << 24 | exits */
    CS_THREAD,          /* thread switched in: slot << 24 | tick */
    CS_USB_STAGE,       /* USB ISR stage (USB_STAGE_*), 0 outside */
    CS_USB_GINTSTS,     /* USB ISR's last GINTSTS & GINTMSK */
    CS_USB_DMA_OUT,     /* last OUT DMA: addr */
    CS_USB_DMA_IN,      /* last IN DMA: addr */
    CS_USB_THREAD,      /* usb_thread loops << 8 | in-wait flag */
    CS_LCD,             /* lcd_update_rect entries << 1 | inside */
    CS_PL_LOCK,         /* PodLink service lock holder's caller, 0 free */
    CS_PL_WORKER,       /* PodLink worker steps << 1 | inside step */
    CS_PL_IRQ,          /* PodLink IRQ-masked site: site << 1 | inside */
    CS_DMA_Q,           /* last pl080 queue: prio << 24 | bytes */
    CS_DMA_SRC,
    CS_DMA_DST,
    CS_AUDIO_SEEK,      /* audio thread seek phase, 0 outside */
    CS_CODEC,           /* codec callback phase, top byte; setting in low bits */
    CS_CODEC_ARG,       /* codec callback's last argument */
    CS_BUF_OP,          /* buffer operation phase << 24 | handle ID */
    CS_BUF_POS,         /* requested position or relative advance */
    CS_BUF_PTR,         /* last handle address from the seek/advance lookup */
    CS_REBUFFER,        /* buffering thread rebuffer phase, 0 outside */
    CS_LYRICS,          /* lyrics screen phase, 0 outside */
    CS_COUNT
};

enum usb_stage
{
    USB_STAGE_NONE = 0,
    USB_STAGE_ISR,          /* inside usb_dw_irq */
    USB_STAGE_AHB_IDLE,     /* waiting for AHB idle */
    USB_STAGE_FIFO_FLUSH,   /* waiting for a FIFO flush */
    USB_STAGE_EP_DISABLE,   /* waiting for EPDIS */
    USB_STAGE_GONAK,        /* waiting for global OUT NAK */
    USB_STAGE_GINAK,        /* waiting for global IN NAK */
    USB_STAGE_INNAK,        /* waiting for an IN EP's NAK */
    USB_STAGE_EPMIS,        /* token mismatch handling */
    USB_STAGE_SETUP,        /* SETUP handling */
};

#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && CONFIG_CPU == S5L8702 && \
    !defined(BOOTLOADER)
#define HAVE_CRUMBS

/* Uncached DRAM status words, retained across a warm reset. */
extern volatile uint32_t *const crumb_status_a;
extern volatile uint32_t *const crumb_status_extra_a;
#define CRUMB_STATUS_BASE 16

void crumb(enum crumb_id id, uint32_t value);
static inline void crumb_status(enum crumb_status i, uint32_t v)
{
    if (i < CRUMB_STATUS_BASE)
        crumb_status_a[i] = v;
    else
        crumb_status_extra_a[i - CRUMB_STATUS_BASE] = v;
}

/* Crash context saved while the storage and scheduler may be unavailable. */
enum crumb_crash_kind {
    CRUMB_CRASH_PANIC = 1, CRUMB_CRASH_EXCEPTION,
    CRUMB_CRASH_HANG, CRUMB_CRASH_FREEZE,
};
struct crumb_crash_context
{
    uint32_t kind, pc, sp, lr;
    uint32_t exception, status, fault_status, fault_address;
    uint32_t ui_lr, ui_wait, ui_caller;
    uint32_t calls[4];
    char thread[16];
};
/* Commit registers before inspecting memory that may be damaged. */
void crumbs_crash_save(const char *message, const struct crumb_crash_context *context);
void crumbs_crash_record(const char *message, const struct crumb_crash_context *context);

/* Very first thing at boot: takes the previous boot's ring, starts this one */
void crumbs_boot(void);
#ifdef HAVE_IPOD_CRASH_RECOVERY
bool crumbs_recovery_disk(void);
bool crumbs_recovery_hold(void);
#endif
/* Once the disk is up: saves the previous ring if that boot did not shut
 * down cleanly. After boot, with USB and iAP up: shows it until a key or
 * another event (returned) */
void crumbs_report(void);
long crumbs_report_show(void);
/* Before an intended power off, reboot or ROLO */
void crumbs_clean(void);
/* Tick-time integrity checks (vector page, IRAM code, banked registers,
 * page tables) and the MMU guard: high vectors, VA page 0 a read-only zero
 * page, IRAM code read-only. crumbs_guard_off() restores low vectors before ROLO. */
void crumbs_guard_init(void);
void crumbs_guard_off(void);
/* For the debug menu: the previous boot's ring as lines, and the self-tests */
int crumbs_lines(char (**lines)[48]);
void crumbs_freeze_test(int kind);
#else
static inline void crumb(int id, uint32_t value) { (void)id; (void)value; }
static inline void crumb_status(int i, uint32_t v) { (void)i; (void)v; }
static inline void crumbs_boot(void) { }
static inline void crumbs_report(void) { }
static inline long crumbs_report_show(void) { return 0; }
static inline void crumbs_clean(void) { }
static inline void crumbs_guard_init(void) { }
static inline void crumbs_guard_off(void) { }
#endif

#endif /* CRUMBS_H */
