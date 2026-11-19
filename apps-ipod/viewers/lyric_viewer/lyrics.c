/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lrcplayer.c
 * Synchronised lyrics: finding, loading and the document model.
 *
 * Copyright (C) 2008-2009 Teruaki Kawashima
 * GNU General Public License (version 2+)
 *
 * Reads timed lyrics for a track and holds them as a list of lines, each with
 * a start time and optionally per-word times. No drawing and no input: a
 * viewer asks what line N says and which line belongs at time T.
 *
 * Sources: timed sidecars, plain text sidecars, then embedded lyrics.
 * Remote text enters the same model through lyrics_begin/append/finish_text.
 *
 * Memory: one immovable buffer holds everything. Line and word structs are
 * allocated downwards from its top, line text upwards from its bottom, and
 * they meet in the middle; when they meet, loading simply stops and the
 * lyrics are short. Buffer pointers are handed out to callers, so the block
 * must not move -- hence buflib_ops_locked.
 *
 * UTF-16 lyrics files are skipped. read_line() splits on single bytes and
 * desynchronises on the first line of a UTF-16 file; upstream worked around
 * this by rewriting the user's file as UTF-8, which a viewer should not do.
 *
 * Parts, in order:
 *   - the buffer and its two-ended allocator
 *   - the document model: lines, words, sorting
 *   - finding a lyrics file
 *   - parsing .lrc and .snc
 *   - reading embedded lyrics
 *   - loading, and the public accessors
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "config.h"
#include "system.h"          /* ARRAYLEN */
#include "string-extra.h"    /* strlcpy */
#include "file.h"            /* MAX_PATH, open/read/close/lseek, file_exists */
#include "rbunicode.h"       /* iso_decode_ex, utf16*decode, UTF_8 */
#include "core_alloc.h"      /* core_alloc_ex, buflib_ops_locked */
#include "metadata_common.h" /* metadata and on-demand tag readers */
#include "settings/settings.h"       /* global_settings */
#include "system/strutil.h"          /* read_line, fix_path_part */
#include "lyrics.h"

/* Longest line read from a lyrics file, in bytes before decoding. */
#define MAX_LINE_LEN    256

/* Big enough for a long song with per-word timing; loading stops cleanly if
 * a file needs more. */
#define LYRICS_BUFFER_SIZE  0x8000      /* 32 kiB */
#define TTML_BUFFER_SIZE    0x18000     /* 96 KiB including cue metadata */

/* An ID3 lyrics frame may require de-escaping. Stage a bounded prefix inside
 * the document arena, and keep both document allocators below that prefix. */
#define ID3_LYRICS_MAX   ((LYRICS_BUFFER_SIZE / 3) & ~3u)
#define UTF8_LINE_SIZE   (MAX_LINE_LEN * 3 + 2)

enum lyrics_type
{
    TYPE_NONE = 0,
    TYPE_LRC,       /* .lrc, in the user's codepage unless it has a BOM */
    TYPE_LRC8,      /* .lrc8, always UTF-8 */
    TYPE_SNC,
    TYPE_TTML,     /* local Apple TTML or normalized remote paragraphs */
    TYPE_SYLT,      /* ID3v2 synchronised lyrics frame */
    TYPE_TEXT,      /* plain UTF-8 text, local or remote */
};

struct lyrics_word
{
    long time;                  /* ms, or -1 to start with the line */
    unsigned char *text;
};

struct lyrics_line
{
    long time;                  /* ms, before [offset:] is applied */
    short nword;
    struct lyrics_line *next;
    /* Words are stored last-first, because they are allocated downwards:
     * words[nword-1] is the first word, and its text -- the whole line. */
    struct lyrics_word *words;
};

/* TTML extends the legacy arena entries only for documents that use it. */
struct lyrics_ttml_word
{
    struct lyrics_word base;
    long end;
    short agent;
    bool timed;
};

struct lyrics_ttml_line
{
    struct lyrics_line base;
    struct lyrics_ttml_line *background;
    long end;
    short agent;
    bool timed;
};

/* What one entry of a SYLT frame takes from the top of the buffer. */
#define SYLT_SLACK  (sizeof(struct lyrics_line) \
                     + sizeof(struct lyrics_word) + 8)

static int lyrics_handle = 0;
static int ttml_packet_handle;
static unsigned char *buffer;
static size_t lyrics_capacity = LYRICS_BUFFER_SIZE;
static size_t buf_used;         /* text, growing up from 0 */
static size_t buf_end;          /* structs, growing down from the top */

/* A copy of the track being searched for, taken before anything else happens.
 *
 * audio_current_track() hands back a pointer to the playback engine's live
 * mp3entry, not a copy of one -- and allocating the working buffer below can
 * compact the buflib arena and run playback's shrink callback, which is
 * enough for that entry to be refilled underneath us. Reading id3->path
 * afterwards then finds an empty string, the search builds no candidate at
 * all, and the result is indistinguishable from "this track has no lyrics".
 * Nothing past the copy touches the caller's entry. */
static struct
{
    char path[MAX_PATH];
    char title[MAX_PATH];
    char artist[MAX_PATH];
    unsigned long length;
    unsigned int codectype;
} track;

static struct
{
    char file[MAX_PATH];
    char *title;
    char *artist;
    long offset;                /* ms, from the [offset:] tag */
    long length;                /* track length, ms */
    int nline;
    struct lyrics_line *head, **tail;
    struct lyrics_line **index; /* nline entries, in time order */
    enum lyrics_type type;
    long last_ttml_time;
    unsigned short remote_line_size;
    short voices;
    bool loaded;
} lrc;

/* A remote LRC line can span multiple page rows. */
static unsigned char remote_lrc_line[UTF8_LINE_SIZE];

/* ---------------------------------------------------------------------------
 * The buffer and its two-ended allocator
 * ------------------------------------------------------------------------ */

/* Space left between the two ends, less the room the line index will need
 * once loading finishes. Reserving it as we go means build_index() cannot
 * fail after the lines it has to describe are already parsed. */
static size_t buf_free(void)
{
    size_t reserve = (lrc.nline + 1) * sizeof(struct lyrics_line *);

    if (buf_used + reserve >= buf_end)
        return 0;
    return buf_end - buf_used - reserve;
}

/* Append a string to the text area. With join, it overwrites the previous
 * string's terminator, so the two become one -- that is how the words of a
 * line end up contiguous. Returns where the string landed, NULL if full. */
static unsigned char *bufadd(const unsigned char *str, bool join)
{
    size_t siz = strlen((const char *)str) + 1;
    unsigned char *pos;

    if (siz > buf_free() + join)
        return NULL;
    if (join)
        buf_used--;
    pos = &buffer[buf_used];
    strcpy((char *)pos, (const char *)str);
    buf_used += siz;
    return pos;
}

static void *alloc_buf(size_t siz)
{
    siz = (siz + 3) & ~3;
    if (siz > buf_free())
        return NULL;
    buf_end -= siz;
    return &buffer[buf_end];
}

static void reset_data(void)
{
    if (ttml_packet_handle > 0)
        core_free(ttml_packet_handle);
    ttml_packet_handle = 0;
    memset(&lrc, 0, sizeof(lrc));
    lrc.tail = &lrc.head;
    buf_used = 0;
    buf_end = lyrics_capacity;
}

/* ---------------------------------------------------------------------------
 * The document model
 * ------------------------------------------------------------------------ */

static struct lyrics_word *new_word(long time, const unsigned char *text,
                                    bool join)
{
    struct lyrics_word *word = alloc_buf(sizeof(struct lyrics_word));

    if (word == NULL)
        return NULL;
    if ((word->text = bufadd(text, join)) == NULL)
        return NULL;
    word->time = time;
    return word;
}

/* Append a line, optionally starting its text. */
static bool add_line(struct lyrics_line *line, const unsigned char *text)
{
    line->nword = 0;
    line->next = NULL;
    line->words = NULL;
    if (text)
    {
        if ((line->words = new_word(-1, text, false)) == NULL)
            return false;
        line->nword++;
    }
    *lrc.tail = line;
    lrc.tail = &line->next;
    lrc.nline++;
    return true;
}

/* Put the lines in time order, keeping equal times in file order, and drop
 * any that never got text -- a line with no words would break every
 * accessor. Recounts nline, which is why it must run before build_index(). */
static void sort_lines(void)
{
    struct lyrics_line *p = lrc.head, **q, *next;
    long time_max = 0;

    lrc.head = NULL;
    lrc.tail = &lrc.head;
    lrc.nline = 0;

    while (p != NULL)
    {
        next = p->next;
        if (p->nword)
        {
            /* already the latest? then only the tail needs looking at */
            q = (p->time >= time_max)? lrc.tail: &lrc.head;
            while ((*q) && (*q)->time <= p->time)
                q = &((*q)->next);
            p->next = *q;
            *q = p;
            if (!p->next)
            {
                time_max = p->time;
                lrc.tail = &p->next;
            }
            lrc.nline++;
        }
        p = next;
    }
}

/* Turn the list into an array so lines can be reached by index. Space for it
 * was reserved by buf_free() as the lines were parsed. */
static bool build_index(void)
{
    size_t siz = (lrc.nline * sizeof(struct lyrics_line *) + 3) & ~3;
    struct lyrics_line *line;
    int i = 0;

    if (buf_used + siz > buf_end)
        return false;
    buf_end -= siz;
    lrc.index = (struct lyrics_line **)&buffer[buf_end];
    for (line = lrc.head; line; line = line->next)
        lrc.index[i++] = line;
    return true;
}

static struct lyrics_line *get_line(int index)
{
    if (!lrc.index || index < 0 || index >= lrc.nline)
        return NULL;
    return lrc.index[index];
}

