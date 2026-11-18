/* PodBox continuity handoff. GNU General Public License, version 2+. */
#include "config.h"
#ifdef HAVE_USB_PODLINK
#include "handoff_sheet.h"
#include "audio.h"
#include "backlight.h"
#include "button.h"
#include "cider_wps.h"
#include "draw/line.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "font.h"
#include "input/action.h"
#include "kernel.h"
#include "lang.h"
#include "metadata.h"
#include "rbpaths.h"
#include "settings/settings.h"
#include "metadata/album_palette.h"
#include "ui/frame.h"
#include "ui/statusbar.h"
#include "core_alloc.h"
#include "system/shutdown.h"
#include "system.h"
#include "ui/animation.h"
#include "ui/native_theme_runtime.h"
#include "ui/perf.h"
#include "ui/retained.h"
#include "widgets/dialog.h"
#include <stdio.h>
#include <string.h>

#define NOTE_GRID 96
#define OPEN_FRAMES ANIM_F(16)
#define CLOSE_FRAMES ANIM_F(12)
#define SHEET_DIM_PERCENT 35

struct sheet_ui {
    bool leave;
    int small_font, button_font, large_font;
};

struct sheet_style {
    unsigned bg, fg, muted, unselected_text, selected_text, placeholder;
    enum ui_bg_kind background;
    int margin, cover_size, button_height, button_gap, metadata_gap, line_gap;
    int progress_gap, progress_height, error_offset, outline_width;
    int scrim_alpha, cover_border_alpha, button_border_alpha, progress_alpha;
};

struct retained_sheet;

struct sheet_presentation {
    struct viewport vp, *outer;
    fb_data *fb, *back, *card;
    int handle, offset;
    unsigned open_frames, close_frames;
    unsigned open_trace;
    bool inhibited, boosted, interrupted;
    struct retained_sheet *retained;
};

/* Only the UI thread enters a handoff sheet or interrupts it for a modal. */
static struct sheet_presentation *native_sheet;

static void center_text(int x, int y, int width, int height, int font,
                        unsigned color, const unsigned char *text)
{
    int w, h;
    lcd_setfont(font);
    lcd_getstringsize(text, &w, &h);
    lcd_set_foreground(color);
    lcd_putsxy(x + (width - w) / 2, y + (height - h) / 2, text);
}

static void outline(int x, int y, int width, int height, int line, unsigned alpha)
{
    if (!line)
        return;
    lcd_blendrect(x, y, width, line, alpha);
    lcd_blendrect(x, y + height - line, width, line, alpha);
    if (height > 2 * line)
    {
        lcd_blendrect(x, y + line, line, height - 2 * line, alpha);
        lcd_blendrect(x + width - line, y + line, line, height - 2 * line, alpha);
    }
}

static void cover_placeholder(const struct sheet_style *s, int x, int y, int size)
{
    lcd_set_foreground(s->placeholder);
    lcd_fillrect(x, y, size, size);
    lcd_set_foreground(s->muted);
    /* The note glyph is authored on a 96 px grid and follows the cover size. */
    const int stem = MAX(1, 2 * size / NOTE_GRID);
    const int head_w = 10 * size / NOTE_GRID, head_h = 6 * size / NOTE_GRID;
    int left = x + 40 * size / NOTE_GRID, right = x + 60 * size / NOTE_GRID;
    int first = y + 38 * size / NOTE_GRID, second = y + 34 * size / NOTE_GRID;
    lcd_fillrect(x + 32 * size / NOTE_GRID, y + 56 * size / NOTE_GRID, head_w, head_h);
    lcd_fillrect(x + 52 * size / NOTE_GRID, y + 52 * size / NOTE_GRID, head_w, head_h);
    lcd_fillrect(left, first, stem, 20 * size / NOTE_GRID);
    lcd_fillrect(right, second, stem, 20 * size / NOTE_GRID);
    for (int i = 0; i < stem; ++i)
        lcd_drawline(left, first + i, right + stem - 1, second + i);
}

