#!/usr/bin/env python3
"""Stage stock PictureFlow with an AstraPod library, cache and worker."""
from hashlib import sha256
from pathlib import Path
import re
import sys

src, output = map(Path, sys.argv[1:])
source = src.read_bytes()
if sha256(source).hexdigest() != "54c770d01e59de524109f3baede448f41588226ca435d7ed68f8daff8c96df1f":
    sys.exit(f"{src}: review PictureFlow adapters for the changed upstream source")
code = source.decode()


def replace(before, after, count=1):
    global code
    if code.count(before) != count:
        raise ValueError(f"{src}: expected {count} occurrence(s) of {before!r}")
    code = code.replace(before, after)


def section(start, end, replacement=""):
    global code
    a = code.index(start)
    b = code.index(end, a + len(start))
    code = code[:a] + replacement + code[b:]


replace('#include "plugin.h"', '#include "debug.h"\n#include "plugin.h"')
section('#include "albumart.h"', "/******************************* Globals", """\
#include "database/db_summary.h"
#include "draw/resize.h"
#include "draw/jpeg_common.h"
#include "files/filetypes.h"
#include "lib/pluginlib_exit.h"
#include "lib/helper.h"
#include "lib/configfile.h"
#include "lib/mylcd.h"
#include "lib/playback_control.h"
#include "pluginbitmaps/pictureflow_emptyslide.h"
#include "pluginbitmaps/pictureflow_logo.h"


""")
section("/*\n *  Targets which use plugin_get_audio_buffer()", "#define PF_PREV",
        "#define PF_PLAYBACK_CAPABLE 1\n\n")
section("#define SLIDE_CACHE_SIZE", "/** structs we use */", """\
#define MAX_SLIDES_COUNT 10
#define CACHE_PREFIX ROCK_PF_CACHE_DIR
#define CONFIG_VERSION 1
#define CONFIG_FILE CACHE_PREFIX "/pictureflow.cfg"

""")
for field in ("     int cache_version;\n", "     bool update_albumart;\n",
              "    size_t borrowed;\n", "    size_t used;\n"):
    replace(field, "")

section("struct pf_index_t", "struct pf_track_t", """\
struct pf_album {
    const struct album_data *row;
    const char *name;
    const char *artist;
};

struct pf_index_t {
    struct db_summary_t summary;
    int album_ct;
    int album_untagged_idx;
    struct pf_album *album_index;
    void *buf;
    size_t buf_sz;
};

""")
section("struct albumart_t", "struct slide_data")
section("struct slide_cache", "struct track_data")
section("\nstruct track_data {", "enum pf_scroll_line_type", """\
struct track_data {
    int disc;
    int track;
    int order;
    int name_idx;
    int filename_idx;
};

""")
section("struct pfraw_header", "enum show_album_name_values")
replace("static struct albumart_t aa_cache;\n", "")
for key in ("cache version", "art cache pos", "art cache inspected", "update albumart"):
    code, count = re.subn(r'^    \{[^\n]*"' + key + r'"[^\n]*\n', "", code,
                          flags=re.MULTILINE)
    assert count == 1
replace("static bool is_initial_slide = true;\n", "")
section("static struct pf_slide_cache pf_sldcache;", "static int cover_animation_keyframe", """\
static struct pf_index_t pf_idx;
static struct pf_track_t pf_tracks;
static size_t pf_track_bytes;
static struct mp3entry id3;

void reset_track_list(void);

""")
section("#if PF_PLAYBACK_CAPABLE\nstatic bool insert_whole_album;", "static void config_set_defaults", """\
static bool insert_whole_album;
static bool pf_art_running(int done, int rows);

""")
replace("     cfg->cache_version = CACHE_REBUILD;\n", "")
replace("     cfg->update_albumart = false;\n", "")
replace("    .get_size = get_size\n", "    .get_size = get_size,\n    .progress = pf_art_running,\n")
replace("""\
    context &= ~CONTEXT_LOCKED;
    return pf_contexts[context & ~CONTEXT_PLUGIN];""", """\
#ifdef BUTTON_RC_VOL_UP
    if (context & CONTEXT_REMOTE)
    {
        static const struct button_mapping remote[] = {
            {PF_MENU, BUTTON_RC_MENU | BUTTON_REPEAT, BUTTON_RC_MENU},
            {PF_QUIT, BUTTON_RC_MENU | BUTTON_REL, BUTTON_NONE},
            {PF_QUIT, BUTTON_RC_MENU | BUTTON_REL | BUTTON_REPEAT, BUTTON_NONE},
            {PF_PREV, BUTTON_RC_VOL_UP, BUTTON_NONE},
            {PF_NEXT, BUTTON_RC_VOL_DOWN, BUTTON_NONE},
            {PF_PREV, BUTTON_RC_VOL_UP | BUTTON_REL | BUTTON_REPEAT, BUTTON_NONE},
            {PF_NEXT, BUTTON_RC_VOL_DOWN | BUTTON_REL | BUTTON_REPEAT, BUTTON_NONE},
            {PF_PREV_REPEAT, BUTTON_RC_VOL_UP | BUTTON_REPEAT, BUTTON_NONE},
            {PF_NEXT_REPEAT, BUTTON_RC_VOL_DOWN | BUTTON_REPEAT, BUTTON_NONE},
            LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD | CONTEXT_REMOTE)
        };
        return remote;
    }
#endif
    return pf_contexts[context & 1];""")
