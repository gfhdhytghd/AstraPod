/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Browsers for the stock media libraries.
 ****************************************************************************/
#ifndef STOCK_MEDIA_H
#define STOCK_MEDIA_H

#include "files/stock_db.h"

int stock_videos_screen(enum stock_video_kind kind, int title);
int stock_photos_screen(void);
int stock_photos_settings(void);

struct gui_synclist;
int stock_media_list_action(int action, struct gui_synclist *lists);
#endif