static void art_square(const struct sheet_style *s, int handle, int x, int y)
{
    struct bitmap *bm;
    bool drawn = false;
    if (wps_art_pin(handle, true))
    {
        if (wps_art_bitmap(handle, &bm) && bm->width > 0 && bm->height > 0)
        {
            /* Square center crop. A native pixel row is the only scratch. */
            const fb_data *source = (const fb_data *)bm->data;
            int side = MIN(bm->width, bm->height);
            source += (bm->height - side) / 2 * bm->width + (bm->width - side) / 2;
            fb_data row[LCD_WIDTH];
            int size = s->cover_size;
            for (int dy = 0; dy < size; ++dy)
            {
                const fb_data *src = source + dy * side / size * bm->width;
                for (int dx = 0; dx < size; ++dx)
                    row[dx] = src[dx * side / size];
                lcd_bitmap(row, x, y + dy, size, 1);
            }
            drawn = true;
        }
        wps_art_pin(handle, false);
    }
    if (!drawn)
        cover_placeholder(s, x, y, s->cover_size);
    lcd_set_foreground(s->fg);
    outline(x, y, s->cover_size, s->cover_size, s->outline_width, s->cover_border_alpha);
}

static void sheet_button(const struct sheet_style *s, int x, int y, int width, bool focus,
                         int font, const unsigned char *text)
{
    lcd_set_foreground(s->fg);
    if (focus)
        lcd_fillrect(x, y, width, s->button_height);
    else
        outline(x, y, width, s->button_height, s->outline_width, s->button_border_alpha);
    center_text(x, y, width, s->button_height, font,
                focus ? s->selected_text : s->unselected_text, text);
}

struct sheet_layout {
    const struct sheet_style *style;
    int small, button, large;
    int small_h, button_h, large_h;
    int cover_y, button_y, bar_y, text_y, text_h, inner;
};

static struct sheet_layout sheet_measure(const struct sheet_ui *ui, const struct sheet_style *s)
{
    const int small = ui->small_font >= 0 ? ui->small_font : screens[0].getuifont();
    const int button = ui->button_font >= 0 ? ui->button_font : small;
    const int large = ui->large_font >= 0 ? ui->large_font : font_get_ui_bold();
    const int small_h = font_get(small)->height, large_h = font_get(large)->height;
    const int text_h = 2 * small_h + large_h + 2 * s->line_gap;
    const int button_y = LCD_HEIGHT - s->margin - s->button_height;
    const int cover_y = (button_y - (s->cover_size + s->metadata_gap + text_h)) / 2;
    return (struct sheet_layout){
        .style = s, .small = small, .button = button, .large = large,
        .small_h = small_h, .button_h = font_get(button)->height, .large_h = large_h,
        .cover_y = cover_y, .button_y = button_y,
        .bar_y = button_y + s->button_height + s->progress_gap,
        .text_y = cover_y + s->cover_size + s->metadata_gap, .text_h = text_h,
        .inner = LCD_WIDTH - 2 * s->margin,
    };
}

static void sheet_background(const struct sheet_style *s, int handle, fb_data *pixels)
{
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(s->bg);
    if (s->background == UI_BG_SOLID ||
        !album_art_render_background(pixels, LCD_WIDTH, LCD_HEIGHT, handle,
                                     s->background == UI_BG_BLURRED_COVER, s->fg))
        lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    else if (s->background == UI_BG_COVER_SCRIM)
        lcd_blendrect(0, 0, LCD_WIDTH, LCD_HEIGHT, s->scrim_alpha);
}

static void sheet_metadata(const struct handoff_sheet *view,
                           const struct sheet_layout *m, int x, int line)
{
    const struct sheet_style *s = m->style;
    const int inner = m->inner;
    char fitted[TEXT_FIT_BUF];
    center_text(x, line, inner, m->small_h, m->small, s->muted, str(LANG_HANDOFF_TITLE));
    line += m->small_h + s->line_gap;
    center_text(x, line, inner, m->large_h, m->large, s->fg,
                text_fit(view->title, fitted, sizeof fitted, m->large, inner));
    line += m->large_h + s->line_gap;
    char subtitle[112];
    struct mp3entry *track = audio_current_track();
    const char *artist = track && track->artist ? track->artist : "";
    snprintf(subtitle, sizeof subtitle, "%s%s%.96s", view->target,
             *view->target && *artist ? " · " : "", artist);
    center_text(x, line, inner, m->small_h, m->small, s->muted,
                text_fit(subtitle, fitted, sizeof fitted, m->small, inner));
}

