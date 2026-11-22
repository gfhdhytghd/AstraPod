# AstraPod .link reader and appender. GNU GPL version 2 or later.
ROCK_SHORTCUTS_STEMS := shortcuts_append shortcuts_view shortcuts_common

$(addprefix $(ROCK_OUT)/staged/shortcuts/,$(addsuffix .c,$(ROCK_SHORTCUTS_STEMS))): \
$(ROCK_OUT)/staged/shortcuts/%.c: $(ROCK_SRC)/shortcuts/%.c \
    $(APPSDIR)/plugins/stage-shortcuts.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_SHORTCUTS_STEMS))): \
$(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/shortcuts/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -I$(ROCK_SRC)/shortcuts \
	    -MMD -MP -c $< -o $@

$(addprefix $(ROCK_OUT)/modules/apps/,shortcuts_view.rock shortcuts_append.rock): \
    $(ROCK_OBJECTS)/shortcuts_common.o
