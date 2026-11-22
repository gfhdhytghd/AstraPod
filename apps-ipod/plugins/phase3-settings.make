# Staged Phase 3 utility adapters. GPL-2.0-or-later.
ROCK_PHASE3_SETTINGS := windows_lnk eq_optimizer tagnavi_edit random_folder_advance_config

$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_PHASE3_SETTINGS))): \
    $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
    $(APPSDIR)/plugins/stage-phase3-settings.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_PHASE3_SETTINGS))): \
    $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/eq_optimizer.rock: $(ROCK_OBJECTS)/lib/pluginlib_actions.o

$(ROCK_OUT)/staged/windows_lnk.c: $(APPSDIR)/plugins/windows-link-compat.inc
$(ROCK_OUT)/staged/random_folder_advance_config.c: $(APPSDIR)/plugins/random-folder-compat.inc
