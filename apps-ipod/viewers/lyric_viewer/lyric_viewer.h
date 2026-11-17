/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The synchronised-lyrics screen: three lines of the playing track, the one
 * being sung between the one before and the one after.
 ****************************************************************************/

#ifndef _LYRIC_VIEWER_H
#define _LYRIC_VIEWER_H

#include <stdbool.h>
#include <stdint.h>

/* One animated page for local timed/plain text and remote now/lyrics.
 * SELECT or MENU returns; an empty playback source fades in a message. */
int lyric_viewer(void);
#define LYRICS_PERF_PATH ROCKBOX_DIR "/lyrics-perf.txt"
#ifdef HAVE_USB_PODLINK
void lyrics_remote_track(const char *id, uint32_t generation, uint32_t library, long duration);
void lyrics_remote_cancel(void);
bool lyrics_remote_step(void);
#endif

#endif /* _LYRIC_VIEWER_H */
