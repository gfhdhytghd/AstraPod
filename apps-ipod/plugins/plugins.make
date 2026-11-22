# AstraPod plugin source build. GNU GPL version 2 or later.
ROCKS :=
ifneq (,$(filter -DASTRAPOD_ROCK_COMPAT,$(EXTRA_DEFINES)))
ROCK_SRC := $(ROOTDIR)/apps/plugins
ROCK_SDK := $(APPSDIR)/plugins/sdk
ROCK_OUT := $(BUILDDIR)/rock-compat
ROCK_TOOLS := $(TOOLSDIR)/rock-compat
ROCK_MANIFEST := $(APPSDIR)/plugins/rock-modules.txt
ROCK_MODULES := $(shell cat $(ROCK_MANIFEST))
ifeq ($(ROCK_PITCH_DETECTOR),1)
ROCK_MODULES += apps/pitch_detector
endif
ROCKS := $(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_MODULES)))
INCLUDES += -I$(ROCK_OUT)

ROCK_STRICT := -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion
ROCK_FLAGS = $(ROCK_EXTRA_INCLUDES) -I$(ROCK_SDK) -I$(ROCK_SRC) -I$(ROCK_SRC)/lib \
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

# Shell accepts the same installed paths as the bundle, including its opt-in app.
.PHONY: rock-test-paths-inputs
$(ROCK_OUT)/rock_test_paths.h: $(ROCK_MANIFEST) rock-test-paths-inputs
	$(SILENT)mkdir -p "$(@D)"
	$(SILENT)printf 'PLUGIN_DIR "/%s.rock",\n' $(shell cat $(ROCK_MANIFEST)) \
	    $(if $(filter 1,$(ROCK_PITCH_DETECTOR)),apps/pitch_detector) > "$@.tmp"
	$(SILENT)cmp -s "$@.tmp" "$@" || mv "$@.tmp" "$@"
	$(SILENT)rm -f "$@.tmp"

$(APPSBUILDDIR)/plugins/podlink_plugin_test.o: $(ROCK_OUT)/rock_test_paths.h

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

ROCK_UTILITIES := db_folder_select dict credits alarmclock calendar dart_scorer resistor shopper settings_dumper
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_UTILITIES))): $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c $(APPSDIR)/plugins/stage-utilities.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(APPSDIR)/plugins/stage-utilities.py $< $@

$(ROCK_OUT)/staged/calendar.c: $(APPSDIR)/plugins/calendar-memo.inc
$(ROCK_OUT)/staged/shopper.c: $(APPSDIR)/plugins/shopper-file.inc

ROCK_GAME_ADAPTERS := blackjack bubbles chip8 clix invadrox jewels pegbox rockblox \
    solitaire spacerocks wormlet codebuster rocklife superdom
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_GAME_ADAPTERS))): $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c $(APPSDIR)/plugins/stage-games.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(APPSDIR)/plugins/stage-games.py $< $@

$(ROCK_OUT)/staged/vu_meter.c: $(ROCK_SRC)/vu_meter.c $(APPSDIR)/plugins/stage-utilities.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(APPSDIR)/plugins/stage-utilities.py $< $@

$(ROCK_OUT)/staged/metronome.c: $(ROCK_SRC)/metronome.c $(APPSDIR)/plugins/stage-metronome.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(APPSDIR)/plugins/stage-metronome.py $< $@

ROCK_OSCILLOSCOPE_FLAGS := -DROCK_OSCILLOSCOPE_WAVEFORM
$(ROCK_OBJECTS)/oscilloscope.o: private ROCK_FLAGS += $(ROCK_OSCILLOSCOPE_FLAGS)

$(ROCK_OUT)/staged/oscilloscope.c: $(ROCK_SRC)/oscilloscope.c $(APPSDIR)/plugins/stage-oscilloscope.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(APPSDIR)/plugins/stage-oscilloscope.py $< $@

$(ROCK_OUT)/staged/rb_info.c: $(ROCK_SRC)/rb_info.c \
    $(APPSDIR)/plugins/stage-rb-info.py $(APPSDIR)/plugins/rb-info-list.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $@

