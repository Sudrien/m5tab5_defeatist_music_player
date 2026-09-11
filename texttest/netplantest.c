/*
 * netplantest.c -- the decision table netstream.c will act on.
 *
 * None of this can be observed on hardware without waiting for a station
 * to fail in the right way, which is why it is a table and not an if
 * chain buried in a reconnect loop. The checks below are the behaviour,
 * written down: a 404 station is given up on, a 502 is retried, a
 * redirect is followed five times and not six, and a reconnect never
 * reuses a resolved URL.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "netplan.h"

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

static void action_is(int status, netplan_action_t want)
{
    const netplan_action_t got = netplan_action(status);
    CHECK(got == want, "status %d -> %s, wanted %s", status,
          netplan_action_name(got), netplan_action_name(want));
}

int main(void)
{
    printf("netplantest\n");

    /* ---------------------------------------------------------------- */
    /* A body worth reading                                              */
    /* ---------------------------------------------------------------- */
    action_is(200, NETPLAN_PLAY);
    action_is(206, NETPLAN_PLAY);

    /* ---------------------------------------------------------------- */
    /* Redirects, including 303 -- this is a GET, so "GET this instead"  */
    /* is the same instruction                                           */
    /* ---------------------------------------------------------------- */
    action_is(301, NETPLAN_REDIRECT);
    action_is(302, NETPLAN_REDIRECT);   /* what Zeno actually sends */
    action_is(303, NETPLAN_REDIRECT);
    action_is(307, NETPLAN_REDIRECT);
    action_is(308, NETPLAN_REDIRECT);
    /* 304 is not a redirect and nothing here sends a conditional GET;
     * it falls to RETRY, which is the right answer for "inexplicable". */
    action_is(304, NETPLAN_RETRY);

    /* ---------------------------------------------------------------- */
    /* The station saying no, in ways that do not change on a retry      */
    /* ---------------------------------------------------------------- */
    action_is(400, NETPLAN_FATAL);
    action_is(401, NETPLAN_FATAL);
    action_is(403, NETPLAN_FATAL);
    action_is(404, NETPLAN_FATAL);      /* a station that is off the air */
    action_is(405, NETPLAN_FATAL);
    action_is(410, NETPLAN_FATAL);
    action_is(451, NETPLAN_FATAL);

    /* ---------------------------------------------------------------- */
    /* Transient, and the unrecognised default                           */
    /* ---------------------------------------------------------------- */
    action_is(429, NETPLAN_RETRY);      /* a relay at its listener cap */
    for (int s = 500; s <= 599; s++) action_is(s, NETPLAN_RETRY);
    action_is(0, NETPLAN_RETRY);        /* no status parsed at all */
    action_is(-1, NETPLAN_RETRY);
    action_is(999, NETPLAN_RETRY);
    action_is(418, NETPLAN_RETRY);

    /* Every code in range resolves to exactly one action and none of
     * them crashes the classifier. */
    for (int s = -10; s < 1000; s++) {
        const netplan_action_t a = netplan_action(s);
        CHECK(a == NETPLAN_PLAY || a == NETPLAN_REDIRECT ||
              a == NETPLAN_RETRY || a == NETPLAN_FATAL,
              "status %d gave action %d", s, (int)a);
    }

    /* ---------------------------------------------------------------- */
    /* Shoutcast's ICY status line                                       */
    /* ---------------------------------------------------------------- */
    CHECK(netplan_icy_status("ICY 200 OK") == 200, "ICY 200 OK not recognised");
    CHECK(netplan_icy_status("ICY 401 Service Unavailable") == 401,
          "ICY 401 not recognised");
    CHECK(netplan_icy_status("HTTP/1.0 200 OK") == 0, "HTTP line read as ICY");
    CHECK(netplan_icy_status("ICYbadly") == 0, "\"ICYbadly\" read as a status");
    CHECK(netplan_icy_status("ICY  200 OK") == 200, "extra space broke it");
    CHECK(netplan_icy_status("ICY OK") == 0, "no digits still gave a status");
    CHECK(netplan_icy_status("") == 0, "empty line gave a status");
    CHECK(netplan_icy_status("IC") == 0, "short line read off the end");
    CHECK(netplan_icy_status(NULL) == 0, "NULL gave a status");
    /* And the point of it: an ICY line classifies like the HTTP one. */
    CHECK(netplan_action(netplan_icy_status("ICY 200 OK")) == NETPLAN_PLAY,
          "ICY 200 did not become PLAY");

    /* ---------------------------------------------------------------- */
    /* Hop limit: five followed, the sixth refused                       */
    /* ---------------------------------------------------------------- */
    for (int h = 0; h < NETPLAN_HOPS_MAX; h++) {
        CHECK(netplan_may_redirect(h), "hop %d refused below the limit", h);
    }
    CHECK(!netplan_may_redirect(NETPLAN_HOPS_MAX),
          "hop %d allowed at the limit -- a redirect loop never ends",
          NETPLAN_HOPS_MAX);
    CHECK(!netplan_may_redirect(NETPLAN_HOPS_MAX + 1), "past the limit allowed");
    CHECK(!netplan_may_redirect(-1), "a negative hop count allowed");

    /* ---------------------------------------------------------------- */
    /* Backoff: 1, 2, 4, 8 seconds, then stop                            */
    /* ---------------------------------------------------------------- */
    CHECK(netplan_backoff_ms(0) == 1000, "first backoff %d", netplan_backoff_ms(0));
    CHECK(netplan_backoff_ms(1) == 2000, "second backoff %d", netplan_backoff_ms(1));
    CHECK(netplan_backoff_ms(2) == 4000, "third backoff %d", netplan_backoff_ms(2));
    CHECK(netplan_backoff_ms(3) == 8000, "fourth backoff %d", netplan_backoff_ms(3));
    CHECK(netplan_backoff_ms(NETPLAN_ATTEMPTS_MAX) == -1,
          "the fifth attempt was not refused: %d",
          netplan_backoff_ms(NETPLAN_ATTEMPTS_MAX));
    CHECK(netplan_backoff_ms(99) == -1, "a large count was not refused");
    CHECK(netplan_backoff_ms(-1) == 1000, "a negative count did not clamp");

    /* It doubles, it never goes backwards, and the whole schedule is
     * bounded -- fifteen seconds from the first failure to FAILED. The
     * bound is the useful number: it is how long the screen can say
     * "retrying" before it says something a listener can act on. */
    {
        int total = 0, prev = 0;
        for (int i = 0; i < NETPLAN_ATTEMPTS_MAX; i++) {
            const int ms = netplan_backoff_ms(i);
            CHECK(ms > 0, "attempt %d has no wait", i);
            CHECK(ms >= prev, "attempt %d waits less than %d did", i, i - 1);
            prev = ms;
            total += ms;
        }
        CHECK(total == 15000, "the whole schedule is %d ms, wanted 15000", total);
    }

    /* ---------------------------------------------------------------- */
    /* The sixty-second token: a reconnect never reuses a resolved URL   */
    /* ---------------------------------------------------------------- */
    {
        const char *station = "https://stream.zeno.fm/erunhwj5lekvv";
        const char *resolved =
            "https://stream-285.surfernetwork.com/erunhwj5lekvv?zt=eyJhbGci";
        const char *use = netplan_reconnect_from(station, resolved);
        CHECK(use == station,
              "a reconnect would reuse the redirected URL, whose token "
              "lives 60 s -- this works for a minute and then never again");
        CHECK(strcmp(use, station) == 0, "reconnect URL is not the station URL");
        /* Including when there was no redirect at all. */
        CHECK(netplan_reconnect_from(station, NULL) == station,
              "no-redirect case did not give the station URL");
    }

    /* ---------------------------------------------------------------- */
    /* What survives a reconnect                                         */
    /* ---------------------------------------------------------------- */
    CHECK(netplan_keep_buffer_on_reconnect(),
          "buffered audio from before the drop is still good audio");

    /* ---------------------------------------------------------------- */
    /* Progress, so a station that connects and dies is not retried      */
    /* forever                                                           */
    /* ---------------------------------------------------------------- */
    CHECK(!netplan_made_progress(0), "zero bytes counted as progress");
    CHECK(!netplan_made_progress(200), "200 bytes counted as progress");
    CHECK(!netplan_made_progress(NETPLAN_PROGRESS_BYTES - 1),
          "one byte short counted as progress");
    CHECK(netplan_made_progress(NETPLAN_PROGRESS_BYTES),
          "the threshold itself did not count");
    CHECK(netplan_made_progress(50u * 1024 * 1024), "50 MB did not count");

    /* ---------------------------------------------------------------- */
    /* Every state has a name, so a log line can never be a bare number  */
    /* ---------------------------------------------------------------- */
    {
        const netstream_state_t all[] = {
            NETSTREAM_IDLE, NETSTREAM_CONNECTING, NETSTREAM_BUFFERING,
            NETSTREAM_PLAYING, NETSTREAM_RETRYING, NETSTREAM_FAILED,
            NETSTREAM_STOPPING,
        };
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            const char *n = netstream_state_name(all[i]);
            CHECK(n && n[0] && strcmp(n, "?") != 0,
                  "state %d has no name", (int)all[i]);
            for (size_t j = 0; j < i; j++) {
                CHECK(strcmp(n, netstream_state_name(all[j])) != 0,
                      "states %d and %d share the name \"%s\"",
                      (int)all[i], (int)all[j], n);
            }
        }
        CHECK(strcmp(netstream_state_name((netstream_state_t)99), "?") == 0,
              "an unknown state did not fall back to \"?\"");
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
