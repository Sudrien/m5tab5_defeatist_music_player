/*
 * netstream.h -- one HTTP(S) audio stream, into a ring, on its own task.
 *
 * This is phase 1 of the stream path in CLAUDE.md: bytes off the network
 * and into a buffer, with no decoder and no UI. Nothing here plays
 * anything. `netstream_read()` exists so that phase 2 has something to
 * read and so that the probe can prove this file on hardware first.
 *
 *     netstream task --> compressed ring --> (phase 2: decode loop)
 *     HTTP, TLS, redirects, ICY            256 KB, PSRAM
 *
 * WHAT THE PROBE MEASURED, AND WHAT THIS FILE IS SHAPED BY
 *
 * The numbers are in CLAUDE.md; these are the three that became code.
 *
 * 1. **The link gives 1.05x idle and 0.97x beside card playback**, with
 *    five-second windows swinging 0.83-1.31x. So a buffer is not
 *    optional and it is not small: this ring is 256 KB, four seconds of
 *    WNZK's unusually heavy 512 kbit/s, and most stations send a quarter
 *    of that, where the same ring is sixteen seconds. The deep jitter
 *    buffer is the *PCM* ring downstream (3520 KB, about eighteen
 *    seconds) -- this one only decouples TLS reads from decoding and
 *    absorbs a burst.
 *
 * 2. **Zeno's 302 carries a JWT good for sixty seconds.** Every
 *    reconnect therefore starts from the station URL. That is
 *    `netplan_reconnect_from()`, in netplan.h, with a test.
 *
 * 3. **One TLS session costs about 13 KB of internal RAM** even with
 *    mbedTLS allocating from PSRAM, and internal free with a track
 *    playing was 53-63 KB. So: **one session at a time, ever.** This
 *    file refuses to open a second connection while one is up, and the
 *    probe goes away when the stream path lands. Nothing else in the
 *    program may open HTTPS while a stream plays.
 *
 * THE RING IS STATIC AND ALLOCATED ONCE
 *
 * `xStreamBufferCreateWithCaps()` is banned here -- see "Heap
 * corruption: what actually happened". The storage is one PSRAM
 * allocation in `netstream_init()` that is never freed, with
 * `xStreamBufferCreateStatic()` on top, exactly as the PCM ring does it.
 * A stream that starts and stops forty times allocates nothing.
 *
 * WHAT CROSSES A TASK BOUNDARY
 *
 * Values, not handles. `netstream_state()`, `netstream_buffered()` and
 * `netstream_kbps()` are integers written by this task and readable by
 * anyone; ui_task never sees the ring handle, the HTTP client or the
 * demuxer. The two strings -- the ICY name and the current title -- are
 * copied out under a short mutex rather than returned as pointers, for
 * the same reason: the task rewrites them whenever a metadata block
 * arrives, and a pointer into them is a race the caller cannot see.
 *
 * `netstream_play()` and `netstream_stop()` are safe from ui_task and
 * from the decode loop. They post a request and return; they do not
 * connect, do not close a socket and do not block on the network. A
 * connect is up to several seconds (the probe saw 0.7 s and 1.5 s of
 * connect+TLS on two hops, first audio at 2.8 s) and ui_task is the
 * single writer of the framebuffer.
 *
 * PAUSE IS NOT A THING HERE
 *
 * A live stream cannot be paused and resumed where it was. player.c's
 * pause will call `netstream_stop()` and play will call
 * `netstream_play()` again; holding a connection open while paused fills
 * both rings and then stalls the server anyway. That decision lives in
 * phase 3, but it is why there is no `netstream_pause()` to find.
 *
 * WHAT IS NOT HERE YET, ON PURPOSE
 *
 * No HLS, no .pls or .m3u resolution, no Ogg/Opus assumptions, no
 * recording, no timeshift. Sniffing the first bytes to choose a codec is
 * phase 2's, and `streamsniff.h` already does it -- this file records
 * what the first bytes looked like and the Content-Type, and leaves the
 * choice alone.
 *
 * NOT COMPILED, NOT FLASHED
 *
 * `icydemux.h` and `netplan.h` are host-tested under ASan and UBSan
 * (`icydemuxtest`, 4064 checks; `netplantest`, 1202 checks). This file
 * is not: it needs esp_http_client and FreeRTOS, and texttest's fake
 * headers do not reach them. It has never been through a compiler. Per
 * CLAUDE.md that is stated rather than assumed, and a build alone -- no
 * flash, no cable -- would settle the enum spellings it is most likely
 * to have got wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netplan.h"            /* netstream_state_t, and the decisions */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The compressed ring. Four seconds of WNZK at 512 kbit/s, sixteen of a
 * 128 kbit/s station. PSRAM, allocated once, never freed.
 */