static void sheet_controls(const struct handoff_sheet *view, const struct sheet_ui *ui,
                           const struct sheet_layout *m, int x, int y)
{
    const struct sheet_style *s = m->style;
    const int inner = m->inner;
    if (view->reason)
    {
        center_text(x, y - s->error_offset, inner, m->large_h, m->large, s->fg,
                    str(LANG_HANDOFF_FAILED));
        center_text(x, y + m->large_h, inner, m->small_h, m->small, s->muted,
                    str(view->reason));
        return;
    }

    if (view->preparing)
    {
        center_text(x, y, inner, m->button_h, m->button, s->fg,
                    str(LANG_HANDOFF_PREPARING));
        lcd_set_foreground(s->fg);
        lcd_blendrect(x, y + s->button_height + s->progress_gap, inner,
                      s->progress_height, s->progress_alpha);
        return;
    }
    int width = (inner - s->button_gap) / 2;
    sheet_button(s, x, y, width, !ui->leave, m->button, str(LANG_HANDOFF_ACCEPT));
    sheet_button(s, x + width + s->button_gap, y, width, ui->leave, m->button,
                 str(LANG_HANDOFF_LEAVE));
}

static int sheet_action(struct sheet_ui *ui, const struct handoff_sheet *view, int action)
{
    if (IS_SYSEVENT(action) && action != SYS_USB_CONFIRM)
        default_event_handler(action);
    if (action == ACTION_STD_MENU) return DIALOG_CANCEL;
    if (view->reason) return DIALOG_CONTINUE;
    if (view->preparing) return DIALOG_CONTINUE;
    int button;
    get_action_statuscode(&button);
    if (action == ACTION_STD_OK && button != BUTTON_RIGHT)
        return ui->leave ? DIALOG_CANCEL : DIALOG_ACCEPT;
    if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT ||
        (action == ACTION_STD_OK && button == BUTTON_RIGHT))
        ui->leave = true;
    if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT ||
        (action == ACTION_STD_CANCEL && button == BUTTON_LEFT))
        ui->leave = false;
    return DIALOG_CONTINUE;
}

struct retained_sheet {
    struct ui_view backdrop, cover, metadata, controls;
    struct ui_view below, image, input, progress;
    struct ui_page card, base, overlay;
    struct ui_nav nav;
    struct ui_layer base_pixels, card_pixels;
    struct frame_buffer_t card_buffer;
    struct sheet_layout layout;
    const struct handoff_sheet *model;
    struct sheet_ui *input_state;
    int art, disposition;
};

static struct viewport *component_canvas(struct viewport *vp,
                                         struct retained_sheet *s,
                                         struct ui_rect clip)
{
    bool buffered = s && s->card_pixels.handle > 0;
    *vp = (struct viewport){ .buffer = buffered ? &s->card_buffer : NULL };
    viewport_set_fullscreen(vp, SCREEN_MAIN);
    vp->x = clip.x;
    vp->y = clip.y;
    vp->width = clip.width;
    vp->height = clip.height;
    struct viewport *old = lcd_set_viewport(vp);
    lcd_set_drawmode(DRMODE_FG);
    return old;
}

static void paint_background(const struct ui_view *v, const struct ui_view_frame *frame)
{
    struct retained_sheet *s = v->data;
    struct viewport vp;
    struct viewport *old = component_canvas(&vp, s, frame->clip);
    sheet_background(s->layout.style, s->art, s->card_buffer.fb_ptr);
    lcd_set_viewport(old);
}

static void paint_cover(const struct ui_view *v, const struct ui_view_frame *frame)
{
    struct retained_sheet *s = v->data;
    struct viewport vp;
    struct viewport *old = component_canvas(&vp, s, frame->clip);
    art_square(s->layout.style, s->art, frame->bounds.x - vp.x, frame->bounds.y - vp.y);
    lcd_set_viewport(old);
}

static void paint_metadata(const struct ui_view *v, const struct ui_view_frame *frame)
{
    struct retained_sheet *s = v->data;
    struct viewport vp;
    struct viewport *old = component_canvas(&vp, s, frame->clip);
    sheet_metadata(s->model, &s->layout, s->layout.style->margin - vp.x, frame->bounds.y - vp.y);
    lcd_set_viewport(old);
}

