# Stage the upstream XWorld interpreter. GNU GPL version 2 or later.
ROCK_XWORLD_SRC := $(ROCK_SRC)/xworld
ROCK_XWORLD_STAGE := $(ROCK_OUT)/staged/xworld
ROCK_XWORLD_OBJ := $(ROCK_OBJECTS)/xworld
ROCK_XWORLD_INPUTS := $(wildcard $(ROCK_XWORLD_SRC)/*.[ch])
ROCK_XWORLD_HEADERS := $(addprefix $(ROCK_XWORLD_STAGE)/,$(notdir $(filter %.h,$(ROCK_XWORLD_INPUTS))))
ROCK_XWORLD_UNITS := $(notdir $(call preprocess,$(ROCK_XWORLD_SRC)/SOURCES))
ROCK_XWORLD_PATCH := $(APPSDIR)/plugins/xworld-compat.patch

$(addprefix $(ROCK_XWORLD_STAGE)/,$(notdir $(ROCK_XWORLD_INPUTS))) &: $(ROCK_XWORLD_INPUTS) $(ROCK_XWORLD_PATCH)
	$(SILENT)mkdir -p $(ROCK_XWORLD_STAGE)
	$(SILENT)cp $(ROCK_XWORLD_INPUTS) $(ROCK_XWORLD_STAGE)/
	$(SILENT)patch --batch --forward --ignore-whitespace --fuzz=0 -s -p1 -d $(ROCK_XWORLD_STAGE) < $(ROCK_XWORLD_PATCH)

$(addprefix $(ROCK_XWORLD_OBJ)/,$(filter-out xworld.o,$(ROCK_XWORLD_UNITS:.c=.o))): $(ROCK_XWORLD_OBJ)/%.o: $(ROCK_XWORLD_STAGE)/%.c $(ROCK_FINGERPRINT) | $(ROCK_XWORLD_HEADERS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC xworld/$(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/xworld.o: $(ROCK_XWORLD_STAGE)/xworld.c $(ROCK_FINGERPRINT) | $(ROCK_XWORLD_HEADERS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC xworld/$(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/games/xworld.rock: \
    $(addprefix $(ROCK_XWORLD_OBJ)/,$(filter-out xworld.o,$(ROCK_XWORLD_UNITS:.c=.o))) \
    $(addprefix $(ROCK_OBJECTS)/lib/,display_text.o helper.o \
        pluginlib_actions.o bmp_smooth_scale.o)
-include $(wildcard $(ROCK_XWORLD_OBJ)/*.d)
