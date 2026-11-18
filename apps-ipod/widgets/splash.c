/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/splash.c
 * Copyright (C) Daniel Stenberg (2002)
 * GNU General Public License (version 2+)
 *
 * Transient centred message overlay -- splash(), splashf() and the
 * progress variant. Saves and restores whatever it covers.
 ****************************************************************************/
#include "stdarg.h"
#include "string.h"
#include "rbunicode.h"
#include "stdio.h"
#include "kernel.h"
#include "draw/screen_access.h"
#include "lang.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "splash.h"
#include "draw/viewport.h"
#include "draw/text_wrap.h"
#include "draw/progress_bar.h"
#include "font.h"
#include "rbpaths.h"
#include "skin/skin_albumart_color.h"
#include "system/activity.h"
#include "dialog.h"
#include "core_alloc.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"

static long progress_next_tick, talked_tick, progress_last_call;

#define MAXLINES  (LCD_HEIGHT/6)
#define MAXBUFFER 512
#define RECT_SPACING 10     /* inner padding between the box border and content */
#define PROGRESS_SPACING 4  /* extra gap between the text and the progress bar */
#define SPLASH_FONT_CACHE_GLYPHS 128

struct splash_meter {
    int current, total;
};

struct splash_wait {
    struct viewport *previous;
    int ticks;
};

struct splash_message {
    struct dialog_style style;
    char text[MAXBUFFER];
    struct {
        const char *str;
        size_t len;
    } lines[MAXLINES];
    int count, line_height, meter_gap, meter_radius;
};

static void splash_draw_text(struct screen *screen, struct viewport *content,
                             const struct splash_message *message)
{
    struct viewport *prev = screen->set_viewport(content);
    screen->set_drawmode(DRMODE_FG);
    for (int i = 0; i < message->count; i++)
        screen->putsxyf(0, i * message->line_height, "%.*s",
                       message->lines[i].len, message->lines[i].str);
    screen->set_drawmode(DRMODE_SOLID);
    screen->set_viewport(prev);
}

static void splash_draw_meter(struct screen *screen, const struct viewport *box,
                              const struct splash_message *message,
                              const struct splash_meter *meter)
{
    int size = message->line_height;
    int pad = message->style.box_margin;
    progress_bar_draw(screen, pad, box->height - size - pad,
                      box->width - pad * 2, size, meter->current,
                      meter->total, message->meter_radius);
}

#if LCD_DEPTH == 16 && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
enum splash_part { SPLASH_SHADOW_RIGHT, SPLASH_SHADOW_BOTTOM,
                   SPLASH_FRAME, SPLASH_TEXT, SPLASH_METER, SPLASH_PARTS };

struct splash_part_view {
    struct ui_rect rect;
    struct ui_view view;
    struct ui_layer layer;
};

static size_t splash_area(struct ui_rect rect)
{
    return rect.width > 0 && rect.height > 0 ? rect.width * rect.height : 0;
}

static uint32_t splash_rgb888(unsigned rgb565)
{
    return (RGB_UNPACK_RED(rgb565) << 16) | (RGB_UNPACK_GREEN(rgb565) << 8) |
            RGB_UNPACK_BLUE(rgb565);
}

