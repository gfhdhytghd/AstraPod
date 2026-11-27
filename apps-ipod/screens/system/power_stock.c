/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stock iPod Classic 2.0.5 power screens and Home > Sleep behaviour under
 * style.menu_tree=stock (Fruit Company and Fruit Company Ultra).
 *
 * Parts: procedural battery and reflection renderer, charging standby loop,
 * low-battery screen, and the Sleep moon/stars preview and action.
 ****************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "backlight.h"
#include "button.h"
#include "font.h"
#include "lcd.h"
#include "lang.h"
#include "power.h"
#include "powermgmt.h"
#include "rbpaths.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "metadata/book_resume.h"
#include "screens/extras/extras_bar.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/activity.h"
#include "system/app_buffer.h"
#include "system/shutdown.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/menu.h"
#include "power_stock.h"

/* Main cylindrical battery geometry measured from stock 2.0.5 screenshots. */
#define BAT_X0      82
#define BAT_X1      236
#define BAT_Y0      85
#define BAT_Y1      156
#define BAT_H       (BAT_Y1 - BAT_Y0 + 1)
#define BAT_CAP_W   7
#define BAT_FILL_X0 (BAT_X0 + BAT_CAP_W)
#define BAT_FILL_X1 (BAT_X1 - BAT_CAP_W)
#define BAT_NUB_X0  (BAT_X1 + 1)
#define BAT_NUB_X1  242
#define BAT_NUB_Y0  107
#define BAT_NUB_Y1  138
#define BAT_REFL_H  37
#define BAT_REFL_Y0 (BAT_Y1 + 1)

enum battery_glyph {
    GLYPH_NONE = 0,
    GLYPH_BOLT,
    GLYPH_PLUG,
};

struct battery_spec {
    int percent;
    bool red;
    enum battery_glyph glyph;
    const char *title;
    const char *detail;
    bool show_bar;
};

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int num, int den)
{
    if (den <= 0)
        return a;
    if (num <= 0)
        return a;
    if (num >= den)
        return b;
    int ar = (int)((a >> 16) & 255), br = (int)((b >> 16) & 255);
    int ag = (int)((a >> 8) & 255), bg = (int)((b >> 8) & 255);
    int ab = (int)(a & 255), bb = (int)(b & 255);
    unsigned r = (unsigned)(ar + ((br - ar) * num) / den);
    unsigned g = (unsigned)(ag + ((bg - ag) * num) / den);
    unsigned bl = (unsigned)(ab + ((bb - ab) * num) / den);
    return (r << 16) | (g << 8) | bl;
}

static uint32_t sample_stops(const uint32_t *stops, int count, int y, int h)
{
    if (h <= 1 || count <= 1)
        return stops[0];
    int pos = y * (count - 1);
    int idx = pos / (h - 1);
    int rem = pos % (h - 1);
    if (idx >= count - 1)
        return stops[count - 1];
    return lerp_rgb(stops[idx], stops[idx + 1], rem, h - 1);
}

/* Cylindrical horizontal bulge (0..4 px) for end caps and fill meniscus. */
static int cap_bulge(int dy)
{
    int mid = BAT_H / 2;
    int d = dy - mid;
    int r2 = mid * mid;
    int inside = r2 - d * d;
    if (inside <= 0)
        return 0;
    return (inside * 4 + r2 / 2) / r2;
}

/* Signed cross product for 2D triangle hit test. */
static int tri_edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static bool in_tri(int ax, int ay, int bx, int by, int cx, int cy,
                   int px, int py)
{
    int e1 = tri_edge(ax, ay, bx, by, px, py);
    int e2 = tri_edge(bx, by, cx, cy, px, py);
    int e3 = tri_edge(cx, cy, ax, ay, px, py);
    return (e1 >= 0 && e2 >= 0 && e3 >= 0) ||
           (e1 <= 0 && e2 <= 0 && e3 <= 0);
}

