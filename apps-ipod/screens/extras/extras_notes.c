/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Notes, as stock 2.0.5 draws it: a list of text notes (including
 * .txt and extensionless files), subdirectories, and the built-in
 * "Instructions" note, sorted by title (honouring <TITLE> tags). Short notes
 * (up to 4 KB) render stock HTML-lite markup (<BR>, <P>, <TITLE>,
 * <A HREF="...">) with selectable blue underlined links; longer files open in
 * the document reader.
 *
 * Parts: scanning and <TITLE> extraction, HTML-lite parsing, the page
 * renderer and link navigation, then the folder list loop.
 ****************************************************************************/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
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
#include "settings/settings.h"
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
#include "extras_bar.h"

#define NOTES_DIR          "/Notes"
#define NOTES_ROWS         256
#define NOTES_NAMES        12288
#define NOTES_TEXT         4096      /* stock shows a note's first 4 KB      */
#define NOTES_INSTRUCTIONS (-1)      /* the row of the built-in note         */
#define NOTES_MAX_LINKS    32
#define NOTES_MAX_LINES    256
#define NOTES_MAX_DEPTH    4
#define NOTES_LINK_DEPTH   8

/* Page layout, the top of a line's box from the top of the screen. */
#define NP_NOTE_X          5
#define NP_NOTE_W          (LCD_WIDTH - 2 * NP_NOTE_X - 4)
#define NP_NOTE_TOP        25
#define NP_LAST_TOP        221       /* a scrolled-out note's last line      */
#define NP_HOWTO_X         24
#define NP_HOWTO_W         (LCD_WIDTH - 2 * NP_HOWTO_X)
#define NP_HOWTO_TOP       57
#define NP_HOWTO_BODY      82        /* the first paragraph under the heading */
#define NP_HOWTO_GAP       16        /* between its two paragraphs           */
#define NP_HOWTO_RGB       0x474747
#define NP_LINK_RGB        LCD_RGBPACK(25, 76, 224)

struct note_entry {
    short disp;                      /* into names[], or NOTES_INSTRUCTIONS  */
    short file;                      /* filename in names[], or -1           */
    bool is_dir;
};

struct note_link {
    uint16_t start;                  /* byte offset in rendered[]            */
    uint16_t len;
    char href[128];
};

struct note_line {
    uint16_t start;
    uint16_t len;
};

struct notes {
    int count;
    struct note_entry row[NOTES_ROWS];
    char names[NOTES_NAMES];
    char raw[NOTES_TEXT + 1];
    char rendered[NOTES_TEXT + 1];
    char page_title[128];
    int link_count;
    struct note_link links[NOTES_MAX_LINKS];
    int line_count;
    struct note_line lines[NOTES_MAX_LINES];
};

static const char *notes_disp(const struct notes *n, const struct note_entry *e)
{
    return e->disp == NOTES_INSTRUCTIONS
           ? (const char *)str(LANG_EXTRAS_NOTES_INSTRUCTIONS)
           : n->names + e->disp;
}

static const struct notes *notes_sorting;

static int notes_cmp(const void *a, const void *b)
{
    return strcasecmp(notes_disp(notes_sorting, a),
                      notes_disp(notes_sorting, b));
}

/* Accept .txt, .linx, and extensionless files; skip hidden and known
 * non-text media/binary files. */
static bool notes_is_text_file(const char *name)
{
    if (name[0] == '.' || !name[0])
        return false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return true;
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".linx") ||
        !strcasecmp(dot, ".text") || !strcasecmp(dot, ".md"))
        return true;
    return false;
}

