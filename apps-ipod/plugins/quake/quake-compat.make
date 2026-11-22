# Build the stock software engine with the AstraPod SDL platform adapter.
QUAKE_PORT := $(APPSDIR)/plugins/quake
QUAKE_UPSTREAM := $(ROCK_SRC)/sdl
QUAKE_SRC := $(QUAKE_UPSTREAM)/progs/quake
QUAKE_STAGE := $(ROCK_OUT)/staged/quake
QUAKE_OBJ := $(ROCK_OBJECTS)/quake
QUAKE_STOCK := $(sort $(filter-out sys_sdl snd_sdl cd_rockbox,$(notdir \
    $(basename $(filter %.c,$(call preprocess,$(QUAKE_UPSTREAM)/SOURCES.quake))))) cd_null)
QUAKE_FLAGS = $(ROCK_FLAGS) -I$(QUAKE_STAGE) -I$(QUAKE_PORT) \
    -I$(QUAKE_SRC) -I$(QUAKE_UPSTREAM) -fcommon -fgnu89-inline \
    -fno-strict-aliasing -O2 -DSDL

$(QUAKE_STAGE)/%.c: $(QUAKE_SRC)/%.c $(QUAKE_PORT)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

QUAKE_HEADERS := $(addprefix $(QUAKE_STAGE)/,quakedef.h progs.h mathlib.h)

$(QUAKE_HEADERS): $(QUAKE_STAGE)/%.h: $(QUAKE_SRC)/%.h $(QUAKE_PORT)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

# Compile all stock sources from the stage so quoted headers use staged copies.
$(addprefix $(QUAKE_OBJ)/,$(addsuffix .o,$(QUAKE_STOCK))): $(QUAKE_OBJ)/%.o: \
    $(QUAKE_STAGE)/%.c $(QUAKE_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC quake/$(@F))$(CC) $(QUAKE_FLAGS) -MMD -MP -c $< -o $@

$(addprefix $(QUAKE_OBJ)/,quake_sys.o quake_io.o quake_sdl.o quake_audio.o): $(QUAKE_OBJ)/%.o: \
    $(QUAKE_PORT)/%.c $(QUAKE_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC quake/$(@F))$(CC) $(QUAKE_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/quake.o: $(QUAKE_PORT)/entry.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC Quake entry)$(CC) $(QUAKE_FLAGS) -MMD -MP -c $< -o $@

QUAKE_MODULE := $(ROCK_OUT)/modules/games/quake.rock
$(QUAKE_MODULE): $(addprefix $(QUAKE_OBJ)/,$(addsuffix .o,$(QUAKE_STOCK)) \
    quake_sys.o quake_io.o quake_sdl.o quake_audio.o) $(ROCK_OBJECTS)/plugin_crt0_large_stack.o

ifeq ($(APP_TYPE),sdl-sim)
$(QUAKE_MODULE): private ROCK_LINK += -Wl,--no-as-needed -lm
else
$(QUAKE_STAGE)/native_math.c: $(QUAKE_UPSTREAM)/wrappers.c $(QUAKE_PORT)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(QUAKE_OBJ)/native_math.o: $(QUAKE_STAGE)/native_math.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC quake/$(@F))$(CC) $(QUAKE_FLAGS) -MMD -MP -c $< -o $@

$(QUAKE_STAGE)/snd_mix_arm.S: $(QUAKE_SRC)/snd_mix_arm.S $(QUAKE_PORT)/stage.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(QUAKE_OBJ)/snd_mix_arm.o: $(QUAKE_STAGE)/snd_mix_arm.S $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,AS quake/$(@F))$(CC) $(QUAKE_FLAGS) -c $< -o $@

$(QUAKE_MODULE): $(QUAKE_OBJ)/native_math.o $(QUAKE_OBJ)/snd_mix_arm.o $(FIXEDPOINTLIB)
endif

-include $(wildcard $(QUAKE_OBJ)/*.d)
