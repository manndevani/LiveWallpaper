# livewall for Linux. Needs: a C compiler, pkg-config, wayland-scanner and the
# development files for wayland-client, wayland-egl, wayland-protocols, egl,
# glesv2, x11 and libmpv (headers only: libmpv is loaded at runtime).
#
#   make            -> build/livewall
#   make install    -> ~/.local/bin/livewall (PREFIX=/usr/local for everyone)

CC      ?= cc
PREFIX  ?= $(HOME)/.local
BUILD   := build
PKGS    := wayland-client wayland-egl egl glesv2 x11
CFLAGS  ?= -O2
VERSION := $(shell sed -n 's/^\#define LW_VERSION_STR "\(.*\)"/\1/p' ../src/version.h)
CFLAGS  += -DLW_VERSION='"$(VERSION)"' -std=c11 -D_GNU_SOURCE -Wall -Wextra -I$(BUILD) $(shell pkg-config --cflags $(PKGS) mpv)
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -ldl -lm

WL_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
SCANNER      := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

GEN := $(BUILD)/builtin_shaders.h \
       $(BUILD)/wlr-layer-shell-unstable-v1-client-protocol.h $(BUILD)/wlr-layer-shell-unstable-v1-protocol.c \
       $(BUILD)/xdg-shell-client-protocol.h $(BUILD)/xdg-shell-protocol.c
SRC := main.c config.c gl.c media.c png.c wayland.c x11.c
OBJ := $(SRC:%.c=$(BUILD)/%.o) $(BUILD)/wlr-layer-shell-unstable-v1-protocol.o $(BUILD)/xdg-shell-protocol.o

$(BUILD)/livewall: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)
	strip $@

$(BUILD)/%.o: %.c lw.h $(GEN) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(BUILD)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/builtin_shaders.h: embed.sh $(wildcard ../shaders/glsl/*.glsl) | $(BUILD)
	sh embed.sh ../shaders/glsl > $@

$(BUILD)/wlr-layer-shell-unstable-v1-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml | $(BUILD)
	$(SCANNER) client-header $< $@
$(BUILD)/wlr-layer-shell-unstable-v1-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml | $(BUILD)
	$(SCANNER) private-code $< $@
$(BUILD)/xdg-shell-client-protocol.h: | $(BUILD)
	$(SCANNER) client-header $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
$(BUILD)/xdg-shell-protocol.c: | $(BUILD)
	$(SCANNER) private-code $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

$(BUILD):
	mkdir -p $@

install: $(BUILD)/livewall
	install -Dm755 $(BUILD)/livewall $(DESTDIR)$(PREFIX)/bin/livewall

clean:
	rm -rf $(BUILD)

.PHONY: install clean
