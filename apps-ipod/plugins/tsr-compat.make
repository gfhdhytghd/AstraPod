# Executable TSR images. GNU GPL version 2 or later.
SRC += $(APPSDIR)/plugins/rock_tsr.c

ROCK_TSR_LINK = $(ROCK_LINK)
ROCK_TSR_LINK_INPUT = $(ROCK_LINK_INPUT)
ifneq ($(APP_TYPE),sdl-sim)
ROCK_TSR_SCRIPT := $(ROCK_OUT)/tsr.link
ROCK_TSR_ORIGIN = $(shell $(shell $(CC) -print-prog-name=nm) -n \
    $(BUILDDIR)/rockbox.elf | awk '$$3 == "rock_tsr_arena" {print "0x" $$1}')
ROCK_TSR_LINK = -nostdlib -Wl,--gc-sections \
    -Wl,--defsym,rock_tsr_origin=$(ROCK_TSR_ORIGIN) -T$(ROCK_TSR_SCRIPT)
ROCK_TSR_LINK_INPUT = $(filter-out $(ROCK_OUT)/plugin.link,$(ROCK_LINK_INPUT)) \
    $(ROCK_TSR_SCRIPT) $(BUILDDIR)/rockbox.elf

$(ROCK_TSR_SCRIPT): $(APPSDIR)/plugins/plugin.lds $(APPSDIR)/plugins/rock_tsr.h \
    $(FIRMDIR)/export/config/$(MODELNAME).h
	$(SILENT)mkdir -p "$(@D)"
	$(call preprocess2file,$<,$@,-DPLUGIN -DASTRAPOD_ROCK -DASTRAPOD_TSR)
endif

$(ROCK_OBJECTS)/tsr_crt0.o: $(ROCK_SRC)/plugin_crt0.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -DROCK_TSR_IMAGE -MMD -MP -c $< -o $@

# Invoke explicitly. Neither image belongs to the package manifest.
$(ROCK_OBJECTS)/tsr-probe-%.o: $(TOOLSDIR)/tests/fixtures/rock-tsr/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

# Both the suffix and the CRT header select the TSR loader.
define rock_link_tsr
$(ROCK_OUT)/modules/$(1).tsr.rock: $(2) $(ROCK_OBJECTS)/tsr_crt0.o $(ROCK_TSR_LINK_INPUT)
	$$(SILENT)mkdir -p "$$(@D)"
	$$(call PRINTS,LD $$(@F))$$(CC) $$(ROCK_FLAGS) $$(ROCK_TSR_LINK) \
	    -o $$@.elf $$(filter %.o %.a,$$^) -lgcc
	$$(SILENT)$$(ROCK_OUTPUT)
endef

$(eval $(call rock_link_tsr,demos/tsr_probe,$(ROCK_OBJECTS)/tsr-probe-resident.o))

$(ROCK_OUT)/modules/demos/tsr_foreground_probe.rock: $(ROCK_OBJECTS)/tsr-probe-foreground.o \
    $(ROCK_OBJECTS)/plugin_crt0.o $(ROCK_LINK_INPUT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) \
	    -o $@.elf $(filter %.o %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)