static unsigned splash_theme_color(const struct ui_theme *theme, enum ui_color_role role,
                                   const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static bool splash_native_supported(struct screen *screen, const struct viewport *vp,
                                    struct ui_album_palette *palette)
{
    if (!ui_native_enabled() || screen->screen_type != SCREEN_MAIN ||
        !talk_on_ui_thread() || !lcd_active() || lcd_updates_held() ||
        vp->buffer != &lcd_framebuffer_default)
        return false;

    unsigned dominant, vivid;
    if (dynamic_colors_roles(&dominant, &vivid))
    {
        /* Scrim's album-derived dialog formulas retain their compatibility
         * path until the new palette has parity evidence across album states. */
        if (!strcmp(global_settings.native_theme, "scrim"))
            return false;
        palette->has_dominant = palette->has_vivid = true;
        palette->dominant = splash_rgb888(dominant);
        palette->vivid = splash_rgb888(vivid);
    }
    return true;
}

static int splash_native_style(struct splash_message *message, int font,
                               const struct ui_album_palette *palette)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    const uint16_t *metrics = theme.metric;
    message->style = (struct dialog_style){
        .box_fg = splash_theme_color(&theme, UI_COLOR_MODAL_FG, palette),
        .box_bg = splash_theme_color(&theme, UI_COLOR_MODAL_BG, palette),
        .box_border_color = splash_theme_color(&theme, UI_COLOR_MODAL_BORDER, palette),
        .box_shadow_color = splash_theme_color(&theme, UI_COLOR_MODAL_SHADOW, palette),
        .box_border_width = metrics[UI_METRIC_MODAL_BORDER_WIDTH],
        .box_margin = metrics[UI_METRIC_MODAL_PADDING],
        .box_shadow_offset = metrics[UI_METRIC_MODAL_SHADOW_OFFSET],
    };
    message->meter_gap = metrics[UI_METRIC_SPLASH_PROGRESS_GAP];
    message->meter_radius = metrics[UI_METRIC_SPLASH_PROGRESS_RADIUS];

    char path[sizeof(FONT_DIR "/.fnt") + UI_THEME_FONT_NAME_MAX];
    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", theme.font[UI_FONT_BODY]);
    if (font_filename_matches_loaded_id(font, path))
        return -1;
    return ui_theme_font_load(&theme, UI_FONT_BODY, SPLASH_FONT_CACHE_GLYPHS);
}

static void splash_capture_part(struct splash_part_view *part, fb_data *pixels,
                                size_t *offset, int handle, const struct viewport *tile)
{
    if (!splash_area(part->rect))
        return;
    struct ui_rect r = part->rect;
    part->layer = (struct ui_layer){handle, r.width, r.height, *offset};
    for (int y = r.y; y < r.y + r.height; y++)
    {
        memcpy(pixels + *offset, pixels + y * tile->width + r.x,
               r.width * sizeof(*pixels));
        *offset += r.width;
    }
}

/* Rasterize into a packed offscreen viewport, then let the retained views
 * compose the frame, shadow, label and meter over the current page. */
static bool splash_present_native(struct screen *screen, struct viewport *box,
                                  const struct splash_message *message,
                                  const struct splash_meter *meter,
                                  struct splash_wait *wait)
{
    if (box->width <= 0 || box->height <= 0 || box->x < 0 || box->y < 0 ||
        box->x + box->width > LCD_WIDTH || box->y + box->height > LCD_HEIGHT)
        return false;

    int x = box->x, y = box->y, w = box->width, h = box->height;
    int shadow = MAX(0, message->style.box_shadow_offset);
    int right = MIN(LCD_WIDTH, x + w + shadow);
    int bottom = MIN(LCD_HEIGHT, y + h + shadow);
    struct ui_rect damage = {x, y, right - x, bottom - y};
    struct dialog_insets insets;
    dialog_get_insets(&message->style, &insets);
    int line_height = message->line_height;
    int pad = message->style.box_margin;
    int shadow_right = MAX(x + w, x + shadow);
    int shadow_bottom = MAX(y + h, y + shadow);
    struct splash_part_view parts[SPLASH_PARTS] = {
        [SPLASH_SHADOW_RIGHT] = {.rect = {
            shadow_right, y + shadow, right - shadow_right, bottom - y - shadow}},
        [SPLASH_SHADOW_BOTTOM] = {.rect = {
            x + shadow, shadow_bottom, w - shadow, bottom - shadow_bottom}},
        [SPLASH_FRAME] = {.rect = {0, 0, w, h}},
        [SPLASH_TEXT] = {.rect = {
            insets.left, insets.top, w - insets.left - insets.right,
            MIN(h - insets.top - insets.bottom, message->count * line_height)}},
    };
    if (meter)
    {
        int top = MAX(0, h - line_height - pad);
        parts[SPLASH_METER].rect = (struct ui_rect){
            pad, top, w - 2 * pad, h - pad - top};
    }

    size_t body_pixels = splash_area(parts[SPLASH_FRAME].rect);
    /* The last pixel is shared by both solid shadow views. */
    size_t pixels_needed = body_pixels * 2 + 1;
    for (int i = SPLASH_TEXT; i < SPLASH_PARTS; i++)
        pixels_needed += splash_area(parts[i].rect);
    int handle = core_alloc_free_space(pixels_needed * sizeof(fb_data));
    if (handle <= 0)
        return false;

