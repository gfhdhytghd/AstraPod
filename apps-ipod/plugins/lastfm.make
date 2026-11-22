# Native Audioscrobbler exporter and viewer. GNU GPL version 2 or later.
define rock_lastfm
$(ROCK_OUT)/staged/$(1).c: $(ROCK_SRC)/$(1).c \
    $(APPSDIR)/plugins/stage-lastfm.py $(APPSDIR)/plugins/$(2)-compat.inc \
    $(APPSDIR)/plugins/lastfm-log-compat.h
	$$(SILENT)mkdir -p $$(@D)
	$$(SILENT)python3 $$(word 2,$$^) $$< $$(word 3,$$^) $$@

$(ROCK_OBJECTS)/$(1).o: $(ROCK_OUT)/staged/$(1).c $(ROCK_FINGERPRINT)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,CC $$(@F))$$(CC) $$(ROCK_FLAGS) -MMD -MP -c $$< -o $$@
endef

$(eval $(call rock_lastfm,lastfm_scrobbler,lastfm-scrobbler))
$(eval $(call rock_lastfm,lastfm_scrobbler_viewer,lastfm-viewer))
