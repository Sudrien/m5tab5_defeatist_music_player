/*
 * icydemux.h -- ICY metadata out of a stream body, a byte at a time,
 * with nothing to run.
 *
 * A station that answered `Icy-MetaData: 1` sends `icy-metaint` bytes of
 * audio, then a single length byte L, then 16*L bytes of metadata, then
 * `icy-metaint` bytes of audio again, for as long as the connection
 * lasts. L is very often 0, which is the station saying "nothing has
 * changed" and costs one byte.
 *
 * WHY THIS IS ITS OWN FILE
 *
 * The probe had this loop inline (streamprobe.c), and it worked, because
 * the probe reads into one buffer and throws the result away. The stream
 * path cannot: the audio bytes go to a ring on another task, the titles
 * go to the screen, and the connection can drop in the middle of a
 * metadata block and be reopened -- so the state has to be a value that
 * is owned, reset and inspected rather than three locals in a read loop.
 *
 * Once that state is a struct it can be fed by a host test, and the
 * interesting cases are exactly the ones a 2048-byte read boundary makes
 * rare on hardware and common in a test: a length byte arriving as the
 * last byte of a read, a metadata block split across three reads, a
 * block that is longer than any title has a right to be.
 *
 * THE ONE PROPERTY THAT MATTERS
 *
 * **Feeding this in pieces must give the same answer as feeding it
 * whole.** Every byte is either audio or metadata; there is no
 * lookahead, no buffering of undecided bytes, and no position that
 * depends on how a read happened to be cut. `icydemuxtest` asserts that
 * directly by running the same body through in 1-byte, 3-byte, 1000-byte
 * and single-shot pieces and requiring identical audio out and identical
 * titles.
 *
 * WHAT IT DOES NOT DO
 *
 * It does not allocate, block, or know what a stream is. `out` is the
 * caller's and must have room for `n` bytes -- audio out is never more
 * than bytes in, and is less exactly by the metadata removed.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "streamsniff.h"        /* icy_stream_title() */

#ifdef __cplusplus
extern "C" {
#endif

/* 255 * 16. The protocol's maximum, so a block can never overrun this. */
#define ICY_META_MAX    (4080)
#define ICY_TITLE_MAX   (256)

typedef enum {
    ICY_IN_AUDIO = 0,   /* counting down metaint audio bytes */
    ICY_IN_LENGTH,      /* the next byte is L */
    ICY_IN_META,        /* collecting 16*L metadata bytes */
} icy_phase_t;

typedef struct {
    int         metaint;        /* 0: no metadata in this stream at all */
    icy_phase_t phase;
    int         audio_left;     /* bytes until the next length byte */
    int         meta_left;      /* metadata bytes still to collect */
    int         meta_len;       /* collected so far */
    char        meta[ICY_META_MAX + 1];

    /* Published results. `titles` counts completed metadata blocks that
     * contained a StreamTitle, including empty ones -- WNZK sends " - "
     * forever -- so a caller can tell "no title yet" from "the title is
     * empty", which are different things on screen. */
    char     title[ICY_TITLE_MAX];
    uint32_t titles;
    uint64_t audio_bytes;       /* audio passed out since init */
    uint64_t meta_bytes;        /* metadata consumed, length bytes included */
    uint32_t blocks;            /* metadata blocks seen, empty ones included */
} icydemux_t;

/*
 * `metaint` is the icy-metaint header, or 0 when the station sent none.
 * Zero is not an error and is the common case for a station that ignored
 * the request header: every byte is then audio and this is a memcpy.
 */
static inline void icydemux_init(icydemux_t *d, int metaint)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    /* A negative or absurd metaint is a header this code will not trust;
     * treat it as "no metadata" rather than desynchronising the body. */
    d->metaint = (metaint > 0 && metaint <= (1 << 24)) ? metaint : 0;
    d->phase = ICY_IN_AUDIO;
    d->audio_left = d->metaint;
}

