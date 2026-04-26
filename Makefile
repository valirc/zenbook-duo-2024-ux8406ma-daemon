# =============================================================================
#  zbd — Zenbook Duo Daemon
#  Makefile
#
#  Targets (most common):
#      make            -- alias for `make all`, builds debug binaries.
#      make debug      -- explicit debug build (-O0 -g3, no -DNDEBUG).
#      make release    -- hardened release build
#                         (-O2 -DNDEBUG -fstack-protector-strong, PIE, RELRO).
#      make install    -- install everything under $(DESTDIR)$(PREFIX) and
#                         $(DESTDIR)/etc, $(DESTDIR)/usr/share, ...
#      make uninstall  -- remove what `install` placed on the system.
#      make clean      -- delete obj/ and bin/.
#      make format     -- run clang-format in place on src/ and tests/.
#      make lint       -- run clang-tidy on every translation unit.
#      make check      -- shorthand for `format-check` + `lint` + `test`.
#      make test       -- build and run the criterion unit tests.
#      make deb        -- build a .deb with debuild (requires debian/).
#      make help       -- print this list.
#
#  Variables a usuario puede sobrescribir:
#      DESTDIR         (empty)   — root for staged installs (packaging).
#      PREFIX          /usr/local — installation prefix.
#      DEBUG=1                   — force debug build (default).
#      RELEASE=1                 — force release build.
#      CC                        — C compiler (default: cc).
#      EXTRA_CFLAGS              — appended to CFLAGS.
#      EXTRA_LDFLAGS             — appended to LDFLAGS.
# =============================================================================

# ---- Project metadata --------------------------------------------------------
PROJECT      := zbd
VERSION      := 2.0.0-dev

# ---- Toolchain ---------------------------------------------------------------
CC           ?= cc
PKG_CONFIG   ?= pkg-config
INSTALL      ?= install

# ---- Installation paths ------------------------------------------------------
DESTDIR      ?=
PREFIX       ?= /usr/local
BIN_DIR      ?= $(PREFIX)/bin
SBIN_DIR     ?= $(PREFIX)/sbin
SYSCONF_DIR  ?= /etc
ICON_DIR     ?= $(PREFIX)/share/icons/zbd
WALLPAPER_DIR?= $(PREFIX)/share/backgrounds/zbd
SYSTEMD_SYS  ?= /usr/lib/systemd/system
SYSTEMD_USER ?= $(PREFIX)/lib/systemd/user
DBUS_SYSTEM_D?= /usr/share/dbus-1/system.d
POLKIT_DIR   ?= /usr/share/polkit-1/actions

# ---- Build mode --------------------------------------------------------------
ifeq ($(RELEASE),1)
  OPT_FLAGS  := -O2 -DNDEBUG
  HARDEN     := -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                -fPIE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
else
  OPT_FLAGS  := -O0 -g3 -fno-omit-frame-pointer
  HARDEN     :=
endif

# ---- Compilation flags -------------------------------------------------------
WARNINGS     := -Wall -Wextra -Wformat=2 -Wformat-security \
                -Wshadow -Wpointer-arith -Wstrict-prototypes \
                -Wmissing-prototypes -Wcast-align -Wnull-dereference \
                -Wno-deprecated-declarations
PKG_DEPS     := gtk+-3.0 ayatana-appindicator3-0.1 libusb-1.0 \
                glib-2.0 gio-2.0 libudev
PKG_CFLAGS   := $(shell $(PKG_CONFIG) --cflags $(PKG_DEPS))
PKG_LIBS     := $(shell $(PKG_CONFIG) --libs   $(PKG_DEPS))