    fb_data *pixels = core_get_data_pinned(handle);
    pixels[pixels_needed - 1] = message->style.box_shadow_color;
    for (int i = SPLASH_SHADOW_RIGHT; i <= SPLASH_SHADOW_BOTTOM; i++)
        if (splash_area(parts[i].rect))
            parts[i].layer = (struct ui_layer){handle, 1, 1, pixels_needed - 1};

    struct frame_buffer_t buffer = { .fb_ptr = pixels, .stride = w, .elems = body_pixels };
    struct viewport tile = *box;
    tile.x = tile.y = 0;
    tile.buffer = &buffer;
    struct viewport *prev = screen->set_viewport(&tile);
    struct dialog_style tile_style = message->style;
    tile_style.box_shadow_offset = 0;
    struct viewport content;
    dialog_frame_box(screen, &tile, &tile_style, &content);
    size_t offset = body_pixels;
    splash_capture_part(&parts[SPLASH_FRAME], pixels, &offset, handle, &tile);
    splash_draw_text(screen, &content, message);
    splash_capture_part(&parts[SPLASH_TEXT], pixels, &offset, handle, &tile);
    if (meter)
    {
        splash_draw_meter(screen, &tile, message, meter);
        splash_capture_part(&parts[SPLASH_METER], pixels, &offset, handle, &tile);
    }
    screen->set_viewport(prev);
    core_put_data_pinned(pixels);

    static const struct ui_view_ops layer_ops = { .paint = ui_layer_paint };
    struct ui_view root;
    ui_view_init(&root, damage, NULL, NULL);
    for (int i = 0; i < SPLASH_PARTS; i++)
    {
        if (!parts[i].layer.handle)
            continue;
        struct ui_rect rect = parts[i].rect;
        if (i < SPLASH_FRAME)
        {
            rect.x -= damage.x;
            rect.y -= damage.y;
        }
        ui_view_init(&parts[i].view, rect, &layer_ops, &parts[i].layer);
        ui_view_append(&root, &parts[i].view);
    }
    struct ui_page page;
    struct ui_nav nav = {0};
    ui_page_init(&page, &root);
    ui_nav_push(&nav, &page, UI_POPUP);
    ui_nav_draw(&nav);
    if (wait && wait->ticks)
    {
        screen->set_viewport(wait->previous);
        sleep(wait->ticks);
        wait->ticks = 0;
    }
    /* The caller still paints over the last frame. */
    ui_nav_pop(&nav);
    core_free(handle);
    return true;
}
#endif

/* The box is centred in an area area_w x area_h from vp's origin: the
 * physical display for a splash, the caller's viewport for splashf_in(),
 * which has no shadow, since the shadow is drawn to the screen itself */
static bool splash_internal(struct screen *screen, const char *fmt, va_list ap,
                            struct viewport *vp, const struct splash_meter *meter,
                            int area_w, int area_h, bool shadow,
                            struct splash_wait *wait)
{
    static int max_width[NB_SCREENS];
#if LCD_DEPTH == 16 && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
    static unsigned theme_generation;
#endif
    static enum current_activity last_act = ACTIVITY_UNKNOWN;
    enum current_activity act = get_current_activity();

    if (last_act != act) /* changed activities reset max_width */
    {
        FOR_NB_SCREENS(i)
            max_width[i] = 0;
        last_act = act;
    }
    struct splash_message message;
    if (vsnprintf(message.text, sizeof(message.text), fmt, ap) <= 0)
        return false;
    const char *next = message.text;
    while (*next == ' ' || *next == '\r' || *next == '\n')
        next++;
    if (!*next)
        return false;

    /* the popup is framed with the default dialog style, but keeps its own
     * padding */
    message.style = *dialog_get_default_style();
    message.style.box_margin = RECT_SPACING;
    message.meter_gap = PROGRESS_SPACING;
    message.meter_radius = global_settings.progress_bar_radius;
    if (!shadow)
        message.style.box_shadow_offset = 0;

