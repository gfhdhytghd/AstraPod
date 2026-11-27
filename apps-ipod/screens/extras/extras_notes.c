/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Notes, as stock draws it: a list of the .txt files in /Notes and
 * a built-in "Instructions" note, sorted together by name, and each opening
 * as a full-width page of text titled with its name. A page holds 4 KB, as
 * stock's does; a longer file opens in the document reader instead.
 *
 * Parts: the list of notes, the page, then the screen's loop.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "font.h"
#include "core_alloc.h"
#include "lang.h"
#include "lcd.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/text_wrap.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include "widgets/list.h"
#include "viewers/text_viewer/text_viewer.h"
#include "root_menu.h"
#include "extras.h"

#define NOTES_DIR    "/Notes"
#define NOTES_ROWS   256
#define NOTES_NAMES  8192
#define NOTES_TEXT   4096            /* stock shows a note's first 4 KB      */
#define NOTES_INSTRUCTIONS (-1)     /* the row of the built-in note         */

/* Page layout, the top of a line's box from the top of the screen. */
#define NP_NOTE_X      5
#define NP_NOTE_W      (LCD_WIDTH - 2 * NP_NOTE_X)
#define NP_NOTE_TOP    25
#define NP_LAST_TOP    221           /* a scrolled-out note's last line      */
#define NP_HOWTO_X     24
#define NP_HOWTO_W     (LCD_WIDTH - 2 * NP_HOWTO_X)
#define NP_HOWTO_TOP   57
#define NP_HOWTO_BODY  82            /* the first paragraph under the heading */
#define NP_HOWTO_GAP   16            /* between its two paragraphs           */
#define NP_HOWTO_RGB   0x474747      /* their colour                         */

struct notes {
    int count;
    short name[NOTES_ROWS];          /* into names, or NOTES_INSTRUCTIONS    */
    char names[NOTES_NAMES];
    char text[NOTES_TEXT + 1];
};

static const char *notes_name(const struct notes *n, int row)
{
    return n->name[row] == NOTES_INSTRUCTIONS
           ? (const char *)str(LANG_EXTRAS_NOTES_INSTRUCTIONS)
           : n->names + n->name[row];
}

static const struct notes *notes_sorting;

static int notes_cmp(const void *a, const void *b)
{
    return strcasecmp(notes_name(notes_sorting, (const short *)a
                                 - notes_sorting->name),
                      notes_name(notes_sorting, (const short *)b
                                 - notes_sorting->name));
}

static bool notes_is_txt(const char *name)
{
    size_t len = strlen(name);
    return name[0] != '.' && len > 4 && !strcasecmp(name + len - 4, ".txt");
}

static void notes_scan(struct notes *n)
{
    DIR *dir = opendir(NOTES_DIR);
    int used = 0;

    n->count = 0;
    n->name[n->count++] = NOTES_INSTRUCTIONS;
    while (dir && n->count < NOTES_ROWS)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        int len = strlen((const char *)entry->d_name) + 1;
        if ((dir_get_info(dir, entry).attribute & ATTR_DIRECTORY)
            || !notes_is_txt((const char *)entry->d_name)
            || used + len > NOTES_NAMES)
            continue;
        memcpy(n->names + used, entry->d_name, len);
        n->name[n->count++] = used;
        used += len;
    }
    if (dir)
        closedir(dir);
    notes_sorting = n;
    qsort(n->name, n->count, sizeof(n->name[0]), notes_cmp);
}

static const char *notes_get_name(int row, void *data, char *buf, size_t size)
{
    (void)buf;
    (void)size;
    return notes_name(data, row);
}

/* A page: the built-in note's heading and two paragraphs, or a note's text. */
struct notes_page {
    const char *heading;
    const char *body[2];
    int x, width, top;
    int font, heading_font;
    int scroll, scroll_max;
};

/* Draws the page into the current viewport, or with `d` NULL only measures
 * it; returns the top of the last line of text. */
static int np_lay_out(struct screen *d, struct notes_page *p)
{
    int pitch = font_get(p->font)->height;
    int y = p->top - p->scroll, last = y;

    if (p->heading)
    {
        if (d)
        {
            struct viewport *vp = *d->current_viewport;
            vp->font = p->heading_font;
            d->putsxy(p->x, y, (const unsigned char *)p->heading);
            vp->font = p->font;
            vp->fg_pattern = LCD_RGBPACK(NP_HOWTO_RGB >> 16,
                                         (NP_HOWTO_RGB >> 8) & 255,
                                         NP_HOWTO_RGB & 255);
        }
        y += NP_HOWTO_BODY - NP_HOWTO_TOP;
    }
    for (int i = 0; i < 2 && p->body[i]; i++)
    {
        int lines = d ? text_wrap_draw(d, p->body[i], p->x, y, p->width, 'l')
                      : text_wrap_count(p->body[i], p->width, p->font);
        last = y + (lines - 1) * pitch;
        y += lines * pitch + NP_HOWTO_GAP;
    }
    return last;
}

/* Text blends its colour at 8 bits only when the colour is registered, as
 * the theme's are; stock's grey is not among them. */