/* Stock's centered lightning bolt (x 151..172, y 101..144) or plug glyph. */
static unsigned battery_glyph_alpha(enum battery_glyph glyph, int x, int y)
{
    if (glyph == GLYPH_BOLT)
    {
        bool hit = in_tri(167, 100, 151, 123, 162, 123, x, y) ||
                   in_tri(160, 119, 172, 119, 154, 145, x, y);
        return hit ? 185 : 0;
    }
    if (glyph == GLYPH_PLUG)
    {
        /* 2-prong power plug silhouette centered around (160, 121). */
        if (y >= 111 && y <= 131 && x >= 153 && x <= 165)
            return 185;
        if (y >= 114 && y <= 128 && x >= 149 && x < 153)
            return 185;
        if ((y >= 114 && y <= 117) || (y >= 125 && y <= 128))
            if (x > 165 && x <= 173)
                return 185;
        if (y >= 119 && y <= 123 && x >= 142 && x < 149)
            return 185;
    }
    return 0;
}

/* Compute one pixel of the cylindrical battery at local y `dy` (0..BAT_H-1)
 * and screen column `x`. Returns false when transparent. */
static bool battery_pixel(const struct battery_spec *spec, int x, int dy,
                          uint32_t *rgb_out, unsigned *alpha_out)
{
    /* Vertical colour stops measured on stock 2.0.5 charging screenshots. */
    static const uint32_t green_stops[] = {
        0x5a9e48, 0xb2e69c, 0x9ae27c, 0x6ac84a, 0x3b8c25, 0x397529, 0x568846,
    };
    static const uint32_t red_stops[] = {
        0x983630, 0xf29086, 0xe85448, 0xcc2c24, 0x8c1c18, 0x761c18, 0x963832,
    };
    static const uint32_t glass_stops[] = {
        0x727476, 0xa8aeb2, 0x767a7d, 0x525658, 0x424548, 0x3d4144, 0x5a5c5e,
    };
    static const uint32_t chrome_stops[] = {
        0x686c70, 0xf6f8fa, 0xc8ccd0, 0x262c32, 0x161c22, 0x363c42, 0x74787c,
    };
    static const uint32_t nub_stops[] = {
        0x9aa0a4, 0xf2f4f6, 0x9ea4a8, 0x44484c, 0x1a1e22, 0x585c60,
    };

    /* Positive terminal nub on the right side. */
    int y = BAT_Y0 + dy;
    if (x >= BAT_NUB_X0 && x <= BAT_NUB_X1)
    {
        if (y < BAT_NUB_Y0 || y > BAT_NUB_Y1)
            return false;
        int ny = y - BAT_NUB_Y0;
        int nh = BAT_NUB_Y1 - BAT_NUB_Y0 + 1;
        /* Soft rounded outer corners on the nub. */
        if (x >= BAT_NUB_X1 - 1 && (ny == 0 || ny == nh - 1))
            return false;
        uint32_t c = sample_stops(nub_stops, ARRAYLEN(nub_stops), ny, nh);
        if (x == BAT_NUB_X0 || x == BAT_NUB_X1)
            c = lerp_rgb(c, 0x202428, 1, 3);
        *rgb_out = c;
        *alpha_out = 240;
        return true;
    }

    int bulge = cap_bulge(dy);
    int left_outer = BAT_X0 + (3 - bulge);
    int right_outer = BAT_X1 - (3 - bulge);
    if (x < left_outer || x > right_outer)
        return false;

    /* Top and bottom dark metallic glass rim. */
    if (dy <= 1 || dy >= BAT_H - 2)
    {
        *rgb_out = (dy == 0 || dy == BAT_H - 1) ? 0x3c4044 : 0x585c60;
        *alpha_out = 225;
        return true;
    }

    /* Curved left and right chrome end caps. */
    int left_inner = left_outer + BAT_CAP_W;
    int right_inner = right_outer - BAT_CAP_W;
    if (x < left_inner || x > right_inner)
    {
        uint32_t c = sample_stops(chrome_stops, ARRAYLEN(chrome_stops),
                                  dy, BAT_H);
        int edge_d = x < left_inner ? (x - left_outer) : (right_outer - x);
        if (edge_d == 0)
            c = lerp_rgb(c, 0x181c20, 2, 3);
        else if (edge_d >= BAT_CAP_W - 2)
            c = lerp_rgb(c, 0x1e2428, 1, 2);
        *rgb_out = c;
        *alpha_out = 248;
        return true;
    }

    /* Interior: filled portion (green or red) vs translucent dark glass. */
    int pct = MIN(MAX(spec->percent, 0), 100);
    int fill_span = BAT_FILL_X1 - BAT_FILL_X0;
    int fill_x = BAT_FILL_X0 + (fill_span * pct) / 100;
    if (pct > 0 && fill_x < BAT_FILL_X0 + 4)
        fill_x = BAT_FILL_X0 + 4;
    /* Slight cylindrical curvature on the fill's right meniscus. */
    int meniscus = fill_x + (pct > 0 && pct < 100 ? (bulge - 2) : 0);

    uint32_t rgb;
    unsigned alpha;
    if (pct > 0 && x <= meniscus)
    {
        const uint32_t *pal = spec->red ? red_stops : green_stops;
        rgb = sample_stops(pal, ARRAYLEN(green_stops), dy - 2, BAT_H - 4);
        /* Soft edge highlight right at the meniscus. */
        if (pct < 100 && x == meniscus)
            rgb = lerp_rgb(rgb, 0xffffff, 1, 5);
        alpha = 245;
    }
    else
    {
        rgb = sample_stops(glass_stops, ARRAYLEN(glass_stops),
                           dy - 2, BAT_H - 4);
        alpha = 175;
    }

    unsigned ga = battery_glyph_alpha(spec->glyph, x, y);
    if (ga)
    {
        rgb = lerp_rgb(rgb, 0x141814, ga, 255);
        if (alpha < 225)
            alpha = 225;
    }
    *rgb_out = rgb;
    *alpha_out = alpha;
    return true;
}

