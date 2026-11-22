# Checked DRAM overlays. GNU GPL version 2 or later.
ROCK_OVERLAY_CRT := $(ROCK_OBJECTS)/overlay_crt0.o
$(ROCK_OVERLAY_CRT): $(ROCK_SRC)/plugin_crt0.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -DROCK_OVERLAY_IMAGE -MMD -MP -c $< -o $@

# Both the launcher and overlay are linked for the foreground arena base.
# The launcher returns and the runtime unloads it before loading the overlay.
define rock_link_overlay
$(ROCK_OUT)/modules/overlays/$(1).ovl: $(2) $(ROCK_OVERLAY_CRT) $(ROCK_LINK_INPUT)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,LD $$(@F))$$(CC) $$(ROCK_FLAGS) $$(ROCK_LINK) -o $$@.elf $$(filter %.o %.a,$$^) -lgcc
	$$(SILENT)$$(ROCK_OUTPUT)
endef

# The probe is built only when its two output paths are requested explicitly.
ROCK_OVERLAY_PROBE_DIR := $(TOOLSDIR)/tests/fixtures/rock-overlay
$(ROCK_OBJECTS)/overlay-probe-%.o: $(ROCK_OVERLAY_PROBE_DIR)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/demos/overlay_probe.rock: $(ROCK_OBJECTS)/overlay-probe-launcher.o $(ROCK_OBJECTS)/plugin_crt0.o $(ROCK_LINK_INPUT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) -o $@.elf $(filter %.o %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)

$(eval $(call rock_link_overlay,probe,$(ROCK_OBJECTS)/overlay-probe-payload.o))

$(ROCK_OUT)/modules/overlays/probe_regular.rock: $(ROCK_OBJECTS)/overlay-probe-payload.o $(ROCK_OBJECTS)/plugin_crt0.o $(ROCK_LINK_INPUT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) -o $@.elf $(filter %.o %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)
