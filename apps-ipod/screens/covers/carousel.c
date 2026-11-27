/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/pictureflow/pictureflow.c
 * Copyright (C) 2007 Jonas Hurrelmann (j@outpo.st)
 * Copyright (C) 2007 Nicolas Pennequin
 * Copyright (C) 2007 Ariya Hidayat (ariya@kde.org) (original Qt Version)
 * GNU General Public License (version 2+)
 *
 * Original code: http://code.google.com/p/pictureflow/
 *
 * The cover-flow engine shared by album_covers.c and artist_portraits.c:
 * slide cache, 3D projection, scrolling, input loop and worker thread.
 * Each screen supplies a model.
 *
 * The stock camera projects the 3D carousel into Q16 screen coordinates. Flat layout
 * and album-panel texture sampling use PFreal with 10 fractional bits.
 *
 * Slides are decoded on a worker thread into a fixed set of cache slots.
 * Which slot holds which slide is tracked by the lla_* helpers -- a doubly
 * linked list whose "pointers" are array indices rather than addresses, so
 * the whole structure survives being moved and costs no allocation. Reading
 * the cache requires buf_ctx_lock().
 *
 * Parts, in order:
 *   - fixed-point and geometry constants, module state, theme colours
 *   - config load/save, and the database availability check
 *   - fixed-point arithmetic and trig
 *   - pixel row output helpers for the three source formats
 *   - scrolling text lines
 *   - the slide cache: the .pfraw format, the worker thread, and the
 *     index-linked-list that tracks slot use
 *   - the projection: offsets, per-slide render, and the full-screen compose
 *   - animation: stepping between slides and settling
 *   - init and cleanup, then the input loop and carousel_run() entry point
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>           /* rand, for the random pick */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "string-extra.h"
#include "system/library_files.h"
#include "config.h"
#ifdef SIMULATOR
#include "sim/shim-cider.h"
#endif
#include "system.h"          /* MIN/MAX/ALIGN_BUFFER/ALIGN_DOWN */
#include "rbpaths.h"
#include "input/action.h"           /* get_action/get_custom_action/button_mapping */
#include "draw/screen_access.h"    /* FOR_NB_SCREENS, screens[] */
#include "kernel.h"           /* threads, mutex, queue, current_tick */
#include "core_alloc.h"       /* buflib types for buf_ctx (see init()) */
#include "system/app_buffer.h"           /* app_claim_buffer() -- see init() */
#include "database/tagcache.h"
#include "playlist/playlist.h"
#include "playlist/catalog.h"
#include "files/filetypes.h"
#include "widgets/list_select.h"
#include "settings/settings.h"
#include "draw/color.h"
#include "draw/line.h"              /* text_fit() */
#include "draw/img_filter.h"    /* the slides' own treatment */
#include "draw/round_rect.h"    /* the album panel's selection marker */
#include "audio/sound_feedback.h" /* keyclick_click, for the random spin */
#include "lang.h"
#include "widgets/splash.h"
#include "draw/viewport.h"
#include "draw/color.h"           /* color_contrast */
#include "system/activity.h"
#include "system/app_util.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "system/hash.h"
#include "usb.h"                     /* SYS_USB_CONNECTED handling */
#include "screens/context_menu.h"           /* context_menu_show_playlist_cat/menu */
#include "metadata/albumart.h"         /* struct dim */
#include "metadata/art_cache.h"   /* shared database-driven thumbnail cache */
#include "metadata.h"         /* struct mp3entry, get_metadata */
#include "dir.h"
#include "file.h"
#include "widgets/yesno.h"            /* gui_syncyesno_run */
#include "root_menu.h"
#include "input/agents_shortcut.h"
#include "screens/browse/browser.h"
#include "screens/browse/browser_db.h"
#include "screens/playback/track_info.h"          /* browse_id3 */
#include "audio.h"            /* audio_status, audio_current_track */
#include "screens/playback/quick_screen.h"
#include "lcd.h"
#include "font.h"
#include "bidi.h"
#include "draw/icon_bitmaps.h"
#include "widgets/menu.h"             /* MENUITEM_STRINGLIST, do_menu */
#include "draw/bmp.h"              /* read_bmp_file */
#include "draw/jpeg_load.h"        /* read_jpeg_file */
#include "power.h"
#include "backlight.h"        /* backlight_set_timeout(_plugged) */
#include "cpu.h"
#include "metadata/album_palette.h"
#include "metadata/album_blur.h"
#include "ui/frame.h"
#include "ui/statusbar.h"
#include "ui/native_theme_runtime.h"
#include "ui/theme_art.h"
#include "ui/native_wps.h"
#include "ui/perf.h"
#include "ui/retained.h"
#include "album_covers.h"
#include "carousel.h"
#include "panel_perf.h"
#include "stock_motion.h"
#include "reflection.h"
#include "database/path_key.h"     /* the shared engine: carousel_idx, the model */
#include "screens/playback/cider.h"
#include "screens/playback/cider_library.h"
#include "speech/talk.h"
#include "ui/animation.h"
#include "ui/pace.h"
#include "lcd-vsync.h"
#ifdef HAVE_LCD_VSYNC
#include "lcd-te-s5l8702.h"
#endif
#include "ui/scene.h"
#include "ui/menu_pane.h"
#include "screens/fruit_menu.h"

/******************************* Globals ***********************************/
static fb_data *lcd_fb;

#define PF_PREV ACTION_STD_PREV
#define PF_PREV_REPEAT ACTION_STD_PREVREPEAT
#define PF_NEXT ACTION_STD_NEXT
#define PF_NEXT_REPEAT ACTION_STD_NEXTREPEAT
#define PF_SELECT ACTION_STD_OK
#define PF_BACK ACTION_STD_CANCEL
#define PF_MENU ACTION_STD_MENU
#define PF_WPS ACTION_TREE_WPS
#define PF_JMP ACTION_LISTTREE_PGDOWN
#define PF_JMP_PREV ACTION_LISTTREE_PGUP

#define PF_QUIT (LAST_ACTION_PLACEHOLDER + 1)
#define PF_TRACKLIST (LAST_ACTION_PLACEHOLDER + 2)
#define PF_SORTING_NEXT (LAST_ACTION_PLACEHOLDER + 3)
#define PF_SORTING_PREV (LAST_ACTION_PLACEHOLDER + 4)
#define PF_RANDOM (LAST_ACTION_PLACEHOLDER + 5)

/* Both this fork's targets are CONFIG_KEYPAD == IPOD_4G_PAD (confirmed via
 * firmware/export/config/ipod6g.h and ipodvideo.h), so only that keypad's
 * button mapping is kept -- every other target's mapping the original
 * cross-target plugin supported has been dropped rather than left as dead
 * conditional code. */
const struct button_mapping pf_context_album_scroll[] =
{
    {PF_JMP_PREV,     BUTTON_LEFT,                BUTTON_NONE},
    {PF_JMP_PREV,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {PF_JMP,          BUTTON_RIGHT,               BUTTON_NONE},
    {PF_JMP,          BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_RIGHT},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_PLUGIN|1)
};

const struct button_mapping pf_context_buttons[] =
{
    {ACTION_STD_QUICKSCREEN, BUTTON_MENU|BUTTON_REPEAT, BUTTON_MENU},
    {PF_QUIT,         BUTTON_MENU|BUTTON_REL,     BUTTON_MENU},
#ifdef IPOD_ACCESSORY_PROTOCOL
    {ACTION_STD_QUICKSCREEN, BUTTON_RC_MENU|BUTTON_REPEAT, BUTTON_RC_MENU},
    {PF_QUIT,         BUTTON_RC_MENU|BUTTON_REL,    BUTTON_RC_MENU},
    /* Remote Left/Right flip one album; holding keeps flipping */
    {PF_PREV,         BUTTON_RC_LEFT,                BUTTON_NONE},
    {PF_PREV_REPEAT,  BUTTON_RC_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {PF_NEXT,         BUTTON_RC_RIGHT,               BUTTON_NONE},
    {PF_NEXT_REPEAT,  BUTTON_RC_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_NONE,     BUTTON_RC_LEFT|BUTTON_REL,     BUTTON_RC_LEFT},
    {ACTION_NONE,     BUTTON_RC_RIGHT|BUTTON_REL,    BUTTON_RC_RIGHT},
#endif
    {PF_SORTING_NEXT, BUTTON_SELECT|BUTTON_MENU,  BUTTON_NONE},
    {PF_SORTING_PREV, BUTTON_SELECT|BUTTON_PLAY,  BUTTON_NONE},
    /* Once per hold: later repeats carry a REPEAT prebutton, so they fall
     * through to CONTEXT_TREE, which claims them. */
    {PF_RANDOM,       BUTTON_PLAY|BUTTON_REPEAT,  BUTTON_PLAY},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_TREE)
};
const struct button_mapping *pf_contexts[] =
{
    pf_context_album_scroll,
    pf_context_buttons
};

#define LCD_BUF lcd_fb
#define G_BRIGHT(y) LCD_RGBPACK(y,y,y)
#define BUFFER_WIDTH LCD_WIDTH
/* pix_t, the index structs, the scroll-line enum, the SUCCESS/ERROR_* codes and
 * struct carousel_model now live in carousel.h (shared with artist_portraits.c). */

static pix_t pf_bg_color;     /* theme background, replaces G_BRIGHT(0) */
pix_t pf_fg_color;     /* theme foreground, replaces G_BRIGHT(255) */

/* Pre-extracted RGB565 channel masks for fade_color() performance */
static unsigned int pf_bg_rb;
static unsigned int pf_bg_g;   /* pf_bg_color & 0x7e0 */

static inline unsigned int fade_red_blue(pix_t pixel)
{
    /* Keep the red fraction out of the blue lane at arbitrary Q8 light. */
    return ((pixel & 0xf800u) << 5) | (pixel & 0x1fu);
}

static pix_t pf_color(const struct ui_theme *theme, const struct ui_album_palette *palette,
                      enum ui_color_role role)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static void pf_update_dynamic_colors(void)
{
    struct ui_theme theme;
    struct ui_album_palette palette;
    ui_theme_snapshot(&theme);
    album_palette_snapshot(&palette);
    /* The setting picks which of the theme's two colours fills the screen;
     * the captions always take the other. */
    bool bg_from_fg = global_settings.album_covers_background
                        == CAROUSEL_BG_FOREGROUND;
    pix_t bg = pf_color(&theme, &palette, UI_COLOR_BACKGROUND);
    pix_t fg = pf_color(&theme, &palette, UI_COLOR_FOREGROUND);
    pf_bg_color = bg_from_fg ? fg : bg;
    pf_fg_color = bg_from_fg ? bg : fg;
    if (global_settings.album_covers_background == CAROUSEL_BG_CUSTOM)
    {
        unsigned custom = global_settings.album_covers_custom_color;
        unsigned white = LCD_RGBPACK(255, 255, 255);
        unsigned black = LCD_RGBPACK(0, 0, 0);

        pf_bg_color = (pix_t)custom;
        pf_fg_color = (pix_t)(color_contrast(white, custom) >=
                              color_contrast(black, custom) ? white : black);
    }
    pf_bg_rb = fade_red_blue(pf_bg_color);
    pf_bg_g  = pf_bg_color & 0x7e0;
}

/* for fixed-point arithmetic, we need minimum 32-bit long
   long long (64-bit) might be useful for multiplication and division */
#define PFREAL_SHIFT 10
#define PFREAL_ONE (1 << PFREAL_SHIFT)
#define PFREAL_HALF (PFREAL_ONE >> 1)

#define IANGLE_MAX 1024
#define IANGLE_MASK 1023

#define DISPLAY_HEIGHT (LCD_HEIGHT * 2 / 3)
#define DISPLAY_WIDTH MAX((LCD_HEIGHT * LCD_PIXEL_ASPECT_HEIGHT / \
    LCD_PIXEL_ASPECT_WIDTH / 2), (LCD_WIDTH * 2 / 5))
/* The album flip's camera, further back so the card turning towards the
 * viewer stays in proportion. */
#define FLIP_DIST 240
#define FLIP_DIST_R (FLIP_DIST << PFREAL_SHIFT)


/* Caption lines are inset from the screen edges by this much. A caption long
 * enough to scroll has to overhang its box to reveal its tail, so the inset
 * is a clip region (see carousel_text_begin()) and not just a starting
 * offset: without it a scrolling album name runs right into the bezel. */
#define PF_TEXT_MARGIN 20
#define PF_TEXT_WIDTH  (LCD_WIDTH - 2 * PF_TEXT_MARGIN)

#define MAX_SLIDES_COUNT 10
/* Flat slots per side and the caption prefetch radius. */

/* --- The flat view ---------------------------------------------------------
 * Covers lie face-on, all the same size and all on the same line: two piles
 * squared off against the screen edges, and the current cover on top in the
 * middle. The piles are flat -- every card in one sits exactly on the one
 * below, so only the top of each is ever visible, and there is no stack to see.
 *
 * A card at cx = 0 occupies the middle DISPLAY_WIDTH columns, so a pile at
 * -PF_PILE_CX runs off the left edge with its inner strip underneath the centre
 * card. That overlap is what makes the centre card read as lying *on* the pile;
 * with the two exactly abutting, the boundary is ambiguous.
 *
 * Timing is the flat view's own (see flat_pace()). The 3D view's ease curve
 * spends most of a flip crawling through its last tenth, which reads as a
 * rotation settling and as a stall for a card being dealt; and a fixed duration
 * is wrong in the other direction, since it caps how fast the user can get
 * through the covers. The flat view runs at a constant rate per cover, set from
 * how fast the user is actually scrolling: PF_FLAT_FLIP when they are taking
 * their time, down to PF_FLAT_MIN, below which there is no animation left to
 * see and the covers just change in the middle. Both scale with the scroll
 * speed setting. */
#define PF_PILE_OVERLAP 16
#define PF_PILE_CX      ((DISPLAY_WIDTH - PF_PILE_OVERLAP) * PFREAL_ONE)
#define PF_FLAT_FLIP ANIM_F(15)
/* Below this duration a cover snaps directly into the centre. */
#define PF_FLAT_MIN  ANIM_F(4)

/* The deal: the cover on top slides off to its pile, and the top of the other
 * pile slides into the middle behind it, PF_DEAL_LEAD of a flip later.
 *
 * That lead is the whole of it, and it is a narrow window. A cover at rest lies
 * on top of both piles, overlapping each by PF_PILE_OVERLAP, so the two moving
 * covers start out overlapping by that much -- and the lead is what pulls them
 * apart. Too much and the background shows between them, which is glaring, and
 * on the 5G makes the tearing far worse (a large area changing colour rather
 * than shifting). Too little and they travel locked together, which reads as
 * the whole row sliding rather than one card being dealt.
 *
 * At 0.11 the pair stays about two pixels overlapped for the middle of the
 * flip: no background, and the moment they swap over in the stacking order --
 * which they must, since the arriving cover ends up on top of the one it
 * started underneath -- has almost nothing to swap, so it cannot be seen. */
#define PF_DEAL_LEAD (PFREAL_ONE * 11 / 100)
#define PF_DEAL_SPAN (PFREAL_ONE - PF_DEAL_LEAD)

/* How far a flip may stretch as the user stops, as a multiple of the pace they
 * were scrolling at -- see flat_pace(). */
#define PF_FLAT_SETTLE 3

/* The cover geometry is code-defined. Native theme tokens supply the
 * foreground, background and caption fonts. */
#define THREAD_STACK_SIZE DEFAULT_STACK_SIZE + 0x200

/* Timing overlay, off. Set to 1 to draw a two-line readout over the covers:
 * frames a second, microseconds inside the render, the worst gap between two
 * frames in ms, microseconds inside the flush, this screen's own share of wall
 * clock, slides fetched by the loader, then the clear and slide loops timed
 * apart, nanoseconds of slide loop per pixel written, and the CPU clock with
 * its boost count.
 *
 * Worth keeping, because none of that is guessable: measured on the 5G, the
 * flush is ~6% of wall clock and the render ~94%. Two rules if it is switched
 * back on: nothing timed may yield inside its timing, since USEC_TIMER is
 * wall clock and would bill the codec's time to the render; and
 * ns-per-pixel means nothing without the clock beside it, being ~43 cycles at
 * 80 MHz and ~16 at 30. */
#define PF_SHOW_STATS 0

#define EV_EXIT 9999
#define EV_WAKEUP 1337

/* Ordered Bayer dithering for 24-bit to RGB565 conversion */
static const unsigned char pf_dither_table[16] =
    {   0,192, 48,240, 12,204, 60,252,  3,195, 51,243, 15,207, 63,255 };
#define PF_DITHERY(y) (pf_dither_table[(y) & 15] & 0xAA)
#define PF_DITHERX(x) (pf_dither_table[(x) & 15])
#define PF_DITHERXDY(x,dy) (PF_DITHERX(x) ^ dy)

/* some magic numbers for cache_version. */

/* current version for cover cache */
#define CONFIG_FILE LIB_COVERS_FILE

/** structs we use */
struct slide_data {
    int slide_index;
    PFreal cx;
    PFreal cy;      /* downward shift of a flat pile */
    int alpha;      /* flat view opacity */
    int cached_slot; /* last cache slot this slide resolved to (see surface()) */
};

struct slide_cache {
    int index;      /* index of the cached slide */
    int hid;        /* handle ID of the cached slide */
    unsigned int version;
    short next; /* "next" slide, with LRU last */
    short prev; /* "previous" slide */
};

struct rect {
    int left;
    int right;
    int top;
    int bottom;
};

struct load_slide_event_data {
    int slide_index;
    int cache_index;
};

struct pf_slide_cache
{
    struct slide_cache cache[SLIDE_CACHE_SIZE];
    int free;
    int used;
    int left_idx;
    int right_idx;
    int center_idx;
};

struct pf_scroll_line_info {
    long ticks;         /* number of ticks between each move */
    long delay;         /* number of ticks to delay starting scrolling */
    int step;           /* pixels to move */
    long next_scroll;   /* tick of the next move */
};

struct caption_span { int left, right; };

struct pf_scroll_line {
    struct caption_span ink;
    int ink_top, ink_end;
    uint32_t text_hash;
    int font;
    int width;          /* width of the string */
    int offset;         /* x coordinate of the string */
    int step;           /* 0 if scroll is disabled. otherwise, pixels to move */
    long start_tick;    /* tick when to start scrolling */
};

struct pfraw_header {
    int32_t width;          /* bmap width in pixels */
    int32_t height;         /* bmap height in pixels */
};

/* Scratch for the placeholder slide, carved out of the app buffer by
 * init(). create_empty_slide() is the only thing that draws into it. */
static void *pf_placeholder_buf;
struct pf_config_t pf_cfg;

/** below we allocate the memory we want to use **/

static pix_t *buffer; /* for now it always points to the lcd framebuffer */
static int pf_height;    /* viewport height (LCD_HEIGHT minus status bar) */
static int pf_half_height;      /* rows of drawable (post text-margin) height above center */
static int pf_lower_half;       /* rows of drawable (post text-margin) height below center */
static int pf_draw_y_shift;     /* rows skipped below pf_vp_y for a TOP text caption's margin */
static int pf_caption_band;     /* rows the covers give up to the caption */
/* The caption's own rows, measured from the top of the viewport. Not the same
 * as the band: the band is a reservation, and carousel_caption_layout() places
 * the text against the covers, rows away from where the band starts. Sized for
 * a two-line caption, which contains the one-line case -- both sit against the
 * covers, so the taller block covers the shorter. */
static int pf_text_y, pf_text_h;
/* Rows the last render actually wrote to, again from the top of the viewport.
 * A caption can only be repainted on its own if the covers stayed clear of it,
 * and the covers are the wrong shape to answer that from the layout alone: art
 * keeps its aspect ratio, so how tall a cover comes out is a property of the
 * album, not of the screen. */
static int pf_slides_y0, pf_slides_y1;
static int pf_title_font = -1, pf_body_font = -1;

int carousel_caption_font(bool bold)
{
    if (bold)
        return pf_title_font >= 0 ? pf_title_font : font_get_ui_bold();
    return pf_body_font >= 0 ? pf_body_font : screens[SCREEN_MAIN].getuifont();
}

static int pf_vp_y;             /* viewport y-offset (status bar height) */
static struct viewport pf_vp;   /* PF rendering viewport (below status bar) */
static bool pf_show_statusbar;  /* only the stock look shows the status bar */
/* The stock look: Cover Flow as the stock firmware draws it, under a theme
 * with the stock Now Playing layout. The status bar lies over the top
 * pf_chrome_h rows of the full-screen viewport and nothing else draws
 * there, and every slide carries its reflection (slide_reflection()).
 * Fixed for the whole visit, because the slide cache is sized by it. */
static bool pf_stock;
static int pf_chrome_h;
/* The stock look's selected panel row: the theme's selection gradient, a
 * colour a row of the panel's pitch. */
static pix_t panel_bar[ALBUM_BLUR_ROW_PITCH];
static struct ui_scene pf_scene;
/* Home's album colours, artwork and theme when pf_scene was taken of it. */
static unsigned pf_scene_palette, pf_scene_art, pf_scene_theme;
/* pf_scene holds Home's split menu, which the stock look slides aside. */
static bool pf_scene_redrawn, pf_scene_split;
static void bar_drop_free(void);
static bool pf_wps_entry;
/* Fruit Company's caption is gone while the menu returns, and on the way in
 * waits with its status bar until `caption_reveal`. */
static bool pf_caption_hidden, caption_waits;
static uint32_t caption_reveal;
static bool flat_view(void);
static bool pf_shown, pf_entry_prepared, cover_resume_dirty;
static int pf_neighbor_light = 256;
static struct {
    struct ui_scene sheet;
    const struct carousel_model *model;
    int index;
    bool panel_open, returning, restored_pixels;
} wps_handover;

static void close_album_panel(void);
static void release_playback_scene(void);
static bool playback_panel_return(void);

void carousel_prepare_transition(void)
{
    if (pf_entry_prepared || wps_handover.returning)
        return;
    ui_scene_discard(&pf_scene);
    memset(&pf_scene, 0, sizeof(pf_scene));
    ui_scene_capture_modal(&pf_scene);
    pf_scene_split = menu_pane_themed() && !cider_active();
    pf_scene_palette = album_palette_generation();
    pf_scene_art = album_art_generation();
    pf_scene_theme = ui_theme_generation();
    pf_scene_redrawn = false;
    pf_entry_prepared = true;
}

/* Whether Home has changed since pf_scene was taken of it, as it does when
 * another album starts playing. */
static bool pf_scene_stale(void)
{
    return (album_palette_pending() && !pf_scene_redrawn) ||
           pf_scene_palette != album_palette_generation() ||
           pf_scene_art != album_art_generation() ||
           pf_scene_theme != ui_theme_generation();
}

/* Draw a stale Home afresh into pf_scene, so that leaving still fades into
 * it. The new album's colours are waited for, up to PF_SCENE_WAIT; past
 * that Home is drawn with what it has. False when pf_scene still holds an
 * out-of-date page, as it does for a parent other than Home. */
#define PF_SCENE_WAIT (HZ / 2)
static bool pf_scene_refresh(void)
{
    if (pf_scene.backdrop_handle <= 0 || !pf_scene_stale())
        return true;
    long until = current_tick + PF_SCENE_WAIT;
    album_palette_refresh();
    while (album_palette_pending() && TIME_BEFORE(current_tick, until))
    {
        sleep(1);
        album_palette_refresh();
    }
    if (ui_scene_repaint_backdrop(&pf_scene))
    {
        pf_scene_palette = album_palette_generation();
        pf_scene_art = album_art_generation();
        pf_scene_theme = ui_theme_generation();
        pf_scene_redrawn = true;
    }
    return !pf_scene_stale();
}

void carousel_cancel_transition(void)
{
    if (pf_entry_prepared)
    {
        ui_scene_discard(&pf_scene);
        pf_entry_prepared = false;
    }
}

static void show_carousel_entry(void)
{
    if (pf_entry_prepared)
    {
        if (flat_view())
            ui_scene_show_modal(&pf_scene, "covers-up");
        pf_entry_prepared = false;
    }
    pf_shown = true;
}

bool carousel_wps_pending(void)
{
    return wps_handover.model != NULL;
}

bool carousel_wps_returning(void)
{
    return wps_handover.returning;
}

void carousel_wps_begin(void)
{
    if (flat_view())
        ui_scene_show_modal(&wps_handover.sheet, "covers-wps-up");
    else if (wps_handover.panel_open && !playback_panel_return())
    {
        /* A failed local play returns to the panel before WPS starts. */
        close_album_panel();
        wps_handover.panel_open = false;
    }
}

int carousel_wps_back(void)
{
    if (!wps_handover.model)
        return GO_TO_PREVIOUS;
    wps_handover.returning = true;
    return wps_handover.model->external ? GO_TO_CIDER : GO_TO_ALBUM_COVERS;
}

void carousel_wps_end(bool back)
{
    if (back)
    {
        if (flat_view())
            wps_handover.restored_pixels =
                ui_scene_close_modal(&wps_handover.sheet, true, "covers-wps-down");
        else
            pf_wps_entry = true;
        return;
    }
    release_playback_scene();
    if (wps_handover.model)
    {
        if (wps_handover.panel_open)
            close_album_panel();
        ui_scene_discard(&wps_handover.sheet);
        ui_scene_discard(&pf_scene);
        pf_wps_entry = false;
        wps_handover.model = NULL;
        wps_handover.returning = false;
    }
}
/* Theme state left pushed for cleanup() to pop. cleanup() also runs after a
 * failed init(), so it cannot assume there is one. */
static bool pf_theme_pushed;
/* Whether this screen holds a cpu_boost(). init() takes one for the load;
 * album_covers_loop() keeps it only while there is drawing to do. */
static bool pf_boosted;

static struct slide_data center_slide;
static struct slide_data left_slides[MAX_SLIDES_COUNT];
static struct slide_data right_slides[MAX_SLIDES_COUNT];
/* Where the covers are along the row, as a slide index << 16. */
static int slide_frame;
/* Nonzero while the covers move, and in Flat the direction of the flip. */
static int step;
/* The slide the wheel has picked, which the covers move towards. */
static int target;
static unsigned pf_motion_scale = UI_THEME_MOTION_ONE;

static unsigned motion_frames(unsigned frames)
{
    return MAX(1u, (frames * pf_motion_scale + UI_THEME_MOTION_ONE / 2) /
                    UI_THEME_MOTION_ONE);
}
/* Flat's flip speed is a step per this many authored frames, see
 * scroll_speed(). */
#define PF_SPEED_FRAMES 2
/* Frames are presented on the deadlines these give, one pace per animated
 * loop, since each learns its own rate and wake phase. The covers are sent
 * apart from the caption so a moving band fits one panel refresh; the album
 * panel sends the whole screen, which does not, and settles at half rate. */
static struct ui_pace flow_pace, panel_pace, wps_pace;
/* The flip's clock, see update_scroll_animation(): its start, the last frame
 * sampled from it and the step's carried remainder. */
static uint32_t anim_clock;
static unsigned anim_sampled_frame;
static int anim_frac;
/* Direction of the gap on screen. Reversing travel can walk the same
 * geometry backwards, then choose a new gap when it crosses the centre. */
static int anim_pose_step;
int center_index = 0; /* index of the slide that is in the center */
static int number_of_slides;
/* Flat view pacing: when the user last asked for a cover and how far apart they
 * have been asking, plus the two per-flip decisions those feed -- whether this
 * cover is going past too quickly to be worth dealing, and whether any card is
 * currently off its resting place. See flat_pace(). */
static uint32_t pf_flat_last_input;
static int  pf_flat_interval;
static bool pf_flat_snap;
static bool pf_flat_moving;
/* Whether the arriving cover has taken the top of the stack from the one it is
 * replacing. See flat_animate(). */
static bool pf_flat_handover;

/* Random pick (held PLAY): how many recent picks it avoids, and how far short
 * of the pick the scroll starts, by Random Spin Length -- the spin is a cut to
 * there and a scroll the rest of the way, so it takes the same time in a
 * library of any size. */
#define PF_RANDOM_HISTORY 8
static const int pf_random_spin[] = { 6, 12, 24 };
/* The recent picks as slide indices, newest first. An index means nothing once
 * the order changes, so they are forgotten on a re-sort, a rebuild and a
 * switch to the other carousel. */
static int pf_random_recent[PF_RANDOM_HISTORY];
static int pf_random_count;
static const struct carousel_model *pf_random_model;
/* A random spin under way: each cover it passes clicks, as the wheel would.
 * Any other move of the carousel ends it. */
static bool pf_random_spinning;

static struct pf_slide_cache pf_sldcache;
static unsigned int slide_cache_generation;

struct carousel_workspace {
    unsigned long loader_stack[THREAD_STACK_SIZE / sizeof(long)];
};

/* The loader's stack address must survive yields and buflib compaction. */
static struct carousel_workspace *workspace;
/* queue (as array) for scheduling load_surface */

static int empty_slide_hid;

/* Index of the "coverflow" size in the shared albumart cache (art_sizes.h),
 * resolved once in init(); -1 if unavailable (falls back to the local pfraw
 * cache). */
static int pf_cover_size_idx = -1;

unsigned int thread_id;
struct event_queue thread_q;

static struct buflib_context buf_ctx;

struct db_summary_t carousel_idx;

static bool thread_is_running;
static bool wants_to_quit;

/* Bumped by the loader thread each time a slide near enough to be on screen
 * gets its surface. The main loop draws a frame only when the picture has
 * actually changed, and a cover arriving is one of the ways it changes -- the
 * one nothing else would tell it about. Slides loaded further out are read long
 * before they are looked at and would only ask for frames showing nothing new.
 */
static volatile unsigned int pf_slides_loaded;

#if PF_SHOW_STATS
/* Every slide the loader fetches, near the centre or not -- pf_slides_loaded
 * counts only the ones worth a frame, and it is the far ones that cost the
 * disk. */
static volatile unsigned int pf_loads_total;
/* The clear and the slide loops timed apart, and the pixels the slide loops
 * actually wrote. Time per pixel is the number that matters: the loops are
 * maybe fifteen cycles of arithmetic each, so anything far above that is the
 * memory system, not the code. */
static unsigned long pf_clear_us;
static unsigned long pf_slides_us;
static unsigned long pf_px;
#endif

/*
    Prevent picture loading thread from allocating
    buflib memory while the main thread may be
    performing buffer-shifting operations.
*/
static struct mutex buf_ctx_mutex;

static struct pf_scroll_line_info scroll_line_info;
static struct pf_scroll_line scroll_lines[PF_MAX_SCROLL_LINES];

/* What the caption currently on screen was built for. Forgotten alongside the
 * scroll lines in init_scroll_lines() -- see carousel_caption_changed(). */
static int  caption_index;
static int  caption_variant;
static bool caption_valid;
/* The caption rows the last paint changed, from the top of pf_vp. */
static int caption_rows_y, caption_rows_h;
/* The rows paint_caption() leaves to carousel_album_caption() to clear, so an
 * unchanged artist line can stay. Zero high while paint_caption() clears. */
static int caption_clear_y, caption_clear_h;
static struct caption_span previous_ink, painted_ink;
static int painted_top = INT_MAX, painted_end;
static bool painted_ink_known;
static const char *caption_line;

/* Whether this loop's model draws through carousel_album_caption(). */
static bool caption_clears_itself;
/* The artist line on screen, so a new caption can tell whether it changes. */
static struct {
    uint32_t text;
    int x, y, font;
    pix_t fg;
    bool valid, album_in_box;
} caption_artist;

enum ePFS{ePFS_ARTIST = 0, ePFS_ALBUM};

/* The cover scroll pose is independent of the album panel's flip pose. */
enum pf_states {
    pf_idle = 0,
    pf_scrolling
};

static int pf_state;

/** code */
static void pf_clear_band(int y, int h);
static bool free_slide_prio(int prio);
static inline bool load_and_prepare_surface(int slide_index,int cache_index,int prio);
bool load_new_slide(void);
int load_surface(int);
static void free_all_slide_prio(int prio);

/* The artist-portraits model lives in artist_portraits.c (over the same
 * carousel engine). The active model is selected by the entry point
 * (album_covers / artist_portraits) before init() runs. */
static const struct carousel_model *model = NULL;

/* Exact app-buffer snapshot across a track-list visit, off the UI stack. */
static int resume_handle;
static void *owned_buffer, *resume_placeholder;
static size_t owned_bytes;
static const struct carousel_model *resume_model;
static struct db_summary_t resume_index;
static bool resume_stock;
void carousel_forget_resume(void)
{
    if (resume_handle > 0) core_free(resume_handle);
    resume_handle = 0;
    resume_model = NULL;
}
static bool carousel_save_resume(void)
{
    carousel_forget_resume();
    if (thread_is_running || !owned_buffer || !owned_bytes) return false;
    int handle = core_alloc(owned_bytes);
    if (handle <= 0) return false;
    memcpy(core_get_data(handle), owned_buffer, owned_bytes);
    resume_handle = handle;
    resume_model = model;
    resume_stock = pf_stock;
    resume_index = carousel_idx;
    resume_placeholder = pf_placeholder_buf;
    return true;
}
static bool carousel_restore_resume(void *buffer, size_t bytes)
{
    if (resume_handle <= 0) return false;
    if (resume_model != model || resume_stock != pf_stock ||
        owned_buffer != buffer || owned_bytes != bytes)
    {
        carousel_forget_resume();
        return false;
    }
    memcpy(buffer, core_get_data(resume_handle), bytes);
    carousel_idx = resume_index;
    pf_placeholder_buf = resume_placeholder;
    carousel_forget_resume();
    return true;
}

static inline void buf_ctx_lock(void)
{
    mutex_lock(&buf_ctx_mutex);
}

static inline void buf_ctx_unlock(void)
{
    mutex_unlock(&buf_ctx_mutex);
}

/* The carousel's own settings file, kept apart from global_settings because
 * they are the engine's state rather than anything the settings menus present.
 *
 * Deliberately bare-bones: one fixed path and no version-gated migration, so
 * an unreadable or outdated file falls back to config_set_defaults() rather
 * than being converted. The line parsing is the core's own
 * (read_line/settings_parseline). */
#define TYPE_INT  1
#define TYPE_ENUM 2
#define TYPE_BOOL 4

struct configdata
{
    int type;
    int min;
    int max;    /* enum: number of values */
    union {
        int *int_p;
        bool *bool_p;
    };
    const char *name;
    const char * const *values; /* enum only */
};

static void config_load(const char *filename, const struct configdata *cfg,
                        int num_items)
{
    int fd;
    int i, j;
    char *name;
    char *val;
    char buf[128];
    int tmp;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
        return;

    while (read_line(fd, buf, sizeof(buf)) > 0)
    {
        /* Both are left untouched for a comment or a line with no colon, and
         * they are uninitialised locals -- a config truncated by a power loss
         * mid-save is enough to reach the strcmp below with a stray pointer. */
        if (!settings_parseline(buf, &name, &val))
            continue;

        for (i = 0; i < num_items; i++)
        {
            if (strcmp(cfg[i].name, name))
                continue;
            switch (cfg[i].type)
            {
                case TYPE_INT:
                    tmp = atoi(val);
                    if (tmp >= cfg[i].min && tmp <= cfg[i].max)
                        *cfg[i].int_p = tmp;
                    break;
                case TYPE_BOOL:
                    *cfg[i].bool_p = (bool)atoi(val);
                    break;
                case TYPE_ENUM:
                    for (j = 0; j < cfg[i].max; j++)
                    {
                        if (!strcmp(cfg[i].values[j], val))
                            *cfg[i].int_p = j;
                    }
                    break;
            }
            break;
        }
    }
    close(fd);
}

static void config_save(const char *filename, const struct configdata *cfg,
                        int num_items)
{
    int fd = creat(filename, 0666);
    int i;

    if (fd < 0)
        return;

    for (i = 0; i < num_items; i++)
    {
        switch (cfg[i].type)
        {
            case TYPE_INT:
                fdprintf(fd, "%s: %d\n", cfg[i].name, *cfg[i].int_p);
                break;
            case TYPE_BOOL:
                fdprintf(fd, "%s: %d\n", cfg[i].name, (int)*cfg[i].bool_p);
                break;
            case TYPE_ENUM:
                fdprintf(fd, "%s: %s\n", cfg[i].name,
                         cfg[i].values[*cfg[i].int_p]);
                break;
        }
    }
    close(fd);
}

static struct configdata config[] =
{
    { TYPE_INT, 0, 100, { .int_p = &pf_cfg.cache_version }, "cache version", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &pf_cfg.last_album }, "last album", NULL },
};

#define CONFIG_NUM_ITEMS (sizeof(config) / sizeof(struct configdata))

void pf_config_save(void)
{
    config_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS);
}

static void pf_config_load(void)
{
    config_load(CONFIG_FILE, config, CONFIG_NUM_ITEMS);
}

static bool check_database(void)
{
    bool needwarn = true;
    int spin = 5;

    struct tagcache_stat *stat = tagcache_get_stat();

    /* The album and artist tables come with the RAM copy, not with the
     * database, and every model reads them. */
    while ( !(stat->initialized && stat->ready && tagcache_is_in_ram()) )
    {
        if (--spin > 0)
        {
            sleep(HZ/5);
        }
        else if (needwarn)
        {
            /* Waiting on the database is activity, not an error -- show it in
             * the status bar rather than over the carousel. */
            needwarn = false;
            ui_set_working(true);
        }
        else
        {
            ui_set_working(false);
            return false;
        }

        yield();
        stat = tagcache_get_stat();
    }
    ui_set_working(false);
    return true;
}

static void config_set_defaults(struct pf_config_t *cfg)
{
     cfg->cache_version = CACHE_REBUILD;
     cfg->last_album = 0;
}

static inline PFreal fmul(PFreal a, PFreal b)
{
    return (a*b) >> PFREAL_SHIFT;
}

/* ARMv5+ has a clz instruction equivalent to our function.
 */
#if (defined(CPU_ARM) && (ARM_ARCH > 4))
static inline int clz(uint32_t v)
{
    return __builtin_clz(v);
}

/* Otherwise, use our clz, which can be inlined */
#else
static const char clz_lut[16] = { 4, 3, 2, 2, 1, 1, 1, 1,
                                  0, 0, 0, 0, 0, 0, 0, 0 };
/* This clz is based on the log2(n) implementation at
 * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogLookup
 * It is not any faster than the one above, but trades 16B in the lookup table
 * for a savings of 12B per each inlined call.
 */
static inline int clz(uint32_t v)
{
    int r = 28;
    if (v >= 0x10000)
    {
        v >>= 16;
        r -= 16;
    }
    if (v & 0xff00)
    {
        v >>= 8;
        r -= 8;
    }
    if (v & 0xf0)
    {
        v >>= 4;
        r -= 4;
    }
    return r + clz_lut[v];
}
#endif

/* Return the maximum possible left shift for a signed int32, without
 * overflow
 */
static inline int allowed_shift(int32_t val)
{
    uint32_t uval = val ^ (val >> 31);
    return clz(uval) - 1;
}

/* Calculate num/den, with the result shifted left by PFREAL_SHIFT, by shifting
 * num and den before dividing.
 */
static inline PFreal fdiv(PFreal num, PFreal den)
{
    int shift = allowed_shift(num);
    shift = MIN(PFREAL_SHIFT, shift);
    num <<= shift;
    den >>= PFREAL_SHIFT - shift;
    return num / den;
}

/* n / FLIP_DIST, for n >= 0. 240 is 16 * 15, and 0x88888889 >> 35 is the
 * exact reciprocal of 15 for any 32-bit numerator. */
static inline int div_flip_dist(int n)
{
    if (FLIP_DIST != 240 || n < 0)
        return n / FLIP_DIST;
    return (int)(((uint64_t)((unsigned)n >> 4) * 0x88888889u) >> 35);
}

/* num / den for 0 <= num < 2^31 and den > 0, given recip = 0xffffffff / den.
 * The product is never more than one short of the quotient, which the test
 * after it puts right -- so one division buys any number of quotients by the
 * same divisor. */
static inline int div_by_recip(unsigned int num, unsigned int den,
                               unsigned int recip)
{
    unsigned int q = ((uint64_t)num * recip) >> 32;

    if ((q + 1) * den <= num)
        q++;
    return q;
}

#define fmin(a,b) (((a) < (b)) ? (a) : (b))
#define fmax(a,b) (((a) > (b)) ? (a) : (b))
#define fbound(min,val,max) (fmax((min),fmin((max),(val))))


/* warning: regenerate the table if IANGLE_MAX and PFREAL_SHIFT are changed! */
static const short sin_tab[] = {
        0,   100,   200,   297,   392,   483,   569,   650,
      724,   792,   851,   903,   946,   980,  1004,  1019,
     1024,  1019,  1004,   980,   946,   903,   851,   792,
      724,   650,   569,   483,   392,   297,   200,   100,
        0,  -100,  -200,  -297,  -392,  -483,  -569,  -650,
     -724,  -792,  -851,  -903,  -946,  -980, -1004, -1019,
    -1024, -1019, -1004,  -980,  -946,  -903,  -851,  -792,
     -724,  -650,  -569,  -483,  -392,  -297,  -200,  -100,
        0
};

static inline PFreal fsin(int iangle)
{
    iangle &= IANGLE_MASK;

    int i = (iangle >> 4);
    PFreal p = sin_tab[i];
    PFreal q = sin_tab[(i+1)];
    PFreal g = (q - p);
    return p + g * (iangle-i*16)/16;
}

static inline PFreal fcos(int iangle)
{
    return fsin(iangle + (IANGLE_MAX >> 2));
}

static void output_row_8_transposed(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint8_rgb *qp = (struct uint8_rgb*)row_in;
    unsigned r, g, b;
    int col = 0;
    uint8_t dy = PF_DITHERY(row);
    for (; dest < end; dest += ctx->bm->height, col++)
    {
        int delta = ctx->dither ? PF_DITHERXDY(col, dy) : 127;
        r = (31 * qp->red + (qp->red >> 3) + delta) >> 8;
        g = (63 * qp->green + (qp->green >> 2) + delta) >> 8;
        b = (31 * qp->blue + (qp->blue >> 3) + delta) >> 8;
        qp++;
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}

/* read_image_file() is called without FORMAT_TRANSPARENT so
 * it's safe to ignore alpha channel in the next two functions */
static void output_row_32_transposed(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    int r, g, b;
    int col = 0;
    uint8_t dy = PF_DITHERY(row);
    for (; dest < end; dest += ctx->bm->height, col++)
    {
        int delta = ctx->dither ? PF_DITHERXDY(col, dy) : 127;
        r = SC_OUT(qp->r, ctx);
        g = SC_OUT(qp->g, ctx);
        b = SC_OUT(qp->b, ctx);
        qp++;
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}

static void output_row_32_transposed_fromyuv(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    int col = 0;
    uint8_t dy = PF_DITHERY(row);
    for (; dest < end; dest += ctx->bm->height, col++)
    {
        unsigned r, g, b, y, u, v;
        int delta = ctx->dither ? PF_DITHERXDY(col, dy) : 127;
        y = SC_OUT(qp->b, ctx);
        u = SC_OUT(qp->g, ctx);
        v = SC_OUT(qp->r, ctx);
        qp++;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}

static unsigned int get_size(struct bitmap *bm)
{
    return bm->width * bm->height * sizeof(pix_t);
}

const struct custom_format format_transposed = {
    .output_row_8 = output_row_8_transposed,
    .output_row_32 = {
        output_row_32_transposed,
        output_row_32_transposed_fromyuv
    },
    .get_size = get_size
};

static const struct button_mapping* get_context_map(int context)
{
    /* action_code_lookup() ORs in CONTEXT_REMOTE for any button with
     * BUTTON_REMOTE bits set (true on this target, which supports an
     * accessory/headphone remote) -- without masking it here too,
     * pf_contexts[context & ~CONTEXT_PLUGIN] indexes ~2 billion entries
     * past this 2-element array on a remote button press. */
    context &= ~(CONTEXT_LOCKED | CONTEXT_REMOTE);
    return pf_contexts[context & ~CONTEXT_PLUGIN];
}

/* scrolling */
static void init_scroll_lines(void)
{
    int i;
    static const char scroll_tick_table[16] = {
     /* Hz values:
        1, 1.25, 1.55, 2, 2.5, 3.12, 4, 5, 6.25, 8.33, 10, 12.5, 16.7, 20, 25, 33 */
        100, 80, 64, 50, 40, 32, 25, 20, 16, 12, 10, 8, 6, 5, 4, 3
    };

    scroll_line_info.ticks = scroll_tick_table[global_settings.scroll_speed];
    scroll_line_info.step = global_settings.scroll_step;
    scroll_line_info.delay = global_settings.scroll_delay / (HZ / 10);
    scroll_line_info.next_scroll = current_tick;
    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
        scroll_lines[i].step = 0;

    /* The captions go with them. Everything above has just been forgotten, so
     * a model still believing its caption is current would skip the
     * set_scroll_line() that rebuilds it. */
    caption_valid = false;
}

/* See carousel.h. A random spin is a show of the covers going past, so its
 * caption names each one as it reaches the middle. */
int carousel_caption_index(void)
{
    if (pf_state != pf_scrolling || pf_random_spinning)
        return center_index;
    return target;
}

/* See carousel.h. Records what it was asked about, so a model calls it once
 * per draw and uses the answer for every line of its caption. */
bool carousel_caption_changed(int index, int variant)
{
    if (caption_valid && caption_index == index && caption_variant == variant)
        return false;

    caption_index = index;
    caption_variant = variant;
    caption_valid = true;
    return true;
}

/* An external provider can fill the centre row without moving the selection
 * or completing any artwork. Its slide version changes from missing to ready
 * in that case. Rebuild both caption scroll lines as well as repainting them;
 * their usual index/variant key still describes the placeholder. */
static bool update_caption_version(unsigned int *seen)
{
    if (!model->art_version)
        return false;
    unsigned int next = model->art_version(carousel_caption_index());
    if (*seen == next)
        return false;
    *seen = next;
    caption_valid = false;
    return true;
}

const char *carousel_caption_fit(const char *str, char *buf, size_t size)
{
    caption_line = global_settings.scrolling_enabled ? str
         : text_fit(str, buf, size, lcd_getfont(), PF_TEXT_WIDTH);
    return caption_line;
}

/* The model's title is a phrase, resolved each time it is shown. */
static const char *model_title(void)
{
    if (model->external)
        return cider_remote_name();
    const char *title = pf_stock && model->stock_title ? model->stock_title
                                                       : model->title;
    return P2STR((unsigned char *)title);
}

void set_scroll_line(const char *str, enum pf_scroll_line_type type)
{
    struct pf_scroll_line *s = &scroll_lines[type];
    int id = lcd_getfont();
    font_lock(id, true);
    struct font *font = font_get(id);
    if (font->pbf.file_size)
    {
        /* PBF advances place the line; ink includes bearings and combining marks. */
        struct font_run run = {0};
        s->ink = (struct caption_span){INT_MAX, INT_MIN};
        s->ink_top = s->ink_end = 0;
        s->width = 0;
        for (const ucschar_t *p = bidi_l2v((const unsigned char *)str, 1); p && *p; p++)
        {
            struct font_glyph glyph;
            s->width = font_run_next(font, &run, *p, &glyph);
            if (glyph.bits && glyph.height && glyph.width)
            {
                s->ink_top = MIN(s->ink_top, glyph.y);
                s->ink_end = MAX(s->ink_end, glyph.y + glyph.height);
                s->ink.left = MIN(s->ink.left, glyph.x);
                s->ink.right = MAX(s->ink.right, glyph.x + glyph.width);
            }
        }
        s->font = id;
        s->text_hash = fnv1a_str(str);
        font_lock(id, false);
    }
    else
    {
        font_lock(id, false);
        s->font = INT_MIN;
        s->width = lcd_getstringsize(str, NULL, NULL);
    }
    s->step = 0;
    s->offset = 0;
    s->start_tick = current_tick + scroll_line_info.delay;
    /* Offsets are relative to the inset caption box, so 0 is PF_TEXT_MARGIN
     * in from the screen edge -- carousel_text_begin() supplies the origin. */
    if (PF_TEXT_WIDTH - s->width < 0)
        s->step = scroll_line_info.step;
    else
        s->offset = (PF_TEXT_WIDTH - s->width) / 2;
}

/* Clip and shift drawing to the caption box for the duration of one caption.
 * Callers restore with carousel_text_end(). Enter this before setting the
 * font or colour: both are properties of the current viewport, so anything
 * set beforehand is lost on the switch. */
struct viewport *carousel_text_begin(void)
{
    static struct viewport text_vp;

    text_vp = pf_vp;
    if (pf_vp.width > 2 * PF_TEXT_MARGIN)
    {
        text_vp.x = pf_vp.x + PF_TEXT_MARGIN;
        text_vp.width = pf_vp.width - 2 * PF_TEXT_MARGIN;
    }
    painted_ink = (struct caption_span){LCD_WIDTH, 0};
    painted_ink_known = !(text_vp.flags & VP_FLAG_ALIGNMENT_MASK);
    caption_line = NULL;
    return lcd_set_viewport(&text_vp);
}

void carousel_text_end(struct viewport *saved)
{
    lcd_set_viewport(saved);
}

int get_scroll_line_offset(enum pf_scroll_line_type type, int y)
{
    const struct pf_scroll_line *line = &scroll_lines[type];
    if (!caption_line || line->font != lcd_getfont() ||
        line->text_hash != fnv1a_str(caption_line))
    {
        const struct font *font = font_get(lcd_getfont());
        painted_top = MIN(painted_top, y - font->pbf.ink_above);
        painted_end = MAX(painted_end, y + (int)font->height + font->pbf.ink_below);
        painted_ink_known = false;
    }
    else
    {
        painted_top = MIN(painted_top, y + line->ink_top);
        painted_end = MAX(painted_end, y + line->ink_end);
    }
    if (painted_ink_known && line->ink.left < line->ink.right)
    {
        const struct viewport *vp = lcd_current_viewport;
        int left = MAX(0, line->offset + line->ink.left);
        int right = MIN(vp->width, line->offset + line->ink.right);
        if (left < right)
        {
            painted_ink.left = MIN(painted_ink.left, vp->x + left);
            painted_ink.right = MAX(painted_ink.right, vp->x + right);
        }
    }
    caption_line = NULL;
    return line->offset;
}

/* True when a line actually moved, so the caller knows a frame is owed. A line
 * parked at either end waiting out its delay does not count -- it is showing
 * the same pixels as the frame before. */
static bool update_scroll_lines(void)
{
    int i;
    bool moved = false;

    if (TIME_BEFORE(current_tick, scroll_line_info.next_scroll))
        return false;

    scroll_line_info.next_scroll = current_tick + scroll_line_info.ticks;

    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
    {
        struct pf_scroll_line *s = &scroll_lines[i];
        if (s->step && TIME_BEFORE(s->start_tick, current_tick))
        {
            s->offset -= s->step;
            moved = true;

            if (s->offset >= 0) {
                /* at beginning of line */
                s->offset = 0;
                s->step = scroll_line_info.step;
                s->start_tick = current_tick + scroll_line_info.delay * 2;
            }
            if (s->offset <= PF_TEXT_WIDTH - s->width) {
                /* at end of line */
                s->offset = PF_TEXT_WIDTH - s->width;
                s->step = -scroll_line_info.step;
                s->start_tick = current_tick + scroll_line_info.delay * 2;
            }
        }
    }

    return moved;
}


/**
 Save the given bitmap as filename in the pfraw format
 */
static bool save_pfraw(char* filename, struct bitmap *bm)
{
    struct pfraw_header bmph;
    size_t pixbytes = sizeof( pix_t ) * bm->width * bm->height;
    bool ok;
    bmph.width = bm->width;
    bmph.height = bm->height;
    int fh = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if( fh < 0 ) return false;

    /* Both writes checked, and the file removed if either falls short. A
     * truncated cache file keeps its valid header, so read_pfraw() accepts it
     * and renders whatever the short body left behind -- and being in the
     * cache, it is re-read on every open until something deletes it. */
    ok = (write(fh, &bmph, sizeof(struct pfraw_header))
              == (ssize_t)sizeof(struct pfraw_header))
      && (write(fh, bm->data, pixbytes) == (ssize_t)pixbytes);
    close( fh );

    if (!ok)
        remove(filename);

    return ok;
}

/* Placeholder slide fill: a plain box, a touch off the dark theme background,
 * shown while a cover loads or when a folder has none. Deliberately not the
 * bold podboxnoart -- on a carousel of unloaded slides that was too loud. */
#define EMPTY_SLIDE_COLOR LCD_RGBPACK(0x12, 0x16, 0x20)

/**
  Create the placeholder slide, shown while loading or when no cover was found.
 */
static int create_empty_slide(bool force)
{
    if (!pf_placeholder_buf)
        return false;

    if ( force || ! file_exists( EMPTY_SLIDE ) )  {
        struct bitmap bm = { 0 };
        pix_t *px = pf_placeholder_buf;
        int n = DISPLAY_WIDTH * DISPLAY_WIDTH;
        int i;

        for (i = 0; i < n; i++)
            px[i] = EMPTY_SLIDE_COLOR;

        bm.width = DISPLAY_WIDTH;
        bm.height = DISPLAY_WIDTH;
        bm.format = FORMAT_NATIVE;
        bm.data = pf_placeholder_buf;

        if (!save_pfraw(EMPTY_SLIDE, &bm))
            return false;
    }

    return true;
}

/**
 Thread used for loading and preparing bitmaps in the background
 */
static void thread(void)
{
    /* SSD mode: poll more frequently since disk access is cheap */
    long sleep_time = (global_settings.storage_mode == 2)
                      ? HZ : 5 * HZ;
    struct queue_event ev;
    while (1) {
        queue_wait_w_tmo(&thread_q, &ev, sleep_time);
        switch (ev.id) {
            case EV_EXIT:
                return;
            case EV_WAKEUP:
                /* we just woke up */
                break;
            case SYS_USB_CONNECTED:
                /* This queue is in the broadcast list, so the storage handover
                 * counts an acknowledgement from it and will not give the host
                 * the disk until it arrives. Then park: loading a slide reads
                 * a disk that is no longer ours to read.
                 *
                 * Parked by hand rather than with usb_wait_for_disconnect(),
                 * which discards every event but the disconnect -- including
                 * the EV_EXIT that end_pf_thread() posts before waiting on
                 * this thread, which would then wait forever. */
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                while (1) {
                    queue_wait(&thread_q, &ev);
                    if (ev.id == SYS_USB_DISCONNECTED)
                        break;
                    if (ev.id == EV_EXIT)
                        return;
                }
                continue;
        }

        if(ev.id != SYS_TIMEOUT) {
            bool usb_pending = false;
            while (1) {
                /* Drain the wakeups that piled up while we were loading, and
                 * keep going, rather than abandoning the run to fetch them.
                 *
                 * A wakeup means the centre slide moved. It does not mean this
                 * run is stale: load_new_slide() reads center_index afresh
                 * every call, so the next load already aims at the new
                 * neighbourhood -- going back through the queue to learn that
                 * changes nothing except when the loading resumes. Trap:
                 * abandoning the run on any pending event loads roughly one
                 * slide per centre change during a scroll, against the
                 * 2 * ALBUM_COVERS_NUM_SLIDES + 1 a moving centre needs, so the
                 * art only catches up once the animation stops. */
                while (!queue_empty(&thread_q)) {
                    queue_wait_w_tmo(&thread_q, &ev, 0);
                    if (ev.id == EV_EXIT)
                        return;
                    if (ev.id == SYS_USB_CONNECTED) {
                        /* Put it back and stop the run: the acknowledgement
                         * belongs in one place, and the outer loop is it. */
                        queue_post(&thread_q, ev.id, ev.data);
                        usb_pending = true;
                        break;
                    }
                }
                if (usb_pending)
                    break;

                buf_ctx_lock();
                if (model->art_version)
                {
                    const int radius = ALBUM_COVERS_NUM_SLIDES + CIDER_COVER_LEAD;
                    for (int slot = 0; slot < SLIDE_CACHE_SIZE; slot++)
                    {
                        int index = pf_sldcache.cache[slot].index;
                        if (pf_sldcache.cache[slot].hid <= 0 ||
                            abs(index - center_index) > radius)
                            continue;
                        unsigned int version = model->art_version(index);
                        unsigned int previous = pf_sldcache.cache[slot].version;
                        if (!version || version == previous ||
                            ((version >> 2) == (previous >> 2) &&
                             (version & 3) <= (previous & 3)))
                            continue;
                        int old = pf_sldcache.cache[slot].hid;
                        if (load_and_prepare_surface(index,slot,abs(center_index-index)) &&
                            old != empty_slide_hid)
                            buflib_free(&buf_ctx,old);
                    }
                }
                bool slide_loaded = load_new_slide();
                buf_ctx_unlock();
                if (!slide_loaded)
                    break;
                yield();
            }
        }
    }
}


/**
 End the thread by posting the EV_EXIT event
 */
static void end_pf_thread(void)
{
    if ( thread_is_running ) {
        queue_post(&thread_q, EV_EXIT, 0);
        thread_wait(thread_id);
        /* remove the thread's queue from the broadcast list */
        queue_delete(&thread_q);
        thread_is_running = false;
    }
}


/**
 Create the thread an setup the event queue
 */
static bool create_pf_thread(void)
{
    if (thread_is_running) return true;
    /* put the thread's queue in the bcast list */
    queue_init(&thread_q, true);
    if ((thread_id = create_thread(
                           thread,
                           workspace->loader_stack,
                           sizeof(workspace->loader_stack),
                            0,
                           "Picture load thread"
                               IF_PRIO(, PRIORITY_BUFFERING)
                               IF_COP(, CPU)
                                      )
        ) == 0) {
        /* A queue left registered blocks every USB connect unacknowledged */
        queue_delete(&thread_q);
        return false;
    }
    thread_is_running = true;
    queue_post(&thread_q, EV_WAKEUP, 0);
    return true;
}


static void wake_pf_thread(void)
{
    if (thread_is_running)
    {
        queue_remove_from_head(&thread_q, EV_WAKEUP);
        queue_post(&thread_q, EV_WAKEUP, 0);
    }
}

void carousel_album_caption(const char *albumtxt, const char *artisttxt,
                            int year, bool untagged, int revision)
{
    char album_and_year[MAX_PATH];
    char album_fit[TEXT_FIT_BUF], artist_fit[TEXT_FIT_BUF];
    const char *album_line, *artist_line;
    struct pf_caption cap;
    int albumtxt_x, albumtxt_y, artisttxt_x;
    bool show_artist;

    if (global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;

    show_artist = (global_settings.album_covers_show_album_name == ALBUM_AND_ARTIST_TOP
                || global_settings.album_covers_show_album_name == ALBUM_AND_ARTIST_BOTTOM);

    if (global_settings.album_covers_show_year == ALBUM_YEAR_BEFORE
        && year > 0)
    {
        snprintf(album_and_year, sizeof(album_and_year), "%d \xe2\x80\x93 %s",
                  year, albumtxt);
    }
    else if (global_settings.album_covers_show_year == ALBUM_YEAR_AFTER
        && year > 0)
    {
        snprintf(album_and_year, sizeof(album_and_year), "%s \xe2\x80\x93 %d",
                  albumtxt, year);
    }
    else
        snprintf(album_and_year, sizeof(album_and_year), "%s", albumtxt);

    int bold = carousel_caption_font(true);
    bool ready = font_prefetch_text(bold, album_and_year);
    ready &= font_prefetch_text(bold, ".");
    if (show_artist)
        ready &= font_prefetch_text(bold, artisttxt);
    if (!ready)
        return;

    struct viewport *saved_vp = carousel_text_begin();
    lcd_set_foreground(pf_fg_color);

    /* The year is the caption's other input: moving it changes the string
     * without moving the selection. */
    bool album_changed = carousel_caption_changed(
                             carousel_caption_index(),
                             revision ^ global_settings.album_covers_show_year);

    /* Switched to carousel_caption_font(true) (plain FONT_UI, a no-op, if no bold font is
     * configured -- see its declaration) *before* set_scroll_line() below:
     * that measures the string's pixel width against whatever font is
     * currently active, to center/scroll it, so it has to see the album
     * name's *actual* rendering font, not FONT_UI, or a bold font with a
     * different average glyph width would end up mis-centered. */
    int album_font = carousel_caption_font(true);
    lcd_setfont(album_font);
    album_line = carousel_caption_fit(album_and_year, album_fit,
                                      sizeof(album_fit));
    if (album_changed)
        set_scroll_line(album_line, PF_SCROLL_ALBUM);

    /* The engine measures the font and places the block against the covers,
     * so top and bottom captions need no separate arithmetic here. */
    carousel_caption_layout(show_artist, &cap);
    albumtxt_y = cap.y1;

    albumtxt_x = get_scroll_line_offset(PF_SCROLL_ALBUM, albumtxt_y);

    /* Measured in the artist font before anything is drawn, so an artist line
     * the same as the one on screen is neither cleared nor drawn again. Only
     * the album line's rows change then. */
    bool keep_artist = false;
    caption_clears_itself = true;
    if (show_artist)
    {
        int artist_font = carousel_caption_font(true);
        lcd_setfont(artist_font);
        artist_line = carousel_caption_fit(artisttxt, artist_fit,
                                           sizeof(artist_fit));
        if (album_changed)
            set_scroll_line(artist_line, PF_SCROLL_ARTIST);
        artisttxt_x = get_scroll_line_offset(PF_SCROLL_ARTIST, cap.y2);
        uint32_t text = fnv1a_str(artist_line);
        int album_end = albumtxt_y + font_get(album_font)->height;
        const struct pf_scroll_line *album = &scroll_lines[PF_SCROLL_ALBUM];
        const struct pf_scroll_line *artist = &scroll_lines[PF_SCROLL_ARTIST];
        bool in_box = painted_ink_known && album->ink_top >= 0 &&
                      albumtxt_y + album->ink_end <= album_end;
        keep_artist = caption_clear_h > 0 && caption_artist.valid &&
                      caption_artist.album_in_box && in_box &&
                      caption_artist.text == text &&
                      caption_artist.x == artisttxt_x &&
                      caption_artist.y == cap.y2 &&
                      caption_artist.font == artist_font &&
                      caption_artist.fg == pf_fg_color &&
                      album_end <= cap.y2 + artist->ink_top &&
                      albumtxt_y >= caption_clear_y &&
                      album_end <= caption_clear_y + caption_clear_h;
        caption_artist.album_in_box = in_box;
        caption_artist.text = text;
        caption_artist.x = artisttxt_x;
        caption_artist.y = cap.y2;
        caption_artist.font = artist_font;
        caption_artist.fg = pf_fg_color;
        caption_artist.valid = true;
        if (keep_artist)
        {
            caption_rows_y = albumtxt_y;
            caption_rows_h = album_end - albumtxt_y;
        }
    }
    else
        caption_artist.valid = false;
    /* pf_clear_band() clears in pf_vp's background, set by paint_caption(). */
    if (caption_clear_h > 0)
    {
        if (keep_artist)
            pf_clear_band(caption_rows_y, caption_rows_h);
        else
            pf_clear_band(caption_clear_y, caption_clear_h);
        caption_clear_h = 0;
    }

    lcd_setfont(album_font);
    if (!show_artist || !untagged)
        lcd_putsxy(albumtxt_x, albumtxt_y, album_line);
    if (show_artist && !keep_artist)
        lcd_putsxy(artisttxt_x, cap.y2, artist_line);
    /* Restored after the caption: render_all_slides()/the FPS overlay/etc
     * all assume pf_vp's font is the real UI font. Both lines are bold.
     * carousel_caption_font(false), not the FONT_UI constant -- see init()'s
     * comment on carousel_caption_font(true). */
    lcd_setfont(carousel_caption_font(false));
    carousel_text_end(saved_vp);
}


static void initialize_slide_cache(void)
{
    int i= 0;
    for (i = 0; i < SLIDE_CACHE_SIZE; i++) {
        pf_sldcache.cache[i].hid = 0;
        pf_sldcache.cache[i].index = 0;
        pf_sldcache.cache[i].next = i + 1;
        pf_sldcache.cache[i].prev = i - 1;
    }
    pf_sldcache.cache[0].prev = i - 1;
    pf_sldcache.cache[i - 1].next = 0;

    pf_sldcache.free = 0;
    pf_sldcache.used = -1;
    pf_sldcache.left_idx = -1;
    pf_sldcache.right_idx = -1;
    pf_sldcache.center_idx = -1;
}


/*
 * The following functions implement the linked-list-in-array used to manage
 * the LRU cache of slides, and the list of free cache slots.
 */

#define _SEEK_RIGHT_WHILE(start, cond) \
({ \
    int ind_, next_ = (start); \
    int i_ = 0; \
    do { \
        ind_ = next_; \
        next_ = pf_sldcache.cache[ind_].next; \
        i_++; \
    } while (next_ != pf_sldcache.used && (cond) && i_ <= SLIDE_CACHE_SIZE); \
    if (i_ > SLIDE_CACHE_SIZE) \
    /* TODO: Not supposed to happen */ \
        ind_ = -1; \
    ind_; \
})

#define _SEEK_LEFT_WHILE(start, cond) \
({ \
    int ind_, next_ = (start); \
    int i_ = 0; \
    do { \
        ind_ = next_; \
        next_ = pf_sldcache.cache[ind_].prev; \
        i_++; \
    } while (ind_ != pf_sldcache.used && (cond) && i_ <= SLIDE_CACHE_SIZE); \
    if (i_ > SLIDE_CACHE_SIZE) \
    /* TODO: Not supposed to happen */ \
        ind_ = -1; \
    ind_; \
})

/**
 Pop the given item from the linked list starting at *head, returning the next
 item, or -1 if the list is now empty.
*/
static inline int lla_pop_item (int *head, int i)
{
    if ((unsigned)i >= SLIDE_CACHE_SIZE) return -1;
    int prev = pf_sldcache.cache[i].prev;
    int next = pf_sldcache.cache[i].next;
    if (i == next)
    {
        *head = -1;
        return -1;
    }
    else if (i == *head)
        *head = next;
    pf_sldcache.cache[next].prev = prev;
    pf_sldcache.cache[prev].next = next;
    return next;
}


/**
 Pop the head item from the list starting at *head, returning the index of the
 item, or -1 if the list is already empty.
*/
static inline int lla_pop_head (int *head)
{
    int i = *head;
    if (i != -1)
        lla_pop_item(head, i);
    return i;
}

/**
 Insert the item at index i before the one at index p.
*/
static inline void lla_insert (int i, int p)
{
    if ((unsigned)i >= SLIDE_CACHE_SIZE || (unsigned)p >= SLIDE_CACHE_SIZE) return;
    int next = p;
    int prev = pf_sldcache.cache[next].prev;
    pf_sldcache.cache[next].prev = i;
    pf_sldcache.cache[prev].next = i;
    pf_sldcache.cache[i].next = next;
    pf_sldcache.cache[i].prev = prev;
}


/**
 Insert the item at index i at the end of the list starting at *head.
*/
static inline void lla_insert_tail (int *head, int i)
{
    if ((unsigned)i >= SLIDE_CACHE_SIZE) return;
    if (*head == -1)
    {
        *head = i;
        pf_sldcache.cache[i].next = i;
        pf_sldcache.cache[i].prev = i;
    } else
        lla_insert(i, *head);
}

/**
 Insert the item at index i before the one at index p.
*/
static inline void lla_insert_after(int i, int p)
{
    if ((unsigned)i >= SLIDE_CACHE_SIZE || (unsigned)p >= SLIDE_CACHE_SIZE) return;
    p = pf_sldcache.cache[p].next;
    lla_insert(i, p);
}


/**
 Insert the item at index i before the one at index p in the list starting at
 *head
*/
static inline void lla_insert_before(int *head, int i, int p)
{
    if ((unsigned)i >= SLIDE_CACHE_SIZE || (unsigned)p >= SLIDE_CACHE_SIZE) return;
    lla_insert(i, p);
    if (*head == p)
        *head = i;
}


/**
 Free the used slide at index i, and its buffer, and move it to the free
 slides list.
*/
static inline void free_slide(int i)
{
    if ((unsigned)i >= SLIDE_CACHE_SIZE) return;
    if (pf_sldcache.cache[i].hid != empty_slide_hid)
        buflib_free(&buf_ctx, pf_sldcache.cache[i].hid);
    pf_sldcache.cache[i].index = -1;
    lla_pop_item(&pf_sldcache.used, i);
    lla_insert_tail(&pf_sldcache.free, i);
    if (pf_sldcache.used == -1)
    {
        pf_sldcache.right_idx = -1;
        pf_sldcache.left_idx = -1;
        pf_sldcache.center_idx = -1;
    }
}


/**
 Free one slide ranked above the given priority. If no such slide can be found,
 return false.
*/
static bool free_slide_prio(int prio)
{
    if (pf_sldcache.used == -1)
        return false;

    int i, prio_max;
    int l = pf_sldcache.used;
    int r = pf_sldcache.cache[pf_sldcache.used].prev;

    int prio_l = pf_sldcache.cache[l].index < center_index ?
           center_index - pf_sldcache.cache[l].index : 0;
    int prio_r = pf_sldcache.cache[r].index > center_index ?
           pf_sldcache.cache[r].index - center_index : 0;
    if (prio_l > prio_r)
    {
        i = l;
        prio_max = prio_l;
    } else {
        i = r;
        prio_max = prio_r;
    }
    if (prio_max > prio)
    {
        if (i == pf_sldcache.left_idx)
            pf_sldcache.left_idx = pf_sldcache.cache[i].next;
        if (i == pf_sldcache.right_idx)
            pf_sldcache.right_idx = pf_sldcache.cache[i].prev;
        free_slide(i);
        return true;
    } else
        return false;
}


/**
 Free all slides ranked above the given priority.
*/
void carousel_drop_slides(void)
{
    /* Every slide, the centre one included, under the lock the loader reads
     * the cache with; the wakeup has it load them again at once. */
    buf_ctx_lock();
    free_all_slide_prio(-1);
    buf_ctx_unlock();
    wake_pf_thread();
}

void carousel_provider_changed(void)
{
    wake_pf_thread();
}

static void free_all_slide_prio(int prio)
{
    ++slide_cache_generation;
    while (free_slide_prio(prio))
    {;;}
}


/* Bytes a slide of `pixels` pixels needs for its reflection, with two
 * texels of slack for the sampler's last rounded row. */
static size_t reflection_bytes(size_t pixels)
{
    return pf_stock ? (pixels / 2 + 2) * sizeof(pix_t) : 0;
}

/* The reflection follows the slide's pixels: reflection_rows() texels for
 * each column of its centred square. */
static inline const pix_t *slide_reflection(const struct dim *bmp)
{
    return pf_stock ? (const pix_t *)(bmp + 1) + bmp->width * bmp->height
                    : NULL;
}

static void make_reflection(struct dim *bmp)
{
    if (!pf_stock)
        return;
    int size = MIN(bmp->width, bmp->height);
    const pix_t *crop = (const pix_t *)(bmp + 1) +
        (bmp->width - size) / 2 * bmp->height + (bmp->height - size) / 2;
    reflection_build((pix_t *)slide_reflection(bmp), crop, size, bmp->height,
                     pf_bg_color);
}

/* Read a pfraw image into the slide cache; a buflib handle, or -1.
 *
 * Only the placeholder comes through here, and both callers assign the result
 * to empty_slide_hid -- so failing to -1 is what makes the failure sayable.
 * Handing back empty_slide_hid instead would hand a caller its own stale
 * answer: by the time it runs, buflib_init() has emptied the context that
 * handle came from, and nothing in the new one answers to it. */
static int read_pfraw(char* filename, int prio)
{
    struct pfraw_header bmph;
    int fh = open(filename, O_RDONLY);
    if( fh < 0 )
        return -1;

    /* A short read (truncated/corrupted cache file, e.g. from a power-off
     * mid-write) leaves bmph partially or fully uninitialized, and a
     * width/height larger than anything save_pfraw() could have written
     * (bounded by DISPLAY_WIDTH/DISPLAY_HEIGHT) means the file is corrupt
     * either way -- trusting either lets the size calculation below and
     * every later access via bm->width/height walk arbitrarily far past
     * this allocation. Treat both as "no cached image" rather than
     * propagating garbage dimensions. */
    if (read(fh, &bmph, sizeof(struct pfraw_header)) != sizeof(struct pfraw_header) ||
        bmph.width <= 0 || bmph.height <= 0 ||
        bmph.width > DISPLAY_WIDTH || bmph.height > DISPLAY_HEIGHT)
    {
        close(fh);
        return -1;
    }

    int size =  sizeof(int) + sizeof(struct dim) +
                sizeof(pix_t) * bmph.width * bmph.height +
                reflection_bytes(bmph.width * bmph.height);

    int hid;
    do {
        hid = buflib_alloc(&buf_ctx, size);
    } while (hid < 0 && free_slide_prio(prio));

    if (hid < 0) {
        close( fh );
        return -1;
    }

    char *block = buflib_get_data(&buf_ctx, hid);
    *(int *)block = sizeof(int);           /* see get_slide() */
    struct dim *bm = (struct dim *)(block + sizeof(int));

    bm->width = bmph.width;
    bm->height = bmph.height;
    pix_t *data = (pix_t*)(sizeof(struct dim) + (char *)bm);
    size_t pixbytes = sizeof( pix_t ) * bm->width * bm->height;

    /* The header was checked above for exactly this reason; the body needs the
     * same. A short read here leaves the tail of a freshly allocated slide
     * holding whatever the buffer last contained, which renders as garbage. */
    if (read(fh, data, pixbytes) != (ssize_t)pixbytes)
    {
        close( fh );
        buflib_free(&buf_ctx, hid);
        return -1;
    }

    close( fh );
    make_reflection(bm);
    return hid;
}

/* The chain the slides are drawn through, compiled from the setting on first
 * use and whenever the setting moves. Kept here rather than recompiled per
 * slide: a scroll loads slides continuously and the spec parses the same every
 * time. carousel_filter_changed() is what tells the menu a reload is due. */
static struct img_filter carousel_chain;
static int carousel_chain_from[CAROUSEL_FILTER_SLOTS] = { -1, -1, -1 };
static char carousel_chain_from_text[CAROUSEL_FILTER_MAX] = "\1";

static const struct img_filter *carousel_filter(void)
{
    char spec[CAROUSEL_FILTER_MAX];
    const char *text = global_settings.album_covers_filter_chain;
    size_t used = 0;

    /* Both halves are compared, not just the one in force: switching between
     * them changes the picture without either alone having changed. */
    if (!memcmp(carousel_chain_from, global_settings.album_covers_filter,
                sizeof(carousel_chain_from)) &&
        !strcmp(carousel_chain_from_text, text))
        return carousel_chain.stages ? &carousel_chain : NULL;

    spec[0] = '\0';
    if (text[0])
    {
        /* Written out in full, so the slots have nothing to say. Overlong is
         * treated as no filter rather than as a truncated one, which would
         * compile to something the theme never asked for. */
        if (strlcpy(spec, text, sizeof(spec)) >= sizeof(spec))
            spec[0] = '\0';
    }
    else
    for (int i = 0; i < CAROUSEL_FILTER_SLOTS; i++)
    {
        const char *p = CAROUSEL_FILTER_CFG_VALS;
        int idx = global_settings.album_covers_filter[i];
        size_t len;

        if (idx <= 0)                       /* entry 0 is "off" */
            continue;
        while (idx-- > 0 && p)
        {
            p = strchr(p, ',');
            if (p)
                p++;
        }
        if (!p)
            continue;
        len = strcspn(p, ",");
        if (used + len + 2 > sizeof(spec))
            continue;
        if (used)
            spec[used++] = '+';
        memcpy(spec + used, p, len);
        used += len;
        spec[used] = '\0';
    }

    /* IMG_CLASSES_INPLACE refuses `blur` and the adaptive filters. The list
     * above cannot name them anyway; a chain written out by hand can, and is
     * refused here -- the tier rule lives in the engine rather than in the
     * list, and that is the right way round. A chain that will not compile
     * leaves the slides unfiltered rather than half filtered. */
    if (!img_filter_compile(spec, IMG_CLASSES_INPLACE, &carousel_chain))
        carousel_chain.stages = 0;
    memcpy(carousel_chain_from, global_settings.album_covers_filter,
           sizeof(carousel_chain_from));
    strlcpy(carousel_chain_from_text, text, sizeof(carousel_chain_from_text));
    return carousel_chain.stages ? &carousel_chain : NULL;
}

/* Room to move a read up to a cache line. The 5G's disk does DMA only into a
 * line-aligned buffer and copies with the CPU otherwise. */
#ifdef HAVE_CPU_CACHE_ALIGN
#define PF_READ_SLACK (CACHEALIGN_SIZE - 1)
#else
#define PF_READ_SLACK 0
#endif

/* A whole-file aligned read leaves native columns and their dimensions in
 * place. Row-major thumbnails are transposed into the same private block. */
static struct dim *read_aat(int fh, ssize_t len, char *block)
{
    struct art_cache_header hdr;
    static pix_t rowbuf[DISPLAY_WIDTH];
    int row, col, w, h;
    char *file = CACHEALIGN_UP(block + sizeof(int));
    struct dim *bm = (struct dim *)(file + sizeof(hdr) - sizeof(struct dim));
    pix_t *dst = (pix_t *)(file + sizeof(hdr));

    if (read(fh, file, len) != len)
    {
        return NULL;
    }

    memcpy(&hdr, file, sizeof(hdr));
    w = hdr.width;
    h = hdr.height;
    if (hdr.magic != ART_CACHE_MAGIC ||
        hdr.version != ART_CACHE_FORMAT_VERSION ||
        w == 0 || h == 0 || w > DISPLAY_WIDTH || h > DISPLAY_HEIGHT ||
        len - (ssize_t)sizeof(hdr) < (ssize_t)(sizeof(pix_t) * w * h))
    {
        return NULL;
    }

    *(int *)block = (char *)bm - block;    /* see get_slide() */
    bm->width = w;
    bm->height = h;

    /* The "coverflow" size is stored column-major (art_sizes.h), which is the
     * order rendering wants, so it is already in place. The cache falls back
     * to rows for a thumbnail it could not store transposed -- a non-square
     * COVER fit -- and that one is read again, transposed a row at a time. */
    if (hdr.layout != AA_COLUMNS)
    {
        if (lseek(fh, sizeof(hdr), SEEK_SET) != (off_t)sizeof(hdr))
        {
            return NULL;
        }
        for (row = 0; row < h; row++)
        {
            if (read(fh, rowbuf, sizeof(pix_t) * w)
                != (ssize_t)(sizeof(pix_t) * w))
            {
                return NULL;
            }
            for (col = 0; col < w; col++)
                dst[col * h + row] = rowbuf[col];
        }
    }

    return bm;
}

/**
  Load the surface for the given slide_index into the cache at cache_index.
 */
static inline bool load_and_prepare_surface(const int slide_index,
                                            const int cache_index,
                                            const int prio)
{
    if ((unsigned)cache_index >= SLIDE_CACHE_SIZE || slide_index < 0)
        return false;
    unsigned int generation = slide_cache_generation;
    int center = center_index;
    unsigned int version = 0;
    unsigned int hash = model->read_art ? 0 : model->art_key(slide_index);

    /* The new surface stays private until its pixels are ready.
     * A cache reset can retire the request while the loader yields. */
    buf_ctx_unlock();
    struct bitmap bitmap;
    ssize_t len = 0;
    size_t allocation = 0;
    int fh = -1;
    if (model->read_art)
    {
        if (model->read_art(slide_index, &bitmap, &version))
        {
            len = sizeof(pix_t) * bitmap.width * bitmap.height;
            allocation = sizeof(int) + sizeof(struct dim) + len +
                         reflection_bytes(bitmap.width * bitmap.height);
        }
    }
    else if (hash && pf_cover_size_idx >= 0)
    {
        static char path[MAX_PATH];
        art_cache_thumb_path(hash, pf_cover_size_idx, path, sizeof(path));
        fh = open(path, O_RDONLY);
        if (fh >= 0)
        {
            const ssize_t header = sizeof(struct art_cache_header);
            const ssize_t most = header + sizeof(pix_t) * DISPLAY_WIDTH * DISPLAY_HEIGHT;
            len = ffilesize(fh);
            if (len > most)
                len = most;
            if (len > header)
                allocation = sizeof(int) + PF_READ_SLACK + len +
                             reflection_bytes((len - header) / sizeof(pix_t));
        }
    }

    buf_ctx_lock();
    bool current = generation == slide_cache_generation && center == center_index;
    int hid = empty_slide_hid;
    char *block = NULL;
    if (current && allocation)
    {
        do {
            hid = buflib_alloc(&buf_ctx, allocation);
        } while (hid < 0 && free_slide_prio(prio));
        if (hid > 0)
            block = buflib_get_data_pinned(&buf_ctx, hid);
    }
    buf_ctx_unlock();

    struct dim *bm = NULL;
    if (block)
    {
        if (fh >= 0)
            bm = read_aat(fh, len, block);
        else
        {
            *(int *)block = sizeof(int);
            bm = (struct dim *)(block + sizeof(int));
            bm->width = bitmap.width;
            bm->height = bitmap.height;
            memcpy(bm + 1, bitmap.data, len);
        }
        if (bm)
        {
            img_filter_apply_banded((pix_t *)(bm + 1), bm->height,
                                    bm->width, carousel_filter());
            make_reflection(bm);
        }
    }
    if (fh >= 0)
        close(fh);

    buf_ctx_lock();
    current = current && generation == slide_cache_generation;
    if (block)
    {
        buflib_put_data_pinned(&buf_ctx, block);
        if (!current || !bm)
        {
            buflib_free(&buf_ctx, hid);
            hid = empty_slide_hid;
        }
    }
    if (!current || hid < 0)
        return false;

    pf_sldcache.cache[cache_index].hid = hid;
    pf_sldcache.cache[cache_index].version = version;
    pf_sldcache.cache[cache_index].index = slide_index;

    if (abs(slide_index - center_index) <= ALBUM_COVERS_NUM_SLIDES)
        pf_slides_loaded++;

#if PF_SHOW_STATS
    pf_loads_total++;
#endif

    return true;
}


/**
 Load the "next" slide that we can load, freeing old slides if needed, provided
 that they are further from center_index than the current slide
*/
bool load_new_slide(void)
{
    if (wants_to_quit)
        return false;

    int i = -1;

    if (pf_sldcache.center_idx != -1)
    {
        int next, prev;
        if (pf_sldcache.cache[pf_sldcache.center_idx].index != center_index)
        {
            if (pf_sldcache.cache[pf_sldcache.center_idx].index < center_index)
            {
                pf_sldcache.center_idx = _SEEK_RIGHT_WHILE(pf_sldcache.center_idx,
                                pf_sldcache.cache[next_].index <= center_index);
                if (pf_sldcache.center_idx == -1)
                    goto fatal_fail;

                prev = pf_sldcache.center_idx;
                next = pf_sldcache.cache[pf_sldcache.center_idx].next;
            }
            else
            {
                pf_sldcache.center_idx = _SEEK_LEFT_WHILE(pf_sldcache.center_idx,
                                pf_sldcache.cache[next_].index >= center_index);
                if (pf_sldcache.center_idx == -1)
                    goto fatal_fail;

                next = pf_sldcache.center_idx;
                prev = pf_sldcache.cache[pf_sldcache.center_idx].prev;
            }
            if (pf_sldcache.cache[pf_sldcache.center_idx].index != center_index)
            {
                if (pf_sldcache.free == -1)
                    free_slide_prio(0);

                i = lla_pop_head(&pf_sldcache.free);
                if (!load_and_prepare_surface(center_index, i, 0))
                    goto fail_and_refree;

                if (pf_sldcache.cache[next].index == -1)
                {
                    if (pf_sldcache.cache[prev].index == -1)
                        goto insert_first_slide;
                    else
                        next = pf_sldcache.cache[prev].next;
                }
                lla_insert(i, next);
                if (pf_sldcache.cache[i].index < pf_sldcache.cache[pf_sldcache.used].index)
                    pf_sldcache.used = i;

                pf_sldcache.center_idx = i;
                pf_sldcache.left_idx = i;
                pf_sldcache.right_idx = i;
                return true;
            }
        }
        int left, center, right;
        left = pf_sldcache.cache[pf_sldcache.left_idx].index;
        center = pf_sldcache.cache[pf_sldcache.center_idx].index;
        right = pf_sldcache.cache[pf_sldcache.right_idx].index;

        if (left > center)
            pf_sldcache.left_idx = pf_sldcache.center_idx;
        if (right < center)
            pf_sldcache.right_idx = pf_sldcache.center_idx;

        pf_sldcache.left_idx = _SEEK_LEFT_WHILE(pf_sldcache.left_idx,
            pf_sldcache.cache[ind_].index - 1 == pf_sldcache.cache[next_].index);

        pf_sldcache.right_idx = _SEEK_RIGHT_WHILE(pf_sldcache.right_idx,
            pf_sldcache.cache[ind_].index + 1 == pf_sldcache.cache[next_].index);
        if (pf_sldcache.right_idx == -1 || pf_sldcache.left_idx == -1)
            goto fatal_fail;


        /* update indices */
        left = pf_sldcache.cache[pf_sldcache.left_idx].index;
        center = pf_sldcache.cache[pf_sldcache.center_idx].index;
        right = pf_sldcache.cache[pf_sldcache.right_idx].index;

        int prio_l = center - left + 1;
        int prio_r = right - center + 1;
        if ((prio_l < prio_r
             || right >= number_of_slides - 1) && left > 0)
        {
            if (pf_sldcache.free == -1 && !free_slide_prio(prio_l))
            {
                return false;
            }

            i = lla_pop_head(&pf_sldcache.free);
            if (load_and_prepare_surface(left - 1, i, prio_l))
            {
                lla_insert_before(&pf_sldcache.used, i, pf_sldcache.left_idx);
                pf_sldcache.left_idx = i;
                return true;
            }
        } else if(right < number_of_slides - 1)
        {
            if (pf_sldcache.free == -1 && !free_slide_prio(prio_r))
            {
                return false;
            }

            i = lla_pop_head(&pf_sldcache.free);
            if (load_and_prepare_surface(right + 1, i, prio_r))
            {
                lla_insert_after(i, pf_sldcache.right_idx);
                pf_sldcache.right_idx = i;
                return true;
            }
        }
    } else {
        i = lla_pop_head(&pf_sldcache.free);
        if (load_and_prepare_surface(center_index, i, 0))
        {
insert_first_slide:
            pf_sldcache.cache[i].next = i;
            pf_sldcache.cache[i].prev = i;
            pf_sldcache.center_idx = i;
            pf_sldcache.left_idx = i;
            pf_sldcache.right_idx = i;
            pf_sldcache.used = i;
            return true;
        }
    }
fail_and_refree:
    if (i != -1)
    {
        lla_insert_tail(&pf_sldcache.free, i);
    }
    return false;
fatal_fail:
    free_all_slide_prio(0);
    initialize_slide_cache();
    return false;
}


/**
  Get a slide from the buffer
 */
static inline struct dim *get_slide(const int hid)
{
    /* Every buflib handle is positive, so this covers "none" (0) and "could
     * not load it" (-1) alike. buflib_get_data() indexes handle_table[-hid],
     * which for a negative one lands outside the buffer entirely. */
    if (hid <= 0)
        return NULL;

    /* A slide's block opens with an int giving where its struct dim starts,
     * the pixels following straight after. Not fixed, because read_aat() puts
     * the dim wherever the file's own alignment leaves it. An offset rather
     * than a pointer: buflib may move the block. */
    char *block = buflib_get_data(&buf_ctx, hid);

    return (struct dim *)(block + *(int *)block);
}


/**
 Return the requested surface for the given slide.

 The cache is a linked-list-in-array, so locating a slide_index is an O(n)
 walk of the used list. render_slide() calls this for every visible slide,
 every frame, and a slide normally stays in the same cache slot across frames.
 slide->cached_slot memoizes the slot found last time: a slot's index is unique
 within the used list, so if cache[slot].index still equals slide_index (and the
 slot holds a real handle) it is guaranteed to be the same slot, and we can skip
 the scan. This check reproduces the scan's result exactly on a hit and simply
 falls back to the full scan on a miss (evicted/reused/not-yet-resolved slot),
 so no explicit invalidation is needed. The bitmap data is always fetched fresh
 via get_slide() because buflib may relocate it between frames.
*/
static inline struct dim *surface(struct slide_data *slide)
{
    const int slide_index = slide->slide_index;
    if (slide_index < 0)
        return 0;
    if (slide_index >= number_of_slides)
        return 0;

    int slot = slide->cached_slot;
    if ((unsigned)slot < SLIDE_CACHE_SIZE &&
        pf_sldcache.cache[slot].index == slide_index &&
        pf_sldcache.cache[slot].hid != 0)
        return get_slide(pf_sldcache.cache[slot].hid);

    int i;
    if ((i = pf_sldcache.used ) != -1)
    {
        int j = 0;
        do {
            if (pf_sldcache.cache[i].index == slide_index) {
                slide->cached_slot = i;
                return get_slide(pf_sldcache.cache[i].hid);
            }
            i = pf_sldcache.cache[i].next;
            j++;
        } while (i != pf_sldcache.used && j < SLIDE_CACHE_SIZE);
    }
    return get_slide(empty_slide_hid);
}

static bool flat_view(void)
{
    return global_settings.album_covers_view_mode == CAROUSEL_VIEW_FLAT;
}

/* Reflections, the caption's place and the steady half rate belong to the
 * 3D view; the flat view under the stock look keeps only the status bar. */
static bool stock_look(void)
{
    return pf_stock && !flat_view();
}

/* The stock look presents every second refresh and stays there: its frame
 * with reflections does not fit one refresh, and changing rate in the
 * middle of a motion shows as a stumble. */
static uint32_t flow_pace_next(struct ui_pace *pace, uint32_t start)
{
    pace->half_rate = stock_look();
    if (pace->half_rate)
        pace->recover_full_rate = false;
    return ui_pace_next(pace, start);
}

/* Moves a stock-look motion's clock that began at `*start` on to the
 * refresh now under way, and returns by how much: the motion then resumes
 * from the pose it last showed, however long the work since took. */
static uint32_t stock_rebase(uint32_t *start)
{
    uint32_t edge = ui_clock_start(), late = edge - *start;
    *start = edge;
    return late;
}

/* The alpha a cover sitting on a pile is drawn at, the setting being how far
 * toward the background it is taken. Note this is a dimming, not a
 * transparency: render_slide() blends toward the background colour, so a pile
 * cover drawn over another one still hides it, exactly as an undimmed one
 * would. Which is what the piles want -- covers, further back. */
static int pile_alpha(void)
{
    return 256 - 256 * global_settings.album_covers_pile_fade / 100;
}

/* How far below the middle cover a pile sits. */
static PFreal pile_offset(void)
{
    return global_settings.album_covers_pile_offset * PFREAL_ONE;
}

/* Ease in and out: 3u^2 - 2u^3, for `u` in 0..PFREAL_ONE.
 *
 * Used for the drop onto a pile and the lift back off it, while the sideways
 * travel stays linear. A card crossing at a steady rate while its height eases
 * away and back is how one behaves being dealt; easing both would read as the
 * whole move slowing down in the middle, which is the 3D view's ease curve and
 * the thing the flat view was given a constant rate to get away from. */
static PFreal smoothstep(PFreal u)
{
    PFreal uu;

    if (u <= 0)
        return 0;
    if (u >= PFREAL_ONE)
        return PFREAL_ONE;

    uu = (u * u) >> PFREAL_SHIFT;
    return (uu * (3 * PFREAL_ONE - 2 * u)) >> PFREAL_SHIFT;
}

static uint32_t motion_clock(void)
{
    return USEC_TIMER;
}

static unsigned motion_elapsed(uint32_t now, uint32_t since)
{
    return ui_clock_frame(now - since);
}

/* Scale the duration by the scroll speed setting, relative to 200%. */
static int flat_duration(unsigned frames)
{
    unsigned period = motion_frames(frames);
    return MAX(period * 200 / global_settings.album_covers_scroll_speed, 1u);
}

/* Time this cover should take, so the deal keeps up with the user rather than
 * lagging behind them: how far apart their scroll actions have been coming,
 * stretched as the input dries up so the last cover of a gesture settles rather
 * than merely stopping.
 *
 * The stretch is a multiple of their own pace, not a fixed duration. A fixed one
 * is what makes a fast flick end in a full-length deal -- covers going by at
 * 2 f apiece and then one taking 15 f, which reads as a stumble rather than a
 * stop. Everything the flat view does is timed off the wheel; this is the part
 * that is easiest to get wrong, because a settle that is too quick to see is
 * the obvious failure and a settle that is out of proportion is not. */
static int flat_pace(uint32_t now)
{
    int base = MAX(pf_flat_interval, 1);
    int since = (int32_t)(now - pf_flat_last_input) >= 0 ?
                motion_elapsed(now, pf_flat_last_input) : 0;
    int pace = MAX(base, MIN(since, base * PF_FLAT_SETTLE));

    return MIN(pace, flat_duration(PF_FLAT_FLIP));
}

/* Time the flip in progress should take. Covers being snapped rather than
 * dealt keep to the gesture's own rate and ignore the settling term above: they
 * have no animation to land, so stretching one only holds a still picture on
 * the screen, and an even cadence is the whole of what a snapped scroll has to
 * look at. Handing them back to the 3D view's ease curve, which was the first
 * try, gives the opposite -- each cover decelerating almost to a halt and then
 * changing without warning. */
static int flat_rate(uint32_t now)
{
    if (pf_flat_snap)
        return MAX(pf_flat_interval, 1);
    return MAX(flat_pace(now), flat_duration(PF_FLAT_MIN));
}

/* Every scroll action, including one at the end of the row, says how fast the
 * user is going. */
static void flat_note_scroll(void)
{
    int full = flat_duration(PF_FLAT_FLIP);
    uint32_t now = motion_clock();
    int gap = motion_elapsed(now, pf_flat_last_input);

    pf_flat_last_input = now;
    if (gap >= full)
        pf_flat_interval = full;    /* a fresh gesture: forget the old pace */
    else
        pf_flat_interval = (pf_flat_interval + gap) / 2;
}

/* Points to the current foreground render's counters only during capture. */
static uint32_t *flow_stages;
struct flow_card {
    int id;
    bool linear;
    struct stock_edge edge[2];
};

static struct stock_flow flow_row;
static struct stock_visible flow_visible[STOCK_MAX_VISIBLE];
static struct flow_card flow_cards[STOCK_MAX_VISIBLE];
static int flow_count;

static void coverflow_place(uint32_t now)
{
    uint32_t start = flow_stages ? USEC_TIMER : 0;
    flow_count = stock_sample(&flow_row, now, flow_visible);
    if (flow_stages)
    {
        uint32_t sampled = USEC_TIMER;
        flow_stages[UI_COVER_MOTION] += sampled - start;
        start = sampled;
    }
    for (int i = 0; i < flow_count; i++)
    {
        flow_cards[i].id = flow_visible[i].album;
        flow_cards[i].linear = flow_visible[i].linear;
        stock_project(flow_visible[i].pose, flow_cards[i].edge);
    }
    if (flow_stages)
        flow_stages[UI_COVER_PROJECT] += (uint32_t)USEC_TIMER - start;
}

static void coverflow_idle(void)
{
    flow_pace.recover_full_rate = true;
    center_slide.slide_index = center_index;
    uint32_t now = USEC_TIMER;
    stock_init(&flow_row, number_of_slides, center_index, now, false);
    coverflow_place(now);
}

/* Every card on its resting place: one cover in the middle, and the rest
 * squared up in the two piles, each exactly on top of the one below it. */
static void flat_idle(void)
{
    flow_pace.recover_full_rate = false;
    int pile = pile_alpha();
    PFreal drop = pile_offset();

    center_slide.cx = 0;
    center_slide.cy = 0;
    center_slide.alpha = 256;
    center_slide.slide_index = center_index;

    int i;
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *l = &left_slides[i];
        struct slide_data *r = &right_slides[i];

        l->alpha = r->alpha = pile;
        l->cy = r->cy = drop;
        l->cx = -PF_PILE_CX;
        r->cx = PF_PILE_CX;
        l->slide_index = center_index - 1 - i;
        r->slide_index = center_index + 1 + i;
    }

    pf_flat_moving = false;
}

/**
 adjust slides so that they are in "steady state" position
 */
static void reset_slides(void)
{
    if (flat_view())
        flat_idle();
    else
        coverflow_idle();
}


/* Nominal cover height. Covers are scaled into DISPLAY_WIDTH x DISPLAY_HEIGHT
 * keeping their aspect, so a square one -- nearly all of them -- comes out
 * width-limited at this height. The caption is placed against this rather than
 * against the cover currently on screen, so it stays put when a cover of some
 * other shape scrolls past. */
#define PF_NOMINAL_ART_H  MIN(DISPLAY_WIDTH, DISPLAY_HEIGHT)

/* Flat layout reserves a caption band before centering the piles. */
#define PF_CAPTION_BAND   55

/* Clear rows between the nominal cover and the caption's first line box. */
#define PF_CAPTION_GAP    4

/* The stock caption: baselines at rows 204 and 222 under the reflection. */
#define STOCK_CAPTION_BASELINE 204
#define STOCK_CAPTION_PITCH    18

/* Second line's offset from the first line's box top: 17/12 of the caption
 * font's ascent, so 17 rows for a 12-row ascent against stock's measured 17.5.
 *
 * Measured from the ascent rather than the box height, because a box carries a
 * descent running anywhere from a fifth to nearly a third of its height
 * depending on the font, and spacing off the bottom hands that variation
 * straight to the gap. The 5/12 of an ascent left below the baseline exceeds a
 * descent (ascent runs ~70% of the box, descent ~28%), so a descending album
 * name still clears the line below. */
static int caption_line2(void)
{
    if (stock_look())
        return STOCK_CAPTION_PITCH;
    return font_get(carousel_caption_font(true))->ascent * 17 / 12;
}

static int caption_block(bool two_lines)
{
    int height = font_get(carousel_caption_font(true))->height;

    return two_lines ? caption_line2() + height : height;
}

void carousel_caption_layout(bool two_lines, struct pf_caption *out)
{
    int art_top = (pf_height - pf_caption_band - PF_NOMINAL_ART_H) / 2;

    if (art_top < 0)        /* cover taller than the drawable area: it is
                             * clipped to that area, so it starts at the top */
        art_top = 0;
    art_top += pf_draw_y_shift;

    /* The 3D view keeps the caption at its fixed screen position. */
    if (stock_look() && !pf_draw_y_shift)
        out->y1 = STOCK_CAPTION_BASELINE -
                  font_get(carousel_caption_font(true))->ascent;
    else if (!flat_view())
        out->y1 = pf_draw_y_shift ?
                  MAX(pf_chrome_h, 31 - caption_block(two_lines)) : 175;
    else if (pf_draw_y_shift)    /* caption above the covers */
        out->y1 = MAX(0, art_top - PF_CAPTION_GAP - caption_block(two_lines));
    else
        out->y1 = art_top + PF_NOMINAL_ART_H + PF_CAPTION_GAP;
    out->y2 = out->y1 + caption_line2();
}

static void update_caption_bounds(void)
{
    pf_text_y = pf_text_h = 0;
    if (pf_caption_band)
    {
        struct pf_caption cap;
        carousel_caption_layout(true, &cap);
        /* PBF ink can extend past the line box. Reserve only the overhang
         * of the displayed glyphs, so normal captions leave the art intact. */
        pf_text_y = MAX(0, MIN(cap.y1, painted_top));
        int end = MAX(cap.y1 + caption_block(true), painted_end);
        pf_text_h = MAX(0, MIN(end, pf_height) - pf_text_y);
    }
}

/* Switching views in the settings menu also moves the caption. */
void carousel_refresh(void)
{
    update_caption_bounds();
    reset_slides();
    if (!flat_view() && pf_state == pf_scrolling)
        stock_scroll(&flow_row, target - flow_row.selected, USEC_TIMER);
}

/**
   Fade the given color toward the theme background color.
   a = 256 means fully opaque (original color), a = 0 means fully bg.
 */
/* The part of a fade that depends only on the slide's alpha and the background,
 * lifted out of the pixel loop. Both background terms are whole multiplies, and
 * leaving them where they were -- inside a function called per pixel -- had the
 * loop doing four multiplies and two global loads for every pixel where two
 * multiplies do. The compiler does not lift them itself at -Os. */
struct pf_fade
{
    unsigned int a;     /* alpha, rounded to the step the blend works in */
    unsigned int rb;    /* background red/blue contribution, pre-multiplied */
    unsigned int g;     /* background green contribution */
};

static inline void fade_prepare(struct pf_fade *f, unsigned int a)
{
    a = (a + 2) & 0x1fc;
    f->a  = a * pf_neighbor_light / 256;
    f->rb = pf_bg_rb * ((256 - a) * pf_neighbor_light / 256);
    f->g  = pf_bg_g  * ((256 - a) * pf_neighbor_light / 256);
}

static inline pix_t fade_color(pix_t c, unsigned int fa, unsigned int frb,
                               unsigned int fg)
{
    unsigned int rb = (fade_red_blue(c) * fa + frb) >> 8;
    unsigned int g = ((c & 0x7e0) * fa + fg) >> 8;
    return ((rb >> 5) & 0xf800) | (g & 0x07e0) | (rb & 0x001f);
}

/* Clear `h` rows starting `y` rows down this screen's viewport -- never the
 * status bar, which belongs to the theme. Uses screen->clear_viewport(), which
 * fills with bg_pattern, unlike lcd_fillrect(DRMODE_SOLID) which fills with
 * fg_pattern.
 *
 * The viewport is static because lcd_set_viewport() keeps the pointer rather
 * than a copy, so one on the stack would be dangling by the time anything drew
 * through it. */
static void pf_clear_rect(int x, int y, int w, int h)
{
    static struct viewport band_vp;
    struct viewport *saved;

    if (y < pf_chrome_h)
    {
        h -= pf_chrome_h - y;
        y = pf_chrome_h;
    }
    if (h <= 0 || w <= 0)
        return;

    band_vp = pf_vp;
    band_vp.y = pf_vp.y + y;
    band_vp.x += x;
    band_vp.width = w;
    band_vp.height = h;
    saved = lcd_set_viewport(&band_vp);
    screens[SCREEN_MAIN].clear_viewport();
    lcd_set_viewport(saved);
}

static void pf_clear_band(int y, int h)
{
    if (caption_clear_h)
        pf_clear_rect(previous_ink.left, y, previous_ink.right - previous_ink.left, h);
    else
        pf_clear_rect(0, y, pf_vp.width, h);
}

/* Where a bitmap `sh` rows tall sits in the drawable area. Shared, because two
 * separate things need the same answer and must not drift: the render, to know
 * where to start, and the frame logic, to know which rows a cover can reach.
 *
 * Clamped, because the lower loop reads its source row unsigned: a start below
 * row 0 would wrap to a huge index and read far outside the bitmap rather than
 * clipping. The upper loop needs no clamp; a negative start simply fails its
 * first test. */
static int slide_voff(int sh, PFreal cy)
{
    int voff = ((pf_half_height + pf_lower_half) - sh) / 2;

    if (cy > 0)
        voff += cy >> PFREAL_SHIFT;
    if (voff > pf_half_height)
        voff = pf_half_height;
    return voff;
}

/* Enclose the native-size flat bitmap and its dirty-row margin. */
#define PF_ROW_MARGIN 2

static void slide_rows(struct slide_data *slide, int *y0, int *y1)
{
    struct dim *bmp = surface(slide);
    int voff, sh;

    if (!bmp)
    {
        *y0 = pf_height;
        *y1 = 0;
        return;
    }
    sh = bmp->height;
    voff = slide_voff(sh, slide->cy);
    *y0 = MAX(0, voff - PF_ROW_MARGIN) + pf_draw_y_shift;
    *y1 = MIN(pf_half_height + pf_lower_half, voff + sh + PF_ROW_MARGIN)
        + pf_draw_y_shift;
}

/* Slides are drawn in tiles: PF_TILE_W columns -- one 16-byte cache line of a
 * row -- a band of PF_TILE_ROWS rows at a time. The PP502x cache is 4-way with
 * 128 sets of 16-byte lines, so rows 16 apart share a set and only 64 rows of
 * one column can be cached at once. Drawn a whole column at a time, a cover
 * column (up to 160 rows) evicts its own lines before the next column, which
 * shares every one of them, comes to write to them; tiled, each line is fetched
 * once for all eight, and a pixel costs ~20% less on the 5G. */
#define PF_TILE_W    8
#ifdef CPU_PP
#define PF_TILE_ROWS 32
#else
#define PF_TILE_ROWS LCD_HEIGHT
#endif

/* One column of a tile, from the middle row outward: upward or downward. */
struct pf_col
{
    const pix_t *src;   /* the source column */
    pix_t *px;          /* next pixel to write */
    int p;              /* source row, fixed point */
    int dp;             /* step in p per row: -dy upward, +dy downward */
    int n;              /* rows left to write */
};

/* The ARM926 halfword store can advance the destination by a register stride. */
static inline pix_t *column_store(pix_t *pixel, pix_t value, int step)
{
#if defined(IPOD_6G) && defined(__arm__) && !defined(__thumb__) && !defined(SIMULATOR)
    asm volatile("strh %1, [%0], %2" : "+&r"(pixel)
                 : "r"(value), "r"(step * (int)sizeof(*pixel)) : "memory");
    return pixel;
#else
    *pixel = value;
    return pixel + step;
#endif
}

/* `n` pixels of one column, from where it left off, `step` apart in the
 * framebuffer. The two runs are functions of their own, never inlined, so that
 * everything a pixel touches stays in a register: folded into the tile loop at
 * -Os the faded run reloaded its fade terms and its pointer from the stack on
 * every pixel, at nearly twice the cost of the arithmetic it does. Unrolled,
 * because -Os will not and the loop branch is a quarter of an opaque pixel. */
static void NO_INLINE draw_run(struct pf_col *k, int n, int step)
{
    const pix_t *src = k->src;
    pix_t *pixel = k->px;
    unsigned int p = k->p;
    const int dp = k->dp;

    if (dp == PFREAL_ONE && n >= 4)
    {
        const pix_t *at = src + (p >> PFREAL_SHIFT);
        int full = n & ~3;
        n -= full;
        p += full * (unsigned)dp;
        do
        {
#pragma GCC unroll 4
            for (int i = 0; i < 4; i++)
                pixel = column_store(pixel, *at++, step);
            full -= 4;
        } while (full);
    }
#if defined(IPOD_6G) && defined(__arm__) && !defined(__thumb__) && !defined(SIMULATOR)
    /* Keep the texel address in halfwords. Carry from the fractional
     * step advances it by one extra texel before the next sample. */
    if (n >= 8)
    {
        unsigned at = ((uintptr_t)src >> 1) + (p >> PFREAL_SHIFT);
        unsigned frac = p << (32 - PFREAL_SHIFT);
        unsigned increment = (unsigned)dp << (32 - PFREAL_SHIFT);
        unsigned blocks = n >> 3;
        unsigned value;
        p += (n & ~7) * (unsigned)dp;
        n &= 7;
        asm volatile(
            "1:\n"
            ".rept 8\n"
            "ldrh %[v], [%[at], %[at]]\n"
            "adds %[frac], %[frac], %[inc]\n"
            "adc %[at], %[at], %[whole]\n"
            "strh %[v], [%[dst]], %[stride]\n"
            ".endr\n"
            "subs %[blocks], %[blocks], #1\n"
            "bne 1b"
            : [at] "+&r"(at), [frac] "+&r"(frac), [dst] "+&r"(pixel),
              [blocks] "+&r"(blocks), [v] "=&r"(value)
            : [inc] "r"(increment), [whole] "r"(dp >> PFREAL_SHIFT),
              [stride] "r"(step * (int)sizeof(*pixel))
            : "cc", "memory");
    }
#else
    for (; n >= 4; n -= 4)
    {
#pragma GCC unroll 4
        for (int i = 0; i < 4; i++)
        {
            pixel = column_store(pixel, src[p >> PFREAL_SHIFT], step);
            p += dp;
        }
    }
#endif
    while (n-- > 0)
    {
        *pixel = src[p >> PFREAL_SHIFT]; p += dp; pixel += step;
    }
    k->p = p;
    k->px = pixel;
}

static void NO_INLINE draw_run_faded(struct pf_col *k, int n, int step,
                                     const struct pf_fade *f)
{
    const pix_t *src = k->src;
    pix_t *pixel = k->px;
    unsigned int p = k->p;
    const int dp = k->dp;
    const unsigned int fa = f->a, frb = f->rb, fg = f->g;

    for (; n >= 2; n -= 2)
    {
        *pixel = fade_color(src[p >> PFREAL_SHIFT], fa, frb, fg);
        p += dp; pixel += step;
        *pixel = fade_color(src[p >> PFREAL_SHIFT], fa, frb, fg);
        p += dp; pixel += step;
    }
    if (n > 0)
    {
        *pixel = fade_color(src[p >> PFREAL_SHIFT], fa, frb, fg);
        p += dp; pixel += step;
    }
    k->p = p;
    k->px = pixel;
}

/* Draw one half, upper or lower, of a tile's columns a band at a time. `step`
 * is the framebuffer stride in the half's direction; `f` is NULL for an opaque
 * slide. Returns the pixels written. */
static int draw_tile_half(struct pf_col *cols, int ncols, int step,
                          const struct pf_fade *f)
{
    int total = 0;
    bool more = true;

    while (more)
    {
        more = false;
        for (int c = 0; c < ncols; c++)
        {
            struct pf_col *k = &cols[c];
            int n = MIN(k->n, PF_TILE_ROWS);

            if (n <= 0)
                continue;
#ifdef CPU_PP
            k->n -= n;
            more |= (k->n > 0);
#endif
            total += n;
            if (f)
                draw_run_faded(k, n, step, f);
            else
                draw_run(k, n, step);
        }
    }
    return total;
}

/* The split line is a pixel boundary. extent is the source distance from
 * that line to the silhouette. Only the last pixel needs the framebuffer. */
struct pf_extent { int rows; unsigned int coverage; };

static inline struct pf_extent project_extent(int extent, int dy, unsigned int recip)
{
    int rows = div_by_recip(extent, dy, recip);
    unsigned int rem = extent - rows * dy;
    unsigned int fraction = ((uint64_t)rem * recip) >> 24;
    return (struct pf_extent){rows, (fraction + 2) & 0x1fc};
}

static inline void cover_half(struct pf_col *col, struct pf_extent span,
                              int limit, int step, pix_t edge,
                              const struct pf_fade *fade)
{
    int full = span.rows;
    col->n = MIN(full, limit);
    if (full >= limit)
        return;
    unsigned int coverage = span.coverage;
    if (!coverage)
        return;
    if (fade)
        edge = fade_color(edge, fade->a, fade->rb, fade->g);
    pix_t *pixel = col->px + full * step;
    unsigned int bg = *pixel;
    *pixel = fade_color(edge, coverage, fade_red_blue(bg) * (256 - coverage),
                        (bg & 0x07e0) * (256 - coverage));
}

#define LCDADDR(x, y) (&buffer[(y)*BUFFER_WIDTH + (x)])

/* Flat cards translate at their native bitmap size. */
static void render_slide(struct slide_data *slide, int alpha)
{
    struct dim *bmp = surface(slide);
    if (!bmp)
        return;
    const pix_t *src = (const pix_t *)(bmp + 1);
    int left = (slide->cx + (LCD_WIDTH - bmp->width) * PFREAL_HALF
                + PFREAL_ONE - 1) >> PFREAL_SHIFT;
    int top = slide_voff(bmp->height, slide->cy);
    int y = MAX(0, top);
    int rows = MIN(top + bmp->height, pf_half_height + pf_lower_half) - y;
    if (rows <= 0)
        return;
    struct pf_fade fade;
    fade_prepare(&fade, alpha);
    for (int x = MAX(0, left); x < MIN(LCD_WIDTH, left + bmp->width); x++)
    {
        struct pf_col col = {src + (x - left) * bmp->height, LCDADDR(x, y),
                             (y - top) * PFREAL_ONE, PFREAL_ONE, rows};
        if (alpha == 256 && pf_neighbor_light == 256)
            draw_run(&col, rows, BUFFER_WIDTH);
        else
            draw_run_faded(&col, rows, BUFFER_WIDTH, &fade);
#if PF_SHOW_STATS
        pf_px += rows;
#endif
    }
}

/* The panel flip projects from FLIP_DIST, with the pivot at the
 * centre of the card. Only the artwork is texture mapped. The moving back
 * is a solid surface, and its text is drawn separately at the flat pose. */
static void render_flip_face(int angle, int width, int height,
                             bool back, pix_t color, int center_y, int band)
{
    if (angle <= -256 || angle >= 256 || width <= 0 || height <= 0 ||
        center_y <= 0 || center_y >= band)
        return;

    const pix_t *src = NULL;
    int sw = width, sh = height, stride = sh;
    if (!back)
    {
        struct dim *bmp = surface(&center_slide);
        if (!bmp)
            return;
        sw = bmp->width;
        sh = bmp->height;
        src = (const pix_t *)(bmp + 1);
        stride = sh;
    }
    if (sw <= 0 || sh <= 0)
        return;

    const int cosine = fcos(angle), sine = fsin(angle);
    const int plane = 2 * FLIP_DIST_R;
    /* Left/right edge offsets in whole pixels, rounded outward. Test pixel
     * centres against the texture interval to resolve the last half pixel. */
    const int left_den = plane - width * sine;
    const int right_den = plane + width * sine;
    if (left_den <= 0 || right_den <= 0)
        return;
    const int edge = FLIP_DIST * width * cosine;
    const int first = MAX(0, LCD_WIDTH / 2 - (edge + left_den - 1) / left_den);
    const int last = MIN(LCD_WIDTH, (LCD_WIDTH + 1) / 2 + (edge + right_den - 1) / right_den);
    const int u_scale = sw * PFREAL_ONE / width;
    const int v_scale = sh * PFREAL_ONE / height;
    const int split = sh * PFREAL_HALF;
    struct pf_col cols[PF_TILE_W];
    int ncols = 0;
    for (int x = first; x < last; ++x)
    {
        const int xp2 = 2 * x + 1 - LCD_WIDTH;
        int xs = xp2 * PFREAL_HALF;
        int dy = v_scale;
        if (angle)
        {
            const int denom = 2 * FLIP_DIST * cosine - xp2 * sine;
            if (denom <= 0)
                continue;
            xs = fdiv(xp2 * FLIP_DIST_R, denom);
            dy = fmul(v_scale, div_flip_dist(FLIP_DIST_R + fmul(xs, sine)));
        }
        int u = xs + width * PFREAL_HALF;
        if ((unsigned)u >= (unsigned)(width * PFREAL_ONE))
            continue;
        const int col = fmul(u, u_scale) >> PFREAL_SHIFT;
        const pix_t *column = back ? NULL : src + col * stride;
        dy = MAX(1, dy);
        unsigned int recip = 0xffffffffu / dy;
        struct pf_extent top = project_extent(split, dy, recip);
        struct pf_col upper = {column, LCDADDR(x, center_y - 1), 0, -dy, 0};
        struct pf_col lower = {column, LCDADDR(x, center_y), 0, dy, 0};
        cover_half(&upper, top, center_y, -BUFFER_WIDTH, back ? color : column[0], NULL);
        cover_half(&lower, top, band - center_y, BUFFER_WIDTH, back ? color : column[sh - 1], NULL);
        if (back)
        {
            for (int y = center_y - upper.n; y < center_y + lower.n; y++)
                *LCDADDR(x, y) = color;
            continue;
        }
        /* Pixel centres sample half a destination step below the top edge. */
        cols[ncols++] = (struct pf_col){column, LCDADDR(x, center_y - upper.n),
            split - upper.n * dy + dy / 2, dy, upper.n + lower.n};
        if (ncols == PF_TILE_W || (x & (PF_TILE_W - 1)) == PF_TILE_W - 1)
        {
            draw_tile_half(cols, ncols, BUFFER_WIDTH, NULL);
            ncols = 0;
        }
    }
    if (ncols)
        draw_tile_half(cols, ncols, BUFFER_WIDTH, NULL);
}

void set_current_slide(const int slide_index)
{
    buf_ctx_lock();
    int old_center_index = center_index;
    pf_state = pf_idle;
    step = 0;
    unsigned scale = ui_theme_duration(UI_THEME_MOTION_ONE);
    bool clock_changed = pf_motion_scale != scale;
    pf_motion_scale = scale;
    if (clock_changed || !flat_view())
    {
        pf_flat_interval = flat_duration(PF_FLAT_FLIP);
        pf_flat_last_input = motion_clock();
    }
    pf_random_spinning = false;
    center_index = fbound(0, slide_index, number_of_slides - 1);
    if (thread_is_running && old_center_index != center_index)
    {
        wake_pf_thread();
    }
    target = center_index;
    slide_frame = center_index << 16;
    reset_slides();
    buf_ctx_unlock();
}

static void return_to_idle_state(void)
{
    if (pf_state == pf_scrolling)
        set_current_slide(target);
    pf_state = pf_idle;
}

/* Set right before jumping to an album's track list (PF_SELECT below),
 * consumed here the next time Album covers opens. Needed because Album
 * covers doesn't resume an existing session when the user backs out of
 * that track list -- root_menu.c's GO_TO_PREVIOUS handling re-enters via a
 * fresh album_covers(NULL) call, same as any other new entry, and
 * selected_file is NULL either way -- so without this, the branch below
 * would use its normal "audio_status() ? now-playing : last_album"
 * fresh-entry heuristic and land on whatever's currently playing instead
 * of the cover the user just came from. That heuristic makes sense when
 * actually entering fresh (from the main menu, a WPS shortcut, etc., where
 * seeing "the current album" first is reasonable) but not when coming
 * straight back from browsing this exact cover's own tracks. */
bool pf_resume_last_album = false;
/* The index to resume to, captured separately from pf_cfg.last_album:
 * init() calls pf_config_load() (reloading pf_cfg from its on-disk file)
 * before set_initial_slide() runs, which clobbers whatever was just
 * assigned to pf_cfg.last_album in the PF_SELECT handler below with
 * whatever index was last saved to disk -- silently resuming the wrong
 * album (observed as always landing back on the first one). This is a
 * purely transient, in-memory signal that must never round-trip through
 * the persisted config. */
int pf_resume_album_index;

/* carousel_settle: settle an in-progress scroll animation to idle. */
void carousel_settle(void)
{
    return_to_idle_state();
}

/* carousel_reload: restart the render pipeline over the current index buffer.
 * The loader thread is stopped first (so `compare`, if given, can re-sort the
 * album index without racing it), then the slide cache is rebuilt and the
 * loader restarted. */
void carousel_reload(int (*compare)(const void *, const void *))
{
    end_pf_thread(); /* stop loading of covers */

    if (compare)
    {
        qsort(carousel_idx.album_index, carousel_idx.album_ct,
              sizeof(struct album_data), compare);
        pf_random_count = 0;
    }

    /* Empty cache and restart cover loading thread */
    buflib_init(&buf_ctx, (void *)carousel_idx.buf, carousel_idx.buf_sz);

    /* The handle from before the buflib_init() above means nothing now, so the
     * placeholder has to be read into the new context -- and the result
     * checked, as init() checks it. Left unchecked, a -1 reaches get_slide()
     * for every slide that has no cover yet. Zero is the honest answer when it
     * cannot be had: those slides then draw nothing until the screen is
     * reopened, which is where create_empty_slide() writes the file again. */
    empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0);
    if (empty_slide_hid < 0)
    {
        empty_slide_hid = 0;
        splash(HZ, ID2P(LANG_CAROUSEL_NO_PLACEHOLDER));
    }

    initialize_slide_cache();
    create_pf_thread();
}

/**
  Start the animation for changing slides
 */
static void start_animation(void)
{
    if (!pf_motion_scale)
    {
        set_current_slide(target);
        return;
    }
    step = (target < center_slide.slide_index) ? -1 : 1;
    pf_state = pf_scrolling;
    anim_clock = ui_clock_start();
    anim_sampled_frame = 0;
    if (!flat_view())
    {
        stock_scroll(&flow_row, target - flow_row.selected, USEC_TIMER);
        return;
    }
    anim_pose_step = step;
    anim_frac = 0;
    pf_flat_snap = flat_pace(motion_clock()) < flat_duration(PF_FLAT_MIN);
}

static void reverse_animation(int direction)
{
    target = center_index + (anim_pose_step == direction ? direction : 0);
    step = direction;
    anim_clock = ui_clock_start();
    anim_sampled_frame = 0;
}

/* One wheel click in the 3D view: the target moves a slide, and the covers
 * chase it from wherever they are. */
static void flow_click(int direction)
{
    int to = fbound(0, target + direction, number_of_slides - 1);
    if (to == target || flow_row.phase == STOCK_LEAVE)
        return;
    if (pf_state == pf_scrolling)
        stock_scroll(&flow_row, to - target, USEC_TIMER);
    target = to;
    if (pf_state != pf_scrolling)
        start_animation();
}

/* The wheel posts physical detents in bits 24..30 and velocity below them.
 * A key or accessory action without a wheel payload moves one album. */
static int flow_wheel_steps(void)
{
    int button;
    unsigned data = get_action_data();
    get_action_statuscode(&button);
    if ((button & (BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD)) && (data >> 31))
        return MAX(1u, (data >> 24) & 0x7f);
    return 1;
}

static void show_previous_slide(void)
{
    pf_random_spinning = false;
    if (!flat_view()) {
        flow_click(-flow_wheel_steps());
        return;
    }
    flat_note_scroll();
    if (step == 0) {
        if (center_index > 0) {
            target = center_index - 1;
            start_animation();
        }
    } else if ( step > 0 ) {
        reverse_animation(-1);
    } else {
        target = MAX(0, target - 1);
    }
}

static void show_next_slide(void)
{
    pf_random_spinning = false;
    if (!flat_view()) {
        flow_click(flow_wheel_steps());
        return;
    }
    flat_note_scroll();
    if (step == 0) {
        if (center_index < number_of_slides - 1) {
            target = center_index + 1;
            start_animation();
        }
    } else if ( step < 0 ) {
        reverse_animation(1);
    } else {
        target = MIN(target + 1, number_of_slides - 1);
    }
}

static bool random_was_recent(int index, int depth)
{
    for (int i = 0; i < depth; i++)
        if (pf_random_recent[i] == index)
            return true;
    return false;
}

/* Spin to a random slide, never the current one or a recent pick. With few
 * slides the history shrinks, so there is always one left to choose. */
static void show_random_slide(void)
{
    int n = number_of_slides;
    int depth, pick, from, spin, i;

    if (n < 2)
        return;
    if (pf_random_model != model)
    {
        pf_random_model = model;
        pf_random_count = 0;
    }

    return_to_idle_state();

    depth = MIN(pf_random_count, n - 2);
    srand(current_tick);
    do
        pick = rand() % n;
    while (pick == center_index || random_was_recent(pick, depth));

    for (i = PF_RANDOM_HISTORY - 1; i > 0; i--)
        pf_random_recent[i] = pf_random_recent[i - 1];
    pf_random_recent[0] = pick;
    if (pf_random_count < PF_RANDOM_HISTORY)
        pf_random_count++;

    spin = pf_random_spin[global_settings.album_covers_random_spin
                          % ARRAYLEN(pf_random_spin)];
    from = (pick > center_index) ? MAX(center_index, pick - spin)
                                 : MIN(center_index, pick + spin);
    if (from != center_index)
        set_current_slide(from);
    target = pick;

    /* Paced as a fast flick, so the flat view snaps the covers going past and
     * deals the last few, rather than dealing every one at a browsing pace. */
    pf_flat_last_input = motion_clock();
    pf_flat_interval = MAX(flat_duration(PF_FLAT_MIN) / PF_FLAT_SETTLE, 1);
    start_animation();
    pf_random_spinning = pf_state == pf_scrolling;
}

/* A flat pile hides everything but its top card, so only four covers are ever
 * worth drawing, and the order they go in is fixed rather than sorted. Both
 * moving covers sit above both piles throughout -- a cover at rest lies on the
 * piles it overlaps, and neither of these is at rest -- so the only question is
 * which of the two is above the other, and that is the handover. */
/* Piles taken all the way to the background have nothing left to draw, and
 * drawing them anyway would paint background over what is behind them. */
static void flat_draw(struct slide_data *slide)
{
    if (slide->alpha > 0)
        render_slide(slide, slide->alpha);
}

static void flat_render(bool center)
{
    struct slide_data *pile_near, *pile_next, *arriving;

    if (!pf_flat_moving) {
        flat_draw(&left_slides[0]);
        flat_draw(&right_slides[0]);
        if (center)
            flat_draw(&center_slide);   /* overlaps both, so it goes last */
        return;
    }

    pile_near = (step > 0) ? &left_slides[0]  : &right_slides[0];
    pile_next = (step > 0) ? &right_slides[1] : &left_slides[1];
    arriving  = (step > 0) ? &right_slides[0] : &left_slides[0];

    flat_draw(pile_near);
    flat_draw(pile_next);
    if (pf_flat_handover) {
        flat_draw(&center_slide);
        flat_draw(arriving);
    } else {
        flat_draw(arriving);
        flat_draw(&center_slide);
    }
}

/* stock_sample emits outer covers first and the front cover last. */
struct flow_face
{
    const pix_t *pixels;
    /* The reflection continues each column below the cover: `rows` texels
     * a column, or none. */
    const pix_t *refl;
    int rows;
    int stride, size, crop;
    int first, end, t0, dt, start_height;
    bool linear;
    int top, top_delta, end_height; /* Q16 projected edge coordinates */
};

struct flow_column
{
    int y0, height, t; /* Q16 top and height and Q30 fraction along the winning face */
    const struct flow_face *face;
};

static struct flow_column flow_columns[LCD_WIDTH];

/* A column's rows from its top, the reflection's included: it is half the
 * cover's height, as its texture is half the cover's. */
static inline int flow_extent(const struct flow_face *face, int height)
{
    return face->rows ? height + height / 2 : height;
}
static int16_t flow_top[LCD_WIDTH], flow_end[LCD_WIDTH];
static struct flow_face flow_faces[STOCK_MAX_VISIBLE];
static struct slide_data flow_surfaces[STOCK_MAX_VISIBLE];

static int flow_clip_top(void)
{
    return MAX(pf_chrome_h, pf_draw_y_shift && pf_text_h ? pf_text_y + pf_text_h : 0);
}

/* The stock look draws its caption over the reflections. */
static int flow_clip_end(void)
{
    return !stock_look() && !pf_draw_y_shift && pf_text_h ? pf_text_y : pf_height;
}

/* For d normalized to [2^31,2^32), m approximates 2^62/d. Bin midpoints
 * seed two fixed-point Newton iterations. Callers correct the final quotient
 * against the original integer operands. */
static const uint16_t flow_recip_seed[128] = {
    65281, 64777, 64281, 63792, 63310, 62836, 62369, 61909, 61455, 61008, 60568, 60133,
    59705, 59283, 58867, 58457, 58053, 57654, 57260, 56872, 56489, 56111, 55738, 55370,
    55007, 54649, 54295, 53946, 53601, 53261, 52925, 52593, 52265, 51942, 51622, 51306,
    50995, 50686, 50382, 50081, 49784, 49490, 49200, 48913, 48630, 48349, 48072, 47798,
    47528, 47260, 46995, 46733, 46474, 46218, 45965, 45714, 45467, 45222, 44979, 44739,
    44502, 44267, 44035, 43805, 43577, 43352, 43129, 42908, 42690, 42474, 42260, 42048,
    41838, 41631, 41425, 41222, 41020, 40820, 40623, 40427, 40233, 40041, 39851, 39662,
    39476, 39291, 39108, 38926, 38746, 38568, 38392, 38217, 38044, 37872, 37702, 37533,
    37366, 37200, 37036, 36873, 36712, 36552, 36393, 36236, 36080, 35926, 35772, 35620,
    35470, 35320, 35172, 35026, 34880, 34735, 34592, 34450, 34309, 34169, 34031, 33893,
    33757, 33622, 33487, 33354, 33222, 33091, 32961, 32832,
};

struct flow_recip {
    uint32_t m;
    unsigned norm;
};

static inline struct flow_recip flow_reciprocal(uint32_t denominator)
{
    unsigned norm = __builtin_clz(denominator);
    uint32_t d = denominator << norm;
    uint32_t m = (uint32_t)flow_recip_seed[(d >> 24)-128] << 15;
    for (int i = 0; i < 2; i++) {
        int32_t error = (1 << 30) - (uint32_t)((uint64_t)d*m >> 32);
        m += (int32_t)((int64_t)m*error >> 30);
    }
    return (struct flow_recip){ m, norm };
}

/* Q30 quotients use the normalized reciprocal, then correct its last bits
 * against the original numerator and denominator. The low word suffices:
 * the initial estimate differs by at most a few units on a clipped span. */
static inline uint32_t flow_quotient(uint32_t low, uint32_t width, uint32_t q)
{
    int32_t rem = low - q*width;
    while (rem < 0) {
        rem += width;
        q--;
    }
    while ((uint32_t)rem >= width) {
        rem -= width;
        q++;
    }
    return q;
}

static void flow_cover_columns(const struct flow_card *card, int slot, int spread)
{
    struct flow_face *face = &flow_faces[slot];
    face->end = 0;
    int x0 = card->edge[0].x, x1 = card->edge[1].x;
    /* Album-panel neighbours spread in world pixels (Q10). */
    int dx = card->id < center_index ? -spread : spread;
    if (dx)
    {
        x0 += (int64_t)dx * STOCK_CAMERA * (1LL << 22) / card->edge[0].depth;
        x1 += (int64_t)dx * STOCK_CAMERA * (1LL << 22) / card->edge[1].depth;
    }
    int edge0 = (x0 + 32767) >> 16;
    int edge1 = (x1 + 32767) >> 16;
    bool reversed = edge0 > edge1;
    int left = MIN(x0, x1), width = abs(x1 - x0);
    int first = MAX(0, MIN(edge0, edge1));
    int end = MIN(LCD_WIDTH, MAX(edge0, edge1));
    if (first >= end)
        return;

    struct slide_data *slide = &flow_surfaces[slot];
    slide->slide_index = card->id;
    const struct dim *bmp = surface(slide);
    if (!bmp)
        return;
#ifdef SIMULATOR
    if (model->external)
    {
        int slot = slide->cached_slot;
        unsigned quality = slot >= 0 ? pf_sldcache.cache[slot].version & 3 : 0;
        sim_cider_visible(card->id, bmp != get_slide(empty_slide_hid) ? quality : 0,
                          ((const pix_t *)(bmp + 1))[0]);
    }
#endif
    /* Interpolate in Q30: a rounded Q16 step accumulates enough error
     * across a face to exchange nearly coincident depth samples. */
    int offset = MAX(0, MIN(width, (first << 16) + 32768 - left));
    int t = 1 << 29, dt = 0;
    if (width) {
        struct flow_recip rec = flow_reciprocal(width);
        uint32_t q = ((uint64_t)(unsigned)offset * rec.m) >> (32-rec.norm);
        t = flow_quotient((unsigned)offset << 30, width, q);
        if (end - first > 1) {
            q = rec.norm <= 16 ? rec.m >> (16-rec.norm) : rec.m << (rec.norm-16);
            dt = flow_quotient(0, width, q);
        }
    }
    int h0 = card->edge[0].bottom - card->edge[0].top;
    int h1 = card->edge[1].bottom - card->edge[1].top;
    int size = MIN(bmp->width, bmp->height);
    const pix_t *refl = stock_look() ? slide_reflection(bmp) : NULL;
    *face = (struct flow_face) {
        .pixels = (const pix_t *)(bmp+1) + (bmp->height-size)/2,
        .refl = refl, .rows = refl ? reflection_rows(size) : 0,
        .stride = bmp->height, .size = size, .crop = (bmp->width-size)/2,
        .linear = card->linear, .top = card->edge[0].top,
        .top_delta = card->edge[1].top - card->edge[0].top,
        .start_height = h0, .end_height = h1, .first = first, .end = end
    };
    if (reversed)
    {
        t = (1 << 30) - t;
        dt = -dt;
    }
    const bool depth_test = flow_row.phase == STOCK_SCROLL || flow_row.phase == STOCK_REST;
    face->t0 = t;
    face->dt = dt;
    for (int x = first; x < end; x++, t = (uint32_t)t + dt)
    {
        int h = h0 + ((int64_t)(h1 - h0) * t >> 30);
        int y0 = face->top + ((int64_t)face->top_delta * t >> 30);
        struct flow_column *col = &flow_columns[x];
        /* At rest and while scrolling, projected height gives reciprocal
         * depth on this ray. Intersecting faces can exchange order mid-span. */
        if (depth_test)
        {
            if (h < col->height)
                continue;
        }
        else if (col->height)
        {
            flow_top[x] = MIN(flow_top[x], col->y0 >> 16);
            flow_end[x] = MAX(flow_end[x],
                (col->y0 + flow_extent(col->face, col->height) + 65535) >> 16);
        }
        col->y0 = y0;
        col->height = h;
        col->t = t;
        col->face = face;
    }
}

/* y1 ends the column's extent; refl is its reflection column, if any. */
struct flow_sample
{
    const pix_t *src, *refl;
    int size, rows, y0, y1, first, last, dy, p, x_weight;
};

static void flow_sample_column(const struct flow_column *col, int top, int end,
                               struct flow_sample *sample)
{
    const struct flow_face *face = col->face;
    int size = face->size;
    int t = col->t;
    /* Only the nearest face needs a texture column. One reciprocal also
     * gives the vertical texture step, whose denominator drops six bits. */
    int u_height = (int64_t)face->end_height * MAX(0, t) >> 30;
    uint32_t num = (uint32_t)u_height * size;
    uint32_t height = col->height, rows = height >> 6;
    struct flow_recip rec = flow_reciprocal(rows);
    uint32_t u_guess = ((uint64_t)num * rec.m) >> (68-rec.norm);
    int32_t u_rem = num - u_guess*height;
    while (u_rem < 0) {
        u_guess--;
        u_rem += height;
    }
    if (u_rem >= (int32_t)height) {
        u_guess++;
        u_rem -= height;
    }
    int u = MIN(u_guess, (unsigned)(size - 1));
    int x_weight = 0;
    if (face->linear)
    {
        /* Texture coordinate zero is half a texel before the first centre. */
        int fraction = ((uint64_t)(uint32_t)u_rem * rec.m) >> (63 - rec.norm);
        int coord = fbound(0, (int)u_guess * 32 + fraction - 16, (size - 1) * 32);
        u = coord >> 5;
        x_weight = coord & 31;
    }
    const pix_t *src = face->pixels + (face->crop + u) * face->stride;
    const pix_t *refl = face->rows ? face->refl + u * face->rows : NULL;
    int y0 = col->y0, y1 = y0 + flow_extent(face, col->height);
    int first = MAX(top, (y0 + 65535) >> 16);
    int last = MIN(end, y1 >> 16);
    uint32_t d_num = (uint32_t)size << 20;
    int dy = ((uint64_t)d_num * rec.m) >> (62-rec.norm);
    int32_t d_rem = d_num - dy*rows;
    dy += (d_rem >= (int32_t)rows) - (d_rem < 0);
    int p = ((int64_t)((first << 16) + 32768 - y0) * dy) >> 16;
    *sample = (struct flow_sample){ src, refl, size, face->rows, y0, y1, first,
                                    last, dy, p, x_weight };
}

/* Separate RGB565 into lanes with five spare bits for interpolation. */
#define FLOW_COLOR_LANES 0x07e0f81fu

static inline uint32_t flow_lanes(uint32_t color)
{
    return (color | (color << 16)) & FLOW_COLOR_LANES;
}

static inline uint32_t flow_mix(uint32_t a, uint32_t b, unsigned weight)
{
    return (a + (((b - a) * weight) >> 5)) & FLOW_COLOR_LANES;
}

static __attribute__((noinline, optimize("O3"))) void flow_draw_linear(const struct flow_sample *sample, int src_stride,
                             pix_t *dst, int dst_stride, const struct pf_fade *ink)
{
    /* Pad the endpoints to clamp the bilinear footprint to the square crop.
     * A reflection's texels follow the cover's, as its quad follows. */
    static uint32_t texels[DISPLAY_HEIGHT * 3 / 2 + 2];
    uint32_t *at = texels + 1;
    const pix_t *src = sample->src, *refl = sample->refl;
    int size = sample->size, rows = sample->rows, weight = sample->x_weight;
    if (weight)
    {
        for (int j = 0; j < size; j++)
            at[j] = flow_mix(flow_lanes(src[j]), flow_lanes(src[j + src_stride]), weight);
        for (int j = 0; j < rows; j++)
            at[size + j] = flow_mix(flow_lanes(refl[j]), flow_lanes(refl[j + rows]), weight);
    }
    else
    {
        for (int j = 0; j < size; j++)
            at[j] = flow_lanes(src[j]);
        for (int j = 0; j < rows; j++)
            at[size + j] = flow_lanes(refl[j]);
    }
    size += rows;
    at[-1] = at[0];
    at[size] = at[size - 1];
    int p = sample->p - PFREAL_HALF;
    for (int y = sample->first; y < sample->last; y++, dst += dst_stride, p += sample->dy)
    {
        int j = p >> PFREAL_SHIFT;
        unsigned v = (p >> (PFREAL_SHIFT - 5)) & 31;
        uint32_t c = flow_mix(at[j], at[j + 1], v);
        pix_t color = c | (c >> 16);
        *dst = ink ? fade_color(color, ink->a, ink->rb, ink->g) : color;
    }
}

/* Rows whose texel lies past the cover read on in its reflection. */
static void flow_draw_nearest(const struct flow_sample *sample, pix_t *dst,
                              int stride, const struct pf_fade *ink)
{
    int n = sample->last - sample->first, split = n;
    int limit = sample->size << PFREAL_SHIFT;
    if (sample->refl)
        split = sample->p >= limit ? 0 :
                MIN(n, (limit - sample->p + sample->dy - 1) / sample->dy);
    struct pf_col run = {sample->src, dst, sample->p, sample->dy, split};
    for (int part = 0; part < 2; part++)
    {
        if (run.n > 0)
        {
            if (ink)
                draw_run_faded(&run, run.n, stride, ink);
            else
                draw_run(&run, run.n, stride);
        }
        run.src = sample->refl;
        run.p -= limit;
        run.n = n - split;
    }
#if PF_SHOW_STATS
    pf_px += n;
#endif
}

static void flow_draw_span(const struct flow_column *col, int top, int end,
                           pix_t *pixels, int stride, const struct pf_fade *ink)
{
    struct flow_sample sample;
    flow_sample_column(col, top, end, &sample);
    const pix_t *src = sample.src;
    int size = sample.size, y0 = sample.y0, y1 = sample.y1;
    int first = sample.first, last = sample.last;
    if (first < last && col->face->linear)
        flow_draw_linear(&sample, col->face->stride, pixels + first * stride, stride, ink);
    else if (first < last)
        flow_draw_nearest(&sample, pixels + first * stride, stride, ink);
    /* Only the outer row on each silhouette has fractional coverage. */
    for (int edge = 0; edge < 2; edge++)
    {
        int y = edge ? y1 >> 16 : y0 >> 16;
        int amount = edge ? (y1 & 65535) : ((-y0) & 65535);
        if (!amount || y < top || y >= end)
            continue;
        unsigned a = ((amount + 512) >> 10) * 4;
        pix_t pixel = !edge ? src[0] : sample.refl ? sample.refl[sample.rows - 1]
                                                   : src[size - 1];
        if (ink)
            pixel = fade_color(pixel, ink->a, ink->rb, ink->g);
        pix_t *dest = pixels + y * stride;
        *dest = fade_color(pixel, a, fade_red_blue(*dest) * (256 - a),
                           (*dest & 0x07e0) * (256 - a));
    }
}

/* Draw exposed strips of earlier faces above and below the front face's
 * opaque span. The order also composites fractional silhouette rows. */
static void flow_draw_column(int x, int top, int end, pix_t *pixels,
                             int stride, const struct pf_fade *ink)
{
    const struct flow_column *front = &flow_columns[x];
    if (!front->height)
        return;
    const struct flow_face *face = front->face;
    int y0 = front->y0;
    int first = (y0 + 65535) >> 16;
    int last = (y0 + flow_extent(face, front->height)) >> 16;
    if (flow_top[x] < first || flow_end[x] > last)
    {
        for (const struct flow_face *f = flow_faces; f < face; f++)
        {
            if (x < f->first || x >= f->end)
                continue;
            int t = f->t0 + (x - f->first) * f->dt;
            int h = f->start_height + ((int64_t)(f->end_height - f->start_height) * t >> 30);
            int y = f->top + ((int64_t)f->top_delta * t >> 30);
            struct flow_column back = { y, h, t, f };
            if ((y >> 16) < first && first > top)
                flow_draw_span(&back, top, MIN(end, first), pixels, stride, ink);
            if (((y + flow_extent(f, h) + 65535) >> 16) > last && last < end)
                flow_draw_span(&back, MAX(top, last), end, pixels, stride, ink);
        }
    }
    flow_draw_span(front, top, end, pixels, stride, ink);
}

#define FLOW_STRIP_WIDTH 16
static pix_t flow_strip[LCD_HEIGHT * FLOW_STRIP_WIDTH] CACHEALIGN_ATTR;

/* One aligned RGB565 cache line per row. Both source and destination point
 * to the first requested screen row. One of them is the contiguous strip. */
static inline void flow_copy_strip(pix_t *dst, int dst_stride, const pix_t *src,
                                   int src_stride, int rows)
{
#if defined(IPOD_6G) && defined(__arm__) && !defined(__thumb__) && !defined(SIMULATOR)
    asm volatile(
        "1:\n"
        "ldmia %[src]!, {r1-r8}\n"
        "stmia %[dst]!, {r1-r8}\n"
        "add %[src], %[src], %[src_gap]\n"
        "add %[dst], %[dst], %[dst_gap]\n"
        "subs %[rows], %[rows], #1\n"
        "bne 1b"
        : [src] "+&r"(src), [dst] "+&r"(dst), [rows] "+&r"(rows)
        : [src_gap] "Ir"((src_stride - FLOW_STRIP_WIDTH) * (int)sizeof(*src)),
          [dst_gap] "Ir"((dst_stride - FLOW_STRIP_WIDTH) * (int)sizeof(*dst))
        : "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "cc", "memory");
#else
    for (int y = 0; y < rows; y++)
        memcpy(dst + y * dst_stride, src + y * src_stride,
               FLOW_STRIP_WIDTH * sizeof(*src));
#endif
}

/* The uncommitted strip and the framebuffer are cache-line aligned. */
static bool flow_row_equal(const pix_t *strip, const pix_t *frame)
{
    typedef uint32_t pixel_pair __attribute__((may_alias));
    const pixel_pair *a = (const pixel_pair *)strip;
    const pixel_pair *b = (const pixel_pair *)frame;
#pragma GCC unroll 8
    for (int i = 0; i < FLOW_STRIP_WIDTH / 2; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

struct flow_damage { int top, end; };
static struct flow_damage *flow_damage;

/* Compare before writeback, including background exposed by moving artwork. */
static void flow_write_strip(pix_t *frame, const pix_t *strip, int y, int rows)
{
    if (flow_damage)
    {
        while (rows && y < flow_damage->top && flow_row_equal(strip, frame))
        {
            y++;
            rows--;
            strip += FLOW_STRIP_WIDTH;
            frame += BUFFER_WIDTH;
        }
        if (!rows)
            return;
        int end = y + rows;
        while (end > flow_damage->end && end > y &&
               flow_row_equal(strip + (end-y-1) * FLOW_STRIP_WIDTH,
                              frame + (end-y-1) * BUFFER_WIDTH))
            end--;
        if (end == y)
            return;
        flow_damage->top = MIN(flow_damage->top, y);
        flow_damage->end = MAX(flow_damage->end, end);
        rows = end - y;
    }
    flow_copy_strip(frame, BUFFER_WIDTH, strip, FLOW_STRIP_WIDTH, rows);
}

static void flow_paint(bool center, int spread, int clear_y, int clear_h)
{
    uint32_t start = flow_stages ? USEC_TIMER : 0;
    if (clear_h > 0 && clear_y < pf_chrome_h)
    {
        clear_h -= pf_chrome_h - clear_y;
        clear_y = pf_chrome_h;
        if (clear_h <= 0)
            return;
    }
    for (int x = 0; x < LCD_WIDTH; x++)
    {
        flow_columns[x].height = 0;
        flow_top[x] = pf_height;
        flow_end[x] = 0;
    }
    for (int i = 0; i < flow_count; i++)
        if (center || flow_cards[i].id != center_index)
            flow_cover_columns(&flow_cards[i], i, spread);
        else
            flow_faces[i].end = 0;
    if (flow_stages)
    {
        uint32_t now = USEC_TIMER;
        flow_stages[UI_COVER_COLUMNS] += now - start;
        start = now;
    }

    struct pf_fade fade;
    fade_prepare(&fade, 256);
    const struct pf_fade *ink = pf_neighbor_light == 256 ? NULL : &fade;
    int top = flow_clip_top(), end = flow_clip_end();
    pix_t *frame = buffer - pf_draw_y_shift * BUFFER_WIDTH;
    uint32_t clear_us = 0;
    if (clear_h > 0)
    {
        bool inverse = pf_vp.drawmode & DRMODE_INVERSEVID;
        const pix_t *backdrop = inverse ? NULL : lcd_get_backdrop();
        pix_t bg = inverse ? pf_vp.fg_pattern : pf_bg_color;
        top = MAX(top, clear_y);
        end = MIN(end, clear_y + clear_h);
        for (int x = 0; x < LCD_WIDTH; x += FLOW_STRIP_WIDTH)
        {
            uint32_t clear_start = flow_stages ? USEC_TIMER : 0;
            pix_t *strip = flow_strip + clear_y * FLOW_STRIP_WIDTH;
            if (backdrop)
                flow_copy_strip(strip, FLOW_STRIP_WIDTH,
                    backdrop + (pf_vp_y + clear_y) * BUFFER_WIDTH + x,
                    BUFFER_WIDTH, clear_h);
            else
                memset16(strip, bg, clear_h * FLOW_STRIP_WIDTH);
            if (flow_stages)
                clear_us += (uint32_t)USEC_TIMER - clear_start;
            for (int col = 0; col < FLOW_STRIP_WIDTH; col++)
                flow_draw_column(x + col, top, end, flow_strip + col,
                                 FLOW_STRIP_WIDTH, ink);
            flow_write_strip(frame + clear_y * BUFFER_WIDTH + x, strip, clear_y, clear_h);
        }
    }
    else
        for (int x = 0; x < LCD_WIDTH; x++)
            flow_draw_column(x, top, end, frame + x, BUFFER_WIDTH, ink);
    if (flow_stages)
    {
        flow_stages[UI_COVER_CLEAR] += clear_us;
        flow_stages[UI_COVER_BLIT] += (uint32_t)USEC_TIMER - start - clear_us;
    }
}

/* The rows the covers will disturb this frame: the ones they can reach now,
 * and the ones the last frame left behind and this clear has to wipe.
 * pf_slides_y0/y1 is left holding just the former, which is what the caption
 * has to keep clear of.
 *
 * Taken over every slide rather than the ones about to be drawn: which those
 * are depends on the alpha ramp, the answer is the same whenever the covers are
 * all one size, and over-reporting only costs a few rows of clear and flush. */
static void cover_rows(int *y0, int *y1)
{
    int lo, hi, a, b, i;

    if (flat_view())
    {
        slide_rows(&center_slide, &lo, &hi);

        for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
        {
            slide_rows(&left_slides[i], &a, &b);
            lo = MIN(lo, a);
            hi = MAX(hi, b);
            slide_rows(&right_slides[i], &a, &b);
            lo = MIN(lo, a);
            hi = MAX(hi, b);
        }
    }
    else
    {
        lo = pf_height;
        hi = 0;
        for (i = 0; i < flow_count; i++)
        {
            const struct flow_card *card = &flow_cards[i];
            if (MAX(card->edge[0].x, card->edge[1].x) <= 0 ||
                MIN(card->edge[0].x, card->edge[1].x) >= LCD_WIDTH * 65536)
                continue;
            int end[2];
            for (int e = 0; e < 2; e++)
            {
                const struct stock_edge *edge = &card->edge[e];
                end[e] = edge->bottom + (pf_stock ? (edge->bottom - edge->top) / 2 : 0);
            }
            lo = MIN(lo, MIN(card->edge[0].top, card->edge[1].top) >> 16);
            hi = MAX(hi, (MAX(end[0], end[1]) + 65535) >> 16);
        }
        lo = MAX(lo, flow_clip_top());
        hi = MIN(hi, flow_clip_end());
    }
    *y0 = MIN(lo, pf_slides_y0);
    *y1 = MAX(hi, pf_slides_y1);
    pf_slides_y0 = lo;
    pf_slides_y1 = hi;
}

/* Clears `h` rows from `y` down the viewport and draws the covers into them.
 * The caller works out the band -- it also has to flush it, and on the frames
 * that carry a caption as well the two are decided together. */
static void render_all_slides(int y, int h)
{
#if PF_SHOW_STATS
    unsigned long t0;
#endif
    lcd_set_background(pf_bg_color);
#if PF_SHOW_STATS
    t0 = USEC_TIMER;
#endif
    bool flat = flat_view();
    if (flat)
        pf_clear_band(y, h);
#if PF_SHOW_STATS
    pf_clear_us = USEC_TIMER - t0;
    pf_px = 0;
    t0 = USEC_TIMER;
#endif

    /* Trap: no yield anywhere in here. The covers are placed for one moment,
     * and a turn handed to the codec mid-frame shows them late by however long
     * it took, which differs from frame to frame and reads as judder. The
     * frame's one yield comes after its flush. */
    if (flat)
        flat_render(true);
    else
        flow_paint(true, 0, y, h);
#if PF_SHOW_STATS
    pf_slides_us = USEC_TIMER - t0;
#endif
}

/* Flat mid-flip. `s` is the direction (the scroll's step), and ftick how far
 * through the gap between two covers this frame has got: 0 at the start,
 * PFREAL_ONE at the end. The timing, the boundary snap that reassigns
 * slide_index and the loader wake-ups stay in update_scroll_animation(). */
static void flat_animate(int s, PFreal ftick)
{
    struct slide_data *arriving;
    PFreal off, in;

    /* Going past too quickly to be worth animating: a card would get two or
     * three frames to cross the screen and be sampled at arbitrary points along
     * the way. The 3D view blurs that into motion with its tilt and its fade;
     * flat covers have neither, so it strobes instead. Leave the piles at rest
     * and let the covers simply change in the middle, which is what you want
     * when hunting for one anyway.
     *
     * The scroll then stops dead rather than settling. Replaying the missed
     * move afterwards flickers: it has to bring back the cover a snapped
     * scroll already took out of the middle. */
    if (pf_flat_snap) {
        flat_idle();
        return;
    }

    /* Everything squared up, then move the two cards that are not. */
    flat_idle();

    off = MIN(ftick * PFREAL_ONE / PF_DEAL_SPAN, PFREAL_ONE);
    in = (ftick <= PF_DEAL_LEAD)
       ? 0 : MIN((ftick - PF_DEAL_LEAD) * PFREAL_ONE / PF_DEAL_SPAN, PFREAL_ONE);

    arriving = (s > 0) ? &right_slides[0] : &left_slides[0];

    center_slide.cx = -s * fmul(PF_PILE_CX, off);
    arriving->cx = s * fmul(PF_PILE_CX, PFREAL_ONE - in);

    /* Each takes the dimming and the height of where it is going, over the move
     * that takes it there: the one leaving sinks into its pile, the one
     * arriving comes up to full and rises to the middle as it lands. */
    {
        int pile = pile_alpha();
        int span = 256 - pile;
        PFreal drop = pile_offset();

        center_slide.alpha = 256 - ((span * off) >> PFREAL_SHIFT);
        arriving->alpha = pile + ((span * in) >> PFREAL_SHIFT);

        center_slide.cy = fmul(drop, smoothstep(off));
        arriving->cy = drop - fmul(drop, smoothstep(in));
    }

    /* They start out overlapped with the cover on top over the pile, and end
     * overlapped the other way round, so the swap goes in the middle where they
     * are barely touching. */
    pf_flat_handover = ftick >= PFREAL_HALF;
    pf_flat_moving = true;
}

/* Flat's flip speed, as a step per PF_SPEED_FRAMES authored frames. Flat
 * deals two covers per flip, one after the other, so it runs at a constant
 * rate set by the pace the user is scrolling at: see flat_rate(). */
static int scroll_speed(uint32_t sample_clock)
{
    return 65536 * PF_SPEED_FRAMES / flat_rate(sample_clock);
}

/* Authored frames since the flip's last sample. */
static unsigned scroll_time(uint32_t now)
{
    unsigned frame = ui_clock_frame(now - anim_clock);
    unsigned elapsed = frame - anim_sampled_frame;
    anim_sampled_frame = frame;
    return elapsed;
}

static int slide_at(int frame, int direction)
{
    return direction < 0 ? (frame + 0xffff) >> 16 : frame >> 16;
}

/* The middle slot changes hands: the slides take their new places and the
 * loader learns the new centre. */
static void move_center(int index)
{
    if (center_index == index)
        return;
    center_index = index;
    wake_pf_thread();
    if (pf_random_spinning)
        keyclick_click(true, BUTTON_SCROLL_FWD);
    center_slide.slide_index = center_index;
    for (int i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
        left_slides[i].slide_index = center_index - 1 - i;
    for (int i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
        right_slides[i].slide_index = center_index + 1 + i;
}

/* The covers at rest on the target. */
static void land(void)
{
    pf_state = pf_idle;
    pf_random_spinning = false;
    slide_frame = center_index << 16;
    step = 0;
}

static void flow_update(void)
{
    uint32_t now = USEC_TIMER;
    coverflow_place(now);
    move_center(flow_count ? flow_cards[flow_count - 1].id : target);
    if (!stock_animating(&flow_row, now))
    {
        move_center(target);
        if (flow_row.phase != STOCK_LEAVE)
            stock_init(&flow_row, number_of_slides, target, now, false);
        land();
    }
}

static void update_scroll_animation(void)
{
    if (step == 0)
        return;
    if (!flat_view())
    {
        flow_update();
        return;
    }

    unsigned left = scroll_time(USEC_TIMER);
    int target_frame = target << 16;
    /* Clamp distance as well as time so crossing the collection bounds is
     * impossible, even when the next frame is late. */
    int remain = (step < 0) ? slide_frame - target_frame
                            : target_frame - slide_frame;
    if (remain < 0)
        remain = 0;

    /* Recalculate the speed for each elapsed frame, so a frame that advances
     * several, at half rate or after a late wake, follows the same pace. */
    int adv = 0;
    unsigned sample = anim_sampled_frame - left;
    while (left > 0 && adv < remain)
    {
        uint32_t sample_clock = anim_clock + ui_frame_clock(++sample);
        int previous = slide_frame + adv * step;
        int num = scroll_speed(sample_clock) + anim_frac;
        int a = num / PF_SPEED_FRAMES;

        anim_frac = num - a * PF_SPEED_FRAMES;
        adv = MIN(adv + a, remain);
        if (slide_at(previous, step) != slide_at(slide_frame + adv * step, step))
            pf_flat_snap = flat_pace(sample_clock) < flat_duration(PF_FLAT_MIN);
        left--;
    }

    slide_frame += adv * step;

    /* Which slide is in the centre, and how far this flip has got towards
     * the next one. A boundary is shared by two gaps, so the answer depends
     * on the direction: moving right, C<<16 starts the gap above C; moving
     * left it ends the gap below C, and the centre is still C. Rounding
     * away from the direction of travel resolves both, and matters because
     * a frame can land exactly on a boundary.
     *
     * tick is progress through the gap, 0 at the start, measured along the
     * direction of travel, so the geometry below reads the same either way. */
    int offset = slide_frame - (center_index << 16);
    if (offset == 0 || (offset < 0) == (step < 0))
        anim_pose_step = step;
    int pose_step = anim_pose_step;

    move_center(slide_at(slide_frame, pose_step));

    /* Derived after the crossing, so nothing can describe the gap we just
     * left: carrying ftick across a crossing shows as a one-frame flash. */
    int tick = (pose_step < 0) ? ((-slide_frame) & 0xffff)
                          : (slide_frame & 0xffff);
    PFreal ftick = (tick * PFREAL_ONE) >> 16;

    if (slide_frame == target_frame) {
        flat_idle();
        land();
        return;
    }

    flat_animate(pose_step, ftick);
}

static void cleanup(void)
{
    /* Join before theme callbacks, provider changes or scratch release. */
    end_pf_thread();
    wants_to_quit = true;
    bar_drop_free();

    /* Give the theme back. Whatever comes next draws itself, so no forced
     * redraw from here. */
    if (pf_theme_pushed)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, false);
        pf_theme_pushed = false;
    }

    if (pf_boosted)
    {
        cpu_boost(false);
        pf_boosted = false;
    }
    /* Turn on backlight timeout (revert to settings) -- inlined equivalent
     * of apps/plugins/lib/helper.c's backlight_use_settings(), which isn't
     * linkable from core code. */
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);

    if (pf_title_font >= 0)
        font_unload(pf_title_font);
    if (pf_body_font >= 0)
        font_unload(pf_body_font);
    pf_title_font = pf_body_font = -1;

    /* The index borrows the shared app arena until the screen closes. */
    app_release_buffer("album covers");
}

enum {
    PF_SORT_ALBUMS_BY,
    PF_GOTO_LAST_ALBUM,
    PF_GOTO_WPS,
    PF_REBUILD_CACHE,
    PF_UPDATE_CACHE,
    PF_MENU_QUIT,
};

static void error_wait(const char *message)
{
    int button;

    splashf(0, "%s -- %s", message, str(LANG_PRESS_ANY_BUTTON));
    while ((button = get_action(CONTEXT_STD, 1)) == ACTION_NONE)
        yield();
    /* A USB connect has to reach the handler, or it is never acknowledged */
    if (default_event_handler(button) == SYS_USB_CONNECTED)
        return;
    sleep(2 * HZ);
}

static bool init(void)
{
    int ret = SUCCESS;
    void *buf;
    size_t buf_size;

    cpu_boost(true); /* revert in cleanup */
    pf_boosted = true;

    wants_to_quit = false;

    /* must appear before config load */
    pf_placeholder_buf = NULL;

    config_set_defaults(&pf_cfg); /* must appear before pf_config_load */
    pf_config_load();

    pf_stock = ui_theme_style(UI_STYLE_WPS_LAYOUT) == UI_WPS_LAYOUT_CLASSIC;
    pf_show_statusbar = pf_stock;
    pf_chrome_h = pf_stock ? ui_theme_metric(UI_METRIC_STATUS_HEIGHT) : 0;
    pf_theme_pushed = true;
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, pf_show_statusbar, NULL);
    viewport_set_defaults(&pf_vp, SCREEN_MAIN);

    pf_vp.buffer = &lcd_framebuffer_default;
    pf_vp.x = 0;
    pf_vp.y = 0;
    pf_vp.width = LCD_WIDTH;
    pf_vp.height = LCD_HEIGHT;
    pf_vp_y = 0;
    pf_height = LCD_HEIGHT;

    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    pf_title_font = ui_theme_font_load(&theme, UI_FONT_TITLE, 64);
    for (int y = 0; pf_stock && y < ALBUM_BLUR_ROW_PITCH; y++)
    {
        uint32_t rgb = ui_theme_gradient_at(
            &theme.gradient[UI_GRADIENT_SELECTION], y, ALBUM_BLUR_ROW_PITCH,
            theme.color[UI_COLOR_SELECTION]);
        panel_bar[y] = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff);
    }
    panel_cache_follow_theme();
    pf_body_font = ui_theme_font_load(&theme, UI_FONT_BODY, 64);

    /* Flat piles fit in the drawable band. The 3D view uses the panel camera
     * and clips to the caption's actual line boxes. */
    {
        bool text_at_top;
        int draw_height;

        switch (global_settings.album_covers_show_album_name)
        {
            case ALBUM_NAME_HIDE:
                pf_caption_band = 0;
                text_at_top = false;
                break;
            case ALBUM_NAME_TOP:
            case ALBUM_AND_ARTIST_TOP:
                pf_caption_band = PF_CAPTION_BAND;
                text_at_top = true;
                break;
            case ALBUM_NAME_BOTTOM:
            case ALBUM_AND_ARTIST_BOTTOM:
            default:
                pf_caption_band = PF_CAPTION_BAND;
                text_at_top = false;
                break;
        }

        draw_height = pf_height - pf_caption_band;
        if (draw_height < 0)
            draw_height = 0;

        pf_half_height = draw_height / 2;
        pf_lower_half = draw_height - pf_half_height;
        pf_draw_y_shift = text_at_top ? pf_caption_band : 0;

        update_caption_bounds();

        /* Nothing has been drawn yet, so claim the lot: the first frame is a
         * full one regardless, and this must never read as "the covers are
         * clear of the caption" before a render has said so. */
        pf_slides_y0 = 0;
        pf_slides_y1 = pf_height;
    }

    pf_update_dynamic_colors();

    /* The shared app buffer (app_buffer.h), a static region apart from the
     * audio buffer pool, held for as long as this screen is up and handed
     * back by app_release_buffer() in cleanup(). Trap: not
     * core_alloc_maximum(), whose block comes out of the audio pool and so
     * stops playback -- and with it the dynamic colours, which need a
     * current track. */
    buf = app_claim_buffer(&buf_size, "album covers");
    /* restore checks the previous base/size before recording this claim */
    bool restored = carousel_restore_resume(buf, buf_size);
    owned_buffer = buf; owned_bytes = buf_size;
    if (restored)
    {
        lcd_setfont(carousel_caption_font(false));
        init_scroll_lines();
        buffer = LCD_BUF + (pf_vp_y + pf_draw_y_shift) * BUFFER_WIDTH;
        pf_state = pf_idle; step = 0; target = center_index;
        reset_slides();
        lcd_set_drawmode(DRMODE_FG);
        return true;
    }

    /* No failure to check for: the region is a static array, so the call either
     * returns it or panics because someone else is holding it. */

    /* store buffer pointers and sizes */
    carousel_idx.buf = buf;
    carousel_idx.buf_sz = buf_size;

    lcd_setfont(carousel_caption_font(false));

    if (!dir_exists(CACHE_PREFIX))
    {
        if (mkdir(CACHE_PREFIX) < 0)
        {
            char msg[MAX_PATH];

            snprintf(msg, sizeof(msg), "%s " CACHE_PREFIX,
                     str(LANG_CAROUSEL_NO_DIR));
            error_wait(msg);
            return false;
        }
    }

    mutex_init(&buf_ctx_mutex);

    init_scroll_lines();

    ret = model->build_index();

    /* The index and the slides share the app buffer, which a big enough
     * library fills: from about 12,000 albums on a 5G. */
    if (ret == ERROR_BUFFER_FULL)
    {
        error_wait(str(LANG_CAROUSEL_TOO_LARGE));
        return false;
    }
    else if (ret == ERROR_NO_ALBUMS)
    {
        error_wait(str(LANG_CAROUSEL_NO_ALBUMS));
        return false;
    }
    else if (ret == ERROR_NO_ARTISTS)
    {
        error_wait(str(LANG_CAROUSEL_NO_ARTISTS));
        return false;
    }
    else if (ret == ERROR_USER_ABORT)
        return false;

    number_of_slides = model->count();
    /* slide_frame holds a slide index << 16 in an int, so slides past 32767
     * would overflow it; those are not offered. */
    if (number_of_slides > 0x7fff)
        number_of_slides = 0x7fff;

    /* Reserve the scratch the placeholder is drawn into. Sized by its only
     * user: create_empty_slide() fills DISPLAY_WIDTH squared and save_pfraw()
     * writes that back out. Cover art never passes through here -- the
     * thumbnails belong to the shared cache (metadata/art_cache.c) and
     * read_aat() reads them straight into buflib. */
    size_t placeholder_sz = ALIGN_UP((size_t)DISPLAY_WIDTH * DISPLAY_WIDTH
                                   * sizeof(pix_t), sizeof(long));
    /* The largest slide read_aat() will accept, and so what one costs the
     * cache below. */
    size_t slide_max = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(pix_t)
                     + sizeof(int) + PF_READ_SLACK + sizeof(struct art_cache_header);

    ALIGN_BUFFER(carousel_idx.buf, carousel_idx.buf_sz, sizeof(long));

    /* Against what the index actually left, not against itself. buf_sz is a
     * size_t, so a reservation larger than the space remaining wraps it to
     * about 4GB and buflib_init() below puts its handle table past the end of
     * memory -- which the first slide allocation then writes to. The cache
     * also has to keep enough for the placeholder and one cover, or there is
     * nothing for the screen to show. */
    if (carousel_idx.buf_sz < placeholder_sz + 2 * slide_max)
    {
        error_wait(str(LANG_CAROUSEL_TOO_LARGE));
        return false;
    }

    pf_placeholder_buf = carousel_idx.buf;

    carousel_idx.buf += placeholder_sz;
    carousel_idx.buf_sz -= placeholder_sz;

    pf_cover_size_idx = art_cache_size_index("coverflow");

    buflib_init(&buf_ctx, (void *)carousel_idx.buf, carousel_idx.buf_sz);
    initialize_slide_cache();

    if (!create_empty_slide(model->owns_cache_version &&
                            pf_cfg.cache_version != CACHE_VERSION))
    {
        if (model->owns_cache_version)
        {
            pf_cfg.cache_version = CACHE_REBUILD;
            pf_config_save();
        }
        error_wait(str(LANG_CAROUSEL_NO_PLACEHOLDER));
        return false;
    }

    /* Model-specific one-off setup after the slide cache is up (album covers
     * generates its pfraw fallback thumbnails here; artist portraits has none). */
    if (model->prepare)
        model->prepare();

    if ((empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0)) < 0)
    {
        error_wait(str(LANG_CAROUSEL_NO_PLACEHOLDER));
        return false;
    }

    buffer = LCD_BUF;
    buffer += (pf_vp_y + pf_draw_y_shift) * BUFFER_WIDTH; /* offset below
        status bar, plus a TOP text caption's reserved margin, if any (see
        pf_draw_y_shift's assignment above) */

    pf_state = pf_idle;

    slide_frame = 0;
    step = 0;
    target = 0;

    reset_slides();

    lcd_set_drawmode(DRMODE_FG);

    return true;
}

/* carousel_reinit: full teardown + rebuild of the engine. */
bool carousel_reinit(void)
{
    pf_random_count = 0;
    int index = center_index;
    cleanup();
    if (!init())
        return false;
    set_current_slide(index);
    create_pf_thread();
    return true;
}

#if PF_SHOW_STATS
/* One second's worth of frames, reduced to a line drawn over the covers.
 *
 * The line reads "<n>fps r<us> w<ms> f<us> b<pct>% l<n>": the frame rate
 * actually achieved, the average microseconds inside the render, the longest
 * gap from one frame's start to the next -- a stutter the fps hides -- the
 * average microseconds inside the flush,
 * the share of wall clock those two accounted for, and how many slides the
 * loader fetched. Between them those separate the three candidates, which is
 * the whole point of it:
 *
 *   fps at the cap                -> this loop is keeping up; look elsewhere
 *   fps low, busy high            -> saturated by its own drawing; transfer or
 *                                    draw less, and f vs r says which
 *   fps low, busy low             -> starved, not busy. The time is going to
 *                                    other threads, and l says whether the
 *                                    slide loader is the one taking it.
 *
 * Read the averages knowing that not every frame draws covers: a caption-only
 * frame counts here, contributes nothing to clr or sld, and pulls both down.
 * Scroll, where every frame is a full one, is the reading to compare against.
 */
static struct
{
    unsigned long render;       /* usec accumulated over the window */
    unsigned long worst_gap;    /* longest usec from one frame's start to the
                                 * next's, in the window */
    unsigned long last_start;
    unsigned long flush;
    int frames;
    unsigned long clear;
    unsigned long slides;
    unsigned long px;
    unsigned int loads;         /* pf_loads_total when the window opened */
    long window_start;
    char line[48];
    char line2[48];
} pf_stats;

static void pf_stats_frame(unsigned long start_us, unsigned long render_us,
                           unsigned long flush_us)
{
    long now = current_tick;
    long elapsed;
    unsigned long gap = start_us - pf_stats.last_start;

    pf_stats.last_start = start_us;
    if (gap > pf_stats.worst_gap)
        pf_stats.worst_gap = gap;
    pf_stats.render += render_us;
    pf_stats.flush  += flush_us;
    pf_stats.clear  += pf_clear_us;
    pf_stats.slides += pf_slides_us;
    pf_stats.px     += pf_px;
    pf_stats.frames++;

    elapsed = now - pf_stats.window_start;
    if (elapsed < HZ || pf_stats.frames < 1)
        return;

    /* Wall clock the window covered, in the same units as the totals. `busy` is
     * this screen's own share of it -- the yielded time belongs to whichever
     * thread ran, not to us. */
    unsigned long span = (unsigned long)elapsed * (1000000 / HZ);
    unsigned long busy = pf_stats.render + pf_stats.flush;

    snprintf(pf_stats.line, sizeof(pf_stats.line),
             "%ldfps r%lu w%lu f%lu b%lu%% l%u%s",
             (long)pf_stats.frames * HZ / elapsed,
             pf_stats.render / pf_stats.frames,
             pf_stats.worst_gap / 1000,
             pf_stats.flush / pf_stats.frames,
             span ? busy * 100 / span : 0,
             pf_loads_total - pf_stats.loads,
             art_cache_is_busy() ? " ART" : "");

    /* nspx is the whole point: nanoseconds of slide-loop time per pixel those
     * loops wrote. Target-independent, so it means the same on both. */
    /* The clock the above was measured at, because ns/px means opposite things
     * at 30 MHz and 80 MHz -- healthy arithmetic in one reading, stalled memory
     * in the other -- and cpu_boost() being *called* is not evidence that it is
     * in effect. */
    snprintf(pf_stats.line2, sizeof(pf_stats.line2),
             "clr%lu sld%lu ns%lu %ldM b%d",
             pf_stats.clear / pf_stats.frames,
             pf_stats.slides / pf_stats.frames,
             pf_stats.px ? pf_stats.slides * 1000 / pf_stats.px : 0,
             FREQ / 1000000, get_cpu_boost_counter());

    pf_stats.render = 0;
    pf_stats.worst_gap = 0;
    pf_stats.flush = 0;
    pf_stats.clear = 0;
    pf_stats.slides = 0;
    pf_stats.px = 0;
    pf_stats.frames = 0;
    pf_stats.loads = pf_loads_total;
    pf_stats.window_start = now;
}

static void pf_stats_draw(void)
{
    if (!pf_stats.line[0])
        return;

    /* In the caption's place, below the covers, where it can be read. */
    int h = font_get(FONT_SYSFIXED)->height;
    int y = (pf_text_h >= 2 * h) ? pf_text_y + (pf_text_h - 2 * h) / 2
                                 : pf_height - 2 * h;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_foreground(pf_fg_color);
    lcd_putsxy(2, y, pf_stats.line);
    lcd_putsxy(2, y + h, pf_stats.line2);
    lcd_setfont(carousel_caption_font(false));
}
#endif /* PF_SHOW_STATS */

/* A frame deadline is never further ahead than this. One that seems to be
 * has passed long enough ago for the timer to wrap. */
#define PF_DEADLINE_AHEAD_US 100000

static bool frame_due(uint32_t deadline)
{
    uint32_t ahead = deadline - (uint32_t)USEC_TIMER;
    return ahead == 0 || ahead > PF_DEADLINE_AHEAD_US;
}

/* The input wait to a frame deadline. Without a precise wake the wait rounds
 * down to whole ticks, and the last part is slept rather than spun through. */
static int frame_wait(uint32_t deadline)
{
    if (frame_due(deadline))
        return TIMEOUT_NOBLOCK;
    int timeout = ui_clock_wait_until(deadline);
    return timeout == TIMEOUT_NOBLOCK && !frame_due(deadline) ? 1 : timeout;
}

#define ALBUM_PANEL_WIDTH ALBUM_BLUR_PANEL_W
#define ALBUM_PANEL_HEIGHT ALBUM_BLUR_PANEL_H
#define ALBUM_PANEL_X ((LCD_WIDTH - ALBUM_PANEL_WIDTH) / 2)
#define ALBUM_PANEL_Y (LCD_HEIGHT - ALBUM_PANEL_HEIGHT)
#define FLIP_MID PFREAL_ONE
#define FLIP_OPEN (2 * FLIP_MID)

#define PANEL_DIVIDER ALBUM_BLUR_DIVIDER
#define PANEL_ROWS_Y ALBUM_BLUR_ROWS_TOP
#define PANEL_PITCH ALBUM_BLUR_ROW_PITCH
#define PANEL_LEFT ALBUM_BLUR_TEXT_LEFT
#define PANEL_HEADER_RIGHT ALBUM_BLUR_HEADER_RIGHT
#define PANEL_ROWS_RIGHT ALBUM_BLUR_ROWS_RIGHT
/* Left of the text, as in the theme's lists: the selection marker, then the
 * playing or marked sign. The scroll bar sits in the right inset, clear of
 * the durations. */
#define PANEL_MARK_X 3
#define PANEL_SIGN_X 8
#define PANEL_BAR_X (ALBUM_PANEL_WIDTH - 6)
#define PANEL_TIME_GAP 8
#define ALBUM_PANEL_ROWS ALBUM_BLUR_ROWS
#define PANEL_CACHE (2 * ALBUM_PANEL_ROWS + 1)

struct panel_track {
    int index; /* One-based, so a zeroed slot is unused. */
    unsigned int duration;
    bool playing, glyphs_ready;
    char title[CAROUSEL_TRACK_TEXT];
    char fitted[CAROUSEL_TRACK_TEXT], time[16];
    int time_x;
};

enum panel_command {
    PANEL_ADD, PANEL_NEXT, PANEL_LAST, PANEL_SELECT_MANY, PANEL_CATALOG,
    PANEL_EXISTING, PANEL_NEW,
};

static const int panel_command_labels[] = {
    LANG_ADD, LANG_PLAY_NEXT, LANG_PLAY_LAST, LANG_SELECT_MANY, LANG_ADD_TO_PL,
    LANG_ADD_TO_EXISTING_PL, LANG_CATALOG_ADD_TO_NEW,
};

static struct album_panel {
    struct carousel_album_header header;
    struct panel_request request;
    const struct panel_cached *cached, *adjacent;
    int cached_first, adjacent_first;
    struct panel_track rows[PANEL_CACHE];
    struct album_blur_source blur_source;
    unsigned blur_generation;
    int selected, top, count, disc_count;
    enum panel_command commands[ALBUM_PANEL_ROWS];
    int command_count, command_selected, marked;
    bool catalog_menu, menu_ready, selecting;
    unsigned char marks[LIST_SELECT_MAX_ROWS / 8];
    int album_font, title_font, small_font;
    int scroll_y, scroll_from;
    uint32_t scroll_clock;
    unsigned scroll_frames;
    int from, to, via, position;
    struct stock_flip flip;
    int back_handle;
    bool back_ready;
    /* The stock look's flip has yet to draw its first frame. */
    bool flip_first;
    unsigned first, second, sampled, trace;
    /* due is the next flip or list scroll frame's deadline. */
    uint32_t clock, opened, due;
    unsigned painted_rows, painted_discs;
    bool open_logged;
    long next_read;
    bool live, opening, moving, dirty, missing, play_pending, play_failed, have_blur;
    char fitted_title[CAROUSEL_TRACK_TEXT], fitted_artist[CAROUSEL_TRACK_TEXT];
    char count_label[80];
    int dot_x, count_x;
    int digit_width;
    bool tabular, header_ready, digits_ready, resuming;
    /* The theme's list background is solid rather than the blurred cover. */
    bool solid;
    /* The selected title, scrolled by `marquee` pixels when it is too long for
     * its row. Zero `marquee_step` means it fits or is waiting. */
    int marquee_track, marquee, marquee_step, marquee_width, marquee_room;
    long marquee_start;
} panel;

/* The panel's colours, all from the theme. The rows take a list's colours
 * and its selection marker; `dim` is the durations' quieter `muted`. */
struct panel_ink {
    pix_t back, band, title, text, dim, edge, accent;
    pix_t selection, selected_text, selected_muted;
    int mark_width, mark_alpha;
    unsigned char mark_mask[2];
};

static void panel_ink(struct panel_ink *ink)
{
    ink->back = ui_theme_rgb565(UI_COLOR_BACKGROUND);
    ink->band = ui_theme_rgb565(UI_COLOR_LIST_TINT);
    ink->title = ui_theme_rgb565(UI_COLOR_FOREGROUND);
    /* A band the same colour as the rows would not read as one. */
    if (ink->band == ink->back)
        ink->band = color_blend(ink->back, ink->title, 32);
    ink->text = ui_theme_rgb565(UI_COLOR_MUTED);
    ink->dim = color_blend(ink->text, ink->back, 64);
    ink->edge = ui_theme_rgb565(UI_COLOR_SEPARATOR);
    ink->accent = ui_theme_rgb565(UI_COLOR_ACCENT);
    ink->selection = ui_theme_rgb565(UI_COLOR_SELECTION);
    ink->selected_text = ui_theme_rgb565(UI_COLOR_SELECTED_TEXT);
    ink->selected_muted = ui_theme_rgb565(UI_COLOR_SELECTED_SECONDARY);
    ink->mark_width = ui_theme_metric(UI_METRIC_LIST_SELECTION_WIDTH);
    ink->mark_alpha = ui_theme_metric(UI_METRIC_LIST_SELECTION_ALPHA);
    round_rect_build_mask(ink->mark_mask, 1, ink->mark_alpha);
    if (stock_look())
    {
        /* Stock's back: white text on its blue header, rows in the list's
         * own colour, edged in the header tile's last row. */
        ink->text = ink->title;
        ink->title = LCD_RGBPACK(255, 255, 255);
        ink->edge = LCD_RGBPACK(126, 129, 131);
    }
}

/* The marker beside the selected row at `y`. */
static void panel_mark(int y, const struct panel_ink *ink)
{
    if (!ink->mark_width)
        return;
    lcd_set_foreground(ink->selection);
    fill_round_rect_aa(&screens[SCREEN_MAIN], PANEL_MARK_X, y + 3,
                       ink->mark_width, PANEL_PITCH - 6,
                       MIN(ink->mark_width / 2, 1), ink->mark_mask,
                       ink->mark_alpha);
}

static void panel_layout_header(void);
static void panel_layout_digits(void);
static void prepare_stock_back(void);
static void panel_layout_track(struct panel_track *row);

static struct album_blur_source panel_blur_source(int index)
{
    struct album_blur_source source = {0};
    if (model->blur_source)
        model->blur_source(index, &source);
    else
        source.local = model->art_key(index);
    return source;
}

static void panel_drop_pages(void)
{
    panel_cache_bind(NULL, NULL);
    panel_cache_release(panel.cached);
    panel_cache_release(panel.adjacent);
    panel.cached = panel.adjacent = NULL;
    panel.cached_first = panel.adjacent_first = -1;
}

static void close_album_panel(void)
{
    if (!panel.live)
        return;
    ui_perf_end(panel.trace);
    if (panel.back_handle > 0)
        core_free(panel.back_handle);
    panel_drop_pages();
    panel_cache_hold_fonts(false);
    memset(&panel, 0, sizeof(panel));
}

static bool request_panel(int index, struct panel_request *request)
{
    memset(request, 0, sizeof(*request));
    request->locale = fnv1a_str(global_settings.lang_file);
    return model->panel_request(index, request);
}

static void open_album_panel(int selected, int top)
{
    if (!model->panel_request || !request_panel(center_index, &panel.request))
        return;
    if (!flat_view())
        panel.back_handle = core_alloc_free_space(256 * 256 * sizeof(pix_t));
    panel.opened = USEC_TIMER;
    caption_artist.valid = false;
    panel_cache_hold_fonts(true);
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    panel.solid = theme.background[UI_BG_LIST] == UI_BG_SOLID;
    panel_perf_record(PP_SELECT, 0, center_index, model->external);
    panel.live = true;
    panel.opening = true;
    panel.count = -1;
    panel.cached_first = panel.adjacent_first = -1;
    panel.album_font = panel.title_font = panel.small_font = -1;
    panel.blur_source = panel_blur_source(center_index);
    strlcpy(panel.header.title, panel.request.title, sizeof(panel.header.title));
    strlcpy(panel.header.artist, panel.request.artist, sizeof(panel.header.artist));
    panel.selected = selected;
    panel.top = top;
    panel.scroll_y = top * PANEL_PITCH;
    panel.dirty = panel.missing = true;
    const struct panel_cached *hit = panel_cache_acquire(&panel.request, 0);
    if (hit)
    {
        panel_perf_record(PP_RAM, USEC_TIMER - panel.opened, center_index, model->external);
        panel_cache_release(hit);
    }
}

static int flip_position(unsigned frame)
{
    if (frame < panel.first)
        return panel.from + (panel.via - panel.from) * (int)frame / (int)panel.first;
    frame -= panel.first;
    if (frame < panel.second)
        return panel.via + (panel.to - panel.via) * (int)frame / (int)panel.second;
    return panel.to;
}

static void flip_to(bool open)
{
    if (!flat_view())
    {
        if (panel.moving && panel.to == (open ? FLIP_OPEN : 0))
            return;
        /* The stock look draws the opening back before the turn starts,
         * not on the frame where it first shows. */
        bool prepared = stock_look() && open && !panel.moving &&
                        panel.back_handle > 0 && pf_motion_scale;
        if (prepared)
        {
            buf_ctx_lock();
            prepare_stock_back();
            buf_ctx_unlock();
        }
        uint32_t now = USEC_TIMER;
        struct stock_pose current = panel.moving ? stock_flip_pose(&panel.flip, now) :
            stock_current(&flow_row, center_index, now, stock_scroll_progress(&flow_row, now)).pose;
        ui_perf_end(panel.trace);
        stock_flip_start(&panel.flip, open ? STOCK_TO_BACK : STOCK_TO_FRONT, current, now);
        panel.clock = panel.due = now;
        panel.to = open ? FLIP_OPEN : 0;
        panel.moving = pf_motion_scale && panel.back_handle > 0;
        panel.position = panel.moving ? FLIP_MID : panel.to;
        panel.back_ready = prepared;
        panel.flip_first = stock_look() && panel.moving;
        panel.dirty = true;
        panel.trace = panel.moving ? ui_perf_begin(open ? "cover-flip-open" : "cover-flip-close") : 0;
        return;
    }
    if (panel.moving)
    {
        panel.position = flip_position(ui_clock_frame((uint32_t)USEC_TIMER - panel.clock));
        ui_perf_end(panel.trace);
    }
    panel.from = panel.position;
    panel.to = open ? FLIP_OPEN : 0;
    panel.via = MIN(MAX(FLIP_MID, MIN(panel.from, panel.to)), MAX(panel.from, panel.to));
    /* A reversal starts at the current pose and takes the remaining part of
     * each half. The closing path undoes the opening curves. */
    unsigned first = ui_theme_duration(open ? ANIM_F(9) : ANIM_F(8));
    unsigned second = ui_theme_duration(open ? ANIM_F(9) : ANIM_F(7));
    panel.first = (first * abs(panel.via - panel.from) + FLIP_MID - 1) / FLIP_MID;
    panel.second = (second * abs(panel.to - panel.via) + FLIP_MID - 1) / FLIP_MID;
    panel.sampled = (unsigned)-1;
    panel.clock = panel.due = ui_clock_start();
    panel.moving = panel.first || panel.second;
    panel.dirty = true;
    if (!panel.moving)
        panel.position = panel.to;
    panel.trace = panel.moving ? ui_perf_begin(open ? "cover-flip-open" : "cover-flip-close") : 0;
}

static const struct panel_cached *panel_disc_data(void)
{
    return panel.cached ? panel.cached : panel.adjacent;
}

static int panel_disc_track(int n)
{
    return panel_cache_disc(panel_disc_data(), n);
}

static void panel_follow_selection(bool animate)
{
    int row = panel.selected;
    for (int i = 0; i < panel.disc_count && panel_disc_track(i) <= panel.selected; i++)
        row++;

    int top = MIN(MAX(panel.top, row - (ALBUM_PANEL_ROWS - 2)), row - 1);
    top = MAX(0, MIN(top, panel.count + panel.disc_count - ALBUM_PANEL_ROWS));
    int first = MAX(0, (row - ALBUM_PANEL_ROWS + 1) * PANEL_PITCH);
    int from = MIN(MAX(first, panel.scroll_y), row * PANEL_PITCH);
    if (animate && top == panel.top && from == panel.scroll_y)
        return;

    panel.top = top;
    panel.scroll_frames = animate && panel.position == FLIP_OPEN
                        ? ui_theme_duration(ANIM_F(8)) : 0;
    if (panel.scroll_frames)
    {
        /* Keep up with fast input, then use native list cubic ease-out. */
        panel.scroll_y = panel.scroll_from = from;
        panel.scroll_clock = panel.due = ui_clock_start();
    }
    else
        panel.scroll_y = top * PANEL_PITCH;
}

static int panel_first_track(int first_row, int *disc)
{
    int i = 0;
    while (i < panel.disc_count && panel_disc_track(i) + i < first_row)
        i++;
    *disc = i;
    return first_row - i;
}

static bool panel_chinese(void);

static void panel_disc_label(int disc, char *label, size_t size)
{
    const char *name = panel_chinese() ? "光盘" : (const char *)str(LANG_DISC);
    snprintf(label, size, "%s %u", name, panel_cache_disc_number(panel_disc_data(), disc));
}

static const struct panel_cache_row *panel_source_row(int track)
{
    const struct panel_cache_row *source = panel_cache_row(panel.cached, track);
    return source ? source : panel_cache_row(panel.adjacent, track);
}

static void panel_read_rows(void)
{
    if (panel.count <= 0)
        return;
    uint64_t playing_key = !model->external && audio_status() && !cider_active()
                         ? panel_cache_playing_key() : 0;
#ifdef HAVE_USB_PODLINK
    char remote_id[sizeof(((struct cider_state *)0)->id)] = "";
    if (model->external && cider_active())
        cider_current_id(remote_id);
#endif
    int first = panel.scroll_y / PANEL_PITCH;
    int rows = 2 * ALBUM_PANEL_ROWS + (panel.scroll_y % PANEL_PITCH != 0);
    int small = panel.small_font >= 0 ? panel.small_font : FONT_SYSFIXED;
    int disc;
    int track = panel_first_track(first, &disc);
    panel.missing = false;
    for (int i = 0; i < rows && track < panel.count; i++)
    {
        if (disc < panel.disc_count && panel_disc_track(disc) == track)
        {
            char label[80];
            panel_disc_label(disc, label, sizeof(label));
            font_prefetch_text(small, label);
            disc++;
            continue;
        }

        const struct panel_cache_row *source = panel_source_row(track);
        struct panel_track *row = &panel.rows[track % PANEL_CACHE];
        if (!source)
            panel.missing = true;
        else
        {
            if (row->index != track + 1 || (panel.request.remote &&
                (row->duration != source->duration || strcmp(row->title, source->title))))
            {
                memset(row, 0, sizeof(*row));
                row->index = track + 1;
                row->duration = source->duration;
                strlcpy(row->title, source->title, sizeof(row->title));
                panel.dirty = true;
            }
            bool playing = playing_key && source->playing_key == playing_key;
#ifdef HAVE_USB_PODLINK
            playing |= *remote_id && (!strcmp(source->remote_id, remote_id) ||
                                      cider_route_matches(source->route, remote_id));
#endif
            if (playing != row->playing)
                panel.dirty = true;
            row->playing = playing;
            if (!row->glyphs_ready || panel.opening)
            {
                panel_layout_track(row);
                panel.dirty |= row->glyphs_ready;
            }
        }
        track++;
    }
}

/* Request an adjacent shard before either page intersects its boundary. */
static int panel_page_first(void)
{
    int disc;
    int track = panel_first_track(panel.scroll_y / PANEL_PITCH, &disc);
    return track / PANEL_PAGE_ROWS * PANEL_PAGE_ROWS;
}

static int panel_page_adjacent(int first)
{
    if (first < 0)
        return -1;
    int disc;
    int track = panel_first_track(panel.scroll_y / PANEL_PITCH, &disc);
    int next = first + PANEL_PAGE_ROWS;
    if (next < panel.count && next - MAX(track, panel.selected) <= PANEL_CACHE + 1)
        return next;
    return first > 0 && track - first < ALBUM_PANEL_ROWS ? first - PANEL_PAGE_ROWS : -1;
}

static bool panel_acquire_page(const struct panel_cached **data, int *slot_first, int first)
{
    if (*data && *slot_first == first && !panel.request.remote)
        return false;
    const struct panel_cached *next = first < 0 ? NULL : panel_cache_acquire(&panel.request, first);
    if (next == *data || (!next && first >= 0))
    {
        panel_cache_release(next);
        return false;
    }
    /* Neither old pack is used until the final pair has been bound. */
    panel_cache_bind(NULL, NULL);
    panel_cache_release(*data);
    *data = next;
    *slot_first = first;
    return true;
}

static void panel_read_tracks(void)
{
    int first = panel_page_first(), nearby = panel_page_adjacent(first);
    if (panel.adjacent && panel.adjacent_first == first)
    {
        const struct panel_cached *old = panel.cached;
        int old_first = panel.cached_first;
        panel.cached = panel.adjacent;
        panel.cached_first = first;
        panel.adjacent = old;
        panel.adjacent_first = old_first;
    }
    int previous_count = panel.count;
    bool changed = panel_acquire_page(&panel.cached, &panel.cached_first, first);
    changed |= panel_acquire_page(&panel.adjacent, &panel.adjacent_first, nearby);
    panel_cache_bind(panel.cached, panel.adjacent);
    if (!panel.digits_ready && panel.small_font >= 0)
        panel_layout_digits();

    if (changed && panel.cached)
    {
        bool update_header = panel.count != panel_cache_total(panel.cached) ||
            strcmp(panel.header.artist, panel_cache_artist(panel.cached));
        panel.count = panel_cache_total(panel.cached);
        panel.disc_count = panel_cache_discs(panel.cached);
        strlcpy(panel.header.artist, panel_cache_artist(panel.cached), sizeof(panel.header.artist));
        if (update_header)
            panel.header_ready = false;
        if (previous_count != panel.count)
        {
            panel.selected = MAX(0, MIN(panel.count - 1, panel.selected));
            panel_follow_selection(false);
        }
        panel.dirty = true;
    }
    if ((!panel.header_ready || panel.opening) &&
        panel.album_font >= 0 && panel.title_font >= 0)
    {
        panel_layout_header();
        panel.dirty |= panel.header_ready;
    }
    panel.missing = !panel.cached;
    if (panel.title_font >= 0 && panel.small_font >= 0)
        panel_read_rows();
    panel.next_read = current_tick + MAX(1, HZ / 25);
}

/* The first back uses the same leased glyph packs as the live panel. */
static bool panel_opening_ready(void)
{
    if (!panel.cached || !panel.header_ready)
        return false;
    int disc;
    int track = panel_first_track(panel.scroll_y / PANEL_PITCH, &disc);
    for (int i = 0; i < ALBUM_PANEL_ROWS && track < panel.count; i++)
    {
        if (disc < panel.disc_count && panel_disc_track(disc) == track)
        {
            char label[80];
            panel_disc_label(disc++, label, sizeof(label));
            if (!font_prefetch_text(panel.small_font, label))
                return false;
            continue;
        }
        const struct panel_track *row = &panel.rows[track % PANEL_CACHE];
        if (row->index != track + 1 || !row->glyphs_ready)
            return false;
        track++;
    }
    return true;
}

/* Only snapshot RAM provider state here. Track lookups, disk reads and font
 * prep are done by bgtask, with the centre at the head of its window. */
static void panel_request_window(void)
{
    static struct panel_request window[PANEL_WINDOW];
    unsigned count = 0;
    if (panel.live)
    {
        struct panel_request *active = &window[0];
        if (request_panel(center_index, active) && !panel_cache_same(active, &panel.request))
        {
            if (active->key != panel.request.key)
            {
                close_album_panel();
                panel_cache_window(NULL, 0);
                return;
            }
            panel_drop_pages();
            panel.command_count = panel.marked = 0;
            panel.selecting = false;
            panel.request = *active;
            panel.count = -1;
            panel.disc_count = 0;
            panel.header_ready = false;
            panel.missing = panel.dirty = true;
            memset(panel.rows, 0, sizeof(panel.rows));
            strlcpy(panel.header.title, active->title, sizeof(panel.header.title));
            strlcpy(panel.header.artist, active->artist, sizeof(panel.header.artist));
        }
        *active = panel.request;
        active->first = panel_page_first();
        count = 1;
        int adjacent = panel_page_adjacent(active->first);
        if (adjacent >= 0)
        {
            window[count] = *active;
            window[count++].first = adjacent;
        }
    }
    else if (request_panel(center_index, &window[0]))
        count = 1;

    /* Keep neighbouring albums warm when the active panel is on its first
     * page. Additional pages of the active album take the next slot first. */
    if (count && !window[0].remote && window[0].first == 0)
    {
        if (center_index > 0 && count < PANEL_WINDOW &&
            request_panel(center_index - 1, &window[count]))
            count++;
        if (center_index + 1 < number_of_slides && count < PANEL_WINDOW &&
            request_panel(center_index + 1, &window[count]))
            count++;
    }
    panel_cache_window(window, count);
}


static void panel_step_scroll(void)
{
    uint32_t elapsed = (uint32_t)USEC_TIMER - panel.scroll_clock;
    unsigned frame = ui_clock_frame(elapsed);
    const struct ui_tween glide = {
        .from = panel.scroll_from, .to = panel.top * PANEL_PITCH,
        .frames = panel.scroll_frames,
    };
    int y = ui_tween_sample(&glide, frame);
    if (frame >= panel.scroll_frames)
        panel.scroll_frames = 0;
    if (y != panel.scroll_y)
    {
        int previous = panel.scroll_y;
        panel.scroll_y = y;
        if (previous / PANEL_PITCH != y / PANEL_PITCH ||
            previous % PANEL_PITCH == 0)
            panel_read_rows();
        panel.dirty = true;
    }
}

static void panel_layout_digits(void)
{
    int body = panel.title_font >= 0 ? panel.title_font : FONT_SYSFIXED;
    if (!font_prefetch_text(body, "0123456789:"))
        return;
    struct font *font = font_get(body);
    panel.digit_width = font_get_width(font, '0');
    panel.tabular = true;
    for (int ch = '1'; ch <= '9'; ch++)
    {
        int width = font_get_width(font, ch);
        panel.tabular &= width == panel.digit_width;
        panel.digit_width = MAX(panel.digit_width, width);
    }
    panel.digits_ready = true;
}

static bool panel_text(const char *label, int x, int baseline, int right)
{
    char fitted[CAROUSEL_TRACK_TEXT];
    int font = lcd_getfont();
    bool ready = font_prefetch_text(font, label);
    ready &= font_prefetch_text(font, ".");
    if (!ready)
        return false;
    const char *text = text_fit(label, fitted, sizeof(fitted), font, right - x);
    lcd_putsxy(x, baseline - font_get(font)->ascent, text);
    return true;
}

static bool panel_chinese(void)
{
    return !strcmp(global_settings.lang_file, "chinese-simp");
}

static const char panel_middot[] = " · ";

static void panel_fit(const char *label, char *fitted, size_t size, int font, int width)
{
    if (text_fit(label, fitted, size, font, width) == label)
        strlcpy(fitted, label, size);
}

static void panel_layout_header(void)
{
    int title = panel.album_font >= 0 ? panel.album_font : FONT_SYSFIXED;
    int small = panel.small_font >= 0 ? panel.small_font : FONT_SYSFIXED;
    panel.dot_x = 0;
    panel.count_x = PANEL_LEFT;
    panel.count_label[0] = 0;
    if (panel.selecting)
        snprintf(panel.count_label, sizeof(panel.count_label), str(LANG_N_SELECTED), panel.marked);
    else if (panel.count >= 0 && !stock_look())
    {
        const char *format = panel_chinese() ? "%d 首" : (const char *)str(LANG_N_TRACKS_TITLE);
        int year = panel.request.album.year;
        int n = year > 0 ? snprintf(panel.count_label, sizeof(panel.count_label),
                                    "%d%s", year, panel_middot) : 0;
        snprintf(panel.count_label + n, sizeof(panel.count_label) - n, format, panel.count);
    }
    bool ready = font_prefetch_text(title, panel.header.title);
    ready &= font_prefetch_text(title, ".");
    ready &= font_prefetch_text(small, panel.header.artist);
    ready &= font_prefetch_text(small, panel.count_label);
    ready &= font_prefetch_text(small, " ·.");
    panel.header_ready = ready;
    if (!ready)
        return;
    int budget = PANEL_HEADER_RIGHT - PANEL_LEFT;
    panel_fit(panel.header.title, panel.fitted_title, sizeof(panel.fitted_title),
              title, budget);
    int spacing = font_getstringsize(panel_middot, NULL, NULL, small);
    bool counted = panel.count >= 0 && *panel.count_label;
    if (counted)
        budget -= spacing + font_getstringsize(panel.count_label, NULL, NULL, small);
    panel.fitted_artist[0] = 0;
    if (budget >= 3 * font_get_width(font_get(small), '.'))
        panel_fit(panel.header.artist, panel.fitted_artist, sizeof(panel.fitted_artist),
                  small, budget);
    if (counted && *panel.fitted_artist)
    {
        panel.dot_x = PANEL_LEFT + font_getstringsize(panel.fitted_artist, NULL, NULL, small);
        panel.count_x = panel.dot_x + spacing;
    }
}

static void panel_layout_track(struct panel_track *row)
{
    int title = panel.title_font >= 0 ? panel.title_font : FONT_SYSFIXED;
    bool ready = font_prefetch_text(title, row->title);
    ready &= font_prefetch_text(title, ".");
    row->glyphs_ready = ready && (!row->duration || panel.digits_ready);
    if (!row->glyphs_ready)
        return;
    int end = PANEL_ROWS_RIGHT;
    if (row->duration)
    {
        unsigned int s = row->duration / 1000;
        if (s >= 3600)
            snprintf(row->time, sizeof(row->time), "%u:%02u:%02u",
                     s / 3600, s / 60 % 60, s % 60);
        else
            snprintf(row->time, sizeof(row->time), "%u:%02u", s / 60, s % 60);

        if (panel.tabular)
            end -= font_getstringsize(row->time, NULL, NULL, title);
        else
        {
            int colon = font_get_width(font_get(title), ':');
            for (const char *ch = row->time; *ch; ch++)
                end -= *ch == ':' ? colon : panel.digit_width;
        }
        row->time_x = end;
        end -= PANEL_TIME_GAP;
    }
    panel_fit(row->title, row->fitted, sizeof(row->fitted), title, end - PANEL_LEFT);
}

/* The top of a line of `font` centred in a span `height` rows tall. */
static int panel_line_top(int font, int height)
{
    return (height - font_get(font)->height) / 2;
}

static void panel_header(const struct panel_ink *ink)
{
    int title = panel.album_font >= 0 ? panel.album_font : FONT_SYSFIXED;
    int meta = panel.small_font >= 0 ? panel.small_font : FONT_SYSFIXED;
    bool ready = panel.header_ready;
    if (ready)
    {
        ready &= font_prefetch_text(title, panel.fitted_title);
        ready &= font_prefetch_text(meta, panel.fitted_artist);
        ready &= font_prefetch_text(meta, panel.count_label);
        ready &= !panel.dot_x || font_prefetch_text(meta, panel_middot);
    }
    int title_height = font_get(title)->height;
    int meta_height = font_get(meta)->height;
    int title_y = (PANEL_DIVIDER - title_height - meta_height) / 2;
    int y = title_y + title_height;
    lcd_set_foreground(ink->title);
    lcd_setfont(title);
    if (ready)
        lcd_putsxy(PANEL_LEFT, title_y, panel.fitted_title);
    lcd_setfont(meta);
    lcd_set_foreground(stock_look() ? ink->title : ink->text);
    if (ready && *panel.fitted_artist)
        lcd_putsxy(PANEL_LEFT, y, panel.fitted_artist);
    if (ready && panel.dot_x)
        lcd_putsxy(panel.dot_x, y, panel_middot);
    if (ready && *panel.count_label)
        lcd_putsxy(panel.count_x, y, panel.count_label);
}

static void panel_duration(const struct panel_track *row, int y)
{
    if (panel.tabular)
    {
        lcd_putsxy(row->time_x, y, row->time);
        return;
    }

    /* A proportional replacement for the 12 px face also gets tabular digits. */
    int x = row->time_x;
    struct font *font = font_get(lcd_getfont());
    for (const char *p = row->time; *p; p++)
    {
        char ch[2] = {*p, '\0'};
        int width = font_get_width(font, *p);
        int cell = *p == ':' ? width : panel.digit_width;
        lcd_putsxy(x + (cell - width) / 2, y, ch);
        x += cell;
    }
}

static bool panel_marked(int track)
{
    return panel.selecting && (panel.marks[track / 8] & (1 << (track % 8)));
}

static void panel_toggle_mark(void)
{
    int track = panel.selected;
    panel.marked += panel_marked(track) ? -1 : 1;
    panel.marks[track / 8] ^= 1 << (track % 8);
    panel.header_ready = false;
    panel.dirty = true;
}

static void panel_paint_menu(struct viewport *view, const struct panel_ink *ink)
{
    int title = panel.album_font >= 0 ? panel.album_font : FONT_SYSFIXED;
    int body = panel.title_font >= 0 ? panel.title_font : FONT_SYSFIXED;
    const struct panel_cache_row *row = panel_source_row(panel.selected);
    char count[48];
    const char *heading = row ? row->title : panel.header.title;
    if (panel.selecting)
    {
        snprintf(count, sizeof(count), str(LANG_N_SELECTED), panel.marked);
        heading = count;
    }

    lcd_setfont(title);
    lcd_set_foreground(ink->title);
    panel.menu_ready = panel_text(heading, PANEL_LEFT,
                                  panel_line_top(title, PANEL_DIVIDER) +
                                  font_get(title)->ascent, PANEL_HEADER_RIGHT);

    struct viewport rows = *view;
    rows.y += PANEL_ROWS_Y;
    rows.height = ALBUM_PANEL_ROWS * PANEL_PITCH;
    lcd_set_viewport(&rows);
    lcd_setfont(body);
    int baseline = panel_line_top(body, PANEL_PITCH) + font_get(body)->ascent;
    for (int i = 0; i < panel.command_count; ++i)
    {
        int y = i * PANEL_PITCH;
        bool selected = i == panel.command_selected;
        if (selected)
            panel_mark(y, ink);
        lcd_set_foreground(selected ? ink->selected_text : ink->text);
        const char *label = str(panel_command_labels[panel.commands[i]]);
        panel.menu_ready &= panel_text(label, PANEL_LEFT, y + baseline,
                                       PANEL_ROWS_RIGHT);
    }
    lcd_set_viewport(view);
}

/* The row the selected track is drawn in, counting disc labels. */
static int panel_selected_row(void)
{
    int row = panel.selected;
    for (int i = 0; i < panel.disc_count &&
                    panel_disc_track(i) <= panel.selected; i++)
        row++;
    return row;
}

static bool panel_marquee_on(int track)
{
    return track + 1 == panel.marquee_track && panel.marquee;
}

/* A selected title too long for its row scrolls with the caption's scroll
 * settings, from its start to its end and back. */
static void panel_marquee_select(int track, const struct panel_track *row,
                                 int body)
{
    if (track + 1 == panel.marquee_track)
        return;
    panel.marquee_track = track + 1;
    panel.marquee = panel.marquee_step = 0;
    int end = row->duration ? row->time_x - PANEL_TIME_GAP : PANEL_ROWS_RIGHT;
    panel.marquee_room = end - PANEL_LEFT;
    panel.marquee_width = font_getstringsize(row->title, NULL, NULL, body);
    if (global_settings.scrolling_enabled && strcmp(row->title, row->fitted) &&
        panel.marquee_width > panel.marquee_room)
    {
        panel.marquee_step = MAX(1, scroll_line_info.step);
        panel.marquee_start = current_tick + scroll_line_info.delay;
    }
}

/* True when the selected title moved. */
static bool panel_marquee_step(void)
{
    if (!panel.marquee_step || TIME_BEFORE(current_tick, panel.marquee_start))
        return false;
    int end = panel.marquee_width - panel.marquee_room;
    panel.marquee += panel.marquee_step;
    panel.marquee_start = current_tick + scroll_line_info.ticks;
    if (panel.marquee >= end || panel.marquee <= 0)
    {
        panel.marquee = MIN(MAX(panel.marquee, 0), end);
        panel.marquee_step = -panel.marquee_step;
        panel.marquee_start = current_tick + scroll_line_info.delay * 2;
    }
    return true;
}

/* One track row at `y` in the rows viewport `rows`. */
static void panel_paint_track(struct viewport *rows, int track, int y,
                              const struct panel_ink *ink)
{
    int body = panel.title_font >= 0 ? panel.title_font : FONT_SYSFIXED;
    struct panel_track *row = &panel.rows[track % PANEL_CACHE];
    bool selected = track == panel.selected;
    pix_t text = selected ? ink->selected_text : ink->text;
    if (selected && stock_look())
    {
        for (int i = 0; i < PANEL_PITCH; i++)
        {
            lcd_set_foreground(panel_bar[i]);
            lcd_hline(1, rows->width - 2, y + i);
        }
    }
    else if (selected)
        panel_mark(y, ink);
    bool ready = row->index == track + 1 && row->glyphs_ready;
    if (ready)
    {
        ready &= font_prefetch_text(body, row->fitted);
        ready &= font_prefetch_text(body, row->time);
        /* Retry layout and repaint when a formerly resident glyph returns. */
        row->glyphs_ready = ready;
    }
    if (!ready)
    {
        char number[16];
        snprintf(number, sizeof(number), "%d", track + 1);
        lcd_setfont(FONT_SYSFIXED);
        lcd_set_foreground(text);
        lcd_putsxy(PANEL_LEFT, y + panel_line_top(FONT_SYSFIXED, PANEL_PITCH),
                   number);
        return;
    }

    panel.painted_rows++;
    int mark = y + (PANEL_PITCH - 7) / 2;
    if (panel_marked(track))
    {
        lcd_set_foreground(text);
        lcd_drawline(PANEL_SIGN_X, mark + 3, PANEL_SIGN_X + 2, mark + 5);
        lcd_drawline(PANEL_SIGN_X + 2, mark + 5, PANEL_SIGN_X + 6, mark);
    }
    else if (row->playing)
    {
        lcd_set_foreground(ink->accent);
        for (int i = 0; i < 7; i++)
            lcd_hline(PANEL_SIGN_X, PANEL_SIGN_X + MIN(i, 6 - i), mark + i);
    }
    int top = y + panel_line_top(body, PANEL_PITCH);
    lcd_setfont(body);
    if (row->duration)
    {
        lcd_set_foreground(selected ? ink->selected_muted : ink->dim);
        panel_duration(row, top);
    }
    lcd_set_foreground(text);
    if (selected)
        panel_marquee_select(track, row, body);
    if (!panel_marquee_on(track))
    {
        lcd_putsxy(PANEL_LEFT, top, row->fitted);
        return;
    }
    if (!font_prefetch_text(body, row->title))
        return;
    struct viewport clip = *rows;
    clip.x += PANEL_LEFT;
    clip.y += y;
    clip.width = panel.marquee_room;
    clip.height = PANEL_PITCH;
    lcd_set_viewport(&clip);
    lcd_setfont(body);
    lcd_set_foreground(ink->selected_text);
    lcd_putsxy(-panel.marquee, top - y, row->title);
    lcd_set_viewport(rows);
}

static void panel_paint_scrollbar(int height, int y0,
                                  const struct panel_ink *ink)
{
    if (panel.count <= ALBUM_PANEL_ROWS)
        return;
    int total = panel.count + panel.disc_count;
    int thumb = MAX(12, height * ALBUM_PANEL_ROWS / total);
    int y = (int64_t)(height - thumb) * panel.scroll_y /
            ((total - ALBUM_PANEL_ROWS) * PANEL_PITCH);
    lcd_set_foreground(ink->edge);
    lcd_fillrect(PANEL_BAR_X, -y0, 2, height);
    lcd_set_foreground(ink->text);
    lcd_fillrect(PANEL_BAR_X, y - y0, 2, thumb);
}

static void panel_paint_rows(struct viewport *view, const struct panel_ink *ink)
{
    int small = panel.small_font >= 0 ? panel.small_font : FONT_SYSFIXED;
    int title = panel.title_font >= 0 ? panel.title_font : FONT_SYSFIXED;

    struct viewport rows = *view;
    rows.y += PANEL_ROWS_Y;
    rows.height = ALBUM_PANEL_ROWS * PANEL_PITCH;
    lcd_set_viewport(&rows);
    panel.painted_rows = panel.painted_discs = 0;
    if (panel.play_failed || panel.count <= 0)
    {
        const char *label = panel.play_failed ? str(LANG_PLAYLIST_ACCESS_ERROR) :
            panel.count == -1 ? str(LANG_WAIT) :
            panel.count == -2 ? str(LANG_TAGCACHE_BUSY) : str(LANG_NO_FILES);
        lcd_setfont(title);
        lcd_set_foreground(ink->text);
        panel_text(label, PANEL_LEFT,
                   panel_line_top(title, PANEL_PITCH) + font_get(title)->ascent,
                   PANEL_ROWS_RIGHT);
    }
    else
    {
        int disc;
        int track = panel_first_track(panel.scroll_y / PANEL_PITCH, &disc);
        for (int y = -(panel.scroll_y % PANEL_PITCH);
             y < rows.height && track < panel.count; y += PANEL_PITCH)
        {
            if (disc < panel.disc_count && panel_disc_track(disc) == track)
            {
                char label[80];
                panel_disc_label(disc, label, sizeof(label));
                lcd_setfont(small);
                lcd_set_foreground(ink->dim);
                if (font_prefetch_text(small, label))
                    panel.painted_discs++;
                int baseline = panel_line_top(small, PANEL_PITCH) +
                               font_get(small)->ascent;
                panel_text(label, PANEL_LEFT, y + baseline, PANEL_ROWS_RIGHT);
                disc++;
                continue;
            }
            panel_paint_track(&rows, track, y, ink);
            track++;
        }
        panel_paint_scrollbar(rows.height, 0, ink);
    }
    lcd_set_viewport(view);
}

static struct viewport panel_viewport(void)
{
    struct viewport view = pf_vp;
    view.x = ALBUM_PANEL_X;
    view.y = ALBUM_PANEL_Y;
    view.width = ALBUM_PANEL_WIDTH;
    view.height = ALBUM_PANEL_HEIGHT;
    view.flags &= ~VP_FLAG_ALIGNMENT_MASK;
    return view;
}

static void NO_INLINE paint_album_panel(void)
{
    struct viewport view = panel_viewport();
    struct viewport *previous = lcd_set_viewport(&view);
    struct panel_ink ink;
    panel_ink(&ink);
    lcd_set_background(ink.back);
    lcd_set_drawmode(DRMODE_SOLID);
    panel.have_blur = !panel.solid &&
        album_blur_draw_panel(&panel.blur_source, 0, ALBUM_PANEL_HEIGHT);
    /* The blurred cover carries its own darker header band. */
    if (!panel.have_blur)
    {
        lcd_clear_viewport();
        lcd_set_foreground(ink.band);
        if (!stock_look())
            lcd_fillrect(1, 1, view.width - 2, PANEL_DIVIDER - 1);
    }
    /* The card runs off the bottom of the screen: no edge there. Stock's
     * header tile starts at the card's top, its edge row below the tile. */
    lcd_set_foreground(ink.edge);
    if (stock_look())
    {
        ui_art_tile(STOCK_ART_COVERFLOW_BACKSIDE_TOP, view.x, view.y,
                    view.width, false,
                    (struct ui_rect){view.x, view.y, view.width, PANEL_DIVIDER});
        lcd_hline(0, view.width - 1, PANEL_DIVIDER - 1);
    }
    else
        lcd_hline(0, view.width - 1, 0);
    lcd_vline(0, 1, view.height - 1);
    lcd_vline(view.width - 1, 1, view.height - 1);

    lcd_set_drawmode(DRMODE_FG);
    if (panel.command_count)
        panel_paint_menu(&view, &ink);
    else
    {
        panel_header(&ink);
        panel_paint_rows(&view, &ink);
    }
    lcd_set_viewport(previous);
}

/* Moves the selected title on: repaints its row alone. */
static void panel_repaint_marquee(void)
{
    int y = panel_selected_row() * PANEL_PITCH - panel.scroll_y;
    if (y < 0 || y + PANEL_PITCH > ALBUM_PANEL_ROWS * PANEL_PITCH)
        return;
    struct viewport view = panel_viewport();
    struct viewport rows = view;
    rows.y += PANEL_ROWS_Y;
    rows.height = ALBUM_PANEL_ROWS * PANEL_PITCH;
    struct viewport *previous = lcd_set_viewport(&view);
    struct panel_ink ink;
    panel_ink(&ink);
    /* The row's backdrop again first: no bar covers the old title. */
    int top = PANEL_ROWS_Y + y;
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(ink.back);
    if (!panel.have_blur ||
        !album_blur_draw_panel(&panel.blur_source, top, PANEL_PITCH))
        lcd_fillrect(0, top, view.width, PANEL_PITCH);
    lcd_set_foreground(ink.edge);
    lcd_vline(0, top, top + PANEL_PITCH - 1);
    lcd_vline(view.width - 1, top, top + PANEL_PITCH - 1);
    lcd_set_viewport(&rows);
    lcd_set_drawmode(DRMODE_FG);
    unsigned painted = panel.painted_rows;
    panel_paint_track(&rows, panel.selected, y, &ink);
    panel.painted_rows = painted;
    struct viewport bar = rows;
    bar.y += y;
    bar.height = PANEL_PITCH;
    lcd_set_viewport(&bar);
    panel_paint_scrollbar(rows.height, y, &ink);
    lcd_set_viewport(previous);
    lcd_update_rect(view.x + 1, rows.y + y, view.width - 2, PANEL_PITCH);
}
/* Neighbours stay visible around the turning panel. */
/* Match the live panel's inset at BACK. Rows below the LCD have the same
 * theme background as the list. The texture is column-major like cover art. */
static void prepare_stock_back(void)
{
    if (panel.back_handle <= 0)
        return;
    paint_album_panel();
    pix_t *back = core_get_data_pinned(panel.back_handle);
    pix_t bg = ui_theme_rgb565(UI_COLOR_BACKGROUND);
    memset16(back, bg, 256 * 256);
    int side = MAX(256, MAX(ALBUM_PANEL_WIDTH, ALBUM_PANEL_HEIGHT));
    int inset_x = (side - ALBUM_PANEL_WIDTH) / 2;
    int inset_y = fbound(0, ALBUM_PANEL_Y - 27, side - ALBUM_PANEL_HEIGHT);
    for (int x = 0; x < 256; x++)
    {
        int sx = x * side / 256 - inset_x;
        if ((unsigned)sx >= ALBUM_PANEL_WIDTH)
            continue;
        for (int y = 0; y < 256; y++)
        {
            int sy = y * side / 256 - inset_y;
            if ((unsigned)sy < ALBUM_PANEL_HEIGHT)
                back[x * 256 + y] = LCD_BUF[(ALBUM_PANEL_Y + sy) * BUFFER_WIDTH +
                                             ALBUM_PANEL_X + sx];
        }
    }
    core_put_data_pinned(back);
    panel.back_ready = true;
}

/* Map a column-major square onto the two projected vertical edges.
 * The panel reads left to right when the back faces the viewer. With
 * `reflection`, map instead the quad half as tall directly below the face,
 * whose texture holds tex_h texels a column. */
static void stock_draw_part(pix_t *dst, struct stock_pose pose, const pix_t *texture,
                            int tex_w, int tex_h, int stride,
                            const struct ui_rect *clip, bool reflection)
{
    struct stock_edge edge[2];
    stock_project(pose, edge);
    bool reverse = edge[0].x > edge[1].x;
    struct stock_edge left = edge[reverse], right = edge[!reverse];
    int width = right.x - left.x;
    int first = MAX(clip->x, (left.x + 32767) >> 16);
    int end = MIN(clip->x + clip->width, (right.x + 32767) >> 16);
    if (first >= end)
        return;
    int hl = left.bottom - left.top, hr = right.bottom - right.top;
    bool mirror = reverse != stock_back_face(pose);
    for (int x = first; x < end; x++)
    {
        int t = ((int64_t)((x << 16) + 32768 - left.x) << 16) / width;
        int h = hl + ((int64_t)(hr - hl) * t >> 16);
        int top = left.top + ((int64_t)(right.top - left.top) * t >> 16);
        int u_height = h;
        if (reflection)
        {
            top += h;
            h /= 2;
        }
        int y = MAX(clip->y, (top + 32767) >> 16);
        int bottom = MIN(clip->y + clip->height, (top + h + 32767) >> 16);
        if (bottom <= y)
            continue;
        int u = ((int64_t)hr * t * tex_w / u_height) >> 16;
        u = MIN(tex_w - 1, u);
        if (mirror)
            u = tex_w - 1 - u;
        int dy = ((int64_t)tex_h << (16 + PFREAL_SHIFT)) / h;
        int v = ((int64_t)((y << 16) + 32768 - top) * dy) >> 16;
        struct pf_col col = {texture + u * stride, dst + y * BUFFER_WIDTH + x,
                             v, dy, bottom - y};
        draw_run(&col, col.n, BUFFER_WIDTH);
    }
}

static void stock_draw_clipped_quad(pix_t *dst, struct stock_pose pose, const pix_t *texture,
                            int tex_w, int tex_h, int stride, const struct ui_rect *clip)
{
    stock_draw_part(dst, pose, texture, tex_w, tex_h, stride, clip, false);
}

/* A cover's reflection, `refl` holding reflection_rows(side) texels for
 * each of its `side` columns. */
static void stock_draw_reflection(pix_t *dst, struct stock_pose pose,
                                  const pix_t *refl, int side)
{
    const struct ui_rect screen = {0, pf_chrome_h, LCD_WIDTH, LCD_HEIGHT - pf_chrome_h};
    int rows = reflection_rows(side);
    stock_draw_part(dst, pose, refl, side, rows, rows, &screen, true);
}

static void stock_draw_quad(pix_t *dst, struct stock_pose pose, const pix_t *texture,
                            int tex_w, int tex_h, int stride)
{
    const struct ui_rect screen = {0, pf_chrome_h, LCD_WIDTH, LCD_HEIGHT - pf_chrome_h};
    stock_draw_clipped_quad(dst, pose, texture, tex_w, tex_h, stride, &screen);
}

static void render_stock_flip(struct stock_pose pose, int album, uint32_t now,
                              bool show_label)
{
    coverflow_place(now);
    bool visible = pose.x > -STOCK_BAND * STOCK_ONE && pose.x < STOCK_BAND * STOCK_ONE;
    bool back = stock_back_face(pose);
    if (visible && back && !panel.back_ready)
        prepare_stock_back();
    flow_paint(false, 0, 0, pf_height);
    if (visible && back && panel.back_handle > 0)
    {
        const pix_t *tex = core_get_data_pinned(panel.back_handle);
        stock_draw_quad(LCD_BUF, pose, tex, 256, 256, 256);
        core_put_data_pinned((void *)tex);
    }
    else if (visible && !back)
    {
        struct slide_data slide = {.slide_index = album, .cached_slot = -1};
        const struct dim *bmp = surface(&slide);
        if (bmp)
        {
            int size = MIN(bmp->width, bmp->height);
            const pix_t *crop = (const pix_t *)(bmp + 1) +
                (bmp->width - size) / 2 * bmp->height + (bmp->height - size) / 2;
            stock_draw_quad(LCD_BUF, pose, crop, size, size, bmp->height);
            if (stock_look())
                stock_draw_reflection(LCD_BUF, pose, slide_reflection(bmp), size);
        }
    }
    if (show_label)
        model->draw_text();
}

static void NO_INLINE render_album_flip(int position, pix_t color)
{
    pf_clear_band(0, pf_height);
    if (!position)
    {
        flat_render(true);
        model->draw_text();
        return;
    }
    struct dim *cover = surface(&center_slide);
    int w = cover ? cover->width : DISPLAY_WIDTH;
    int h = cover ? cover->height : DISPLAY_WIDTH;
    int cy = pf_vp_y + pf_draw_y_shift + slide_voff(h, center_slide.cy) + h / 2;
    bool back = position >= FLIP_MID;
    const struct ui_tween ease = {.from = 0, .to = FLIP_MID,
                                  .frames = FLIP_MID, .ease_in = !back};
    int turn = ui_tween_sample(&ease, back ? position - FLIP_MID : position);
    int angle = back ? -256 + 256 * turn / FLIP_MID : 256 * turn / FLIP_MID;
    pf_neighbor_light = 256 - 128 * MIN(position, FLIP_MID) / FLIP_MID;
    flat_render(false);
    pf_neighbor_light = 256;
    if (back)
    {
        w += (ALBUM_PANEL_WIDTH - w) * turn / FLIP_MID;
        h += (ALBUM_PANEL_HEIGHT - h) * turn / FLIP_MID;
        cy += (ALBUM_PANEL_Y + ALBUM_PANEL_HEIGHT / 2 - cy) * turn / FLIP_MID;
    }
    pix_t *saved = buffer;
    buffer = LCD_BUF;
    if (position != FLIP_OPEN)
        render_flip_face(angle, w, h, back, color, cy, LCD_HEIGHT);
    buffer = saved;
}

static void draw_album_flip(bool present)
{
    show_carousel_entry();
    lcd_set_viewport(&pf_vp);
    pf_update_dynamic_colors();
    lcd_set_background(pf_bg_color);
    lcd_set_foreground(pf_fg_color);
    lcd_set_drawmode(DRMODE_FG);
    pix_t color = ui_theme_rgb565(UI_COLOR_MODAL_BG);
    buf_ctx_lock();
    uint32_t render_start = panel.trace ? USEC_TIMER : 0;
    if (flat_view())
        render_album_flip(panel.position, color);
    else if (panel.position == FLIP_OPEN || !panel.position)
    {
        uint32_t now = USEC_TIMER;
        if (flow_row.phase != STOCK_REST && !stock_animating(&flow_row, now))
            stock_init(&flow_row, number_of_slides, center_index, now, false);
        coverflow_place(now);
        flow_paint(!panel.position, 0, 0, pf_height);
        if (!panel.position)
            model->draw_text();
    }
    else
    {
        uint32_t now = USEC_TIMER;
        render_stock_flip(stock_flip_pose(&panel.flip, now), center_index, now, panel.to == 0);
    }
    uint32_t render = panel.trace ? (uint32_t)USEC_TIMER - render_start : 0;
    buf_ctx_unlock();
    bool opened = false;
    if (panel.position == FLIP_OPEN)
    {
        uint32_t draw_start = USEC_TIMER;
        paint_album_panel();
        uint32_t draw = (uint32_t)USEC_TIMER - draw_start;
        render += draw;
        panel_perf_record(PP_DRAW, draw, panel.painted_rows, panel.have_blur);
        int expected = MIN(panel.count, ALBUM_PANEL_ROWS);
        if (!panel.open_logged && expected >= 0 && panel.header_ready &&
            panel.painted_rows + panel.painted_discs >= (unsigned)expected)
            opened = true;
    }
    if (!present)
        return;
    uint32_t submit = USEC_TIMER;
    lcd_update();
    if (panel.trace)
    {
        uint32_t submitted = USEC_TIMER;
        ui_perf_note_render(render);
        ui_perf_note_submit(submitted - submit);
    }
    if (opened)
    {
        panel.open_logged = true;
        panel_perf_record(PP_OPEN, USEC_TIMER - panel.opened, panel.painted_rows,
                          panel.have_blur);
    }
    if (pf_scene.capturing)
        ui_scene_start(&pf_scene);
    panel.dirty = false;
}

/* A flip or a list scroll samples and presents a frame only on its
 * deadlines; other changes draw when they come. */
static void step_album_flip(void)
{
    if (panel.opening || panel.to == FLIP_OPEN)
    {
        panel.album_font = panel_cache_font(PANEL_ALBUM);
        panel.title_font = panel_cache_font(PANEL_BODY);
        panel.small_font = panel_cache_font(PANEL_SMALL);
    }
    if (panel.opening)
    {
        if (TIME_BEFORE(current_tick, panel.next_read))
            return;
        panel_read_tracks();
        bool ready = panel_opening_ready();
        const struct panel_cache_row *selected = panel_source_row(panel.selected);
        bool can_wait = !panel.request.remote || (selected && *selected->title);
        if (!ready && can_wait && (uint32_t)USEC_TIMER - panel.opened < 2000000u)
            return;
        panel.opening = false;
        if (ready)
            flip_to(true);
        else
        {
            /* Unavailable metadata or fonts still get the live panel's
             * loading state, without putting that state on a turning face. */
            panel.position = panel.to = FLIP_OPEN;
            panel.dirty = true;
        }
    }
    bool done = false;
    bool scrolling = panel.scroll_frames && panel.position == FLIP_OPEN &&
                     panel.to == FLIP_OPEN;
    bool walls = !flat_view() && stock_animating(&flow_row, USEC_TIMER);
    bool animating = panel.moving || scrolling || walls;
    bool due = frame_due(panel.due);
    if (walls && due)
        panel.dirty = true;
    if (panel.moving && due)
    {
        if (!flat_view())
        {
            done = stock_flip_done(&panel.flip, USEC_TIMER);
            panel.position = done ? panel.to : FLIP_MID;
            panel.dirty = true;
        }
        else
        {
            unsigned frame = ui_clock_frame((uint32_t)USEC_TIMER - panel.clock);
            if (frame != panel.sampled)
            {
                panel.sampled = frame;
                panel.position = flip_position(frame);
                panel.dirty = true;
            }
            done = frame >= panel.first + panel.second;
        }
    }
    if (!flat_view() || (panel.position == FLIP_OPEN && panel.to == FLIP_OPEN))
    {
        unsigned blur_generation = album_blur_generation();
        if (blur_generation != panel.blur_generation)
        {
            panel.blur_generation = blur_generation;
            /* A cold blur can arrive while either flip shows the back. */
            if (!panel.solid && !panel.have_blur)
                panel.back_ready = false;
            panel.dirty = true;
        }
    }
    if (panel.to == FLIP_OPEN && (panel.position == FLIP_OPEN || !flat_view()))
    {
        if (panel.scroll_frames && due)
            panel_step_scroll();
        if (!panel.resuming && !TIME_BEFORE(current_tick, panel.next_read))
            panel_read_tracks();
    }
    if (panel.position == FLIP_OPEN && panel.to == FLIP_OPEN && !animating &&
        !panel.command_count && panel_marquee_step() && !panel.dirty &&
        lcd_active())
        panel_repaint_marquee();
    bool draw = (panel.dirty || (panel.command_count && !panel.menu_ready)) &&
                lcd_active() && (!animating || due);
    if (draw)
    {
        draw_album_flip(true);
        panel.resuming = false;
        if (panel.flip_first && panel.moving)
            panel.flip.start_us += stock_rebase(&panel.clock);
        panel.flip_first = false;
    }
    if (animating && due)
    {
        uint32_t start = panel.moving || walls ? panel.clock : panel.scroll_clock;
        panel.due = flow_pace_next(&panel_pace, start);
    }
    if (draw)
    {
        if (done)
        {
            ui_perf_end(panel.trace);
            panel.trace = 0;
            panel.moving = false;
        }
        if (!panel.position && !panel.moving)
            close_album_panel();
    }
}

static void panel_open_actions(void)
{
    panel.command_count = panel.command_selected = 0;
    panel.catalog_menu = false;
    if (audio_status() & AUDIO_STATUS_PLAY)
    {
        panel.commands[panel.command_count++] = PANEL_ADD;
        panel.commands[panel.command_count++] = PANEL_NEXT;
        panel.commands[panel.command_count++] = PANEL_LAST;
    }
    if (!panel.selecting && panel.count <= LIST_SELECT_MAX_ROWS)
        panel.commands[panel.command_count++] = PANEL_SELECT_MANY;
    panel.commands[panel.command_count++] = PANEL_CATALOG;
    panel.dirty = true;
}

static bool panel_targets_changed(void)
{
    return !tagcache_is_in_ram() || panel.request.generation != tagcache_generation() ||
           panel.request.revision != (uint32_t)tagcache_commit_id() ||
           agents_nav_pending() != AGENTS_NAV_NONE;
}

static int panel_batch_handle;

/* The batch uses the same ordered IDs as the cache, including offscreen rows. */
static bool panel_apply_tracks(int position, const char *playlist, bool new_playlist)
{
    struct tagcache_search search;
    if (panel_targets_changed() || !tagcache_search(&search, tag_filename))
        return false;

    struct playlist_insert_context insert;
    bool ok = playlist || playlist_insert_context_create(NULL, &insert, position,
                                                          false, false) >= 0;
    struct db_panel_track *tracks = panel_batch_handle > 0
        ? core_get_data_pinned(panel_batch_handle) : NULL;
    const struct panel_cache_row *row = panel_source_row(panel.selected);
    char path[MAX_PATH];
    for (int i = tracks ? 0 : panel.selected;
         ok && i < (tracks ? panel.count : panel.selected + 1); ++i)
    {
        if (tracks && !panel_marked(i))
            continue;
        int id = tracks ? tracks[i].track.id : row ? row->id : -1;
        uint64_t key = tracks ? tracks[i].path_key : row ? row->playing_key : 0;
        ok = !panel_targets_changed() && id >= 0 && tagcache_find_key(key) == id &&
             tagcache_retrieve(&search, id, tag_filename, path, sizeof(path));
        if (ok)
        {
            ok = playlist ? catalog_insert_into(playlist, new_playlist, path,
                                                FILE_ATTR_AUDIO) >= 0
                          : playlist_insert_context_add(&insert, path) >= 0;
            new_playlist = false;
            if (ok && tracks)
            {
                panel.marks[i / 8] &= ~(1 << (i % 8));
                panel.marked--;
            }
        }
    }
    if (tracks)
        core_put_data_pinned(tracks);
    tagcache_search_finish(&search);
    if (!playlist)
    {
        playlist_insert_context_release(&insert);
        if (insert.count > 0)
            playlist_set_modified(NULL, true);
    }
    return ok;
}

static int panel_write_tracks(const char *playlist, bool new_playlist)
{
    bool ok = panel_apply_tracks(0, playlist, new_playlist);
    if (!ok)
        splash(HZ, ID2P(LANG_PLAYLIST_ACCESS_ERROR));
    return ok ? 0 : -1;
}

static void panel_run_command(enum panel_command command)
{
    static const int positions[] = {
        [PANEL_ADD] = PLAYLIST_INSERT,
        [PANEL_NEXT] = PLAYLIST_INSERT_FIRST,
        [PANEL_LAST] = PLAYLIST_INSERT_LAST,
    };
    panel.command_count = 0;
    if (command == PANEL_SELECT_MANY)
    {
        memset(panel.marks, 0, sizeof(panel.marks));
        panel.selecting = true;
        panel.marked = 0;
        panel_toggle_mark();
        return;
    }
    if (command == PANEL_CATALOG)
    {
        panel.catalog_menu = true;
        panel.commands[0] = PANEL_EXISTING;
        panel.commands[1] = PANEL_NEW;
        panel.command_count = 2;
        panel.command_selected = 0;
        return;
    }

    bool ready = !panel_targets_changed();
    if (ready && panel.selecting)
    {
        int count;
        panel_batch_handle = db_summary_album_panel_tracks(&panel.request.album, &count,
                                                           panel_targets_changed);
        ready = panel_batch_handle > 0 && count == panel.count;
    }

    bool applied = false;
    if (!ready)
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
    else if (command == PANEL_EXISTING || command == PANEL_NEW)
        applied = catalog_add_to_a_playlist("/", FILE_ATTR_AUDIO,
                                           command == PANEL_NEW, NULL,
                                           panel_write_tracks);
    else if (audio_status() & AUDIO_STATUS_PLAY)
    {
        applied = panel_apply_tracks(positions[command], NULL, false);
        if (!applied)
            splash(HZ, ID2P(LANG_PLAYLIST_ACCESS_ERROR));
    }

    if (panel_batch_handle > 0)
        core_free(panel_batch_handle);
    panel_batch_handle = 0;
    if (applied)
        panel.selecting = false;
    panel.header_ready = false;
}

static void panel_menu_action(int button)
{
    switch (button)
    {
    case PF_NEXT:
    case PF_NEXT_REPEAT:
        panel.command_selected = MIN(panel.command_selected + 1, panel.command_count - 1);
        break;
    case PF_PREV:
    case PF_PREV_REPEAT:
        panel.command_selected = MAX(0, panel.command_selected - 1);
        break;
    case PF_BACK:
    case PF_QUIT:
        if (panel.catalog_menu)
            panel_open_actions();
        else
            panel.command_count = 0;
        break;
    case PF_SELECT:
    case PF_TRACKLIST:
        panel_run_command(panel.commands[panel.command_selected]);
        break;
    default:
        return;
    }
    panel.dirty = true;
}

static int album_panel_action(int button)
{
    if (panel.command_count)
    {
        panel_menu_action(button);
        return CAROUSEL_MENU_STAY;
    }
    switch (button)
    {
    case PF_NEXT:
    case PF_NEXT_REPEAT:
    case PF_PREV:
    case PF_PREV_REPEAT:
        if (panel.count > 0 && !panel.play_pending)
        {
            int delta = button == PF_NEXT || button == PF_NEXT_REPEAT ? 1 : -1;
            int selected = MAX(0, MIN(panel.count - 1, panel.selected + delta));
            if (selected != panel.selected || panel.play_failed)
            {
                panel.selected = selected;
                panel.play_failed = false;
                panel.dirty = true;
                panel_follow_selection(true);
                panel.next_read = current_tick;
            }
        }
        break;
    case PF_BACK:
    case PF_QUIT:
        if (panel.selecting)
        {
            panel.selecting = false;
            panel.header_ready = false;
            panel.dirty = true;
        }
        else
            flip_to(false);
        break;
    case ACTION_STD_CONTEXT:
        if (!panel.request.remote && panel.position == FLIP_OPEN &&
            panel.to == FLIP_OPEN && !panel.play_pending && !panel_targets_changed() &&
            (panel.selecting ? panel.marked > 0 : panel_source_row(panel.selected) != NULL))
            panel_open_actions();
        break;
    case PF_SELECT:
    case PF_TRACKLIST:
        if (panel.selecting)
            panel_toggle_mark();
        else if (panel.to == 0)
            flip_to(true);
        else if (panel.count > 0 && !panel.play_pending)
        {
            panel.play_failed = false;
            const struct panel_cache_row *row = panel_source_row(panel.selected);
            if (!row)
                break;
            int ret = model->panel_play(center_index, panel.selected, &panel.request, row);
            if (ret != CAROUSEL_PLAY_PENDING)
                return ret;
            panel.play_pending = true;
            panel.dirty = true;
        }
        break;
    default:
        break;
    }
    return CAROUSEL_MENU_STAY;
}

struct carousel_frame {
    struct ui_page page;
    struct ui_view root, slides, captions;
    bool joined, tracing;
    uint32_t caption_us;
};

static void paint_slides(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    render_all_slides(frame->clip.y - pf_vp_y, frame->clip.height);
}

/* A hidden caption still queues its glyphs, so it can show on time. */
static void prefetch_caption(int index)
{
    struct carousel_album_header text;
    int font = carousel_caption_font(true);
    if (model->caption_at && index >= 0 && index < number_of_slides &&
        model->caption_at(index, &text))
    {
        font_prefetch_text(font, ".0123456789 \xe2\x80\x93");
        font_prefetch_text(font, text.title);
        font_prefetch_text(font, text.artist);
    }
}

static void paint_caption(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct carousel_frame *paint = view->data;
    uint32_t start = paint->tracing ? USEC_TIMER : 0;
    painted_ink_known = false;
    painted_top = INT_MAX;
    painted_end = 0;
    caption_rows_y = frame->clip.y - pf_vp_y;
    caption_rows_h = frame->clip.height;
    if (!paint->joined)
    {
        lcd_set_background(pf_bg_color);
        caption_clear_y = caption_rows_y;
        caption_clear_h = caption_rows_h;
        if (!caption_clears_itself)
        {
            pf_clear_band(caption_clear_y, caption_clear_h);
            caption_clear_h = 0;
        }
    }
#if !PF_SHOW_STATS
    if (!pf_caption_hidden)
        model->draw_text();
    else
        prefetch_caption(carousel_caption_index());
#endif
    /* A caption that drew nothing leaves its rows to be cleared here. */
    if (caption_clear_h > 0)
    {
        pf_clear_band(caption_clear_y, caption_clear_h);
        caption_clear_h = 0;
        caption_artist.valid = false;
    }
    int old_top = pf_text_y, old_end = pf_text_y + pf_text_h;
    update_caption_bounds();
    if (old_top != pf_text_y || old_end != pf_text_y + pf_text_h)
    {
        caption_rows_y = MIN(old_top, pf_text_y);
        caption_rows_h = MAX(old_end, pf_text_y + pf_text_h) - caption_rows_y;
    }
    if (paint->tracing)
        paint->caption_us += (uint32_t)USEC_TIMER - start;
}

static const struct ui_view_ops slide_ops = { .paint = paint_slides };
static const struct ui_view_ops caption_ops = { .paint = paint_caption };

static void carousel_frame_init(struct carousel_frame *paint)
{
    ui_view_init(&paint->root, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT},
                 NULL, NULL);
    ui_view_init(&paint->slides, (struct ui_rect){0, 0, LCD_WIDTH, 0}, &slide_ops, NULL);
    ui_view_init(&paint->captions, (struct ui_rect){0, 0, LCD_WIDTH, 0}, &caption_ops, paint);
    ui_view_append(&paint->root, &paint->slides);
    ui_view_append(&paint->root, &paint->captions);
    ui_page_init(&paint->page, &paint->root);
}

static void prefetch_captions(int previous)
{
    if (!model->caption_at || global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;
    int font = carousel_caption_font(true);
    int at = carousel_caption_index();
    int radius = 2 * ALBUM_COVERS_NUM_SLIDES;
    if (previous < 0)
        font_prefetch_text(font, ".0123456789 –");
    for (int i = 1; i <= 2 * radius; i++)
    {
        int index = at + (i % 2 ? (i + 1) / 2 : -i / 2);
        if (previous >= 0 && abs(index - previous) <= radius)
            continue;
        struct carousel_album_header text;
        if (index >= 0 && index < number_of_slides && model->caption_at(index, &text))
        {
            font_prefetch_text(font, text.title);
            font_prefetch_text(font, text.artist);
        }
    }
}

/* The default RGB565 framebuffer is aligned. Compare two pixels per load;
 * a one-pixel margin on either side is safe to include in the transfer. */
static void caption_ink_bounds(struct carousel_frame *paint, int top, int end,
                               int *left, int *right)
{
    typedef uint32_t pixel_pair __attribute__((may_alias));
    uint32_t start = paint->tracing ? USEC_TIMER : 0;
    uint32_t bg = (uint32_t)pf_bg_color * 0x10001u;
    int lo = LCD_WIDTH / 2, hi = 0;
    for (int y = top; y < end; y++)
    {
        const pixel_pair *row = (const pixel_pair *)(LCD_BUF + LCD_WIDTH * (pf_vp_y + y));
        int x = 0;
        while (x < lo && row[x] == bg)
            x++;
        lo = x;
        if (lo == LCD_WIDTH / 2)
            continue;
        x = LCD_WIDTH / 2;
        while (x > hi && row[x-1] == bg)
            x--;
        hi = x;
    }
    *left = 2 * lo;
    *right = 2 * hi;
    if (paint->tracing)
        paint->caption_us += (uint32_t)USEC_TIMER - start;
}

static void carousel_paint(struct carousel_frame *paint, bool art, bool caption,
                           bool joined, int y, int height)
{
    paint->joined = joined;
    paint->slides.value[UI_Y] = pf_vp_y + y;
    paint->slides.value[UI_HEIGHT] = art ? height : 0;
    paint->captions.value[UI_Y] = pf_vp_y + pf_text_y;
    paint->captions.value[UI_HEIGHT] = caption ? pf_text_h : 0;
    caption_rows_y = pf_text_y;
    caption_rows_h = pf_text_h;
    if (art)
        ui_page_invalidate(&paint->page, (struct ui_rect){0, pf_vp_y + y, LCD_WIDTH, height});
    if (caption)
        ui_page_invalidate(&paint->page, (struct ui_rect){0, pf_vp_y + pf_text_y, LCD_WIDTH, pf_text_h});
    font_prefetch_begin();
    int previous = caption_valid ? caption_index : -1;
    bool warm_neighbours = caption && previous != carousel_caption_index();
    /* The caption queues its own glyphs before the neighbouring titles. */
    ui_page_paint(&paint->page);
    if (warm_neighbours)
    {
        uint32_t start = paint->tracing ? USEC_TIMER : 0;
        prefetch_captions(previous);
        if (paint->tracing)
            paint->caption_us += (uint32_t)USEC_TIMER - start;
    }
    font_prefetch_end();
}

static bool restored_covers_ready(void)
{
    buf_ctx_lock();
    bool ready = true;
    int first = MAX(0, center_index - ALBUM_COVERS_NUM_SLIDES + 1);
    int last = MIN(number_of_slides - 1, center_index + ALBUM_COVERS_NUM_SLIDES - 1);
    for (int index = first; index <= last && ready; index++)
    {
        struct slide_data slide = {.slide_index = index, .cached_slot = -1};
        surface(&slide);
        ready = slide.cached_slot >= 0;
    }
    buf_ctx_unlock();
    return ready;
}

static void remember_current_cover(void)
{
    if (model->remember && model->remember(center_index))
        cover_resume_dirty = true;
}

/* The DMA producer has one pending staging slot. Leave it to the artwork
 * until the interrupt claims its buffer. The driver still arbitrates a race
 * with another producer after this check. */
static bool caption_stage_busy(uint32_t *slot)
{
#ifdef HAVE_LCD_VSYNC
    if (lcd_te_async_enabled() && lcd_te_pending())
    {
        if (slot)
            lcd_vsync_async_timing(NULL, NULL, slot, NULL);
        return true;
    }
#else
    (void)slot;
#endif
    return false;
}

static int stage_caption(int left, int right, int *top, int end, int pixels,
                         struct ui_perf_transfers *timing)
{
    int rows = MIN(end - *top, pixels / (right-left));
    if (rows > 0)
    {
        uint32_t start = timing ? USEC_TIMER : 0;
        lcd_update_rect(left, *top, right-left, rows);
        if (timing)
            ui_perf_add_transfer(timing, start, UI_PERF_CAPTION);
        *top += rows;
    }
    return rows;
}

/* The menu cross-fade follows the wall stagger. Leave uses the remaining
 * 250 ms of the backdated stock timer. */
#define FLOW_MENU_FADE_US STOCK_MENU_TIME(500000u)
#define FLOW_LEAVE_FADE_US 250000u

static bool flow_present_fade(struct ui_scene *source, unsigned opacity)
{
    if (opacity >= UI_FULL_OPACITY || !flow_count)
        return ui_scene_present_fade(source, opacity, NULL, NULL);
    static int16_t fade_top[LCD_WIDTH], fade_end[LCD_WIDTH];
    const int top = flow_clip_top(), end = flow_clip_end();
    for (int x = 0; x < LCD_WIDTH; x++)
    {
        const struct flow_column *c = &flow_columns[x];
        int first = MIN(flow_top[x], c->y0 >> 16);
        int last = MAX(flow_end[x],
                       (c->y0 + (c->height ? flow_extent(c->face, c->height) : 0)
                        + 65535) >> 16);
        fade_top[x] = c->height ? MAX(top, first) : 0;
        fade_end[x] = c->height ? MIN(end, last) : 0;
    }
    return ui_scene_present_fade(source, opacity, fade_top, fade_end);
}

/* The stock look slides the menu instead of fading it: on the way in its
 * rows leave to the left with quadratic ease-in while the covers show at
 * once in the pane's place; on the way out, after the first 50 ms, the rows
 * return from the left and the pane from the right with quadratic ease-out
 * in 200 ms. A leave that interrupts the entry brings the rows back from
 * where they had reached. */
#define FLOW_SPLIT_LEAVE_DELAY_US 50000u
static int flow_split_entered(uint32_t age)
{
    int u = MIN(age, FLOW_MENU_FADE_US) * 256 / FLOW_MENU_FADE_US;
    return MENU_PANE_X * u * u / 65536;
}

static bool flow_present_split(struct ui_scene *source, uint32_t age, bool leave,
                               int rows_from)
{
    const int half = MENU_PANE_X;
    if (!leave)
        return ui_scene_present_split(source, -flow_split_entered(age), half);
    const uint32_t duration = FLOW_LEAVE_FADE_US - FLOW_SPLIT_LEAVE_DELAY_US;
    age = age > FLOW_SPLIT_LEAVE_DELAY_US ? age - FLOW_SPLIT_LEAVE_DELAY_US : 0;
    int u = MIN(age, duration) * 256 / duration;
    int rest = (256 - u) * (256 - u);
    return ui_scene_present_split(source, -(rows_from * rest / 65536),
                                  half * rest / 65536);
}

/* Fruit Company runs stock 1.1.2's page slide: the menu's page moves its
 * own width in 270 ms, quadratic both ways. Leaving the menu, its rows and
 * their status bar accelerate away left and the pane is covers at once;
 * returning, the rows come from the left and the pane from the right, the
 * same distance from home, slowing to rest. A return that interrupts the
 * entry starts where the curves meet, `leave_skip` into it. */
#define FLOW_STOCK_SLIDE_US 270000u

static int flow_stock_offset(uint32_t age, bool back)
{
    int u = MIN(age, FLOW_STOCK_SLIDE_US) * 256 / FLOW_STOCK_SLIDE_US;
    if (back)
        u = 256 - u;
    return LCD_WIDTH * u * u / 65536;
}

static bool flow_present_stock(struct ui_scene *source, uint32_t age, bool back)
{
    int away = flow_stock_offset(age, back);
    return ui_scene_present_split(source, -away, back ? away : MENU_PANE_X);
}

/* The stock look enters from a menu without a status bar of its own: the
 * menu's leaves with its rows, and Cover Flow's drops in from the top with
 * quadratic ease-out, 100 ms long 363 ms after the rows begin to move.
 * Fruit Company's is stock's: 90 ms long, 355 ms after, when the caption
 * shows too. Until it is down, the bar waits in `handle`, taken again
 * whenever the status bar redraws, and the rows it has not reached show
 * the ground. A leave stops it where it is. */
#define BAR_DROP_DELAY_US 363000u
#define BAR_DROP_US 100000u
#define FLOW_STOCK_CHROME_US 355000u
#define FLOW_STOCK_BAR_US 90000u
static struct {
    int handle;
    uint32_t start, length;
    int rows;           /* the bar's rows on screen */
    bool stale, frozen;
} bar_drop;

static void bar_drop_free(void)
{
    if (bar_drop.handle > 0)
        bar_drop.handle = core_free(bar_drop.handle);
    bar_drop.handle = 0;
}

/* The status bar has drawn itself over the rows it is held out of. */
static void bar_drop_take(void)
{
    if (bar_drop.handle <= 0)
        return;
    pix_t *bar = core_get_data_pinned(bar_drop.handle);
    memcpy(bar, LCD_BUF, pf_chrome_h * LCD_WIDTH * sizeof(*bar));
    core_put_data_pinned(bar);
    bar_drop.stale = true;
}

static bool bar_drop_hold(void)
{
    bar_drop_free();
    if (pf_chrome_h <= 0)
        return false;
    bar_drop.handle = core_alloc_free_space(pf_chrome_h * LCD_WIDTH *
                                            sizeof(pix_t));
    if (bar_drop.handle <= 0)
        return false;
    sb_update(SCREEN_MAIN, true);
    bar_drop_take();
    bar_drop.rows = 0;
    return true;
}

static void bar_drop_begin(uint32_t now, bool stock)
{
    if (!bar_drop_hold())
        return;
    bar_drop.start = now + (stock ? FLOW_STOCK_CHROME_US : BAR_DROP_DELAY_US);
    bar_drop.length = stock ? FLOW_STOCK_BAR_US : BAR_DROP_US;
    bar_drop.frozen = false;
}

/* Stock's return to the menu takes Cover Flow's bar away at once. */
static void bar_drop_hide(void)
{
    if (bar_drop.handle <= 0 && !bar_drop_hold())
        return;
    bar_drop.rows = 0;
    bar_drop.frozen = true;
    bar_drop.stale = true;
}

static bool bar_drop_waiting(void)
{
    return bar_drop.handle > 0 && !bar_drop.frozen;
}

static int bar_drop_rows(uint32_t now)
{
    if (bar_drop.frozen)
        return bar_drop.rows;
    int32_t t = now - bar_drop.start;
    if (t <= 0)
        return 0;
    int u = 256 - (int)(MIN((uint32_t)t, bar_drop.length) * 256 /
                        bar_drop.length);
    return pf_chrome_h - pf_chrome_h * u * u / 65536;
}

/* When the bar next moves. */
static uint32_t bar_drop_due(uint32_t now)
{
    return (int32_t)(now - bar_drop.start) < 0 ? bar_drop.start
                                                 : ui_clock_start() + ui_frame_period();
}

static bool bar_drop_owed(uint32_t now)
{
    return bar_drop.handle > 0 &&
           (bar_drop.stale || bar_drop_rows(now) != bar_drop.rows);
}

/* Lay the bar's rows for `now` into the framebuffer. True when they
 * changed there. */
static bool bar_drop_compose(uint32_t now)
{
    if (!bar_drop_owed(now))
        return false;
    int rows = bar_drop_rows(now);
    const pix_t *bar = core_get_data_pinned(bar_drop.handle);
    for (int y = 0; y < pf_chrome_h; y++)
    {
        pix_t *row = LCD_BUF + y * LCD_WIDTH;
        if (y < rows)
            memcpy(row, bar + (pf_chrome_h - rows + y) * LCD_WIDTH,
                   LCD_WIDTH * sizeof(*row));
        else
            memset16(row, pf_bg_color, LCD_WIDTH);
    }
    core_put_data_pinned((void *)bar);
    bar_drop.rows = rows;
    bar_drop.stale = false;
    if (rows == pf_chrome_h && !bar_drop.frozen)
        bar_drop_free();
    return true;
}

/* The album panel puts the whole bar down at once. */
static void bar_drop_finish(void)
{
    if (caption_waits)
        caption_reveal = USEC_TIMER;
    if (!bar_drop_waiting())
        return;
    bar_drop.start = USEC_TIMER - bar_drop.length;
    bar_drop_compose(USEC_TIMER);
    lcd_update_rect(0, 0, LCD_WIDTH, pf_vp_y + pf_chrome_h);
}

/* The local playlist needs the carousel's scratch arena before WPS can read
 * its art. Retain just the visible textures and wall poses across that handoff. */
struct playback_texture {
    int album, handle, width, height;
    bool reflected;     /* its reflection follows its pixels */
};
static struct {
    struct playback_texture wall[STOCK_MAX_VISIBLE + 1], art;
    struct stock_pose centre, playing;
    struct stock_flow walls;
    int count, album;
    struct ui_rect slot;
    struct album_blur_source source;
    uint32_t return_start;
    uint32_t action_us, playlist_us;
    /* Under the stock look, the frame Now Playing's chrome fades in over,
     * or that chrome and its mask as it fades out; and the status bar
     * Cover Flow showed, which the turn back fades to. */
    int chrome, bar;
    pix_t bg;
    bool pending, from_panel, returning;
} playback_scene;

static void drop_playback_bitmap(int *handle)
{
    if (*handle > 0)
        core_free(*handle);
    *handle = 0;
}

static void release_playback_scene(void)
{
    for (int i = 0; i < playback_scene.count; i++)
        drop_playback_bitmap(&playback_scene.wall[i].handle);
    drop_playback_bitmap(&playback_scene.art.handle);
    drop_playback_bitmap(&playback_scene.chrome);
    drop_playback_bitmap(&playback_scene.bar);
    memset(&playback_scene, 0, sizeof(playback_scene));
}

static bool playback_panel_return(void)
{
    return playback_scene.from_panel;
}

bool carousel_wps_present_pending(void)
{
    return playback_scene.pending;
}

static struct playback_texture *playback_cover(int album)
{
    for (int i = 0; i < playback_scene.count; i++)
        if (playback_scene.wall[i].album == album)
            return &playback_scene.wall[i];
    return NULL;
}

static void keep_playback_cover(int album)
{
    if (playback_cover(album) || playback_scene.count >= (int)ARRAYLEN(playback_scene.wall))
        return;
    struct slide_data slide = {.slide_index = album, .cached_slot = -1};
    const struct dim *bmp = surface(&slide);
    if (!bmp)
        return;
    int side = MIN(bmp->width, bmp->height);
    if (side <= 0)
        return;
    bool reflected = stock_look();
    int rows = reflected ? reflection_rows(side) : 0;
    int handle = core_alloc_free_space(side * (side + rows) * sizeof(pix_t));
    if (handle <= 0)
        return;
    pix_t *dst = core_get_data_pinned(handle);
    const pix_t *src = (const pix_t *)(bmp + 1) +
            (bmp->width - side) / 2 * bmp->height + (bmp->height - side) / 2;
    for (int x = 0; x < side; x++)
        memcpy(dst + x * side, src + x * bmp->height, side * sizeof(*dst));
    if (reflected)
        memcpy(dst + side * side, slide_reflection(bmp),
               side * rows * sizeof(*dst));
    core_put_data_pinned(dst);
    playback_scene.wall[playback_scene.count++] =
        (struct playback_texture){album, handle, side, side, reflected};
}

static struct stock_pose playback_target(struct ui_rect slot)
{
    struct stock_pose pose;
    if (slot.width && native_wps_art_turned())
        return stock_playing_pose();
    if (stock_wps_pose(slot.x, slot.y, slot.width, slot.height, &pose))
        return pose;
    return stock_playing_pose();
}

/* The exposed slot includes the native margins of a non-square cover. */
static void capture_return_art(void)
{
    struct ui_rect slot = playback_scene.slot;
    drop_playback_bitmap(&playback_scene.art.handle);
    if (!slot.width || !native_wps_has_art())
        return;
    int handle = core_alloc_free_space(slot.width * slot.height * sizeof(pix_t));
    if (handle <= 0)
        return;
    const pix_t *src = (const pix_t *)lcd_framebuffer_default.fb_ptr +
                       slot.y * LCD_WIDTH + slot.x;
    pix_t *dst = core_get_data_pinned(handle);
    for (int x = 0; x < slot.width; x++)
        for (int y = 0; y < slot.height; y++)
            dst[x * slot.height + y] = src[y * LCD_WIDTH + x];
    core_put_data_pinned(dst);
    playback_scene.art = (struct playback_texture){0, handle, slot.width,
                                                   slot.height, false};
}

static void paint_playback_texture(struct stock_pose pose, const struct playback_texture *texture)
{
    if (!texture || texture->handle <= 0)
        return;
    pix_t *pixels = core_get_data_pinned(texture->handle);
    stock_draw_quad(LCD_BUF, pose, pixels, texture->width, texture->height, texture->height);
    if (texture->reflected && !stock_back_face(pose))
        stock_draw_reflection(LCD_BUF, pose,
                              pixels + texture->width * texture->height,
                              texture->width);
    core_put_data_pinned(pixels);
}

/* A turned cover on screen is no texture: take the WPS's own cover, which
 * the return turn starts from. */
static void capture_turned_art(void)
{
    drop_playback_bitmap(&playback_scene.art.handle);
    int side = native_wps_art_side();
    int handle = side ? core_alloc_free_space(side * side * sizeof(pix_t)) : 0;
    if (handle <= 0)
        return;
    pix_t *dst = core_get_data_pinned(handle);
    native_wps_copy_art(dst, side);
    core_put_data_pinned(dst);
    playback_scene.art = (struct playback_texture){0, handle, side, side, false};
}

static void paint_return_landing(const struct ui_rect *clip)
{
    const struct playback_texture *art = &playback_scene.art;
    const pix_t *src = core_get_data_pinned(art->handle);
    stock_draw_clipped_quad(LCD_BUF, playback_scene.playing, src,
                            art->width, art->height, art->height, clip);
    core_put_data_pinned((void *)src);
}

/* The WPS borrows the same texture as the last turn frame. */
static void paint_playback_landing(const struct ui_rect *clip)
{
    const struct playback_texture *cover = playback_cover(playback_scene.album);
    if (cover)
    {
        const pix_t *src = core_get_data_pinned(cover->handle);
        stock_draw_clipped_quad(LCD_BUF, playback_scene.playing, src,
                                cover->width, cover->height, cover->height, clip);
        core_put_data_pinned((void *)src);
    }
}

static void paint_playback_cover(struct stock_pose pose, int album)
{
    const struct playback_texture *saved = playback_cover(album);
    if (saved || !playback_scene.returning)
    {
        paint_playback_texture(pose, saved);
        return;
    }
    struct slide_data slide = {.slide_index = album, .cached_slot = -1};
    const struct dim *bmp = surface(&slide);
    if (bmp)
    {
        int side = MIN(bmp->width, bmp->height);
        const pix_t *crop = (const pix_t *)(bmp + 1) +
            (bmp->width - side) / 2 * bmp->height + (bmp->height - side) / 2;
        stock_draw_quad(LCD_BUF, pose, crop, side, side, bmp->height);
        if (stock_look())
            stock_draw_reflection(LCD_BUF, pose, slide_reflection(bmp), side);
    }
}

#define PLAYBACK_SLIDE_US 270000u

static void paint_playback_row(struct stock_flow *walls, struct stock_pose centre, uint32_t now)
{
    int n = stock_sample(walls, now, flow_visible);
    memset16(LCD_BUF + pf_chrome_h * LCD_WIDTH, playback_scene.bg,
             LCD_WIDTH * (LCD_HEIGHT - pf_chrome_h));
    for (int i = 0; i < n; i++)
        if (flow_visible[i].album != playback_scene.album)
            paint_playback_cover(flow_visible[i].pose, flow_visible[i].album);
    struct playback_texture back = {0, panel.back_handle, 256, 256, false};
    if (stock_back_face(centre))
        paint_playback_texture(centre, &back);
    else if (playback_scene.art.handle > 0)
        paint_playback_texture(centre, &playback_scene.art);
    else
        paint_playback_cover(centre, playback_scene.album);
}

/* Under the stock look Now Playing's status bar, text and controls fade in
 * over the turn to it and out over the turn back, in no fewer refreshes
 * than this; the cover turns as it does without them. */
#define CHROME_FADE_FRAMES 10
#define CHROME_FRAME (LCD_WIDTH * LCD_HEIGHT)

/* `weight` 32nds of the way from `from` to `to`. */
static inline pix_t chrome_blend(pix_t to, pix_t from, unsigned weight)
{
    const uint32_t mask = 0x07e0f81f;
    uint32_t f = (to | ((uint32_t)to << 16)) & mask;
    uint32_t b = (from | ((uint32_t)from << 16)) & mask;
    uint32_t c = (b + (((f - b) * weight) >> 5)) & mask;
    return c | (c >> 16);
}

/* How far in, of 32, a fade begun at `start` and lasting `length` is. */
static unsigned chrome_weight(uint32_t now, uint32_t start, uint32_t length)
{
    return MIN(32u, (now - start) * 32 / MAX(length, 1u));
}

/* Keep the status bar the turn to Now Playing leaves, for the turn back
 * too, and room for the frame the chrome is composed over. */
static bool chrome_fade_open(void)
{
    size_t rows = pf_chrome_h * LCD_WIDTH;
    playback_scene.bar = core_alloc_free_space(rows * sizeof(pix_t));
    playback_scene.chrome = core_alloc_free_space(CHROME_FRAME * sizeof(pix_t));
    if (playback_scene.bar <= 0 || playback_scene.chrome <= 0)
    {
        drop_playback_bitmap(&playback_scene.bar);
        drop_playback_bitmap(&playback_scene.chrome);
        return false;
    }
    pix_t *bar = core_get_data_pinned(playback_scene.bar);
    memcpy(bar, LCD_BUF, rows * sizeof(pix_t));
    core_put_data_pinned(bar);
    return true;
}

/* Put back the status bar Cover Flow showed, or the background if none
 * was kept. */
static void chrome_restore_bar(void)
{
    size_t rows = pf_chrome_h * LCD_WIDTH;
    if (playback_scene.bar <= 0)
    {
        memset16(LCD_BUF, playback_scene.bg, rows);
        return;
    }
    const pix_t *bar = core_get_data_pinned(playback_scene.bar);
    memcpy(LCD_BUF, bar, rows * sizeof(pix_t));
    core_put_data_pinned((void *)bar);
}

/* Now Playing's chrome over the turn's frame, `weight` 32nds of the way in
 * from the status bar the turn left. */
static void chrome_fade_in(unsigned weight)
{
    chrome_restore_bar();
    if (!weight)
        return;
    pix_t *under = core_get_data_pinned(playback_scene.chrome);
    memcpy(under, LCD_BUF, CHROME_FRAME * sizeof(pix_t));
    native_wps_paint_chrome();
    for (int i = 0; i < CHROME_FRAME; i++)
        if (LCD_BUF[i] != under[i])
            LCD_BUF[i] = chrome_blend(LCD_BUF[i], under[i], weight);
    core_put_data_pinned(under);
}

/* Keep the page's chrome drawn over its background alone, and which of its
 * pixels differ from that background. The screen is left as it was. */
static bool chrome_fade_capture(void)
{
    int handle = core_alloc_free_space(CHROME_FRAME * sizeof(pix_t) +
                                       CHROME_FRAME / 8);
    if (handle <= 0)
        return false;
    pix_t *chrome = core_get_data_pinned(handle);
    uint8_t *mask = (uint8_t *)(chrome + CHROME_FRAME);
    memcpy(chrome, LCD_BUF, CHROME_FRAME * sizeof(pix_t));
    memset16(LCD_BUF, playback_scene.bg, CHROME_FRAME);
    native_wps_paint_chrome();
    memset(mask, 0, CHROME_FRAME / 8);
    for (int i = 0; i < CHROME_FRAME; i++)
    {
        pix_t drawn = LCD_BUF[i];
        LCD_BUF[i] = chrome[i];
        chrome[i] = drawn;
        if (drawn != playback_scene.bg)
            mask[i >> 3] |= 1 << (i & 7);
    }
    core_put_data_pinned(chrome);
    playback_scene.chrome = handle;
    return true;
}

/* The page's chrome over the turn back's frame, `weight` 32nds of it left,
 * over the status bar Cover Flow showed. */
static void chrome_fade_out(unsigned weight)
{
    chrome_restore_bar();
    if (!weight)
        return;
    const pix_t *chrome = core_get_data_pinned(playback_scene.chrome);
    const uint8_t *mask = (const uint8_t *)(chrome + CHROME_FRAME);
    for (int i = 0; i < CHROME_FRAME; i++)
        if (mask[i >> 3] & (1 << (i & 7)))
            LCD_BUF[i] = chrome_blend(chrome[i], LCD_BUF[i], weight);
    core_put_data_pinned((void *)chrome);
}

/* The WPS moves one screen width over the stationary landing. */
static void slide_playback_controls(bool incoming, uint32_t start,
                                    uint32_t pace_start, unsigned trace)
{
    if (!native_wps_active() || !lcd_active())
        return;
    uint32_t elapsed = incoming ? MIN((uint32_t)USEC_TIMER - start,
                                     PLAYBACK_SLIDE_US) : 0;
    for (;;)
    {
        unsigned remaining = incoming ? PLAYBACK_SLIDE_US - elapsed : elapsed;
        unsigned t = remaining * 1024 / PLAYBACK_SLIDE_US;
        int hidden = (LCD_WIDTH * t * t + (1u << 19)) / (1u << 20);
        uint32_t rendering = trace ? USEC_TIMER : 0;
        if (incoming && elapsed == PLAYBACK_SLIDE_US)
            native_wps_paint_prepared();
        else
        {
            if (!incoming && elapsed && playback_scene.slot.width && hidden < LCD_WIDTH)
                native_wps_pick_up_art();
            native_wps_paint_slide(hidden, playback_scene.bg);
        }
        uint32_t submitting = trace ? USEC_TIMER : 0;
        lcd_update();
        if (trace)
        {
            ui_perf_note_render(submitting - rendering);
            ui_perf_note_submit((uint32_t)USEC_TIMER - submitting);
        }
        if (elapsed == PLAYBACK_SLIDE_US || !lcd_active())
            break;
        /* The stock look's slide starts from its first frame's refresh. */
        if (!elapsed && stock_look())
            pace_start += stock_rebase(&start);
        if (incoming)
            ui_clock_sleep_until(flow_pace_next(&wps_pace, pace_start));
        else
            ui_clock_sleep(ui_clock_wait_until(flow_pace_next(&flow_pace, pace_start)));
        elapsed = MIN((uint32_t)USEC_TIMER - start, PLAYBACK_SLIDE_US);
    }
}

void carousel_wps_capture_return(uint32_t action_us)
{
    if (flat_view())
        return;
    playback_scene.returning = true;
    native_wps_art_layout(&playback_scene.slot);
    playback_scene.playing = playback_target(playback_scene.slot);
    bool turned = playback_scene.slot.width && native_wps_art_turned();
    if (turned && pf_motion_scale && lcd_active())
        capture_turned_art();
    /* The uncovered cover is the return turn's first frame. */
    if (turned && playback_scene.art.handle > 0)
        native_wps_set_landing(paint_return_landing);
    bool fade = stock_look() && pf_motion_scale && lcd_active() &&
                native_wps_active() && chrome_fade_capture();
    if (pf_motion_scale && !fade)
        slide_playback_controls(false, action_us, action_us, 0);
    playback_scene.return_start = USEC_TIMER;
    if (!turned && pf_motion_scale && lcd_active())
        capture_return_art();
}

static struct stock_flip playback_turn(void)
{
    struct stock_flip flip;
    stock_flip_start(&flip, playback_scene.from_panel ? STOCK_PANEL_TO_PLAYING :
                     STOCK_COVER_TO_PLAYING, playback_scene.centre,
                     playback_scene.walls.start_us);
    flip.target = playback_scene.playing;
    return flip;
}

void carousel_wps_note_playlist(uint32_t elapsed)
{
    playback_scene.playlist_us = elapsed;
}

void carousel_wps_present(const struct wps_state *state)
{
    playback_scene.pending = false;
    if (!lcd_active())
        return;

    bool animate = pf_motion_scale && native_wps_active();
    const char *name = playback_scene.from_panel ? "cover-panel-wps" : "cover-flow-wps";
    uint32_t setup = USEC_TIMER;
    cpu_boost(true);
    if (!animate)
    {
        unsigned trace = ui_perf_begin(name);
        native_wps_render(state, true);
        ui_perf_end(trace);
        cpu_boost(false);
        return;
    }

    bool fade = stock_look() && chrome_fade_open();
    native_wps_paint_handoff(state, &playback_scene.source);
    const char *bitmap_state = native_wps_has_art() ? "ready" : "not-painted";
    char route = playback_scene.from_panel ? 'C' : 'D';

    struct ui_rect measured;
    const char *reason = playback_scene.slot.width ? "configured-slot" :
                                                     "theme-no-art-slot";
    if (native_wps_art_layout(&measured))
    {
        playback_scene.slot = measured;
        playback_scene.playing = playback_target(measured);
        reason = measured.width ? "page-slot" : "theme-no-art-slot";
    }
    unsigned trace = ui_perf_begin(name);
    /* The playlist and the first WPS paint can both touch cold storage. The
     * complete turn starts after both, for all themes. */
    playback_scene.walls.start_us = ui_clock_start();
    struct stock_flip flip = playback_turn();
    uint32_t start = flip.start_us, now = USEC_TIMER;
    memset(&wps_pace, 0, sizeof(wps_pace));
    flow_pace_next(&wps_pace, start);
    uint32_t turn_us = flip.delay + flip.duration;
    uint32_t fade_us = MAX(turn_us - MIN(now - start, turn_us),
                           CHROME_FADE_FRAMES * ui_frame_period());
    uint32_t fade_start = now;
    uint32_t setup_us = now - setup;
    for (;;)
    {
        uint32_t rendering = trace ? USEC_TIMER : 0;
        bool landed = stock_flip_done(&flip, now);
        bool fading = fade && native_wps_active();
        unsigned weight = fading ? chrome_weight(now, fade_start, fade_us) : 0;
        landed &= !fading || weight == 32;
        if (fading && landed)
        {
            if (playback_cover(playback_scene.album))
                native_wps_set_landing(paint_playback_landing);
            native_wps_paint_prepared();
        }
        else
        {
            paint_playback_row(&playback_scene.walls,
                               stock_flip_pose(&flip, now), now);
            if (fading)
                chrome_fade_in(weight);
        }
        uint32_t submitting = trace ? USEC_TIMER : 0;
        lcd_update();
        if (trace)
        {
            ui_perf_note_render(submitting - rendering);
            ui_perf_note_submit((uint32_t)USEC_TIMER - submitting);
        }
        if (landed || !lcd_active())
            break;
        ui_clock_sleep_until(flow_pace_next(&wps_pace, start));
        now = USEC_TIMER;
    }
    if (!fade && native_wps_active() && lcd_active())
    {
        uint32_t landed_at = USEC_TIMER;
        if (playback_cover(playback_scene.album))
            native_wps_set_landing(paint_playback_landing);
        /* Keep the slide on the turn's presentation clock. */
        ui_clock_sleep_until(flow_pace_next(&wps_pace, start));
        slide_playback_controls(true, landed_at, start, trace);
    }
    drop_playback_bitmap(&playback_scene.chrome);
    cpu_boost(false);
    ui_perf_end(trace);
    ui_perf_wps_target(route, &playback_scene.slot, reason, bitmap_state,
                       setup - playback_scene.action_us, playback_scene.playlist_us,
                       setup_us);
}

/* Rebuild the back from the live panel after playback has changed the theme
 * colours or the playing-track marker. */
static void refresh_return_panel(void)
{
    lcd_set_viewport(&pf_vp);
    pf_update_dynamic_colors();
    lcd_set_background(pf_bg_color);
    lcd_set_foreground(pf_fg_color);
    lcd_set_drawmode(DRMODE_FG);
    font_prefetch_begin();
    panel_read_tracks();
    prepare_stock_back();
    font_prefetch_end();
    panel.blur_generation = album_blur_generation();
}

/* A new key ends the turn back; the release of the one that began it
 * does not. */
static bool return_interrupted(void)
{
#if (defined(HAVE_USB_PODLINK) || (defined(SIMULATOR) && defined(IPOD_6G))) \
    && !defined(BOOTLOADER)
    return button_queue_wait_pending(TIMEOUT_NOBLOCK);
#else
    return !button_queue_empty();
#endif
}

static void return_from_playback(void)
{
    bool to_panel = playback_scene.from_panel && panel.live;
    playback_scene.album = center_index;
    playback_scene.bg = pf_bg_color;
    struct stock_flip flip;
    if (to_panel)
    {
        panel.resuming = false;
        panel_cache_pump();
        album_blur_request_window(&panel.blur_source, 1);
        panel.back_ready = false;
        flow_row = playback_scene.walls;
    }
    uint32_t start = playback_scene.return_start, now = USEC_TIMER;
    if (to_panel)
    {
        stock_reverse_walls(&flow_row, start);
        stock_flip_start(&flip, STOCK_PLAYING_TO_PANEL, playback_scene.playing, start);
    }
    else
    {
        stock_enter(&flow_row, number_of_slides, center_index, start, true);
        stock_flip_start(&flip, STOCK_FROM_PLAYING, playback_scene.playing, start);
        flip.start = playback_scene.playing;
    }
    show_carousel_entry();
    uint32_t fade_us = MAX(flip.delay + flip.duration,
                           CHROME_FADE_FRAMES * ui_frame_period());
    if (pf_motion_scale && lcd_active() && flow_row.count)
    {
        for (;;)
        {
            bool landed = stock_flip_done(&flip, now) && !stock_animating(&flow_row, now);
            unsigned weight = playback_scene.chrome > 0 ?
                              32 - chrome_weight(now, start, fade_us) : 0;
            landed = landed && !weight;
            struct stock_pose current = stock_flip_pose(&flip, now);
            if (to_panel && stock_back_face(current) &&
                (!panel.back_ready || (!panel.have_blur &&
                   panel.blur_generation != album_blur_generation())))
                refresh_return_panel();
            if (landed && to_panel)
            {
                /* BACK has native panel scale. Use the live painter for
                 * this endpoint, including the current playing marker. */
                set_current_slide(center_index);
                draw_album_flip(false);
            }
            else
            {
                buf_ctx_lock();
                paint_playback_row(&flow_row, current, now);
                buf_ctx_unlock();
                if (!to_panel)
                    model->draw_text();
            }
            if (playback_scene.chrome > 0)
                chrome_fade_out(weight);
            if (landed)
            {
                lcd_update();
                break;
            }
            lcd_update();
            ui_clock_sleep(ui_clock_wait_until(flow_pace_next(&flow_pace, start)));
            now = USEC_TIMER;
            if (!lcd_active() || return_interrupted())
                break;
        }
    }
    set_current_slide(center_index);
    if (to_panel)
    {
        panel.next_read = current_tick;
        panel.dirty = true;
    }
    release_playback_scene();
}

/* Retain the covers and back before releasing the carousel scratch arena. */
static void prepare_playback_turn(int album, bool from_panel, uint32_t action_us)
{
    native_wps_art_layout(&playback_scene.slot);
    playback_scene.playing = playback_target(playback_scene.slot);
    playback_scene.album = album;
    playback_scene.source = panel_blur_source(album);
    playback_scene.bg = pf_bg_color;
    playback_scene.centre = stock_current(&flow_row, album, action_us,
                                         stock_scroll_progress(&flow_row, action_us)).pose;
    playback_scene.walls = flow_row;
    stock_leave_to_playing(&playback_scene.walls, action_us);
    buf_ctx_lock();
    keep_playback_cover(album);
    for (int i = 0; i < playback_scene.walls.length; i++)
        keep_playback_cover(playback_scene.walls.first + i);
    if (from_panel)
        prepare_stock_back();
    buf_ctx_unlock();
}

static int stock_to_wps(int album, bool from_panel, int destination, uint32_t action_us)
{
    if (destination != CAROUSEL_PLAY_ALBUM && destination != GO_TO_WPS &&
        destination != CAROUSEL_PLAY_PENDING)
        return destination;
    if (flat_view() && destination != CAROUSEL_PLAY_PENDING)
        return destination;
    release_playback_scene();
    playback_scene.from_panel = from_panel;
    playback_scene.action_us = action_us;
    while (destination == CAROUSEL_PLAY_PENDING)
    {
        int status = model->panel_play_status();
        if (status > 0)
            destination = GO_TO_WPS;
        else if (status < 0)
        {
            if (from_panel)
                panel.play_failed = panel.dirty = true;
            release_playback_scene();
            return CAROUSEL_MENU_STAY;
        }
        else if (default_event_handler(get_custom_action(CONTEXT_PLUGIN,
                     MAX(1, HZ / 20), get_context_map)) == SYS_USB_CONNECTED)
        {
            release_playback_scene();
            return GO_TO_ROOT;
        }
    }
    if (pf_motion_scale && lcd_active() && !flat_view())
        prepare_playback_turn(album, from_panel, USEC_TIMER);
    move_center(album);
    playback_scene.pending = true;
    return destination;
}

static int album_covers_loop(unsigned *trace, bool restored_pixels)
{
    int ret;
    int button;
    unsigned art_seen_generation = art_cache_generation();
    unsigned palette_seen_generation = album_palette_generation();
    struct carousel_frame paint;
    carousel_frame_init(&paint);
    caption_clears_itself = false;
    caption_artist.valid = false;
    /* What the screen owes, split so that the two things that change on their
     * own account can be repaired on their own account. The covers are the
     * expensive half by a wide margin, and while a track plays most frames want
     * nothing to do with them: the status bar ticks its clock about seven times
     * a second, and a caption too long for its box steps sideways at the scroll
     * rate. Neither is a reason to clear the viewport, re-render every slide
     * and flush the screen, which is what one shared "redraw" flag costs.
     *
     * All three are sticky: the frame rate cap can defer a frame to a later
     * time round the loop, and a reason to draw must survive until one draws. */
    bool art_owed = true;
    bool caption_owed = true;
    /* The slide named by the caption on screen, or by the one owed. */
    int captioned_index = -1;
    unsigned int seen_caption_version = 0;
    unsigned glyph_epoch = font_prefetch_epoch();
    bool full_flush_owed = false;
    int dirty_top = LCD_HEIGHT, dirty_end = 0;
    int dirty_left = LCD_WIDTH, dirty_right = 0;
    /* The caption's rows, painted and not yet sent. */
    bool caption_flush_owed = false;
    int caption_flush_top = 0, caption_flush_end = 0;
    int caption_left = 0, caption_right = LCD_WIDTH;
    int caption_pixels = 0, deferred_frames = 0, deferred_rows = 0;
    uint32_t caption_due = 0;
    bool caption_wake = false;
    struct ui_perf_transfers transfers = {0};
    previous_ink = (struct caption_span){0, LCD_WIDTH};
    bool trace_attempted = false;
    bool profile_input = ui_perf_enabled();
    unsigned target_changes = 0;
    /* The viewport as a whole has changed and every row of it has to be cleared
     * and sent, not just the rows the two halves above account for. True for
     * the first frame, for anything that drew over the screen and did not say
     * so, and for a change of background colour. */
    bool full_repaint_owed = true;
    pix_t last_bg = pf_bg_color;
    uint32_t frame_clock = USEC_TIMER;
    /* The next frame's deadline: from flow_pace while the covers move, and
     * otherwise the next refresh, so other changes draw no more than once a
     * refresh. flow_start is when the motion began. */
    uint32_t flow_due = frame_clock, flow_start = frame_clock;
    bool flow_moving = false, leaving = false;
    struct ui_scene *entry_source = &pf_scene;
    struct ui_scene *fade = NULL;
    uint32_t fade_start = 0;
    unsigned opacity = 256, leave_from = 256;
    /* How far the rows were from home when the leave began. */
    int leave_rows = MENU_PANE_X;
    /* Fruit Company keeps the covers still on the way in and out, and
     * slides its split menus as stock does; a return that interrupts the
     * entry is `leave_skip` into stock's return. */
    bool still_covers = false, stock_slide = false;
    uint32_t leave_skip = 0;
    pf_caption_hidden = caption_waits = false;
    int leave_result = GO_TO_PREVIOUS;
    unsigned int seen_slides_loaded = pf_slides_loaded;
    long boost_until = current_tick + HZ;
    int spoken_index = -1;
    long speak_due = 0;         /* when the slide at rest is said; 0 none */
    long provider_at = current_tick;
    int remembered_index = -1;
    int blur_index = -1;
    unsigned blur_slide_revision = 0, blur_art_revision = 0, blur_theme_revision = 0;
    bool first_frame = true, panel_waiting = false, panel_settled = false;
    long request_panel_at = current_tick;
    int requested_center = -1;
    long loader_retry = current_tick + HZ;

    while (true) {
        if (!thread_is_running && TIME_AFTER(current_tick, loader_retry) &&
            !usb_host_wants_storage())
        {
            create_pf_thread();
            loader_retry = current_tick + HZ;
        }
        /* An artwork pass finished while the carousel was open. Slides loaded
         * while it ran may have got the placeholder because their thumbnail had
         * not been generated yet, so drop the cached surfaces and let the
         * loader thread fetch them again -- now they exist. Everything goes,
         * the centre slide included: it is the one being looked at, and the
         * loader reloads it first. */
        unsigned art_generation = art_cache_generation();
        if (art_seen_generation != art_generation && !art_cache_is_busy())
        {
            buf_ctx_lock();
            free_all_slide_prio(-1);
            buf_ctx_unlock();
            wake_pf_thread();
            art_owed = true;   /* every slide on screen just went away */
            art_seen_generation = art_generation;
        }

        /* Idle-wake-rate backoff: how long to wait for a button before looking
         * around again. Waking finds nothing to draw most of the time (see the
         * frame test below) and costs almost nothing, so this only decides how
         * promptly the screen notices a change, not how much work it does.
         * Back off to twice a second when it is genuinely static: not animating
         * a scroll, no caption text scrolling, cover cache fully built, no
         * dynamic-colour fade in progress, and playback stopped (while playing,
         * the status bar's time updates every second and a track change can
         * start a colour fade, both of which want the normal rate). A button
         * press interrupts the wait immediately, so responsiveness is
         * unaffected. */
        bool caption_scrolling = scroll_lines[PF_SCROLL_TRACK].step
                              || scroll_lines[PF_SCROLL_ALBUM].step
                              || scroll_lines[PF_SCROLL_ARTIST].step;
        bool quiescent = pf_state != pf_scrolling
                      && !caption_scrolling
                      && !album_palette_needs_repaint()
                      && !(audio_status() & AUDIO_STATUS_PLAY);
        /* With the LCD asleep nothing is drawn or boosted: what is owed
         * stays owed, and the wake's full update shows it. */
        bool dark = !lcd_active();
        int timeout;

        if (restored_pixels)
            timeout = 1;
        else if (!dark && panel.live)
        {
            if (panel.opening)
                timeout = MAX(0, panel.next_read - current_tick);
            else if (panel.moving || (!flat_view() && stock_animating(&flow_row, USEC_TIMER)) ||
                (panel.scroll_frames && panel.position == FLIP_OPEN))
                timeout = frame_wait(panel.due);
            else
            {
                timeout = panel.dirty ? TIMEOUT_NOBLOCK :
                          panel.missing || panel.play_pending || panel.to == FLIP_OPEN
                          ? HZ / 20 : HZ / 5;
                if (panel.marquee_step)
                    timeout = MIN(timeout,
                                  MAX(1, panel.marquee_start - current_tick));
            }
        }
        else if (!dark && (pf_state == pf_scrolling || fade || art_owed ||
                           caption_owed || caption_flush_owed))
        {
            /* There is a frame coming, so wait for its deadline rather than
             * going round again to find it still not due. Zero when it is due
             * or overdue: the poll returns at once and it is drawn. */
            uint32_t due = caption_wake && TIME_BEFORE(caption_due, flow_due) ?
                           caption_due : flow_due;
            timeout = frame_wait(due);
        }
        else
            timeout = (quiescent || dark) ? HZ/2 : HZ/16;

        if (speak_due)
        {
            long left = speak_due - current_tick;
            if (left < timeout || timeout == TIMEOUT_BLOCK)
                timeout = left > 0 ? (int)left : 0;
        }

        if (panel_waiting)
            timeout = timeout == TIMEOUT_BLOCK ? MAX(1, HZ / 25)
                                               : MIN(timeout, MAX(1, HZ / 25));
        if (!dark && !panel.live && bar_drop_waiting())
        {
            int wait = frame_wait(bar_drop_due(USEC_TIMER));
            timeout = timeout == TIMEOUT_BLOCK ? wait : MIN(timeout, wait);
        }
        if (!dark && caption_waits)
        {
            int wait = frame_wait(caption_reveal);
            timeout = timeout == TIMEOUT_BLOCK ? wait : MIN(timeout, wait);
        }
        if (font_prefetch_busy())
            timeout = timeout == TIMEOUT_BLOCK ? 1 : MIN(timeout, 1);
        bool inherited_inhibit = ui_flush_inhibited();
        ui_inhibit_flush(true);
        if (first_frame)
            button = ACTION_NONE;
        else
        {
            int context = CONTEXT_PLUGIN;
            if (!flat_view() && !panel.live && !leaving)
                context |= CONTEXT_WHEEL_DETENTS;
            button = get_custom_action(context, timeout, get_context_map);
        }
        uint32_t action_us = USEC_TIMER;
        first_frame = false;
        ui_inhibit_flush(inherited_inhibit);
        unsigned epoch = font_prefetch_epoch();
        if (epoch != glyph_epoch)
        {
            glyph_epoch = epoch;
            caption_owed = true;
            if (panel.live)
            {
                panel.next_read = current_tick;
                panel.dirty = true;
            }
        }
        /* A hold of Play from now on is the random spin */
        if (!(button_status() & BUTTON_PLAY))
            button_set_sw_poweroff_state(false);

        if (model->tick && (!panel.live || !TIME_BEFORE(current_tick, provider_at)))
        {
            model->tick();
            provider_at = current_tick + 1;
        }
        album_palette_refresh();
        if (model->external)
        {
            int count = model->count();
            if (count == 0) return GO_TO_PREVIOUS;
            if (count > 0 && count != number_of_slides)
            {
                number_of_slides = MIN(count,0x7fff);
                set_current_slide(MIN(center_index,number_of_slides-1));
            }
        }
        /* The index's seeks, and the captions read through them, hold only
         * for the commit they were read in. Checked here, between the wait and
         * anything that uses a seek: after a commit or a rebuild the index is
         * built again and the button dropped; while the tables are down the
         * screen closes, as it would refuse to open. */
        if (!model->external && !db_summary_current(&carousel_idx))
        {
            restored_pixels = false;
            close_album_panel();
            end_pf_thread();
            if (!check_database())
            {
                splash(HZ, tagcache_ram_failed()
                           ? ID2P(LANG_TAGCACHE_RAM_FAILED)
                           : tagcache_ram_refused()
                           ? ID2P(LANG_TAGCACHE_RAM_REFUSED)
                           : ID2P(LANG_TAGCACHE_BUSY));
                return GO_TO_PREVIOUS;
            }
            if (!carousel_reinit())
                return GO_TO_PREVIOUS;
            model->set_initial(NULL);
            if (pf_show_statusbar)
                sb_set_persistent_title(model_title(), Icon_NOICON, SCREEN_MAIN);
            lcd_set_viewport(&pf_vp);
            lcd_set_background(pf_bg_color);
            lcd_set_foreground(pf_fg_color);
            lcd_set_drawmode(DRMODE_FG);
            button = ACTION_NONE;
            art_owed = true;
            caption_owed = true;
            full_repaint_owed = true;
            full_flush_owed = true;
        }

        /* The sheet restored the real pixels. Keep them on LCD while the
         * carousel's released art cache fills again. Input can take over. */
        if (restored_pixels)
        {
            if (!panel.resuming && !dark && button == ACTION_NONE && agents_nav_pending() == AGENTS_NAV_NONE &&
                ui_clock_frame((uint32_t)USEC_TIMER - frame_clock) < ANIM_F(15) &&
                !restored_covers_ready())
                continue;
            restored_pixels = false;
        }

        if (!flat_view() && (pf_entry_prepared || (pf_wps_entry && !pf_shown)))
        {
            show_carousel_entry();
            if (pf_motion_scale && !panel.live)
            {
                fade_start = USEC_TIMER;
                fade = !pf_wps_entry && entry_source->backdrop_handle > 0 ? entry_source : NULL;
                opacity = fade ? 0 : 256;
                still_covers = !pf_wps_entry && stock_look() && fruit_menu_active();
                if (still_covers)
                    stock_init(&flow_row, number_of_slides, center_index,
                               fade_start, false);
                else
                    stock_enter(&flow_row, number_of_slides, center_index,
                                fade_start, pf_wps_entry);
                stock_slide = still_covers && fade && pf_scene_split;
                if (fade && stock_look() && pf_scene_split)
                    bar_drop_begin(fade_start, stock_slide);
                pf_caption_hidden = caption_waits = stock_slide;
                caption_reveal = fade_start + FLOW_STOCK_CHROME_US;
                step = 1;
                pf_state = pf_scrolling;
            }
        }

        if (pf_state == pf_idle && remembered_index != center_index)
        {
            remember_current_cover();
            remembered_index = center_index;
        }

        if (model->panel_request)
        {
            if (pf_state == pf_idle && (requested_center != center_index ||
                !panel_settled || !TIME_BEFORE(current_tick, request_panel_at)))
            {
                panel_request_window();
                requested_center = center_index;
                request_panel_at = current_tick + MAX(1, HZ / 25);
                panel_settled = true;
            }
            else if (pf_state != pf_idle && panel_settled)
            {
                panel_cache_window(NULL, 0);
                album_blur_request_window(NULL, 0);
                blur_index = -1;
                panel_settled = false;
            }
            panel_waiting = panel_cache_pump();
        }

        if (model->panel_request && pf_state == pf_idle &&
            (blur_index != center_index || blur_slide_revision != pf_slides_loaded ||
             blur_art_revision != art_cache_generation() ||
             blur_theme_revision != ui_theme_generation()))
        {
            blur_index = center_index;
            blur_slide_revision = pf_slides_loaded;
            blur_art_revision = art_cache_generation();
            blur_theme_revision = ui_theme_generation();
            struct album_blur_source sources[ALBUM_BLUR_NEIGHBORS] = {0};
            int count = 0;
            sources[count++] = panel_blur_source(center_index);
            if (center_index > 0)
                sources[count++] = panel_blur_source(center_index - 1);
            if (center_index + 1 < number_of_slides)
                sources[count++] = panel_blur_source(center_index + 1);
            album_blur_request_window(sources, count);
            if (panel.live)
                panel.blur_source = sources[0];
        }

        /* Merge status damage with the next cover/caption submission. */
        {
            struct ui_dirty_rect dirty[UI_MAX_DIRTY_RECTS];
            int n = ui_take_dirty_rects(SCREEN_MAIN, dirty);
            int i;

            for (i = 0; i < n; i++)
            {
                if (dirty[i].y < pf_vp_y + pf_chrome_h)
                    bar_drop_take();
                /* The stock look's status bar is no part of the covers'. */
                if (dirty[i].y < pf_vp_y + pf_height
                    && dirty[i].y + dirty[i].h > pf_vp_y + pf_chrome_h)
                {
                    /* It could have painted anywhere in the viewport, not just
                     * over the covers, so the whole of it has to be laid down
                     * again rather than the rows the covers account for. */
                    art_owed = true;
                    full_repaint_owed = true;
                    full_flush_owed = true;
                }
                else
                {
                    dirty_left = MIN(dirty_left, dirty[i].x);
                    dirty_right = MAX(dirty_right, dirty[i].x + dirty[i].w);
                    dirty_top = MIN(dirty_top, dirty[i].y);
                    dirty_end = MAX(dirty_end, dirty[i].y + dirty[i].h);
                }
            }
        }

        /* While the worker prepares the page, Menu cancels the request and
         * the wheel can immediately move on to another album. */
        if (panel.opening)
        {
            switch (button)
            {
            case PF_BACK:
            case PF_QUIT:
                button = ACTION_NONE;
                /* Fall through. */
            case PF_NEXT:
            case PF_NEXT_REPEAT:
            case PF_PREV:
            case PF_PREV_REPEAT:
                close_album_panel();
                break;
            }
        }
        if (panel.live && agents_nav_pending() == AGENTS_NAV_NONE)
        {
            bar_drop_finish();
            int result = panel.opening ? CAROUSEL_MENU_STAY :
                         album_panel_action(button);
            if (result != CAROUSEL_MENU_STAY)
            {
                result = stock_to_wps(center_index, true, result, action_us);
                if (result != CAROUSEL_MENU_STAY)
                    return result;
            }
            if (panel.play_pending)
            {
                int status = model->panel_play_status();
                if (status > 0)
                    return stock_to_wps(center_index, true, GO_TO_WPS, action_us);
                if (status < 0)
                {
                    panel.play_pending = false;
                    panel.play_failed = true;
                    panel.dirty = true;
                }
            }
            if (!dark && (panel.moving || panel.dirty))
                boost_until = current_tick + HZ;
            if (TIME_BEFORE(current_tick, boost_until) != pf_boosted)
            {
                pf_boosted = !pf_boosted;
                cpu_boost(pf_boosted);
            }

            unsigned slides_loaded = pf_slides_loaded;
            if (button == ACTION_REDRAW || art_owed ||
                seen_slides_loaded != slides_loaded)
                panel.dirty = true;
            seen_slides_loaded = slides_loaded;
            art_owed = caption_owed = false;
            font_prefetch_begin();
            step_album_flip();
            font_prefetch_end();
            switch (button)
            {
            case PF_WPS:
            case ACTION_STD_QUICKSCREEN:
            case ACTION_WPS_QUICKSCREEN:
#ifdef HAVE_USB_PODLINK
            case ACTION_AGENTS_REMOTE_SWITCH:
            case ACTION_AGENTS_SWITCH:
            case ACTION_REMOTE_WPS:
            case ACTION_AGENTS_REMOTE_COVERS:
            case ACTION_AGENTS_COVERS:
#endif
                break;
            default:
                if (default_event_handler(button) == SYS_USB_CONNECTED)
                    return GO_TO_ROOT;
                yield();
                continue;
            }
        }

        if (leaving && button != ACTION_NONE)
        {
            if (default_event_handler(button) == SYS_USB_CONNECTED)
                return GO_TO_ROOT;
            return leave_result;
        }

        /* Apply wheel input before sampling the new frame. Other actions
         * dispatch below without waiting for a cover render. */
        int old_target = target;
        switch (button)
        {
        case PF_NEXT:
        case PF_NEXT_REPEAT:
            show_next_slide();
            art_owed = true;
            button = ACTION_NONE;
            break;
        case PF_PREV:
        case PF_PREV_REPEAT:
            show_previous_slide();
            art_owed = true;
            button = ACTION_NONE;
            break;
        }

        if (profile_input && old_target != target && pf_state == pf_scrolling)
            target_changes++;

        /* Said where the row comes to rest, not at every slide it passes:
         * once it has stayed there for TALK_SETTLE. */
        if (pf_state != pf_idle)
        {
            if (speak_due || spoken_index >= 0)
                talk_shutup();
            speak_due = 0;
            spoken_index = -1;
        }
        else if (center_index != spoken_index && model->speak
                 && global_settings.talk_menu)
        {
            if (!speak_due)
                speak_due = current_tick + TALK_SETTLE;
            else if (!TIME_BEFORE(current_tick, speak_due))
            {
                speak_due = 0;
                spoken_index = center_index;
                model->speak(center_index);
            }
        }

        /* Runs every time round, not just on the frames that draw: it is what
         * notices that a caption is due to move. */
        if (update_caption_version(&seen_caption_version))
            caption_owed = true;
        if (update_scroll_lines())
            caption_owed = true;
        /* The caption names where the wheel sends the covers, so a click owes
         * it at the next frame whether the covers draw then or not. */
        if (captioned_index != carousel_caption_index())
        {
            captioned_index = carousel_caption_index();
            caption_owed = true;
        }

        if (caption_waits && (int32_t)(USEC_TIMER - caption_reveal) >= 0)
        {
            pf_caption_hidden = caption_waits = false;
            caption_owed = true;
        }
        if (pf_state == pf_scrolling || fade || bar_drop_owed(USEC_TIMER))
            art_owed = true;

        if (seen_slides_loaded != pf_slides_loaded)
        {
            seen_slides_loaded = pf_slides_loaded;
            art_owed = true;
        }

        /* The caption is drawn in the resolved foreground colour, so a fade
         * moves it as much as it moves the covers -- and the two colours do not
         * have to move together, so the caption cannot wait for a background
         * change to notice. */
        unsigned palette_generation = album_palette_generation();
        if (palette_generation != palette_seen_generation)
        {
            palette_seen_generation = palette_generation;
            art_owed = true;
            caption_owed = true;
        }

        /* Repainting the caption on its own means wiping its rows first, and
         * that is only safe where no cover reached into them. Whether one did
         * is a property of the album on screen rather than of the layout --
         * art keeps its aspect ratio -- so it is measured, by the render that
         * last ran. Where they do meet, the covers have to be redrawn too. */
        if (caption_owed && !art_owed
            && (pf_text_h <= 0
                || (pf_text_y < pf_slides_y1
                    && pf_text_y + pf_text_h > pf_slides_y0)))
            art_owed = true;

        /* Draw only what would come out different, and only at a frame
         * deadline. Re-rendering the covers is far and away the most
         * expensive thing this loop does, and an idle frame pays all of it to
         * arrive at pixels identical to the ones already there -- out of the
         * same budget the codec is drawing on, since the wait above
         * deliberately keeps its normal rate while a track is running.
         *
         * Moving covers present on flow_pace's deadlines: one frame a panel
         * refresh while a frame fits in one, otherwise every second refresh.
         * Any other change draws at the next refresh at most. */
        /* Boosted while there is drawing to do and for a second after, so a
         * caption step or a slide arriving does not switch the clock each
         * time; an idle screen gives the boost back. */
        dark = !lcd_active();
        bool motion = !dark && (pf_state == pf_scrolling || fade);
        if (!motion && trace_attempted)
        {
            ui_perf_end(*trace);
            *trace = 0;
            trace_attempted = false;
        }
        if (!dark && (pf_state == pf_scrolling || art_owed || caption_owed
                      || caption_scrolling))
            boost_until = current_tick + HZ;
        if (TIME_BEFORE(current_tick, boost_until) != pf_boosted)
        {
            pf_boosted = !pf_boosted;
            cpu_boost(pf_boosted);
        }

        if (!motion)
            flow_moving = false;
        bool dispatch_first = button != ACTION_NONE ||
                              agents_nav_pending() != AGENTS_NAV_NONE;
        if (full_repaint_owed)
        {
            /* A menu or theme change can replace the saved caption pixels.
             * The full viewport redraw publishes their replacement. */
            caption_flush_owed = false;
            caption_pixels = deferred_frames = 0;
            caption_wake = false;
        }
        if (caption_wake && frame_due(caption_due))
            caption_wake = false;
        bool finish_caption = caption_owed && caption_flush_owed && caption_pixels >=
                              (caption_right - caption_left) * (caption_flush_end - caption_flush_top);
        if (!fade && motion && !flat_view() && !stock_look() && !dispatch_first &&
            caption_flush_owed &&
            caption_pixels > 0 && (!caption_stage_busy(NULL) ||
            ((finish_caption || deferred_frames >= 2) && frame_due(flow_due))))
        {
            /* Use the previous art band's spare pixels before painting over
             * its caption. Finish the old title if it fits. Otherwise
             * a sustained queue may defer it for one art frame. */
            deferred_rows += stage_caption(caption_left, caption_right, &caption_flush_top,
                                           caption_flush_end, caption_pixels,
                                           *trace ? &transfers : NULL);
            caption_flush_owed = caption_flush_top < caption_flush_end;
            caption_pixels = 0;
            caption_wake = false;
            deferred_frames = 0;
        }
        if (!fade && !dark && caption_flush_owed && (!motion || flat_view()) && frame_due(flow_due) &&
            !dispatch_first)
        {
            /* The caption painted with the last covers, on its own deadline.
             * The covers wait for the next one. */
            int rows = caption_flush_end - caption_flush_top;
            uint32_t submitting = *trace ? USEC_TIMER : 0;
            lcd_update_rect(caption_left, caption_flush_top, caption_right - caption_left, rows);
            caption_flush_owed = false;
            caption_pixels = deferred_frames = deferred_rows = 0;
            caption_wake = false;
            transfers = (struct ui_perf_transfers){0};
            if (*trace)
            {
                uint32_t submitted = USEC_TIMER;
                ui_perf_note_rects(UI_PERF_CAPTION, 0, rows);
                ui_perf_note_submit(submitted - submitting);
            }
            flow_due = flow_moving ? flow_pace_next(&flow_pace, flow_start)
                                   : ui_clock_start() + ui_frame_period();
            yield();
        }
        else if (!dark && (art_owed || caption_owed) && frame_due(flow_due) &&
                 !dispatch_first)
        {
            show_carousel_entry();
            if (fade)
                full_flush_owed = true;
            if (motion && !trace_attempted)
            {
                transfers = (struct ui_perf_transfers){0};
                deferred_rows = 0;
                *trace = ui_perf_begin(model->motion_trace ?: "cover-flow");
                trace_attempted = true;
            }
            bool motion_begins = motion && !flow_moving;
            if (motion_begins)
            {
                flow_start = ui_clock_start();
                flow_moving = true;
            }
            bool drew_art = art_owed;
            /* Keep the framebuffer caption stable until its last slice reaches
             * the panel. Rapid selection changes otherwise rewind the slice
             * cursor to the title and starve the artist. */
            bool drew_caption = caption_owed && (!caption_flush_owed || fade);
            /* The rows each half touches, and so the rows it has to clear
             * before it draws and send afterwards. Nothing else in the viewport
             * has changed since it was last flushed. */
            int art_y = 0, art_h = 0;
            bool joined;
            uint32_t lock_start = *trace ? USEC_TIMER : 0;
            buf_ctx_lock();
            uint32_t frame_start = *trace ? USEC_TIMER : 0;
            uint32_t stages[UI_COVER_STAGE_COUNT];
            if (*trace)
            {
                memset(stages, 0, sizeof(stages));
                if (!flat_view())
                    flow_stages = stages;
            }

            art_owed = false;
            caption_owed &= !drew_caption;
#if PF_SHOW_STATS
            unsigned long t_start = USEC_TIMER, t_flush;
            /* The overlay sits over the covers rather than in them, so it needs
             * the whole viewport in play to have anywhere to go. */
            full_repaint_owed = true;
#endif

            lcd_set_viewport(&pf_vp);
            lcd_set_drawmode(DRMODE_FG);

            pf_update_dynamic_colors();
            /* A fade repaints the ground itself, so every row of the viewport
             * comes out different whether anything was drawn on it or not. */
            if (pf_bg_color != last_bg)
            {
                last_bg = pf_bg_color;
                full_repaint_owed = true;
            }

            if (drew_art)
            {
                if (pf_state == pf_scrolling)
                {
                    update_scroll_animation();
                    /* Publish the complete framebuffer once on the final pose. */
                    if (pf_state == pf_idle && !leaving)
                    {
                        full_flush_owed = true;
                        drew_caption |= caption_owed || captioned_index != carousel_caption_index();
                        caption_owed = false;
                        captioned_index = carousel_caption_index();
                    }
                }
                cover_rows(&art_y, &art_h);
                art_h -= art_y;
                if (art_h <= 0)     /* no slide has a bitmap yet: nothing to
                                     * clear, draw or send */
                    drew_art = false;
            }

            /* Whether the covers and the caption have to be handled as one
             * region. They normally do not touch -- the caption is placed below
             * the covers, or above them -- but how far down a cover reaches is
             * a property of the album, so where they do meet the clear has to
             * take both at once, or the second would wipe the first. */
            joined = full_repaint_owed
                  || (drew_art && pf_text_h > 0
                      && pf_text_y < art_y + art_h
                      && pf_text_y + pf_text_h > art_y);

            /* Joined only by an overlap, the clear repaints the same ground
             * outside the covers' rows and the caption's, so only those rows
             * are sent. */
            int paint_y = art_y, paint_h = art_h;
            if (joined)
            {
                paint_y = 0;
                paint_h = pf_height;
                drew_art = true;
                drew_caption = (pf_text_h > 0);
                if (full_repaint_owed)
                {
                    art_y = paint_y;
                    art_h = paint_h;
                }
            }

            paint.tracing = *trace != 0;
            paint.caption_us = 0;
            int ink_left = 0, ink_right = LCD_WIDTH;
            struct flow_damage damage = { LCD_HEIGHT, 0 };
            if (drew_art && motion && !flat_view() && !joined && !full_flush_owed)
                flow_damage = &damage;
            carousel_paint(&paint, drew_art, drew_caption, joined,
                           paint_y, paint_h);
            if (flow_damage)
            {
                flow_damage = NULL;
                art_y = damage.top;
                art_h = MAX(0, damage.end - damage.top);
            }
            if (drew_caption && !flat_view())
            {
                int left = painted_ink.left, right = painted_ink.right;
                if (!painted_ink_known || joined || lcd_get_backdrop())
                    caption_ink_bounds(&paint, pf_text_y, pf_text_y + pf_text_h,
                                       &left, &right);
                if (!joined)
                {
                    ink_left = MIN(left, previous_ink.left);
                    ink_right = MAX(right, previous_ink.right);
                }
                previous_ink.left = left;
                previous_ink.right = right;
            }
            else if (drew_caption)
            {
                previous_ink.left = 0;
                previous_ink.right = LCD_WIDTH;
            }
            uint32_t render = *trace ? (uint32_t)USEC_TIMER - frame_start : 0;
#if PF_SHOW_STATS
            pf_stats_draw();
            t_flush = USEC_TIMER;
#endif

            flow_stages = NULL;
            buf_ctx_unlock();
            if (*trace)
            {
                uint32_t measured = paint.caption_us;
                for (unsigned i = 0; i < UI_COVER_OTHER; i++)
                    measured += stages[i];
                stages[UI_COVER_OTHER] = render - measured;
                ui_perf_note_cover_work(target_changes, frame_start - lock_start,
                                        paint.caption_us, stages);
            }
            target_changes = 0;

            /* Art uses a contiguous full-width DMA. Caption damage keeps its
             * own bounds until its last row has been staged. */
            if (drew_art && art_h > 0)
            {
                dirty_top = MIN(dirty_top, pf_vp_y + art_y);
                dirty_end = MAX(dirty_end, pf_vp_y + art_y + art_h);
            }
            if (drew_caption && pf_text_h > 0 && caption_rows_h > 0 && ink_right > ink_left)
            {
                int top = pf_vp_y + caption_rows_y, end = top + caption_rows_h;
                caption_left = caption_flush_owed ? MIN(caption_left, ink_left) : ink_left;
                caption_right = caption_flush_owed ? MAX(caption_right, ink_right) : ink_right;
                caption_flush_top = caption_flush_owed ?
                                    MIN(caption_flush_top, top) : top;
                caption_flush_end = caption_flush_owed ?
                                    MAX(caption_flush_end, end) : end;
                caption_flush_owed = true;
            }
            if (full_flush_owed)
            {
                dirty_top = 0;
                dirty_end = LCD_HEIGHT;
                full_flush_owed = false;
            }
            if (bar_drop_compose(USEC_TIMER))
            {
                dirty_top = 0;
                dirty_end = MAX(dirty_end, pf_vp_y + pf_chrome_h);
            }
            unsigned parts = UI_PERF_ART, caption_rows = 0;
            if (caption_flush_owed)
            {
                int top = caption_flush_top, end = caption_flush_end;
                /* The stock look sends the caption with the covers, so a
                 * frame is one transfer in one refresh. */
                if (stock_look() && dirty_end > dirty_top)
                {
                    dirty_top = MIN(dirty_top, top);
                    dirty_end = MAX(dirty_end, end);
                }
                if (dirty_end <= top || dirty_top >= end)
                    ;
                else if (dirty_top <= top && end <= dirty_end)
                {
                    caption_flush_owed = false;
                    parts |= UI_PERF_CAPTION;
                    caption_rows = end - top;
                }
                else if (dirty_top >= top)
                    dirty_top = MIN(end, dirty_end);
                else
                    dirty_end = top;
                if (dirty_end <= dirty_top && caption_flush_owed)
                {
                    dirty_top = top;
                    dirty_end = end;
                    caption_flush_owed = false;
                    parts = UI_PERF_CAPTION;
                    caption_rows = end - top;
                }
            }
            int rows = MAX(0, dirty_end - dirty_top);
            if (rows)
            {
                uint32_t start = *trace ? USEC_TIMER : 0;
                if (fade)
                {
                    uint32_t age = (uint32_t)USEC_TIMER - fade_start;
                    unsigned duration = stock_slide ?
                        FLOW_STOCK_SLIDE_US - (leaving ? leave_skip : 0) :
                        leaving ? FLOW_LEAVE_FADE_US : FLOW_MENU_FADE_US;
                    unsigned phase = MIN(age, duration) * 256 / duration;
                    unsigned alpha = leaving ? leave_from * (256 - phase) / 256 : phase;
                    bool shown = stock_slide
                        ? flow_present_stock(fade, age + (leaving ? leave_skip : 0),
                                             leaving)
                        : stock_look() && pf_scene_split
                        ? flow_present_split(fade, age, leaving, leave_rows)
                        : flow_present_fade(fade, alpha);
                    if (shown)
                        opacity = alpha;
                    if (phase == 256 && !leaving)
                    {
                        if (opacity != 256)
                            lcd_update();
                        opacity = 256;
                        fade = NULL;
                    }
                }
                else
                    lcd_update_rect(0, dirty_top, LCD_WIDTH, rows);
                if (*trace)
                    ui_perf_add_transfer(&transfers, start, parts & UI_PERF_ART ?
                                         UI_PERF_ART : UI_PERF_CAPTION);
            }
            else
                parts = 0;

            /* The stock look's motion starts from the refresh its first frame
             * finished in, however long that frame took to draw, so the
             * second frame follows it by two refreshes and moves two
             * refreshes' worth. */
            if (motion_begins && stock_look())
            {
                uint32_t late = stock_rebase(&flow_start);
                flow_row.start_us += late;
                fade_start += late;
                bar_drop.start += late;
                caption_reveal += late;
            }
            /* Set the art deadline from the art DMA. A second, narrow DMA can
             * use the remaining bandwidth without reserving another art TE. */
            flow_due = motion ? flow_pace_next(&flow_pace, flow_start)
                              : ui_clock_start() + ui_frame_period();
            caption_pixels = 0;
            caption_wake = false;
            if (motion && !flat_view() && !stock_look() && caption_flush_owed &&
                rows < 150)
            {
                caption_pixels = (150 - rows) * LCD_WIDTH;
                uint32_t slot;
                if (caption_stage_busy(&slot))
                {
                    /* One wake between art deadlines; button wakes and the
                     * next art deadline also get to use the released slot. */
                    uint32_t now = USEC_TIMER;
                    caption_due = TIME_BEFORE(now, slot) ? slot + 100 : now + 500;
                    caption_wake = TIME_BEFORE(caption_due, flow_due);
                    deferred_frames++;
                }
                else
                {
                    deferred_rows += stage_caption(caption_left, caption_right,
                                                   &caption_flush_top, caption_flush_end,
                                                   caption_pixels, *trace ? &transfers : NULL);
                    caption_flush_owed = caption_flush_top < caption_flush_end;
                    caption_pixels = 0;
                    deferred_frames = 0;
                }
            }
            else
                deferred_frames = 0;
            if (deferred_rows)
            {
                caption_rows += deferred_rows;
                parts |= UI_PERF_CAPTION;
                deferred_rows = 0;
            }
            if (*trace && transfers.sampled)
            {
                ui_perf_note_rects(parts, parts & UI_PERF_ART ? rows : 0, caption_rows);
                ui_perf_note_render(render);
                ui_perf_note_transfers(&transfers);
                transfers = (struct ui_perf_transfers){0};
            }
            if (pf_scene.capturing)
                ui_scene_start(&pf_scene);
            first_frame = false;
            dirty_top = LCD_HEIGHT;
            dirty_end = 0;
            dirty_left = LCD_WIDTH;
            dirty_right = 0;
            full_repaint_owed = false;
#if PF_SHOW_STATS
            pf_stats_frame(t_start, t_flush - t_start, USEC_TIMER - t_flush);
#endif
            yield();
        }

        else if (!art_owed && !caption_owed)
        {
            if (!dark && dirty_end > dirty_top && !dispatch_first)
            {
                lcd_update_rect(dirty_left, dirty_top, dirty_right - dirty_left, dirty_end - dirty_top);
                dirty_left = LCD_WIDTH;
                dirty_right = 0;
                dirty_top = LCD_HEIGHT;
                dirty_end = 0;
            }
        }

        /* Whatever the button below is about to change, the frame showing it
         * has not been drawn yet -- this one went out before the switch. */
        if (button != ACTION_NONE)
        {
            art_owed = true;
            /* Only the wheel is on the hot path. Everything else can put
             * something over the whole screen on its way -- the in-screen menu,
             * a splash, the USB screen -- and none of them leave a note, so the
             * next frame repaints the lot rather than the covers' own rows. */
            if (button != PF_NEXT && button != PF_NEXT_REPEAT
                && button != PF_PREV && button != PF_PREV_REPEAT)
                full_repaint_owed = true;
        }

        /* Still covers have no motion of their own to wait for, so the
         * leave lasts as long as the menu's return. */
        if (leaving && (pf_state == pf_idle || dark) &&
            !(still_covers && fade && !dark &&
              (uint32_t)USEC_TIMER - fade_start <
              (stock_slide ? FLOW_STOCK_SLIDE_US - leave_skip
                           : FLOW_LEAVE_FADE_US)))
            return leave_result;

        enum agents_nav_screen nav = agents_nav_pending();
        if (nav == AGENTS_NAV_COVERS) agents_nav_take(nav);
        else if (nav == AGENTS_NAV_BACK)
        {
            agents_nav_take(nav);
            leave_result = CAROUSEL_REMOTE_RETURN;
            button = PF_BACK;
        }
        else if (nav == AGENTS_NAV_AGENTS) return GO_TO_AGENTS;
        else if (nav == AGENTS_NAV_WPS) return GO_TO_WPS;
        switch (button) {
#ifdef HAVE_USB_PODLINK
        case ACTION_AGENTS_REMOTE_SWITCH:
        case ACTION_AGENTS_SWITCH:
            return GO_TO_AGENTS;
        case ACTION_REMOTE_WPS:
            return GO_TO_WPS;
        case ACTION_AGENTS_REMOTE_COVERS:
        case ACTION_AGENTS_COVERS:
            leave_result = CAROUSEL_REMOTE_RETURN;
            goto leave_covers;
#endif
        case PF_WPS:
            if (!flat_view() && !panel.live && model->play_album &&
                !(audio_status() & AUDIO_STATUS_PLAY) && !cider_active())
            {
                int result = stock_to_wps(target, false, model->play_album(target), action_us);
                if (result != CAROUSEL_MENU_STAY)
                    return result;
                art_owed = caption_owed = full_repaint_owed = true;
                break;
            }
            return GO_TO_WPS;
        case PF_QUIT:
        case PF_BACK:
#ifdef HAVE_USB_PODLINK
        leave_covers:
#endif
            if (flat_view() || !pf_motion_scale || dark || leaving)
                return leave_result;
            if (pf_scene.backdrop_handle > 0 && pf_scene_stale())
            {
                /* Join before the theme changes, as for the modal screens. */
                end_pf_thread();
                pf_scene_refresh();
                create_pf_thread();
                lcd_set_viewport(&pf_vp);
                lcd_set_background(pf_bg_color);
                lcd_set_foreground(pf_fg_color);
                lcd_set_drawmode(DRMODE_FG);
            }
            leave_rows = fade && !leaving
                ? flow_split_entered((uint32_t)USEC_TIMER - fade_start)
                : MENU_PANE_X;
            leave_skip = fade && !leaving
                ? FLOW_STOCK_SLIDE_US -
                  MIN((uint32_t)USEC_TIMER - fade_start, FLOW_STOCK_SLIDE_US)
                : 0;
            fade_start = USEC_TIMER;
            leave_from = opacity;
            fade = pf_scene.backdrop_handle > 0 ? &pf_scene : NULL;
            stock_slide = still_covers && fade && pf_scene_split;
            bar_drop.frozen = true;
            if (stock_slide)
            {
                bar_drop_hide();
                pf_caption_hidden = true;
                caption_waits = false;
                caption_owed = true;
            }
            if (!still_covers)
                stock_leave(&flow_row, fade_start);
            step = 1;
            pf_state = pf_scrolling;
            leaving = true;
            break;
        case ACTION_STD_QUICKSCREEN:
        case ACTION_WPS_QUICKSCREEN:
            /* Leave the model, position and surfaces alive under the overlay.
             * Join before the theme changes, as for the other modal screens. */
            ui_perf_end(*trace);
            *trace = 0;
            trace_attempted = false;
            end_pf_thread();
            ret = quick_screen_current(button, NULL);
            if (ret == QUICKSCREEN_IN_USB) return GO_TO_ROOT;
            create_pf_thread();
            if (pf_show_statusbar)
                sb_set_persistent_title(model_title(), Icon_NOICON, SCREEN_MAIN);
            lcd_set_viewport(&pf_vp);
            lcd_set_background(pf_bg_color);
            lcd_set_foreground(pf_fg_color);
            lcd_set_drawmode(DRMODE_FG);
            break;
        case PF_MENU:
            /* The in-screen menu is model-specific: both models open the shared
             * Carousel settings menu but act on different settings afterwards.
             * Optional (on_menu may be NULL). It returns a GO_TO_* to exit, or
             * a CAROUSEL_MENU_* sentinel to stay. */
            if (!model->on_menu)
                break;
            ui_perf_end(*trace);
            *trace = 0;
            trace_attempted = false;
            end_pf_thread();
            ret = model->on_menu();
            if (ret < 0)
                create_pf_thread();
            if (ret >= 0)
                return ret;   /* a GO_TO_* screen code */
            /* Handled in place -- restore the carousel's own status bar/viewport
             * after the menu overlay, and (on a rebuild) its colours. */
            if (pf_show_statusbar)
                sb_set_persistent_title(model_title(), Icon_NOICON, SCREEN_MAIN);
            lcd_set_viewport(&pf_vp);
            if (ret == CAROUSEL_MENU_RELOADED)
            {
                lcd_set_background(pf_bg_color);
                lcd_set_foreground(pf_fg_color);
            }
            lcd_set_drawmode(DRMODE_FG);
            break;

        case PF_SORTING_NEXT:
            end_pf_thread();
            model->sort_next();
            create_pf_thread();
            break;
        case PF_SORTING_PREV:
            end_pf_thread();
            model->sort_prev();
            create_pf_thread();
            break;
        case PF_RANDOM:
            show_random_slide();
            break;
        case PF_JMP:
        {
            int new_idx = model->jump_next();
            if (new_idx != center_index)
            {
                pf_state = pf_idle;
                set_current_slide(new_idx);
            }
            break;
        }
        case PF_JMP_PREV:
        {
            int new_idx = model->jump_prev();
            if (new_idx != center_index)
            {
                pf_state = pf_idle;
                set_current_slide(new_idx);
            }
            break;
        }
        case ACTION_STD_CONTEXT:
            if (!model->on_context)
                break;
            if (pf_state == pf_scrolling)
                set_current_slide(target);
            action_wait_for_release();
            end_pf_thread();
            carousel_forget_resume();
            return model->on_context(center_index);

        case PF_TRACKLIST:
        case PF_SELECT:
        {
            if (model->panel_request)
            {
                /* The album the caption names: the scroll's target. */
                if (pf_state == pf_scrolling)
                {
                    if (flat_view())
                        set_current_slide(target);
                    else
                    {
                        move_center(target);
                        land();
                    }
                    pf_state = pf_idle;
                }
                ui_perf_end(*trace);
                *trace = 0;
                trace_attempted = false;
                open_album_panel(0, 0);
                break;
            }
            /* Settle the animation onto the slide the user is looking at, so
             * center_index is the one they picked, then let the model drill in. */
            if (pf_state == pf_scrolling)
                set_current_slide(target);
            end_pf_thread();
            if (!carousel_save_resume())
            {
                splash(HZ, "Not enough memory to keep covers");
                create_pf_thread();
                break;
            }
            int destination = model->enter(center_index);
            if (destination != GO_TO_ALBUM_COVERS_TRACKS &&
                !(model->external && destination == GO_TO_PREVIOUS))
                carousel_forget_resume();
            return destination;
        }
        default:
            if (default_event_handler(button) == SYS_USB_CONNECTED)
                return GO_TO_ROOT;
            break;
        }
    }
}

/* Run the coverflow carousel over the given model. Both public entry points
 * (album_covers / artist_portraits) are thin wrappers that select the model. */
static int carousel_body(const struct carousel_model *m, const char *selected_file);
int carousel_run(const struct carousel_model *m, const char *selected_file)
{
    if (!agents_nav_enter(AGENTS_NAV_COVERS))
    {
        carousel_cancel_transition();
        if (wps_handover.returning || playback_scene.returning)
            carousel_wps_end(false);
        return GO_TO_PREVIOUS;
    }
    int handle = core_alloc(sizeof(*workspace));
    if (handle <= 0)
    {
        carousel_cancel_transition();
        agents_nav_leave(AGENTS_NAV_COVERS);
        if (wps_handover.returning || playback_scene.returning)
            carousel_wps_end(false);
        return GO_TO_PREVIOUS;
    }
    workspace = core_get_data_pinned(handle);
    pf_shown = false;
    int result = carousel_body(m, selected_file);
    if (pf_wps_entry)
    {
        ui_scene_discard(&wps_handover.sheet);
        pf_wps_entry = false;
    }
    panel_cache_window(NULL, 0);
    album_blur_request_window(NULL, 0);
    carousel_cancel_transition();
    end_pf_thread();
    workspace = NULL;
    core_unpin(handle);
    core_free(handle);
    enum agents_nav_screen nav = agents_nav_pending();
    if (nav == AGENTS_NAV_BACK)
    {
        agents_nav_take(nav);
        result = CAROUSEL_REMOTE_RETURN;
    }
    else if (nav == AGENTS_NAV_AGENTS) result = GO_TO_AGENTS;
    else if (nav == AGENTS_NAV_WPS) result = GO_TO_WPS;
    else if (nav == AGENTS_NAV_COVERS) agents_nav_take(nav);
    if (m->panel_request && (result == GO_TO_WPS || result == CAROUSEL_PLAY_ALBUM))
    {
        wps_handover.model = m;
        wps_handover.returning = false;
        wps_handover.restored_pixels = false;
        wps_handover.index = center_index;
        wps_handover.panel_open = panel.live && panel.to != 0;
        if (wps_handover.panel_open)
        {
            ui_perf_end(panel.trace);
            panel.trace = 0;
            panel.moving = panel.play_pending = panel.play_failed = false;
            panel.scroll_frames = 0;
        }
        else
            close_album_panel();
        if (flat_view())
            ui_scene_capture_modal(&wps_handover.sheet);
        /* The Cover Flow pixels are fullscreen. Its viewport stack has
         * already unwound to Home so that playback can take over. */
        wps_handover.sheet.status_height = 0;
    }
    else
    {
        close_album_panel();
        if (!pf_scene_refresh())
            ui_scene_discard(&pf_scene);
        if (pf_scene.backdrop_handle > 0 && flat_view())
            ui_scene_close_modal(&pf_scene, pf_shown, "covers-down");
        else if (pf_scene.backdrop_handle > 0 && pf_shown &&
                 (result == GO_TO_PREVIOUS || result == CAROUSEL_REMOTE_RETURN))
            ui_scene_restore_modal(&pf_scene);
        else
            ui_scene_discard(&pf_scene);
        carousel_wps_end(false);
    }
    agents_nav_leave(AGENTS_NAV_COVERS);
    return result;
}

static int carousel_body(const struct carousel_model *m, const char *selected_file)
{
    int ret;

    end_pf_thread();
    if (wps_handover.model && (wps_handover.model != m || !wps_handover.returning))
        carousel_wps_end(false);
    if (resume_model && resume_model != m) carousel_forget_resume();
    model = m;

    /* Self-managed (rather than left to root_menu.c's load_screen(), which
     * only wraps the main-menu dispatch path) since this is also reachable
     * directly from apps/gui/wps.c (the "coverflow" WPS select-action and
     * the custom STOP-opens-coverflow behavior), bypassing that dispatcher
     * entirely. Activity tracking covers both entry paths. */
    bool restored_pixels = wps_handover.returning && wps_handover.restored_pixels;
    push_activity_without_refresh(ACTIVITY_ALBUMCOVERS);
    carousel_prepare_transition();

    if (!model->external && !check_database())
    {
        /* Said out loud: the working indicator check_database() raises while
         * it waits stops with it, so without this the screen simply hands
         * back to whatever opened it and the press reads as ignored. A
         * refused RAM copy never loads, so that says why instead. */
        splash(HZ, tagcache_ram_failed()
                   ? ID2P(LANG_TAGCACHE_RAM_FAILED)
                   : tagcache_ram_refused()
                   ? ID2P(LANG_TAGCACHE_RAM_REFUSED)
                   : ID2P(LANG_TAGCACHE_BUSY));
        pop_current_activity();
        return GO_TO_PREVIOUS;
    }

    lcd_set_viewport(NULL);
    lcd_fb = lcd_framebuffer_default.fb_ptr;

    if (!init())
    {
        cleanup();
        pop_current_activity();
        return GO_TO_PREVIOUS;
    }

    /* Set here, before the loop starts, so the status bar carries this
     * screen's title from the moment it opens rather than whatever the
     * previous screen left behind. The PF_MENU case in album_covers_loop()
     * re-sets it on the way back from the in-screen menu. */
    if (pf_show_statusbar)
        sb_set_persistent_title(model_title(), Icon_NOICON, SCREEN_MAIN);

    /* Jump to selected_file's album if one was passed, otherwise the
     * currently-playing track's album, otherwise wherever was last viewed. */
    model->set_initial(selected_file);
    if (wps_handover.returning)
    {
        set_current_slide(MIN(wps_handover.index, number_of_slides - 1));
        if (wps_handover.panel_open)
        {
            static struct panel_request current;
            if (request_panel(center_index, &current) && panel_cache_same(&panel.request, &current))
            {
                panel.position = panel.to = FLIP_OPEN;
                /* The panel's leased pages also survive time spent in Lyrics. */
                panel.resuming = panel.dirty = true;
                panel.next_read = current_tick;
            }
            else
                close_album_panel();
        }
        if (!pf_wps_entry)
            ui_scene_discard(&wps_handover.sheet);
        wps_handover.model = NULL;
        wps_handover.returning = false;
    }
    create_pf_thread();
    if (playback_scene.returning)
    {
        pf_wps_entry = true;
        return_from_playback();
    }
    else if (!wps_handover.model && (playback_scene.pending || playback_scene.from_panel))
        release_playback_scene();

    /* Play held is the random spin here, and held long enough it would power
     * off. Held Play on the playing screen opens this, and that hold still
     * powers off, until Play is let go: see album_covers_loop(). */
    button_set_sw_poweroff_state(button_status() & BUTTON_PLAY);
    unsigned trace = 0;
    cover_resume_dirty = false;
    ret = album_covers_loop(&trace, restored_pixels || pf_entry_prepared);
    wheel_set_detent_mode(false);
    button_set_sw_poweroff_state(true);

    remember_current_cover();
    if (cover_resume_dirty)
        status_save(true);
    cleanup();
    ui_perf_end(trace);

    /* CAROUSEL_PLAY_ALBUM belongs with these: album_covers() turns it into
     * GO_TO_WPS, so the screen underneath is about to be replaced either way
     * and a refresh on the way past would only flash it. So does
     * CAROUSEL_CONTEXT, which a menu replaces. */
    if (ret == GO_TO_WPS || ret == GO_TO_PREVIOUS_MUSIC || ret == GO_TO_PLUGIN
        || ret == CAROUSEL_PLAY_ALBUM || ret == CAROUSEL_CONTEXT
        || (!flat_view() && pf_scene.backdrop_handle > 0 &&
            (ret == GO_TO_PREVIOUS || ret == CAROUSEL_REMOTE_RETURN)))
        pop_current_activity_without_refresh();
    else
        pop_current_activity();

    return ret;
}
