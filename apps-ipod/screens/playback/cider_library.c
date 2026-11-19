/* Paged Cider rows; called under the resident client mutex. GPL v2+. */
#include "config.h"
#include "cider_library.h"
#include "kernel.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define PAGE_SLOTS 3
static struct
{
    struct cider_row rows[CIDER_PAGE_ROWS];
    int offset, count;
    unsigned int request, used;
    bool pending, tracks;
    long requested;
} pages[PAGE_SLOTS];
struct library_query
{
    char route[380];
    int total, wanted;
    bool stale;
};

static struct library_query library_query = {.total = -1};
static struct library_query track_query = {.total = -1};
static uint32_t library_generation;
static unsigned int clock, request_id = 0x10000000;

_Static_assert(sizeof pages + sizeof library_query + sizeof track_query < 112 * 1024,
               "Cider module RAM budget");

static void reset_query(struct library_query *query)
{
    query->total = -1;
    query->wanted = 0;
    query->stale = false;
}

void cider_library_reset(void)
{
    memset(pages, 0, sizeof pages);
    reset_query(&library_query);
    reset_query(&track_query);
}

void cider_library_invalidate(uint32_t generation)
{
    if (!generation || generation == library_generation)
        return;
    cider_library_reset();
    library_generation = generation;
}

static void open_query(struct library_query *query, const char *route)
{
    if (!strcmp(query->route, route))
        return;
    snprintf(query->route, sizeof query->route, "%s", route);
    reset_query(query);
    bool tracks = query == &track_query;
    for (int i = 0; i < PAGE_SLOTS; i++)
        if (pages[i].tracks == tracks)
            memset(&pages[i], 0, sizeof pages[i]);
}

void cider_library_open(const char *route) { open_query(&library_query, route); }
void cider_library_tracks_open(const char *route) { open_query(&track_query, route); }
int cider_library_total(void) { return library_query.total; }
int cider_library_tracks_total(bool *stale)
{
    if (stale)
        *stale = track_query.stale;
    return track_query.total;
}
bool cider_library_stale(void) { return library_query.stale; }
uint32_t cider_library_revision(void) { return library_generation; }

static bool query_row(struct library_query *query, int index, struct cider_row *row)
{
    if (index < 0 || (query->total >= 0 && index >= query->total))
        return false;
    bool tracks = query == &track_query;
    for (int i = 0; i < PAGE_SLOTS; i++)
        if (tracks == pages[i].tracks && index >= pages[i].offset &&
            index < pages[i].offset + pages[i].count)
        {
            *row = pages[i].rows[index - pages[i].offset];
            pages[i].used = ++clock;
            if (!tracks && index >= pages[i].offset + pages[i].count - 8)
                query->wanted = pages[i].offset + pages[i].count;
            return true;
        }
    /* The transport may truncate a CL frame before 50 rows. Ask for the
     * missing index so a short page can never hide the rest of the album. */
    query->wanted = index;
    return false;
}

bool cider_library_row(int index, struct cider_row *row)
{
    return query_row(&library_query, index, row);
}

bool cider_library_track_row(int index, struct cider_row *row)
{
    return query_row(&track_query, index, row);
}

static unsigned int next_query(struct library_query *query, char *out,
                               unsigned int size, uint32_t gen)
{
    int wanted = query->wanted;
    if (!*query->route || !gen || (query->total >= 0 && wanted >= query->total))
        return 0;
    bool tracks = query == &track_query;
    /* Two track pages keep the six visible rows available at a page boundary. */
    int first = tracks ? 1 : 0;
    int end = *track_query.route && !tracks ? 1 : PAGE_SLOTS;
    int oldest = first;
    for (int i = first; i < end; i++)
    {
        if (pages[i].tracks == tracks)
        {
            if (wanted >= pages[i].offset &&
                wanted < pages[i].offset + pages[i].count)
                return 0;
            if (pages[i].pending)
            {
                if (TIME_BEFORE(current_tick, pages[i].requested + 5 * HZ))
                    return 0;
                pages[i].pending = false;
            }
        }
        if (pages[i].used < pages[oldest].used)
            oldest = i;
    }
    pages[oldest].offset = wanted;
    pages[oldest].count = 0;
    pages[oldest].pending = true;
    pages[oldest].tracks = tracks;
    pages[oldest].request = ++request_id;
    pages[oldest].requested = current_tick;
    pages[oldest].used = ++clock;
    return snprintf(out, size, "cq\t%lu\t%u\t%d\t50\t%s", (unsigned long)gen,
                    pages[oldest].request, wanted, query->route);
}

