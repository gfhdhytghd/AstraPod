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
#include "dir.h"
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
#define LV_SCROLL_FRAMES ANIM_F(20)
#define LV_STAGGER_FRAMES ANIM_F(2)
#define LV_FOCUS_FRAMES ANIM_F(12)
#define LV_RETURN_FRAMES ANIM_F(15)
#define LV_WHEEL_GLIDE ANIM_F(8)
#define LV_WHEEL_SPRING ANIM_F(10)
#define LV_WHEEL_LAG 3
#define LV_GAP_PERIOD ANIM_F(48)
#define LV_GAP_EXIT ANIM_F(8)
#define LV_GAP_MS 4000
#define LV_CORRECTION_MS 500
#define LV_ENTRY_FRAMES ANIM_F(13)
#define LV_TRACK_FRAMES ANIM_F(10)
#define LV_ENTRY_SHIFT 24
#define LV_ROWS 3
#define LV_TEXT 800
#define LV_LAYOUTS 61
#define LV_MASKS 127
#define LV_BODY_TOP 35
#define LV_BODY_BOTTOM (LCD_HEIGHT - 6)
#define LV_LINE_PAD 12
#define LV_POSES ((LV_BODY_BOTTOM - LV_BODY_TOP) / LV_LINE_PAD + 8)
#define LV_ANCHOR (LCD_HEIGHT * 42 / 100)
#define LV_STRIP_WIDTH (LCD_WIDTH - 32)
#define LV_FONT_CHOICES 5
#define LV_COVERAGE 128
#define LV_DIM 154          /* 60% in Q8 */
#define LV_SWEEP_EDGE 12
#define LV_LOCAL_VOCAL_LIMIT ANIM_F(6)
#if defined(IPOD_6G) && defined(__arm__)
#define LV_PIXELS_ARM __attribute__((target("arm")))
#else
#define LV_PIXELS_ARM
#endif

struct lv_row { unsigned short start, length, before; };
struct lv_layout {
    int line, count, width, font, height;
    struct lv_row rows[LV_ROWS];
};
struct lv_mask {
    int key, width, first, end;
};
struct lv_pose {
    short from_y, to_y;
    unsigned short from_alpha, from_focus;
};
struct lv_wheel {
    int from, pull;
    uint32_t at;
    unsigned glide, spring;
};
struct lv_word {
    int line, index, before, width;
    long start, end;
};
enum lv_phase { LV_RESTORE, LV_GLYPHS, LV_BLEND, LV_LCD, LV_WAIT, LV_PHASE_COUNT };
enum lv_type { LV_STEADY, LV_ENTRY, LV_LINE, LV_MANUAL, LV_TRACK, LV_SWEEP, LV_TYPE_COUNT };
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
    fb_data *snapshot, *layer, *background, *raster;
    unsigned char *masks;
    struct lv_layout *layouts;
    struct lv_mask *glyphs;
    int font, font_height, raster_height, small_font;
    int large_fonts[LV_FONT_CHOICES], large_count;
    uint32_t (*coverage)[LV_COVERAGE];
    unsigned bg, bottom, fg, accent;
    struct lv_pose poses[LV_POSES];
    struct lv_word word[2];
    struct lv_wheel wheel;
    int wheel_offset, highlighted;
    bool returning;
    uint32_t motion_at, motion_frame, painted_frame, gap_at, gap_exit_at;
    unsigned scroll_frames, stagger_frames, focus_frames, motion_frames;
    int pose_first, pose_count, pose_focus, sweep, gap_exit_scale;
    long vocal_elapsed;
    uint32_t vocal_at, vocal_seq;
    long vocal_raw, vocal_anchor;
    unsigned vocal_rate;
    bool vocal_clock_valid;
    int old_gap_scale, gap_frame;
    bool gap, gap_exit, motion_down;
    struct lyrics_mode mode;
    char track[MAX_PATH];
    long elapsed, length;
    uint32_t sample_at, sample_seq, track_clock, remote_serial, art_gen, palette_gen, seek_at, seek_tick, seek_seq;
    long seek_target, seek_from, sample_elapsed;
    uint32_t entry_at, entry_tick, present_at, phase_us[LV_WAIT];
    unsigned phase_mask;
    enum lv_type last_type;
    struct lv_perf perf;
    int art_handle;
    int old_progress, old_words;
    int entry_handle, entry_frame, track_frame;
    uint32_t entry_clock;
    unsigned entry_frames, track_frames;
    bool remote, pending, held, paused, track_fade, initial, claimed, sample_valid;
    bool painted, art_ready, first_pending, boost, seek_pending, seek_consumed, seek_running;
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
    uint32_t budget = ui_frame_clock(2);
    bool late = elapsed > budget;
    if (type != LV_STEADY && type != LV_SWEEP && lv.last_type == type)
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
    static const char *const types[] = { "steady", "entry", "line change", "manual scroll", "track change", "word sweep" };
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
    unsigned mixed = (((lo << 5) + (hi - lo) * weight) >> 5) & 0x07e0f81f;
    return mixed | (mixed >> 16);
}
static unsigned lv_background(int y)
{
    return color_blend(lv.bg, lv.bottom, y * 256 / LCD_HEIGHT);
}
static int lv_font(void)
{
    return lv.font >= 0 ? lv.font : screens[SCREEN_MAIN].getuifont();
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

static bool lv_contains(int id, const char *text, uint32_t *coverage)
{
    struct font *font = font_get(id);
    const unsigned char *p = (const unsigned char *)text;
    while (*p)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        if (ch <= ' ' || ch == 0xa0 || (ch >= 0x2000 && ch <= 0x200d))
            continue;
        uint32_t key = (uint32_t)ch << 2;
        uint32_t *entry = coverage ? coverage + (ch % LV_COVERAGE) : NULL;
        if (entry && (*entry & ~3u) == key)
        {
            if (!(*entry & 1)) return false;
            continue;
        }
        bool found = font_has_glyph(font, ch);
        if (entry) *entry = key | 2 | found;
        if (!found) return false;
    }
    return true;
}

static int lv_face(const char *text, bool background, bool large)
{
    if (background && !lv.mode.manual && lv.small_font >= 0 &&
        lv_contains(lv.small_font, text, lv.coverage ? lv.coverage[LV_FONT_CHOICES] : NULL))
        return lv.small_font;
    if (!large || background || !lv.masks)
        return lv_font();
    for (int i = 0; i < lv.large_count; i++)
        if (lv_contains(lv.large_fonts[i], text, lv.coverage ? lv.coverage[i] : NULL))
            return lv.large_fonts[i];
    return lv_font();
}