static void paint_controls(const struct ui_view *v, const struct ui_view_frame *frame)
{
    struct retained_sheet *s = v->data;
    struct viewport vp;
    struct viewport *old = component_canvas(&vp, s, frame->clip);
    sheet_controls(s->model, s->input_state, &s->layout,
                   s->layout.style->margin - vp.x, s->layout.button_y - vp.y);
    lcd_set_viewport(old);
}

static void paint_progress(const struct ui_view *v, const struct ui_view_frame *frame)
{
    const struct retained_sheet *s = v->data;
    struct viewport vp;
    struct viewport *old = component_canvas(&vp, NULL, frame->clip);
    lcd_set_foreground(s->layout.style->fg);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_viewport(old);
}

static bool route_controls(struct ui_view *v, int action)
{
    struct retained_sheet *s = v->data;
    s->disposition = sheet_action(s->input_state, s->model, action);
    return true;
}

static const struct ui_view_ops background_ops = { .paint = paint_background };
static const struct ui_view_ops cover_ops = { .paint = paint_cover };
static const struct ui_view_ops metadata_ops = { .paint = paint_metadata };
static const struct ui_view_ops controls_ops = { .paint = paint_controls };
static const struct ui_view_ops progress_ops = { .paint = paint_progress };
static const struct ui_view_ops input_ops = { .action = route_controls };
static const struct ui_view_ops image_ops = { .paint = ui_layer_paint };
static const struct ui_rect sheet_bounds = {0, 0, LCD_WIDTH, LCD_HEIGHT};

static void retained_sheet_init(struct retained_sheet *s, struct sheet_presentation *p,
                                const struct handoff_sheet *model, struct sheet_ui *input,
                                const struct sheet_layout *m)
{
    s->model = model;
    s->input_state = input;
    s->layout = *m;
    s->card_buffer = lcd_framebuffer_default;
    if (p->card)
        s->card_buffer.fb_ptr = p->card;
    const struct sheet_style *style = m->style;
    int cover_size = style->cover_size;
    int buttons_y = MIN(m->button_y - style->error_offset,
                        m->button_y + (style->button_height - m->button_h) / 2);
    struct ui_rect buttons = {0, buttons_y, LCD_WIDTH, LCD_HEIGHT - buttons_y};

    ui_view_init(&s->backdrop, sheet_bounds, &background_ops, s);
    ui_view_init(&s->cover, (struct ui_rect){(LCD_WIDTH - cover_size) / 2,
                 m->cover_y, cover_size, cover_size}, &cover_ops, s);
    ui_view_init(&s->metadata, (struct ui_rect){0, m->text_y, LCD_WIDTH, m->text_h}, &metadata_ops, s);
    ui_view_init(&s->controls, buttons, &controls_ops, s);
    ui_view_append(&s->backdrop, &s->cover);
    ui_view_append(&s->backdrop, &s->metadata);
    ui_view_append(&s->backdrop, &s->controls);
    ui_page_init(&s->card, &s->backdrop);

    s->base_pixels = (struct ui_layer){.handle = p->handle, .width = LCD_WIDTH,
                                       .height = LCD_HEIGHT};
    s->card_pixels = s->base_pixels;
    s->card_pixels.handle = p->card ? p->handle : -1;
    s->card_pixels.offset_pixels = LCD_WIDTH * LCD_HEIGHT;
    ui_view_init(&s->below, sheet_bounds, &image_ops, &s->base_pixels);
    ui_page_init(&s->base, &s->below);
    ui_view_init(&s->image, sheet_bounds, &image_ops, &s->card_pixels);
    s->image.value[UI_OFFSET_Y] = p->offset;
    s->below.value[UI_CLIP_HEIGHT] = p->offset;
    ui_view_init(&s->input, buttons, &input_ops, s);
    ui_view_init(&s->progress, (struct ui_rect){style->margin, m->bar_y, 0,
                 style->progress_height}, &progress_ops, s);
    ui_view_append(&s->image, &s->input);
    ui_view_append(&s->image, &s->progress);
    ui_page_init(&s->overlay, &s->image);
    s->overlay.focus = &s->input;
    ui_nav_push(&s->nav, &s->base, UI_PAGE);
    ui_nav_push(&s->nav, &s->overlay, UI_SHEET);
}

