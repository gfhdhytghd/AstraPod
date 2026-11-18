/* Animated Now Playing lyrics. GPL version 2 or later.
 * The unused plugin arena supplies the cached picture, glyphs and transition snapshot.
 * No audio buffer is stolen. Without snapshot space we fade through the
 * fallback colour. No document pointers survive a draw or a track reload. */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "config.h"
#include "core_alloc.h"
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
#include "button_chord.h"
#include "backlight.h"
#include "audio.h"
#include "audio/playback.h"
#include "metadata.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/color.h"
#include "draw/text_wrap.h"
#include "draw/bmp.h"
#include "metadata/album_palette.h"
#include "ui/frame.h"
#include "input/action.h"
#include "input/agents_shortcut.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/app_buffer.h"
#include "system/strutil.h"
#include "root_menu.h"
#include "screens/playback/wps.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"
#include "screens/playback/cider_library.h"
#include "ui/native_theme_runtime.h"
#include "ui/perf.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "lyrics.h"
#include "lyrics_motion.h"
#include "lyric_viewer.h"
#ifdef SIMULATOR
#include <SDL_timer.h>
#endif

#define LV_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define LV_FRAME (LV_PIXELS * sizeof(fb_data))
#define LV_TICKS(ms) MAX(1, ((ms) * HZ + 999) / 1000)
#define LV_LINE_FRAMES ANIM_F(16)
#define LV_ENTRY_FRAMES ANIM_F(13)
#define LV_TRACK_FRAMES ANIM_F(12)
#define LV_ENTRY_SHIFT 24
#define LV_ROWS 3
#define LV_TEXT 800
#define LV_LAYOUTS 24
#define LV_MASKS 64
#define LV_BODY_TOP 31
#define LV_BODY_BOTTOM (LCD_HEIGHT - 6)
#if defined(IPOD_6G) && defined(__arm__)
#define LV_PIXELS_ARM __attribute__((target("arm")))
#else
#define LV_PIXELS_ARM
#endif

struct lv_row { unsigned short start, length, before; };
struct lv_layout {
    int line;
    unsigned char count[2];
    struct lv_row rows[2][LV_ROWS];
};
/* Optional extra space in the arena, one entry per layout slot. */
struct lv_cues { unsigned short width[2][32]; };
struct lv_mask {
    int key;
};
enum lv_phase { LV_RESTORE, LV_GLYPHS, LV_BLEND, LV_LCD, LV_WAIT, LV_PHASE_COUNT };
enum lv_type { LV_STEADY, LV_ENTRY, LV_LINE, LV_MANUAL, LV_TRACK, LV_TYPE_COUNT };
static const uint32_t perf_bounds[] = {
    0, 250, 500, 750, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000,
    10000, 12000, 16000, 20000, 25000, 30000, 33000, 34000, 40000, 50000,
    75000, 100000, 150000, 250000, 500000, 1000000
};
struct lv_stat {
    uint64_t sum;
    uint32_t count, maximum, buckets[ARRAYLEN(perf_bounds) + 1];
};
struct lv_perf {
    struct lv_stat total, phase[LV_PHASE_COUNT], type[LV_TYPE_COUNT];
    uint32_t dropped, first, build, builds, gaps, gap_time;
    unsigned cpu_low, cpu_high, boosted, audio, remote_audio;
    unsigned lcd_updates, raster_rows, animated_frames, boosted_animations, entry_prep_us;
    uint32_t entry_fonts_us, entry_document_us, entry_art_us;
    long seek_issue_us, seek_frame_us, seek_clock_us;
    bool seek_remote;
};
struct lv_state {
    struct viewport vp, mask_vp;
    struct frame_buffer_t mask_fb;
    struct ui_page page;
    struct ui_view backdrop, body, progress;
    fb_data *snapshot, *layer, *background, *masks;
    struct lv_layout *layouts;
    struct lv_mask *glyphs;
    int fonts[2], small, large, mask_pixels;
    unsigned bg, bottom, fg, accent;
    struct lyrics_motion scroll;
    struct lyrics_mode mode;
    char track[MAX_PATH];
    long elapsed, length;
    uint32_t track_clock, remote_serial, art_gen, palette_gen, seek_at, seek_tick, seek_seq;
    long seek_target, seek_from;
    uint32_t entry_at, entry_tick, present_at, phase_us[LV_WAIT];
    unsigned phase_mask;
    enum lv_type last_type;
    struct lv_perf perf;
    int art_handle;
    int old_position, old_progress, old_words;
    int entry_handle, entry_frame, track_frame;
    uint32_t entry_clock;
    unsigned entry_frames, track_frames;
    bool remote, pending, held, paused, track_fade, initial, claimed;
    bool painted, art_ready, first_pending, boost, seek_pending, seek_consumed, seek_running;
    struct lv_cues *cues;
};
/* All users run synchronously on the UI thread. The page state is on its stack. */
static struct lv_state *lv_state;
#define lv (*lv_state)

#define perf (lv.perf)

#ifdef HAVE_USB_PODLINK
/* The existing 32 KiB document is shared by WPS prefetch and Lyrics.
 * Both run on the UI thread. Page transport stays in the resident worker. */
static struct {
    char id[80];
    uint32_t generation, library, serial, nonce;
    uint32_t at, deadline, retry, first_ms, complete_ms;
    long duration;
    int rows;
    bool done, first_ready;
} remote_doc;
#endif

static uint32_t lv_clock(void)
{
#ifdef SIMULATOR
    uint64_t count = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    return (count / freq) * 1000000 + (count % freq) * 1000000 / freq;
#elif defined(USEC_TIMER)
    return USEC_TIMER;
#else
    return (uint32_t)current_tick * (1000000 / HZ);
#endif
}

/* Own at most one reference, including when playback already owns another. */
static void lv_boost(bool enable)
{
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(SIMULATOR)
    if (enable != lv.boost)
    {
        cpu_boost(enable);
        lv.boost = enable;
    }
#else
    (void)enable;
#endif
}

static void lv_stat_add(struct lv_stat *stat, uint32_t us)
{
    unsigned lo = 0, hi = ARRAYLEN(perf_bounds);
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (us > perf_bounds[mid]) lo = mid + 1;
        else hi = mid;
    }
    stat->buckets[lo]++;
    stat->count++;
    stat->sum += us;
    stat->maximum = MAX(stat->maximum, us);
}

static unsigned long lv_p95(const struct lv_stat *stat)
{
    unsigned remaining = stat->count - stat->count / 20;
    for (unsigned i = 0; i < ARRAYLEN(perf_bounds); i++)
    {
        if (stat->buckets[i] >= remaining)
            return perf_bounds[i];
        remaining -= stat->buckets[i];
    }
    return stat->maximum; /* p95 overflow bin: use the observed upper bound. */
}

static void lv_phase_end(enum lv_phase phase, uint32_t start)
{
    lv.phase_us[phase] += lv_clock() - start;
    lv.phase_mask |= 1u << phase;
}

static void lv_record(uint32_t started, enum lv_type type, bool visible)
{
    uint32_t now = lv_clock(), elapsed = now - started;
    lv_stat_add(&perf.total, elapsed);
    lv_stat_add(&perf.type[type], elapsed);
    for (unsigned i = 0; i < LV_WAIT; i++)
    {
        if (lv.phase_mask & (1u << i)) lv_stat_add(&perf.phase[i], lv.phase_us[i]);
        lv.phase_us[i] = 0;
    }
    lv.phase_mask = 0;
    uint32_t budget = ui_frame_clock(1);
    bool late = elapsed > budget;
    if (type != LV_STEADY && lv.last_type == type)
    {
        uint32_t interval = now - lv.present_at;
        perf.gaps++;
        perf.gap_time += interval;
        late |= interval > budget;
    }
    perf.dropped += late;
    lv.last_type = type;
    lv.present_at = now;
    if (lv.first_pending && visible)
    {
        perf.first = now - lv.entry_at;
        lv.first_pending = false;
    }
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(SIMULATOR)
    unsigned mhz = (FREQ + 500000) / 1000000;
    if (!perf.cpu_low || mhz < perf.cpu_low) perf.cpu_low = mhz;
    perf.cpu_high = MAX(perf.cpu_high, mhz);
    bool boosted = get_cpu_boost_counter() > 0;
    perf.boosted += boosted;
    perf.boosted_animations += type != LV_STEADY && boosted;
#endif
    perf.animated_frames += type != LV_STEADY;
    perf.audio += (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) == AUDIO_STATUS_PLAY;
    perf.remote_audio += lv.remote && !lv.paused && lv.track[0];
}

