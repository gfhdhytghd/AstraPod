# AstraPod FFT source build. GNU GPL version 2 or later.
ROCK_FFT_SOURCES := $(filter-out fft.c,$(notdir $(call preprocess,$(ROCK_SRC)/fft/SOURCES)))
ROCK_FFT_OBJS := $(addprefix $(ROCK_OBJECTS)/fft/,$(ROCK_FFT_SOURCES:.c=.o))
ROCK_FFT_HELPERS := $(addprefix $(ROCK_OBJECTS)/fft/,osd.o configfile.o)

$(ROCK_OUT)/staged/fft/fft.c: $(ROCK_SRC)/fft/fft.c $(APPSDIR)/plugins/stage-fft.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OBJECTS)/fft.o: $(ROCK_OUT)/staged/fft/fft.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC fft/$(@F))$(CC) $(ROCK_FLAGS) -O3 -DFIXED_POINT=16 \
	    -iquote $(ROCK_SRC)/fft -MMD -MP -c $< -o $@

$(ROCK_FFT_OBJS): private ROCK_FLAGS += -O3 -DFIXED_POINT=16

$(addprefix $(ROCK_OUT)/staged/fft/,osd.c configfile.c): \
$(ROCK_OUT)/staged/fft/%.c: $(ROCK_SRC)/lib/%.c $(APPSDIR)/plugins/stage-fft.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_FFT_HELPERS): $(ROCK_OBJECTS)/fft/%.o: \
    $(ROCK_OUT)/staged/fft/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC fft/$(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/demos/fft.rock: $(ROCK_FFT_OBJS) \
    $(ROCK_FFT_HELPERS) $(FIXEDPOINTLIB) \
    $(addprefix $(ROCK_OBJECTS)/lib/,pluginlib_actions.o \
        helper.o xlcd_core.o xlcd_draw.o xlcd_scroll.o)

$(eval $(call rock_module_bitmaps,demos/fft,fft_colors))

-include $(wildcard $(ROCK_OBJECTS)/fft/*.d)
