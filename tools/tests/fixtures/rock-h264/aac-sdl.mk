# GNU GPL version 2 or later.
# Run in a dedicated ipod6g SDL build configured with --rock-compat.
# After make bin, make -f <repo>/tools/tests/fixtures/rock-h264/aac-sdl.mk test-aac
override BINARY := rockboxui-aac-probe
include Makefile

AAC_DIR := $(BUILDDIR)/aac-probe
AAC_FIXTURE := $(ROOTDIR)/tools/tests/fixtures/rock-h264
AAC_OBJS := $(addprefix $(AAC_DIR)/,probe.o video_audio.o mp4_demux.o lease.o)
AAC_CODEC := $(RBCODEC_BLD)/codecs/aac.codec
AAC_MP4 := $(BUILDDIR)/simdisk/probe.mp4

.PHONY: test-aac
test-aac: $(BUILDDIR)/$(BINARY) $(AAC_CODEC) $(AAC_MP4)
	mkdir -p simdisk/.rockbox/codecs
	cp $(AAC_CODEC) simdisk/.rockbox/codecs/
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 3 25 ./$(BINARY)

$(BUILDDIR)/$(BINARY): $(AAC_OBJS)
$(BUILDDIR)/$(BINARY): private LDOPTS += -Wl,--wrap=root_menu \
    -Wl,--wrap=pcm_play_dma_complete_callback -Wl,--wrap=mixer_channel_stop

$(AAC_DIR)/probe.o: $(AAC_FIXTURE)/aac-sdl.c $(ROCK_FINGERPRINT)
	mkdir -p $(@D)
	$(CC) $(ROCK_H264_FLAGS) -c $< -o $@

$(AAC_DIR)/video_audio.o: $(ROCK_H264_STAGE)/video_audio.c $(ROCK_FINGERPRINT)
	mkdir -p $(@D)
	$(CC) $(ROCK_H264_FLAGS) -c $< -o $@

$(AAC_DIR)/mp4_demux.o: $(ROCK_H264_SRC)/mp4_demux.c $(ROCK_FINGERPRINT)
	mkdir -p $(@D)
	$(CC) $(ROCK_H264_FLAGS) -c $< -o $@

# Enable the native lease only in this test object. Keep the SDL SDK layout.
$(AAC_DIR)/lease.o: $(APPSDIR)/plugins/rock_h264_audio.c $(APPSDIR)/plugins/rock_h264_audio.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -DHAVE_HW_H264 -c $< -o $@

$(AAC_MP4): $(AAC_FIXTURE)/aac-sdl.mk
	mkdir -p $(@D)
	ffmpeg -loglevel error -y -f lavfi -i 'testsrc2=size=160x120:rate=24' \
	    -f lavfi -i 'aevalsrc=0.20*sin(2*PI*(440*t+440*max(t-2\,0))):s=44100' \
	    -t 6 -map 0:v -map 1:a -c:v libx264 -profile:v baseline -level:v 3.0 \
	    -pix_fmt yuv420p -g 48 -c:a aac -profile:a aac_low -ac 2 \
	    -movflags +faststart $@
