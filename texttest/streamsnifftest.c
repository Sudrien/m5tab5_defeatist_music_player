/*
 * streamsnifftest.c -- main/streamsniff.h, compiled from the header.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "streamsniff.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/* Two MPEG1 layer III frames, 128 kbps, 44.1 kHz, no padding: 417 bytes each. */
static size_t mp3_frames(uint8_t *b, size_t off, int count)
{
    for (int f = 0; f < count; f++) {
        const size_t at = off + (size_t)f * 417;
        memset(b + at, 0x55, 417);
        b[at] = 0xFF; b[at + 1] = 0xFB; b[at + 2] = 0x90; b[at + 3] = 0x44;
    }
    return off + (size_t)count * 417;
}

/* Two ADTS frames of `len` bytes each. */
static size_t adts_frames(uint8_t *b, size_t off, int count, size_t len)
{
    for (int f = 0; f < count; f++) {
        const size_t at = off + (size_t)f * len;
        memset(b + at, 0x11, len);
        b[at] = 0xFF; b[at + 1] = 0xF1; b[at + 2] = 0x50;
        b[at + 3] = (uint8_t)(0x80 | ((len >> 11) & 0x03));
        b[at + 4] = (uint8_t)((len >> 3) & 0xFF);
        b[at + 5] = (uint8_t)(((len & 0x07) << 5) | 0x1F);
        b[at + 6] = 0xFC;
    }
    return off + (size_t)count * len;
}

int main(void)
{
    uint8_t b[4096];

    printf("signatures at the start\n");
    CHECK(sniff_bytes((const uint8_t *)"ID3\x04\x00", 5) == SNIFF_ID3, "ID3");
    CHECK(sniff_bytes((const uint8_t *)"OggS\x00\x02", 6) == SNIFF_OGG, "Ogg");
    CHECK(sniff_bytes((const uint8_t *)"fLaC\x00\x00", 6) == SNIFF_FLAC, "FLAC");
    CHECK(sniff_bytes((const uint8_t *)"#EXTM3U\n#EXT-X", 14) == SNIFF_M3U, "M3U");
    CHECK(sniff_bytes((const uint8_t *)"[playlist]\nFile1=", 17) == SNIFF_PLS, "PLS");
    CHECK(sniff_bytes((const uint8_t *)"\r\n <!DOCTYPE html>", 18) == SNIFF_HTML, "HTML after whitespace");

    printf("MPEG frames, from the start and from mid-frame\n");
    size_t n = mp3_frames(b, 0, 3);
    CHECK(sniff_bytes(b, n) == SNIFF_MP3, "clean start");
    memset(b, 0x33, 200);
    n = mp3_frames(b, 200, 3);
    CHECK(sniff_bytes(b, n) == SNIFF_MP3, "joined mid-frame");

    printf("a stray 0xFF inside a frame is not a header\n");
    memset(b, 0x22, sizeof b);
    b[10] = 0xFF; b[11] = 0xFB; b[12] = 0x90; b[13] = 0x44;   /* looks like a header */
    CHECK(sniff_bytes(b, 1000) == SNIFF_UNKNOWN, "one fake sync with no second: %s",
          sniff_name(sniff_bytes(b, 1000)));

    printf("ADTS is AAC, not MP3\n");
    n = adts_frames(b, 0, 3, 300);
    CHECK(sniff_bytes(b, n) == SNIFF_AAC_ADTS, "ADTS: %s", sniff_name(sniff_bytes(b, n)));
    memset(b, 0x44, 50);
    n = adts_frames(b, 50, 3, 300);
    CHECK(sniff_bytes(b, n) == SNIFF_AAC_ADTS, "ADTS mid-frame: %s", sniff_name(sniff_bytes(b, n)));

    printf("nothing, and too little\n");
    memset(b, 0, sizeof b);
    CHECK(sniff_bytes(b, sizeof b) == SNIFF_UNKNOWN, "zeros");
    CHECK(sniff_bytes((const uint8_t *)"ID", 2) == SNIFF_UNKNOWN, "two bytes");
    CHECK(sniff_bytes(NULL, 100) == SNIFF_UNKNOWN, "null");
    {
        unsigned s = 7;
        int bad = 0;
        for (int t = 0; t < 20000; t++) {
            const size_t len = 4 + (s % 700);
            for (size_t i = 0; i < len; i++) { s = s * 1103515245u + 12345u; b[i] = (uint8_t)(s >> 16); }
            const sniff_t r = sniff_bytes(b, len);
            if (r > SNIFF_HTML) bad++;
        }
        CHECK(bad == 0, "random bytes gave %d out-of-range answers", bad);
    }

    printf("ICY StreamTitle\n");
    char t[64];
    const char m1[] = "StreamTitle='Radio Orient - Arabic';StreamUrl='';";
    CHECK(icy_stream_title(m1, sizeof m1 - 1, t, sizeof t) && strcmp(t, "Radio Orient - Arabic") == 0, "plain: '%s'", t);
    const char m2[] = "StreamTitle='Rock 'n' Roll Hour';";
    CHECK(icy_stream_title(m2, sizeof m2 - 1, t, sizeof t) && strcmp(t, "Rock 'n' Roll Hour") == 0, "inner apostrophes: '%s'", t);
    const char m3[] = "StreamTitle='';";
    CHECK(icy_stream_title(m3, sizeof m3 - 1, t, sizeof t) == 1 && t[0] == '\0', "empty title is a title");
    const char m4[] = "StreamUrl='x';";
    CHECK(icy_stream_title(m4, sizeof m4 - 1, t, sizeof t) == 0 && t[0] == '\0', "no title");
    const char m5[] = "StreamTitle='\xD8\xB1\xD8\xA7\xD8\xAF\xD9\x8A\xD9\x88';\0\0\0\0";
    CHECK(icy_stream_title(m5, sizeof m5 - 1, t, sizeof t) && strcmp(t, "\xD8\xB1\xD8\xA7\xD8\xAF\xD9\x8A\xD9\x88") == 0, "UTF-8 passes through");
    const char m6[] = "StreamTitle='abcdefghijklmnopqrstuvwxyz';";
    char tiny[8];
    CHECK(icy_stream_title(m6, sizeof m6 - 1, tiny, sizeof tiny) && strcmp(tiny, "abcdefg") == 0, "truncated: '%s'", tiny);
    const char m7[] = "StreamTitle='unterminated";
    CHECK(icy_stream_title(m7, sizeof m7 - 1, t, sizeof t) && strcmp(t, "unterminated") == 0, "runs to the end: '%s'", t);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