static void draw_retained_card(struct sheet_presentation *p)
{
    struct retained_sheet *s = p->retained;
    s->art = album_art_current();
    ui_page_invalidate(&s->card, sheet_bounds);
    ui_page_set(&s->overlay, &s->progress, UI_WIDTH, 0);
    if (p->card)
        ui_page_paint(&s->card);
    else
        ui_page_draw(&s->card);
}

static void present_sheet(struct sheet_presentation *p, int offset, bool repaint)
{
    struct retained_sheet *s = p->retained;
    /* The opaque card invalidates its old and new rectangles. The substrate
     * clip follows that movement so covered rows are never composited.
     * Derive brightness from the same eased position in both directions. */
    s->below.value[UI_CLIP_HEIGHT] = offset;
    int dim = (LCD_HEIGHT - offset) * UI_FULL_OPACITY * SHEET_DIM_PERCENT
                / (LCD_HEIGHT * 100);
    ui_page_set(&s->base, &s->below, UI_BRIGHTNESS, UI_FULL_OPACITY - dim);
    ui_page_set(&s->overlay, &s->image, UI_OFFSET_Y, offset);
    if (repaint)
        ui_page_invalidate(&s->overlay, (struct ui_rect){0, offset, LCD_WIDTH,
                                                       LCD_HEIGHT - offset});
    if (p->card)
        ui_nav_draw(&s->nav);
}

static int progress_width(const struct handoff_sheet *view, int inner_width)
{
    if (!view->armed || TIME_BEFORE(current_tick, view->start)) return 0;
    unsigned long age = MIN(current_tick - view->start, 3 * HZ);
    return inner_width * age / (3 * HZ);
}

static int draw_progress(const struct handoff_sheet *view, struct sheet_presentation *p,
                         const struct sheet_layout *m, int painted)
{
    int width = progress_width(view, m->inner);
    if (width <= painted) return painted;
    if (p->card)
    {
        struct retained_sheet *s = p->retained;
        ui_page_set(&s->overlay, &s->progress, UI_WIDTH, width);
        ui_nav_draw(&s->nav);
        return width;
    }
    const struct sheet_style *s = m->style;
    lcd_set_foreground(s->fg);
    lcd_fillrect(s->margin, m->bar_y, width, s->progress_height);
    lcd_update_rect(s->margin + painted, m->bar_y, width - painted, s->progress_height);
    return width;
}

static void set_boost(struct sheet_presentation *p, bool boosted)
{
    if (p->boosted != boosted)
    {
        cpu_boost(boosted);
        p->boosted = boosted;
    }
}

static void release_presentation(struct sheet_presentation *p)
{
    if (p->back)
    {
        memcpy(p->fb, p->back, FRAMEBUFFER_SIZE);
        if (!p->card)
            lcd_update();
        core_unpin(p->handle);
        core_free(p->handle);
    }
    set_boost(p, false);
    lcd_set_viewport(p->outer);
    ui_inhibit_flush(true);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (ui_hold_scrolling(false))
        sb_invalidate();
    ui_inhibit_flush(p->inhibited);
}

void handoff_sheet_interrupt(void)
{
    struct sheet_presentation *p = native_sheet;
    if (!p)
        return;
    native_sheet = NULL;
    p->interrupted = true;
    lcd_set_viewport(&p->vp);
    if (p->card)
        present_sheet(p, LCD_HEIGHT, false);
    if (p->retained)
        ui_nav_pop(&p->retained->nav);
    release_presentation(p);
}

static void close_native_sheet(struct sheet_presentation *p)
{
    if (!p->card || p->offset == LCD_HEIGHT)
        return;
    unsigned trace = ui_perf_begin("handoff-close");
    set_boost(p, true);
    struct ui_tween tween = {
        .start_clock = USEC_TIMER,
        .from = p->offset, .to = LCD_HEIGHT, .frames = p->close_frames,
        .ease_in = true,
    };
    while (p->offset < LCD_HEIGHT)
    {
        uint32_t elapsed = (uint32_t)USEC_TIMER - tween.start_clock;
        int next = ui_tween_sample(&tween, ui_clock_frame(elapsed));
        if (next != p->offset)
        {
            present_sheet(p, next, false);
            p->offset = next;
        }
        int ticks = ui_clock_timeout(elapsed, tween.frames, HZ);
        if (ticks && button_queue_wait_pending(ticks))
        {
            /* Finish immediately. The next owner will read the same event. */
            present_sheet(p, LCD_HEIGHT, false);
            p->offset = LCD_HEIGHT;
        }
    }
    ui_perf_end(trace);
}

