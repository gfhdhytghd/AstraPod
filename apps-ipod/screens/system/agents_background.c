/* Pocket remote notifications. GPL version 2+. */
#include "config.h"
#include "agents_background.h"
#include "herdremote_model.h"
#include "audio/beep.h"
#include "backlight.h"
#include "button.h"
#include "core_alloc.h"
#include "draw/color.h"
#include "draw/viewport.h"
#include "draw/line.h"
#include "draw/round_rect.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "metadata/album_palette.h"
#include "plugins/rock_compat.h"
#include "podlink.h"
#include "settings/settings.h"
#include "system.h"
#include "system/activity.h"
#include "ui/animation.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "ui/native_wps.h"
#include "usb.h"
#include <stdio.h>
#include <string.h>

#define TOAST_H 44
#define TOAST_W (LCD_WIDTH - 16)
#define APPROVAL_INK LCD_RGBPACK(210, 15, 57)
#define FAILURE_INK LCD_RGBPACK(224, 79, 51)
#define WORKING_INK LCD_RGBPACK(64, 160, 43)
#define DONE_INK LCD_RGBPACK(23, 146, 153)

struct agents_bg_summary {
    bool connected;
    uint8_t waiting, failed, working, unread_done;
    uint32_t event_serial;
};
_Static_assert(sizeof(struct agents_bg_summary) <= 16, "Agent badge budget");

struct toast {
    struct agents_background_target target;
    char project_label[41], summary[124];
    uint8_t kind;                    /* agents_badge_kind */
    long duration;
};

static struct agents_bg_summary summary;
static struct toast latest;
static struct hr_popped observed;
static long toast_ends, toast_started;
static uint32_t toast_clock, shown_serial;
static volatile unsigned handshake;
static unsigned tone;
static long tone_at;
static bool needs_redraw;

static bool t3_id(const char *id)
{
    return !strncmp(id, "t3", 2);
}

static unsigned int badge_color(unsigned kind)
{
    switch (kind)
    {
        case AGENTS_BADGE_WAITING: return APPROVAL_INK;
        case AGENTS_BADGE_FAILED: return FAILURE_INK;
        case AGENTS_BADGE_WORKING: return WORKING_INK;
        default: return DONE_INK;
    }
}

static void publish(const struct toast *event)
{
    int irq = disable_irq_save();
    latest = *event;
    toast_started = 0;
    toast_ends = current_tick + event->duration;
    ++summary.event_serial;
    needs_redraw = true;
    if (event->kind != AGENTS_BADGE_DONE)
    {
        tone = event->kind == AGENTS_BADGE_WAITING ? 2 : 3;
        tone_at = current_tick;
    }
    restore_irq(irq);
    if (event->kind != AGENTS_BADGE_DONE)
        backlight_on();
}

static void clear_toast(void)
{
    latest.kind = 0;
    toast_started = 0;
    tone = 0;
    needs_redraw = true;
}