CPPFLAGS     += -Isrc/include -D_GNU_SOURCE
CFLAGS       += -std=c17 -pthread -fPIC $(WARNINGS) $(OPT_FLAGS) $(HARDEN) \
                $(PKG_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS      += -pthread $(HARDEN) $(EXTRA_LDFLAGS)
LDLIBS       += $(PKG_LIBS)

# ---- Source layout -----------------------------------------------------------
SRC_DIR      := src
OBJ_DIR      := obj
BIN_DIR_BLD  := bin

# Common (linked into both binaries)
COMMON_SRCS  := $(SRC_DIR)/runtime.c $(SRC_DIR)/exec.c $(SRC_DIR)/config.c \
                $(SRC_DIR)/teclado.c $(SRC_DIR)/pantalla.c $(SRC_DIR)/audio.c \
                $(SRC_DIR)/display.c \
                $(SRC_DIR)/display_xrandr.c \
                $(SRC_DIR)/display_gdctl.c \
                $(SRC_DIR)/monitor_bluetooth.c \
                $(SRC_DIR)/monitor_orientacion.c \
                $(SRC_DIR)/monitor_teclado_usb.c
COMMON_OBJS  := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(COMMON_SRCS))

# Entry points
SYSTEM_OBJ   := $(OBJ_DIR)/main.o
TRAY_OBJ     := $(OBJ_DIR)/gui_daemon.o

SYSTEM_BIN   := $(BIN_DIR_BLD)/zbd-system
TRAY_BIN     := $(BIN_DIR_BLD)/zbd-tray

# ---- Default target ----------------------------------------------------------
.PHONY: all debug release install uninstall clean format format-check lint \
        test check deb help

all: debug

debug:   ; @$(MAKE) --no-print-directory _build
release: ; @$(MAKE) --no-print-directory RELEASE=1 _build

_build: $(SYSTEM_BIN) $(TRAY_BIN)
	@echo "  BUILD-OK  $(VERSION) ($(if $(RELEASE),release,debug))"

# ---- Linking -----------------------------------------------------------------
$(SYSTEM_BIN): $(COMMON_OBJS) $(SYSTEM_OBJ) | $(BIN_DIR_BLD)
	@echo "  LD       $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TRAY_BIN): $(COMMON_OBJS) $(TRAY_OBJ) | $(BIN_DIR_BLD)
	@echo "  LD       $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ---- Compilation -------------------------------------------------------------
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "  CC       $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR) $(BIN_DIR_BLD):
	@mkdir -p $@

# ---- Installation ------------------------------------------------------------
install: release
	@echo "  INSTALL  binaries"
	$(INSTALL) -d $(DESTDIR)$(SBIN_DIR) $(DESTDIR)$(BIN_DIR)
	$(INSTALL) -m 0755 $(SYSTEM_BIN) $(DESTDIR)$(SBIN_DIR)/
	$(INSTALL) -m 0755 $(TRAY_BIN)   $(DESTDIR)$(BIN_DIR)/
	@echo "  INSTALL  configuration"
	$(INSTALL) -d $(DESTDIR)$(SYSCONF_DIR)/zbd
	@if [ ! -e $(DESTDIR)$(SYSCONF_DIR)/zbd/zbd.conf ]; then \
	    $(INSTALL) -m 0644 conf/zbd.conf $(DESTDIR)$(SYSCONF_DIR)/zbd/zbd.conf ; \
	  else \
	    echo "  KEEP     $(DESTDIR)$(SYSCONF_DIR)/zbd/zbd.conf (user-modified)" ; \
	  fi
	@echo "  INSTALL  resources"
	$(INSTALL) -d $(DESTDIR)$(ICON_DIR)
	$(INSTALL) -m 0644 icono.svg $(DESTDIR)$(ICON_DIR)/zbd-tray.svg
	$(INSTALL) -d $(DESTDIR)$(WALLPAPER_DIR)
	$(INSTALL) -m 0644 fondos/bg_edp1.jpg $(DESTDIR)$(WALLPAPER_DIR)/
	$(INSTALL) -m 0644 fondos/bg_edp2.jpg $(DESTDIR)$(WALLPAPER_DIR)/
	@if [ -d systemd ]; then \
	    echo "  INSTALL  systemd units" ; \
	    $(INSTALL) -d $(DESTDIR)$(SYSTEMD_SYS) $(DESTDIR)$(SYSTEMD_USER) ; \
	    [ -f systemd/zbd-system.service ] && $(INSTALL) -m 0644 systemd/zbd-system.service $(DESTDIR)$(SYSTEMD_SYS)/ || true ; \
	    [ -f systemd/zbd-tray.service ]   && $(INSTALL) -m 0644 systemd/zbd-tray.service   $(DESTDIR)$(SYSTEMD_USER)/   || true ; \
	  fi
	@if [ -d polkit ]; then \
	    echo "  INSTALL  polkit policy" ; \
	    $(INSTALL) -d $(DESTDIR)$(POLKIT_DIR) ; \
	    [ -f polkit/org.anexa.zbd.policy ] && $(INSTALL) -m 0644 polkit/org.anexa.zbd.policy $(DESTDIR)$(POLKIT_DIR)/ || true ; \
	  fi
	@if [ -d dbus ]; then \
	    echo "  INSTALL  dbus policy" ; \
	    $(INSTALL) -d $(DESTDIR)$(DBUS_SYSTEM_D) ; \
	    [ -f dbus/org.anexa.zbd.conf ] && $(INSTALL) -m 0644 dbus/org.anexa.zbd.conf $(DESTDIR)$(DBUS_SYSTEM_D)/ || true ; \
	  fi
	@echo "  DONE     installed under $(DESTDIR)$(PREFIX)"