/*
 * The state that belongs to a *connection*, cleared without losing what
 * belongs to the station. A reconnect starts a fresh body -- the byte
 * counters restart at the new stream's first byte -- but the title on
 * screen is the last thing the listener was told and stays until the new
 * connection says otherwise.
 */
static inline void icydemux_reconnect(icydemux_t *d, int metaint)
{
    if (!d) return;
    char     keep_title[ICY_TITLE_MAX];
    uint32_t keep_titles = d->titles;
    memcpy(keep_title, d->title, sizeof(keep_title));
    icydemux_init(d, metaint);
    memcpy(d->title, keep_title, sizeof(d->title));
    d->titles = keep_titles;
}

/*
 * Feed `n` bytes of body. Audio bytes are written to `out` in order and
 * the count returned; `out` must have room for `n`. Completed metadata
 * blocks update `d->title` and `d->titles`.
 *
 * In-place is allowed: `out` may be `in`. Audio bytes only ever move
 * left (metadata is removed), so a forward copy never reads a byte it
 * has already written.
 */
static inline size_t icydemux_feed(icydemux_t *d, const uint8_t *in, size_t n,
                                   uint8_t *out)
{
    if (!d || !in || !out) return 0;
    if (d->metaint <= 0) {
        if (n) memmove(out, in, n);
        d->audio_bytes += n;
        return n;
    }

    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        switch (d->phase) {
        case ICY_IN_AUDIO: {
            /* A run, not a byte: this is the 16000-in-16001 case and
             * doing it one byte at a time was measurable in the probe. */
            size_t take = n - i;
            if (take > (size_t)d->audio_left) take = (size_t)d->audio_left;
            memmove(out + o, in + i, take);
            o += take;
            i += take;
            d->audio_left -= (int)take;
            d->audio_bytes += take;
            if (d->audio_left == 0) d->phase = ICY_IN_LENGTH;
            break;
        }
        case ICY_IN_LENGTH: {
            const int len = (int)in[i++] * 16;
            d->meta_bytes++;
            d->blocks++;
            d->meta_len = 0;
            if (len == 0) {
                /* The common block: "nothing has changed". No title is
                 * reported, and in particular the last one is kept. */
                d->phase = ICY_IN_AUDIO;
                d->audio_left = d->metaint;
            } else {
                d->meta_left = len;
                d->phase = ICY_IN_META;
            }
            break;
        }
        case ICY_IN_META: {
            size_t take = n - i;
            if (take > (size_t)d->meta_left) take = (size_t)d->meta_left;
            /* meta_left never exceeds 4080 and meta is 4081, so this
             * cannot truncate; the clamp is here because the day it can
             * is the day someone widens the length byte. */
            size_t room = sizeof(d->meta) - 1 - (size_t)d->meta_len;
            size_t keep = take < room ? take : room;
            memcpy(d->meta + d->meta_len, in + i, keep);
            d->meta_len += (int)keep;
            i += take;
            d->meta_left -= (int)take;
            d->meta_bytes += take;
            if (d->meta_left == 0) {
                d->meta[d->meta_len] = '\0';
                /* Parsed into a scratch buffer, not into d->title.
                 * icy_stream_title() terminates its output before it
                 * searches, so handing it d->title directly blanks the
                 * title on every block that has no StreamTitle -- and a
                 * StreamUrl-only block is a thing stations send. The
                 * screen went empty and came back, for no reason a
                 * listener could see. Found by icydemuxtest. */
                char parsed[ICY_TITLE_MAX];
                if (icy_stream_title(d->meta, (size_t)d->meta_len,
                                     parsed, sizeof(parsed))) {
                    memcpy(d->title, parsed, sizeof(d->title));
                    d->titles++;
                }
                d->phase = ICY_IN_AUDIO;
                d->audio_left = d->metaint;
            }
            break;
        }
        default:
            /* Unreachable; a corrupted phase is treated as audio rather
             * than as a loop that never advances `i`. */
            d->phase = ICY_IN_AUDIO;
            d->audio_left = d->metaint;
            break;
        }
    }
    return o;
}

#ifdef __cplusplus
}
#endif
