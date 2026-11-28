/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Fruit Company's Voice Memos: the stock main menu row that shows while a
 * headset microphone is plugged in, its menu, the recording screen and the
 * menu a paused recording shows.
 ****************************************************************************/
#ifndef VOICE_MEMOS_H
#define VOICE_MEMOS_H

#include <stdbool.h>
#include "config.h"
#include "widgets/menu.h"
#include "widgets/list.h"

#if defined(HAVE_RECORDING) && defined(HAVE_MIKEY_REMOTE)
/* The main menu's Voice Memos row, a menu. */
extern const struct menu_item_ex voice_memos_menu;
/* Whether the rows of `menu`, the main menu or Voice Memos, changed since
 * the last call: the microphone came or went, or the memos changed. */
bool voice_memos_refresh(const struct menu_item_ex *menu);
/* Whether `menu` is a Voice Memos menu, and the pane beside `item` in a
 * stock menu: true after setting it. */
bool voice_memos_owns(const struct menu_item_ex *menu);
bool voice_memos_pane(const struct menu_item_ex *menu,
                      const struct menu_item_ex *item);
/* The Quality row's value, and on Select its next value. */
bool voice_memos_describe(const struct menu_item_ex *item,
                          enum list_row_kind *kind, const char **value);
bool voice_memos_step(const struct menu_item_ex *item);
/* Whether a recording is paused, which the status bar shows. */
bool voice_memos_paused(void);
/* Whether Play/Pause plays the selected `row` of `menu`: a memo's row. */
bool voice_memos_plays(const struct menu_item_ex *menu,
                       const struct menu_item_ex *row);
#else
static inline bool voice_memos_refresh(const struct menu_item_ex *menu)
{ (void)menu; return false; }
static inline bool voice_memos_owns(const struct menu_item_ex *menu)
{ (void)menu; return false; }
static inline bool voice_memos_pane(const struct menu_item_ex *menu,
                                    const struct menu_item_ex *item)
{ (void)menu; (void)item; return false; }
static inline bool voice_memos_describe(const struct menu_item_ex *item,
                                        enum list_row_kind *kind,
                                        const char **value)
{ (void)item; (void)kind; (void)value; return false; }
static inline bool voice_memos_step(const struct menu_item_ex *item)
{ (void)item; return false; }
static inline bool voice_memos_paused(void) { return false; }
static inline bool voice_memos_plays(const struct menu_item_ex *menu,
                                     const struct menu_item_ex *row)
{ (void)menu; (void)row; return false; }
#endif

#endif