/* [offset:] shifts every timestamp, and is applied on the way out rather than
 * stored: lines written [t1][t2]text share one word list, so baking it in
 * would shift those words twice. */
static long apply_offset(long time)
{
    if (lrc.offset > 0 && time > LONG_MAX - lrc.offset)
        return LONG_MAX;
    time += lrc.offset;
    return (time < 0)? 0: time;
}

/* ---------------------------------------------------------------------------
 * Finding a lyrics file
 *
 * For /aaa/bbb/ccc/ddd.mp3 this looks for ddd.lrc (then .lrc8, then .snc) in
 * /aaa/bbb/ccc/, then /aaa/bbb/, then /aaa/, then /. The track's title is
 * tried as a file name too, when it differs from the audio file's.
 * ------------------------------------------------------------------------ */

static const struct
{
    const char *ext;
    enum lyrics_type type;
} lyrics_ext[] = {
    { ".lrc",  TYPE_LRC  },
    { ".lrc8", TYPE_LRC8 },
    { ".snc",  TYPE_SNC  },
    { ".txt",  TYPE_TEXT },
};

static bool find_lyrics_file(bool plain)
{
    char path_buf[MAX_PATH], fname[MAX_PATH];
    char *names[3] = {NULL, NULL, NULL};
    char *p, *dir;
    unsigned int t, timed = ARRAYLEN(lyrics_ext) - 1;
    int i, len;

    strlcpy(path_buf, track.path, sizeof(path_buf));

    /* All this needs is a directory to search: test for the separator it is
     * about to walk back from, rather than insisting the path starts at the
     * root. A volume-qualified path ("/<HDD0>/Music/...") is normal here, and
     * so is anything else the playlist hands over -- the only thing that
     * cannot work is a bare filename with no directory at all. */
    p = strrchr(path_buf, '/');
    if (p == NULL)
        return false;
    names[0] = p + 1;
    if ((p = strrchr(names[0], '.')) != NULL)
        *p = 0;
    if (track.title[0] && strcmp(names[0], track.title))
    {
        strlcpy(fname, track.title, sizeof(fname));
        fix_path_part(fname, 0, sizeof(fname) - 1);
        names[1] = fname;
    }

    /* walk up the directory chain by truncating path_buf a level at a time.
     * There is no dir_exists() gate: file_exists() on a path under a missing
     * directory is false anyway, so the gate only ever saved a few probes --
     * while being one more thing that has to agree about volume prefixes and
     * trailing separators. albumart.c searches beside a track the same way. */
    dir = path_buf;
    p = names[0] - 1;
    do {
        *p = 0;
        len = snprintf(lrc.file, MAX_PATH, "%s/", dir);
        if (len >= MAX_PATH)
            continue;
        for (t = plain ? timed : 0; t < timed + plain; t++)
        {
            for (i = 0; names[i] != NULL; i++)
            {
                snprintf(&lrc.file[len], MAX_PATH - len, "%s%s",
                         names[i], lyrics_ext[t].ext);
                if (file_exists(lrc.file))
                {
                    lrc.type = lyrics_ext[t].type;
                    return true;
                }
            }
        }
    } while ((p = strrchr(dir, '/')) != NULL);

    lrc.file[0] = 0;
    return false;
}

/* Apple lyric TTML uses absolute media times. The same reader accepts local
 * UTF-8 TTML and the normalized, independently paginated remote paragraphs. */
#define TTML_FILE_MAX (256 * 1024)
#define TTML_PACKET_MAX 8192u
#define TTML_AGENTS 64
#define TTML_PERSON 3
#define TTML_GROUP  0x100
#define TTML_OTHER  0x200
#define TTML_DEPTH 18

struct xml_slice { const char *p; size_t len; };
struct xml_token {
    struct xml_slice name, text;
    const char *attrs;
    bool close, empty, cdata;
};
struct ttml_agent {
    struct xml_slice id;
    short kind, code;
    bool used;
};
struct ttml_reader {
    struct ttml_agent agents[TTML_AGENTS];
    int count, voices;
    unsigned people, groups, others;
    char *ids;
    size_t ids_left;
    bool wire, exhausted;
};
struct ttml_span { long begin, end; short agent; bool bg, timed; };

static bool xml_is(struct xml_slice s, const char *word)
{
    return strlen(word) == s.len && !memcmp(s.p, word, s.len);
}

static bool xml_same(struct xml_slice a, struct xml_slice b)
{
    return a.len == b.len && !memcmp(a.p, b.p, a.len);
}

static const char *xml_space(const char *p)
{
    while (isspace((unsigned char)*p)) p++;
    return p;
}

static struct xml_slice xml_name(const char **cursor)
{
    const char *p = *cursor, *start = p;
    while (*p && (isalnum((unsigned char)*p) || strchr(":._-", *p)))
        p++;
    *cursor = p;
    return (struct xml_slice){start, p - start};
}

static struct xml_slice xml_attr(const struct xml_token *tag, const char *name)
{
    const char *p = tag->attrs;
    if (!p) return (struct xml_slice){0};
    for (;;)
    {
        p = xml_space(p);
        struct xml_slice key = xml_name(&p);
        if (!key.len) break;
        p = xml_space(p);
        if (*p++ != '=') break;
        p = xml_space(p);
        char quote = *p++;
        if (quote != '"' && quote != '\'') break;
        const char *end = strchr(p, quote);
        if (!end) break;
        if (!strncmp(name, "ttm:", 4))
        {
            /* XML prefixes can differ between TTML producers. */
            const char *colon = memchr(key.p, ':', key.len);
            if (colon && (colon - key.p != 5 || memcmp(key.p, "xmlns", 5)))
            {
                key.len -= colon + 1 - key.p;
                key.p = colon + 1;
            }
            if (colon && xml_is(key, name + 4)) return (struct xml_slice){p, end - p};
        }
        else if (xml_is(key, name)) return (struct xml_slice){p, end - p};
        p = end + 1;
    }
    return (struct xml_slice){0};
}

static bool xml_next(const char **cursor, struct xml_token *t)
{
    const char *p = *cursor;
    *t = (struct xml_token){0};
    while (*p)
    {
        const char *end;
        if (*p != '<')
        {
            end = strchr(p, '<');
            if (!end) end = p + strlen(p);
            t->text = (struct xml_slice){p, end - p};
            *cursor = end;
            return true;
        }
        if (!strncmp(p, "<![CDATA[", 9))
        {
            end = strstr(p + 9, "]]>");
            if (!end) return false;
            t->text = (struct xml_slice){p + 9, end - p - 9};
            t->cdata = true;
            *cursor = end + 3;
            return true;
        }
        if (!strncmp(p, "<!--", 4) || p[1] == '?')
        {
            bool comment = p[1] == '!';
            end = strstr(p + (comment ? 4 : 2), comment ? "-->" : "?>");
            if (!end) return false;
            p = end + (comment ? 3 : 2);
            continue;
        }
        p++;
        t->close = *p == '/';
        if (t->close) p++;
        t->name = xml_name(&p);
        if (!t->name.len) return false; /* Also rejects DTDs. */
        for (size_t i = 0; i < t->name.len; i++)
            if (t->name.p[i] == ':')
            {
                t->name.len -= i + 1;
                t->name.p += i + 1;
                break;
            }

        t->attrs = p;
        p = xml_space(p);
        while (*p && *p != '>' && *p != '/')
        {
            if (!xml_name(&p).len) return false;
            p = xml_space(p);
            if (*p++ != '=') return false;
            p = xml_space(p);
            if (*p != '\'' && *p != '"') return false;
            end = strchr(p + 1, *p);
            if (!end) return false;
            p = xml_space(end + 1);
        }
        t->empty = *p == '/';
        if (t->empty) p++;
        if (*p != '>') return false;
        *cursor = p + 1;
        return true;
    }
    *cursor = p;
    return false;
}

static long ttml_number(struct xml_slice s, long limit)
{
    long n = 0;
    if (!s.len) return -1;
    for (size_t i = 0; i < s.len; i++)
    {
        unsigned digit = (unsigned)(s.p[i] - '0');
        if (digit > 9 || n > (limit - (long)digit) / 10) return -1;
        n = 10 * n + digit;
    }
    return n;
}

static long ttml_time(struct xml_slice s)
{
    if (!s.len || s.len > 20) return -1;
    bool ms = s.len > 2 && !memcmp(s.p + s.len - 2, "ms", 2);
    if (ms) s.len -= 2;
    else if (s.p[s.len - 1] == 's') s.len--;
    long whole = 0;
    unsigned clocks = 0;
    const char *p = s.p, *end = p + s.len, *start = p;
    while (p < end && *p != '.')
    {
        if (*p == ':')
        {
            long part = ttml_number((struct xml_slice){start, p - start}, 100000);
            if (ms || part < 0 || clocks++ >= 2 || whole > 100000) return -1;
            whole = whole * 60 + part;
            start = p + 1;
        }
        p++;
    }
    long part = ttml_number((struct xml_slice){start, p - start}, 86400000);
    if (part < 0 || whole > 1440000) return -1;
    whole = whole * 60 + part;
    if (whole > (ms ? 86400000 : 86400)) return -1;
    long fraction = 0, factor = ms ? 0 : 100;
    if (p < end && ++p == end) return -1;
    while (p < end)
    {
        unsigned digit = (unsigned)(*p++ - '0');
        if (digit > 9) return -1;
        fraction += digit * factor;
        factor /= 10;
    }
    return ms ? whole : whole * 1000 + fraction;
}

