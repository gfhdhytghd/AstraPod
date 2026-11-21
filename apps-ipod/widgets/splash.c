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
#include "system/activity.h"
#include "dialog.h"
#include "core_alloc.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "button.h"
#include "ui/scene.h"
#include "ui/sheet.h"

static long progress_next_tick, talked_tick, progress_last_call;

#define MAXLINES  (LCD_HEIGHT/6)
#define MAXBUFFER 512
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
        uint16_t offset, length;
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
                       (int)message->lines[i].length,
                       message->text + message->lines[i].offset);
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
static bool splash_present_native(struct screen *screen, const struct viewport *box,
                                  const struct splash_message *message,
                                  const struct splash_meter *meter,
                                  struct splash_wait *wait)
{
    if (box->width <= 0 || box->height <= 0 || box->width > LCD_WIDTH ||
        box->height > LCD_HEIGHT)
        return false;

    if (!wait || wait->ticks <= 0 || ui_flush_inhibited())
        return false;

    int w = box->width, h = box->height;
    size_t body = (size_t)w * h;
    int handle = core_alloc_free_space(FRAMEBUFFER_SIZE + body * sizeof(fb_data));
    if (handle <= 0)
        return false;

    ui_scene_interrupt();
    fb_data *pixels = core_get_data_pinned(handle);
    fb_data *tile_pixels = pixels + LCD_WIDTH * LCD_HEIGHT;
    memcpy(pixels, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
    struct frame_buffer_t buffer = {.fb_ptr = tile_pixels, .stride = w, .elems = body};
    struct viewport tile = *box;
    tile.x = tile.y = 0;
    tile.buffer = &buffer;
    struct viewport *previous = screen->set_viewport(&tile);
    struct viewport content;
    dialog_frame_box(screen, &tile, &message->style, &content);
    splash_draw_text(screen, &content, message);
    if (meter)
        splash_draw_meter(screen, &tile, message, meter);
    screen->set_viewport(wait->previous);

    struct ui_sheet sheet = { .light = UI_FULL_OPACITY };
    struct ui_rect bounds = {box->x, box->y, w, h};
    ui_sheet_begin(&sheet, true, "splash");
    do {
        ui_sheet_paint(&sheet, pixels, tile_pixels, bounds, false);
    } while (ui_sheet_wait(&sheet));

    bool interrupted = sheet.active;
    if (!interrupted)
    {
#if defined(HAVE_USB_PODLINK) || (defined(SIMULATOR) && defined(IPOD_6G))
        interrupted = button_queue_wait_pending(wait->ticks);
#else
        sleep(wait->ticks);
#endif
    }
    if (!interrupted)
    {
        ui_sheet_begin(&sheet, false, "splash");
        do {
            ui_sheet_paint(&sheet, pixels, tile_pixels, bounds, false);
        } while (ui_sheet_wait(&sheet));
        interrupted = sheet.active;
    }
    ui_sheet_cancel(&sheet);
    if (interrupted)
    {
        memcpy(lcd_framebuffer_default.fb_ptr, pixels, FRAMEBUFFER_SIZE);
        ui_mark_dirty(SCREEN_MAIN);
        ui_flush_dirty();
    }
    wait->ticks = 0;
    screen->set_viewport(previous);
    core_put_data_pinned(pixels);
    core_free(handle);
    return true;
}
#endif

/* The box is centred in an area area_w x area_h from vp's origin: the
 * physical display for a splash or the caller's viewport for splashf_in(). */
static bool splash_internal(struct screen *screen, const char *fmt, va_list ap,
                            struct viewport *vp, const struct splash_meter *meter,
                            int area_w, int area_h, bool whole_screen,
                            struct splash_wait *wait)
{
    static int max_width[NB_SCREENS];
    static unsigned theme_generation;
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

    dialog_style_default(&message.style);

    message.meter_gap = ui_theme_metric(UI_METRIC_SPLASH_PROGRESS_GAP);
    message.meter_radius = ui_theme_metric(UI_METRIC_SPLASH_PROGRESS_RADIUS);
    int *observed_width = &max_width[screen->screen_type];
    int old_font = vp->font;
    int body_font = -1;
    unsigned generation = ui_theme_generation();
    if (theme_generation != generation)
    {
        *observed_width = 0;
        theme_generation = generation;
    }
    bool standalone = whole_screen && screen->screen_type == SCREEN_MAIN &&
                      talk_on_ui_thread() && lcd_active() &&
                      vp->buffer == &lcd_framebuffer_default;
    if (standalone)
        body_font = ui_theme_current_font_load(UI_FONT_BODY, SPLASH_FONT_CACHE_GLYPHS);

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
        const char *start = next;
        next = text_wrap_next(next, line_width, pf, &end);
        message.lines[line].offset = start - message.text;
        message.lines[line].length = end - start;
        w = font_getstringnsize((const unsigned char *)start, end - start,
                                NULL, NULL, fontnum);
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

    int x = (area_w - width) / 2;
    int yy = (area_h - height) / 2;
    vp->x = whole_screen ? x : vp->x + x;
    vp->y = whole_screen ? yy : vp->y + yy;
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
    if (standalone)
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
        if (whole_screen)
        {
            screen->update_rect(vp->x, vp->y, vp->width, vp->height);
            painted = true;
        }
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

static bool progress_due(int current, int total, long now)
{
    return current >= total || !TIME_BEFORE(now, progress_next_tick);
}

bool splash_progress_needs_draw(int current, int total)
{
    return progress_due(current, total, current_tick);
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

    if (!progress_due(current, total, now))
        return;
    if (current < total)
    {
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
