/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The synchronised-lyrics screen: three lines of the playing track, the one
 * being sung between the one before and the one after.
 ****************************************************************************/

#ifndef _LYRIC_VIEWER_H
#define _LYRIC_VIEWER_H

/* One animated page for local timed/plain text and remote now/lyrics.
 * SELECT or MENU returns; an empty playback source fades in a message. */
int lyric_viewer(void);

#endif /* _LYRIC_VIEWER_H */
