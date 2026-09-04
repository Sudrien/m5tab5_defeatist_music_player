# texttest -- host build of the real main/gfx.c text path.
#
# Same bargain as seektest: the sanitizers are the point, so they are not
# optional flags a hurried run can drop. -fsanitize=address,undefined with
# -fno-sanitize-recover means the first overrun or signed overflow aborts
# with a stack trace rather than being counted and continued past.
#
#   make        build and run
#   make clean
#
# This does NOT replace a flash. CLAUDE.md is emphatic about that and it
# has been right every time: host testing cannot see the DSI panel, PSRAM
# timing, or the ESP-IDF build. It catches layout arithmetic, which is
# what changed here, and nothing else.
#
# SPDX-License-Identifier: MIT

CC      ?= cc
CFLAGS  := -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           -fno-omit-frame-pointer \
           -I. -Ifake -I../main -I../components/ark12
LDFLAGS := -fsanitize=address,undefined -lm

SRCS := main.c shim.c ../main/gfx.c ../components/ark12/ark12.c
BIN  := texttest

# The USB control-transfer ownership handoff, which needs threads and its
# own main(). Separate binary rather than another file in BIN.
CTRL_BIN := ctrltest

all: run run-ctrl

$(BIN): $(SRCS) shim.h ../main/gfx.h ../components/ark12/ark12.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(BIN)

$(CTRL_BIN): ctrltest.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -o $@ ctrltest.c $(LDFLAGS) -pthread

run-ctrl: $(CTRL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CTRL_BIN)

clean:
	rm -f $(BIN) $(CTRL_BIN)
	rm -rf $(BIN).dSYM $(CTRL_BIN).dSYM

.PHONY: all run run-ctrl clean
