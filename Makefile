# Project configuration
PROJECT_NAME := waycentral
LIB_SRCS := src/ap.c src/audio.c src/batt.c src/clock.c src/date.c \
	src/hyprland.c src/lock.c src/music.c src/network.c src/sysmon.c \
	src/util.c src/sysmon/cpu.c src/sysmon/mem.c src/sysmon/net.c \
	src/updates/aur.c src/updates/pacman.c
TRAY_SRCS := src/tray/systray.c
LIB_DEPS := libalpm libcurl json-c libsystemd libpulse
TRAY_DEPS := gtk+-3.0
BIN_SRCS := waycentral.c
HEADER := waycentral.h
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
PKG_DIR := $(BUILD_DIR)/package

VERSION ?= $(shell cat ./VERSION 2>/dev/null || echo "0.0.0")
MAJOR := $(firstword $(subst ., ,$(VERSION)))

CC ?= cc
CFLAGS ?= -O2
DEBUG_FLAGS := -g -O0
WARN_FLAGS := -Wall -Wextra -Wno-format-truncation
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(LIB_DEPS))
DEP_LIBS := $(shell $(PKG_CONFIG) --libs $(LIB_DEPS))
TRAY_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(TRAY_DEPS))
TRAY_LIBS := $(shell $(PKG_CONFIG) --libs $(TRAY_DEPS))
COMMON_FLAGS = $(WARN_FLAGS) -fPIC -fvisibility=hidden $(DEP_CFLAGS) \
	-DWC_VERSION='"$(VERSION)"'

ifeq ($(shell id -u),0)
PREFIX ?= /usr
else
PREFIX ?= $(HOME)/.local
endif
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCDIR ?= $(PREFIX)/include
PCDIR ?= $(LIBDIR)/pkgconfig
MODDIR ?= $(LIBDIR)/$(PROJECT_NAME)
LICENSEDIR ?= $(PREFIX)/share/licenses/$(PROJECT_NAME)

# Outputs
LIB := lib$(PROJECT_NAME).so
SONAME := $(LIB).$(MAJOR)
REAL_LIB := $(LIB).$(VERSION)
STATIC_LIB := lib$(PROJECT_NAME).a
RELEASE_BIN := $(BIN_DIR)/$(PROJECT_NAME)
DEBUG_BIN := $(BIN_DIR)/$(PROJECT_NAME)-debug
PACKAGE_FILE := $(PROJECT_NAME)-$(VERSION).tar.gz
PC_FILE := $(BUILD_DIR)/$(PROJECT_NAME).pc
TRAY_MOD := $(BIN_DIR)/$(PROJECT_NAME)-tray.so

RELEASE_OBJS := $(LIB_SRCS:%.c=$(BUILD_DIR)/release/%.o)
DEBUG_OBJS := $(LIB_SRCS:%.c=$(BUILD_DIR)/debug/%.o)

# Default target
all: release debug package

# Binary (library linked in statically) + shared and static library
release: $(RELEASE_BIN) $(BIN_DIR)/$(REAL_LIB) $(BIN_DIR)/$(STATIC_LIB) \
	$(PC_FILE) $(TRAY_MOD)

debug: $(DEBUG_BIN)

# Compile test
test:
	$(CC) $(DEBUG_FLAGS) $(WARN_FLAGS) $(DEP_CFLAGS) \
		-DWC_VERSION='"$(VERSION)"' $(BIN_SRCS) $(LIB_SRCS) \
		-o $(PROJECT_NAME)-test $(DEP_LIBS)

$(BUILD_DIR)/release/%.o: %.c $(HEADER) src/util.h src/updates/updates.h VERSION
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DNDEBUG $(COMMON_FLAGS) -c $< -o $@

$(BUILD_DIR)/debug/%.o: %.c $(HEADER) src/util.h src/updates/updates.h VERSION
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $(COMMON_FLAGS) -c $< -o $@

