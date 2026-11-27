/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Contacts, as stock draws it: the vCards in /Contacts, one file
 * each or many to a file, listed by first or last name as Settings > Sort By
 * says, each opening a page of its numbers, emails, addresses and note.
 * With none, Select does nothing and the Extras pane reads "No Contacts".
 *
 * A list row is a name over the first number, or the first email. By first
 * name a row reads "First Last", by last name "Last, First"; a card with no
 * name sorts and shows by its formatted name, then its company. The page
 * reads the card again from its file.
 *
 * vCard 2.1, 3.0 and 4.0 read alike: folded lines, quoted-printable values
 * and their soft line breaks, and backslash escapes. Text is UTF-8 unless a
 * CHARSET says ISO-8859-1 or Windows-1252. Photos and properties not shown
 * are skipped.
 *
 * Parts: reading a card, scanning the folder, the list, the page, then the
 * screen's loop.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
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
#include "speech/talk.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "ui/theme_art.h"
#include "widgets/list.h"
#include "root_menu.h"
#include "extras.h"

#define CONTACTS_DIR   "/Contacts"
#define MAX_FILES      256
#define MAX_CONTACTS   1000
#define POOL_BYTES     49152         /* file names and list texts           */
#define LINE_BYTES     1024          /* a property, unfolded; longer is cut */
#define PAGE_ITEMS     32
#define PAGE_BYTES     4096
#define NONE           0xffff        /* no text                             */

/* The list's two-line rows, as stock's */
#define ROW_H          44
#define ROW_TEXT_X     5
#define ROW_TITLE_BASE 18
#define ROW_SUB_BASE   35
#define ROW_CHEVRON_R  16            /* chevron from the right edge         */
#define ROW_CHEVRON_Y  15
#define SUB_RGB        LCD_RGBPACK(107, 109, 107)
#define ROW_RULE_RGB   LCD_RGBPACK(222, 223, 222)

/* The page, from the top of the screen */
#define PG_PHOTO_Y     20            /* the 55-pixel photo, at x 0          */
#define PG_NAME_X      65
#define PG_NAME_BASE   49
#define PG_HEAD_RULE   75
#define PG_HEAD_GAP    22            /* that rule to the first baseline     */
#define PG_GROUP_GAP   21            /* a group's rule to its first baseline */
#define PG_PITCH       15
#define PG_RULE_GAP    14            /* a group's last baseline to the rule */
#define PG_LABEL_R     89            /* labels end here, right-aligned      */
#define PG_VALUE_X     92
#define PG_VALUE_R     305
#define PG_VALUE_RGB   LCD_RGBPACK(74, 77, 74)
#define PG_RULE_RGB    LCD_RGBPACK(214, 211, 214)

/** Reading a card **/

/* A file read a buffer at a time, its offset kept so a card can be found
 * again. */
struct reader {
    int fd;
    int len, pos;
    uint32_t offset;                 /* of buf[pos] in the file             */
    unsigned char buf[512];
};

static int rd_peek(struct reader *r)
{
    if (r->pos >= r->len)
    {
        r->len = read(r->fd, r->buf, sizeof(r->buf));
        r->pos = 0;
        if (r->len <= 0)
        {
            r->len = 0;
            return -1;
        }
    }
    return r->buf[r->pos];
}

static int rd_getc(struct reader *r)
{
    int c = rd_peek(r);
    if (c >= 0)
    {
        r->pos++;
        r->offset++;
    }
    return c;
}

/* Whether `hay`, `n` bytes long, holds `needle`, ignoring ASCII case */
static bool has_word(const char *hay, size_t n, const char *needle)
{
    size_t len = strlen(needle);
    for (size_t i = 0; i + len <= n; i++)
        if (!strncasecmp(hay + i, needle, len))
            return true;
    return false;
}

/* The next property, unfolded into `line` and cut to fit; false at the end
 * of the file. `start` is where it began. A quoted-printable value whose
 * line ends in '=' goes on to the next line. */
static bool read_property(struct reader *r, char *line, uint32_t *start)
{
    size_t n = 0;
    int c;

    *start = r->offset;
    if (rd_peek(r) < 0)
        return false;
    for (;;)
    {
        while ((c = rd_getc(r)) >= 0 && c != '\n')
            if (c != '\r' && n < LINE_BYTES - 1)
                line[n++] = c;
        c = rd_peek(r);
        if (c == ' ' || c == '\t')
        {
            rd_getc(r);
            continue;
        }
        const char *colon = memchr(line, ':', n);
        if (c >= 0 && colon && n && line[n - 1] == '=' &&
            has_word(line, colon - line, "QUOTED-PRINTABLE"))
        {
            n--;
            continue;
        }
        break;
    }
    line[n] = '\0';
    return true;
}