void agents_background_update(const struct hr_model *model, int type, bool foreground)
{
    if (type != 'H' && type != 'W' && type != 'S' && type != 'Q' &&
        type != 'X' && type != 'T' && type != 'E')
        return;

    /* A foreground hello can complete before the resident producer has run.
     * Repeating hr would reset the computer's current subscriptions. */
    if (type == 'H' && foreground && !handshake)
        handshake = 1;

    int waiting = 0, working = 0, failed = 0;
    const struct hr_question *new_question = NULL;
    bool toast_question_pending = false, t3_projects = false;
    for (int i = 0; i < model->nws; ++i)
    {
        const struct hr_workspace *ws = &model->ws[i];
        if (!t3_id(ws->id))
            continue;
        t3_projects = true;
        working += MAX(ws->working, ws->tab1 == 'w' || ws->other == 'w');
        failed += ws->tab1 == 'f' || ws->other == 'f';
    }
    for (int i = 0; i < model->nquestions; ++i)
    {
        const struct hr_question *q = &model->questions[i];
        if (!t3_id(q->id))
            continue;
        ++waiting;
        if (type == 'Q')
        {
            int before = observed.next;
            hr_popped_add(&observed, q->id);
            if (before != observed.next && !new_question)
                new_question = q;
        }
        if (!strcmp(q->id, latest.target.question))
            toast_question_pending = true;
    }

    bool connected = model->hello && (model->t3_health_known ? model->t3_ok :
                                      t3_projects || waiting);
    int irq = disable_irq_save();
    if (summary.connected != connected || summary.waiting != waiting ||
        summary.working != MIN(working, UINT8_MAX) || summary.failed != failed)
        needs_redraw = true;
    summary.connected = connected;
    summary.waiting = waiting;
    summary.working = MIN(working, UINT8_MAX);
    summary.failed = failed;
    if (latest.kind == AGENTS_BADGE_WAITING && !toast_question_pending)
        clear_toast();
    if (type == 'E' && model->event.kind == HR_EVENT_FAIL)
    {
        summary.failed = MAX(1, summary.failed);
        needs_redraw = true;
    }
    if (type == 'E' && !foreground && model->event.kind == HR_EVENT_DONE)
    {
        if (summary.unread_done < UINT8_MAX)
            ++summary.unread_done;
        needs_redraw = true;
    }
    restore_irq(irq);

    if (foreground || !connected)
        return;
    struct toast event = {0};
    if (new_question)
    {
        const struct hr_question *q = new_question;
        event.kind = AGENTS_BADGE_WAITING;
        event.duration = 6 * HZ;
        hr_copy(event.target.question, q->id, sizeof event.target.question);
        hr_copy(event.target.project, q->workspace, sizeof event.target.project);
        hr_copy(event.target.thread, q->pane, sizeof event.target.thread);
        hr_copy(event.target.thread_title, q->agent, sizeof event.target.thread_title);
        const char *label = q->tab;
        for (int i = 0; i < model->nws; ++i)
            if (!strcmp(model->ws[i].id, q->workspace))
            {
                label = model->ws[i].label;
                break;
            }
        hr_copy(event.project_label, label, sizeof event.project_label);
        hr_copy(event.summary, q->nlines ? q->lines[q->nlines - 1] : q->agent,
                sizeof event.summary);
    }
    else if (type == 'E')
    {
        bool failed_event = model->event.kind == HR_EVENT_FAIL;
        if (!failed_event && !is_backlight_on(true))
            return;
        event.kind = failed_event ? AGENTS_BADGE_FAILED : AGENTS_BADGE_DONE;
        event.target.status = failed_event ? 'f' : 'd';
        event.duration = (failed_event ? 5 : 4) * HZ;
        hr_copy(event.target.project, model->event.project, sizeof event.target.project);
        hr_copy(event.target.thread, model->event.thread, sizeof event.target.thread);
        hr_copy(event.target.thread_title, model->event.thread_title,
                sizeof event.target.thread_title);
        hr_copy(event.project_label, model->event.project_label, sizeof event.project_label);
        hr_copy(event.summary, model->event.summary[0] ? model->event.summary :
                model->event.thread_title, sizeof event.summary);
    }
    if (event.kind)
        publish(&event);
}

void agents_background_disconnected(void)
{
    int irq = disable_irq_save();
    summary = (struct agents_bg_summary){ .event_serial = summary.event_serial };
    memset(&observed, 0, sizeof observed);
    clear_toast();
    restore_irq(irq);
}

void agents_background_enter(void)
{
    int irq = disable_irq_save();
    summary.unread_done = 0;
    clear_toast();
    restore_irq(irq);
}

bool agents_background_take_target(struct agents_background_target *target)
{
    int irq = disable_irq_save();
    bool valid = latest.kind && toast_started && shown_serial == summary.event_serial &&
                 TIME_BEFORE(current_tick, toast_ends + 3 * HZ);
    if (valid)
        *target = latest.target;
    summary.unread_done = 0;
    clear_toast();
    restore_irq(irq);
    return valid;
}

static const char *const hello[] = { "hr\t1", "tc\t1", "tc\t2", "qx\t1" };

bool agents_background_handshake_complete(void)
{
    return handshake >= ARRAYLEN(hello);
}

void agents_background_handshake_sent(void)
{
    handshake = ARRAYLEN(hello);
}

unsigned int agents_background_next(char *record, unsigned int size)
{
    if (agents_background_foreground() || agents_background_handshake_complete() ||
        size < 4)
        return 0;
    memcpy(record, hello[handshake++], 4);
    return 4;
}

void agents_background_reset(void)
{
    handshake = 0;
    agents_background_reset_model();
    agents_background_disconnected();
}

