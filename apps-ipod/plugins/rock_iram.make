# Dedicated opt-in IRAM probe. GPL version 2 or later.
SRC += $(APPSDIR)/plugins/rock_iram.c $(APPSDIR)/plugins/rock_iram_hooks.c

ROCK_IRAM_HOOKS := codec_load_buf codec_load_file codec_close audio_play \
    default_event_handler default_event_handler_ex
ROCK_IRAM_WRAP := $(foreach symbol,$(ROCK_IRAM_HOOKS),-Xlinker --wrap=$(symbol))
ifeq ($(APP_TYPE),sdl-sim)
LDOPTS += $(ROCK_IRAM_WRAP)
else
CORE_LDOPTS += $(ROCK_IRAM_WRAP)
endif

ROCK_IRAM_PROBE := $(ROCK_OUT)/modules/demos/iram_probe.rock
ifeq ($(APP_TYPE),sdl-sim)
ROCK_IRAM_LINK = $(ROCK_LINK)
else
ROCK_IRAM_INPUT := $(ROCK_OUT)/plugin-iram.link
ROCK_IRAM_LINK = -nostdlib -T$(ROCK_IRAM_INPUT) -Wl,--gc-sections
$(ROCK_IRAM_INPUT): $(APPSDIR)/plugins/plugin.lds $(APPSDIR)/plugins/rock_iram_layout.h $(FIRMDIR)/export/config/$(MODELNAME).h
	$(SILENT)mkdir -p "$(@D)"
	$(call preprocess2file,$<,$@,-DPLUGIN -DASTRAPOD_ROCK -DROCK_REQUEST_IRAM)
endif

$(ROCK_OBJECTS)/iram-probe.o: $(APPSDIR)/plugins/examples/iram_probe.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -DROCK_REQUEST_IRAM -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/iram-crt0.o: $(ROCK_SRC)/plugin_crt0.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -DROCK_REQUEST_IRAM -MMD -MP -c $< -o $@

$(ROCK_IRAM_PROBE): $(ROCK_OBJECTS)/iram-probe.o $(ROCK_OBJECTS)/iram-crt0.o $(ROCK_IRAM_INPUT) $(ROCK_LINK_INPUT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_IRAM_LINK) -o $@.elf $(filter %.o %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)

.PHONY: rock-iram-probe
rock-iram-probe: $(ROCK_IRAM_PROBE)
