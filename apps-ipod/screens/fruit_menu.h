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
/* Whether `menu` is one of the stock tree's menus with a pane of its own,
 * and what that pane shows beside its row `item` there. */
bool fruit_menu_owns(const struct menu_item_ex *menu);
/* Whether `menu` is any menu of the stock tree, which shows every row
 * whatever Settings Mode is. */
bool fruit_menu_is_stock(const struct menu_item_ex *menu);
enum list_pane_kind fruit_menu_item_pane(const struct menu_item_ex *menu,
                                         const struct menu_item_ex *item,
                                         int *art);

#endif
