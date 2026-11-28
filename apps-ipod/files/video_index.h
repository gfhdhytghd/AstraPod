/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to video_index.c: the kind, names, length and cover of every
 * video file the player holds outside the iTunes library.
 ****************************************************************************/
#ifndef _VIDEO_INDEX_H
#define _VIDEO_INDEX_H

#include <stdbool.h>
#include <stdint.h>
#include "file.h"                       /* MAX_PATH */

#define VIDEO_INDEX_TITLE 128
#define VIDEO_INDEX_NAME  96

/* A release date packed in a word, month and day 0 where unknown */
#define VIDEO_DATE(year, month, day) \
    ((uint32_t)(year) << 9 | (uint32_t)(month) << 5 | (uint32_t)(day))
#define VIDEO_DATE_YEAR(d)  ((d) >> 9)
#define VIDEO_DATE_MONTH(d) (((d) >> 5) & 15)
#define VIDEO_DATE_DAY(d)   ((d) & 31)

/* One video. kind is a Videos category, enum stock_video_kind's
 * STOCK_MOVIE, STOCK_MUSIC_VIDEO or STOCK_TV_SHOW. artist and show are
 * empty where the file names none, and title the file's name without its
 * extension. */
struct video_entry
{
    const char *path;
    const char *title;
    const char *artist;
    const char *show;
    uint64_t art_key;           /* names its cover thumbnail; 0 for none */
    unsigned kind;
    uint32_t duration_ms;
    uint32_t season, episode;
    uint32_t released;          /* VIDEO_DATE(), 0 for none */
    long cover_at;              /* the 'covr' image's offset, or -1 */
    uint32_t cover_size;
};

/* The documents walk's side, on the background thread: begin, then each
 * video file it passes with its directory entry's size and time, then end.
 * A file inside the iTunes library is passed by: the library names it. */
void video_index_begin(void);
void video_index_found(const char *path, unsigned long size,
                       unsigned long mtime);
void video_index_end(bool completed);

/* Whether the index is missing or was written by an older version */
bool video_index_stale(void);

/* Where video_index_each() puts each video's strings */
struct video_index_text
{
    char path[MAX_PATH];
    char title[VIDEO_INDEX_TITLE];
    char artist[VIDEO_INDEX_NAME];
    char show[VIDEO_INDEX_NAME];
};

/* Every indexed video, until `fn` returns false. The strings are in
 * `text`, and last only for the call. */
typedef bool (*video_index_fn)(const struct video_entry *e, void *data);
void video_index_each(video_index_fn fn, void *data,
                      struct video_index_text *text);

#endif /* _VIDEO_INDEX_H */
