/*
 * streamplan.h -- what play_stream() shows and what it does next.
 *
 * Phase 3's decisions, separated from phase 3's plumbing. `play_stream()`
 * has to combine two state machines that already exist -- netstream's
 * connection state (netplan.h) and the buffer phase (bufferplan.h) --
 * into the two answers a frame of playback needs: **what goes on the
 * screen**, and **whether this stream is over**. Neither answer needs a
 * socket, a decoder or a ring, so neither is written inside the loop
 * where it cannot be tested.
 *
 * WHY THIS IS NOT JUST `bufplan_phase_name()`
 *
 * Because the two machines disagree, routinely and correctly, and the
 * screen has to pick one of them. The cases that forced this file out of
 * player.c:
 *
 *   netstream        bufplan        what a listener should see
 *   ---------------  -------------  --------------------------------
 *   RETRYING         PLAYING        the station name. Nothing is wrong
 *                                   *yet*: eighteen seconds of PCM ring
 *                                   means a reconnect is inaudible, and
 *                                   flashing "Reconnecting" over music
 *                                   that is still playing reports the
 *                                   plumbing rather than the experience.
 *   PLAYING          REBUFFERING    "Buffering". The link recovered but
 *                                   the ring has not, and the silence is
 *                                   the thing that needs explaining.
 *   FAILED           DRAINING       the station name, still. There is
 *                                   audio left to hear and it is the
 *                                   station's.
 *   RETRYING         REBUFFERING    "Reconnecting". Now it is audible
 *                                   and now it is worth saying.
 *
 * The rule that falls out: **the buffer decides whether to complain, the
 * connection decides what the complaint says.** A stall is only reported
 * when it can be heard.
 *
 * THE TITLE IS NOT ALWAYS THE TITLE
 *
 * WNZK sends " - " forever, which `netstream_has_title()` already
 * distinguishes from an absent one. But a station that sends its own
 * name as the title -- and several do, for the whole broadcast -- would
 * have the screen print the same string twice. `streamplan_lines()`
 * suppresses the second line when it matches the first, case-folded and
 * trimmed, so that comparison lives here with a test rather than in a
 * draw call.
 *
 * PAUSE IS A DISCONNECT, AND THAT IS A DECISION WITH A CONSEQUENCE
 *
 * netstream.h says there is no `netstream_pause()` and why. What it does
 * not say is what the screen does in the four seconds after the resume,
 * and the answer is not "Paused": it is "Buffering", because that is what
 * is happening, and a transport that says "Paused" while it is actually
 * reconnecting invites a second press. `streamplan_transport()` maps a
 * press to one of four actions and the caller does no reasoning of its
 * own.
 *
 * HOST-TESTED, NOT COMPILED FOR THE TARGET
 *
 * `texttest/streamplantest.c`. Pure inline functions over enums and
 * strings: no FreeRTOS, no esp_http_client, no floats. Nothing here has
 * been through the cross compiler, and per CLAUDE.md that is stated
 * rather than assumed.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bufferplan.h"         /* bufplan_phase_t */
#include "netplan.h"            /* netstream_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the status line says, as a value rather than a string, so that
 * the caller can choose whether it is drawn at all and the test can
 * assert on it without comparing prose.
 */
typedef enum {
    STREAMPLAN_STATUS_NONE = 0, /* playing normally; draw the station */
    STREAMPLAN_CONNECTING,      /* no sound yet, first attempt */
    STREAMPLAN_BUFFERING,       /* silent, filling, link is fine */
    STREAMPLAN_RECONNECTING,    /* silent, and the link is why */
    STREAMPLAN_FAILED,          /* silent, and nothing more is coming */
} streamplan_status_t;

static inline const char *streamplan_status_text(streamplan_status_t s)
{
    switch (s) {
    /* "" rather than NULL: every caller of this is building a line to
     * draw, and a NULL that has to be checked before each one is a crash
     * waiting for the one call site that forgets. */
    case STREAMPLAN_STATUS_NONE: return "";
    case STREAMPLAN_CONNECTING:  return "Connecting";
    case STREAMPLAN_BUFFERING:   return "Buffering";
    case STREAMPLAN_RECONNECTING: return "Reconnecting";
    case STREAMPLAN_FAILED:      return "No signal";
    default:                     return "";
    }
}

/*
 * The status line for one frame.
 *
 * `audible` is bufplan_out_t.audible -- whether the writer is running.
 * It is passed rather than derived from the phase because the phase that
 * means "audible" is two of them (PLAYING and DRAINING) and duplicating
 * that knowledge here is how the two would drift apart.
 */
