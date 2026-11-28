/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/list.h
 * Copyright (C) 2005 by Kevin Ferrare
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Interface to the list widget -- gui_synclist and the callback types
 * every list screen implements.
 ****************************************************************************/

#ifndef _GUI_LIST_H_
#define _GUI_LIST_H_

#include "config.h"
#include "input/action.h"
#include "draw/icon.h"
#include "draw/screen_access.h"
#include "draw/line.h"

struct dim;

#define SCROLLBAR_WIDTH global_settings.scrollbar_width

enum synclist_cursor
{
    SYNCLIST_CURSOR_NOSTYLE = 0,
    SYNCLIST_CURSOR_INVERT,
    SYNCLIST_CURSOR_COLOR,
    SYNCLIST_CURSOR_GRADIENT,
};

/*
 * The gui_list is based on callback functions, if you want the list
 * to display something you have to provide it a function that
 * tells it what to display.
 * There are three callback function :
 * one to get the text, one to get the icon and one to get the color
 */

/*
 * Icon callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  Returns a pointer to the icon, the value inside it is used to display the
 *          icon after the function returns.
 * Note : we use the ICON type because the real type depends of the plateform
 */
typedef enum themable_icons list_get_icon(int selected_item, void * data);
/*
 * Album art callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  - size : the requested thumbnail dimensions to decode/scale to
 *  Returns a pointer to a ready-to-draw bitmap, or NULL if this item has
 *          no album art (e.g. it isn't an album row, or none was found).
 *  Note: the returned pointer is only valid until the next call into the
 *        callback's backing cache, so it must be drawn immediately.
 */
typedef const struct bitmap *list_get_albumart(int selected_item, void *data,
                                              struct dim *size);
/*
 * Text callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  - buffer : a buffer to put the resulting text on it
 *             (The content of the buffer may not be used by the list, we use
 *             the return value of the function in all cases to avoid filling
 *             a buffer when it's not necessary)
 *  - buffer_len : length of the buffer
 * Returns a pointer to a string that contains the text to display
 */
typedef const char * list_get_name(int selected_item, void * data,
                                   char * buffer, size_t buffer_len);
/*
 * Draw callback
 *  - display : functions supplied depends on the screen call originated from (typ: MAIN)
 *  - list_info : a pointer to an internal struct containing item display information
 */
/* owner drawn lists need to know this info */
struct list_putlineinfo_t {
    int x;
    int y;
    int item_indent;
    int item_offset;
    int line;

    int icon;
    int icon_width;

    struct screen *display;
    struct viewport *vp;
    struct line_desc *linedes;
    struct gui_synclist * list;
    const char *dsp_text;

    bool is_selected;
    bool is_title;
    bool show_cursor;
    bool have_icons;
};

typedef void list_draw_item(struct list_putlineinfo_t *list_info);
/*
 * Voice callback
 *  - selected_item : an integer that tells the number of the item to speak
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 * Returns an integer, 0 means success, ignored really...
 */
typedef int list_speak_item(int selected_item, void * data);
/*
 * Color callback
 *  - selected_item : an integer that tells the number of the item to display
 *  - data : a void pointer to the data you gave to the list when you
 *           initialized it
 *  Returns an int with the lower 16 bits representing the color to display the
 *          selected item, negative value for default coloring.
 */
typedef int list_get_color(int selected_item, void * data);

/*
 * Row kind callback: describe what choosing the row does.
 */
enum list_row_kind
{
    LIST_ROW_PLAIN,             /* the list says nothing more */
    LIST_ROW_MENU,              /* opens another menu */
    LIST_ROW_TOGGLE,            /* a setting Select changes in place */
    LIST_ROW_SETTING,           /* a setting Select opens a picker for */
    LIST_ROW_ACTION,            /* does something, or goes somewhere */
    LIST_ROW_COMMAND,           /* a library list's own row: [Random] and kin */
    LIST_ROW_CONTAINER,         /* a library item that lists its contents */
    LIST_ROW_TRACK,             /* something that plays */
    LIST_ROW_FILE,              /* any other file */
    LIST_ROW_OPTION,            /* one of a setting's values, in its picker */
    LIST_ROW_CUSTOM,            /* a view from tagnavi_custom.config */
    LIST_ROW_CUSTOM_MENU,       /* opens a menu from tagnavi_custom.config */
};
typedef enum list_row_kind list_get_kind(int selected_item, void * data);
/* Range callback: where a number setting's value sits in its
 * range, 0 to 100, or -1 on any other row. */
