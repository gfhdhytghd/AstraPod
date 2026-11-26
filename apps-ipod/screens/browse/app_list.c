/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Games, Other Apps and Demos lists: the installed plugin modules of one
 * kind, and in Games the Music Quiz, as an album list draws its albums --
 * one tall row each, the app's icon where an album's cover would be, then
 * its name in the player's language, sorted by that name. Choosing a row
 * starts the app; leaving it comes back to the row.
 *
 * The icons live in THEME_DIR/app-icons.bin, written by
 * apps-ipod/bitmaps/source/apps/gen_app_icons.py, which documents the file.
 * A row's icon is read from it when the row is drawn, into a few slots that
 * last while the list is shown. An app without an icon draws the pack's
 * placeholder tile; without the pack, the list's own glyph.
 *
 * Parts, in order:
 *   - the apps
 *   - the icons
 *   - the list
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "kernel.h"
#include "core_alloc.h"
#include "lang.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "settings/settings.h"
#include "draw/bmp.h"
#include "speech/talk.h"
#include "input/action.h"
#include "system/activity.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "plugins/rock_compat.h"
#include "app_list.h"

/***************************************************************************
 * the apps
 ****************************************************************************/

#define MUSIC_QUIZ "native/music_quiz"

/* `id` is the module's path under PLUGIN_DIR without ".rock", and the name
 * of its icon in the pack. */
struct app
{
    const char *id;
    unsigned short lang;
    unsigned char kind;
};

/* The modules bundle-rocks.sh can install, less the ones that only mean
 * something opened with a file or by another plugin -- viewers/, search,
 * file_picker and view_text -- and the Extras menu's own rows: alarmclock,
 * calendar, clock and stopwatch. tools/tests/app_list_test.py holds the
 * same list. */
