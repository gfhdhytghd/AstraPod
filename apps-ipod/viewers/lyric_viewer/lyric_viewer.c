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
#include "crumbs.h"
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
#include "draw/line.h"
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
#include "ui/pace.h"
#include "ui/perf.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "lyrics.h"
#include "lyrics_motion.h"
#include "lyrics_chrome.h"
#include "lyric_viewer.h"
#if defined(IPOD_6G) && defined(HAVE_USB_PODLINK)
#include "podlink-shell-playback.h"
#include "sm1-test-control.h"
#endif
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
#define LV_GAP_PERIOD ANIM_F(120)
#define LV_GAP_MIN_MS 2000
#define LV_LINE_ESTIMATE_MS 4000
#define LV_CORRECTION_MS 500
#define LV_ENTRY_FRAMES ANIM_F(13)
#define LV_TRACK_FRAMES ANIM_F(10)
#define LV_ENTRY_SHIFT 24
#define LV_ROWS 3
#define LV_TEXT 800
#define LV_LAYOUTS 61
#define LV_MASKS 127
#define LV_FULL_TOP 35
#define LV_FULL_BOTTOM (LCD_HEIGHT - 6)
#define LV_EDGE_FADE (lv.classic ? 12 : 24)
#define LV_LINE_PAD 4
#define LV_MIN_ROW (8 + LV_LINE_PAD)
#define LV_POSES ((LV_FULL_BOTTOM - LV_FULL_TOP) / LV_MIN_ROW + 8)
/* The lyrics area and the row the current line centres on, per layout. */
#define LV_BODY_TOP (lv.body_top)
#define LV_BODY_BOTTOM (lv.body_bottom)
#define LV_ANCHOR (lv.anchor)
#define LV_STRIP_WIDTH (LCD_WIDTH - 32)
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
    int line, count, width, font, height, ascent, ink_above;
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
enum lv_crumb_phase {
    LV_CRUMB_CLOSED, LV_CRUMB_OPEN, LV_CRUMB_DRAW, LV_CRUMB_LCD,
    LV_CRUMB_LINE_SEEK, LV_CRUMB_SHELL,
};
enum lv_crumb_event {
    LV_CRUMB_ENTER = 1, LV_CRUMB_EXIT, LV_CRUMB_WHEEL_START,
    LV_CRUMB_WHEEL_END, LV_CRUMB_LINE_REDRAW, LV_CRUMB_SEEK_CLOCK,
};
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
    enum lv_crumb_phase crumb_phase;
    struct viewport vp, mask_vp;
    struct frame_buffer_t mask_fb;
    struct ui_page page;
    struct ui_view backdrop, body, progress, status;
    /* style.lyrics=classic: the chrome, and the selection bar's rows. */
    struct lyrics_chrome chrome;
    bool classic, status_dirty;
    int body_top, body_bottom, anchor, band_top, band_bottom;
    unsigned band_fg;
    fb_data *snapshot, *layer, *background, *raster;
    unsigned char *masks;
    struct lv_layout *layouts;
    struct lv_mask *glyphs;
    int font, font_height, raster_height, small_font, large_font;
    int main_pitch, main_ascent, gap_height, spill;
    unsigned font_epoch;
    int font_center;
    bool font_reflow;
    unsigned bg, bottom, fg, accent;
    struct lv_pose poses[LV_POSES];
    struct lv_word word[2];
    struct lv_wheel wheel;
    int wheel_offset, highlighted, manual_prev, manual_mix;
    unsigned manual_origin, manual_outgoing, manual_progress;
    uint32_t manual_at;
    bool returning;
    uint32_t motion_at, motion_frame, painted_frame, gap_at;
    unsigned scroll_frames, stagger_frames, focus_frames, motion_frames;
    int pose_first, pose_count, pose_focus, sweep, gap_row, pose_gap;
    long vocal_elapsed;
    uint32_t vocal_at, vocal_seq;
    long vocal_raw, vocal_anchor;
    unsigned vocal_rate;
    bool vocal_clock_valid;
    int old_gap_opacity;
    bool gap, gap_transition, motion_down;
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
    int old_progress, old_words, fill;
    int entry_handle, entry_frame, track_frame;
    uint32_t entry_clock;
    unsigned entry_frames, track_frames;
    bool remote, pending, held, paused, track_fade, initial, claimed, sample_valid;
    bool painted, art_ready, first_pending, boost, seek_pending, seek_consumed, seek_running;
};
/* The only active lyrics page owns this state on the UI thread. */
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

static struct ui_pace lv_pace;

/* Ticks to the next frame presented of an animation elapsed into its run,
 * on the panel's refresh; zero once it has run its frames. */
