/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to app_list.c: the Games, Other Apps and Demos lists.
 ****************************************************************************/
#ifndef _APP_LIST_H
#define _APP_LIST_H

enum app_list_kind { APP_LIST_GAMES, APP_LIST_OTHER, APP_LIST_DEMOS };

/* GO_TO_PREVIOUS when the user backs out; another GO_TO_* code to leave
 * for that screen (GO_TO_ROOT after USB, GO_TO_WPS, ...). */
int app_list_screen(enum app_list_kind kind);
/* How many apps the list holds: looked for the first time it is asked, and
 * again each time the list opens. */
int app_list_count(enum app_list_kind kind);

#endif /* _APP_LIST_H */