static int run_sheet(struct handoff_sheet *view, struct sheet_ui *ui,
                     struct sheet_presentation *p, int (*advance)(int),
                     const struct sheet_layout *layout)
{
    struct ui_tween tween = { .from = LCD_HEIGHT, .frames = p->open_frames };
    bool dirty = true, entering = true;
    int painted = 0;
    for (;;)
    {
        lcd_set_viewport(&p->vp);
        if (dirty)
        {
            painted = 0;
            draw_retained_card(p);
        }
        if (entering)
        {
            tween.start_clock = USEC_TIMER;
            entering = false;
        }
        int ticks = HZ / 10;
        if (p->card && (p->offset || dirty))
        {
            unsigned frame = ui_clock_frame((uint32_t)USEC_TIMER - tween.start_clock);
            int next = p->offset ? ui_tween_sample(&tween, frame) : 0;
            if (!next && p->offset && frame < tween.frames)
                next = 1;
            if (p->offset != next || dirty)
                present_sheet(p, next, dirty);
            p->offset = next;
            if (next)
                ticks = ui_clock_timeout((uint32_t)USEC_TIMER - tween.start_clock,
                                         tween.frames, HZ);
        }
        if (!p->offset)
        {
            ui_perf_end(p->open_trace);
            p->open_trace = 0;
            set_boost(p, false);
            if (!view->reason)
                painted = draw_progress(view, p, layout, painted);
        }

        bool leave = ui->leave, preparing = view->preparing;
        const char *target = view->target;
        int reason = view->reason;
        int action = get_action(CONTEXT_STD, ticks);
        if (!p->interrupted &&
            (action == SYS_USB_CONNECTED || action == SYS_USB_DISCONNECTED ||
             action == SYS_POWEROFF || action == SYS_REBOOT))
        {
            handoff_sheet_interrupt();
            default_event_handler(action);
        }
        if (p->interrupted)
        {
            /* The system modal took over the display, not the proposal.
             * Let the protocol detect lost links and keep a live ticket. */
            int result = advance(DIALOG_CONTINUE);
            return result == DIALOG_CONTINUE ? DIALOG_ABORT : result;
        }
        if (action == SYS_ACCESSORY_CONNECTED && p->offset)
        {
            /* Dock identification has no screen of its own. Hand it the
             * settled display without dismissing the current proposal. */
            present_sheet(p, 0, false);
            p->offset = 0;
            set_boost(p, false);
        }
        ui_nav_dispatch(&p->retained->nav, action);
        int result = advance(p->retained->disposition);
        if (result != DIALOG_CONTINUE)
        {
            ui_perf_end(p->open_trace);
            p->open_trace = 0;
            if (p->close_frames)
                close_native_sheet(p);
            else if (p->card)
                present_sheet(p, LCD_HEIGHT, false);
            return result;
        }
        dirty = leave != ui->leave || preparing != view->preparing ||
                target != view->target || reason != view->reason;
    }
}

static NO_INLINE int run_retained_sheet(struct handoff_sheet *view, struct sheet_ui *ui,
                                        struct sheet_presentation *p, int (*advance)(int),
                                        const struct sheet_layout *layout)
{
    struct retained_sheet retained = {0};
    p->retained = &retained;
    retained_sheet_init(&retained, p, view, ui, layout);
    if (p->card && p->open_frames)
        p->open_trace = ui_perf_begin("handoff-open");
    int answer = run_sheet(view, ui, p, advance, layout);
    ui_perf_end(p->open_trace);
    if (!p->interrupted)
        ui_nav_pop(&retained.nav);
    p->retained = NULL;
    return answer;
}

