CC ?= cc
CPPFLAGS ?=
EXTRA_CPPFLAGS ?=
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
STATIC_LDFLAGS ?=
else
STATIC_LDFLAGS ?= -static
endif
TEST_LDFLAGS ?=
VERSION_BASE ?= $(shell bash .github/scripts/resolve_version.sh --base 2>/dev/null || printf dev)
VERSION ?= $(shell if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git describe --tags --exact-match --dirty 2>/dev/null || printf '%s-%s%s\n' "$(VERSION_BASE)" "$$(git rev-parse --short HEAD)" "$$(git diff --quiet 2>/dev/null || echo -dirty)"; else printf '%s\n' "$(VERSION_BASE)"; fi)
VERSION_CPPFLAG := -DHOLD_VERSION=\"$(VERSION)\"

# Every translation unit. wildcard is portable in GNU make; one glob per layer
# directory (GNU make has no portable recursive **). main.c sits directly
# under src/; the layers live in src/<layer>/.
SRCS := $(wildcard src/*.c) \
        $(wildcard src/core/*.c) \
        $(wildcard src/platform/*.c) \
        $(wildcard src/store/*.c) \
        $(wildcard src/term/*.c) \
        $(wildcard src/console/*.c) \
        $(wildcard src/access/*.c) \
        $(wildcard src/runtime/*.c) \
        $(wildcard src/screen/*.c) \
        $(wildcard src/cli/*.c)

# Vendored libvterm (third_party/libvterm, MIT): built with its own flags so the
# project's warning set (and CI's -Werror) applies to Hold's code only, while
# optimization and sanitizer flags still carry over.
VTERM_SRCS := $(wildcard third_party/libvterm/src/*.c)
VTERM_CFLAGS := $(filter-out -Werror -Wall -Wextra -Wpedantic -std=%,$(CFLAGS)) -std=c99 -w

# Separate object trees per build "personality" so the test objects (built with
# -DHOLD_TESTING and a different HOLD_BOOT_ID_PATH) can never be linked
# into a release binary, and vice versa.
VTERM_OBJS := $(patsubst third_party/libvterm/src/%.c,obj/vterm/%.o,$(VTERM_SRCS))
OBJS      := $(patsubst src/%.c,obj/%.o,$(SRCS)) $(VTERM_OBJS)
TEST_OBJS := $(patsubst src/%.c,obj-test/%.o,$(SRCS)) $(VTERM_OBJS)

INCLUDES := -Iinclude -Ithird_party/libvterm/include
ALL_CPPFLAGS := $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(INCLUDES) $(VERSION_CPPFLAG)
TEST_CPPFLAGS := -DHOLD_TESTING -DHOLD_BOOT_ID_PATH='"/tmp/hold_test_boot_id"'

all: hold

obj/vterm/%.o: third_party/libvterm/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -Ithird_party/libvterm/include $(VTERM_CFLAGS) -c -o $@ $<

obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) -c -o $@ $<

hold: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(STATIC_LDFLAGS) -o $@ $(OBJS)

hold-dynamic: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o hold-dynamic $(OBJS)

obj-test/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) -o hold $(TEST_OBJS)
	@bash tests/test_hold.sh
	@$(MAKE) screen-test
	@bash tests/test_version_makefile.sh
	@bash tests/test_release_installer.sh
	@$(MAKE) test-040

screen-test: $(VTERM_OBJS)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o screen-test tests/screen_test.c src/screen/screen.c $(VTERM_OBJS)
	@./screen-test

test-040: hold-dynamic
	@HOLD_BIN=./hold-dynamic bash tests/test_040_dockerish.sh

print-version:
	@printf '%s\n' '$(VERSION)'

check: test

# Full local CI mirror (one command, same rigor as GitHub CI): static + dynamic
# -Werror builds, the regression suite + hash-vector, ASan/UBSan, cppcheck, and
# the layer-dependency lint. Skips (visibly) only what the local toolchain lacks.
ci:
	@bash scripts/ci.sh

# Layer dependency-direction lint, shared verbatim with the GitHub CI job.
lint:
	@bash scripts/lint_layers.sh

clean:
	rm -f hold hold-dynamic screen-test
	rm -rf obj obj-test

.PHONY: all clean test test-040 check ci lint screen-test print-version
