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

/* What one entry of a SYLT frame takes from the top of the buffer. */
#define SYLT_SLACK  (sizeof(struct lyrics_line) \
                     + sizeof(struct lyrics_word) + 8)

static int lyrics_handle = 0;
static unsigned char *buffer;
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
    unsigned short remote_line_size;
    bool loaded;
} lrc;

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
    memset(&lrc, 0, sizeof(lrc));
    lrc.tail = &lrc.head;
    buf_used = 0;
    buf_end = LYRICS_BUFFER_SIZE;
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

/* Shared by SYLT/USLT, including their ID3v2.2 spellings. */
static int read_id3_lyrics(int fd, bool timed, unsigned char *frame,
                           bool *clipped)
{
    unsigned char b[10];
    if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, b, 10) != 10
        || memcmp(b, "ID3", 3) || b[3] < 2 || b[3] > 4)
        return 0;

    int version = b[3], flags = b[5], header = version == 2 ? 6 : 10;
    off_t end = 10 + syncsafe(b + 6);
    if (end > ffilesize(fd) || (version == 2 && (flags & 0x40)))
        return 0;
    bool ff = false;
    bool *global = version < 4 && (flags & 0x80) ? &ff : NULL;
    if (flags & 0x40)
    {
        if (!read_id3(fd, b, 4, end, global))
            return 0;
        uint32_t n = version == 3 ? get_long_be(b) : syncsafe(b);
        if (version == 4)
            n -= 4;
        if (n > (uint32_t)end || !skip_id3(fd, n, end, global))
            return 0;
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
        *clipped = len > ID3_LYRICS_MAX;
        int n = MIN(len, ID3_LYRICS_MAX);
        if (!read_id3(fd, frame, n, end, global))
            break;
        if (version == 4 && ((fmt & 2) || (flags & 0x80)))
        {
            ff = false;
            n = id3_unsynchronize((char *)frame, n, &ff);
        }
        if (n <= prefix)
            break;
        if (prefix)
        {
            n -= prefix;
            memmove(frame, frame + prefix, n);
        }
        return n;
    }
    return 0;
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
    unsigned char *frame = buffer + LYRICS_BUFFER_SIZE - ID3_LYRICS_MAX;
    bool clipped;
    int len = read_id3_lyrics(fd, timed, frame, &clipped);
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

    if (find_lyrics_file(false) || find_lyrics_file(true))
        load_lyrics_file();
    else
        load_embedded();

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
    if (size >= sizeof utf8_line - lrc.remote_line_size)
        return false;
    memcpy(utf8_line + lrc.remote_line_size, text, size + 1);
    lrc.remote_line_size += size;
    if (more)
        return true;
    lrc.remote_line_size = 0;
    int lines = lrc.nline;
    return parse_lrc_line((char *)utf8_line) && lrc.nline == lines + 1;
}

bool lyrics_finish_lrc(long duration)
{
    if (lrc.remote_line_size)
        return false;
    lrc.length = duration;
    sort_lines();
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
    return (const char *)line->words[line->nword - 1 - word].text;
}

long lyrics_word_time(int index, int word)
{
    struct lyrics_line *line = get_line(index);
    long time;

    if (!line || word < 0 || word >= line->nword)
        return -1;
    time = line->words[line->nword - 1 - word].time;
    return (time < 0)? -1: apply_offset(time);
}