/* Peek at the beginning of a note file for <TITLE>...</TITLE>. */
static bool notes_read_title(const char *path, char *out, size_t out_sz)
{
    char head[257];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t len = read(fd, head, sizeof(head) - 1);
    close(fd);
    if (len <= 0)
        return false;
    head[len] = '\0';

    const char *p = head;
    while (*p)
    {
        if (*p == '<' && !strncasecmp(p, "<title>", 7))
        {
            p += 7;
            const char *end = p;
            while (*end && strncasecmp(end, "</title>", 8) != 0)
                end++;
            if (!*end)
                return false;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                p++;
            while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
                               end[-1] == '\r' || end[-1] == '\n'))
                end--;
            if (end <= p)
                return false;
            size_t n = MIN((size_t)(end - p), out_sz - 1);
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

static void notes_scan_dir(struct notes *n, const char *dir_path,
                           bool include_instructions)
{
    DIR *dir = opendir(dir_path);
    int used = 0;

    n->count = 0;
    if (include_instructions)
        n->row[n->count++] = (struct note_entry){
            .disp = NOTES_INSTRUCTIONS, .file = -1, .is_dir = false
        };
    while (dir && n->count < NOTES_ROWS)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        const char *ename = (const char *)entry->d_name;
        if (ename[0] == '.')
            continue;
        bool is_dir = (dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) != 0;
        if (!is_dir && !notes_is_text_file(ename))
            continue;

        int flen = (int)strlen(ename) + 1;
        char title[96];
        bool has_title = false;
        if (!is_dir)
        {
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", dir_path, ename);
            has_title = notes_read_title(full, title, sizeof(title));
        }
        int dlen = has_title ? (int)strlen(title) + 1 : 0;
        if (used + flen + dlen > NOTES_NAMES)
            continue;

        short foff = (short)used;
        memcpy(n->names + used, ename, flen);
        used += flen;
        short doff = foff;
        if (has_title)
        {
            doff = (short)used;
            memcpy(n->names + used, title, dlen);
            used += dlen;
        }
        n->row[n->count++] = (struct note_entry){
            .disp = doff, .file = foff, .is_dir = is_dir
        };
    }
    if (dir)
        closedir(dir);
    notes_sorting = n;
    qsort(n->row, n->count, sizeof(n->row[0]), notes_cmp);
}

static const char *notes_get_name(int row, void *data, char *buf, size_t size)
{
    (void)buf;
    (void)size;
    struct notes *n = data;
    return notes_disp(n, &n->row[row]);
}

static enum list_row_kind notes_get_kind(int row, void *data)
{
    struct notes *n = data;
    return n->row[row].is_dir ? LIST_ROW_MENU : LIST_ROW_OPTION;
}

/* ---- HTML-lite parser for stock iPod Notes markup ---- */

static bool tag_matches(const char *p, const char *name)
{
    size_t len = strlen(name);
    if (strncasecmp(p, name, len) != 0)
        return false;
    char c = p[len];
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' ||
           c == '\n' || c == '\0';
}

static bool extract_href(const char *tag_start, const char *tag_end,
                         char *out, size_t out_sz)
{
    const char *p = tag_start;
    while (p < tag_end)
    {
        if (!strncasecmp(p, "href", 4) &&
            (p == tag_start || isspace((unsigned char)p[-1])))
        {
            p += 4;
            while (p < tag_end && isspace((unsigned char)*p))
                p++;
            if (p >= tag_end || *p != '=')
                continue;
            p++;
            while (p < tag_end && isspace((unsigned char)*p))
                p++;
            if (p >= tag_end)
                return false;
            char quote = (*p == '"' || *p == '\'') ? *p++ : '\0';
            const char *val = p;
            while (p < tag_end &&
                   (quote ? *p != quote
                          : !isspace((unsigned char)*p) && *p != '>'))
                p++;
            size_t len = MIN((size_t)(p - val), out_sz - 1);
            memcpy(out, val, len);
            out[len] = '\0';
            return len > 0;
        }
        p++;
    }
    return false;
}