# Read the compiled native enum and the platform's button constants.
$(ROCK_OUT)/rb-info-input.h: $(APPSDIR)/input/action.h \
    $(APPSDIR)/plugins/gen-rb-info-input.py $(ROCK_FINGERPRINT)
	$(SILENT)$(CC) $(ROCK_FLAGS) -E -P -x c $< -o $@.actions
	$(SILENT)$(CC) $(ROCK_FLAGS) -E -dM -include button.h -x c /dev/null \
	    -MMD -MP -MF $@.d -MT $@ -o $@.buttons
	$(SILENT)python3 $(word 2,$^) $@.actions $@.buttons $@

$(ROCK_OBJECTS)/rb_info.o: $(ROCK_OUT)/rb-info-input.h
-include $(wildcard $(ROCK_OUT)/rb-info-input.h.d)

$(ROCK_OUT)/staged/pitch_detector.c: $(ROCK_SRC)/pitch_detector.c $(APPSDIR)/plugins/stage-pitch.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OUT)/staged/stats.c $(ROCK_OUT)/staged/search.c: $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
    $(APPSDIR)/plugins/%-compat.inc $(APPSDIR)/plugins/stage-file-utilities.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 3,$^) $< $(word 2,$^) $@ $(word 4,$^)

$(ROCK_OUT)/staged/stats.c: $(ROCK_SRC)/lib/mul_id3.c

$(ROCK_OUT)/staged/wavview.c: $(ROCK_SRC)/wavview.c $(APPSDIR)/plugins/wavview-scan.inc $(APPSDIR)/plugins/stage-wavview.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 3,$^) $< $(word 2,$^) $@

ROCK_STAGED := 2048 amaze brickmania chopper dice fireworks flipit jackpot metronome \
    minesweeper oscilloscope periodic_table pitch_detector playing_time pong rb_info robotfindskitten snake snake2 star stopwatch \
    lib/highscore search stats vu_meter wavview $(ROCK_UTILITIES) $(ROCK_GAME_ADAPTERS)
$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_STAGED))): $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/oscilloscope.o: $(APPSDIR)/plugins/plugins.make

$(ROCK_OBJECTS)/%.o: $(ROCK_SRC)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/podlink_echo.o: $(APPSDIR)/plugins/examples/podlink_echo.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

ROCK_BITMAP_DIR := $(ROCK_OUT)/bitmaps/pluginbitmaps
ROCK_NATIVE_BMPS := $(call preprocess,$(ROCK_SRC)/bitmaps/native/SOURCES)
ROCK_MONO_BMPS := $(call preprocess,$(ROCK_SRC)/bitmaps/mono/SOURCES)

define rock_module_bitmaps
ROCK_BITMAPS += $(2)
$(ROCK_OBJECTS)/$(notdir $(1)).o: $(addprefix $(ROCK_BITMAP_DIR)/,$(addsuffix .h,$(2)))
$(ROCK_OUT)/modules/$(1).rock: $(addprefix $(ROCK_BITMAP_DIR)/,$(addsuffix .o,$(2)))
endef

$(eval $(call rock_module_bitmaps,games/jackpot,jackpot_slots))
$(eval $(call rock_module_bitmaps,demos/logo,rockboxlogo))
$(eval $(call rock_module_bitmaps,games/flipit,flipit_cursor flipit_tokens))
$(eval $(call rock_module_bitmaps,demos/matrix,matrix_bold matrix_normal))
$(eval $(call rock_module_bitmaps,demos/credits,creditslogo))
$(eval $(call rock_module_bitmaps,apps/resistor,resistor))
$(eval $(call rock_module_bitmaps,apps/pitch_detector,pitch_notes))

$(ROCK_OUT)/upstream-credits/credits.raw: $(DOCSDIR)/CREDITS $(ROCK_SRC)/credits.pl
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,Create upstream credits.raw)perl $(ROCK_SRC)/credits.pl < $< > $@

$(ROCK_OBJECTS)/credits.o: $(ROCK_OUT)/upstream-credits/credits.raw
$(ROCK_OBJECTS)/credits.o: ROCK_EXTRA_INCLUDES := -I$(ROCK_OUT)/upstream-credits