    int *observed_width = &max_width[screen->screen_type];
    int old_font = vp->font;
    int body_font = -1;
#if LCD_DEPTH == 16 && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
    struct ui_album_palette palette = {0};
    bool native = shadow && splash_native_supported(screen, vp, &palette);
    if (native)
    {
        unsigned generation = ui_theme_generation();
        if (theme_generation != generation)
        {
            *observed_width = 0;
            theme_generation = generation;
        }
        body_font = splash_native_style(&message, old_font, &palette);
    }
#endif

    int pad = message.style.box_margin;
    int fontnum = body_font >= 0 ? body_font : old_font;
    int space_w, chr_h;
    font_getstringsize(" ", &space_w, &chr_h, fontnum);
    message.line_height = chr_h;
    message.count = 0;

    /* A viewport too narrow for a single space, or too short for one line of
     * text, gives the word wrap below nothing to make progress on and it spins
     * forever. Nothing useful could be drawn at that size anyway. */
    int line_width = vp->width - 2 * pad;
    if (line_width < space_w || vp->height < chr_h)
    {
        if (body_font >= 0)
            font_unload(body_font);
        return false;
    }

    int maxw = MAX(0, *observed_width - 2 * pad);
    int y = chr_h + (meter ? chr_h + message.meter_gap : 0);
    font_lock(fontnum, true);
    struct font *pf = font_get(fontnum);
    while (*next)
    {
        const char *end;
        int w;

        int line = message.count;
        if (line > 0)
        {
            if ((y + chr_h * 2 > vp->height) || (line >= MAXLINES))
                break;  /* screen full or out of lines */
            y += chr_h;
        }
        message.lines[line].str = next;
        next = text_wrap_next(next, line_width, pf, &end);
        message.lines[line].len = end - message.lines[line].str;
        w = font_getstringnsize((const unsigned char *)message.lines[line].str,
                                message.lines[line].len, NULL, NULL, fontnum);
        if (w > maxw)
            maxw = w;
        message.count++;
    }
    font_lock(fontnum, false);

    /* prepare viewport
     * First boundaries, then the background filling, then the border and finally
     * the text*/

    screen->scroll_stop();

    int width = MIN(maxw + 2 * pad, vp->width);
    int height = MIN(y + 2 * pad, area_h);

    /* Centre on the physical display, not within vp: a themed SBS can inset the
     * UI viewport (e.g. %Vi(-,3,28,302,-,-)), which would leave the box visibly
     * off-centre on screen. */
    vp->x = (area_w - width) / 2;
    vp->y = (area_h - height) / 2;
    vp->width = width;
    vp->height = height;

    /* prevent artifacts by locking to max width observed on repeated calls */
    *observed_width = width;

    vp->flags |=  VP_FLAG_ALIGN_CENTER;

    if (body_font >= 0)
    {
        vp->font = body_font;
        screen->set_viewport(vp);
    }
    bool painted = false;
#if LCD_DEPTH == 16 && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
    if (native)
        painted = splash_present_native(screen, vp, &message, meter, wait);
#else
    (void)wait;
#endif

    if (!painted)
    {
        struct viewport content;
        dialog_frame_box(screen, vp, &message.style, &content);
        splash_draw_text(screen, &content, &message);
        if (meter)
            splash_draw_meter(screen, vp, &message, meter);
    }
    if (body_font >= 0)
    {
        vp->font = old_font;
        font_unload(body_font);
    }

    return !painted; /* Direct drawing needs a viewport update. */
}

void splashf(int ticks, const char *fmt, ...)
{
    va_list ap;

    /* fmt may be a so called virtual pointer. See settings.h. */
    long id;
    if((id = P2ID((const unsigned char*)fmt)) >= 0)
        /* If fmt specifies a voicefont ID, and voice menus are
           enabled, then speak it. */
        cond_talk_ids_fq(id);

    /* If fmt is a lang ID then get the corresponding string (which
       still might contain % place holders). */
    fmt = P2STR((unsigned char *)fmt);

    /* Text that is not a lang ID only the synthesiser can say. A splash
     * shown for no time is a status redrawn in a loop, so is left alone, as
     * is one whose caller has just said it and forced it to be heard out. */
    if (id < 0 && ticks > 0 && global_settings.talk_menu
        && talk_on_ui_thread() && !talk_force_enqueue_pending())
    {
        char text[128];
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        talk_text(text, false);
        talk_force_enqueue_next();
    }

    struct splash_wait wait = { .ticks = ticks };
    FOR_NB_SCREENS(i)
    {
        struct screen * screen = &(screens[i]);
        struct viewport vp;
        viewport_set_defaults(&vp, screen->screen_type);
        struct viewport *last_vp = screen->set_viewport(&vp);
        wait.previous = last_vp;

        va_start(ap, fmt);
        if (splash_internal(screen, fmt, ap, &vp, NULL, screen->lcdwidth,
                            screen->lcdheight, true, NB_SCREENS == 1 ? &wait : NULL))
            screen->update_viewport();
        va_end(ap);

        screen->set_viewport(last_vp);
    }
    if (wait.ticks)
        sleep(wait.ticks);
}