#define NETSTREAM_RING_BYTES    (256 * 1024)

/* Longest station URL accepted, and the redirect target it resolves to.
 * The probe's Zeno redirect was 190 characters with its token; 512 is
 * room for a longer one, and the first probe's 160-byte cut is the
 * reason this is not smaller. */
#define NETSTREAM_URL_MAX       (512)
#define NETSTREAM_NAME_MAX      (96)

/*
 * Allocates the ring and starts the task. Call once from app_main(),
 * before anything can ask for a stream. Returns false if the PSRAM
 * allocation or the task creation failed, after which every
 * netstream_play() is refused and the state stays IDLE.
 */
bool netstream_init(void);

/*
 * Ask for a stream. `url` is the station URL -- the one from the station
 * list, never a redirect target. `name` is the list's name for it, used
 * on screen until the station sends an icy-name of its own; it may be
 * NULL.
 *
 * Returns immediately. A stream already running is stopped first. The
 * result is visible through netstream_state(): CONNECTING, then
 * BUFFERING, then PLAYING, or RETRYING and FAILED.
 *
 * Safe from ui_task and from the decode loop.
 */
bool netstream_play(const char *url, const char *name);

/*
 * Ask for the stream to stop. Returns immediately; the state reaches
 * IDLE when the task has closed the connection. The ring is emptied, so
 * a decoder reading it gets 0 and should treat that as end of stream.
 *
 * Safe from ui_task and from the decode loop. Idempotent.
 */
void netstream_stop(void);

/*
 * Wait for the stop to complete, up to `timeout_ms`. For the callers
 * that must not leave a TLS session open behind them -- turning the
 * radio off, raising the portal, shutting down -- and for nobody else.
 * Never call this from ui_task. True if the task reached IDLE.
 */
bool netstream_stop_wait(int timeout_ms);

/*
 * Read up to `n` compressed bytes, blocking up to `timeout_ms` for the
 * first of them. Returns what it got, which may be 0 on a timeout or on
 * a stopped stream. This is phase 2's entry point and the probe's.
 *
 * The caller is whoever is decoding, and there is exactly one of them.
 */
size_t netstream_read(void *buf, size_t n, int timeout_ms);

/* ------------------------------------------------------------------ */
/* Published values. Any task, any time.                               */
/* ------------------------------------------------------------------ */

netstream_state_t netstream_state(void);

/* Bytes in the ring now, and that as a percentage of its size. The
 * percentage is what a screen wants; the bytes are what a watermark
 * decision wants. */
size_t netstream_buffered(void);
int    netstream_ring_pct(void);

/* Measured receive rate over the last window, in kbit/s of body -- the
 * probe's number, kept because it is how a link that cannot keep up
 * announces itself. 0 before the first window. */
int netstream_kbps(void);

/* How many attempts have failed since audio last flowed, and the status
 * code of the last response (0 if none). For the screen and the log. */
int netstream_failures(void);
int netstream_last_status(void);

/*
 * The station's name and current ICY title, copied into the caller's
 * buffer and always terminated. The title is empty when the station has
 * not sent one, and *also* when it sends an empty one -- WNZK sends
 * " - " forever. `netstream_has_title()` tells those apart so a screen
 * can show the station name instead of a blank line.
 */
void netstream_name(char *out, size_t out_size);
void netstream_title(char *out, size_t out_size);
bool netstream_has_title(void);

#ifdef __cplusplus
}
#endif