typedef int list_get_range(int selected_item, void * data);

/*
 * Playing callback: whether the row is what is
 * playing now.
 */
typedef bool list_is_playing(int selected_item, void * data);

/* What a split-screen menu shows beside its rows for the selected row, under
 * a theme with style.menu_pane=split. A list without the callback stays full
 * width. The callback may set `art` to the picture's enum stock_art_id; it
 * is -1 on entry. */
enum list_pane_kind
{
    LIST_PANE_INFO,             /* the row's name, tile and value */
    LIST_PANE_CATEGORY,         /* the row's picture and name */
    LIST_PANE_ART,              /* a slideshow of the library's covers */
    LIST_PANE_PLAYING,          /* the playing track */
    LIST_PANE_DEVICE,           /* the player's name and free space */
    LIST_PANE_SHUFFLE,          /* the player's name and song count */
    LIST_PANE_PICTURE,          /* what menu_pane_set_picture() last gave */
    LIST_PANE_PHOTO,            /* photos from menu_pane_set_photos() */
};
typedef enum list_pane_kind list_get_pane(int selected_item, void * data,
                                          int *art);

/* Emit a child's title (-1) and labels. A pending background scan returns
 * false so the idle renderer polls it again with the same token. */
typedef void list_child_text(int row, const char *text, void *context);
typedef bool list_get_child_texts(int item, void *data, int rows,
                                 unsigned token, list_child_text *text,
                                 void *context);

/* The icon-sized status cell is already clipped and active for this callback. */
typedef bool list_draw_status(int selected_item, void *data,
                              struct screen *display, struct viewport *vp);

/* Optional summary above the rows. The list borrows these fields while it is
 * shown. The artwork pointer is consumed before any other artwork request. */
struct list_header
{
    const char *title, *subtitle, *detail;
    const struct bitmap *(*cover)(void *data, struct dim *size);
    void *data;
};

struct list_selection_color
{
    /* text color, in native lcd format
     * (convert with LCD_RGBPACK() if necessary) */
    unsigned text_color;
    /* only STYLE_GRADIENT supported set line_color & line_end_color the same
     * for solid color, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned line_color;
    unsigned line_end_color;
    /* viewport foreground and background, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned fg_color;
    unsigned bg_color;
    /* To enable:
     * call gui_synclist_set_sel_color(gui_synclist*, list_selection_color*)
     * If using the default viewport you should call
     * gui_synclist_set_sel_color(gui_synclist*, NULL) when finished */
};

struct gui_synclist
{
    /*flags to hold settings show: icons, scrollbar etc..*/
    int scrollbar;
    int cursor_style;
    bool show_icons;
    bool keyclick;
    bool talk_menu;
    bool wraparound;
    bool scroll_paginated;
    /* Load missing visible glyphs before the frame can reach the panel. */
    bool require_row_glyphs;
    /* whether the text of the whole items of the list have to be
     * scrolled or only for the selected item */
    bool scroll_all;
    int nb_items;
    int selected_item;

    int start_item[NB_SCREENS]; /* the item that is displayed at the top of the screen */
    /* the number of lines that are selected at the same time */
    int selected_size;
    /* the number of pixels each line occupies (including optional padding on touchscreen */
    int line_height[NB_SCREENS];
    int offset_position[NB_SCREENS]; /* the list's screen scroll placement in pixels */
    long scheduled_talk_tick, dirty_tick;

