# Staged XRick with AstraPod entry, audio and input. GPL version 2 or later.
XRICK_SRC := $(ROCK_SRC)/xrick
XRICK_STAGE := $(ROCK_OUT)/staged/xrick-tree
XRICK_STAMP := $(XRICK_STAGE)/xrick/.staged
XRICK_SOURCES := $(call preprocess,$(XRICK_SRC)/SOURCES)
XRICK_SOURCES := $(filter-out %/main_rockbox.c %/syssnd_rockbox.c %/sysevt_rockbox.c,$(XRICK_SOURCES))
XRICK_OBJECTS := $(patsubst $(XRICK_SRC)/%.c,$(ROCK_OBJECTS)/xrick/%.o,$(XRICK_SOURCES))
XRICK_ADAPTERS := $(addprefix $(ROCK_OBJECTS)/xrick/,main.o audio.o input.o validate.o)
XRICK_FLAGS = -I$(XRICK_STAGE) $(ROCK_FLAGS) -DASTRAPOD_XRICK -I$(APPSDIR)/plugins

$(XRICK_STAMP): $(APPSDIR)/plugins/stage-xrick.py $(APPSDIR)/plugins/xrick-stage.patch \
    $(shell find $(XRICK_SRC) -type f)
	$(SILENT)mkdir -p $(XRICK_STAGE)
	$(SILENT)python3 $< $(XRICK_SRC) $(word 2,$^) $(@D)
	$(SILENT)touch $@

$(XRICK_OBJECTS): $(ROCK_OBJECTS)/xrick/%.o: $(XRICK_STAMP) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC xrick/$*.c)$(CC) $(XRICK_FLAGS) -MMD -MP -c $(XRICK_STAGE)/xrick/$*.c -o $@

$(XRICK_ADAPTERS): $(ROCK_OBJECTS)/xrick/%.o: $(APPSDIR)/plugins/xrick-%.c $(XRICK_STAMP) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC xrick-$*.c)$(CC) $(XRICK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/xrick.o: $(XRICK_OBJECTS) $(XRICK_ADAPTERS)
	$(call PRINTS,LD $(@F))$(CC) -r -nostdlib -o $@ $^

$(ROCK_OUT)/modules/games/xrick.rock: $(ROCK_OBJECTS)/lib/helper.o
-include $(XRICK_OBJECTS:.o=.d) $(XRICK_ADAPTERS:.o=.d)
