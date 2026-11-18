CC ?= cc
BUILD ?= build
TARGET_SOURCES ?= examples/loopback.c
TARGET_CFLAGS ?=
TARGET_CPPFLAGS ?=
TARGET_LDLIBS ?=
LOCAL_FUSE := $(CURDIR)/.deps/fuse
export PKG_CONFIG_PATH := $(LOCAL_FUSE)/lib/pkgconfig:$(PKG_CONFIG_PATH)
FUSE_CFLAGS := $(shell PKG_CONFIG_PATH=$(LOCAL_FUSE)/lib/pkgconfig:$(PKG_CONFIG_PATH) pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell PKG_CONFIG_PATH=$(LOCAL_FUSE)/lib/pkgconfig:$(PKG_CONFIG_PATH) pkg-config --libs fuse3 2>/dev/null)
CPPFLAGS += -Iinclude $(FUSE_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC
LDLIBS += $(FUSE_LIBS) -ldl -pthread $(TARGET_LDLIBS)
RPATH = -Wl,-rpath,'$$ORIGIN'
TARGET_OBJS = $(patsubst %.c,$(BUILD)/target/%.o,$(TARGET_SOURCES))

.PHONY: all check check-deps clean package FORCE
all: check-deps $(BUILD)/fuse-bypass.so $(BUILD)/target-fuse $(BUILD)/io-test $(BUILD)/io-test64 $(BUILD)/io-bench runtime-libs
check-deps:
	@test -n "$(FUSE_LIBS)" || { echo 'Install libfuse3 development headers or run scripts/bootstrap-fuse.sh'; exit 1; }
$(BUILD)/target/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TARGET_CPPFLAGS) $(CFLAGS) $(TARGET_CFLAGS) -Dmain=fuse_bypass_target_main -c $< -o $@
$(BUILD)/%.o: src/%.c include/runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/fuse-bypass.so: $(BUILD)/intercept.o $(BUILD)/runtime.o $(TARGET_OBJS) FORCE
	$(CC) -shared -Wl,-z,defs -Wl,--wrap=fuse_main_real $(filter %.o,$^) $(LDLIBS) $(RPATH) -o $@
$(BUILD)/target-fuse: $(TARGET_SOURCES) FORCE
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(TARGET_CPPFLAGS) $(CFLAGS) $(TARGET_CFLAGS) $(TARGET_SOURCES) $(LDLIBS) $(RPATH) -o $@
$(BUILD)/io-test: tests/io-test.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@
$(BUILD)/io-test64: tests/io-test.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=2 $< -o $@
$(BUILD)/io-bench: tests/io-bench.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -lm -o $@
.PHONY: runtime-libs
runtime-libs:
	@mkdir -p $(BUILD)
	@if test -f $(LOCAL_FUSE)/lib/libfuse3.so.3; then cp -L $(LOCAL_FUSE)/lib/libfuse3.so.3 $(BUILD)/libfuse3.so.3; fi
check: all
	python3 tests/check.py $(abspath $(BUILD))
package: all
	mkdir -p dist
	tar -czf dist/fuse-bypass-x86_64.tar.gz $(BUILD) README.md docs scripts third_party src include examples tests Makefile $(wildcard .deps/libfuse-3.10.5.tar.gz)
clean:
	rm -rf $(BUILD)

.PHONY: check-upstream
check-upstream: $(BUILD)/io-test
	@test -f .deps/libfuse-fuse-3.10.5/example/hello.c || { echo 'Run scripts/bootstrap-fuse.sh first'; exit 1; }
	$(MAKE) BUILD=build-upstream TARGET_SOURCES=.deps/libfuse-fuse-3.10.5/example/hello.c TARGET_CFLAGS=-Wno-error=sign-compare build-upstream/fuse-bypass.so runtime-libs
	python3 tests/upstream.py
