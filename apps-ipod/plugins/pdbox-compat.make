# Stock PDa engine with an AstraPod foreground entry. GPL-2.0-or-later.
ROCK_PDBOX_SRC := $(ROCK_SRC)/pdbox
ROCK_PDBOX_STAGE := $(ROCK_OUT)/staged/pdbox
ROCK_PDBOX_OBJ := $(ROCK_OBJECTS)/pdbox
ROCK_PDBOX_UNITS := $(filter-out pdbox.c pdbox-net.c PDa/src/s_audio_rockbox.c,\
    $(patsubst $(ROCK_PDBOX_SRC)/%,%,$(call preprocess,$(ROCK_PDBOX_SRC)/SOURCES)))
ROCK_PDBOX_NAMES := $(ROCK_PDBOX_UNITS:.c=.o)
ROCK_PDBOX_HEADERS := $(patsubst $(ROCK_PDBOX_SRC)/%,%,\
    $(wildcard $(ROCK_PDBOX_SRC)/*.h $(ROCK_PDBOX_SRC)/PDa/*/*.h))
ROCK_PDBOX_FLAGS := -DROCKBOX -I$(ROCK_PDBOX_STAGE) \
    -I$(APPSDIR)/plugins -fno-strict-aliasing -Wno-cast-function-type -fwrapv
ROCK_PDBOX_STAGER := $(APPSDIR)/plugins/stage-pdbox.py

$(addprefix $(ROCK_PDBOX_STAGE)/,$(ROCK_PDBOX_UNITS) $(ROCK_PDBOX_HEADERS)): \
    $(ROCK_PDBOX_STAGE)/%: $(ROCK_PDBOX_SRC)/% $(ROCK_PDBOX_STAGER)
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(ROCK_PDBOX_STAGER) $* $< $@

$(addprefix $(ROCK_PDBOX_OBJ)/,$(ROCK_PDBOX_NAMES)): \
    $(ROCK_PDBOX_OBJ)/%.o: $(ROCK_PDBOX_STAGE)/%.c $(ROCK_FINGERPRINT) \
    | $(addprefix $(ROCK_PDBOX_STAGE)/,$(ROCK_PDBOX_HEADERS))
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_PDBOX_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/pdbox.o $(ROCK_PDBOX_OBJ)/pdbox-patch.o: \
    $(ROCK_FINGERPRINT) | $(addprefix $(ROCK_PDBOX_STAGE)/,$(ROCK_PDBOX_HEADERS))

$(ROCK_OBJECTS)/pdbox.o: $(APPSDIR)/plugins/pdbox-compat.c
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_PDBOX_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_PDBOX_OBJ)/pdbox-patch.o: $(APPSDIR)/plugins/pdbox-patch.c
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_PDBOX_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/pdbox.rock: \
    $(addprefix $(ROCK_PDBOX_OBJ)/,$(ROCK_PDBOX_NAMES) pdbox-patch.o) $(TLSFLIB)
$(ROCK_OUT)/modules/apps/pdbox.rock: private ROCK_LINK += -Wl,--exclude-libs,ALL

-include $(foreach subdir,. PDa/src PDa/intern PDa/extra,$(wildcard $(ROCK_PDBOX_OBJ)/$(subdir)/*.d))
