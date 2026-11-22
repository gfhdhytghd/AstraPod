# Native specialist service adapters. GNU GPL version 2 or later.
ROCK_SPECIALISTS := file_picker main_menu_config pitch_screen properties theme_remove
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_SPECIALISTS))): $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c $(APPSDIR)/plugins/stage-specialist.py $(APPSDIR)/plugins/specialist-entry.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $@

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_SPECIALISTS))): $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/file_picker.rock: $(ROCK_OBJECTS)/lib/arg_helper.o
