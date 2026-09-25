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
# This does NOT replace a flash. ARCHITECTURE.md is emphatic about that and it
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

# The four screen angles, and the control square that rides on them.
# gfx.c's logical->panel map and touch.c's inverse have to agree exactly
# or every control is at its own mirror image, which on the board looks
# like a miscalibrated digitiser rather than like arithmetic. Neither
# file builds on a host -- one needs the DPI driver, the other
# esp_lcd_touch -- so both maps are transcribed there, and the layout
# arithmetic they feed is checked with them.
ROTATE_BIN := rotatetest
# The clock floor read off a card's root, before NTP can run. Four
# comparisons standing between foreign timestamps and a permanent
# one-way latch: too loose and a file dated 2107 refuses every real NTP
# reply for the life of the device, too tight and it silently never
# helps. Header-only, like tailplantest, so this compiles cardtime.h
# itself and there is nothing to drift from.
CARDTIME_BIN := cardtimetest

# The settings pages' one scrollbar. The only way to reach the sleep
# timer in landscape, where 860 px of page sit in a 504 px viewport, so
# an off-by-one at either end of its travel reads as a control that
# cannot be set rather than as a scrollbar that is wrong. Header-only,
# like cardtimetest.
MENUSCROLL_BIN := menuscrolltest

# The media index's path order and reconcile step. An order that
# disagrees with the walk tombstones and re-adds folders on every walk
# without ever crashing, so it is checked here or nowhere. Header-only,
# like cardtimetest.
MEDIAINDEX_BIN := mediaindextest

# The media walk, over a real directory tree in /tmp: the order the
# index needs, and completeness-or-failure. Compiles the real
# mediawalk.c; the ESP-IDF surface it calls is stubbed in the test.
MEDIAWALK_BIN := mediawalktest

# One reconcile end to end: the real mediasync.c reading and writing
# real index files in /tmp, with the catalog, tags and walk supplied by
# the test. What a failure must leave untouched is the point.
MEDIASYNC_BIN := mediasynctest

# Fade and end-of-audio detection. Links the real loudness.c, and builds
# it once more at -O2 -Werror as an object -- not -fsyntax-only, which
# stops before the optimiser that the -O2-only warnings come out of.
FADE_BIN := fadetest

# The track-boundary rules. Header-only, so this compiles tailplan.h
# itself and there is nothing to drift from.
# The "same station?" rule behind the gold star. Header-only, like
# tailplantest, so this compiles favmatch.h itself and there is nothing
# to drift from.
FAVMATCH_BIN := favmatchtest

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

# stations.m3u. Every other file in the network series reads bytes from
# a server; this one reads bytes from a person with a text editor, which
# is a wider input and a worse one.
STATION_BIN := stationlisttest

# 24-bit PCM down to the 16 everything downstream is built from. The
# arithmetic ran untested from 0803 until 0003 moved it to a header: it
# was a static in decoder.c, and decoder.c does not build on a host.
PCMFOLD_BIN := pcmfoldtest

# When a stream is allowed to make a sound. Two thresholds and not one,
# because a single one flaps at the watermark and that is heard as
# chopping rather than as a pause.
BUFPLAN_BIN := bufferplantest

# 5062: stb_image as the last JPEG decoder -- progressive covers and the
# flavours TJpgDec calls JDR_FMT3. stb_image.h is gitignored; fetched
# here, pinned and hash-checked, the same as cmake/vendored.cmake.
STBJPEG_BIN := stbjpegtest

# 5068: a station URL with an address in place of its host, and the Host
# header that puts the name back.
ADDRPIN_BIN := addrpintest
STB_H   := ../components/stbjpeg/stb_image.h
STB_URL := https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
STB_SHA := 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3

# Which decoder a stream's bytes go to. The bytes win over Content-Type,
# because handing AAC to minimp3 does not fail -- it produces noise, and
# noise from a live stream is hard to tell from a bad connection.
CODEC_BIN := codecplantest

# MPEG frame lengths. An MP3 header does not carry its frame length, so a
# wrong table entry desynchronises the stream and shows up as bytes lost
# -- the figure the probe uses to certify icydemux and the ring. The test
# computes the lengths a second time, longhand.
MP3_BIN := mp3counttest

