/* PictureFlow slide cache and worker. GPL-2.0-or-later. */
#define PF_ART_PIXELS (DISPLAY_WIDTH * DISPLAY_HEIGHT)
#define PF_DECODE_BYTES (768 * 1024)
#define PF_MAX_SLOTS (2 * MAX_SLIDES_COUNT + 3)

struct pf_slide {
    struct dim size;
    pix_t pixels[PF_ART_PIXELS];
};

struct pf_slot {
    int index;
    struct pf_slide slide;
};

static struct pf_slot *pf_slots;
static int pf_slot_count;
static struct pf_slide pf_empty;
static unsigned char *pf_decode_buffer;
/* The host's signal stack is part of the SDK thread minimum. */
#define PF_WORKER_STACK_BYTES MAX(32 * 1024, DEFAULT_STACK_SIZE + 16 * 1024)
static unsigned long pf_worker_stack[PF_WORKER_STACK_BYTES / sizeof(long)];
static unsigned int pf_worker;
static bool pf_resize;
static volatile bool pf_worker_exit;
static volatile int pf_requested_slide;

static bool pf_art_running(int done, int rows)
{
    (void)done;
    (void)rows;
    return !pf_worker_exit && !rb->rock_worker_stopping();
}

static void draw_splashscreen(void)
{
    rb->lcd_set_background(N_BRIGHT(0));
    rb->lcd_set_foreground(N_BRIGHT(255));
    rb->lcd_clear_display();
    rb->lcd_bitmap(pictureflow_logo, (LCD_WIDTH - BMPWIDTH_pictureflow_logo) / 2,
                   10, BMPWIDTH_pictureflow_logo, BMPHEIGHT_pictureflow_logo);
    rb->lcd_putsxy(10, 55, "Reading music database...");
    rb->lcd_update();
}

static bool pf_cache_init(void)
{
    ALIGN_BUFFER(pf_idx.buf, pf_idx.buf_sz, sizeof(long));
    if (pf_idx.buf_sz < PF_DECODE_BYTES + 3 * sizeof(*pf_slots) + 64 * 1024)
        return false;

    pf_decode_buffer = pf_idx.buf;
    pf_slots = (void *)(pf_decode_buffer + PF_DECODE_BYTES);
    size_t remaining = pf_idx.buf_sz - PF_DECODE_BYTES;
    pf_slot_count = MIN(PF_MAX_SLOTS, (remaining - 64 * 1024) / sizeof(*pf_slots));
    /* A centered window must have distinct modulo slots at both ends. */
    if (!(pf_slot_count & 1))
        pf_slot_count--;
    pf_tracks.names = (char *)(pf_slots + pf_slot_count);
    pf_track_bytes = ALIGN_DOWN(remaining - pf_slot_count * sizeof(*pf_slots),
                                __alignof__(struct track_data));

    const int width = BMPWIDTH_pictureflow_emptyslide;
    const int height = BMPHEIGHT_pictureflow_emptyslide;
    pf_empty.size = (struct dim){width, height};
    for (int x = 0; x < width; x++)
        for (int y = 0; y < height; y++)
            pf_empty.pixels[x * height + y] = LCD_STRIDEFORMAT == VERTICAL_STRIDE
                ? pictureflow_emptyslide[x * height + y]
                : pictureflow_emptyslide[y * width + x];
    return true;
}

static unsigned int mfnv(const char *str)
{
    unsigned int hash = 0x811C9DC5;
    while (*str)
        hash = (hash ^ (unsigned char)*str++) * 16777619;
    return hash;
}

static size_t pf_pixel_bytes(const struct dim *size)
{
    if (size->width < 1 || size->width > DISPLAY_WIDTH ||
        size->height < 1 || size->height > DISPLAY_HEIGHT)
        return 0;
    return (size_t)size->width * size->height * sizeof(pix_t);
}

static bool pf_cache_clear(void)
{
    DIR *directory = rb->opendir(CACHE_PREFIX);
    if (!directory)
        return false;

    bool ok = true;
    struct dirent *entry;
    while ((entry = rb->readdir(directory)) != NULL)
    {
        const char *ext = rb->strchr(entry->d_name, '.');
        if (!ext || (rb->strcmp(ext, ".pfraw") &&
                     rb->strcmp(ext, ".pfraw.tmp")))
            continue;
        char path[MAX_PATH];
        if (rb->snprintf(path, sizeof(path), "%s/%s", CACHE_PREFIX,
                         entry->d_name) >= (int)sizeof(path) ||
            rb->remove(path) < 0)
            ok = false;
    }
    rb->closedir(directory);
    return ok;
}