section("static int compare_albums", "static int jmp_idx_prev", """\
#include "pictureflow-library.inc"
#include "pictureflow-cache.inc"

""")
cache_wait = code[code.index("    if (aa_cache.inspected"):code.index("    if (pf_cfg.sort_albums_by == SORT_BY_YEAR)")]
replace(cache_wait, "", count=2)
replace("char *current_selection = get_slide_name", "const char *current_selection = get_slide_name", count=2)
section("        int album_idx, artist_idx;", "\n    }\n    rb->splashf", """\
        for (i = 0; i < pf_idx.album_ct; i++)
            if (!rb->strcmp(get_album_name(i), current_album) &&
                !rb->strcasecmp(get_album_artist(i), current_artist))
                return i;
""")
section("/**\n  Compare two unsigned ints passed via pointers.", "/**\n adjust slides")
section("    int old_center_index = center_index;", "    target = center_index;", """\
    step = 0;
    center_index = fbound(0, slide_index, number_of_slides - 1);
    pf_requested_slide = center_index;
""")
replace("retrieve_id3(&id3, selected_file)", "rb->get_metadata(&id3, -1, selected_file)")
section("    int i, album_idx, artist_idx;", "    set_initial_slide(NULL);", """\
    for (int i = 0; i < pf_idx.album_ct; i++)
    {
        if (hash_album == mfnv(get_album_name(i)) &&
            hash_artist == mfnv(get_album_artist(i)))
        {
            set_current_slide(i);
            pf_cfg.last_album = i;
            return;
        }
    }
""")
section("    /* Only change sorting once artwork", "    return_to_idle_state();")
section("    /* Empty cache and restart cover loading thread */", "\n#ifdef USEGSLIB", """\
    reselect(hash_album, hash_artist);
    if (!create_pf_thread())
        rb->splash(HZ, "Could not restart the PictureFlow worker");

""")
replace("        rb->queue_post(&thread_q, EV_WAKEUP, 0);", "        pf_requested_slide = center_index;")
section("    wants_to_quit = true;", "    /* Turn on backlight", """\
    end_pf_thread();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif

""")
for update in ("false", "true"):
    before = f"""\
                pf_cfg.update_albumart = {update};
                pf_cfg.cache_version = CACHE_REBUILD;
                rb->remove(EMPTY_SLIDE);
                configfile_save(CONFIG_FILE, config,
                                CONFIG_NUM_ITEMS, CONFIG_VERSION);
                return -3; /* re-init */"""
    replace(before, """\
                configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
                return -3; /* re-init */""" if update == "true" else
            "                return -4; /* rebuild */", count=2 if update == "false" else 1)
# Resize switches cache keys. Reopen with the selected setting.
start = code.index("            case 6:")
b = code.index("                return -4; /* rebuild */", start)
code = code[:b] + """\
                configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
                return -3; /* re-init */""" + code[b + len("                return -4; /* rebuild */"):]
replace("if (!rb->yesno_pop_confirm(", "if (!pf_confirm(", count=3)
replace("                playback_control(NULL);", """\
                if (playback_control(NULL))
                    return PLUGIN_USB_CONNECTED;""")
replace("pf_cfg.show_fps || aa_cache.inspected < pf_idx.album_ct", "pf_cfg.show_fps", count=2)
section("static void draw_album_text(void);\nstatic void show_track_list_loading", "/**\n  Display the list of tracks")
section("#ifdef HAVE_TC_RAMCACHE\n", "        create_track_index(center_slide.slide_index);")
section("#if PF_PLAYBACK_CAPABLE\nstatic int show_id3_info", "/**\n   Draw the current album name",
        '#include "pictureflow-ui.inc"\n\n')