enum {
    T_HOME = 1, T_WORK = 2, T_CELL = 4, T_FAX = 8, T_PAGER = 16,
    T_MAIN = 32, T_QP = 64, T_LATIN1 = 128, T_SKIP = 256,
};

/* Splits `line` in place into its name, without a group, and its value;
 * returns the T_ flags its parameters give. */
static int split_property(char *line, char **name, char **value)
{
    static const struct { const char *word; int flag; } words[] = {
        { "HOME", T_HOME }, { "WORK", T_WORK }, { "CELL", T_CELL },
        { "IPHONE", T_CELL }, { "MOBILE", T_CELL }, { "FAX", T_FAX },
        { "PAGER", T_PAGER }, { "MAIN", T_MAIN },
        { "QUOTED-PRINTABLE", T_QP }, { "ISO-8859-1", T_LATIN1 },
        { "WINDOWS-1252", T_LATIN1 }, { "BASE64", T_SKIP }, { "B", T_SKIP },
    };
    char *colon = strchr(line, ':');
    int flags = 0;

    *value = colon ? colon + 1 : line + strlen(line);
    if (colon)
        *colon = '\0';
    char *dot = strchr(line, '.');
    char *semi = strchr(line, ';');
    *name = dot && (!semi || dot < semi) ? dot + 1 : line;
    if (!semi)
        return 0;
    *semi = '\0';
    /* Each parameter is TYPE=a,b, ENCODING=x, CHARSET=x or a bare word */
    for (char *p = semi + 1; *p; )
    {
        size_t len = strcspn(p, ";,=");
        if (p[len] == '=')
        {
            p += len + 1;
            continue;
        }
        for (unsigned i = 0; i < ARRAYLEN(words); i++)
            if (strlen(words[i].word) == len &&
                !strncasecmp(p, words[i].word, len))
                flags |= words[i].flag;
        p += len + (p[len] != '\0');
    }
    return flags;
}

static int hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Decodes `value` in place: quoted-printable, then Latin-1 to UTF-8 when it
 * fits, then line ends to '\n'. Backslash escapes stay for unescape(). */
static void decode(char *value, int flags)
{
    char *w = value;
    for (char *r = value; *r; )
    {
        if ((flags & T_QP) && r[0] == '=' && hex(r[1]) >= 0 && hex(r[2]) >= 0)
        {
            *w++ = hex(r[1]) * 16 + hex(r[2]);
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
    if (flags & T_LATIN1)
    {
        size_t len = strlen(value), high = 0;
        for (size_t i = 0; i < len; i++)
            high += (unsigned char)value[i] >= 0x80;
        if (len + high < LINE_BYTES)
        {
            value[len + high] = '\0';
            for (size_t i = len; i-- > 0; )
            {
                unsigned char c = value[i];
                if (c >= 0x80)
                {
                    value[i + high] = 0x80 | (c & 0x3f);
                    value[i + --high] = 0xc0 | (c >> 6);
                }
                else
                    value[i + high] = c;
            }
        }
    }
    w = value;
    for (char *r = value; *r; r++)
        if (*r != '\r')
            *w++ = *r;
        else if (r[1] != '\n')
            *w++ = '\n';
    *w = '\0';
}

/* Undoes a text value's backslash escapes in place */
static void unescape(char *s)
{
    char *w = s;
    for (; *s; s++)
    {
        if (*s == '\\' && s[1])
        {
            s++;
            *w++ = *s == 'n' || *s == 'N' ? '\n' : *s;
        }
        else
            *w++ = *s;
    }
    *w = '\0';
}

/* Splits a structured value in place at each unescaped ';' into its first
 * `max` parts, each unescaped; missing parts are "". */
static void split_parts(char *value, char **part, int max)
{
    int n = 0;
    part[n++] = value;
    for (char *s = value; *s; s++)
    {
        if (*s == '\\' && s[1])
            s++;
        else if (*s == ';')
        {
            *s = '\0';
            if (n == max)
                break;
            part[n++] = s + 1;
        }
    }
    for (int i = 0; i < n; i++)
        unescape(part[i]);
    while (n < max)
        part[n++] = "";
}

/* Drops spaces at either end in place */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                   s[len - 1] == '\n'))
        s[--len] = '\0';
    return s;
}

