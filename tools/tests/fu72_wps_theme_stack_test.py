"""FU72: lyrics/Cover Flow round trips from the WPS keep the theme stack level.

Runs the production theme stack (viewport.c) and the production WPS pieces
(enter_playback, leave_playback, the lyrics and Cover Flow branches) in a model
of the WPS loop. Then plugs USB during playback and pushes the USB screen. FU70 re-pushed the WPS level after every
lyrics visit and panicked "Stack overflow... viewportmanager".
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)


def block(text, start):
    i = text.index(start)
    j = text.index("{", i) + 1
    depth = 1
    while depth:
        depth += (text[j] == "{") - (text[j] == "}")
        j += 1
    return text[i:j]


def fn(text, name):
    m = re.search(r"(?m)^[A-Za-z_][\w \t*]*\b" + name + r"\([^;{}]*\)\s*\{", text)
    assert m, name
    return block(text, m.group(0))


vp = (ROOT / "apps-ipod/draw/viewport.c").read_text()
wps = (ROOT / "apps-ipod/screens/playback/wps.c").read_text()
depth = re.search(r"(?m)^#define VPSTACK_DEPTH \d+$", vp).group(0)
theme = "\n".join(fn(vp, n) for n in ("viewportmanager_theme_enabled",
                                      "viewport_changed",
                                      "viewportmanager_theme_enable",
                                      "viewportmanager_theme_undo"))
enter = fn(wps, "enter_playback")
assert "enter_playback(state);" in wps
lyrics = block(wps, "if (button == ACTION_WPS_BROWSE || button == ACTION_WPS_LYRICS)")
covers = block(wps, "if (act == HOTKEY_ALBUM_COVERS)")
assert "lyric_viewer()" in lyrics and "show_album_covers" in covers

src = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for (int i = 0; i < NB_SCREENS; i++)
#define SCREEN_MAIN 0
#define GO_TO_ROOT 1
#define GO_TO_WPS 2
#define LCD_EVENT_ACTIVATION 1
#define DEFAULT_UPDATE_DELAY 0
enum screen_type { SCREEN_MAIN_T };
enum { ACTION_NONE, ACTION_WPS_BROWSE, ACTION_WPS_LYRICS, ACTION_WPS_HOTKEY,
       BUTTON_USB, BUTTON_HEADSET, BUTTON_DONE };
enum { HOTKEY_OFF, HOTKEY_ALBUM_COVERS };
struct viewport { int x; };
struct screen { void (*scroll_stop)(void); };
struct wps_state { int unused; };
struct ui_scene { bool open; };
static struct screen screens[1] = {{ NULL }};
static jmp_buf panic_jmp;
static char panic_msg[64];
static bool wps_fullscreen, playback_open, classic;
#define GUI_EVENT_THEME_CHANGED 2
static void scroll_stop(void) {}
static void panicf(const char *msg) { snprintf(panic_msg, sizeof panic_msg, "%s", msg); longjmp(panic_jmp, 1); }
static void viewport_set_defaults(struct viewport *v, enum screen_type s) { (void)v; (void)s; }
static void sb_clear_transient_title(int screen) { (void)screen; }
static void sb_invalidate(void) {}
static void ui_take_dirty(int screen) { (void)screen; }
static void send_event(int event, void *data) { (void)data; assert(event == GUI_EVENT_THEME_CHANGED); }
static void remove_event(int e, void *h) { (void)e; (void)h; }
static void wps_lcd_activation_hook(void) {}
static void cider_spectrum_wps(bool visible) { (void)visible; }
static bool native_wps_active(void) { return playback_open; }
static bool native_wps_art_turned(void) { return classic; }
static bool lyrics_classic_layout(void) { return classic; }
static void menu_pane_viewport_left(int depth) { (void)depth; }
static void native_wps_open(const struct wps_state *s)
{ (void)s; assert(!playback_open && wps_fullscreen); playback_open = true; }
static void native_wps_close(void) { playback_open = false; }
static void native_wps_invalidate(void) { assert(playback_open); }
static void ui_scene_interrupt(void) {}
static void ui_scene_open_modal(struct ui_scene *s, const char *r)
{ assert(!s->open && !strcmp(r, "lyrics-up")); s->open = true; }
static void ui_scene_close_modal(struct ui_scene *s, bool animate, const char *r)
{ assert(s->open && animate && !strcmp(r, "lyrics-down")); s->open = false; }
@DEPTH@
static unsigned char viewport_depth[NB_SCREENS];
static uint16_t chrome_visibility[NB_SCREENS];
@THEME@
@LEAVE@
@ENTER@
/* A pushed page pops its own level, like lyric_viewer() and Cover Flow. */
static struct viewport page_vp;
static int lyric_viewer(void)
{ viewportmanager_theme_enable(SCREEN_MAIN, false, &page_vp);
  viewportmanager_theme_undo(SCREEN_MAIN, false); return GO_TO_WPS; }
static int lyric_viewer_from_wps(struct ui_scene *scene)
{
    assert(!playback_open);
    ui_scene_open_modal(scene, "lyrics-up");
    int result = lyric_viewer();
    ui_scene_close_modal(scene, true, "lyrics-down");
    return result;
}
static int global_settings_wps_select_action = HOTKEY_ALBUM_COVERS;
#define HK_CTX_GET(a, b) (b)
static bool show_album_covers(long *go_to, bool r, uint32_t action_us)
{ (void)go_to; (void)r; (void)action_us; leave_playback(); viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
  viewportmanager_theme_undo(SCREEN_MAIN, false); return false; }
static bool return_agents;
/* USB during playback: the consent dialog, then the USB screen, both over the WPS. */
static void usb_insert(void)
{ struct viewport parent;
  viewportmanager_theme_enable(SCREEN_MAIN, false, &parent);     /* usb screen */
  viewportmanager_theme_undo(SCREEN_MAIN, false); }

static const int *script; static int pos;
static int wps_loop(void)
{
    long button = 0; bool restore = true;
    uint32_t action_us = 0;
    struct ui_scene scene = {0}, *lyrics_scene = &scene;
    struct wps_state state = {0};
    while (1)
    {
        if (restore) { restore = false; enter_playback(&state); }
        button = script[pos++];
        if (button == BUTTON_DONE) { leave_playback(); return 0; }
        if (button == BUTTON_USB) { usb_insert(); continue; }
        if (button == BUTTON_HEADSET) continue; /* Mikey keys never touch the stack */
        do {
@LYRICS@
        } while (0);
        if (button == ACTION_WPS_HOTKEY) do {
            int act = HK_CTX_GET(0, global_settings_wps_select_action);
            long go_to;
@COVERS@
        } while (0);
    }
}

static int run(const char *name, const int *s)
{
    viewport_depth[0] = 0; chrome_visibility[0] = 1; screens[0].scroll_stop = scroll_stop;
    script = s; pos = 0; panic_msg[0] = 0;
    if (setjmp(panic_jmp)) { printf("FAIL %s: panic '%s' at step %d, top %d\n", name, panic_msg, pos, viewport_depth[0]); return 1; }
    wps_loop();
    if (viewport_depth[0] != 0 || playback_open || wps_fullscreen) { printf("FAIL %s: top %d after leaving WPS\n", name, viewport_depth[0]); return 1; }
    printf("PASS %s: %d steps, stack back to 0\n", name, pos);
    return 0;
}

int main(void)
{
    enum { N = 40 };
    static int lyr[2*N+2], lyrh[3*N+2], cov[N+2], usb_only[2] = { BUTTON_USB, BUTTON_DONE };
    for (int i = 0; i < N; i++) { lyr[2*i] = ACTION_WPS_LYRICS; lyr[2*i+1] = i % 2 ? ACTION_WPS_BROWSE : ACTION_NONE; }
    lyr[2*N] = BUTTON_USB; lyr[2*N+1] = BUTTON_DONE;
    for (int i = 0; i < N; i++) { lyrh[3*i] = BUTTON_HEADSET; lyrh[3*i+1] = ACTION_WPS_LYRICS; lyrh[3*i+2] = BUTTON_HEADSET; }
    lyrh[3*N] = BUTTON_USB; lyrh[3*N+1] = BUTTON_DONE;
    for (int i = 0; i < N; i++) cov[i] = ACTION_WPS_HOTKEY;
    cov[N] = BUTTON_USB; cov[N+1] = BUTTON_DONE;
    int bad = 0;
    bad += run("usb on WPS", usb_only);
    bad += run("lyrics x40 then usb", lyr);
    bad += run("headset + lyrics x40 then usb", lyrh);
    bad += run("coverflow x40 then usb", cov);
    classic = true;
    bad += run("classic lyrics x40 then usb", lyr);
    return bad != 0;
}
'''
src = (src.replace("@DEPTH@", depth).replace("@THEME@", theme)
          .replace("@LEAVE@", fn(wps, "leave_playback")).replace("@ENTER@", enter)
          .replace("@LYRICS@", lyrics).replace("@COVERS@", covers))
(OUT / "fu72_wps_theme_stack.c").write_text(src)
exe = OUT / "fu72_wps_theme_stack"
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-o", str(exe), str(OUT / "fu72_wps_theme_stack.c")], check=True)
r = subprocess.run([str(exe)], capture_output=True, text=True)
print(r.stdout, end="")
assert r.returncode == 0, "theme stack leaked"
print("PASS FU72 WPS theme stack stays balanced across lyrics, Cover Flow, headset and USB")