static void notes_parse_markup(struct notes *n, const char *fallback_title)
{
    const char *r = n->raw;
    char *w = n->rendered;
    char *wend = n->rendered + NOTES_TEXT;
    int active_link = -1;

    strlcpy(n->page_title, fallback_title, sizeof(n->page_title));
    n->link_count = 0;

    while (*r && w < wend)
    {
        if (*r == '\r')
        {
            r++;
            if (*r == '\n')
                r++;
            *w++ = '\n';
            continue;
        }
        if (*r == '<')
        {
            const char *end = strchr(r + 1, '>');
            if (end)
            {
                const char *tag = r + 1;
                while (tag < end && isspace((unsigned char)*tag))
                    tag++;
                if (tag_matches(tag, "br"))
                {
                    *w++ = '\n';
                    r = end + 1;
                    /* Absorb a single newline right after <BR>. */
                    if (*r == '\r')
                        r++;
                    if (*r == '\n')
                        r++;
                    continue;
                }
                if (tag_matches(tag, "p"))
                {
                    if (w > n->rendered && w[-1] != '\n')
                        *w++ = '\n';
                    if (w > n->rendered && (w - n->rendered < 2 || w[-2] != '\n')
                        && w < wend)
                        *w++ = '\n';
                    r = end + 1;
                    continue;
                }
                if (tag_matches(tag, "/p"))
                {
                    if (w > n->rendered && w[-1] != '\n')
                        *w++ = '\n';
                    r = end + 1;
                    if (*r == '\r')
                        r++;
                    if (*r == '\n')
                        r++;
                    continue;
                }
                if (tag_matches(tag, "title"))
                {
                    const char *tstart = end + 1;
                    const char *tend = tstart;
                    while (*tend && strncasecmp(tend, "</title>", 8) != 0)
                        tend++;
                    if (*tend)
                    {
                        const char *s = tstart, *e = tend;
                        while (s < e && isspace((unsigned char)*s))
                            s++;
                        while (e > s && isspace((unsigned char)e[-1]))
                            e--;
                        if (e > s)
                        {
                            size_t len = MIN((size_t)(e - s),
                                             sizeof(n->page_title) - 1);
                            memcpy(n->page_title, s, len);
                            n->page_title[len] = '\0';
                        }
                        r = tend + 8;
                        if (*r == '\r')
                            r++;
                        if (*r == '\n')
                            r++;
                        continue;
                    }
                }
                if (tag_matches(tag, "a") && n->link_count < NOTES_MAX_LINKS)
                {
                    struct note_link *lk = &n->links[n->link_count];
                    if (extract_href(tag + 1, end, lk->href, sizeof(lk->href)))
                    {
                        lk->start = (uint16_t)(w - n->rendered);
                        lk->len = 0;
                        active_link = n->link_count++;
                    }
                    r = end + 1;
                    continue;
                }
                if (tag_matches(tag, "/a"))
                {
                    if (active_link >= 0)
                    {
                        struct note_link *lk = &n->links[active_link];
                        lk->len = (uint16_t)((w - n->rendered) - lk->start);
                        if (lk->len == 0)
                            n->link_count--;
                        active_link = -1;
                    }
                    r = end + 1;
                    continue;
                }
                /* Strip any other HTML tag. */
                r = end + 1;
                continue;
            }
        }
        if (*r == '&')
        {
            if (!strncasecmp(r, "&amp;", 5))       { *w++ = '&';  r += 5; continue; }
            if (!strncasecmp(r, "&lt;", 4))        { *w++ = '<';  r += 4; continue; }
            if (!strncasecmp(r, "&gt;", 4))        { *w++ = '>';  r += 4; continue; }
            if (!strncasecmp(r, "&quot;", 6))      { *w++ = '"';  r += 6; continue; }
            if (!strncasecmp(r, "&apos;", 6))      { *w++ = '\''; r += 6; continue; }
            if (!strncasecmp(r, "&nbsp;", 6))      { *w++ = ' ';  r += 6; continue; }
        }
        *w++ = *r++;
    }
    if (active_link >= 0)
    {
        struct note_link *lk = &n->links[active_link];
        lk->len = (uint16_t)((w - n->rendered) - lk->start);
        if (lk->len == 0)
            n->link_count--;
    }
    *w = '\0';
}

/* ---- the page ---- */

struct notes_page {
    const char *heading;
    const char *body[2];
    struct notes *notes;             /* non-NULL for markup note pages       */
    int x, width, top;
    int font, heading_font;
    int scroll, scroll_max;
    int link_focus;                  /* focused link index, or -1            */
    int chosen_link;                 /* link chosen via SELECT, or -1        */
};

