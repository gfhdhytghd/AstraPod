# Disk Tidy.
$(ROCK_OUT)/staged/disktidy.c: $(ROCK_SRC)/disktidy.c $(APPSDIR)/plugins/disktidy-compat.inc $(APPSDIR)/plugins/stage-disktidy.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 3,$^) $< $(word 2,$^) $@

$(ROCK_OBJECTS)/disktidy.o: $(ROCK_OUT)/staged/disktidy.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/disktidy.rock: $(ROCK_OBJECTS)/lib/playback_control.o
