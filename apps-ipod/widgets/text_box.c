/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lib/simple_viewer.c
 * Copyright (C) 2010 Teruaki Kawashima
 *
 * Core (non-plugin) full-screen scrollable text viewer, ported from the
 * simple_viewer plugin lib (apps/plugins/lib/simple_viewer.c). Used by the
 * Track Info screen (browse_id3) to display a single tag value full-screen.
 * GNU General Public License (version 2+)
 *
 * Full-screen scrollable display for a string already in memory: word
 * wraps, paginates and scrolls. Distinct from viewers/text_viewer, which
 * streams documents from a file.
 ****************************************************************************/

#include <ctype.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "system.h"          /* MIN */
#include "lcd.h"
#include "font.h"
#include "kernel.h"          /* yield, TIMEOUT_BLOCK */
#include "input/action.h"
#include "settings/settings.h"        /* global_settings */
#include "draw/screen_access.h"   /* screens[] */
#include "draw/viewport.h"
#include "draw/scrollbar.h"
#include "metadata/album_palette.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "system/shutdown.h"
#include "screens/playback/track_info.h"
#include "text_box.h"         /* view_text prototype */
#include "draw/text_wrap.h"
#include "speech/talk.h"

/* The text keeps this far from either side of the screen, and from the
 * scroll bar, which sits TB_EDGE in from its own side */
#define TB_MARGIN  6
#define TB_BAR_GAP 4
#define TB_EDGE    2
/* Above and below the title, and below the rule under it */
#define TB_TITLE_PAD 4

struct view_info {
    struct ui_view view;
    struct ui_nav nav;
    struct ui_page page;
    int handle;
    struct font* pf;
    int font;
    int title_font;     /* the theme's bold UI font */
    int title_h;        /* the title's band, rule included; 0 for none */
    struct viewport scrollbar_vp; /* viewport for scrollbar */
    struct viewport vp;
    const char *title;
    const char *text;   /* displayed text */
    int display_lines;  /* number of lines can be displayed */
    int line_count;     /* number of lines */
    int line;           /* current first line */
    int start;          /* possition of first line in text  */
};

/* Where the line after the one starting at `text` starts; *end is where that
 * one's own text ends. */
static const char* get_next_line(const char *text, struct view_info *info,
                                 const char **end)
{
    const char *next;

    font_lock(info->font, true);
    next = text_wrap_next_punct(text, info->vp.width, info->pf, end);
    font_lock(info->font, false);
    return next;
}

static void calc_line_count(struct view_info *info)
{
    const char *ptr = info->text, *end;
    int i = 0;
    bool scrollbar = false;

    while (*ptr)
    {
        ptr = get_next_line(ptr, info, &end);
        i++;
        if (!scrollbar && i > info->display_lines)
        {
            ptr = info->text;
            i = 0;
            int bar_w = global_settings.scrollbar_width;
            int text_l = info->vp.x, text_r = info->vp.x + info->vp.width;

            info->scrollbar_vp = info->vp;
            info->scrollbar_vp.width = bar_w;
            /* Off still has one, on the right: a page longer than the
             * screen has no other sign of what is below */
            if (global_settings.scrollbar == SCROLLBAR_LEFT)
            {
                info->scrollbar_vp.x = TB_EDGE;
                text_l = MAX(text_l, TB_EDGE + bar_w + TB_BAR_GAP);
            }
            else
            {
                info->scrollbar_vp.x = text_r + TB_MARGIN - TB_EDGE - bar_w;
                text_r = MIN(text_r, info->scrollbar_vp.x - TB_BAR_GAP);
            }
            info->vp.x = text_l;
            info->vp.width = text_r - text_l;
            scrollbar = true;
        }
    }
    info->line_count = i;
}

static void calc_first_line(struct view_info *info, int line)
{
    const char *ptr = info->text, *end;
    int i = 0;

    if (line > info->line_count - info->display_lines)
        line = info->line_count - info->display_lines;
    if (line < 0)
        line = 0;

    if (info->line <= line)
    {
        ptr += info->start;
        i = info->line;
    }
    while (*ptr && i < line)
    {
        ptr = get_next_line(ptr, info, &end);
        i++;
    }
    info->start = ptr - info->text;
    info->line = i;
}

static int init_view(struct view_info *info,
                     const char *title, const char *text)
{
    viewport_set_defaults(&info->vp, SCREEN_MAIN);
    info->font = screens[SCREEN_MAIN].getuifont();
    info->pf = font_get(info->font);
    info->title_font = font_get_ui_bold();
    /* padding, title, padding, rule, padding */
    info->title_h = font_get(info->title_font)->height + 3 * TB_TITLE_PAD + 1;
    info->vp.x += TB_MARGIN;
    info->vp.width -= 2 * TB_MARGIN;

    info->title = title;
    info->text = text;
    info->line_count = 0;
    info->line = 0;
    info->start = 0;

    /* no title for small screens. */
    if (!title || (info->vp.height - info->title_h) / info->pf->height < 4)
    {
        info->title = NULL;
        info->title_h = 0;
    }
    info->vp.y += info->title_h;
    info->vp.height -= info->title_h;
    info->display_lines = info->vp.height / info->pf->height;

    calc_line_count(info);
    return 0;
}