static void pf_load_slide(int index, struct pf_slot *slot)
{
    unsigned int album = mfnv(get_album_name(index));
    unsigned int artist = mfnv(get_album_artist(index));
    char key[24];
    rb->snprintf(key, sizeof(key), "%08x-%08x-%d", album, artist, pf_resize);

    struct dim size;
    ssize_t cached = rb->rock_pf_cache_read(key, &size, pf_decode_buffer,
                                           PF_ART_PIXELS * sizeof(pix_t));
    bool hit = cached > 0 && (size_t)cached == pf_pixel_bytes(&size);
    if (!hit && pf_art_running(0, 0))
    {
        struct bitmap bitmap = {
            .data = pf_decode_buffer, .width = DISPLAY_WIDTH,
            .height = DISPLAY_HEIGHT,
        };
        int format = FORMAT_NATIVE | FORMAT_KEEP_ASPECT;
        if (pf_resize)
            format |= FORMAT_RESIZE;

        int len = rb->rock_pf_art(&pf_idx.summary, pf_idx.album_index[index].row,
                                  &bitmap, PF_DECODE_BYTES, format,
                                  &format_transposed);
        size = (struct dim){bitmap.width, bitmap.height};
        if (!pf_resize && (len <= 0 || !pf_pixel_bytes(&size)) &&
            pf_art_running(0, 0))
        {
            bitmap.width = DISPLAY_WIDTH;
            bitmap.height = DISPLAY_HEIGHT;
            len = rb->rock_pf_art(&pf_idx.summary, pf_idx.album_index[index].row,
                                  &bitmap, PF_DECODE_BYTES,
                                  format | FORMAT_RESIZE, &format_transposed);
            size = (struct dim){bitmap.width, bitmap.height};
        }
        size_t bytes = pf_pixel_bytes(&size);
        hit = len > 0 && bytes > 0 && pf_art_running(0, 0);
        if (hit)
            rb->rock_pf_cache_write(key, &size, pf_decode_buffer, bytes);
    }

    /* Publish after file I/O. The renderer only keeps a pointer until yield. */
    slot->slide.size = hit ? size : (struct dim){0, 0};
    if (hit)
        rb->memcpy(slot->slide.pixels, pf_decode_buffer, pf_pixel_bytes(&size));
    slot->index = index;
}

static void pf_slide_worker(void *unused)
{
    (void)unused;
    while (pf_art_running(0, 0))
    {
        int center = pf_requested_slide;
        int index = -1;
        for (int distance = 0; distance <= pf_slot_count / 2; distance++)
        {
            int next = center + distance;
            if (next < number_of_slides &&
                pf_slots[next % pf_slot_count].index != next)
            {
                index = next;
                break;
            }
            next = center - distance;
            if (distance && next >= 0 && pf_slots[next % pf_slot_count].index != next)
            {
                index = next;
                break;
            }
        }
        if (index >= 0)
        {
            pf_load_slide(index, &pf_slots[index % pf_slot_count]);
            rb->yield();
        }
        else
            rb->sleep(MAX(1, HZ / 50));
    }
}

static void end_pf_thread(void)
{
    if (pf_worker)
    {
        pf_worker_exit = true;
        rb->rock_worker_join(pf_worker);
        pf_worker = 0;
    }
}

static bool create_pf_thread(void)
{
    for (int i = 0; i < pf_slot_count; i++)
        pf_slots[i].index = -1;
    pf_resize = pf_cfg.resize;
    pf_worker_exit = false;
    pf_requested_slide = center_index;
    pf_worker = rb->rock_worker_start(pf_slide_worker, NULL,
                                      pf_worker_stack, sizeof(pf_worker_stack));
    return pf_worker != 0;
}

static struct dim *surface(int index)
{
    if (index < 0 || index >= number_of_slides)
        return NULL;
    struct pf_slot *slot = &pf_slots[index % pf_slot_count];
    if (slot->index == index && slot->slide.size.width)
        return &slot->slide.size;
    return &pf_empty.size;
}
