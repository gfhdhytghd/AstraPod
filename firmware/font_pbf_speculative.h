/* Idle-page PBF prefetch, private to font_pbf.c. GPL version 2+. */
#ifndef FONT_PBF_SPECULATIVE_H
#define FONT_PBF_SPECULATIVE_H

#define WARM_BYTES (128 * 1024)
#define WARM_SLOTS 256
#define WARM_LIMIT (WARM_SLOTS / 2)

static unsigned warm_generation = 1;
static unsigned warm_used, warm_count, warm_order, warm_cache_ticket;
/* Keep the arena outside the boot image and its BSS. */
static int warm_handle;
static struct {
    unsigned serial, offset;
    ucschar_t codepoint;
} warm_index[WARM_SLOTS];

static bool warm_current(unsigned ticket)
{
    return !ticket ||
           ticket == __atomic_load_n(&warm_generation, __ATOMIC_RELAXED);
}

void font_prefetch_speculative_cancel(void)
{
    if (!schedule_prefetch)
        return;

    mutex_lock(&batch_lock);
    unsigned next = warm_generation + 1;
    __atomic_store_n(&warm_generation, next ? next : 1, __ATOMIC_RELAXED);
    for (int i = 0; i < MAXFONTS; i++)
        if (pending[i].ticket)
            pending[i].count = 0;
    mutex_unlock(&batch_lock);
}

static unsigned warm_slot(unsigned serial, ucschar_t cp)
{
    unsigned slot = (cp ^ (cp >> 8) ^ (serial * 31)) & (WARM_SLOTS - 1);
    while (warm_index[slot].serial &&
           (warm_index[slot].codepoint != cp ||
            warm_index[slot].serial != serial))
        slot = (slot + 1) & (WARM_SLOTS - 1);
    return slot;
}

static const unsigned char *warm_glyph(const struct font *font, ucschar_t cp)
{
    if (!warm_count)
        return NULL;
    unsigned slot = warm_slot(font->pbf.serial, cp);
    const unsigned char *base = warm_index[slot].serial ?
                                core_get_data(warm_handle) : NULL;
    return base ? base + warm_index[slot].offset : NULL;
}

static void warm_promote(struct font_cache_entry *entry, void *context)
{
    const unsigned char *record = context;
    memcpy(entry->bitmap, record, 6 + (u16(record) & 0x7fff) + ALPHA_PADDING);
}

static const unsigned char *warm_glyph_for_draw(struct font *font, ucschar_t cp)
{
    const unsigned char *record = warm_glyph(font, cp);
    if (!record)
        return NULL;

    struct font_cache_entry *entry = font_cache_get(
        &font->cache, cp, false, warm_promote, (void *)record);
    if (!u16(entry->bitmap))
        warm_promote(entry, (void *)record);
    return entry->bitmap;
}

static unsigned warm_record_bytes(const struct pbf_record *record)
{
    unsigned bytes = 6 + (u16(record->header) & 0x7fff) + ALPHA_PADDING;
    return (bytes + 3) & ~3u;
}

static void warm_publish(const struct font *font)
{
    /* Make room at the start of a sweep so siblings cannot displace the
     * selected child. Glyphs used in a paint enter the ordinary cache. */
    unsigned bytes = 0;
    for (unsigned i = 0; i < batch_count; i++)
        bytes += warm_record_bytes(&records[i]);
    if (warm_cache_ticket != job.ticket)
    {
        if (bytes > WARM_BYTES - warm_used ||
            warm_used > WARM_BYTES / 4 || warm_count > WARM_LIMIT / 4)
        {
            memset(warm_index, 0, sizeof(warm_index));
            warm_used = warm_count = 0;
        }
        warm_cache_ticket = job.ticket;
    }

    unsigned char *bits = core_get_data_pinned(warm_handle);
    for (unsigned i = 0; i < batch_count; i++)
    {
        const struct pbf_record *r = &records[i];
        unsigned slot = warm_slot(font->pbf.serial, r->codepoint);
        unsigned size = warm_record_bytes(r);
        if (warm_index[slot].serial || size > WARM_BYTES - warm_used ||
            warm_count == WARM_LIMIT)
            continue;
        copy_record(bits + warm_used, r);
        warm_index[slot].serial = font->pbf.serial;
        warm_index[slot].offset = warm_used;
        warm_index[slot].codepoint = r->codepoint;
        warm_used += size;
        warm_count++;
    }
    core_put_data_pinned(bits);
}

static int next_batch(void)
{
    int speculative = -1;
    for (int offset = 1; offset <= MAXFONTS; offset++)
    {
        int id = (job_font + offset) % MAXFONTS;
        if (!pending[id].count)
            continue;
        if (!pending[id].ticket)
            return id;
        if (speculative < 0 ||
            (int)(pending[id].order - pending[speculative].order) < 0)
            speculative = id;
    }
    return speculative;
}

static bool batch_aborted(void)
{
    return suspended || !warm_current(job.ticket);
}

static bool prefetch_class(struct pbf_request *request, unsigned ticket)
{
    if (!request->count || request->ticket == ticket)
    {
        if (!request->count && ticket)
            request->order = ++warm_order;
        request->ticket = ticket;
        return true;
    }
    if (ticket)
        return false;

    /* Foreground text takes the queue slot for this font. */
    request->ticket = request->count = 0;
    return true;
}

#endif
