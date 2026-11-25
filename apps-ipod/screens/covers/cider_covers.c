/* Cider album provider for the native carousel. GPL version 2 or later. */
#include "metadata/album_blur.h"
#include "config.h"
#include "carousel.h"
#include "core_alloc.h"
#include "screens/playback/quick_screen.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_library.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "root_menu.h"
#include "input/action.h"
#include "input/agents_shortcut.h"
#include "font.h"
#include "widgets/splash.h"
#include "lang.h"
#include "kernel.h"
#include "system/shutdown.h"
#include "string-extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int resume_index, previous_center;
static int travel = 1;
static bool flicking;
static long moved_at;
static unsigned int revision;
static struct cider_row row_ahead;
static fb_data *surface;
_Static_assert(sizeof *surface == sizeof(uint16_t), "Remote RGB565 art");

static int build(void)
{
    return cider_page_total() > 0 ? SUCCESS : ERROR_NO_ALBUMS;
}
static int count(void)
{
    return cider_page_total();
}
static unsigned int key(int index)
{
    (void)index;
    return 0;
}
static void noop(void)
{
}
static int jump(void)
{
    return center_index;
}
static void initial(const char *file)
{
    (void)file;
    set_current_slide(MAX(0, MIN(count() - 1, resume_index)));
    previous_center = center_index;
    moved_at = current_tick - HZ;
    flicking = false;
}

static bool remember(int index)
{
    struct cider_row row;
    if (!cider_page_row(index, &row))
        return false;
    if (global_status.cover_remote_index == index &&
        !strcmp(global_status.cover_remote_id, row.id))
        return false;
    global_status.cover_remote_index = index;
    snprintf(global_status.cover_remote_id, sizeof(global_status.cover_remote_id), "%s", row.id);
    return true;
}
static bool legacy_panel_labels(const char *route)
{
    if (strncmp(route, "album/t", 7) || strlen(route + 7) != 24)
        return false;
    for (const char *p = route + 7; *p; p++)
        if (!strchr("0123456789abcdef", *p))
            return false;
    return true;
}

static bool panel_request(int index, struct panel_request *request)
{
    struct cider_row album;
    if (!cider_panel_album(index, &album, &request->revision))
        return false;
    request->remote = true;
    snprintf(request->title, sizeof(request->title), "%s", album.title);
    snprintf(request->artist, sizeof(request->artist), "%s", album.subtitle);
    char *plain = strstr(album.route, "?format=panel");
    if (plain)
        *plain = 0;
    else if (legacy_panel_labels(album.route))
    {
        request->legacy_labels = true;
        char *genre = strstr(request->artist, " · ");
        if (genre)
            *genre = 0;
    }
    strlcpy(request->route, album.route, sizeof(request->route));
    request->key = panel_cache_identity(cider_remote_name(), album.id, request->route);
    return true;
}

static void blur_source(int index, struct album_blur_source *source)
{
    struct cider_row row;
    if (cider_page_peek(index, &row) && strlen(row.art) == 64)
        strlcpy(source->remote, row.art, sizeof(source->remote));
}

