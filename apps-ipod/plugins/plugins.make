# AstraPod plugin source build. GNU GPL version 2 or later.
ROCKS :=
ifneq (,$(filter -DASTRAPOD_ROCK_COMPAT,$(EXTRA_DEFINES)))
ROCK_SRC := $(ROOTDIR)/apps/plugins
ROCK_SDK := $(APPSDIR)/plugins/sdk
ROCK_OUT := $(BUILDDIR)/rock-compat
ROCK_TOOLS := $(TOOLSDIR)/rock-compat
ROCK_MANIFEST := $(APPSDIR)/plugins/rock-modules.txt
ROCK_MODULES := $(shell cat $(ROCK_MANIFEST))
ROCKS := $(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_MODULES)))
INCLUDES += -I$(ROCK_OUT)

ROCK_STRICT := -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion
ROCK_FLAGS = -I$(ROCK_SDK) -I$(ROCK_SRC) -I$(ROCK_SRC)/lib -DPLUGIN \
    $(CFLAGS) $(SHARED_CFLAGS) $(ROCK_STRICT) -fno-lto -fno-builtin \
    -ffunction-sections -fdata-sections
-include $(ROCK_TOOLS)/tables.make

# The preprocessed SDK captures target switches, included type layouts and IDs.
# Compare content so unrelated core rebuilds keep the same fingerprint.
.PHONY: rock-fingerprint-inputs
$(ROCK_OUT)/rock_fingerprint.h: $(BUILDDIR)/lang_enum.h $(ROCK_COMPAT_TABLES) rock-fingerprint-inputs
	$(SILENT)mkdir -p "$(ROCK_OUT)"
	$(SILENT)$(CC) $(ROCK_FLAGS) -DROCK_FINGERPRINT_PROBE -x c -E -P -dD \
	    $(ROCK_SDK)/plugin.h -o $@.i
	$(SILENT)python3 $(ROCK_TOOLS)/fingerprint.py $@.i $@
	$(SILENT)rm -f $@.i

$(APPSBUILDDIR)/plugins/rock_runtime.o: $(ROCK_OUT)/rock_fingerprint.h
$(APPSBUILDDIR)/sim/rock_host.o: $(ROCK_OUT)/rock_fingerprint.h
$(APPSBUILDDIR)/plugins/rock_runtime.o $(APPSBUILDDIR)/plugins/rock_files.o: CFLAGS += $(ROCK_STRICT)

ifeq ($(APP_TYPE),sdl-sim)
ROCK_LINK = $(SHARED_LDFLAGS) $(GLOBAL_LDOPTS) -Wl,--gc-sections
ROCK_OUTPUT = mv $@.elf $@
else
ROCK_LINK = -nostdlib -T$(ROCK_OUT)/plugin.link -Wl,--gc-sections
ROCK_LINK_INPUT := $(ROCK_OUT)/plugin.link $(SETJMPLIB)
ROCK_OUTPUT = $(call objcopy_plugin,$@.elf,$@)
$(ROCK_OUT)/plugin.link: $(APPSDIR)/plugins/plugin.lds $(FIRMDIR)/export/config/$(MODELNAME).h
	$(SILENT)mkdir -p "$(ROCK_OUT)"
	$(call preprocess2file,$<,$@,-DPLUGIN -DASTRAPOD_ROCK)
endif

$(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c $(ROCK_TOOLS)/stage-usb.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(ROCK_TOOLS)/stage-usb.py $< $@

ROCK_FINGERPRINT := $(ROCK_OUT)/rock_fingerprint.h
ROCK_OBJECTS := $(ROCK_OUT)/obj
$(ROCK_OBJECTS)/dice.o $(ROCK_OBJECTS)/stopwatch.o: $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/%.o: $(ROCK_SRC)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/demos/snow.rock: $(ROCK_OBJECTS)/lib/pluginlib_actions.o
$(ROCK_OUT)/modules/apps/chessclock.rock: $(ROCK_OBJECTS)/lib/playback_control.o
$(ROCK_OUT)/modules/apps/countdown_timer.rock: $(ROCK_OBJECTS)/lib/pluginlib_actions.o
$(ROCK_OUT)/modules/games/dice.rock: $(addprefix $(ROCK_OBJECTS)/lib/,pluginlib_actions.o configfile.o playback_control.o)
$(ROCKS): $(ROCK_OUT)/modules/%.rock: $(ROCK_OBJECTS)/plugin_crt0.o $(ROCK_LINK_INPUT) $(ROCK_MANIFEST)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) -o $@.elf $(filter %.o %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)

$(foreach module,$(ROCK_MODULES),$(eval $(ROCK_OUT)/modules/$(module).rock: $(ROCK_OBJECTS)/$(notdir $(module)).o))

-include $(wildcard $(ROCK_OBJECTS)/*.d $(ROCK_OBJECTS)/lib/*.d)
endif