static int lv_frame_wait(uint32_t elapsed, unsigned frames)
{
    if (ui_clock_frame(elapsed) >= frames)
        return TIMEOUT_NOBLOCK;
    uint32_t start = (uint32_t)USEC_TIMER - elapsed;
    return MAX(1, ui_clock_wait_until(ui_pace_next(&lv_pace, start)));
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

static void lv_crumb_phase(enum lv_crumb_phase phase)
{
    lv.crumb_phase = phase;
    crumb_status(CS_LYRICS, phase);
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
    enum lv_crumb_phase phase = lv.crumb_phase;
    lv_crumb_phase(LV_CRUMB_LCD);
    uint32_t start = lv_clock();
    if (w == LCD_WIDTH && h == LCD_HEIGHT) lcd_update();
    else lcd_update_rect(x, y, w, h);
    lv_crumb_phase(phase);
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
    if (lv.classic)
        return lyrics_chrome_row(&lv.chrome, y);
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
    lv.band_fg = lv_color(&theme, &palette, UI_COLOR_SELECTED_TEXT);

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
    if (!lv.background || handle < 0 || bg == UI_BG_SOLID || lv.classic)
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
static int lv_width(const char *start, const char *end, int font)
{
    return font_getstringnsize((const unsigned char *)start, end > start ? end - start : 0, NULL, NULL, font);
}

static int lv_face(bool background, bool large)
{
    if (background && lv.small_font >= 0)
        return lv.small_font;
    if (large && lv.large_font >= 0)
        return lv.large_font;
    return lv_font();
}

static int lv_ink_above(const struct font *font)
{
#ifdef HAVE_PODBOX_FONT
    return font->pbf.ink_above;
#else
    (void)font;
    return 0;
#endif
}

static int lv_ink_height(const struct font *font)
{
#ifdef HAVE_PODBOX_FONT
    return font->height + font->pbf.ink_above + font->pbf.ink_below;
#else
    return font->height;
#endif
}

static const struct lv_layout *lv_layout(int line, bool background, bool large,
                                          const char *text, struct lv_layout *tmp)
{
    int key = line * 4 + 2 * background + large;
    struct lv_layout *layout = lv.layouts ? &lv.layouts[key % LV_LAYOUTS] : tmp;
    if (!lv.layouts || layout->line != key)
    {
        layout->font = lv_face(background, large);
        font_lock(layout->font, true);
        struct font *font = font_get(layout->font);
        layout->height = lv_ink_height(font);
        layout->ink_above = lv_ink_above(font);
        layout->ascent = font->ascent + layout->ink_above;
        if (!font_prefetch_text(layout->font, text))
        {
            /* The provisional row keeps scrolling responsive. It is neither
             * painted nor saved as a completed layout. */
            memset(layout->rows, 0, sizeof(layout->rows));
            layout->width = 0;
            layout->count = 1;
            layout->line = -1;
            lv.font_reflow = true;
            font_lock(layout->font, false);
            return layout;
        }
        const char *next = text;
        int count = 0;
        do {
            const char *start = next, *end;
            next = text_wrap_next_punct(start, LV_STRIP_WIDTH, font, &end);
            int before = lv_width(text, start, layout->font);
            layout->rows[count++] = (struct lv_row){start - text, end - start, before};
            layout->width = before + MIN(LV_STRIP_WIDTH, lv_width(start, end, layout->font));
        } while (*next && count < LV_ROWS);
        font_lock(layout->font, false);
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

static const unsigned char *lv_strip(int key, const char *text, int font, int height, int ink_above,
                                     const struct lv_mask **metrics)
{
    int slot = key < 0 ? LV_MASKS : key % LV_MASKS;
    struct lv_mask *glyph = &lv.glyphs[slot];
    unsigned char *mask = lv.masks + slot * lv.raster_height * LV_STRIP_WIDTH;
    if (glyph->key != key || key < 0)
    {
        int w = MIN(LV_STRIP_WIDTH, font_getstringsize((const unsigned char *)text, NULL, NULL, font));
        memset(lv.raster, 0, height * LCD_WIDTH * sizeof(*lv.raster));
        lcd_set_viewport(&lv.mask_vp);
        lv.mask_vp.height = height;
        lcd_setfont(font);
        lcd_set_foreground(LCD_WHITE);
        lcd_set_drawmode(DRMODE_FG);
        lcd_putsxy(0, ink_above, text);
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
                                const unsigned char *levels, int ink_above)
{
    top = MAX(top, y);
    bottom = MIN(bottom, y + height);
    if (opacity <= 0 || bottom <= top) return;
    if (text && !font_prefetch_text(font, text))
        return;
    if (!lv.masks)
    {
        int width = MIN(LV_STRIP_WIDTH, font_getstringsize((const unsigned char *)text, NULL, NULL, font));
        struct viewport clip = lv.vp;
        clip.y = top;
        clip.height = bottom - top;
        struct viewport *previous = lcd_set_viewport(&clip);
        lcd_setfont(font);
        lcd_set_drawmode(DRMODE_FG);
        bool band = y + height / 2 >= lv.band_top && y + height / 2 < lv.band_bottom;
        lcd_set_foreground(band ? lv.band_fg :
                           color_blend(lv_background(MAX(0, y)), lv.fg, opacity));
        lcd_putsxy(lv_text_x(width, align), y + ink_above - top, text);
        lcd_set_viewport(previous);
        return;
    }
    const struct lv_mask *metrics;
    const unsigned char *mask = lv_strip(key, text, font, height, ink_above, &metrics);
    int first = align == 0 ? metrics->first : 0;
    int width = (align == 2 ? metrics->end : metrics->width) - first;
    if (width <= 0) return;
    mask += first;
    if (sweep >= 0) sweep = MAX(0, sweep - first);
    if (levels) levels += first;
    fb_data *frame = lv_frame() + lv_text_x(width, align);
    uint32_t ink = lv_expand(lv.fg), band = lv_expand(lv.band_fg);
    for (int r = top; r < bottom; r++)
    {
        int edge = MIN(r - LV_BODY_TOP, LV_BODY_BOTTOM - r);
        int a = key >= 0 ? opacity * MAX(0, MIN(LV_EDGE_FADE, edge)) / LV_EDGE_FADE : opacity;
        /* Whatever passes through the selection bar reads as selected. */
        bool selected = r >= lv.band_top && r < lv.band_bottom;
        uint32_t fg = selected ? band : ink;
        if (selected && a) a = 256;
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

static int lv_lyric_lead(void)
{
    return global_settings.lyric_lead;
}

/* Keep transport and progress on playback time. All lyrics consumers use
 * the shifted and clamped clock, after any offset carried by the document. */
static long lv_lyric_elapsed(void)
{
    int64_t time = (int64_t)lv.elapsed + lv_lyric_lead();
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
    if (layout->line < 0)
        return -1;
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
            int font = layout->font;
            *word = (struct lv_word){
                .line = index, .index = cue,
                .before = lv_width(text, MIN(start, limit), font),
                .width = lv_width(text, MIN(end, limit), font) - lv_width(text, MIN(start, limit), font),
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

static int lv_background_height(int line, const char *text)
{
    struct lv_layout tmp;
    const struct lv_layout *layout = lv_layout(line, true, false, text, &tmp);
    return layout->count * layout->height;
}

static int lv_main_span(int line, const char *text)
{
    struct lv_layout tmp;
    int normal = lv_layout(line, false, false, text, &tmp)->count;
    int large = lv_layout(line, false, true, text, &tmp)->count;
    return MAX(normal, large) * lv.main_pitch;
}

static int lv_height(int line)
{
    const char *text = lyrics_text(line);
    struct lyrics_vocal bg = lyrics_vocal_at(line, true);
    return (text ? lv_main_span(line, text) : lv.main_pitch) + LV_LINE_PAD +
           (bg.text ? lv_background_height(line, bg.text) : 0);
}

static int lv_distance_alpha(int index, int focus, int gap)
{
    bool previous = index < (gap >= 0 ? gap : focus);
    int distance = gap >= 0 ? abs(index - (gap - previous)) + 1 : abs(index - focus);
    /* Untimed lyrics under the classic layout are a page of text. */
    if (distance == 0 || (lv.classic && !lyrics_timed())) return 256;
    if (distance == 1)
    {
        int amount = previous ? global_settings.lyric_prev_opacity : global_settings.lyric_next_opacity;
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
    int scroll = (lv.returning || lv.gap_transition) ? lyrics_ease(MIN(age, lv.scroll_frames) * 256 / MAX(1u, lv.scroll_frames)) :
                                lyrics_spring(age, lv.scroll_frames);
    int emphasis = lyrics_ease(lv.focus_frames ? MIN(frame, lv.focus_frames) * 256 / lv.focus_frames : 256);
    pose.from_y = lyrics_lerp(pose.from_y, pose.to_y, scroll);
    int alpha = lv_distance_alpha(index, lv.pose_focus, lv.pose_gap);
    if (lv.pose_gap >= 0)
    {
        /* The insertion row owns focus even while its neighbours move. */
        pose.from_alpha = alpha;
        pose.from_focus = 0;
    }
    else
    {
        pose.from_alpha = lyrics_lerp(pose.from_alpha, alpha, emphasis);
        pose.from_focus = lyrics_lerp(pose.from_focus, index == lv.pose_focus ? 256 : 0, emphasis);
    }
    return pose;
}

static int lv_distance(int from, int to, int gap_row)
{
    int distance = 0;
    for (int i = MIN(from, to); i < MAX(from, to); i++)
        distance += (lv_height(i) + lv_height(i + 1)) / 2 +
                    (gap_row == i + 1 ? lv.gap_height : 0);
    return to < from ? -distance : distance;
}

static int lv_follow_anchor(int focus)
{
    if (!lyrics_ttml() || focus <= 0 || lv.gap_row >= 0)
        return LV_ANCHOR;
    struct lyrics_vocal current = lyrics_vocal_at(focus, false);
    struct lyrics_vocal previous = lyrics_vocal_at(focus - 1, false);
    struct lyrics_vocal background = lyrics_vocal_at(focus - 1, true);
    if (MAX(previous.end, background.end) <= current.begin)
        return LV_ANCHOR;
    /* Leave room above the focus for the overlapping voice and its sub-row. */
    int half = (lv_height(focus) - LV_LINE_PAD) / 2;
    int top = LV_BODY_TOP + LV_EDGE_FADE;
    return MAX(LV_ANCHOR, MIN(LV_BODY_BOTTOM - half, top + lv_height(focus - 1) + half));
}

/* Keep one motion. Re-target each visible row from its sampled pose. A
 * burst caught mid-wave shares one clock without re-queuing row delays. */
static void lv_motion(int focus, bool animate, uint32_t now)
{
    int from = lv.pose_focus, count = lyrics_loaded() ? lyrics_count() : 0;
    struct lv_pose old[LV_POSES];
    int old_first = lv.pose_first, old_count = lv.pose_count;
    unsigned frame = lv_motion_age(now);
    bool gap_changed = lv.gap_row != lv.pose_gap;
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
            shift = lv_distance(from, focus, lv.pose_gap);
    }
    int first = focus, y = lv_follow_anchor(focus);
    int gap_height = lv.gap_height;
    if (lv.gap_row >= 0)
    {
        int half = (lv_height(focus) + gap_height) / 2;
        y += lv.gap_row == 0 ? half : -half;
    }
    while (first > 0)
    {
        int prev_h = lv_height(first - 1);
        int prev_y = y - (prev_h + lv_height(first)) / 2
                     - (lv.gap_row == first ? gap_height : 0);
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
    lv.pose_gap = lv.gap_row;
    lv.gap_transition = gap_changed;
    lv.motion_at = now;
    lv.motion_down = focus < from;
    lv.scroll_frames = moving ? ui_theme_duration(lv.returning ? LV_RETURN_FRAMES : LV_SCROLL_FRAMES) : 0;
    lv.stagger_frames = lv.scroll_frames && !retarget && !lv.returning && !gap_changed ?
                        ui_theme_duration(LV_STAGGER_FRAMES) : 0;
    lv.focus_frames = moving ? ui_theme_duration(LV_FOCUS_FRAMES) : 0;
    for (int i = first; i < count && lv.pose_count < LV_POSES; i++)
    {
        int h = lv_height(i);
        int slot = i - old_first;
        int old_y = moving && slot >= 0 && slot < old_count ? old[slot].from_y : y + shift;
        if (MIN(y, old_y) - h / 2 >= LV_BODY_BOTTOM)
            break;
        struct lv_pose pose = {
            .from_y = y + shift, .to_y = y,
            .from_alpha = lv_distance_alpha(i, from, -1),
            .from_focus = !lv.returning && i == from ? 256 : 0,
        };
        if (moving && slot >= 0 && slot < old_count)
        {
            pose = old[slot];
            pose.to_y = y;
        }
        lv.poses[lv.pose_count++] = pose;
        y += (h + lv_height(i + 1)) / 2 + (lv.gap_row == i + 1 ? gap_height : 0);
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
    int half = lv_height(focus) / 2;
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
    lv.returning = lv.gap = false;
    lv.gap_row = -1;
    if (!continuing)
    {
        lv.highlighted = old;
        lv.manual_prev = -1;
        lv.manual_mix = 256;
        lv.manual_origin = 256;
    }
    lv.track_fade = false;
    lv_entry_cancel();
    lv.painted_frame = UINT32_MAX;
}

static bool lv_gap_update(long elapsed, int sung);

static void lv_return(int focus, uint32_t now)
{
    lv.wheel_offset = lv_wheel_at(now);
    lv.mode.manual = false;
    lv.mode.focus = focus;
    long elapsed = lv_lyric_elapsed();
    lv_gap_update(elapsed, lyrics_index_at(elapsed));
    lv.returning = true;
    lv_motion(focus, true, now);
}

static bool lv_voice_lit(struct lyrics_vocal voice, long elapsed)
{
    return voice.text && elapsed >= voice.begin && elapsed < voice.end;
}

/* Word ranges remain in logical text order. Each timed range sweeps
 * independently from its performer's side, including overlapping cues. */
static bool lv_vocal_levels(unsigned char levels[LCD_WIDTH], int i, bool background,
                            struct lyrics_vocal voice, const struct lv_row *row, int font_id)
{
    if (lv.mode.manual || !global_settings.lyric_highlight || !lv.masks)
        return false;
    long elapsed = lv.vocal_elapsed;
    bool lit = lv_voice_lit(voice, elapsed);
    memset(levels, lit && !voice.timed ? LV_SWEEP_EDGE : 0, LCD_WIDTH);
    if (!lit || !voice.timed)
        return true;

    const char *first = voice.text + row->start, *last = first + row->length;
    int x = 0;
    struct lyrics_cue cue = lyrics_vocal_cue(i, background, 0);
    for (int j = 0; j < voice.words; j++)
    {
        struct lyrics_cue next = lyrics_vocal_cue(i, background, j + 1);
        const char *end = next.text ? next.text : last;
        if (cue.text >= last) break;
        if (end > first)
        {
            int right = MIN(LV_STRIP_WIDTH, lv_width(first, MIN(end, last), font_id));
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
            x = right;
        }
        cue = next;
    }
    return true;
}

static void lv_line_block(int i, bool background, const char *line, int block_top, bool large,
                          int alpha, int sweep, int dim, int align, int top, int bottom)
{
    if (!alpha) return;
    struct lv_layout tmp;
    const struct lv_layout *layout = lv_layout(i, background, large, line, &tmp);
    struct lyrics_vocal voice = lyrics_vocal_at(i, background);
    int n = layout->count, h = layout->height;
    int pitch = background ? h : lv.main_pitch;
    int y = background ? block_top : block_top + lv.main_ascent - layout->ascent;
    for (int r = 0; r < n; r++, y += pitch)
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
                render = text_fit(start, text, sizeof(text), layout->font, LV_STRIP_WIDTH);
            else
            {
                memcpy(text, start, length);
                text[length] = 0;
                render = text;
            }
        }
        int edge = sweep < 0 ? -1 : MAX(0, sweep - row->before);
        unsigned char levels[LCD_WIDTH];
        bool vocal = voice.text && lv_vocal_levels(levels, i, background, voice, row, layout->font);
        lv_text(render, key, layout->font, h, y, alpha, edge, dim, top, bottom,
                align, vocal ? levels : NULL, layout->ink_above);
    }
}

static void lv_line(int i, struct lv_pose pose, int top, int bottom)
{
    const char *line = lyrics_text(i);
    if (!line || !pose.from_alpha) return;
    struct lyrics_vocal voice = lyrics_vocal_at(i, false), bg = lyrics_vocal_at(i, true);
    bool ready = font_prefetch_text(lv_face(false, false), line);
    ready &= font_prefetch_text(lv_face(false, true), line);
    if (bg.text)
        ready &= font_prefetch_text(lv_face(true, false), bg.text);
    if (!ready)
        return;
    int side = lyrics_agent_alignment(voice.agent);
    int align = side < 0 ? global_settings.lyric_align : side;
    int focus = pose.from_focus;
    if (focus)
    {
        struct lv_layout tmp;
        if (lv_layout(i, false, true, line, &tmp)->font == lv_font())
            focus = 0;
    }
    int height = lv_main_span(i, line);
    int bg_height = bg.text ? lv_background_height(i, bg.text) : 0;
    int y = pose.from_y - (height + bg_height) / 2;
    if (y + height + bg_height <= top || y >= bottom)
        return;
    int dim = voice.text && !lv.mode.manual ? LV_DIM : lyrics_lerp(256, LV_DIM, pose.from_focus);
    bool timed = !lv.mode.manual && !lv.gap && global_settings.lyric_highlight;
    int main_alpha = timed && lv_voice_lit(voice, lv.vocal_elapsed) ? 256 : pose.from_alpha;
    int bg_alpha = timed && lv_voice_lit(bg, lv.vocal_elapsed) ? 256 : pose.from_alpha;
    bool sweep = i == lv.pose_focus && !voice.text && lv.sweep >= 0 && !lv.mode.manual;
    int text_bottom = MIN(bottom, y + height + lv.spill);
    int text_top = MAX(top, y - lv.spill);
    if (lv.mode.manual && !lv.classic)
        align = 0;
    if (focus < 256)
        lv_line_block(i, false, line, y, false, main_alpha * (256 - focus) / 256,
                      sweep ? lv_sweep(i, lv.vocal_elapsed, false) : -1, dim, align, text_top, text_bottom);
    if (focus)
        lv_line_block(i, false, line, y, true, main_alpha * focus / 256,
                      sweep ? lv.sweep : -1, dim, align, text_top, text_bottom);
    if (bg.text)
        lv_line_block(i, true, bg.text, y + height, false,
                      bg_alpha * LV_DIM / 256, -1, LV_DIM, align, MAX(top, y + height), bottom);
}

static int lv_browse_highlight(int focus, int y)
{
    int direction = y < LV_ANCHOR ? 1 : -1;
    int count = lyrics_count();
    for (int i = focus + direction; i >= 0 && i < count; i += direction)
    {
        int next = y + direction * ((lv_height(focus) + lv_height(i)) / 2);
        if (abs(next - LV_ANCHOR) >= abs(y - LV_ANCHOR)) break;
        y = next;
        focus = i;
    }
    return focus;
}

static unsigned lv_manual_progress(uint32_t now)
{
    unsigned duration = ui_theme_duration(LV_FOCUS_FRAMES);
    unsigned age = ui_clock_frame(now - lv.manual_at);
    return duration ? lyrics_ease(MIN(age, duration) * 256 / duration) : 256;
}

static int lv_manual_emphasis(int line)
{
    if (line == lv.highlighted)
        return lv.manual_mix;
    if (line == lv.manual_prev)
        return lv.manual_outgoing * (256 - lv.manual_progress) / 256;
    return 0;
}

static bool lv_manual_focus(uint32_t now)
{
    int old = lv.manual_mix;
    int index = lv_browse_highlight(lv.mode.focus, LV_ANCHOR + lv.wheel_offset);
    unsigned progress = lv_manual_progress(now);
    bool changed = index != lv.highlighted;
    if (changed)
    {
        lv.manual_origin = index == lv.manual_prev ?
                            lv.manual_outgoing * (256 - progress) / 256 : 0;
        lv.manual_prev = lv.highlighted;
        lv.manual_outgoing = lv.manual_mix;
        lv.highlighted = index;
        lv.manual_at = now;
        progress = lv_manual_progress(now);
    }
    lv.manual_progress = progress;
    lv.manual_mix = lyrics_lerp(lv.manual_origin, 256, progress);
    return changed || old != lv.manual_mix || progress < 256;
}

static void lv_browse_body(int top, int bottom)
{
    int focus = lv.mode.focus;
    int y = LV_ANCHOR + lv.wheel_offset;
    int count = lyrics_count();
    for (int direction = -1; direction <= 1; direction += 2)
    {
        int center = y;
        int i = focus;
        do {
            int h = lv_height(i);
            if (center + h / 2 > top && center - h / 2 < bottom &&
                (direction > 0 || i != focus))
                lv_line(i, (struct lv_pose){.from_y = center,
                        .from_alpha = lv_distance_alpha(i, lv.highlighted, -1),
                        .from_focus = lv_manual_emphasis(i)}, top, bottom);
            int next = i + direction;
            if ((direction < 0 && center + h / 2 < top) ||
                (direction > 0 && center - h / 2 >= bottom) ||
                next < 0 || next >= count)
                break;
            center += direction * ((h + lv_height(next)) / 2);
            i = next;
        } while (true);
    }
    /* The margin tick stays still while glyph strips move on the grid. */
    if (!lv.classic && LV_ANCHOR >= top && LV_ANCHOR < bottom)
        lv_frame()[LV_ANCHOR * LCD_WIDTH + 6] = lv.accent;
}

static int lv_gap_opacity(uint32_t now)
{
    if (!lv.gap)
        return 0;
    unsigned span = ui_theme_duration(LV_GAP_PERIOD);
    if (!span)
        return 256;
    span = MAX(LV_GAP_PERIOD, span);
    unsigned frame = ui_clock_frame(now - lv.gap_at) % span;
    int phase = MIN(frame, span - frame) * 512 / span;
    int weight = (phase * phase * (768 - 2 * phase)) >> 16;
    return lyrics_lerp(96, 256, weight);
}

static void lv_dots(int center, int opacity, int top, int bottom)
{
    /* 8 px circle. Each entry integrates a 16x16 subpixel grid once.
     * Coverage and position within the row are independent of the breath. */
    static const uint8_t circle[8][8] = {
        { 0, 11, 45, 62, 62, 45, 11,  0},
        {11, 60, 64, 64, 64, 64, 60, 11},
        {45, 64, 64, 64, 64, 64, 64, 45},
        {62, 64, 64, 64, 64, 64, 64, 62},
        {62, 64, 64, 64, 64, 64, 64, 62},
        {45, 64, 64, 64, 64, 64, 64, 45},
        {11, 60, 64, 64, 64, 64, 60, 11},
        { 0, 11, 45, 62, 62, 45, 11,  0},
    };
    fb_data *frame = lv_frame();
    uint32_t fg = lv_expand(center >= lv.band_top && center < lv.band_bottom ?
                            lv.band_fg : lv.fg);
    const int center_x = lv_text_x(36, global_settings.lyric_align) + 18;
    for (int i = -1; i <= 1; i++)
    {
        int x = center_x + i * 14 - 4;
        for (int y = MAX(center - 4, top); y < MIN(center + 4, bottom); y++)
        {
            const uint8_t *row = circle[y - center + 4];
            for (unsigned dx = 0; dx < ARRAYLEN(circle[0]); dx++)
                if (row[dx])
                    lv_ink_pixel(frame + y * LCD_WIDTH + x + dx, row[dx], opacity, fg);
        }
    }
}

/* Use the same moving neighbour poses as the lyric list. The dots appear
 * when the inserted row has room for their complete native mask. */
static void lv_gap_body(int top, int bottom)
{
    int lo = LV_BODY_TOP, hi = LV_BODY_BOTTOM;
    for (int slot = 0; slot < lv.pose_count; slot++)
    {
        int line = lv.pose_first + slot;
        if (line != lv.gap_row - 1 && line != lv.gap_row)
            continue;
        struct lv_pose pose = lv_pose_at(slot, lv.motion_frame);
        int height = lv_height(line);
        if (line < lv.gap_row)
            lo = pose.from_y + (height + 1) / 2;
        else
            hi = pose.from_y - height / 2;
    }
    if (hi - lo >= 8)
        lv_dots(MAX(lo + 4, MIN(hi - 4, LV_ANCHOR)), lv.old_gap_opacity, top, bottom);
}

struct lv_band_pick { int above_y, above_h, below_y, below_h; };

static void lv_band_take(struct lv_band_pick *pick, int y, int h)
{
    if (y <= LV_ANCHOR && y > pick->above_y)
    {
        pick->above_y = y;
        pick->above_h = h;
    }
    else if (y > LV_ANCHOR && y < pick->below_y)
    {
        pick->below_y = y;
        pick->below_h = h;
    }
}

/* The selection bar stays on the anchor row. Its height follows the lines
 * there, blended by how near each centre is, so it grows and shrinks as a
 * wrapped line scrolls through it. */
static void lv_band(void)
{
    lv.band_top = lv.band_bottom = 0;
    if (!lv.classic || !lyrics_loaded() || !lyrics_timed() || !lyrics_count())
        return;
    /* The bar waits for its line's glyphs, so the two appear together. */
    const char *focus = lyrics_text(lv.mode.focus);
    if (focus && !font_prefetch_text(lv_face(false, false), focus))
        return;
    struct lv_band_pick pick = { INT_MIN, 0, INT_MAX, 0 };
    if (lv.mode.manual)
    {
        int count = lyrics_count();
        for (int direction = -1; direction <= 1; direction += 2)
        {
            int center = LV_ANCHOR + lv.wheel_offset, i = lv.mode.focus;
            for (;;)
            {
                int h = lv_height(i), next = i + direction;
                lv_band_take(&pick, center, h);
                if (next < 0 || next >= count || (center - LV_ANCHOR) * direction > 0)
                    break;
                center += direction * ((h + lv_height(next)) / 2);
                i = next;
            }
        }
    }
    else
        for (int slot = 0; slot < lv.pose_count; slot++)
            lv_band_take(&pick, lv_pose_at(slot, lv.motion_frame).from_y,
                         lv_height(lv.pose_first + slot));
    int height = pick.above_y == INT_MIN ? pick.below_h :
                 pick.below_y == INT_MAX ? pick.above_h :
                 lyrics_lerp(pick.above_h, pick.below_h,
                             (LV_ANCHOR - pick.above_y) * 256 / (pick.below_y - pick.above_y));
    lv.band_top = LV_ANCHOR - height / 2;
    lv.band_bottom = lv.band_top + height;
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
    lv_band();
    if (lv.band_bottom > lv.band_top)
        lyrics_chrome_band(&lv.chrome, lv.band_top, lv.band_bottom - lv.band_top,
                           top, bottom);
    if (lv.mode.manual)
    {
        lv_browse_body(top, bottom);
        return;
    }
    for (int slot = 0; slot < lv.pose_count; slot++)
    {
        int i = lv.pose_first + slot;
        lv_line(i, lv_pose_at(slot, lv.motion_frame), top, bottom);
    }
    if (lv.gap)
        lv_gap_body(top, bottom);
    if (!lv.pose_count && !lv.pending)
    {
        const char *text = lv_status();
        int ink_above = lv_ink_above(font_get(lv_font()));
        if (!font_prefetch_text(lv_font(), text))
            return;
        if (lv.classic)
        {
            /* Grey letters pressed into the page, with a white edge below. */
            int y = LV_ANCHOR - lv.font_height / 2;
            unsigned fg = lv.fg;
            lv.fg = LCD_WHITE;
            lv_text(text, -1, lv_font(), lv.font_height, y + 1, 256, -1, 256, top, bottom, 1,
                    NULL, ink_above);
            lv.fg = fg;
            lv_text(text, -1, lv_font(), lv.font_height, y, 120, -1, 256, top, bottom, 1,
                    NULL, ink_above);
        }
        else
            lv_text(text, -1, lv_font(), lv.font_height, LV_ANCHOR, 256, -1, 256, top, bottom, 1,
                    NULL, ink_above);
    }
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
    if (lv.classic)
    {
        lyrics_chrome_strip(&lv.chrome, lv.fill, frame->clip);
        return;
    }
    lcd_set_viewport(&lv.vp);
    lcd_set_foreground(lv.accent);
    lcd_fillrect(frame->clip.x, frame->clip.y, frame->clip.width, frame->clip.height);
}

static const struct ui_view_ops lv_backdrop_ops = { .paint = lv_paint_backdrop };
static const struct ui_view_ops lv_body_ops = { .paint = lv_paint_body };
static void lv_paint_status(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    lyrics_chrome_status(&lv.chrome, frame->clip);
}

static const struct ui_view_ops lv_progress_ops = { .paint = lv_paint_progress };
static const struct ui_view_ops lv_status_ops = { .paint = lv_paint_status };

/* The layout comes from style.lyrics, and the anchor also from the document. */
static void lv_geometry(void)
{
    if (!lv.classic)
    {
        lv.body_top = LV_FULL_TOP;
        lv.body_bottom = LV_FULL_BOTTOM;
        lv.anchor = LCD_HEIGHT * 42 / 100;
        return;
    }
    lv.body_top = lv.chrome.status_height;
    lv.body_bottom = LYRICS_CHROME_STRIP;
    int span = lv.body_bottom - lv.body_top;
    lv.anchor = !lyrics_loaded() ? lv.body_top + span / 2 :
                lyrics_timed() ? lv.body_top + span * 42 / 100 :
                lv.body_top + LV_EDGE_FADE + lv.main_pitch / 2;
}

static void lv_page_init(void)
{
    ui_view_init(&lv.backdrop, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT},
                 &lv_backdrop_ops, NULL);
    ui_view_init(&lv.body, (struct ui_rect){0, LV_BODY_TOP, LCD_WIDTH, LV_BODY_BOTTOM - LV_BODY_TOP},
                 &lv_body_ops, NULL);
    int strip = lv.classic ? LYRICS_CHROME_STRIP : LCD_HEIGHT - 2;
    ui_view_init(&lv.progress, (struct ui_rect){0, strip, lv.classic ? LCD_WIDTH : 0,
                                                LCD_HEIGHT - strip},
                 &lv_progress_ops, NULL);
    ui_view_append(&lv.backdrop, &lv.body);
    ui_view_append(&lv.backdrop, &lv.progress);
    if (lv.classic)
    {
        ui_view_init(&lv.status, (struct ui_rect){0, 0, LCD_WIDTH, lv.chrome.status_height},
                     &lv_status_ops, NULL);
        ui_view_append(&lv.backdrop, &lv.status);
    }
    ui_page_init(&lv.page, &lv.backdrop);
}

enum lv_output { LV_DRAW_ONLY, LV_UPDATE };

/* Repaint the body for motion and only the active band for the word sweep. */
static bool lv_vocal_window(int i, long elapsed)
{
    struct lyrics_vocal main = lyrics_vocal_at(i, false), bg = lyrics_vocal_at(i, true);
    return lv_voice_lit(main, elapsed) || lv_voice_lit(bg, elapsed);
}

static bool lv_vocal_active(long elapsed)
{
    if (!lyrics_ttml() || lv.mode.manual || !global_settings.lyric_highlight)
        return false;
    for (int i = lv.pose_first; i < lv.pose_first + lv.pose_count; i++)
        if (lv_vocal_window(i, elapsed))
            return true;
    return false;
}

static bool lv_draw_pixels(bool force, enum lv_output output, fb_data *layer)
{
    uint32_t now = lv_clock();
    lv.motion_frame = lv.mode.manual ? MIN(lv_wheel_age(now), lv_wheel_duration()) : lv_motion_age(now);
    lv.wheel_offset = lv.mode.manual ? lv_wheel_at(now) : lv.wheel_offset;
    int progress = lv.classic ? lyrics_chrome_fill(lv.elapsed, lv.length) :
                   lv.length > 0 ? (int)((int64_t)MAX(0, MIN(lv.elapsed, lv.length)) * LCD_WIDTH / lv.length) : 0;
    bool times = lv.classic && lyrics_chrome_times(&lv.chrome, lv.elapsed, lv.length);
    bool status = lv.status_dirty;
    lv.vocal_elapsed = lv_lyric_elapsed();
    lv.sweep = lv.gap ? -1 : lyrics_ttml() ?
               (lv_vocal_active(lv.vocal_elapsed) ? lv.vocal_elapsed : -1) :
               lv_sweep(lv.pose_focus, lv.vocal_elapsed, true);
    int dots = lv_gap_opacity(now);
    bool manual_changed = lv.mode.manual && lv_manual_focus(now);
    bool full = force || !lv.painted;
    bool body = full || lv.motion_frame != lv.painted_frame ||
                dots != lv.old_gap_opacity || manual_changed;
    bool bar = full || progress != lv.old_progress || times;
    bool sung = lv.sweep != lv.old_words;
    if (!body && !bar && !sung && !status)
        return false;
    lv.status_dirty = false;

    lv.old_gap_opacity = dots;
    int top = LV_BODY_TOP, bottom = LV_BODY_BOTTOM;
    /* The word clock can advance during the same scroll frame. Repaint the
     * strips where the body draws them now, before they reach the target. */
    if (!body && sung && lv.masks && lyrics_ttml())
    {
        top = LV_BODY_BOTTOM;
        bottom = LV_BODY_TOP;
        for (int slot = 0; slot < lv.pose_count; slot++)
        {
            int i = lv.pose_first + slot;
            if (!lv_vocal_window(i, lv.vocal_elapsed) &&
                !lv_vocal_window(i, lv.old_words))
                continue;
            int y = lv_pose_at(slot, lv.motion_frame).from_y;
            int height = lv_height(i) - LV_LINE_PAD;
            top = MIN(top, y - height / 2);
            bottom = MAX(bottom, y - height / 2 + height);
        }
        top = MAX(top, LV_BODY_TOP);
        bottom = MIN(bottom, LV_BODY_BOTTOM);
        if (top >= bottom) { top = LV_BODY_TOP; bottom = LV_BODY_BOTTOM; }
    }
    else if (!body && sung && lv.masks && lv.pose_count)
    {
        int height = lv_height(lv.pose_focus) - LV_LINE_PAD;
        int slot = lv.pose_focus - lv.pose_first;
        int y = lv_pose_at(slot, lv.motion_frame).from_y - height / 2;
        top = MAX(top, y);
        bottom = MIN(bottom, y + height);
    }
    if (layer && !full)
        memcpy(lv_frame(), layer, LV_FRAME);
    /* Full-width damage strips match the RGB565 background restore routine. */
    int strip = lv.classic ? LYRICS_CHROME_STRIP : LCD_HEIGHT - 2;
    lv.progress.value[UI_WIDTH] = lv.classic ? LCD_WIDTH : progress;
    lv.fill = progress;
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
            ui_page_invalidate(&lv.page, (struct ui_rect){0, strip, LCD_WIDTH, LCD_HEIGHT - strip});
            ui_page_paint(&lv.page);
            top = !(body || sung) ? strip : top;
            bottom = LCD_HEIGHT;
        }
        if (status)
        {
            int height = lv.chrome.status_height;
            ui_page_invalidate(&lv.page, (struct ui_rect){0, 0, LCD_WIDTH, height});
            ui_page_paint(&lv.page);
            if (!(body || sung || bar))
                bottom = height;
            top = 0;
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

static void lv_prefetch_row(int i)
{
    const char *text = lyrics_text(i);
    if (!text || !*text)
        return;
    font_prefetch_text(lv_face(false, false), text);
    font_prefetch_text(lv_face(false, true), text);
    struct lyrics_vocal bg = lyrics_vocal_at(i, true);
    if (bg.text)
        font_prefetch_text(lv_face(true, false), bg.text);
}

static void lv_prefetch_document(void)
{
    int count = lyrics_count();
    int first = MAX(0, lv.mode.focus - 5), end = MIN(count, lv.mode.focus + 11);
    lv.font_center = lv.mode.focus;
    font_prefetch_begin();
    /* Keep the initial viewport ahead of the rest of a long document. */
    for (int i = first; i < end; i++)
        lv_prefetch_row(i);
    for (int i = 0; i < first; i++)
        lv_prefetch_row(i);
    for (int i = end; i < count; i++)
        lv_prefetch_row(i);
    font_prefetch_text(lv_face(false, false), ".");
    font_prefetch_text(lv_face(false, true), ".");
    font_prefetch_text(lv_face(true, false), ".");
    font_prefetch_end();
}

static bool lv_draw_layer(bool force, enum lv_output output, fb_data *layer)
{
    lv_crumb_phase(LV_CRUMB_DRAW);
    font_prefetch_begin();
    unsigned epoch = font_prefetch_epoch();
    if (lv.font_epoch != epoch)
    {
        lv.font_epoch = epoch;
        force = true;
        if (lv.font_reflow)
        {
            lv.font_reflow = false;
            if (!lv.mode.manual && lyrics_loaded())
                lv_motion(lv.mode.focus, lv.returning, lv_clock());
        }
    }
    /* A later seek or scroll can put a different part of a long document
     * on screen after the initial bounded batch. */
    if (lyrics_loaded() && lv.font_center != lv.mode.focus)
    {
        lv.font_center = lv.mode.focus;
        for (int i = MAX(0, lv.mode.focus - 5);
             i < MIN(lyrics_count(), lv.mode.focus + 11); i++)
            lv_prefetch_row(i);
    }
    bool drawn = lv_draw_pixels(force, output, layer);
    font_prefetch_end();
    lv_crumb_phase(LV_CRUMB_OPEN);
    return drawn;
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

struct lv_interval { long end, next; };

static struct lv_interval lv_gap_interval(int sung)
{
    if (sung < 0)
        return (struct lv_interval){0, lyrics_time(0)};
    long start = lyrics_time(sung), next = lyrics_end_time(sung);
    long end = lyrics_vocal_end_time(sung);
    if (end < 0)
    {
        long last = lyrics_word_time(sung, lyrics_word_count(sung) - 1);
        end = MAX(start + LV_LINE_ESTIMATE_MS, last);
    }
    return (struct lv_interval){MAX(start, MIN(next, end)), next};
}

static long lv_short_switch(struct lv_interval interval)
{
    long gap = interval.next - interval.end;
    return gap > 0 && gap < LV_GAP_MIN_MS ? interval.end + gap / 2 : interval.next;
}

static int lv_follow_index(long elapsed, int sung)
{
    if (sung < 0) return 0;
    if (sung + 1 >= lyrics_count()) return sung;
    return sung + (elapsed >= lv_short_switch(lv_gap_interval(sung)));
}

static bool lv_gap_update(long elapsed, int sung)
{
    struct lv_interval interval = lv_gap_interval(sung);
    bool active = lyrics_timed() && lyrics_loaded() && !lv.mode.manual &&
        interval.next - interval.end >= LV_GAP_MIN_MS &&
        elapsed >= interval.end && elapsed < interval.next;
    if (active && lyrics_ttml())
    {
        for (int i = lv.pose_first; i < lv.pose_first + lv.pose_count; i++)
            if (lv_vocal_window(i, elapsed))
            {
                active = false;
                break;
            }
    }
    int row = active ? sung + 1 : -1;
    if (row == lv.gap_row)
        return false;
    if (active)
        lv.gap_at = lv_clock();
    lv.gap = active;
    lv.gap_row = row;
    return true;
}

static void lv_follow(long elapsed, bool jump)
{
    if (!lyrics_loaded()) return;
    int sung = lyrics_index_at(elapsed);
    int focus = lv_follow_index(elapsed, sung);
    int old_focus = lv.mode.focus;
    bool gap_changed = !lv.returning && lv_gap_update(elapsed, sung);
    if (jump && !lv.mode.manual)
    {
        lv.mode.manual = false;
        lv.mode.focus = focus;
        lv_focus(true);
    }
    else if (lv.mode.manual)
    {
        if (lyrics_timed() && (uint32_t)current_tick - lv.mode.input >= 4 * HZ)
            lv_return(focus, lv_clock());
    }
    else
    {
        if (focus < old_focus && lyrics_time(old_focus) - elapsed < LV_CORRECTION_MS)
            focus = old_focus;
        if (!lv.returning && lyrics_mode_follow(&lv.mode, lyrics_timed(), focus, current_tick, HZ))
            lv_focus(abs(lv.mode.focus - old_focus) > 4);
        else if (gap_changed)
            lv_focus(false);
    }
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
    lv.gap = lv.returning = false;
    lv.gap_row = lv.pose_gap = -1;
    lv.mode = (struct lyrics_mode){0};
    if (lyrics_timed())
    {
        long elapsed = lv_lyric_elapsed();
        lv.mode.focus = lv_follow_index(elapsed, lyrics_index_at(elapsed));
    }
    lv_invalidate_text();
    lv_geometry();
    lv_gap_update(lv_lyric_elapsed(), lyrics_index_at(lv_lyric_elapsed()));
    lv_prefetch_document();
    lv_motion(lv.mode.focus, false, lv_clock());
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
    lv_geometry();
    if (!lv.mode.manual && lyrics_timed())
        lv.mode.focus = MAX(0, lyrics_index_at(lv_lyric_elapsed()));
    lv_prefetch_document();
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
    int focus = lv_follow_index(elapsed, lyrics_index_at(elapsed));
    if (lyrics_timed())
        lv_follow(elapsed, focus != lv.mode.focus);
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
        crumb(CR_LYRICS_REDRAW, LV_CRUMB_WHEEL_START);
        lv_entry_cancel();
        lv_boost(false);
        wps_lyrics_seek(action, lv_seek_draw);
        crumb(CR_LYRICS_REDRAW, LV_CRUMB_WHEEL_END);
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
        crumb(CR_LYRICS_REDRAW, LV_CRUMB_SEEK_CLOCK);
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

    /* Invert the display clock so the selected line starts at the seek target. */
    int64_t time = (int64_t)lyrics_time(picked) - lv_lyric_lead();
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
    lv_crumb_phase(LV_CRUMB_LINE_SEEK);
    crumb(CR_LYRICS_SEEK, target);
    if (lv.remote) cider_control("seek", MIN(target, INT_MAX));
    else audio_seek_now(target);
    perf.seek_issue_us = lv_clock() - pressed;

    lv_crumb_phase(LV_CRUMB_DRAW);
    crumb(CR_LYRICS_REDRAW, LV_CRUMB_LINE_REDRAW);
    lv.mode.direction = 0;
    lv.elapsed = target;
    lv_entry_cancel();
    lv_return(picked, pressed);
    lv_boost(true);
    lv_draw(true, LV_UPDATE);
    perf.seek_frame_us = lv_clock() - pressed;
    lv_record(pressed, LV_LINE, false);
    return true;
}

/* The status bar and progress row are on the first frame, as Now Playing
 * left them. */
static void __attribute__((noinline)) lv_first_chrome(void)
{
    const struct mp3entry *id3 = cider_active() ? get_wps_state()->id3 :
        audio_status() & AUDIO_STATUS_PLAY ? audio_current_track() : NULL;
    long elapsed = id3 ? (long)id3->elapsed : 0, length = id3 ? (long)id3->length : 0;
    const struct ui_rect all = {0, 0, LCD_WIDTH, LCD_HEIGHT};
    lyrics_chrome_times(&lv.chrome, elapsed, length);
    lyrics_chrome_strip(&lv.chrome, lyrics_chrome_fill(elapsed, length), all);
    lyrics_chrome_status(&lv.chrome, all);
}

static void lv_first_frame(uint32_t started)
{
    lv_background_build(-1);
    lv_restore(0, LCD_HEIGHT);
    if (lv.classic)
        lv_first_chrome();
    lv_flush(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lv_record(started, LV_ENTRY, true);
}

static void __attribute__((noinline)) lv_open_layout(void)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    /* A colour mode other than the theme's keeps the full layout. */
    lv.classic = theme.style[UI_STYLE_LYRICS] == UI_LYRICS_CLASSIC &&
                 global_settings.lyric_colour_mode == 0;
    if (lv.classic)
        lyrics_chrome_open(&lv.chrome, &theme);
    lv_geometry();
}

static void __attribute__((noinline)) lv_load_fonts(void)
{
    lv.font = ui_theme_current_font_load(UI_FONT_LYRICS, 64);
    lv.small_font = ui_theme_current_font_load(UI_FONT_LYRICS_BACKGROUND, 32);
    lv.large_font = ui_theme_current_font_load(UI_FONT_LYRICS_FOCUS, 64);
    font_prefetch_mark_hot(lv.font);
    font_prefetch_mark_hot(lv.small_font);
    font_prefetch_mark_hot(lv.large_font);
    const struct font *normal = font_get(lv_font());
    const struct font *focus = font_get(lv_face(false, true));
    const struct font *bg = font_get(lv_face(true, false));
    /* PBF1 bearings can extend past both ends of the logical cell.
     * Keep that ink in the native mask and in the shared row pitch. */
    int normal_ascent = normal->ascent + lv_ink_above(normal);
    int focus_ascent = focus->ascent + lv_ink_above(focus);
    lv.font_height = lv_ink_height(normal);
    int focus_height = lv_ink_height(focus);
    lv.raster_height = MAX(MAX(lv.font_height, focus_height), lv_ink_height(bg));
    lv.main_ascent = MAX(normal_ascent, focus_ascent);
    lv.main_pitch = lv.main_ascent + MAX(lv.font_height - normal_ascent, focus_height - focus_ascent);
    lv.gap_height = lv.font_height + LV_LINE_PAD;
    lv.spill = 0;
    if (lv.classic)
    {
        /* Rows a font cell apart, as a list's are. Ink that reaches past
         * the cell overlaps the next row's cell instead of being cut. */
        lv.main_ascent = normal->ascent;
        lv.main_pitch = normal->height;
        lv.gap_height = lv.main_pitch + LV_LINE_PAD;
        lv.spill = lv.raster_height - normal->height;
    }
}

int lyric_viewer(void)
{
    static struct lv_state state;
    if (lv_state)
        return GO_TO_PREVIOUS;
    uint32_t entry_at = lv_clock();
    memset(&state, 0, sizeof(state));
    lv_state = &state;
    lv_crumb_phase(LV_CRUMB_OPEN);
    crumb(CR_LYRICS_REDRAW, LV_CRUMB_ENTER);
    lv.font = lv.small_font = lv.large_font = -1;
    lv.gap_row = lv.pose_gap = -1;
    lv.initial = lv.first_pending = true;
    lv.entry_at = entry_at;
    lv.entry_tick = current_tick;
    lv_open_layout();
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
    if (arena && bytes >= layout_bytes + glyph_bytes + raster_bytes + mask_bytes)
    {
        lv.layouts = (void *)arena;
        lv.glyphs = (void *)((char *)arena + layout_bytes);
        lv.raster = (void *)((char *)lv.glyphs + glyph_bytes);
        lv.masks = (void *)((char *)lv.raster + raster_bytes);
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
            if (jump)
                crumb(CR_LYRICS_REDRAW, LV_CRUMB_SEEK_CLOCK);
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
        if (lv.classic && lyrics_chrome_poll(&lv.chrome))
            lv.status_dirty = true;
        long lyric_at = lv_lyric_elapsed();
        if (lv.returning && lv.painted_frame == lv.motion_frames)
            lv.returning = false;
        if (!lv.seek_pending || perf.seek_clock_us >= 0)
            lv_follow(lyric_at, jump);
        uint32_t now = lv_clock();
        bool line_moving = lv.mode.manual ? lv_wheel_age(now) < lv_wheel_duration() ||
                           lv_manual_progress(now) < 256 : lv_motion_age(now) < lv.motion_frames;
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
                struct lv_interval interval = lv_gap_interval(sung);
                long change = interval.next;
                if (interval.next - interval.end >= LV_GAP_MIN_MS && interval.end > lyric_at)
                    change = interval.end;
                else if (sung >= 0 && sung + 1 < lyrics_count() && lv_short_switch(interval) > lyric_at)
                    change = lv_short_switch(interval);
                if (change > lyric_at) timeout = MIN(timeout, LV_TICKS(change - lyric_at));
            }
            timeout = MIN(timeout, lv.classic ?
                          LV_TICKS(lyrics_chrome_next_ms(lv.elapsed, lv.length)) :
                          lyrics_progress_ticks(lv.elapsed, lv.length, LCD_WIDTH, HZ));
        }
        if (animated || sweeping)
            timeout = MAX(1, timeout - (int)((uint32_t)current_tick - frame_at));
        /* One pace presents every animation, so a half-rate viewer keeps
         * its frame parity: a frame waits for its presentation slot. */
        uint32_t clock = lv_clock();
        if (lv.entry_handle)
            timeout = MIN(timeout, lv_frame_wait(clock - lv.entry_clock, lv.entry_frames));
        else if (lv.track_fade)
            timeout = MIN(timeout, lv_frame_wait(clock - lv.track_clock, lv.track_frames));
        else if (line_moving)
        {
            uint32_t at = lv.mode.manual ? lv.wheel.at : lv.motion_at;
            unsigned frames = lv.mode.manual ? lv_wheel_duration() : lv.motion_frames;
            /* Manual progress keeps animating after the wheel settles. */
            if (lv.mode.manual && lv_wheel_age(clock) >= frames)
                frames = UINT_MAX;
            timeout = MIN(timeout, lv_frame_wait(clock - at, frames));
        }
        else if (gap_moving)
            timeout = MIN(timeout, lv_frame_wait(clock - lv.gap_at, UINT_MAX));
        else if (sweeping)
            timeout = MIN(timeout, lv_frame_wait((uint32_t)USEC_TIMER - lv_pace.start,
                                                 UINT_MAX));
        if (font_prefetch_busy())
            timeout = timeout < 0 ? 1 : MIN(timeout, 1);
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
#if defined(IPOD_6G) && defined(HAVE_USB_PODLINK)
            case SYS_SM1_TEST_CONTROL:
            {
                unsigned command = button_get_data();
                if (command != SHELL_PLAYBACK_EVENT && command != SM1_TEST_STOP)
                {
                    event = action;
                    goto done;
                }
                lv_crumb_phase(LV_CRUMB_SHELL);
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    result = GO_TO_ROOT;
                    goto done;
                }
                lv_crumb_phase(LV_CRUMB_OPEN);
                break;
            }
#endif
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
    if (lv.large_font >= 0) font_unload(lv.large_font);
    if (lv.classic) lyrics_chrome_close(&lv.chrome);
    lv_backlight(false); lcd_set_viewport(NULL);
    viewportmanager_theme_undo(SCREEN_MAIN, false); pop_current_activity();
    ui_hold_scrolling(false); ui_inhibit_flush(old_inhibit);
    if (lv.claimed) app_release_buffer("lyrics");
    lv_crumb_phase(LV_CRUMB_CLOSED);
    crumb(CR_LYRICS_REDRAW, LV_CRUMB_EXIT);
    lv_state = NULL;
    if (event && default_event_handler(event) == SYS_USB_CONNECTED) result = GO_TO_ROOT;
    return result;
}
