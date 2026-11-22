# Stock SDL 1.2 and Wolf3D behind the AstraPod foreground SDK.
WOLF_SRC := $(ROCK_SRC)/sdl
WOLF_STAGE := $(ROCK_OUT)/staged/wolf3d
WOLF_OBJ := $(ROCK_OBJECTS)/wolf3d
WOLF_HEADERS := $(patsubst $(WOLF_SRC)/include/%,$(WOLF_STAGE)/include/%,\
    $(filter-out %/stdio.h,$(wildcard $(WOLF_SRC)/include/*.h)))
WOLF_UNITS := $(patsubst $(WOLF_SRC)/%,%,$(filter $(WOLF_SRC)/src/%,\
    $(call preprocess,$(WOLF_SRC)/SOURCES))) \
    $(filter-out %/signon.c,$(shell cat $(WOLF_SRC)/SOURCES.wolf)) wrappers.c \
    $(addprefix SDL_mixer/,mixer.c music.c effects_internal.c effect_position.c effect_stereoreverse.c load_aiff.c load_voc.c)
WOLF_PATCHED := wrappers.c src/thread/rockbox/SDL_sysmutex.c \
    src/thread/rockbox/SDL_syssem.c src/video/rockbox/SDL_rockboxvideo.c \
    progs/wolf3d/fmopl_gpl.c progs/wolf3d/wl_main.c progs/wolf3d/id_sd.c \
    progs/wolf3d/id_pm.c progs/wolf3d/id_ca.c \
    progs/wolf3d/wl_menu.c
WOLF_BACKENDS := src/audio/rockbox/SDL_rockboxaudio.c \
    src/thread/rockbox/SDL_systhread.c src/timer/rockbox/SDL_systimer.c
WOLF_FLAGS = -I$(WOLF_STAGE)/include $(ROCK_FLAGS) \
    -I$(APPSDIR)/plugins -I$(ROOTDIR)/lib/tlsf/src -I$(WOLF_SRC) \
    -O2 -fno-strict-aliasing -fvisibility=hidden -DDECLSPEC= -Wno-undef \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -Wno-strict-prototypes -Wno-missing-field-initializers -Wno-sign-compare \
    -Wno-implicit-fallthrough -Wno-pointer-sign
WOLF_STAGE_TOOL := $(APPSDIR)/plugins/stage-wolf3d.py

$(WOLF_STAGE)/include/SDL_config_rockbox.h: $(WOLF_SRC)/include/SDL_config_rockbox.h $(WOLF_STAGE_TOOL)
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(filter-out %/SDL_config_rockbox.h,$(WOLF_HEADERS)): $(WOLF_STAGE)/include/%: $(WOLF_SRC)/include/%
	$(SILENT)mkdir -p $(@D)
	$(SILENT)cp $< $@

$(addprefix $(WOLF_STAGE)/,$(WOLF_PATCHED)): $(WOLF_STAGE)/%.c: $(WOLF_SRC)/%.c $(WOLF_STAGE_TOOL)
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(WOLF_OBJ)/progs/wolf3d/id_sd.o: $(APPSDIR)/plugins/wolf3d-digitized.inc
$(addprefix $(WOLF_OBJ)/,$(WOLF_PATCHED:.c=.o)): $(WOLF_OBJ)/%.o: $(WOLF_STAGE)/%.c $(WOLF_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC wolf3d/$(@F))$(CC) $(WOLF_FLAGS) -I$(dir $(WOLF_SRC)/$*.c) -MMD -MP -c $< -o $@

define wolf_backend
$(WOLF_OBJ)/$(1:.c=.o): $(APPSDIR)/plugins/wolf3d-$(notdir $(1)) $(WOLF_HEADERS) $(ROCK_FINGERPRINT)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,CC wolf3d/$$(@F))$$(CC) $$(WOLF_FLAGS) -I$(dir $(WOLF_SRC)/$(1)) -MMD -MP -c $$< -o $$@
endef
$(foreach unit,$(WOLF_BACKENDS),$(eval $(call wolf_backend,$(unit))))

$(WOLF_OBJ)/%.o: $(WOLF_SRC)/%.c $(WOLF_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC wolf3d/$(@F))$(CC) $(WOLF_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/wolf3d.o: $(APPSDIR)/plugins/wolf3d.c $(WOLF_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(WOLF_FLAGS) -MMD -MP -c $< -o $@

$(addprefix $(WOLF_STAGE)/,stdio_compat.c pluginlib_bmp.c): $(WOLF_STAGE)/%.c: $(ROCK_SRC)/lib/%.c $(WOLF_STAGE_TOOL)
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(addprefix $(WOLF_OBJ)/,stdio_compat.o pluginlib_bmp.o): $(WOLF_OBJ)/%.o: $(WOLF_STAGE)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC wolf3d/$(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(WOLF_OBJ)/crt0.o: $(ROCK_SRC)/plugin_crt0.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC wolf3d/$(@F))$(CC) $(ROCK_FLAGS) -fvisibility=hidden \
	    -DROCK_REQUEST_LARGE_STACK -MMD -MP -c $< -o $@

WOLF_LINK_INPUT := $(addprefix $(WOLF_OBJ)/,$(WOLF_UNITS:.c=.o)) \
    $(WOLF_OBJ)/crt0.o $(WOLF_OBJ)/stdio_compat.o $(WOLF_OBJ)/pluginlib_bmp.o \
    $(TLSFLIB) $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/games/wolf3d.rock: $(WOLF_LINK_INPUT)
$(ROCK_OUT)/modules/games/wolf3d.rock: private ROCK_LINK += -Wl,--exclude-libs,ALL
ifeq ($(APP_TYPE),sdl-sim)
$(ROCK_OUT)/modules/games/wolf3d.rock: private ROCK_LINK += -Wl,-Bsymbolic,-z,defs

.PHONY: wolf3d-host-probe
wolf3d-host-probe: $(ROCK_OUT)/wolf3d-probe.rock

$(WOLF_OBJ)/probe.o: $(ROCK_TOOLS)/wolf3d-probe.c $(WOLF_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(WOLF_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/wolf3d-probe.rock: $(ROCK_OBJECTS)/wolf3d.o $(WOLF_OBJ)/probe.o $(WOLF_LINK_INPUT)
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) \
	    -Wl,-Bsymbolic,-z,defs,--exclude-libs,ALL,--wrap=my_main -o $@ $^ -lgcc
endif

-include $(wildcard $(WOLF_OBJ)/*.d $(WOLF_OBJ)/*/*.d $(WOLF_OBJ)/*/*/*.d $(WOLF_OBJ)/*/*/*/*.d $(WOLF_OBJ)/*/*/*/*/*.d)
