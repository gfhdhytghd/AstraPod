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

#undef strcmp
#undef yield
#undef splashf

#define GAME_PATH PLUGIN_GAMES_DIR "/"
#define APP_PATH PLUGIN_APPS_DIR "/"
#define DEMO_PATH PLUGIN_DEMOS_DIR "/"
#define ACTION_HOOK "astrapod.action"

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
    if (active)
        lua_close(active);
    rb->lcd_scroll_stop();
    rb->lcd_set_viewport(NULL);
    active = NULL;
    lua_file_cleanup();
}

static void check_exit(void)
{
    if (rb->plugin_usb_taken())
        exit(PLUGIN_USB_CONNECTED);
}

static int get_lua_action(int timeout)
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
    int action = pluginlib_getaction(timeout, contexts, ARRAYLEN(contexts));
    if (IS_SYSEVENT(action))
        exit_on_usb(action);
    check_exit();
    return action;
}

static int lua_action(lua_State *L)
{
    lua_pushinteger(L, get_lua_action(luaL_checkint(L, 1)));
    return 1;
}

static int lua_get_action(lua_State *L)
{
    int context = luaL_checkint(L, 1);
    if (context != CONTEXT_STD && context != CONTEXT_WPS)
        return luaL_argerror(L, 1, "unsupported input context");
    int result = rb->get_action(context, luaL_checkint(L, 2));
    if (IS_SYSEVENT(result))
        exit_on_usb(result);
    check_exit();
    lua_pushinteger(L, result);
    return 1;
}

static int lua_button_get(lua_State *L)
{
    long result = rb->button_get(lua_toboolean(L, 1));
    if (IS_SYSEVENT(result))
        exit_on_usb(result);
    check_exit();
    lua_pushinteger(L, result);
    return 1;
}

static int lua_event_register(lua_State *L)
{
    if (rb->strcmp(luaL_checkstring(L, 1), "action"))
        return luaL_argerror(L, 1, "only foreground action events are supported");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_setfield(L, LUA_REGISTRYINDEX, ACTION_HOOK);
    return 0;
}

static int lua_yield_to_firmware(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, ACTION_HOOK);
    if (lua_isfunction(L, -1))
    {
        int action = get_lua_action(TIMEOUT_NOBLOCK);
        if (action != ACTION_NONE)
        {
            lua_pushinteger(L, action);
            lua_call(L, 1, 0);
        }
        else
            lua_pop(L, 1);
    }
    else
        lua_pop(L, 1);
    rb->yield();
    check_exit();
    return 0;
}

static int lua_menu(lua_State *L)
{
    const unsigned char *title = (const unsigned char *)luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int count = lua_objlen(L, 2);
    if (count < 1 || count > MENU_COUNT_MASK)
        return luaL_argerror(L, 2, "invalid menu size");

    const char **items = lua_newuserdata(L, count * sizeof(*items));
    int selected = luaL_optint(L, 3, 0);
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
    check_exit();
    lua_pushinteger(L, action);
    return 1;
}

static int lua_ticks(lua_State *L)
{
    lua_pushinteger(L, *rb->current_tick);
    return 1;
}

static int lua_sleep_ticks(lua_State *L)
{
    rb->sleep(luaL_checkint(L, 1));
    check_exit();
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
    if (IS_SYSEVENT(button))
        exit_on_usb(button);
    check_exit();
    lua_pushinteger(L, button);
    return 1;
}

static int lua_splash(lua_State *L)
{
    rb->splashf(luaL_checkint(L, 1), "%s", luaL_checkstring(L, 2));
    check_exit();
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

static int lua_read_setting(lua_State *L)
{
    const char *group = luaL_checkstring(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (!rb->strcmp(group, "global_settings") &&
        !rb->strcmp(name, "list_wraparound"))
        lua_pushinteger(L, rb->global_settings->list_wraparound);
    else
        lua_pushnil(L);
    return 1;
}

static int lua_actions(lua_State *L)
{
    static const struct lua_int_reg actions[] = {
        RB_CONSTANT(ACTION_NONE), RB_CONSTANT(ACTION_STD_CANCEL),
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

static int lua_settings(lua_State *L)
{
    lua_getglobal(L, "rb");
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "list_wraparound");
    lua_setfield(L, -2, "list_wraparound");
    lua_setfield(L, -2, "global_settings");
    lua_setfield(L, -2, "system");
    return 0;
}

static int lua_rbsettings(lua_State *L)
{
    lua_getglobal(L, "rb");
    lua_newtable(L);
    lua_pushcfunction(L, lua_read_setting);
    lua_setfield(L, -2, "read");
    lua_setfield(L, -2, "settings");
    return 0;
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
        {"backlight_force_on", lua_light_on},
        {"backlight_use_settings", lua_light_reset},
        {"file_exists", lua_exists},
        {"dir_exists", lua_directory_exists},
        {NULL, NULL},
    };
    static const struct lua_int_reg values[] = {
        RB_CONSTANT(LCD_WIDTH), RB_CONSTANT(LCD_HEIGHT), RB_CONSTANT(LCD_DEPTH),
        RB_CONSTANT(HZ), RB_CONSTANT(FONT_UI), RB_CONSTANT(FONT_SYSFIXED),
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
    lua_newtable(L);
    lua_pushcfunction(L, lua_event_register);
    lua_setfield(L, -2, "register");
    lua_setglobal(L, "rockev");
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
    if (!path || (rb->strcmp(path, GAME_PATH "picross.lua") &&
                  rb->strcmp(path, GAME_PATH "pixel-painter.lua") &&
                  rb->strcmp(path, GAME_PATH "boomshine.lua") &&
                  rb->strcmp(path, APP_PATH "stopwatch-lua.lua") &&
                  rb->strcmp(path, DEMO_PATH "helloworld.lua") &&
                  rb->strcmp(path, DEMO_PATH "test_viewports.lua")))
    {
        rb->splashf(HZ, "%s", "Choose a bundled Lua script.");
        return PLUGIN_ERROR;
    }

    size_t size;
    void *arena = rb->plugin_get_buffer(&size);
    if (!arena || size < 256 * 1024)
        return PLUGIN_ERROR;

    rb->memset(arena, 0, sizeof(uint32_t));
    if (init_memory_pool(size, arena) == (size_t)-1)
        return PLUGIN_ERROR;

    pixel_painter = !rb->strcmp(path, GAME_PATH "pixel-painter.lua");
    active = luaL_newstate();
    if (!active)
        return PLUGIN_ERROR;
    rb_atexit(cleanup);
    enum plugin_status result = PLUGIN_OK;
    lua_pushcfunction(active, run_game);
    lua_pushstring(active, path);
    if (lua_pcall(active, 1, 0, 0) != 0)
    {
        const char *reason = lua_tostring(active, -1);
        rb->view_text("Lua error", reason ? reason : "Lua stopped.");
        result = PLUGIN_ERROR;
    }
    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : result;
}
