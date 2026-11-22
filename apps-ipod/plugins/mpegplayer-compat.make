# Stock MPEGPlayer with foreground ownership. GNU GPL version 2 or later.
ROCK_MPEG_SRC := $(ROCK_SRC)/mpegplayer
ROCK_MPEG_STAGE := $(ROCK_OUT)/staged/mpegplayer
ROCK_MPEG_OBJ := $(ROCK_OBJECTS)/mpegplayer
ROCK_MPEG_GLUE := $(APPSDIR)/plugins
ROCK_MPEG_MAD := $(ROOTDIR)/lib/rbcodec/codecs/libmad
ROCK_MPEG_FLAGS = $(ROCK_FLAGS) -I$(ROCK_MPEG_STAGE) -I$(ROCK_MPEG_GLUE) \
    -I$(ROOTDIR)/lib/rbcodec -fgnu89-inline
ROCK_MPEG_PATCH := alloc.c audio_thread.c disk_buf.c mpeg_misc.c mpegplayer.h \
    stream_mgr.c stream_mgr.h stream_thread.h video_out_rockbox.c video_thread.c \
    libmpeg2/header.c libmpeg2/decode.c
ROCK_MPEG_SOURCES := $(patsubst $(ROCK_MPEG_SRC)/%,%,\
    $(filter-out %/mpegplayer.c %/mpeg_settings.c %/pcm_output.c,\
        $(call preprocess,$(ROCK_MPEG_SRC)/SOURCES)))
ROCK_MPEG_HEADERS := $(patsubst $(ROCK_MPEG_SRC)/%,%,\
    $(wildcard $(ROCK_MPEG_SRC)/*.h $(ROCK_MPEG_SRC)/libmpeg2/*.h))
ROCK_MPEG_INPUTS := $(addprefix $(ROCK_MPEG_STAGE)/,$(ROCK_MPEG_SOURCES) $(ROCK_MPEG_HEADERS))

$(addprefix $(ROCK_MPEG_STAGE)/,$(ROCK_MPEG_PATCH)): $(ROCK_MPEG_STAGE)/%: $(ROCK_MPEG_SRC)/% $(ROCK_MPEG_GLUE)/stage-mpegplayer.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(filter-out $(addprefix $(ROCK_MPEG_STAGE)/,$(ROCK_MPEG_PATCH)),$(ROCK_MPEG_INPUTS)): $(ROCK_MPEG_STAGE)/%: $(ROCK_MPEG_SRC)/%
	$(SILENT)mkdir -p $(@D)
	$(SILENT)cp $< $@

$(addprefix $(ROCK_MPEG_OBJ)/,$(addsuffix .o,$(filter %.c,$(ROCK_MPEG_SOURCES)))): $(ROCK_MPEG_OBJ)/%.c.o: $(ROCK_MPEG_STAGE)/%.c $(ROCK_FINGERPRINT) | $(ROCK_MPEG_INPUTS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC mpegplayer/$(*F))$(CC) $(ROCK_MPEG_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_MPEG_OBJ)/%.S.o: $(ROCK_MPEG_STAGE)/%.S $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,AS mpegplayer/$(*F))$(CC) $(ROCK_MPEG_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OBJECTS)/mpegplayer.o: $(ROCK_MPEG_GLUE)/mpegplayer-compat.c $(ROCK_FINGERPRINT) | $(ROCK_MPEG_INPUTS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_MPEG_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_MPEG_OBJ)/pcm.o: $(ROCK_MPEG_GLUE)/mpegplayer-pcm.c $(ROCK_FINGERPRINT) | $(ROCK_MPEG_INPUTS)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_MPEG_FLAGS) -MMD -MP -c $< -o $@

# libmad is a private module copy. Use the usual decoder flags but put its
# assembly and data in DRAM. The native playback codec keeps its normal build.
ROCK_MPEG_MAD_SOURCES := $(patsubst $(ROCK_MPEG_MAD)/%,%,\
    $(call preprocess,$(ROCK_MPEG_MAD)/SOURCES))
ROCK_MPEG_MAD_FLAGS = $(MPEGMADFLAGS) -fno-lto -ffunction-sections \
    -fdata-sections -include $(ROCK_MPEG_GLUE)/mpegplayer-dram.h

$(ROCK_MPEG_OBJ)/mad/%.c.o: $(ROCK_MPEG_MAD)/%.c $(ROCK_MPEG_GLUE)/mpegplayer-dram.h
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC mpegplayer/mad/$(*F))$(CC) $(ROCK_MPEG_MAD_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_MPEG_OBJ)/mad/%.S.o: $(ROCK_MPEG_MAD)/%.S $(ROCK_MPEG_GLUE)/mpegplayer-dram.h
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,AS mpegplayer/mad/$(*F))$(CC) $(ROCK_MPEG_MAD_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/mpegplayer.rock: $(ROCK_MPEG_OBJ)/pcm.o \
    $(addprefix $(ROCK_MPEG_OBJ)/,$(addsuffix .o,$(ROCK_MPEG_SOURCES))) \
    $(addprefix $(ROCK_MPEG_OBJ)/mad/,$(addsuffix .o,$(ROCK_MPEG_MAD_SOURCES)))
-include $(wildcard $(ROCK_MPEG_OBJ)/*.d $(ROCK_MPEG_OBJ)/*/*.d)
