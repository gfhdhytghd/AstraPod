/* Typed adapters for the bundled Lua helpers. GNU GPL version 2 or later. */
#include "bindings.h"
#include "lauxlib.h"

#undef strcmp

static void integer_field(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

static void boolean_field(lua_State *L, const char *name, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}

static void string_field(lua_State *L, const char *name, const char *value)
{
    lua_pushstring(L, value);
    lua_setfield(L, -2, name);
}

static int lua_track(lua_State *L)
{
    const struct mp3entry *track = rb->audio_current_track();
    if (!track)
        return 0;

    lua_createtable(L, 0, 15);
    string_field(L, "path", track->path);
    string_field(L, "title", track->title);
    string_field(L, "artist", track->artist);
    string_field(L, "album", track->album);
    string_field(L, "albumartist", track->albumartist);
    string_field(L, "genre", track->genre_string);
    string_field(L, "composer", track->composer);
    string_field(L, "comment", track->comment);
    integer_field(L, "tracknum", track->tracknum);
    integer_field(L, "discnum", track->discnum);
    integer_field(L, "year", track->year);
    integer_field(L, "length", track->length);
    integer_field(L, "elapsed", track->elapsed);
    integer_field(L, "bitrate", track->bitrate);
    integer_field(L, "frequency", track->frequency);
    return 1;
}

static int settings_snapshot(lua_State *L, const char *group)
{
    if (!rb->strcmp(group, "audio_current_track"))
        return lua_track(L);

    lua_newtable(L);
    if (!rb->strcmp(group, "global_settings"))
    {
        boolean_field(L, "list_wraparound", rb->global_settings->list_wraparound);
        boolean_field(L, "playlist_shuffle", rb->global_settings->playlist_shuffle);
        boolean_field(L, "show_icons", rb->global_settings->show_icons);
        boolean_field(L, "talk_menu", rb->global_settings->talk_menu);
        integer_field(L, "repeat_mode", rb->global_settings->repeat_mode);
        integer_field(L, "max_files_in_playlist",
                      rb->global_settings->max_files_in_playlist);
        string_field(L, "font", (const char *)rb->global_settings->font_file);
        return 1;
    }
    if (!rb->strcmp(group, "global_status"))
    {
        integer_field(L, "volume", rb->lua_volume());
        integer_field(L, "audio_status", rb->audio_status());
        integer_field(L, "playlist_amount", rb->playlist_amount());
        return 1;
    }
    lua_pop(L, 1);
    return 0;
}

static int settings_dump(lua_State *L)
{
    return settings_snapshot(L, luaL_checkstring(L, 1));
}

static int settings_read(lua_State *L)
{
    const char *group = luaL_checkstring(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (!settings_snapshot(L, group))
        return 0;
    lua_getfield(L, -1, name);
    /* Picross uses the stock integer representation of this field. */
    if (!rb->strcmp(group, "global_settings") &&
        !rb->strcmp(name, "list_wraparound"))
        lua_pushinteger(L, lua_toboolean(L, -1));
    return 1;
}

int lua_settings(lua_State *L)
{
    lua_getglobal(L, "rb");
    lua_newtable(L);
    lua_newtable(L);
    string_field(L, "list_wraparound", "list_wraparound");
    lua_setfield(L, -2, "global_settings");
    lua_setfield(L, -2, "system");
    return 0;
}

int lua_rbsettings(lua_State *L)
{
    lua_getglobal(L, "rb");
    lua_newtable(L);
    lua_pushcfunction(L, settings_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, settings_dump);
    lua_setfield(L, -2, "dump");
    lua_setfield(L, -2, "settings");
    return 0;
}

static int lua_audio(lua_State *L)
{
    static const char *const names[] = {
        "status", "pause", "resume", "stop", "prev", "next",
        "seek", "elapsed", "length", NULL
    };
    int command = luaL_checkoption(L, 1, NULL, names);
    lua_check_exit();
    switch (command)
    {
        case 0:
            lua_pushinteger(L, rb->audio_status());
            return 1;
        case 1: rb->audio_pause(); break;
        case 2: rb->audio_resume(); break;
        case 3: rb->audio_stop(); break;
        case 4: rb->audio_prev(); break;
        case 5: rb->audio_next(); break;
        default:
        {
            long target = 0;
            if (command == 6)
                target = luaL_checklong(L, 2);
            const struct mp3entry *track = rb->audio_current_track();
            if (!track)
                return 0;
            if (command == 6)
            {
                if (track->length)
                    rb->audio_seek_now(MIN(MAX(target, 0), (long)track->length - 1));
                break;
            }
            lua_pushinteger(L, command == 7 ? track->elapsed : track->length);
            return 1;
        }
    }
    lua_check_exit();
    return 0;
}

static int lua_fade_volume(lua_State *L)
{
    int expected = luaL_checkint(L, 1);
    int value = luaL_checkint(L, 2);
    lua_pushboolean(L, rb->lua_fade_volume(expected, value));
    return 1;
}

static int lua_min_volume(lua_State *L)
{
    lua_pushinteger(L, rb->sound_min(SOUND_VOLUME));
    return 1;
}

static int lua_sleep_timer(lua_State *L)
{
    lua_pushboolean(L, rb->lua_sleep_timer(luaL_checkint(L, 1)));
    return 1;
}

static int lua_play_playlist(lua_State *L)
{
    lua_pushboolean(L, rb->lua_play_playlist());
    lua_check_exit();
    return 1;
}

static int lua_pick_file(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *extensions = luaL_optstring(L, 2, NULL);
    char path[MAX_PATH];
    rb->lcd_scroll_stop();
    rb->lcd_set_viewport(NULL);
    int result = rb->pick_file(path, sizeof(path), "/", title,
                               extensions, 0, false);
    lua_check_exit();
    if (result <= 0)
        return 0;
    lua_pushstring(L, path);
    return 1;
}

static int lua_file_preview(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int offset = luaL_optint(L, 2, 0);
    int limit = luaL_optint(L, 3, 512);
    luaL_argcheck(L, offset >= 0, 2, "invalid offset");
    luaL_argcheck(L, limit > 0 && limit <= 1024, 3, "invalid preview size");

    char bytes[1024];
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    off_t size = rb->ffilesize(fd);
    ssize_t length = -1;
    if (size >= offset && rb->lseek(fd, offset, SEEK_SET) == offset)
        length = rb->read(fd, bytes, limit);
    rb->close(fd);
    lua_check_exit();
    if (length < 0)
        return 0;
    lua_pushlstring(L, bytes, length);
    lua_pushnumber(L, size);
    return 2;
}

#define LIBRARY_BATCH 64

static int library_query(lua_State *L, int tag, int offset, int limit,
                         bool sample, unsigned int seed)
{
    /* Allocate everything before taking the tagcache read lease. */
    char (*rows)[TAGCACHE_BUFSZ] = lua_newuserdata(L, limit * sizeof(*rows));
    struct tagcache_search *search = lua_newuserdata(L, sizeof(*search));
    char value[TAGCACHE_BUFSZ];
    lua_check_exit();
    if (!rb->tagcache_search(search, tag))
    {
        lua_pushnil(L);
        lua_pushliteral(L, "Library unavailable. Build or update the Database.");
        return 2;
    }

    unsigned int seen = 0;
    int count = 0;
    int action = ACTION_NONE;
    while ((sample || count < limit) &&
           rb->tagcache_get_next(search, value, sizeof(value)))
    {
        if ((seen & 15) == 0)
        {
            action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
            if (IS_SYSEVENT(action) || action == ACTION_STD_CANCEL ||
                action == ACTION_STD_MENU)
                break;
        }

        unsigned int position = seen++;
        if (position < (unsigned int)offset)
            continue;
        if (sample && count == limit)
        {
            /* Reservoir sampling over the native filename query. */
            seed = 1664525u * seed + 1013904223u;
            position = (uint64_t)seed * seen >> 32;
            if (position >= (unsigned int)limit)
                continue;
        }
        else
            position = count++;
        rb->strlcpy(rows[position], value, sizeof(*rows));
    }
    bool failed = search->failed;
    rb->tagcache_search_finish(search);
    if (IS_SYSEVENT(action))
        exit_on_usb(action);
    lua_check_exit();

    if (failed || IS_SYSEVENT(action) ||
        action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
    {
        lua_pushnil(L);
        lua_pushstring(L, failed ? "Could not read the library." : "Canceled");
        return 2;
    }

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i)
    {
        lua_pushstring(L, rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushboolean(L, !sample && count == limit);
    return 2;
}

static int lua_library_tags(lua_State *L)
{
    static const char *const names[] = {
        "Artist", "Album artist", "Album", "Genre", "Composer", NULL
    };
    static const int tags[] = {
        tag_artist, tag_albumartist, tag_album, tag_genre, tag_composer
    };
    int tag = tags[luaL_checkoption(L, 1, NULL, names)];
    int offset = luaL_optint(L, 2, 0);
    luaL_argcheck(L, offset >= 0, 2, "invalid library offset");
    return library_query(L, tag, offset, LIBRARY_BATCH, false, 0);
}

static int lua_random_tracks(lua_State *L)
{
    int count = luaL_checkint(L, 1);
    luaL_argcheck(L, count > 0 && count <= 500, 1, "invalid playlist size");
    return library_query(L, tag_filename, 0, count, true, *rb->current_tick);
}

void lua_register_bindings(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"audio", lua_audio},
        {"track", lua_track},
        {"fade_volume", lua_fade_volume},
        {"minimum_volume", lua_min_volume},
        {"set_sleep_timer", lua_sleep_timer},
        {"play_random_playlist", lua_play_playlist},
        {"pick_file", lua_pick_file},
        {"file_preview", lua_file_preview},
        {"library_tags", lua_library_tags},
        {"random_tracks", lua_random_tracks},
        {NULL, NULL}
    };
    luaL_register(L, NULL, methods);
}