static void put_battery(struct stock_canvas *c, const struct battery_spec *spec)
{
    int y0 = MAX(c->y, BAT_Y0);
    int y1 = MIN(c->y + c->height, BAT_Y1 + 1 + BAT_REFL_H);
    int x0 = MAX(c->x, BAT_X0);
    int x1 = MIN(c->x + c->width, BAT_NUB_X1 + 1);

    for (int y = y0; y < y1; y++)
    {
        bool refl = y >= BAT_REFL_Y0;
        int dy = refl ? (BAT_H - 1 - (y - BAT_REFL_Y0)) : (y - BAT_Y0);
        if (dy < 0 || dy >= BAT_H)
            continue;
        unsigned refl_scale = refl ?
            (unsigned)((BAT_REFL_H - (y - BAT_REFL_Y0)) * 110 / BAT_REFL_H) :
            255;
        if (!refl_scale)
            continue;
        for (int x = x0; x < x1; x++)
        {
            uint32_t rgb;
            unsigned alpha;
            if (!battery_pixel(spec, x, dy, &rgb, &alpha))
                continue;
            if (refl)
                alpha = (alpha * refl_scale) / 255;
            stock_canvas_blend(c, x, y, rgb, alpha);
        }
    }
}

static void put_centered_text(struct stock_canvas *c, int font,
                              const char *text, int y, uint32_t rgb)
{
    if (!text || !*text || font < 0)
        return;
    int w = 0;
    font_getstringsize((const unsigned char *)text, &w, NULL, font);
    stock_canvas_text(c, font, text, (LCD_WIDTH - w) / 2, y, rgb);
}

