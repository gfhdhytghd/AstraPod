/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/folder_select.h
 * Public entry point for the core folder-tree picker (widgets/folder_select.c),
 * ported from the db_folder_select plugin.
 * GNU General Public License (version 2+)
 *
 * Interface to folder_select.c.
 ****************************************************************************/

#ifndef _FOLDER_SELECT_H_
#define _FOLDER_SELECT_H_

#include <stdbool.h>
#include <stddef.h>

/* Show a folder tree under `header_text`; `setting` is a ':'-delimited list of
 * selected paths, loaded on entry and (after a "save changes?" prompt) written
 * back, up to `setting_len` bytes. Returns true if the setting was changed. */
bool folder_select(char *header_text, char *setting, int setting_len);

/* Run the picker with caller-owned, aligned scratch for the duration. */
bool folder_select_in_buffer(char *header_text, char *setting, int setting_len,
                             void *scratch, size_t scratch_size);

#endif /* _FOLDER_SELECT_H_ */
