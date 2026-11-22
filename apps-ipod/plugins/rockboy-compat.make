# Standalone Rockboy candidate. GPL-2.0-or-later.
ROCK_GB_SRC := $(ROCK_SRC)/rockboy
ROCK_GB_STAGE := $(ROCK_OUT)/staged/rockboy
ROCK_GB_OBJ := $(ROCK_OBJECTS)/rockboy
ROCK_GB_UNITS := cpu emu events fastmem hw lcd lcdc loader mem rbsound rtc sound sys_rockbox
ROCK_GB_HEADERS := $(patsubst $(ROCK_GB_SRC)/%,$(ROCK_GB_STAGE)/%,$(wildcard $(ROCK_GB_SRC)/*.h))
ROCK_GB_ADAPTERS := emu loader rbsound rockboy sound sys_rockbox
ROCK_GB_FLAGS = $(ROCK_FLAGS) -I$(ROCK_GB_SRC) -fno-strict-aliasing \
    -include $(APPSDIR)/plugins/rockboy-build.h

$(ROCK_GB_STAGE)/%.h: $(ROCK_GB_SRC)/%.h
	$(SILENT)mkdir -p $(@D)
	$(SILENT)cp $< $@

$(addprefix $(ROCK_GB_STAGE)/,$(addsuffix .c,$(ROCK_GB_ADAPTERS))): $(ROCK_GB_STAGE)/%.c: \
    $(ROCK_GB_SRC)/%.c $(APPSDIR)/plugins/stage-rockboy.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@
$(ROCK_GB_STAGE)/rockboy.c: $(APPSDIR)/plugins/rockboy-compat.inc
$(ROCK_GB_STAGE)/rbsound.c: $(APPSDIR)/plugins/rockboy-pcm.inc

$(addprefix $(ROCK_GB_OBJ)/,$(addsuffix .o,$(filter-out rockboy,$(ROCK_GB_ADAPTERS)))): $(ROCK_GB_OBJ)/%.o: $(ROCK_GB_STAGE)/%.c $(ROCK_GB_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC rockboy/$(@F))$(CC) $(ROCK_GB_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_GB_OBJ)/%.o: $(ROCK_GB_SRC)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC rockboy/$(@F))$(CC) $(ROCK_GB_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/rockboy.o: $(ROCK_GB_STAGE)/rockboy.c $(ROCK_GB_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_GB_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/games/rockboy.rock: \
    $(addprefix $(ROCK_GB_OBJ)/,$(addsuffix .o,$(ROCK_GB_UNITS))) $(ROCK_OBJECTS)/lib/helper.o
-include $(wildcard $(ROCK_GB_OBJ)/*.d)