    list_get_icon *callback_get_item_icon;
    list_get_albumart *callback_get_item_albumart;
    void (*callback_prefetch_art)(int item, void *data, int dim);
    /* Uniform row height override in pixels; 0 uses the theme.
     * Lets a list (e.g. the database album list) ask for taller rows without a
     * per-item callback -- every row is this height. */
    int line_height_override;
    bool value_below;                  /* two-line row with a subtitle */
    list_get_name *callback_get_item_name;
    list_speak_item *callback_speak_item;
    list_draw_item *callback_draw_item;

    /* The data that will be passed to the callback function YOU implement */
    void * data;
    /* The optional title, set to NULL for none */
    const char * title;
    /* Optional title icon */
    enum themable_icons title_icon;

    int title_color;
    list_get_color *callback_get_item_color;
    struct list_selection_color *selection_color;
    struct viewport *parent[NB_SCREENS];

    /* Optional row roles and annotations. */
    list_get_kind *callback_get_item_kind;
    list_get_name *callback_get_item_value;     /* a setting row's value */
    list_get_range *callback_get_item_range;    /* ...and where it sits */
    list_is_playing *callback_item_is_playing;
    list_get_name *callback_get_item_description;
    list_get_name *callback_get_item_subtitle;
    list_get_name *callback_get_index_key;
    list_draw_status *callback_draw_status;
    list_get_pane *callback_get_item_pane;
    /* Number from the last row up when a list is sorted newest first. The
     * oldest track stays numbered 1. */
    bool count_down;
    bool unnumbered;
    bool show_notes;
    bool default_viewport;
    const struct list_header *header;
    const char *footer;
    const char *empty_title, *empty_hint;

    /* Changes whenever the items may have: init, the item count or the kind
     * callback. Unique across lists, so a reused struct gets a new one. */
    unsigned generation;
    /* Lets an idle list load the glyphs of the pages its rows open. */
    list_get_child_texts *callback_child_texts;
};


extern void list_init(void);

extern void gui_synclist_init(
    struct gui_synclist * lists,
    list_get_name callback_get_item_name,
    void * data,
    bool scroll_all,
    int selected_size,
    struct viewport parent[NB_SCREENS] /* NOTE: new screens should NOT set this to NULL */
    );
extern void gui_synclist_set_nb_items(struct gui_synclist * lists, int nb_items);
extern void gui_synclist_set_icon_callback(struct gui_synclist * lists, list_get_icon icon_callback);
extern void gui_synclist_set_albumart_callback(struct gui_synclist * lists, list_get_albumart albumart_callback);
/* Uniform row height in pixels. A zero height uses the theme. */
extern void gui_synclist_set_row_height(struct gui_synclist * lists, int height);
/* The row height after applying the list's optional override. */
int list_item_height(struct gui_synclist *list, enum screen_type screen);
extern void gui_synclist_set_voice_callback(struct gui_synclist * lists, list_speak_item voice_callback);
extern void gui_synclist_set_viewport_defaults(struct viewport *vp, enum screen_type screen);
extern void gui_synclist_set_color_callback(struct gui_synclist * lists, list_get_color color_callback);
extern void gui_synclist_set_sel_color(struct gui_synclist * lists, struct list_selection_color *list_sel_color);
/* Optional row kind, displayed value, and playback state. */
extern void gui_synclist_set_row_callbacks(struct gui_synclist * lists,
                                           list_get_kind kind_callback,
                                           list_get_name value_callback,
                                           list_is_playing playing_callback);
/* Optional setting value as a percentage. */
extern void gui_synclist_set_range_callback(struct gui_synclist * lists,
                                            list_get_range range_callback);
/* Track numbering policy for this list. */
extern void gui_synclist_set_count_down(struct gui_synclist * lists,
                                        bool count_down);
extern void gui_synclist_set_unnumbered(struct gui_synclist * lists,
                                        bool unnumbered);
/* Optional notes for the selected item. */
extern void gui_synclist_set_description_callback(struct gui_synclist * lists,
                                                  list_get_name callback);
