/* Cider album provider for the native carousel. GPL version 2 or later. */
#include "metadata/album_blur.h"
#include "config.h"
#include "carousel.h"
#include "album_covers.h"
#include "panel_perf.h"
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
#include "system/hash.h"
#include "string-extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int resume_index, previous_center;
static int travel = 1;
static bool flicking;
static int first_track_album = -1;
static long first_track_deadline;
static struct panel_request first_track_request;
static long moved_at, prefetch_at;
static unsigned int revision;
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
    first_track_album = -1;
    set_current_slide(MAX(0, MIN(count() - 1, resume_index)));
    previous_center = center_index;
    moved_at = current_tick - HZ;
    prefetch_at = current_tick;
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
    request->artwork = panel_cache_identity(album.art, "", "");
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

/* A remote album can use an opaque route. Ask its first track to play in
 * that album's context, just like selecting row zero in the open panel. */
static int play_album(int index)
{
    memset(&first_track_request, 0, sizeof(first_track_request));
    first_track_request.locale = fnv1a_str(global_settings.lang_file);
    if (!panel_request(index, &first_track_request))
        return CAROUSEL_MENU_STAY;
    first_track_album = index;
    first_track_deadline = current_tick + 6 * HZ;
    panel_cache_window(&first_track_request, 1);
    return CAROUSEL_PLAY_PENDING;
}

static int play_status(void)
{
    if (first_track_album < 0)
        return cider_play_status();
    if (!cider_available() || cider_panel_revision() != first_track_request.revision ||
        TIME_AFTER(current_tick, first_track_deadline))
    {
        first_track_album = -1;
        return -1;
    }
    const struct panel_cached *page = panel_cache_acquire(&first_track_request, 0);
    if (!page)
    {
        panel_cache_pump();
        return 0;
    }
    const struct panel_cache_row *row = panel_cache_row(page, 0);
    int result = row ? panel_play(first_track_album, 0, &first_track_request, row)
                     : CAROUSEL_MENU_STAY;
    panel_cache_release(page);
    first_track_album = -1;
    return result == CAROUSEL_PLAY_PENDING ? 0 : -1;
}

static unsigned int cover_version(const struct cider_row *row, unsigned int quality)
{
    return (fnv1a_str(*row->art ? row->art : row->id) << 2) | quality;
}

