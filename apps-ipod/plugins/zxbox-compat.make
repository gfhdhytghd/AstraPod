# ZX Spectrum foreground module. GNU GPL version 2 or later.
ROCK_ZX_SRC := $(ROCK_SRC)/zxbox
ROCK_ZX_STAGE := $(ROCK_OUT)/staged/zxbox
ROCK_ZX_OBJ := $(ROCK_OBJECTS)/zxbox
ROCK_ZX_UNITS := $(patsubst $(ROCK_ZX_SRC)/%.c,%,$(call preprocess,$(ROCK_ZX_SRC)/SOURCES))
ROCK_ZX_FLAGS := -I$(ROCK_ZX_SRC)
ROCK_ZX_HEADERS := $(patsubst $(ROCK_ZX_SRC)/%,$(ROCK_ZX_STAGE)/%,$(wildcard $(ROCK_ZX_SRC)/*.h))

$(ROCK_ZX_STAGE)/%.h: $(ROCK_ZX_SRC)/%.h $(APPSDIR)/plugins/stage-zxbox.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(addprefix $(ROCK_ZX_STAGE)/,$(addsuffix .c,$(ROCK_ZX_UNITS))): $(ROCK_ZX_STAGE)/%.c: $(ROCK_ZX_SRC)/%.c $(APPSDIR)/plugins/stage-zxbox.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@
$(ROCK_ZX_STAGE)/spsound.c: $(APPSDIR)/plugins/zxbox-stream.inc

$(addprefix $(ROCK_ZX_OBJ)/,$(addsuffix .o,$(filter-out zxbox zxbox_keyb,$(ROCK_ZX_UNITS)))): $(ROCK_ZX_OBJ)/%.o: $(ROCK_ZX_STAGE)/%.c $(ROCK_ZX_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC zxbox/$(@F))$(CC) $(ROCK_FLAGS) $(ROCK_ZX_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/zxbox.o: $(ROCK_ZX_STAGE)/zxbox.c $(ROCK_ZX_HEADERS) $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC zxbox/$(@F))$(CC) $(ROCK_FLAGS) $(ROCK_ZX_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/games/zxbox.rock: $(addprefix $(ROCK_ZX_OBJ)/,$(addsuffix .o,$(filter-out zxbox zxbox_keyb,$(ROCK_ZX_UNITS))))
-include $(wildcard $(ROCK_ZX_OBJ)/*.d)