static struct ttml_agent *ttml_agent(struct ttml_reader *r, struct xml_slice id)
{
    for (int i = 0; i < r->count; i++)
        if (xml_same(id, r->agents[i].id)) return &r->agents[i];
    if (r->count == TTML_AGENTS) return NULL;
    if (r->ids)
    {
        if (id.len > r->ids_left) return NULL;
        memcpy(r->ids, id.p, id.len);
        id.p = r->ids;
        r->ids += id.len;
        r->ids_left -= id.len;
    }
    struct ttml_agent *a = &r->agents[r->count++];
    *a = (struct ttml_agent){.id = id, .kind = TTML_PERSON};
    return a;
}

static short ttml_combine(short a, short b)
{
    if (!a) return b;
    if (!b || a == b) return a;
    return 1;
}

static short ttml_performer(struct ttml_reader *r, const struct xml_token *t,
                            short inherited)
{
    struct xml_slice ids = xml_attr(t, r->wire ? "a" : "ttm:agent");
    if (!ids.p) return inherited;
    if (r->wire)
    {
        long code = ttml_number(ids, TTML_OTHER + TTML_AGENTS - 1);
        return code < 0 ? 0 : code;
    }
    short voice = 0;
    struct ttml_agent *first = NULL;
    for (size_t i = 0; i < ids.len;)
    {
        size_t start = i;
        while (i < ids.len && !isspace((unsigned char)ids.p[i])) i++;
        if (i > start)
        {
            struct ttml_agent *a = ttml_agent(r, (struct xml_slice){ids.p + start, i - start});
            if (a)
            {
                if (!a->used)
                {
                    a->used = true;
                    r->voices++;
                    a->code = a->kind == TTML_PERSON ? TTML_PERSON + r->people++ :
                              a->kind == TTML_GROUP ? TTML_GROUP + r->groups++ :
                                                     TTML_OTHER + r->others++;
                }
                voice = first && first != a ? 1 : ttml_combine(voice, a->code);
                if (!first) first = a;
            }
        }
        if (i < ids.len) i++;
    }
    return voice;
}

static struct ttml_span ttml_span(struct ttml_reader *r, const struct xml_token *t,
                                   struct ttml_span parent)
{
    long begin = ttml_time(xml_attr(t, "begin"));
    long end = ttml_time(xml_attr(t, "end"));
    long dur = ttml_time(xml_attr(t, "dur"));
    bool timed = parent.timed || begin >= 0;
    if (begin < 0) begin = parent.begin;
    if (end < 0 && begin >= 0 && dur >= 0) end = begin + dur;
    if (end < 0 && !timed) end = parent.end;
    return (struct ttml_span){
        .begin = begin, .end = end, .timed = timed,
        .agent = ttml_performer(r, t, parent.agent),
        .bg = parent.bg || (r->wire ? xml_is(xml_attr(t, "bg"), "1") :
                                     xml_is(xml_attr(t, "ttm:role"), "x-bg")),
    };
}

static unsigned long xml_entity(struct xml_slice s)
{
    if (xml_is(s, "amp")) return '&';
    if (xml_is(s, "lt")) return '<';
    if (xml_is(s, "gt")) return '>';
    if (xml_is(s, "quot")) return '"';
    if (xml_is(s, "apos")) return '\'';
    if (!s.len || *s.p++ != '#') return 0xfffd;
    s.len--;
    unsigned base = 10;
    if (s.len && *s.p == 'x') { base = 16; s.p++; s.len--; }
    unsigned long n = 0;
    for (size_t i = 0; i < s.len; i++)
    {
        unsigned char c = s.p[i];
        unsigned digit = c >= '0' && c <= '9' ? c - '0' : tolower(c) - 'a' + 10;
        if (digit >= base) return 0xfffd;
        n = n * base + digit;
        if (n > 0x10ffff) return 0xfffd;
    }
    return !n || (n >= 0xd800 && n <= 0xdfff) ? 0xfffd : n;
}

static bool ttml_word(struct ttml_reader *r, struct lyrics_ttml_line *line, struct xml_slice text,
                      struct ttml_span span, bool cdata)
{
    struct lyrics_line *base = &line->base;
    /* Reserve a complete cue and write decoded text directly into its arena. */
    bool join = base->nword > 0;
    struct lyrics_ttml_word *prior = (void *)base->words;
    bool new = !join || prior->base.time != span.begin ||
               prior->end != span.end || prior->agent != span.agent || prior->timed != span.timed;
    if (buf_free() < sizeof(*prior) + 5) goto full;
    if (join) buf_used--;
    unsigned char *dest = buffer + buf_used;
    size_t initial = buf_used;
    for (size_t i = 0; i < text.len; i++)
    {
        unsigned char c = text.p[i];
        unsigned char bytes[4], *end = bytes + 1;
        bytes[0] = c;
        if (c == '&' && !cdata)
        {
            size_t j = i + 1;
            while (j < text.len && j - i < 12 && text.p[j] != ';') j++;
            if (j < text.len && text.p[j] == ';')
            {
                end = utf8encode(xml_entity((struct xml_slice){text.p + i + 1, j - i - 1}), bytes);
                i = j;
            }
        }
        if (isspace(bytes[0]))
        {
            if ((!join && buf_used == initial) ||
                (buf_used && buffer[buf_used - 1] == ' '))
                continue;
            bytes[0] = ' ';
        }
        if (buf_free() < sizeof(*prior) + (size_t)(end - bytes) + 1) goto full;
        memcpy(buffer + buf_used, bytes, end - bytes);
        buf_used += end - bytes;
    }
    if (buf_used != initial && new)
    {
        struct lyrics_ttml_word *word = alloc_buf(sizeof(*word));
        *word = (struct lyrics_ttml_word){.base = {span.begin, dest},
                                         .end = span.end, .agent = span.agent, .timed = span.timed};
        base->words = &word->base;
        base->nword++;
    }
    if (join || buf_used != initial)
        buffer[buf_used++] = 0;
    for (size_t i = initial; i + 1 < buf_used; i++)
        if (buffer[i] != ' ')
        {
            line->agent = ttml_combine(line->agent, span.agent);
            if (span.begin >= 0 && (base->time < 0 || span.begin < base->time))
                base->time = span.begin;
            if (span.end > line->end) line->end = span.end;
            line->timed |= span.timed;
            break;
        }
    return true;
full:
    r->exhausted = true;
    return false;
}

static void ttml_trim(struct lyrics_ttml_line *line)
{
    if (!line->base.nword) return;
    while (buf_used >= 2 && buffer[buf_used - 2] == ' ')
        buffer[--buf_used - 1] = 0;
    struct lyrics_ttml_word *word = (void *)line->base.words;
    if (!*word->base.text)
    {
        buf_end += sizeof(*word);
        line->base.words = &(word + 1)->base;
        line->base.nword--;
    }
}

/* Read each voice in its own pass so both text and cues stay contiguous. */
static bool ttml_voice(struct ttml_reader *r, const char **p, struct ttml_span root,
                       struct lyrics_ttml_line *line, bool background, bool *has_bg)
{
    struct ttml_span spans[TTML_DEPTH] = {root};
    struct xml_slice names[TTML_DEPTH] = {{"p", 1}};
    int depth = 0;
    struct xml_token t;
    while (xml_next(p, &t))
    {
        struct ttml_span span = spans[depth];
        if (t.close)
        {
            if (!xml_same(names[depth], t.name)) return false;
            if (!depth) return true;
            depth--;
        }
        else if (t.name.len)
        {
            if (xml_is(t.name, "br"))
            {
                if (span.bg == background &&
                    !ttml_word(r, line, (struct xml_slice){" ", 1}, span, false)) return false;
            }
            if (!t.empty)
            {
                if (++depth == TTML_DEPTH) return false;
                spans[depth] = ttml_span(r, &t, span);
                names[depth] = t.name;
                *has_bg |= spans[depth].bg;
            }
        }
        else if (span.bg == background && !ttml_word(r, line, t.text, span, t.cdata))
            return false;
    }
    return false;
}

static bool ttml_paragraph(struct ttml_reader *r, const char **p, struct ttml_span span)
{
    size_t used = buf_used, end = buf_end;
    const char *body = *p;
    struct lyrics_ttml_line *line = alloc_buf(sizeof(*line));
    if (!line)
    {
        r->exhausted = true;
        return false;
    }
    *line = (struct lyrics_ttml_line){.base.time = span.begin, .end = span.end, .agent = span.agent};
    bool bg = false;
    span.timed = false; /* Paragraph timing alone does not start a word sweep. */
    if (!ttml_voice(r, p, span, line, false, &bg)) goto fail;
    ttml_trim(line);
    if (!line->base.nword)
    {
        buf_used = used;
        buf_end = end;
        return true;
    }
    if (bg)
    {
        struct lyrics_ttml_line *back = alloc_buf(sizeof(*back));
        if (!back)
        {
            r->exhausted = true;
            goto fail;
        }
        *back = (struct lyrics_ttml_line){.base.time = -1, .end = -1};
        size_t back_used = buf_used;
        if (!ttml_voice(r, &body, span, back, true, &bg)) goto fail;
        ttml_trim(back);
        if (back->base.nword) line->background = back;
        else
        {
            buf_end += sizeof(*back);
            buf_used = back_used;
        }
    }
    if (line->base.time < 0) line->base.time = lrc.tail == &lrc.head ? 0 : lrc.last_ttml_time;
    lrc.last_ttml_time = line->base.time;
    *lrc.tail = &line->base;
    lrc.tail = &line->base.next;
    lrc.nline++;
    return true;
fail:
    buf_used = used;
    buf_end = end;
    return false;
}

struct ttml_document {
    struct ttml_reader reader;
    struct xml_slice names[TTML_DEPTH];
    short agents[TTML_DEPTH];
    int depth;
};

#define TTML_SCOPE_NAME 40

