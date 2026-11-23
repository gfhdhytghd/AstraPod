# MochiPod V3 from github.com/colliderkkz/mochipod, fetched at a pinned commit
# into $(MOCHIPOD_DIR) the first time. Its stage.py builds the same modules as
# one translation unit.
MOCHIPOD_URL := https://github.com/colliderkkz/mochipod.git
MOCHIPOD_COMMIT := fd60ce6ac21f4dec5bba7aa6f462af1558200f3a
MOCHIPOD_DIR ?= $(HOME)/.cache/podbox-mochipod/$(MOCHIPOD_COMMIT)
MOCHIPOD_UNITS := core/shop core/family core/rtc_guard core/growth \
    core/characters core/game core/play core/ui core/sound storage/save \
    input/wheel render/lcd platform/rockbox

$(MOCHIPOD_DIR)/.fetched:
	$(SILENT)rm -rf "$(@D)" && mkdir -p "$(@D)"
	$(SILENT)git -C "$(@D)" init -q
	$(SILENT)git -C "$(@D)" fetch -q --depth 1 $(MOCHIPOD_URL) $(MOCHIPOD_COMMIT)
	$(SILENT)git -C "$(@D)" checkout -q $(MOCHIPOD_COMMIT)
	$(SILENT)touch $@

$(ROCK_OUT)/staged/mochipod.c: $(MOCHIPOD_DIR)/.fetched
	$(SILENT)mkdir -p $(@D)
	$(SILENT)printf '#include "$(MOCHIPOD_DIR)/src/%s.c"\n' $(MOCHIPOD_UNITS) > $@

# The SDK names upstream's rb->filesize ffilesize.
$(ROCK_OBJECTS)/mochipod.o: $(ROCK_OUT)/staged/mochipod.c $(ROCK_FINGERPRINT)
	$(SILENT)mkdir -p $(@D)
	$(call PRINTS,CC $(@F))$(CC) $(ROCK_FLAGS) -Dfilesize=ffilesize -MMD -MP -c $< -o $@
