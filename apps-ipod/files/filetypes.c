/***************************************************************************
 * Original code from RockBox
 * was: apps/filetypes.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The filetype registry: maps extensions to attributes, icons, colours and
 * the core viewer that opens them. Loads the colour and viewer theme
 * files.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "plugins/rock_compat.h"
#include "rbpaths.h"  /* THEME_DIR, ICON_DIR */
#include "string.h"

#include "settings/settings.h"
#include "debug.h"
#include "lang.h"
#include "kernel.h"
#include "system/app_util.h"
#include "system.h"
#include "draw/color.h"
#include "system/strutil.h"
#include "filetypes.h"
#include "viewers/text_viewer/text_viewer.h"
#include "viewers/image_viewer/image_viewer_pub.h"
#include "games/arcade/shell.h"
#include "dir.h"
#include "file.h"
#include "draw/icon_bitmaps.h"
#include "metadata_common.h"      /* mp4_has_video */
/*#define LOGF_ENABLE*/
#include "logf.h"

/* max filetypes (extensions & icons stored here) */
#define MAX_FILETYPES 192

static void fill_from_builtin(const char*,int) INIT_ATTR;
static void read_builtin_types_init(void) INIT_ATTR;

/* string array for known audio file types (tree_attr == FILE_ATTR_AUDIO) */
static const char* inbuilt_audio_filetypes[] = {
    "mp3", "mp2", "mpa", "mp1", "ogg", "oga", "wma", "wmv", "asf", "wav",
    "flac", "ac3", "a52", "mpc", "wv", "m4a", "m4b", "mp4", "mod", "mpga",
    "shn", "aif", "aiff", "spx", "opus", "sid", "adx", "nsf", "nsfe", "spc",
    "ape", "mac", "sap", "rm", "ra", "rmvb", "cmc", "cm3", "cmr", "cms", "dmc",
    "dlt", "mpt", "mpd", "rmt", "tmc", "tm8", "tm2", "oma", "aa3", "at3", "mmf",
    "au", "snd", "vox", "w64", "tta", "ay", "vtx", "gbs", "hes", "sgc", "vgm",
    "vgz", "kss", "aac", "dsf", "dff", "iso",
};

struct filetype_inbuilt {
    const char* extension;
    int tree_attr;
};

/* a table for the known file types, besides audio */
static const struct filetype_inbuilt inbuilt_filetypes[] = {
    { "m3u",  FILE_ATTR_M3U },
    { "m3u8", FILE_ATTR_M3U },
    { "cfg",  FILE_ATTR_CFG },
    { "log",  FILE_ATTR_LOG   },
    { "lng",  FILE_ATTR_LNG   },
    { "rock", FILE_ATTR_ROCK  },
    { "lua",  FILE_ATTR_LUA   },
    { "opx",  FILE_ATTR_OPX   },
    { "pbf",  FILE_ATTR_FONT  },
    { "kbd",  FILE_ATTR_KBD   },
    { "cue",  FILE_ATTR_CUE   },
#ifdef BOOTFILE_EXT
    { BOOTFILE_EXT,  FILE_ATTR_MOD },
#endif
    /* Types owned by the core-linked viewers. Compiled in here rather than
     * read from a config file, because the viewers are part of the binary. */
    { "txt",  FILE_ATTR_TXT },
    { "md",   FILE_ATTR_TXT },
    { "nfo",  FILE_ATTR_TXT },
    { "html", FILE_ATTR_TXT },
    { "htm",  FILE_ATTR_TXT },
    { "rtf",  FILE_ATTR_TXT },
    { "fb2",  FILE_ATTR_BOOK },
    { "epub", FILE_ATTR_BOOK },
    { "mobi", FILE_ATTR_BOOK },
    { "prc",  FILE_ATTR_BOOK },
    { "azw",  FILE_ATTR_BOOK },
    { "azw3", FILE_ATTR_BOOK },
    { "pdb",  FILE_ATTR_BOOK },
    { "docx", FILE_ATTR_TXT },
    { "pdf",  FILE_ATTR_TXT },
    /* Lyrics read as words here, with the timing stripped (ts_lyrics.c). The
     * synchronised viewer is reached from the WPS, not from the browser. */
    { "lrc",  FILE_ATTR_LYRICS },
    { "lrc8", FILE_ATTR_LYRICS },
    { "snc",  FILE_ATTR_LYRICS },
    { "bmp",  FILE_ATTR_IMG },
    { "jpg",  FILE_ATTR_IMG },
    { "jpe",  FILE_ATTR_IMG },
    { "jpeg", FILE_ATTR_IMG },
    { "png",  FILE_ATTR_IMG },
    { "ppm",  FILE_ATTR_IMG },
    { "gif",  FILE_ATTR_IMG },
    { "webp", FILE_ATTR_IMG },
    { "pbg",  FILE_ATTR_GAME },
#ifdef HAVE_VIDEO_PLAYER
    /* Not .mov: the player misreads QuickTime's sound descriptions */
    { "m4v",  FILE_ATTR_VIDEO },
#endif
};

