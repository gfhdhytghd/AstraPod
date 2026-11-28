/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Browsers for the stock media libraries.
 ****************************************************************************/
#ifndef STOCK_MEDIA_H
#define STOCK_MEDIA_H

#include "files/stock_db.h"

int stock_videos_screen(enum stock_video_kind kind, int title);
/* How many videos each Videos category holds, and the video playlists */
struct stock_video_census {
    int movies, tv_shows, music_videos, playlists;
};
void stock_videos_census(struct stock_video_census *census);
int stock_photos_screen(void);
int stock_photos_settings(void);
int stock_videos_settings(void);

struct gui_synclist;
int stock_media_list_action(int action, struct gui_synclist *lists);
#endif
