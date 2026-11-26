/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to fruit_about.c.
 ****************************************************************************/

#ifndef _FRUIT_ABOUT_H_
#define _FRUIT_ABOUT_H_

/* Settings > About as the stock firmware draws it. 0 to go back to the
 * menu, or a GO_TO_* code (root_menu.h), e.g. GO_TO_ROOT after USB. */
int fruit_about_screen(void);

#endif /* _FRUIT_ABOUT_H_ */
