/* Native widget adapters for foreground plugins. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_WIDGETS_H
#define ASTRAPOD_ROCK_WIDGETS_H

#include "plugins/rock_display.h"
#include "rbunicode.h"
#include "widgets/keyboard.h"
#include "widgets/list.h"
#include "widgets/text_box.h"
#include "widgets/yesno.h"

void rock_flush_settings(void);
void rock_begin_settings(void);

struct rock_native_scope rock_modal_begin(void);
void rock_modal_end(const struct rock_native_scope *scope);
void rock_modal_end_with_resume(const struct rock_native_scope *scope,
                                enum rock_display_resume resume);

__typeof__(kbd_input) rock_kbd_input;
int rock_kbd_input_raw(char *buffer, int buflen);
__typeof__(gui_syncyesno_run) rock_yesno;
__typeof__(view_text) rock_view_text;
bool rock_set_color(const char *title, unsigned *color, unsigned banned);

void rock_list_release(bool present);
void rock_list_discard(void);
__typeof__(gui_synclist_init) rock_list_init;
__typeof__(gui_synclist_draw) rock_list_draw;
__typeof__(gui_synclist_do_button) rock_list_button;
__typeof__(gui_synclist_set_title) rock_list_title;
__typeof__(gui_synclist_scroll_stop) rock_list_stop;

#define ROCK_RAW_VOID(name, args, call) void rock_raw_##name args;
#define ROCK_RAW_VALUE(type, name, args, call) type rock_raw_##name args;
#include "rock_raw_lcd.def"
#undef ROCK_RAW_VOID
#undef ROCK_RAW_VALUE
__typeof__(lcd_putsxyf) rock_raw_lcd_putsxyf;
__typeof__(lcd_putsf) rock_raw_lcd_putsf;

#endif