static const struct app apps[] = {
#ifdef ASTRAPOD_ROCK_COMPAT
    { "games/2048", LANG_APP_2048, APP_LIST_GAMES },
    { "games/amaze", LANG_APP_AMAZE, APP_LIST_GAMES },
    { "games/blackjack", LANG_APP_BLACKJACK, APP_LIST_GAMES },
    { "games/brickmania", LANG_APP_BRICKMANIA, APP_LIST_GAMES },
    { "games/bubbles", LANG_APP_BUBBLES, APP_LIST_GAMES },
    { "games/chessbox", LANG_APP_CHESSBOX, APP_LIST_GAMES },
    { "games/chopper", LANG_APP_CHOPPER, APP_LIST_GAMES },
    { "games/clix", LANG_APP_CLIX, APP_LIST_GAMES },
    { "games/codebuster", LANG_APP_CODEBUSTER, APP_LIST_GAMES },
    { "games/dice", LANG_APP_DICE, APP_LIST_GAMES },
    { "games/doom", LANG_APP_DOOM, APP_LIST_GAMES },
    { "games/duke3d", LANG_APP_DUKE3D, APP_LIST_GAMES },
    { "games/flipit", LANG_APP_FLIPIT, APP_LIST_GAMES },
    { "games/goban", LANG_APP_GOBAN, APP_LIST_GAMES },
    { "games/invadrox", LANG_APP_INVADROX, APP_LIST_GAMES },
    { "games/jackpot", LANG_APP_JACKPOT, APP_LIST_GAMES },
    { "games/jewels", LANG_APP_JEWELS, APP_LIST_GAMES },
    { "games/maze", LANG_APP_MAZE, APP_LIST_GAMES },
    { "games/mazezam", LANG_APP_MAZEZAM, APP_LIST_GAMES },
    { "games/minesweeper", LANG_APP_MINESWEEPER, APP_LIST_GAMES },
    { "games/mochipod", LANG_APP_MOCHIPOD, APP_LIST_GAMES },
    { "games/pacbox", LANG_APP_PACBOX, APP_LIST_GAMES },
    { "games/pegbox", LANG_APP_PEGBOX, APP_LIST_GAMES },
    { "games/pong", LANG_APP_PONG, APP_LIST_GAMES },
    { "games/quake", LANG_APP_QUAKE, APP_LIST_GAMES },
    { "games/reversi", LANG_APP_REVERSI, APP_LIST_GAMES },
    { "games/robotfindskitten", LANG_APP_ROBOTFINDSKITTEN, APP_LIST_GAMES },
    { "games/rockblox", LANG_APP_ROCKBLOX, APP_LIST_GAMES },
    { "games/rockblox1d", LANG_APP_ROCKBLOX1D, APP_LIST_GAMES },
    { "games/rockboy", LANG_APP_ROCKBOY, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-blackbox", LANG_APP_SGT_BLACKBOX, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-bridges", LANG_APP_SGT_BRIDGES, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-cube", LANG_APP_SGT_CUBE, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-dominosa", LANG_APP_SGT_DOMINOSA, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-fifteen", LANG_APP_SGT_FIFTEEN, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-filling", LANG_APP_SGT_FILLING, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-flip", LANG_APP_SGT_FLIP, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-flood", LANG_APP_SGT_FLOOD, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-galaxies", LANG_APP_SGT_GALAXIES, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-group", LANG_APP_SGT_GROUP, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-guess", LANG_APP_SGT_GUESS, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-inertia", LANG_APP_SGT_INERTIA, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-keen", LANG_APP_SGT_KEEN, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-lightup", LANG_APP_SGT_LIGHTUP, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-loopy", LANG_APP_SGT_LOOPY, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-magnets", LANG_APP_SGT_MAGNETS, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-map", LANG_APP_SGT_MAP, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-mines", LANG_APP_SGT_MINES, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-mosaic", LANG_APP_SGT_MOSAIC, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-net", LANG_APP_SGT_NET, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-netslide", LANG_APP_SGT_NETSLIDE, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-palisade", LANG_APP_SGT_PALISADE, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-pattern", LANG_APP_SGT_PATTERN, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-pearl", LANG_APP_SGT_PEARL, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-pegs", LANG_APP_SGT_PEGS, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-range", LANG_APP_SGT_RANGE, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-rect", LANG_APP_SGT_RECT, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-samegame", LANG_APP_SGT_SAMEGAME, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-signpost", LANG_APP_SGT_SIGNPOST, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-singles", LANG_APP_SGT_SINGLES, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-sixteen", LANG_APP_SGT_SIXTEEN, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-slant", LANG_APP_SGT_SLANT, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-slide", LANG_APP_SGT_SLIDE, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-sokoban", LANG_APP_SGT_SOKOBAN, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-solo", LANG_APP_SGT_SOLO, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-tents", LANG_APP_SGT_TENTS, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-towers", LANG_APP_SGT_TOWERS, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-tracks", LANG_APP_SGT_TRACKS, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-twiddle", LANG_APP_SGT_TWIDDLE, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-undead", LANG_APP_SGT_UNDEAD, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-unequal", LANG_APP_SGT_UNEQUAL, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-unruly", LANG_APP_SGT_UNRULY, APP_LIST_GAMES },
    { "games/sgt_puzzles/sgt-untangle", LANG_APP_SGT_UNTANGLE, APP_LIST_GAMES },
    { "games/sliding_puzzle", LANG_APP_SLIDING_PUZZLE, APP_LIST_GAMES },
    { "games/snake", LANG_APP_SNAKE, APP_LIST_GAMES },
    { "games/snake2", LANG_APP_SNAKE2, APP_LIST_GAMES },
    { "games/sokoban", LANG_APP_SOKOBAN, APP_LIST_GAMES },
    { "games/solitaire", LANG_APP_SOLITAIRE, APP_LIST_GAMES },
    { "games/spacerocks", LANG_APP_SPACEROCKS, APP_LIST_GAMES },
    { "games/star", LANG_APP_STAR, APP_LIST_GAMES },
    { "games/sudoku", LANG_APP_SUDOKU, APP_LIST_GAMES },
    { "games/superdom", LANG_APP_SUPERDOM, APP_LIST_GAMES },
    { "games/wolf3d", LANG_APP_WOLF3D, APP_LIST_GAMES },
    { "games/wormlet", LANG_APP_WORMLET, APP_LIST_GAMES },
    { "games/xobox", LANG_APP_XOBOX, APP_LIST_GAMES },
    { "games/xrick", LANG_APP_XRICK, APP_LIST_GAMES },
    { "games/xworld", LANG_APP_XWORLD, APP_LIST_GAMES },
    { "games/zxbox", LANG_APP_ZXBOX, APP_LIST_GAMES },
    { "apps/announce_status", LANG_APP_ANNOUNCE_STATUS, APP_LIST_OTHER },
    { "apps/calculator", LANG_APP_CALCULATOR, APP_LIST_OTHER },
    { "apps/chessclock", LANG_APP_CHESSCLOCK, APP_LIST_OTHER },
    { "apps/countdown_timer", LANG_APP_COUNTDOWN_TIMER, APP_LIST_OTHER },
    { "apps/cue_playlist", LANG_APP_CUE_PLAYLIST, APP_LIST_OTHER },
    { "apps/dart_scorer", LANG_APP_DART_SCORER, APP_LIST_OTHER },
    { "apps/db_folder_select", LANG_APP_DB_FOLDER_SELECT, APP_LIST_OTHER },
    { "apps/dict", LANG_APP_DICT, APP_LIST_OTHER },
    { "apps/disktidy", LANG_APP_DISKTIDY, APP_LIST_OTHER },
    { "apps/eq_optimizer", LANG_APP_EQ_OPTIMIZER, APP_LIST_OTHER },
    { "apps/h264_player", LANG_APP_H264_PLAYER, APP_LIST_OTHER },
    { "apps/imageviewer", LANG_APP_IMAGEVIEWER, APP_LIST_OTHER },
    { "apps/imageviewer_compat", LANG_APP_IMAGEVIEWER_COMPAT, APP_LIST_OTHER },
    { "apps/keybox", LANG_APP_KEYBOX, APP_LIST_OTHER },
    { "apps/keyremap", LANG_APP_KEYREMAP, APP_LIST_OTHER },
    { "apps/lamp", LANG_APP_LAMP, APP_LIST_OTHER },
    { "apps/lastfm_scrobbler", LANG_APP_LASTFM_SCROBBLER, APP_LIST_OTHER },
    { "apps/lastfm_scrobbler_viewer", LANG_APP_LASTFM_SCROBBLER_VIEWER,
      APP_LIST_OTHER },
    { "apps/lrcplayer", LANG_APP_LRCPLAYER, APP_LIST_OTHER },
    { "apps/main_menu_config", LANG_APP_MAIN_MENU_CONFIG, APP_LIST_OTHER },
    { "apps/md5sum", LANG_APP_MD5SUM, APP_LIST_OTHER },
    { "apps/metronome", LANG_APP_METRONOME, APP_LIST_OTHER },
    { "apps/midi", LANG_APP_MIDI, APP_LIST_OTHER },
    { "apps/mikmod", LANG_APP_MIKMOD, APP_LIST_OTHER },
    { "apps/mpegplayer", LANG_APP_MPEGPLAYER, APP_LIST_OTHER },
    { "apps/open_plugins", LANG_APP_OPEN_PLUGINS, APP_LIST_OTHER },
    { "apps/otp", LANG_APP_OTP, APP_LIST_OTHER },
    { "apps/pdbox", LANG_APP_PDBOX, APP_LIST_OTHER },
    { "apps/periodic_table", LANG_APP_PERIODIC_TABLE, APP_LIST_OTHER },
    { "apps/pitch_detector", LANG_APP_PITCH_DETECTOR, APP_LIST_OTHER },
    { "apps/pitch_screen", LANG_APP_PITCH_SCREEN, APP_LIST_OTHER },
    { "apps/playing_time", LANG_APP_PLAYING_TIME, APP_LIST_OTHER },
    { "apps/properties", LANG_APP_PROPERTIES, APP_LIST_OTHER },
    { "apps/random_folder_advance_config",
      LANG_APP_RANDOM_FOLDER_ADVANCE_CONFIG, APP_LIST_OTHER },
    { "apps/rb_info", LANG_APP_RB_INFO, APP_LIST_OTHER },
    { "apps/remote_control", LANG_APP_REMOTE_CONTROL, APP_LIST_OTHER },
    { "apps/resistor", LANG_APP_RESISTOR, APP_LIST_OTHER },
    { "apps/rockpaint", LANG_APP_ROCKPAINT, APP_LIST_OTHER },
    { "apps/settings_dumper", LANG_APP_SETTINGS_DUMPER, APP_LIST_OTHER },
    { "apps/shortcuts_append", LANG_APP_SHORTCUTS_APPEND, APP_LIST_OTHER },
    { "apps/shortcuts_view", LANG_APP_SHORTCUTS_VIEW, APP_LIST_OTHER },
    { "apps/sort", LANG_APP_SORT, APP_LIST_OTHER },
    { "apps/speedread", LANG_APP_SPEEDREAD, APP_LIST_OTHER },
    { "apps/stats", LANG_APP_STATS, APP_LIST_OTHER },
    { "apps/tagnavi_edit", LANG_APP_TAGNAVI_EDIT, APP_LIST_OTHER },
    { "apps/text_editor", LANG_APP_TEXT_EDITOR, APP_LIST_OTHER },
    { "apps/text_viewer", LANG_APP_TEXT_VIEWER, APP_LIST_OTHER },
    { "apps/theme_remove", LANG_APP_THEME_REMOVE, APP_LIST_OTHER },
    { "apps/battery_bench", LANG_APP_BATTERY_BENCH, APP_LIST_DEMOS },
    { "demos/bounce", LANG_APP_BOUNCE, APP_LIST_DEMOS },
    { "demos/credits", LANG_APP_CREDITS, APP_LIST_DEMOS },
    { "demos/cube", LANG_APP_CUBE, APP_LIST_DEMOS },
    { "demos/demystify", LANG_APP_DEMYSTIFY, APP_LIST_DEMOS },
    { "demos/fft", LANG_APP_FFT, APP_LIST_DEMOS },
    { "demos/fire", LANG_APP_FIRE, APP_LIST_DEMOS },
    { "demos/fireworks", LANG_APP_FIREWORKS, APP_LIST_DEMOS },
    { "demos/fractals", LANG_APP_FRACTALS, APP_LIST_DEMOS },
    { "demos/logo", LANG_APP_LOGO, APP_LIST_DEMOS },
    { "demos/matrix", LANG_APP_MATRIX, APP_LIST_DEMOS },
    { "demos/mosaique", LANG_APP_MOSAIQUE, APP_LIST_DEMOS },
    { "demos/oscilloscope", LANG_APP_OSCILLOSCOPE, APP_LIST_DEMOS },
    { "demos/pictureflow", LANG_APP_PICTUREFLOW, APP_LIST_DEMOS },
    { "demos/plasma", LANG_APP_PLASMA, APP_LIST_DEMOS },
    { "demos/podlink_echo", LANG_APP_PODLINK_ECHO, APP_LIST_DEMOS },
    { "demos/rocklife", LANG_APP_ROCKLIFE, APP_LIST_DEMOS },
    { "demos/snow", LANG_APP_SNOW, APP_LIST_DEMOS },
    { "demos/starfield", LANG_APP_STARFIELD, APP_LIST_DEMOS },
    { "demos/test_kbd", LANG_APP_TEST_KBD, APP_LIST_DEMOS },
    { "demos/vu_meter", LANG_APP_VU_METER, APP_LIST_DEMOS },
#endif
    { MUSIC_QUIZ, LANG_MUSIC_QUIZ, APP_LIST_GAMES },
};

