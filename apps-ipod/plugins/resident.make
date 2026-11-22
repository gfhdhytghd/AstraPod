# Foreground controls for the firmware-owned resident services.
ROCK_RESIDENT_MODULES := battery_bench announce_status
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_RESIDENT_MODULES))): \
    $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
        $(APPSDIR)/plugins/%-compat.inc $(APPSDIR)/plugins/stage-resident.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 3,$^) $< $(word 2,$^) $@

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_RESIDENT_MODULES))): \
    $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@