static unsigned text_color(const struct ui_theme *theme, enum ui_color_role role,
                           const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static void paint_text(const struct ui_view *node, const struct ui_view_frame *frame)
{
    struct view_info *info = node->data;
    char output[LCD_WIDTH + 1];
    const char *text, *ptr, *end;
    int max_show, line;
    struct screen *display = &screens[SCREEN_MAIN];
    struct ui_theme theme;
    struct ui_album_palette palette;
    ui_theme_snapshot(&theme);
    album_palette_snapshot(&palette);
    unsigned fg = text_color(&theme, UI_COLOR_FOREGROUND, &palette);
    unsigned bg = text_color(&theme, UI_COLOR_BACKGROUND, &palette);

    struct viewport all;

    struct viewport *previous = *display->current_viewport;
    all = (struct viewport){
        .x = frame->clip.x,
        .y = frame->clip.y,
        .width = frame->clip.width,
        .height = frame->clip.height,
        .flags = VP_DEFAULT_FLAGS,
        .drawmode = DRMODE_SOLID,
        .buffer = &lcd_framebuffer_default,
        .font = info->title_font,
    };
    all.fg_pattern = fg;
    all.bg_pattern = bg;
    display->set_viewport(&all);
    display->clear_viewport();

    if (info->title)
    {
        int w, h, x;
        int rule_y = info->title_h - TB_TITLE_PAD - 1;

        all.font = info->title_font;
        display->getstringsize(info->title, &w, &h);
        x = MAX((frame->bounds.width - w) / 2, TB_MARGIN);
        display->putsxy(x - all.x, TB_TITLE_PAD - all.y, info->title);
        display->hline(TB_MARGIN - all.x, frame->bounds.width - 1 - TB_MARGIN - all.x,
                       rule_y - all.y);
    }

    info->vp.fg_pattern = fg;
    info->vp.bg_pattern = bg;
    info->scrollbar_vp.fg_pattern = fg;
    info->scrollbar_vp.bg_pattern = bg;

    max_show = MIN(info->line_count - info->line, info->display_lines);
    text = info->text + info->start;

    display->set_viewport(&info->vp);
    for (line = 0; line < max_show; line++)
    {
        int len;
        ptr = get_next_line(text, info, &end);
        len = MIN(end - text, (int)sizeof(output) - 1);
        while(len > 0 && isspace(text[len-1]))
            len--;
        memcpy(output, text, len);
        output[len] = 0;
        display->puts(0, line, output);
        text = ptr;
    }
    if (info->line_count > info->display_lines)
    {
        display->set_viewport(&info->scrollbar_vp);
        gui_scrollbar_draw(display, (info->scrollbar_vp.width? 0: 1), 0,
                info->scrollbar_vp.width - 1, info->scrollbar_vp.height,
                info->line_count, info->line, info->line + max_show,
                VERTICAL);
    }

    display->set_viewport(previous);
}

static void draw_text(struct view_info *info)
{
    album_palette_refresh();
    ui_page_invalidate(&info->page, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT});
    ui_nav_draw(&info->nav);
}

static void scroll_up(struct view_info *info, int n)
{
    if (info->line <= 0)
        return;

    calc_first_line(info, info->line-n);
    draw_text(info);
    yield();
}

static void scroll_down(struct view_info *info, int n)
{
    if (info->line + info->display_lines >= info->line_count)
        return;

    calc_first_line(info, info->line+n);
    draw_text(info);
    yield();
}

static void scroll_to_top(struct view_info *info)
{
    if (info->line <= 0)
        return;

    calc_first_line(info, 0);
    draw_text(info);
}

static void scroll_to_bottom(struct view_info *info)
{
    if (info->line + info->display_lines >= info->line_count)
        return;

    calc_first_line(info, info->line_count - info->display_lines);
    draw_text(info);
}

static void text_view_close(void *data)
{
    struct view_info *info = data;
    ui_nav_pop(&info->nav);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    int handle = info->handle;
    core_put_data_pinned(info);
    core_free(handle);
}

int view_text(const char *title, const char *text)
{
    static const struct ui_view_ops ops = {.paint = paint_text};
    int handle = core_alloc_free_space(sizeof(struct view_info));
    if (handle <= 0)
        return 0;

    struct view_info *info = core_get_data_pinned(handle);
    *info = (struct view_info){.handle = handle};
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    init_view(info, title, text);
    ui_view_init(&info->view, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT}, &ops, info);
    ui_page_init(&info->page, &info->view);
    ui_nav_push(&info->nav, &info->page, UI_PAGE);
    draw_text(info);
    if (global_settings.talk_menu)
    {
        if (title && P2ID((unsigned char *)title) >= 0)
            talk_id(P2ID((unsigned char *)title), false);
        else if (title)
            talk_text(title, false);
        talk_text(text, true);
    }

    while (true)
    {
        int action = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        switch (action)
        {
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            scroll_up(info, 1);
            break;
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            scroll_down(info, 1);
            break;
        case ACTION_LISTTREE_PGUP:
            scroll_up(info, info->display_lines);
            break;
        case ACTION_LISTTREE_PGDOWN:
            scroll_down(info, info->display_lines);
            break;
        case ACTION_STD_QUICKSCREEN:
            scroll_to_top(info);
            break;
        case ACTION_STD_CONTEXT:
            scroll_to_bottom(info);
            break;
        case ACTION_REDRAW:
            draw_text(info);
            break;
        case ACTION_STD_MENU:
        case ACTION_STD_OK:
        case ACTION_STD_CANCEL:
            text_view_close(info);
            return action == ACTION_STD_MENU;
        case SYS_USB_CONNECTED:
            default_event_handler_ex(action, text_view_close, info);
            return 1;
        default:
            default_event_handler(action);
            break;
        }
    }
}
