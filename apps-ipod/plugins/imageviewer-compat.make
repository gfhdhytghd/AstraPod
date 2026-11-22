# Callable image decoder overlays. GNU GPL version 2 or later.
ROCK_IMAGE_SRC := $(ROCK_SRC)/imageviewer
ROCK_IMAGE_STAGE := $(ROCK_OUT)/staged/imageviewer
ROCK_IMAGE_OBJ := $(ROCK_OBJECTS)/imageviewer
ROCK_IMAGE_FLAGS = $(ROCK_FLAGS) -I$(ROCK_IMAGE_STAGE) -I$(ROCK_IMAGE_SRC) \
    -I$(ROOTDIR)/lib/tlsf/src -I$(APPSDIR)/draw
ROCK_IMAGE_FORMATS := bmp jpeg png ppm gif jpegp
ROCK_IMAGE_OVERLAYS := $(foreach fmt,$(ROCK_IMAGE_FORMATS),$(ROCK_OUT)/modules/overlays/image_$(fmt).ovl)

$(ROCK_IMAGE_STAGE)/%: $(ROCK_IMAGE_SRC)/% $(APPSDIR)/plugins/stage-imageviewer.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OBJECTS)/imageviewer_compat.o: $(ROCK_IMAGE_STAGE)/imageviewer.c $(ROCK_IMAGE_STAGE)/imageviewer.h $(ROCK_IMAGE_STAGE)/jpeg/yuv2rgb.h $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_IMAGE_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_IMAGE_OBJ)/image_decoder.o: $(ROCK_IMAGE_STAGE)/image_decoder.c $(ROCK_IMAGE_STAGE)/imageviewer.h $(ROCK_IMAGE_STAGE)/jpeg/yuv2rgb.h $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_IMAGE_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/imageviewer_compat.rock: $(ROCK_IMAGE_OBJ)/image_decoder.o $(ROCK_IMAGE_OVERLAYS) \
    $(addprefix $(ROCK_OBJECTS)/lib/,configfile.o helper.o playback_control.o xlcd_core.o xlcd_draw.o xlcd_scroll.o)


$(ROCK_IMAGE_OBJ)/%.o: $(ROCK_IMAGE_SRC)/%.c $(ROCK_IMAGE_STAGE)/imageviewer.h $(ROCK_IMAGE_STAGE)/jpeg/yuv2rgb.h $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(subst $(ROCK_IMAGE_SRC)/,,$<))$(CC) $(ROCK_IMAGE_FLAGS) \
	    -DIMGDEC -include $(ROCK_IMAGE_STAGE)/imageviewer.h -include $(ROCK_IMAGE_STAGE)/jpeg/yuv2rgb.h -MMD -MP -c $< -o $@

ROCK_IMAGE_JPEGP_OBJECTS := $(patsubst $(ROCK_IMAGE_SRC)/%.c,$(ROCK_IMAGE_OBJ)/%.o, \
    $(call preprocess,$(ROCK_IMAGE_SRC)/jpegp/SOURCES))
$(ROCK_IMAGE_JPEGP_OBJECTS): $(ROCK_IMAGE_OBJ)/jpegp/%.o: $(ROCK_IMAGE_STAGE)/jpegp/%.c $(ROCK_IMAGE_STAGE)/jpegp/rb_glue.h \
    $(ROCK_IMAGE_STAGE)/imageviewer.h $(ROCK_IMAGE_STAGE)/jpeg/yuv2rgb.h $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_IMAGE_FLAGS) -I$(ROCK_IMAGE_SRC)/jpegp \
	    -DIMGDEC -include $(ROCK_IMAGE_STAGE)/imageviewer.h -MMD -MP -c $< -o $@

ROCK_IMAGE_CRT := $(ROCK_OBJECTS)/image_decoder_crt0.o
$(ROCK_IMAGE_CRT): $(ROCK_SRC)/plugin_crt0.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -DROCK_IMAGE_DECODER -MMD -MP -c $< -o $@

ifneq ($(APP_TYPE),sdl-sim)
ROCK_IMAGE_MEMORY := $(ROCK_IMAGE_OBJ)/image-decoder-memory.o
$(ROCK_IMAGE_MEMORY): $(APPSDIR)/plugins/image-decoder-memory.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@
ROCK_IMAGE_LINK := $(ROCK_OUT)/image-decoder.link
$(ROCK_IMAGE_LINK): $(APPSDIR)/plugins/plugin.lds $(APPSDIR)/plugins/rock_image_overlay.h
	$(SILENT)mkdir -p $(@D)
	$(call preprocess2file,$<,$@,-DPLUGIN -DASTRAPOD_ROCK -DASTRAPOD_IMAGE_DECODER)
endif

define rock_image_link
$(ROCK_OUT)/modules/overlays/image_$(1).ovl: \
    $(patsubst $(ROCK_IMAGE_SRC)/%.c,$(ROCK_IMAGE_OBJ)/%.o,$(call preprocess,$(ROCK_IMAGE_SRC)/$(1)/SOURCES)) \
    $(ROCK_IMAGE_CRT) $(ROCK_IMAGE_MEMORY) $(ROCK_IMAGE_LINK) $(filter-out $(ROCK_OUT)/plugin.link,$(ROCK_LINK_INPUT)) $(2)
	$$(SILENT)mkdir -p $$(@D)
	$$(call PRINTS,LD $$(@F))$$(CC) $$(ROCK_IMAGE_FLAGS) $$(subst $(ROCK_OUT)/plugin.link,$(ROCK_IMAGE_LINK),$$(ROCK_LINK)) \
	    -Wl,--exclude-libs,ALL -o $$@.elf $$(filter %.o %.a,$$^) -lgcc
	$$(SILENT)$$(ROCK_OUTPUT)
endef

ROCK_IMAGE_SMOOTH := $(ROCK_OBJECTS)/lib/bmp_smooth_scale.o
$(eval $(call rock_image_link,jpeg))
$(eval $(call rock_image_link,jpegp,$(TLSFLIB)))
$(eval $(call rock_image_link,png,$(ROCK_IMAGE_SMOOTH)))
$(eval $(call rock_image_link,ppm,$(ROCK_IMAGE_SMOOTH)))
$(eval $(call rock_image_link,gif,$(ROCK_IMAGE_SMOOTH) $(TLSFLIB)))
$(eval $(call rock_image_link,bmp,$(ROCK_IMAGE_SMOOTH)))

-include $(wildcard $(ROCK_IMAGE_OBJ)/*.d $(ROCK_IMAGE_OBJ)/*/*.d)
