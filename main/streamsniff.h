/*
 * streamsniff.h -- what the first bytes of an HTTP body are, and what an
 * ICY metadata block says, with nothing to run.
 *
 * For the stream probe: a station's Content-Type is often wrong or
 * generic (audio/mpeg on AAC, application/octet-stream on anything), so
 * the bytes are asked as well. Header-only and pure, so texttest compiles
 * this file.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNIFF_UNKNOWN = 0,
    SNIFF_MP3,          /* MPEG audio frame sync, layer I-III */
    SNIFF_AAC_ADTS,     /* AAC in ADTS: sync with layer 00 */
    SNIFF_ID3,          /* an ID3v2 tag in front of something */
    SNIFF_OGG,
    SNIFF_FLAC,
    SNIFF_M3U,          /* #EXTM3U, or HLS -- a playlist, not audio */
    SNIFF_PLS,          /* [playlist] */
    SNIFF_HTML,         /* a web page where a stream was expected */
} sniff_t;

static inline const char *sniff_name(sniff_t s)
{
    switch (s) {
    case SNIFF_MP3:      return "MP3 frames";
    case SNIFF_AAC_ADTS: return "AAC (ADTS)";
    case SNIFF_ID3:      return "ID3 tag";
    case SNIFF_OGG:      return "Ogg";
    case SNIFF_FLAC:     return "FLAC";
    case SNIFF_M3U:      return "M3U/HLS playlist";
    case SNIFF_PLS:      return "PLS playlist";
    case SNIFF_HTML:     return "HTML";
    default:             return "unknown";
    }
}

/*
 * Look for a signature at the start, or -- for raw MPEG/ADTS, which a
 * server can start mid-frame -- a frame sync anywhere in the first `n`
 * bytes, confirmed by a second sync one frame length later for MP3 so a
 * stray 0xFF in the middle of a frame is not taken for a header.
 */