static struct
{
    int count;
    unsigned char app[ARRAYLEN(apps)];      /* rows, as indexes into apps[] */
    uint32_t icon[ARRAYLEN(apps)];          /* each row's icon in the pack */
} rows;

static bool installed(const struct app *app)
{
    char path[MAX_PATH];

    if (!strcmp(app->id, MUSIC_QUIZ))
        return true;
    snprintf(path, sizeof(path), PLUGIN_DIR "/%s.rock", app->id);
    return file_exists(path);
}

static int by_name(const void *a, const void *b)
{
    int x = *(const unsigned char *)a, y = *(const unsigned char *)b;
    int order = strcasecmp(str(apps[x].lang), str(apps[y].lang));

    return order ? order : x - y;
}

static void collect(enum app_list_kind kind)
{
    rows.count = 0;
    for (unsigned i = 0; i < ARRAYLEN(apps); i++)
        if (apps[i].kind == kind && installed(&apps[i]))
            rows.app[rows.count++] = i;
    qsort(rows.app, rows.count, sizeof(rows.app[0]), by_name);
}

static int launch(const struct app *app)
{
    if (!strcmp(app->id, MUSIC_QUIZ))
        return GO_TO_MUSIC_QUIZ;
#ifdef ASTRAPOD_ROCK_COMPAT
    char path[MAX_PATH];

    snprintf(path, sizeof(path), PLUGIN_DIR "/%s.rock", app->id);
    return rock_open_from_browser(path, NULL);
#else
    return GO_TO_PREVIOUS;
#endif
}

