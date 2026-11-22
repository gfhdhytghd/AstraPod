# DRAM Pacbox source build. GNU GPL version 2 or later.
ROCK_PACBOX_SRC := $(call preprocess,$(ROCK_SRC)/pacbox/SOURCES)
ROCK_PACBOX_OBJ := $(patsubst $(ROCK_SRC)/pacbox/%.c,$(ROCK_OBJECTS)/pacbox/%.o,$(filter-out %/pacbox.c,$(ROCK_PACBOX_SRC)))

$(ROCK_OUT)/staged/pacbox.c: $(ROCK_SRC)/pacbox/pacbox.c \
    $(APPSDIR)/plugins/stage-pacbox.py $(APPSDIR)/plugins/pacbox-config.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $@

$(ROCK_OBJECTS)/pacbox.o: $(ROCK_OUT)/staged/pacbox.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -I$(ROCK_SRC)/pacbox -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/pacbox/%.o: $(ROCK_SRC)/pacbox/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(filter-out -O%,$(ROCK_FLAGS)) -O2 -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/games/pacbox.rock: $(ROCK_PACBOX_OBJ) $(ROCK_OBJECTS)/lib/helper.o
-include $(wildcard $(ROCK_OBJECTS)/pacbox/*.d)