static inline sniff_t sniff_bytes(const uint8_t *b, size_t n)
{
    if (!b || n < 4) return SNIFF_UNKNOWN;
    if (memcmp(b, "ID3", 3) == 0) return SNIFF_ID3;
    if (memcmp(b, "OggS", 4) == 0) return SNIFF_OGG;
    if (memcmp(b, "fLaC", 4) == 0) return SNIFF_FLAC;
    if (n >= 7 && memcmp(b, "#EXTM3U", 7) == 0) return SNIFF_M3U;
    if (n >= 10 && memcmp(b, "[playlist]", 10) == 0) return SNIFF_PLS;
    {
        size_t i = 0;
        while (i < n && (b[i] == ' ' || b[i] == '\r' || b[i] == '\n' || b[i] == '\t')) i++;
        if (i < n && b[i] == '<') return SNIFF_HTML;
    }

    static const int kbps_v1_l3[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int kbps_v2_l3[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int rate_v1[4] = { 44100, 48000, 32000, 0 };

    for (size_t i = 0; i + 4 <= n; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xF0) != 0xF0) {
            /* MPEG-2.5 has 0xFFE; only layer III is looked for there. */
            if (!(b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0)) continue;
        }
        const int layer = (b[i + 1] >> 1) & 3;
        if (layer == 0) {
            /* ADTS: 12-bit sync 0xFFF, layer 00. Confirm by frame length. */
            if ((b[i + 1] & 0xF0) != 0xF0 || i + 7 > n) continue;
            const size_t flen = ((size_t)(b[i + 3] & 0x03) << 11) | ((size_t)b[i + 4] << 3) | (b[i + 5] >> 5);
            if (flen < 7) continue;
            if (i + flen + 2 <= n) {
                if (b[i + flen] == 0xFF && (b[i + flen + 1] & 0xF6) == 0xF0) return SNIFF_AAC_ADTS;
                continue;
            }
            return SNIFF_AAC_ADTS;      /* too short to confirm; one sync */
        }
        if (layer != 1) continue;       /* layer III only: what stations send */
        const int ver = (b[i + 1] >> 3) & 3;     /* 3 = MPEG1, 2 = MPEG2, 0 = 2.5 */
        if (ver == 1) continue;
        const int bi = b[i + 2] >> 4, si = (b[i + 2] >> 2) & 3, pad = (b[i + 2] >> 1) & 1;
        if (bi == 0 || bi == 15 || si == 3) continue;
        const int rate = rate_v1[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
        const int kbps = (ver == 3 ? kbps_v1_l3 : kbps_v2_l3)[bi];
        const size_t flen = (size_t)((ver == 3 ? 144000 : 72000) * kbps / rate + pad);
        if (flen < 24) continue;
        if (i + flen + 2 <= n) {
            if (b[i + flen] == 0xFF && (b[i + flen + 1] & 0xE0) == 0xE0) return SNIFF_MP3;
            continue;
        }
        return SNIFF_MP3;
    }
    return SNIFF_UNKNOWN;
}

/*
 * The StreamTitle out of one ICY metadata block (the text after the
 * length byte, not NUL-terminated), copied into `out`. Returns 1 when a
 * title was found -- which may be empty, and stations send empty ones --
 * and 0 when the block has no StreamTitle. Always terminates `out`.
 */
static inline int icy_stream_title(const char *meta, size_t n, char *out, size_t out_size)
{
    if (!out || !out_size) return 0;
    out[0] = '\0';
    if (!meta) return 0;
    static const char key[] = "StreamTitle='";
    const size_t kl = sizeof(key) - 1;
    for (size_t i = 0; i + kl <= n; i++) {
        if (memcmp(meta + i, key, kl) != 0) continue;
        size_t j = i + kl, o = 0;
        /* The value ends at "';" -- a bare apostrophe can be part of it. */
        while (j < n && meta[j] != '\0') {
            if (meta[j] == '\'' && (j + 1 >= n || meta[j + 1] == ';' || meta[j + 1] == '\0')) break;
            if (o + 1 < out_size) out[o++] = meta[j];
            j++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/*
 * ADTS, counted as it arrives: how many AAC samples a byte stream holds,
 * so a download can be compared with real time.
 *
 * Fed the audio bytes only -- ICY metadata already removed -- in pieces of
 * any size. Hunts for a sync, reads the 7-byte header, skips the rest of
 * the frame, repeats. Bytes thrown away while hunting are counted, so a
 * stream that is not really ADTS, or loses bytes, says so.
 *
 * HE-AAC note: ADTS carries the core sample rate. An HE-AAC stream at
 * 44.1 kHz announces 22.05 kHz with 1024 samples a frame, and the
 * seconds this computes are still right, because both halve together.
 */
typedef struct {
    uint8_t  hdr[7];
    unsigned have;          /* header bytes collected */
    unsigned skip;          /* body bytes left in this frame */
    uint64_t samples;
    uint32_t frames;
    uint64_t lost;          /* bytes discarded hunting for a sync */
    unsigned rate, channels, profile;   /* from the last good header */
    unsigned min_len, max_len;
} adts_count_t;

static inline void adts_count_bytes(adts_count_t *c, const uint8_t *b, size_t n)
{
    static const unsigned rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                        24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    if (!c || !b) return;
    for (size_t i = 0; i < n; ) {
        if (c->skip) {
            const size_t take = (n - i) < c->skip ? (n - i) : c->skip;
            c->skip -= (unsigned)take;
            i += take;
            continue;
        }
        const uint8_t x = b[i++];
        if (c->have == 0) {
            if (x == 0xFF) c->hdr[c->have++] = x; else c->lost++;
            continue;
        }
        if (c->have == 1) {
            if ((x & 0xF6) == 0xF0) { c->hdr[c->have++] = x; }
            else if (x == 0xFF)     { c->lost++; }          /* stay at have == 1 */
            else                    { c->lost += 2; c->have = 0; }
            continue;
        }
        c->hdr[c->have++] = x;
        if (c->have < 7) continue;

        const uint8_t *h = c->hdr;
        const unsigned ri = (h[2] >> 2) & 0x0F;
        const unsigned flen = ((unsigned)(h[3] & 0x03) << 11) | ((unsigned)h[4] << 3) | (h[5] >> 5);
        c->have = 0;
        if (ri >= 13 || flen < 7) {
            c->lost += 7;
            continue;
        }
        c->rate = rates[ri];
        c->channels = ((h[2] & 0x01) << 2) | (h[3] >> 6);
        c->profile = (h[2] >> 6) + 1;
        c->frames++;
        c->samples += 1024u * ((h[6] & 0x03) + 1u);
        if (!c->min_len || flen < c->min_len) c->min_len = flen;
        if (flen > c->max_len) c->max_len = flen;
        c->skip = flen - 7;
    }
}

/* Seconds of audio counted so far, in milliseconds. */
static inline uint64_t adts_ms(const adts_count_t *c)
{
    return (c && c->rate) ? c->samples * 1000u / c->rate : 0;
}

/*
 * The payload of a JWT, decoded. `tok` is header.payload.signature in
 * base64url (length `n`, not terminated); the middle part is decoded into
 * `out` and terminated. 0 if it is not three parts or does not decode.
 */
static inline int jwt_payload(const char *tok, size_t n, char *out, size_t out_size)
{
    if (!tok || !out || out_size == 0) return 0;
    out[0] = '\0';
    size_t d1 = n, d2 = n;
    for (size_t i = 0; i < n; i++) {
        if (tok[i] != '.') continue;
        if (d1 == n) d1 = i; else if (d2 == n) { d2 = i; break; }
    }
    if (d1 == n || d2 == n || d2 <= d1 + 1) return 0;

    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = d1 + 1; i < d2; i++) {
        const char ch = tok[i];
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-' || ch == '+') v = 62;
        else if (ch == '_' || ch == '/') v = 63;
        else if (ch == '=') break;
        else return 0;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= out_size) { out[o] = '\0'; return 0; }
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    return o > 0;
}

/* An integer JSON field, the flat kind a JWT payload has: "key":123. */
static inline int json_int(const char *json, const char *key, int64_t *out)
{
    if (!json || !key || !out) return 0;
    const size_t kl = strlen(key);
    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, kl) != 0 || p[1 + kl] != '"') continue;
        const char *q = p + 2 + kl;
        while (*q == ' ') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ') q++;
        int neg = 0;
        if (*q == '-') { neg = 1; q++; }
        if (*q < '0' || *q > '9') return 0;
        int64_t v = 0;
        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
        *out = neg ? -v : v;
        return 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