static inline streamplan_status_t streamplan_status(netstream_state_t net,
                                                    bufplan_phase_t phase,
                                                    bool audible)
{
    /* Sound is coming out. Whatever the connection is doing, it is not
     * the listener's problem yet -- see the table at the top. */
    if (audible) return STREAMPLAN_STATUS_NONE;

    /* Silent and terminal. ENDED after a failure is the only ENDED that
     * needs saying: one reached by a stop request is a stream the
     * listener ended on purpose, and the screen has already moved on. */
    if (net == NETSTREAM_FAILED) return STREAMPLAN_FAILED;

    /* Silent, and the connection is the reason. */
    if (net == NETSTREAM_RETRYING || net == NETSTREAM_CONNECTING) {
        /* First attempt or a later one: the distinction is whether any
         * audio has been heard, which is exactly what PREROLL means. */
        return phase == BUFPLAN_PREROLL ? STREAMPLAN_CONNECTING
                                        : STREAMPLAN_RECONNECTING;
    }

    /* Silent, link is up: the ring is filling and that is all. */
    if (phase == BUFPLAN_PREROLL) return STREAMPLAN_CONNECTING;
    if (phase == BUFPLAN_REBUFFERING) return STREAMPLAN_BUFFERING;

    /* ENDED, or DRAINING with an empty ring. Nothing to say. */
    return STREAMPLAN_STATUS_NONE;
}

/* ------------------------------------------------------------------ */
/* The two lines of text                                               */
/* ------------------------------------------------------------------ */

#define STREAMPLAN_LINE_MAX     (256)

typedef struct {
    /* The station: icy-name if the station sent one, else the list's
     * name. Never empty if either was given. */
    char top[STREAMPLAN_LINE_MAX];
    /* The ICY title, or empty. Empty also when it merely repeats the
     * station name, which is a thing stations do for hours. */
    char bottom[STREAMPLAN_LINE_MAX];
} streamplan_lines_t;

/*
 * Case-folded, whitespace-collapsed equality, over ASCII only.
 *
 * ASCII only is deliberate and is a limitation, not an oversight: the
 * comparison exists to catch a station repeating its own name, those
 * names are overwhelmingly ASCII, and case-folding UTF-8 correctly needs
 * tables this player has no other use for. A non-Latin title that
 * repeats a non-Latin name will be shown twice, which is a cosmetic
 * miss and not a wrong string.
 */
static inline bool streamplan_same_text(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (;;) {
        while (*a == ' ' || *a == '\t' || *a == '-') a++;
        while (*b == ' ' || *b == '\t' || *b == '-') b++;
        if (!*a || !*b) break;
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    while (*a == ' ' || *a == '\t' || *a == '-') a++;
    while (*b == ' ' || *b == '\t' || *b == '-') b++;
    return !*a && !*b;
}

/* True if the string has anything in it but spaces, tabs and dashes.
 * WNZK's forever-title is " - ", and `netstream_has_title()` reports it
 * as present because the station did send it. */
static inline bool streamplan_text_useful(const char *s)
{
    if (!s) return false;
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '-') return true;
    }
    return false;
}

static inline void streamplan_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Build the two lines.
 *
 * `icy_name` is what the station called itself, `list_name` what the
 * station list called it, `title` the current ICY title. Any of them may
 * be NULL or empty.
 */
static inline void streamplan_lines(const char *icy_name,
                                    const char *list_name,
                                    const char *title,
                                    streamplan_lines_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* The station's own name wins over the list's: the list was typed by
     * whoever made the file and the station is authoritative about
     * itself. A useless icy-name -- blank, or the dash filler -- falls
     * back rather than blanking the line. */
    if (streamplan_text_useful(icy_name)) {
        streamplan_copy(out->top, sizeof(out->top), icy_name);
    } else if (streamplan_text_useful(list_name)) {
        streamplan_copy(out->top, sizeof(out->top), list_name);
    }

    if (!streamplan_text_useful(title)) return;
    /* The repeat. Compared against whichever name actually got used, not
     * against both: if the list name is on screen, a title matching the
     * *unused* icy-name is still new information. */
    if (streamplan_same_text(title, out->top)) return;
    streamplan_copy(out->bottom, sizeof(out->bottom), title);
}