static void lv_stat_save(int fd, const char *name, const struct lv_stat *stat)
{
    fdprintf(fd, "%s: %lu %lu/%lu/%lu\n", name, (unsigned long)stat->count,
             stat->count ? (unsigned long)(stat->sum / stat->count) : 0UL,
             lv_p95(stat), (unsigned long)stat->maximum);
}

struct lyrics_perf_history lyrics_perf_scan(int fd)
{
    struct lyrics_perf_history history = { .latest = -1 };
    off_t position = 0, block = -1;
    uint32_t session = 0;
    char line[64];
    int bytes;
    while (fd >= 0 && (bytes = read_line(fd, line, sizeof(line))) > 0)
    {
        if (position == 0 && !strncmp(line, "frames: ", 8))
        {
            /* The previous recorder saved one visit without a header. */
            history = (struct lyrics_perf_history){ .latest = 0, .session = 1, .count = 1 };
        }
        if (!strncmp(line, LYRICS_PERF_HEADER, sizeof(LYRICS_PERF_HEADER) - 1))
        {
            session = strtoul(line + sizeof(LYRICS_PERF_HEADER) - 1, NULL, 10);
            block = position;
        }
        else if (block >= 0 && session && bytes == (int)sizeof(LYRICS_PERF_END) &&
                 !strcmp(line, LYRICS_PERF_END))
        {
            history.latest = block;
            history.session = session;
            history.count++;
            block = -1;
        }
        position += bytes;
    }
    return history;
}

/* Leave room for a complete report, including maximum-width counters. */
#define LV_PERF_RESERVE 1536
static uint32_t lv_perf_session;

