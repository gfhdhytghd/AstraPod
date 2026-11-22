# Stock XRick with AstraPod foreground audio and input. GPL version 2 or later.
XRICK_DIR := $(ROCK_SRC)/xrick
XRICK_SOURCES := $(call preprocess,$(XRICK_DIR)/SOURCES)
XRICK_SOURCES := $(filter-out %/syssnd_rockbox.c %/sysevt_rockbox.c,$(XRICK_SOURCES))
XRICK_OBJECTS := $(patsubst $(XRICK_DIR)/%.c,$(ROCK_OBJECTS)/xrick/%.o,$(XRICK_SOURCES))
XRICK_ADAPTERS := $(addprefix $(ROCK_OBJECTS)/xrick/,audio.o input.o validate.o)
XRICK_FLAGS = $(ROCK_FLAGS) -DASTRAPOD_XRICK -I$(APPSDIR)/plugins

$(ROCK_OBJECTS)/xrick/%.o: $(XRICK_DIR)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC xrick/$*.c)$(CC) $(XRICK_FLAGS) -MMD -MP -c $< -o $@

$(XRICK_ADAPTERS): $(ROCK_OBJECTS)/xrick/%.o: $(APPSDIR)/plugins/xrick-%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC xrick-$*.c)$(CC) $(XRICK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/xrick.o: $(XRICK_OBJECTS) $(XRICK_ADAPTERS)
	$(call PRINTS,LD $(@F))$(CC) -r -nostdlib -o $@ $^

$(ROCK_OUT)/modules/games/xrick.rock: $(ROCK_OBJECTS)/lib/helper.o
-include $(XRICK_OBJECTS:.o=.d) $(XRICK_ADAPTERS:.o=.d)
