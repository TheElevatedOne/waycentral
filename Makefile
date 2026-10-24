# Project configuration
PROJECT_NAME := waycentral
LIB_SRCS := src/ap.c src/batt.c src/clock.c src/date.c src/lock.c \
	src/sysmon.c src/util.c src/sysmon/cpu.c src/sysmon/mem.c src/sysmon/net.c
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
WARN_FLAGS := -Wall -Wextra
LDFLAGS ?=
COMMON_FLAGS = $(WARN_FLAGS) -fPIC -fvisibility=hidden \
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

RELEASE_OBJS := $(LIB_SRCS:%.c=$(BUILD_DIR)/release/%.o)
DEBUG_OBJS := $(LIB_SRCS:%.c=$(BUILD_DIR)/debug/%.o)

# Default target
all: release debug package

# Binary (library linked in statically) + shared and static library
release: $(RELEASE_BIN) $(BIN_DIR)/$(REAL_LIB) $(BIN_DIR)/$(STATIC_LIB) $(PC_FILE)

debug: $(DEBUG_BIN)

# Compile test
test:
	$(CC) $(DEBUG_FLAGS) $(WARN_FLAGS) -DWC_VERSION='"$(VERSION)"' \
		$(BIN_SRCS) $(LIB_SRCS) -o $(PROJECT_NAME)-test

$(BUILD_DIR)/release/%.o: %.c $(HEADER) src/util.h VERSION
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DNDEBUG $(COMMON_FLAGS) -c $< -o $@

$(BUILD_DIR)/debug/%.o: %.c $(HEADER) src/util.h VERSION
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_FLAGS) $(COMMON_FLAGS) -c $< -o $@

$(RELEASE_BIN): $(BIN_SRCS) $(RELEASE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -DNDEBUG $(WARN_FLAGS) $^ -o $@ $(LDFLAGS)
	@cp -f LICENSE $(BIN_DIR)/LICENSE 2>/dev/null || true
	@echo "✅ Built release binary: $@"

$(DEBUG_BIN): $(BIN_SRCS) $(DEBUG_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(DEBUG_FLAGS) $(WARN_FLAGS) $^ -o $@ $(LDFLAGS)
	@echo "🐛 Built debug binary: $@"

$(BIN_DIR)/$(REAL_LIB): $(RELEASE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(SONAME) $^ -o $@ $(LDFLAGS)
	@ln -sf $(REAL_LIB) $(BIN_DIR)/$(SONAME)
	@ln -sf $(SONAME) $(BIN_DIR)/$(LIB)
	@echo "📚 Built shared library: $@"

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

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROJECT_NAME) \
		$(DESTDIR)$(LIBDIR)/$(REAL_LIB) $(DESTDIR)$(LIBDIR)/$(SONAME) \
		$(DESTDIR)$(LIBDIR)/$(LIB) $(DESTDIR)$(LIBDIR)/$(STATIC_LIB) \
		$(DESTDIR)$(INCDIR)/$(HEADER) $(DESTDIR)$(PCDIR)/$(PROJECT_NAME).pc

# Package binary, library and header into a tar.gz
package: release
	@rm -rf $(PKG_DIR)
	@mkdir -p $(PKG_DIR)
	@cp -P $(RELEASE_BIN) $(BIN_DIR)/$(LIB)* $(BIN_DIR)/$(STATIC_LIB) \
		$(HEADER) LICENSE $(PKG_DIR)/
	@tar -czf $(PACKAGE_FILE) -C $(PKG_DIR) .
	@echo "📦 Packaged into: $(PACKAGE_FILE)"

# Clean up build artifacts
clean:
	rm -rf $(BUILD_DIR) $(PROJECT_NAME)-*.tar.gz
	@echo "🧹 Cleaned up build files"

.PHONY: all release debug test clean install uninstall package
