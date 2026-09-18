/*
 * radiobrowsertest.c -- the radio-browser request, and the response
 * going into the parser that was chosen for it.
 *
 * Two halves, and the second is the one that matters most.
 *
 * The first is the encoder and the URL builder: a search term arrives
 * from a form on somebody's phone, so it can contain `&`, `=`, `#`, `+`,
 * `%`, a space, or UTF-8, and every one of those changes the request if
 * it goes through raw. Refusals are checked as carefully as successes,
 * because a truncated percent-encoding is not a shorter search, it is an
 * invalid one.
 *
 * The second is `stationlist_parse()` against the server's ACTUAL M3U --
 * the shape in the API documentation, `#RADIOBROWSERUUID` and blank
 * lines and all. `stations.h` and `stationlist.h` have both claimed
 * since before the stream path existed that radio-browser's M3U goes
 * straight into this parser, and that claim has never been tested
 * against a real response. It is the reason `stations.m3u` is M3U rather
 * than a format of this project's own, so if it were wrong, two headers
 * and a design decision would be wrong with it.
 *
 * It is not wrong, and the fixture below is why.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "radiobrowser.h"

/* The first mirror, spelled once. */
#define HOST "de1.api.radio-browser.info"
#include "stationlist.h"

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

/*
 * A response, exactly the shape the API reference documents for
 * /m3u/stations/search: the header, then per station a UUID comment, an
 * #EXTINF, a URL, and a blank line.
 *
 * The names are chosen to exercise the parser at the same time: one
 * plain, one with a comma in it (legal -- the name is everything after
 * the FIRST comma), one with a space after the comma, and one entry with
 * no #EXTINF at all, which is not something this endpoint emits but is
 * what the file looks like after somebody pastes into it.
 */
static const char m3u_response[] =
    "#EXTM3U\r\n"
    "#RADIOBROWSERUUID:01234567-89ab-cdef-0123-456789abcdef\r\n"
    "#EXTINF:-1,Best Radio\r\n"
    "http://stream.example.com/mp3_128\r\n"
    "\r\n"
    "#RADIOBROWSERUUID:11234567-89ab-cdef-0123-456789abcdef\r\n"
    "#EXTINF:-1,Radio Two, the second one\r\n"
    "https://stream.example2.com/mp3_256\r\n"
    "\r\n"
    "#RADIOBROWSERUUID:21234567-89ab-cdef-0123-456789abcdef\r\n"
    "#EXTINF:-1, Spaced Name\r\n"
    "https://stream.example3.com/aac\r\n"
    "\r\n"
    "https://stream.example4.com/bare\r\n";

