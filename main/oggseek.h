/*
 * oggseek.h -- seeking an Ogg stream by bisecting page granule
 * positions.
 *
 * WHAT THE LIBRARY ACTUALLY DOES, RATHER THAN WHAT WAS ASSUMED
 *
 * This was written off twice on the grounds that it could not be known
 * whether esp_audio_codec's Ogg parser would accept pages from a new
 * position -- a demuxer is entitled to treat a page sequence number that
 * jumps as a hole, and to drop pages or fail rather than carry on. The
 * component ships as a precompiled archive, so the header says nothing
 * about it.
 *
 * The archive does. `esp_ogg_parse_frame` in
 * `libesp_audio_simple_dec.a` is 1232 bytes of RISC-V and it:
 *
 *   - scans forward for the `OggS` capture pattern anywhere in the
 *     buffer it is given, and reports the skipped bytes rather than
 *     failing -- so resynchronisation is a supported operation, not a
 *     hope;
 *   - checks the version byte and compares the page serial number
 *     against the one it learned from the beginning of the stream;
 *   - never reads the page sequence number at offset 18, and never
 *     reads the CRC at offset 22. There is no CRC table in the object
 *     at all -- the only .rodata in it is format strings.
 *
 * So a sequence-number jump is invisible to it and a page from the
 * middle of the file is an ordinary page. That is the whole of what
 * made this uncertain, and it is answered by reading the binary rather
 * than by flashing a board.
 *
 * WHAT IT DOES NOT ANSWER, AND WHY THE HEADERS ARE REPLAYED
 *
 * The parser holds two pieces of state that a jump invalidates: a flag
 * saying the beginning-of-stream headers have been parsed, and a
 * partially-assembled packet, spliced across pages by `append_packet`.
 * The first is why a fresh parser cannot simply be pointed at the
 * middle of a file; the second is why the existing one cannot be left
 * alone across a jump, or the tail of the packet that was in flight
 * gets glued to the head of a packet from the new position.
 *
 * So the same shape as 0701 and 0704: close the decoder, reopen it,
 * replay the stream's own header pages verbatim, then the pages at the
 * target. Verbatim matters -- real pages already carry correct CRCs,
 * and although this parser does not check them, the next version of it
 * might, and a synthesised page would be the kind of thing that works
 * until it does not.
 *
 * Vorbis is why the preamble can be large: its three header packets
 * include the codebooks, which run to several KB and can span pages.
 * Opus is two short pages. Both are found the same way -- pages from
 * the start of the file with a granule position of zero are headers,
 * and the first page with a nonzero granule is audio.
 *
 * CHAINED FILES ARE ONE STREAM AS FAR AS THIS GOES
 *
 * An Ogg file may hold several logical streams end to end -- two
 * podcasts concatenated with `cat` is a legal Ogg. The parser in the
 * archive compares every page's serial number against the one it
 * learned at the start and drops the rest, so the player hears the
 * first stream and then silence. Everything here is bounded to that
 * same first stream: the extent it occupies is what the bar shows and
 * what a drag can address.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "storage_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How much of the front of the file may be kept as the resume preamble.
 *
 * A Vorbis setup header is a few KB in every file anybody will play;
 * this is room for an unusual one without room for a runaway
 * allocation from a corrupt segment table.
 */
#define OGG_HEADER_MAX      (64 * 1024)

typedef struct {
    bool     ok;
    bool     opus;              /* granule is 48 kHz units, and pre-skipped */
    uint32_t serial;
    uint32_t rate;              /* granule divisor: 48000 for Opus */
    uint32_t pre_skip;          /* Opus only; 0 for Vorbis */
    long     first_audio;       /* byte offset of the first audio page */
    long     file_end;          /* end of THIS logical stream, which is
                                 * the end of the file unless chained */
    uint64_t last_granule;      /* 0 when it could not be established */

    uint8_t *header;            /* the header pages, verbatim */
    size_t   header_len;
} ogg_seek_t;

/*
 * Read the header pages and find where the audio starts. Allocates;
 * ogg_seek_free() releases. False for anything that is not an Ogg
 * stream this player can route, which includes an Ogg whose first page
 * is neither Vorbis nor Opus.
 */
bool ogg_seek_probe(FILE *f, ogg_seek_t *os);
void ogg_seek_free(ogg_seek_t *os);

/*
 * Byte offset of the page to resume from, or -1.
 *
 * `landed_sec` is filled with the position that page actually
 * represents, which is at or before the target: a seek lands on a page
 * boundary and pages are tens of milliseconds. The caller anchors its
 * clock to this rather than to what it asked for, for the reason
 * decoder_seek_sec_at() gives.
 *
 * Only pages that do NOT continue a packet from the previous page are
 * candidates. Landing on a continuation hands the parser the tail of a
 * packet whose head it has never seen.
 */
long ogg_seek_find(FILE *f, const ogg_seek_t *os, uint32_t sec,
                   uint32_t *landed_sec);

/*
 * Where the logical stream carrying `serial` stops, for a file where
 * the tail does not belong to it.
 *
 * `*stream_end` is the offset just past its last page and
 * `*last_granule` that page's granule position, either of which may be
 * left alone if it could not be established. Bisects, then walks the
 * last few pages, so it costs a couple of dozen short reads and is only
 * worth calling once the cheap tail window has come back foreign.
 *
 * duration.c calls this for the same reason ogg_seek_probe() does, with
 * its own I/O class, which is why it is here rather than static.
 */
bool ogg_stream_extent(FILE *f, uint32_t serial, long from, long file_end,
                       storage_io_class_t cls,
                       long *stream_end, uint64_t *last_granule);

/* The header pages, for replay after a seek. Returns a pointer into the
 * probe's own storage -- borrowed, valid until ogg_seek_free(). */
const uint8_t *ogg_seek_preamble(const ogg_seek_t *os, size_t *len);

#ifdef __cplusplus
}
#endif
