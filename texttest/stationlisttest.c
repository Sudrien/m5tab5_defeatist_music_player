/*
 * stationlisttest.c -- stations.m3u, parsed, against the files people
 * actually have.
 *
 * Every other file in the network series reads bytes from a server.
 * This one reads bytes from a person with a text editor, which is a
 * wider input and a worse one, so the cases below are mostly the messy
 * ones: a BOM, CRLF, a pasted bare URL, a name with a comma in it,
 * directives from a player that rewrote the file, and a line somebody
 * was halfway through editing when they saved.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static station_t st[STATIONLIST_MAX];
static stationlist_stats_t stats;

static int parse(const char *text)
{
    memset(st, 0, sizeof(st));
    return stationlist_parse(text, strlen(text), st, STATIONLIST_MAX, &stats);
}

int main(void)
{
    printf("stationlisttest\n");

    /* ---------------------------------------------------------------- */
    /* The clean case                                                    */
    /* ---------------------------------------------------------------- */
    {
        const int n = parse("#EXTM3U\n"
                            "#EXTINF:-1,WNZK-AM\n"
                            "https://stream.zeno.fm/erunhwj5lekvv\n");
        CHECK(n == 1, "clean file gave %d stations", n);
        CHECK(strcmp(st[0].name, "WNZK-AM") == 0, "name \"%s\"", st[0].name);
        CHECK(strcmp(st[0].url, "https://stream.zeno.fm/erunhwj5lekvv") == 0,
              "url \"%s\"", st[0].url);
        CHECK(stats.bad_scheme == 0 && stats.too_long == 0,
              "clean file reported %d bad, %d long", stats.bad_scheme, stats.too_long);
    }

    /* ---------------------------------------------------------------- */
    /* What files actually arrive as                                     */
    /* ---------------------------------------------------------------- */

    /* A UTF-8 BOM from Notepad, and CRLF. Without the BOM skip the first
     * line is neither a directive nor a URL and the first station is
     * silently lost. */
    {
        const int n = parse("\xEF\xBB\xBF#EXTM3U\r\n"
                            "#EXTINF:-1,BBC 6 Music\r\n"
                            "http://stream.example/6music\r\n");
        CHECK(n == 1, "BOM + CRLF gave %d stations", n);
        CHECK(strcmp(st[0].name, "BBC 6 Music") == 0,
              "BOM/CRLF name \"%s\" -- a stray \\r would show on screen", st[0].name);
        CHECK(strcmp(st[0].url, "http://stream.example/6music") == 0,
              "BOM/CRLF url \"%s\"", st[0].url);
    }

    /* A bare URL, which is what pasting gives. Labelled with the host,
     * because a row with no label cannot be chosen from. */
    {
        const int n = parse("https://ice.example.org:8000/stream.aac\n");
        CHECK(n == 1, "bare URL gave %d stations", n);
        CHECK(strcmp(st[0].name, "ice.example.org:8000") == 0,
              "bare URL labelled \"%s\"", st[0].name);
    }

    /* A name with a comma is legal and the name is everything after the
     * FIRST comma. */
    {
        const int n = parse("#EXTINF:-1,Jazz, all night\n"
                            "http://a.example/j\n");
        CHECK(n == 1, "comma name gave %d stations", n);
        CHECK(strcmp(st[0].name, "Jazz, all night") == 0,
              "comma name became \"%s\"", st[0].name);
    }

    /* A space after the comma, which half the exporters emit. */
    {
        parse("#EXTINF:-1, Spaced\nhttp://a.example/s\n");
        CHECK(strcmp(st[0].name, "Spaced") == 0, "spaced name \"%s\"", st[0].name);
    }

    /* Blank lines, indentation, and directives from a player that
     * rewrote the file. None of them stops it working. */
    {
        const int n = parse("#EXTM3U\n"
                            "\n"
                            "#EXT-X-VERSION:3\n"
                            "   \n"
                            "#EXTINF:-1,One\n"
                            "  http://a.example/1  \n"
                            "\n"
                            "#PLAYLIST:whatever\n"
                            "#EXTINF:-1,Two\n"
                            "http://a.example/2\n");
        CHECK(n == 2, "messy-but-valid file gave %d stations", n);
        CHECK(strcmp(st[0].name, "One") == 0 && strcmp(st[1].name, "Two") == 0,
              "names \"%s\" and \"%s\"", st[0].name, st[1].name);
        CHECK(strcmp(st[0].url, "http://a.example/1") == 0,
              "indented url kept whitespace: \"%s\"", st[0].url);
    }

    /* An #EXTINF whose station line never came, followed by another
     * station: the orphaned name must not attach to the wrong URL. This
     * is the one that would be wrong in a way nobody notices -- every
     * station showing the previous station's name. */
    {
        const int n = parse("#EXTINF:-1,Orphan\n"
                            "#EXTINF:-1,Real\n"
                            "http://a.example/r\n");
        CHECK(n == 1, "orphaned EXTINF gave %d stations", n);
        CHECK(strcmp(st[0].name, "Real") == 0,
              "the second EXTINF did not win: \"%s\"", st[0].name);
    }
    {
        const int n = parse("#EXTINF:-1,Named\n"
                            "file:///sd/nope.mp3\n"
                            "http://a.example/bare\n");
        CHECK(n == 1, "rejected line gave %d stations", n);
        CHECK(strcmp(st[0].name, "a.example") == 0,
              "a rejected URL's name leaked onto the next station: \"%s\"",
              st[0].name);
    }

    /* ---------------------------------------------------------------- */
    /* Schemes: two are allowed and nothing else is                      */
    /* ---------------------------------------------------------------- */
    CHECK(station_url_ok("http://a.example/x"), "http refused");
    CHECK(station_url_ok("https://a.example/x"), "https refused");
    CHECK(station_url_ok("HTTPS://a.example/x"), "uppercase scheme refused");
    CHECK(station_url_ok("HtTp://a.example/x"), "mixed-case scheme refused");
    CHECK(!station_url_ok("file:///sd/Music/x.mp3"),
          "file:// accepted -- a stream player walking the card");
    CHECK(!station_url_ok("javascript:alert(1)"), "javascript: accepted");
    CHECK(!station_url_ok("data:audio/aac;base64,AAAA"), "data: accepted");
    CHECK(!station_url_ok("ftp://a.example/x"), "ftp accepted");
    CHECK(!station_url_ok("rtsp://a.example/x"), "rtsp accepted");
    CHECK(!station_url_ok("C:\\Music\\x.mp3"), "a Windows path accepted");
    CHECK(!station_url_ok("//a.example/x"), "a scheme-relative URL accepted");
    CHECK(!station_url_ok("a.example/x"), "a bare host accepted");
    CHECK(!station_url_ok("http://"), "a scheme with no host accepted");
    CHECK(!station_url_ok("https://"), "https with no host accepted");
    CHECK(!station_url_ok("http:/a.example"), "one slash accepted");
    CHECK(!station_url_ok(""), "the empty string accepted");
    CHECK(!station_url_ok(NULL), "NULL accepted");
    /* A scheme longer than the buffer must not run off the end. */
    CHECK(!station_url_ok("averylongschemename://a.example/x"),
          "an overlong scheme accepted");

    {
        const int n = parse("#EXTINF:-1,Bad\nfile:///sd/x.mp3\n"
                            "#EXTINF:-1,Worse\njavascript:alert(1)\n"
                            "#EXTINF:-1,Good\nhttps://a.example/g\n");
        CHECK(n == 1, "two bad schemes gave %d stations", n);
        CHECK(stats.bad_scheme == 2, "%d bad schemes counted, wanted 2",
              stats.bad_scheme);
        CHECK(strcmp(st[0].name, "Good") == 0, "survivor is \"%s\"", st[0].name);
    }

    /* ---------------------------------------------------------------- */
    /* Hosts                                                             */
    /* ---------------------------------------------------------------- */
    {
        char h[64];
        station_host("https://a.b.example/path?q=1", h, sizeof(h));
        CHECK(strcmp(h, "a.b.example") == 0, "host \"%s\"", h);
        station_host("http://a.example:8000", h, sizeof(h));
        CHECK(strcmp(h, "a.example:8000") == 0, "host with port \"%s\"", h);
        station_host("http://a.example?q", h, sizeof(h));
        CHECK(strcmp(h, "a.example") == 0, "host before query \"%s\"", h);
        station_host("http://a.example#f", h, sizeof(h));
        CHECK(strcmp(h, "a.example") == 0, "host before fragment \"%s\"", h);
        station_host("nonsense", h, sizeof(h));
        CHECK(h[0] == '\0', "host out of nonsense: \"%s\"", h);
        station_host(NULL, h, sizeof(h));
        CHECK(h[0] == '\0', "host out of NULL: \"%s\"", h);
        /* A host longer than the label buffer truncates, terminated. */
        char small[8];
        station_host("http://averylonghostname.example/x", small, sizeof(small));
        CHECK(strlen(small) == 7, "truncated host is %zu chars", strlen(small));
    }

    /* ---------------------------------------------------------------- */
    /* Limits                                                            */
    /* ---------------------------------------------------------------- */

    /* Overlong URLs are refused rather than truncated: a truncated URL
     * is a station that connects to the wrong thing. */
    {
        char big[STATION_URL_MAX + 200];
        strcpy(big, "https://a.example/");
        memset(big + 18, 'q', STATION_URL_MAX + 100);
        strcpy(big + 18 + STATION_URL_MAX + 100, "\n");
        const int n = parse(big);
        CHECK(n == 0, "an overlong URL became %d stations", n);
        CHECK(stats.too_long == 1, "%d overlong lines counted", stats.too_long);
    }

    /* More stations than the cap: the first STATIONLIST_MAX are kept,
     * the rest counted, and nothing is written past the array. */
    {
        char *text = malloc(STATIONLIST_MAX * 80 + 4096);
        text[0] = '\0';
        strcat(text, "#EXTM3U\n");
        for (int i = 0; i < STATIONLIST_MAX + 10; i++) {
            char line[80];
            snprintf(line, sizeof(line), "#EXTINF:-1,S%d\nhttp://a.example/%d\n", i, i);
            strcat(text, line);
        }
        const int n = parse(text);
        CHECK(n == STATIONLIST_MAX, "cap gave %d, wanted %d", n, STATIONLIST_MAX);
        CHECK(stats.overflowed == 10, "%d overflowed, wanted 10", stats.overflowed);
        CHECK(strcmp(st[0].name, "S0") == 0, "first is \"%s\"", st[0].name);
        CHECK(strcmp(st[STATIONLIST_MAX - 1].name, "S63") == 0,
              "last is \"%s\"", st[STATIONLIST_MAX - 1].name);
        free(text);
    }

    /* A very long name truncates into the field, terminated. */
    {
        char text[600];
        strcpy(text, "#EXTINF:-1,");
        memset(text + 11, 'N', 400);
        strcpy(text + 411, "\nhttp://a.example/x\n");
        const int n = parse(text);
        CHECK(n == 1, "long name gave %d stations", n);
        CHECK(strlen(st[0].name) == STATION_NAME_MAX - 1,
              "long name is %zu chars", strlen(st[0].name));
    }

    /* ---------------------------------------------------------------- */
    /* Degenerate input                                                  */
    /* ---------------------------------------------------------------- */
    CHECK(parse("") == 0, "empty file gave stations");
    CHECK(parse("\n\n\r\n\r\n") == 0, "a file of newlines gave stations");
    CHECK(parse("#EXTM3U\n") == 0, "a header-only file gave stations");
    CHECK(stationlist_parse(NULL, 0, st, STATIONLIST_MAX, &stats) == 0,
          "NULL text gave stations");
    CHECK(stationlist_parse("http://a.example/x\n", 19, NULL, 4, &stats) == 0,
          "NULL out gave stations");
    CHECK(stationlist_parse("http://a.example/x\n", 19, st, 0, &stats) == 0,
          "max 0 gave stations");
    /* stats may be NULL. */
    CHECK(stationlist_parse("http://a.example/x\n", 19, st, 4, NULL) == 1,
          "NULL stats refused a valid file");

    /* No trailing newline -- the last line still counts. A file edited
     * on a phone very often has none. */
    {
        const int n = parse("#EXTINF:-1,Last\nhttp://a.example/last");
        CHECK(n == 1, "no trailing newline gave %d stations", n);
        CHECK(strcmp(st[0].name, "Last") == 0, "name \"%s\"", st[0].name);
    }

    /* `n` is honoured, not strlen: a buffer read off the card in a fixed
     * block is not terminated where the file ends. */
    {
        const char text[] = "http://a.example/1\nhttp://a.example/2\n";
        memset(st, 0, sizeof(st));
        const int n = stationlist_parse(text, 19, st, STATIONLIST_MAX, &stats);
        CHECK(n == 1, "a length-limited parse read past n: %d stations", n);
        CHECK(strcmp(st[0].url, "http://a.example/1") == 0, "url \"%s\"", st[0].url);
    }

    /* Binary, which is what a card gives when the file is not the file
     * anyone thought it was. It must not crash and must not invent
     * stations. */
    {
        char junk[1024];
        srand(20260911);
        for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (char)(rand() % 256);
        memset(st, 0, sizeof(st));
        const int n = stationlist_parse(junk, sizeof(junk), st, STATIONLIST_MAX, &stats);
        CHECK(n >= 0 && n <= STATIONLIST_MAX, "binary gave %d stations", n);
        for (int i = 0; i < n; i++) {
            CHECK(station_url_ok(st[i].url),
                  "binary produced a station with url \"%s\"", st[i].url);
        }
    }

    /* Random line soup: no crash, and everything produced is a URL that
     * passes the scheme check. Under ASan, which is the point. */
    {
        static const char *frag[] = {
            "#EXTM3U", "#EXTINF:-1,Name", "#EXTINF:-1,", "#EXTINF:", "#EXT-X-A:1",
            "http://a.example/x", "https://b.example:8000/y", "file:///x",
            "", "   ", "\t", "javascript:x", "#", "#EXTINF:-1,a,b,c",
            "https://", "http://c", "not a url at all", "\xEF\xBB\xBF",
        };
        const int nfrag = (int)(sizeof(frag) / sizeof(frag[0]));
        for (int iter = 0; iter < 3000; iter++) {
            char text[4096];
            text[0] = '\0';
            const int lines = rand() % 40;
            for (int l = 0; l < lines; l++) {
                strcat(text, frag[rand() % nfrag]);
                strcat(text, (rand() % 4) ? "\n" : "\r\n");
            }
            memset(st, 0, sizeof(st));
            const int n = stationlist_parse(text, strlen(text), st,
                                            STATIONLIST_MAX, &stats);
            if (n < 0 || n > STATIONLIST_MAX) {
                CHECK(0, "iter %d gave %d stations", iter, n);
                break;
            }
            int bad = 0;
            for (int i = 0; i < n; i++) {
                if (!station_url_ok(st[i].url)) bad++;
                if (!st[i].name[0]) bad++;
            }
            if (bad) {
                CHECK(0, "iter %d produced %d unusable stations", iter, bad);
                break;
            }
            checks++;
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
