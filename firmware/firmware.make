#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

INCLUDES += -I$(FIRMDIR) -I$(FIRMDIR)/export -I$(FIRMDIR)/drivers \
			-I$(FIRMDIR)/include -I$(FIRMDIR)/kernel/include
ifndef APP_TYPE
INCLUDES += -I$(FIRMDIR)/libc/include
endif

ifneq ($(filter reggen,$(TOOLSET)),)
  include $(FIRMDIR)/reggen/reggen.make
endif

FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/asm/SOURCES)
FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/SOURCES)
# Experimental firmware implementation. C is the default and needs no Rust.
PODLINK_RUST ?= 0
ifeq ($(PODLINK_RUST),1)
  ifneq ($(MODELNAME),ipod6g)
    $(error PODLINK_RUST=1 is currently supported only for ipod6g)
  endif
  ifneq ($(APP_TYPE),)
    $(error PODLINK_RUST=1 requires a native firmware build)
  endif
  ifneq ($(findstring -DBOOTLOADER,$(EXTRA_DEFINES)),)
    $(error PODLINK_RUST=1 requires a normal firmware build)
  endif
  FIRMLIB_SRC := $(filter-out %/usbstack/podlink.c %/usbstack/podlink_wire.c %/usbstack/podlink_service.c,$(FIRMLIB_SRC))
  FIRMLIB_SRC += $(FIRMDIR)/usbstack/podlink_rust_platform.c
  PODLINK_RUST_DIR := $(FIRMDIR)/usbstack/podlink-rust
  PODLINK_RUST_TARGET := $(BUILDDIR)/podlink-rust-target
  PODLINK_RUST_LIB := $(PODLINK_RUST_TARGET)/armv5te-none-eabi/release/libpodlink_rust.a
  CORE_LDOPTS += -L$(dir $(PODLINK_RUST_LIB))
  PODLINK_RUST_TOOLCHAIN ?= nightly
  $(PODLINK_RUST_LIB): $(wildcard $(PODLINK_RUST_DIR)/src/*.rs) $(PODLINK_RUST_DIR)/Cargo.toml $(PODLINK_RUST_DIR)/Cargo.lock
	$(call PRINTS,RUST PodLink)RUSTFLAGS="-Ctarget-cpu=arm926ej-s -Cpanic=abort -Cforce-frame-pointers=yes" cargo +$(PODLINK_RUST_TOOLCHAIN) build --locked --manifest-path $(PODLINK_RUST_DIR)/Cargo.toml --target-dir $(PODLINK_RUST_TARGET) --release --features firmware --target armv5te-none-eabi -Zbuild-std=core
endif
FIRMLIB_OBJ := $(call c2obj, $(FIRMLIB_SRC))
FIRMLIB_OBJ += $(BUILDDIR)/sysfont.o
OTHER_SRC += $(FIRMLIB_SRC)

FIRMLIB = $(BUILDDIR)/firmware/libfirmware.a

ifeq ($(SYSFONT),)
SYSFONT = 08-Schumacher-Clean
endif
SYSFONTX = $(ROOTDIR)/fonts/$(SYSFONT).bdf

CLEANOBJS += $(BUILDDIR)/sysfont.* $(BUILDDIR)/version.*

# Limits for the built-in sysfont: ASCII for bootloaders, ISO8859-1 for normal builds
ifneq (,$(findstring -DBOOTLOADER,$(EXTRA_DEFINES)))
	MAXCHAR = 127
else
	MAXCHAR = 255
endif

# Invalidate the archive on either switch direction, even when older C objects
# already exist. Never leave Rust platform members in a subsequent C build.
PODLINK_MODE_STAMP := $(BUILDDIR)/.podlink-mode-$(PODLINK_RUST)
$(PODLINK_MODE_STAMP):
	$(SILENT)rm -f $(BUILDDIR)/.podlink-mode-0 $(BUILDDIR)/.podlink-mode-1
	$(SILENT)touch $@

$(FIRMLIB): $(FIRMLIB_OBJ) $(PODLINK_MODE_STAMP)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $(FIRMLIB_OBJ) >/dev/null

$(BUILDDIR)/sysfont.h: $(SYSFONTX) $(TOOLS) $(BUILDDIR)/firmware/common/config.o
	$(call PRINTS,CONVBDF $(subst $(ROOTDIR)/,,$<))$(TOOLSDIR)/convbdf -l $(MAXCHAR) -h -o $@ $<

$(BUILDDIR)/sysfont.o: $(SYSFONTX) $(BUILDDIR)/sysfont.h
	$(call PRINTS,CONVBDF $(subst $(ROOTDIR)/,,$<))$(TOOLSDIR)/convbdf -l $(MAXCHAR) -c -o $(BUILDDIR)/sysfont.c $<
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$(BUILDDIR)/sysfont.c))$(CC) $(CFLAGS) -c $(BUILDDIR)/sysfont.c -o $@

ifdef USE_LTO
$(BUILDDIR)/firmware/asm/%.o: CFLAGS += -fno-lto
$(BUILDDIR)/firmware/kernel/thread.o: CFLAGS += -fno-lto
endif

# GNU make (at least) has a bug/feature that exported variable are not available
# in the shell function (but are in recipe). Thus we need to explicitely pass
# the VERSION environement variable
SVNVERSION:=$(shell VERSION='$(VERSION)' $(TOOLSDIR)/version.sh $(ROOTDIR))
OLDSVNVERSION:=$(shell grep 'RBVERSION' $(BUILDDIR)/rbversion.h 2>/dev/null|cut -d '"' -f 2 || echo "NOREVISION")

CORE_LIBS:=$(PODLINK_RUST_LIB) $(FIRMLIB)

ifneq ($(SVNVERSION),$(OLDSVNVERSION))
.PHONY: $(BUILDDIR)/rbversion.h
endif

$(BUILDDIR)/rbversion.h:
	$(call PRINTS,GEN $(@F))$(TOOLSDIR)/genversion.sh $(BUILDDIR) $(SVNVERSION)