/***************************************************************************
 * the icons
 ****************************************************************************/

#define ICON_PACK THEME_DIR "/app-icons.bin"
#define ICON_ID_BYTES 48
#define ICON_SIZES_MAX 4
#define ICON_SIZE_MAX 72        /* the largest list art a theme can ask for */
/* Enough for the rows a built-in theme shows at once, part rows too. */
#define ICON_SLOTS 8

static struct
{
    int size;           /* 0 until the first row is drawn, -1 with no pack */
    int handle;         /* the slots, 0 when not allocated */
    int item[ICON_SLOTS];
    int victim;
    struct bitmap bitmap;
} icons;

/* A slot holds one icon as the pack stores it: RGB565 rows, as both
 * players' LCDs take them, then 4-bit alpha rows, each part 4-byte aligned. */
static size_t pixel_bytes(int size)
{
    return ALIGN_UP((size_t)size * size * FB_DATA_SZ, 4);
}

static size_t slot_bytes(int size)
{
    return pixel_bytes(size) + ALIGN_UP((size_t)(size + 1) / 2 * size, 4);
}

/* Pick the pack's largest size that fits a `want` pixel cell, or its
 * smallest, and find each row's icon at that size. */
static bool icons_find(int want)
{
    struct { char magic[4]; uint32_t version, count, sizes; } head;
    uint32_t sizes[ICON_SIZES_MAX], placeholder = 0;
    unsigned char entry[ICON_ID_BYTES + 4 * ICON_SIZES_MAX];
    int fd = open(ICON_PACK, O_RDONLY);
    int pick = -1;
    bool ok = false;

    if (fd < 0)
        return false;
    if (read(fd, &head, sizeof(head)) != (ssize_t)sizeof(head) ||
        memcmp(head.magic, "AICN", 4) || letoh32(head.version) != 1)
        goto out;
    head.count = letoh32(head.count);
    head.sizes = letoh32(head.sizes);
    if (head.sizes < 1 || head.sizes > ICON_SIZES_MAX ||
        read(fd, sizes, 4 * head.sizes) != (ssize_t)(4 * head.sizes))
        goto out;
    for (unsigned i = 0; i < head.sizes; i++)
    {
        sizes[i] = letoh32(sizes[i]);
        bool fits = sizes[i] <= (uint32_t)want;
        bool picked_fits = pick >= 0 && sizes[pick] <= (uint32_t)want;
        if (pick < 0 || (fits ? !picked_fits || sizes[i] > sizes[pick]
                              : !picked_fits && sizes[i] < sizes[pick]))
            pick = i;
    }
    if (sizes[pick] < 1 || sizes[pick] > ICON_SIZE_MAX)
        goto out;

    memset(rows.icon, 0, sizeof(rows.icon));
    size_t entry_bytes = ICON_ID_BYTES + 4 * head.sizes;
    for (uint32_t n = 0; n < head.count; n++)
    {
        if (read(fd, entry, entry_bytes) != (ssize_t)entry_bytes)
            goto out;
        entry[ICON_ID_BYTES - 1] = '\0';
        uint32_t offset = load_le32(entry + ICON_ID_BYTES + 4 * pick);
        if (!strcmp((char *)entry, "*"))
            placeholder = offset;
        for (int r = 0; r < rows.count; r++)
            if (!strcmp((char *)entry, apps[rows.app[r]].id))
                rows.icon[r] = offset;
    }
    for (int r = 0; r < rows.count; r++)
        if (!rows.icon[r])
            rows.icon[r] = placeholder;
    icons.size = sizes[pick];
    ok = true;
out:
    close(fd);
    return ok;
}