/* Wrap n->rendered into n->lines[] for fast drawing and link hit-testing. */
static void np_wrap_note(struct notes_page *p)
{
    struct notes *n = p->notes;
    struct font *pf = font_get(p->font);
    const char *base = n->rendered;
    const char *at = base;

    n->line_count = 0;
    font_lock(p->font, true);
    while (*at && n->line_count < NOTES_MAX_LINES)
    {
        if (*at == '\n')
        {
            n->lines[n->line_count++] = (struct note_line){
                (uint16_t)(at - base), 0
            };
            at++;
            continue;
        }
        const char *nl = strchr(at, '\n');
        const char *seg_end = nl ? nl : at + strlen(at);
        while (at < seg_end && n->line_count < NOTES_MAX_LINES)
        {
            const char *end;
            const char *next = text_wrap_next(at, p->width, pf, &end);
            if (end > seg_end)
                end = seg_end;
            if (next > seg_end)
                next = seg_end;
            n->lines[n->line_count++] = (struct note_line){
                (uint16_t)(at - base), (uint16_t)(end - at)
            };
            if (next <= at)
                break;
            at = next;
        }
        if (nl)
            at = nl + 1;
    }
    font_lock(p->font, false);
}

static int np_measure_substr(int font, const char *s, int len)
{
    char buf[160];
    int n = MIN(len, (int)sizeof(buf) - 1);
    if (n <= 0)
        return 0;
    memcpy(buf, s, n);
    buf[n] = '\0';
    int w = 0;
    font_getstringsize((const unsigned char *)buf, &w, NULL, font);
    return w;
}

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
        for (int i = 0; i < 2 && p->body[i]; i++)
        {
            int lines = d ? text_wrap_draw(d, p->body[i], p->x, y, p->width, 'l')
                          : text_wrap_count(p->body[i], p->width, p->font);
            last = y + (lines - 1) * pitch;
            y += lines * pitch + NP_HOWTO_GAP;
        }
        return last;
    }

    struct notes *n = p->notes;
    int ascent = font_get(p->font)->ascent;
    for (int i = 0; i < n->line_count; i++)
    {
        int ly = y + i * pitch;
        last = ly;
        if (!d)
            continue;
        struct viewport *vp = *d->current_viewport;
        if (ly + pitch < 0 || ly >= vp->height)
            continue;
        const struct note_line *ln = &n->lines[i];
        char buf[160];
        int len = MIN((int)ln->len, (int)sizeof(buf) - 1);
        memcpy(buf, n->rendered + ln->start, len);
        buf[len] = '\0';
        vp->font = p->font;
        vp->fg_pattern = LCD_BLACK;
        d->putsxy(p->x, ly, (const unsigned char *)buf);

        /* Underline any hyperlink overlapping this wrapped line. */
        int lstart = ln->start;
        int lend = ln->start + len;
        for (int li = 0; li < n->link_count; li++)
        {
            const struct note_link *lk = &n->links[li];
            int ks = lk->start;
            int ke = lk->start + lk->len;
            int os = MAX(lstart, ks);
            int oe = MIN(lend, ke);
            if (os >= oe)
                continue;
            int x0 = p->x + np_measure_substr(p->font, n->rendered + lstart,
                                              os - lstart);
            int x1 = x0 + np_measure_substr(p->font, n->rendered + os, oe - os);
            if (x1 <= x0)
                continue;
            vp->fg_pattern = NP_LINK_RGB;
            int uy = ly + ascent + 1;
            d->hline(x0, x1 - 1, uy);
            if (li == p->link_focus)
                d->hline(x0, x1 - 1, uy + 1);
        }
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
    if (p->scroll_max > 0)
    {
        int track_h = vp.height - 8;
        int total_h = vp.height + p->scroll_max;
        int thumb_h = MAX(20, track_h * vp.height / total_h);
        int thumb_y = 4 + (track_h - thumb_h) * p->scroll / p->scroll_max;
        vp.fg_pattern = LCD_RGBPACK(140, 142, 140);
        d->fillrect(vp.width - 4, thumb_y, 3, thumb_h);
    }
    d->set_viewport(last);
}

static const struct ui_view_ops np_ops = { .paint = np_paint };