# The window between the ring and the decoder. Losing bytes at a refill
# boundary does not crash -- the decoder resynchronises and the stream
# plays with a click every few seconds, which from outside is "internet
# radio is a bit crackly" with no file to compare against.
FRAMEWIN_BIN := framewintest

# What play_stream() shows and what a press does. The screen has to pick
# between two state machines that disagree on purpose -- a reconnect
# behind a full PCM ring is not a message, an empty ring on a healthy
# link is -- and that choice is a table rather than an if chain in a
# loop nobody can step through.
STREAMPLAN_BIN := streamplantest

# Moving through the station list. The parser has its own test; this is
# the index arithmetic, which is where (i - 1) % n bites.
STATIONNAV_BIN := stationnavtest

# The radio-browser request, and the server's own M3U through the parser
# that was chosen for it. Links stationlist.h as well as radiobrowser.h on
# purpose: the encoder and the parser are checked against each other.
RADIOBROWSER_BIN := radiobrowsertest

# The minute-wide level strip: bucket arithmetic over a ring, plus the
# reserve-to-columns conversion the drawing will use.
LEVELHIST_BIN := levelhisttest

# When the USB Ethernet cable counts as a network. Header-only, like
# favmatchtest, so this compiles netlink.h itself -- and ethcfg.h, the
# Realtek configuration choice.
NETLINK_BIN := netlinktest

# Cue sheets. Header-only; the test reads the five sheets out of
# ../test_audio_files so the bytes under test are the bytes on the card.
CUE_BIN := cuesheettest

all: run run-ctrl run-wifi run-clock run-rotate run-cardtime run-mediaindex run-mediawalk run-mediasync run-menuscroll run-fade run-tail run-portal run-old check-ui run-bright run-sleep run-sniff run-icy run-netplan run-stations run-pcmfold run-bufplan run-stbjpeg run-addrpin run-codec run-mp3 run-framewin run-streamplan run-stationnav run-radiobrowser run-levelhist run-favmatch run-netlink run-cue

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

$(ROTATE_BIN): rotatetest.c
	$(CC) $(WARNCFLAGS) -c rotatetest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ rotatetest.c $(LDFLAGS)

run-rotate: $(ROTATE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ROTATE_BIN)
$(CARDTIME_BIN): cardtimetest.c ../main/cardtime.h
	$(CC) $(WARNCFLAGS) -c cardtimetest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ cardtimetest.c $(LDFLAGS)

run-cardtime: $(CARDTIME_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CARDTIME_BIN)

$(MEDIAINDEX_BIN): mediaindextest.c ../main/mediaindex.h
	$(CC) $(WARNCFLAGS) -c mediaindextest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediaindextest.c $(LDFLAGS)

run-mediaindex: $(MEDIAINDEX_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIAINDEX_BIN)

$(MEDIAWALK_BIN): mediawalktest.c ../main/mediawalk.c ../main/mediawalk.h ../main/mediadir.h ../main/mediaindex.h shim.c shim.h
	$(CC) $(WARNCFLAGS) -c mediawalktest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/mediawalk.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediawalktest.c ../main/mediawalk.c shim.c $(LDFLAGS)

run-mediawalk: $(MEDIAWALK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIAWALK_BIN)

$(MEDIASYNC_BIN): mediasynctest.c ../main/mediasync.c ../main/mediasync.h ../main/mediaindex.h ../main/mediacat.h
	$(CC) $(WARNCFLAGS) -c mediasynctest.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c ../main/mediasync.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mediasynctest.c ../main/mediasync.c $(LDFLAGS)

run-mediasync: $(MEDIASYNC_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MEDIASYNC_BIN)

$(MENUSCROLL_BIN): menuscrolltest.c ../main/menuscroll.h
	$(CC) $(WARNCFLAGS) -c menuscrolltest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ menuscrolltest.c $(LDFLAGS)

run-menuscroll: $(MENUSCROLL_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MENUSCROLL_BIN)

$(FADE_BIN): fadetest.c ../main/loudness.c ../main/loudness.h
	$(CC) $(WARNCFLAGS) -D_GNU_SOURCE -I../main -c ../main/loudness.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ fadetest.c ../main/loudness.c $(LDFLAGS)

