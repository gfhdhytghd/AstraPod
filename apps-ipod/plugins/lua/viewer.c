/* Foreground launcher for bundled Rockbox Lua games. GPL-2.0-or-later. */
#include "plugin.h"
#include "root_menu.h"
#include "lib/helper.h"
#include "lib/pluginlib_actions.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luadir.h"
#include "rocklib.h"
#include "rocklib_img.h"
#include "rock-lua-file.h"
#include "bindings.h"
#include "paths.h"

#undef strcmp
#undef yield
#undef splashf

static const struct lua_entry {
    const char *path;
    const char *title;
} entries[] = {
#define LUA_ENTRY(category, file, title) {PLUGIN_DIR "/" #category "/" #file ".lua", title},
#include "entries.def"
#undef LUA_ENTRY
};

static const struct lua_entry *next_entry;
static void *pool;
static size_t pool_size;
static size_t lua_used;
static size_t lua_high;
static lua_Alloc original_alloc;
static void *original_alloc_context;

static lua_State *active;
static bool pixel_painter;

void *get_new_area(size_t *size)
{
    /* TLSF may ask for more memory. The plugin owns one arena only. */
    *size = 0;
    return (void *)~0u;
}

static void cleanup(void)
{
    if (!active)
        return;

    lua_reset_events();
    rb->lcd_scroll_stop();
    rb->lcd_set_viewport(NULL);
    lua_close(active);
    active = NULL;
    lua_file_cleanup();
}

void lua_check_exit(void)
{
    if (rb->plugin_usb_taken())
        exit(PLUGIN_USB_CONNECTED);
}

static int service_input(int input)
{
    if (IS_SYSEVENT(input) || (input & BUTTON_MULTIMEDIA))
    {
        /* The action layer returns multimedia keys as raw button codes. */
        exit_on_usb(input);
        input = ACTION_NONE;
    }
    lua_check_exit();
    return input;
}

int lua_poll_action(int timeout)
{
    static const struct button_mapping menu[] = {
        {PLA_CANCEL, BUTTON_MENU | BUTTON_REL, BUTTON_MENU},
        LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_PLUGIN),
    };
    static const struct button_mapping exit[] = {
        {PLA_EXIT, BUTTON_MENU | BUTTON_REL, BUTTON_MENU},
        LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_PLUGIN),
    };
    const struct button_mapping *contexts[] = {
        pixel_painter ? menu : exit, pla_main_ctx,
    };
    return service_input(pluginlib_getaction(timeout, contexts,
                                             ARRAYLEN(contexts)));
}

static int lua_action(lua_State *L)
{
    lua_pushinteger(L, lua_poll_action(luaL_checkint(L, 1)));
    return 1;
}

static int lua_get_action(lua_State *L)
{
    int context = luaL_checkint(L, 1);
    if (context != CONTEXT_STD && context != CONTEXT_WPS)
        return luaL_argerror(L, 1, "unsupported input context");
    int result = rb->get_action(context, luaL_checkint(L, 2));
    lua_pushinteger(L, service_input(result));
    return 1;
}

static int lua_button_get(lua_State *L)
{
    long result = rb->button_get(lua_toboolean(L, 1));
    service_input(result);
    lua_pushinteger(L, result);
    return 1;
}

static int lua_menu(lua_State *L)
{
    const unsigned char *title = (const unsigned char *)luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int count = lua_objlen(L, 2);
    if (count < 1 || count > MENU_COUNT_MASK)
        return luaL_argerror(L, 2, "invalid menu size");

    int selected = luaL_optint(L, 3, 0);
    const char **items = lua_newuserdata(L, count * sizeof(*items));
    if (selected < 0 || selected >= count)
        selected = 0;

    /* Keep menu strings on the Lua stack while native drawing borrows them. */
    luaL_checkstack(L, count, "menu strings");
    for (int i = 0; i < count; i++)
    {
        lua_rawgeti(L, 2, i + 1);
        items[i] = luaL_checkstring(L, -1);
    }

    struct menu_callback_with_desc desc = {NULL, (unsigned char *)title, Icon_NOICON};
    struct menu_item_ex menu = {
        MT_RETURN_ID | MENU_HAS_DESC | MENU_ITEM_COUNT(count),
        {.strings = items},
        {.callback_and_desc = &desc},
    };
    int action = rb->do_menu(&menu, &selected, NULL, false);
    lua_check_exit();
    lua_pushinteger(L, action);
    return 1;
}

