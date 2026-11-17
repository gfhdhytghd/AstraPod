/* Animated Now Playing lyrics. GPL version 2 or later.
 * The unused plugin arena supplies glyph, snapshot and composition frames.
 * No audio buffer is stolen. Without snapshot space we fade through the
 * gradient. No document pointers survive a draw or a track reload. */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "file.h"
#include "kernel.h"
#include "font.h"
#include "rbpaths.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "lcd.h"
#include "button.h"
#include "backlight.h"
#include "audio.h"
#include "metadata.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/color.h"
#include "draw/text_wrap.h"
#include "draw/bmp.h"
#include "draw/round_rect.h"
#include "skin/skin_engine.h"
#include "skin/skin_albumart_color.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/app_buffer.h"
#include "root_menu.h"
#include "screens/playback/wps.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"
#include "screens/playback/cider_library.h"
#include "lyrics.h"
#include "lyrics_motion.h"
#include "lyric_viewer.h"

#define LV_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define LV_FRAME (LV_PIXELS * sizeof(fb_data))
#define LV_TICKS(ms) MAX(1, ((ms) * HZ + 999) / 1000)
#define LV_ROWS 3
#define LV_TEXT 800
static struct {
    struct viewport vp, mask_vp;
    struct frame_buffer_t mask_fb;
    fb_data *mask, *snapshot, *layer;
    int fonts[3], small, large;
    unsigned bg, bottom, fg, accent;
    struct lyrics_motion scroll, pill;
    struct lyrics_mode mode;
    char track[MAX_PATH], title[MAX_PATH];
    long elapsed, length;
    uint32_t changed, remote_deadline, remote_retry, art_gen;
    int remote_rows;
    bool remote, pending, held, paused, track_fade, initial, snapshot_wps, claimed;
} lv;
static fb_data *lv_frame(void)
{
    lcd_set_viewport(NULL);
    return FBADDR(0, 0);
}
static void *lv_mask_address(int x, int y) { return lv.mask + y * LCD_WIDTH + x; }
/* RGB565 interpolation, without per-pixel colour-space conversions. */
static fb_data lv_mix(fb_data a, fb_data b, unsigned p)
{
    return (((((a & 0xf800) * (256 - p) + (b & 0xf800) * p) >> 8) & 0xf800)
          | ((((a & 0x07e0) * (256 - p) + (b & 0x07e0) * p) >> 8) & 0x07e0)
          | ((((a & 0x001f) * (256 - p) + (b & 0x001f) * p) >> 8) & 0x001f));
}
static unsigned lv_background(int y) { return color_blend(lv.bg, lv.bottom, y * 256 / LCD_HEIGHT); }
static int lv_font(int slot)
{
    return lv.fonts[slot] >= 0 ? lv.fonts[slot] : screens[SCREEN_MAIN].getuifont();
}
static bool lv_art_colour(unsigned *dominant)
{
    int handle = wps_art_handle(0);
    struct bitmap *bmp;
    if (handle < 0 || !wps_art_pin(handle, true)) return false;
    bool have = wps_art_bitmap(handle, &bmp) && bmp->width > 0 && bmp->height > 0;
    if (have)
    {
        /* Quantised modal colour, sampled before unpinning the movable art.
         * Independent of the theme's dynamic-colour switch. */
        unsigned short bins[64] = {0};
        unsigned best = 0, winner = 0;
        const fb_data *data = (const fb_data *)bmp->data;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
        {
            unsigned c = data[(y * bmp->height / 16) * bmp->width + x * bmp->width / 16];
            int bin = ((c >> 10) & 0x30) | ((c >> 7) & 0x0c) | ((c >> 3) & 3);
            if (++bins[bin] > best) { best = bins[bin]; winner = c; }
        }
        *dominant = winner;
    }
    wps_art_pin(handle, false);
    return have;
}
static void lv_colours(void)
{
    unsigned dominant, vivid;
    lv.bg = global_settings.bg_color; lv.fg = global_settings.fg_color;
    if (global_settings.lyric_colour_mode == 1)
    { lv.bg = global_settings.fg_color; lv.fg = global_settings.bg_color; }
    if (global_settings.lyric_colour_mode == 2) { lv.bg = LCD_WHITE; lv.fg = LCD_BLACK; }
    if (global_settings.lyric_colour_mode == 3) { lv.bg = LCD_BLACK; lv.fg = LCD_WHITE; }
    lv.accent = dynamic_colors_resolve(global_settings.fg_color);
    if (!global_settings.lyric_colour_mode && lv_art_colour(&dominant))
    {
        unsigned unused;
        if (!dynamic_colors_roles(&unused, &vivid)) vivid = dominant;
        lv.bg = color_blend(LCD_BLACK, dominant, 90);
        lv.fg = LCD_WHITE; lv.accent = color_blend(vivid, LCD_WHITE, 80);
    }
    lv.bottom = color_blend(LCD_BLACK, lv.bg, 28);
}
static void lv_backlight(bool hold)
{
    hold = hold && global_settings.lyric_backlight;
    if (hold == lv.held) return;
    lv.held = hold;
    backlight_set_timeout(hold && global_settings.backlight_timeout > 0 ? 0 : global_settings.backlight_timeout);
    backlight_set_timeout_plugged(hold && global_settings.backlight_timeout_plugged > 0 ? 0 : global_settings.backlight_timeout_plugged);
}
static void lv_ellipsize(char *text, int font, int width)
{
    struct font *pf = font_get(font);
    const unsigned char *p = (const unsigned char *)text;
    int w = 0, cut = 0, dots = font_get_width(pf, '.') * 3;
    while (*p)
    {
        ucschar_t ch;
        const unsigned char *next = utf8decode(p, &ch);
        if (!IS_DIACRITIC(ch)) w += font_get_width(pf, ch);
        if (w + dots > width) break;
        cut = next - (const unsigned char *)text; p = next;
    }
    if (*p) strcpy(text + cut, "...");
}
struct lv_row { const char *text; int length; };
static int lv_wrap(const char *text, int font, struct lv_row rows[LV_ROWS])
{
    int n = 0;
    do {
        const char *end;
        rows[n].text = text;
        text = text_wrap_next_punct(text, LCD_WIDTH - 32, font_get(font), &end);
        rows[n].length = end - rows[n].text; n++;
    } while (*text && n < LV_ROWS);
    return n;
}
/* Draw into an independent mask, then alpha-composite both bitmap sizes.
 * Their overlap cross-fades too; no glyph replacement at a size threshold. */
