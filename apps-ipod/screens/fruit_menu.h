/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Fruit Company's menu tree: the stock iPod Classic menus, in place of
 * AstraPod's main menu while the theme sets style.menu_tree=stock.
 ****************************************************************************/
#ifndef FRUIT_MENU_H
#define FRUIT_MENU_H

#include <stdbool.h>
#include "widgets/menu.h"
#include "widgets/list.h"

/* Whether the current theme asks for the stock tree. */
bool fruit_menu_active(void);
/* The stock main menu. */
const struct menu_item_ex *fruit_menu_root(void);
/* The shared category menu for themes with the Classic video controls. */
const struct menu_item_ex *fruit_menu_videos(void);
/* Whether `menu` is one of the stock tree's menus with a pane of its own,
 * and what that pane shows beside its row `item` there. */
bool fruit_menu_owns(const struct menu_item_ex *menu);
/* Whether `menu` is any menu of the stock tree, which shows every row
 * whatever Settings Mode is. */
bool fruit_menu_is_stock(const struct menu_item_ex *menu);
enum list_pane_kind fruit_menu_item_pane(const struct menu_item_ex *menu,
                                         const struct menu_item_ex *item,
                                         int *art);
/* Set the pane of a row titled `title` that leads to every video: a
 * slideshow of their covers. Return LIST_PANE_PICTURE after it. */
void fruit_videos_pane(const char *title);
/* For a Settings row stock changes in place: the row's kind and the value
 * it shows, and, on Select, stepping it to its next value. False for any
 * other row. */
bool fruit_menu_describe(const struct menu_item_ex *item,
                         enum list_row_kind *kind, const char **value);
bool fruit_menu_step(const struct menu_item_ex *item);

#endif