static unsigned themed_color(const struct ui_theme *theme, const struct ui_album_palette *palette,
                             enum ui_color_role role)
{
    uint32_t color = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

static NO_INLINE void load_native_style(struct sheet_style *s, struct sheet_ui *ui)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    struct ui_album_palette palette;
    album_palette_snapshot(&palette);
    const uint16_t *m = theme.metric;
    *s = (struct sheet_style){
        .bg = themed_color(&theme, &palette, UI_COLOR_HANDOFF_BG),
        .fg = themed_color(&theme, &palette, UI_COLOR_HANDOFF_FG),
        .muted = themed_color(&theme, &palette, UI_COLOR_HANDOFF_MUTED),
        .unselected_text = themed_color(&theme, &palette, UI_COLOR_HANDOFF_UNSELECTED_TEXT),
        .selected_text = themed_color(&theme, &palette, UI_COLOR_HANDOFF_SELECTED_TEXT),
        .placeholder = themed_color(&theme, &palette, UI_COLOR_HANDOFF_COVER_PLACEHOLDER),
        .background = theme.background[UI_BG_HANDOFF],
        .margin = m[UI_METRIC_HANDOFF_MARGIN],
        .cover_size = m[UI_METRIC_HANDOFF_COVER_SIZE],
        .button_height = m[UI_METRIC_HANDOFF_BUTTON_HEIGHT],
        .button_gap = m[UI_METRIC_HANDOFF_BUTTON_GAP],
        .metadata_gap = m[UI_METRIC_HANDOFF_METADATA_GAP],
        .line_gap = m[UI_METRIC_HANDOFF_LINE_GAP],
        .progress_gap = m[UI_METRIC_HANDOFF_PROGRESS_GAP],
        .progress_height = m[UI_METRIC_HANDOFF_PROGRESS_HEIGHT],
        .error_offset = m[UI_METRIC_HANDOFF_ERROR_OFFSET],
        .outline_width = m[UI_METRIC_HANDOFF_OUTLINE_WIDTH],
        .scrim_alpha = m[UI_METRIC_HANDOFF_SCRIM_ALPHA],
        .cover_border_alpha = m[UI_METRIC_HANDOFF_COVER_BORDER_ALPHA],
        .button_border_alpha = m[UI_METRIC_HANDOFF_BUTTON_BORDER_ALPHA],
        .progress_alpha = m[UI_METRIC_HANDOFF_PROGRESS_ALPHA],
    };
    ui->small_font = ui_theme_font_load(&theme, UI_FONT_HANDOFF_CAPTION, 48);
    ui->button_font = ui_theme_font_load(&theme, UI_FONT_BODY, 48);
    ui->large_font = ui_theme_font_load(&theme, UI_FONT_HANDOFF_TITLE, 48);
}

int handoff_sheet_run(struct handoff_sheet *view, int (*advance)(int))
{
    struct sheet_style style;
    struct sheet_ui ui = {0};
    load_native_style(&style, &ui);
    struct sheet_layout layout = sheet_measure(&ui, &style);
    struct sheet_presentation p = {
        .open_frames = ui_theme_duration(OPEN_FRAMES),
        .close_frames = ui_theme_duration(CLOSE_FRAMES),
        .inhibited = ui_flush_inhibited(),
    };
    p.handle = core_alloc_free_space(HANDOFF_FRAME_BYTES);
    bool cached_card = p.handle > 0;
    if (!cached_card)
        p.handle = core_alloc_free_space(FRAMEBUFFER_SIZE);
    viewport_set_fullscreen(&p.vp, SCREEN_MAIN);
    p.outer = lcd_set_viewport(&p.vp);
    p.fb = FBADDR(0, 0);
    if (p.handle > 0)
    {
        core_pin(p.handle);
        p.back = core_get_data(p.handle);
        if (cached_card)
        {
            p.card = p.back + LCD_WIDTH * LCD_HEIGHT;
            if (p.open_frames)
                p.offset = LCD_HEIGHT;
        }
        memcpy(p.back, p.fb, FRAMEBUFFER_SIZE);
    }
    ui_inhibit_flush(true);
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    screens[0].scroll_stop();
    ui_hold_scrolling(true);
    backlight_on();
    action_handoff_enter();
    ui_inhibit_flush(false);
    set_boost(&p, true);

    native_sheet = &p;
    int result = run_retained_sheet(view, &ui, &p, advance, &layout);
    native_sheet = NULL;
    if (!p.interrupted)
        release_presentation(&p);
    if (ui.large_font >= 0) font_unload(ui.large_font);
    if (ui.button_font >= 0) font_unload(ui.button_font);
    if (ui.small_font >= 0) font_unload(ui.small_font);
    action_handoff_enter();
    return result;
}

void handoff_present_remote(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
#endif