static bool ttml_document_tag(struct ttml_document *doc, const struct xml_token *t,
                              char (*saved)[TTML_SCOPE_NAME])
{
    struct ttml_reader *r = &doc->reader;
    int depth = doc->depth;
    if (t->close)
    {
        if (!depth || !xml_same(doc->names[depth - 1], t->name)) return false;
        doc->depth--;
        return true;
    }

    if (!r->wire && xml_is(t->name, "agent"))
    {
        struct xml_slice id = xml_attr(t, "xml:id"), kind = xml_attr(t, "type");
        struct ttml_agent *a = id.len ? ttml_agent(r, id) : NULL;
        if (a)
            a->kind = xml_is(kind, "group") ? TTML_GROUP :
                      !kind.len || xml_is(kind, "person") ? TTML_PERSON : TTML_OTHER;
    }
    if (t->name.len && !t->empty)
    {
        if (depth + 1 == TTML_DEPTH) return false;
        struct xml_slice name = t->name;
        if (saved)
        {
            if (name.len > sizeof(*saved)) return false;
            memcpy(saved[depth], name.p, name.len);
            name.p = saved[depth];
        }
        doc->names[depth] = name;
        doc->agents[depth + 1] = ttml_performer(r, t, doc->agents[depth]);
        doc->depth++;
    }
    return true;
}

static struct ttml_span ttml_paragraph_span(struct ttml_document *doc,
                                            const struct xml_token *t)
{
    struct ttml_span parent = {.begin = -1, .end = -1, .agent = doc->agents[doc->depth]};
    return ttml_span(&doc->reader, t, parent);
}

static bool parse_ttml(const char *p, bool wire)
{
    struct ttml_document doc = {.reader.wire = wire};
    struct xml_token t;
    lrc.type = TYPE_TTML;
    while (xml_next(&p, &t))
    {
        if (!t.close && !t.empty && xml_is(t.name, "p"))
        {
            struct ttml_span span = ttml_paragraph_span(&doc, &t);
            if (!ttml_paragraph(&doc.reader, &p, span)) return false;
        }
        else if (!ttml_document_tag(&doc, &t, NULL))
            return false;
    }
    if (!wire) lrc.voices = doc.reader.voices;
    return !*p && !doc.depth;
}

static void ttml_expand(void)
{
    if (lyrics_capacity == TTML_BUFFER_SIZE) return;
    int h = core_alloc_ex(TTML_BUFFER_SIZE, &buflib_ops_locked);
    if (h <= 0) return;
    core_free(lyrics_handle);
    lyrics_handle = h;
    buffer = core_get_data(h);
    buf_end = lyrics_capacity = TTML_BUFFER_SIZE;
}

static void ttml_release_extra(void)
{
    if (lyrics_capacity != TTML_BUFFER_SIZE) return;
    core_free(lyrics_handle);
    lyrics_handle = core_alloc_ex(LYRICS_BUFFER_SIZE, &buflib_ops_locked);
    buffer = lyrics_handle > 0 ? core_get_data(lyrics_handle) : NULL;
    lyrics_capacity = LYRICS_BUFFER_SIZE;
}

