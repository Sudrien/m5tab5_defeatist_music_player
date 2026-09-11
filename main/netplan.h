/*
 * netplan.h -- what to do about a status code, a hop and a failure, with
 * nothing to run.
 *
 * netstream.c has a socket, a task, a TLS session and a ring; none of
 * that can be tested anywhere but on the board. The *decisions* it makes
 * are a small table, and they are the part that will be wrong: whether
 * 303 is followed, whether 404 is retried forever, whether the fourth
 * failure waits 8 seconds or 8 minutes. Those live here, pure, so
 * `netplantest` can assert them.
 *
 * THE ONE THAT COST A PROBE RUN TO LEARN
 *
 * Zeno answers the station URL with a 302 to a host whose query string
 * carries a JWT, and the probe decoded it: `"iat":1789097220,
 * "exp":1789097280` -- the redirected URL is good for sixty seconds.
 * **So a reconnect starts from the station URL, always.** Caching the
 * resolved URL across a retry works for the first minute of testing and
 * then fails forever, which is the worst shape a bug can have. The rule
 * is a function here (`netplan_reconnect_from`) rather than a comment in
 * the reconnect path, because a comment is not something a test can
 * fail.
 *
 * WHY A FAILURE IS CLASSIFIED AND NOT JUST COUNTED
 *
 * A radio station that is off the air answers 404, and retrying it every
 * second for the rest of the evening is worse than saying so: the screen
 * says FAILED, the listener picks another station, and nothing holds a
 * TLS session open pointlessly. A 502 from a busy relay, or a TCP reset,
 * is the opposite -- that one comes back, usually within seconds. The
 * two are distinguished by status, and the classification is the whole
 * of the difference between a player that recovers and a player that
 * hammers a dead URL.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many redirects are followed before giving up. Zeno uses one. Five
 * is room for a station behind a CDN behind a vanity domain, and a
 * bound, because a redirect loop is otherwise a task that never stops.
 */
#define NETPLAN_HOPS_MAX        (5)

/*
 * The backoff schedule, in milliseconds, indexed by how many attempts
 * have already failed. After the last one the stream is FAILED and stops
 * trying; the listener presses play.
 */
#define NETPLAN_BACKOFF_MS      { 1000, 2000, 4000, 8000 }
#define NETPLAN_ATTEMPTS_MAX    (4)

/*
 * Published state. These are values, deliberately: the rule in CLAUDE.md
 * is that a cross-task fact is an integer somebody writes and anybody
 * reads, never a handle. ui_task reads this; it never touches the ring,
 * the client or the demuxer.
 */
typedef enum {
    NETSTREAM_IDLE = 0,     /* nothing asked for */
    NETSTREAM_CONNECTING,   /* DNS, TCP, TLS, headers */
    NETSTREAM_BUFFERING,    /* connected, filling before sound */
    NETSTREAM_PLAYING,      /* audio flowing */
    NETSTREAM_RETRYING,     /* dropped, waiting out the backoff */
    NETSTREAM_FAILED,       /* given up; needs a press */
    NETSTREAM_STOPPING,     /* asked to stop, unwinding */
} netstream_state_t;

static inline const char *netstream_state_name(netstream_state_t s)
{
    switch (s) {
    case NETSTREAM_IDLE:       return "idle";
    case NETSTREAM_CONNECTING: return "connecting";
    case NETSTREAM_BUFFERING:  return "buffering";
    case NETSTREAM_PLAYING:    return "playing";
    case NETSTREAM_RETRYING:   return "retrying";
    case NETSTREAM_FAILED:     return "failed";
    case NETSTREAM_STOPPING:   return "stopping";
    default:                   return "?";
    }
}

/* What to do with the response that just arrived. */
typedef enum {
    NETPLAN_PLAY = 0,       /* a body worth reading */
    NETPLAN_REDIRECT,       /* follow Location, if hops are left */
    NETPLAN_RETRY,          /* transient: back off and start again */
    NETPLAN_FATAL,          /* this URL will not work; stop and say so */
} netplan_action_t;

static inline const char *netplan_action_name(netplan_action_t a)
{
    switch (a) {
    case NETPLAN_PLAY:     return "play";
    case NETPLAN_REDIRECT: return "redirect";
    case NETPLAN_RETRY:    return "retry";
    case NETPLAN_FATAL:    return "fatal";
    default:               return "?";
    }
}