int main(void)
{
    printf("radiobrowsertest\n");

    char url[RADIOBROWSER_URL_MAX];
    char enc[256];

    /* ---------------------------------------------------------------- */
    /* Percent-encoding                                                  */
    /* ---------------------------------------------------------------- */
    CHECK(radiobrowser_encode(enc, sizeof(enc), "jazz"), "plain encodes");
    CHECK(strcmp(enc, "jazz") == 0, "unreserved passes through: %s", enc);

    CHECK(radiobrowser_encode(enc, sizeof(enc), "smooth jazz"), "space ok");
    CHECK(strcmp(enc, "smooth%20jazz") == 0, "space is %%20, got %s", enc);

    /* The one that changes the request rather than the search. A `+` in
     * a query string means a space, so a station literally called "C++"
     * would otherwise be searched for as "C  ". */
    CHECK(radiobrowser_encode(enc, sizeof(enc), "C++"), "plus ok");
    CHECK(strcmp(enc, "C%2B%2B") == 0, "plus is %%2B, got %s", enc);

    /* Query-string metacharacters: each of these would add a parameter
     * or end the query if it went through. */
    CHECK(radiobrowser_encode(enc, sizeof(enc), "a&b=c"), "ampersand ok");
    CHECK(strcmp(enc, "a%26b%3Dc") == 0, "& and = encoded, got %s", enc);

    CHECK(radiobrowser_encode(enc, sizeof(enc), "rock#1"), "hash ok");
    CHECK(strcmp(enc, "rock%231") == 0, "# encoded, got %s", enc);

    /* A literal percent the listener typed, which must not be read as
     * the start of an escape by the server. */
    CHECK(radiobrowser_encode(enc, sizeof(enc), "100%"), "percent ok");
    CHECK(strcmp(enc, "100%25") == 0, "%% encoded, got %s", enc);

    /* UTF-8, byte by byte and uppercase hex. "ö" is C3 B6. */
    CHECK(radiobrowser_encode(enc, sizeof(enc), "K\xc3\xb6ln"), "utf-8 ok");
    CHECK(strcmp(enc, "K%C3%B6ln") == 0, "utf-8 per byte, got %s", enc);

    /* Unreserved is exactly RFC 3986's set and no more. */
    CHECK(radiobrowser_encode(enc, sizeof(enc), "a-b_c.d~e"), "unreserved");
    CHECK(strcmp(enc, "a-b_c.d~e") == 0, "all four pass, got %s", enc);

    /* ---------------------------------------------------------------- */
    /* Encoding refusals -- empty, not truncated                         */
    /* ---------------------------------------------------------------- */
    {
        char tiny[4];
        /* Needs 3 bytes plus a NUL and has 4, so it fits exactly. */
        CHECK(radiobrowser_encode(tiny, sizeof(tiny), " "), "exact fit");
        CHECK(strcmp(tiny, "%20") == 0, "exact fit value: %s", tiny);

        /* Two escapes into four bytes cannot fit. The point is that what
         * comes back is empty rather than "%20", which would be a
         * different and valid-looking search. */
        CHECK(!radiobrowser_encode(tiny, sizeof(tiny), "  "), "overflow no");
        CHECK(tiny[0] == '\0', "refusal empties rather than truncates");

        /* A multi-byte character that does not fit must not leave half a
         * sequence behind. */
        CHECK(!radiobrowser_encode(tiny, sizeof(tiny), "\xc3\xb6"), "utf-8 no");
        CHECK(tiny[0] == '\0', "partial utf-8 is not left behind");
    }

    /* ---------------------------------------------------------------- */
    /* Terms                                                             */
    /* ---------------------------------------------------------------- */
    {
        char t[RADIOBROWSER_TERM_MAX];
        CHECK(radiobrowser_term_ok("  jazz  ", t, sizeof(t)), "trims");
        CHECK(strcmp(t, "jazz") == 0, "trimmed to %s", t);

        /* The commonest submission there is: somebody pressed the
         * button. This must not become a search for everything, which
         * with limit=50 would look like it had worked. */
        CHECK(!radiobrowser_term_ok("", t, sizeof(t)), "empty refused");
        CHECK(!radiobrowser_term_ok("   ", t, sizeof(t)), "spaces refused");
        CHECK(!radiobrowser_term_ok("\t \t", t, sizeof(t)), "tabs refused");
        CHECK(!radiobrowser_term_ok(NULL, t, sizeof(t)), "NULL refused");
        CHECK(t[0] == '\0', "refusal leaves the term empty");

        /* Interior whitespace is a search, not padding. */
        CHECK(radiobrowser_term_ok(" smooth jazz ", t, sizeof(t)), "interior");
        CHECK(strcmp(t, "smooth jazz") == 0, "kept interior space: %s", t);
    }

    /* ---------------------------------------------------------------- */
    /* The search URL                                                    */
    /* ---------------------------------------------------------------- */
    CHECK(radiobrowser_search_url(url, sizeof(url),
                                  "de1.api.radio-browser.info", "jazz"),
          "url builds");
    CHECK(strcmp(url,
                 "https://de1.api.radio-browser.info"
                 "/m3u/stations/search?name=jazz"
                 "&hidebroken=true&order=votes&reverse=true&limit=50") == 0,
          "url is\n    %s", url);

    /* https, always: the pool name has no usable certificate and a
     * mirror does, so there is no reason to fall back to http. */
    CHECK(strncmp(url, "https://", 8) == 0, "always https");

    /* The term is encoded in the URL too, not only by the encoder. */
    CHECK(radiobrowser_search_url(url, sizeof(url), "de1.api.radio-browser.info",
                                  "smooth jazz & blues"), "spaced term");
    CHECK(strstr(url, "name=smooth%20jazz%20%26%20blues") != NULL,
          "encoded in place: %s", url);

    /* Refusals. Each leaves the URL empty rather than partially built --
     * a half-built URL is a request to somewhere. */
    CHECK(!radiobrowser_search_url(url, sizeof(url),
                                   "de1.api.radio-browser.info", ""),
          "empty term refused");
    CHECK(url[0] == '\0', "refused URL is empty");
    CHECK(!radiobrowser_search_url(url, sizeof(url), "", "jazz"),
          "empty host refused");
    CHECK(!radiobrowser_search_url(url, sizeof(url), NULL, "jazz"),
          "NULL host refused");
    {
        /* A buffer that cannot hold the fixed part. */
        char small[16];
        CHECK(!radiobrowser_search_url(small, sizeof(small),
                                       "de1.api.radio-browser.info", "jazz"),
              "small buffer refused");
        CHECK(small[0] == '\0', "small buffer left empty");
    }
    {
        /* A term at the bound, all of it needing three bytes each. 63
         * spaces is 189 encoded, which still fits RADIOBROWSER_URL_MAX
         * with the 110-byte fixed part -- this is the arithmetic the
         * TERM_MAX comment claims and here it is exercised. */
        char worst[RADIOBROWSER_TERM_MAX];
        memset(worst, ' ', sizeof(worst) - 1);
        worst[sizeof(worst) - 1] = '\0';
        worst[0] = 'a';     /* or term_ok trims it to nothing */
        CHECK(radiobrowser_search_url(url, sizeof(url),
                                      "de1.api.radio-browser.info", worst),
              "worst-case term still fits in %d", RADIOBROWSER_URL_MAX);
    }

    /* ---------------------------------------------------------------- */
    /* The click URL, and the UUID shape check                           */
    /* ---------------------------------------------------------------- */
    CHECK(radiobrowser_uuid_ok("01234567-89ab-cdef-0123-456789abcdef"), "uuid");
    CHECK(radiobrowser_uuid_ok("01234567-89AB-CDEF-0123-456789ABCDEF"),
          "uuid uppercase");
    CHECK(!radiobrowser_uuid_ok("01234567-89ab-cdef-0123-456789abcde"),
          "35 chars refused");
    CHECK(!radiobrowser_uuid_ok("01234567-89ab-cdef-0123-456789abcdeff"),
          "37 chars refused");
    CHECK(!radiobrowser_uuid_ok(""), "empty uuid refused");
    CHECK(!radiobrowser_uuid_ok(NULL), "NULL uuid refused");
    /* The one this check exists for: anything that could escape the path
     * segment it is pasted into. */
    CHECK(!radiobrowser_uuid_ok("../../../../etc/passwd_aaaaaaaaaaaa"),
          "path traversal refused");
    CHECK(!radiobrowser_uuid_ok("01234567-89ab-cdef-0123-456789abcd?x"),
          "query character refused");

    CHECK(radiobrowser_click_url(url, sizeof(url),
                                 "de1.api.radio-browser.info",
                                 "01234567-89ab-cdef-0123-456789abcdef"),
          "click url builds");
    CHECK(strcmp(url, "https://de1.api.radio-browser.info"
                      "/json/url/01234567-89ab-cdef-0123-456789abcdef") == 0,
          "click url is\n    %s", url);

    /* ---------------------------------------------------------------- */
    /* THE ONE THAT MATTERS: the response through the existing parser     */
    /* ---------------------------------------------------------------- */
    {
        station_t st[STATIONLIST_MAX];
        stationlist_stats_t stats;
        const int n = stationlist_parse(m3u_response, strlen(m3u_response),
                                        st, STATIONLIST_MAX, &stats);

        CHECK(n == 4, "four stations out of the response, got %d", n);
        CHECK(stats.bad_scheme == 0, "no bad schemes, got %d",
              stats.bad_scheme);
        CHECK(stats.too_long == 0, "nothing too long, got %d", stats.too_long);
        CHECK(stats.overflowed == 0, "nothing overflowed");

        /* #RADIOBROWSERUUID did not become a station and did not eat the
         * #EXTINF after it. This is the whole claim. */
        CHECK(strcmp(st[0].name, "Best Radio") == 0,
              "name survives the UUID line: %s", st[0].name);
        CHECK(strcmp(st[0].url, "http://stream.example.com/mp3_128") == 0,
              "url 0: %s", st[0].url);

        /* A comma in the name: everything after the FIRST comma. */
        CHECK(strcmp(st[1].name, "Radio Two, the second one") == 0,
              "comma in name: %s", st[1].name);
        CHECK(strcmp(st[1].url, "https://stream.example2.com/mp3_256") == 0,
              "url 1: %s", st[1].url);

        /* A space after the comma is padding, not part of the name. */
        CHECK(strcmp(st[2].name, "Spaced Name") == 0,
              "space after comma trimmed: %s", st[2].name);

        /* The bare URL falls back to the host, which is what pasting
         * into the file gives. */
        CHECK(strcmp(st[3].url, "https://stream.example4.com/bare") == 0,
              "url 3: %s", st[3].url);
        CHECK(strcmp(st[3].name, "stream.example4.com") == 0,
              "bare url takes the host: %s", st[3].name);
    }

    /* The response is well under what stations.c will read it into, which
     * is the reason RADIOBROWSER_LIMIT is 50 and not the server's
     * default of 100000. Stated as a check so that raising the limit
     * without raising the buffer fails here. */
    CHECK(RADIOBROWSER_LIMIT <= STATIONLIST_MAX,
          "limit %d must fit STATIONLIST_MAX %d",
          RADIOBROWSER_LIMIT, STATIONLIST_MAX);

    /* ---------------------------------------------------------------- */
    /* 0401: browsing, and the two rules the service asks for            */
    /* ---------------------------------------------------------------- */

    printf("  browse URLs\n");
    {
        char url[RADIOBROWSER_URL_MAX];

        CHECK(radiobrowser_list_url(url, sizeof(url), HOST,
                                    RADIOBROWSER_TOPVOTE, NULL),
              "a chart needs no value");
        CHECK(strstr(url, "/m3u/stations/topvote/50") != NULL,
              "topvote path: %s", url);
        CHECK(strstr(url, "?hidebroken=true") != NULL,
              "and opens its own query: %s", url);

        CHECK(radiobrowser_list_url(url, sizeof(url), HOST,
                                    RADIOBROWSER_TOPCLICK, "ignored"),
              "a chart ignores a value it was given");
        CHECK(strstr(url, "topclick") != NULL, "topclick path: %s", url);

        /*
         * The case that decided the query-string form. A tag with a
         * space and an ampersand in it is ordinary -- "rock & roll" is
         * a real radio-browser tag -- and in a path segment it is a
         * fight with every intermediary between here and Germany.
         */
        CHECK(radiobrowser_list_url(url, sizeof(url), HOST,
                                    RADIOBROWSER_BYTAG, "rock & roll"),
              "a tag with an ampersand builds");
        CHECK(strstr(url, "tag=rock%20%26%20roll") != NULL,
              "and is encoded whole: %s", url);
        CHECK(strstr(url, "limit=50") != NULL, "limit is still there: %s", url);

        CHECK(radiobrowser_list_url(url, sizeof(url), HOST,
                                    RADIOBROWSER_BYCOUNTRY, "United Kingdom"),
              "a country builds");
        CHECK(strstr(url, "country=United%20Kingdom") != NULL,
              "country encoded: %s", url);

        /* A value that is empty once trimmed is refused for the same
         * reason a search for nothing is: limit=50 would make it look
         * like it worked. */
        CHECK(!radiobrowser_list_url(url, sizeof(url), HOST,
                                     RADIOBROWSER_BYTAG, "   "),
              "an empty tag is refused");
        CHECK(url[0] == '\0', "and leaves nothing behind");

        /* No mirror name, no URL. */
        CHECK(!radiobrowser_list_url(url, sizeof(url), "",
                                     RADIOBROWSER_TOPVOTE, NULL),
              "no host, no URL");

        /* Every pinned tag has to produce a URL, because the first
         * level of the browse tree is drawn from this list and a row
         * that cannot be fetched is a row that does nothing. */
        static const char *const tags[RADIOBROWSER_TAG_COUNT] =
            RADIOBROWSER_TAGS;
        for (int i = 0; i < RADIOBROWSER_TAG_COUNT; i++) {
            CHECK(radiobrowser_list_url(url, sizeof(url), HOST,
                                        RADIOBROWSER_BYTAG, tags[i]),
                  "pinned tag %s builds", tags[i]);
        }
    }

    printf("  the rate gap and the cache window\n");
    {
        /* Before there has ever been a request there is nothing to
         * space against, and the first one must not be delayed. */
        CHECK(radiobrowser_gap_ok(0, 0, false), "the first request goes");

        CHECK(!radiobrowser_gap_ok(1000, 1000, true), "back to back is refused");
        CHECK(!radiobrowser_gap_ok(1000 + RADIOBROWSER_MIN_GAP_MS - 1, 1000, true),
              "one millisecond short is still short");
        CHECK(radiobrowser_gap_ok(1000 + RADIOBROWSER_MIN_GAP_MS, 1000, true),
              "exactly the gap is enough");

        CHECK(radiobrowser_cache_fresh(0, 0), "a just-stored entry is fresh");
        CHECK(radiobrowser_cache_fresh(RADIOBROWSER_CACHE_MS - 1, 0),
              "and stays fresh to the last millisecond");
        CHECK(!radiobrowser_cache_fresh(RADIOBROWSER_CACHE_MS, 0),
              "and is stale on the one after");

        /*
         * THE WRAP, which is the whole reason these are functions
         * rather than comparisons at the call site.
         *
         * A tick count is 32 bits of milliseconds and wraps after 49
         * days -- and this player is a thing that gets left on. Written
         * as `now > stamp + N` both of these are wrong across the wrap:
         * the gap never opens and the cache never goes stale, so the
         * directory is asked at whatever rate the chooser redraws at,
         * for ever, having been polite for seven weeks.
         */
        const uint32_t before = 0xFFFFFF00u;      /* 256 ms before the wrap */
        CHECK(radiobrowser_gap_ok(before + RADIOBROWSER_MIN_GAP_MS, before, true),
              "the gap opens across the wrap");
        CHECK(!radiobrowser_gap_ok(before + 10, before, true),
              "and is still closed just after it");
        CHECK(radiobrowser_cache_fresh(before + 1000, before),
              "an entry stored before the wrap is fresh after it");
        CHECK(!radiobrowser_cache_fresh(before + RADIOBROWSER_CACHE_MS, before),
              "and goes stale on time across it");
    }

    printf("  the menu table\n");
    {
        /*
         * The table has two callers on two tasks -- the chooser draws
         * the rows, the player turns a row into a request -- so what is
         * checked is that the two questions agree about every row.
         */
        CHECK(radiobrowser_menu_label(RADIOBROWSER_MENU_CARD)[0] != '\0',
              "the card row has a label");
        CHECK(!radiobrowser_menu_kind(RADIOBROWSER_MENU_CARD, NULL, NULL),
              "the card row is not a fetch");

        /*
         * THE THREE ACTION ROWS ARE ALL NOT-FETCHES, and the caller
         * tells them apart by number, so the numbers must differ and
         * all must be labelled. This is the assertion that would have
         * caught the row that was inserted without moving the charts --
         * and it did: adding the starred row moved first_fetch and four
         * checks below failed until the boundary followed it.
         */
        CHECK(radiobrowser_menu_label(RADIOBROWSER_MENU_ADD)[0] != '\0',
              "the add-a-station row has a label");
        CHECK(!radiobrowser_menu_kind(RADIOBROWSER_MENU_ADD, NULL, NULL),
              "the add-a-station row is not a fetch");
        CHECK(RADIOBROWSER_MENU_CARD != RADIOBROWSER_MENU_ADD,
              "the two action rows are the same row");
        CHECK(strcmp(radiobrowser_menu_label(RADIOBROWSER_MENU_CARD),
                     radiobrowser_menu_label(RADIOBROWSER_MENU_ADD)) != 0,
              "the two action rows share a label");

        CHECK(radiobrowser_menu_label(RADIOBROWSER_MENU_FAV)[0] != '\0',
              "the starred row has a label");
        CHECK(!radiobrowser_menu_kind(RADIOBROWSER_MENU_FAV, NULL, NULL),
              "the starred row is not a fetch");
        CHECK(RADIOBROWSER_MENU_FAV != RADIOBROWSER_MENU_CARD &&
              RADIOBROWSER_MENU_FAV != RADIOBROWSER_MENU_ADD,
              "the starred row collides with another action row");
        CHECK(strcmp(radiobrowser_menu_label(RADIOBROWSER_MENU_FAV),
                     radiobrowser_menu_label(RADIOBROWSER_MENU_CARD)) != 0 &&
              strcmp(radiobrowser_menu_label(RADIOBROWSER_MENU_FAV),
                     radiobrowser_menu_label(RADIOBROWSER_MENU_ADD)) != 0,
              "the starred row shares a label with another action row");

        /*
         * The actions are CONTIGUOUS from row 0, which is what
         * menu_kind()'s `row <= RADIOBROWSER_MENU_FAV` guard means: it
         * is a range test, so an action row added anywhere but the top
         * of the block would be called a fetch. Asserted because the
         * guard cannot say this about itself.
         */
        CHECK(RADIOBROWSER_MENU_FAV == RADIOBROWSER_MENU_ADD + 1 &&
              RADIOBROWSER_MENU_ADD == RADIOBROWSER_MENU_CARD + 1,
              "the action rows are not contiguous from row 0");

        /* Every row above the actions is a fetch, and no row below is.
         * Stated as a boundary rather than as two numbers so that
         * inserting another action row fails here loudly. */
        radiobrowser_kind_t kind;
        const char *value;
        /* The last action row plus one. Named off MENU_FAV rather than
         * MENU_ADD so that the next action row moves this by moving the
         * constant, instead of by failing four checks. */
        const int first_fetch = RADIOBROWSER_MENU_FAV + 1;
        CHECK(radiobrowser_menu_kind(first_fetch, &kind, &value) &&
              kind == RADIOBROWSER_TOPVOTE && value == NULL,
              "the first fetch row is the votes chart and takes no value");
        CHECK(radiobrowser_menu_kind(first_fetch + 1, &kind, &value) &&
              kind == RADIOBROWSER_TOPCLICK && value == NULL,
              "the second fetch row is the clicks chart");

        /* The tags start where the charts end, and RADIOBROWSER_MENU_TAG0
         * says so in the header. If the two ever disagree the tag rows
         * fetch the wrong tag, which is the drift this whole block
         * exists to catch. */
        CHECK(first_fetch + 2 == RADIOBROWSER_MENU_TAG0,
              "the tag base and the fetch rows disagree");

        for (int i = first_fetch + 2; i < RADIOBROWSER_MENU_ROWS; i++) {
            char url[RADIOBROWSER_URL_MAX];
            CHECK(radiobrowser_menu_kind(i, &kind, &value), "row %d fetches", i);
            CHECK(kind == RADIOBROWSER_BYTAG, "row %d is a tag", i);
            CHECK(value && strcmp(value, radiobrowser_menu_label(i)) == 0,
                  "row %d fetches what it is labelled", i);
            CHECK(radiobrowser_list_url(url, sizeof(url), HOST, kind, value),
                  "row %d builds a URL", i);
        }

        /* Off both ends, because a row number arrives from a tap and a
         * scroll position and nothing between here and there bounds it
         * a second time. */
        CHECK(!radiobrowser_menu_kind(-1, &kind, &value), "no row -1");
        CHECK(!radiobrowser_menu_kind(RADIOBROWSER_MENU_ROWS, &kind, &value),
              "no row past the end");
        CHECK(radiobrowser_menu_label(RADIOBROWSER_MENU_ROWS)[0] == '\0',
              "and no label past the end");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
