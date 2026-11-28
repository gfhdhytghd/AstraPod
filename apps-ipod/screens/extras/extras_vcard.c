/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * vCard 2.1 / 3.0 / 4.0 scanner, loader, and lossless atomic editor for
 * /Contacts .vcf files and /Contacts/AstraPod/<UID>.vcf.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extras_vcard.h"

#ifdef EXTRAS_HOST_TEST
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t vc_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size)
    {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#define strlcpy vc_strlcpy
#else
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "dir.h"
#include "file.h"
#endif

#define VC_LINE_BUF 1024

enum {
    F_HOME = 1, F_WORK = 2, F_CELL = 4, F_FAX = 8, F_PAGER = 16,
    F_MAIN = 32, F_QP = 64, F_LATIN1 = 128, F_SKIP = 256, F_CP1252 = 512,
};

/* Split vCard property in place: strip optional group prefix ("item1."),
 * return parameter flags. */
static int split_vc_prop(char *line, char **name, char **value)
{
    static const struct { const char *word; int flag; } words[] = {
        { "HOME", F_HOME }, { "WORK", F_WORK }, { "CELL", F_CELL },
        { "IPHONE", F_CELL }, { "MOBILE", F_CELL }, { "FAX", F_FAX },
        { "PAGER", F_PAGER }, { "MAIN", F_MAIN },
        { "QUOTED-PRINTABLE", F_QP }, { "ISO-8859-1", F_LATIN1 },
        { "WINDOWS-1252", F_CP1252 }, { "BASE64", F_SKIP }, { "B", F_SKIP },
    };
    char *colon = extras_param_separator(line, ':');
    int flags = 0;

    *value = colon ? colon + 1 : line + strlen(line);
    if (colon)
        *colon = '\0';
    char *dot = strchr(line, '.');
    char *semi = strchr(line, ';');
    *name = (dot && (!semi || dot < semi)) ? dot + 1 : line;
    if (!semi)
        return 0;
    *semi = '\0';

    for (char *p = semi + 1; *p; )
    {
        size_t len = strcspn(p, ";,=\"");
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

static int hex_val(int c)
{
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void decode_vc_val(char *value, int flags, size_t buf_size)
{
    char *w = value;
    for (char *r = value; *r; )
    {
        if ((flags & F_QP) && r[0] == '=' && hex_val(r[1]) >= 0 &&
            hex_val(r[2]) >= 0)
        {
            *w++ = (char)(hex_val(r[1]) * 16 + hex_val(r[2]));
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';

    if (flags & (F_LATIN1 | F_CP1252))
    {
        static const uint16_t cp1252[] = {
            0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021,
            0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0x8d, 0x17d, 0x8f,
            0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
            0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178,
        };
        size_t input = 0, output = 0;
        while (value[input])
        {
            unsigned c = (unsigned char)value[input];
            if ((flags & F_CP1252) && c >= 0x80 && c <= 0x9f)
                c = cp1252[c - 0x80];
            size_t width = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            if (output + width >= buf_size)
                break;
            input++;
            output += width;
        }
        value[output] = '\0';
        while (input)
        {
            unsigned c = (unsigned char)value[--input];
            if ((flags & F_CP1252) && c >= 0x80 && c <= 0x9f)
                c = cp1252[c - 0x80];
            if (c < 0x80)
                value[--output] = c;
            else
            {
                value[--output] = 0x80 | (c & 0x3f);
                if (c < 0x800)
                    value[--output] = 0xc0 | (c >> 6);
                else
                {
                    value[--output] = 0x80 | ((c >> 6) & 0x3f);
                    value[--output] = 0xe0 | (c >> 12);
                }
            }
        }
    }
    w = value;
    for (char *r = value; *r; r++)
    {
        if (*r != '\r')
            *w++ = *r;
        else if (r[1] != '\n')
            *w++ = '\n';
    }
    *w = '\0';
}

bool vcard_read_text(char *line, size_t capacity, char **name, char **value)
{
    int flags = split_vc_prop(line, name, value);
    if (flags & F_SKIP)
        return false;
    decode_vc_val(*value, flags, (size_t)(line + capacity - *value));
    return true;
}

void vcard_split_parts(char *value, char **part, int max)
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
        extras_unescape_text(part[i]);
    while (n < max)
        part[n++] = "";
}

static enum vcard_phone_type flags_to_phone_type(int flags)
{
    if (flags & F_CELL) return VCARD_TEL_MOBILE;
    if (flags & F_FAX) return VCARD_TEL_FAX;
    if (flags & F_PAGER) return VCARD_TEL_PAGER;
    if (flags & F_MAIN) return VCARD_TEL_MAIN;
    if (flags & F_HOME) return VCARD_TEL_HOME;
    if (flags & F_WORK) return VCARD_TEL_WORK;
    return VCARD_TEL_OTHER;
}

static const char *phone_type_str(enum vcard_phone_type t, bool v4)
{
    static const char *const up[VCARD_TEL_TYPE_COUNT] = {
        "CELL", "HOME", "WORK", "MAIN", "FAX", "PAGER", "OTHER"
    };
    static const char *const lo[VCARD_TEL_TYPE_COUNT] = {
        "cell", "home", "work", "main", "fax", "pager", "other"
    };
    if ((unsigned)t >= VCARD_TEL_TYPE_COUNT)
        t = VCARD_TEL_OTHER;
    return v4 ? lo[t] : up[t];
}

static const char *kind_type_str(enum vcard_kind_type t, bool v4)
{
    static const char *const up[VCARD_KIND_TYPE_COUNT] = {
        "HOME", "WORK", "OTHER"
    };
    static const char *const lo[VCARD_KIND_TYPE_COUNT] = {
        "home", "work", "other"
    };
    if ((unsigned)t >= VCARD_KIND_TYPE_COUNT)
        t = VCARD_KIND_OTHER;
    return v4 ? lo[t] : up[t];
}

static int parse_n_digits(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static bool parse_bday(const char *val, struct extras_date *d)
{
    while (*val == ' ' || *val == '\t')
        val++;
    size_t len = strlen(val);
    if (len != 8 && len != 10)
        return false;
    int y = parse_n_digits(val, 4);
    if (y < 1)
        return false;
    int m, day;
    if (len == 10)
    {
        m = parse_n_digits(val + 5, 2);
        if (val[4] != '-' || val[7] != '-')
            return false;
        day = parse_n_digits(val + 8, 2);
    }
    else
    {
        m = parse_n_digits(val + 4, 2);
        day = parse_n_digits(val + 6, 2);
    }
    if (m < 1 || m > 12 || day < 1 || day > extras_month_days(y, m))
        return false;
    d->year = y;
    d->month = m;
    d->day = day;
    return true;
}

/* ---- Directory scan helper ---- */

static bool dir_has_vcf(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir)
        return false;
    bool found = false;
    while (!found)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        const char *name = (const char *)entry->d_name;
        if (name[0] == '.')
            continue;
        bool is_dir;
#ifdef EXTRAS_HOST_TEST
        char sub[MAX_PATH];
        struct stat st;
        snprintf(sub, sizeof(sub), "%s/%s", path, name);
        is_dir = (stat(sub, &st) == 0 && S_ISDIR(st.st_mode));
#else
        is_dir = (dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) != 0;
#endif
        if (is_dir)
        {
            if (depth == 0)
            {
                char subp[MAX_PATH];
                snprintf(subp, sizeof(subp), "%s/%s", path, name);
                found = dir_has_vcf(subp, 1);
            }
        }
        else if (extras_has_ext(name, ".vcf"))
            found = true;
    }
    closedir(dir);
    return found;
}

bool vcard_any_in(const char *dir)
{
    return dir_has_vcf(dir ? dir : VCARD_DIR, 0);
}

/* ---- Init / Load / Format ---- */

void vcard_init_new(struct vcard_contact *c)
{
    memset(c, 0, sizeof(*c));
    c->version = VCARD_VER_30;
    c->bday = (struct extras_date){ 1990, 1, 1 };
}

void vcard_format_addr(const struct vcard_addr *a, char *out, size_t size)
{
    size_t n = 0;
    if (size)
        out[0] = '\0';
#define ADD_LINE(...) do { \
        if (n < size) { \
            if (n > 0) n += (size_t)snprintf(out + n, size - n, "\n"); \
            if (n < size) n += (size_t)snprintf(out + n, size - n, __VA_ARGS__); \
        } \
    } while (0)
    if (a->pobox[0]) ADD_LINE("%s", a->pobox);
    if (a->ext[0]) ADD_LINE("%s", a->ext);
    if (a->street[0]) ADD_LINE("%s", a->street);
    if (a->region[0])
        ADD_LINE("%s%s%s%s%s", a->city, a->city[0] ? ", " : "",
                 a->region, a->postal[0] ? " " : "", a->postal);
    else
    {
        if (a->city[0]) ADD_LINE("%s", a->city);
        if (a->postal[0]) ADD_LINE("%s", a->postal);
    }
    if (a->country[0]) ADD_LINE("%s", a->country);
#undef ADD_LINE
}

static void parse_addr(char *val, int flags, struct vcard_addr *a)
{
    char *part[7];
    vcard_split_parts(val, part, 7);
    a->type = (flags & F_HOME) ? VCARD_KIND_HOME :
              (flags & F_WORK) ? VCARD_KIND_WORK :
                                 VCARD_KIND_OTHER;
    strlcpy(a->pobox, extras_trim(part[0]), sizeof(a->pobox));
    strlcpy(a->ext, extras_trim(part[1]), sizeof(a->ext));
    strlcpy(a->street, extras_trim(part[2]), sizeof(a->street));
    strlcpy(a->city, extras_trim(part[3]), sizeof(a->city));
    strlcpy(a->region, extras_trim(part[4]), sizeof(a->region));
    strlcpy(a->postal, extras_trim(part[5]), sizeof(a->postal));
    strlcpy(a->country, extras_trim(part[6]), sizeof(a->country));
}

bool vcard_load(const char *path, uint32_t card_start, struct vcard_contact *c)
{
    char saved_path[MAX_PATH];
    strlcpy(saved_path, path, sizeof(saved_path));
    memset(c, 0, sizeof(*c));
    strlcpy(c->path, saved_path, sizeof(c->path));
    c->card_start = card_start;
    c->is_existing = true;
    c->version = VCARD_VER_30;
    c->bday = (struct extras_date){ 1990, 1, 1 };

    int fd = open(c->path, O_RDONLY);
    if (fd < 0)
        return false;

    struct extras_reader r;
    extras_reader_init(&r, fd, card_start);
    char line[VC_LINE_BUF];
    uint32_t rs = 0, re = 0;
    bool got_end = false;

    while (extras_read_prop(&r, line, sizeof(line), &rs, &re, true))
    {
        char *prop, *val;
        int flags = split_vc_prop(line, &prop, &val);
        if (flags & F_SKIP)
            continue;
        decode_vc_val(val, flags, (size_t)(line + sizeof(line) - val));

        if (!strcasecmp(prop, "END") && !strcasecmp(extras_trim(val), "VCARD"))
        {
            c->card_end = re;
            got_end = true;
            break;
        }
        if (!strcasecmp(prop, "VERSION"))
        {
            char *v = extras_trim(val);
            if (!strncmp(v, "4.", 2)) c->version = VCARD_VER_40;
            else if (!strncmp(v, "2.", 2)) c->version = VCARD_VER_21;
            else c->version = VCARD_VER_30;
        }
        else if (!strcasecmp(prop, "UID"))
        {
            extras_unescape_text(val);
            strlcpy(c->uid, extras_trim(val), sizeof(c->uid));
        }
        else if (!strcasecmp(prop, "FN"))
        {
            extras_unescape_text(val);
            strlcpy(c->fn, extras_trim(val), sizeof(c->fn));
        }
        else if (!strcasecmp(prop, "N"))
        {
            char *part[5];
            vcard_split_parts(val, part, 5);
            strlcpy(c->last, extras_trim(part[0]), sizeof(c->last));
            strlcpy(c->first, extras_trim(part[1]), sizeof(c->first));
            strlcpy(c->middle, extras_trim(part[2]), sizeof(c->middle));
            strlcpy(c->prefix, extras_trim(part[3]), sizeof(c->prefix));
            strlcpy(c->suffix, extras_trim(part[4]), sizeof(c->suffix));
        }
        else if (!strcasecmp(prop, "ORG"))
        {
            char *part[1];
            vcard_split_parts(val, part, 1);
            strlcpy(c->org, extras_trim(part[0]), sizeof(c->org));
        }
        else if (!strcasecmp(prop, "TEL") && c->phone_count < VCARD_MAX_PHONES)
        {
            extras_unescape_text(val);
            char *v = extras_trim(val);
            if (*v)
            {
                c->source_phones[c->phone_count] = rs;
                struct vcard_phone *p = &c->phones[c->phone_count++];
                p->source_offset = rs;
                p->type = flags_to_phone_type(flags);
                strlcpy(p->value, v, sizeof(p->value));
            }
        }
        else if (!strcasecmp(prop, "EMAIL") &&
                 c->email_count < VCARD_MAX_EMAILS)
        {
            extras_unescape_text(val);
            char *v = extras_trim(val);
            if (*v)
            {
                c->source_emails[c->email_count] = rs;
                struct vcard_email *e = &c->emails[c->email_count++];
                e->source_offset = rs;
                e->type = (flags & F_HOME) ? VCARD_KIND_HOME :
                          (flags & F_WORK) ? VCARD_KIND_WORK :
                                             VCARD_KIND_OTHER;
                strlcpy(e->value, v, sizeof(e->value));
            }
        }
        else if (!strcasecmp(prop, "ADR") && c->addr_count < VCARD_MAX_ADDRS)
        {
            c->source_addrs[c->addr_count] = rs;
            struct vcard_addr *a = &c->addrs[c->addr_count++];
            a->source_offset = rs;
            parse_addr(val, flags, a);
        }
        else if (!strcasecmp(prop, "BDAY"))
        {
            if (parse_bday(val, &c->bday))
                c->has_bday = true;
        }
        else if (!strcasecmp(prop, "NOTE"))
        {
            extras_unescape_text(val);
            strlcpy(c->note, extras_trim(val), sizeof(c->note));
        }
    }
    close(fd);

    strlcpy(c->orig_first, c->first, sizeof(c->orig_first));
    strlcpy(c->orig_last, c->last, sizeof(c->orig_last));
    strlcpy(c->orig_org, c->org, sizeof(c->orig_org));
    strlcpy(c->orig_note, c->note, sizeof(c->orig_note));
    c->orig_has_bday = c->has_bday;
    c->orig_bday = c->bday;
    return got_end;
}

/* ---- Writers ---- */

static void compute_fn(const struct vcard_contact *c, char *out, size_t size)
{
    if (c->first[0] && c->last[0])
        snprintf(out, size, "%s %s", c->first, c->last);
    else if (c->first[0])
        strlcpy(out, c->first, size);
    else if (c->last[0])
        strlcpy(out, c->last, size);
    else if (c->org[0])
        strlcpy(out, c->org, size);
    else if (c->fn[0])
        strlcpy(out, c->fn, size);
    else
        strlcpy(out, "No Name", size);
}

static bool write_n_prop(int fd, const struct vcard_contact *c)
{
    char e0[192], e1[192], e2[128], e3[64], e4[64], line[700];
    extras_escape_text(c->last, e0, sizeof(e0));
    extras_escape_text(c->first, e1, sizeof(e1));
    extras_escape_text(c->middle, e2, sizeof(e2));
    extras_escape_text(c->prefix, e3, sizeof(e3));
    extras_escape_text(c->suffix, e4, sizeof(e4));
    snprintf(line, sizeof(line), "N:%s;%s;%s;%s;%s", e0, e1, e2, e3, e4);
    return extras_write_folded(fd, line);
}

static bool write_phone(int fd, const struct vcard_phone *p, bool v4)
{
    if (!p->value[0])
        return true;
    char prefix[64];
    bool uri = v4 && !strncasecmp(p->value, "tel:", 4);
    snprintf(prefix, sizeof(prefix), "TEL;TYPE=%s%s:",
             phone_type_str(p->type, v4), v4 && !uri ? ";VALUE=text" : "");
    if (uri)
    {
        char line[128];
        snprintf(line, sizeof(line), "%s%s", prefix, p->value);
        return extras_write_folded(fd, line);
    }
    return extras_write_text_prop(fd, prefix, p->value);
}

static bool write_email(int fd, const struct vcard_email *e, bool v4)
{
    if (!e->value[0])
        return true;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "EMAIL;TYPE=%s:",
             kind_type_str(e->type, v4));
    return extras_write_text_prop(fd, prefix, e->value);
}

static bool write_addr(int fd, const struct vcard_addr *a, bool v4)
{
    char e0[96], e1[96], e2[192], e3[128], e4[96], e5[64], e6[128];
    char line[900];
    extras_escape_text(a->pobox, e0, sizeof(e0));
    extras_escape_text(a->ext, e1, sizeof(e1));
    extras_escape_text(a->street, e2, sizeof(e2));
    extras_escape_text(a->city, e3, sizeof(e3));
    extras_escape_text(a->region, e4, sizeof(e4));
    extras_escape_text(a->postal, e5, sizeof(e5));
    extras_escape_text(a->country, e6, sizeof(e6));
    snprintf(line, sizeof(line), "ADR;TYPE=%s:%s;%s;%s;%s;%s;%s;%s",
             kind_type_str(a->type, v4), e0, e1, e2, e3, e4, e5, e6);
    return extras_write_folded(fd, line);
}

static bool write_new_rows(int fd, const struct vcard_contact *c, bool v4)
{
    for (int i = 0; i < c->phone_count; i++)
        if (!c->phones[i].source_offset && !write_phone(fd, &c->phones[i], v4))
            return false;
    for (int i = 0; i < c->email_count; i++)
        if (!c->emails[i].source_offset && !write_email(fd, &c->emails[i], v4))
            return false;
    for (int i = 0; i < c->addr_count; i++)
        if (!c->addrs[i].source_offset && !write_addr(fd, &c->addrs[i], v4))
            return false;
    return true;
}

static bool source_contains(const uint32_t *rows, size_t count, uint32_t offset)
{
    for (size_t i = 0; i < count; i++)
        if (rows[i] == offset)
            return true;
    return false;
}

static bool same_addr(const struct vcard_addr *a, const struct vcard_addr *b)
{
    return a->type == b->type && !strcmp(a->pobox, b->pobox) &&
           !strcmp(a->ext, b->ext) && !strcmp(a->street, b->street) &&
           !strcmp(a->city, b->city) && !strcmp(a->region, b->region) &&
           !strcmp(a->postal, b->postal) && !strcmp(a->country, b->country);
}

static bool write_bday(int fd, const struct vcard_contact *c, bool v4)
{
    if (!c->has_bday)
        return true;
    char line[48];
    if (v4)
        snprintf(line, sizeof(line), "BDAY:%04d%02d%02d",
                 c->bday.year, c->bday.month, c->bday.day);
    else
        snprintf(line, sizeof(line), "BDAY:%04d-%02d-%02d",
                 c->bday.year, c->bday.month, c->bday.day);
    return extras_write_folded(fd, line);
}

bool vcard_save(const char *contacts_dir, struct vcard_contact *c)
{
    char now_stamp[24];
    char fn_buf[160];
    char out_line[192];
    extras_now_utc_stamp(now_stamp, sizeof(now_stamp));
    compute_fn(c, fn_buf, sizeof(fn_buf));

    if (!c->uid[0])
        extras_gen_uid(c->uid, sizeof(c->uid));

    if (!c->is_existing || c->card_end <= c->card_start)
    {
        const char *base = contacts_dir ? contacts_dir : VCARD_DIR;
        snprintf(c->path, sizeof(c->path), "%s/%s/%s.vcf",
                 base, VCARD_SUBDIR, c->uid);

        struct extras_atomic a;
        if (!extras_atomic_open(&a, c->path))
            return false;

        bool ok = extras_write_folded(a.fd, "BEGIN:VCARD") &&
                  extras_write_folded(a.fd, "VERSION:3.0");
        if (ok)
        {
            snprintf(out_line, sizeof(out_line), "UID:%s", c->uid);
            ok = extras_write_folded(a.fd, out_line);
        }
        if (ok) ok = write_n_prop(a.fd, c);
        if (ok) ok = extras_write_text_prop(a.fd, "FN:", fn_buf);
        if (ok && c->org[0]) ok = extras_write_text_prop(a.fd, "ORG:", c->org);
        if (ok) ok = write_new_rows(a.fd, c, false);
        if (ok) ok = write_bday(a.fd, c, false);
        if (ok && c->note[0])
            ok = extras_write_text_prop(a.fd, "NOTE:", c->note);
        if (ok)
        {
            snprintf(out_line, sizeof(out_line), "REV:%s", now_stamp);
            ok = extras_write_folded(a.fd, out_line);
        }
        if (ok) ok = extras_write_folded(a.fd, "END:VCARD");

        if (!ok)
        {
            extras_atomic_abort(&a);
            return false;
        }
        return extras_atomic_commit(&a);
    }

    int src = open(c->path, O_RDONLY);
    if (src < 0)
        return false;
    int copy_fd = open(c->path, O_RDONLY);
    if (copy_fd < 0)
    {
        close(src);
        return false;
    }

    struct extras_atomic a;
    if (!extras_atomic_open(&a, c->path))
    {
        close(src);
        close(copy_fd);
        return false;
    }

    bool v4 = (c->version == VCARD_VER_40);
    bool upgrade = (c->version == VCARD_VER_21);
    bool name_changed = strcmp(c->first, c->orig_first) ||
                        strcmp(c->last, c->orig_last);
    bool note_changed = strcmp(c->note, c->orig_note);
    bool org_changed = strcmp(c->org, c->orig_org) != 0;
    bool bday_changed = c->has_bday != c->orig_has_bday ||
                        (c->has_bday &&
                         extras_date_cmp(&c->bday, &c->orig_bday) != 0);

    bool wrote_ver = false, wrote_uid = false, wrote_rev = false;
    bool wrote_n = false, wrote_fn = false, wrote_org = false;
    bool wrote_bday = false, wrote_note = false;

    bool ok = extras_copy_range(copy_fd, a.fd, 0, c->card_start);

    struct extras_reader r;
    extras_reader_init(&r, src, c->card_start);
    char line[VC_LINE_BUF];
    uint32_t rs = 0, re = 0;
    bool end_seen = false;

    while (ok && r.offset < c->card_end &&
           extras_read_prop(&r, line, sizeof(line), &rs, &re, true))
    {
        char *prop, *val;
        int flags = split_vc_prop(line, &prop, &val);

        if (!strcasecmp(prop, "BEGIN"))
        {
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
            continue;
        }
        if (!strcasecmp(prop, "END") && !strcasecmp(extras_trim(val), "VCARD"))
        {
            if (ok && !wrote_ver)
                ok = extras_write_folded(a.fd, v4 ? "VERSION:4.0" :
                                                    "VERSION:3.0");
            if (ok && !wrote_uid)
            {
                snprintf(out_line, sizeof(out_line), "UID:%s", c->uid);
                ok = extras_write_folded(a.fd, out_line);
            }
            if (ok && !wrote_n) ok = write_n_prop(a.fd, c);
            if (ok && !wrote_fn)
                ok = extras_write_text_prop(a.fd, "FN:", fn_buf);
            if (ok && !wrote_org && c->org[0])
                ok = extras_write_text_prop(a.fd, "ORG:", c->org);
            if (ok) ok = write_new_rows(a.fd, c, v4);
            if (ok && !wrote_bday) ok = write_bday(a.fd, c, v4);
            if (ok && !wrote_note && c->note[0])
                ok = extras_write_text_prop(a.fd, "NOTE:", c->note);
            if (ok && !wrote_rev)
            {
                snprintf(out_line, sizeof(out_line), "REV:%s", now_stamp);
                ok = extras_write_folded(a.fd, out_line);
            }
            if (ok)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            end_seen = re == c->card_end;
            break;
        }

        if (!strcasecmp(prop, "VERSION"))
        {
            if (c->version == VCARD_VER_21)
                ok = extras_write_folded(a.fd, "VERSION:3.0");
            else
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            wrote_ver = true;
        }
        else if (!strcasecmp(prop, "UID"))
        {
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
            wrote_uid = true;
        }
        else if (!strcasecmp(prop, "REV"))
        {
            snprintf(out_line, sizeof(out_line), "REV:%s", now_stamp);
            ok = extras_write_folded(a.fd, out_line);
            wrote_rev = true;
        }
        else if (!strcasecmp(prop, "N"))
        {
            if (!wrote_n)
                ok = name_changed || upgrade ? write_n_prop(a.fd, c) :
                     extras_copy_range(copy_fd, a.fd, rs, re);
            wrote_n = true;
        }
        else if (!strcasecmp(prop, "FN"))
        {
            if (!wrote_fn)
                ok = name_changed || upgrade ||
                     (org_changed && !c->first[0] && !c->last[0]) ?
                     extras_write_text_prop(a.fd, "FN:", fn_buf) :
                     extras_copy_range(copy_fd, a.fd, rs, re);
            wrote_fn = true;
        }
        else if (!strcasecmp(prop, "ORG"))
        {
            if (!wrote_org)
            {
                if (!org_changed && !(upgrade && (flags & (F_QP | F_LATIN1 | F_CP1252))))
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                else if (c->org[0])
                    ok = extras_write_text_prop(a.fd, "ORG:", c->org);
            }
            wrote_org = true;
        }
        else if (!strcasecmp(prop, "TEL") &&
                 source_contains(c->source_phones, VCARD_MAX_PHONES, rs))
        {
            for (int i = 0; i < c->phone_count; i++)
            {
                const struct vcard_phone *p = &c->phones[i];
                if (p->source_offset != rs)
                    continue;
                decode_vc_val(val, flags, (size_t)(line + sizeof(line) - val));
                extras_unescape_text(val);
                char original[sizeof(p->value)];
                strlcpy(original, extras_trim(val), sizeof(original));
                ok = !upgrade && p->type == flags_to_phone_type(flags) &&
                     !strcmp(p->value, original) ?
                     extras_copy_range(copy_fd, a.fd, rs, re) :
                     write_phone(a.fd, p, v4);
                break;
            }
        }
        else if (!strcasecmp(prop, "EMAIL") &&
                 source_contains(c->source_emails, VCARD_MAX_EMAILS, rs))
        {
            for (int i = 0; i < c->email_count; i++)
            {
                const struct vcard_email *e = &c->emails[i];
                if (e->source_offset != rs)
                    continue;
                enum vcard_kind_type type = flags & F_HOME ? VCARD_KIND_HOME :
                        flags & F_WORK ? VCARD_KIND_WORK : VCARD_KIND_OTHER;
                decode_vc_val(val, flags, (size_t)(line + sizeof(line) - val));
                extras_unescape_text(val);
                char original[sizeof(e->value)];
                strlcpy(original, extras_trim(val), sizeof(original));
                ok = !upgrade && e->type == type &&
                     !strcmp(e->value, original) ?
                     extras_copy_range(copy_fd, a.fd, rs, re) :
                     write_email(a.fd, e, v4);
                break;
            }
        }
        else if (!strcasecmp(prop, "ADR") &&
                 source_contains(c->source_addrs, VCARD_MAX_ADDRS, rs))
        {
            for (int i = 0; i < c->addr_count; i++)
            {
                const struct vcard_addr *addr = &c->addrs[i];
                if (addr->source_offset != rs)
                    continue;
                struct vcard_addr original = {0};
                decode_vc_val(val, flags, (size_t)(line + sizeof(line) - val));
                parse_addr(val, flags, &original);
                ok = !upgrade && same_addr(addr, &original) ?
                     extras_copy_range(copy_fd, a.fd, rs, re) :
                     write_addr(a.fd, addr, v4);
                break;
            }
        }
        else if (!strcasecmp(prop, "BDAY"))
        {
            if (!wrote_bday)
            {
                if (!bday_changed)
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                else
                    ok = write_bday(a.fd, c, v4);
            }
            wrote_bday = true;
        }
        else if (!strcasecmp(prop, "NOTE"))
        {
            if (!note_changed && !upgrade)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else if (!wrote_note && c->note[0])
                ok = extras_write_text_prop(a.fd, "NOTE:", c->note);
            wrote_note = true;
        }
        else
        {
            /* Unknown property (PHOTO, X-*, IMPP, URL, etc.): copy verbatim. */
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
        }
    }

    ok = ok && end_seen && extras_copy_to_eof(copy_fd, a.fd, c->card_end);

    close(src);
    close(copy_fd);
    if (!ok)
    {
        extras_atomic_abort(&a);
        return false;
    }
    return extras_atomic_commit(&a);
}

bool vcard_delete(const char *path, uint32_t card_start, uint32_t card_end)
{
    if (card_end <= card_start)
        return false;

    int src = open(path, O_RDONLY);
    if (src < 0)
        return false;

    /* Check if any other VCARD exists outside [card_start, card_end). */
    bool other_card = false;
    struct extras_reader r;
    extras_reader_init(&r, src, 0);
    char line[256];
    uint32_t rs = 0, re = 0;
    while (extras_read_prop(&r, line, sizeof(line), &rs, &re, false))
    {
        if (rs >= card_start && rs < card_end)
            continue;
        char *prop, *val;
        split_vc_prop(line, &prop, &val);
        if (!strcasecmp(prop, "BEGIN") &&
            !strcasecmp(extras_trim(val), "VCARD"))
        {
            other_card = true;
            break;
        }
    }

    if (!other_card)
    {
        close(src);
        return remove(path) == 0;
    }

    struct extras_atomic a;
    if (!extras_atomic_open(&a, path))
    {
        close(src);
        return false;
    }

    bool ok = extras_copy_range(src, a.fd, 0, card_start) &&
              extras_copy_to_eof(src, a.fd, card_end);
    close(src);
    if (!ok)
    {
        extras_atomic_abort(&a);
        return false;
    }
    return extras_atomic_commit(&a);
}
