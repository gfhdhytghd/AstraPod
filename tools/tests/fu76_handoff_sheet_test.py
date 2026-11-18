"""Render the real sheet and exercise the real dialog and theme stack."""
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
draw = re.sub(r'^#(?:include|if|endif).*$', '', draw, flags=re.M)
dialog = (ROOT / 'apps-ipod/widgets/dialog.c').read_text()
theme = (ROOT / 'apps-ipod/draw/viewport.c').read_text()
types = header('apps-ipod/widgets/dialog.h', 'enum dialog_disposition')
types = 'enum dialog_disposition' + types
sheet = 'struct handoff_sheet' + header('apps-ipod/screens/playback/handoff_sheet.h',
                                        'struct handoff_sheet')
theme_functions = ['viewportmanager_theme_enable', 'viewportmanager_theme_undo',
                   'viewportmanager_theme_enabled']
dialog_functions = ['dialog_init', 'dialog_draw_one', 'dialog_run']
theme_struct = block(theme, r'^struct viewport_stack_item\s*\{')
theme_depth = re.search(r'^#define VPSTACK_DEPTH \d+$', theme, re.M).group()
ports = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define HZ 100
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_BLEND_OPAQUE 15
#define LCD_BLACK 0
#define LCD_WHITE 255
#define LCD_RGBPACK(r,g,b) (r)
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
       ACTION_STD_PREV, ACTION_STD_PREVREPEAT, ACTION_STD_NEXT, ACTION_STD_NEXTREPEAT };
enum { LANG_HANDOFF_TITLE, LANG_HANDOFF_FAILED, LANG_HANDOFF_ACCEPT, LANG_HANDOFF_LEAVE,
       LANG_HANDOFF_PREPARING, LANG_HANDOFF_NOT_FOUND };
static const char *strings[] = { "接力到电脑", "接力失败", "接力", "留在 iPod", "正在接力…",
                                 "电脑上找不到这首歌" };
