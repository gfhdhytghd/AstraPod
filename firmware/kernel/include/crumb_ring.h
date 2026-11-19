/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Breadcrumb ring that outlives a hard reset (MENU+SELECT), apart from any
 * hardware so the host test runs this very code (tools/tests/crumb_ring_test.c).
 *
 * The firmware keeps the ring in DRAM that the bootloader and crt0 leave
 * alone. A header and each entry carry their own check words. An entry
 * half written when the reset came fails its check and is left out.
 *
 * Entries: an id, a 32-bit value, the tick, a sequence number, and how many
 * times in a row the same id and value came (a poll loop is one entry).
 * Status words: what is going on right now (the IRQ being served, the USB
 * interrupt's stage, the thread switched in...), raw, overwritten in place.
 ****************************************************************************/
#ifndef CRUMB_RING_H
#define CRUMB_RING_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CRUMB_MAGIC    0x43524d42u  /* "CRMB" */
#define CRUMB_VERSION  1u
#define CRUMB_CLEAN    0x434c454eu  /* "CLEN": shut down on purpose */
#define CRUMB_ENTRIES  96
#define CRUMB_STATUS   16
#define CRUMB_SALT     0x9e3779b9u

struct crumb_entry
{
    uint32_t seq;
    uint32_t tick;
    uint32_t id_rep;    /* id << 16 | repeats (saturating) */
    uint32_t value;
    uint32_t check;
};

struct crumb_ring
{
    uint32_t magic, version, boot, clean;
    uint32_t head_check;
    uint32_t seq;                       /* the next entry's */
    uint32_t status[CRUMB_STATUS];
    struct crumb_entry e[CRUMB_ENTRIES];
};

/* What was found at boot: the newest entries, oldest first */
struct crumb_snapshot
{
    bool valid;
    bool clean;
    uint32_t boot;
    uint32_t status[CRUMB_STATUS];
    int n;
    struct crumb_entry e[32];
};

static inline uint32_t crumb_head_check(uint32_t magic, uint32_t version,
                                        uint32_t boot)
{
    return (magic ^ CRUMB_SALT) + (version << 8) + boot * 0x01000193u;
}

static inline uint32_t crumb_entry_check(uint32_t seq, uint32_t tick,
                                         uint32_t id_rep, uint32_t value)
{
    uint32_t h = CRUMB_SALT ^ seq;
    h = (h ^ tick) * 0x01000193u;
    h = (h ^ id_rep) * 0x01000193u;
    h = (h ^ value) * 0x01000193u;
    return h ^ (h >> 15);
}

/* Starts a new boot's ring; nothing of the old one is kept */
static inline void crumb_ring_start(volatile struct crumb_ring *r,
                                    uint32_t boot)
{
    for (int i = 0; i < CRUMB_ENTRIES; i++)
    {
        r->e[i].seq = 0;
        r->e[i].check = 0;      /* never a valid check for an all-zero entry */
        r->e[i].tick = r->e[i].id_rep = r->e[i].value = 0;
    }
    for (int i = 0; i < CRUMB_STATUS; i++)
        r->status[i] = 0;
    r->seq = 1;
    r->clean = 0;
    r->boot = boot;
    r->version = CRUMB_VERSION;
    r->magic = CRUMB_MAGIC;
    r->head_check = crumb_head_check(CRUMB_MAGIC, CRUMB_VERSION, boot);
}

static inline bool crumb_ring_head_ok(const volatile struct crumb_ring *r)
{
    return r->magic == CRUMB_MAGIC && r->version == CRUMB_VERSION &&
           r->head_check == crumb_head_check(r->magic, r->version, r->boot);
}

static inline bool crumb_entry_ok(const volatile struct crumb_entry *e)
{
    return e->seq != 0 &&
           e->check == crumb_entry_check(e->seq, e->tick, e->id_rep, e->value);
}

/* The caller masks interrupts. The check word is written last, so a reset
 * in between leaves an entry that fails it. */
static inline void crumb_ring_put(volatile struct crumb_ring *r,
                                  uint32_t tick, uint16_t id, uint32_t value)
{
    uint32_t last = r->seq - 1;
    volatile struct crumb_entry *e = &r->e[last % CRUMB_ENTRIES];

    if (last && e->seq == last && (e->id_rep >> 16) == id &&
        e->value == value && crumb_entry_ok(e))
    {
        uint32_t rep = e->id_rep & 0xffff;
        e->check = 0;
        e->tick = tick;
        e->id_rep = (uint32_t)id << 16 | (rep < 0xffff ? rep + 1 : rep);
        e->check = crumb_entry_check(e->seq, tick, e->id_rep, value);
        return;
    }
    uint32_t seq = r->seq;
    e = &r->e[seq % CRUMB_ENTRIES];
    e->check = 0;
    e->seq = seq;
    e->tick = tick;
    e->id_rep = (uint32_t)id << 16;
    e->value = value;
    e->check = crumb_entry_check(seq, tick, e->id_rep, value);
    r->seq = seq + 1;
}

/* The newest valid run of entries, up to max, oldest first. The ring's own
 * seq may lag the newest entry by one (reset between the two writes), so
 * the newest is found from the entries themselves. */
static inline int crumb_ring_newest(const volatile struct crumb_ring *r,
                                    struct crumb_entry *out, int max)
{
    uint32_t top = 0;
    for (int i = 0; i < CRUMB_ENTRIES; i++)
        if (crumb_entry_ok(&r->e[i]) &&
            (uint32_t)i == r->e[i].seq % CRUMB_ENTRIES && r->e[i].seq > top)
            top = r->e[i].seq;
    if (!top)
        return 0;
    int n = 0;
    for (uint32_t s = top; s && n < max && n < CRUMB_ENTRIES; s--)
    {
        const volatile struct crumb_entry *e = &r->e[s % CRUMB_ENTRIES];
        if (!crumb_entry_ok(e) || e->seq != s)
            break;
        n++;
    }
    for (int i = 0; i < n; i++)
    {
        const volatile struct crumb_entry *e =
            &r->e[(top - (uint32_t)(n - 1 - i)) % CRUMB_ENTRIES];
        out[i].seq = e->seq;
        out[i].tick = e->tick;
        out[i].id_rep = e->id_rep;
        out[i].value = e->value;
        out[i].check = e->check;
    }
    return n;
}

/* From either copy, whichever proves itself with the newer entries. Returns
 * a bit mask of the copies whose header was valid (1: a, 2: b). */
static inline int crumb_snapshot_take(const volatile struct crumb_ring *a,
                                      const volatile struct crumb_ring *b,
                                      struct crumb_snapshot *s)
{
    struct crumb_entry ea[32], eb[32];
    int max = (int)(sizeof s->e / sizeof s->e[0]);
    bool oka = crumb_ring_head_ok(a), okb = b && crumb_ring_head_ok(b);
    int na = oka ? crumb_ring_newest(a, ea, max) : 0;
    int nb = okb ? crumb_ring_newest(b, eb, max) : 0;
    const volatile struct crumb_ring *r = NULL;
    const struct crumb_entry *src = NULL;
    int n = 0;

    memset(s, 0, sizeof *s);
    if (oka && (!okb || (na && (!nb || ea[na - 1].seq >= eb[nb - 1].seq))))
        r = a, src = ea, n = na;
    else if (okb)
        r = b, src = eb, n = nb;
    if (r)
    {
        s->valid = true;
        s->clean = r->clean == CRUMB_CLEAN;
        s->boot = r->boot;
        for (int i = 0; i < CRUMB_STATUS; i++)
            s->status[i] = r->status[i];
        s->n = n;
        memcpy(s->e, src, (size_t)n * sizeof *src);
    }
    return (oka ? 1 : 0) | (okb ? 2 : 0);
}

#endif /* CRUMB_RING_H */
