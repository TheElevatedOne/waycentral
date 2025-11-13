# Project configuration
PROJECT_NAME := waycentral
SRC_DIRS := . src src/sysmon
SRCS := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJ_DIR := build/objects
BIN_DIR := build/bin
OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)
CFLAGS := -Wall -Wextra -O2
DEBUG_FLAGS := -Wall -Wextra -g -O0
LDFLAGS :=
PREFIX := /usr
LOCAL_PREFIX := $(HOME)/.local

VERSION := $(shell cat VERSION || echo "0.0.0")

# Binaries
RELEASE_BIN := $(BIN_DIR)/$(PROJECT_NAME)
DEBUG_BIN := $(BIN_DIR)/$(PROJECT_NAME)-debug
PACKAGE_FILE := $(PROJECT_NAME)-$(VERSION).tar.gz

# Default target
all: release debug package

# Compile release
release: CFLAGS += -DNDEBUG
release: $(RELEASE_BIN)

# Compile debug
debug: CFLAGS := $(DEBUG_FLAGS)
debug: $(DEBUG_BIN)

# Linking rules
$(RELEASE_BIN): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@cp -f LICENSE $(BIN_DIR)/LICENSE 2>/dev/null || true
	@echo "✅ Built release binary: $@"

$(DEBUG_BIN): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@cp -f LICENSE $(BIN_DIR)/LICENSE 2>/dev/null || true
	@echo "🐛 Built debug binary: $@"

# Object compilation (outputs to build/objects)
$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Installation logic
install:
	@if [ "$$(id -u)" -eq 0 ]; then \
		echo "Installing to $(PREFIX)/bin..."; \
		install -Dm755 $(RELEASE_BIN) $(PREFIX)/bin/$(PROJECT_NAME); \
	else \
		echo "Installing to $(LOCAL_PREFIX)/bin..."; \
		install -Dm755 $(RELEASE_BIN) $(LOCAL_PREFIX)/bin/$(PROJECT_NAME); \
	fi

uninstall:
	@if [ "$$(id -u)" -eq 0 ]; then \
		echo "Uninstalling from $(PREFIX)/bin..."; \
		rm -f $(PREFIX)/bin/$(PROJECT_NAME); \
	else \
		echo "Uninstalling from $(LOCAL_PREFIX)/bin..."; \
		rm -f $(LOCAL_PREFIX)/bin/$(PROJECT_NAME); \
	fi

# Package build/bin into a tar.gz
package: release
	@tar -czf $(PACKAGE_FILE) -C $(BIN_DIR) .
	@echo "📦 Packaged binaries into: $(PACKAGE_FILE)"

# Clean up build artifacts
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(PROJECT_NAME).tar.gz
	@echo "🧹 Cleaned up build files"

.PHONY: all release debug clean install uninstall package
