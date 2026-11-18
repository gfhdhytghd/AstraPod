/***************************************************************************
 * Viewport manager. GPL version 2+.
 * Based on Rockbox viewport.c by Jonathan Gordon (2008).
 *
 * The stack holds the native chrome visibility of each foreground screen.
 * Screen geometry comes from the active native theme.
 ****************************************************************************/

#include "config.h"
#include "font.h"
#include "lcd.h"
#include "kernel.h"
#include "panic.h"
#include "system.h"
#include "system/appevents.h"
#include "speech/language.h"
#include "ui/frame.h"
#include "ui/statusbar.h"
#include "viewport.h"

#define VPSTACK_DEPTH 16

static uint16_t chrome_visibility[NB_SCREENS];
static unsigned char viewport_depth[NB_SCREENS];

bool viewportmanager_theme_enabled(enum screen_type screen)
{
    return (chrome_visibility[screen] >> viewport_depth[screen]) & 1u;
}

static void viewport_changed(enum screen_type screen, bool previous, bool redraw)
{
    bool visible = viewportmanager_theme_enabled(screen);
    sb_clear_transient_title(screen);
    if (!visible && previous)
    {
        screens[screen].scroll_stop();
        ui_take_dirty(screen);
    }
    if (visible && (!previous || redraw))
        sb_invalidate();
    send_event(GUI_EVENT_THEME_CHANGED, NULL);
}

void viewportmanager_theme_enable(enum screen_type screen, bool enable,
                                 struct viewport *viewport)
{
    bool previous = viewportmanager_theme_enabled(screen);
    if (viewport_depth[screen] == VPSTACK_DEPTH - 1)
        panicf("Stack overflow... viewportmanager");

    unsigned shift = ++viewport_depth[screen];
    uint16_t bit = 1u << shift;
    chrome_visibility[screen] &= ~bit;
    if (enable)
        chrome_visibility[screen] |= bit;
    viewport_changed(screen, previous, false);
    if (viewport)
        viewport_set_defaults(viewport, screen);
}

void viewportmanager_theme_undo(enum screen_type screen, bool force_redraw)
{
    if (!viewport_depth[screen])
        panicf("Stack underflow... viewportmanager");
    bool previous = viewportmanager_theme_enabled(screen);
    --viewport_depth[screen];
    viewport_changed(screen, previous, force_redraw);
}

static void viewportmanager_redraw(unsigned short id, void *data)
{
    (void)id;
    FOR_NB_SCREENS(i)
        sb_update(i, data != NULL);
}

static void viewportmanager_update(unsigned short id, void *data)
{
    viewportmanager_redraw(id, data);
    ui_flush_dirty();
    sb_motion_presented();
}

void viewportmanager_init(void)
{
    FOR_NB_SCREENS(i)
        chrome_visibility[i] = 1;
    add_event(GUI_EVENT_ACTIONREDRAW, viewportmanager_redraw);
    add_event(GUI_EVENT_ACTIONUPDATE, viewportmanager_update);
}

void viewportmanager_theme_changed(int which)
{
    (void)which;
    sb_invalidate();
    send_event(GUI_EVENT_THEME_CHANGED, NULL);
}

int viewport_get_nb_lines(const struct viewport *vp)
{
    return vp->height / font_get(vp->font)->height;
}

static void viewport_set_area(struct viewport *vp, enum screen_type screen, bool chrome)
{
    screens[screen].init_viewport(vp);
    vp->x = 0;
    vp->y = 0;
    vp->width = screens[screen].lcdwidth;
    vp->height = screens[screen].lcdheight;
    vp->flags &= ~VP_FLAG_ALIGNMENT_MASK;
    if (lang_is_rtl())
        vp->flags |= VP_FLAG_ALIGN_RIGHT;
    vp->font = screens[screen].getuifont();
    vp->drawmode = DRMODE_SOLID;
    vp->fg_pattern = LCD_WHITE;
    vp->bg_pattern = LCD_BLACK;
    sb_apply_viewport(vp, screen, chrome);
}

void viewport_set_fullscreen(struct viewport *vp, enum screen_type screen)
{
    viewport_set_area(vp, screen, false);
}

void viewport_set_defaults(struct viewport *vp, enum screen_type screen)
{
    vp->buffer = NULL;
    vp->flags = VP_DEFAULT_FLAGS;
    viewport_set_area(vp, screen, viewportmanager_theme_enabled(screen));
}

void viewport_set_buffer(struct viewport *vp, struct frame_buffer_t *buffer,
                         enum screen_type screen)
{
    if (!vp)
        vp = *screens[screen].current_viewport;
    vp->buffer = buffer && buffer->elems == 0 ? NULL : buffer;
    screens[screen].init_viewport(vp);
}