static bool icons_read(int item, int slot)
{
    size_t bytes = slot_bytes(icons.size);
    int fd = open(ICON_PACK, O_RDONLY);
    bool ok;

    if (fd < 0)
        return false;
    char *store = core_get_data_pinned(icons.handle);
    ok = lseek(fd, rows.icon[item], SEEK_SET) == (off_t)rows.icon[item] &&
         read(fd, store + slot * bytes, bytes) == (ssize_t)bytes;
    core_put_data_pinned(store);
    close(fd);
    return ok;
}

/* The slots are freed before an app starts, which wants the memory. */
static void icons_release(void)
{
    if (icons.handle > 0)
        core_free(icons.handle);
    icons.handle = 0;
}

static const struct bitmap *get_art(int item, void *data, struct dim *size)
{
    int slot;
    (void)data;

    if (!icons.size && !icons_find(MIN(size->width, size->height)))
        icons.size = -1;
    if (icons.size < 0 || item < 0 || item >= rows.count || !rows.icon[item])
        return NULL;
    if (icons.handle <= 0)
    {
        icons.handle = core_alloc(ICON_SLOTS * slot_bytes(icons.size));
        if (icons.handle <= 0)
        {
            icons.handle = 0;
            return NULL;
        }
        for (slot = 0; slot < ICON_SLOTS; slot++)
            icons.item[slot] = -1;
    }

    for (slot = 0; slot < ICON_SLOTS; slot++)
        if (icons.item[slot] == item)
            break;
    if (slot == ICON_SLOTS)
    {
        slot = icons.victim;
        icons.victim = (icons.victim + 1) % ICON_SLOTS;
        icons.item[slot] = -1;
        if (!icons_read(item, slot))
            return NULL;
        icons.item[slot] = item;
    }

    icons.bitmap.width = icons.bitmap.height = icons.size;
    icons.bitmap.format = FORMAT_NATIVE;
    icons.bitmap.maskdata = NULL;
    icons.bitmap.alpha_offset = pixel_bytes(icons.size);
    icons.bitmap.data = (unsigned char *)core_get_data(icons.handle) +
                        slot * slot_bytes(icons.size);
    size->width = size->height = icons.size;
    return &icons.bitmap;
}