#ifdef ASTRAPOD_ROCK_COMPAT
static const struct {
    const char *extension;
    const char *module;
} rock_viewers[] = {
    { "cells", PLUGIN_DEMOS_DIR "/rocklife.rock" },
    { "ch8", VIEWERS_DIR "/chip8.rock" },
    { "md5", PLUGIN_APPS_DIR "/md5sum.rock" },
    { "md5sum", PLUGIN_APPS_DIR "/md5sum.rock" },
    { "md5list", PLUGIN_APPS_DIR "/md5sum.rock" },
    { "lnk", VIEWERS_DIR "/windows_lnk.rock" },
    { "sok", PLUGIN_GAMES_DIR "/sokoban.rock" },
    { "ss", PLUGIN_GAMES_DIR "/sudoku.rock" },
    { "shopper", VIEWERS_DIR "/shopper.rock" },
    { "scrobbler", PLUGIN_APPS_DIR "/lastfm_scrobbler_viewer.rock" },
    { "link", PLUGIN_APPS_DIR "/shortcuts_view.rock" },
    { "pgn", PLUGIN_GAMES_DIR "/chessbox.rock" },
    { "sgf", PLUGIN_GAMES_DIR "/goban.rock" },
    { "ssg", PLUGIN_GAMES_DIR "/superdom.rock" },
};
#endif

struct fileattr_icon_voice {
    int tree_attr;
    uint16_t icon;
    int16_t voiceclip; /* -1 for types with no spoken name */
};

/* a table for the known file types icons & voice clips */
static const struct fileattr_icon_voice inbuilt_attr_icons_voices[] = {
    { FILE_ATTR_AUDIO, Icon_Audio,     VOICE_EXT_MPA },
    { FILE_ATTR_M3U,   Icon_Playlist,  LANG_PLAYLIST },
    { FILE_ATTR_CFG,   Icon_Config,    VOICE_EXT_CFG },
    { FILE_ATTR_LNG,   Icon_Language,  LANG_LANGUAGE },
    { FILE_ATTR_ROCK,  Icon_Plugin,    VOICE_EXT_ROCK },
    { FILE_ATTR_LUA,   Icon_Plugin,    VOICE_EXT_ROCK },
    { FILE_ATTR_OPX,   Icon_Plugin,    VOICE_EXT_ROCK },
    { FILE_ATTR_FONT,  Icon_Font,      VOICE_EXT_FONT },
    { FILE_ATTR_KBD,   Icon_Keyboard,  VOICE_EXT_KBD },
    { FILE_ATTR_CUE,   Icon_Bookmark,  VOICE_EXT_CUESHEET },
#if defined(BOOTFILE_EXT) || defined(BOOTFILE_EXT2)
    { FILE_ATTR_MOD,   Icon_Firmware,  VOICE_EXT_AJZ },
#endif
    /* Rendered by the native file list's icon font. */
    { FILE_ATTR_TXT,   Icon_Document, -1 },
    { FILE_ATTR_IMG,   Icon_Image, -1 },
    { FILE_ATTR_BOOK,  Icon_Book, -1 },
    { FILE_ATTR_LYRICS, Icon_Lyrics, -1 },
    { FILE_ATTR_GAME,  Icon_Plugin,    -1 },
    { FILE_ATTR_VIDEO, Icon_Video,     -1 },
#ifdef ASTRAPOD_ROCK_COMPAT
    { FILE_ATTR_ROCK_VIEWER, Icon_Plugin, -1 },
#endif
};

static int filetype_inbuilt_index(int tree_attr)
{
    size_t count = ARRAYLEN(inbuilt_attr_icons_voices);
    /* try to find a inbuilt index for the extension, if known */
    tree_attr &= FILE_ATTR_MASK; /* file type */

    for (size_t i = count - 1; i < count; i--)
    {
        if (tree_attr == inbuilt_attr_icons_voices[i].tree_attr)
        {
            logf("%s found attr %d id", __func__, tree_attr);
            return i;
        }
    }
    logf("%s not found attr %d", __func__, tree_attr);
    return -1;
}

long filetype_get_voiceclip(int attr)
{
    if (global_settings.talk_filetype)
    {
        int index = filetype_inbuilt_index(attr);
        if (index >= 0 && inbuilt_attr_icons_voices[index].voiceclip >= 0)
        {
            logf("%s found attr %d id %d", __func__, attr,
                 inbuilt_attr_icons_voices[index].voiceclip);
            return inbuilt_attr_icons_voices[index].voiceclip;
        }
    }
    logf("%s not found attr %d", __func__, attr);
    return -1;
}

struct file_type {
    enum themable_icons icon; /* the icon which shall be used for it, NOICON if unknown */
    unsigned char  attr; /* FILE_ATTR_MASK >> 8 */
    const char* extension; /* NULL for none */
};

static struct file_type filetypes[MAX_FILETYPES];

static int filetype_count = 0;

static int find_extension(const char* extension)
{
    if (extension)
    {
        for (int i=1; i<filetype_count; i++)
        {
            if (filetypes[i].extension &&
                !strcasecmp(extension, filetypes[i].extension))
                return i;
        }
    }
    return -1;
}

