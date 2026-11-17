/* FU51: the breadcrumb ring (firmware/kernel/include/crumb_ring.h), compiled
 * unchanged for the host. A "reset" here is simply reading the ring again,
 * after arbitrary damage. No device access. GPL version 2 or later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "crumb_ring.h"

static struct crumb_ring a, b;

static void fill(struct crumb_ring *r, int count, uint16_t id0)
{
    for (int i = 0; i < count; i++)
        crumb_ring_put(r, (uint32_t)(1000 + i), (uint16_t)(id0 + i % 7),
                       (uint32_t)i);
}

int main(void)
{
    struct crumb_snapshot s;
    struct crumb_entry e[32];

    assert(sizeof(struct crumb_ring) <= 2048);

    /* Garbage (never written) is no ring */
    memset(&a, 0xa5, sizeof a);
    memset(&b, 0x00, sizeof b);
    assert(crumb_snapshot_take(&a, &b, &s) == 0 && !s.valid);

    /* A fresh ring has no entries but a valid header */
    crumb_ring_start(&a, 7);
    crumb_ring_start(&b, 7);
    assert(crumb_snapshot_take(&a, &b, &s) == 3 && s.valid && s.n == 0);
    assert(s.boot == 7 && !s.clean);

    /* Wrap several times: the newest 32, oldest first, consecutive */
    fill(&a, 1000, 1);
    fill(&b, 1000, 1);
    assert(crumb_snapshot_take(&a, &b, &s) == 3 && s.n == 32);
    for (int i = 0; i < 32; i++)
    {
        assert(s.e[i].value == (uint32_t)(1000 - 32 + i));
        assert(s.e[i].tick == (uint32_t)(1000 + 1000 - 32 + i));
        if (i)
            assert(s.e[i].seq == s.e[i - 1].seq + 1);
    }

    /* Repeats collapse into one entry with a count */
    crumb_ring_start(&a, 1);
    for (int i = 0; i < 70000; i++)
        crumb_ring_put(&a, (uint32_t)i, 9, 42);
    crumb_ring_put(&a, 70000, 10, 1);
    assert(crumb_ring_newest(&a, e, 32) == 2);
    assert((e[0].id_rep >> 16) == 9 && (e[0].id_rep & 0xffff) == 0xffff);
    assert(e[0].tick == 69999 && (e[1].id_rep >> 16) == 10);
    /* same id, other value: a new entry */
    crumb_ring_put(&a, 70001, 10, 2);
    assert(crumb_ring_newest(&a, e, 32) == 3);

    /* An entry torn by the reset (check written last) is left out, and the
     * run stops there: only what follows it in order is trusted */
    crumb_ring_start(&a, 2);
    fill(&a, 50, 1);
    a.e[(a.seq - 1) % CRUMB_ENTRIES].check ^= 1;     /* newest torn */
    int n = crumb_ring_newest(&a, e, 32);
    assert(n == 32 && e[31].value == 48);
    a.e[(a.seq - 10) % CRUMB_ENTRIES].value ^= 0x100; /* one bit decays */
    n = crumb_ring_newest(&a, e, 32);
    assert(n == 8 && e[0].value == 41 && e[7].value == 48);

    /* The ring's seq lags its newest entry by one: entry still found */
    crumb_ring_start(&a, 3);
    fill(&a, 20, 1);
    a.seq--;
    assert(crumb_ring_newest(&a, e, 32) == 20 && e[19].value == 19);

    /* One copy lost (header broken): the other serves; mask says which */
    crumb_ring_start(&a, 4);
    crumb_ring_start(&b, 4);
    fill(&a, 40, 1);
    fill(&b, 40, 1);
    a.head_check ^= 1;
    assert(crumb_snapshot_take(&a, &b, &s) == 2 && s.valid && s.n == 32);
    a.head_check ^= 1;
    b.magic = 0;
    assert(crumb_snapshot_take(&a, &b, &s) == 1 && s.valid && s.n == 32);

    /* Both valid: the newer entries win; status words come along */
    crumb_ring_start(&a, 5);
    crumb_ring_start(&b, 5);
    fill(&a, 10, 1);
    fill(&b, 12, 1);
    b.status[3] = 0x1234;
    assert(crumb_snapshot_take(&a, &b, &s) == 3 && s.n == 12);
    assert(s.status[3] == 0x1234);

    /* Clean shutdown marker */
    b.clean = CRUMB_CLEAN;
    crumb_snapshot_take(&a, &b, &s);
    assert(s.clean);

    printf("crumb_ring_test: ok\n");
    return 0;
}