static int panel_play(int index, int track, const struct panel_request *request,
                      const struct panel_cache_row *row)
{
    if (cider_panel_revision() != request->revision ||
        strncmp(row->route, "play/", sizeof("play/") - 1))
        return CAROUSEL_MENU_STAY;
    resume_index = index;
    return cider_play_in(row->route, request->route, track)
        ? CAROUSEL_PLAY_PENDING : CAROUSEL_MENU_STAY;
}
static bool art(int index, struct bitmap *bitmap)
{
    struct cider_row row;
    if (!cider_page_peek(index, &row))
        return false;
    int dim = CIDER_COVER_DIM;
    if (!cider_copy_art(row.art, "M", surface, CIDER_COVER_SCRATCH_BYTES))
    {
        /* A stored final tier must never be replaced with a blurred XS.
         * read-ahead owns its disk request; keep the native slide pending. */
        if (cider_art_local(row.art, "M")) return false;
        if (cider_copy_art(row.art, "XS", surface, 32 * 32 * sizeof(*surface)))
        {
            /* Convert rows to columns in place before expanding. In column
             * order every source index is at or below its destination. */
            for (int x = 1; x < 32; x++)
                for (int y = 0; y < x; y++)
                {
                    fb_data pixel = surface[x * 32 + y];
                    surface[x * 32 + y] = surface[y * 32 + x];
                    surface[y * 32 + x] = pixel;
                }
            for (int x = dim - 1; x >= 0; x--)
                for (int y = dim - 1; y >= 0; y--)
                    surface[x * dim + y] = surface[(x / 4) * 32 + y / 4];
        }
        else
        {
            fb_data color =
                LCD_RGBPACK(row.bg >> 16, (row.bg >> 8) & 255, row.bg & 255);
            for (int i = 0; i < dim * dim; i++)
                surface[i] = color;
        }
    }
    bitmap->width = bitmap->height = dim;
    bitmap->format = FORMAT_NATIVE;
    bitmap->data = (unsigned char *)surface;
    return true;
}
static void tick(void)
{
    if (center_index != previous_center)
    {
        long elapsed = current_tick - moved_at;
        int distance = center_index - previous_center;
        travel = distance > 0 ? 1 : -1;
        flicking = elapsed > 0 && abs(distance) * HZ >= 3 * elapsed;
        previous_center = center_index;
        moved_at = current_tick;
    }
    else if (!TIME_BEFORE(current_tick, moved_at + HZ / 3))
        flicking = false;
    bool fast = flicking;
    int window[9];
    int n = cider_window(center_index, travel, window);
    const char *visible[9];
    char visible_keys[9][65];
    unsigned int visible_count = 0;
    bool page_needed = false;
    for (int i = 0; i < n; i++)
    {
        struct cider_row row;
        if (window[i] >= count())
            continue;
        /* The center and the first visible miss drive paging. The slide
         * loader and read-ahead inspect whatever is already cached. */
        bool have = i ? cider_page_peek(window[i], &row)
                      : cider_page_row(window[i], &row);
        if (i && !have && !page_needed)
            have = cider_page_row(window[i], &row);
        if (!have)
        {
            page_needed = true;
            continue;
        }
        snprintf(visible_keys[visible_count], sizeof visible_keys[0], "%s",
                 row.art);
        visible[visible_count] = visible_keys[visible_count];
        visible_count++;
    }
    if (visible_count)
    {
        cider_warm_art(visible, MIN(visible_count, 7), "M");
        cider_retain_art(visible, visible_count, "M");
    }
    /* Six M records fit ahead of nine visible covers in the art arena.
     * The measured read bound sizes the window; the benchmark reports the
     * throughput limit independently of this latency window. */
    static char ahead_keys[16][65];
    const char *ahead[16];
    unsigned int nahead = 0;
    for (unsigned int i = 0; i < visible_count; i++)
    {
        snprintf(ahead_keys[nahead], sizeof ahead_keys[0], "%s", visible[i]);
        ahead[nahead] = ahead_keys[nahead]; nahead++;
    }
    unsigned int extra = cider_read_ahead_count(61, visible_count);
    for (unsigned int i = 0; i < extra && nahead < ARRAYLEN(ahead); i++)
    {
        int at = center_index + travel * (5 + i);
        if (at < 0 || at >= count() || !cider_page_peek(at, &row_ahead)) continue;
        snprintf(ahead_keys[nahead], sizeof ahead_keys[0], "%s", row_ahead.art);
        ahead[nahead] = ahead_keys[nahead]; nahead++;
    }
    cider_read_ahead(ahead, nahead, "M");
    const char *xs[5];
    unsigned int missing = 0;
    bool full_requested = false;
    for (unsigned int i = 0; i < nahead; i++)
    {
        const char *art_key = ahead[i];
        if (cider_has_art(art_key, "M")) continue;
        if (missing < ARRAYLEN(xs) && !cider_art_local(art_key, "M") &&
            !cider_has_art(art_key, "XS"))
            xs[missing++] = art_key;
        if (!full_requested && i < visible_count && (!fast || !i))
            full_requested = cider_prefetch_art(art_key, "M");
    }
    if (missing)
        cider_request_xs(xs, missing);
    unsigned int next = cider_art_revision();
    if (revision != next)
    {
        revision = next;
        carousel_provider_changed();
    }
}
static bool caption_at(int index, struct carousel_album_header *text)
{
    struct cider_row row;
    if (!cider_page_peek(index, &row))
        return false;
    strlcpy(text->title, row.title, sizeof(text->title));
    strlcpy(text->artist, row.subtitle, sizeof(text->artist));
    return true;
}

