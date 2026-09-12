/*
 * mp3count.h -- MPEG audio frames, counted as they arrive, so an MP3
 * download can be compared with real time.
 *
 * `streamsniff.h` counts ADTS frames and nothing else, because the only
 * station the probe had ever been pointed at was AAC. Pointed at an MP3
 * station it does not fail -- **it silently stops measuring.**
 * `adts.frames` stays 0, `adts_ms()` returns 0, every window prints
 * 0.00x, and the summary block disappears. A probe that quietly reports
 * nothing is worse than one that breaks, so this is the counterpart.
 *
 * Same shape as `adts_count_t` on purpose: fed the audio bytes only, in
 * pieces of any size, hunting for a sync, reading the header, skipping
 * the body, counting the bytes thrown away while hunting. That last
 * figure is the one that matters -- **0 bytes lost hunting** is what
 * four runs of the AAC station established about `icydemux` and the
 * ring, and the same assertion should be available on an MP3 station
 * rather than having to be taken on trust.
 *
 * WHAT MAKES MP3 HARDER THAN ADTS
 *
 * An ADTS header carries its own frame length. An MP3 header does not:
 * the length is computed from the bitrate, the sample rate and the
 * padding bit, and the arithmetic differs between MPEG1 and MPEG2/2.5.
 * So a wrong table entry does not produce a rejected frame, it produces
 * a frame of the wrong length, which desynchronises the stream and shows
 * up as bytes lost -- the exact signal this exists to measure. The
 * tables are therefore tested against frame lengths computed
 * independently in the test rather than copied from the same source.
 *
 * Sync is also weaker. ADTS wants 0xFFF and a layer field; MP3 wants
 * 0xFFE and everything after it is plausible-looking. Eleven bits of
 * sync in a 128 kbit/s stream will false-positive on music data, so a
 * candidate header is only accepted when its *computed* length lands on
 * another valid header -- the same confirmation the ADTS counter gets
 * for free from its length field.
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

typedef struct {
    uint8_t  hdr[4];
    unsigned have;          /* header bytes collected */
    unsigned skip;          /* body bytes left in this frame */
    uint64_t samples;
    uint32_t frames;
    uint64_t lost;          /* bytes discarded hunting for a sync */
    /* From the last good header. `version` is 1, 2 or 25 (for MPEG 2.5);
     * `layer` is 1, 2 or 3. */
    unsigned rate, channels, bitrate, version, layer;
    unsigned min_len, max_len;
} mp3_count_t;

/*
 * Frame length in bytes, and samples per frame, for a candidate header.
 * Returns 0 for anything this will not count: a bad sync, a reserved
 * version or layer, a free-format or reserved bitrate index, a reserved
 * sample rate. `*samples` is only written when the return is non-zero.
 *
 * Free format (bitrate index 0) is legal MPEG and is refused here: its
 * frame length is not in the header and recovering it means measuring
 * the distance between syncs, which is a different program. No streaming
 * encoder emits it.
 */
static inline unsigned mp3_frame_len(const uint8_t *h, unsigned *samples,
                                     unsigned *rate_out, unsigned *bitrate_out,
                                     unsigned *version_out, unsigned *layer_out,
                                     unsigned *channels_out)
{
    /* [kbit/s] indexed by [version is MPEG1 ? 0 : 1][layer 1..3][index]. */
    static const unsigned br[2][4][16] = {
        {   /* MPEG1 */
            { 0 },
            { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0 }, /* L1 */
            { 0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0 }, /* L2 */
            { 0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,0 }, /* L3 */
        },
        {   /* MPEG2 and MPEG2.5 */
            { 0 },
            { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0 },    /* L1 */
            { 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0 },    /* L2 */
            { 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0 },    /* L3 */
        },
    };
    static const unsigned sr[4][3] = {
        { 11025, 12000,  8000 },    /* MPEG2.5 */
        { 0, 0, 0 },                /* reserved */
        { 22050, 24000, 16000 },    /* MPEG2 */
        { 44100, 48000, 32000 },    /* MPEG1 */
    };

    if (!h) return 0;
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;

    const unsigned vbits = (h[1] >> 3) & 0x03;
    const unsigned lbits = (h[1] >> 1) & 0x03;
    if (vbits == 1 || lbits == 0) return 0;         /* both reserved */

    const unsigned layer = 4 - lbits;               /* 01->3, 10->2, 11->1 */
    const unsigned version = vbits == 3 ? 1 : (vbits == 2 ? 2 : 25);
    /* Index 3 is the reserved sample rate and must be rejected *before*
     * it indexes a three-element row. Checking the value afterwards
     * reads one past the end -- caught by UBSan in mp3counttest, and on
     * a device it would have read whatever followed the table and
     * computed a frame length from it. */
    const unsigned sri = (h[2] >> 2) & 0x03;
    if (sri >= 3) return 0;
    const unsigned srate = sr[vbits][sri];
    if (!srate) return 0;

    const unsigned bi = (h[2] >> 4) & 0x0F;
    if (bi == 0 || bi == 15) return 0;              /* free format, reserved */
    const unsigned bitrate = br[version == 1 ? 0 : 1][layer][bi];
    if (!bitrate) return 0;

    const unsigned pad = (h[2] >> 1) & 0x01;

    /* Layer 1 counts in 4-byte slots; layers 2 and 3 in bytes. Layer 3
     * on MPEG2/2.5 has half the samples per frame, which is why the
     * constant differs -- getting that wrong gives a frame of the wrong
     * length rather than a rejected one. */
    unsigned len, spf;
    if (layer == 1) {
        spf = 384;
        len = (12u * bitrate * 1000u / srate + pad) * 4u;
    } else {
        spf = (layer == 3 && version != 1) ? 576 : 1152;
        len = (spf / 8u) * bitrate * 1000u / srate + pad;
    }
    if (len < 4) return 0;

    if (samples) *samples = spf;
    if (rate_out) *rate_out = srate;
    if (bitrate_out) *bitrate_out = bitrate;
    if (version_out) *version_out = version;
    if (layer_out) *layer_out = layer;
    if (channels_out) *channels_out = (((h[3] >> 6) & 0x03) == 3) ? 1 : 2;
    return len;
}

static inline void mp3_count_bytes(mp3_count_t *c, const uint8_t *b, size_t n)
{
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
            if ((x & 0xE0) == 0xE0) { c->hdr[c->have++] = x; }
            else if (x == 0xFF)     { c->lost++; }       /* stay at have == 1 */
            else                    { c->lost += 2; c->have = 0; }
            continue;
        }
        c->hdr[c->have++] = x;
        if (c->have < 4) continue;

        unsigned spf = 0, rate = 0, bitrate = 0, version = 0, layer = 0, ch = 0;
        const unsigned len = mp3_frame_len(c->hdr, &spf, &rate, &bitrate,
                                           &version, &layer, &ch);
        c->have = 0;
        if (!len) {
            c->lost += 4;
            continue;
        }
        c->rate = rate;
        c->bitrate = bitrate;
        c->version = version;
        c->layer = layer;
        c->channels = ch;
        c->frames++;
        c->samples += spf;
        if (!c->min_len || len < c->min_len) c->min_len = len;
        if (len > c->max_len) c->max_len = len;
        c->skip = len - 4;
    }
}

/* Seconds of audio counted so far, in milliseconds. */
static inline uint64_t mp3_ms(const mp3_count_t *c)
{
    return (c && c->rate) ? c->samples * 1000u / c->rate : 0;
}

#ifdef __cplusplus
}
#endif