$(eval $(call rock_module_bitmaps,games/2048,_2048_background _2048_tiles))
$(eval $(call rock_module_bitmaps,games/amaze,amaze_tiles_9 amaze_tiles_7))
$(eval $(call rock_module_bitmaps,games/brickmania,brickmania_pads brickmania_short_pads \
    brickmania_long_pads brickmania_bricks brickmania_powerups brickmania_ball \
    brickmania_gameover brickmania_break))
$(eval $(call rock_module_bitmaps,games/snake2,snake2_header1 snake2_header2 \
    snake2_left snake2_right snake2_bottom))
$(eval $(call rock_module_bitmaps,games/star,star_tiles))
$(eval $(call rock_module_bitmaps,games/minesweeper,minesweeper_tiles))
$(eval $(call rock_module_bitmaps,games/blackjack,card_deck card_back))
$(eval $(call rock_module_bitmaps,games/bubbles,bubbles_background bubbles_bubble bubbles_emblem))
$(eval $(call rock_module_bitmaps,games/invadrox,invadrox_background invadrox_aliens \
    invadrox_ships invadrox_bombs invadrox_alien_explode invadrox_shield \
    invadrox_ufo invadrox_ufo_explode invadrox_numbers invadrox_fire))
$(eval $(call rock_module_bitmaps,games/jewels,jewels))
$(eval $(call rock_module_bitmaps,games/pegbox,pegbox_header pegbox_pieces))
$(eval $(call rock_module_bitmaps,games/rockblox,rockblox_background))
$(eval $(call rock_module_bitmaps,games/solitaire,card_deck card_back solitaire_suitsi))
$(eval $(call rock_module_bitmaps,games/superdom,superdom_boarditems))

# Use upstream's target-selected bitmap and encoding.
rock_bitmap_name = $(firstword $(subst ., ,$(notdir $(1))))
$(foreach bmp,$(ROCK_NATIVE_BMPS) $(ROCK_MONO_BMPS),$(if $(filter $(call rock_bitmap_name,$(bmp)),$(ROCK_BITMAPS)),\
    $(eval $(ROCK_BITMAP_DIR)/$(call rock_bitmap_name,$(bmp)).c $(ROCK_BITMAP_DIR)/$(call rock_bitmap_name,$(bmp)).h: $(bmp))))

rock_bitmap_converter = $(if $(filter $(ROCK_SRC)/bitmaps/mono/%,$(1)),$(BMP2RB_MONO),$(BMP2RB_NATIVE))
$(ROCK_BITMAP_DIR)/%.c $(ROCK_BITMAP_DIR)/%.h: $(TOOLSDIR)/bmp2rb
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,BMP2RB $(*F))$(call rock_bitmap_converter,$(filter %.bmp,$^)) -b -h $(@D) $(filter %.bmp,$^) > $(@D)/$*.c

$(ROCK_BITMAP_DIR)/%.o: $(ROCK_BITMAP_DIR)/%.c $(ROCK_BITMAP_DIR)/%.h
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

ROCK_ACTION_MODULES := demos/snow demos/logo demos/mosaique demos/bounce \
    demos/matrix apps/countdown_timer games/dice games/jackpot games/rockblox1d \
    games/robotfindskitten apps/periodic_table games/2048 games/amaze \
    demos/fire demos/fireworks demos/plasma demos/starfield \
    apps/alarmclock apps/dart_scorer apps/metronome apps/resistor demos/demystify games/bubbles \
    demos/rocklife games/codebuster games/superdom apps/pitch_detector
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_ACTION_MODULES))): $(ROCK_OBJECTS)/lib/pluginlib_actions.o
$(ROCK_OUT)/modules/games/dice.rock: $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o playback_control.o)
$(ROCK_OUT)/modules/games/jackpot.rock: $(ROCK_OBJECTS)/lib/picture.o
$(ROCK_OUT)/modules/games/robotfindskitten.rock: $(ROCK_OBJECTS)/lib/display_text.o
$(ROCK_OUT)/modules/demos/cube.rock: $(ROCK_OBJECTS)/lib/xlcd_draw.o
$(ROCK_OUT)/modules/demos/cube.rock $(ROCK_OUT)/modules/demos/bounce.rock: $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/apps/chessclock.rock: $(ROCK_OBJECTS)/lib/playback_control.o
$(ROCK_OUT)/modules/apps/calendar.rock: $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o playback_control.o)
$(ROCK_OUT)/modules/viewers/shopper.rock: $(ROCK_OBJECTS)/lib/playback_control.o
$(addprefix $(ROCK_OUT)/modules/,apps/lamp.rock demos/lamp.rock): $(addprefix $(ROCK_OBJECTS)/lib/,pluginlib_actions.o helper.o)
$(ROCK_OUT)/modules/demos/credits.rock: $(ROCK_OBJECTS)/lib/helper.o
$(ROCK_OUT)/modules/apps/dart_scorer.rock: $(addprefix $(ROCK_OBJECTS)/lib/,display_text.o helper.o playback_control.o)
$(ROCK_OUT)/modules/apps/resistor.rock: $(addprefix $(ROCK_OBJECTS)/lib/,display_text.o helper.o picture.o)
ROCK_GAME_SCORES := games/2048 games/brickmania games/xobox games/snake games/snake2 \
    games/blackjack games/bubbles games/clix games/invadrox games/jewels games/rockblox \
    games/spacerocks