static int lv_perf_open(void)
{
    if (!lv_perf_session)
    {
        int fd = open(LYRICS_PERF_PATH, O_RDONLY);
        if (fd >= 0)
        {
            lv_perf_session = lyrics_perf_scan(fd).session;
            close(fd);
        }
        if (!lv_perf_session && (fd = open(LYRICS_PERF_ARCHIVE, O_RDONLY)) >= 0)
        {
            lv_perf_session = lyrics_perf_scan(fd).session;
            close(fd);
        }
    }

    int fd = open(LYRICS_PERF_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return fd;
    off_t size = ffilesize(fd);
    if (size >= 0 && size <= LYRICS_PERF_FILE_LIMIT - LV_PERF_RESERVE)
        return fd;
    if (size > LYRICS_PERF_FILE_LIMIT && ftruncate(fd, LYRICS_PERF_FILE_LIMIT) < 0)
        size = -1;
    if (close(fd) < 0 || size < 0 || rename(LYRICS_PERF_PATH, LYRICS_PERF_ARCHIVE) < 0)
        return -1;
    return open(LYRICS_PERF_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
}

/* Keep the file I/O call frame out of the drawing loop. */
static void __attribute__((noinline)) lv_perf_save(void)
{
    int fd = lv_perf_open();
    if (fd < 0) return;
    fdprintf(fd, "\n" LYRICS_PERF_HEADER "%lu\nUptime (s): %lu\nSource: %s%s\n"
             "Track: %s\nDuration (ms): %lu\n", (unsigned long)++lv_perf_session,
             (unsigned long)(lv.entry_tick / HZ), lv.remote ? "Remote / " : "Local",
             lv.remote ? cider_remote_name() : "",
             lyrics_loaded() && lyrics_timed() ? "Timed" : "Untimed",
             (unsigned long)((uint32_t)current_tick - lv.entry_tick) * (1000 / HZ));
    const struct lv_stat *all = &perf.total;
    fdprintf(fd, "frames: %lu\navg/p95/max ms: %lu/%lu/%lu\n", (unsigned long)all->count,
             all->count ? (unsigned long)((all->sum / all->count + 500) / 1000) : 0UL,
             (lv_p95(all) + 999) / 1000, (unsigned long)((all->maximum + 999) / 1000));
    fdprintf(fd, "dropped (30 fps): %lu\nSELECT to first ms: %lu\n",
             (unsigned long)perf.dropped, (unsigned long)((perf.first + 999) / 1000));
    if (perf.seek_frame_us >= 0)
        fdprintf(fd, "SELECT seek %s (issue/frame/clock us): %ld/%ld/%ld\n",
                 perf.seek_remote ? "remote" : "local",
                 perf.seek_issue_us, perf.seek_frame_us, perf.seek_clock_us);
    fdprintf(fd, "blur max ms: %lu\nart background renders: %lu\n",
             (unsigned long)((perf.build + 999) / 1000), (unsigned long)perf.builds);
    fdprintf(fd, "animation fps: %lu\nCPU MHz (0 unknown): %u-%u\n",
             perf.gap_time ? (unsigned long)((uint64_t)perf.gaps * 1000000 / perf.gap_time) : 0UL,
             perf.cpu_low, perf.cpu_high);
    fdprintf(fd, "boost/local audio/remote: %u/%u/%u\n", perf.boosted, perf.audio, perf.remote_audio);
#ifdef HAVE_USB_PODLINK
    if (lv.remote && lv.remote_serial == remote_doc.serial)
        fdprintf(fd, "prefetch first/complete ms: %ld/%ld\n",
                 remote_doc.first_ready ? (long)remote_doc.first_ms : -1L,
                 remote_doc.complete_ms != UINT32_MAX ? (long)remote_doc.complete_ms : -1L);
#endif
    fdprintf(fd, "entry setup us: %u\n", perf.entry_prep_us);
    fdprintf(fd, "entry fonts/document/art us: %u/%u/%u\n",
             (unsigned)perf.entry_fonts_us, (unsigned)perf.entry_document_us,
             (unsigned)perf.entry_art_us);
    fdprintf(fd, "animation boosted/frames: %u/%u\nLCD submits: %u\nmask rasters: %u\n",
             perf.boosted_animations, perf.animated_frames, perf.lcd_updates, perf.raster_rows);
    fdprintf(fd, "samples avg/p95/max us\n");
    static const char *const phases[] = { "restore", "text/glyph", "blend", "LCD", "wait" };
    static const char *const types[] = { "steady", "entry", "line change", "manual scroll", "track change" };
    for (int i = 0; i < LV_PHASE_COUNT; i++) lv_stat_save(fd, phases[i], &perf.phase[i]);
    for (int i = 0; i < LV_TYPE_COUNT; i++) lv_stat_save(fd, types[i], &perf.type[i]);
    fdprintf(fd, LYRICS_PERF_END "\n");
    close(fd);
}

static void lv_flush(int x, int y, int w, int h)
{
    if (lcd_updates_held() || !lcd_active())
        return;
    uint32_t start = lv_clock();
    if (w == LCD_WIDTH && h == LCD_HEIGHT) lcd_update();
    else lcd_update_rect(x, y, w, h);
    ui_note_flush(lv_clock() - start);
    lv_phase_end(LV_LCD, start);
    perf.lcd_updates++;
}

static fb_data *lv_frame(void)
{
    lcd_set_viewport(NULL);
    return FBADDR(0, 0);
}
static void *lv_mask_address(int x, int y)
{
    return lv.mask_fb.fb_ptr + y * LCD_WIDTH + x;
}

/* With 5-bit alpha the spaced RGB565 fields can blend in parallel. */
static inline __attribute__((always_inline)) fb_data lv_mix(fb_data a, fb_data b, unsigned p)
{
    unsigned lo = ((a | ((uint32_t)a << 16)) & 0x07e0f81f);
    unsigned hi = ((b | ((uint32_t)b << 16)) & 0x07e0f81f);
    unsigned weight = (p + 4) >> 3;
    unsigned mixed = ((lo * (32 - weight) + hi * weight) >> 5) & 0x07e0f81f;
    return mixed | (mixed >> 16);
}
static unsigned lv_background(int y)
{
    return color_blend(lv.bg, lv.bottom, y * 256 / LCD_HEIGHT);
}
static int lv_font(int slot)
{
    return lv.fonts[slot] >= 0 ? lv.fonts[slot] : screens[SCREEN_MAIN].getuifont();
}

static unsigned lv_color(const struct ui_theme *theme,
                         const struct ui_album_palette *palette, enum ui_color_role role)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static void lv_background_build(int handle)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    struct ui_album_palette palette;
    album_palette_snapshot(&palette);
    lv.bg = lv_color(&theme, &palette, UI_COLOR_BACKGROUND);
    lv.fg = lv_color(&theme, &palette, UI_COLOR_FOREGROUND);
    lv.accent = lv_color(&theme, &palette, UI_COLOR_ACCENT);

    switch (global_settings.lyric_colour_mode)
    {
        case 1:
        {
            unsigned fg = lv.fg;
            lv.fg = lv.bg;
            lv.bg = fg;
            break;
        }
        case 2: lv.bg = LCD_WHITE; lv.fg = LCD_BLACK; break;
        case 3: lv.bg = LCD_BLACK; lv.fg = LCD_WHITE; break;
    }
    lv.bottom = color_blend(lv.bg, LCD_BLACK, 28);
    lv.art_ready = false;

    enum ui_bg_kind bg = theme.background[UI_BG_LYRICS];
    if (!lv.background || handle < 0 || bg == UI_BG_SOLID)
        return;

    uint32_t start = lv_clock();
    lv.art_ready = album_art_render_background(lv.background, LCD_WIDTH, LCD_HEIGHT,
                                                handle, bg == UI_BG_BLURRED_COVER, lv.fg);
    if (lv.art_ready)
    {
        perf.builds++;
        perf.build = MAX(perf.build, lv_clock() - start);
    }
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
static int lv_width(const char *start, const char *end, struct font *font)
{
    const unsigned char *p = (const unsigned char *)start;
    int width = 0;
    while (p < (const unsigned char *)end)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        if (!IS_DIACRITIC(ch)) width += font_get_width(font, ch);
    }
    return width;
}

static int lv_wrap(const char *line, int font, struct lv_row *rows)
{
    struct font *pf = font_get(font);
    const char *text = line, *previous = line;
    int n = 0, before = 0;
    do {
        const char *start = text, *end;
        text = text_wrap_next_punct(start, LCD_WIDTH - 32, pf, &end);
        before += lv_width(previous, start, pf);
        rows[n++] = (struct lv_row){start - line, end - start, before};
        previous = start;
    } while (*text && n < LV_ROWS);
    return n;
}

static const struct lv_layout *lv_layout(int line, const char *text, struct lv_layout *tmp)
{
    int slot = line % LV_LAYOUTS;
    struct lv_layout *layout = lv.layouts ? &lv.layouts[slot] : tmp;
    if (!lv.layouts || layout->line != line)
    {
        layout->line = line;
        if (lv.cues) memset(&lv.cues[slot], 0xff, sizeof(*lv.cues));
        for (int size = 0; size < 2; size++)
            layout->count[size] = lv_wrap(text, lv_font(size), layout->rows[size]);
    }
    return layout;
}

_Static_assert(LCD_WIDTH - 32 < 512, "glyph x must fit in a packed scanline entry");

/* Compact each raster scanline in place. An entry is a 9-bit x position
 * and the original 0..64 coverage. Even a solid row leaves the right
 * margin free for the end marker. */
static LV_PIXELS_ARM void lv_encode_ink(fb_data *row)
{
    int out = 0;
    for (int x = 0; x < LCD_WIDTH - 32; x++)
    {
        unsigned green = (row[x] >> 5) & 63;
        if (green) row[out++] = (x << 7) | (green + (green >> 5));
    }
    row[out] = USHRT_MAX;
}

static inline uint32_t lv_expand(fb_data color)
{
    return (color | (uint32_t)color << 16) & 0x07e0f81f;
}

static inline fb_data lv_pack(uint32_t sum)
{
    uint32_t mixed = (sum >> 5) & 0x07e0f81f;
    return mixed | (mixed >> 16);
}

/* Entries are sorted by x. Split once at the karaoke boundary. */
static inline __attribute__((always_inline))
const fb_data *lv_ink_until(const fb_data *ink, fb_data *dst, int limit,
                            unsigned opacity, uint32_t fg)
{
    for (; *ink < limit; ink++)
    {
        unsigned code = *ink;
        unsigned w = (opacity * (code & 127) + 256) >> 9;
        fb_data *pixel = dst + (code >> 7);
        *pixel = lv_pack(lv_expand(*pixel) * (32 - w) + fg * w);
    }
    return ink;
}

static LV_PIXELS_ARM void lv_ink_row(const fb_data *ink, fb_data *dst, int width,
                                     int sung, unsigned a, unsigned dim, uint32_t fg)
{
    if (sung >= 0 && sung < width && dim != a)
    {
        ink = lv_ink_until(ink, dst, sung << 7, a, fg);
        a = dim;
    }
    lv_ink_until(ink, dst, width << 7, a, fg);
}

/* Render a wrapped row once into a movable-on-screen RGB565 mask. */
static LV_PIXELS_ARM void lv_text(const char *text, int key, int font, int x, int y, int opacity,
                    int sung_width, int unsung, int top, int bottom, bool edges)
{
    int h = font_get(font)->height;
    top = MAX(top, y); bottom = MIN(bottom, y + h);
    if (opacity <= 0 || bottom <= top) return;
    if (!lv.masks)
    {
        lcd_set_viewport(&lv.vp); lcd_setfont(font); lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(color_blend(lv_background(MAX(0, y)), lv.fg, opacity));
        lcd_putsxy(x, y, text);
        return;
    }
    int slot = key < 0 ? LV_MASKS : key % LV_MASKS;
    struct lv_mask *glyph = &lv.glyphs[slot];
    fb_data *mask = lv.masks + slot * lv.mask_pixels;
    if (key < 0 || glyph->key != key)
    {
        lv.mask_fb.fb_ptr = mask;
        memset(mask, 0, h * LCD_WIDTH * sizeof(*mask));
        lcd_set_viewport(&lv.mask_vp); lcd_setfont(font);
        lcd_set_foreground(LCD_WHITE); lcd_set_drawmode(DRMODE_FG);
        lcd_putsxy(0, 0, text);
        glyph->key = key;
        perf.raster_rows++;
        for (int r = 0; r < h; r++)
            lv_encode_ink(mask + r * LCD_WIDTH);
    }
    fb_data *frame = lv_frame();
    uint32_t fg = lv_expand(lv.fg);
    for (int r = top; r < bottom; r++)
    {
        int a = edges ? opacity * lyrics_edge_opacity(r, LCD_HEIGHT) / 256 : opacity;
        lv_ink_row(mask + (r - y) * LCD_WIDTH, frame + r * LCD_WIDTH + x,
                   LCD_WIDTH - 16 - x, sung_width, a, a * unsung / 256, fg);
    }
}

struct lv_playback_sample {
    long elapsed, position;
    uint32_t sequence;
};

/* WPS supplies the interpolated remote time; local playback supplies codec
 * time. Keep the raw position separately to acknowledge a requested seek. */
static struct lv_playback_sample lv_playback_clock(const struct mp3entry *id3)
{
    struct lv_playback_sample sample = { .elapsed = id3 ? id3->elapsed : 0 };
    if (lv.remote)
        sample.sequence = cider_wps_clock(&sample.position);
    else
    {
        sample.position = sample.elapsed;
        sample.sequence = audio_seek_serial();
    }
    return sample;
}

/* Keep transport and progress on playback time. All lyrics consumers use
 * the shifted and clamped clock, after any offset carried by the document. */
static long lv_lyric_elapsed(void)
{
    int64_t time = (int64_t)lv.elapsed + global_settings.lyric_lead;
    return MAX(0, MIN(time, lv.length > 0 ? lv.length : LONG_MAX));
}

/* Find the next word boundary or pixel; the page sleeps until then. */
static int lv_word_width(int index, int font, long *next)
{
    const char *text = lyrics_text(index);
    if (!text || !lyrics_timed() || !global_settings.lyric_highlight) return -1;
    long elapsed = lv_lyric_elapsed();
    int slot = index % LV_LAYOUTS;
    unsigned short *widths = lv.cues && lv.layouts[slot].line == index ?
        lv.cues[slot].width[font == lv_font(1)] : NULL;
    int n = lyrics_word_count(index), width = 0;
    for (int w = 0; w < n; w++)
    {
        long start = lyrics_word_time(index, w);
        if (start < 0) return -1;
        long stop = w + 1 < n ? lyrics_word_time(index, w + 1) : lyrics_end_time(index);
        unsigned short *cached = widths && (unsigned)w < ARRAYLEN(lv.cues[slot].width[0]) ?
            widths + w : NULL;
        int part;
        if (cached && *cached != USHRT_MAX)
            part = *cached;
        else
        {
            const char *p = lyrics_word_text(index, w);
            const char *end = w + 1 < n ? lyrics_word_text(index, w + 1) : text + strlen(text);
            part = lv_width(p, end, font_get(font));
            if (cached && part < USHRT_MAX) *cached = part;
        }
        int sung = lyrics_word_progress(elapsed, start, stop, part);
        width += sung;
        if (elapsed < stop)
        {
            if (next)
                *next = elapsed < start ? start : part > 0 ?
                    start + ((int64_t)(sung + 1) * (stop - start) + part - 1) / part : stop;
            break;
        }
    }
    return width;
}
static int lv_sung_width(int index, int font)
{
    return lv_word_width(index, font, NULL);
}

static int lv_height(int i, int position)
{
    struct lv_layout tmp;
    const char *text = lyrics_text(i);
    if (!text) return lv.small + 12;
    const struct lv_layout *layout = lv_layout(i, text, &tmp);
    return lyrics_line_height(layout->count[0] * lv.small, layout->count[1] * lv.large,
                              lyrics_focus_weight(i, position));
}

static void lv_line(int i, int center, int position, int top, int bottom)
{
    struct lv_layout tmp;
    const char *line = lyrics_text(i);
    if (!line) return;
    const struct lv_layout *layout = lv_layout(i, line, &tmp);
    int weight = lyrics_focus_weight(i, position), rel = i * 256 - position;
    int nearest = (rel < 0 ? global_settings.lyric_prev_opacity : global_settings.lyric_next_opacity) * 256 / 100;
    int opacity = lyrics_opacity(rel, nearest);
    for (int size = 0; size < 2; size++)
    {
        int alpha = opacity * (size ? weight : 256 - weight) / 256;
        int n = layout->count[size], h = size ? lv.large : lv.small;
        int y = center - n * h / 2;
        if (!alpha || y >= bottom || y + n * h <= top) continue;
        int font = lv_font(size), sung = weight ? lv_sung_width(i, font) : -1;
        for (int r = 0; r < n; r++, y += h)
        {
            if (y >= bottom || y + h <= top) continue;
            const struct lv_row *row = &layout->rows[size][r];
            int key = (i * 2 + size) * LV_ROWS + r;
            char text[LV_TEXT];
            const char *render = NULL;
            if (!lv.masks || lv.glyphs[key % LV_MASKS].key != key)
            {
                const char *start = line + row->start;
                size_t length = MIN(row->length, sizeof(text) - 4);
                while (length && ((unsigned char)start[length] & 0xc0) == 0x80) length--;
                if (r == n - 1 && start[length] && row->length < sizeof(text) - 4)
                { strlcpy(text, start, sizeof text); lv_ellipsize(text, font, LCD_WIDTH - 32); }
                else { memcpy(text, start, length); text[length] = 0; }
                render = text;
            }
            lv_text(render, key, font, 16, y, alpha,
                    sung < 0 ? -1 : MAX(0, sung - row->before), 256 - weight * 102 / 256, top, bottom, true);
        }
    }
}

static const char *lv_status(void)
{
    return lv.track[0] ? "No lyrics" : "Nothing playing";
}
static LV_PIXELS_ARM void lv_restore(int top, int bottom)
{
    uint32_t start = lv_clock();
    fb_data *frame = lv_frame() + top * LCD_WIDTH;
    if (lv.art_ready)
        memcpy(frame, lv.background + top * LCD_WIDTH, (bottom - top) * LCD_WIDTH * sizeof(*frame));
    else for (int y = top; y < bottom; y++)
    {
        fb_data color = lv_background(y);
        for (int x = 0; x < LCD_WIDTH; x++) *frame++ = color;
    }
    lv_phase_end(LV_RESTORE, start);
}

static void lv_body(int position, int top, int bottom)
{
    int base = position / 256;
    int center = lyrics_line_y(LCD_HEIGHT * 42 / 100, position % 256,
                               lv_height(base, position), lv_height(base + 1, position));
    int count = lyrics_loaded() ? lyrics_count() : 0;
    for (int i = base, y = center; i < count && y < bottom + 100; i++)
    {
        lv_line(i, y, position, top, bottom);
        y += (lv_height(i, position) + lv_height(i + 1, position)) / 2;
    }
    for (int i = base - 1, y = center; i >= 0; i--)
    {
        y -= (lv_height(i, position) + lv_height(i + 1, position)) / 2;
        if (y < top - 100) break;
        lv_line(i, y, position, top, bottom);
    }
    if (!count && !lv.pending) lv_text(lv_status(), -1, lv_font(0), 16, LCD_HEIGHT * 42 / 100,
                        256, -1, 256, top, bottom, false);
}

static void lv_paint_backdrop(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    lv_restore(frame->clip.y, frame->clip.y + frame->clip.height);
}

static void lv_paint_body(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    uint32_t start = lv_clock();
    lv_body(frame->scroll_offset, frame->clip.y, frame->clip.y + frame->clip.height);
    lv_phase_end(LV_GLYPHS, start);
}

static void lv_paint_progress(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    lcd_set_viewport(&lv.vp);
    lcd_set_foreground(lv.accent);
    lcd_fillrect(frame->clip.x, frame->clip.y, frame->clip.width, frame->clip.height);
}

static const struct ui_view_ops lv_backdrop_ops = { .paint = lv_paint_backdrop };
static const struct ui_view_ops lv_body_ops = { .paint = lv_paint_body };
static const struct ui_view_ops lv_progress_ops = { .paint = lv_paint_progress };

static void lv_page_init(void)
{
    ui_view_init(&lv.backdrop, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT},
                 &lv_backdrop_ops, NULL);
    ui_view_init(&lv.body, (struct ui_rect){0, LV_BODY_TOP, LCD_WIDTH, LV_BODY_BOTTOM - LV_BODY_TOP},
                 &lv_body_ops, NULL);
    ui_view_init(&lv.progress, (struct ui_rect){0, LCD_HEIGHT - 2, 0, 2},
                 &lv_progress_ops, NULL);
    ui_view_append(&lv.backdrop, &lv.body);
    ui_view_append(&lv.backdrop, &lv.progress);
    ui_page_init(&lv.page, &lv.backdrop);
}