/* Optional letter-jump key for a row: a key of quickscroll_key_of() or a
 * group label such as a year. Empty or NULL files the row by its name. */
extern void gui_synclist_set_index_key_callback(struct gui_synclist * lists,
                                                list_get_name callback);
/* Optional content pane beside the rows. */
extern void gui_synclist_set_pane_callback(struct gui_synclist * lists,
                                           list_get_pane callback);
/* Optional texts of the page each row opens, so their glyphs load before
 * the page does. */
extern void gui_synclist_set_child_texts_callback(struct gui_synclist * lists,
                                                  list_get_child_texts callback);
/* Optional status cell for each visible row. */
extern void gui_synclist_set_status_callback(struct gui_synclist * lists,
                                             list_draw_status callback);
void gui_synclist_set_header(struct gui_synclist *list, const struct list_header *header);
void gui_synclist_set_notes(struct gui_synclist *list, bool show);
void gui_synclist_draw_status(struct gui_synclist *list);
/* True while the selection is still travelling. Its frames reach the LCD
 * themselves, so a wait for input need not submit the whole screen. */
bool gui_synclist_moving(const struct gui_synclist *list);
/* Repaint a region of the main list's last backdrop without submitting it. */
bool gui_synclist_paint_background(const struct gui_synclist *list,
                                   const struct viewport *region);
extern void gui_synclist_speak_item(struct gui_synclist * lists);
extern int gui_synclist_get_nb_items(struct gui_synclist * lists);

extern int  gui_synclist_get_sel_pos(struct gui_synclist * lists);

extern void gui_synclist_draw(struct gui_synclist * lists);
/* Set up and draw the opening frame. */
void gui_synclist_draw_settled(struct gui_synclist *lists);
/* What a draw does first when the list or its UI viewport has changed: lay
 * out the rows and re-choose the top one. For a caller about to set the top
 * row itself, which the next draw would otherwise undo. */
void gui_synclist_layout(struct gui_synclist *lists);
/* Suppress the LCD flush of the next list draw(s) -- render only, no update. */
void gui_synclist_inhibit_flush(bool inhibit);
bool gui_synclist_flush_inhibited(void);
extern void gui_synclist_scroll_stop(struct gui_synclist *lists);
extern void gui_synclist_select_item(struct gui_synclist * lists,
                                     int item_number);
/* Force the row shown at the top of the screen, overriding the scroll margin
 * gui_synclist_select_item() applies. Call it after selecting; it holds only
 * until the selection next moves. */
extern void gui_synclist_set_top_item(struct gui_synclist * lists,
                                      int item_number);
/* How many rows fit on screen. Needed to tell whether a top row chosen by the
 * caller would push the selection out of sight -- the list only re-adjusts
 * itself when the selection moves, so an unreachable top row would simply
 * stay wrong. */
extern int gui_synclist_get_nb_lines(struct gui_synclist * lists,
                                     enum screen_type screen);
extern void gui_synclist_add_item(struct gui_synclist * lists);
extern void gui_synclist_del_item(struct gui_synclist * lists);
extern void gui_synclist_set_title(struct gui_synclist * lists, const char * title,
                                   enum themable_icons icon);

extern bool gui_synclist_keyclick_callback(int action, void* data);
/*
 * Do the action implied by the given button,
 * returns true if the action was handled.
 * NOTE: *action may be changed regardless of return value
 */
extern bool gui_synclist_do_button(struct gui_synclist * lists, int *action);

/* If the list has a pending postponed scheduled announcement, that
   may become due before the next get_action tmieout. This function
   adjusts the get_action timeout appropriately. */
extern int list_do_action_timeout(struct gui_synclist *lists, int timeout);
/* For a screen that may leave after list_do_action_timeout() without calling
   gui_synclist_do_button(): drops retained paint state for a closing list. */
extern void gui_synclist_forget(struct gui_synclist *lists);
/* This one combines a get_action call (with timeout overridden by
   list_do_action_timeout) with the gui_synclist_do_button call, for
   convenience. */