static bool load_ttml_file(void)
{
    int fd = open(lrc.file, O_RDONLY);
    if (fd < 0) return false;
    off_t size = ffilesize(fd);
    int h = size > 0 && size <= TTML_FILE_MAX ?
            core_alloc_ex(size + 1, &buflib_ops_locked) : -1;
    bool ok = false;
    if (h > 0)
    {
        char *xml = core_get_data(h);
        if (read(fd, xml, size) == size)
        {
            xml[size] = 0;
            ttml_expand();
            ok = !memchr(xml, 0, size) && parse_ttml(xml, false) && lrc.nline;
        }
        core_free(h);
    }
    close(fd);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Parsing .lrc and .snc
 * ------------------------------------------------------------------------ */

static char *parse_int(char *ptr, int *val)
{
    *val = 0;
    while (isdigit((unsigned char)*ptr))
    {
        if (*val >= 1000)
            break;
        *val = *val * 10 + (*ptr++ - '0');
    }
    return ptr;
}

/* Read the contents of one [...] or <...> tag. Returns its time in ms, -1 for
 * an id tag that was consumed, or -10 for anything unrecognised.
 * Time tags are [mm:ss], [mm:ss.xx] or [mm:ss.xxx]; id tags are only looked
 * for at the head of a line, which is where the format puts them. */
static long get_time_value(char *tag, bool read_id_tags)
{
    long time;
    char *ptr;
    int val;

    if (read_id_tags)
    {
        /* keep title and artist only when they say something the track's own
         * metadata does not */
        if (!strncmp(tag, "ti:", 3))
        {
            if (!track.title[0] || strcmp(&tag[3], track.title))
                lrc.title = (char *)bufadd((unsigned char *)&tag[3], false);
            return -1;
        }
        if (!strncmp(tag, "ar:", 3))
        {
            if (!track.artist[0] || strcmp(&tag[3], track.artist))
                lrc.artist = (char *)bufadd((unsigned char *)&tag[3], false);
            return -1;
        }
        if (!strncmp(tag, "offset:", 7))
        {
            lrc.offset = strtol(&tag[7], NULL, 10);
            return -1;
        }
    }

    /* minute */
    ptr = parse_int(tag, &val);
    if (ptr - tag < 1 || ptr - tag > 2 || *ptr != ':')
        return -10;
    time = val * 60000;
    /* second */
    tag = ptr + 1;
    ptr = parse_int(tag, &val);
    if (ptr - tag != 2 || (*ptr != '.' && *ptr != ':' && *ptr != '\0'))
        return -10;
    time += val * 1000;

    if (*ptr != '\0')
    {
        /* fraction: two digits are hundredths, three are milliseconds */
        tag = ptr + 1;
        ptr = parse_int(tag, &val);
        if (ptr - tag < 2 || ptr - tag > 3 || *ptr != '\0')
            return -10;
        time += ((ptr - tag) == 3)? val: val * 10;
    }

    return time;
}

/* One line of a .lrc file:
 *   [time]text
 *   [time]...[time]text          the same text at several times
 *   [time]<time>word<time>word   per-word timing
 */
static bool parse_lrc_line(char *line)
{
    struct lyrics_line *lyr = NULL, *first = NULL;
    struct lyrics_word *word;
    long time, word_time;
    char *str, *tagstart, *tagend;
    int nword = 0;

    /* the [time] tags at the head of the line */
    str = line;
    while (1)
    {
        if (*str != '[')
            break;
        tagend = strchr(str, ']');
        if (tagend == NULL)
            break;
        *tagend = 0;
        time = get_time_value(str + 1, !lyr);
        *tagend++ = ']';
        if (time < 0)
            break;
        lyr = alloc_buf(sizeof(struct lyrics_line));
        if (lyr == NULL)
            return false;
        if (!first)
            first = lyr;
        lyr->time = (time / 10) * 10;
        if (!add_line(lyr, NULL))
            return false;
        str = tagend;
    }
    if (!first)
        return true;    /* an id tag line, a comment, or junk */

    /* Start the line's text as an empty string; each word joins onto it, so
     * the words end up as one run and the first word's text is the line. */
    if (bufadd((const unsigned char *)"", false) == NULL)
        return false;

    /* the <time> tags within the line */
    word_time = -1;
    tagstart = str;
    while (*tagstart)
    {
        tagstart = strchr(tagstart, '<');
        if (!tagstart)
            break;
        tagend = strchr(tagstart, '>');
        if (!tagend)
            break;
        *tagend = 0;
        time = get_time_value(tagstart + 1, false);
        *tagend++ = '>';
        if (time < 0)
        {
            tagstart++;     /* not a time tag; leave it in the text */
            continue;
        }
        *tagstart = 0;
        if (*str || word_time != -1)
        {
            if (new_word(word_time, (unsigned char *)str, true) == NULL)
                return false;
            nword++;
        }
        tagstart = str = tagend;
        word_time = time;
    }
    if ((word = new_word(word_time, (unsigned char *)str, true)) == NULL)
        return false;
    nword++;

    /* [t1][t2]text made several lines that all read the same: point them at
     * the one word list. Word times then belong to the first of them. */
    for (lyr = first; lyr; lyr = lyr->next)
    {
        lyr->nword = nword;
        lyr->words = word;
    }
    return true;
}

/* One line of a .snc file. A time tag sits on a line of its own (possibly
 * with text after it) and the lines that follow belong to it:
 *   \xa2\xe2hhmmssxx\xa2\xd0
 *   line 1
 *   line 2
 */
#define SNC_TAG_START "\xa2\xe2"
#define SNC_TAG_END   "\xa2\xd0"

static bool parse_snc_line(char *line, unsigned char *raw_line, int line_encoding)
{
    /* The tag is raw bytes that iso_decode_ex() may have rewritten, so it is
     * matched against raw_line rather than the decoded copy. */
    if (strlen((char *)raw_line) >= 12
        && !memcmp(raw_line, SNC_TAG_START, 2)
        && !memcmp(raw_line + 10, SNC_TAG_END, 2))
    {
        const unsigned char *pos = raw_line + 2;
        struct lyrics_line *lyr;
        unsigned char *end;
        int hh, mm, ss, xx;

        hh = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        mm = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        ss = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        xx = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        pos += 2;   /* SNC_TAG_END */

        lyr = alloc_buf(sizeof(struct lyrics_line));
        if (lyr == NULL)
            return false;
        lyr->time = hh*3600000 + mm*60000 + ss*1000 + xx*10;
        if (!add_line(lyr, (const unsigned char *)""))
            return false;
        if (pos[0] == 0)
            return true;

        /* text after the tag is the first line of its lyric */
        end = iso_decode_ex(pos, (unsigned char *)line, line_encoding,
                            strlen((const char *)pos) + 1,
                            UTF8_LINE_SIZE - 2);
        *end = 0;
    }
    if (lrc.head)
    {
        strcat(line, "\n");
        if (bufadd((unsigned char *)line, true) == NULL)
            return false;
    }
    return true;
}

static void load_lyrics_file(void)
{
    unsigned char header[3], raw_line[MAX_LINE_LEN], utf8_line[UTF8_LINE_SIZE];
    int line_encoding = global_settings.default_codepage;
    unsigned char *end;
    int fd, got;

    if (lrc.type == TYPE_LRC8 || lrc.type == TYPE_TEXT)
        line_encoding = UTF_8;

    fd = open(lrc.file, O_RDONLY);
    if (fd < 0)
        return;

    got = read(fd, header, sizeof(header));
    if (got == (int)sizeof(header) && !memcmp(header, "\xef\xbb\xbf", 3))
        line_encoding = UTF_8;
    else if (got >= 2 && (!memcmp(header, "\xff\xfe", 2)
                          || !memcmp(header, "\xfe\xff", 2)))
    {
        close(fd);      /* UTF-16 -- see the note at the top of the file */
        return;
    }
    else
        lseek(fd, 0, SEEK_SET);

    while (read_line(fd, (char *)raw_line, MAX_LINE_LEN) > 0)
    {
        end = iso_decode_ex(raw_line, utf8_line, line_encoding,
                            strlen((const char *)raw_line) + 1,
                            UTF8_LINE_SIZE - 2);
        *end = 0;
        bool ok;
        if (lrc.type == TYPE_SNC)
            ok = parse_snc_line((char *)utf8_line, raw_line, line_encoding);
        else if (lrc.type == TYPE_TEXT)
            ok = lyrics_append_text((char *)utf8_line);
        else
            ok = parse_lrc_line((char *)utf8_line);
        if (!ok)
            break;
    }
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Reading embedded lyrics
 * ------------------------------------------------------------------------ */

enum tag_encoding { TAG_LATIN1, TAG_UTF16_LE, TAG_UTF16_BE, TAG_UTF8 };

static int tag_unit(const unsigned char *p, enum tag_encoding enc)
{
    if (enc == TAG_UTF16_LE)
        return p[0] | (p[1] << 8);
    if (enc == TAG_UTF16_BE)
        return (p[0] << 8) | p[1];
    return *p;
}

static int tag_unit_size(enum tag_encoding enc)
{
    return enc == TAG_UTF16_LE || enc == TAG_UTF16_BE ? 2 : 1;
}

static int skip_bom(const unsigned char *p, int len, enum tag_encoding *enc)
{
    if (*enc == TAG_UTF8)
        return len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3) ? 3 : 0;
    if (len >= 2 && tag_unit_size(*enc) == 2)
    {
        int bom = tag_unit(p, TAG_UTF16_BE);
        if (bom == 0xfffe || bom == 0xfeff)
        {
            *enc = bom == 0xfffe ? TAG_UTF16_LE : TAG_UTF16_BE;
            return 2;
        }
    }
    return 0;
}

static void decode_tag_line(const unsigned char *line, int len,
                            enum tag_encoding enc, bool shortened,
                            unsigned char *text)
{
    int unit = tag_unit_size(enc);
    if (len >= unit && tag_unit(line + len - unit, enc) == '\r')
        len -= unit;

    unsigned char *end = text;
    if (unit == 2)
    {
        /* The firmware decoder expects a complete surrogate pair. */
        for (int i = 0; i + 1 < len; i += 2)
        {
            unsigned int c = tag_unit(line + i, enc);
            if (c >= 0xd800 && c <= 0xdbff && i + 3 < len)
            {
                unsigned int low = tag_unit(line + i + 2, enc);
                if (low >= 0xdc00 && low <= 0xdfff)
                {
                    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                    i += 2;
                }
            }
            end = utf8encode(c >= 0xd800 && c <= 0xdfff ? 0xfffd : c, end);
        }
    }
    else
    {
        if (enc == TAG_UTF8 && shortened)
        {
            /* Drop any last codepoint whose bytes did not fit. */
            while (len > 0 && (line[len - 1] & 0xc0) == 0x80)
                len--;
            if (len > 0 && line[len - 1] >= 0xc0)
                len--;
        }
        end = iso_decode_ex(line, text, enc == TAG_UTF8 ? UTF_8 : ISO_8859_1,
                            len, UTF8_LINE_SIZE - 1);
    }
    *end = 0;
}

/* Native MP4 and Vorbis data stream in batches. ID3 supplies a memory view
 * of the decoded frame. Both paths feed the existing document parsers. */
static void read_tag_lines(struct ogg_file *file, const unsigned char *data,
                           long size, enum tag_encoding enc, bool timed,
                           bool clipped)
{
    unsigned char chunk[256], line[MAX_LINE_LEN], text[UTF8_LINE_SIZE];
    int used = 0, unit = tag_unit_size(enc);
    bool first = true, shortened = false;

    clipped |= size > LYRICS_BUFFER_SIZE;
    size = MIN(size, LYRICS_BUFFER_SIZE);
    while (size >= unit)
    {
        int n = MIN(size, (long)sizeof(chunk));
        const unsigned char *p = data;
        if (data)
            data += n;
        else
        {
            n = ogg_file_read(file, chunk, n);
            if (n <= 0)
                break;
            p = chunk;
        }
        size -= n;

        if (first)
        {
            int bom = skip_bom(p, n, &enc);
            p += bom;
            n -= bom;
            first = false;
        }

        for (; n >= unit; p += unit, n -= unit)
        {
            int c = tag_unit(p, enc);
            if (!c)
            {
                size = 0;
                break;
            }
            if (c == '\n')
            {
                decode_tag_line(line, used, enc, shortened, text);
                if (!(timed ? parse_lrc_line((char *)text)
                            : lyrics_append_text((char *)text)))
                    return;
                used = 0;
                shortened = false;
            }
            else if (used + unit <= (int)sizeof(line))
            {
                memcpy(line + used, p, unit);
                used += unit;
            }
            else
                shortened = true;
        }
    }
    if (used && !clipped)
    {
        decode_tag_line(line, used, enc, shortened, text);
        if (timed)
            parse_lrc_line((char *)text);
        else
            lyrics_append_text((char *)text);
    }
}

/* Read the tag as UTF-8 without buffering its whole value. SYLT supplies
 * NUL-terminated strings separated by four-byte timestamps. TTML carries
 * its own timing, so concatenate the strings and skip the frame timestamps. */
struct tag_input {
    ssize_t (*read)(void *, unsigned char *, size_t);
    void *source;
    long left;
    enum tag_encoding enc;
    unsigned char raw[256], utf8[4];
    size_t raw_at, raw_count, utf8_at, utf8_count;
    int pending_unit;
    bool sylt, first_unit;
};

static int tag_input_byte(struct tag_input *in)
{
    if (in->raw_at == in->raw_count)
    {
        if (in->left <= 0) return -1;
        ssize_t n = in->read(in->source, in->raw, MIN(in->left, (long)sizeof(in->raw)));
        if (n <= 0)
        {
            in->left = 0;
            return -1;
        }
        in->left -= n;
        in->raw_at = 0;
        in->raw_count = n;
    }
    return in->raw[in->raw_at++];
}

static int tag_input_unit(struct tag_input *in)
{
    if (in->pending_unit >= 0)
    {
        int unit = in->pending_unit;
        in->pending_unit = -1;
        return unit;
    }
    int first = tag_input_byte(in);
    if (first < 0 || tag_unit_size(in->enc) == 1) return first;
    int second = tag_input_byte(in);
    if (second < 0) return -1;
    return in->enc == TAG_UTF16_LE ? first | (second << 8) : (first << 8) | second;
}

static bool tag_skip_string(struct tag_input *in)
{
    /* The descriptor may establish the byte order for a following string
     * without a BOM. An explicit BOM on the following string overrides it. */
    int unit = tag_input_unit(in);
    if (tag_unit_size(in->enc) == 2 && (unit == 0xfffe || unit == 0xfeff))
    {
        if (unit == 0xfffe)
            in->enc = in->enc == TAG_UTF16_LE ? TAG_UTF16_BE : TAG_UTF16_LE;
        unit = tag_input_unit(in);
    }
    while (unit > 0)
        unit = tag_input_unit(in);
    return unit == 0;
}

static int tag_input_utf8(struct tag_input *in)
{
    if (in->utf8_at < in->utf8_count)
        return in->utf8[in->utf8_at++];

    int c;
    for (;;)
    {
        c = tag_input_unit(in);
        if (c < 0) return -1;
        if (!c)
        {
            if (!in->sylt) return -1;
            for (int n = 0; n < 4; n++)
                if (tag_input_byte(in) < 0) return -1;
            in->first_unit = true;
            continue;
        }

        bool first = in->first_unit;
        in->first_unit = false;
        if (first && tag_unit_size(in->enc) == 2 && (c == 0xfffe || c == 0xfeff))
        {
            if (c == 0xfffe)
                in->enc = in->enc == TAG_UTF16_LE ? TAG_UTF16_BE : TAG_UTF16_LE;
            continue;
        }
        break;
    }

    if (in->enc == TAG_UTF8) return c;
    if (c >= 0xd800 && c <= 0xdbff)
    {
        int low = tag_input_unit(in);
        if (low >= 0xdc00 && low <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
        else
        {
            in->pending_unit = low;
            c = 0xfffd;
        }
    }
    else if (c >= 0xdc00 && c <= 0xdfff)
        c = 0xfffd;

    in->utf8_count = utf8encode(c, in->utf8) - in->utf8;
    in->utf8_at = 1;
    return in->utf8[0];
}

struct xml_stream {
    struct tag_input *input;
    unsigned char prefix[5];
    unsigned at, count;
    int next;
};

static int xml_stream_get(struct xml_stream *s)
{
    return s->at < s->count ? s->prefix[s->at++] : tag_input_utf8(s->input);
}

static bool tag_is_ttml(struct xml_stream *s)
{
    int c;
    for (;;)
    {
        c = tag_input_utf8(s->input);
        if (c == 0xef)
        {
            if (tag_input_utf8(s->input) != 0xbb || tag_input_utf8(s->input) != 0xbf)
                return false;
        }
        else if (c < 0 || !isspace(c))
            break;
    }
    if (c != '<') return false;
    s->prefix[0] = c;
    c = tag_input_utf8(s->input);
    if (c != '?' && c != 't') return false;
    s->prefix[1] = c;
    s->count = c == '?' ? 5 : 4;
    for (unsigned i = 2; i < s->count; i++)
    {
        c = tag_input_utf8(s->input);
        if (c < 0) return false;
        s->prefix[i] = c;
    }
    return !memcmp(s->prefix, "<?xml", 5) ||
           (!memcmp(s->prefix, "<tt", 3) && (c == '>' || c == '/' || isspace(c)));
}

/* Framing only. The existing XML reader still validates and interprets
 * every structural tag and parses each paragraph with its two voices. */
struct xml_chunk {
    char *text;
    size_t room, len;
    bool clipped;
};
enum xml_piece { XML_EOF, XML_ERROR, XML_SKIP, XML_TEXT, XML_TAG };

static void xml_stream_take(struct xml_stream *s, struct xml_chunk *chunk)
{
    if (chunk)
    {
        if (chunk->len + 1 < chunk->room)
            chunk->text[chunk->len++] = s->next;
        else
            chunk->clipped = true;
    }
    s->next = xml_stream_get(s);
}

static bool xml_stream_literal(struct xml_stream *s, struct xml_chunk *chunk,
                               const char *text)
{
    for (; *text; text++)
    {
        if (s->next != *text) return false;
        xml_stream_take(s, chunk);
    }
    return true;
}

static bool xml_stream_until(struct xml_stream *s, struct xml_chunk *chunk,
                             const char *terminator)
{
    unsigned long target = 0, mask = 0, tail = 0;
    for (; *terminator; terminator++)
    {
        target = (target << 8) | (unsigned char)*terminator;
        mask = (mask << 8) | 0xff;
    }
    while (s->next >= 0)
    {
        tail = ((tail << 8) | (unsigned)s->next) & mask;
        xml_stream_take(s, chunk);
        if (tail == target) return true;
    }
    return false;
}

static enum xml_piece xml_stream_piece(struct xml_stream *s, struct xml_chunk *chunk)
{
    enum xml_piece piece = XML_TEXT;
    if (s->next < 0) return XML_EOF;
    if (s->next != '<')
    {
        while (s->next >= 0 && s->next != '<')
            xml_stream_take(s, chunk);
    }
    else
    {
        xml_stream_take(s, chunk);
        if (s->next == '?')
        {
            xml_stream_take(s, NULL);
            return xml_stream_until(s, NULL, "?>") ? XML_SKIP : XML_ERROR;
        }
        if (s->next == '!')
        {
            xml_stream_take(s, chunk);
            if (s->next == '-')
                return xml_stream_literal(s, NULL, "--") &&
                       xml_stream_until(s, NULL, "-->") ? XML_SKIP : XML_ERROR;
            if (!xml_stream_literal(s, chunk, "[CDATA[") ||
                !xml_stream_until(s, chunk, "]]>")) return XML_ERROR;
        }
        else
        {
            piece = XML_TAG;
            int quote = 0;
            for (;;)
            {
                int c = s->next;
                if (c < 0) return XML_ERROR;
                xml_stream_take(s, chunk);
                if (quote)
                {
                    if (c == quote) quote = 0;
                }
                else if (c == '>' || c == '<')
                {
                    if (c == '<') return XML_ERROR;
                    break;
                }
                else if (c == '\'' || c == '"')
                    quote = c;
            }
        }
    }
    chunk->text[chunk->len] = 0;
    return piece;
}

#define TTML_TAG_ID_BYTES 2048
#define TTML_TAG_SCRATCH (TTML_PACKET_MAX + TTML_TAG_ID_BYTES)

static bool tag_ttml_paragraph(struct xml_stream *s, struct ttml_document *doc,
                               struct ttml_span span, char *packet)
{
    size_t used = 0;
    bool clipped = false;
    for (;;)
    {
        /* Retain enough space to recognise the close tag. When the paragraph
         * is too large, scan to its end and resume with the next paragraph. */
        clipped |= TTML_PACKET_MAX - used < TTML_SCOPE_NAME + 4;
        if (clipped) used = 0;
        struct xml_chunk part = {packet + used, TTML_PACKET_MAX - used, 0, false};
        enum xml_piece piece = xml_stream_piece(s, &part);
        if (piece == XML_SKIP) continue;
        if (piece == XML_ERROR || piece == XML_EOF) return false;

        clipped |= part.clipped;
        if (piece == XML_TAG && !part.clipped)
        {
            const char *p = part.text;
            struct xml_token token;
            if (!xml_next(&p, &token)) return false;
            if (token.close && xml_is(token.name, "p"))
            {
                const char *body = packet;
                return clipped || ttml_paragraph(&doc->reader, &body, span);
            }
        }
        used += part.len;
    }
}

/* Returns false only for a non-XML tag. A detected XML tag stays TTML even
 * if malformed. Keep complete paragraphs when the document arena fills. */
static bool load_tag_ttml(struct tag_input *input)
{
    struct xml_stream stream = {.input = input};
    if (!tag_is_ttml(&stream)) return false;

    size_t arena_end = buf_end;
    buf_end -= TTML_TAG_SCRATCH;
    char *packet = (char *)buffer + buf_end;
    char scope_names[TTML_DEPTH][TTML_SCOPE_NAME];
    struct ttml_document doc = {
        .reader = {.ids = packet + TTML_PACKET_MAX, .ids_left = TTML_TAG_ID_BYTES},
    };
    bool root = false, valid = false;
    stream.next = xml_stream_get(&stream);
    lrc.type = TYPE_TTML;
    for (;;)
    {
        struct xml_chunk part = {packet, TTML_PACKET_MAX, 0, false};
        enum xml_piece piece = xml_stream_piece(&stream, &part);
        if (piece == XML_EOF)
        {
            valid = root && !doc.depth;
            break;
        }
        if (piece == XML_ERROR) break;
        if (piece != XML_TAG) continue;

        const char *p = packet;
        struct xml_token token;
        if (part.clipped || !xml_next(&p, &token)) break;
        if (!root)
        {
            if (token.close || !xml_is(token.name, "tt")) break;
            root = true;
        }
        if (!token.close && !token.empty && xml_is(token.name, "p"))
        {
            struct ttml_span span = ttml_paragraph_span(&doc, &token);
            if (!tag_ttml_paragraph(&stream, &doc, span, packet))
            {
                valid = doc.reader.exhausted;
                break;
            }
        }
        else if (!ttml_document_tag(&doc, &token, scope_names))
            break;
    }
    /* Model records end directly below the scratch region. Keep their
     * addresses stable until build_index() uses the space reserved for it. */
    if (!valid || !lrc.nline)
    {
        reset_data();
        lrc.length = track.length;
        buf_end = arena_end;
    }
    else
        lrc.voices = doc.reader.voices;
    return true;
}

static ssize_t tag_read_vorbis(void *source, unsigned char *out, size_t size)
{
    return ogg_file_read(source, out, size);
}

static void load_tag_text(struct ogg_file *file, const unsigned char *data,
                          long size, enum tag_encoding enc, bool clipped)
{
    if (size <= 0)
        return;
    struct ogg_file origin;
    off_t pos = 0;
    if (file)
    {
        origin = *file;
        pos = lseek(file->fd, 0, SEEK_CUR);
        if (pos < 0) return;
        struct tag_input in = {
            .read = tag_read_vorbis, .source = file, .left = size, .enc = enc,
            .pending_unit = -1, .first_unit = true,
        };
        if (load_tag_ttml(&in)) return;
        if (lseek(file->fd, pos, SEEK_SET) < 0) return;
        *file = origin;
    }
    /* Bound the search for a timestamp even when a tag contains megabytes
     * of untimed text. The line and word model then owns the arena limit. */
    size_t end = buf_end; /* a staged ID3 frame occupies the arena tail */
    lrc.type = TYPE_LRC8;
    read_tag_lines(file, data, size, enc, true, clipped);
    if (lrc.head && lrc.head->nword)
        return;

    if (file)
    {
        if (pos < 0 || lseek(file->fd, pos, SEEK_SET) < 0)
            return;
        *file = origin;
    }
    reset_data();
    buf_end = end;
    lrc.length = track.length;
    lrc.type = TYPE_TEXT;
    read_tag_lines(file, data, size, enc, false, clipped);
}

static uint32_t syncsafe(const unsigned char *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14)
         | ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static bool read_id3(int fd, void *out, int len, off_t end, bool *ff)
{
    char *p = out;
    do
    {
        off_t pos = lseek(fd, 0, SEEK_CUR);
        if (pos < 0 || len > end - pos || read(fd, p, len) != len)
            return false;
        int n = ff ? id3_unsynchronize(p, len, ff) : len;
        p += n;
        len -= n;
    } while (len);
    return true;
}

static bool skip_id3(int fd, int len, off_t end, bool *ff)
{
    if (!ff)
    {
        off_t pos = lseek(fd, 0, SEEK_CUR);
        return pos >= 0 && len <= end - pos && lseek(fd, len, SEEK_CUR) >= 0;
    }

    unsigned char scrap[128];
    while (len > 0)
    {
        int n = MIN(len, (int)sizeof(scrap));
        if (!read_id3(fd, scrap, n, end, ff))
            return false;
        len -= n;
    }
    return true;
}

struct id3_lyrics {
    int fd;
    off_t start, end;
    uint32_t remaining;
    int prefix;
    bool global_unsync, frame_unsync, ff;
};

/* Shared by SYLT/USLT, including their ID3v2.2 spellings. */
static bool find_id3_lyrics(int fd, bool timed, struct id3_lyrics *tag)
{
    unsigned char b[10];
    if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, b, 10) != 10
        || memcmp(b, "ID3", 3) || b[3] < 2 || b[3] > 4)
        return false;

    int version = b[3], flags = b[5], header = version == 2 ? 6 : 10;
    off_t end = 10 + syncsafe(b + 6);
    if (end > ffilesize(fd) || (version == 2 && (flags & 0x40)))
        return false;
    bool ff = false;
    bool *global = version < 4 && (flags & 0x80) ? &ff : NULL;
    if (flags & 0x40)
    {
        if (!read_id3(fd, b, 4, end, global))
            return false;
        uint32_t n = version == 3 ? get_long_be(b) : syncsafe(b);
        if (version == 4)
            n -= 4;
        if (n > (uint32_t)end || !skip_id3(fd, n, end, global))
            return false;
    }

    while (read_id3(fd, b, header, end, global) && b[0])
    {
        uint32_t len = version == 2 ? get_long_be(b + 2) & 0xffffff
                     : version == 3 ? get_long_be(b + 4) : syncsafe(b + 4);
        bool match = !memcmp(b, timed ? (version == 2 ? "SLT" : "SYLT")
                                      : (version == 2 ? "ULT" : "USLT"),
                             version == 2 ? 3 : 4);
        int fmt = version == 2 ? 0 : b[9];
        off_t pos = lseek(fd, 0, SEEK_CUR);
        if (pos < 0 || end < pos || len > (uint32_t)(end - pos))
            break;
        if (!match || (fmt & (version == 4 ? 0x0c : 0xc0)))
        {
            if (!skip_id3(fd, len, end, global))
                break;
            continue;
        }

        int prefix = !!(fmt & (version == 4 ? 0x40 : 0x20));
        if (version == 4 && (fmt & 1))
            prefix += 4; /* data length indicator */
        *tag = (struct id3_lyrics){
            .fd = fd, .start = pos, .end = end, .remaining = len, .prefix = prefix,
            .global_unsync = global != NULL,
            .frame_unsync = version == 4 && ((fmt & 2) || (flags & 0x80)),
            .ff = ff,
        };
        return true;
    }
    return false;
}

