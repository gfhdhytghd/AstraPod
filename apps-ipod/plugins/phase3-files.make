# Phase 3 foreground file utilities. GNU GPL version 2 or later.
ROCK_PHASE3_FILES := sort text_editor speedread otp
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_PHASE3_FILES))): $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
    $(APPSDIR)/plugins/stage-phase3-files.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OUT)/staged/sort.c: $(APPSDIR)/plugins/sort-compat.inc
$(ROCK_OUT)/staged/text_editor.c: $(APPSDIR)/plugins/text-editor-io.inc
$(ROCK_OUT)/staged/speedread.c: $(APPSDIR)/plugins/speedread-io.inc
$(ROCK_OUT)/staged/otp.c: $(addprefix $(APPSDIR)/plugins/,otp-io.inc otp-ui.inc)

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_PHASE3_FILES))): $(ROCK_OBJECTS)/%.o: \
    $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/text_editor.rock: $(ROCK_OBJECTS)/lib/playback_control.o
$(ROCK_OUT)/modules/apps/speedread.rock: $(addprefix $(ROCK_OBJECTS)/lib/,helper.o pluginlib_actions.o) $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/apps/otp.rock: $(addprefix $(ROCK_OBJECTS)/lib/,display_text.o sha1.o)
