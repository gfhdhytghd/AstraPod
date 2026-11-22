# Duke3D uses the stock engine and SDL declarations with a foreground backend.
DUKE_GLUE := $(APPSDIR)/plugins/duke3d
DUKE_SRC := $(ROCK_SRC)/sdl
DUKE_STAGE := $(ROCK_OUT)/staged/duke3d
DUKE_OBJ := $(ROCK_OBJECTS)/duke3d
DUKE_UNITS := $(filter-out %/dsl.c %/sdl_midi.c %/icon.c %/dummy_audiolib.c,\
    $(shell cat $(DUKE_SRC)/SOURCES.duke))
DUKE_UNITS += wrappers.c progs/duke3d/Game/src/audiolib/nomusic.c
DUKE_HEADERS := $(filter-out %/stdio.h,$(wildcard $(DUKE_SRC)/include/*.h \
    $(DUKE_SRC)/progs/duke3d/Engine/src/*.h \
    $(DUKE_SRC)/progs/duke3d/Game/src/*.h \
    $(DUKE_SRC)/progs/duke3d/Game/src/audiolib/*.h))
DUKE_CONFIG := $(DUKE_STAGE)/include/SDL_config_rockbox.h
DUKE_FLAGS = -I$(DUKE_STAGE)/include $(ROCK_FLAGS) \
    -I$(DUKE_GLUE) -I$(DUKE_SRC)/progs/duke3d/Game/src/audiolib \
    -I$(ROOTDIR)/lib/tlsf/src -DSDL -DELF -DDECLSPEC= \
    -include $(DUKE_GLUE)/duke-build.h -fno-strict-aliasing -fwrapv \
    -fgnu89-inline
# The stock 32-bit renderer passes a few pointers through integer registers.
DUKE_LEGACY_FLAGS := -Wno-error=incompatible-pointer-types -Wno-error=int-conversion

$(DUKE_STAGE)/%: $(DUKE_SRC)/% $(DUKE_GLUE)/stage.py $(DUKE_GLUE)/grp.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(DUKE_GLUE)/stage.py $< $@

.SECONDARY: $(addprefix $(DUKE_STAGE)/,$(DUKE_UNITS))

DUKE_STAGED_HEADERS := $(patsubst $(DUKE_SRC)/%,$(DUKE_STAGE)/%, \
    $(DUKE_HEADERS))

$(DUKE_CONFIG): | $(filter-out $(DUKE_CONFIG),$(DUKE_STAGED_HEADERS))

$(DUKE_STAGE)/stdio_compat.c: $(ROCK_SRC)/lib/stdio_compat.c $(DUKE_GLUE)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(DUKE_GLUE)/stage.py $< $@

$(DUKE_OBJ)/%.o: $(DUKE_STAGE)/%.c $(DUKE_STAGE)/include/SDL_config_rockbox.h $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC Duke3D/$*.c)$(CC) $(DUKE_FLAGS) $(DUKE_LEGACY_FLAGS) -MMD -MP -c $< -o $@

$(DUKE_OBJ)/glue/%.o: $(DUKE_GLUE)/%.c $(DUKE_STAGE)/include/SDL_config_rockbox.h $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC Duke3D/$*.c)$(CC) $(DUKE_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/duke3d.o: $(DUKE_GLUE)/entry.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -I$(ROOTDIR)/lib/tlsf/src -MMD -MP -c $< -o $@

# The stock CON interpreter uses 32-bit words that also hold pointers.
ifneq ($(APP_TYPE),sdl-sim)
DUKE_ENGINE := yes
else ifneq (,$(filter -m32,$(GCCOPTS)))
DUKE_ENGINE := yes
endif

ifeq ($(DUKE_ENGINE),yes)
$(ROCK_OUT)/modules/games/duke3d.rock: $(addprefix $(DUKE_OBJ)/,$(DUKE_UNITS:.c=.o)) \
    $(addprefix $(DUKE_OBJ)/,stdio_compat.o glue/io.o glue/sdl.o glue/pcm.o) \
    $(ROCK_OBJECTS)/plugin_crt0_large_stack.o $(TLSFLIB) $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/games/duke3d.rock: private ROCK_LINK += -Wl,--exclude-libs,ALL
-include $(wildcard $(addprefix $(DUKE_OBJ)/,$(DUKE_UNITS:.c=.d) stdio_compat.d glue/*.d))
else
$(ROCK_OUT)/modules/games/duke3d.rock: $(ROCK_OBJECTS)/plugin_crt0.o
endif
