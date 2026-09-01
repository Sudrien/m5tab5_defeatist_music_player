/*
 * tsseek.h -- seeking an MPEG transport stream by bisecting the
 * presentation timestamps in its PES headers.
 *
 * WHAT THE ARCHIVE SAYS, AGAIN
 *
 * 0705 established that reading the precompiled parser answers
 * questions its header does not, so this one was checked the same way
 * before a line was written. `ts_parse.c.obj` in
 * `libesp_audio_simple_dec.a` is 2894 bytes of RISC-V and it:
 *
 *   - compares against the 0x47 sync byte in nine places, so it
 *     resynchronises rather than requiring aligned input;
 *   - parses PAT and PMT and filters by PID, then reads PES headers;
 *   - contains exactly one four-bit mask, and it is the PSI section
 *     length's high nibble. **The continuity counter is not tracked.**
 *
 * A continuity counter that jumps is what a demuxer would use to notice
 * a seek, and this one does not look. So a packet from the middle of
 * the file is an ordinary packet, exactly as it was for Ogg.
 *
 * WHAT STILL HAS TO BE REPLAYED
 *
 * The PAT and the PMT. Without them the parser does not know which PID
 * carries audio or what codec is in it, and a fresh parser handed
 * mid-file packets would sit there filtering for a PID it has not been
 * told about. They are two 188-byte packets at the front of the file
 * and they are replayed verbatim -- the same shape as Ogg's header
 * pages and WAV's synthesised header, for the third time.
 *
 * WHY THE LATTICE MAKES THIS THE EASIEST OF THE THREE
 *
 * A transport stream is a fixed-size packet lattice: every packet
 * starts at `base + n * stride`, so unlike FLAC and Ogg there is no
 * resynchronisation to get right and no confirmation to construct. A
 * candidate offset either is on the lattice or is not, and the sync
 * byte confirms it.
 *
 * WHAT IT REFUSES
 *
 * PTS is 33 bits at 90 kHz, so it wraps every 26.5 hours, and a stream
 * spliced from two sources can restart it part way. A bisection needs a
 * monotonically increasing key; where the last timestamp is not after
 * the first, the key is not monotonic and there is nothing to search.
 * That is refused at the probe -- no seek, and no duration either --
 * rather than searched anyway, because a bisection over a non-monotonic
 * key does not fail, it converges on the wrong packet.
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

/* PAT and PMT, one packet each, at the largest stride handled. */
#define TS_PREAMBLE_MAX     (2 * 204)

typedef struct {
    bool     ok;
    long     base;              /* offset of the first packet */
    int      stride;            /* 188, 192 (m2ts) or 204 (with FEC) */
    int      packet;            /* 188 always; the rest is padding */
    uint16_t audio_pid;
    uint8_t  stream_type;
    long     file_end;
    uint64_t first_pts;         /* 90 kHz */
    uint64_t last_pts;

    uint8_t  preamble[TS_PREAMBLE_MAX];
    size_t   preamble_len;
} ts_seek_t;

/*
 * Find the lattice, the audio PID and the timestamp range. False for
 * anything that is not a transport stream this can search, which
 * includes one whose timestamps do not increase -- see above.
 *
 * The handle's position is saved and restored.
 */
bool ts_seek_probe(FILE *f, ts_seek_t *ts);

/* Seconds, or 0 when the range is not known. */
uint32_t ts_seek_duration_sec(const ts_seek_t *ts);

/*
 * Offset of the packet to resume from, or -1. `landed_sec` is where
 * that packet actually starts, which is at or before the target: the
 * caller anchors its clock to this rather than to the request, for the
 * reason decoder_seek_sec_at() gives.
 */
long ts_seek_find(FILE *f, const ts_seek_t *ts, uint32_t sec,
                  uint32_t *landed_sec);

/* PAT and PMT, for replay after a seek. */
const uint8_t *ts_seek_preamble(const ts_seek_t *ts, size_t *len);

#ifdef __cplusplus
}
#endif
