/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Contacts: browses, creates, edits, and deletes vCard 2.1/3.0/4.0
 * contacts in /Contacts and /Contacts/AstraPod/<UID>.vcf, preserving
 * unknown properties and sibling cards byte-for-byte with atomic writes.
 *
 * Draws the stock 2.0.5 look in Classic / Fruit Company / Fruit Company
 * Ultra and uses Scrim's native theme palette in Scrim.
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
#include "widgets/keyboard.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "root_menu.h"
#include "extras.h"
#include "extras_bar.h"
#include "extras_picker.h"
#include "extras_store.h"
#include "extras_vcard.h"

#define CONTACTS_DIR   VCARD_DIR
#define MAX_FILES      256
#define MAX_CONTACTS   1000
#define POOL_BYTES     49152
#define LINE_BYTES     1024
#define PAGE_ITEMS     36
#define PAGE_BYTES     4096
#define NONE           0xffff

/* The list's two-line rows, as stock's. */
#define ROW_H          44
#define ROW_TEXT_X     5
#define ROW_TITLE_BASE 18
#define ROW_SUB_BASE   35
#define ROW_CHEVRON_R  16
#define ROW_CHEVRON_Y  15
#define SUB_RGB        LCD_RGBPACK(107, 109, 107)
#define ROW_RULE_RGB   LCD_RGBPACK(222, 223, 222)

/* The detail page, from the top of the screen. */
#define PG_PHOTO_Y     20
#define PG_NAME_X      65
#define PG_NAME_BASE   49
#define PG_HEAD_RULE   75
#define PG_HEAD_GAP    22
#define PG_GROUP_GAP   21
#define PG_PITCH       15
#define PG_RULE_GAP    14
#define PG_LABEL_R     89
#define PG_VALUE_X     92
#define PG_VALUE_R     305
#define PG_VALUE_RGB   LCD_RGBPACK(74, 77, 74)
#define PG_RULE_RGB    LCD_RGBPACK(214, 211, 214)

static bool is_stock_look(void)
{
    return ui_theme_style(UI_STYLE_ART) == UI_ART_PLASTIC && stock_art_ready();
}

static int phone_type_lang(enum vcard_phone_type t)
{
    static const int ids[VCARD_TEL_TYPE_COUNT] = {
        LANG_CONTACTS_MOBILE, LANG_CONTACTS_HOME, LANG_CONTACTS_WORK,
        LANG_CONTACTS_MAIN, LANG_CONTACTS_FAX, LANG_CONTACTS_PAGER,
        LANG_CONTACTS_OTHER,
    };
    return ids[(unsigned)t < VCARD_TEL_TYPE_COUNT ? t : VCARD_TEL_OTHER];
}

static int kind_type_lang(enum vcard_kind_type t)
{
    static const int ids[VCARD_KIND_TYPE_COUNT] = {
        LANG_CONTACTS_HOME, LANG_CONTACTS_WORK, LANG_CONTACTS_OTHER,
    };
    return ids[(unsigned)t < VCARD_KIND_TYPE_COUNT ? t : VCARD_KIND_OTHER];
}

/* ---- Shared choice list & confirm helpers ---- */

struct c_choice_ctx {
    const char *const *items;
    int count;
};

static const char *c_choice_name(int i, void *data, char *buf, size_t size)
{
    (void)buf; (void)size;
    struct c_choice_ctx *c = data;
    return c->items[i];
}

static enum list_row_kind c_choice_kind(int i, void *data)
{
    (void)i; (void)data;
    return LIST_ROW_OPTION;
}

