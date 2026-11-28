/* Paged Cider rows; called under the resident client mutex. GPL v2+. */
#include "config.h"
#include "cider_library.h"
#include "kernel.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define PAGE_SLOTS 3
/* A Rufin page can include a server timeout followed by a fallback fetch. */
#define PAGE_TIMEOUT (8 * HZ)
#define PAGE_LOOKBACK (CIDER_PAGE_ROWS - 9)

struct library_query
{
    char route[380];
    int total, wanted, offset;
    bool stale;
};

static struct library_query library_queries[2] = {{.total = -1}, {.total = -1}};
static struct library_query *library_query = &library_queries[0];
static struct library_query track_query = {.total = -1};
static struct page_slot
{
    struct cider_row rows[CIDER_PAGE_ROWS];
    struct library_query *query;
    int offset, count;
    unsigned int request, used;
    bool pending;
    long requested;
} pages[PAGE_SLOTS];

static uint32_t library_generation;
static unsigned int clock, request_id = 0x10000000;
static bool library_open;

_Static_assert(sizeof pages + sizeof library_queries + sizeof track_query < 112 * 1024,
               "Cider module RAM budget");

static void reset_query(struct library_query *query)
{
    query->total = -1;
    query->wanted = query->offset = 0;
    query->stale = false;
}

static void clear_pages(void)
{
    for (int i = 0; i < PAGE_SLOTS; i++)
        pages[i].query = NULL;
    reset_query(&library_queries[0]);
    reset_query(&library_queries[1]);
    reset_query(&track_query);
}

void cider_library_reset(void)
{
    clear_pages();
    library_generation = 0;
}

void cider_library_invalidate(uint32_t generation)
{
    if (generation != library_generation)
        clear_pages();
    library_generation = generation;
}

static void forget_pages(struct library_query *query, bool pending_only)
{
    for (int i = 0; i < PAGE_SLOTS; i++)
        if (pages[i].query == query && (!pending_only || pages[i].pending))
            pages[i].query = NULL;
}

static void replace_route(struct library_query *query, const char *route)
{
    forget_pages(query, false);
    reset_query(query);
    snprintf(query->route, sizeof query->route, "%s", route);
}

void cider_library_open(const char *route)
{
    library_open = *route != 0;
    if (library_open && !strcmp(library_query->route, route))
        return;
    forget_pages(library_query, true);
    if (!library_open)
        return;

    library_query = library_query == &library_queries[0] ?
                    &library_queries[1] : &library_queries[0];
    if (strcmp(library_query->route, route) || library_query->stale)
        replace_route(library_query, route);
}

void cider_library_tracks_open(const char *route)
{
    if (strcmp(track_query.route, route))
        replace_route(&track_query, route);
}

int cider_library_total(void)
{
    return library_open ? library_query->total : -1;
}

bool cider_library_pending(void)
{
    for (int i = 0; library_open && i < PAGE_SLOTS; i++)
        if (pages[i].query == library_query && pages[i].pending &&
            TIME_BEFORE(current_tick, pages[i].requested + PAGE_TIMEOUT))
            return true;
    return false;
}

int cider_library_tracks_total(bool *stale)
{
    if (stale)
        *stale = track_query.stale;
    return track_query.total;
}
bool cider_library_stale(void) { return library_open && library_query->stale; }
uint32_t cider_library_revision(void) { return library_generation; }

static bool contains(const struct page_slot *page, int index)
{
    return index >= page->offset && index < page->offset + page->count;
}

static bool query_row(struct library_query *query, int index,
                      struct cider_row *row, bool request)
{
    if (index < 0 || (query->total >= 0 && index >= query->total))
        return false;
    int before = -1, after = 1000001;
    for (int i = 0; i < PAGE_SLOTS; i++)
    {
        struct page_slot *page = &pages[i];
        if (page->query != query || !page->count)
            continue;
        if (contains(page, index))
        {
            *row = page->rows[index - page->offset];
            if (request)
            {
                page->used = ++clock;
                int end = page->offset + page->count;
                if (query != &track_query && index >= end - 8 &&
                    (query->wanted == index || query->wanted == end ||
                     contains(page, query->wanted)))
                    query->wanted = query->offset = end;
                else if (query != &track_query && page->offset > 0 &&
                         index < page->offset + 8 &&
                         (query->wanted == index ||
                          query->wanted == page->offset - 1 ||
                          contains(page, query->wanted)))
                {
                    int prev = page->offset - 1;
                    bool have_prev = false;
                    for (int j = 0; j < PAGE_SLOTS; j++)
                        if (pages[j].query == query && pages[j].count &&
                            contains(&pages[j], prev))
                            have_prev = true;
                    if (!have_prev)
                    {
                        query->wanted = prev;
                        query->offset = page->offset >= CIDER_PAGE_ROWS
                                      ? page->offset - CIDER_PAGE_ROWS : 0;
                    }
                }
            }
            return true;
        }
        if (page->offset > index && page->offset < after)
            after = page->offset;
        if (page->offset < index && page->offset + page->count > before)
            before = page->offset + page->count;
    }
    if (request && query->wanted != index)
    {
        query->wanted = index;
        query->offset = index;
        if (query != &track_query && after <= 1000000 &&
            (before < 0 || after - index < index - before))
            query->offset = index > PAGE_LOOKBACK ? index - PAGE_LOOKBACK : 0;
    }
    return false;
}

