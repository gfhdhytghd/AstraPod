# iPod hardware H.264 viewer. GNU GPL version 2 or later.
ROCK_H264_SRC := $(ROCK_SRC)/h264_player
ROCK_H264_OBJ := $(ROCK_OBJECTS)/h264
ROCK_H264_STAGE := $(ROCK_OUT)/staged/h264
ROCK_H264_FLAGS = $(ROCK_FLAGS) -I$(ROCK_H264_SRC) -I$(ROOTDIR)/lib/rbcodec/codecs \
    -I$(FIRMDIR)/target/arm/s5l8702/ipod6g

$(ROCK_H264_STAGE)/%.c: $(ROCK_H264_SRC)/%.c $(APPSDIR)/plugins/stage-h264.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 2,$^) $< $@

$(ROCK_OBJECTS)/h264_player.o: $(ROCK_H264_STAGE)/h264_player.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_H264_FLAGS) -MMD -MP -c $< -o $@

ifneq ($(APP_TYPE),sdl-sim)
SRC += $(APPSDIR)/plugins/rock_h264_audio.c
$(ROCK_H264_OBJ)/mp4_demux.o: $(ROCK_H264_SRC)/mp4_demux.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_H264_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_H264_OBJ)/video_playback.o: $(ROCK_H264_STAGE)/video_playback.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_H264_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_H264_OBJ)/video_audio.o: $(ROCK_H264_STAGE)/video_audio.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_H264_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/h264_player.rock: $(ROCK_H264_OBJ)/mp4_demux.o $(ROCK_H264_OBJ)/video_playback.o $(ROCK_H264_OBJ)/video_audio.o
endif
-include $(wildcard $(ROCK_H264_OBJ)/*.d)
