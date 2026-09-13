/*
 * jsonpicktest.c -- the one-field JSON reader, against the shapes
 * radio-browser actually sends.
 *
 * The interesting cases are all about NOT matching: a field name that
 * appears inside somebody else's value, a null where a string was
 * expected, a value too long for the buffer. Every one of those failure
 * modes ends with this player fetching a URL nobody chose, which is why
 * there are more refusal cases below than success ones.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "jsonpick.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                           \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL (%s:%d): ", __FILE__, __LINE__);                 \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
        failures++;                                                     \
    }                                                                   \
} while (0)

#define PICK(json, key) \
    jsonpick_string((json), strlen(json), (key), out, sizeof(out))

int main(void)
{
    char out[256];

    printf("jsonpicktest\n");

    printf("  the ordinary case\n");
    {
        /* Trimmed from a real /json/stations/byuuid response: a
         * one-element array holding one flat object. */
        static const char *doc =
            "[{\"changeuuid\":\"9608f9a1-0601-11e8-ae97-52543be04c81\","
            "\"stationuuid\":\"9608f9a4-0601-11e8-ae97-52543be04c81\","
            "\"name\":\"WNZK-AM\","
            "\"url\":\"https://stream.zeno.fm/erunhwj5lekvv\","
            "\"favicon\":\"https://www.wnzk.com/logo.png\","
            "\"tags\":\"talk,ethnic\",\"votes\":42,\"bitrate\":0}]";

        CHECK(PICK(doc, "favicon"), "favicon is found");
        CHECK(strcmp(out, "https://www.wnzk.com/logo.png") == 0,
              "favicon value: %s", out);

        CHECK(PICK(doc, "name") && strcmp(out, "WNZK-AM") == 0,
              "name too: %s", out);
        CHECK(PICK(doc, "stationuuid"), "and the uuid");
        CHECK(strcmp(out, "9608f9a4-0601-11e8-ae97-52543be04c81") == 0,
              "uuid value: %s", out);

        /* A number is not a string and must not be handed back as one. */
        CHECK(!PICK(doc, "votes"), "a number is not a string");
        CHECK(out[0] == '\0', "and leaves nothing behind");
        CHECK(!PICK(doc, "absent"), "an absent key is absent");
    }

    printf("  a key name hiding inside a value\n");
    {
        /*
         * THE CASE A strstr() WOULD GET WRONG, and not a contrived one:
         * radio-browser's tags and homepage fields carry arbitrary user
         * text, and "favicon" is an ordinary enough word to appear in
         * it. Matching there would make one station's tags into another
         * station's artwork URL.
         */
        static const char *doc =
            "[{\"tags\":\"we have no favicon: really\","
            "\"favicon\":\"https://example.com/real.png\"}]";

        CHECK(PICK(doc, "favicon"), "the real key is found");
        CHECK(strcmp(out, "https://example.com/real.png") == 0,
              "and it is the real one: %s", out);
    }
    {
        /* The same, with the decoy AFTER the real field, so a scanner
         * that stops at the first plausible thing is not simply lucky
         * about ordering. */
        static const char *doc =
            "[{\"favicon\":\"https://example.com/real.png\","
            "\"homepage\":\"http://x/?q=favicon:fake\"}]";
        CHECK(PICK(doc, "favicon") &&
              strcmp(out, "https://example.com/real.png") == 0,
              "ordering does not matter: %s", out);
    }

    printf("  null, empty and missing\n");
    {
        /* Every one of these is common in the directory -- most
         * stations have no artwork at all -- and every one must come
         * back as "no URL" rather than as a URL. */
        CHECK(!PICK("[{\"favicon\":null}]", "favicon"), "null is not a string");
        CHECK(out[0] == '\0', "null leaves nothing");

        CHECK(PICK("[{\"favicon\":\"\"}]", "favicon"), "empty is a string");
        CHECK(out[0] == '\0', "and it is empty, which the caller must test");

        CHECK(!PICK("[{\"favicon\":[1,2]}]", "favicon"), "an array is refused");
        CHECK(!PICK("[{\"favicon\":{\"a\":\"b\"}}]", "favicon"),
              "an object is refused");
        CHECK(!PICK("", "favicon"), "an empty document");
        CHECK(!PICK("not json at all", "favicon"), "and one that is not JSON");
    }

    printf("  escapes\n");
    {
        CHECK(PICK("[{\"n\":\"a\\\"b\"}]", "n") && strcmp(out, "a\"b") == 0,
              "an escaped quote: %s", out);
        CHECK(PICK("[{\"n\":\"a\\\\b\"}]", "n") && strcmp(out, "a\\b") == 0,
              "an escaped backslash: %s", out);
        CHECK(PICK("[{\"n\":\"http:\\/\\/x\"}]", "n") &&
              strcmp(out, "http://x") == 0,
              "escaped slashes, which JSON encoders emit: %s", out);
        CHECK(PICK("[{\"n\":\"a\\u00e9b\"}]", "n") &&
              strcmp(out, "a\\u00e9b") == 0,
              "\\u is passed through whole: %s", out);

        /*
         * An escaped quote inside a value must not end the value, or
         * everything after it is read as structure -- which is how a
         * scanner finds a "key" that is really the second half of
         * somebody's station name.
         */
        static const char *doc =
            "[{\"name\":\"The \\\"favicon\\\": \\\"gotcha\\\"\","
            "\"favicon\":\"https://example.com/ok.png\"}]";
        CHECK(PICK(doc, "favicon") &&
              strcmp(out, "https://example.com/ok.png") == 0,
              "an escaped quote does not open a fake key: %s", out);
    }

    printf("  malformed, which the network can always send\n");
    {
        CHECK(!PICK("[{\"favicon\":\"unterminated", "favicon"),
              "a value with no closing quote");
        CHECK(out[0] == '\0', "leaves nothing");
        CHECK(!PICK("[{\"unterminated", "favicon"), "a key with no closing quote");
        CHECK(!PICK("[{\"favicon\"", "favicon"), "a key with no value");
        CHECK(!PICK("[{\"favicon\":", "favicon"), "a colon with no value");
    }

    printf("  truncation is refused, not performed\n");
    {
        char small[10];
        const char *doc = "[{\"favicon\":\"https://example.com/long.png\"}]";
        const bool ok = jsonpick_string(doc, strlen(doc), "favicon",
                                        small, sizeof(small));
        /* A truncated URL is a DIFFERENT URL. Fetching it would be a
         * request to somewhere nobody chose, so this refuses rather
         * than shortens. */
        CHECK(!ok, "a value too long is refused");
        CHECK(small[0] == '\0', "and nothing partial is left");
    }

    printf("  not NUL-terminated\n");
    {
        /* The fetch path hands over a length and a buffer; nothing
         * promises a terminator, and reading past the length would be
         * reading whatever the last response left behind. */
        char buf[64];
        memset(buf, 'x', sizeof(buf));
        memcpy(buf, "[{\"favicon\":\"ab\"}]", 18);
        CHECK(jsonpick_string(buf, 18, "favicon", out, sizeof(out)) &&
              strcmp(out, "ab") == 0,
              "the length bounds the scan: %s", out);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