bool cider_library_row(int index, struct cider_row *row)
{
    return library_open && query_row(library_query, index, row, true);
}

bool cider_library_peek(int index, struct cider_row *row)
{
    return library_open && query_row(library_query, index, row, false);
}

bool cider_library_track_row(int index, struct cider_row *row)
{
    return query_row(&track_query, index, row, true);
}

static unsigned int next_query(struct library_query *query, char *out,
                               unsigned int size, uint32_t gen)
{
    int wanted = query->wanted;
    if (!*query->route || !gen || (query->total >= 0 && wanted >= query->total))
        return 0;

    struct page_slot *victim = NULL, *track_page = NULL, *own_victim = NULL;
    int parent_pages = 0;
    for (int i = 0; i < PAGE_SLOTS; i++)
    {
        struct page_slot *page = &pages[i];
        if (page->query == query)
        {
            if (contains(page, wanted))
                return 0;
            if (page->pending &&
                TIME_BEFORE(current_tick, page->requested + PAGE_TIMEOUT))
                return 0;
            page->pending = false;
            if (!own_victim || page->used < own_victim->used)
                own_victim = page;
        }
        else if (page->query && page->query != &track_query)
            parent_pages++;
        if (page->query == &track_query)
        {
            track_page = page;
            continue;
        }
        if (page->query && page->pending)
            continue;
        if (!victim || (victim->query &&
                        (!page->query || page->used < victim->used)))
            victim = page;
    }
    /* A panel consumes each track fragment before requesting the next one. */
    if (query == &track_query && track_page)
        victim = track_page;
    if (!victim)
        return 0;
    /* Keep the parent's last page when two slots can serve the active view. */
    if (!*track_query.route && parent_pages == 1 && own_victim &&
        victim->query && victim->query != query)
        victim = own_victim;

    unsigned int n = snprintf(out, size, "cq\t%lu\t%u\t%d\t%d\t%s",
                              (unsigned long)gen, ++request_id, query->offset,
                              CIDER_PAGE_ROWS, query->route);
    if (n >= size)
        return 0;
    victim->query = query;
    victim->offset = query->offset;
    victim->count = 0;
    victim->pending = true;
    victim->request = request_id;
    victim->requested = current_tick;
    victim->used = ++clock;
    return n;
}

unsigned int cider_library_next_list(char *out, unsigned int size, uint32_t gen)
{
    return library_open ? next_query(library_query, out, size, gen) : 0;
}

unsigned int cider_library_next_tracks(char *out, unsigned int size, uint32_t gen)
{
    return next_query(&track_query, out, size, gen);
}

unsigned int cider_library_next(char *out, unsigned int size, uint32_t gen)
{
    unsigned int n = cider_library_next_tracks(out, size, gen);
    return n ? n : cider_library_next_list(out, size, gen);
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
    if (v[0] != generation || v[6] > CIDER_PAGE_ROWS ||
        v[3] > 1000000 || v[4] > 1000000 || v[5] > 1)
        return false;
    struct page_slot *page = NULL;
    for (int i = 0; i < PAGE_SLOTS; i++)
        if (pages[i].query && pages[i].pending && pages[i].request == v[1] &&
            (unsigned long)pages[i].offset == v[3])
            page = &pages[i];
    if (!page)
        return false;
    /* Only CG can announce a new generation. Free the request for a retry. */
    if (v[2] != library)
    {
        page->query = NULL;
        return false;
    }
    if (v[3] > v[4] || v[6] > v[4] - v[3])
        return false;
    for (unsigned int i = 0; i < v[6]; i++)
    {
        struct cider_row *r = &page->rows[i];
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
    struct library_query *query = page->query;
    query->total = v[4];
    query->stale = v[5] != 0;
    page->count = v[6];
    page->pending = false;
    int end_index = page->offset + page->count;
    /* A short backward page may stop before the requested row. Continue
     * from its end without sliding the offset back on repeated lookups. */
    if (page->count && query->offset == page->offset &&
        query->wanted >= end_index)
        query->offset = end_index;
    return true;
}
