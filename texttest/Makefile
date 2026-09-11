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

# A second, non-sanitised pass at -O2 -Werror, run before the real build.
#
# ASan needs -O1 or the reports lose their frames, but several warnings --
# -Wformat-truncation is the one that caught us -- only fire after the
# constant propagation that -O2 does. The firmware is built at -O2 with
# warnings as errors, so a file could pass every check here and fail the
# real build on a diagnostic this harness structurally could not produce.
# That happened, twice, on code that had been "checked".
#
# Warnings only: no binary is kept and nothing is run from it.
WARNCFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
              -D_GNU_SOURCE -I. -Ifake -I../main -I../components/ark12

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

# The saved-network list. Its own binary for the same reason ctrltest is:
# its own main(), and no overlap with the text path.
WIFI_BIN := wifistoretest

# The clock floor. Its own binary for the same reason the others are.
CLOCK_BIN := clocktest

# Fade and end-of-audio detection. Links the real loudness.c, and builds
# it once more at -O2 -Werror as an object -- not -fsyntax-only, which
# stops before the optimiser that the -O2-only warnings come out of.
FADE_BIN := fadetest

# The track-boundary rules. Header-only, so this compiles tailplan.h
# itself and there is nothing to drift from.
TAIL_BIN := tailplantest

# The setup portal's form parsing, HTML escaping and DNS replies: the
# code that reads bytes from anyone in radio range.
PORTAL_BIN := portalwebtest

# Fourteen older checks, written alongside the patches they cover and
# never added here, so nothing ran them after the session that wrote
# them. Each has its own main() and reports "all passed" or "FAILURES",
# returning non-zero on the second.
#
# Only cachetest compiles a file the firmware builds (mediacache.c, with
# shim.c for heap_caps_*). The rest TRANSCRIBE a rule out of player.c --
# the release gate, the ring order, the crossfade exits -- or, for
# pooltest, arithmetic out of esp_jpeg's tjpgd.c. They can drift from the
# code they describe; running them at least keeps them from drifting from
# themselves unnoticed.
OLD_STANDALONE := committest exittest kepttest orderingtest pooltest \
                  postest releasetest ringwaittest setstest stalltest \
                  tailtest trimtest xfadetest
CACHE_BIN := cachetest
OLD_BINS  := $(CACHE_BIN) $(OLD_STANDALONE)

# Screens that cannot run on a host but can be compiled against the real
# gfx.h and the fake headers here, so a -Werror at -O2 is caught before
# idf.py -- the error 0018 shipped was exactly that kind.
UI_CHECK := ../main/sleeppage.c

# Brightness: the backlight/filter split and the RGB565 scaling.
BRIGHT_BIN := brightnesstest

# The sleep timer's arithmetic.
SLEEP_BIN := sleeptimertest

# The stream probe's byte sniffing and ICY title parsing.
SNIFF_BIN := streamsnifftest

# The ICY demultiplexer. Header-only, so this compiles icydemux.h itself
# and there is nothing to drift from. The property it exists for --
# feeding a body in pieces gives the same answer as feeding it whole --
# cannot be observed on hardware, where metaint is 16000 and the reads
# are 2048, without waiting for the one read in eight that splits a
# metadata block.
ICY_BIN := icydemuxtest

# The connection policy: which statuses are followed, retried and given
# up on, and the backoff schedule. netstream.c cannot be host-tested --
# it is esp_http_client and a task -- but its decisions are a table, and
# the table is the part that will be wrong.
NETPLAN_BIN := netplantest

all: run run-ctrl run-wifi run-clock run-fade run-tail run-portal run-old check-ui run-bright run-sleep run-sniff run-icy run-netplan

$(BIN): $(SRCS) shim.h ../main/gfx.h ../components/ark12/ark12.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(BIN)

$(CTRL_BIN): ctrltest.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -o $@ ctrltest.c $(LDFLAGS) -pthread

run-ctrl: $(CTRL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CTRL_BIN)

$(WIFI_BIN): wifistoretest.c ../main/wifistore.c ../main/wifistore.h fake/wifistore_host.h
	$(CC) $(CFLAGS) -DWIFISTORE_HOST -o $@ wifistoretest.c ../main/wifistore.c $(LDFLAGS)

