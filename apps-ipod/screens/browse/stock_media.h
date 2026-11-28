/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Browsers for the stock media libraries.
 ****************************************************************************/
#ifndef STOCK_MEDIA_H
#define STOCK_MEDIA_H

#include "lcd.h"                        /* fb_data */
#include "files/stock_db.h"

int stock_videos_screen(enum stock_video_kind kind, int title);
/* How many videos each Videos category holds, and the video playlists */
struct stock_video_census {
    int movies, tv_shows, music_videos, playlists;
};
void stock_videos_census(struct stock_video_census *census);
/* For the menu pane's slideshow, on its thread: a cover of a video of
 * `kinds` (enum stock_video_kind bits), from the library's artwork or the
 * loose videos' thumbnails, at random and other than the cover `previous`,
 * into `pixels` VIDEO_ART_PANE square. Returns which cover it is, or -1
 * for none. The library's are those the last census found. */
int stock_videos_pane_cover(unsigned kinds, int previous, fb_data *pixels);
int stock_photos_screen(void);
int stock_photos_settings(void);
int stock_videos_settings(void);

struct gui_synclist;
int stock_media_list_action(int action, struct gui_synclist *lists);
#endif
