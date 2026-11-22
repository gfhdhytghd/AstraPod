# AstraPod ports of Rockpaint, Sliding Puzzle and Sokoban. GPL version 2+.
PHASE3_GAME_STAGE := $(APPSDIR)/plugins/stage-phase3-games.py
PHASE3_GAME_NAMES := rockpaint sliding_puzzle sokoban lib/pluginlib_bmp

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(PHASE3_GAME_NAMES))): \
    $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(PHASE3_GAME_NAMES))): \
    $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c $(PHASE3_GAME_STAGE)
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OUT)/staged/sokoban.c: $(APPSDIR)/plugins/sokoban-file.inc
$(ROCK_OUT)/staged/rockpaint.c: $(APPSDIR)/plugins/rockpaint-image.inc
$(ROCK_OBJECTS)/sokoban.o $(ROCK_OBJECTS)/rockpaint.o: \
    ROCK_EXTRA_INCLUDES := -I$(APPSDIR)/plugins

$(eval $(call rock_module_bitmaps,apps/rockpaint,rockpaint rockpaint_hsvrgb))
$(eval $(call rock_module_bitmaps,games/sliding_puzzle,sliding_puzzle))
$(eval $(call rock_module_bitmaps,games/sokoban,sokoban_tiles))

$(ROCK_OUT)/modules/apps/rockpaint.rock: \
    $(addprefix $(ROCK_OBJECTS)/lib/,pluginlib_bmp.o rgb_hsv.o playback_control.o)
$(ROCK_OUT)/modules/games/sokoban.rock: $(ROCK_OBJECTS)/lib/playback_control.o
