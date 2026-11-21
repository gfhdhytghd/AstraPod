"""Render the native handoff sheet and exercise input and framebuffer ownership."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)


def block(text, pattern):
    match = re.search(pattern, text, re.M)
    assert match, pattern
    end, depth = text.index('{', match.start()) + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def function(text, name):
    return block(text, r'^\w[\w *]+\b' + name + r'\([^;{}]*\)\s*\{')


def header(path, start):
    return (ROOT / path).read_text().split(start, 1)[1].split('#endif', 1)[0]


action_source = (ROOT / 'apps-ipod/input/action.c').read_text()
shutdown = (ROOT / 'apps-ipod/system/shutdown.c').read_text()
accessory_case = shutdown.split('case SYS_ACCESSORY_CONNECTED:', 1)[1].split('return SYS_ACCESSORY_CONNECTED;', 1)[0]
filter_release = action_source.split('    if (last->wait_for_release)\n', 1)[1].split(
    '    else if', 1)[0].split('   /*', 1)[0]
live_entry = 'void action_handoff_enter(void)' in action_source
draw = (ROOT / 'apps-ipod/screens/playback/handoff_sheet.c').read_text()
native_entry = 'static void close_native_sheet(' in draw
retained_entry = 'static void retained_sheet_init(' in draw
themed_entry = 'void load_native_style(' in draw
dimmed_entry = 'SHEET_DIM_PERCENT' in draw
if dimmed_entry:
    present = function(draw, 'present_sheet')
    draw = draw.replace(present, present.replace(
        '            ui_nav_draw(&s->nav);',
        '''            frame_offset = offset;
            frame_brightness = s->below.value[UI_BRIGHTNESS];
            ui_nav_draw(&s->nav);
            frame_offset = -1;''').replace(
        '        else\n            frame_offset = offset;',
        '        else {\n            frame_offset = offset;').replace(
        '            frame_offset = -1;\n    }',
        '            frame_offset = -1;\n        }\n    }'))
animation = ''
if native_entry:
    animation = (ROOT / 'apps-ipod/ui/animation.h').read_text()
    animation += (ROOT / 'apps-ipod/ui/animation.c').read_text().replace('#include "animation.h"', '')
    animation += function((ROOT / 'firmware/drivers/button_queue.c').read_text(),
                          'button_queue_wait_pending')
retained = ''
if retained_entry:
    retained = (ROOT / 'apps-ipod/ui/retained.h').read_text()
    retained += (ROOT / 'apps-ipod/ui/retained.c').read_text()
    retained = re.sub(r'^#include [^<].*$', '', retained, flags=re.M)
theme_tokens = ''
if themed_entry:
    theme_tokens = (ROOT / 'apps-ipod/ui/native_theme.h').read_text()
    theme_tokens += (ROOT / 'apps-ipod/ui/native_theme.c').read_text()
    theme_tokens = re.sub(r'^#include [^<].*$', '', theme_tokens, flags=re.M)
    theme_tokens += r'''
static void ui_theme_snapshot(struct ui_theme *theme) { ui_theme_builtin(theme, false); }
static unsigned ui_theme_duration(unsigned frames)
{ return global_settings.animations ? frames : 0; }
static int ui_theme_font_load(const struct ui_theme *theme, enum ui_font_role role, int glyphs)
{
    char path[64];
    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", theme->font[role]);
    return font_load_ex(path, 0, glyphs);
}
static bool album_palette_snapshot(struct ui_album_palette *palette)
{
    *palette = (struct ui_album_palette){0};
    return false;
}
'''
draw = re.sub(r'^#(?:include|if|endif).*$', '', draw, flags=re.M)
types = 'enum dialog_disposition' + header('apps-ipod/widgets/dialog.h', 'enum dialog_disposition')
sheet = 'struct handoff_sheet' + header('apps-ipod/screens/playback/handoff_sheet.h',
                                        'struct handoff_sheet')
ports = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct dialog_native_content;
struct dialog_native_ui;
#define HZ 100
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_BLEND_OPAQUE 15
#define LCD_BLACK 0
#define LCD_WHITE 255
#define LCD_RGBPACK(r,g,b) (r)
#define RGB_UNPACK_RED(c) (c)
#define RGB_UNPACK_GREEN(c) (c)
#define RGB_UNPACK_BLUE(c) (c)
#define NO_INLINE __attribute__((noinline))
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for (int i = 0; i < NB_SCREENS; ++i)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TIME_AFTER(a,b) ((long)(a)>(long)(b))
#define TIME_BEFORE(a,b) TIME_AFTER(b,a)
#define DRMODE_FG 1
#define CONTEXT_STD 1
#define CONTEXT_PLUGIN 2
#define DIALOG_FONT_INHERIT (-1)
#define VP_FLAG_VP_SET_CLEAN 1
#define Icon_NOICON 0
#define TIMEOUT_BLOCK -1
#define CUSTOM_STATUSBAR 0
#define SYS_USB_CONFIRM 1000
#define SYS_USB_CONNECTED 1001
#define SYS_ACCESSORY_CONNECTED 1002
#define SYS_USB_DISCONNECTED 1003
#define SYS_POWEROFF 1004
#define SYS_REBOOT 1005
#define IS_SYSEVENT(a) ((a) >= SYS_USB_CONFIRM)
#define TEXT_FIT_BUF 256
#define FONT_DIR "/fonts"
#define BUTTON_RIGHT 1
#define BUTTON_LEFT 2
#define BUTTON_SELECT 4
#define BUTTON_REL 8
#define ACTION_UNKNOWN (-1)
#define FRAMEBUFFER_SIZE sizeof pixels
#define HANDOFF_FRAME_BYTES (2 * FRAMEBUFFER_SIZE)
enum { ACTION_NONE, ACTION_STD_MENU, ACTION_STD_OK, ACTION_STD_CANCEL,
       ACTION_STD_PREV, ACTION_STD_PREVREPEAT, ACTION_STD_NEXT, ACTION_STD_NEXTREPEAT,
       ACTION_REDRAW };
enum { LANG_HANDOFF_TITLE, LANG_HANDOFF_FAILED, LANG_HANDOFF_ACCEPT, LANG_HANDOFF_LEAVE,
       LANG_HANDOFF_PREPARING, LANG_HANDOFF_NOT_FOUND };
static const char *strings[] = { "接力到电脑", "接力失败", "接力", "留在 iPod", "正在接力…",
                                 "电脑上找不到这首歌" };
#define str(id) ((const unsigned char *)strings[id])
typedef unsigned char fb_data;
struct frame_buffer_t { fb_data *fb_ptr; };
struct viewport { int x,y,width,height,font; struct frame_buffer_t *buffer; };
struct screen {
    struct viewport *(*set_viewport)(struct viewport *);
    struct viewport *(*set_viewport_ex)(struct viewport *, int);
    void (*scroll_stop)(void);
    void (*scroll_stop_viewport)(struct viewport *);
    void (*update_viewport)(void);
    int (*getuifont)(void);
};
struct dialog;
struct dialog_style { int box_font, box_shadow_offset; };
struct bitmap { int width, height; void *data; };
struct mp3entry { const char *artist; };
struct font { int height; };
struct button_mapping {};
enum screen_type { SCREEN_MAIN };
static long current_tick;
static struct { bool animations, native_ui; } global_settings;
static intptr_t queued;
static bool system_took_over, interrupted;
static unsigned queue_waits, boost_count;
static unsigned active_trace;
static unsigned ui_perf_begin(const char *route)
{
    assert((!strcmp(route, "handoff-open") || !strcmp(route, "handoff-close")) &&
           !active_trace);
    return active_trace = 1;
}
static void ui_perf_end(unsigned token)
{
    if (token)
    {
        assert(token == active_trace);
        active_trace = 0;
    }
}
static void ui_note_render(uint32_t elapsed) { (void)elapsed; }
static int native_allocs, dock_events;
static int progress_updates, pins;
#define USEC_TIMER ((uint32_t)(current_tick * (ui_frame_clock(UI_ANIM_FPS) / HZ)))
static struct viewport base = {0, 0, LCD_WIDTH, LCD_HEIGHT, 1, NULL}, *active = &base;
static int glyph_mode, foreground, font_id, shown, raw_button, action_index, scenario;
static bool inhibited, showing_art;
static int hold, last_answer, phase_changes;
static bool exiting, fail_alloc;
static int last_offset, in_frames, out_frames, early_input, reads, draws, closes;
static long animation_start, open_tick, exit_start;
static struct { bool wait_for_release; int button; } action_last;
static unsigned char pixels[LCD_HEIGHT][LCD_WIDTH];
static unsigned char original[LCD_HEIGHT][LCD_WIDTH];
static unsigned char panel[LCD_HEIGHT][LCD_WIDTH];
#ifdef DIMMED_SHEET
static int frame_offset = -1, frame_brightness;
#endif
static unsigned char frames[2][LCD_HEIGHT][LCD_WIDTH];
static struct frame_buffer_t lcd_framebuffer_default = { &pixels[0][0] };
static fb_data *draw_address(int x, int y)
{
    struct frame_buffer_t *buffer = active->buffer ?: &lcd_framebuffer_default;
    return buffer->fb_ptr + (y + active->y) * LCD_WIDTH + x + active->x;
}
#define FBADDR(x,y) draw_address(x,y)
static struct font loaded_fonts[] = {{0},{12},{19},{27}};
static unsigned live_fonts;
static struct font *font_get(int i) { return &loaded_fonts[i]; }
static int font_get_ui_bold(void) { return 3; }
static int font_load_ex(const char *path, int size, int glyphs)
{
    assert(size == 0 && glyphs == 48);
    int id = !strcmp(path, FONT_DIR "/12-noto-sans.fnt") ? 1 :
             !strcmp(path, FONT_DIR "/18-noto-sans.fnt") ? 2 :
             !strcmp(path, FONT_DIR "/26-noto-sans-medium.fnt") ? 3 : 0;
    assert(id && !(live_fonts & (1u << id)));
    live_fonts |= 1u << id;
    return id;
}
static void font_unload(int id)
{
    assert(live_fonts & (1u << id));
    live_fonts &= ~(1u << id);
}
static int ui_font(void) { return 1; }
static void lcd_setfont(int i) { font_id = i; }
static void lcd_set_drawmode(int mode) { glyph_mode = mode; }
static void lcd_set_foreground(unsigned fg) { foreground = fg; }
static void lcd_getstringsize(const unsigned char *text, int *w, int *h)
{
    int width = 0;
    for (; *text; ++text)
        if (*text < 128) width += 8;
        else if (*text >= 192) width += 16;
    *w = width;
    *h = loaded_fonts[font_id].height;
}
static void lcd_putsxy(int x, int y, const unsigned char *text)
{
    int w, h;
    lcd_getstringsize(text, &w, &h);
    assert(glyph_mode == DRMODE_FG && active->width == LCD_WIDTH);
    x += active->x;
    y += active->y;
    if (!strcmp((const char *)text, strings[LANG_HANDOFF_ACCEPT]))
        assert(font_id == 2 && x == 16 + (140-w)/2 && y == 188 + (36-h)/2);
    if (!strcmp((const char *)text, strings[LANG_HANDOFF_LEAVE]))
        assert(font_id == 2 && x == 164 + (140-w)/2 && y == 188 + (36-h)/2);
    if (font_id == 2) assert(y >= 188 && y + h <= 216);
    if (!strcmp((const char *)text, strings[LANG_HANDOFF_PREPARING]))
        assert(font_id == 2 && y == 188);
    if (!strcmp((const char *)text, strings[LANG_HANDOFF_FAILED]))
        assert(font_id == 3 && y == 184);
    assert(x >= 0 && x + w <= LCD_WIDTH && y >= 0 && y + h <= LCD_HEIGHT);
}
static void lcd_fillrect(int x, int y, int w, int h)
{
    assert(x >= 0 && y >= 0 && x+w <= active->width && y+h <= active->height);
    for (int row = y; row < y+h; ++row) memset(FBADDR(x, row), foreground, w);
}
static void lcd_blendrect(int x, int y, int w, int h, unsigned p)
{
    assert(x >= 0 && y >= 0 && x+w <= active->width && y+h <= active->height);
    assert(p <= LCD_BLEND_OPAQUE);
    for (int row = y; row < y+h; ++row)
        for (int col = x; col < x+w; ++col)
        {
            fb_data *pixel = FBADDR(col, row);
            *pixel = (*pixel*(15-p) + foreground*p)/15;
        }
}
static void lcd_drawline(int x, int y, int a, int b)
{ (void)x;(void)y;(void)a;(void)b; }
static void lcd_bitmap(const fb_data *data, int x, int y, int w, int h)
{ assert(h==1); memcpy(FBADDR(x,y), data, w); }
static void lcd_update_rect(int x, int y, int w, int h)
{
    assert(x >= 0 && y >= 0 && x + w <= LCD_WIDTH && y + h <= LCD_HEIGHT && w > 0 && h > 0);
    for (int row = y; row < y+h; ++row)
        memcpy(&panel[row][x], &pixels[row][x], w);
    if (h == 2 && y == 229)
    {
        assert(scenario == 14 && !exiting && x >= 16 && x + w <= 304);
        ++progress_updates;
        return;
    }
    assert(x == 0 && y + h == LCD_HEIGHT && w == LCD_WIDTH);
    int offset = 0;
#ifdef DIMMED_SHEET
    if (frame_offset >= 0)
    {
        offset = frame_offset;
        unsigned shade = 256 - (LCD_HEIGHT - offset) * 256 * 35 / (LCD_HEIGHT * 100);
        assert(frame_brightness == (int)shade);
        for (int row = 0; row < offset; ++row)
        {
            unsigned src = original[row][0];
            unsigned blue = ((src & 31) * shade) >> 8;
            unsigned green = (((src >> 5) & 63) * shade) >> 8;
            assert(panel[row][0] == ((green << 5) | blue));
        }
    }
    else
#endif
    {
        while (offset < LCD_HEIGHT && !memcmp(panel[offset], original[offset], LCD_WIDTH))
            ++offset;
    }
    if (offset == LCD_HEIGHT && last_offset == LCD_HEIGHT && !exiting)
        return; /* Initial substrate presentation, before the card is visible. */
    assert(y <= MIN(offset, last_offset));
    if (exiting)
    {
        assert(offset >= last_offset);
        ++out_frames;
    }
    else
    {
        assert(offset <= last_offset);
        if (!offset && last_offset) open_tick = current_tick;
        ++in_frames;
    }
    last_offset = offset;
    ++shown;
}
static int album_art_current(void) {return showing_art ? 7 : -1;}
static bool album_art_render_background(fb_data *pixels, int width, int height,
                                        int handle, bool blur, unsigned foreground)
{
    (void)blur;
    assert(width == LCD_WIDTH && height == LCD_HEIGHT && foreground == LCD_WHITE);
    ++draws;
    if (handle < 0) return false;
    memset(pixels, 100, width * height);
    return true;
}
static bool wps_art_pin(int handle, bool pin) { (void)pin; return handle == 7; }
static bool wps_art_bitmap(int handle, struct bitmap **out)
{
    static unsigned char image[] = {92,92,92,92};
    static struct bitmap bm = {2,2,image};
    assert(handle==7);
    *out = &bm;
    return true;
}
static unsigned color_blend(unsigned a, unsigned b, int p)
{ return (a*(256-p)+b*p)/256; }
static unsigned color_contrast(unsigned a, unsigned b)
{ return a >= b ? a - b : b - a; }
static unsigned color_tone_bound(unsigned c, int tone, int bound)
{ (void)tone; (void)bound; return c; }
static bool dynamic_colors_roles(unsigned *dominant, unsigned *vivid)
{ (void)dominant; (void)vivid; return false; }
static const char *text_fit(const char *t, char *b, size_t n, int font, int w)
{ (void)b;(void)n;(void)font;(void)w; return t; }
static struct mp3entry *audio_current_track(void)
{ static struct mp3entry track = {"Artist"}; return &track; }
static void sleep(int n)
{
#ifdef LIVE_ENTRY
    current_tick += n + 1;
#else
    current_tick += n;
#endif
}
static void get_action_statuscode(int *button) { *button = raw_button; }
static void default_event_handler(int action)
{
    if (action == SYS_USB_CONNECTED)
    {
        assert(active == &base && !hold && !boost_count && closes == 1);
        assert(!memcmp(panel, original, sizeof panel));
        system_took_over = true;
        memset(pixels, 45, sizeof pixels);
        memset(panel, 45, sizeof panel);
    }
    if (action == SYS_ACCESSORY_CONNECTED)
    {
        assert(last_offset == 0 && hold && !boost_count && closes == 0);
        ++dock_events;
    }
}
static bool ui_flush_inhibited(void) { return inhibited; }
static void ui_inhibit_flush(bool state) { inhibited = state; }
static bool ui_hold_scrolling(bool state) { hold += state ? 1 : -1; return !hold; }
static void sb_invalidate(void) {}
static unsigned viewport_depth;
static bool viewport_visible[2];
static void viewportmanager_theme_enable(int screen, bool enabled, struct viewport *viewport)
{
    assert(!screen && viewport_depth < 2 && !viewport);
    viewport_visible[viewport_depth++] = enabled;
}
static void viewportmanager_theme_undo(int screen, bool redraw)
{
    (void)redraw;
    assert(!screen && viewport_depth);
    viewport_depth--;
}
static int dirty_rect[4];
static void ui_mark_dirty_rect(int screen, int x, int y, int w, int h)
{
    assert(!screen);
    dirty_rect[0] = x; dirty_rect[1] = y; dirty_rect[2] = w; dirty_rect[3] = h;
}
static void ui_flush_dirty(void)
{
    if (inhibited || !dirty_rect[2])
        return;
    lcd_update_rect(dirty_rect[0], dirty_rect[1], dirty_rect[2], dirty_rect[3]);
    dirty_rect[2] = 0;
}
static void lcd_update(void) { lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT); }
static void button_clear_pressed(void) {}
void action_wait_for_release(void);
static size_t core_allocatable(void) { return fail_alloc ? 0 : sizeof frames; }
static int core_alloc_free_space(size_t n)
{
    assert(n == sizeof frames || n == FRAMEBUFFER_SIZE);
    ++native_allocs;
    return fail_alloc ? -1 : 1;
}
static void *core_get_data(int handle) { assert(handle == 1); return frames; }
static void core_pin(int handle) { assert(handle == 1); ++pins; }
static void core_unpin(int handle) { assert(handle == 1 && pins > 0); --pins; }
static int core_free(int handle) { assert(handle == 1 && !pins); closes++; return 0; }
static void *core_get_data_pinned(int handle)
{ core_pin(handle); return core_get_data(handle); }
static void core_put_data_pinned(const void *data)
{ assert(data == frames); core_unpin(1); }
static void backlight_on(void) {}
static void cpu_boost(bool enabled)
{
    if (enabled) ++boost_count;
    else
    {
        assert(boost_count);
        --boost_count;
    }
}
static bool is_backlight_on(bool wait) { (void)wait; return true; }
static bool dynamic_colors_needs_repaint(void) { return false; }
static struct viewport *set_viewport(struct viewport *vp)
{ struct viewport *old=active; active=vp ?: &base; return old; }
#define lcd_set_viewport set_viewport
static struct viewport *set_viewport_ex(struct viewport *vp, int flags)
{ (void)flags; return set_viewport(vp); }
static void viewport_set_fullscreen(struct viewport *vp, int screen)
{
    (void)screen;
    struct frame_buffer_t *buffer = vp->buffer;
    *vp = base;
    vp->buffer = buffer;
}
static void scroll_stop(void) {}
static void scroll_stop_vp(struct viewport *vp) { (void)vp; }
static void flush(void)
{
    assert(active->x == 0 && active->y == 0 && active->width == LCD_WIDTH &&
           active->height == LCD_HEIGHT);
    if (shown == 0 && scenario < 3)
    {
        assert(pixels[188][16] == 255 && pixels[223][155] == 255);
        assert(pixels[188][164] == (showing_art ? 115 : 85));
        assert(pixels[215][170] < pixels[188][164]);
    }
    if (shown == 3 && scenario == 1)
    {
        assert(pixels[188][164] == 255 && pixels[223][303] == 255);
        assert(pixels[215][20] < pixels[188][16]);
    }
    shown++;
}
static struct screen screens[] = {{set_viewport,set_viewport_ex,scroll_stop,scroll_stop_vp,
                                    flush,ui_font}};
static void sb_set_persistent_title(const char *t, int i, int s) { (void)t;(void)i;(void)s; }
static const char *sb_get_persistent_title(int s) { (void)s; return NULL; }
static void viewport_set_defaults(struct viewport *vp, int s)
{ (void)s; *vp=(struct viewport){5,5,310,230,1,NULL}; }
static void toggle_theme(int i,bool force) { (void)i;(void)force; }
static void panicf(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
static const struct dialog_style *dialog_get_default_style(void)
{ static const struct dialog_style style={DIALOG_FONT_INHERIT,0}; return &style; }
static void dialog_frame_box(struct screen *s, struct viewport *box,
                             struct dialog_style *style, struct viewport *content)
{ (void)s;(void)box;(void)style;(void)content; assert(!"sheet must bypass framed dialog"); }
static int standard_select(void);

#ifdef NATIVE_SHEET
void handoff_sheet_interrupt(void);
struct event_queue {};
struct queue_event {};
static struct event_queue button_queue;
static void queue_wait_w_tmo(struct event_queue *queue, struct queue_event *event, int ticks)
{
    assert(queue == &button_queue && event == NULL && ticks >= 0);
    ++queue_waits;
    if (scenario == 10 && !queued) queued = SYS_USB_CONNECTED;
    if (scenario == 12 && !queued) queued = BUTTON_RIGHT;
    if (!queued) current_tick += ticks;
}
static bool queue_empty(const struct event_queue *queue)
{
    assert(queue == &button_queue);
    return !queued;
}
#endif

static int get_action(int context, int ticks)
{
    assert(context == CONTEXT_STD && ticks >= 0 && !inhibited && hold == 1);
    assert(viewport_depth == 2 && !viewport_visible[1]);
    current_tick += ticks;
    raw_button=0;
    ++reads;
    if (!shown) return ACTION_NONE;
    if (scenario == 5)
    {
        if (exiting) return ACTION_NONE;
        if (!early_input++)
        {
            assert(shown == 1);
            return standard_select();
        }
        return ACTION_STD_MENU;
    }
    if (scenario == 6)
        return last_offset == 0 && reads > 4 ? ACTION_STD_MENU : ACTION_NONE;
    if (scenario == 7)
        return shown == 1 ? ACTION_STD_MENU : ACTION_NONE;
    if (scenario == 8)
        return ACTION_STD_MENU;
#ifdef NATIVE_SHEET
    if (scenario == 9 || scenario == 11)
    {
        assert(last_offset > 0 && last_offset < LCD_HEIGHT);
        exiting = true;
        interrupted = true;
        if (scenario == 11)
            return SYS_USB_CONNECTED;
        /* Model the action wrapper tearing down the UI before a nested dialog. */
        handoff_sheet_interrupt();
        default_event_handler(SYS_USB_CONNECTED);
        return ACTION_REDRAW;
    }
    if (scenario == 10 || scenario == 12)
        return last_offset == 0 ? ACTION_STD_MENU : ACTION_NONE;
    if (scenario == 13 && !dock_events)
    {
        assert(last_offset > 0 && last_offset < LCD_HEIGHT);
        return SYS_ACCESSORY_CONNECTED;
    }
    if (scenario == 13) return ACTION_STD_MENU;
    if (scenario == 14)
        return progress_updates >= 3 ? ACTION_STD_MENU : ACTION_NONE;
#endif
    if (scenario == 1)
    {
        if (action_index++ == 0) {raw_button=BUTTON_RIGHT; return ACTION_STD_OK;}
        if (action_index == 2) current_tick += 12;
        if (action_index <= 3) return ACTION_NONE;
        return ACTION_STD_OK;
    }
    if (scenario == 2)
    {
        if (action_index++ == 0) return ACTION_STD_NEXT;
        if (action_index == 2) {raw_button=BUTTON_LEFT; return ACTION_STD_CANCEL;}
        return ACTION_STD_OK;
    }
    return scenario >= 3 ? ACTION_NONE : ACTION_STD_MENU;
}
static int get_custom_action(int c,int t,const struct button_mapping *(*map)(int))
{ (void)map; return get_action(c,t); }
'''
adapter = r'''
#define has_flag(button, bits) (((button) & (bits)) == (bits))
static int standard_select(void)
{
    int result = ACTION_NONE;
    for (int i = 0; i < 2; i++)
    {
        int code = i ? BUTTON_SELECT | BUTTON_REL : BUTTON_SELECT;
        int *button = &code;
        __typeof__(action_last) *last = &action_last;
        if (last->wait_for_release)
RELEASE_FILTER
        if (code == (BUTTON_SELECT | BUTTON_REL) && last->button == BUTTON_SELECT)
            result = ACTION_STD_OK;
        if (code) last->button = code;
    }
    return result;
}
#define LANG_ACCESSORY_CONNECTED 17
#define ID2P(id) (id)
static void splash(int ticks, int id) { assert(id == 17); sleep(ticks); }
static int accessory(int event)
{
    switch (event)
    {
        case SYS_ACCESSORY_CONNECTED:
ACCESSORY_CASE
            return SYS_ACCESSORY_CONNECTED;
        default: return 0;
    }
}
'''.replace('RELEASE_FILTER', filter_release).replace('ACCESSORY_CASE', accessory_case)

scenario = r'''
static int advance_test(int answer)
{
    last_answer = answer;
    phase_changes++;
    if (shown && scenario >= 3 && scenario <= 4) answer = DIALOG_ABORT;
    if (answer != DIALOG_CONTINUE)
    {
        exiting = true;
        exit_start = current_tick;
    }
    return answer;
}

static void assert_accessory_without_delay(void)
{
    long dock_tick = current_tick;
    assert(accessory(SYS_ACCESSORY_CONNECTED) == SYS_ACCESSORY_CONNECTED);
    printf("Accessory event blocked for %ld ticks\n", current_tick - dock_tick);
    fflush(stdout);
    assert(current_tick == dock_tick);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
    {
        assert_accessory_without_delay();
        return 0;
    }
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
#ifdef NATIVE_SHEET
#ifdef DIMMED_SHEET
    assert(OPEN_FRAMES == 16 && CLOSE_FRAMES == 12);
#endif
    global_settings.animations = true;
    native_allocs = 0;
    for (scenario = 5; scenario <= 13; ++scenario)
    {
        struct handoff_sheet view = { .title = "Synthetic music", .target = "Rufin" };
        fail_alloc = scenario == 8;
        showing_art = scenario % 2 == 0;
        action_index = phase_changes = shown = reads = early_input = 0;
        in_frames = out_frames = draws = closes = queue_waits = 0;
        last_offset = LCD_HEIGHT;
        exiting = system_took_over = interrupted = false;
        queued = 0;
        memset(original, 23, sizeof original);
        memset(original[0], 77, LCD_WIDTH);
        memcpy(pixels, original, sizeof pixels);
        memcpy(panel, original, sizeof panel);
        animation_start = current_tick;
        int result = handoff_sheet_run(&view, advance_test);
        int expected = scenario == 5 ? DIALOG_ACCEPT :
                       scenario == 9 || scenario == 11 ? DIALOG_ABORT : DIALOG_CANCEL;
        assert(result == expected && shown && phase_changes);
        assert(active == &base && !hold && !boost_count && !live_fonts);
        assert(closes == !fail_alloc);
        if (scenario == 6)
        {
            assert(in_frames >= 8 && out_frames >= 6);
            assert(ui_clock_frame((open_tick - animation_start) *
                                  (ui_frame_clock(UI_ANIM_FPS) / HZ)) == OPEN_FRAMES);
            assert(ui_clock_frame((current_tick - exit_start) *
                                  (ui_frame_clock(UI_ANIM_FPS) / HZ)) == CLOSE_FRAMES);
        }
        if (scenario == 5) assert(early_input == 1);
        if (scenario == 9 || scenario == 11)
        {
            assert(interrupted && system_took_over && queue_waits == 0);
            assert(panel[0][0] == 45 && pixels[0][0] == 45);
            assert(last_answer == DIALOG_CONTINUE);
        }
        else if (!fail_alloc)
            assert(!memcmp(panel, original, sizeof panel));
        if (scenario == 10 || scenario == 12)
        {
            assert(queue_waits == 1 && current_tick == exit_start);
            assert(queued == (scenario == 10 ? SYS_USB_CONNECTED : BUTTON_RIGHT));
            assert(!action_last.wait_for_release);
        }
        if (scenario == 13)
            assert(dock_events == 1 && last_answer == DIALOG_CANCEL);
    }
    assert(native_allocs == 10);
    puts("Native sheet: frame clock, first input, reversal, free-only fallback, USB takeover, and non-consuming close");
#endif
#ifdef RETAINED_SHEET
    for (int mode = 0; mode < 2; ++mode)
    {
        global_settings.animations = mode == 0;
        for (scenario = 5; scenario <= 14; ++scenario)
        {
            if (mode && scenario != 6 && scenario != 8 && scenario != 14) continue;
            struct handoff_sheet view = { .title = "Synthetic music", .target = "Rufin" };
            if (scenario == 14)
            {
                view.preparing = view.armed = true;
                view.start = current_tick;
            }
            fail_alloc = scenario == 8;
            showing_art = scenario % 2 == 0;
            action_index = phase_changes = shown = reads = early_input = 0;
            in_frames = out_frames = draws = closes = queue_waits = progress_updates = 0;
            last_offset = LCD_HEIGHT;
            exiting = system_took_over = interrupted = false;
            dock_events = 0;
            queued = 0;
            memset(original, 23, sizeof original);
            memset(original[0], 77, LCD_WIDTH);
            memcpy(pixels, original, sizeof pixels);
            memcpy(panel, original, sizeof panel);
            int result = handoff_sheet_run(&view, advance_test);
            int expected = scenario == 5 ? DIALOG_ACCEPT :
                           scenario == 9 || scenario == 11 ? DIALOG_ABORT : DIALOG_CANCEL;
            assert(result == expected && shown && phase_changes);
            assert(active == &base && !hold && !boost_count && !live_fonts && !pins);
            assert(!active_trace);
            assert(closes == !fail_alloc);
            if (scenario == 9 || scenario == 11)
                assert(system_took_over && panel[0][0] == 45);
            else if (!fail_alloc)
                assert(!memcmp(panel, original, sizeof panel));
            if (scenario == 14) assert(progress_updates >= 3);
            if (mode && !fail_alloc) assert(in_frames == 1 && out_frames == 1);
        }
    }
    puts("Retained handoff: motion, static mode, progress, fallback and USB takeover");
#endif
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    assert(viewport_depth == 0);
    assert_accessory_without_delay();
    puts("FU83: first visible SELECT, wheel, MENU, warmup, slide reversal, real background pixels, and no accessory delay");
}
'''
c = OUT / 'sheet.c'
buttons = function(action_source, 'action_wait_for_release')
if live_entry:
    buttons += function(action_source, 'action_handoff_enter')
native_ports = function((ROOT / 'apps-ipod/ui/scene.c').read_text(),
                        'ui_scene_modal_body_offset') + r'''
static bool rock_ui_raw(void) { return false; }
static unsigned album_art_generation(void) { return 0; }
static unsigned album_blur_generation(void) { return 0; }
static void album_palette_refresh(void) {}
static const void *ui_scene_modal_parent(void) { return NULL; }
static void ui_scene_modal_restored(const void *p) { assert(!p); }
static int sb_visible_height(void) { return 0; }
static void yield(void) { current_tick++; }
static unsigned color_fit_contrast(unsigned ink, unsigned bg, int contrast)
{ (void)bg;(void)contrast;return ink; }
static fb_data mock_background[LCD_WIDTH * LCD_HEIGHT];
static int native_wps_background(int art, unsigned generation, enum ui_bg_kind mode, unsigned fg)
{
    (void)generation;(void)mode;
    return album_art_render_background(mock_background,LCD_WIDTH,LCD_HEIGHT,art,true,fg) ? 7 : -1;
}
static void native_wps_paint_background(const struct viewport *vp,int background,unsigned bg)
{
    if (background > 0)
        lcd_set_foreground(mock_background[0]);
    else
        lcd_set_foreground(bg);
    lcd_fillrect(0,0,vp->width,vp->height);
}
'''
c.write_text(('#define LIVE_ENTRY\n' if live_entry else '') +
             ('#define NATIVE_SHEET\n' if native_entry else '') +
             ('#define RETAINED_SHEET\n' if retained_entry else '') +
             ('#define DIMMED_SHEET\n' if dimmed_entry else '') +
             ports + animation + types + sheet + '\n' + buttons + adapter +
             theme_tokens + native_ports + retained + draw + scenario)
binary = OUT / 'sheet'
subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-sign', '-Wno-unused-function', '-Wno-unused-variable',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', str(c),
                '-o', str(binary)], check=True)
subprocess.run([binary, *sys.argv[3:]], check=True)
