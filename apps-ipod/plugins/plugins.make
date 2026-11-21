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
ROCK_FLAGS = -I$(ROCK_SDK) -I$(ROCK_SRC) -I$(ROCK_SRC)/lib \
    -I$(ROCK_OUT)/bitmaps -DPLUGIN \
    $(CFLAGS) $(SHARED_CFLAGS) $(ROCK_STRICT) -fno-lto -fno-builtin \
    -ffunction-sections -fdata-sections
-include $(ROCK_TOOLS)/tables.make

# The preprocessed SDK captures target switches, included type layouts and IDs.
# Compare content so unrelated core rebuilds keep the same fingerprint.
.PHONY: rock-fingerprint-inputs
$(ROCK_OUT)/rock_fingerprint.h: $(BUILDDIR)/lang_enum.h $(BUILDDIR)/sysfont.h $(ROCK_COMPAT_TABLES) rock-fingerprint-inputs
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
ifneq ($(APP_TYPE),sdl-sim)
# The compiler may emit libcalls even with -fno-builtin.
ROCK_LINK_INPUT += $(ROCK_OBJECTS)/lib/gcc-support.o
endif

ROCK_STAGED := dice flipit jackpot robotfindskitten stopwatch
$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_STAGED))): $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/%.o: $(ROCK_SRC)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

ROCK_BITMAP_DIR := $(ROCK_OUT)/bitmaps/pluginbitmaps
ROCK_NATIVE_BMPS := $(call preprocess,$(ROCK_SRC)/bitmaps/native/SOURCES)

define rock_module_bitmaps
ROCK_BITMAPS += $(2)
$(ROCK_OBJECTS)/$(notdir $(1)).o: $(addprefix $(ROCK_BITMAP_DIR)/,$(addsuffix .h,$(2)))
$(ROCK_OUT)/modules/$(1).rock: $(addprefix $(ROCK_BITMAP_DIR)/,$(addsuffix .o,$(2)))
endef

$(eval $(call rock_module_bitmaps,games/jackpot,jackpot_slots))
$(eval $(call rock_module_bitmaps,demos/logo,rockboxlogo))
$(eval $(call rock_module_bitmaps,games/flipit,flipit_cursor flipit_tokens))
$(eval $(call rock_module_bitmaps,demos/matrix,matrix_bold matrix_normal))

# Use upstream's target-selected bitmap and drop its optional dimension suffix.
rock_bitmap_name = $(firstword $(subst ., ,$(notdir $(1))))
$(foreach bmp,$(ROCK_NATIVE_BMPS),$(if $(filter $(call rock_bitmap_name,$(bmp)),$(ROCK_BITMAPS)),\
    $(eval $(ROCK_BITMAP_DIR)/$(call rock_bitmap_name,$(bmp)).c $(ROCK_BITMAP_DIR)/$(call rock_bitmap_name,$(bmp)).h: $(bmp))))

$(ROCK_BITMAP_DIR)/%.c $(ROCK_BITMAP_DIR)/%.h: $(TOOLSDIR)/bmp2rb
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,BMP2RB $(*F))$(BMP2RB_NATIVE) -b -h $(@D) $(filter %.bmp,$^) > $(@D)/$*.c

$(ROCK_BITMAP_DIR)/%.o: $(ROCK_BITMAP_DIR)/%.c $(ROCK_BITMAP_DIR)/%.h
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

ROCK_ACTION_MODULES := demos/snow demos/logo demos/mosaique demos/bounce \
    demos/matrix apps/countdown_timer games/dice games/jackpot games/rockblox1d \
    games/robotfindskitten apps/periodic_table
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_ACTION_MODULES))): $(ROCK_OBJECTS)/lib/pluginlib_actions.o
$(ROCK_OUT)/modules/games/dice.rock: $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o playback_control.o)
$(ROCK_OUT)/modules/games/jackpot.rock: $(ROCK_OBJECTS)/lib/picture.o
$(ROCK_OUT)/modules/games/robotfindskitten.rock: $(ROCK_OBJECTS)/lib/display_text.o
$(ROCK_OUT)/modules/demos/cube.rock: $(ROCK_OBJECTS)/lib/xlcd_draw.o
$(ROCK_OUT)/modules/demos/cube.rock $(ROCK_OUT)/modules/demos/bounce.rock: $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/apps/chessclock.rock: $(ROCK_OBJECTS)/lib/playback_control.o
$(ROCK_OUT)/modules/demos/lamp.rock: $(addprefix $(ROCK_OBJECTS)/lib/,pluginlib_actions.o helper.o)
$(ROCKS): $(ROCK_OUT)/modules/%.rock: $(ROCK_OBJECTS)/plugin_crt0.o $(ROCK_LINK_INPUT) $(ROCK_MANIFEST)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) -o $@.elf $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)

$(foreach module,$(ROCK_MODULES),$(eval $(ROCK_OUT)/modules/$(module).rock: $(ROCK_OBJECTS)/$(notdir $(module)).o))

-include $(wildcard $(ROCK_OBJECTS)/*.d $(ROCK_OBJECTS)/lib/*.d $(ROCK_BITMAP_DIR)/*.d)
endif
