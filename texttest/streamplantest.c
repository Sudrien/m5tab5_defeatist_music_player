/*
 * streamplantest.c -- what play_stream() shows, and what a press does.
 *
 * The cases worth writing down are the ones where netstream and
 * bufferplan disagree, because those are the ones a loop written by
 * feel gets wrong: a reconnect behind eighteen seconds of buffered PCM
 * is not a thing to report, and a recovered link with an empty ring
 * still is. The table at the top of streamplan.h is the specification
 * and this is that table, executed.
 *
 * Also here: the station that sends its own name as the title for six
 * hours, which is the only reason streamplan_same_text() exists.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "streamplan.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do {                           \
    checks++;                                           \
    if (!(cond)) {                                      \
        failures++;                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
    }                                                   \
} while (0)

static void status_is(netstream_state_t net, bufplan_phase_t phase,
                      bool audible, streamplan_status_t want)
{
    const streamplan_status_t got = streamplan_status(net, phase, audible);
    CHECK(got == want, "%s + %s + %s -> \"%s\", wanted \"%s\"",
          netstream_state_name(net), bufplan_phase_name(phase),
          audible ? "audible" : "silent",
          streamplan_status_text(got), streamplan_status_text(want));
}

static void lines_are(const char *icy, const char *list, const char *title,
                      const char *want_top, const char *want_bottom)
{
    streamplan_lines_t l;
    streamplan_lines(icy, list, title, &l);
    CHECK(strcmp(l.top, want_top) == 0,
          "top \"%s\", wanted \"%s\"", l.top, want_top);
    CHECK(strcmp(l.bottom, want_bottom) == 0,
          "bottom \"%s\", wanted \"%s\"", l.bottom, want_bottom);
}

static void press_is(streamplan_press_t p, bool connected, bool other,
                     streamplan_action_t want)
{
    const streamplan_action_t got = streamplan_transport(p, connected, other);
    CHECK(got == want, "press %d (%s, %s) -> %s, wanted %s", (int)p,
          connected ? "connected" : "idle", other ? "list" : "one station",
          streamplan_action_name(got), streamplan_action_name(want));
}

int main(void)
{
    printf("streamplantest\n");

    /* ---------------------------------------------------------------- */
    /* Audible beats everything. The four rows of the header's table.    */
    /* ---------------------------------------------------------------- */

    /* A reconnect nobody can hear is not a message. */
    status_is(NETSTREAM_RETRYING, BUFPLAN_PLAYING, true,
              STREAMPLAN_STATUS_NONE);
    /* A failure with audio still queued is not a message either. */
    status_is(NETSTREAM_FAILED, BUFPLAN_DRAINING, true,
              STREAMPLAN_STATUS_NONE);
    /* Recovered link, empty ring: the silence is the story. */
    status_is(NETSTREAM_PLAYING, BUFPLAN_REBUFFERING, false,
              STREAMPLAN_BUFFERING);
    /* Silent AND retrying: now the link is the story. */
    status_is(NETSTREAM_RETRYING, BUFPLAN_REBUFFERING, false,
              STREAMPLAN_RECONNECTING);

    /* ---------------------------------------------------------------- */
    /* Before the first sound                                            */
    /* ---------------------------------------------------------------- */

    status_is(NETSTREAM_CONNECTING, BUFPLAN_PREROLL, false,
              STREAMPLAN_CONNECTING);
    status_is(NETSTREAM_BUFFERING, BUFPLAN_PREROLL, false,
              STREAMPLAN_CONNECTING);
    /* Bytes arriving, ring still under the 4 s watermark: the link is
     * fine and the wait is the buffer's. Still "Connecting" rather than
     * "Buffering", because nothing has been heard yet and the two words
     * mean the same thing to a listener at this point -- while after
     * audio they do not. */
    status_is(NETSTREAM_PLAYING, BUFPLAN_PREROLL, false,
              STREAMPLAN_CONNECTING);
    /* A retry before ANY audio is still the first connection as far as
     * the listener is concerned. */
    status_is(NETSTREAM_RETRYING, BUFPLAN_PREROLL, false,
              STREAMPLAN_CONNECTING);

    /* ---------------------------------------------------------------- */
    /* Given up                                                          */
    /* ---------------------------------------------------------------- */

    status_is(NETSTREAM_FAILED, BUFPLAN_PREROLL, false, STREAMPLAN_FAILED);
    status_is(NETSTREAM_FAILED, BUFPLAN_REBUFFERING, false, STREAMPLAN_FAILED);
    status_is(NETSTREAM_FAILED, BUFPLAN_ENDED, false, STREAMPLAN_FAILED);

    /* A stream the listener ended: netstream is IDLE, not FAILED, and
     * there is nothing to report. This is the case that would put "No
     * signal" on screen on the way out of a station if FAILED and IDLE
     * were treated alike. */
    status_is(NETSTREAM_IDLE, BUFPLAN_ENDED, false, STREAMPLAN_STATUS_NONE);
    status_is(NETSTREAM_STOPPING, BUFPLAN_ENDED, false,
              STREAMPLAN_STATUS_NONE);

    /* Draining with an empty ring: about to end, nothing to say. */
    status_is(NETSTREAM_IDLE, BUFPLAN_DRAINING, false,
              STREAMPLAN_STATUS_NONE);

    /* ---------------------------------------------------------------- */
    /* The two lines                                                     */
    /* ---------------------------------------------------------------- */

    /* The ordinary case. */
    lines_are("WUOM", "Michigan Radio", "Morning Edition",
              "WUOM", "Morning Edition");
    /* No icy-name: the list's name carries it. */
    lines_are(NULL, "Michigan Radio", "Morning Edition",
              "Michigan Radio", "Morning Edition");
    /* No name at all from either side. */
    lines_are(NULL, NULL, "Morning Edition", "", "Morning Edition");

    /* WNZK's forever-title, which netstream_has_title() reports as
     * present because the station really did send it. */
    lines_are("WNZK-AM", "WNZK", " - ", "WNZK-AM", "");
    lines_are("WNZK-AM", "WNZK", "", "WNZK-AM", "");
    lines_are("WNZK-AM", "WNZK", "---", "WNZK-AM", "");

    /* The station that sends its own name as the title. Case and the
     * dash padding both folded. */
    lines_are("WUOM", "Michigan Radio", "WUOM", "WUOM", "");
    lines_are("WUOM", "Michigan Radio", "wuom", "WUOM", "");
    lines_are("WUOM", "Michigan Radio", " - WUOM - ", "WUOM", "");
    /* But a title matching the name that was NOT used is still news:
     * the screen is showing the list's name, so "WUOM" adds something. */
    lines_are(NULL, "Michigan Radio", "WUOM", "Michigan Radio", "WUOM");

    /* A useless icy-name falls back rather than blanking the line. */
    lines_are(" - ", "Michigan Radio", "Morning Edition",
              "Michigan Radio", "Morning Edition");
    lines_are("", "Michigan Radio", NULL, "Michigan Radio", "");

    /* Not a repeat: a prefix is not a match. This is the check that
     * stops "WUOM" suppressing "WUOM News at Nine". */
    lines_are("WUOM", "Michigan Radio", "WUOM News at Nine",
              "WUOM", "WUOM News at Nine");

    /* ---------------------------------------------------------------- */
    /* same_text on its own                                              */
    /* ---------------------------------------------------------------- */

    CHECK(streamplan_same_text("a", "A"), "case");
    CHECK(streamplan_same_text("", ""), "both empty");
    CHECK(streamplan_same_text(" - ", ""), "dashes are nothing");
    CHECK(!streamplan_same_text("ab", "a"), "prefix is not equal");
    CHECK(!streamplan_same_text("a", "ab"), "prefix is not equal, reversed");
    CHECK(!streamplan_same_text(NULL, "a"), "NULL is never equal");
    CHECK(!streamplan_same_text("a", NULL), "NULL is never equal, reversed");

    CHECK(streamplan_text_useful("x"), "a letter is useful");
    CHECK(!streamplan_text_useful(" - "), "the filler is not");
    CHECK(!streamplan_text_useful(""), "empty is not");
    CHECK(!streamplan_text_useful(NULL), "NULL is not");

    /* ---------------------------------------------------------------- */
    /* Truncation. STREAMPLAN_LINE_MAX is netstream's title maximum, so   */
    /* an over-long input can only come from a list name, but the copy    */
    /* must terminate either way.                                         */
    /* ---------------------------------------------------------------- */
    {
        char big[STREAMPLAN_LINE_MAX * 2];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        streamplan_lines_t l;
        streamplan_lines(big, NULL, NULL, &l);
        CHECK(strlen(l.top) == STREAMPLAN_LINE_MAX - 1,
              "long name truncated to %zu", strlen(l.top));
    }

    /* A NULL out must not crash. */
    streamplan_lines("a", "b", "c", NULL);

    /* ---------------------------------------------------------------- */
    /* The transport                                                     */
    /* ---------------------------------------------------------------- */

    /* Pause is a disconnect; play is a fresh connection. */
    press_is(STREAMPLAN_PRESS_PLAYPAUSE, true, true, STREAMPLAN_DISCONNECT);
    press_is(STREAMPLAN_PRESS_PLAYPAUSE, false, true, STREAMPLAN_CONNECT);
    /* And with a one-station list, unchanged: play/pause is about the
     * connection, not the list. */
    press_is(STREAMPLAN_PRESS_PLAYPAUSE, true, false, STREAMPLAN_DISCONNECT);
    press_is(STREAMPLAN_PRESS_PLAYPAUSE, false, false, STREAMPLAN_CONNECT);

    press_is(STREAMPLAN_PRESS_STOP, true, true, STREAMPLAN_LEAVE);
    press_is(STREAMPLAN_PRESS_STOP, false, false, STREAMPLAN_LEAVE);

    press_is(STREAMPLAN_PRESS_NEXT, true, true, STREAMPLAN_CONNECT);
    press_is(STREAMPLAN_PRESS_PREV, true, true, STREAMPLAN_CONNECT);
    /* One station in the list: next does NOT restart it. */
    press_is(STREAMPLAN_PRESS_NEXT, true, false, STREAMPLAN_NOTHING);
    press_is(STREAMPLAN_PRESS_PREV, true, false, STREAMPLAN_NOTHING);
    /* Next off a failed station is still next: the list is the way out
     * of a station that will not play. */
    press_is(STREAMPLAN_PRESS_NEXT, false, true, STREAMPLAN_CONNECT);

    /* ---------------------------------------------------------------- */
    /* is_connected -- STOPPING counts, and why                          */
    /* ---------------------------------------------------------------- */

    CHECK(streamplan_is_connected(NETSTREAM_CONNECTING), "connecting");
    CHECK(streamplan_is_connected(NETSTREAM_BUFFERING), "buffering");
    CHECK(streamplan_is_connected(NETSTREAM_PLAYING), "playing");
    CHECK(streamplan_is_connected(NETSTREAM_RETRYING), "retrying");
    /* A socket is still open. One session, ever. */
    CHECK(streamplan_is_connected(NETSTREAM_STOPPING), "stopping holds one");
    CHECK(!streamplan_is_connected(NETSTREAM_IDLE), "idle");
    CHECK(!streamplan_is_connected(NETSTREAM_FAILED), "failed");

    /* A play press while STOPPING must therefore be a disconnect, not a
     * second connect. That is the race netstream.h refuses outright. */
    press_is(STREAMPLAN_PRESS_PLAYPAUSE,
             streamplan_is_connected(NETSTREAM_STOPPING), true,
             STREAMPLAN_DISCONNECT);

    /* ---------------------------------------------------------------- */
    /* source_done -- the IDLE that is not done                          */
    /* ---------------------------------------------------------------- */

    /* THE BUG THIS EXISTS FOR. netstream_play() posts a request and
     * returns without touching the state, so the first reading after it
     * is IDLE and means "not started yet". Calling that done ended a
     * stream on its opening step: 99 ms silent, 0 frames. */
    CHECK(!streamplan_source_done(NETSTREAM_IDLE, false, true),
          "IDLE before the task has started is NOT done");

    /* Once the task has been seen live, IDLE is a real stop. */
    CHECK(streamplan_source_done(NETSTREAM_IDLE, true, true),
          "IDLE after running is done");

    /* A pause disconnects on purpose and leaves IDLE. Ending the stream
     * there would make pause destroy the station. */
    CHECK(!streamplan_source_done(NETSTREAM_IDLE, true, false),
          "IDLE while paused waits for the resume");
    CHECK(!streamplan_source_done(NETSTREAM_IDLE, false, false),
          "IDLE while paused and never live is still not done");

    /* FAILED is done whatever else is true -- including before the task
     * was ever seen live, which is a station that refused on its first
     * attempt. */
    CHECK(streamplan_source_done(NETSTREAM_FAILED, false, true),
          "FAILED is done even unseen");
    CHECK(streamplan_source_done(NETSTREAM_FAILED, true, false),
          "FAILED is done even paused");

    /* Everything live is not done. RETRYING especially: netplan's
     * backoff runs to 15 s and the stall giveup is 30 s. */
    CHECK(!streamplan_source_done(NETSTREAM_CONNECTING, true, true), "connecting");
    CHECK(!streamplan_source_done(NETSTREAM_BUFFERING, true, true), "buffering");
    CHECK(!streamplan_source_done(NETSTREAM_PLAYING, true, true), "playing");
    CHECK(!streamplan_source_done(NETSTREAM_RETRYING, true, true),
          "RETRYING is live, not done");
    CHECK(!streamplan_source_done(NETSTREAM_STOPPING, true, true),
          "STOPPING still has a session");

    /* ---------------------------------------------------------------- */
    /* done                                                              */
    /* ---------------------------------------------------------------- */

    CHECK(streamplan_done(true, false), "bufplan finished ends it");
    CHECK(streamplan_done(false, true), "leaving ends it");
    CHECK(streamplan_done(true, true), "both");
    CHECK(!streamplan_done(false, false), "neither");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