static const struct lv_layout *lv_layout(int line, bool background, bool large,
                                          const char *text, struct lv_layout *tmp)
{
    int key = line * 8 + 4 * background + 2 * large + (background && lv.mode.manual);
    struct lv_layout *layout = lv.layouts ? &lv.layouts[key % LV_LAYOUTS] : tmp;
    if (!lv.layouts || layout->line != key)
    {
        layout->font = lv_face(text, background, large);
        struct font *font = font_get(layout->font);
        layout->height = font->height;
        const char *next = text, *previous = text;
        int before = 0, count = 0;
        do {
            const char *start = next, *end;
            next = text_wrap_next_punct(start, LV_STRIP_WIDTH, font, &end);
            before += lv_width(previous, start, font);
            layout->rows[count++] = (struct lv_row){start - text, end - start, before};
            previous = start;
            layout->width = before + MIN(LV_STRIP_WIDTH, lv_width(start, end, font));
        } while (*next && count < LV_ROWS);
        layout->line = key;
        layout->count = count;
    }
    return layout;
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

static inline __attribute__((always_inline))
void lv_ink_pixel(fb_data *dst, unsigned coverage, unsigned opacity, uint32_t fg)
{
    unsigned w = (opacity * coverage + 256) >> 9;
    uint32_t base = lv_expand(*dst);
    *dst = lv_pack((base << 5) + (fg - base) * w);
}

/* Both sizes store native coverage. The painter advances one source byte
 * per destination pixel, with clipping only between the cached rows. */
static LV_PIXELS_ARM void lv_ink_row(fb_data *dst, const unsigned char *row, int width,
                                     unsigned alpha, int sweep, unsigned dim, uint32_t fg)
{
    int lit = sweep < 0 ? width : MAX(0, MIN(width, sweep - LV_SWEEP_EDGE));
    int soft = sweep < 0 ? width : MAX(lit, MIN(width, sweep));
    int x = 0;
    for (; x < lit; x++)
        if (row[x]) lv_ink_pixel(dst + x, row[x], alpha, fg);
    for (; x < soft; x++)
        if (row[x]) lv_ink_pixel(dst + x, row[x],
                                dim + (alpha - dim) * (sweep - x) / LV_SWEEP_EDGE, fg);
    for (; x < width; x++)
        if (row[x]) lv_ink_pixel(dst + x, row[x], dim, fg);
}

static const unsigned char *lv_strip(int key, const char *text, int font, int height,
                                     const struct lv_mask **metrics)
{
    int slot = key < 0 ? LV_MASKS : key % LV_MASKS;
    struct lv_mask *glyph = &lv.glyphs[slot];
    unsigned char *mask = lv.masks + slot * lv.raster_height * LV_STRIP_WIDTH;
    if (glyph->key != key || key < 0)
    {
        int w = MIN(LV_STRIP_WIDTH, lv_width(text, text + strlen(text), font_get(font)));
        memset(lv.raster, 0, height * LCD_WIDTH * sizeof(*lv.raster));
        lcd_set_viewport(&lv.mask_vp);
        lcd_setfont(font);
        lcd_set_foreground(LCD_WHITE);
        lcd_set_drawmode(DRMODE_FG);
        lcd_putsxy(0, 0, text);
        int first = w, end = 0;
        for (int y = 0; y < height; y++)
        {
            unsigned char *row = mask + y * LV_STRIP_WIDTH;
            for (int x = 0; x < w; x++)
            {
                unsigned green = (lv.raster[y * LCD_WIDTH + x] >> 5) & 63;
                row[x] = green + (green >> 5);
                if (green)
                {
                    first = MIN(first, x);
                    end = MAX(end, x + 1);
                }
            }
        }
        glyph->key = key;
        glyph->width = w;
        glyph->first = first;
        glyph->end = end;
        perf.raster_rows++;
    }
    *metrics = glyph;
    return mask;
}

static int lv_text_x(int width, int align)
{
    const int margin = (LCD_WIDTH - LV_STRIP_WIDTH) / 2;
    return align == 2 ? LCD_WIDTH - margin - width :
           align == 1 ? (LCD_WIDTH - width) / 2 : margin;
}

static LV_PIXELS_ARM void lv_text(const char *text, int key, int font, int height, int y, int opacity,
                                int sweep, int dim, int top, int bottom, int align,
                                const unsigned char *levels)
{
    top = MAX(top, y);
    bottom = MIN(bottom, y + height);
    if (opacity <= 0 || bottom <= top) return;
    if (!lv.masks)
    {
        int width = MIN(LV_STRIP_WIDTH, lv_width(text, text + strlen(text), font_get(font)));
        struct viewport clip = lv.vp;
        clip.y = top;
        clip.height = bottom - top;
        struct viewport *previous = lcd_set_viewport(&clip);
        lcd_setfont(font);
        lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(color_blend(lv_background(MAX(0, y)), lv.fg, opacity));
        lcd_putsxy(lv_text_x(width, align), y - top, text);
        lcd_set_viewport(previous);
        return;
    }
    const struct lv_mask *metrics;
    const unsigned char *mask = lv_strip(key, text, font, height, &metrics);
    int first = align == 0 ? metrics->first : 0;
    int width = (align == 2 ? metrics->end : metrics->width) - first;
    if (width <= 0) return;
    mask += first;
    if (sweep >= 0) sweep = MAX(0, sweep - first);
    if (levels) levels += first;
    fb_data *frame = lv_frame() + lv_text_x(width, align);
    uint32_t fg = lv_expand(lv.fg);
    for (int r = top; r < bottom; r++)
    {
        int edge = MIN(r - LV_BODY_TOP, LV_BODY_BOTTOM - r);
        int a = key >= 0 ? opacity * MAX(0, MIN(24, edge)) / 24 : opacity;
        if (!a) continue;
        const unsigned char *row = mask + (r - y) * LV_STRIP_WIDTH;
        fb_data *dst = frame + r * LCD_WIDTH;
        int low = a * dim / 256;
        if (!levels)
            lv_ink_row(dst, row, width, a, sweep, low, fg);
        else
            for (int x = 0; x < width; x++)
                if (row[x])
                    lv_ink_pixel(dst + x, row[x], low + (a - low) * levels[x] / LV_SWEEP_EDGE, fg);
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

/* Local PCM publishes whole chunk positions. Sample between those anchors
 * for TTML sweeps, with a limit so a stalled decoder cannot run the lyrics. */
static long lv_local_vocal_clock(long raw, uint32_t sequence, uint32_t now, bool paused)
{
    uint32_t age = now - lv.vocal_at;
    if (!lv.vocal_clock_valid || paused || lv.seek_pending ||
        sequence != lv.vocal_seq || raw < lv.vocal_raw || raw - lv.vocal_raw >= LV_CORRECTION_MS)
    {
        lv.vocal_at = now;
        lv.vocal_seq = sequence;
        lv.vocal_raw = lv.vocal_anchor = raw;
        lv.vocal_rate = 256;
        lv.vocal_clock_valid = !paused && !lv.seek_pending;
        return raw;
    }
    long time = lv.vocal_anchor +
        MIN(age, ui_frame_clock(LV_LOCAL_VOCAL_LIMIT)) * lv.vocal_rate / 256000;
    if (raw != lv.vocal_raw)
    {
        if (age >= ui_frame_clock(ANIM_F(1)))
        {
            long measured = (raw - lv.vocal_raw) * 256000 / (long)age;
            long correction = MAX(-32, MIN(32, raw - time)) * 3;
            lv.vocal_rate = MAX(64, MIN(384, measured + correction));
        }
        lv.vocal_raw = raw;
        lv.vocal_anchor = time;
        lv.vocal_at = now;
    }
    return time;
}

/* Keep transport and progress on playback time. All lyrics consumers use
 * the shifted and clamped clock, after any offset carried by the document. */
static long lv_lyric_elapsed(void)
{
    int64_t time = (int64_t)lv.elapsed + global_settings.lyric_lead;
    return MAX(0, MIN(time, lv.length > 0 ? lv.length : LONG_MAX));
}

/* Measure a cue when it becomes current, then sample that measurement
 * against FU99's playback clock. The parser can expose an untimed prefix. */
static int lv_sweep(int index, long elapsed, bool large)
{
    const char *text = lyrics_text(index);
    int count = lyrics_word_count(index);
    if (!text || !lyrics_timed() || lv.mode.manual || !global_settings.lyric_highlight ||
        (count <= 1 && lyrics_word_time(index, 0) < 0))
        return -1;

    struct lv_layout tmp;
    const struct lv_layout *layout = lv_layout(index, false, large, text, &tmp);
    struct lv_word *word = &lv.word[large];
    if (word->line != index || elapsed < word->start || elapsed >= word->end)
    {
        int cue = word->line == index && elapsed >= word->start ? word->index : 0;
        while (cue + 1 < count && lyrics_word_time(index, cue + 1) <= elapsed)
            cue++;
        if (word->line != index || word->index != cue)
        {
            const char *start = lyrics_word_text(index, cue);
            const struct lv_row *last = &layout->rows[layout->count - 1];
            const char *limit = text + last->start + last->length;
            const char *end = cue + 1 < count ? lyrics_word_text(index, cue + 1) : limit;
            struct font *font = font_get(layout->font);
            *word = (struct lv_word){
                .line = index, .index = cue,
                .before = lv_width(text, MIN(start, limit), font),
                .width = lv_width(start, MIN(end, limit), font),
                .start = lyrics_word_time(index, cue),
                .end = cue + 1 < count ? lyrics_word_time(index, cue + 1) : lyrics_end_time(index),
            };
            if (word->start < 0) word->start = lyrics_time(index);
        }
    }
    if (word->before >= layout->width)
        return layout->width + LV_SWEEP_EDGE;
    int feather = word->before + word->width >= layout->width ? LV_SWEEP_EDGE : 0;
    return word->before + lyrics_word_progress(elapsed, word->start, word->end, word->width + feather);
}

static int lv_block_height(int line, bool background, bool large, const char *text)
{
    if (!text) return lv.font_height;
    struct lv_layout tmp;
    const struct lv_layout *layout = lv_layout(line, background, large, text, &tmp);
    return layout->count * layout->height;
}

static int lv_height(int line, int emphasis)
{
    const char *text = lyrics_text(line);
    int height = lv_block_height(line, false, false, text);
    if (emphasis)
        height = lyrics_lerp(height, lv_block_height(line, false, true, text), emphasis);
    struct lyrics_vocal bg = lyrics_vocal_at(line, true);
    if (bg.text)
        height += lv_block_height(line, true, false, bg.text);
    return height + LV_LINE_PAD;
}

static int lv_distance_alpha(int index, int focus)
{
    int distance = abs(index - focus);
    if (distance == 0) return 256;
    if (distance == 1)
    {
        int amount = index < focus ? global_settings.lyric_prev_opacity :
                                    global_settings.lyric_next_opacity;
        return (amount * 256 + 50) / 100;
    }
    return distance == 2 ? 102 : 64;
}

static unsigned lv_motion_age(uint32_t now)
{
    return MIN(lv.motion_frames, ui_clock_frame(now - lv.motion_at));
}

static struct lv_pose lv_pose_at(int slot, unsigned frame)
{
    struct lv_pose pose = lv.poses[slot];
    int index = lv.pose_first + slot;
    unsigned delay = (lv.motion_down ? lv.pose_count - 1 - slot : slot) * lv.stagger_frames;
    unsigned age = frame > delay ? frame - delay : 0;
    int scroll = lv.returning ? lyrics_ease(MIN(age, lv.scroll_frames) * 256 / MAX(1u, lv.scroll_frames)) :
                                lyrics_spring(age, lv.scroll_frames);
    int emphasis = lyrics_ease(lv.focus_frames ? MIN(frame, lv.focus_frames) * 256 / lv.focus_frames : 256);
    pose.from_y = lyrics_lerp(pose.from_y, pose.to_y, scroll);
    pose.from_alpha = lyrics_lerp(pose.from_alpha, lv_distance_alpha(index, lv.pose_focus), emphasis);
    pose.from_focus = lyrics_lerp(pose.from_focus, index == lv.pose_focus ? 256 : 0, emphasis);
    return pose;
}

static int lv_distance(int from, int to, int emphasized)
{
    int distance = 0;
    for (int i = MIN(from, to); i < MAX(from, to); i++)
        distance += (lv_height(i, i == emphasized ? 256 : 0) +
                     lv_height(i + 1, i + 1 == emphasized ? 256 : 0)) / 2;
    return to < from ? -distance : distance;
}

/* Keep one motion. Re-target each visible row from its sampled pose. A
 * burst caught mid-wave shares one clock without re-queuing row delays. */
static void lv_motion(int focus, bool animate, uint32_t now)
{
    int from = lv.pose_focus, count = lyrics_loaded() ? lyrics_count() : 0;
    struct lv_pose old[LV_POSES];
    int old_first = lv.pose_first, old_count = lv.pose_count;
    unsigned frame = lv_motion_age(now);
    bool moving = animate && (lv.returning || (old_count && abs(focus - from) <= 4));
    bool retarget = moving && frame < lv.motion_frames;
    for (int i = 0; moving && !lv.returning && i < old_count; i++)
        old[i] = lv_pose_at(i, frame);
    if (lv.returning) old_count = 0;
    int shift = 0;
    if (moving)
    {
        if (lv.returning)
        {
            shift = abs(focus - from) <= 4 ? lv_distance(from, focus, -1) :
                    (focus > from ? LV_BODY_BOTTOM - LV_ANCHOR : LV_BODY_TOP - LV_ANCHOR);
            shift += lv.wheel_offset;
        }
        else
            shift = lv_distance(from, focus, from);
    }
    int first = focus, y = LV_ANCHOR;
    while (first > 0)
    {
        int prev_h = lv_height(first - 1, 0);
        int prev_y = y - (prev_h + lv_height(first, first == focus ? 256 : 0)) / 2;
        int slot = first - 1 - old_first;
        int old_y = moving && slot >= 0 && slot < old_count ? old[slot].from_y : prev_y + shift;
        if (MAX(prev_y, old_y) + prev_h / 2 <= LV_BODY_TOP)
            break;
        y = prev_y;
        first--;
    }
    lv.pose_count = 0;
    lv.pose_first = first;
    lv.pose_focus = focus;
    lv.motion_at = now;
    lv.motion_down = focus < from;
    lv.scroll_frames = moving ? ui_theme_duration(lv.returning ? LV_RETURN_FRAMES : LV_SCROLL_FRAMES) : 0;
    lv.stagger_frames = lv.scroll_frames && !retarget && !lv.returning ?
                        ui_theme_duration(LV_STAGGER_FRAMES) : 0;
    lv.focus_frames = moving ? ui_theme_duration(LV_FOCUS_FRAMES) : 0;
    for (int i = first; i < count && lv.pose_count < LV_POSES; i++)
    {
        int h = lv_height(i, i == focus ? 256 : 0);
        int slot = i - old_first;
        int old_y = moving && slot >= 0 && slot < old_count ? old[slot].from_y : y + shift;
        if (MIN(y, old_y) - h / 2 >= LV_BODY_BOTTOM)
            break;
        struct lv_pose pose = {
            .from_y = y + shift, .to_y = y,
            .from_alpha = lv_distance_alpha(i, from),
            .from_focus = !lv.returning && i == from ? 256 : 0,
        };
        if (moving && slot >= 0 && slot < old_count)
        {
            pose = old[slot];
            pose.to_y = y;
        }
        lv.poses[lv.pose_count++] = pose;
        y += (h + lv_height(i + 1, 0)) / 2;
    }
    lv.motion_frames = MAX(lv.focus_frames, lv.scroll_frames + MAX(0, lv.pose_count - 1) * lv.stagger_frames);
    lv.painted_frame = UINT32_MAX;
}

static unsigned lv_wheel_duration(void)
{
    return lv.wheel.glide + (lv.wheel.pull ? lv.wheel.spring : 0);
}

static unsigned lv_wheel_age(uint32_t now)
{
    return ui_clock_frame(now - lv.wheel.at);
}

static int lv_wheel_at(uint32_t now)
{
    unsigned age = lv_wheel_age(now);
    if (age < lv.wheel.glide)
    {
        const struct ui_tween move = {.from = lv.wheel.from, .to = lv.wheel.pull,
                                      .frames = lv.wheel.glide};
        return ui_tween_sample(&move, age);
    }
    const struct ui_tween spring = {.from = lv.wheel.pull, .frames = lv.wheel.spring};
    return ui_tween_sample(&spring, age - lv.wheel.glide);
}

static void lv_entry_cancel(void);

static void lv_browse(int direction, uint32_t now)
{
    bool continuing = lv.mode.manual;
    int old = lv.mode.focus;
    int offset = continuing ? lv_wheel_at(now) : 0;
    if (continuing)
        now = lv.wheel.at + ui_frame_clock(lv_wheel_age(now));
    int count = lyrics_count();
    int step = lyrics_wheel_step(&lv.mode, direction, current_tick, HZ);
    int requested = old + direction * step;
    lv.mode.manual = true;
    lv.mode.focus = MAX(0, MIN(count - 1, requested));
    int focus = lv.mode.focus;
    int from = offset + lv_distance(old, focus, -1);
    int min_from = lv_distance(MIN(count - 1, focus + LV_WHEEL_LAG), focus, -1);
    int max_from = lv_distance(MAX(0, focus - LV_WHEEL_LAG), focus, -1);
    int half = lv_height(focus, 0) / 2;
    int pull = 0;
    if (requested < 0 || requested >= count)
    {
        if (continuing && old == focus)
            pull = MAX(0, -direction * lv.wheel.pull);
        pull += (half - pull + 1) / 2;
        pull *= -direction;
    }
    lv.wheel = (struct lv_wheel) {
        .from = MAX(min_from - half, MIN(max_from + half, from)),
        .pull = pull, .at = now,
        .glide = ui_theme_duration(LV_WHEEL_GLIDE),
        .spring = ui_theme_duration(LV_WHEEL_SPRING),
    };
    if (!lv.wheel.glide) lv.wheel.pull = 0;
    lv.focus_frames = lv.stagger_frames = lv.scroll_frames = lv.motion_frames = 0;
    lv.pose_focus = focus;
    lv.returning = lv.gap = lv.gap_exit = false;
    lv.track_fade = false;
    lv_entry_cancel();
    lv.painted_frame = UINT32_MAX;
}

static void lv_return(int focus, uint32_t now)
{
    lv.wheel_offset = lv_wheel_at(now);
    lv.mode.manual = false;
    lv.mode.focus = focus;
    lv.returning = true;
    lv_motion(focus, true, now);
}

/* Word ranges remain in logical text order. Each timed range sweeps
 * independently from its performer's side, including overlapping cues. */
static bool lv_vocal_levels(unsigned char levels[LCD_WIDTH], int i, bool background,
                            struct lyrics_vocal voice, const struct lv_row *row, int font_id)
{
    if (lv.mode.manual || !global_settings.lyric_highlight ||
        (!voice.timed && !background) || !lv.masks)
        return false;
    long elapsed = lv.vocal_elapsed;
    if (!background && (elapsed < voice.begin || elapsed > voice.end))
        return false;
    memset(levels, 0, LCD_WIDTH);
    if (background && (elapsed < voice.begin || elapsed >= voice.end))
        return true;
    struct font *font = font_get(font_id);
    const char *first = voice.text + row->start, *last = first + row->length;
    const char *at = first;
    int width = 0;
    struct lyrics_cue cue = lyrics_vocal_cue(i, background, 0);
    for (int j = 0; j < voice.words; j++)
    {
        struct lyrics_cue next = lyrics_vocal_cue(i, background, j + 1);
        const char *end = next.text ? next.text : last;
        if (cue.text >= last) break;
        if (end > at)
        {
            int x = MIN(LV_STRIP_WIDTH, width);
            const char *limit = MIN(end, last);
            width += lv_width(at, limit, font);
            at = limit;
            int right = MIN(LV_STRIP_WIDTH, width);
            long begin = cue.begin < 0 ? voice.begin : cue.begin;
            long stop = cue.end >= begin ? cue.end :
                        next.begin > begin ? next.begin : voice.end;
            int run = right - x;
            int progress = cue.timed ? lyrics_word_progress(elapsed, begin, stop, run + LV_SWEEP_EDGE) :
                           elapsed >= begin && elapsed < stop ? run + LV_SWEEP_EDGE : 0;
            int dir = lyrics_agent_alignment(cue.agent);
            for (int col = 0; col < run; col++)
            {
                int distance = dir == 2 ? run - 1 - col : col;
                levels[x + col] = MAX(0, MIN(LV_SWEEP_EDGE, progress - distance));
            }
        }
        cue = next;
    }
    return true;
}

static void lv_line_block(int i, bool background, const char *line, int center, bool large,
                          int alpha, int sweep, int dim, int align, int top, int bottom)
{
    if (!alpha) return;
    struct lv_layout tmp;
    const struct lv_layout *layout = lv_layout(i, background, large, line, &tmp);
    struct lyrics_vocal voice = lyrics_vocal_at(i, background);
    int n = layout->count, h = layout->height;
    int y = center - n * h / 2;
    for (int r = 0; r < n; r++, y += h)
    {
        if (y >= bottom || y + h <= top) continue;
        const struct lv_row *row = &layout->rows[r];
        int key = layout->line * LV_ROWS + r;
        char text[LV_TEXT];
        const char *render = NULL;
        if (!lv.masks || lv.glyphs[key % LV_MASKS].key != key)
        {
            const char *start = line + row->start;
            size_t length = MIN(row->length, sizeof(text) - 4);
            while (length && ((unsigned char)start[length] & 0xc0) == 0x80) length--;
            if (r == n - 1 && start[length] && row->length < sizeof(text) - 4)
            { strlcpy(text, start, sizeof text); lv_ellipsize(text, layout->font, LV_STRIP_WIDTH); }
            else { memcpy(text, start, length); text[length] = 0; }
            render = text;
        }
        int edge = sweep < 0 ? -1 : MAX(0, sweep - row->before);
        unsigned char levels[LCD_WIDTH];
        bool vocal = voice.text && lv_vocal_levels(levels, i, background, voice, row, layout->font);
        lv_text(render, key, layout->font, h, y, alpha, edge, dim, top, bottom,
                align, vocal ? levels : NULL);
    }
}

static void lv_line(int i, struct lv_pose pose, int top, int bottom)
{
    const char *line = lyrics_text(i);
    if (!line || !pose.from_alpha) return;
    struct lyrics_vocal voice = lyrics_vocal_at(i, false), bg = lyrics_vocal_at(i, true);
    int side = lyrics_agent_alignment(voice.agent);
    int align = side < 0 ? global_settings.lyric_align : side;
    int focus = pose.from_focus;
    if (focus)
    {
        struct lv_layout tmp;
        if (lv_layout(i, false, true, line, &tmp)->font == lv_font())
            focus = 0;
    }
    int height = lv_block_height(i, false, false, line);
    if (focus)
        height = lyrics_lerp(height, lv_block_height(i, false, true, line), focus);
    int bg_height = bg.text ? lv_block_height(i, true, false, bg.text) : 0;
    int y = pose.from_y - (height + bg_height) / 2;
    if (y + height + bg_height + LV_LINE_PAD / 2 <= top || y - LV_LINE_PAD / 2 >= bottom)
        return;
    int dim = voice.text ? LV_DIM : lyrics_lerp(256, LV_DIM, pose.from_focus);
    bool sweep = i == lv.pose_focus && !voice.text && lv.sweep >= 0;
    int text_bottom = MIN(bottom, y + height + LV_LINE_PAD / 2);
    int text_top = MAX(top, y - LV_LINE_PAD / 2);
    if (focus < 256)
        lv_line_block(i, false, line, y + height / 2, false, pose.from_alpha * (256 - focus) / 256,
                      sweep ? lv_sweep(i, lv.vocal_elapsed, false) : -1, dim, align, text_top, text_bottom);
    if (focus)
        lv_line_block(i, false, line, y + height / 2, true, pose.from_alpha * focus / 256,
                      sweep ? lv.sweep : -1, dim, align, text_top, text_bottom);
    if (bg.text)
        lv_line_block(i, true, bg.text, y + height + bg_height / 2, false,
                      pose.from_alpha * LV_DIM / 256, -1, LV_DIM, align, top, bottom);
}

static int lv_browse_highlight(int focus, int y)
{
    int direction = y < LV_ANCHOR ? 1 : -1;
    int count = lyrics_count();
    for (int i = focus + direction; i >= 0 && i < count; i += direction)
    {
        int next = y + direction * ((lv_height(focus, 0) + lv_height(i, 0)) / 2);
        if (abs(next - LV_ANCHOR) >= abs(y - LV_ANCHOR)) break;
        y = next;
        focus = i;
    }
    return focus;
}

static void lv_browse_body(int top, int bottom)
{
    int focus = lv.mode.focus;
    int y = LV_ANCHOR + lv.wheel_offset;
    int count = lyrics_count();
    lv.highlighted = lv_browse_highlight(focus, y);
    for (int direction = -1; direction <= 1; direction += 2)
    {
        int center = y;
        int i = focus;
        do {
            int h = lv_height(i, 0);
            if (center + h / 2 > top && center - h / 2 < bottom &&
                (direction > 0 || i != focus))
                lv_line(i, (struct lv_pose){.from_y = center,
                        .from_alpha = lv_distance_alpha(i, lv.highlighted)}, top, bottom);
            int next = i + direction;
            if ((direction < 0 && center + h / 2 < top) ||
                (direction > 0 && center - h / 2 >= bottom) ||
                next < 0 || next >= count)
                break;
            center += direction * ((h + lv_height(next, 0)) / 2);
            i = next;
        } while (true);
    }
    /* The margin tick stays still while glyph strips move on the grid. */
    if (LV_ANCHOR >= top && LV_ANCHOR < bottom)
        lv_frame()[LV_ANCHOR * LCD_WIDTH + 6] = lv.accent;
}

static int lv_gap_scale(uint32_t now)
{
    if (!lv.gap) return 0;
    unsigned span = ui_theme_duration(LV_GAP_PERIOD);
    if (!span) return 256;
    unsigned frame = ui_clock_frame(now - lv.gap_at) % span;
    int phase = MIN(frame, span - frame) * 512 / span;
    /* A smooth triangle has zero velocity at both ends of the breath. */
    int weight = (phase * phase * (768 - 2 * phase)) >> 16;
    return lyrics_lerp(205, 256, weight);
}

static void lv_dots(int center, int scale, int alpha, int top, int bottom)
{
    fb_data *frame = lv_frame();
    uint32_t fg = lv_expand(lv.fg);
    int radius = 4 * scale;
    int span = 36;
    int x_center = lv_text_x(span, global_settings.lyric_align) + span / 2;
    for (int i = -1; i <= 1; i++)
    {
        int x = x_center + i * 14 * scale / 256;
        for (int y = MAX(center - 4, top); y < MIN(center + 5, bottom); y++)
            for (int dx = -4; dx <= 4; dx++)
            {
                int dy = y - center;
                int cover = (radius * radius - (dx * dx + dy * dy) * 65536) / 2048;
                if (cover > 0)
                    lv_ink_pixel(frame + y * LCD_WIDTH + x + dx, MIN(64, cover), alpha, fg);
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
    else for (int y = top; y < bottom; y++, frame += LCD_WIDTH)
        memset16(frame, lv_background(y), LCD_WIDTH);
    lv_phase_end(LV_RESTORE, start);
}

static void lv_body(int top, int bottom)
{
    if (lv.mode.manual)
    {
        lv_browse_body(top, bottom);
        return;
    }
    for (int slot = 0; slot < lv.pose_count; slot++)
    {
        int i = lv.pose_first + slot;
        if (i == lv.pose_focus && lv.gap) continue;
        lv_line(i, lv_pose_at(slot, lv.motion_frame), top, bottom);
    }
    if (lv.gap)
        lv_dots(LV_ANCHOR, lv.old_gap_scale, 256, top, bottom);
    if (lv.gap_exit)
    {
        unsigned duration = ui_theme_duration(LV_GAP_EXIT);
        int p = lyrics_ease(duration ? MIN((unsigned)lv.gap_frame, duration) * 256 / duration : 256);
        lv_dots(LV_ANCHOR, lyrics_lerp(lv.gap_exit_scale, 0, p), 256 - p, top, bottom);
    }
    if (!lv.pose_count && !lv.pending)
        lv_text(lv_status(), -1, lv_font(), lv.font_height, LV_ANCHOR, 256, -1, 256, top, bottom, 1, NULL);
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
    lv_body(frame->clip.y, frame->clip.y + frame->clip.height);
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

/* Repaint the body for motion and only the active band for the word sweep. */
static bool lv_vocal_window(int i, long elapsed, bool words_only)
{
    struct lyrics_vocal main = lyrics_vocal_at(i, false), bg = lyrics_vocal_at(i, true);
    return (main.text && (!words_only || main.timed) && elapsed >= main.begin && elapsed < main.end) ||
           (bg.text && elapsed >= bg.begin && elapsed < bg.end);
}

static bool lv_vocal_active(long elapsed)
{
    if (!lyrics_ttml() || lv.mode.manual || !global_settings.lyric_highlight)
        return false;
    for (int i = lv.pose_first; i < lv.pose_first + lv.pose_count; i++)
        if (lv_vocal_window(i, elapsed, true))
            return true;
    return false;
}

static bool lv_draw_layer(bool force, enum lv_output output, fb_data *layer)
{
    uint32_t now = lv_clock();
    lv.motion_frame = lv.mode.manual ? MIN(lv_wheel_age(now), lv_wheel_duration()) : lv_motion_age(now);
    lv.wheel_offset = lv.mode.manual ? lv_wheel_at(now) : lv.wheel_offset;
    int progress = lv.length > 0 ? (int)((int64_t)MAX(0, MIN(lv.elapsed, lv.length)) * LCD_WIDTH / lv.length) : 0;
    lv.vocal_elapsed = lv_lyric_elapsed();
    lv.sweep = lyrics_ttml() ? (lv_vocal_active(lv.vocal_elapsed) ? lv.vocal_elapsed : -1) :
               lv.gap ? -1 : lv_sweep(lv.pose_focus, lv.vocal_elapsed, true);
    int dots = lv_gap_scale(now);
    bool full = force || !lv.painted;
    int fold = lv.gap_exit ? (int)ui_clock_frame(now - lv.gap_exit_at) : -1;
    bool body = full || lv.motion_frame != lv.painted_frame || dots != lv.old_gap_scale ||
                fold != lv.gap_frame;
    bool bar = full || progress != lv.old_progress;
    bool sung = lv.sweep != lv.old_words;
    if (!body && !bar && !sung)
        return false;

    lv.old_gap_scale = dots;
    lv.gap_frame = fold;
    int top = LV_BODY_TOP, bottom = LV_BODY_BOTTOM;
    if (!body && sung && lv.masks && lyrics_ttml())
    {
        top = LV_BODY_BOTTOM;
        bottom = LV_BODY_TOP;
        for (int slot = 0; slot < lv.pose_count; slot++)
        {
            int i = lv.pose_first + slot;
            if (!lv_vocal_window(i, lv.vocal_elapsed, true) &&
                !lv_vocal_window(i, lv.old_words, true))
                continue;
            int y = lv.poses[slot].to_y;
            int height = lv_height(i, i == lv.pose_focus ? 256 : 0) - LV_LINE_PAD;
            top = MIN(top, y - height / 2);
            bottom = MAX(bottom, y - height / 2 + height);
        }
        top = MAX(top, LV_BODY_TOP);
        bottom = MIN(bottom, LV_BODY_BOTTOM);
        if (top >= bottom) { top = LV_BODY_TOP; bottom = LV_BODY_BOTTOM; }
    }
    else if (!body && sung && lv.masks)
    {
        int height = lv_height(lv.pose_focus, 256) - LV_LINE_PAD;
        int y = LV_ANCHOR - height / 2;
        top = MAX(top, y);
        bottom = MIN(bottom, y + height);
    }
    if (layer && !full)
        memcpy(lv_frame(), layer, LV_FRAME);
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
    lv.painted_frame = lv.motion_frame;
    lv.old_progress = progress;
    lv.old_words = lv.sweep;
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
    lv.word[0].line = lv.word[1].line = -1;
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

static void lv_focus(bool crossfade)
{
    lv.returning = false;
    if (crossfade)
        lv_changed();
    lv_motion(lv.mode.focus, !crossfade, lv_clock());
}

static void lv_gap_update(long elapsed, int sung, bool jump)
{
    bool active = lyrics_timed() && lyrics_loaded() && !lv.mode.manual &&
        (sung < 0 ? lyrics_time(0) >= LV_GAP_MS :
         elapsed - lyrics_time(sung) >= LV_GAP_MS && elapsed < lyrics_end_time(sung));
    if (active && lyrics_ttml())
    {
        for (int i = lv.pose_first; i < lv.pose_first + lv.pose_count; i++)
            if (lv_vocal_window(i, elapsed, false))
            {
                active = false;
                break;
            }
    }
    if (lv.gap && !active && !jump)
    {
        lv.gap_exit_at = lv_clock();
        lv.gap_exit_scale = lv_gap_scale(lv.gap_exit_at);
        lv.gap_exit = ui_theme_duration(LV_GAP_EXIT) > 0;
    }
    if (active && !lv.gap)
    {
        lv.gap_at = lv_clock();
        lv.gap_exit = false;
    }
    lv.gap = active;
}

static void lv_follow(long elapsed, bool jump)
{
    if (!lyrics_loaded()) return;
    int sung = lyrics_index_at(elapsed);
    int old_focus = lv.mode.focus;
    if (jump && !lv.mode.manual)
    {
        lv.mode.manual = false;
        lv.mode.focus = MAX(0, sung);
        lv.gap_exit = false;
        lv_focus(true);
    }
    else if (lv.mode.manual)
    {
        if (lyrics_timed() && (uint32_t)current_tick - lv.mode.input >= 4 * HZ)
            lv_return(MAX(0, sung), lv_clock());
    }
    else
    {
        if (sung < old_focus &&
            lyrics_time(old_focus) - elapsed < LV_CORRECTION_MS)
            sung = old_focus;
        if (!lv.returning && lyrics_mode_follow(&lv.mode, lyrics_timed(), MAX(0, sung), current_tick, HZ))
            lv_focus(abs(lv.mode.focus - old_focus) > 4);
    }
    lv_gap_update(elapsed, sung, jump);
}

/* Sequence changes can also be regular remote anchors. Compare the new
 * sample with the running visual clock before interpreting one as a seek. */
static bool lv_clock_jump(struct lv_playback_sample clock, uint32_t now)
{
    long predicted = lv.sample_elapsed + (lv.paused ? 0 : (now - lv.sample_at) / 1000);
    bool jump = lv.sample_valid && lv.sample_seq != clock.sequence &&
                labs(clock.elapsed - predicted) >= LV_CORRECTION_MS;
    lv.sample_at = now;
    lv.sample_seq = clock.sequence;
    lv.sample_elapsed = clock.elapsed;
    lv.sample_valid = true;
    return jump;
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
    snprintf(query, sizeof query, "now/lyrics?nonce=%lu&lrc=1&duet=1&track=%s",
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
        bool ttml = !strcmp(row.id, "ttml2") || !strcmp(row.id, "ttml2+");
        bool more = !strcmp(row.id, ttml ? "ttml2+" : "lrc1+");
        bool timed = ttml || more || !strcmp(row.id, "lrc1");
        if ((remote_doc.rows && (timed != lyrics_timed() || ttml != lyrics_ttml())) ||
            !(ttml ? lyrics_append_ttml(row.title, more, atoi(row.subtitle)) :
              timed ? lyrics_append_lrc(row.title, more) : lyrics_append_text(row.title)))
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
    bool complete = total >= 0 && remote_doc.rows == total;
    if (lyrics_ttml() && (complete || expired) && !lyrics_ttml_complete())
        valid = false;
    if (!valid) lyrics_begin_text();
    else if (appended && lyrics_publish_remote(remote_doc.duration) && !remote_doc.first_ready)
    {
        remote_doc.first_ready = true;
        remote_doc.first_ms = (uint32_t)(current_tick - remote_doc.at) * (1000 / HZ);
    }
    remote_doc.done = expired || complete;
    if (valid && complete)
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
    lv.sample_valid = false;
    lv.vocal_clock_valid = false;
    lv.gap = lv.gap_exit = lv.returning = false;
    lv.mode = (struct lyrics_mode){0};
    if (lyrics_timed()) lv.mode.focus = MAX(0, lyrics_index_at(lv_lyric_elapsed()));
    lv_invalidate_text();
    lv_motion(lv.mode.focus, false, lv_clock());
    lv_gap_update(lv_lyric_elapsed(), lyrics_index_at(lv_lyric_elapsed()), true);
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
    if (!lv.mode.manual && lyrics_timed())
        lv.mode.focus = MAX(0, lyrics_index_at(lv_lyric_elapsed()));
    if (!lv.mode.manual)
    {
        lv.returning = false;
        lv_motion(lv.mode.focus, false, lv_clock());
    }
    lv_backlight(lyrics_loaded());
#endif
}
static void lv_track_frame(uint32_t started, bool force)
{
    unsigned age = ui_clock_frame(lv_clock() - lv.track_clock);
    if (lv.layer && lv.track_frame < 0)
    {
        /* Draw the seek destination once. Intermediate frames only mix
         * the two cached images. Playback keeps running through the fade. */
        /* Start a new destination from a full draw. Partial damage may
         * reuse this layer only after its non-lyrics bands are initialized. */
        lv_draw_layer(true, LV_DRAW_ONLY, lv.layer);
        lv.track_clock = lv_clock();
        age = 0;
    }
    else if (lv.layer && force)
        lv_draw_layer(false, LV_DRAW_ONLY, lv.layer);
    else if ((int)age == lv.track_frame)
        return;

    int p = MIN(256, age * 256 / lv.track_frames);
    if (lv.layer)
        lv_blend_track(p);
    else
    {
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
    long elapsed = lv_lyric_elapsed();
    int focus = MAX(0, lyrics_index_at(elapsed));
    if (lyrics_timed() && focus != lv.mode.focus)
        lv_follow(elapsed, true);
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
    int picked = lv_browse_highlight(lv.mode.focus, LV_ANCHOR + lv_wheel_at(pressed));
    if (picked == lyrics_index_at(lv_lyric_elapsed()))
    {
        lv_return(picked, pressed);
        return true;
    }

    /* Invert the display clock, including the configured lead. lyrics_time
     * already includes the document offset. */
    int64_t time = (int64_t)lyrics_time(picked) - global_settings.lyric_lead;
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

    lv.mode.direction = 0;
    lv.elapsed = target;
    lv_entry_cancel();
    lv_return(picked, pressed);
    lv_gap_update(lv_lyric_elapsed(), picked, true);
    lv_boost(true);
    lv_draw(true, LV_UPDATE);
    perf.seek_frame_us = lv_clock() - pressed;
    lv_record(pressed, LV_LINE, false);
    return true;
}

static void lv_first_frame(uint32_t started)
{
    lv_background_build(-1);
    lv_restore(0, LCD_HEIGHT);
    lv_flush(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lv_record(started, LV_ENTRY, true);
}

struct lv_candidate {
    char path[MAX_PATH];
    unsigned height;
};

static bool lv_font_header(const char *path, unsigned *height)
{
    unsigned char header[8];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = read(fd, header, sizeof header) == sizeof header && !memcmp(header, "RB12", 4);
    close(fd);
    if (ok) *height = header[6] | (unsigned)header[7] << 8;
    return ok;
}

static void __attribute__((noinline)) lv_load_fonts(void)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    lv.font = ui_theme_font_load(&theme, UI_FONT_LYRICS, 64);
    lv.font_height = font_get(lv_font())->height;
    lv.raster_height = lv.font_height;
    lv.small_font = font_load_ex(FONT_DIR "/12-noto-sans.fnt", 0, 32);
    if (lv.small_font >= 0)
        lv.raster_height = MAX(lv.raster_height, (int)font_get(lv.small_font)->height);

    struct lv_candidate choices[LV_FONT_CHOICES];
    unsigned count = 0;
    DIR *dir = opendir(FONT_DIR);
    if (!dir) return;
    struct dirent *file;
    while ((file = readdir(dir)))
    {
        size_t len = strlen(file->d_name);
        if (len <= 4 || strcasecmp(file->d_name + len - 4, ".fnt") ||
            sizeof(FONT_DIR "/") + len > sizeof(choices[0].path))
            continue;
        struct lv_candidate candidate;
        memcpy(candidate.path, FONT_DIR "/", sizeof(FONT_DIR "/") - 1);
        memcpy(candidate.path + sizeof(FONT_DIR "/") - 1, file->d_name, len + 1);
        if (!lv_font_header(candidate.path, &candidate.height) ||
            candidate.height > (unsigned)lv.font_height * 125 / 100 ||
            candidate.height * 100 < (unsigned)lv.font_height * 110)
            continue;
        unsigned i = 0;
        while (i < count && (choices[i].height < candidate.height ||
               (choices[i].height == candidate.height && strcmp(choices[i].path, candidate.path) < 0)))
            i++;
        if (i >= LV_FONT_CHOICES) continue;
        if (count < LV_FONT_CHOICES) count++;
        memmove(choices + i + 1, choices + i, (count - i - 1) * sizeof(*choices));
        choices[i] = candidate;
    }
    closedir(dir);

    for (unsigned i = 0; i < count; i++)
    {
        int id = font_load_ex(choices[i].path, 0, 64);
        if (id < 0) continue;
        lv.large_fonts[lv.large_count++] = id;
        lv.raster_height = MAX(lv.raster_height, (int)font_get(id)->height);
    }
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
    perf.entry_fonts_us = lv_clock() - fonts_at;
    size_t raster_pixels = LCD_WIDTH * lv.raster_height;
    size_t layout_bytes = LV_LAYOUTS * sizeof(*lv.layouts);
    size_t glyph_bytes = (LV_MASKS + 1) * sizeof(*lv.glyphs);
    size_t raster_bytes = raster_pixels * sizeof(*lv.raster);
    size_t mask_bytes = (LV_MASKS + 1) * lv.raster_height * LV_STRIP_WIDTH;
    size_t coverage_bytes = (LV_FONT_CHOICES + 1) * sizeof(*lv.coverage);
    if (arena && bytes >= layout_bytes + glyph_bytes + raster_bytes + mask_bytes + coverage_bytes)
    {
        lv.layouts = (void *)arena;
        lv.glyphs = (void *)((char *)arena + layout_bytes);
        lv.raster = (void *)((char *)lv.glyphs + glyph_bytes);
        lv.coverage = (void *)((char *)lv.raster + raster_bytes);
        memset(lv.coverage, 0, coverage_bytes);
        lv.masks = (void *)((char *)lv.coverage + coverage_bytes);
        lv.mask_fb = (struct frame_buffer_t){.fb_ptr = lv.raster, .get_address_fn = lv_mask_address,
            .stride = LCD_WIDTH, .elems = raster_pixels};
        lv.mask_vp = lv.vp;
        lv.mask_vp.buffer = &lv.mask_fb;
        lv.mask_vp.width = LV_STRIP_WIDTH;
        lv.mask_vp.height = lv.raster_height;
        lv.mask_vp.flags = 0;
        lv.mask_vp.bg_pattern = LCD_BLACK;
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
        bool paused = remote ? s->paused : (audio_status() & AUDIO_STATUS_PAUSE) != 0;
        bool jump = false;
        if (id3)
        {
            struct lv_playback_sample clock = lv_playback_clock(id3);
            jump = lv_clock_jump(clock, started) && !lv.seek_pending;
            lv.elapsed = lv_seek_elapsed(clock);
            if (!remote && lyrics_ttml())
                lv.elapsed = lv_local_vocal_clock(lv.elapsed, clock.sequence, started, paused);
            lv.length = id3->length;
        }
        lv.paused = paused;
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
        if (lv.returning && lv.painted_frame == lv.motion_frames)
            lv.returning = false;
        if (!lv.seek_pending || perf.seek_clock_us >= 0)
            lv_follow(lyric_at, jump);
        uint32_t now = lv_clock();
        unsigned gap_frames = ui_theme_duration(LV_GAP_EXIT);
        if (lv.gap_exit && ui_clock_frame(now - lv.gap_exit_at) >= gap_frames)
        {
            lv.gap_exit = false;
            lv.painted_frame = UINT32_MAX;
        }
        bool line_moving = lv.mode.manual ? lv_wheel_age(now) < lv_wheel_duration() :
                           lv_motion_age(now) < lv.motion_frames || lv.gap_exit;
        bool gap_moving = lv.gap && ui_theme_duration(LV_GAP_PERIOD);
        bool animated = lv.entry_handle || lv.track_fade || line_moving || gap_moving;
        bool sweeping = !lv.paused && !lv.gap &&
                        (lyrics_ttml() ? lv_vocal_active(lyric_at) :
                                         lv_sweep(lv.pose_focus, lyric_at, true) >= 0);
        if (animated && !trace)
            trace = ui_perf_begin(lv.entry_handle ? "lyrics-entry" :
                                  lv.track_fade ? "lyrics-track" : "lyrics-line");
        else if (!animated && trace)
        {
            ui_perf_end(trace);
            trace = 0;
        }
        if (animated || sweeping) lv_boost(true);
        if (lv.entry_handle)
            lv_entry_frame(started);
        else if (lv.track_fade)
            lv_track_frame(started, false);
        else if (lv_draw(false, LV_UPDATE))
            lv_record(started, animated ? (lv.mode.manual ? LV_MANUAL : LV_LINE) :
                      lv.sweep != -1 ? LV_SWEEP : LV_STEADY, false);
        else if (!animated)
            lv.last_type = LV_STEADY;

        if (lv.boost && !lv.entry_handle && !lv.track_fade &&
            !line_moving && !gap_moving && !sweeping)
            lv_boost(false);
        int focus = lv.mode.focus;
        int timeout = lyrics_idle_ticks(lyric_at, lyrics_time(focus + 1), lv.paused, animated, sweeping,
                                       &lv.mode, lyrics_timed(), current_tick, HZ);
        if (lv.seek_pending && perf.seek_clock_us < 0)
            timeout = MIN(timeout, MAX(1, lv.remote ? HZ / 20 : HZ / 50));
        if (!lv.paused && !animated)
        {
            if (!lv.mode.manual && lyrics_timed())
            {
                int sung = lyrics_index_at(lyric_at);
                long next = sung < 0 ? lyrics_time(0) : lyrics_end_time(sung);
                long gap = sung < 0 ? next : lyrics_time(sung) + LV_GAP_MS;
                if (gap > lyric_at) next = MIN(next, gap);
                if (next > lyric_at) timeout = MIN(timeout, LV_TICKS(next - lyric_at));
            }
            timeout = MIN(timeout, lyrics_progress_ticks(lv.elapsed, lv.length, LCD_WIDTH, HZ));
        }
        if (animated || sweeping)
            timeout = MAX(1, timeout - (int)((uint32_t)current_tick - frame_at));
        if (sweeping)
            timeout = MIN(timeout, MAX(1, HZ / (int)UI_ANIM_FPS));
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
            uint32_t clock = lv_clock();
            uint32_t at = lv.mode.manual ? lv.wheel.at : lv.gap_exit ? lv.gap_exit_at : lv.motion_at;
            unsigned frames = lv.mode.manual ? lv_wheel_duration() : lv.gap_exit ? gap_frames : lv.motion_frames;
            int next_frame = ui_clock_timeout(clock - at, frames, HZ);
            unsigned age = lv.mode.manual ? MIN(lv_wheel_age(clock), frames) : lv_motion_age(clock);
            if (age != lv.painted_frame)
                next_frame = TIMEOUT_NOBLOCK;
            timeout = MIN(timeout, next_frame);
        }
        else if (gap_moving)
            timeout = MIN(timeout, MAX(1, HZ / (int)UI_ANIM_FPS));
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
                    lv_browse(direction, pressed);
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
    if (lv.font >= 0) font_unload(lv.font);
    if (lv.small_font >= 0) font_unload(lv.small_font);
    for (int i = 0; i < lv.large_count; i++) font_unload(lv.large_fonts[i]);
    lv_backlight(false); lcd_set_viewport(NULL);
    viewportmanager_theme_undo(SCREEN_MAIN, false); pop_current_activity();
    ui_hold_scrolling(false); ui_inhibit_flush(old_inhibit);
    if (lv.claimed) app_release_buffer("lyrics");
    lv_state = NULL;
    if (event && default_event_handler(event) == SYS_USB_CONNECTED) result = GO_TO_ROOT;
    return result;
}
