# Standalone upstream PictureFlow with native AstraPod queries. GPL-2.0-or-later.
SRC += $(APPSDIR)/plugins/rock_pictureflow.c

ROCK_PF_ADAPTERS := $(addprefix $(APPSDIR)/plugins/,stage-pictureflow.py \
    pictureflow-library.inc pictureflow-cache.inc pictureflow-ui.inc)

$(ROCK_OUT)/staged/pictureflow.c: $(ROCK_SRC)/pictureflow/pictureflow.c $(ROCK_PF_ADAPTERS)
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OBJECTS)/pictureflow.o: private ROCK_FLAGS += -O2
$(eval $(call rock_module_bitmaps,demos/pictureflow,pictureflow_emptyslide pictureflow_logo))

$(ROCK_OUT)/modules/demos/pictureflow.rock: $(addprefix $(ROCK_OBJECTS)/lib/,\
    configfile.o helper.o playback_control.o) $(ROCK_OBJECTS)/plugin_crt0_large_stack.o

$(ROCK_OBJECTS)/pictureflow.o: $(ROCK_OUT)/staged/pictureflow.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -I$(APPSDIR)/plugins -MMD -MP -c $< -o $@

$(ROCK_BITMAP_DIR)/pictureflow_emptyslide.c $(ROCK_BITMAP_DIR)/pictureflow_emptyslide.h: \
    $(ROCK_SRC)/bitmaps/native/pictureflow_emptyslide.100x100x16.bmp
$(ROCK_BITMAP_DIR)/pictureflow_logo.c $(ROCK_BITMAP_DIR)/pictureflow_logo.h: \
    $(ROCK_SRC)/bitmaps/native/pictureflow_logo.193x34x16.bmp
