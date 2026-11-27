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
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "core_alloc.h"
#include "lang.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "screens/fruit_menu.h"
#include "screens/extras/extras_bar.h"
#include "settings/settings.h"
#include "draw/bmp.h"
#include "draw/text_wrap.h"
#include "draw/viewport.h"
#include "speech/talk.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "ui/native_theme_runtime.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "plugins/rock_compat.h"
#include "app_list.h"
#include "ui/scene.h"

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
    { "demos/app_tray_demo", LANG_APP_APP_TRAY_DEMO, APP_LIST_DEMOS },
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

/* What the last look at each list found, -1 before the first */
static int counts[] = { -1, -1, -1 };

static void collect(enum app_list_kind kind)
{
    rows.count = 0;
    for (unsigned i = 0; i < ARRAYLEN(apps); i++)
        if (apps[i].kind == kind && installed(&apps[i]))
            rows.app[rows.count++] = i;
    qsort(rows.app, rows.count, sizeof(rows.app[0]), by_name);
    counts[kind] = rows.count;
}

int app_list_count(enum app_list_kind kind)
{
    if (counts[kind] < 0)
        collect(kind);
    return counts[kind];
}

static int launch(const struct app *app, bool quiet, bool *failed)
{
    if (failed)
        *failed = false;
    if (!strcmp(app->id, MUSIC_QUIZ))
        return GO_TO_MUSIC_QUIZ;
#ifdef ASTRAPOD_ROCK_COMPAT
    char path[MAX_PATH];

    snprintf(path, sizeof(path), PLUGIN_DIR "/%s.rock", app->id);
    if (quiet)
        return rock_open_from_browser_quiet(path, NULL, failed);
    return rock_open_from_browser(path, NULL);
#else
    (void)quiet;
    return GO_TO_PREVIOUS;
#endif
}

/***************************************************************************
 * play counts (Fruit Games catalogue)
 ****************************************************************************/

#define GAMES_PLAYED_FILE ROCKBOX_DIR "/games_played.txt"

static uint16_t game_plays[ARRAYLEN(apps)];

static void games_stats_load(void)
{
    memset(game_plays, 0, sizeof(game_plays));
    int fd = open(GAMES_PLAYED_FILE, O_RDONLY);
    if (fd < 0)
        return;
    char line[96];
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab++ = '\0';
        int n = atoi(tab);
        if (n <= 0)
            continue;
        for (unsigned i = 0; i < ARRAYLEN(apps); i++)
            if (!strcmp(apps[i].id, line))
            {
                game_plays[i] = (uint16_t)MIN(n, 65535);
                break;
            }
    }
    close(fd);
}

