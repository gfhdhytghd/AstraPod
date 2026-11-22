# Build the Lua VM and the AstraPod Lua viewer. GPL-2.0-or-later.
ROCK_LUA_SRC := $(ROCK_SRC)/lua
ROCK_LUA_ADAPTER := $(APPSDIR)/plugins/lua
ROCK_LUA_STAGE := $(ROCK_OUT)/lua-src
ROCK_LUA_OBJ := $(ROCK_OBJECTS)/lua
ROCK_LUA_EXCLUDE := rocklib.c rocklib_events.c rocklua.c tlsf_helper.c strftime.c
ROCK_LUA_SOURCES := $(filter-out $(ROCK_LUA_EXCLUDE),$(notdir $(call preprocess,$(ROCK_LUA_SRC)/SOURCES)))
ROCK_LUA_OBJECTS := $(addprefix $(ROCK_LUA_OBJ)/,$(ROCK_LUA_SOURCES:.c=.o))
ROCK_LUA_HEADERS := $(patsubst $(ROCK_LUA_SRC)/%,$(ROCK_LUA_STAGE)/%,$(wildcard $(ROCK_LUA_SRC)/*.h))
ROCK_LUA_FLAGS := -I$(ROCK_LUA_STAGE) -I$(ROCK_LUA_ADAPTER) -I$(ROCK_LUA_SRC)

$(ROCK_LUA_STAGE)/%.h: $(ROCK_LUA_SRC)/%.h $(ROCK_LUA_ADAPTER)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_LUA_STAGE)/%.c: $(ROCK_LUA_SRC)/%.c $(ROCK_LUA_ADAPTER)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_LUA_OBJECTS): $(ROCK_LUA_OBJ)/%.o: $(ROCK_LUA_STAGE)/%.c $(ROCK_LUA_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC lua/$(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LUA_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_LUA_OBJ)/file.o: $(ROCK_LUA_ADAPTER)/file.c $(ROCK_LUA_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC lua/$(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LUA_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/lua.o: $(ROCK_LUA_ADAPTER)/viewer.c $(ROCK_LUA_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LUA_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/viewers/lua.rock: $(ROCK_LUA_OBJECTS) $(ROCK_LUA_OBJ)/file.o \
    $(ROCK_OBJECTS)/lib/pluginlib_actions.o $(ROCK_OBJECTS)/lib/helper.o $(TLSFLIB)
$(ROCK_OUT)/modules/viewers/lua.rock: private ROCK_LINK += -Wl,--exclude-libs,ALL
-include $(wildcard $(ROCK_LUA_OBJ)/*.d)