static int lua_ticks(lua_State *L)
{
    lua_pushinteger(L, *rb->current_tick);
    return 1;
}

static void *account_alloc(void *context, void *ptr, size_t old, size_t size)
{
    size_t previous = ptr ? old : 0;
    void *result = original_alloc(context, ptr, old, size);
    if (result || !size)
    {
        lua_used = lua_used - previous + size;
        lua_high = MAX(lua_high, lua_used);
    }
    return result;
}

static int lua_mem_stats(lua_State *L)
{
    lua_pushinteger(L, lua_high);
    lua_pushinteger(L, lua_used);
    lua_pushinteger(L, pool_size > lua_used ? pool_size - lua_used : 0);
    return 3;
}

static int lua_scripts(lua_State *L)
{
    lua_createtable(L, ARRAYLEN(entries), 0);
    for (size_t i = 0; i < ARRAYLEN(entries); ++i)
    {
        lua_pushstring(L, entries[i].title);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int lua_launch_script(lua_State *L)
{
    int index = luaL_checkint(L, 1);
    if (index < 1 || index > (int)ARRAYLEN(entries))
        return luaL_argerror(L, 1, "unknown bundled script");
    next_entry = &entries[index - 1];
    return 0;
}

static int lua_clear_buttons(lua_State *L)
{
    (void)L;
    rb->button_clear_queue();
    return 0;
}

static int lua_wait_button(lua_State *L)
{
    int button = rb->button_get_w_tmo(luaL_checkint(L, 1));
    service_input(button);
    lua_pushinteger(L, button);
    return 1;
}

static int lua_splash(lua_State *L)
{
    rb->splashf(luaL_checkint(L, 1), "%s", luaL_checkstring(L, 2));
    lua_check_exit();
    return 0;
}

static int lua_view_text(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *text = luaL_checkstring(L, 2);
    rb->view_text(title, text);
    lua_check_exit();
    return 0;
}

static int lua_light_on(lua_State *L)
{
    (void)L;
    backlight_force_on();
    return 0;
}

static int lua_light_reset(lua_State *L)
{
    (void)L;
    backlight_use_settings();
    return 0;
}

static int lua_directory_exists(lua_State *L)
{
    DIR *dir = rb->opendir(luaL_checkstring(L, 1));
    lua_pushboolean(L, dir != NULL);
    if (dir)
        rb->closedir(dir);
    return 1;
}

static int lua_exists(lua_State *L)
{
    lua_pushboolean(L, rb->file_exists(luaL_checkstring(L, 1)));
    return 1;
}

static int lua_actions(lua_State *L)
{
    static const struct lua_int_reg actions[] = {
        RB_CONSTANT(ACTION_NONE), RB_CONSTANT(ACTION_STD_CANCEL),
        RB_CONSTANT(ACTION_REDRAW),
        RB_CONSTANT(ACTION_WPS_MENU), RB_CONSTANT(ACTION_WPS_PLAY),
        RB_CONSTANT(ACTION_WPS_SKIPNEXT), RB_CONSTANT(ACTION_WPS_SKIPPREV),
        RB_CONSTANT(PLA_UP), RB_CONSTANT(PLA_UP_REPEAT),
        RB_CONSTANT(PLA_DOWN), RB_CONSTANT(PLA_DOWN_REPEAT),
        RB_CONSTANT(PLA_LEFT), RB_CONSTANT(PLA_LEFT_REPEAT),
        RB_CONSTANT(PLA_RIGHT), RB_CONSTANT(PLA_RIGHT_REPEAT),
        RB_CONSTANT(PLA_EXIT), RB_CONSTANT(PLA_CANCEL),
        RB_CONSTANT(PLA_SELECT), RB_CONSTANT(PLA_SELECT_REPEAT),
        RB_CONSTANT(PLA_SELECT_REL),
    };
    lua_getglobal(L, "rb");
    lua_newtable(L);
    for (size_t i = 0; i < ARRAYLEN(actions); i++)
    {
        lua_pushinteger(L, actions[i].value);
        lua_setfield(L, -2, actions[i].name);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "actions");
    lua_newtable(L);
    lua_pushinteger(L, CONTEXT_STD);
    lua_setfield(L, -2, "CONTEXT_STD");
    lua_pushinteger(L, CONTEXT_WPS);
    lua_setfield(L, -2, "CONTEXT_WPS");
    lua_setfield(L, -3, "contexts");
    return 1;
}

static int lua_buttons(lua_State *L)
{
    static const struct lua_int_reg buttons[] = {
        RB_CONSTANT(BUTTON_NONE), RB_CONSTANT(BUTTON_REL), RB_CONSTANT(BUTTON_REPEAT),
        RB_CONSTANT(BUTTON_MENU), RB_CONSTANT(BUTTON_SELECT), RB_CONSTANT(BUTTON_PLAY),
        RB_CONSTANT(BUTTON_LEFT), RB_CONSTANT(BUTTON_RIGHT),
        RB_CONSTANT(BUTTON_SCROLL_BACK), RB_CONSTANT(BUTTON_SCROLL_FWD),
#ifdef BUTTON_RC_MENU
        RB_CONSTANT(BUTTON_RC_MENU), RB_CONSTANT(BUTTON_RC_SELECT),
        RB_CONSTANT(BUTTON_RC_PLAY), RB_CONSTANT(BUTTON_RC_STOP),
        RB_CONSTANT(BUTTON_RC_LEFT), RB_CONSTANT(BUTTON_RC_RIGHT),
        RB_CONSTANT(BUTTON_RC_UP), RB_CONSTANT(BUTTON_RC_DOWN),
        RB_CONSTANT(BUTTON_RC_VOL_UP), RB_CONSTANT(BUTTON_RC_VOL_DOWN),
#endif
    };
    lua_getglobal(L, "rb");
    lua_newtable(L);
    for (size_t i = 0; i < ARRAYLEN(buttons); i++)
    {
        lua_pushinteger(L, buttons[i].value);
        lua_setfield(L, -2, buttons[i].name);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "buttons");
    return 1;
}

static int lua_rock(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"get_plugin_action", lua_action},
        {"get_action", lua_get_action},
        {"button_get", lua_button_get},
        {"current_tick", lua_ticks},
        {"sleep", lua_sleep_ticks},
        {"yield", lua_yield_to_firmware},
        {"button_clear_queue", lua_clear_buttons},
        {"button_get_w_tmo", lua_wait_button},
        {"do_menu", lua_menu},
        {"splash", lua_splash},
        {"view_text", lua_view_text},
        {"backlight_force_on", lua_light_on},
        {"backlight_use_settings", lua_light_reset},
        {"file_exists", lua_exists},
        {"dir_exists", lua_directory_exists},
        {"mem_stats", lua_mem_stats},
        {"scripts", lua_scripts},
        {"launch_script", lua_launch_script},
        {NULL, NULL},
    };
    static const struct lua_int_reg values[] = {
        RB_CONSTANT(LCD_WIDTH), RB_CONSTANT(LCD_HEIGHT), RB_CONSTANT(LCD_DEPTH),
        RB_CONSTANT(HZ), RB_CONSTANT(FONT_UI), RB_CONSTANT(FONT_SYSFIXED),
        RB_CONSTANT(PLUGIN_GOTO_WPS),
        RB_CONSTANT(AUDIO_STATUS_PLAY), RB_CONSTANT(AUDIO_STATUS_PAUSE),
        RB_CONSTANT(PLAYBACK_EVENT_TRACK_CHANGE),
    };
    luaL_register(L, "rb", methods);
    for (size_t i = 0; i < ARRAYLEN(values); i++)
    {
        lua_pushinteger(L, values[i].value);
        lua_setfield(L, -2, values[i].name);
    }
    lua_pushliteral(L, PLUGIN_GAMES_DATA_DIR);
    lua_setfield(L, -2, "PLUGIN_GAMES_DATA_DIR");
    lua_pushliteral(L, PLUGIN_APPS_DATA_DIR);
    lua_setfield(L, -2, "PLUGIN_APPS_DATA_DIR");
    lua_pushliteral(L, PLUGIN_DEMOS_DATA_DIR);
    lua_setfield(L, -2, "PLUGIN_DEMOS_DATA_DIR");
    lua_pushliteral(L, ROCK_LUA_PLAYLIST_DIR ROCK_LUA_PLAYLIST);
    lua_setfield(L, -2, "RANDOM_PLAYLIST");
    lua_register_bindings(L);
    lua_register_events(L);
    return 1;
}

