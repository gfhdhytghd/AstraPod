# Include from the SDK build. Depend on ROCK_COMPAT_TABLES for SDK consumers.
ROCK_COMPAT_DIR := $(BUILDDIR)/rock-compat
ROCK_COMPAT_TABLES := $(addprefix $(ROCK_COMPAT_DIR)/,rock_compat_tables.h rock_compat_input.h rock_compat_sync.h mapping.tsv)
ROCK_COMPAT_TOOL := $(ROOTDIR)/tools/rock-compat/generate.py

# Rescan the plugin tree (including removed or renamed inputs) once per build.
# generate.py preserves output timestamps when content is identical.
.PHONY: rock-compat-scan
$(ROCK_COMPAT_TABLES) &: rock-compat-scan $(BUILDDIR)/lang_enum.h $(APPSBUILDDIR)/genlang-features
	$(SILENT)python3 $(ROCK_COMPAT_TOOL) --build "$(BUILDDIR)" --out "$(ROCK_COMPAT_DIR)"
