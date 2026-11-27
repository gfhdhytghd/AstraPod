/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * System > Agents: the herdr agent dashboard and remote, over PodLink.
 ****************************************************************************/
#ifndef AGENTS_SCREEN_H
#define AGENTS_SCREEN_H
#define AGENTS_EXIT_MENU 0
#define AGENTS_EXIT_OTHER 1
#define AGENTS_EXIT_DEFERRED 5
#define AGENTS_SWITCH_WPS 2
#define AGENTS_SWITCH_COVERS 3
#define AGENTS_SWITCH_REMOTE_WPS 4
int agents_screen(void);
void agents_screen_shortcut_entry(void);
#endif