$(RELEASE_BIN): $(BIN_SRCS) $(RELEASE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -DNDEBUG $(WARN_FLAGS) $^ -o $@ $(LDFLAGS) $(DEP_LIBS)
	@cp -f LICENSE $(BIN_DIR)/LICENSE 2>/dev/null || true
	@echo "✅ Built release binary: $@"

$(DEBUG_BIN): $(BIN_SRCS) $(DEBUG_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(DEBUG_FLAGS) $(WARN_FLAGS) $^ -o $@ $(LDFLAGS) $(DEP_LIBS)
	@echo "🐛 Built debug binary: $@"

$(BIN_DIR)/$(REAL_LIB): $(RELEASE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME) $^ -o $@ $(LDFLAGS) \
		$(DEP_LIBS)
	@ln -sf $(REAL_LIB) $(BIN_DIR)/$(SONAME)
	@ln -sf $(SONAME) $(BIN_DIR)/$(LIB)
	@echo "📚 Built shared library: $@"

# Waybar CFFI module, the wbcffi_* symbols must stay visible
$(TRAY_MOD): $(TRAY_SRCS) src/tray/waybar_cffi_module.h
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(WARN_FLAGS) -fPIC -shared $(TRAY_CFLAGS) $(TRAY_SRCS) \
		-o $@ $(LDFLAGS) $(TRAY_LIBS)
	@echo "🧩 Built Waybar tray module: $@"

$(BIN_DIR)/$(STATIC_LIB): $(RELEASE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(AR) rcs $@ $^

$(PC_FILE): VERSION
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' \
		'prefix=$(PREFIX)' \
		'libdir=$(LIBDIR)' \
		'includedir=$(INCDIR)' \
		'' \
		'Name: $(PROJECT_NAME)' \
		'Description: Centralized Waybar modules as a C library' \
		'Version: $(VERSION)' \
		'Requires.private: $(LIB_DEPS)' \
		'Libs: -L$${libdir} -l$(PROJECT_NAME)' \
		'Cflags: -I$${includedir}' > $@

# Installation logic (root -> /usr, user -> ~/.local; override with PREFIX=)
install: release
	install -Dm755 $(RELEASE_BIN) $(DESTDIR)$(BINDIR)/$(PROJECT_NAME)
	install -Dm755 $(BIN_DIR)/$(REAL_LIB) $(DESTDIR)$(LIBDIR)/$(REAL_LIB)
	ln -sf $(REAL_LIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/$(LIB)
	install -Dm644 $(BIN_DIR)/$(STATIC_LIB) $(DESTDIR)$(LIBDIR)/$(STATIC_LIB)
	install -Dm644 $(HEADER) $(DESTDIR)$(INCDIR)/$(HEADER)
	install -Dm644 $(PC_FILE) $(DESTDIR)$(PCDIR)/$(PROJECT_NAME).pc
	install -Dm755 $(TRAY_MOD) $(DESTDIR)$(MODDIR)/$(PROJECT_NAME)-tray.so

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROJECT_NAME) \
		$(DESTDIR)$(LIBDIR)/$(REAL_LIB) $(DESTDIR)$(LIBDIR)/$(SONAME) \
		$(DESTDIR)$(LIBDIR)/$(LIB) $(DESTDIR)$(LIBDIR)/$(STATIC_LIB) \
		$(DESTDIR)$(INCDIR)/$(HEADER) $(DESTDIR)$(PCDIR)/$(PROJECT_NAME).pc \
		$(DESTDIR)$(MODDIR)/$(PROJECT_NAME)-tray.so

# Package binary, library and header into a tar.gz
package: release
	@rm -rf $(PKG_DIR)
	@mkdir -p $(PKG_DIR)
	@cp -P $(RELEASE_BIN) $(BIN_DIR)/$(LIB)* $(BIN_DIR)/$(STATIC_LIB) \
		$(TRAY_MOD) $(HEADER) LICENSE $(PKG_DIR)/
	@tar -czf $(PACKAGE_FILE) -C $(PKG_DIR) .
	@echo "📦 Packaged into: $(PACKAGE_FILE)"

# Clean up build artifacts
clean:
	rm -rf $(BUILD_DIR) $(PROJECT_NAME)-*.tar.gz
	@echo "🧹 Cleaned up build files"

.PHONY: all release debug test clean install uninstall package
