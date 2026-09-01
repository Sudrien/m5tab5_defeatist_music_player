/*
 * flacseek.h -- seeking a native FLAC stream by bisecting its frame
 * headers.
 *
 * WHY THIS IS NOT cbrseek.c
 *
 * cbrseek.c seeks by proving the byte rate is constant and then doing
 * arithmetic. FLAC is variable by construction -- a silent passage
 * costs a handful of bytes and a dense one costs thousands -- so there
 * is no line to prove and that whole approach is unavailable.
 *
 * What FLAC has instead is better: every frame header carries the
 * sample it starts at, and is protected by a CRC-8 over the header. So
 * the position of any byte in the file can be READ rather than
 * estimated, and finding a target sample is a binary search over byte
 * offsets -- about fifteen probes of a few hundred bytes each on a
 * 60 MB file, at drag time, with nothing read at open.
 *
 * That is the whole difference in accuracy between the two files.
 * cbrseek lands within a tolerated drift; this lands on the frame that
 * contains the sample asked for, exactly, and says which sample that
 * turned out to be.
 *
 * WHY NOT THE SEEKTABLE
 *
 * FLAC has a SEEKTABLE metadata block, which is the obvious thing to
 * read and is deliberately not read here.
 *
 *   - It is optional, and plenty of encoders omit it. A mechanism that
 *     works for every file beats one that works for most files and
 *     needs the other one written anyway.
 *   - Its points are typically ten seconds apart, so it answers a
 *     coarser question than the bisection does, and the residual would
 *     then have to be decoded through -- which is the cost 0703 took on
 *     for MP3 and would be taking on here for no reason.
 *   - The bisection is not slow. Fifteen probes against a card doing
 *     0.85% duty during playback is not a number anybody will feel.
 *
 * If a file has one it is ignored, which costs nothing and removes a
 * second code path that would be exercised only on some files.
 *
 * THE PARSER HAS TO BE RE-PRIMED, AS EVER
 *
 * A FLAC decoder handed frames with no STREAMINFO does not know the
 * block size, the sample rate, the channel count or the bit depth. So
 * this carries the STREAMINFO block read at open and hands it back as a
 * preamble on every seek, exactly as cbrseek.c does for WAV -- the
 * lesson of 0701, applied on purpose this time rather than after the
 * fact.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "fLaC", a 4-byte block header, and 34 bytes of STREAMINFO. */
#define FLAC_PREAMBLE_BYTES  (4 + 4 + 34)

typedef struct {
    bool     ok;
    long     first_frame;       /* byte offset past the metadata blocks */
    long     file_end;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint16_t min_block;         /* used when a header defers its block size */
    uint32_t max_frame;         /* STREAMINFO's max frame size, 0 if unstated */
    uint64_t total_samples;     /* 0 when the stream does not say */
    uint8_t  streaminfo[34];    /* verbatim, for the preamble */
} flac_seek_t;

/*
 * Read STREAMINFO and find where the audio starts. False for anything
 * that is not a native FLAC stream, including FLAC-in-Ogg, which is a
 * different container and a different seek.
 *
 * The handle's position is saved and restored. Two small reads plus one
 * per metadata block.
 */
bool flac_seek_probe(FILE *f, flac_seek_t *fs);

/*
 * Find the frame containing `sec`.
 *
 * Returns the byte offset of that frame's header, or -1. `landed_sample`
 * is filled with the sample that frame starts at, which is at or before
 * the target and is what the caller should show on the clock: the
 * player re-anchors its position counter from a seek, and anchoring it
 * to what was ASKED for rather than what was REACHED is how a clock
 * ends up disagreeing with the audio by the width of a frame.
 */
long flac_seek_find(FILE *f, const flac_seek_t *fs, uint32_t sec,
                    uint64_t *landed_sample);

/* "fLaC" plus the STREAMINFO block, with the last-metadata-block flag
 * set so a fresh decoder goes straight from it to frames. Returns bytes
 * written; `cap` should be at least FLAC_PREAMBLE_BYTES. */
size_t flac_seek_preamble(const flac_seek_t *fs, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