run-wifi: $(WIFI_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(WIFI_BIN)

$(CLOCK_BIN): clocktest.c
	$(CC) $(WARNCFLAGS) -c clocktest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ clocktest.c $(LDFLAGS)

run-clock: $(CLOCK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CLOCK_BIN)

$(FADE_BIN): fadetest.c ../main/loudness.c ../main/loudness.h
	$(CC) $(WARNCFLAGS) -D_GNU_SOURCE -I../main -c ../main/loudness.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ fadetest.c ../main/loudness.c $(LDFLAGS)

run-fade: $(FADE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FADE_BIN)

$(TAIL_BIN): tailplantest.c ../main/tailplan.h
	$(CC) $(WARNCFLAGS) -c tailplantest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ tailplantest.c $(LDFLAGS)

run-tail: $(TAIL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(TAIL_BIN)

$(PORTAL_BIN): portalwebtest.c ../main/portalweb.c ../main/portalweb.h ../main/dnsreply.c ../main/dnsreply.h
	$(CC) $(WARNCFLAGS) -c ../main/portalweb.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/dnsreply.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ portalwebtest.c ../main/portalweb.c ../main/dnsreply.c $(LDFLAGS)

run-portal: $(PORTAL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(PORTAL_BIN)

$(OLD_STANDALONE): %: %.c
	$(CC) $(WARNCFLAGS) -c $< -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -o $@ $< $(LDFLAGS) -pthread

$(CACHE_BIN): cachetest.c ../main/mediacache.c ../main/mediacache.h shim.c shim.h
	$(CC) $(WARNCFLAGS) -c cachetest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/mediacache.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ cachetest.c ../main/mediacache.c shim.c $(LDFLAGS)

# One target for the fourteen, stopping at the first that fails.
run-old: $(OLD_BINS)
	@for t in $(OLD_BINS); do \
		echo "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$$t"; \
		ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$$t || exit 1; \
	done

clean:
	rm -f $(BIN) $(CTRL_BIN) $(WIFI_BIN) $(CLOCK_BIN) $(FADE_BIN) $(TAIL_BIN) $(PORTAL_BIN) $(OLD_BINS) $(BRIGHT_BIN) $(SLEEP_BIN) $(SNIFF_BIN) $(ICY_BIN) $(NETPLAN_BIN)
	rm -rf $(BIN).dSYM $(CTRL_BIN).dSYM $(WIFI_BIN).dSYM $(CLOCK_BIN).dSYM $(FADE_BIN).dSYM $(TAIL_BIN).dSYM $(PORTAL_BIN).dSYM
	rm -rf $(addsuffix .dSYM,$(OLD_BINS))

$(BRIGHT_BIN): brightnesstest.c ../main/brightness.h
	$(CC) $(WARNCFLAGS) -c brightnesstest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ brightnesstest.c $(LDFLAGS)

run-bright: $(BRIGHT_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(BRIGHT_BIN)

$(SLEEP_BIN): sleeptimertest.c ../main/sleeptimer.h
	$(CC) $(WARNCFLAGS) -c sleeptimertest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ sleeptimertest.c $(LDFLAGS)

run-sleep: $(SLEEP_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SLEEP_BIN)

$(SNIFF_BIN): streamsnifftest.c ../main/streamsniff.h
	$(CC) $(WARNCFLAGS) -c streamsnifftest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ streamsnifftest.c $(LDFLAGS)

run-sniff: $(SNIFF_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SNIFF_BIN)

$(ICY_BIN): icydemuxtest.c ../main/icydemux.h ../main/streamsniff.h
	$(CC) $(WARNCFLAGS) -c icydemuxtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ icydemuxtest.c $(LDFLAGS)

run-icy: $(ICY_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ICY_BIN)

$(NETPLAN_BIN): netplantest.c ../main/netplan.h
	$(CC) $(WARNCFLAGS) -c netplantest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ netplantest.c $(LDFLAGS)

run-netplan: $(NETPLAN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(NETPLAN_BIN)

check-ui:
	@for f in $(UI_CHECK); do \
		echo "$(CC) -c $$f"; \
		$(CC) $(WARNCFLAGS) -c $$f -o /dev/null || exit 1; \
	done

.PHONY: all run run-ctrl run-wifi run-clock run-fade run-tail run-portal run-old check-ui run-bright run-sleep run-sniff run-icy run-netplan clean
