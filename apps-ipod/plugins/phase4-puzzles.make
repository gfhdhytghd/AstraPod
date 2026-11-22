# Build the stock puzzle modules against the AstraPod SDK. GPL-2.0-or-later.
$(eval $(call rock_module_bitmaps,games/sudoku,sudoku_normal sudoku_inverse sudoku_start))

rock_puzzle_sources = $(patsubst $(ROCK_SRC)/%,%,$(call preprocess,$(ROCK_SRC)/$(1)/SOURCES))
rock_puzzle_objects = $(addprefix $(ROCK_OBJECTS)/,$(patsubst %.c,%.o,$(filter-out $(2),$(call rock_puzzle_sources,$(1)))))

$(ROCK_OUT)/modules/demos/fractals.rock: \
    $(call rock_puzzle_objects,fractals,fractals/fractal.c) $(addprefix $(ROCK_OBJECTS)/lib/,xlcd_core.o xlcd_scroll.o)
$(ROCK_OUT)/modules/games/reversi.rock: \
    $(call rock_puzzle_objects,reversi,reversi/reversi-gui.c) $(ROCK_OBJECTS)/lib/playback_control.o
$(ROCK_OUT)/modules/games/sudoku.rock: \
    $(call rock_puzzle_objects,sudoku,sudoku/sudoku.c) \
    $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o playback_control.o)

ROCK_PUZZLE_STAGED := sudoku/sudoku reversi/reversi-gui fractals/fractal
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_PUZZLE_STAGED))): \
    $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
    $(APPSDIR)/plugins/stage-phase4-puzzles.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

define rock_puzzle_frontend
$(ROCK_OBJECTS)/$(1).o: $(ROCK_OUT)/staged/$(2).c $(ROCK_FINGERPRINT)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,CC $$(@F))$$(CC) $$(ROCK_FLAGS) -I$(ROCK_SRC)/$(dir $(2)) \
	    -MMD -MP -c $$< -o $$@
endef

$(eval $(call rock_puzzle_frontend,fractals,fractals/fractal))
$(eval $(call rock_puzzle_frontend,reversi,reversi/reversi-gui))
$(eval $(call rock_puzzle_frontend,sudoku,sudoku/sudoku))

-include $(wildcard $(addprefix $(ROCK_OBJECTS)/,fractals/*.d reversi/*.d sudoku/*.d))

ROCK_TATHAM := $(filter games/sgt_puzzles/sgt-%,$(ROCK_MODULES))
ifneq ($(ROCK_TATHAM),)
ROCK_TATHAM_SRC := $(ROCK_SRC)/puzzles
ROCK_TATHAM_DUMMY := $(ROCK_OUT)/tatham-headers
ROCK_TATHAM_HEADERS := $(addprefix $(ROCK_TATHAM_DUMMY)/,assert.h math.h) \
    $(APPSDIR)/plugins/tatham-sdk.h
ROCK_TATHAM_FLAGS = -I$(ROCK_TATHAM_DUMMY) $(filter-out -O%,$(ROCK_FLAGS)) \
    -O2 -fno-strict-aliasing -I$(ROCK_TATHAM_SRC) -I$(ROCK_TATHAM_SRC)/src \
    -include $(APPSDIR)/plugins/tatham-sdk.h -DFOR_REAL -DHAVE_STDINT_H=1 \
    -Wno-unused-parameter -Wno-sign-compare
$(ROCK_TATHAM_DUMMY)/%.h: $(ROCK_TATHAM_SRC)/dummy/%.h
	$(SILENT)mkdir -p $(@D)
	$(SILENT)cp $< $@
ROCK_TATHAM_COMMON := $(call rock_puzzle_objects,puzzles,puzzles/rockbox.c puzzles/rbmalloc.c)
ROCK_TATHAM_ADAPTERS := $(addprefix $(ROCK_OBJECTS)/puzzles/,rockbox.o rbmalloc.o)
ROCK_TATHAM_HELP := $(patsubst games/sgt_puzzles/sgt-%,$(ROCK_OBJECTS)/puzzles/help/%.o,$(ROCK_TATHAM))
ROCK_TATHAM_LIBS := $(addprefix $(ROCK_OBJECTS)/lib/,helper.o playback_control.o display_text.o xlcd_draw.o)
ROCK_TATHAM_ROCKS := $(addprefix $(ROCK_OUT)/modules/,$(addsuffix .rock,$(ROCK_TATHAM)))

ROCK_BITMAPS += puzzles_cursor
$(ROCK_OBJECTS)/puzzles/rockbox.o: $(ROCK_BITMAP_DIR)/puzzles_cursor.h
$(ROCK_TATHAM_ROCKS): $(ROCK_TATHAM_COMMON) $(ROCK_TATHAM_ADAPTERS) \
    $(ROCK_TATHAM_LIBS) $(ROCK_BITMAP_DIR)/puzzles_cursor.o $(TLSFLIB) $(FIXEDPOINTLIB)

ROCK_TATHAM_GAMES := $(call preprocess,$(ROCK_TATHAM_SRC)/SOURCES.games)
define rock_tatham_game
$(ROCK_OBJECTS)/sgt-$(1).o: $(2) $(ROCK_FINGERPRINT) $(ROCK_TATHAM_HEADERS)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,CC $$(@F))$$(CC) $$(ROCK_TATHAM_FLAGS) -MMD -MP -c $$< -o $$@
$(ROCK_OUT)/modules/games/sgt_puzzles/sgt-$(1).rock: $(ROCK_OBJECTS)/puzzles/help/$(1).o
endef
$(foreach src,$(ROCK_TATHAM_GAMES),$(eval $(call rock_tatham_game,$(basename $(notdir $(src))),$(src))))

$(ROCK_OUT)/staged/puzzles/%.c: $(ROCK_TATHAM_SRC)/%.c \
    $(APPSDIR)/plugins/stage-phase4-puzzles.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OUT)/staged/puzzles/rockbox.c: $(APPSDIR)/plugins/tatham-save.inc

$(ROCK_TATHAM_ADAPTERS): $(ROCK_OBJECTS)/puzzles/%.o: $(ROCK_OUT)/staged/puzzles/%.c $(ROCK_FINGERPRINT) $(ROCK_TATHAM_HEADERS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_TATHAM_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_TATHAM_COMMON) $(ROCK_TATHAM_HELP): $(ROCK_OBJECTS)/puzzles/%.o: $(ROCK_TATHAM_SRC)/%.c $(ROCK_FINGERPRINT) $(ROCK_TATHAM_HEADERS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_TATHAM_FLAGS) -MMD -MP -c $< -o $@

-include $(wildcard $(ROCK_OBJECTS)/puzzles/*.d $(ROCK_OBJECTS)/puzzles/*/*.d $(ROCK_OBJECTS)/puzzles/src/*/*.d)
endif
