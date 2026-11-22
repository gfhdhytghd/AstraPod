# RockDoom. GNU GPL version 2 or later.
ROCK_DOOM_SRC := $(call preprocess,$(ROCK_SRC)/doom/SOURCES)
ROCK_DOOM_OBJ := $(patsubst $(ROCK_SRC)/doom/%.c,$(ROCK_OBJECTS)/doom/%.o,$(ROCK_DOOM_SRC))
ROCK_DOOM_STAGED := i_system.c i_sound.c i_video.c rockdoom.c m_menu.c z_zone.c
ROCK_DOOM_FLAGS = $(ROCK_FLAGS) -I$(ROCK_SRC)/doom \
    -include $(APPSDIR)/plugins/doom-build.h -fgnu89-inline \
    -fno-strict-aliasing -Wno-strict-prototypes

$(addprefix $(ROCK_OUT)/staged/doom/,$(ROCK_DOOM_STAGED)): $(ROCK_OUT)/staged/doom/%: \
    $(ROCK_SRC)/doom/% $(APPSDIR)/plugins/stage-doom.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(addprefix $(ROCK_OBJECTS)/doom/,$(ROCK_DOOM_STAGED:.c=.o)): $(ROCK_OBJECTS)/doom/%.o: \
    $(ROCK_OUT)/staged/doom/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_DOOM_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/doom/%.o: $(ROCK_SRC)/doom/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_DOOM_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/doom.o: $(APPSDIR)/plugins/doom-launch.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(eval $(call rock_link_overlay,doom,$(ROCK_DOOM_OBJ) $(ROCK_OBJECTS)/lib/helper.o))
$(ROCK_OUT)/modules/games/doom.rock: $(ROCK_OUT)/modules/overlays/doom.ovl
-include $(wildcard $(ROCK_OBJECTS)/doom/*.d)