/* ------------------------------------------------------------------ */
/* The transport                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    STREAMPLAN_NOTHING = 0,     /* the press means nothing here */
    STREAMPLAN_DISCONNECT,      /* stop the stream, keep the station */
    STREAMPLAN_CONNECT,         /* (re)connect to the current station */
    STREAMPLAN_LEAVE,           /* end the stream and return to the caller */
} streamplan_action_t;

static inline const char *streamplan_action_name(streamplan_action_t a)
{
    switch (a) {
    case STREAMPLAN_NOTHING:    return "nothing";
    case STREAMPLAN_DISCONNECT: return "disconnect";
    case STREAMPLAN_CONNECT:    return "connect";
    case STREAMPLAN_LEAVE:      return "leave";
    default:                    return "?";
    }
}

typedef enum {
    STREAMPLAN_PRESS_PLAYPAUSE = 0,
    STREAMPLAN_PRESS_STOP,
    STREAMPLAN_PRESS_NEXT,
    STREAMPLAN_PRESS_PREV,
} streamplan_press_t;

/*
 * What a press does.
 *
 * `connected` is "netstream is doing something other than sitting idle
 * or having given up" -- i.e. the listener's pause would have something
 * to act on. Next and previous are the caller's: it changes the station
 * and then connects, so both come back as LEAVE only when there is
 * nowhere to go.
 */
static inline streamplan_action_t streamplan_transport(streamplan_press_t p,
                                                        bool connected,
                                                        bool have_other_station)
{
    switch (p) {
    case STREAMPLAN_PRESS_PLAYPAUSE:
        /* The asymmetry is the point: pause drops the connection, and
         * play opens a new one from the station URL. There is no third
         * state to be in. */
        return connected ? STREAMPLAN_DISCONNECT : STREAMPLAN_CONNECT;
    case STREAMPLAN_PRESS_STOP:
        return STREAMPLAN_LEAVE;
    case STREAMPLAN_PRESS_NEXT:
    case STREAMPLAN_PRESS_PREV:
        /* A one-station list: next is not "restart this one". Restarting
         * on a press that means "something else" is the behaviour that
         * makes a transport feel broken. */
        return have_other_station ? STREAMPLAN_CONNECT : STREAMPLAN_NOTHING;
    default:
        return STREAMPLAN_NOTHING;
    }
}

/*
 * Whether a state counts as connected for the above.
 *
 * STOPPING is connected: a socket is still open and a play press during
 * the close must not race a second one into existence -- netstream.h
 * allows exactly one session, ever.
 */
static inline bool streamplan_is_connected(netstream_state_t s)
{
    return s == NETSTREAM_CONNECTING || s == NETSTREAM_BUFFERING ||
           s == NETSTREAM_PLAYING || s == NETSTREAM_RETRYING ||
           s == NETSTREAM_STOPPING;
}

/*
 * Will this source produce no more bytes, ever?
 *
 * This is bufplan_in_t.source_done, and it is here rather than inline in
 * play_stream() because **NETSTREAM_IDLE means two different things and
 * the state alone cannot tell them apart.**
 *
 * netstream_play() posts a request and returns without touching the
 * state, so for the first milliseconds after it is called the state is
 * still IDLE -- "not started yet", not "stopped". Reading that as done
 * ended a stream on its opening step, before a byte existed, and the
 * log read as a station hanging up:
 *
 *   idle -> connecting / HTTP 200 -> play / only 0 audio bytes
 *   stream ended: ended, 0 rebuffers, 99 ms silent, 0 frames
 *
 * `seen_live` is the caller's latch: false until the task has been
 * observed anywhere other than IDLE.
 *
 * `playing` is the pause. A pause disconnects on purpose and leaves the
 * state IDLE, and that must not end the stream -- it waits for a resume.
 *
 * RETRYING is live in both senses and is never done: netplan's backoff
 * runs to 15 s and BUFPLAN_STALL_GIVEUP_MS is 30 s, so a source working
 * through its retries is never cut off.
 */
static inline bool streamplan_source_done(netstream_state_t s,
                                           bool seen_live, bool playing)
{
    if (s == NETSTREAM_FAILED) return true;
    return seen_live && s == NETSTREAM_IDLE && playing;
}

/*
 * Is this stream over, from the buffer plan's `finished` plus the one
 * thing the buffer plan cannot see: that the listener left.
 *
 * Kept as a function of both so that the loop has one condition to test
 * and the test file has one thing to assert.
 */
static inline bool streamplan_done(bool bufplan_finished, bool leaving)
{
    return bufplan_finished || leaving;
}

#ifdef __cplusplus
}
#endif
