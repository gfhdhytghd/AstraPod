# Audio utilities use private DRAM and checked temporary output files.
ROCK_ENC := mp3_encoder wav2wv vbrfix
ROCK_ENC_DIR := $(APPSDIR)/plugins

$(addprefix $(ROCK_OUT)/staged/,$(addsuffix .c,$(ROCK_ENC))): $(ROCK_OUT)/staged/%.c: \
    $(ROCK_SRC)/%.c $(ROCK_ENC_DIR)/stage-encoders.py $(ROCK_ENC_DIR)/%-compat.inc \
    $(ROCK_ENC_DIR)/encoder-file.h
	$(SILENT)mkdir -p "$(@D)"
	$(SILENT)python3 $(word 2,$^) $< $(word 3,$^) $@ $(word 5,$^)

$(ROCK_OUT)/staged/mp3_encoder.c: $(ROCK_ENC_DIR)/wavview-scan.inc
$(ROCK_OUT)/staged/vbrfix.c: $(ROOTDIR)/lib/rbcodec/metadata/mp3data.c

$(addprefix $(ROCK_OBJECTS)/,$(addsuffix .o,$(ROCK_ENC))): $(ROCK_OBJECTS)/%.o: \
    $(ROCK_OUT)/staged/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -I$(ROCK_ENC_DIR) -MMD -MP -c $< -o $@

# The firmware's codec WavPack library can use codec IRAM. Build the encoder
# from the same sources with the plugin SDK's DRAM declarations.
ROCK_WV_LIB := $(ROOTDIR)/lib/rbcodec/codecs/libwavpack
ROCK_WV_OBJ := $(addprefix $(ROCK_OBJECTS)/wavpack/,bits.o metadata.o pack.o words.o wputils.o)
$(ROCK_WV_OBJ): $(ROCK_OBJECTS)/wavpack/%.o: $(ROCK_WV_LIB)/%.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p "$(@D)"
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -include $(ROCK_SDK)/plugin.h -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/viewers/wav2wv.rock: $(ROCK_WV_OBJ)
-include $(wildcard $(ROCK_OBJECTS)/wavpack/*.d)