void filetype_init(void)
{
    /* set the directory item first */
    filetypes[0].extension = NULL;
    filetypes[0].attr   = 0;
    filetypes[0].icon   = Icon_Folder;

    filetype_count = 1;

    read_builtin_types_init();
}

static void fill_from_builtin(const char *ext, int tree_attr)
{
    if (filetype_count >= MAX_FILETYPES)
        return;

    struct file_type *filetype = &filetypes[filetype_count];
    filetype->icon = Icon_NOICON;
    filetype->attr   = tree_attr>>8;
    filetype->extension = ext;

    int index = filetype_inbuilt_index(tree_attr);
    if (index >= 0)
    {
        filetype->icon = inbuilt_attr_icons_voices[index].icon;
    }

    filetype_count++;
}

static void read_builtin_types_init(void)
{
    for(size_t i = 0; (i < ARRAYLEN(inbuilt_audio_filetypes)); i++)
    {
        fill_from_builtin(inbuilt_audio_filetypes[i], FILE_ATTR_AUDIO);
    }

    for(size_t i = 0; (i < ARRAYLEN(inbuilt_filetypes)); i++)
    {
        fill_from_builtin(inbuilt_filetypes[i].extension,
                          inbuilt_filetypes[i].tree_attr);
    }

#ifdef ASTRAPOD_ROCK_COMPAT
    /* Extension lookup returns the first match, so core types take priority. */
    for (size_t i = 0; i < ARRAYLEN(rock_viewers); i++)
        fill_from_builtin(rock_viewers[i].extension, FILE_ATTR_ROCK_VIEWER);
#endif
}

static int file_find_extension(const char* file)
{
    char *extension = strrchr(file, '.');
    if (extension)
        extension++;
    return find_extension(extension);
}

int filetype_get_attr(const char* file)
{
    int i = file_find_extension(file);
    if (i >= 0)
        return (filetypes[i].attr<<8)&FILE_ATTR_MASK;
    return 0;
}

static int find_attr(int attr)
{
    int i;
    /* skip the directory item */
    if ((attr & ATTR_DIRECTORY)==ATTR_DIRECTORY)
        return 0;
    for (i=1; i<filetype_count; i++)
    {
        if ((attr>>8) == filetypes[i].attr)
            return i;
    }
    return -1;
}

int filetype_get_icon(int attr)
{
    int index = find_attr(attr);
    if (index < 0)
        return Icon_NOICON;
    return filetypes[index].icon;
}

bool filetype_supported(int attr)
{
    return find_attr(attr) >= 0;
}

bool filetype_is_video(const char *file, int attr)
{
#ifdef HAVE_VIDEO_PLAYER
    const char *ext = strrchr(file, '.');
    bool video = false;
    int fd;

    if ((attr & FILE_ATTR_MASK) == FILE_ATTR_VIDEO)
        return true;
    if ((attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO || !ext
        || strcasecmp(ext, ".mp4"))
        return false;
    fd = open(file, O_RDONLY);
    if (fd >= 0)
    {
        video = mp4_has_video(fd);
        close(fd);
    }
    return video;
#else
    (void)file; (void)attr;
    return false;
#endif
}

/* Native file open dispatch, including the fixed compatibility viewers. */
bool filetype_open_core_viewer(int attr, const char *file, int *rc)
{
    switch (attr & FILE_ATTR_MASK)
    {
#ifdef HAVE_VIDEO_PLAYER
        case FILE_ATTR_AUDIO:
            if (!filetype_is_video(file, attr))
                return false;
            /* fall through */
        case FILE_ATTR_VIDEO:
            /* The player takes the codec for the film's sound only while
             * nothing else plays; otherwise the film is silent */
            if (audio_status())
                audio_stop();
            *rc = rock_open_from_browser(PLUGIN_APPS_DIR "/h264_player.rock",
                                         file);
            return true;
#endif
#ifdef ASTRAPOD_ROCK_COMPAT
        case FILE_ATTR_LUA:
            *rc = rock_open_from_browser(VIEWERS_DIR "/lua.rock", file);
            return true;
        case FILE_ATTR_ROCK:
            *rc = rock_open_from_browser(file, NULL);
            return true;
        case FILE_ATTR_ROCK_VIEWER:
        {
            int index = file_find_extension(file);
            if (index < 0 || filetypes[index].attr != (FILE_ATTR_ROCK_VIEWER >> 8))
                return false;

            for (size_t i = 0; i < ARRAYLEN(rock_viewers); i++)
            {
                if (!strcmp(filetypes[index].extension, rock_viewers[i].extension))
                {
                    *rc = rock_open_from_browser(rock_viewers[i].module, file);
                    return true;
                }
            }
            return false;
        }
#endif
        case FILE_ATTR_TXT:
        case FILE_ATTR_BOOK:
        case FILE_ATTR_LYRICS:
            *rc = text_viewer(file);
            return true;
        case FILE_ATTR_IMG:
            *rc = image_viewer(file);
            return true;
        case FILE_ATTR_GAME:
            *rc = arcade_play(file);
            return true;
    }
    return false;
}

