# GNU General Public License, version 2 or later.
# From the configured ipod6g SDL build directory:
# make -f ../tools/tests/podlink_shell_settings_sim.mk test-shell-settings
override BINARY := rockboxui-shell-settings-test
include Makefile

SHELL_TEST_OBJECTS := $(BUILDDIR)/shell-settings-test.o $(BUILDDIR)/shell-dispatch-test.o
SHELL_TEST_LANGUAGE := $(APPSBUILDDIR)/lang/english.lng

.PHONY: test-shell-settings
test-shell-settings: $(BUILDDIR)/$(BINARY) $(SHELL_TEST_LANGUAGE)
	$(SILENT)set -eu; ulimit -c 0; disk=$$(mktemp -d); trap 'rm -rf "$$disk"' EXIT; \
	  mkdir -p "$$disk/.rockbox/langs"; \
	  cp "$(SHELL_TEST_LANGUAGE)" "$$disk/.rockbox/langs/"; \
	  cp "$(APPSDIR)/tagnavi.config" "$$disk/.rockbox/"; \
	  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	  timeout -k 3 30 ./$(BINARY) --root "$$disk"

$(BUILDDIR)/$(BINARY): $(SHELL_TEST_OBJECTS)
$(BUILDDIR)/$(BINARY): private LDOPTS += -Wl,--wrap=root_menu

$(BUILDDIR)/shell-settings-test.o: $(ROOTDIR)/tools/tests/podlink_shell_settings_sim.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/shell-dispatch-test.o: $(APPSDIR)/settings/shell_dispatch.c
	$(CC) $(CFLAGS) -c $< -o $@
