# Staged MD5Sum with the upstream digest implementation.
$(ROCK_OUT)/staged/md5sum.c: $(ROCK_SRC)/md5sum.c \
    $(APPSDIR)/plugins/md5sum-compat.inc $(APPSDIR)/plugins/stage-md5sum.py
	$(SILENT)mkdir -p $(@D)
	$(SILENT)python3 $(word 3,$^) $< $(word 2,$^) $@

$(ROCK_OBJECTS)/md5sum.o: $(ROCK_OUT)/staged/md5sum.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -MMD -MP -c $< -o $@

$(ROCK_OUT)/modules/apps/md5sum.rock: $(ROCK_OBJECTS)/lib/md5.o