static void lv_text(const char *text, int font, int x, int y, int opacity,
                    int sung_width, int unsung, bool edges)
{
    int h = font_get(font)->height, top = MAX(0, y), bot = MIN(LCD_HEIGHT, y + h);
    if (opacity <= 0 || bot <= top) return;
    if (!lv.mask)
    {
        lcd_set_viewport(&lv.vp); lcd_setfont(font); lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(color_blend(lv_background(MAX(0, y)), lv.fg, opacity));
        lcd_putsxy(x, y, text); return;
    }
    memset(lv.mask + top * LCD_WIDTH, 0, (bot - top) * LCD_WIDTH * sizeof(fb_data));
    lcd_set_viewport(&lv.mask_vp); lcd_setfont(font);
    lcd_set_foreground(LCD_WHITE); lcd_set_drawmode(DRMODE_FG); lcd_putsxy(x, y, text);
    fb_data *frame = lv_frame();
    for (int r = top; r < bot; r++)
    {
        int a = opacity;
        if (edges) a = a * lyrics_edge_opacity(r, LCD_HEIGHT) / 256;
        for (int c = MAX(0, x); c < LCD_WIDTH - 16; c++)
        {
            unsigned mask = lv.mask[r * LCD_WIDTH + c];
            if (!mask) continue;
            int alpha = a * ((mask >> 5) & 63) / 63;
            if (sung_width >= 0 && c - x >= sung_width) alpha = alpha * unsung / 256;
            fb_data *pixel = frame + r * LCD_WIDTH + c;
            *pixel = lv_mix(*pixel, lv.fg, alpha);
        }
    }
}
/* Smooth pixel boundary inside the active word. UTF-8 is never split. */
static int lv_sung_width(int index, int font)
{
    const char *text = lyrics_text(index);
    if (!text || !lyrics_timed() || !global_settings.lyric_highlight) return -1;
    int n = lyrics_word_count(index), width = 0;
    for (int w = 0; w < n; w++)
    {
        long start = lyrics_word_time(index, w);
        if (start < 0) return -1;
        const char *p = lyrics_word_text(index, w);
        const char *end = w + 1 < n ? lyrics_word_text(index, w + 1) : text + strlen(text);
        long stop = w + 1 < n ? lyrics_word_time(index, w + 1) : lyrics_end_time(index);
        int part = 0;
        while (p < end)
        {
            ucschar_t ch;
            p = (const char *)utf8decode((const unsigned char *)p, &ch);
            if (!IS_DIACRITIC(ch)) part += font_get_width(font_get(font), ch);
        }
        width += lyrics_word_progress(lv.elapsed, start, stop, part);
        if (lv.elapsed < stop) break;
    }
    return width;
}
static int lv_height(int i, int position)
{
    struct lv_row rows[LV_ROWS];
    const char *text = lyrics_text(i);
    if (!text) return lv.small + 12;
    int small = lv_wrap(text, lv_font(1), rows) * lv.small;
    int large = lv_wrap(text, lv_font(2), rows) * lv.large;
    return lyrics_line_height(small, large, lyrics_focus_weight(i, position));
}
static void lv_line(int i, int center, int position)
{
    struct lv_row rows[LV_ROWS]; char text[LV_TEXT];
    const char *line = lyrics_text(i);
    if (!line) return;
    int weight = lyrics_focus_weight(i, position), rel = i * 256 - position;
    int nearest = (rel < 0 ? global_settings.lyric_prev_opacity : global_settings.lyric_next_opacity) * 256 / 100;
    int opacity = lyrics_opacity(rel, nearest);
    for (int size = 0; size < 2; size++)
    {
        int alpha = opacity * (size ? weight : 256 - weight) / 256;
        if (!alpha) continue;
        int font = lv_font(size + 1), h = size ? lv.large : lv.small;
        int n = lv_wrap(line, font, rows), sung = lv_sung_width(i, font);
        for (int r = 0; r < n; r++)
        {
            size_t length = MIN(rows[r].length, (int)sizeof(text) - 4);
            if (r == n - 1 && rows[r].text[length])
            { strlcpy(text, rows[r].text, sizeof text); lv_ellipsize(text, font, LCD_WIDTH - 32); }
            else { memcpy(text, rows[r].text, length); text[length] = 0; }
            int before = 0;
            const unsigned char *prefix = (const unsigned char *)line;
            while (prefix < (const unsigned char *)rows[r].text)
            {
                ucschar_t ch; prefix = utf8decode(prefix, &ch);
                if (!IS_DIACRITIC(ch)) before += font_get_width(font_get(font), ch);
            }
            lv_text(text, font, 16, center - n * h / 2 + r * h, alpha,
                    sung < 0 ? -1 : MAX(0, sung - before), 256 - weight * 102 / 256, true);
        }
    }
}
static const char *lv_status(void)
{
    return lv.pending ? "Loading lyrics..." : !lv.track[0] ? "Nothing playing" : "No lyrics";
}
static void lv_draw(void)
{
    fb_data *frame = lv_frame();
    for (int y = 0; y < LCD_HEIGHT; y++)
    {
        fb_data color = lv_background(y);
        for (int x = 0; x < LCD_WIDTH; x++) frame[y * LCD_WIDTH + x] = color;
    }
    int position = lyrics_motion_at(&lv.scroll, current_tick), base = position / 256;
    int center = lyrics_line_y(LCD_HEIGHT * 42 / 100, position % 256,
                               lv_height(base, position), lv_height(base + 1, position));
    int count = lyrics_loaded() ? lyrics_count() : 0;
    for (int i = base, y = center; i < count && y < LCD_HEIGHT + 100; i++)
    {
        lv_line(i, y, position);
        y += (lv_height(i, position) + lv_height(i + 1, position)) / 2;
    }
    for (int i = base - 1, y = center; i >= 0; i--)
    {
        y -= (lv_height(i, position) + lv_height(i + 1, position)) / 2;
        if (y < -100) break;
        lv_line(i, y, position);
    }
    if (!count) lv_text(lv_status(), lv_font(1), 16, LCD_HEIGHT * 42 / 100, 256, -1, 256, false);
    char title[MAX_PATH]; strlcpy(title, lv.title, sizeof title);
    int pill = lyrics_motion_at(&lv.pill, current_tick);
    lv_ellipsize(title, lv_font(0), LCD_WIDTH - 32 - pill * 58 / 256);
    lv_text(title, lv_font(0), 16, 10, 141, -1, 256, false);
    if (pill)
    {
        lcd_set_viewport(&lv.vp);
        lcd_set_foreground(color_blend(lv_background(10), lv.accent, pill / 4));
        fill_round_rect(&screens[SCREEN_MAIN], LCD_WIDTH - 66, 7, 50, 20, 8);
        lv_text("Manual", lv_font(0), LCD_WIDTH - 62, 10, pill, -1, 256, false);
    }
    lcd_set_viewport(&lv.vp); lcd_set_foreground(lv.accent);
    int progress = lv.length > 0 ? (int)((int64_t)MIN(lv.elapsed, lv.length) * LCD_WIDTH / lv.length) : 0;
    lcd_fillrect(0, LCD_HEIGHT - 2, progress, 2);
}
static void lv_present(int opacity, int shift, bool underlay)
{
    fb_data *frame = lv_frame();
    if (lv.layer) memcpy(lv.layer, frame, LV_FRAME);
    for (int y = LCD_HEIGHT - 1; y >= 0; y--)
    {
        unsigned bg = lv_background(y);
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            int at = y * LCD_WIDTH + x;
            fb_data page = y >= shift ? (lv.layer ? lv.layer : frame)[at - shift * LCD_WIDTH] : bg;
            fb_data back = underlay && lv.snapshot ? lv_mix(lv.snapshot[at], LCD_BLACK, opacity * 110 / 256) : bg;
            frame[at] = lv_mix(back, page, opacity);
        }
    }
    lcd_update();
}
static void lv_transition(bool opening)
{
    uint32_t start = current_tick; int duration = LV_TICKS(opening ? 220 : 180);
    for (;;)
    {
        uint32_t frame_at = current_tick;
        int age = (uint32_t)current_tick - start;
        int p = lyrics_ease(MIN(256, age * 256 / duration));
        int alpha = opening ? p : 256 - p;
        if (!lv.snapshot)
        {
            /* Two animated halves through the background need no saved WPS.
             * skin_update draws the live WPS without flushing the panel. */
            int half = MIN(512, age * 512 / duration);
            bool page = opening ? half >= 256 : half < 256;
            int a = half < 256 ? 256 - lyrics_ease(half) : lyrics_ease(half - 256);
            if (page) lv_draw(); else wps_lyrics_backdrop();
            lv_present(a, page ? 24 * (256 - a) / 256 : 0, false);
        }
        else { lv_draw(); lv_present(alpha, 24 * (256 - alpha) / 256, lv.snapshot_wps); }
        if (age >= duration) break;
        sleep(MAX(1, HZ / 30 - (int)((uint32_t)current_tick - frame_at)));
    }
}
static void lv_changed(void)
{
    if (!lv.initial)
    {
        if (!lv.snapshot)
        {
            uint32_t start = current_tick;
            for (;;)
            {
                uint32_t frame_at = current_tick;
                int age = (uint32_t)current_tick - start;
                int p = lyrics_ease(MIN(256, age * 256 / LV_TICKS(100)));
                lv_draw(); lv_present(256 - p, 0, false);
                if (p == 256) break;
                sleep(MAX(1, HZ / 30 - (int)((uint32_t)current_tick - frame_at)));
            }
        }
        if (lv.snapshot) memcpy(lv.snapshot, lv_frame(), LV_FRAME);
        lv.snapshot_wps = false; lv.changed = current_tick; lv.track_fade = true;
    }
}
static void lv_focus(void)
{
    lyrics_retarget(&lv.scroll, lv.mode.focus * 256, current_tick, LV_TICKS(260));
}
#ifdef HAVE_USB_PODLINK
static void lv_remote_request(void)
{
    static uint32_t nonce;
    char query[144];
    /* Remote WPS paths are cider/<CN id> for both backends. */
    const char *key = lv.track + sizeof("cider/") - 1;
    if (strncmp(lv.track, "cider/", sizeof("cider/") - 1) || !*key)
        return;
    snprintf(query, sizeof query, "now/lyrics?nonce=%lu-%lu&lrc=1&track=%s",
             (unsigned long)current_tick, (unsigned long)++nonce, key);
    cider_page_open(query);
    lv.pending = true;
}
#endif
static void lv_reload(struct mp3entry *id3, bool remote)
{
    lv_changed();
    bool have_title = id3 && id3->title && id3->title[0];
    /* Copy metadata before lyrics_load can compact the playback buffer. */
    strlcpy(lv.track, id3 ? id3->path : "", sizeof lv.track);
    const char *name = id3 ? strrchr(lv.track, '/') : NULL;
    snprintf(lv.title, sizeof lv.title, "%s%s%s", id3 && id3->title ? id3->title : name ? name + 1 : "",
             id3 && id3->artist ? " — " : "", id3 && id3->artist ? id3->artist : "");
    lv.remote = remote; lv.pending = false; lv.remote_rows = 0; lv.remote_retry = 0;
    lv.elapsed = id3 ? id3->elapsed : 0; lv.length = id3 ? id3->length : 0;
    if (remote)
    {
#ifdef HAVE_USB_PODLINK
        if (lyrics_begin_text() == LYRICS_OK)
            lv_remote_request();
        lv.remote_deadline = current_tick + 5 * HZ;
#endif
    }
    else if (id3) lyrics_load(id3);
    else lyrics_close();
    if (!have_title && lyrics_title())
        snprintf(lv.title, sizeof lv.title, "%s%s%s", lyrics_title(),
                 lyrics_artist() ? " — " : "", lyrics_artist() ? lyrics_artist() : "");
    lv.mode = (struct lyrics_mode){0};
    if (lyrics_timed()) lv.mode.focus = MAX(0, lyrics_index_at(lv.elapsed));
    lv.scroll = (struct lyrics_motion){.from = lv.mode.focus * 256, .to = lv.mode.focus * 256};
    lv.pill = (struct lyrics_motion){0};
    lv_colours(); lv_backlight(lyrics_loaded());
    lv.art_gen = skin_albumart_gen();
    lv.changed = current_tick;
}
static void lv_remote_page(void)
{
#ifdef HAVE_USB_PODLINK
    if (!lv.remote || !lv.pending) return;
    bool expired = !TIME_BEFORE(current_tick, lv.remote_deadline);
    if (lv.remote_retry && !expired)
    {
        if (TIME_BEFORE(current_tick, lv.remote_retry)) return;
        lv.remote_retry = 0;
        lv_remote_request();
        return;
    }
    int count = cider_page_total();
    if (count < 0 && !expired) return;
    bool valid = true;
    struct cider_row row;
    while (lv.remote_rows < count)
    {
        if (!cider_page_row(lv.remote_rows, &row))
        {
            if (!expired) return;
            break;
        }
        if (!strcmp(row.id, "lyric-wait"))
        {
            lyrics_begin_text();
            lv.remote_rows = 0;
            if (!expired)
            {
                lv.remote_retry = current_tick + HZ / 2;
                return;
            }
            break;
        }
        bool more = !strcmp(row.id, "lrc1+");
        bool timed = more || !strcmp(row.id, "lrc1");
        valid = !lv.remote_rows || timed == lyrics_timed();
        if (!valid || !(timed ? lyrics_append_lrc(row.title, more) : lyrics_append_text(row.title)))
        {
            valid = !timed && valid; /* Legacy text keeps the complete prefix. */
            break;
        }
        lv.remote_rows++; lv.remote_deadline = current_tick + 5 * HZ;
    }
    /* A CN can arrive while the last page is being copied. */
    const struct mp3entry *id3 = get_wps_state()->id3;
    if (!cider_active() || !id3 || strcmp(id3->path, lv.track)) return;
    lv_changed();
    if (!valid || (lyrics_timed() &&
        (lv.remote_rows != count || !lyrics_finish_lrc(lv.length))))
        lyrics_begin_text();
    else if (!lyrics_timed())
        lyrics_finish_text();
    if (lyrics_timed())
    {
        lv.mode.focus = MAX(0, lyrics_index_at(lv.elapsed));
        lv.scroll = (struct lyrics_motion){.from = lv.mode.focus * 256,
                                           .to = lv.mode.focus * 256};
    }
    lv.pending = false;
    lv_backlight(lyrics_loaded());
#endif
}
/* The WPS supplies identical seek acceleration/release and source dispatch. */
static void lv_seek_draw(void)
{
    struct wps_state *s = get_wps_state();
    if (lv.remote && (!s->id3 || strcmp(s->id3->path, lv.track)))
        return;
    if (s->id3) lv.elapsed = s->id3->elapsed + s->ff_rewind_count;
    if (lyrics_loaded() && lyrics_mode_follow(&lv.mode, lyrics_timed(), lyrics_index_at(lv.elapsed), current_tick, HZ)) lv_focus();
    lv_draw(); lcd_update();
}
static void lv_transport(int action)
{
    if (action == ACTION_WPS_SEEKFWD || action == ACTION_WPS_SEEKBACK)
    { wps_lyrics_seek(action, lv_seek_draw); return; }
    if (lv.remote)
    { cider_control(action == ACTION_WPS_PLAY ? "toggle" : action == ACTION_WPS_SKIPNEXT ? "next" : "previous", 0); return; }
    if (action == ACTION_WPS_PLAY) wps_do_playpause(false);
    else wps_lyrics_skip(action == ACTION_WPS_SKIPNEXT ? 1 : -1);
}
int lyric_viewer(void)
{
    static const char *const fonts[] = { FONT_DIR "/12-noto-sans.fnt", FONT_DIR "/18-noto-sans.fnt", FONT_DIR "/26-noto-sans-medium.fnt" };
    size_t bytes = 0; fb_data *arena = app_try_claim_buffer(&bytes, "lyrics");
    memset(&lv, 0, sizeof lv); lv.initial = true;
    lv.claimed = arena != NULL;
    /* Hold the shared scratch arena until teardown; never alias its owner. */
    if (arena && bytes >= LV_FRAME) { lv.mask = arena; arena += LV_PIXELS; bytes -= LV_FRAME; }
    if (arena && bytes >= LV_FRAME) { lv.snapshot = arena; arena += LV_PIXELS; bytes -= LV_FRAME; }
    if (arena && bytes >= LV_FRAME) lv.layer = arena;
    if (lv.snapshot) { memcpy(lv.snapshot, lv_frame(), LV_FRAME); lv.snapshot_wps = true; }
    skin_hold_scrolling(true);
    bool old_inhibit = skin_flush_inhibited(); skin_inhibit_flush(true);
    push_current_activity(ACTIVITY_LYRICS);
    viewportmanager_theme_enable(SCREEN_MAIN, false, &lv.vp);
    for (int i = 0; i < 3; i++) lv.fonts[i] = font_load(fonts[i]);
    lv.small = font_get(lv_font(1))->height; lv.large = font_get(lv_font(2))->height;
    lv.mask_fb = (struct frame_buffer_t){.fb_ptr = lv.mask, .get_address_fn = lv_mask_address, .stride = LCD_WIDTH, .elems = LV_PIXELS};
    lv.mask_vp = lv.vp; lv.mask_vp.buffer = &lv.mask_fb; lv.mask_vp.bg_pattern = LCD_BLACK;
    struct mp3entry *first = cider_active() ? get_wps_state()->id3 : audio_current_track();
    if (!cider_active() && !(audio_status() & AUDIO_STATUS_PLAY)) first = NULL;
    lv_reload(first, cider_active()); lv_transition(true); lv.initial = false;
    int result = GO_TO_PREVIOUS, event = 0;
    for (;;)
    {
        uint32_t frame_at = current_tick;
        struct wps_state *s = get_wps_state();
        bool remote = cider_active();
        struct mp3entry *id3 = remote ? s->id3 : audio_current_track();
        if (!remote && !(audio_status() & AUDIO_STATUS_PLAY)) id3 = NULL;
        if (!remote) s->id3 = id3;
        if (remote != lv.remote || strcmp(id3 ? id3->path : "", lv.track))
        {
            lv_reload(id3, remote);
            id3 = remote ? get_wps_state()->id3 : audio_current_track();
        }
        if (id3) { lv.elapsed = id3->elapsed; lv.length = id3->length; }
        lv.paused = remote ? s->paused : (audio_status() & AUDIO_STATUS_PAUSE) != 0;
        lv_remote_page();
        if (lv.art_gen != skin_albumart_gen())
        {
            lv_changed(); lv_colours(); lv.art_gen = skin_albumart_gen();
        }
        bool was_manual = lv.mode.manual;
        if (lyrics_loaded() && lyrics_mode_follow(&lv.mode, lyrics_timed(), lyrics_index_at(lv.elapsed), current_tick, HZ)) lv_focus();
        if (was_manual != lv.mode.manual)
            lyrics_retarget(&lv.pill, lv.mode.manual && lyrics_timed() ? 256 : 0, current_tick, LV_TICKS(180));
        lv_draw();
        if (lv.track_fade)
        {
            int age = (uint32_t)current_tick - lv.changed;
            int p = MIN(256, age * 256 / LV_TICKS(lv.snapshot ? 200 : 100));
            fb_data *frame = lv_frame();
            for (int y = 0; y < LCD_HEIGHT; y++) for (int x = 0; x < LCD_WIDTH; x++)
            {
                int at = y * LCD_WIDTH + x;
                frame[at] = lv_mix(lv.snapshot ? lv.snapshot[at] : lv_background(y), frame[at], p);
            }
            lv.track_fade = p < 256;
        }
        lcd_update();
        bool animated = lv.track_fade || lyrics_moving(&lv.scroll, current_tick) || lyrics_moving(&lv.pill, current_tick);
        int focus = lv.mode.focus;
        bool words = lyrics_word_count(focus) > 0 && lyrics_word_time(focus, 0) >= 0 &&
                     lv.elapsed < lyrics_end_time(focus) && global_settings.lyric_highlight;
        int timeout = lyrics_idle_ticks(lv.elapsed, lyrics_time(focus + 1), lv.paused, animated, words,
                                       &lv.mode, lyrics_timed(), current_tick, HZ);
        if (!lv.paused) timeout = MIN(timeout, lyrics_progress_ticks(lv.elapsed, lv.length, LCD_WIDTH, HZ));
        if (animated || (!lv.paused && words))
            timeout = MAX(1, timeout - (int)((uint32_t)current_tick - frame_at));
        int action = get_action(CONTEXT_WPS, timeout);
        switch (action)
        {
            case ACTION_WPS_BROWSE: case ACTION_WPS_MENU: goto done;
            case ACTION_WPS_VOLUP: case ACTION_WPS_VOLDOWN:
                if (lyrics_loaded())
                {
                    int direction = action == ACTION_WPS_VOLUP ? 1 : -1;
                    int raw_button = 0;
                    get_action_statuscode(&raw_button);
                    if (raw_button & BUTTON_REMOTE)
                    { direction = -direction; lv.mode.direction = 0; }
                    lyrics_mode_scroll(&lv.mode, direction, lyrics_count(), current_tick, HZ);
                    lv_focus(); lyrics_retarget(&lv.pill, lyrics_timed() ? 256 : 0, current_tick, LV_TICKS(180));
                }
                break;
            case ACTION_WPS_PLAY: case ACTION_WPS_SKIPNEXT: case ACTION_WPS_SKIPPREV:
            case ACTION_WPS_SEEKFWD: case ACTION_WPS_SEEKBACK: lv_transport(action); break;
            case ACTION_NONE: case ACTION_REDRAW: break;
            default: if (IS_SYSEVENT(action)) { event = action; goto done; } break;
        }
    }
done:
    /* Render the current WPS without flushing, including a new title after
     * skip/seek. Returning never reveals an obsolete entry-time snapshot. */
    if (lv.snapshot)
    { wps_lyrics_backdrop(); memcpy(lv.snapshot, lv_frame(), LV_FRAME); lv.snapshot_wps = true; }
    lv_transition(false); lyrics_close();
    for (int i = 0; i < 3; i++) if (lv.fonts[i] >= 0) font_unload(lv.fonts[i]);
    lv_backlight(false); lcd_set_viewport(NULL);
    viewportmanager_theme_undo(SCREEN_MAIN, false); pop_current_activity();
    skin_hold_scrolling(false); skin_inhibit_flush(old_inhibit);
    if (lv.claimed) app_release_buffer("lyrics");
    if (event && default_event_handler(event) == SYS_USB_CONNECTED) result = GO_TO_ROOT;
    return result;
}