/** Scanning the folder **/

struct contact {
    uint16_t file;
    uint32_t offset;                 /* of its BEGIN:VCARD line             */
    uint16_t first, last, name, sub; /* into the pool, or NONE              */
};

struct contacts {
    int count, files, used;
    bool by_last;
    uint16_t file[MAX_FILES];        /* the names, into the pool            */
    struct contact c[MAX_CONTACTS];
    uint16_t order[MAX_CONTACTS];
    char pool[POOL_BYTES];
    char line[LINE_BYTES];
    char row[160];
};

static uint16_t keep(struct contacts *cs, const char *text)
{
    size_t len = strlen(text) + 1;
    if (!*text || cs->used + len > POOL_BYTES)
        return NONE;
    memcpy(cs->pool + cs->used, text, len);
    cs->used += len;
    return cs->used - len;
}

static const char *text(const struct contacts *cs, uint16_t at)
{
    return at == NONE ? "" : cs->pool + at;
}

static bool is_vcf(const char *name)
{
    size_t len = strlen(name);
    return name[0] != '.' && len > 4 && !strcasecmp(name + len - 4, ".vcf");
}

/* The cards in one file, each kept with its name and first number or
 * email */
static void scan_file(struct contacts *cs, int file)
{
    char path[MAX_PATH];
    struct reader r = { .fd = -1 };
    char first[96], last[96], name[96], org[96], tel[64], email[96];
    bool in_card = false;
    uint32_t start, card = 0;

    snprintf(path, sizeof(path), CONTACTS_DIR "/%s", text(cs, cs->file[file]));
    r.fd = open(path, O_RDONLY);
    if (r.fd < 0)
        return;
    while (cs->count < MAX_CONTACTS && read_property(&r, cs->line, &start))
    {
        char *prop, *value;
        int flags = split_property(cs->line, &prop, &value);
        if (!strcasecmp(prop, "BEGIN") && !strcasecmp(trim(value), "VCARD"))
        {
            in_card = true;
            card = start;
            first[0] = last[0] = name[0] = org[0] = tel[0] = email[0] = '\0';
            continue;
        }
        if (!in_card || (flags & T_SKIP))
            continue;
        decode(value, flags);
        if (!strcasecmp(prop, "END"))
        {
            struct contact *c = &cs->c[cs->count];
            in_card = false;
            *c = (struct contact){ .file = file, .offset = card,
                .first = keep(cs, first), .last = keep(cs, last),
                .name = keep(cs, name[0] ? name : org),
                .sub = keep(cs, tel[0] ? tel : email) };
            cs->order[cs->count] = cs->count;
            cs->count++;
        }
        else if (!strcasecmp(prop, "N"))
        {
            char *part[2];
            split_parts(value, part, 2);
            strlcpy(last, trim(part[0]), sizeof(last));
            strlcpy(first, trim(part[1]), sizeof(first));
        }
        else if (!strcasecmp(prop, "FN") || !strcasecmp(prop, "ORG"))
        {
            char *part[1];
            split_parts(value, part, 1);
            strlcpy(prop[0] == 'F' || prop[0] == 'f' ? name : org,
                    trim(part[0]), sizeof(name));
        }
        else if (!strcasecmp(prop, "TEL") && !tel[0])
        {
            unescape(value);
            strlcpy(tel, trim(value), sizeof(tel));
        }
        else if (!strcasecmp(prop, "EMAIL") && !email[0])
        {
            unescape(value);
            strlcpy(email, trim(value), sizeof(email));
        }
    }
    close(r.fd);
}

static void scan(struct contacts *cs)
{
    DIR *dir = opendir(CONTACTS_DIR);
    cs->count = cs->files = cs->used = 0;
    while (dir && cs->files < MAX_FILES)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        if ((dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) ||
            !is_vcf((const char *)entry->d_name))
            continue;
        uint16_t at = keep(cs, (const char *)entry->d_name);
        if (at != NONE)
            cs->file[cs->files++] = at;
    }
    if (dir)
        closedir(dir);
    for (int f = 0; f < cs->files; f++)
        scan_file(cs, f);
}