enum lv_output { LV_DRAW_ONLY, LV_UPDATE };

static uint32_t lv_line_clock(void)
{
    return lv_clock();
}

/* Repaint each changed strip from its background and submit the union once.
 * The time-syllable highlight does not rerasterize the whole page. */
static bool lv_draw_layer(bool force, enum lv_output output, fb_data *layer)
{
    int position = lyrics_motion_at(&lv.scroll, lv_line_clock());
    int progress = lv.length > 0 ? (int)((int64_t)MAX(0, MIN(lv.elapsed, lv.length)) * LCD_WIDTH / lv.length) : 0;
    int words = position % 256 == 0 ? lv_sung_width(position / 256, lv_font(1)) : -1;
    bool full = force || !lv.painted;
    bool body = full || position != lv.old_position;
    bool bar = full || progress != lv.old_progress;
    bool sung = words != lv.old_words;
    if (!body && !bar && !sung)
        return false;

    int top = LV_BODY_TOP, bottom = LV_BODY_BOTTOM;
    if (!body && sung && lv.masks)
    {
        int height = lv_height(position / 256, position) - 12;
        int y = LCD_HEIGHT * 42 / 100 - height / 2;
        top = MAX(top, y);
        bottom = MIN(bottom, y + height);
    }
    if (layer && !full)
        memcpy(lv_frame(), layer, LV_FRAME);
    lv.body.value[UI_SCROLL_OFFSET] = position;
    /* Full-width damage strips match the RGB565 background restore routine. */
    lv.progress.value[UI_WIDTH] = progress;
    if (full)
    {
        ui_page_invalidate(&lv.page, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT});
        ui_page_paint(&lv.page);
        top = 0;
        bottom = LCD_HEIGHT;
    }
    else
    {
        if (body || sung)
        {
            ui_page_invalidate(&lv.page, (struct ui_rect){0, top, LCD_WIDTH, bottom - top});
            ui_page_paint(&lv.page);
        }
        if (bar)
        {
            ui_page_invalidate(&lv.page, (struct ui_rect){0, LCD_HEIGHT - 2, LCD_WIDTH, 2});
            ui_page_paint(&lv.page);
            top = !(body || sung) ? LCD_HEIGHT - 2 : top;
            bottom = LCD_HEIGHT;
        }
    }
    if (output == LV_UPDATE)
        lv_flush(0, top, LCD_WIDTH, bottom - top);
    if (layer)
        memcpy(layer, lv_frame(), LV_FRAME);
    lv.old_position = position;
    lv.old_progress = progress;
    lv.old_words = words;
    lv.painted = true;
    return true;
}

