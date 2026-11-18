"""FU72: lyrics/Cover Flow round trips from the WPS keep the theme stack level.

Runs the production theme stack (viewport.c) and the production WPS pieces
(gwps_leave_wps, restore_theme, the lyrics and Cover Flow branches) in a model
of the WPS loop. Then plugs USB during playback: the consent dialog and the
USB screen each push one level. FU70 re-pushed the WPS level after every
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
item = block(vp, "struct viewport_stack_item") + ";"
theme = "\n".join(fn(vp, n) for n in ("viewportmanager_theme_enable",
                                      "viewportmanager_theme_undo",
                                      "viewportmanager_theme_enabled"))
enter = fn(wps, "gwps_enter_wps")
assert re.search(r"if \(theme_enabled\)\s*restore_theme\(\);", enter)
assert re.search(r"gwps_enter_wps\(theme_enabled\);\s*theme_enabled = true;", wps)
lyrics = block(wps, "if (button == ACTION_WPS_BROWSE || button == ACTION_WPS_LYRICS)")
covers = block(wps, "if (act == HOTKEY_ALBUM_COVERS)")
assert "lyric_viewer()" in lyrics and "show_album_covers" in covers

src = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
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
struct gui_wps { struct screen *display; };
static void scroll_stop(void) {}
static struct screen screen0 = { scroll_stop };
static struct gui_wps gwps0 = { &screen0 };
static struct screen screens[1] = { { scroll_stop } };
static jmp_buf panic_jmp;
static char panic_msg[64];
static void panicf(const char *msg) { snprintf(panic_msg, sizeof panic_msg, "%s", msg); longjmp(panic_jmp, 1); }
static void toggle_theme(enum screen_type s, bool f) { (void)s; (void)f; }
static void viewport_set_defaults(struct viewport *v, enum screen_type s) { (void)v; (void)s; }
static const char *sb_get_persistent_title(enum screen_type s) { (void)s; return NULL; }
static struct gui_wps *skin_get_gwps(int w, int i) { (void)w; (void)i; return &gwps0; }
static bool skin_has_sbs(struct gui_wps *g) { (void)g; return true; }
static void skin_backdrop_show(int b) { (void)b; }
static int sb_get_backdrop(int i) { (void)i; return 0; }
static void remove_event(int e, void *h) { (void)e; (void)h; }
static void sb_skin_set_update_delay(int d) { (void)d; }
static void wps_lcd_activation_hook(void) {}
static void cider_spectrum_wps(bool visible) { (void)visible; }
#define WPS 0
@DEPTH@
@ITEM@
static int theme_stack_top[NB_SCREENS];
static struct viewport_stack_item theme_stack[NB_SCREENS][VPSTACK_DEPTH];
@THEME@
@LEAVE@
@RESTORE@
static void gwps_enter_wps(bool theme_enabled) { if (theme_enabled) restore_theme(); }
/* A pushed page pops its own level, like lyric_viewer() and Cover Flow. */
static struct viewport page_vp;
static int lyric_viewer(void)
{ viewportmanager_theme_enable(SCREEN_MAIN, false, &page_vp);
  viewportmanager_theme_undo(SCREEN_MAIN, false); return GO_TO_WPS; }
static int global_settings_wps_select_action = HOTKEY_ALBUM_COVERS;
#define HK_CTX_GET(a, b) (b)
static bool show_album_covers(long *go_to, bool r)
{ (void)go_to; (void)r; viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
  viewportmanager_theme_undo(SCREEN_MAIN, false); return false; }
static bool return_agents;
/* USB during playback: the consent dialog, then the USB screen, both over the WPS. */
static void usb_insert(void)
{ struct viewport parent;
  viewportmanager_theme_enable(SCREEN_MAIN, true, &parent);      /* dialog_run */
  viewportmanager_theme_undo(SCREEN_MAIN, false);
  viewportmanager_theme_enable(SCREEN_MAIN, false, &parent);     /* usb screen */
  viewportmanager_theme_undo(SCREEN_MAIN, false); }

static const int *script; static int pos;
static int wps_loop(void)
{
    long button = 0; bool restore = true; bool theme_enabled = true;
    while (1)
    {
        if (restore) { restore = false; gwps_enter_wps(theme_enabled); theme_enabled = true; }
        button = script[pos++];
        if (button == BUTTON_DONE) { gwps_leave_wps(true); return 0; }
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
    theme_stack_top[0] = -1; viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    script = s; pos = 0; panic_msg[0] = 0;
    if (setjmp(panic_jmp)) { printf("FAIL %s: panic '%s' at step %d, top %d\n", name, panic_msg, pos, theme_stack_top[0]); return 1; }
    wps_loop();
    if (theme_stack_top[0] != 0) { printf("FAIL %s: top %d after leaving WPS\n", name, theme_stack_top[0]); return 1; }
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
    return bad != 0;
}
'''
src = (src.replace("@DEPTH@", depth).replace("@ITEM@", item).replace("@THEME@", theme)
          .replace("@LEAVE@", fn(wps, "gwps_leave_wps")).replace("@RESTORE@", fn(wps, "restore_theme"))
          .replace("@LYRICS@", lyrics).replace("@COVERS@", covers))
(OUT / "fu72_wps_theme_stack.c").write_text(src)
exe = OUT / "fu72_wps_theme_stack"
subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                "-o", str(exe), str(OUT / "fu72_wps_theme_stack.c")], check=True)
r = subprocess.run([str(exe)], capture_output=True, text=True)
print(r.stdout, end="")
assert r.returncode == 0, "theme stack leaked"
print("PASS FU72 WPS theme stack stays balanced across lyrics, Cover Flow, headset and USB")
