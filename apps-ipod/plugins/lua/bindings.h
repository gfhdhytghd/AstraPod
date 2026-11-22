/* Foreground Lua bindings. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_LUA_BINDINGS_H
#define ASTRAPOD_LUA_BINDINGS_H

#include "lua.h"

void lua_register_bindings(lua_State *L);
int lua_settings(lua_State *L);
int lua_rbsettings(lua_State *L);
void lua_register_events(lua_State *L);
void lua_reset_events(void);
void lua_check_exit(void);
int lua_poll_action(int timeout);
int lua_sleep_ticks(lua_State *L);
int lua_yield_to_firmware(lua_State *L);

#endif