void splashf_in(struct viewport *area, const char *fmt, ...)
{
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport vp = *area;
    struct viewport *last_vp = screen->set_viewport(&vp);
    va_list ap;

    fmt = P2STR((unsigned char *)fmt);
    va_start(ap, fmt);
    splash_internal(screen, fmt, ap, &vp, NULL, area->width, area->height,
                    false, NULL);
    va_end(ap);
    screen->set_viewport(last_vp);
}

/* set delay before progress meter is shown */
void splash_progress_set_delay(long delay_ticks)
{
    progress_next_tick = current_tick + delay_ticks;
    talked_tick = 0;
}

/* Hold the meter's own "loading, n per cent" for a caller that announces the
 * same work itself and says more about it. Sticky, like the delay above: it
 * belongs to the run of calls rather than to one of them, and the caller
 * clears it when the run ends. */
static bool progress_silent = false;

void splash_progress_set_silent(bool silent)
{
    progress_silent = silent;
}

/* splash a progress meter */
void splash_progress(int current, int total, const char *fmt, ...)
{
    va_list ap;
    int vp_flag = VP_FLAG_VP_DIRTY;
    /* progress update tick */
    long now = current_tick;
    /* a run of calls with no gap of two seconds is one meter */
    bool new_run = talked_tick == 0
                   || TIME_AFTER(now, progress_last_call + HZ*2);
    progress_last_call = now;

    if (current < total)
    {
        if(TIME_BEFORE(now, progress_next_tick))
            return;
        /* limit to 20fps */
        progress_next_tick = now + HZ/20;
        vp_flag = 0; /* don't mark vp dirty to prevent flashing */
    }

    /* The meter's message, once: a percentage read out every few seconds
     * talks over everything else and says nothing the bar does not. */
    if (global_settings.talk_menu && !progress_silent && new_run)
    {
        long id = P2ID((const unsigned char *)fmt);
        talked_tick = now;
        if (id >= 0)
            talk_id(id, false);
        else
        {
            char text[128];
            va_start(ap, fmt);
            vsnprintf(text, sizeof(text), fmt, ap);
            va_end(ap);
            talk_text(text, false);
        }
    }

    /* If fmt is a lang ID then get the corresponding string (which
       still might contain % place holders). */
    fmt = P2STR((unsigned char *)fmt);
    FOR_NB_SCREENS(i)
    {
        struct screen * screen = &(screens[i]);
        struct viewport vp;
        viewport_set_defaults(&vp, screen->screen_type);
        struct viewport *last_vp = screen->set_viewport_ex(&vp, vp_flag);

        va_start(ap, fmt);
        const struct splash_meter meter = {current, total};
        if (splash_internal(screen, fmt, ap, &vp, &meter, screen->lcdwidth,
                            screen->lcdheight, true, NULL))
            screen->update_viewport();
        va_end(ap);

        screen->set_viewport(last_vp);
    }
}

void splash_progress_in(struct viewport *area, int current, int total,
                        const char *fmt, ...)
{
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport vp = *area;
    struct viewport *last_vp = screen->set_viewport(&vp);
    va_list ap;

    fmt = P2STR((unsigned char *)fmt);
    va_start(ap, fmt);
    const struct splash_meter meter = {current, total};
    splash_internal(screen, fmt, ap, &vp, &meter, area->width, area->height,
                    false, NULL);
    va_end(ap);
    screen->set_viewport(last_vp);
}
