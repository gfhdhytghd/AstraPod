# Multi-source foreground modules. GNU GPL version 2 or later.
# Keep upstream source selection, while compiling with the AstraPod SDK.
define rock_engine
$(ROCK_OUT)/modules/$(1).rock: $(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(2)))
$(ROCK_OBJECTS)/$(notdir $(1)).o: $(ROCK_OUT)/staged/$(notdir $(1))/$(notdir $(1)).c $(ROCK_FINGERPRINT)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,CC $$(@F))$$(CC) $$(ROCK_FLAGS) \
	    -MMD -MP -c $$< -o $$@
$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(2))): $(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,CC $$(@F))$$(CC) $$(ROCK_FLAGS) \
	    -MMD -MP -c $$< -o $$@
$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(notdir $(1))/$(notdir $(1)) $(2))): $(ROCK_OUT)/staged/%.c: $(ROCK_SRC)/%.c \
    $(APPSDIR)/plugins/stage-rock-engine.py $(3)
	$$(SILENT)mkdir -p $$(@D)
	$$(SILENT)python3 $$(word 2,$$^) $$< $$@
$(3): $(ROCK_OUT)/staged/%: $(ROCK_SRC)/%
	$$(SILENT)mkdir -p $$(@D)
	$$(SILENT)cp $$< $$@
endef

ROCK_ENGINES := $(filter games/chessbox games/goban viewers/rockboy viewers/zxbox,$(ROCK_MODULES))
rock_engine_sources = $(patsubst $(ROCK_SRC)/%.c,%,$(filter-out %/$(1).c,\
    $(call preprocess,$(ROCK_SRC)/$(1)/SOURCES)))
rock_engine_headers = $(patsubst $(ROCK_SRC)/%,$(ROCK_OUT)/staged/%,\
    $(wildcard $(ROCK_SRC)/$(1)/*.h $(ROCK_SRC)/$(1)/z80_op*x.c))
$(foreach engine,$(ROCK_ENGINES),$(eval $(call rock_engine,$(engine),\
    $(call rock_engine_sources,$(notdir $(engine))),\
    $(call rock_engine_headers,$(notdir $(engine))))))

$(ROCK_OUT)/modules/games/chessbox.rock: $(ROCK_OBJECTS)/lib/playback_control.o
$(ROCK_OUT)/modules/games/goban.rock: $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o playback_control.o xlcd_draw.o)
$(eval $(call rock_module_bitmaps,games/chessbox,chessbox_pieces))

-include $(foreach engine,$(ROCK_ENGINES),$(wildcard $(ROCK_OBJECTS)/$(notdir $(engine))/*.d))

include $(APPSDIR)/plugins/rockboy-compat.make
include $(APPSDIR)/plugins/zxbox-compat.make