static void paint_power_screen(const struct battery_spec *spec,
                               int detail_font)
{
    static const char owner[] = "PowerStock";
    size_t size;
    uint32_t *pixels = app_try_claim_buffer(&size, owner);
    if (!pixels)
        return;
    int rows = MAX(1, (int)(size / (LCD_WIDTH * sizeof(uint32_t))));
    struct ui_rect clip = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    for (int y = 0; y < LCD_HEIGHT; y += rows)
    {
        struct stock_canvas band = {
            pixels, 0, y, LCD_WIDTH, MIN(rows, LCD_HEIGHT - y),
        };
        if (stock_art_ready())
            stock_art_put(&band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
        else
        {
            for (int i = 0; i < band.width * band.height; i++)
                band.pixels[i] = 0x262d34;
        }
        put_battery(&band, spec);
        if (spec->detail)
            put_centered_text(&band, detail_font, spec->detail, 198, 0xffffff);
        if (spec->show_bar)
            extras_bar_put_ex(&band, spec->title, false, false);
        stock_canvas_show(&band, clip);
    }
    app_release_buffer(owner);
    lcd_update();
}

/* ---- Charging / Standby screen ---- */

static bool is_charged(int level)
{
    if (charge_state == CHARGING || charge_state == TOPOFF)
        return false;
    return level >= 100;
}

long power_stock_charging_screen(void)
{
    long pending_event = 0;
    bool was_playing = (audio_status() & AUDIO_STATUS_PLAY) &&
                       !(audio_status() & AUDIO_STATUS_PAUSE);

    cancel_shutdown();
    if (was_playing)
        pause_action(true);
    book_resume_save();
    status_save(false);

    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    backlight_on();

    int last_level = -2;
    int last_charge = -1;
    bool last_hold = !button_hold();
    bool need_draw = true;

    /* Consume the held Play/Select that triggered standby before waiting for
     * wake input. */
    action_wait_for_release();
    button_clear_queue();

    while (1)
    {
        int level = battery_level();
        int cstate = charge_state;
        bool held = button_hold();

        if (need_draw || level != last_level || cstate != last_charge ||
            held != last_hold)
        {
            bool charged = is_charged(level);
            struct battery_spec spec = {
                .percent = level >= 0 ? level : 50,
                .red = false,
                .glyph = charged ? GLYPH_PLUG : GLYPH_BOLT,
                .title = str(charged ? LANG_POWER_STOCK_CHARGED :
                                       LANG_POWER_STOCK_CHARGING),
                .detail = NULL,
                .show_bar = true,
            };
            paint_power_screen(&spec, -1);
            last_level = level;
            last_charge = cstate;
            last_hold = held;
            need_draw = false;
        }

        if (!charger_inserted() || !battery_level_safe())
            break;

        int ev = button_get_w_tmo(HZ / 2);
        if (ev == BUTTON_NONE || ev == SYS_TIMEOUT)
            continue;

        if (ev == SYS_CHARGER_DISCONNECTED)
            break;
        if (ev == SYS_USB_CONNECTED || ev == SYS_REBOOT
#ifdef HAVE_USB_CONFIRM
            || ev == SYS_USB_CONFIRM
#endif
        )
        {
            pending_event = ev;
            break;
        }
        if (ev & SYS_EVENT)
        {
            if (ev == SYS_POWEROFF)
                cancel_shutdown();
            continue;
        }
        if (button_hold())
            continue;
        /* Any non-held user button or wheel wake exits standby, and the wake
         * press is swallowed so it does not activate the underlying menu. */
        if (!(ev & BUTTON_REL))
        {
            backlight_on();
            action_wait_for_release();
            button_clear_queue();
            break;
        }
    }

    extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    backlight_on();
    if (!pending_event)
        button_queue_post(BUTTON_REDRAW, 0);
    return pending_event;
}

/* ---- Low-Battery screen ---- */

void power_stock_low_battery_screen(bool critical)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), FONT_DIR "/13-helvetica.pbf");
    int font = font_load_ex(path, 0, global_settings.glyphs_to_cache);
    int level = battery_level();
    struct battery_spec spec = {
        .percent = critical ? 4 : MIN(MAX(level, 4), 10),
        .red = true,
        .glyph = GLYPH_NONE,
        .title = str(LANG_POWER_STOCK_LOW_BATTERY),
        .detail = str(critical ? LANG_POWER_STOCK_CONNECT_POWER :
                                 LANG_SHUTTINGDOWN),
        .show_bar = true,
    };

    backlight_on();
    paint_power_screen(&spec, font >= 0 ? font : FONT_UI);
    if (font >= 0)
        font_unload(font);
    extras_bar_release();
    sleep(HZ * 2);
}