void agents_background_play_tone(void)
{
    int irq = disable_irq_save();
    unsigned int play = tone && !TIME_BEFORE(current_tick, tone_at) ? tone : 0;
    if (play)
    {
        tone = tone == 2 ? 1 : 0;
        tone_at = current_tick + 8 * HZ / 100;
    }
    restore_irq(irq);
    if (play)
        beep_play(play == 3 ? 420 : 760, 22, 384);
}

bool agents_background_redraw_due(void)
{
    bool due = needs_redraw;
    if (tone && !TIME_BEFORE(current_tick, tone_at))
        due = true;
    if (latest.kind && toast_started)
    {
        long elapsed = current_tick - toast_started;
        long left = toast_ends - current_tick;
        due |= elapsed < HZ / 5 || (left <= HZ / 6 && left >= -HZ / 10);
    }
    if (due)
        needs_redraw = false;
    return due;
}

struct agents_badge agents_background_badge(void)
{
    int irq = disable_irq_save();
    struct agents_bg_summary badge = summary;
    restore_irq(irq);
    if (!badge.connected)
        return (struct agents_badge){0};
    const unsigned tick = current_tick;
    if (badge.waiting)
        return (struct agents_badge){ AGENTS_BADGE_WAITING, MIN(badge.waiting, 9),
                                      tick % HZ < 6 * HZ / 10 ? UINT8_MAX : 0 };
    if (badge.failed)
        return (struct agents_badge){ AGENTS_BADGE_FAILED, 0, UINT8_MAX };
    if (badge.working)
    {
        static const uint8_t breathe[] = {
            102, 105, 112, 124, 140, 158, 179, 198, 217, 233, 245, 253,
            255, 253, 245, 233, 217, 198, 179, 158, 140, 124, 112, 105
        };
        return (struct agents_badge){ AGENTS_BADGE_WORKING, 0,
               breathe[(tick / MAX(1, HZ / 10)) % ARRAYLEN(breathe)] };
    }
    if (badge.unread_done)
        return (struct agents_badge){ AGENTS_BADGE_DONE, 0, UINT8_MAX };
    return (struct agents_badge){0};
}

int agents_background_badge_width(struct agents_badge badge)
{
    return badge.count ? 20 : badge.kind ? 14 : 0;
}

void agents_background_draw_badge(struct viewport *clip, struct agents_badge badge,
                                  int x, int y, int font, unsigned background)
{
    if (!badge.kind)
        return;
    int saved_font = clip->font;
    unsigned saved_fg = clip->fg_pattern;
    int saved_mode = clip->drawmode;
    clip->drawmode = DRMODE_FG;
    unsigned color = badge_color(badge.kind);
    x -= clip->x;
    y -= clip->y;
    if (badge.ink)
    {
        clip->fg_pattern = color_blend(background, color, badge.ink);
        lcd_fillrect(x + 2, y, 3, 7);
        lcd_fillrect(x + 1, y + 1, 5, 5);
        lcd_fillrect(x, y + 2, 7, 3);
        if (badge.kind == AGENTS_BADGE_FAILED)
        {
            clip->fg_pattern = background;
            lcd_vline(x + 3, y + 1, y + 3);
            lcd_drawpixel(x + 3, y + 5);
        }
    }
    if (badge.count)
    {
        const unsigned char digits[] = { '0' + badge.count, 0 };
        clip->font = font;
        clip->fg_pattern = color;
        lcd_putsxy(x + 9, y + (7 - font_get(font)->height) / 2, digits);
    }
    clip->fg_pattern = saved_fg;
    clip->font = saved_font;
    clip->drawmode = saved_mode;
}

/* The LCD driver copies the updated rectangle before returning. Keep the
 * underlay intact for retained pages, transitions, and subsequent draws. */
static int overlay_handle;
static fb_data *overlay_pixels;
static struct ui_dirty_rect previous_banner;
static int title_font = -1, body_font = -1;
static unsigned font_generation, previous_palette;
static uint32_t previous_serial;

static void union_damage(struct ui_dirty_rect *damage, struct ui_dirty_rect rect)
{
    if (!rect.h)
        return;
    if (!damage->h)
    {
        *damage = rect;
        return;
    }
    int right = MAX(damage->x + damage->w, rect.x + rect.w);
    int bottom = MAX(damage->y + damage->h, rect.y + rect.h);
    damage->x = MIN(damage->x, rect.x);
    damage->y = MIN(damage->y, rect.y);
    damage->w = right - damage->x;
    damage->h = bottom - damage->y;
}

