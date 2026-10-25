# bubbleload -- run programs inside a bubblewrap sandbox
#
# Targets: all, test, install, clean

PREFIX  ?= /usr/local
DESTDIR ?=
CC      ?= cc

# Where the license text goes; packages usually set this to
# /usr/share/licenses/<pkgname>.
LICENSEDIR ?= $(PREFIX)/share/licenses/bubbleload

BUILD   := build
BIN     := $(BUILD)/bubbleload
SANBIN  := $(BUILD)/bubbleload-san

SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
SANOBJS := $(patsubst src/%.c,$(BUILD)/obj-san/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d) $(SANOBJS:.o=.d)

EMBED   := $(BUILD)/generic_default.h

WARN ?= -Wall -Wextra -Wpedantic -Werror
STD  ?= -std=c11 -D_POSIX_C_SOURCE=200809L
# Hardening flags we add ourselves.  Builds that already pass their own (Arch's
# makepkg CFLAGS carry -D_FORTIFY_SOURCE and -fstack-protector-strong) must
# override this, e.g. "make HARD=", otherwise _FORTIFY_SOURCE is redefined and
# -Werror turns that into an error.
HARD ?= -D_FORTIFY_SOURCE=2 -fstack-protector-strong

CFLAGS  ?= -O2
LDFLAGS ?=
LDLIBS  ?=

ALL_CFLAGS   := $(STD) $(WARN) $(CFLAGS) $(HARD) -fPIE -MMD -MP
ALL_LDFLAGS  := -pie $(LDFLAGS)
SAN_CFLAGS   := $(STD) $(WARN) -O1 -g $(HARD) -fPIE -MMD -MP \
                -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS  := $(ALL_LDFLAGS) -fsanitize=address,undefined

.PHONY: all test install install-profiles clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(SANBIN): $(SANOBJS)
	$(CC) $(SAN_LDFLAGS) -o $@ $(SANOBJS) $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -I$(BUILD) -c -o $@ $<

$(BUILD)/obj-san/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SAN_CFLAGS) -I$(BUILD) -c -o $@ $<

$(BUILD)/obj/main.o $(BUILD)/obj-san/main.o: $(EMBED)

$(EMBED): examples/generic.conf tools/embed.sh
	@mkdir -p $(dir $@)
	sh tools/embed.sh examples/generic.conf bl_default_generic > $@.tmp
	@mv $@.tmp $@

test: $(BIN) $(SANBIN)
	./tests/run.sh $(BIN)
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	./tests/run.sh $(SANBIN)

# System install: binary plus example profiles under DESTDIR/PREFIX.  Nothing
# is written to $HOME; the binary seeds $XDG_CONFIG_HOME/bubbleload itself on
# first launch, so packages need no post-install script.
install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/bubbleload
	install -d $(DESTDIR)$(PREFIX)/share/bubbleload/examples
	install -m 0644 examples/generic.conf examples/opencode.conf \
		$(DESTDIR)$(PREFIX)/share/bubbleload/examples/
	install -d $(DESTDIR)$(LICENSEDIR)
	install -m 0644 LICENSE $(DESTDIR)$(LICENSEDIR)/LICENSE
	install -d $(DESTDIR)$(PREFIX)/share/bash-completion/completions
	install -m 0644 completions/bubbleload.bash \
		$(DESTDIR)$(PREFIX)/share/bash-completion/completions/bubbleload
	install -d $(DESTDIR)$(PREFIX)/share/zsh/site-functions
	install -m 0644 completions/_bubbleload \
		$(DESTDIR)$(PREFIX)/share/zsh/site-functions/_bubbleload
	install -d $(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d
	install -m 0644 completions/bubbleload.fish \
		$(DESTDIR)$(PREFIX)/share/fish/vendor_completions.d/bubbleload.fish
	install -d $(DESTDIR)$(PREFIX)/share/bubbleload/completions
	install -m 0755 completions/bubbleload.sh \
		$(DESTDIR)$(PREFIX)/share/bubbleload/completions/bubbleload.sh

# Convenience for source installs only: copy the example profiles into the
# per-user config directory, keeping files that are already there.  Packaging
# must not use this target -- the binary only seeds generic.conf by itself.
install-profiles:
	@dir=$${XDG_CONFIG_HOME:-$$HOME/.config}/bubbleload; \
	install -d "$$dir"; \
	for f in examples/*.conf; do \
		target="$$dir/$${f##*/}"; \
		if [ -e "$$target" ]; then \
			echo "kept      $$target"; \
		else \
			install -m 0644 "$$f" "$$target"; \
			echo "installed $$target"; \
		fi; \
	done

clean:
	rm -rf $(BUILD)

-include $(DEPS)
