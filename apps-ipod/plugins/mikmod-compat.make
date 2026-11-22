# Bounded foreground ProTracker playback. GNU GPL version 2 or later.
ROCK_MIKMOD_SRC := $(ROCK_SRC)/mikmod
ROCK_MIKMOD_OBJ := $(ROCK_OBJECTS)/mikmod
ROCK_MIKMOD_UNITS := drv_nos load_mod mdriver mloader mlutil mmalloc mmerror \
    mmio mplayer munitrk npertab sloader virtch
ROCK_MIKMOD_FLAGS := -I$(ROCK_MIKMOD_SRC) -I$(ROOTDIR)/lib/tlsf/src \
    -DMIKMOD_STATIC -DNO_HQMIXER -O2

$(ROCK_OUT)/staged/mikmod.c: $(ROCK_MIKMOD_SRC)/mikmod.c \
    $(APPSDIR)/plugins/stage-mikmod.py $(APPSDIR)/plugins/mikmod-compat.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $@

$(ROCK_OBJECTS)/mikmod.o: $(ROCK_OUT)/staged/mikmod.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_MIKMOD_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_MIKMOD_OBJ)/%.o: $(ROCK_MIKMOD_SRC)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_MIKMOD_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/mikmod.rock: \
    $(addprefix $(ROCK_MIKMOD_OBJ)/,$(addsuffix .o,$(ROCK_MIKMOD_UNITS))) $(TLSFLIB)
$(ROCK_OUT)/modules/apps/mikmod.rock: private ROCK_LINK += -Wl,--exclude-libs,ALL

-include $(wildcard $(ROCK_MIKMOD_OBJ)/*.d)
