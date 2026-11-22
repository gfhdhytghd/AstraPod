# Native launcher adapter for Open Plugins. GNU GPL version 2 or later.
$(APPSBUILDDIR)/plugins/rock_catalog.o: $(ROCK_OUT)/rock_test_paths.h

$(ROCK_OUT)/staged/open_plugins.c: $(ROCK_SRC)/open_plugins.c \
    $(APPSDIR)/plugins/stage-open-plugins.py \
    $(APPSDIR)/plugins/open-plugins-compat.inc
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $@

$(ROCK_OBJECTS)/open_plugins.o: $(ROCK_OUT)/staged/open_plugins.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@
