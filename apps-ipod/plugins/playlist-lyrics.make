# Foreground CUE export and lyrics player adapters. GPL version 2 or later.
$(ROCK_OUT)/staged/cue_playlist.c $(ROCK_OUT)/staged/lrcplayer.c: \
    $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
    $(APPSDIR)/plugins/stage-playlist-lyrics.py \
    $(APPSDIR)/plugins/%-compat.inc $(APPSDIR)/plugins/rock-line-reader.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $(word 4,$^) $@

$(ROCK_OBJECTS)/cue_playlist.o $(ROCK_OBJECTS)/lrcplayer.o: \
    $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/lrcplayer.rock: \
    $(addprefix $(ROCK_OBJECTS)/lib/,helper.o playback_control.o pluginlib_actions.o)
