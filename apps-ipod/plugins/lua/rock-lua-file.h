/* Files owned by the Lua viewer. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_LUA_FILE_H
#define ASTRAPOD_ROCK_LUA_FILE_H

int lua_file_open(const char *path, int flags, ...);
int lua_file_close(int fd, bool commit);
void lua_file_failed(int fd);
void lua_file_cleanup(void);
int lua_file_remove(const char *path);
bool lua_puzzle_path(const char *path);

#endif