static ssize_t tag_read_id3(void *source, unsigned char *out, size_t size)
{
    struct id3_lyrics *tag = source;
    while (tag->remaining)
    {
        int n = MIN(tag->remaining, size);
        if (!read_id3(tag->fd, out, n, tag->end, tag->global_unsync ? &tag->ff : NULL))
            return -1;
        tag->remaining -= n;
        if (tag->frame_unsync)
            n = id3_unsynchronize((char *)out, n, &tag->ff);
        if (n) return n;
    }
    return 0;
}

static bool load_id3_ttml(const struct id3_lyrics *tag, bool timed)
{
    struct id3_lyrics source = *tag;
    struct tag_input in = {
        .read = tag_read_id3, .source = &source, .left = tag->remaining,
        .pending_unit = -1, .first_unit = true, .sylt = timed,
    };
    for (int i = 0; i < tag->prefix; i++)
        if (tag_input_byte(&in) < 0) return false;

    int enc = tag_input_byte(&in);
    if (enc < 0 || enc > TAG_UTF8) return false;
    in.enc = enc;
    int header = timed ? 6 : 4;
    for (int i = 1; i < header; i++)
        if (tag_input_byte(&in) < 0) return false;
    return tag_skip_string(&in) && load_tag_ttml(&in);
}

