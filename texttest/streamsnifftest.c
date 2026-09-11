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

    printf("ADTS counting: frames, samples, rate, in pieces of any size\n");
    {
        size_t len = adts_frames(b, 0, 10, 300);          /* 44.1 kHz (index 4), LC, stereo */
        adts_count_t c; memset(&c, 0, sizeof c);
        adts_count_bytes(&c, b, len);
        CHECK(c.frames == 10 && c.samples == 10240 && c.lost == 0, "whole: %u frames, %llu samples, %llu lost",
              c.frames, (unsigned long long)c.samples, (unsigned long long)c.lost);
        CHECK(c.rate == 44100 && c.channels == 2 && c.profile == 2, "rate %u ch %u profile %u", c.rate, c.channels, c.profile);
        CHECK(adts_ms(&c) == 232, "ms %llu", (unsigned long long)adts_ms(&c));
        CHECK(c.min_len == 300 && c.max_len == 300, "lengths");

        adts_count_t d; memset(&d, 0, sizeof d);
        for (size_t i = 0; i < len; i++) adts_count_bytes(&d, b + i, 1);
        CHECK(d.frames == 10 && d.samples == c.samples, "byte at a time: %u", d.frames);

        adts_count_t e; memset(&e, 0, sizeof e);
        uint8_t j[4000];
        memset(j, 0x12, 37);
        memcpy(j + 37, b, len);
        adts_count_bytes(&e, j, 37 + len);
        CHECK(e.frames == 10 && e.lost == 37, "joined mid-stream: %u frames, %llu lost", e.frames, (unsigned long long)e.lost);

        /* A frame with a bad rate index is skipped and counted as lost. */
        adts_count_t f; memset(&f, 0, sizeof f);
        uint8_t k[4000];
        memcpy(k, b, len);
        k[300 + 2] = (uint8_t)((k[300 + 2] & ~0x3C) | (15 << 2));
        adts_count_bytes(&f, k, len);
        CHECK(f.frames < 10 && f.lost >= 7, "bad header: %u frames, %llu lost", f.frames, (unsigned long long)f.lost);

        adts_count_t g; memset(&g, 0, sizeof g);
        adts_count_bytes(&g, NULL, 10);
        CHECK(g.frames == 0 && adts_ms(&g) == 0, "null is nothing");
        CHECK(adts_ms(NULL) == 0, "null counter");
    }

    printf("JWT payload and its exp\n");
    {
        /* {"alg":"HS256"} . {"stream":"x","exp":1789097000} . sig */
        const char tok[] = "eyJhbGciOiJIUzI1NiJ9.eyJzdHJlYW0iOiJ4IiwiZXhwIjoxNzg5MDk3MDAwfQ.c2ln";
        char pl[128];
        CHECK(jwt_payload(tok, sizeof tok - 1, pl, sizeof pl) &&
              strcmp(pl, "{\"stream\":\"x\",\"exp\":1789097000}") == 0, "payload '%s'", pl);
        int64_t exp = 0;
        CHECK(json_int(pl, "exp", &exp) && exp == 1789097000LL, "exp %lld", (long long)exp);
        CHECK(!json_int(pl, "iat", &exp), "absent field");
        CHECK(!json_int("{\"expiry\":5}", "exp", &exp), "prefix of another key");
        CHECK(json_int("{\"exp\" : -3}", "exp", &exp) && exp == -3, "spaces and sign");
        CHECK(!jwt_payload("nodots", 6, pl, sizeof pl), "not a JWT");
        CHECK(!jwt_payload("a..b", 4, pl, sizeof pl), "empty payload");
        CHECK(!jwt_payload("a.b$c.d", 7, pl, sizeof pl), "bad character");
        char tiny[4];
        CHECK(!jwt_payload(tok, sizeof tok - 1, tiny, sizeof tiny) && strlen(tiny) < sizeof tiny, "does not overrun");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