static void copy_banner_pixels(bool save)
{
    struct ui_dirty_rect b = previous_banner;
    fb_data *base = lcd_framebuffer_default.fb_ptr;
    for (int i = 0; i < b.h; ++i)
    {
        fb_data *cell = base + (b.y + i) * LCD_WIDTH + b.x;
        fb_data *copy = overlay_pixels + i * b.w;
        memcpy(save ? copy : cell, save ? cell : copy, b.w * sizeof(fb_data));
    }
}

static unsigned theme_color(const struct ui_theme *theme, enum ui_color_role role,
                            const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static bool eligible(bool cover_ready, bool wps)
{
    if (rock_ui_raw() || !lcd_active() || !is_backlight_on(true) ||
        ui_scrolling_held() || lcd_updates_held() || usb_host_wants_storage())
        return false;
    switch (get_current_activity())
    {
        case ACTIVITY_WPS:
            return wps;
        case ACTIVITY_ALBUMCOVERS:
            return cover_ready;
        case ACTIVITY_MAINMENU:
        case ACTIVITY_SYSTEMSCREEN:
        case ACTIVITY_SETTINGS:
        case ACTIVITY_FILEBROWSER:
        case ACTIVITY_DATABASEBROWSER:
        case ACTIVITY_PLAYLISTVIEWER:
        case ACTIVITY_PLUGINBROWSER:
        case ACTIVITY_PLAYLISTBROWSER:
        case ACTIVITY_SHORTCUTSMENU:
            return viewportmanager_theme_enabled(SCREEN_MAIN);
        default:
            return false;
    }
}

static int slide_height(uint32_t clock, long start, long end)
{
    unsigned open = ui_theme_duration(12);
    unsigned close = ui_theme_duration(10);
    unsigned frame = ui_clock_frame(USEC_TIMER - clock);
    unsigned ticks_left = MAX(0, (int)(end - current_tick));
    unsigned frames_left = ticks_left * UI_ANIM_FPS / HZ;
    if (frame < open)
    {
        struct ui_tween tween = { .from = 0, .to = TOAST_H, .frames = open };
        return ui_tween_sample(&tween, frame);
    }
    if (frames_left < close && TIME_AFTER(current_tick, start))
    {
        struct ui_tween tween = { .from = TOAST_H, .to = 0, .frames = close,
                                  .ease_in = true };
        return ui_tween_sample(&tween, close - frames_left);
    }
    return TOAST_H;
}

static int overlay_font(int font)
{
    return font < 0 ? FONT_UI : font;
}

static void paint_banner(struct ui_dirty_rect b, const struct toast *event,
                         const struct ui_theme *theme,
                         const struct ui_album_palette *palette, bool up)
{
    struct viewport vp = {0};
    viewport_set_fullscreen(&vp, SCREEN_MAIN);
    vp.x = b.x;
    vp.y = b.y;
    vp.width = b.w;
    vp.height = b.h;
    vp.drawmode = DRMODE_FG;
    struct viewport *old = lcd_set_viewport(&vp);
    unsigned accent = badge_color(event->kind);
    unsigned bg = theme_color(theme, UI_COLOR_MODAL_BG, palette);
    unsigned fg = theme_color(theme, UI_COLOR_MODAL_FG, palette);
    unsigned border = theme_color(theme, UI_COLOR_MODAL_BORDER, palette);
    unsigned muted = color_blend(bg, fg, 175);
    int y = up ? 0 : b.h - TOAST_H;

    struct screen *screen = &screens[SCREEN_MAIN];
    int radius = agents_fruit_nav() ? 6 : 0;
    vp.fg_pattern = bg;
    fill_round_rect(screen, 0, y, vp.width, TOAST_H, radius);
    vp.fg_pattern = border;
    draw_round_rect(screen, 0, y, vp.width, TOAST_H, radius, 1);
    vp.fg_pattern = accent;
    lcd_fillrect(0, y + MAX(1, radius), 3, TOAST_H - 2 * MAX(1, radius));

    char line[90], fitted[104];
    const char *kind = event->kind == AGENTS_BADGE_WAITING ? "Approval needed" :
                       event->kind == AGENTS_BADGE_FAILED ? "Failed" : "Done";
    snprintf(line, sizeof line, "%s%s%s", kind,
             event->project_label[0] ? " · " : "", event->project_label);
    vp.font = overlay_font(title_font);
    vp.fg_pattern = fg;
    lcd_putsxy(10, y + 3, (const unsigned char *)text_fit(line, fitted, sizeof fitted,
                                                       vp.font, vp.width - 20));

    static const unsigned char chord[] = "SEL+PLAY";
    vp.font = overlay_font(body_font);
    int hint_w;
    font_getstringsize(chord, &hint_w, NULL, vp.font);
    int hint_x = vp.width - hint_w - 13;
    vp.fg_pattern = muted;
    lcd_putsxy(10, y + 23, (const unsigned char *)text_fit(event->summary, fitted,
                                                        sizeof fitted, vp.font, hint_x - 17));
    vp.fg_pattern = theme_color(theme, UI_COLOR_MODAL_ACCENT, palette);
    lcd_fillrect(hint_x - 4, y + 24, hint_w + 8, 16);
    vp.fg_pattern = theme_color(theme, UI_COLOR_MODAL_SELECTED_TEXT, palette);
    lcd_putsxy(hint_x, y + 23, chord);
    lcd_set_viewport(old);
}

static void unload_overlay_fonts(void)
{
    if (title_font >= 0)
        font_unload(title_font);
    if (body_font >= 0)
        font_unload(body_font);
    title_font = body_font = -1;
}

static bool intersects(struct ui_dirty_rect a, struct ui_dirty_rect b)
{
    return a.w && a.h && b.w && b.h && a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

void agents_background_overlay(struct ui_dirty_rect *damage, bool cover_ready)
{
    bool wps = native_wps_active();
    bool can_draw = eligible(cover_ready, wps);
    int irq = disable_irq_save();
    bool has_event = can_draw && latest.kind &&
                     (!toast_started || TIME_BEFORE(current_tick, toast_ends));
    restore_irq(irq);

    if (has_event && overlay_handle <= 0)
        overlay_handle = core_alloc_free_space(TOAST_W * TOAST_H *
                                                sizeof(fb_data));
    if (overlay_handle <= 0)
        return;

    struct ui_dirty_rect banner = {0};
    struct toast event = {0};
    bool up = false;
    irq = disable_irq_save();
    if (has_event && latest.kind)
    {
        if (!toast_started)
        {
            toast_started = current_tick ?: 1;
            toast_clock = USEC_TIMER;
            toast_ends = toast_started + latest.duration;
        }
        event = latest;
    }
    long start = toast_started, end = toast_ends;
    uint32_t clock = toast_clock, serial = summary.event_serial;
    restore_irq(irq);
    if (event.kind)
    {
        int height = slide_height(clock, start, end);
        up = wps && !agents_fruit_nav() &&
             !strcmp(ui_theme_active_name(), "scrim");
        if (height)
            banner = (struct ui_dirty_rect){8, up ? 26 : LCD_HEIGHT - 6 - height,
                                            TOAST_W, height};
    }
    unsigned generation = ui_theme_generation();
    unsigned palette_generation = album_palette_generation();
    bool colors_changed = font_generation != generation ||
                          previous_palette != palette_generation;
    bool banner_changed = memcmp(&banner, &previous_banner, sizeof banner) ||
                          (banner.h && (previous_serial != serial || colors_changed));
    if (banner_changed)
    {
        union_damage(damage, previous_banner);
        union_damage(damage, banner);
    }
    previous_banner = banner;
    previous_serial = serial;
    previous_palette = palette_generation;

    if (!has_event)
    {
        overlay_handle = core_free(overlay_handle);
        unload_overlay_fonts();
        return;
    }
    if (!intersects(*damage, banner))
        return;

    struct ui_theme theme;
    struct ui_album_palette palette;
    ui_theme_snapshot(&theme);
    album_palette_snapshot(&palette);
    if (font_generation != generation || body_font < 0)
    {
        unload_overlay_fonts();
        title_font = ui_theme_font_load(&theme, UI_FONT_STATUS, 24);
        body_font = ui_theme_font_load(&theme, UI_FONT_WPS_META, 32);
        font_generation = generation;
    }

    overlay_pixels = core_get_data_pinned(overlay_handle);
    copy_banner_pixels(true);
    paint_banner(banner, &event, &theme, &palette, up);
    shown_serial = serial;
}

void agents_background_restore(void)
{
    if (!overlay_pixels)
        return;
    copy_banner_pixels(false);
    core_put_data_pinned(overlay_pixels);
    overlay_pixels = NULL;
}