unsigned int cider_library_next(char *out, unsigned int size, uint32_t gen)
{
    unsigned int n = next_query(&track_query, out, size, gen);
    return n ? n : next_query(&library_query, out, size, gen);
}

static bool field(const char **p, const char *end, char *out, unsigned int size)
{
    unsigned int n = 0;
    while (*p < end && **p != '\t' && **p != '\n')
    {
        if (n + 1 < size)
            out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    if (*p == end)
        return false;
    (*p)++;
    return true;
}
bool cider_library_receive(const void *data, unsigned int size,
                           uint32_t generation, uint32_t library)
{
    const char *p = data, *end = p + size;
    char number[32];
    unsigned long v[7];
    if (!field(&p, end, number, sizeof number) || strcmp(number, "CL"))
        return false;
    for (int i = 0; i < 7; i++)
    {
        if (!field(&p, end, number, sizeof number))
            return false;
        /* strtoul accepts signs and saturates on overflow. Wire indices
         * must be unsigned decimal uint32 values on both host and ARM. */
        uint32_t value = 0;
        for (const char *q = number; *q; q++)
        {
            if (*q < '0' || *q > '9' ||
                value > (UINT32_MAX - (unsigned)(*q - '0')) / 10)
                return false;
            value = value * 10 + *q - '0';
        }
        if (!*number) return false;
        v[i] = value;
    }
    if (v[0] != generation || v[2] < library || v[6] > CIDER_PAGE_ROWS ||
        v[3] > 1000000 || v[4] > 1000000 || v[5] > 1 ||
        v[3] > v[4] || v[6] > v[4] - v[3])
        return false;
    int slot = -1;
    for (int i = 0; i < PAGE_SLOTS; i++)
        if (pages[i].pending && pages[i].request == v[1] &&
            (unsigned long)pages[i].offset == v[3])
            slot = i;
    if (slot < 0)
        return false;
    if (library_generation && v[2] != library_generation)
    {
        reset_query(&library_query);
        reset_query(&track_query);
        for (int i = 0; i < PAGE_SLOTS; i++)
            if (i != slot)
                memset(&pages[i], 0, sizeof pages[i]);
    }
    pages[slot].count = 0;
    for (unsigned int i = 0; i < v[6]; i++)
    {
        struct cider_row *r = &pages[slot].rows[i];
        memset(r, 0, sizeof *r);
        if (!field(&p, end, r->id, sizeof r->id) ||
            !field(&p, end, r->title, sizeof r->title) ||
            !field(&p, end, r->subtitle, sizeof r->subtitle) ||
            !field(&p, end, r->art, sizeof r->art))
            return false;
        if (!field(&p, end, number, sizeof number))
            return false;
        r->duration = strtoul(number, NULL, 10);
        if (!field(&p, end, number, sizeof number))
            return false;
        r->count = atoi(number);
        if (!field(&p, end, number, sizeof number))
            return false;
        r->kind = !strcmp(number, "album")      ? CIDER_ROW_ALBUM
                  : !strcmp(number, "artist")   ? CIDER_ROW_ARTIST
                  : !strcmp(number, "playlist") ? CIDER_ROW_PLAYLIST
                  : !strcmp(number, "song")     ? CIDER_ROW_SONG
                                                : CIDER_ROW_MENU;
        if (!field(&p, end, r->route, sizeof r->route) ||
            !field(&p, end, number, sizeof number))
            return false;
        r->bg = strtoul(number, NULL, 16);
        if (!field(&p, end, number, sizeof number))
            return false;
        r->year = atoi(number);
    }
    if (p != end)
        return false;
    library_generation = v[2];
    struct library_query *query = pages[slot].tracks ? &track_query : &library_query;
    query->total = v[4];
    query->stale = v[5] != 0;
    pages[slot].count = v[6];
    pages[slot].pending = false;
    return true;
}