static int c_show_choice(const char *title, const char *const *items,
                         int count, int initial)
{
    struct c_choice_ctx ctx = { items, count };
    struct gui_synclist list;
    struct ui_scene scene = {0};
    int action, ret = -1;

    ui_scene_open_page(&scene, "con-choice-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_init(&list, c_choice_name, &ctx, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, c_choice_kind, NULL, NULL);
    gui_synclist_set_nb_items(&list, count);
    gui_synclist_select_item(&list, MIN(MAX(initial, 0), count - 1));
    gui_synclist_draw_settled(&list);

    for (;;)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        if (action == ACTION_STD_OK)
        {
            ret = gui_synclist_get_sel_pos(&list);
            break;
        }
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = -2;
            break;
        }
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret != -2, "con-choice-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

static int c_confirm(const char *question, int yes_lang, int no_lang)
{
    const char *lines[1] = { question };
    struct text_message msg = { lines, 1 };
    enum yesno_res r = gui_syncyesno_run_labels(&msg, yes_lang, no_lang);
    if (r == YESNO_USB)
        return -1;
    return r == YESNO_YES ? 1 : r == YESNO_CANCEL ? 2 : 0;
}

/* ---- Scanning the folder (top-level + 1 level of subdirectories) ---- */

struct contact {
    uint16_t file;
    uint32_t offset;
    uint32_t end;
    uint16_t first, last, name, sub;
};

struct contacts {
    int count, files, used;
    bool by_last;
    uint16_t file[MAX_FILES];        /* relative to /Contacts */
    struct contact c[MAX_CONTACTS];
    uint16_t order[MAX_CONTACTS];
    char pool[POOL_BYTES];
    char line[LINE_BYTES];
    char row[160];
};

static uint16_t keep(struct contacts *cs, const char *text)
{
    size_t len = strlen(text) + 1;
    if (!*text || cs->used + (int)len > POOL_BYTES)
        return NONE;
    memcpy(cs->pool + cs->used, text, len);
    cs->used += (int)len;
    return (uint16_t)(cs->used - (int)len);
}

static const char *pool_text(const struct contacts *cs, uint16_t at)
{
    return at == NONE ? "" : cs->pool + at;
}

static void contact_path(const struct contacts *cs, const struct contact *c,
                         char *buf, size_t size)
{
    snprintf(buf, size, CONTACTS_DIR "/%s", pool_text(cs, cs->file[c->file]));
}

static void scan_file(struct contacts *cs, int file)
{
    char path[MAX_PATH];
    struct extras_reader r;
    char first[96], last[96], name[96], org[96], tel[64], email[96];
    bool in_card = false;
    uint32_t start = 0, end = 0, card = 0;

    snprintf(path, sizeof(path), CONTACTS_DIR "/%s",
             pool_text(cs, cs->file[file]));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    extras_reader_init(&r, fd, 0);

    while (cs->count < MAX_CONTACTS &&
           extras_read_prop(&r, cs->line, sizeof(cs->line), &start, &end, true))
    {
        char *prop, *value;
        bool text = vcard_read_text(cs->line, sizeof(cs->line), &prop, &value);
        if (!strcasecmp(prop, "BEGIN") &&
            !strcasecmp(extras_trim(value), "VCARD"))
        {
            in_card = true;
            card = start;
            first[0] = last[0] = name[0] = org[0] = tel[0] = email[0] = '\0';
            continue;
        }
        if (!in_card || !text)
            continue;
        if (!strcasecmp(prop, "END") &&
            !strcasecmp(extras_trim(value), "VCARD"))
        {
            struct contact *c = &cs->c[cs->count];
            in_card = false;
            *c = (struct contact){
                .file = (uint16_t)file, .offset = card, .end = end,
                .first = keep(cs, first), .last = keep(cs, last),
                .name = keep(cs, name[0] ? name : org),
                .sub = keep(cs, tel[0] ? tel : email),
            };
            cs->order[cs->count] = (uint16_t)cs->count;
            cs->count++;
        }
        else if (!strcasecmp(prop, "N"))
        {
            char *part[2];
            vcard_split_parts(value, part, 2);
            strlcpy(last, extras_trim(part[0]), sizeof(last));
            strlcpy(first, extras_trim(part[1]), sizeof(first));
        }
        else if (!strcasecmp(prop, "FN"))
        {
            extras_unescape_text(value);
            strlcpy(name, extras_trim(value), sizeof(name));
        }
        else if (!strcasecmp(prop, "ORG"))
        {
            char *part[1];
            vcard_split_parts(value, part, 1);
            strlcpy(org, extras_trim(part[0]), sizeof(org));
        }
        else if (!strcasecmp(prop, "TEL") && !tel[0])
        {
            extras_unescape_text(value);
            strlcpy(tel, extras_trim(value), sizeof(tel));
        }
        else if (!strcasecmp(prop, "EMAIL") && !email[0])
        {
            extras_unescape_text(value);
            strlcpy(email, extras_trim(value), sizeof(email));
        }
    }
    close(fd);
}

static void collect_dir(struct contacts *cs, const char *rel_prefix)
{
    char dir_path[MAX_PATH];
    if (rel_prefix && *rel_prefix)
        snprintf(dir_path, sizeof(dir_path), CONTACTS_DIR "/%s", rel_prefix);
    else
        strlcpy(dir_path, CONTACTS_DIR, sizeof(dir_path));

    DIR *dir = opendir(dir_path);
    if (!dir)
        return;

    /* Collect subdirs only at top level. */
    char subdirs[16][64];
    int sub_count = 0;

    while (cs->files < MAX_FILES)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        const char *name = (const char *)entry->d_name;
        if (name[0] == '.')
            continue;
        bool is_dir = (dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) != 0;
        if (is_dir)
        {
            if ((!rel_prefix || !*rel_prefix) && sub_count < 16)
                strlcpy(subdirs[sub_count++], name, sizeof(subdirs[0]));
            continue;
        }
        if (!extras_has_ext(name, ".vcf"))
            continue;
        char rel[MAX_PATH];
        if (rel_prefix && *rel_prefix)
            snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, name);
        else
            strlcpy(rel, name, sizeof(rel));
        uint16_t at = keep(cs, rel);
        if (at != NONE)
            cs->file[cs->files++] = at;
    }
    closedir(dir);

    for (int i = 0; i < sub_count && cs->files < MAX_FILES; i++)
        collect_dir(cs, subdirs[i]);
}

static const struct contacts *sorting;

static void sort_keys(const struct contacts *cs, const struct contact *c,
                      const char **key, const char **then)
{
    const char *first = pool_text(cs, c->first);
    const char *last = pool_text(cs, c->last);
    *key = cs->by_last ? (*last ? last : first) : (*first ? first : last);
    *then = cs->by_last ? (*last ? first : "") : (*first ? last : "");
    if (!**key)
        *key = pool_text(cs, c->name);
}

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

static void scan_and_sort(struct contacts *cs)
{
    cs->count = cs->files = cs->used = 0;
    collect_dir(cs, NULL);
    for (int f = 0; f < cs->files; f++)
        scan_file(cs, f);
    cs->by_last = global_settings.contacts_sort == 1;
    if (cs->count > 1)
    {
        sorting = cs;
        qsort(cs->order, (size_t)cs->count, sizeof(cs->order[0]), by_name);
    }
}

bool extras_contacts_any(void)
{
    return vcard_any_in(CONTACTS_DIR);
}

/* ---- Address Sub-Editor ---- */

enum {
    ADRF_TYPE, ADRF_STREET, ADRF_CITY, ADRF_REGION,
    ADRF_POSTAL, ADRF_COUNTRY, ADRF_DONE, ADRF_DELETE
};

struct addr_edit_ctx {
    struct vcard_addr *a;
    bool allow_delete;
    int count;
};

static const char *addr_row_name(int i, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    static const char *const names[8] = {
        "Type", "Street", "City", "Region", "Postal", "Country",
        "Done", "Delete Address",
    };
    return names[i];
}

static const char *addr_row_value(int i, void *data, char *buf, size_t size)
{
    (void)buf; (void)size;
    struct addr_edit_ctx *ctx = data;
    const struct vcard_addr *a = ctx->a;
    switch (i)
    {
        case ADRF_TYPE:    return str(kind_type_lang(a->type));
        case ADRF_STREET:  return a->street[0] ? a->street : "None";
        case ADRF_CITY:    return a->city[0] ? a->city : "None";
        case ADRF_REGION:  return a->region[0] ? a->region : "None";
        case ADRF_POSTAL:  return a->postal[0] ? a->postal : "None";
        case ADRF_COUNTRY: return a->country[0] ? a->country : "None";
        default:           return NULL;
    }
}

static enum list_row_kind addr_row_kind(int i, void *data)
{
    (void)data;
    return (i == ADRF_DONE || i == ADRF_DELETE) ?
           LIST_ROW_ACTION : LIST_ROW_SETTING;
}