static void text(void)
{
    struct cider_row row;
    if (!cider_page_row(carousel_caption_index(), &row))
    {
        snprintf(row.title, sizeof row.title, "...");
        row.subtitle[0] = 0;
        row.year = 0;
    }
    carousel_album_caption(row.title, row.subtitle, row.year, false, revision);
}
static unsigned int version(int index)
{
    struct cider_row row;
    if (!cider_page_peek(index, &row))
        return 0;
    uint32_t hash = 2166136261u;
    for (const char *p = row.id; *p; p++)
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    for (const char *p = row.art; *p; p++)
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    return (hash << 2) | (cider_has_art(row.art, "M") ? 3
                          : cider_has_art(row.art, "XS") ? 2
                                                         : 1);
}
static const struct carousel_model model = {
    .build_index = build,
    .count = count,
    .art_key = key,
    .jump_prev = jump,
    .jump_next = jump,
    .draw_text = text,
    .caption_at = caption_at,
    .sort_next = noop,
    .sort_prev = noop,
    .set_initial = initial,
    .external = true,
    .read_art = art,
    .art_version = version,
    .blur_source = blur_source,
    .tick = tick,
    .panel_request = panel_request,
    .panel_play = panel_play,
    .panel_play_status = cider_play_status,
    .remember = remember,
};

static int wait_for_library(void)
{
    int action = get_action(CONTEXT_STD, HZ / 10);
    if (action == ACTION_STD_QUICKSCREEN &&
        quick_screen_current(action, NULL) == QUICKSCREEN_IN_USB)
        return GO_TO_ROOT;
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU ||
        agents_nav_pending() != AGENTS_NAV_NONE)
        return GO_TO_PREVIOUS;
    if (default_event_handler(action) == SYS_USB_CONNECTED)
        return GO_TO_ROOT;
    return CAROUSEL_MENU_STAY;
}

/* Keep the parent on screen while fetching the saved album's page. A
 * reordered library is searched one cached page at a time. */
static int restore_album(void)
{
    int total = MIN(count(), 0x7fff);
    resume_index = MAX(0, MIN(total - 1, global_status.cover_remote_index));
    if (!global_status.cover_remote_id[0])
        return CAROUSEL_MENU_STAY;
    int first = resume_index / CIDER_PAGE_ROWS * CIDER_PAGE_ROWS;
    for (int visited = 0; visited < total; visited++)
    {
        int index = (first + visited) % total;
        struct cider_row row;
        long deadline = current_tick + 5 * HZ;
        while (!cider_page_row(index, &row))
        {
            if (!cider_available() || !TIME_BEFORE(current_tick, deadline))
                return CAROUSEL_MENU_STAY;
            int action = wait_for_library();
            if (action != CAROUSEL_MENU_STAY)
                return action;
        }
        if (!strcmp(global_status.cover_remote_id, row.id))
        {
            resume_index = index;
            break;
        }
    }
    return CAROUSEL_MENU_STAY;
}

int cider_covers(void)
{
    carousel_prepare_transition();
    cider_page_open("albums?format=panel");
    long deadline = current_tick + 5 * HZ;
    while (cider_page_total() < 0 && cider_available() &&
           (TIME_BEFORE(current_tick, deadline) || cider_page_pending()))
    {
        int action = wait_for_library();
        if (action != CAROUSEL_MENU_STAY)
        {
            carousel_cancel_transition();
            return action;
        }
    }
    if (cider_page_total() <= 0)
    {
        carousel_cancel_transition();
        if (cider_page_total() == 0)
            splash(HZ, cider_page_stale() ? ID2P(LANG_UNAVAILABLE)
                                          : ID2P(LANG_CAROUSEL_NO_ALBUMS));
        else if (cider_available())
            splash(HZ, ID2P(LANG_TIMEOUT));
        return GO_TO_PREVIOUS;
    }
    int restored = restore_album();
    if (restored != CAROUSEL_MENU_STAY)
    {
        carousel_cancel_transition();
        return restored;
    }
    int scratch = core_allocatable() >= CIDER_COVER_SCRATCH_BYTES
                ? core_alloc(CIDER_COVER_SCRATCH_BYTES) : -1;
    if (scratch <= 0)
    {
        carousel_cancel_transition();
        splash(HZ, ID2P(LANG_OUT_OF_MEMORY));
        return GO_TO_PREVIOUS;
    }
    surface = core_get_data_pinned(scratch);
    revision = cider_art_revision();
    /* carousel_run joins the loader before it returns. Until then the handle
     * stays pinned across cache mutex waits and slide-buffer allocations. */
    int ret = carousel_run(&model, NULL);
    surface = NULL;
    core_unpin(scratch);
    core_free(scratch);
    cider_cancel_art();
    return ret == CAROUSEL_REMOTE_RETURN ? CIDER_COVERS_REMOTE_RETURN : ret;
}