static bool art(int index, struct bitmap *bitmap, unsigned int *version)
{
    struct cider_row row = {0};
    bool known = cider_page_peek(index, &row);
    if (!known && abs(index - center_index) > ALBUM_COVERS_NUM_SLIDES)
        return false;
    int dim = CIDER_COVER_DIM;
    unsigned int quality = 3;
    if (!known || !cider_copy_art(row.art, "M", surface, CIDER_COVER_SCRATCH_BYTES))
    {
        if (known && cider_copy_cover_preview(row.art, (uint16_t *)surface))
        {
            quality = 2;
            /* Convert rows to columns in place before expanding. In column
             * order every source index is at or below its destination. */
            for (int x = 1; x < 32; x++)
                for (int y = 0; y < x; y++)
                {
                    fb_data pixel = surface[x * 32 + y];
                    surface[x * 32 + y] = surface[y * 32 + x];
                    surface[y * 32 + x] = pixel;
                }
            if (global_settings.album_covers_view_mode == CAROUSEL_VIEW_FLAT)
                for (int x = dim - 1; x >= 0; x--)
                    for (int y = dim - 1; y >= 0; y--)
                        surface[x * dim + y] = surface[(x / 4) * 32 + y / 4];
            else
                dim = 32;
        }
        else
        {
            quality = 1;
            /* A page has no palette until its artwork arrives on some
             * providers. Keep the card visible during the fetch. */
            uint32_t rgb = row.bg ? row.bg : 0x383838;
            fb_data color = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
            if (!color)
                color = LCD_RGBPACK(56, 56, 56);
            for (int i = 0; i < dim * dim; i++)
                surface[i] = color;
        }
    }
    *version = known ? cover_version(&row, quality) : 0;
    if (quality == 3)
        panel_perf_record(PP_ART_RAM, 0, *version >> 2, CS_M);
    bitmap->width = bitmap->height = dim;
    bitmap->format = FORMAT_NATIVE;
    bitmap->data = (unsigned char *)surface;
    return true;
}
static void tick(void)
{
    unsigned next = cider_art_revision();
    bool moved = center_index != previous_center;
    if (!moved && next == revision && TIME_BEFORE(current_tick, prefetch_at))
        return;
    prefetch_at = current_tick + MAX(1, HZ / 20);
    if (moved)
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
    enum { INNER = 4, NEAR = ALBUM_COVERS_NUM_SLIDES, REACH = NEAR + CIDER_COVER_LEAD,
           WINDOW = 2 * NEAR + 1 + CIDER_COVER_LEAD };
    int window[WINDOW];
    int n = cider_window(center_index, travel, window);
    int inner = n;
    for (int d = INNER + 1; d <= NEAR; d++)
    {
        window[n++] = center_index + travel * d;
        window[n++] = center_index - travel * d;
    }
    for (int d = NEAR + 1; d <= REACH; d++)
        window[n++] = center_index + travel * d;

    static char keys[WINDOW][65];
    const char *ahead[WINDOW], *warm[7];
    int offsets[WINDOW];
    bool host_miss[WINDOW] = {0};
    unsigned total = 0, visible = 0, nwarm = 0;
    int demand = -1;
    int albums = count();
    for (int i = 0; i < n; i++)
    {
        int at = window[i];
        if (at < 0 || at >= albums)
            continue;
        struct cider_row row;
        if (!cider_page_peek(at, &row))
        {
            if (demand < 0)
                demand = at;
            continue;
        }
        if (strlen(row.art) != 64)
            continue;
        memcpy(keys[total], row.art, sizeof keys[0]);
        ahead[total] = keys[total];
        offsets[total] = (at - center_index) * travel;
        total++;
        visible += i < inner;
    }
    if (demand >= 0)
    {
        struct cider_row row;
        cider_page_row(demand, &row);
    }

    /* Full covers share the same arena as the artwork for Now Playing.
     * Use the disk worker's budget for M and keep a small XS lead beyond it. */
    unsigned full = MIN(total, visible + cider_read_ahead_count(61, visible));
    cider_cover_read_ahead(ahead, total, full);
    bool requested = false, preview_needed = false;
    unsigned order[WINDOW], ordered = 0;
    for (unsigned i = 0; i < total; i++)
        if (offsets[i] >= 0)
            order[ordered++] = i;
    for (unsigned i = 0; i < total; i++)
        if (offsets[i] < 0)
            order[ordered++] = i;
    for (unsigned pos = 0; pos < ordered; pos++)
    {
        unsigned i = order[pos];
        const char *key = ahead[i];
        if (cider_has_art(key, "M"))
            continue;
        bool local = cider_art_local(key, "M");
        host_miss[i] = !local;
        if (!local && !cider_cover_preview_local(key))
        {
            preview_needed = true;
            if (!requested)
                requested = cider_prefetch_art(key, "XS");
        }
        if (!local && nwarm < ARRAYLEN(warm))
            warm[nwarm++] = key;
    }
    /* Keep one foreground request alive across wheel steps. Short XS
     * transfers fill holes before any 32 KiB medium image uses the link. */
    if (!requested && !preview_needed && !flicking)
        for (unsigned i = 0; i < full; i++)
            if (host_miss[i] && cider_prefetch_art(ahead[i], "M"))
                break;
    cider_warm_art(warm, nwarm, "XS");
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
    unsigned int quality = cider_has_art(row.art, "M") ? 3 :
                           cider_has_art(row.art, "XS") || cider_has_art(row.art, "S") ||
                           cider_has_art(row.art, "L") ? 2 : 1;
    return cover_version(&row, quality);
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
    .play_album = play_album,
    .panel_play = panel_play,
    .panel_play_status = play_status,
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
    cider_cancel_art();
    revision = cider_art_revision();
    /* carousel_run joins the loader before it returns. Until then the handle
     * stays pinned across cache mutex waits and slide-buffer allocations. */
    panel_perf_art_session(true);
    int ret = carousel_run(&model, NULL);
    surface = NULL;
    core_unpin(scratch);
    core_free(scratch);
    cider_cancel_art();
    panel_perf_art_session(false);
    return ret == CAROUSEL_REMOTE_RETURN ? CIDER_COVERS_REMOTE_RETURN : ret;
}