/* Returns 1 if kept/edited, 2 if deleted, 0 if cancelled, -1 on USB. */
static int run_addr_editor(struct vcard_addr *a, bool allow_delete)
{
    struct addr_edit_ctx ctx = { a, allow_delete, allow_delete ? 8 : 7 };
    struct gui_synclist list;
    struct ui_scene scene = {0};
    int sel = 0, action, ret = 1;

    ui_scene_open_page(&scene, "con-addr-push");
    sb_set_persistent_title("Address", Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_init(&list, addr_row_name, &ctx, false, 1, NULL);
    gui_synclist_set_title(&list, "Address", Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, addr_row_kind, addr_row_value, NULL);
    gui_synclist_set_nb_items(&list, ctx.count);
    gui_synclist_select_item(&list, sel);
    gui_synclist_draw_settled(&list);

    for (;;)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        sel = gui_synclist_get_sel_pos(&list);
        if (action == ACTION_STD_OK)
        {
            if (sel == ADRF_TYPE)
            {
                const char *opts[3] = {
                    str(LANG_CONTACTS_HOME), str(LANG_CONTACTS_WORK),
                    str(LANG_CONTACTS_OTHER),
                };
                int r = c_show_choice("Address Type", opts, 3, a->type);
                if (r == -2) { ret = -1; break; }
                if (r >= 0) a->type = (enum vcard_kind_type)r;
            }
            else if (sel == ADRF_STREET)
                dialog_input_extended(a->street, sizeof(a->street));
            else if (sel == ADRF_CITY)
                dialog_input_extended(a->city, sizeof(a->city));
            else if (sel == ADRF_REGION)
                dialog_input_extended(a->region, sizeof(a->region));
            else if (sel == ADRF_POSTAL)
                dialog_input_extended(a->postal, sizeof(a->postal));
            else if (sel == ADRF_COUNTRY)
                dialog_input_extended(a->country, sizeof(a->country));
            else if (sel == ADRF_DONE)
            {
                ret = 1;
                break;
            }
            else if (sel == ADRF_DELETE)
            {
                ret = 2;
                break;
            }
            sb_set_persistent_title("Address", Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            ret = 1;
            break;
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = -1;
            break;
        }
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret >= 0, "con-addr-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

/* ---- Contact Editor ---- */

enum ce_kind {
    CE_FIRST, CE_LAST, CE_ORG,
    CE_PHONE, CE_ADD_PHONE,
    CE_EMAIL, CE_ADD_EMAIL,
    CE_ADDR, CE_ADD_ADDR,
    CE_BDAY, CE_NOTE, CE_SAVE, CE_DELETE
};

struct ce_row {
    enum ce_kind kind;
    int index;
};

struct ce_ctx {
    struct vcard_contact *vc;
    struct ce_row rows[32];
    int count;
    bool dirty;
};

static void ce_rebuild(struct ce_ctx *ctx)
{
    struct vcard_contact *vc = ctx->vc;
    int n = 0;
    ctx->rows[n++] = (struct ce_row){ CE_FIRST, 0 };
    ctx->rows[n++] = (struct ce_row){ CE_LAST, 0 };
    ctx->rows[n++] = (struct ce_row){ CE_ORG, 0 };
    for (int i = 0; i < vc->phone_count; i++)
        ctx->rows[n++] = (struct ce_row){ CE_PHONE, i };
    if (vc->phone_count < VCARD_MAX_PHONES)
        ctx->rows[n++] = (struct ce_row){ CE_ADD_PHONE, 0 };
    for (int i = 0; i < vc->email_count; i++)
        ctx->rows[n++] = (struct ce_row){ CE_EMAIL, i };
    if (vc->email_count < VCARD_MAX_EMAILS)
        ctx->rows[n++] = (struct ce_row){ CE_ADD_EMAIL, 0 };
    for (int i = 0; i < vc->addr_count; i++)
        ctx->rows[n++] = (struct ce_row){ CE_ADDR, i };
    if (vc->addr_count < VCARD_MAX_ADDRS)
        ctx->rows[n++] = (struct ce_row){ CE_ADD_ADDR, 0 };
    ctx->rows[n++] = (struct ce_row){ CE_BDAY, 0 };
    ctx->rows[n++] = (struct ce_row){ CE_NOTE, 0 };
    ctx->rows[n++] = (struct ce_row){ CE_SAVE, 0 };
    if (vc->is_existing)
        ctx->rows[n++] = (struct ce_row){ CE_DELETE, 0 };
    ctx->count = n;
}

static const char *ce_row_name(int i, void *data, char *buf, size_t size)
{
    struct ce_ctx *ctx = data;
    struct ce_row r = ctx->rows[i];
    switch (r.kind)
    {
        case CE_FIRST:     return "First Name";
        case CE_LAST:      return "Last Name";
        case CE_ORG:       return "Organisation";
        case CE_PHONE:
            return str(phone_type_lang(ctx->vc->phones[r.index].type));
        case CE_ADD_PHONE: return "Add Phone";
        case CE_EMAIL:
            snprintf(buf, size, "%s Email",
                     str(kind_type_lang(ctx->vc->emails[r.index].type)));
            return buf;
        case CE_ADD_EMAIL: return "Add Email";
        case CE_ADDR:
            snprintf(buf, size, "%s Address",
                     str(kind_type_lang(ctx->vc->addrs[r.index].type)));
            return buf;
        case CE_ADD_ADDR:  return "Add Address";
        case CE_BDAY:      return "Birthday";
        case CE_NOTE:      return str(LANG_CONTACTS_NOTES);
        case CE_SAVE:      return str(LANG_SAVE);
        case CE_DELETE:    return "Delete Contact";
        default:           return "";
    }
}

static const char *ce_row_value(int i, void *data, char *buf, size_t size)
{
    struct ce_ctx *ctx = data;
    const struct vcard_contact *vc = ctx->vc;
    struct ce_row r = ctx->rows[i];
    switch (r.kind)
    {
        case CE_FIRST: return vc->first[0] ? vc->first : "None";
        case CE_LAST:  return vc->last[0] ? vc->last : "None";
        case CE_ORG:   return vc->org[0] ? vc->org : "None";
        case CE_PHONE: return vc->phones[r.index].value;
        case CE_EMAIL: return vc->emails[r.index].value;
        case CE_ADDR:
        {
            const struct vcard_addr *a = &vc->addrs[r.index];
            if (a->street[0]) return a->street;
            if (a->city[0]) return a->city;
            return "Edit";
        }
        case CE_BDAY:
            if (!vc->has_bday)
                return "None";
            snprintf(buf, size, "%04d-%02d-%02d",
                     vc->bday.year, vc->bday.month, vc->bday.day);
            return buf;
        case CE_NOTE:  return vc->note[0] ? vc->note : "None";
        default:       return NULL;
    }
}

static enum list_row_kind ce_row_kind(int i, void *data)
{
    struct ce_ctx *ctx = data;
    switch (ctx->rows[i].kind)
    {
        case CE_ADD_PHONE:
        case CE_ADD_EMAIL:
        case CE_ADD_ADDR:
        case CE_SAVE:
        case CE_DELETE:
            return LIST_ROW_ACTION;
        default:
            return LIST_ROW_SETTING;
    }
}

static int pick_phone_type(enum vcard_phone_type cur)
{
    const char *opts[VCARD_TEL_TYPE_COUNT];
    for (int i = 0; i < VCARD_TEL_TYPE_COUNT; i++)
        opts[i] = str(phone_type_lang((enum vcard_phone_type)i));
    return c_show_choice("Phone Type", opts, VCARD_TEL_TYPE_COUNT, cur);
}

static int pick_kind_type(const char *title, enum vcard_kind_type cur)
{
    const char *opts[VCARD_KIND_TYPE_COUNT];
    for (int i = 0; i < VCARD_KIND_TYPE_COUNT; i++)
        opts[i] = str(kind_type_lang((enum vcard_kind_type)i));
    return c_show_choice(title, opts, VCARD_KIND_TYPE_COUNT, cur);
}

/* Returns: 0 = unchanged/cancel, 1 = saved, 2 = deleted, GO_TO_ROOT on USB. */
static int run_contact_editor(struct vcard_contact *vc)
{
    struct ce_ctx ctx = { .vc = vc, .dirty = false };
    struct gui_synclist list;
    struct ui_scene scene = {0};
    const char *title = vc->is_existing ? "Edit Contact" : "New Contact";
    int sel = 0, action, ret = 0;

    ce_rebuild(&ctx);
    ui_scene_open_page(&scene, "con-edit-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);

    gui_synclist_init(&list, ce_row_name, &ctx, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, ce_row_kind, ce_row_value, NULL);
    gui_synclist_set_nb_items(&list, ctx.count);
    gui_synclist_select_item(&list, sel);
    gui_synclist_draw_settled(&list);

    for (;;)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        sel = gui_synclist_get_sel_pos(&list);
        struct ce_row r = ctx.rows[sel];

        if (action == ACTION_STD_CONTEXT &&
            (r.kind == CE_PHONE || r.kind == CE_EMAIL))
        {
            static const char *const opts[3] = {
                "Edit", "Change Type", "Delete"
            };
            int ch = c_show_choice(r.kind == CE_PHONE ? "Phone" : "Email",
                                   opts, 3, 0);
            if (ch == -2) { ret = GO_TO_ROOT; break; }
            if (r.kind == CE_PHONE)
            {
                if (ch == 0)
                {
                    if (dialog_input_extended(vc->phones[r.index].value,
                                     sizeof(vc->phones[0].value)) == 0)
                        ctx.dirty = true;
                }
                else if (ch == 1)
                {
                    int t = pick_phone_type(vc->phones[r.index].type);
                    if (t == -2) { ret = GO_TO_ROOT; break; }
                    if (t >= 0)
                    {
                        vc->phones[r.index].type = (enum vcard_phone_type)t;
                        ctx.dirty = true;
                    }
                }
                else if (ch == 2)
                {
                    memmove(&vc->phones[r.index], &vc->phones[r.index + 1],
                            (size_t)(vc->phone_count - r.index - 1) *
                            sizeof(vc->phones[0]));
                    vc->phone_count--;
                    ctx.dirty = true;
                }
            }
            else
            {
                if (ch == 0)
                {
                    if (dialog_input_extended(vc->emails[r.index].value,
                                     sizeof(vc->emails[0].value)) == 0)
                        ctx.dirty = true;
                }
                else if (ch == 1)
                {
                    int t = pick_kind_type("Email Type",
                                           vc->emails[r.index].type);
                    if (t == -2) { ret = GO_TO_ROOT; break; }
                    if (t >= 0)
                    {
                        vc->emails[r.index].type = (enum vcard_kind_type)t;
                        ctx.dirty = true;
                    }
                }
                else if (ch == 2)
                {
                    memmove(&vc->emails[r.index], &vc->emails[r.index + 1],
                            (size_t)(vc->email_count - r.index - 1) *
                            sizeof(vc->emails[0]));
                    vc->email_count--;
                    ctx.dirty = true;
                }
            }
            ce_rebuild(&ctx);
            gui_synclist_set_nb_items(&list, ctx.count);
            gui_synclist_select_item(&list, MIN(sel, ctx.count - 1));
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
            continue;
        }

        if (action == ACTION_STD_OK)
        {
            if (r.kind == CE_FIRST)
            {
                if (dialog_input_extended(vc->first, sizeof(vc->first)) == 0)
                    ctx.dirty = true;
            }
            else if (r.kind == CE_LAST)
            {
                if (dialog_input_extended(vc->last, sizeof(vc->last)) == 0)
                    ctx.dirty = true;
            }
            else if (r.kind == CE_ORG)
            {
                if (dialog_input_extended(vc->org, sizeof(vc->org)) == 0)
                    ctx.dirty = true;
            }
            else if (r.kind == CE_PHONE)
            {
                char tmp[sizeof(vc->phones[0].value)];
                strlcpy(tmp, vc->phones[r.index].value, sizeof(tmp));
                if (dialog_input_extended(tmp, sizeof(tmp)) == 0)
                {
                    if (!tmp[0])
                    {
                        memmove(&vc->phones[r.index], &vc->phones[r.index + 1],
                                (size_t)(vc->phone_count - r.index - 1) *
                                sizeof(vc->phones[0]));
                        vc->phone_count--;
                    }
                    else
                        strlcpy(vc->phones[r.index].value, tmp,
                                sizeof(vc->phones[0].value));
                    ctx.dirty = true;
                }
            }
            else if (r.kind == CE_ADD_PHONE &&
                     vc->phone_count < VCARD_MAX_PHONES)
            {
                int t = pick_phone_type(VCARD_TEL_MOBILE);
                if (t == -2) { ret = GO_TO_ROOT; break; }
                if (t >= 0)
                {
                    char tmp[sizeof(vc->phones[0].value)] = "";
                    if (dialog_input_extended(tmp, sizeof(tmp)) == 0 && tmp[0])
                    {
                        vc->phones[vc->phone_count].source_offset = 0;
                        vc->phones[vc->phone_count].type =
                            (enum vcard_phone_type)t;
                        strlcpy(vc->phones[vc->phone_count].value, tmp,
                                sizeof(vc->phones[0].value));
                        vc->phone_count++;
                        ctx.dirty = true;
                    }
                }
            }
            else if (r.kind == CE_EMAIL)
            {
                char tmp[sizeof(vc->emails[0].value)];
                strlcpy(tmp, vc->emails[r.index].value, sizeof(tmp));
                if (dialog_input_extended(tmp, sizeof(tmp)) == 0)
                {
                    if (!tmp[0])
                    {
                        memmove(&vc->emails[r.index], &vc->emails[r.index + 1],
                                (size_t)(vc->email_count - r.index - 1) *
                                sizeof(vc->emails[0]));
                        vc->email_count--;
                    }
                    else
                        strlcpy(vc->emails[r.index].value, tmp,
                                sizeof(vc->emails[0].value));
                    ctx.dirty = true;
                }
            }
            else if (r.kind == CE_ADD_EMAIL &&
                     vc->email_count < VCARD_MAX_EMAILS)
            {
                int t = pick_kind_type("Email Type", VCARD_KIND_WORK);
                if (t == -2) { ret = GO_TO_ROOT; break; }
                if (t >= 0)
                {
                    char tmp[sizeof(vc->emails[0].value)] = "";
                    if (dialog_input_extended(tmp, sizeof(tmp)) == 0 && tmp[0])
                    {
                        vc->emails[vc->email_count].source_offset = 0;
                        vc->emails[vc->email_count].type =
                            (enum vcard_kind_type)t;
                        strlcpy(vc->emails[vc->email_count].value, tmp,
                                sizeof(vc->emails[0].value));
                        vc->email_count++;
                        ctx.dirty = true;
                    }
                }
            }
            else if (r.kind == CE_ADDR)
            {
                int ar = run_addr_editor(&vc->addrs[r.index], true);
                if (ar < 0) { ret = GO_TO_ROOT; break; }
                if (ar == 2)
                {
                    memmove(&vc->addrs[r.index], &vc->addrs[r.index + 1],
                            (size_t)(vc->addr_count - r.index - 1) *
                            sizeof(vc->addrs[0]));
                    vc->addr_count--;
                }
                ctx.dirty = true;
            }
            else if (r.kind == CE_ADD_ADDR && vc->addr_count < VCARD_MAX_ADDRS)
            {
                struct vcard_addr na;
                memset(&na, 0, sizeof(na));
                na.type = VCARD_KIND_HOME;
                int ar = run_addr_editor(&na, false);
                if (ar < 0) { ret = GO_TO_ROOT; break; }
                if (ar == 1 && (na.street[0] || na.city[0] || na.region[0] ||
                                na.postal[0] || na.country[0]))
                {
                    vc->addrs[vc->addr_count++] = na;
                    ctx.dirty = true;
                }
            }
            else if (r.kind == CE_BDAY)
            {
                if (vc->has_bday)
                {
                    static const char *const opts[2] = {
                        "Change Birthday", "Clear Birthday"
                    };
                    int ch = c_show_choice("Birthday", opts, 2, 0);
                    if (ch == -2) { ret = GO_TO_ROOT; break; }
                    if (ch == 0)
                    {
                        int pr = extras_pick_date("Birthday", vc->first,
                                                  &vc->bday, 1900, 2099);
                        if (pr < 0) { ret = GO_TO_ROOT; break; }
                        if (pr > 0) ctx.dirty = true;
                    }
                    else if (ch == 1)
                    {
                        vc->has_bday = false;
                        ctx.dirty = true;
                    }
                }
                else
                {
                    vc->bday = (struct extras_date){ 1990, 1, 1 };
                    int pr = extras_pick_date("Birthday", vc->first,
                                              &vc->bday, 1900, 2099);
                    if (pr < 0) { ret = GO_TO_ROOT; break; }
                    if (pr > 0) { vc->has_bday = true; ctx.dirty = true; }
                }
            }
            else if (r.kind == CE_NOTE)
            {
                if (dialog_input_extended(vc->note, sizeof(vc->note)) == 0)
                    ctx.dirty = true;
            }
            else if (r.kind == CE_SAVE)
            {
                if (vcard_save(CONTACTS_DIR, vc))
                {
                    ret = 1;
                    break;
                }
                splash(HZ, "Could not save contact");
            }
            else if (r.kind == CE_DELETE)
            {
                int c = c_confirm("Delete this contact?",
                                  LANG_DELETE, LANG_DIAG_CANCEL);
                if (c < 0) { ret = GO_TO_ROOT; break; }
                if (c == 1)
                {
                    if (vcard_delete(vc->path, vc->card_start, vc->card_end))
                    {
                        ret = 2;
                        break;
                    }
                    splash(HZ, "Could not delete contact");
                }
            }
            ce_rebuild(&ctx);
            gui_synclist_set_nb_items(&list, ctx.count);
            gui_synclist_select_item(&list, MIN(sel, ctx.count - 1));
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            if (ctx.dirty)
            {
                int c = c_confirm("Save changes?", LANG_SAVE, LANG_EXTRAS_DISCARD);
                if (c < 0) { ret = GO_TO_ROOT; break; }
                if (c == 2) continue;
                if (c == 1)
                {
                    if (!vcard_save(CONTACTS_DIR, vc))
                    {
                        splash(HZ, "Could not save contact");
                        gui_synclist_draw(&list);
                        continue;
                    }
                    ret = 1;
                }
            }
            break;
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret != GO_TO_ROOT, "con-edit-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

/* ---- Contacts List & Detail Page ---- */

static int sub_font = -1;

static const char *row_name(struct contacts *cs, int row, char *buf,
                            size_t size)
{
    const struct contact *c = &cs->c[cs->order[row]];
    const char *first = pool_text(cs, c->first);
    const char *last = pool_text(cs, c->last);
    if (*first && *last)
        snprintf(buf, size, cs->by_last ? "%s, %s" : "%s %s",
                 cs->by_last ? last : first, cs->by_last ? first : last);
    else if (*first || *last)
        strlcpy(buf, *first ? first : last, size);
    else if (c->name != NONE)
        strlcpy(buf, pool_text(cs, c->name), size);
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
    (void)row; (void)data;
    return LIST_ROW_MENU;
}

static void draw_row(struct list_putlineinfo_t *info)
{
    struct contacts *cs = info->list->data;
    struct viewport *vp = info->vp;
    const struct viewport *parent = info->list->parent[SCREEN_MAIN];
    int width = parent->x + parent->width - (vp->x + info->x);
    unsigned fg = vp->fg_pattern;
    int font = vp->font;
    int x = info->x + (is_stock_look() ? ROW_TEXT_X : 10);
    bool stock = is_stock_look();

    if (info->is_selected)
    {
        const uint32_t *bar = stock ? ui_art_selection(ROW_H) : NULL;
        if (bar)
        {
            for (int y = 0; y < ROW_H; y++)
            {
                vp->fg_pattern = LCD_RGBPACK(bar[y] >> 16, (bar[y] >> 8) & 255,
                                             bar[y] & 255);
                lcd_hline(info->x, info->x + width - 1, info->y + y);
            }
        }
        else
        {
            vp->fg_pattern = LCD_RGBPACK(30, 34, 44);
            lcd_fillrect(info->x, info->y, width, ROW_H);
            vp->fg_pattern = ui_theme_rgb565(UI_COLOR_SELECTION);
            lcd_fillrect(info->x, info->y + 4, 4, ROW_H - 8);
        }
    }

    vp->fg_pattern = info->is_selected ?
        (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
        (stock ? fg : ui_theme_rgb565(UI_COLOR_FOREGROUND));
    lcd_putsxy(x, info->y + ROW_TITLE_BASE - font_get(font)->ascent,
               (const unsigned char *)info->dsp_text);

    const struct contact *c = &cs->c[cs->order[info->line]];
    if (sub_font >= 0 && c->sub != NONE)
    {
        vp->font = sub_font;
        vp->fg_pattern = info->is_selected ?
            (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
            (stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED));
        lcd_putsxy(x, info->y + ROW_SUB_BASE - font_get(sub_font)->ascent,
                   (const unsigned char *)pool_text(cs, c->sub));
        vp->font = font;
    }

    if (info->is_selected && stock)
        stock_art_draw(STOCK_ART_SYSTEM_SUBMENU,
                       vp->x + info->x + width - ROW_CHEVRON_R,
                       vp->y + info->y + ROW_CHEVRON_Y,
                       (struct ui_rect){ vp->x, vp->y, vp->width, vp->height });
    else if (!info->is_selected)
    {
        vp->fg_pattern = stock ? ROW_RULE_RGB :
                                 ui_theme_rgb565(UI_COLOR_SEPARATOR);
        lcd_hline(info->x, info->x + width - 1, info->y + ROW_H - 1);
    }
    vp->fg_pattern = fg;
}

/* ---- Detail page ---- */

enum { G_PHONE, G_EMAIL, G_ADDRESS, G_INFO, G_NOTE, GROUPS };

struct page_item {
    uint8_t group;
    const char *label;
    uint16_t text;
};

struct page {
    char name[160];
    char title[96];
    int count, used;
    struct page_item item[PAGE_ITEMS];
    char text[PAGE_BYTES];
    int font, label_font, name_font;
    int scroll, scroll_max;
};

static void page_add(struct page *p, int group, const char *label,
                     const char *value)
{
    size_t len = strlen(value) + 1;
    if (!*value || p->count >= PAGE_ITEMS || p->used + (int)len > PAGE_BYTES)
        return;
    memcpy(p->text + p->used, value, len);
    p->item[p->count++] = (struct page_item){
        (uint8_t)group, label, (uint16_t)p->used
    };
    p->used += (int)len;
}

static void page_build_from_vcard(const struct vcard_contact *vc,
                                  struct page *p)
{
    p->count = p->used = 0;
    if (vc->fn[0])
        strlcpy(p->name, vc->fn, sizeof(p->name));
    else if (vc->first[0] || vc->last[0])
        snprintf(p->name, sizeof(p->name), "%s%s%s", vc->first,
                 vc->first[0] && vc->last[0] ? " " : "", vc->last);
    else if (vc->org[0])
        strlcpy(p->name, vc->org, sizeof(p->name));
    else
        strlcpy(p->name, str(LANG_CONTACTS_NO_NAME), sizeof(p->name));

    for (int i = 0; i < vc->phone_count; i++)
        page_add(p, G_PHONE, str(phone_type_lang(vc->phones[i].type)),
                 vc->phones[i].value);
    for (int i = 0; i < vc->email_count; i++)
        page_add(p, G_EMAIL, str(kind_type_lang(vc->emails[i].type)),
                 vc->emails[i].value);
    for (int i = 0; i < vc->addr_count; i++)
    {
        char out[320];
        vcard_format_addr(&vc->addrs[i], out, sizeof(out));
        page_add(p, G_ADDRESS, str(kind_type_lang(vc->addrs[i].type)), out);
    }
    if (vc->org[0] && strcmp(vc->org, p->name) != 0)
        page_add(p, G_INFO, "Org", vc->org);
    if (vc->has_bday)
    {
        char bbuf[16];
        snprintf(bbuf, sizeof(bbuf), "%04d-%02d-%02d",
                 vc->bday.year, vc->bday.month, vc->bday.day);
        page_add(p, G_INFO, "Birthday", bbuf);
    }
    if (vc->note[0])
        page_add(p, G_NOTE, str(LANG_CONTACTS_NOTES), vc->note);
}

static int page_lay_out(struct screen *d, struct page *p, int top)
{
    struct viewport *vp = d ? *d->current_viewport : NULL;
    int value_ascent = font_get(p->font)->ascent;
    int label_ascent = font_get(p->label_font)->ascent;
    int base = top + PG_HEAD_GAP - PG_PITCH;
    bool first_group = true;
    bool stock = is_stock_look();

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
                    vp->fg_pattern = stock ? PG_RULE_RGB :
                                             ui_theme_rgb565(UI_COLOR_SEPARATOR);
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
                int w = 0;
                snprintf(label, sizeof(label), "%s:", it->label);
                vp->font = p->label_font;
                vp->fg_pattern = stock ? LCD_BLACK :
                                         ui_theme_rgb565(UI_COLOR_FOREGROUND);
                font_getstringsize((const unsigned char *)label, &w, NULL,
                                   p->label_font);
                d->putsxy(PG_LABEL_R - w, base - label_ascent,
                          (const unsigned char *)label);
                vp->font = p->font;
                vp->fg_pattern = stock ? PG_VALUE_RGB :
                                         ui_theme_rgb565(UI_COLOR_MUTED);
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
    bool stock = is_stock_look();
    struct viewport vp = {
        .x = frame->bounds.x, .y = frame->bounds.y,
        .width = frame->bounds.width, .height = frame->bounds.height,
        .flags = VP_DEFAULT_FLAGS, .font = p->name_font,
        .drawmode = DRMODE_FG, .buffer = &lcd_framebuffer_default,
        .fg_pattern = stock ? LCD_BLACK : ui_theme_rgb565(UI_COLOR_FOREGROUND),
        .bg_pattern = stock ? LCD_WHITE : ui_theme_rgb565(UI_COLOR_BACKGROUND),
    };
    struct viewport *last = d->set_viewport(&vp);
    int top = -p->scroll - frame->bounds.y;

    d->clear_viewport();
    if (stock)
        stock_art_draw(STOCK_ART_CONTACTS_NOPHOTO, vp.x,
                       vp.y + top + PG_PHOTO_Y,
                       (struct ui_rect){ vp.x, vp.y, vp.width, vp.height });
    else
    {
        vp.fg_pattern = ui_theme_rgb565(UI_COLOR_LIST_TINT);
        d->fillrect(8, top + PG_PHOTO_Y + 6, 44, 44);
        vp.fg_pattern = ui_theme_rgb565(UI_COLOR_FOREGROUND);
    }
    d->putsxy(PG_NAME_X, top + PG_NAME_BASE - font_get(p->name_font)->ascent,
              (const unsigned char *)p->name);
    vp.fg_pattern = stock ? PG_RULE_RGB : ui_theme_rgb565(UI_COLOR_SEPARATOR);
    d->hline(0, vp.width - 1, top + PG_HEAD_RULE);
    page_lay_out(d, p, top + PG_HEAD_RULE);
    if (p->scroll_max > 0)
    {
        int track_h = vp.height - 8;
        int total_h = vp.height + p->scroll_max;
        int thumb_h = MAX(20, track_h * vp.height / total_h);
        int thumb_y = 4 + (track_h - thumb_h) * p->scroll / p->scroll_max;
        vp.fg_pattern = LCD_RGBPACK(140, 142, 140);
        d->fillrect(vp.width - 5, thumb_y, 3, thumb_h);
    }
    d->set_viewport(last);
}

static const struct ui_view_ops page_ops = { .paint = page_paint };

static int load_font(int role, int fallback)
{
    int font = ui_theme_current_font_load(role,
                                          global_settings.glyphs_to_cache);
    return font >= 0 ? font : fallback;
}

/* Shows contact detail; returns 0 (no change), 1 (edited/deleted), or
 * GO_TO_ROOT on USB. */
static int page_show(struct vcard_contact *vc, struct page *p)
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

    page_build_from_vcard(vc, p);
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
            sb_set_persistent_title(str(LANG_FRUIT_CONTACTS), Icon_NOICON,
                                    SCREEN_MAIN);
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
            dirty = false;
        }
        int action = extras_get_action(TIMEOUT_BLOCK, true);
        int to = p->scroll;
        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            to = MIN(p->scroll + PG_PITCH, p->scroll_max);
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            to = MAX(p->scroll - PG_PITCH, 0);
        else if (action == ACTION_STD_OK)
        {
            viewportmanager_theme_undo(SCREEN_MAIN, false);
            int er = run_contact_editor(vc);
            viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
            if (er == GO_TO_ROOT) { ret = GO_TO_ROOT; break; }
            if (er == 2) { ret = 1; break; } /* deleted */
            if (er == 1)
            {
                ret = 1;
                if (!vc->card_end) break;
                page_build_from_vcard(vc, p);
                p->scroll = 0;
                p->scroll_max = MAX(page_lay_out(NULL, p, PG_HEAD_RULE) -
                                    LCD_HEIGHT, 0);
            }
            dirty = true;
            continue;
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            static const char *const opts[2] = {
                "Edit Contact", "Delete Contact"
            };
            viewportmanager_theme_undo(SCREEN_MAIN, false);
            int ch = c_show_choice(p->name, opts, 2, 0);
            if (ch == -2)
            {
                viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
                ret = GO_TO_ROOT;
                break;
            }
            if (ch == 0)
            {
                int er = run_contact_editor(vc);
                viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
                if (er == GO_TO_ROOT) { ret = GO_TO_ROOT; break; }
                if (er == 2) { ret = 1; break; }
                if (er == 1)
                {
                    ret = 1;
                    if (!vc->card_end) break;
                    page_build_from_vcard(vc, p);
                    p->scroll = 0;
                    p->scroll_max = MAX(page_lay_out(NULL, p, PG_HEAD_RULE) -
                                        LCD_HEIGHT, 0);
                }
            }
            else if (ch == 1)
            {
                int cf = c_confirm("Delete this contact?",
                                   LANG_DELETE, LANG_DIAG_CANCEL);
                viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
                if (cf < 0) { ret = GO_TO_ROOT; break; }
                if (cf == 1 &&
                    vcard_delete(vc->path, vc->card_start, vc->card_end))
                {
                    ret = 1;
                    break;
                }
            }
            else
                viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
            dirty = true;
            continue;
        }
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

    ui_scene_close_modal(&scene, ret != GO_TO_ROOT, "contact-page-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (p->name_font != ui) font_unload(p->name_font);
    if (p->label_font != ui) font_unload(p->label_font);
    if (p->font != ui) font_unload(p->font);
    return ret;
}

static void paint_empty_contacts(int head_font, int body_font)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport content;
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    bool stock = is_stock_look();
    content.bg_pattern = stock ? LCD_WHITE :
                                 ui_theme_rgb565(UI_COLOR_BACKGROUND);
    content.fg_pattern = stock ? LCD_BLACK :
                                 ui_theme_rgb565(UI_COLOR_FOREGROUND);
    content.drawmode = DRMODE_SOLID;
    struct viewport *last = d->set_viewport(&content);
    d->clear_viewport();

    int f = head_font >= 0 ? head_font : content.font;
    content.font = f;
    content.drawmode = DRMODE_FG;
    const char *h = str(LANG_FRUIT_NO_CONTACTS);
    int w = 0, ht = 0;
    font_getstringsize((const unsigned char *)h, &w, &ht, f);
    d->putsxy((content.width - w) / 2, content.height / 2 - ht,
              (const unsigned char *)h);

    int sf = body_font >= 0 ? body_font : f;
    content.font = sf;
    content.fg_pattern = stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED);
    const char *hint = "SELECT: New Contact";
    font_getstringsize((const unsigned char *)hint, &w, &ht, sf);
    d->putsxy((content.width - w) / 2, content.height / 2 + 10,
              (const unsigned char *)hint);

    d->set_viewport(last);
    d->update();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
}

/* ---- Main Contacts screen loop ---- */

static int selection;

static int show_list(struct contacts *cs, struct page *p,
                     struct vcard_contact *vc)
{
    struct gui_synclist list;
    int action, ret = 0;
    int head_font = ui_theme_current_font_load(UI_FONT_TITLE,
                                               global_settings.glyphs_to_cache);

    for (bool open = true; open; )
    {
        sb_set_persistent_title(str(LANG_FRUIT_CONTACTS), Icon_NOICON,
                                SCREEN_MAIN);
        if (cs->count == 0)
        {
            paint_empty_contacts(head_font, sub_font);
            action = get_action(CONTEXT_LIST, HZ);
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            {
                open = false;
                break;
            }
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = GO_TO_ROOT;
                open = false;
                break;
            }
            if (action == ACTION_STD_OK || action == ACTION_STD_CONTEXT)
            {
                vcard_init_new(vc);
                int er = run_contact_editor(vc);
                if (er == GO_TO_ROOT) { ret = GO_TO_ROOT; open = false; break; }
                if (er > 0) scan_and_sort(cs);
            }
            continue;
        }

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
            extras_list_do_action(HZ, &list, &action);
            selection = gui_synclist_get_sel_pos(&list);
            if (action == ACTION_STD_OK)
            {
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                const struct contact *c = &cs->c[cs->order[selection]];
                char path[MAX_PATH];
                contact_path(cs, c, path, sizeof(path));
                if (vcard_load(path, c->offset, vc))
                {
                    int pr = page_show(vc, p);
                    if (pr == GO_TO_ROOT)
                    {
                        ret = GO_TO_ROOT;
                        open = false;
                    }
                    else if (pr > 0)
                        scan_and_sort(cs);
                }
                break;
            }
            if (action == ACTION_STD_CONTEXT)
            {
                static const char *const opts[3] = {
                    "New Contact", "Edit Contact", "Delete Contact"
                };
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                int ch = c_show_choice(str(LANG_FRUIT_CONTACTS), opts, 3, 0);
                if (ch == -2) { ret = GO_TO_ROOT; open = false; break; }
                if (ch == 0)
                {
                    vcard_init_new(vc);
                    int er = run_contact_editor(vc);
                    if (er == GO_TO_ROOT) { ret = GO_TO_ROOT; open = false; }
                    else if (er > 0) scan_and_sort(cs);
                }
                else if (ch == 1)
                {
                    const struct contact *c = &cs->c[cs->order[selection]];
                    char path[MAX_PATH];
                    contact_path(cs, c, path, sizeof(path));
                    if (vcard_load(path, c->offset, vc))
                    {
                        int er = run_contact_editor(vc);
                        if (er == GO_TO_ROOT) { ret = GO_TO_ROOT; open = false; }
                        else if (er > 0) scan_and_sort(cs);
                    }
                }
                else if (ch == 2)
                {
                    int cf = c_confirm("Delete this contact?",
                                       LANG_DELETE, LANG_DIAG_CANCEL);
                    if (cf < 0) { ret = GO_TO_ROOT; open = false; }
                    else if (cf == 1)
                    {
                        const struct contact *c = &cs->c[cs->order[selection]];
                        char path[MAX_PATH];
                        contact_path(cs, c, path, sizeof(path));
                        if (vcard_delete(path, c->offset, c->end))
                            scan_and_sort(cs);
                    }
                }
                break;
            }
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU ||
                default_event_handler(action) == SYS_USB_CONNECTED)
            {
                if (action != ACTION_STD_CANCEL && action != ACTION_STD_MENU)
                    ret = GO_TO_ROOT;
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                open = false;
                break;
            }
        }
    }

    if (head_font >= 0)
        font_unload(head_font);
    return ret;
}

int extras_contacts(void)
{
    size_t size = sizeof(struct contacts) + sizeof(struct page) +
                  sizeof(struct vcard_contact);
    int handle = core_alloc_free_space(size);
    int ret = 0;

    if (handle <= 0)
        return 0;
    struct contacts *cs = core_get_data_pinned(handle);
    struct page *p = (struct page *)(cs + 1);
    struct vcard_contact *vc = (struct vcard_contact *)(p + 1);

    scan_and_sort(cs);
    push_current_activity(ACTIVITY_TEXTVIEWER);
    sub_font = ui_theme_current_font_load(UI_FONT_BODY,
                                          global_settings.glyphs_to_cache);
    ret = show_list(cs, p, vc);
    if (sub_font >= 0)
        font_unload(sub_font);
    sub_font = -1;
    pop_current_activity();

    core_put_data_pinned(cs);
    core_free(handle);
    return ret;
}