bool extras_contacts_any(void)
{
    DIR *dir = opendir(CONTACTS_DIR);
    bool any = false;
    while (dir && !any)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        any = !(dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) &&
              is_vcf((const char *)entry->d_name);
    }
    if (dir)
        closedir(dir);
    return any;
}

/* What a card sorts by, first and then: by last name its last name, else
 * its first; by first name the other way about; with neither, its formatted
 * name or company. */
static void sort_keys(const struct contacts *cs, const struct contact *c,
                      const char **key, const char **then)
{
    const char *first = text(cs, c->first), *last = text(cs, c->last);
    *key = cs->by_last ? (*last ? last : first) : (*first ? first : last);
    *then = cs->by_last ? (*last ? first : "") : (*first ? last : "");
    if (!**key)
        *key = text(cs, c->name);
}

static const struct contacts *sorting;

static int by_name(const void *a, const void *b)
{
    const struct contact *ca = &sorting->c[*(const uint16_t *)a];
    const struct contact *cb = &sorting->c[*(const uint16_t *)b];
    const char *ka, *ta, *kb, *tb;
    sort_keys(sorting, ca, &ka, &ta);
    sort_keys(sorting, cb, &kb, &tb);
    int d = strcasecmp(ka, kb);
    if (!d)
        d = strcasecmp(ta, tb);
    return d ? d : (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/** The list **/

static int sub_font = -1;

/* A row's name: "First Last" or "Last, First", else the card's name */
static const char *row_name(struct contacts *cs, int row, char *buf,
                            size_t size)
{
    const struct contact *c = &cs->c[cs->order[row]];
    const char *first = text(cs, c->first), *last = text(cs, c->last);
    if (*first && *last)
        snprintf(buf, size, cs->by_last ? "%s, %s" : "%s %s",
                 cs->by_last ? last : first, cs->by_last ? first : last);
    else if (*first || *last)
        strlcpy(buf, *first ? first : last, size);
    else if (c->name != NONE)
        strlcpy(buf, text(cs, c->name), size);
    else
        strlcpy(buf, str(LANG_CONTACTS_NO_NAME), size);
    return buf;
}

static const char *get_name(int row, void *data, char *buf, size_t size)
{
    return row_name(data, row, buf, size);
}

static int get_talk(int row, void *data)
{
    struct contacts *cs = data;
    talk_spell(row_name(cs, row, cs->row, sizeof(cs->row)), false);
    return 0;
}

static enum list_row_kind get_kind(int row, void *data)
{
    (void)row;
    (void)data;
    return LIST_ROW_MENU;
}

static void draw_row(struct list_putlineinfo_t *info)
{
    struct contacts *cs = info->list->data;
    struct viewport *vp = info->vp;
    const struct viewport *parent = info->list->parent[SCREEN_MAIN];
    /* The row's own width: the viewport is clipped to what shows of it */
    int width = parent->x + parent->width - (vp->x + info->x);
    unsigned fg = vp->fg_pattern;
    int font = vp->font;
    int x = info->x + ROW_TEXT_X;

    /* An owner-drawn row lays its own bar: stock's 44-row one */
    const uint32_t *bar = ui_art_selection(ROW_H);
    for (int y = 0; info->is_selected && bar && y < ROW_H; y++)
    {
        vp->fg_pattern = LCD_RGBPACK(bar[y] >> 16, (bar[y] >> 8) & 255,
                                     bar[y] & 255);
        lcd_hline(info->x, info->x + width - 1, info->y + y);
    }
    vp->fg_pattern = fg;
    lcd_putsxy(x, info->y + ROW_TITLE_BASE - font_get(font)->ascent,
               (const unsigned char *)info->dsp_text);
    const struct contact *c = &cs->c[cs->order[info->line]];
    if (sub_font >= 0 && c->sub != NONE)
    {
        vp->font = sub_font;
        vp->fg_pattern = info->is_selected ? fg : SUB_RGB;
        lcd_putsxy(x, info->y + ROW_SUB_BASE - font_get(sub_font)->ascent,
                   (const unsigned char *)text(cs, c->sub));
        vp->font = font;
    }
    if (info->is_selected)
        stock_art_draw(STOCK_ART_SYSTEM_SUBMENU,
                       vp->x + info->x + width - ROW_CHEVRON_R,
                       vp->y + info->y + ROW_CHEVRON_Y,
                       (struct ui_rect){ vp->x, vp->y, vp->width,
                                         vp->height });
    else
    {
        vp->fg_pattern = ROW_RULE_RGB;
        lcd_hline(info->x, info->x + width - 1, info->y + ROW_H - 1);
    }
    vp->fg_pattern = fg;
}

/** The page **/

enum { G_PHONE, G_EMAIL, G_ADDRESS, G_NOTE, GROUPS };

struct page_item {
    uint8_t group;
    uint16_t label;                  /* a LANG_CONTACTS_ id                 */
    uint16_t text;                   /* into the page's text                */
};

struct page {
    char name[160];
    int count, used;
    struct page_item item[PAGE_ITEMS];
    char text[PAGE_BYTES];
    int font, label_font, name_font;
    int scroll, scroll_max;
};

static void page_add(struct page *p, int group, int label, const char *value)
{
    size_t len = strlen(value) + 1;
    if (!*value || p->count >= PAGE_ITEMS || p->used + len > PAGE_BYTES)
        return;
    memcpy(p->text + p->used, value, len);
    p->item[p->count++] = (struct page_item){ group, label, p->used };
    p->used += len;
}

static int phone_label(int flags)
{
    return flags & T_CELL ? LANG_CONTACTS_MOBILE :
           flags & T_FAX ? LANG_CONTACTS_FAX :
           flags & T_PAGER ? LANG_CONTACTS_PAGER :
           flags & T_MAIN ? LANG_CONTACTS_MAIN :
           flags & T_HOME ? LANG_CONTACTS_HOME :
           flags & T_WORK ? LANG_CONTACTS_WORK : LANG_CONTACTS_OTHER;
}

/* An address as stock sets it out: the street, then "City, Region Postal"
 * or, with no region, the city and the postal code on lines of their own,
 * then the country */
static void address_text(char **part, char *out, size_t size)
{
    char *pobox = trim(part[0]), *ext = trim(part[1]);
    char *street = trim(part[2]), *city = trim(part[3]);
    char *region = trim(part[4]), *postal = trim(part[5]);
    char *country = trim(part[6]);
    size_t n = 0;
#define LINE(...) do { if (n < size) { \
        n += snprintf(out + n, size - n, n ? "\n" : ""); \
        if (n < size) n += snprintf(out + n, size - n, __VA_ARGS__); } \
    } while (0)
    if (*pobox)
        LINE("%s", pobox);
    if (*ext)
        LINE("%s", ext);
    if (*street)
        LINE("%s", street);
    if (*region)
        LINE("%s%s%s%s%s", city, *city ? ", " : "", region,
             *postal ? " " : "", postal);
    else
    {
        if (*city)
            LINE("%s", city);
        if (*postal)
            LINE("%s", postal);
    }
    if (*country)
        LINE("%s", country);
#undef LINE
}