/* ---- Home > Sleep moon/stars right-pane preview & action ---- */

static void put_star(struct stock_canvas *band, int cx, int cy, int r)
{
    int y0 = MAX(band->y, cy - r);
    int y1 = MIN(band->y + band->height, cy + r + 1);
    for (int y = y0; y < y1; y++)
    {
        int dy = abs(y - cy);
        for (int x = cx - r; x <= cx + r; x++)
        {
            int dx = abs(x - cx);
            /* 4-point star sparkle + soft halo. */
            int manhattan = dx + dy;
            int diag = MIN(dx, dy);
            unsigned a = 0;
            if (manhattan <= r && diag <= MAX(1, r / 3))
                a = (unsigned)(245 - (manhattan * 160) / MAX(1, r));
            else if (dx * dx + dy * dy <= (r * r) / 4)
                a = 230;
            if (a)
                stock_canvas_blend(band, x, y, 0xf4f7fb, a);
        }
    }
}

/* Stock 2.0.5 Home > Sleep right pane: glossy silver crescent moon and four
 * twinkling stars centered in the right pane (x 160..320). */
void power_stock_sleep_pane_paint(struct stock_canvas *band)
{
    /* Outer disk (240, 110) r=42; inner cutout (257, 102) r=36. */
    const int ox = 240, oy = 110, orad = 42;
    const int ix = 257, iy = 102, irad = 36;
    int y0 = MAX(band->y, oy - orad - 2);
    int y1 = MIN(band->y + band->height, oy + orad + 3);

    for (int y = y0; y < y1; y++)
    {
        int doy = y - oy;
        int diy = y - iy;
        for (int x = ox - orad - 2; x <= ox + orad + 2; x++)
        {
            int dox = x - ox;
            int dix = x - ix;
            int d_out = dox * dox + doy * doy;
            int d_in = dix * dix + diy * diy;
            if (d_out > orad * orad || d_in < irad * irad)
                continue;
            /* Vertical glossy silver gradient matching stock's Sleep moon. */
            int t = (y - (oy - orad)) * 255 / (2 * orad);
            uint32_t rgb = lerp_rgb(0xffffff, 0xb8c2cc, t, 255);
            /* Diagonal sheen across upper-left arc. */
            if (dox + doy < -10)
                rgb = lerp_rgb(rgb, 0xffffff, 1, 2);
            unsigned a = 240;
            if (d_out > (orad - 1) * (orad - 1) ||
                d_in < (irad + 1) * (irad + 1))
                a = 150;
            stock_canvas_blend(band, x, y, rgb, a);
        }
    }

    /* Four stars around the crescent, matching stock 2.0.5 Sleep preview. */
    put_star(band, 209, 41, 4);
    put_star(band, 278, 30, 8);
    put_star(band, 265, 51, 5);
    put_star(band, 198, 158, 5);
}

int power_stock_sleep_now(void)
{
    if (charger_inserted())
    {
        long ev = power_stock_charging_screen();
        if (ev == SYS_USB_CONNECTED)
            return default_event_handler(ev) == SYS_USB_CONNECTED ?
                   MENU_ATTACHED_USB : 0;
        if (ev)
            default_event_handler(ev);
        return 0;
    }
    sys_poweroff();
    return 0;
}