uninstall:
	@echo "  UNINSTALL"
	rm -f  $(DESTDIR)$(SBIN_DIR)/zbd-system
	rm -f  $(DESTDIR)$(BIN_DIR)/zbd-tray
	rm -rf $(DESTDIR)$(ICON_DIR)
	rm -rf $(DESTDIR)$(WALLPAPER_DIR)
	rm -f  $(DESTDIR)$(SYSTEMD_SYS)/zbd-system.service
	rm -f  $(DESTDIR)$(SYSTEMD_USER)/zbd-tray.service
	rm -f  $(DESTDIR)$(POLKIT_DIR)/org.anexa.zbd.policy
	rm -f  $(DESTDIR)$(DBUS_SYSTEM_D)/org.anexa.zbd.conf
	@echo "  KEEP     $(DESTDIR)$(SYSCONF_DIR)/zbd (user data)"

# ---- Quality -----------------------------------------------------------------
format:
	@echo "  FORMAT"
	@find $(SRC_DIR) tests -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null \
	    | xargs -r clang-format -i

format-check:
	@echo "  FORMAT-CHECK"
	@find $(SRC_DIR) tests -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null \
	    | xargs -r clang-format --dry-run --Werror

lint:
	@echo "  LINT     clang-tidy"
	@find $(SRC_DIR) -name '*.c' \
	    -exec clang-tidy --quiet {} -- $(CPPFLAGS) $(CFLAGS) \;

# ---- Tests -------------------------------------------------------------------
TEST_SRCS    := $(wildcard tests/test_*.c)
TEST_BINS    := $(patsubst tests/test_%.c,bin/test_%,$(TEST_SRCS))

test: $(TEST_BINS)
	@status=0 ; for t in $(TEST_BINS) ; do \
	    echo "  TEST     $$t" ; "$$t" || status=$$? ; \
	done ; exit $$status

bin/test_%: tests/test_%.c $(COMMON_OBJS) | $(BIN_DIR_BLD)
	@echo "  CC+LD    $@"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< \
	    $(filter-out $(SYSTEM_OBJ) $(TRAY_OBJ),$(COMMON_OBJS)) \
	    $(LDLIBS) -lcriterion

check: format-check lint test

# ---- Packaging ---------------------------------------------------------------
deb:
	@if [ ! -d debian ]; then \
	    echo "debian/ no existe; \`make deb\` necesita un debian/control" >&2 ; \
	    exit 1 ; \
	  fi
	dpkg-buildpackage -us -uc -b

# ---- Cleanup -----------------------------------------------------------------
clean:
	@echo "  CLEAN"
	rm -rf $(OBJ_DIR) $(BIN_DIR_BLD)

# ---- Help --------------------------------------------------------------------
help:
	@awk '/^# / && /^# {2,}/ { sub(/^# /, "", $$0); print }' $(MAKEFILE_LIST) | head -40
