/*
 * codecplantest.c -- which decoder the bytes go to, and what happens
 * when the header disagrees.
 *
 * The rule under test is that the bytes win. It matters because the
 * failure mode of getting it wrong is not an error: handing AAC to
 * minimp3 produces noise, and noise from a live stream with no seek bar
 * is very hard to tell from a bad connection. A station with a wrong
 * Content-Type would present as "the internet radio feature is flaky".
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codecplan.h"

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

/* Plenty of bytes, so "not enough yet" is never the reason. */
#define PLENTY  (2048)

static codecplan_t choose(sniff_t s, const char *ct)
{
    return codecplan_choose(s, PLENTY, ct);
}

int main(void)
{
    printf("codecplantest\n");

    /* ---------------------------------------------------------------- */
    /* The bytes win, which is the entire point                          */
    /* ---------------------------------------------------------------- */
    {
        /* The routine real-world case: an AAC station announcing
         * audio/mpeg because someone set it up for MP3 years ago. */
        codecplan_t r = choose(SNIFF_AAC_ADTS, "audio/mpeg");
        CHECK(r.codec == STREAM_CODEC_AAC_ADTS,
              "AAC bytes with an audio/mpeg header went to %s -- minimp3 "
              "would produce noise, not an error", stream_codec_name(r.codec));
        CHECK(r.why == CODECPLAN_FROM_BYTES, "why is %d", (int)r.why);

        r = choose(SNIFF_MP3, "audio/aac");
        CHECK(r.codec == STREAM_CODEC_MP3,
              "MP3 bytes with an audio/aac header went to %s",
              stream_codec_name(r.codec));

        /* And with no header at all, which is common. */
        r = choose(SNIFF_AAC_ADTS, NULL);
        CHECK(r.codec == STREAM_CODEC_AAC_ADTS, "AAC bytes with no header: %s",
              stream_codec_name(r.codec));
        r = choose(SNIFF_MP3, "application/octet-stream");
        CHECK(r.codec == STREAM_CODEC_MP3, "MP3 bytes with octet-stream: %s",
              stream_codec_name(r.codec));

        /* WNZK, as the probe actually found it: both agreed. */
        r = choose(SNIFF_AAC_ADTS, "audio/aac");
        CHECK(r.codec == STREAM_CODEC_AAC_ADTS && r.why == CODECPLAN_FROM_BYTES,
              "the WNZK case did not resolve to AAC from bytes");
    }

    /* ---------------------------------------------------------------- */
    /* The header is consulted only when the bytes say nothing           */
    /* ---------------------------------------------------------------- */
    {
        codecplan_t r = choose(SNIFF_UNKNOWN, "audio/mpeg");
        CHECK(r.codec == STREAM_CODEC_MP3 && r.why == CODECPLAN_FROM_TYPE,
              "unknown bytes + audio/mpeg gave %s (why %d)",
              stream_codec_name(r.codec), (int)r.why);

        r = choose(SNIFF_UNKNOWN, "audio/aacp");
        CHECK(r.codec == STREAM_CODEC_AAC_ADTS,
              "audio/aacp (HE-AAC, arrives as ADTS) gave %s",
              stream_codec_name(r.codec));

        r = choose(SNIFF_UNKNOWN, "application/octet-stream");
        CHECK(r.codec == STREAM_CODEC_NONE && r.why == CODECPLAN_UNKNOWN,
              "octet-stream with unknown bytes gave %s",
              stream_codec_name(r.codec));

        r = choose(SNIFF_UNKNOWN, NULL);
        CHECK(r.codec == STREAM_CODEC_NONE, "nothing at all gave %s",
              stream_codec_name(r.codec));
    }

    /* Content-Type parsing: parameters ignored, case-insensitive. */
    CHECK(codecplan_from_type("audio/mpeg") == STREAM_CODEC_MP3, "audio/mpeg");
    CHECK(codecplan_from_type("AUDIO/MPEG") == STREAM_CODEC_MP3, "uppercase");
    CHECK(codecplan_from_type("audio/mpeg; charset=utf-8") == STREAM_CODEC_MP3,
          "parameters not ignored");
    CHECK(codecplan_from_type("  audio/mpeg") == STREAM_CODEC_MP3,
          "leading space");
    CHECK(codecplan_from_type("audio/aac") == STREAM_CODEC_AAC_ADTS, "audio/aac");
    CHECK(codecplan_from_type("audio/x-aac") == STREAM_CODEC_AAC_ADTS, "x-aac");
    CHECK(codecplan_from_type("audio/ogg") == STREAM_CODEC_NONE, "ogg mapped");
    CHECK(codecplan_from_type("text/html") == STREAM_CODEC_NONE, "html mapped");
    CHECK(codecplan_from_type("") == STREAM_CODEC_NONE, "empty mapped");
    CHECK(codecplan_from_type(NULL) == STREAM_CODEC_NONE, "NULL mapped");
    /* A header longer than the parse buffer must not run off the end. */
    {
        char big[512];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK(codecplan_from_type(big) == STREAM_CODEC_NONE, "overlong header");
        CHECK(!codecplan_type_is_playlist(big), "overlong header as playlist");
    }

    /* ---------------------------------------------------------------- */
    /* Refused by name, because "failed" for all of these is the version  */
    /* that generates a bug report nobody can act on                     */
    /* ---------------------------------------------------------------- */
    {
        codecplan_t r = choose(SNIFF_M3U, NULL);
        CHECK(r.why == CODECPLAN_IS_PLAYLIST, "m3u bytes gave why %d", (int)r.why);
        CHECK(!codecplan_ready(&r), "a playlist was reported as ready to decode");

        r = choose(SNIFF_PLS, NULL);
        CHECK(r.why == CODECPLAN_IS_PLAYLIST, "pls bytes gave why %d", (int)r.why);

        r = choose(SNIFF_HTML, "text/html");
        CHECK(r.why == CODECPLAN_IS_PAGE, "html gave why %d", (int)r.why);

        r = choose(SNIFF_OGG, NULL);
        CHECK(r.why == CODECPLAN_UNSUPPORTED, "ogg gave why %d", (int)r.why);
        CHECK(strstr(r.message, "Ogg") != NULL,
              "the Ogg message does not name Ogg: \"%s\"", r.message);

        r = choose(SNIFF_FLAC, NULL);
        CHECK(r.why == CODECPLAN_UNSUPPORTED, "flac gave why %d", (int)r.why);

        /* A playlist Content-Type is believed even with unremarkable
         * bytes: a .pls served as text may open with a comment or a BOM
         * the sniffer will not recognise. */
        r = choose(SNIFF_UNKNOWN, "audio/x-scpls");
        CHECK(r.why == CODECPLAN_IS_PLAYLIST, "x-scpls gave why %d", (int)r.why);
        r = choose(SNIFF_UNKNOWN, "application/vnd.apple.mpegurl");
        CHECK(r.why == CODECPLAN_IS_PLAYLIST, "HLS type gave why %d", (int)r.why);
        r = choose(SNIFF_UNKNOWN, "AUDIO/X-MPEGURL; charset=utf-8");
        CHECK(r.why == CODECPLAN_IS_PLAYLIST, "mpegurl with params/case gave %d",
              (int)r.why);

        /* But actual audio bytes beat a playlist header, same rule. */
        r = choose(SNIFF_MP3, "audio/x-mpegurl");
        CHECK(r.codec == STREAM_CODEC_MP3,
              "MP3 bytes lost to a playlist Content-Type");
    }

    /* Every refusal carries a message, and no message is empty. */
    {
        const sniff_t all[] = {
            SNIFF_UNKNOWN, SNIFF_MP3, SNIFF_AAC_ADTS, SNIFF_ID3,
            SNIFF_OGG, SNIFF_FLAC, SNIFF_M3U, SNIFF_PLS, SNIFF_HTML,
        };
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            codecplan_t r = choose(all[i], NULL);
            CHECK(r.message && r.message[0],
                  "sniff %d has no message", (int)all[i]);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Not enough bytes is not the same as not audio                     */
    /* ---------------------------------------------------------------- */
    {
        codecplan_t r = codecplan_choose(SNIFF_UNKNOWN, 4, NULL);
        CHECK(r.why == CODECPLAN_NEED_MORE,
              "four bytes reported as unrecognised rather than as not enough");
        CHECK(codecplan_waiting(&r), "NEED_MORE is not reported as waiting");
        CHECK(!codecplan_ready(&r), "NEED_MORE reported as ready");

        r = codecplan_choose(SNIFF_UNKNOWN, 0, "audio/mpeg");
        CHECK(r.why == CODECPLAN_NEED_MORE,
              "zero bytes consulted the header instead of waiting");

        /* At the threshold it stops waiting and answers. */
        r = codecplan_choose(SNIFF_UNKNOWN, CODECPLAN_MIN_SNIFF_BYTES, "audio/mpeg");
        CHECK(r.why == CODECPLAN_FROM_TYPE, "at the threshold: why %d", (int)r.why);

        /* Recognised bytes are acted on immediately even if there are
         * few of them -- the sniffer already confirmed a frame. */
        r = codecplan_choose(SNIFF_MP3, 8, NULL);
        CHECK(r.codec == STREAM_CODEC_MP3,
              "recognised MP3 bytes were held back for being few");
    }

    /* ---------------------------------------------------------------- */
    /* ID3 is a wrapper with a length, not an error                      */
    /* ---------------------------------------------------------------- */
    {
        codecplan_t r = choose(SNIFF_ID3, "audio/mpeg");
        CHECK(r.why == CODECPLAN_NEED_SKIP,
              "an ID3 tag was treated as a codec answer (why %d)", (int)r.why);
        CHECK(codecplan_waiting(&r), "NEED_SKIP is not reported as waiting");
        CHECK(!codecplan_ready(&r), "NEED_SKIP reported as ready");
    }
    {
        /* Syncsafe: seven bits per byte. Read as big-endian this gives
         * the wrong length, silently, and lands mid-frame. */
        uint8_t tag[10] = { 'I','D','3', 3,0, 0, 0,0,2,1 };
        /* 0,0,2,1 syncsafe = (2<<7)|1 = 257. Big-endian would be 513. */
        CHECK(id3_skip_bytes(tag, sizeof(tag)) == 10 + 257,
              "syncsafe size read as %zu, wanted %d",
              id3_skip_bytes(tag, sizeof(tag)), 10 + 257);

        /* The footer flag adds ten. */
        uint8_t footed[10] = { 'I','D','3', 4,0, 0x10, 0,0,2,1 };
        CHECK(id3_skip_bytes(footed, sizeof(footed)) == 10 + 257 + 10,
              "footer not counted: %zu", id3_skip_bytes(footed, sizeof(footed)));

        /* A high bit set in a size byte is not a syncsafe integer. */
        uint8_t bad[10] = { 'I','D','3', 3,0, 0, 0,0,0x80,1 };
        CHECK(id3_skip_bytes(bad, sizeof(bad)) == 0,
              "a malformed size was skipped anyway: %zu",
              id3_skip_bytes(bad, sizeof(bad)));

        /* Not a tag, and too short to tell. */
        uint8_t notag[10] = { 0xFF,0xFB,0,0,0,0,0,0,0,0 };
        CHECK(id3_skip_bytes(notag, sizeof(notag)) == 0, "MP3 frame read as a tag");
        CHECK(id3_skip_bytes(tag, 5) == 0, "a truncated header was trusted");
        CHECK(id3_skip_bytes(NULL, 10) == 0, "NULL read as a tag");

        /* The largest legal tag does not overflow the size computation. */
        uint8_t max[10] = { 'I','D','3', 4,0, 0, 0x7F,0x7F,0x7F,0x7F };
        const size_t want = 10 + (((size_t)0x7F << 21) | ((size_t)0x7F << 14) |
                                  ((size_t)0x7F << 7) | 0x7F);
        CHECK(id3_skip_bytes(max, sizeof(max)) == want,
              "maximum tag size: %zu, wanted %zu",
              id3_skip_bytes(max, sizeof(max)), want);
    }

    /* An ID3 tag in front of real MP3: skip, re-sniff, decode. This is
     * the sequence phase 2 will run, asserted end to end. */
    {
        uint8_t buf[300];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, "ID3", 3);
        buf[3] = 3; buf[4] = 0; buf[5] = 0;
        buf[6] = 0; buf[7] = 0; buf[8] = 1; buf[9] = 0;   /* 128 bytes */
        const size_t skip = 10 + 128;
        /* Two MPEG1 Layer III 128 kbit/s 44.1 kHz frames (417 bytes
         * apart), enough for the sniffer to confirm the first by finding
         * the second... only the header of the second is needed. */
        buf[skip + 0] = 0xFF; buf[skip + 1] = 0xFB;
        buf[skip + 2] = 0x90; buf[skip + 3] = 0x00;

        codecplan_t r = codecplan_choose(sniff_bytes(buf, sizeof(buf)),
                                         sizeof(buf), "audio/mpeg");
        CHECK(r.why == CODECPLAN_NEED_SKIP,
              "a buffer opening with ID3 did not ask for a skip (why %d)",
              (int)r.why);

        const size_t n = id3_skip_bytes(buf, sizeof(buf));
        CHECK(n == skip, "skip length %zu, wanted %zu", n, skip);

        r = codecplan_choose(sniff_bytes(buf + n, sizeof(buf) - n),
                             sizeof(buf) - n, "audio/mpeg");
        CHECK(r.codec == STREAM_CODEC_MP3,
              "after skipping the tag the stream was %s, not MP3",
              stream_codec_name(r.codec));
    }

    /* ---------------------------------------------------------------- */
    /* Totality: every combination answers, nothing crashes, and ready   */
    /* and waiting are never both true                                   */
    /* ---------------------------------------------------------------- */
    {
        static const char *types[] = {
            NULL, "", "audio/mpeg", "audio/aac", "audio/aacp", "audio/ogg",
            "application/octet-stream", "text/html", "audio/x-scpls",
            "application/vnd.apple.mpegurl", "nonsense", "audio/",
            ";;;", "audio/mpeg;;;", " ",
        };
        for (int s = -2; s <= 12; s++) {
            for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
                for (size_t have = 0; have < 200; have += 37) {
                    codecplan_t r = codecplan_choose((sniff_t)s, have, types[t]);
                    CHECK(r.message && r.message[0],
                          "sniff %d type %zu have %zu: no message", s, t, have);
                    CHECK(!(codecplan_ready(&r) && codecplan_waiting(&r)),
                          "sniff %d type %zu have %zu: ready and waiting at once",
                          s, t, have);
                    if (codecplan_ready(&r)) {
                        CHECK(r.codec == STREAM_CODEC_MP3 ||
                              r.codec == STREAM_CODEC_AAC_ADTS,
                              "sniff %d type %zu: ready with codec %d",
                              s, t, (int)r.codec);
                    }
                }
            }
        }
    }

    /* Random bytes never produce a codec the decoder cannot handle, and
     * the whole path is walked under ASan. */
    {
        srand(20260911);
        for (int iter = 0; iter < 5000; iter++) {
            uint8_t buf[256];
            const size_t n = 1 + (size_t)(rand() % sizeof(buf));
            for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rand() % 256);
            const codecplan_t r = codecplan_choose(sniff_bytes(buf, n), n, NULL);
            if (codecplan_ready(&r) && r.codec != STREAM_CODEC_MP3 &&
                r.codec != STREAM_CODEC_AAC_ADTS) {
                CHECK(0, "iter %d: random bytes gave codec %d", iter, (int)r.codec);
                break;
            }
            (void)id3_skip_bytes(buf, n);
            checks++;
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