replace("    char *albumtxt;\n    char *artisttxt;", "    const char *albumtxt;\n    const char *artisttxt;")
section("/**\n  Display an error message and wait for input.", "/**\n  Main function", """\
static bool init(void)
{
    if (!rb->dir_exists(CACHE_PREFIX) && rb->mkdir(CACHE_PREFIX) < 0)
    {
        rb->splash(HZ, "Could not create the PictureFlow cache");
        return false;
    }

    config_set_defaults(&pf_cfg);
    configfile_load(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
    if (pf_cfg.backlight_mode == 0)
        backlight_ignore_timeout();

    rb->lcd_setfont(FONT_UI);
    draw_splashscreen();
    size_t bytes;
    void *arena = rb->plugin_get_buffer(&bytes);
    int ret = create_album_index(arena, bytes);
    if (ret < 0)
    {
        const char *error = ret == ERROR_BUFFER_FULL ? "PictureFlow library exceeds memory" :
            ret == ROCK_PF_DB_BUSY ? "Music database is not ready" :
            "No albums. Update the Music database.";
        rb->splash(HZ * 2, error);
        return false;
    }
    if (!pf_cache_init())
    {
        rb->splash(HZ, "Not enough memory for PictureFlow");
        return false;
    }

    init_scroll_lines();
    init_reflect_table();
    number_of_slides = pf_idx.album_ct;
    center_index = 0;
    pf_state = pf_idle;
    free_borrowed_tracks();
    extra_fade = 0;
    fade = 256;
    recalc_offsets();
    buffer = LCD_BUF;
    set_current_slide(pf_cfg.last_album);
    rb->lcd_set_drawmode(DRMODE_FG);
    if (create_pf_thread())
        return true;
    rb->splash(HZ, "Could not start the PictureFlow worker");
    return false;
}

static bool reinit(bool rebuild)
{
    return_to_idle_state();
    unsigned int album = mfnv(get_album_name(center_index));
    unsigned int artist = mfnv(get_album_artist(center_index));
    end_pf_thread();
    if (rebuild && !pf_cache_clear())
        rb->splash(HZ, "Could not clear all PictureFlow covers");

    if (!init())
        return false;
    reselect(album, artist);
    return true;
}

""")
replace("static int display_settings_menu(void)", """\
static bool pf_confirm(const unsigned char *message)
{
    const char *lines[] = { (const char *)message };
    const struct text_message prompt = { lines, 1 };
    return rb->gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES;
}

static int display_settings_menu(void)""")
replace("                if (!show_track_list() && !prompt_reinit())\n                    return PLUGIN_OK;",
        "                show_track_list();")
section("                if (aa_cache.inspected < pf_idx.album_ct)", "                break;\n        }")
section("            if(aa_cache.inspected >= pf_idx.album_ct)", "\n            if (pf_cfg.show_album_name",
        '            rb->snprintf(fpstxt, sizeof(fpstxt), "FPS: %d", fps);\n')
replace("        current_update = *rb->current_tick;", """\
        if (!rb->rock_pf_db_current(&pf_idx.summary))
        {
            rb->splash(HZ, "Music database changed. Reopen PictureFlow.");
            return PLUGIN_OK;
        }
        current_update = *rb->current_tick;""")
replace("        if (is_initial_slide == false)\n            mylcd_update();", "        mylcd_update();")
replace("            ,instant_update ? 0 : HZ/16,", "            ,instant_update ? 0 : MAX(1, HZ/50),")
replace("            if (ret == -3)\n            {\n                if (!reinit())",
        "            if (ret == -3 || ret == -4)\n            {\n                if (!reinit(ret == -4))")
section("                if (context_menu_ready())", "            }\n            break;\n#endif", """\
                ret = context_menu();
                if (ret != PLUGIN_OK)
                    return ret;
""")
section("    if (!check_database())", "    atexit(cleanup);")
replace("    else\n        ret = PLUGIN_OK;\n\n    if ( ret", "    else\n        return PLUGIN_OK;\n\n    if ( ret")
# Keep the stock projection when a decoded cover is shorter than its horizon.
replace("        const pix_t *ptr = &src[column * bmp->height];", """\
        if (dy <= 0)
            break;
        const pix_t *ptr = &src[column * bmp->height];""")
replace("        pixel = LCDADDR(x, (LCD_HEIGHT/2) );", """\
        int skip = p < 0 ? (-p + dy - 1) / dy : 0;
        pixel = LCDADDR(x, LCD_HEIGHT / 2 + MIN(skip, LCD_HEIGHT / 2 - 1));
        p += skip * dy;""")
code = code.replace("free_borrowed_tracks()", "clear_track_index()")
code = re.sub(r"(pf_idx\.album_index\[[^\]]+\])\.year", r"\1.row->year", code)
output.write_text(code)
