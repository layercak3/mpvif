PKG_CONFIG ?= pkg-config

BASE_CFLAGS = -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter -Wno-c23-extensions -O2 $(shell $(PKG_CONFIG) --cflags mpv wayland-client)
BASE_LDFLAGS = $(shell $(PKG_CONFIG) --libs wayland-client)

WAYLAND_SCANNER = $(shell pkg-config --variable=wayland_scanner wayland-scanner)

HEADERS = foreign-toplevel-management-client-protocol.h virtual-pointer-client-protocol.h i3ipc.h
SOURCES = mpvif-plugin.c foreign-toplevel-management-client-protocol.c virtual-pointer-client-protocol.c

SCRIPTS_DIR := $(HOME)/.config/mpv/scripts

PREFIX := /usr/local
PLUGINDIR := $(PREFIX)/lib/mpvif-plugin
SYS_SCRIPTS_DIR := /etc/mpv/scripts

UID ?= $(shell id -u)

.PHONY: install install-user install-system \
	uninstall uninstall-user uninstall-system \
	clean

mpvif-plugin.so: $(HEADERS) $(SOURCES)
	$(CC) -o mpvif-plugin.so $(SOURCES) $(BASE_CFLAGS) $(CFLAGS) $(BASE_LDFLAGS) $(LDFLAGS) -shared -fPIC

foreign-toplevel-management-client-protocol.h:
	$(WAYLAND_SCANNER) client-header wlr-foreign-toplevel-management-unstable-v1.xml foreign-toplevel-management-client-protocol.h

virtual-pointer-client-protocol.h:
	$(WAYLAND_SCANNER) client-header wlr-virtual-pointer-unstable-v1.xml virtual-pointer-client-protocol.h

foreign-toplevel-management-client-protocol.c:
	$(WAYLAND_SCANNER) private-code wlr-foreign-toplevel-management-unstable-v1.xml foreign-toplevel-management-client-protocol.c

virtual-pointer-client-protocol.c:
	$(WAYLAND_SCANNER) private-code wlr-virtual-pointer-unstable-v1.xml virtual-pointer-client-protocol.c

ifneq ($(UID),0)
install: install-user
uninstall: uninstall-user
else
install: install-system
uninstall: uninstall-system
endif

install-user: mpvif-plugin.so
	install -Dm755 -t $(SCRIPTS_DIR) mpvif-plugin.so

uninstall-user: mpvif-plugin.so
	$(RM) $(SCRIPTS_DIR)/mpvif-plugin.so

install-system: mpvif-plugin.so
	install -Dm755 -t $(DESTDIR)$(PLUGINDIR) mpvif-plugin.so
	mkdir -p $(DESTDIR)$(SYS_SCRIPTS_DIR)
	ln -s $(PLUGINDIR)/mpvif-plugin.so $(DESTDIR)$(SYS_SCRIPTS_DIR)

uninstall-system: mpvif-plugin.so
	$(RM) $(DESTDIR)$(SYS_SCRIPTS_DIR)/mpvif-plugin.so
	$(RM) $(DESTDIR)$(PLUGINDIR)/mpvif-plugin.so
	-rmdir $(DESTDIR)$(PLUGINDIR) 2>/dev/null

clean:
	$(RM) mpvif-plugin.so \
        foreign-toplevel-management-client-protocol.h virtual-pointer-client-protocol.h \
        foreign-toplevel-management-client-protocol.c virtual-pointer-client-protocol.c