#define str(id) ((const unsigned char *)strings[id])
typedef unsigned char fb_data;
struct frame_buffer_t {};
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
static struct viewport base = {0, 0, LCD_WIDTH, LCD_HEIGHT, 1, NULL}, *active = &base;
static int glyph_mode, foreground, font_id, shown, raw_button, action_index, scenario;
static bool inhibited, showing_art;
static int hold, last_answer, phase_changes;
static bool exiting, fail_alloc;
static int last_offset, in_frames, out_frames, early_input, reads, draws, closes;
static long animation_start, exit_start;
static struct { bool wait_for_release; int button; } action_last;
static unsigned char pixels[LCD_HEIGHT][LCD_WIDTH];
static unsigned char original[LCD_HEIGHT][LCD_WIDTH];
static unsigned char panel[LCD_HEIGHT][LCD_WIDTH];
static unsigned char frames[2][LCD_HEIGHT][LCD_WIDTH];
#define FBADDR(x,y) (&pixels[y][x])
static struct font fonts[] = {{0},{12},{19},{27}};
static unsigned live_fonts;
static struct font *font_get(int i) { return &fonts[i]; }
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
    *h = fonts[font_id].height;
}
static void lcd_putsxy(int x, int y, const unsigned char *text)
{
    int w, h;
    lcd_getstringsize(text, &w, &h);
    assert(glyph_mode == DRMODE_FG && active->width == LCD_WIDTH);
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
    assert(x >= 0 && y >= 0 && x+w <= LCD_WIDTH && y+h <= LCD_HEIGHT);
    for (int row = y; row < y+h; ++row) memset(&pixels[row][x], foreground, w);
}
static void lcd_blendrect(int x, int y, int w, int h, unsigned p)
{
    assert(x >= 0 && y >= 0 && x+w <= LCD_WIDTH && y+h <= LCD_HEIGHT);
    assert(p <= LCD_BLEND_OPAQUE);
    for (int row = y; row < y+h; ++row)
        for (int col = x; col < x+w; ++col)
            pixels[row][col] = (pixels[row][col]*(15-p) + foreground*p)/15;
}
static void lcd_drawline(int x, int y, int a, int b)
{ (void)x;(void)y;(void)a;(void)b; }
static void lcd_bitmap(const fb_data *data, int x, int y, int w, int h)
{ assert(h==1); memcpy(&pixels[y][x], data, w); }
static void lcd_update_rect(int x, int y, int w, int h)
{
    assert(x == 0 && y >= 0 && y + h == LCD_HEIGHT && w == LCD_WIDTH && h > 0);
    memcpy(panel[y], pixels[y], h * LCD_WIDTH);
    int offset = 0;
    while (offset < LCD_HEIGHT && !memcmp(panel[offset], original[offset], LCD_WIDTH))
        ++offset;
    assert(y == MIN(offset, last_offset));
    if (exiting)
    {
        assert(offset >= last_offset);
        ++out_frames;
    }
    else
    {
        assert(offset <= last_offset);
        ++in_frames;
    }
    last_offset = offset;
    ++shown;
}
static int skin_lyrics_art(void) {return showing_art ? 7 : -1;}
static bool skin_handoff_draw_blur(int handle)
{
    ++draws;
    if (handle < 0) return false;
    memset(pixels, 100, sizeof pixels);
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
static void default_event_handler(int action) { (void)action; }
static bool skin_flush_inhibited(void) { return inhibited; }
static void skin_inhibit_flush(bool state) { inhibited = state; }
static bool skin_hold_scrolling(bool state) { hold += state ? 1 : -1; return !hold; }
static void skin_request_full_update(int type) { (void)type; }
static void sb_skin_force_next_update(void) {}
static void button_clear_pressed(void) {}
void action_wait_for_release(void);
static size_t core_allocatable(void) { return fail_alloc ? 0 : sizeof frames; }
static int core_alloc(size_t n) { assert(n == sizeof frames); return 1; }
static void *core_get_data(int handle) { assert(handle == 1); return frames; }
static void core_pin(int handle) { assert(handle == 1); }
static void core_unpin(int handle) { assert(handle == 1); }
static void core_free(int handle) { assert(handle == 1); closes++; }
static void backlight_on(void) {}
static void cpu_boost(bool enabled) { (void)enabled; }
static bool is_backlight_on(bool wait) { (void)wait; return true; }
static bool dynamic_colors_needs_repaint(void) { return false; }
static struct viewport *set_viewport(struct viewport *vp)
{ struct viewport *old=active; active=vp; return old; }
#define lcd_set_viewport set_viewport
static struct viewport *set_viewport_ex(struct viewport *vp, int flags)
{ (void)flags; return set_viewport(vp); }
static void viewport_set_fullscreen(struct viewport *vp, int screen)
{ (void)screen; *vp=base; }
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

static int get_action(int context, int ticks)
{
    assert(context == CONTEXT_STD && ticks > 0 && inhibited && hold == 1);
    current_tick += ticks;
    raw_button=0;
    ++reads;
    if (!shown) return ACTION_NONE;
    if (scenario == 5)
    {
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
#define SYS_ACCESSORY_CONNECTED 1002
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
    for (int i = 0; i < 58; ++i)
    {
        struct handoff_sheet view = { .title = "Synthetic music", .target = "Rufin" };
        /* The first new key has no relation to the dock event on the owner. */
        scenario = i == 0 ? 5 : i < 50 ? 0 : i - 49;
        if (i >= 54) scenario = i + 2 - 50;
        showing_art = (i % 2 == 0);
        fail_alloc = scenario == 8;
        if (scenario == 3) view.reason = LANG_HANDOFF_NOT_FOUND;
        if (scenario == 4) view.preparing = true;
        action_last.button = 0;
        action_index = phase_changes = shown = reads = early_input = 0;
        in_frames = out_frames = draws = closes = 0;
        last_offset = LCD_HEIGHT;
        exiting = false;
        inhibited = (i % 2 == 0);
        memset(original, 23, sizeof original);
        memset(original[0], 77, LCD_WIDTH);
        memcpy(pixels, original, sizeof pixels);
        memcpy(panel, original, sizeof panel);
        animation_start = current_tick;
        int result = handoff_sheet_run(&view, advance_test);
        if (scenario == 5) assert(result == DIALOG_ACCEPT && early_input == 1);
        assert(result == (scenario == 3 || scenario == 4 ? DIALOG_ABORT :
                         scenario == 2 || scenario == 5 ? DIALOG_ACCEPT : DIALOG_CANCEL));
        assert(shown > 0 && phase_changes > 0);
        assert(active == &base && theme_stack_top[0] == 1 && !hold && inhibited == (i%2==0));
        assert(!live_fonts);
#ifdef LIVE_ENTRY
        if (!fail_alloc)
        {
            assert(closes == 1 && !memcmp(panel, original, sizeof panel));
            assert(last_offset == LCD_HEIGHT && out_frames > 0);
            assert(current_tick - exit_start == EXIT_TICKS);
        }
        if (scenario == 6)
        {
            assert(draws == 1 && in_frames >= 6 && out_frames >= 5);
            assert(exit_start >= animation_start + ENTER_TICKS);
            printf("Animated LCD updates: enter=%d exit=%d\n", in_frames, out_frames);
        }
        if (scenario == 8) assert(!closes);
#endif
        if (i >= 56) break;
    }
    viewportmanager_theme_undo(SCREEN_MAIN, false);
#ifdef LIVE_ENTRY
    for (int t = 0; t <= ENTER_TICKS; ++t)
        assert(slide_offset(t, LCD_HEIGHT, false) >= slide_offset(t + 1, LCD_HEIGHT, false));
    for (int from = 0; from <= LCD_HEIGHT; ++from)
    {
        assert(slide_offset(0, from, true) == from);
        assert(slide_offset(EXIT_TICKS, from, true) == LCD_HEIGHT);
        for (int t = 0; t <= EXIT_TICKS; ++t)
            assert(slide_offset(t, from, true) <= slide_offset(t + 1, from, true));
    }
    assert(slide_offset(ENTER_TICKS, LCD_HEIGHT, false) == 0);
#endif
    assert(theme_stack_top[0] == 0);
    assert_accessory_without_delay();
    puts("FU83: first visible SELECT, wheel, MENU, warmup, slide reversal, real background pixels, and no accessory delay");
}
'''
c = OUT / 'sheet.c'
buttons = function(action_source, 'action_wait_for_release')
if live_entry:
    buttons += function(action_source, 'action_handoff_enter')
c.write_text(('#define LIVE_ENTRY\n' if live_entry else '') + ports + types + sheet + theme_depth + '\n' + theme_struct + r''';
static int theme_stack_top[NB_SCREENS];
static struct viewport_stack_item theme_stack[NB_SCREENS][VPSTACK_DEPTH];
''' + '\n'.join(function(theme, name) for name in theme_functions) +
             '\n'.join(function(dialog, name) for name in dialog_functions) + buttons + adapter + draw + scenario)
binary = OUT / 'sheet'
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-sign', '-Wno-unused-function', '-Wno-unused-variable',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', str(c),
                '-o', str(binary)], check=True)
subprocess.run([binary, *sys.argv[3:]], check=True)
