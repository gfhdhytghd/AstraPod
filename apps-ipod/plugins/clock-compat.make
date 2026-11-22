# AstraPod Clock source build. GNU GPL version 2 or later.
ROCK_CLOCK_SOURCES := $(filter-out clock.c,$(notdir $(call preprocess,$(ROCK_SRC)/clock/SOURCES)))
ROCK_CLOCK_STAGED := clock_menu.c clock_settings.c
ROCK_CLOCK_OBJS := $(addprefix $(ROCK_OBJECTS)/clock/,$(ROCK_CLOCK_SOURCES:.c=.o))
ROCK_CLOCK_BITMAPS := clock_binary clock_digits clock_smalldigits clock_segments \
    clock_smallsegments clock_logo clock_messages

$(addprefix $(ROCK_OUT)/staged/clock/,clock.c $(ROCK_CLOCK_STAGED)): \
$(ROCK_OUT)/staged/clock/%.c: $(ROCK_SRC)/clock/%.c $(APPSDIR)/plugins/stage-clock.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OBJECTS)/clock.o: $(ROCK_OUT)/staged/clock/clock.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC clock/$(@F))$(CC) $(ROCK_FLAGS) -I$(ROCK_SRC)/clock \
	    -MMD -MP -c $< -o $@

$(addprefix $(ROCK_OBJECTS)/clock/,$(ROCK_CLOCK_STAGED:.c=.o)): \
$(ROCK_OBJECTS)/clock/%.o: $(ROCK_OUT)/staged/clock/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC clock/$(@F))$(CC) $(ROCK_FLAGS) -I$(ROCK_SRC)/clock \
	    -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/clock/clock_bitmaps.o: \
    $(addprefix $(ROCK_BITMAP_DIR)/,$(addsuffix .h,$(ROCK_CLOCK_BITMAPS)))

$(ROCK_OUT)/modules/apps/clock.rock: $(ROCK_CLOCK_OBJS) \
    $(FIXEDPOINTLIB) $(addprefix $(ROCK_OBJECTS)/lib/,pluginlib_actions.o playback_control.o \
        picture.o xlcd_draw.o)

$(eval $(call rock_module_bitmaps,apps/clock,$(ROCK_CLOCK_BITMAPS)))

-include $(wildcard $(ROCK_OBJECTS)/clock/*.d)