/* Reads the card `c` again, into `p` */
static void page_read(struct contacts *cs, const struct contact *c,
                      struct page *p)
{
    char path[MAX_PATH], composed[160] = "";
    struct reader r = { .fd = -1 };
    uint32_t start;

    p->count = p->used = 0;
    p->name[0] = '\0';
    snprintf(path, sizeof(path), CONTACTS_DIR "/%s",
             text(cs, cs->file[c->file]));
    r.fd = open(path, O_RDONLY);
    if (r.fd >= 0 && lseek(r.fd, c->offset, SEEK_SET) == (off_t)c->offset)
    {
        r.offset = c->offset;
        while (read_property(&r, cs->line, &start))
        {
            char *prop, *value;
            int flags = split_property(cs->line, &prop, &value);
            if (flags & T_SKIP)
                continue;
            decode(value, flags);
            if (!strcasecmp(prop, "END"))
                break;
            if (!strcasecmp(prop, "FN"))
            {
                unescape(value);
                strlcpy(p->name, trim(value), sizeof(p->name));
            }
            else if (!strcasecmp(prop, "N"))
            {
                char *part[2];
                split_parts(value, part, 2);
                char *last = trim(part[0]), *first = trim(part[1]);
                snprintf(composed, sizeof(composed), "%s%s%s", first,
                         *first && *last ? " " : "", last);
            }
            else if (!strcasecmp(prop, "TEL"))
            {
                unescape(value);
                page_add(p, G_PHONE, phone_label(flags), trim(value));
            }
            else if (!strcasecmp(prop, "EMAIL"))
            {
                /* Stock calls an email of no kind a work one */
                unescape(value);
                page_add(p, G_EMAIL, flags & T_HOME ? LANG_CONTACTS_HOME :
                         LANG_CONTACTS_WORK, trim(value));
            }
            else if (!strcasecmp(prop, "ADR"))
            {
                char *part[7], out[320];
                split_parts(value, part, 7);
                address_text(part, out, sizeof(out));
                page_add(p, G_ADDRESS, flags & T_HOME ? LANG_CONTACTS_HOME :
                         flags & T_WORK ? LANG_CONTACTS_WORK :
                         LANG_CONTACTS_OTHER, out);
            }
            else if (!strcasecmp(prop, "NOTE"))
            {
                unescape(value);
                page_add(p, G_NOTE, LANG_CONTACTS_NOTES, trim(value));
            }
        }
    }
    if (r.fd >= 0)
        close(r.fd);
    if (!p->name[0])
        strlcpy(p->name, composed[0] ? composed : text(cs, c->name),
                sizeof(p->name));
    if (!p->name[0])
        strlcpy(p->name, str(LANG_CONTACTS_NO_NAME), sizeof(p->name));
}