static void games_stats_save(void)
{
    int fd = open(GAMES_PLAYED_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (unsigned i = 0; i < ARRAYLEN(apps); i++)
        if (game_plays[i] > 0)
            fdprintf(fd, "%s\t%u\n", apps[i].id, (unsigned)game_plays[i]);
    close(fd);
}

static void games_stats_record(unsigned app_idx)
{
    if (app_idx < ARRAYLEN(apps) && game_plays[app_idx] < 65535)
    {
        game_plays[app_idx]++;
        games_stats_save();
    }
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

/* Stock 2.0.5 Games catalogue row geometry (320x240, 4 rows of 55 px). */
#define GAME_ROW_H      55
#define GAME_ICON_W     55
#define GAME_TEXT_X     65
#define GAME_TITLE_BASE 23
#define GAME_SUB_BASE   42
#define GAME_CHEVRON_R  16
#define GAME_CHEVRON_Y  20
#define GAME_TILE_RGB   LCD_RGBPACK(239, 241, 247)
#define GAME_SUB_RGB    LCD_RGBPACK(107, 109, 107)
#define GAME_RULE_RGB   LCD_RGBPACK(222, 223, 222)

/* Stock 2.0.5 dark launch-error page (matches game_klondike.png). */
#define ERR_BG_RGB      0x313431
#define ERR_WHITE       0xffffff
#define ERR_X           25
#define ERR_W           (LCD_WIDTH - 2 * ERR_X)
#define ERR_HEAD_Y      51
#define ERR_BODY_Y      89

static int game_sub_font = -1;

static uint32_t game_sel_rgb(int row)
{
    if (row == 0)
        return 0x96c5da;
    if (row == GAME_ROW_H - 1)
        return 0x004386;
    int t = row - 1, span = GAME_ROW_H - 3;
    int r = (9 * (span - t) + 0 * t) / span;
    int g = (164 * (span - t) + 100 * t) / span;
    int b = (225 * (span - t) + 211 * t) / span;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void format_play_count(unsigned plays, char *buf, size_t size)
{
    if (plays == 0)
        strlcpy(buf, str(LANG_FRUIT_GAMES_NEVER_PLAYED), size);
    else if (plays == 1)
        strlcpy(buf, str(LANG_FRUIT_GAMES_PLAYED_ONCE), size);
    else
        snprintf(buf, size, str(LANG_FRUIT_GAMES_PLAYED_TIMES), plays);
}

static void draw_game_row(struct list_putlineinfo_t *info)
{
    struct viewport *vp = info->vp;
    const struct viewport *parent = info->list->parent[SCREEN_MAIN];
    int width = parent->x + parent->width - (vp->x + info->x);
    unsigned fg = vp->fg_pattern;
    int font = vp->font;
    int row_idx = info->line;

    /* 55x55 icon tile at the left, never covered by the blue selection bar. */
    vp->fg_pattern = GAME_TILE_RGB;
    lcd_fillrect(info->x, info->y, GAME_ICON_W, GAME_ROW_H);
    struct dim sz = { GAME_ICON_W, GAME_ROW_H };
    const struct bitmap *bmp = get_art(row_idx, NULL, &sz);
    if (bmp)
    {
        int bx = info->x + MAX(0, (GAME_ICON_W - bmp->width) / 2);
        int by = info->y + MAX(0, (GAME_ROW_H - bmp->height) / 2);
        lcd_bmp(bmp, bx, by);
    }

    int bar_x = info->x + GAME_ICON_W;
    int bar_w = width - GAME_ICON_W;
    if (info->is_selected && bar_w > 0)
    {
        for (int r = 0; r < GAME_ROW_H; r++)
        {
            uint32_t rgb = game_sel_rgb(r);
            vp->fg_pattern = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255,
                                         rgb & 255);
            lcd_hline(bar_x, bar_x + bar_w - 1, info->y + r);
        }
    }
    else if (bar_w > 0)
    {
        vp->fg_pattern = GAME_RULE_RGB;
        lcd_hline(bar_x, bar_x + bar_w - 1, info->y + GAME_ROW_H - 1);
    }

    vp->fg_pattern = fg;
    lcd_putsxy(info->x + GAME_TEXT_X,
               info->y + GAME_TITLE_BASE - font_get(font)->ascent,
               (const unsigned char *)info->dsp_text);

    if (game_sub_font >= 0 && row_idx >= 0 && row_idx < rows.count)
    {
        char sub[48];
        format_play_count(game_plays[rows.app[row_idx]], sub, sizeof(sub));
        vp->font = game_sub_font;
        vp->fg_pattern = info->is_selected ? fg : GAME_SUB_RGB;
        lcd_putsxy(info->x + GAME_TEXT_X,
                   info->y + GAME_SUB_BASE - font_get(game_sub_font)->ascent,
                   (const unsigned char *)sub);
        vp->font = font;
    }

    if (info->is_selected)
        stock_art_draw(STOCK_ART_SYSTEM_SUBMENU,
                       vp->x + info->x + width - GAME_CHEVRON_R,
                       vp->y + info->y + GAME_CHEVRON_Y,
                       (struct ui_rect){ vp->x, vp->y, vp->width,
                                         vp->height });
    vp->fg_pattern = fg;
}

static void paint_game_error(const char *heading, const char *body,
                             int head_font, int body_font)
{
    uint32_t *band = extras_band(0);
    if (!band)
        return;
    const struct ui_rect all = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    struct font *hf = font_get(head_font);
    struct font *bf = font_get(body_font);
    char line[128];

    for (int y = 0; y < LCD_HEIGHT; y += EXTRAS_BAND_ROWS)
    {
        struct stock_canvas c = { band, 0, y, LCD_WIDTH,
                                  MIN(EXTRAS_BAND_ROWS, LCD_HEIGHT - y) };
        int n = c.width * c.height;
        for (int i = 0; i < n; i++)
            c.pixels[i] = ERR_BG_RGB;

        int cy = ERR_HEAD_Y;
        if (heading && *heading && head_font >= 0)
        {
            font_lock(head_font, true);
            for (const char *at = heading, *end; *at; cy += hf->height)
            {
                const char *next = text_wrap_next(at, ERR_W, hf, &end);
                strlcpy(line, at, MIN((size_t)(end - at) + 1, sizeof(line)));
                stock_canvas_text(&c, head_font, line, ERR_X, cy, ERR_WHITE);
                at = next;
            }
            font_lock(head_font, false);
        }

        cy = MAX(ERR_BODY_Y, cy + 12);
        if (body && *body && body_font >= 0)
        {
            font_lock(body_font, true);
            for (const char *at = body, *end; *at; cy += bf->height)
            {
                const char *next = text_wrap_next(at, ERR_W, bf, &end);
                strlcpy(line, at, MIN((size_t)(end - at) + 1, sizeof(line)));
                stock_canvas_text(&c, body_font, line, ERR_X, cy, ERR_WHITE);
                at = next;
            }
            font_lock(body_font, false);
        }

        extras_bar_put(&c, str(LANG_FRUIT_GAMES_ERROR));
        stock_canvas_show(&c, all);
    }
    lcd_update();
}

static int show_game_error(const char *heading, const char *body)
{
    struct ui_scene scene = {0};
    int ui = screens[SCREEN_MAIN].getuifont();
    int head_font = ui_theme_current_font_load(UI_FONT_TITLE, 64);
    int body_font = ui_theme_current_font_load(UI_FONT_BODY, 128);
    if (head_font < 0)
        head_font = ui;
    if (body_font < 0)
        body_font = ui;

    ui_scene_open_page(&scene, "game-error-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    paint_game_error(heading, body, head_font, body_font);

    int ret = GO_TO_PREVIOUS;
    while (true)
    {
        int action = extras_get_action(TIMEOUT_BLOCK, true);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_OK)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
    }

    ui_scene_close_modal(&scene, ret == GO_TO_PREVIOUS, "game-error-pop");
    extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (head_font != ui)
        font_unload(head_font);
    if (body_font != ui)
        font_unload(body_font);
    return ret;
}

static int fruit_games_screen(void)
{
    struct gui_synclist list;
    struct ui_scene scene = {0};
    int selection = 0, ret = GO_TO_PREVIOUS, action;

    games_stats_load();
    icons.size = 0;
    game_sub_font = ui_theme_current_font_load(UI_FONT_BODY,
                                               global_settings.glyphs_to_cache);
    ui_scene_open_page(&scene, "apps-list-push");
    push_current_activity(ACTIVITY_PLUGINBROWSER);

    for (bool open = true; open; )
    {
        sb_set_persistent_title(str(LANG_PLUGIN_GAMES), Icon_NOICON,
                                SCREEN_MAIN);
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        gui_synclist_init(&list, get_name, NULL, false, 1, NULL);
        gui_synclist_set_title(&list, str(LANG_PLUGIN_GAMES), Icon_NOICON);
        gui_synclist_set_row_callbacks(&list, get_kind, NULL, NULL);
        gui_synclist_set_voice_callback(&list, get_talk);
        gui_synclist_set_row_height(&list, GAME_ROW_H);
        list.callback_draw_item = draw_game_row;
        gui_synclist_set_nb_items(&list, rows.count);
        gui_synclist_select_item(&list, MIN(selection, rows.count - 1));
        gui_synclist_draw_settled(&list);

        for (;;)
        {
            extras_list_do_action(HZ, &list, &action);
            if (action == ACTION_STD_OK)
            {
                selection = gui_synclist_get_sel_pos(&list);
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                icons_release();

                unsigned app_idx = rows.app[selection];
                bool failed = false;
                ret = launch(&apps[app_idx], true, &failed);
                if (ret == GO_TO_MUSIC_QUIZ)
                {
                    ret = root_menu_run_screen(ret);
                    if (ret == 0 || ret == GO_TO_PREVIOUS)
                    {
                        games_stats_record(app_idx);
                        ret = GO_TO_PREVIOUS;
                    }
                }
                else if (failed)
                {
                    ret = show_game_error(
                        str(LANG_FRUIT_GAMES_LAUNCH_ERROR),
                        str(LANG_FRUIT_GAMES_LAUNCH_ERROR_BODY));
                }
                else
                {
                    games_stats_record(app_idx);
                }

                if (ret != GO_TO_PREVIOUS)
                    open = false;
                break;
            }
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            {
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                ret = GO_TO_PREVIOUS;
                open = false;
                break;
            }
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                ret = GO_TO_ROOT;
                open = false;
                break;
            }
        }
    }

    icons_release();
    if (game_sub_font >= 0)
        font_unload(game_sub_font);
    game_sub_font = -1;
    ui_scene_close_modal(&scene, ret == GO_TO_PREVIOUS, "apps-list-pop");
    pop_current_activity();
    return ret;
}

int app_list_screen(enum app_list_kind kind)
{
    static const unsigned short titles[] = {
        [APP_LIST_GAMES] = LANG_PLUGIN_GAMES,
        [APP_LIST_OTHER] = LANG_APP_LIST_OTHER,
        [APP_LIST_DEMOS] = LANG_PLUGIN_DEMOS,
    };
    struct simplelist_info info;
    struct ui_scene scene = {0};
    int selection = 0, ret;

    collect(kind);
    if (!rows.count)
    {
        if (!fruit_menu_active())
            splash(HZ * 2, ID2P(LANG_NO_FILES));
        return GO_TO_PREVIOUS;
    }
    if (fruit_menu_active() && kind == APP_LIST_GAMES)
        return fruit_games_screen();
    icons.size = 0;
    if (fruit_menu_active())
        ui_scene_open_page(&scene, "apps-list-push");
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
        info.wps_on_play = !fruit_menu_active();
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
            ret = launch(&apps[rows.app[selection]], false, NULL);
            if (ret == GO_TO_MUSIC_QUIZ && fruit_menu_active())
            {
                ret = root_menu_run_screen(ret);
                if (ret == 0)
                    ret = GO_TO_PREVIOUS;
            }
            if (ret == GO_TO_PREVIOUS)
                continue;
        }
        break;
    }
    icons_release();
    ui_scene_close_modal(&scene, ret == GO_TO_PREVIOUS, "apps-list-pop");
    action_set_menu_back(menu_back);
    pop_current_activity();
    return ret;
}