static void np_glyph_palette(bool grey)
{
    struct ui_theme theme;
    uint32_t rgb[UI_COLOR_COUNT + 1];

    ui_theme_snapshot(&theme);
    memcpy(rgb, theme.color, sizeof(theme.color));
    rgb[UI_COLOR_COUNT] = NP_HOWTO_RGB;
    lcd_set_glyph_palette(rgb, UI_COLOR_COUNT + grey);
}

static void np_paint(const struct ui_view *view,
                     const struct ui_view_frame *frame)
{
    struct notes_page *p = view->data;
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp = {
        .x = frame->bounds.x, .y = frame->bounds.y,
        .width = frame->bounds.width, .height = frame->bounds.height,
        .flags = VP_DEFAULT_FLAGS, .font = p->font,
        .drawmode = DRMODE_FG, .buffer = &lcd_framebuffer_default,
        .fg_pattern = LCD_BLACK, .bg_pattern = LCD_WHITE,
    };
    struct viewport *last = d->set_viewport(&vp);

    d->clear_viewport();
    if (p->heading)
        np_glyph_palette(true);
    np_lay_out(d, p);
    if (p->heading)
        np_glyph_palette(false);
    d->set_viewport(last);
}

static const struct ui_view_ops np_ops = { .paint = np_paint };

/* Shows the page under `title` until Menu; GO_TO_ROOT after USB, else 0. */
static int np_show(const char *title, struct notes_page *p)
{
    struct ui_rect bounds;
    struct viewport content;
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int ret = 0, pitch;

    p->font = ui_theme_current_font_load(UI_FONT_BODY, 128);
    if (p->font < 0)
        p->font = screens[SCREEN_MAIN].getuifont();
    p->heading_font = p->heading ? ui_theme_current_font_load(UI_FONT_TITLE, 48)
                                 : -1;
    if (p->heading && p->heading_font < 0)
        p->heading_font = p->font;
    pitch = font_get(p->font)->height;

    ui_scene_open_page(&scene, "notes-page-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    bounds = (struct ui_rect){ content.x, content.y, content.width,
                               content.height };
    p->top -= content.y;
    p->scroll = 0;
    p->scroll_max = MAX(np_lay_out(NULL, p) - (NP_LAST_TOP - content.y), 0);
    ui_view_init(&view, bounds, &np_ops, p);
    ui_page_init(&page, &view);

    for (bool dirty = true; ; )
    {
        if (dirty)
        {
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
            dirty = false;
        }
        int action = get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        int to = p->scroll;
        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            to = MIN(p->scroll + pitch, p->scroll_max);
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            to = MAX(p->scroll - pitch, 0);
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
        dirty = to != p->scroll;
        p->scroll = to;
    }

    ui_scene_close_modal(&scene, ret == 0, "notes-page-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (p->heading_font >= 0 && p->heading_font != p->font)
        font_unload(p->heading_font);
    if (p->font != screens[SCREEN_MAIN].getuifont())
        font_unload(p->font);
    return ret;
}

static int notes_open(struct notes *n, int row)
{
    struct notes_page p = {0};
    char path[MAX_PATH];
    const char *name = notes_name(n, row);

    if (n->name[row] == NOTES_INSTRUCTIONS)
    {
        p = (struct notes_page){
            .heading = str(LANG_EXTRAS_NOTES_HOWTO),
            .body = { str(LANG_EXTRAS_NOTES_HOWTO_TEXT),
                      str(LANG_EXTRAS_NOTES_HOWTO_MORE) },
            .x = NP_HOWTO_X, .width = NP_HOWTO_W, .top = NP_HOWTO_TOP,
        };
        return np_show(name, &p);
    }

    snprintf(path, sizeof(path), NOTES_DIR "/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (ffilesize(fd) > NOTES_TEXT)
    {
        close(fd);
        int ret = text_viewer(path);
        return ret == GO_TO_PREVIOUS ? 0 : ret;
    }
    int len = read(fd, n->text, NOTES_TEXT);
    close(fd);
    n->text[MAX(len, 0)] = '\0';
    p = (struct notes_page){
        .body = { n->text }, .x = NP_NOTE_X, .width = NP_NOTE_W,
        .top = NP_NOTE_TOP,
    };
    return np_show(name, &p);
}

int extras_notes(void)
{
    int handle = core_alloc_free_space(sizeof(struct notes));
    struct simplelist_info info;
    struct ui_scene scene = {0};
    int ret = 0, selection = 0;

    if (handle <= 0)
        return 0;
    struct notes *n = core_get_data_pinned(handle);
    notes_scan(n);
    /* Not the main menu's: its rows each carry a chevron, and stock's notes
     * do not. */
    push_current_activity(ACTIVITY_TEXTVIEWER);

    ui_scene_open_page(&scene, "notes-list-push");
    while (ret == 0)
    {
        simplelist_info_init(&info, str(LANG_EXTRAS_NOTES), n->count, n);
        info.get_name = notes_get_name;
        info.selection = selection;
        /* MENU, which the list reports as leaving for the root menu, goes
         * back one screen in stock, to Extras. */
        if (simplelist_show_list(&info) || info.selection < 0)
            break;
        else
        {
            selection = info.selection;
            ret = notes_open(n, selection);
        }
    }

    ui_scene_close_modal(&scene, ret == 0, "notes-list-pop");
    pop_current_activity();
    core_put_data_pinned(n);
    core_free(handle);
    return ret;
}
