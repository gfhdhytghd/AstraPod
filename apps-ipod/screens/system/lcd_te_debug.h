/***************************************************************************
 * GNU General Public License (version 2+)
 ****************************************************************************/

#ifndef LCD_TE_DEBUG_H
#define LCD_TE_DEBUG_H

#include <stdbool.h>

bool lcd_te_debug_screen(void);
int lcd_te_debug_save_report(void);
bool lcd_te_debug_valid_timing(unsigned int timing);
bool lcd_te_debug_toggle(void);
bool lcd_te_debug_timing(void);
const char *lcd_te_debug_sync_state(void);

#endif /* LCD_TE_DEBUG_H */
