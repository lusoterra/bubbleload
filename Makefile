# bubbleload -- run programs inside a bubblewrap sandbox
#
# Targets: all, test, install, clean

PREFIX  ?= /usr/local
DESTDIR ?=
CC      ?= cc

BUILD   := build
BIN     := $(BUILD)/bubbleload
SANBIN  := $(BUILD)/bubbleload-san

SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
SANOBJS := $(patsubst src/%.c,$(BUILD)/obj-san/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d) $(SANOBJS:.o=.d)

EMBED   := $(BUILD)/generic_default.h

WARN := -Wall -Wextra -Wpedantic -Werror
STD  := -std=c11 -D_POSIX_C_SOURCE=200809L
HARD := -D_FORTIFY_SOURCE=2 -fstack-protector-strong

CFLAGS  ?= -O2
LDFLAGS ?=
LDLIBS  ?=

ALL_CFLAGS   := $(STD) $(WARN) $(CFLAGS) $(HARD) -fPIE -MMD -MP
ALL_LDFLAGS  := -pie $(LDFLAGS)
SAN_CFLAGS   := $(STD) $(WARN) -O1 -g $(HARD) -fPIE -MMD -MP \
                -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS  := $(ALL_LDFLAGS) -fsanitize=address,undefined

.PHONY: all test install clean

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

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/bubbleload
	install -d $(DESTDIR)$(PREFIX)/share/bubbleload/examples
	install -m 0644 examples/generic.conf examples/opencode.conf \
		$(DESTDIR)$(PREFIX)/share/bubbleload/examples/

clean:
	rm -rf $(BUILD)

-include $(DEPS)