static void preload(lua_State *L, const char *name, lua_CFunction load)
{
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, load);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);
}

static int run_game(lua_State *L)
{
    const char *path = lua_tostring(L, 1);
    static const luaL_Reg libraries[] = {
        {"", luaopen_base},
        {LUA_LOADLIBNAME, luaopen_package},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_BITLIBNAME, luaopen_bit},
        {LUA_IOLIBNAME, luaopen_io},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_OSLIBNAME, luaopen_os},
        {LUA_ROCKLIBNAME, lua_rock},
        {LUA_ROCKLIBNAME, luaopen_rock_img},
        {LUA_DIRLIBNAME, luaopen_luadir},
    };
    for (size_t i = 0; i < ARRAYLEN(libraries); i++)
    {
        lua_pushcfunction(L, libraries[i].func);
        lua_pushstring(L, libraries[i].name);
        lua_call(L, 1, 0);
    }
    preload(L, "actions", lua_actions);
    preload(L, "buttons", lua_buttons);
    preload(L, "settings", lua_settings);
    preload(L, "rbsettings", lua_rbsettings);

    if (luaL_loadfile(L, path) != 0)
        return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    const struct lua_entry *entry = NULL;
    for (size_t i = 0; path && i < ARRAYLEN(entries); ++i)
        if (!rb->strcmp(path, entries[i].path))
        {
            entry = &entries[i];
            break;
        }
    if (!entry)
    {
        rb->splashf(HZ, "%s", "Choose a bundled Lua script.");
        return PLUGIN_ERROR;
    }

    pool = rb->plugin_get_buffer(&pool_size);
    if (!pool || pool_size < 256 * 1024)
        return PLUGIN_ERROR;

    rb_atexit(cleanup);
    do
    {
        path = entry->path;
        next_entry = NULL;
        rb->memset(pool, 0, sizeof(uint32_t));
        if (init_memory_pool(pool_size, pool) == (size_t)-1)
            return PLUGIN_ERROR;

        pixel_painter = !rb->strcmp(path, PLUGIN_GAMES_DIR "/pixel-painter.lua");
        active = luaL_newstate();
        if (!active)
            return PLUGIN_ERROR;
        lua_used = lua_gc(active, LUA_GCCOUNT, 0) * 1024u +
                   lua_gc(active, LUA_GCCOUNTB, 0);
        lua_high = lua_used;
        original_alloc = lua_getallocf(active, &original_alloc_context);
        lua_setallocf(active, account_alloc, original_alloc_context);

        lua_pushcfunction(active, run_game);
        lua_pushstring(active, path);
        if (lua_pcall(active, 1, 0, 0) != 0)
        {
            const char *reason = lua_tostring(active, -1);
            rb->view_text("Lua error", reason ? reason : "Lua stopped.");
            return PLUGIN_ERROR;
        }
        lua_check_exit();
        cleanup();
        entry = next_entry;
    } while (entry);

    return PLUGIN_OK;
}