extern bool list_do_action(int context, int timeout,
                           struct gui_synclist *lists, int *action);


/** Simplelist implementation.
    USe this if you dont need to reimplement the list code,
    and just need to show a list
 **/

struct simplelist_info {
    const char *title; /* title to show on the list */
    int  count; /* number of items in the list, each item is selection_size high */
    int selection_size; /* list selection size, usually 1 */
    bool scroll_all;
    bool hide_theme;
    bool speak_onshow; /* list speaks first item or 'empty list' */
    bool fruit_nav;
    bool page_with_left_right;
    bool wps_on_play;  /* PLAY exits with selection == -2, meaning "go to the
                          playing screen". Only for callers that can act on it:
                          the settings lists have no GO_TO_* to return, and
                          there PLAY is ignored rather than meaning "back". */
    int  timeout;
    int  selection; /* the item to select when the list is first displayed */
    int  top_item;  /* the row shown first, or -1 for wherever the selection
                       puts it; a short list stays at its top */
                    /* when the list is exited, this will be set to the
                       index of the last item selected, or -1 if the list
                       was exited with ACTION_STD_CANCEL                  */
    int (*action_callback)(int action, struct gui_synclist *lists); /* can be NULL */
        /* action_callback notes:
            action == the action pressed by the user
                _after_ gui_synclist_do_button returns.
            lists == the lists struct so the callback can get selection and count etc. */
    enum themable_icons title_icon;
    list_get_icon *get_icon; /* can be NULL */
    list_get_name *get_name; /* NULL if you're using simplelist_addline() */
    list_speak_item *get_talk; /* can be NULL to not speak */
    list_get_color *get_color;
    list_get_kind *get_kind; /* can be NULL; every row is then plain */
    list_get_name *get_value; /* can be NULL; a setting row's value */
    list_get_name *get_subtitle; /* a second line below the row label */
    list_get_range *get_range; /* can be NULL; a setting row's percentage */
    list_get_albumart *get_albumart; /* can be NULL for no art */
    int row_height; /* 0 uses the theme */
    struct list_selection_color *selection_color;
    void *callback_data; /* data for callbacks */
};

#define SIMPLELIST_MAX_LINES 48
#define SIMPLELIST_MAX_LINELENGTH 48

/** The next three functions are used if the text is mostly static.
    These should be called in the action callback for the list.
 **/
/* reset the amount of lines shown in the list to 0
   Only needed if simplelist_info.get_name == NULL */
void simplelist_reset_lines(void);
/* get the current amount of lines shown */
int simplelist_get_line_count(void);
/* add a line in the list. */
void simplelist_addline(const char *fmt, ...) ATTRIBUTE_PRINTF(1,2);
void simplelist_setline(const char *text);
/* setup the info struct. members not setup in this function need to be assigned manually
   members set in this function:
    info.selection_size = 1;
    info.scroll_all = false;
    info.top_item = -1;
    info.hide_theme = false;
    info.speak_onshow = true;
    info.action_callback = NULL;
    info.title_icon = Icon_NOICON;
    info.get_icon = NULL;
    info.get_name = NULL;
    info.get_voice = NULL;
    info.get_color = NULL;
    info.list_selection_color = NULL;
    info.timeout = HZ/10;
    info.selection = 0;
*/
void simplelist_info_init(struct simplelist_info *info, char* title,
                          int count, void* data);

/* show a list.
   if list->action_callback != NULL it is called with the action ACTION_REDRAW
    before the list is dislplayed for the first time.
   Returns true when the screen was left for the root menu -- MENU, or a USB
    attach. A caller that would otherwise return to its own parent must pass
    that on (GO_TO_ROOT, MENU_ATTACHED_USB or ONPLAY_MAINMENU, whichever its
    own caller reads) rather than swallowing it.
   info->selection is the chosen row, -1 if the list was left without one, or
    -2 for PLAY when wps_on_play is set. */
bool simplelist_show_list(struct simplelist_info *info);

#endif /* _GUI_LIST_H_ */