ROCK_GAME_CONFIG := games/brickmania games/chopper games/mazezam games/snake \
    games/pegbox games/solitaire games/wormlet
ROCK_GAME_TEXT := games/2048 games/brickmania games/star games/blackjack games/clix \
    games/jewels games/pegbox games/rockblox games/solitaire games/spacerocks
ROCK_GAME_PLAYBACK := $(sort $(filter-out games/invadrox,$(ROCK_GAME_SCORES)) $(ROCK_GAME_CONFIG) games/minesweeper games/star)
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_GAME_SCORES))): $(ROCK_OBJECTS)/lib/highscore.o
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_GAME_CONFIG))): $(ROCK_OBJECTS)/lib/configfile.o
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_GAME_TEXT))): $(ROCK_OBJECTS)/lib/display_text.o
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_GAME_PLAYBACK) demos/fireworks)): $(ROCK_OBJECTS)/lib/playback_control.o
ROCK_EXTRA_HELPER := games/2048 games/brickmania games/chopper games/maze games/mazezam \
    games/pong games/xobox demos/fire demos/fireworks demos/plasma demos/starfield \
    demos/demystify games/invadrox games/rockblox games/spacerocks games/wormlet
$(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_EXTRA_HELPER))): $(ROCK_OBJECTS)/lib/helper.o
$(ROCK_OUT)/modules/games/chopper.rock $(ROCK_OUT)/modules/games/bubbles.rock: $(ROCK_OBJECTS)/lib/xlcd_draw.o
$(ROCK_OUT)/modules/games/bubbles.rock: $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/demos/plasma.rock: $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/demos/rocklife.rock $(ROCK_OUT)/modules/demos/vu_meter.rock: $(ROCK_OBJECTS)/lib/helper.o
$(ROCK_OUT)/modules/demos/vu_meter.rock: $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/demos/oscilloscope.rock: $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o helper.o)
$(ROCK_OUT)/modules/games/codebuster.rock: $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o playback_control.o)
$(ROCK_OUT)/modules/games/superdom.rock: $(addprefix $(ROCK_OBJECTS)/lib/,display_text.o playback_control.o)
$(ROCK_OUT)/modules/apps/metronome.rock: $(FIXEDPOINTLIB)
$(ROCK_OUT)/modules/apps/pitch_detector.rock: $(addprefix $(ROCK_OBJECTS)/lib/,picture.o helper.o)

$(ROCKS): $(ROCK_OUT)/modules/%.rock: $(ROCK_OBJECTS)/plugin_crt0.o $(ROCK_LINK_INPUT) $(ROCK_MANIFEST)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,LD $(@F))$(CC) $(ROCK_FLAGS) $(ROCK_LINK) -o $@.elf $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(SILENT)$(ROCK_OUTPUT)

$(foreach module,$(ROCK_MODULES),$(eval $(ROCK_OUT)/modules/$(module).rock: $(ROCK_OBJECTS)/$(notdir $(module)).o))

-include $(wildcard $(ROCK_OBJECTS)/*.d $(ROCK_OBJECTS)/lib/*.d $(ROCK_BITMAP_DIR)/*.d)
endif
