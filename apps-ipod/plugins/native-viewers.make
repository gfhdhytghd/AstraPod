# AstraPod native viewer entry points. GNU GPL version 2 or later.
ROCK_NATIVE_VIEWERS := imageviewer text_viewer
$(foreach viewer,$(ROCK_NATIVE_VIEWERS),$(eval \
    $(ROCK_OUT)/staged/native/$(viewer).c: $(ROCK_SRC)/$(viewer)/$(viewer).c))

$(addprefix $(ROCK_OUT)/staged/native/,$(addsuffix .c,$(ROCK_NATIVE_VIEWERS))): \
    $(APPSDIR)/plugins/stage-native-viewer.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(APPSDIR)/plugins/stage-native-viewer.py $(filter-out %.py,$^) $@

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_NATIVE_VIEWERS))): \
$(ROCK_OBJECTS)/%.o: $(ROCK_OUT)/staged/native/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@