/* Retain the original bounded staging path for ordinary SYLT and USLT. */
static int read_id3_lyrics(const struct id3_lyrics *tag, unsigned char *frame,
                           bool *clipped)
{
    bool ff = tag->ff;
    *clipped = tag->remaining > ID3_LYRICS_MAX;
    int n = MIN(tag->remaining, ID3_LYRICS_MAX);
    if (lseek(tag->fd, tag->start, SEEK_SET) < 0 ||
        !read_id3(tag->fd, frame, n, tag->end, tag->global_unsync ? &ff : NULL))
        return 0;
    if (tag->frame_unsync)
        n = id3_unsynchronize((char *)frame, n, &ff);
    if (n <= tag->prefix) return 0;
    if (tag->prefix)
    {
        n -= tag->prefix;
        memmove(frame, frame + tag->prefix, n);
    }
    return n;
}

static const unsigned char *tag_string_end(const unsigned char *p,
                                           const unsigned char *end,
                                           enum tag_encoding enc)
{
    int unit = tag_unit_size(enc);
    for (; end - p >= unit; p += unit)
        if (!tag_unit(p, enc))
            return p;
    return NULL;
}

static void load_id3_frame(int fd, bool timed)
{
    struct id3_lyrics tag;
    if (!find_id3_lyrics(fd, timed, &tag) || load_id3_ttml(&tag, timed))
        return;

    unsigned char *frame = buffer + LYRICS_BUFFER_SIZE - ID3_LYRICS_MAX;
    bool clipped;
    int len = read_id3_lyrics(&tag, frame, &clipped);
    int head = timed ? 6 : 4;
    if (len <= head || frame[0] > TAG_UTF8 || (timed && frame[4] != 2))
        return; /* only millisecond timestamps */

    enum tag_encoding enc = frame[0];
    const unsigned char *p = frame + head, *end = frame + len;
    int unit = tag_unit_size(enc);
    p += skip_bom(p, end - p, &enc);
    const unsigned char *stop = tag_string_end(p, end, enc);
    if (!stop)
        return;
    p = stop + unit;
    buf_end = frame - buffer;

    if (!timed)
    {
        load_tag_text(NULL, p, end - p, enc, clipped);
        return;
    }
    lrc.type = TYPE_SYLT;
    while (end - p >= unit + 4)
    {
        p += skip_bom(p, end - p, &enc);
        if (end - p >= unit && tag_unit(p, enc) == '\n')
            p += unit;
        stop = tag_string_end(p, end, enc);
        if (!stop || end - stop < unit + 4)
            break;

        unsigned char text[UTF8_LINE_SIZE];
        int n = MIN(stop - p, MAX_LINE_LEN);
        decode_tag_line(p, n, enc, n < stop - p, text);
        p = stop + unit;
        uint32_t time = get_long_be((void *)p);
        p += 4;
        if (time > LONG_MAX)
            break;
        struct lyrics_line *line = alloc_buf(sizeof(*line));
        if (!line)
            break;
        line->time = time;
        if (!add_line(line, text))
            break;
    }
}

static void load_embedded(void)
{
    int type = track.codectype;
    if (type != AFMT_MPA_L1 && type != AFMT_MPA_L2 && type != AFMT_MPA_L3
        && type != AFMT_MP4_ALAC && type != AFMT_MP4_AAC
        && type != AFMT_MP4_AAC_HE && type != AFMT_FLAC
        && type != AFMT_OGG_VORBIS)
        return;

    int fd = open(track.path, O_RDONLY);
    if (fd < 0)
        return;
    if (type == AFMT_MPA_L1 || type == AFMT_MPA_L2 || type == AFMT_MPA_L3)
    {
        load_id3_frame(fd, true);
        if (!lrc.nline)
        {
            reset_data();
            lrc.length = track.length;
            load_id3_frame(fd, false);
        }
    }
    else
    {
        struct ogg_file file;
        long size;
        if (type == AFMT_FLAC || type == AFMT_OGG_VORBIS)
            size = find_vorbis_lyrics(&file, fd, type);
        else
        {
            size = find_mp4_lyrics(fd);
            file = (struct ogg_file){.fd = fd, .packet_remaining = size,
                                     .packet_ended = true};
        }
        load_tag_text(&file, NULL, size, TAG_UTF8, false);
    }
    close(fd);
    if (lrc.nline)
        strlcpy(lrc.file, track.path, sizeof(lrc.file));
}

/* ---------------------------------------------------------------------------
 * Loading, and the public accessors
 * ------------------------------------------------------------------------ */

enum lyrics_result lyrics_load(const struct mp3entry *id3)
{
    reset_data();

    /* An mp3entry is filled in as its track buffers, so it can be handed over
     * still empty -- audio_get_track_metadata() wipes it when even the
     * playlist cannot name the file. That is not "no lyrics"; it is "ask
     * again in a moment". */
    if (!id3 || !id3->path[0])
        return LYRICS_NO_TRACK;

    /* Copy everything needed out of the caller's entry BEFORE allocating --
     * see the comment on `track`. Order matters here, not style. */
    memset(&track, 0, sizeof(track));
    strlcpy(track.path, id3->path, sizeof(track.path));
    if (id3->title)
        strlcpy(track.title, id3->title, sizeof(track.title));
    if (id3->artist)
        strlcpy(track.artist, id3->artist, sizeof(track.artist));
    track.length     = id3->length;
    track.codectype  = id3->codectype;

    /* The buffer is kept from one track to the next and freed only by
     * lyrics_close(): a 32 KiB allocation per track change can shrink the
     * audio buffer and rebuffer the music. */
    if (lyrics_handle <= 0)
    {
        lyrics_handle = core_alloc_ex(LYRICS_BUFFER_SIZE, &buflib_ops_locked);
        if (lyrics_handle <= 0)
        {
            lyrics_handle = 0;
            return LYRICS_NO_MEMORY;
        }
        buffer = core_get_data(lyrics_handle);
    }