/*
 * A status code becomes an action.
 *
 * 200 is the ordinary answer. **ICY 200 OK** -- the old Shoutcast status
 * line -- is normalised to 200 by the caller before it gets here; it is
 * not HTTP and esp_http_client may not parse it, which is a hardware
 * question noted in netstream.h and not a decision.
 *
 * 206 is accepted because a server that decided to answer a rangeless
 * GET with a partial body is still sending audio.
 *
 * 301/302/307/308 are followed as-is. 303 is followed too: it means
 * "GET this instead", and this is a GET.
 *
 * 401, 403, 404, 410 and 451 are the station saying no, in five ways
 * that do not change if asked again. 400 and 405 are this code being
 * wrong about the request, which retrying will not fix either.
 *
 * 429 and every 5xx are transient by definition. So is anything
 * unrecognised: the safe default when a server says something strange is
 * to try once more, not to declare the station dead.
 */
static inline netplan_action_t netplan_action(int status)
{
    if (status == 200 || status == 206) return NETPLAN_PLAY;
    if (status == 301 || status == 302 || status == 303 ||
        status == 307 || status == 308) return NETPLAN_REDIRECT;
    if (status == 400 || status == 401 || status == 403 || status == 404 ||
        status == 405 || status == 410 || status == 451) return NETPLAN_FATAL;
    return NETPLAN_RETRY;
}

/*
 * Shoutcast's `ICY 200 OK` status line, recognised. Some stations still
 * answer with it and an HTTP parser may report 0 or -1 rather than 200.
 * Given the first line of the response, this says what status it means;
 * 0 when the line is not one of these.
 */
static inline int netplan_icy_status(const char *line)
{
    if (!line) return 0;
    if (line[0] != 'I' || line[1] != 'C' || line[2] != 'Y' || line[3] != ' ') return 0;
    const char *p = line + 4;
    while (*p == ' ') p++;
    int v = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) { v = v * 10 + (*p++ - '0'); digits++; }
    return digits ? v : 0;
}

/* Another hop allowed? `hop` is how many have already been taken. */
static inline bool netplan_may_redirect(int hop)
{
    return hop >= 0 && hop < NETPLAN_HOPS_MAX;
}

/*
 * How long to wait before attempt number `failed + 1`, where `failed` is
 * the number that have already failed since the last time audio flowed.
 * -1 means stop: the state goes to FAILED and nothing retries until the
 * listener asks.
 */
static inline int netplan_backoff_ms(int failed)
{
    static const int table[NETPLAN_ATTEMPTS_MAX] = NETPLAN_BACKOFF_MS;
    if (failed < 0) return table[0];
    if (failed >= NETPLAN_ATTEMPTS_MAX) return -1;
    return table[failed];
}

/*
 * Which URL a reconnect starts from.
 *
 * `station` is what the station list holds. `resolved` is where the last
 * redirect landed, which is tempting and wrong: Zeno's carries a token
 * that expires in sixty seconds, and a stored redirect is a URL that
 * works in testing and fails in use. Always the station URL. The
 * argument is taken anyway so that the answer is a decision this file
 * makes rather than a line netstream.c could quietly change.
 */
static inline const char *netplan_reconnect_from(const char *station,
                                                 const char *resolved)
{
    (void)resolved;
    return station;
}

/*
 * Whether a body that has started is worth keeping after a drop.
 *
 * A live stream has no position to resume from: bytes missed are gone,
 * and the compressed ring holds audio from *before* the drop, which is
 * still good audio and should be played out rather than thrown away. So
 * the ring survives a reconnect. What does not survive is the ICY byte
 * phase -- the new body starts its own metaint count -- which is what
 * icydemux_reconnect() is for.
 *
 * Returned as a function so the reasoning has somewhere to live and a
 * test can pin it.
 */
static inline bool netplan_keep_buffer_on_reconnect(void)
{
    return true;
}

/*
 * Whether a failure counter resets. It resets on audio, not on a
 * successful connect: a station that accepts a connection and drops it
 * after two hundred bytes, forever, would otherwise be retried forever
 * at one second apart. `audio_bytes` is what has arrived on the
 * connection that just ended.
 */
#define NETPLAN_PROGRESS_BYTES  (16 * 1024)

static inline bool netplan_made_progress(uint64_t audio_bytes)
{
    return audio_bytes >= NETPLAN_PROGRESS_BYTES;
}

#ifdef __cplusplus
}
#endif