/***************************************************************************
 * the list
 ****************************************************************************/

static const char *get_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return str(apps[rows.app[item]].lang);
}

static int get_talk(int item, void *data)
{
    (void)data;
    talk_id(apps[rows.app[item]].lang, false);
    return 0;
}

/* An album's kind, so the row draws as an album row does. */
static enum list_row_kind get_kind(int item, void *data)
{
    (void)item; (void)data;
    return LIST_ROW_CONTAINER;
}

int app_list_screen(enum app_list_kind kind)
{
    static const unsigned short titles[] = {
        [APP_LIST_GAMES] = LANG_PLUGIN_GAMES,
        [APP_LIST_OTHER] = LANG_APP_LIST_OTHER,
        [APP_LIST_DEMOS] = LANG_PLUGIN_DEMOS,
    };
    struct simplelist_info info;
    int selection = 0, ret;

    collect(kind);
    if (!rows.count)
    {
        splash(HZ * 2, ID2P(LANG_NO_FILES));
        return GO_TO_PREVIOUS;
    }
    icons.size = 0;
    push_current_activity(ACTIVITY_PLUGINBROWSER);
    /* Menu goes back a level, as in the plugin folders. */
    bool menu_back = action_set_menu_back(true);
    for (;;)
    {
        simplelist_info_init(&info, (char *)str(titles[kind]), rows.count,
                             NULL);
        info.get_name = get_name;
        info.get_talk = get_talk;
        info.get_kind = get_kind;
        info.get_albumart = get_art;
        info.row_height = global_settings.db_art_row_height;
        info.selection = selection;
        info.wps_on_play = true;
        if (simplelist_show_list(&info))
            ret = GO_TO_ROOT;
        else if (info.selection == -2)
            ret = GO_TO_WPS;
        else if (info.selection < 0)
            ret = GO_TO_PREVIOUS;
        else
        {
            selection = info.selection;
            icons_release();
            ret = launch(&apps[rows.app[selection]]);
            if (ret == GO_TO_PREVIOUS)
                continue;
        }
        break;
    }
    icons_release();
    action_set_menu_back(menu_back);
    pop_current_activity();
    return ret;
}