    reset_data();
    lrc.length = track.length;

    const char *slash = strrchr(track.path, '/');
    const char *dot = slash ? strrchr(slash, '.') : NULL;
    size_t stem = dot ? (size_t)(dot - track.path) : strlen(track.path);
    bool ttml = false;
    if (slash && stem + sizeof(".ttml") <= sizeof(lrc.file))
    {
        snprintf(lrc.file, sizeof(lrc.file), "%.*s.ttml", (int)stem, track.path);
        if (file_exists(lrc.file))
            ttml = load_ttml_file();
    }
    if (!ttml)
    {
        ttml_release_extra();
        reset_data();
        if (!buffer) return LYRICS_NO_MEMORY;
        lrc.length = track.length;
        if (find_lyrics_file(false) || find_lyrics_file(true))
            load_lyrics_file();
        else
            load_embedded();
    }

    /* Text lines have equal timestamps and keep their input order. */
    if (lyrics_timed()) sort_lines();
    /* without the index nothing can be reached, so that counts as no lyrics */
    lrc.loaded = (lrc.nline > 0) && build_index();

    if (!lrc.loaded)
    {
        reset_data();
        return LYRICS_NOT_FOUND;
    }
    return LYRICS_OK;
}

void lyrics_close(void)
{
    if (lyrics_handle > 0)
        core_free(lyrics_handle);
    lyrics_handle = 0;
    lyrics_capacity = LYRICS_BUFFER_SIZE;
    buffer = NULL;
    reset_data();
}

bool lyrics_loaded(void)
{
    return lrc.loaded;
}

bool lyrics_timed(void) { return lrc.type != TYPE_TEXT; }

enum lyrics_result lyrics_begin_text(void)
{
    ttml_release_extra();
    if (lyrics_handle <= 0)
    {
        lyrics_handle = core_alloc_ex(LYRICS_BUFFER_SIZE, &buflib_ops_locked);
        if (lyrics_handle <= 0) { lyrics_handle = 0; return LYRICS_NO_MEMORY; }
        buffer = core_get_data(lyrics_handle);
    }
    reset_data();
    lrc.type = TYPE_TEXT;
    return LYRICS_OK;
}

bool lyrics_append_text(const char *text)
{
    while (*text)
    {
        const char *end = strchr(text, '\n');
        size_t len = end ? (size_t)(end - text) : strlen(text);
        if (len && text[len - 1] == '\r') len--;
        /* Reserve the entire line before publishing it. */
        if (len + 1 + sizeof(struct lyrics_line) + sizeof(struct lyrics_word)
            + 8 > buf_free()) return false;
        struct lyrics_line *line = alloc_buf(sizeof(*line));
        line->time = 0;
        struct lyrics_word *word = alloc_buf(sizeof(*word));
        word->time = -1;
        word->text = buffer + buf_used;
        memcpy(word->text, text, len);
        word->text[len] = 0;
        buf_used += len + 1;
        line->words = word;
        line->nword = 1;
        line->next = NULL;
        *lrc.tail = line;
        lrc.tail = &line->next;
        lrc.nline++;
        if (!end) break;
        text = end + 1;
    }
    return true;
}

bool lyrics_finish_text(void)
{
    lrc.loaded = lrc.nline > 0 && build_index();
    return lrc.loaded;
}

bool lyrics_append_lrc(const char *text, bool more)
{
    lrc.type = TYPE_LRC8;
    size_t size = strlen(text);
    if (size >= sizeof remote_lrc_line - lrc.remote_line_size)
        return false;
    memcpy(remote_lrc_line + lrc.remote_line_size, text, size + 1);
    lrc.remote_line_size += size;
    if (more)
        return true;
    lrc.remote_line_size = 0;
    int lines = lrc.nline;
    return parse_lrc_line((char *)remote_lrc_line) && lrc.nline == lines + 1;
}

bool lyrics_append_ttml(const char *text, bool more, int voices)
{
    if (voices < 0 || voices > TTML_AGENTS || (lrc.type == TYPE_TTML && lrc.voices != voices))
        return false;
    if (lrc.type != TYPE_TTML) ttml_expand();
    lrc.type = TYPE_TTML;
    lrc.voices = voices;
    if (ttml_packet_handle <= 0)
        ttml_packet_handle = core_alloc_ex(TTML_PACKET_MAX, &buflib_ops_locked);
    if (ttml_packet_handle <= 0) return false;

    char *packet = core_get_data(ttml_packet_handle);
    size_t size = strlen(text);
    if (size >= TTML_PACKET_MAX - lrc.remote_line_size) return false;
    memcpy(packet + lrc.remote_line_size, text, size + 1);
    lrc.remote_line_size += size;
    if (more) return true;
    lrc.remote_line_size = 0;
    int lines = lrc.nline;
    return parse_ttml(packet, true) && lrc.nline == lines + 1;
}

bool lyrics_ttml_complete(void)
{
    if (ttml_packet_handle > 0)
        core_free(ttml_packet_handle);
    ttml_packet_handle = 0;
    return !lrc.remote_line_size;
}

bool lyrics_finish_lrc(long duration)
{
    if (lrc.remote_line_size)
        return false;
    lrc.length = duration;
    sort_lines();
    return lyrics_finish_text();
}

void lyrics_resume_remote(void)
{
    if (lrc.index)
        buf_end += (lrc.nline * sizeof(*lrc.index) + 3) & ~3;
    lrc.index = NULL;
    lrc.loaded = false;
}

bool lyrics_publish_remote(long duration)
{
    if (lyrics_timed())
    {
        lrc.length = duration;
        sort_lines();
    }
    return lyrics_finish_text();
}

const char *lyrics_file(void)
{
    return lrc.file;
}

const char *lyrics_title(void)
{
    return lrc.title;
}

const char *lyrics_artist(void)
{
    return lrc.artist;
}

int lyrics_count(void)
{
    return lrc.nline;
}

const char *lyrics_text(int index)
{
    struct lyrics_line *line = get_line(index);

    if (!line)
        return NULL;
    if (lrc.type == TYPE_TTML)
    {
        const struct lyrics_ttml_word *words = (const void *)line->words;
        return (const char *)words[line->nword - 1].base.text;
    }
    return (const char *)line->words[line->nword - 1].text;
}

long lyrics_time(int index)
{
    struct lyrics_line *line = get_line(index);

    if (!line)
        return 0;
    return apply_offset(line->time);
}

long lyrics_end_time(int index)
{
    long start, end;

    if (index < 0 || index >= lrc.nline)
        return 0;
    if (index + 1 < lrc.nline)
        return lyrics_time(index + 1);

    /* the last line runs to the end of the track */
    start = lyrics_time(index);
    end = lrc.length;
    return (end > start)? end: start;
}

int lyrics_index_at(long time)
{
    int lo = 0, hi = lrc.nline - 1, found = LYRICS_NONE;

    /* the last line to have started */
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (lyrics_time(mid) <= time)
        {
            found = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return found;
}

int lyrics_word_count(int index)
{
    struct lyrics_line *line = get_line(index);

    return line? line->nword: 0;
}

const char *lyrics_word_text(int index, int word)
{
    struct lyrics_line *line = get_line(index);

    if (!line || word < 0 || word >= line->nword)
        return NULL;
    if (lrc.type == TYPE_TTML)
        return lyrics_vocal_cue(index, false, word).text;
    return (const char *)line->words[line->nword - 1 - word].text;
}

long lyrics_word_time(int index, int word)
{
    struct lyrics_line *line = get_line(index);
    long time;

    if (!line || word < 0 || word >= line->nword)
        return -1;
    if (lrc.type == TYPE_TTML)
        return lyrics_vocal_cue(index, false, word).begin;
    time = line->words[line->nword - 1 - word].time;
    return (time < 0)? -1: apply_offset(time);
}

bool lyrics_ttml(void) { return lrc.type == TYPE_TTML; }

int lyrics_agent_alignment(int agent)
{
    if (lrc.voices < 2 || !agent) return -1;
    if (agent < TTML_PERSON || agent >= TTML_GROUP) return 1;
    return ((agent - TTML_PERSON) & 1) ? 2 : 0;
}

static const struct lyrics_ttml_line *ttml_line_at(int index, bool background)
{
    if (!lyrics_ttml()) return NULL;
    const struct lyrics_ttml_line *line = (const void *)get_line(index);
    return line && background ? line->background : line;
}

struct lyrics_vocal lyrics_vocal_at(int index, bool background)
{
    const struct lyrics_ttml_line *line = ttml_line_at(index, background);
    if (!line) return (struct lyrics_vocal){0};
    const struct lyrics_ttml_word *words = (const void *)line->base.words;
    return (struct lyrics_vocal){
        .text = (const char *)words[line->base.nword - 1].base.text,
        .begin = line->base.time < 0 ? lyrics_time(index) : line->base.time,
        .end = line->end < 0 ? lyrics_end_time(index) : line->end,
        .words = line->base.nword, .agent = line->agent, .timed = line->timed,
    };
}

struct lyrics_cue lyrics_vocal_cue(int index, bool background, int word)
{
    const struct lyrics_ttml_line *line = ttml_line_at(index, background);
    if (!line || word < 0 || word >= line->base.nword)
        return (struct lyrics_cue){.begin = -1, .end = -1};
    const struct lyrics_ttml_word *words = (const void *)line->base.words;
    const struct lyrics_ttml_word *cue = &words[line->base.nword - 1 - word];
    return (struct lyrics_cue){(const char *)cue->base.text, cue->base.time,
                               cue->end, cue->agent, cue->timed};
}