run-fade: $(FADE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FADE_BIN)

# The stream levelling carrier, its gate and its slew. Links the real
# main/streamgain.c, and builds it once more at -O2 -Werror for the
# reason FADE_BIN does.
# The one-field JSON reader. Header-only, so the test is the only
# compile it gets -- hence the -Werror pass as well.
JSONPICK_BIN := jsonpicktest

$(JSONPICK_BIN): jsonpicktest.c ../main/jsonpick.h
	$(CC) $(WARNCFLAGS) -c jsonpicktest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ jsonpicktest.c $(LDFLAGS)

run-jsonpick: $(JSONPICK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(JSONPICK_BIN)

SGAIN_BIN := streamgaintest

$(SGAIN_BIN): streamgaintest.c ../main/streamgain.c ../main/streamgain.h
	$(CC) $(WARNCFLAGS) -c ../main/streamgain.c -o /dev/null
	$(CC) $(WARNCFLAGS) -c streamgaintest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ streamgaintest.c ../main/streamgain.c $(LDFLAGS)

run-sgain: $(SGAIN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(SGAIN_BIN)

$(FAVMATCH_BIN): favmatchtest.c ../main/favmatch.h
	$(CC) $(WARNCFLAGS) -c favmatchtest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ favmatchtest.c $(LDFLAGS)

run-favmatch: $(FAVMATCH_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FAVMATCH_BIN)

$(NETLINK_BIN): netlinktest.c ../main/netlink.h ../main/ethcfg.h
	$(CC) $(WARNCFLAGS) -c netlinktest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ netlinktest.c $(LDFLAGS)

run-netlink: $(NETLINK_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(NETLINK_BIN)

$(CUE_BIN): cuesheettest.c ../main/cuesheet.h
	$(CC) $(WARNCFLAGS) -c cuesheettest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ cuesheettest.c $(LDFLAGS)

run-cue: $(CUE_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CUE_BIN)

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
	rm -f $(BIN) $(CTRL_BIN) $(WIFI_BIN) $(CLOCK_BIN) $(FADE_BIN) $(TAIL_BIN) $(PORTAL_BIN) $(OLD_BINS) $(BRIGHT_BIN) $(SLEEP_BIN) $(SNIFF_BIN) $(ICY_BIN) $(NETPLAN_BIN) $(STATION_BIN) $(BUFPLAN_BIN) $(CODEC_BIN) $(MP3_BIN) $(FRAMEWIN_BIN) $(STREAMPLAN_BIN) $(STATIONNAV_BIN) $(RADIOBROWSER_BIN) $(LEVELHIST_BIN) $(SGAIN_BIN) $(JSONPICK_BIN) $(FAVMATCH_BIN) $(NETLINK_BIN) $(CUE_BIN) $(ROTATE_BIN) $(CARDTIME_BIN) $(MEDIAINDEX_BIN) $(MEDIAWALK_BIN) $(MEDIASYNC_BIN) $(MENUSCROLL_BIN) $(STBJPEG_BIN) $(ADDRPIN_BIN)
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

$(STATION_BIN): stationlisttest.c ../main/stationlist.h
	$(CC) $(WARNCFLAGS) -c stationlisttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ stationlisttest.c $(LDFLAGS)

$(PCMFOLD_BIN): pcmfoldtest.c ../main/pcmfold.h
	$(CC) $(WARNCFLAGS) -c pcmfoldtest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ pcmfoldtest.c $(LDFLAGS)

run-pcmfold: $(PCMFOLD_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(PCMFOLD_BIN)

run-stations: $(STATION_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STATION_BIN)

$(BUFPLAN_BIN): bufferplantest.c ../main/bufferplan.h
	$(CC) $(WARNCFLAGS) -c bufferplantest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ bufferplantest.c $(LDFLAGS)

run-bufplan: $(BUFPLAN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(BUFPLAN_BIN)

$(STB_H):
	curl -fsSL "$(STB_URL)" -o $@.tmp
	echo "$(STB_SHA)  $@.tmp" | sha256sum -c - >/dev/null || (rm -f $@.tmp; echo "stb_image.h: hash mismatch"; exit 1)
	mv $@.tmp $@

$(STBJPEG_BIN): stbjpegtest.c ../components/stbjpeg/stbjpeg.c ../components/stbjpeg/stbjpeg.h $(STB_H)
	$(CC) $(WARNCFLAGS) -I../components/stbjpeg -c stbjpegtest.c -o /dev/null
	$(CC) $(CFLAGS) -I../components/stbjpeg -Wno-sign-compare -Wno-unused-function \
	    -Wno-unused-but-set-variable -Wno-type-limits -Wno-shift-negative-value \
	    -Wno-implicit-fallthrough -fwrapv -fno-sanitize=signed-integer-overflow -o $@ stbjpegtest.c ../components/stbjpeg/stbjpeg.c $(LDFLAGS)

run-stbjpeg: $(STBJPEG_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STBJPEG_BIN)

$(ADDRPIN_BIN): addrpintest.c ../main/addrpin.h
	$(CC) $(WARNCFLAGS) -c addrpintest.c -o /dev/null
	$(CC) $(CFLAGS) -o $@ addrpintest.c $(LDFLAGS)

run-addrpin: $(ADDRPIN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(ADDRPIN_BIN)

$(LEVELHIST_BIN): levelhisttest.c ../main/levelhist.h
	$(CC) $(WARNCFLAGS) -c levelhisttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ levelhisttest.c $(LDFLAGS)

run-levelhist: $(LEVELHIST_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(LEVELHIST_BIN)

$(RADIOBROWSER_BIN): radiobrowsertest.c ../main/radiobrowser.h ../main/stationlist.h
	$(CC) $(WARNCFLAGS) -c radiobrowsertest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ radiobrowsertest.c $(LDFLAGS)

run-radiobrowser: $(RADIOBROWSER_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(RADIOBROWSER_BIN)

$(STATIONNAV_BIN): stationnavtest.c ../main/stationlist.h
	$(CC) $(WARNCFLAGS) -c stationnavtest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ stationnavtest.c $(LDFLAGS)

run-stationnav: $(STATIONNAV_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STATIONNAV_BIN)

$(STREAMPLAN_BIN): streamplantest.c ../main/streamplan.h ../main/bufferplan.h ../main/netplan.h
	$(CC) $(WARNCFLAGS) -c streamplantest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ streamplantest.c $(LDFLAGS)

run-streamplan: $(STREAMPLAN_BIN) $(STATIONNAV_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(STREAMPLAN_BIN) $(STATIONNAV_BIN)

$(CODEC_BIN): codecplantest.c ../main/codecplan.h ../main/streamsniff.h
	$(CC) $(WARNCFLAGS) -c codecplantest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ codecplantest.c $(LDFLAGS)

run-codec: $(CODEC_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(CODEC_BIN)

$(MP3_BIN): mp3counttest.c ../main/mp3count.h
	$(CC) $(WARNCFLAGS) -c mp3counttest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ mp3counttest.c $(LDFLAGS)

run-mp3: $(MP3_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(MP3_BIN)

$(FRAMEWIN_BIN): framewintest.c ../main/framewin.h ../main/mp3count.h
	$(CC) $(WARNCFLAGS) -c framewintest.c -o /dev/null
	$(CC) $(CFLAGS) -D_GNU_SOURCE -o $@ framewintest.c $(LDFLAGS)

run-framewin: $(FRAMEWIN_BIN)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./$(FRAMEWIN_BIN)

check-ui:
	@for f in $(UI_CHECK); do \
		echo "$(CC) -c $$f"; \
		$(CC) $(WARNCFLAGS) -c $$f -o /dev/null || exit 1; \
	done

.PHONY: all run-pcmfold run run-ctrl run-wifi run-clock run-rotate run-cardtime run-mediaindex run-mediawalk run-mediasync run-menuscroll run-fade run-tail run-portal run-old check-ui run-bright run-sleep run-sniff run-icy run-netplan run-stations run-pcmfold run-bufplan run-stbjpeg run-addrpin run-codec run-mp3 run-framewin run-streamplan run-stationnav run-radiobrowser run-levelhist run-favmatch run-netlink run-cue clean