/* Draws the page's items below the header's rule at `top` into the current
 * viewport, a rule between groups, or with `d` NULL only measures them;
 * returns the bottom of the last. */
static int page_lay_out(struct screen *d, struct page *p, int top)
{
    struct viewport *vp = d ? *d->current_viewport : NULL;
    int value_ascent = font_get(p->font)->ascent;
    int label_ascent = font_get(p->label_font)->ascent;
    int base = top + PG_HEAD_GAP - PG_PITCH;
    bool first_group = true;

    for (int g = 0; g < GROUPS; g++)
    {
        bool any = false;
        for (int i = 0; i < p->count; i++)
        {
            const struct page_item *it = &p->item[i];
            if (it->group != g)
                continue;
            if (!any && !first_group)
            {
                int rule = base + PG_RULE_GAP;
                if (d)
                {
                    vp->fg_pattern = PG_RULE_RGB;
                    d->hline(0, vp->width - 1, rule);
                }
                base = rule + PG_GROUP_GAP;
            }
            else
                base += PG_PITCH;
            any = true;
            const char *value = p->text + it->text;
            int lines = text_wrap_count(value, PG_VALUE_R - PG_VALUE_X,
                                        p->font);
            if (d)
            {
                char label[48];
                int w;
                snprintf(label, sizeof(label), "%s:", str(it->label));
                vp->font = p->label_font;
                vp->fg_pattern = LCD_BLACK;
                font_getstringsize((const unsigned char *)label, &w, NULL,
                                   p->label_font);
                d->putsxy(PG_LABEL_R - w, base - label_ascent,
                          (const unsigned char *)label);
                vp->font = p->font;
                vp->fg_pattern = PG_VALUE_RGB;
                text_wrap_draw(d, value, PG_VALUE_X, base - value_ascent,
                               PG_VALUE_R - PG_VALUE_X, 'l');
            }
            base += (MAX(lines, 1) - 1) * font_get(p->font)->height;
        }
        first_group &= !any;
    }
    return base + PG_RULE_GAP;
}

static void page_paint(const struct ui_view *view,
                       const struct ui_view_frame *frame)
{
    struct page *p = view->data;
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp = {
        .x = frame->bounds.x, .y = frame->bounds.y,
        .width = frame->bounds.width, .height = frame->bounds.height,
        .flags = VP_DEFAULT_FLAGS, .font = p->name_font,
        .drawmode = DRMODE_FG, .buffer = &lcd_framebuffer_default,
        .fg_pattern = LCD_BLACK, .bg_pattern = LCD_WHITE,
    };
    struct viewport *last = d->set_viewport(&vp);
    int top = -p->scroll - frame->bounds.y;

    d->clear_viewport();
    stock_art_draw(STOCK_ART_CONTACTS_NOPHOTO, vp.x,
                   vp.y + top + PG_PHOTO_Y,
                   (struct ui_rect){ vp.x, vp.y, vp.width, vp.height });
    d->putsxy(PG_NAME_X, top + PG_NAME_BASE - font_get(p->name_font)->ascent,
              (const unsigned char *)p->name);
    vp.fg_pattern = PG_RULE_RGB;
    d->hline(0, vp.width - 1, top + PG_HEAD_RULE);
    page_lay_out(d, p, top + PG_HEAD_RULE);
    d->set_viewport(last);
}