/* Ensure link `idx` is scrolled into view on `p`. */
static void np_scroll_to_link(struct notes_page *p, int idx, int view_h)
{
    if (!p->notes || idx < 0 || idx >= p->notes->link_count)
        return;
    int pitch = font_get(p->font)->height;
    int start = p->notes->links[idx].start;
    for (int i = 0; i < p->notes->line_count; i++)
    {
        const struct note_line *ln = &p->notes->lines[i];
        if (start >= ln->start && start <= ln->start + ln->len)
        {
            int ly = p->top + i * pitch;
            if (ly < p->scroll)
                p->scroll = MAX(0, ly - pitch);
            else if (ly + pitch > p->scroll + view_h)
                p->scroll = MIN(p->scroll_max, ly + 2 * pitch - view_h);
            break;
        }
    }
}

/* Shows the page under `title` until Menu or link activation; returns
 * GO_TO_ROOT after USB, else 0 (with p->chosen_link set if a link was picked). */
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
    if (p->notes)
        np_wrap_note(p);

    ui_scene_open_page(&scene, "notes-page-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    bounds = (struct ui_rect){ content.x, content.y, content.width,
                               content.height };
    p->top -= content.y;
    p->scroll = 0;
    p->scroll_max = MAX(np_lay_out(NULL, p) - (NP_LAST_TOP - content.y), 0);
    p->link_focus = (p->notes && p->notes->link_count > 0) ? 0 : -1;
    p->chosen_link = -1;
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
        int action = extras_get_action(TIMEOUT_BLOCK, false);
        int to = p->scroll;
        int focus = p->link_focus;
        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
        {
            if (p->notes && p->notes->link_count > 1 &&
                focus + 1 < p->notes->link_count && p->scroll == p->scroll_max)
            {
                focus++;
                np_scroll_to_link(p, focus, bounds.height);
                to = p->scroll;
            }
            else if (p->scroll < p->scroll_max)
            {
                to = MIN(p->scroll + pitch, p->scroll_max);
            }
            else if (p->notes && focus + 1 < p->notes->link_count)
            {
                focus++;
            }
        }
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
        {
            if (p->notes && p->notes->link_count > 1 &&
                focus > 0 && p->scroll == 0)
            {
                focus--;
                np_scroll_to_link(p, focus, bounds.height);
                to = p->scroll;
            }
            else if (p->scroll > 0)
            {
                to = MAX(p->scroll - pitch, 0);
            }
            else if (p->notes && focus > 0)
            {
                focus--;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            if (p->notes && p->link_focus >= 0 &&
                p->link_focus < p->notes->link_count)
            {
                p->chosen_link = p->link_focus;
                break;
            }
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
        dirty = (to != p->scroll) || (focus != p->link_focus);
        p->scroll = to;
        p->link_focus = focus;
    }

    ui_scene_close_modal(&scene, ret == 0, "notes-page-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (p->heading_font >= 0 && p->heading_font != p->font)
        font_unload(p->heading_font);
    if (p->font != screens[SCREEN_MAIN].getuifont())
        font_unload(p->font);
    return ret;
}

static void resolve_link_path(const char *base_dir, const char *href,
                              char *out, size_t out_sz)
{
    if (href[0] == '/')
    {
        snprintf(out, out_sz, "%s%s", NOTES_DIR, href);
        if (file_exists(out))
            return;
        strlcpy(out, href, out_sz);
        return;
    }
    snprintf(out, out_sz, "%s/%s", base_dir, href);
    if (file_exists(out))
        return;
    snprintf(out, out_sz, "%s/%s", NOTES_DIR, href);
}

static int notes_open_file(struct notes *n, const char *path,
                           const char *fallback_name, int depth)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (ffilesize(fd) > NOTES_TEXT)
    {
        close(fd);
        int ret = text_viewer(path);
        return ret == GO_TO_PREVIOUS ? 0 : ret;
    }
    int len = read(fd, n->raw, NOTES_TEXT);
    close(fd);
    n->raw[MAX(len, 0)] = '\0';

    for (;;)
    {
        notes_parse_markup(n, fallback_name);
        struct notes_page p = {
            .notes = n, .x = NP_NOTE_X, .width = NP_NOTE_W, .top = NP_NOTE_TOP,
        };
        int ret = np_show(n->page_title, &p);
        if (ret != 0 || p.chosen_link < 0 || depth >= NOTES_LINK_DEPTH)
            return ret;

        char href[128];
        strlcpy(href, n->links[p.chosen_link].href, sizeof(href));
        char dir[MAX_PATH], target[MAX_PATH];
        strlcpy(dir, path, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = '\0';
        else
            strlcpy(dir, NOTES_DIR, sizeof(dir));
        resolve_link_path(dir, href, target, sizeof(target));
        const char *leaf = strrchr(target, '/');
        ret = notes_open_file(n, target, leaf ? leaf + 1 : target, depth + 1);
        if (ret != 0)
            return ret;
        /* Reload our own raw text after returning from a linked note. */
        fd = open(path, O_RDONLY);
        if (fd < 0)
            return 0;
        len = read(fd, n->raw, NOTES_TEXT);
        close(fd);
        n->raw[MAX(len, 0)] = '\0';
    }
}

static int notes_browse_dir(struct notes *n, const char *dir_path,
                            const char *title, int depth)
{
    struct gui_synclist list;
    struct ui_scene scene = {0};
    int ret = 0, selection = 0, action;

    notes_scan_dir(n, dir_path, depth == 0);
    if (n->count == 0)
        return 0;

    ui_scene_open_page(&scene, "notes-list-push");
    while (ret == 0)
    {
        sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        gui_synclist_init(&list, notes_get_name, n, false, 1, NULL);
        gui_synclist_set_title(&list, (char *)title, Icon_NOICON);
        gui_synclist_set_row_callbacks(&list, notes_get_kind, NULL, NULL);
        gui_synclist_set_nb_items(&list, n->count);
        gui_synclist_select_item(&list, MIN(selection, n->count - 1));
        gui_synclist_draw_settled(&list);

        bool picked = false;
        for (;;)
        {
            extras_list_do_action(HZ, &list, &action);
            if (action == ACTION_STD_OK)
            {
                selection = gui_synclist_get_sel_pos(&list);
                picked = true;
                break;
            }
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
                break;
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = GO_TO_ROOT;
                break;
            }
        }
        gui_synclist_scroll_stop(&list);
        viewportmanager_theme_undo(SCREEN_MAIN, false);
        if (!picked)
            break;

        const struct note_entry *e = &n->row[selection];
        if (e->disp == NOTES_INSTRUCTIONS)
        {
            struct notes_page p = {
                .heading = str(LANG_EXTRAS_NOTES_HOWTO),
                .body = { str(LANG_EXTRAS_NOTES_HOWTO_TEXT),
                          str(LANG_EXTRAS_NOTES_HOWTO_MORE) },
                .x = NP_HOWTO_X, .width = NP_HOWTO_W, .top = NP_HOWTO_TOP,
            };
            ret = np_show(notes_disp(n, e), &p);
        }
        else if (e->is_dir && depth < NOTES_MAX_DEPTH)
        {
            char sub_path[MAX_PATH], sub_title[96];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path,
                     n->names + e->file);
            strlcpy(sub_title, notes_disp(n, e), sizeof(sub_title));
            ret = notes_browse_dir(n, sub_path, sub_title, depth + 1);
            notes_scan_dir(n, dir_path, depth == 0);
        }
        else if (!e->is_dir)
        {
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", dir_path,
                     n->names + e->file);
            ret = notes_open_file(n, full, notes_disp(n, e), 0);
        }
    }

    ui_scene_close_modal(&scene, ret == 0, "notes-list-pop");
    return ret;
}

int extras_notes(void)
{
    int handle = core_alloc_free_space(sizeof(struct notes));
    int ret;

    if (handle <= 0)
        return 0;
    struct notes *n = core_get_data_pinned(handle);
    push_current_activity(ACTIVITY_TEXTVIEWER);
    ret = notes_browse_dir(n, NOTES_DIR, str(LANG_EXTRAS_NOTES), 0);
    pop_current_activity();
    core_put_data_pinned(n);
    core_free(handle);
    return ret;
}