static bool lv_draw(bool force, enum lv_output output)
{
    return lv_draw_layer(force, output, NULL);
}

static LV_PIXELS_ARM void lv_present(int opacity, int shift, const fb_data *layer,
                                    const fb_data *snapshot)
{
    uint32_t start = lv_clock();
    if (!shift && opacity == 256)
    {
        if (layer) memcpy(lv_frame(), layer, LV_FRAME);
    }
    else if (!opacity && snapshot)
    {
        memcpy(lv_frame(), snapshot, LV_FRAME);
    }
    else
    {
        fb_data *frame = lv_frame();
        for (int y = LCD_HEIGHT - 1; y >= 0; y--)
        {
            unsigned bg = lv_background(y);
            const fb_data *background = lv.art_ready ? lv.background + y * LCD_WIDTH : NULL;
            const fb_data *page = (layer ? layer : frame) + MAX(0, y - shift) * LCD_WIDTH;
            fb_data *row = frame + y * LCD_WIDTH;
            const fb_data *back = snapshot ? snapshot + y * LCD_WIDTH : NULL;
            const fb_data *source = y >= shift ? page : background;
            if (back && source)
            {
                unsigned w = (opacity + 4) >> 3;
                unsigned light = 32 - ((opacity * 110 / 256 + 4) >> 3);
                for (int x = 0; x < LCD_WIDTH; x++)
                {
                    uint32_t dark = (lv_expand(back[x]) * light >> 5) & 0x07e0f81f;
                    row[x] = lv_pack(dark * (32 - w) + lv_expand(source[x]) * w);
                }
            }
            else for (int x = 0; x < LCD_WIDTH; x++)
            {
                fb_data base = background ? background[x] : bg;
                fb_data behind = back ? lv_mix(back[x], LCD_BLACK, opacity * 110 / 256) : base;
                row[x] = lv_mix(behind, source ? source[x] : bg, opacity);
            }
        }
    }
    ui_note_render(lv_clock() - start);
    lv_phase_end(LV_BLEND, start);
    lv_flush(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

static bool lv_entry_begin(void)
{
    if (ui_scene_automatic())
        return false;
    lv.entry_frames = ui_theme_duration(LV_ENTRY_FRAMES);
    if (!lv.entry_frames)
        return false;
    int handle = core_alloc_free_space(2 * LV_FRAME);
    if (handle <= 0)
        return false;
    fb_data *snapshot = core_get_data_pinned(handle);
    memcpy(snapshot, lv_frame(), LV_FRAME);
    core_put_data_pinned(snapshot);
    lv.entry_handle = handle;
    lv.entry_frame = 0;
    lv.entry_clock = lv_clock();
    return true;
}

static void lv_entry_cancel(void)
{
    if (!lv.entry_handle)
        return;
    lv.entry_handle = core_free(lv.entry_handle);
    lv.painted = false;
}

static void lv_entry_frame(uint32_t started)
{
    int frame = ui_clock_frame((uint32_t)lv_clock() - lv.entry_clock);
    /* The early background is already the first pose. */
    if (!frame || (frame == lv.entry_frame && lv.painted))
        return;

    struct ui_tween tween = { .to = 256, .frames = lv.entry_frames };
    int alpha = ui_tween_sample(&tween, frame);
    fb_data *snapshot = core_get_data_pinned(lv.entry_handle);
    fb_data *layer = snapshot + LV_PIXELS;
    lv_draw_layer(false, LV_DRAW_ONLY, layer);
    lv_present(alpha, LV_ENTRY_SHIFT * (256 - alpha) / 256, layer, snapshot);
    core_put_data_pinned(snapshot);
    lv_record(started, LV_ENTRY, false);
    lv.entry_frame = frame;
    if ((unsigned)frame >= lv.entry_frames)
    {
        lv.entry_handle = core_free(lv.entry_handle);
        lv.last_type = LV_STEADY;
    }
}

static void lv_invalidate_text(void)
{
    if (lv.layouts)
        for (int i = 0; i < LV_LAYOUTS; i++) lv.layouts[i].line = -1;
    if (lv.glyphs) memset(lv.glyphs, 0xff, (LV_MASKS + 1) * sizeof(*lv.glyphs));
    lv.painted = false;
}

static void lv_changed(void)
{
    lv_boost(true);
    if (lv.initial)
        return;
    lv_entry_cancel();
    lv.track_frames = lv.snapshot ? ui_theme_duration(LV_TRACK_FRAMES) : 0;
    if (lv.track_frames)
        memcpy(lv.snapshot, lv_frame(), LV_FRAME);
    lv.track_clock = lv_clock();
    lv.track_frame = -1;
    lv.track_fade = lv.track_frames > 0;
}

static void lv_focus_settled(void)
{
    int position = lv.mode.focus * 256;
    lv.scroll = (struct lyrics_motion){
        .from = position, .to = position, .frame_clock = true,
    };
}

static void lv_focus(void)
{
    unsigned duration = ui_theme_duration(LV_LINE_FRAMES);
    if (duration)
        lyrics_retarget(&lv.scroll, lv.mode.focus * 256, lv_line_clock(), duration);
    else
        lv_focus_settled();
}

static LV_PIXELS_ARM void lv_blend_track(int opacity)
{
    uint32_t start = lv_clock();
    fb_data *frame = lv_frame();
    if (lv.layer)
    {
        if (opacity == 0 || opacity == 256)
            memcpy(frame, opacity ? lv.layer : lv.snapshot, LV_FRAME);
        else
            for (int i = 0; i < LV_PIXELS; i++)
                frame[i] = lv_mix(lv.snapshot[i], lv.layer[i], opacity);
    }
    else
    {
        for (int y = 0; y < LCD_HEIGHT; y++)
        {
            fb_data *row = frame + y * LCD_WIDTH;
            unsigned base = lv_background(y);
            for (int x = 0; x < LCD_WIDTH; x++) row[x] = lv_mix(base, row[x], opacity);
        }
    }
    ui_note_render(lv_clock() - start);
    lv_phase_end(LV_BLEND, start);
    lv_flush(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
#ifdef HAVE_USB_PODLINK
static void remote_request(void)
{
    char query[144];
    snprintf(query, sizeof query, "now/lyrics?nonce=%lu&lrc=1&track=%s",
             (unsigned long)remote_doc.nonce, remote_doc.id);
    cider_page_open(query);
}

void lyrics_remote_cancel(void)
{
    if (!remote_doc.id[0]) return;
    remote_doc.id[0] = 0;
    remote_doc.serial++;
    lyrics_close();
}

void lyrics_remote_track(const char *id, uint32_t generation, uint32_t library, long duration)
{
    if (!*id)
    {
        lyrics_remote_cancel();
        return;
    }
    if (generation == remote_doc.generation && library == remote_doc.library &&
        !strcmp(id, remote_doc.id))
        return;
    strlcpy(remote_doc.id, id, sizeof remote_doc.id);
    remote_doc.generation = generation; remote_doc.library = library;
    remote_doc.serial++; remote_doc.nonce++;
    remote_doc.rows = 0; remote_doc.retry = 0;
    remote_doc.first_ready = false;
    remote_doc.at = current_tick;
    remote_doc.deadline = remote_doc.at + 5 * HZ;
    remote_doc.duration = duration;
    remote_doc.done = lyrics_begin_text() != LYRICS_OK;
    remote_doc.complete_ms = UINT32_MAX;
    if (!remote_doc.done) remote_request();
}

bool lyrics_remote_step(void)
{
    if (!remote_doc.id[0] || remote_doc.done) return false;
    bool expired = !TIME_BEFORE(current_tick, remote_doc.deadline);
    if (remote_doc.retry && !expired)
    {
        if (TIME_BEFORE(current_tick, remote_doc.retry)) return false;
        remote_doc.retry = 0;
        remote_doc.nonce++;
    }
    /* Reclaim the shared browser route if another screen had used it. */
    remote_request();
    int total = cider_page_total();
    if (total < 0 && !expired) return false;
    struct cider_row row;
    bool appended = false, valid = true;
    for (int i = 0; i < 8 && remote_doc.rows < total; i++)
    {
        if (!cider_page_row(remote_doc.rows, &row)) break;
        if (!strcmp(row.id, "lyric-wait"))
        {
            if (!expired && remote_doc.rows == 0)
            {
                remote_doc.retry = current_tick + HZ / 4;
                return false;
            }
            expired = true;
            break;
        }
        if (!appended) lyrics_resume_remote();
        appended = true;
        bool more = !strcmp(row.id, "lrc1+");
        bool timed = more || !strcmp(row.id, "lrc1");
        if ((remote_doc.rows && timed != lyrics_timed()) ||
            !(timed ? lyrics_append_lrc(row.title, more) : lyrics_append_text(row.title)))
        {
            valid = !timed && !lyrics_timed();
            expired = true;
            break;
        }
        remote_doc.rows++;
        remote_doc.deadline = current_tick + 5 * HZ;
    }
    /* CN or the backend identity can change while rows are copied. */
    if (!cider_lyrics_matches(remote_doc.id, remote_doc.generation, remote_doc.library))
        return false;
    if (!valid) lyrics_begin_text();
    else if (appended && lyrics_publish_remote(remote_doc.duration) && !remote_doc.first_ready)
    {
        remote_doc.first_ready = true;
        remote_doc.first_ms = (uint32_t)(current_tick - remote_doc.at) * (1000 / HZ);
    }
    remote_doc.done = expired || (total >= 0 && remote_doc.rows == total);
    if (valid && total >= 0 && remote_doc.rows == total)
        remote_doc.complete_ms = (uint32_t)(current_tick - remote_doc.at) * (1000 / HZ);
    return appended || remote_doc.done;
}
#endif

static void lv_reload(struct mp3entry *id3, bool remote)
{
    lv_changed();
    /* Copy metadata before lyrics_load can compact the playback buffer. */
    strlcpy(lv.track, id3 ? id3->path : "", sizeof lv.track);
    lv.remote = remote; lv.pending = false;
    lv.elapsed = lv_playback_clock(id3).elapsed;
    lv.length = id3 ? id3->length : 0;
    uint32_t document_at = lv.initial ? lv_clock() : 0;
    if (remote)
    {
#ifdef HAVE_USB_PODLINK
        lyrics_remote_step();
        lv.remote_serial = remote_doc.serial;
        lv.pending = !lyrics_loaded() && !remote_doc.done && remote_doc.id[0];
#endif
    }
    else
    {
#ifdef HAVE_USB_PODLINK
        lyrics_remote_cancel();
#endif
        if (id3) lyrics_load(id3);
        else lyrics_close();
    }
    if (lv.initial) perf.entry_document_us = lv_clock() - document_at;
    lv.seek_pending = false;
    lv.mode = (struct lyrics_mode){0};
    if (lyrics_timed()) lv.mode.focus = MAX(0, lyrics_index_at(lv_lyric_elapsed()));
    lv_focus_settled();
    lv_invalidate_text();
    if (lv.track[0])
        album_palette_refresh();
    lv.art_handle = lv.track[0] ? album_art_current() : -1;
    lv.art_gen = album_art_generation();
    lv.palette_gen = album_palette_generation();
    uint32_t art_at = lv.initial ? lv_clock() : 0;
    lv_background_build(lv.art_handle);
    if (lv.initial) perf.entry_art_us = lv_clock() - art_at;
    lv_backlight(lyrics_loaded());
    lv.track_clock = lv_clock();
}
static void lv_remote_page(void)
{
#ifdef HAVE_USB_PODLINK
    if (!lv.remote || !lyrics_remote_step()) return;
    if (lv.pending)
    {
        if (!lv.entry_handle)
            lv_changed();
        lv.pending = !lyrics_loaded() && !remote_doc.done;
    }
    lv_invalidate_text();
    lv_backlight(lyrics_loaded());
#endif
}
static void lv_track_frame(uint32_t started, bool force)
{
    unsigned age = ui_clock_frame(lv_clock() - lv.track_clock);
    bool same_pose = !force && (int)age == lv.track_frame;
    int p = MIN(256, age * 256 / lv.track_frames);
    if (lv.layer)
    {
        bool changed = lv_draw_layer(false, LV_DRAW_ONLY, lv.layer);
        if (same_pose && !changed)
            return;
        lv_blend_track(p);
    }
    else
    {
        if (same_pose)
            return;
        lv_draw(true, p == 256 ? LV_UPDATE : LV_DRAW_ONLY);
        if (p < 256) lv_blend_track(p);
    }
    lv.track_frame = age;
    lv.track_fade = p < 256;
    lv_record(started, LV_TRACK, false);
}

/* The WPS supplies identical seek acceleration/release and source dispatch. */
static void lv_seek_draw(void)
{
    struct wps_state *s = get_wps_state();
    if (lv.remote && (!s->id3 || strcmp(s->id3->path, lv.track)))
        return;
    if (s->id3) lv.elapsed = lv_playback_clock(s->id3).elapsed + s->ff_rewind_count;
    if (lyrics_loaded() && lyrics_mode_follow(&lv.mode, lyrics_timed(), lyrics_index_at(lv_lyric_elapsed()), current_tick, HZ)) lv_focus();
    uint32_t started = lv_clock();
    if (lv.track_fade)
    {
        lv_boost(true);
        lv_track_frame(started, true);
        lv_boost(false);
    }
    else if (lv_draw(false, LV_UPDATE)) lv_record(started, LV_STEADY, false);
}
static void lv_transport(int action)
{
    if (action != ACTION_WPS_PLAY) lv.seek_pending = false;
    if (action == ACTION_WPS_SEEKFWD || action == ACTION_WPS_SEEKBACK)
    {
        lv_entry_cancel();
        lv_boost(false);
        wps_lyrics_seek(action, lv_seek_draw);
        return;
    }
    if (lv.remote)
    { cider_control(action == ACTION_WPS_PLAY ? "toggle" : action == ACTION_WPS_SKIPNEXT ? "next" : "previous", 0); return; }
    if (action == ACTION_WPS_PLAY) wps_do_playpause(false);
    else wps_lyrics_skip(action == ACTION_WPS_SKIPNEXT ? 1 : -1);
}

/* Remote WPS extrapolates between CN records. Only a new sample can
 * confirm the seek, and it must be closer to the target than the old clock. */
static long lv_seek_elapsed(struct lv_playback_sample clock)
{
    long elapsed = clock.elapsed;
    if (!lv.seek_pending) return elapsed;
    uint32_t age = (uint32_t)current_tick - lv.seek_tick;
    if (perf.seek_clock_us < 0)
    {
        bool arrived;
        if (lv.remote)
        {
            long old = lv.seek_from + (lv.seek_running ? (long)age * (1000 / HZ) : 0);
            arrived = clock.sequence != lv.seek_seq &&
                clock.position >= lv.seek_target - 250 && clock.position <= lv.seek_target + 750 &&
                labs(clock.position - lv.seek_target) < labs(clock.position - old);
        }
        else
            arrived = clock.sequence != lv.seek_seq &&
                elapsed >= lv.seek_target - 250 && elapsed <= lv.seek_target + 750;
        if (!arrived)
        {
            if (age < 3 * HZ) return lv.seek_target;
            lv.seek_pending = false;
            return elapsed;
        }
        perf.seek_clock_us = lv_clock() - lv.seek_at;
    }
    /* A codec can quantize backward by one frame and the remote UI can
     * lead its next sample. Hold the visual clock through that handoff. */
    if (lv.remote && age < 3 * HZ &&
        elapsed > lv.seek_target + (long)age * (1000 / HZ) + 750)
        return lv.elapsed;
    if (age >= 3 * HZ && elapsed >= lv.elapsed)
        lv.seek_pending = false;
    return MAX(elapsed, lv.elapsed);
}

static bool lv_select_seek(uint32_t pressed)
{
    if (!lv.mode.manual || !lyrics_loaded() || !lyrics_timed() ||
        (uint32_t)(current_tick - lv.mode.input) >= 4 * HZ ||
        cider_active() != lv.remote)
        return false;

    const struct mp3entry *id3 = lv.remote ? get_wps_state()->id3 :
        audio_status() & AUDIO_STATUS_PLAY ? audio_current_track() : NULL;
    if (!id3 || strcmp(id3->path, lv.track))
        return false;
#ifdef HAVE_USB_PODLINK
    if (lv.remote && (!cider_available() || remote_doc.serial != lv.remote_serial))
        return false;
#endif
    struct lv_playback_sample clock = lv_playback_clock(id3);
    lv.elapsed = clock.elapsed;
    if (lv.mode.focus == lyrics_index_at(lv_lyric_elapsed()))
        return false;

    /* Invert the display clock, including the configured lead. lyrics_time
     * already includes the document offset. */
    int64_t time = (int64_t)lyrics_time(lv.mode.focus) - global_settings.lyric_lead;
    long target = MAX(0, MIN(time, lv.length > 0 ? lv.length : LONG_MAX));
    lv.seek_seq = clock.sequence;
    lv.seek_from = lv.elapsed;
    lv.seek_running = !lv.paused;
    lv.seek_at = pressed;
    lv.seek_tick = current_tick;
    lv.seek_target = target;
    lv.seek_pending = true;
    perf.seek_remote = lv.remote;
    perf.seek_clock_us = -1;
    if (lv.remote) cider_control("seek", MIN(target, INT_MAX));
    else audio_seek_now(target);
    perf.seek_issue_us = lv_clock() - pressed;

    lv.mode.manual = false;
    lv.mode.direction = 0;
    lv_focus_settled();
    lv.elapsed = target;
    lv.track_fade = false;
    lv_entry_cancel();
    lv_boost(true);
    lv_draw(true, LV_UPDATE);
    perf.seek_frame_us = lv_clock() - pressed;
    lv_record(pressed, LV_LINE, false);
    lv_boost(false);
    return true;
}

static void lv_first_frame(uint32_t started)
{
    lv_background_build(-1);
    lv_restore(0, LCD_HEIGHT);
    lv_flush(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lv_record(started, LV_ENTRY, true);
}

static void __attribute__((noinline)) lv_load_fonts(void)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    lv.fonts[0] = ui_theme_font_load(&theme, UI_FONT_LYRICS, 64);
    lv.fonts[1] = ui_theme_font_load(&theme, UI_FONT_LYRICS_FOCUS, 64);
}

int lyric_viewer(void)
{
    uint32_t entry_at = lv_clock();
    struct lv_state state = {
        .initial = true, .entry_at = entry_at, .entry_tick = current_tick, .first_pending = true
    };
    lv_state = &state;
    lv_page_init();
    perf.seek_frame_us = -1;
    lv_boost(true);
    size_t bytes = 0; fb_data *arena = app_try_claim_buffer(&bytes, "lyrics");
    lv.claimed = arena != NULL;
    /* The page owns this arena until teardown. No audio-buffer allocation. */
    if (arena && bytes >= 2 * LV_FRAME)
    {
        lv.snapshot = arena; lv.layer = arena + LV_PIXELS;
        arena += 2 * LV_PIXELS; bytes -= 2 * LV_FRAME;
    }
    if (arena && bytes >= LV_FRAME)
    {
        lv.background = arena;
        arena += LV_PIXELS;
        bytes -= LV_FRAME;
    }
    ui_hold_scrolling(true);
    bool old_inhibit = ui_flush_inhibited(); ui_inhibit_flush(true);
    push_current_activity(ACTIVITY_LYRICS);
    viewportmanager_theme_enable(SCREEN_MAIN, false, &lv.vp);
    uint32_t frame_at = lv_clock();
    lv_first_frame(frame_at);
    uint32_t setup_resume_at = lv_clock();
    if (lv.snapshot) memcpy(lv.snapshot, lv_frame(), LV_FRAME);

    uint32_t fonts_at = lv_clock();
    lv_load_fonts();
    lv.small = font_get(lv_font(0))->height; lv.large = font_get(lv_font(1))->height;
    perf.entry_fonts_us = lv_clock() - fonts_at;
    int mask_height = MAX(lv.small, lv.large);
    lv.mask_pixels = LCD_WIDTH * mask_height;
    size_t layout_bytes = LV_LAYOUTS * sizeof(*lv.layouts);
    size_t glyph_bytes = (LV_MASKS + 1) * sizeof(*lv.glyphs);
    size_t mask_bytes = (LV_MASKS + 1) * lv.mask_pixels * sizeof(*arena);
    size_t cache_bytes = layout_bytes + glyph_bytes + mask_bytes;
    if (arena && bytes >= cache_bytes)
    {
        lv.layouts = (void *)arena;
        lv.glyphs = (void *)((char *)arena + layout_bytes);
        lv.masks = (void *)((char *)lv.glyphs + glyph_bytes);
        if (bytes - cache_bytes >= LV_LAYOUTS * sizeof(*lv.cues))
            lv.cues = (void *)((char *)arena + cache_bytes);
        lv.mask_fb = (struct frame_buffer_t){.get_address_fn = lv_mask_address, .stride = LCD_WIDTH, .elems = lv.mask_pixels};
        lv.mask_vp = lv.vp; lv.mask_vp.buffer = &lv.mask_fb;
        lv.mask_vp.width = LCD_WIDTH - 32; lv.mask_vp.height = mask_height;
        lv.mask_vp.flags = 0; lv.mask_vp.bg_pattern = LCD_BLACK;
    }
    struct wps_state *remote_wps = cider_active() ? get_wps_state() : NULL;
    unsigned status = audio_status();
    struct mp3entry *first = remote_wps ? remote_wps->id3 :
                             status & AUDIO_STATUS_PLAY ? audio_current_track() : NULL;
    lv.paused = remote_wps ? remote_wps->paused : (status & AUDIO_STATUS_PAUSE) != 0;
    lv_reload(first, remote_wps != NULL);
    perf.entry_prep_us = (frame_at - entry_at) + (lv_clock() - setup_resume_at);
    if (!lv_entry_begin())
    {
        uint32_t started = lv_clock();
        lv_draw(true, LV_UPDATE);
        lv_record(started, LV_ENTRY, false);
    }
    lv.initial = false;
    int result = GO_TO_PREVIOUS, event = 0;
    unsigned trace = 0;
    for (;;)
    {
        uint32_t frame_at = current_tick, started = lv_clock();
        struct wps_state *s = get_wps_state();
        bool remote = cider_active();
        struct mp3entry *id3 = remote ? s->id3 : audio_current_track();
        if (!remote && !(audio_status() & AUDIO_STATUS_PLAY)) id3 = NULL;
        if (!remote) s->id3 = id3;
        if (remote != lv.remote || strcmp(id3 ? id3->path : "", lv.track)
#ifdef HAVE_USB_PODLINK
            || (remote && remote_doc.serial != lv.remote_serial)
#endif
           )
        {
            lv_reload(id3, remote);
            id3 = remote ? get_wps_state()->id3 : audio_current_track();
        }
        if (id3)
        {
            lv.elapsed = lv_seek_elapsed(lv_playback_clock(id3));
            lv.length = id3->length;
        }
        lv.paused = remote ? s->paused : (audio_status() & AUDIO_STATUS_PAUSE) != 0;
        lv_remote_page();
        if (lv.track[0])
        {
            album_palette_refresh();
            unsigned palette_gen = album_palette_generation();
            int handle = album_art_current();
            unsigned art_gen = album_art_generation();
            if (palette_gen != lv.palette_gen ||
                (lv.background && (lv.remote || !lv.art_ready) &&
                 (handle != lv.art_handle || (handle >= 0 && art_gen != lv.art_gen))))
            {
                if (!lv.entry_handle)
                    lv_changed();
                lv_background_build(handle);
                lv.art_handle = handle;
                lv.palette_gen = palette_gen;
                lv.painted = false;
            }
            lv.art_gen = art_gen;
        }
        long lyric_at = lv_lyric_elapsed();
        int sung = lyrics_index_at(lyric_at);
        if ((!lv.seek_pending || perf.seek_clock_us >= 0) && lyrics_loaded() &&
            lyrics_mode_follow(&lv.mode, lyrics_timed(), sung, current_tick, HZ)) lv_focus();
        bool line_moving = lyrics_moving(&lv.scroll, lv_line_clock());
        bool animated = lv.entry_handle || lv.track_fade || line_moving;
        if (animated && !trace)
            trace = ui_perf_begin(lv.entry_handle ? "lyrics-entry" :
                                  lv.track_fade ? "lyrics-track" : "lyrics-line");
        else if (!animated && trace)
        {
            ui_perf_end(trace);
            trace = 0;
        }
        if (animated) lv_boost(true);
        if (lv.entry_handle)
            lv_entry_frame(started);
        else if (lv.track_fade)
            lv_track_frame(started, false);
        else if (lv_draw(false, LV_UPDATE))
            lv_record(started, animated ? (lv.mode.manual ? LV_MANUAL : LV_LINE) : LV_STEADY, false);
        else if (!animated)
            lv.last_type = LV_STEADY;

        if (lv.boost && !lv.entry_handle && !lv.track_fade &&
            !lyrics_moving(&lv.scroll, lv_line_clock()))
            lv_boost(false);
        int focus = lv.mode.focus;
        int timeout = lyrics_idle_ticks(lyric_at, lyrics_time(focus + 1), lv.paused, animated, false,
                                       &lv.mode, lyrics_timed(), current_tick, HZ);
        if (lv.seek_pending && perf.seek_clock_us < 0)
            timeout = MIN(timeout, MAX(1, lv.remote ? HZ / 20 : HZ / 50));
        if (!lv.paused && !animated)
        {
            long next = -1;
            lv_word_width(focus, lv_font(1), &next);
            if (next > lyric_at)
                timeout = MIN(timeout, LV_TICKS(next - lyric_at));
            timeout = MIN(timeout, lyrics_progress_ticks(lv.elapsed, lv.length, LCD_WIDTH, HZ));
        }
        if (animated)
            timeout = MAX(1, timeout - (int)((uint32_t)current_tick - frame_at));
        if (lv.entry_handle)
        {
            uint32_t elapsed = (uint32_t)lv_clock() - lv.entry_clock;
            int next_frame = ui_clock_frame(elapsed) != (unsigned)lv.entry_frame
                ? TIMEOUT_NOBLOCK : ui_clock_timeout(elapsed, lv.entry_frames, HZ);
            timeout = MIN(timeout, next_frame);
        }
        else if (lv.track_fade)
        {
            uint32_t elapsed = lv_clock() - lv.track_clock;
            int next_frame = ui_clock_frame(elapsed) != (unsigned)lv.track_frame
                ? TIMEOUT_NOBLOCK : ui_clock_poll_timeout(elapsed, lv.track_frames, HZ);
            timeout = MIN(timeout, next_frame);
        }
        else if (line_moving)
        {
            uint32_t now = lv_line_clock();
            int next_frame = ui_clock_timeout(now - lv.scroll.at, lv.scroll.duration, HZ);
            /* Service input immediately if drawing ran into the next pose. */
            if (lyrics_motion_at(&lv.scroll, now) != lv.old_position)
                next_frame = TIMEOUT_NOBLOCK;
            timeout = MIN(timeout, next_frame);
        }
        uint32_t wait_at = lv_clock();
        int action = get_action(CONTEXT_WPS, timeout);
        uint32_t pressed = lv_clock();
        lv_stat_add(&perf.phase[LV_WAIT], pressed - wait_at);
        switch (action)
        {
#ifdef BUTTON_SELECT
            case ACTION_UNKNOWN:
            {
                int button = 0;
                get_action_statuscode(&button);
                if (button == BUTTON_SELECT
#ifdef BUTTON_RC_SELECT
                    || button == BUTTON_RC_SELECT
#endif
                   )
                {
                    lv.seek_consumed = lv_select_seek(pressed);
#ifdef HAVE_WHEEL_WPS_CHORD
                    if (lv.seek_consumed && button == BUTTON_SELECT)
                        agents_shortcut_commit_select();
#endif
                }
                break;
            }
#endif
            case ACTION_WPS_BROWSE:
                if (lv.seek_consumed)
                {
                    lv.seek_consumed = false;
                    break;
                }
                if (lv_select_seek(pressed)) break;
                goto done;
            case ACTION_WPS_LYRICS: case ACTION_WPS_MENU: goto done;
#ifdef HAVE_USB_PODLINK
            case ACTION_AGENTS_REMOTE_SWITCH: case ACTION_AGENTS_SWITCH:
                agents_nav_request(AGENTS_NAV_AGENTS, action == ACTION_AGENTS_REMOTE_SWITCH);
                goto done;
            case ACTION_AGENTS_REMOTE_COVERS: case ACTION_AGENTS_COVERS:
                agents_nav_request(AGENTS_NAV_COVERS, action == ACTION_AGENTS_REMOTE_COVERS);
                goto done;
#endif
            case ACTION_WPS_VOLUP: case ACTION_WPS_VOLDOWN:
            {
                int raw_button = 0;
                get_action_statuscode(&raw_button);
                if ((raw_button & BUTTON_REMOTE) && cider_wps_action(action))
                    break;
                if (lyrics_loaded())
                {
                    int direction = action == ACTION_WPS_VOLUP ? 1 : -1;
                    if (raw_button & BUTTON_REMOTE)
                    { direction = -direction; lv.mode.direction = 0; }
                    lv.seek_pending = false;
                    lyrics_mode_scroll(&lv.mode, direction, lyrics_count(), current_tick, HZ);
                    lv_focus();
                }
                break;
            }
            case ACTION_WPS_PLAY: case ACTION_WPS_SKIPNEXT: case ACTION_WPS_SKIPPREV:
            case ACTION_WPS_SEEKFWD: case ACTION_WPS_SEEKBACK: lv_transport(action); break;
#ifdef HAVE_MULTIMEDIA_KEYS
            case BUTTON_MULTIMEDIA_PLAYPAUSE:
            case BUTTON_MULTIMEDIA_NEXT: case BUTTON_MULTIMEDIA_PREV:
            case BUTTON_MULTIMEDIA_VOLUME_UP: case BUTTON_MULTIMEDIA_VOLUME_UP | BUTTON_REPEAT:
            case BUTTON_MULTIMEDIA_VOLUME_DOWN: case BUTTON_MULTIMEDIA_VOLUME_DOWN | BUTTON_REPEAT:
                if (!cider_wps_action(action)) default_event_handler(action);
                break;
#endif
            case ACTION_REDRAW: lv.painted = false; break;
            case ACTION_NONE: break;
            default: if (IS_SYSEVENT(action)) { event = action; goto done; } break;
        }
    }
done:
    /* The caller restores its viewport and submits the next complete page. */
    ui_perf_end(trace);
    lv_entry_cancel();
    lv_boost(false);
    lv_perf_save();
    if (!lv.remote) lyrics_close();
    for (unsigned i = 0; i < ARRAYLEN(lv.fonts); i++) if (lv.fonts[i] >= 0) font_unload(lv.fonts[i]);
    lv_backlight(false); lcd_set_viewport(NULL);
    viewportmanager_theme_undo(SCREEN_MAIN, false); pop_current_activity();
    ui_hold_scrolling(false); ui_inhibit_flush(old_inhibit);
    if (lv.claimed) app_release_buffer("lyrics");
    lv_state = NULL;
    if (event && default_event_handler(event) == SYS_USB_CONNECTED) result = GO_TO_ROOT;
    return result;
}