static const struct ui_view_ops page_ops = { .paint = page_paint };

static int load_font(int role, int fallback)
{
    int font = ui_theme_current_font_load(role,
                                          global_settings.glyphs_to_cache);
    return font >= 0 ? font : fallback;
}

/* Shows the page until Menu; GO_TO_ROOT after USB, else 0. */
static int page_show(struct page *p)
{
    struct ui_rect bounds;
    struct viewport content;
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int ret = 0, ui = screens[SCREEN_MAIN].getuifont();

    p->font = load_font(UI_FONT_BODY, ui);
    p->label_font = load_font(UI_FONT_LIST_VALUE, ui);
    p->name_font = load_font(UI_FONT_TITLE, ui);

    ui_scene_open_page(&scene, "contact-page-push");
    sb_set_persistent_title(str(LANG_FRUIT_CONTACTS), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    bounds = (struct ui_rect){ content.x, content.y, content.width,
                               content.height };
    p->scroll = 0;
    p->scroll_max = MAX(page_lay_out(NULL, p, PG_HEAD_RULE) - LCD_HEIGHT, 0);
    ui_view_init(&view, bounds, &page_ops, p);
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
            to = MIN(p->scroll + PG_PITCH, p->scroll_max);
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            to = MAX(p->scroll - PG_PITCH, 0);
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

    ui_scene_close_modal(&scene, ret == 0, "contact-page-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (p->name_font != ui)
        font_unload(p->name_font);
    if (p->label_font != ui)
        font_unload(p->label_font);
    if (p->font != ui)
        font_unload(p->font);
    return ret;
}

/** The screen **/

/* Stock comes back to the row it left */
static int selection;

/* Runs the list until Menu: 0, or GO_TO_ROOT after USB */
static int show_list(struct contacts *cs, struct page *p)
{
    struct gui_synclist list;
    int action, ret = 0;

    for (bool open = true; open; )
    {
        sb_set_persistent_title(str(LANG_FRUIT_CONTACTS), Icon_NOICON,
                                SCREEN_MAIN);
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        gui_synclist_init(&list, get_name, cs, false, 1, NULL);
        gui_synclist_set_title(&list, str(LANG_FRUIT_CONTACTS), Icon_NOICON);
        gui_synclist_set_row_callbacks(&list, get_kind, NULL, NULL);
        gui_synclist_set_voice_callback(&list, get_talk);
        gui_synclist_set_row_height(&list, ROW_H);
        list.callback_draw_item = draw_row;
        gui_synclist_set_nb_items(&list, cs->count);
        gui_synclist_select_item(&list, MIN(selection, cs->count - 1));
        gui_synclist_draw_settled(&list);
        for (;;)
        {
            list_do_action(CONTEXT_TREE, HZ, &list, &action);
            if (action == ACTION_STD_OK)
            {
                selection = gui_synclist_get_sel_pos(&list);
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                page_read(cs, &cs->c[cs->order[selection]], p);
                if (page_show(p) == GO_TO_ROOT)
                {
                    ret = GO_TO_ROOT;
                    open = false;
                }
                break;
            }
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU ||
                default_event_handler(action) == SYS_USB_CONNECTED)
            {
                if (action != ACTION_STD_CANCEL && action != ACTION_STD_MENU)
                    ret = GO_TO_ROOT;
                selection = gui_synclist_get_sel_pos(&list);
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                open = false;
                break;
            }
        }
    }
    return ret;
}

int extras_contacts(void)
{
    size_t size = sizeof(struct contacts) + sizeof(struct page);
    int handle = core_alloc_free_space(size);
    int ret = 0;

    if (handle <= 0)
        return 0;
    struct contacts *cs = core_get_data_pinned(handle);
    struct page *p = (struct page *)(cs + 1);
    scan(cs);
    if (cs->count)
    {
        cs->by_last = global_settings.contacts_sort == 1;
        sorting = cs;
        qsort(cs->order, cs->count, sizeof(cs->order[0]), by_name);
        push_current_activity(ACTIVITY_TEXTVIEWER);
        sub_font = ui_theme_current_font_load(UI_FONT_BODY,
                                              global_settings.glyphs_to_cache);
        ret = show_list(cs, p);
        if (sub_font >= 0)
            font_unload(sub_font);
        sub_font = -1;
        pop_current_activity();
    }
    core_put_data_pinned(cs);
    core_free(handle);
    return ret;
}
