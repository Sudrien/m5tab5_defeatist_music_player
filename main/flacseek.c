/*
 * flacseek.c -- see flacseek.h for why this bisects rather than reads
 * the SEEKTABLE.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "flacseek.h"
#include "storage_io.h"

static const char *TAG = "tab5_flacsk";

/* PLAYBACK class: every caller is the decode loop, at the open or at a
 * drag, and both are moments the listener is waiting through. */
static bool read_at(FILE *f, long off, void *buf, size_t len)
{
    if (off < 0) return false;
    return storage_io_read_at(f, off, buf, len, STORAGE_IO_PLAYBACK);
}

static long file_size(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    return ftell(f);
}

static inline uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* ------------------------------------------------------------------ */
/* Frame headers                                                       */
/* ------------------------------------------------------------------ */

/*
 * The CRC-8 is what makes this reliable rather than a heuristic.
 *
 * Resyncing on a two-byte sync pattern alone is the trap every format
 * in this project sets -- an ID3 tag full of PNG, an AAC payload with
 * FF F1 in it -- and FLAC audio data is high-entropy, so 0xFF 0xF8
 * appears in it constantly. The header's own CRC-8 over its own bytes
 * turns "looks like a header" into "is a header" with a one-in-256
 * residual, and the field checks below plus the second-frame
 * confirmation take that the rest of the way.
 *
 * Polynomial 0x07, no reflection, zero init -- the one FLAC specifies.
 */
static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (uint8_t)((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
        }
    }
    return crc;
}

/* Coded number: UTF-8-alike, up to seven bytes for a 36-bit sample
 * number. Returns bytes consumed, or 0. */
static int coded_number(const uint8_t *p, size_t avail, uint64_t *out)
{
    if (!avail) return 0;
    const uint8_t c = p[0];
    int extra;
    uint64_t v;

    if (!(c & 0x80))        { *out = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; v = c & 0x07; }
    else if ((c & 0xFC) == 0xF8) { extra = 4; v = c & 0x03; }
    else if ((c & 0xFE) == 0xFC) { extra = 5; v = c & 0x01; }
    else if (c == 0xFE)          { extra = 6; v = 0; }
    else return 0;                          /* 0xFF, or a continuation byte */

    if (avail < (size_t)extra + 1) return 0;
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (uint64_t)(p[i] & 0x3F);
    }
    *out = v;
    return extra + 1;
}

static const uint32_t k_rates[16] = {
    0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
    32000, 44100, 48000, 96000, 0, 0, 0, 0
};
static const uint16_t k_bits[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

typedef struct {
    uint64_t sample;        /* first sample of the frame */
    uint32_t block;         /* samples in it */
    int      len;           /* header length in bytes */
} flac_frame_t;

/*
 * Parse a header at p. Everything the stream already told us in
 * STREAMINFO is checked against it: a header that declares a different
 * sample rate or channel count is not this stream's header, whatever
 * its CRC says.
 */
static bool frame_at(const uint8_t *p, size_t avail, const flac_seek_t *fs,
                     flac_frame_t *fr)
{
    if (avail < 5) return false;
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return false;

    const bool variable = (p[1] & 0x01) != 0;
    const uint8_t bs_bits = (uint8_t)(p[2] >> 4);
    const uint8_t sr_bits = (uint8_t)(p[2] & 0x0F);
    const uint8_t ch_bits = (uint8_t)(p[3] >> 4);
    const uint8_t sz_bits = (uint8_t)((p[3] >> 1) & 0x07);

    if (bs_bits == 0 || sr_bits == 0x0F) return false;
    if (p[3] & 0x01) return false;                  /* reserved bit */
    if (ch_bits > 10) return false;                 /* reserved assignments */
    if (sz_bits == 3) return false;                 /* reserved */

    const uint16_t chans = (ch_bits < 8) ? (uint16_t)(ch_bits + 1) : 2;
    if (chans != fs->channels) return false;
    if (sz_bits && k_bits[sz_bits] != fs->bits) return false;
    if (sr_bits && sr_bits < 0x0C && k_rates[sr_bits] != fs->sample_rate) {
        return false;
    }

    uint64_t num = 0;
    const int nlen = coded_number(p + 4, avail - 4, &num);
    if (!nlen) return false;
    size_t at = 4 + (size_t)nlen;

    uint32_t block;
    if (bs_bits == 6) {
        if (at + 1 > avail) return false;
        block = (uint32_t)p[at] + 1;
        at += 1;
    } else if (bs_bits == 7) {
        if (at + 2 > avail) return false;
        block = (((uint32_t)p[at] << 8) | p[at + 1]) + 1;
        at += 2;
    } else if (bs_bits == 1) {
        block = 192;
    } else if (bs_bits <= 5) {
        block = 576u << (bs_bits - 2);
    } else {
        block = 256u << (bs_bits - 8);
    }

    if (sr_bits == 0x0C) at += 1;
    else if (sr_bits == 0x0D || sr_bits == 0x0E) at += 2;
    if (at + 1 > avail) return false;

    if (crc8(p, at) != p[at]) return false;

    /*
     * Fixed blocking counts frames, variable blocking counts samples.
     * Multiplying a sample number by the block size, or failing to
     * multiply a frame number, puts the search in the wrong part of the
     * file by a factor of a few thousand -- and it would still converge,
     * on the wrong answer, because the search only requires the numbers
     * to be ordered.
     */
    fr->sample = variable ? num : num * (uint64_t)block;
    fr->block = block;
    fr->len = (int)at + 1;
    return true;
}

/*
 * First frame header at or after `at` in buf.
 *
 * Confirmed by the following frame where there is buffer left to look
 * in: a real header is followed, within one maximum frame, by another
 * whose sample number is exactly this one's plus its block size. That
 * is a far stronger statement than the CRC alone, and it is free
 * whenever the window is big enough.
 */
static long find_frame(const uint8_t *buf, size_t avail, const flac_seek_t *fs,
                       flac_frame_t *out)
{
    for (size_t i = 0; i + 5 <= avail; i++) {
        if (buf[i] != 0xFF) continue;
        flac_frame_t fr;
        if (!frame_at(buf + i, avail - i, fs, &fr)) continue;

        bool confirmed = true;
        for (size_t j = i + (size_t)fr.len; j + 5 <= avail; j++) {
            if (buf[j] != 0xFF) continue;
            flac_frame_t nx;
            if (!frame_at(buf + j, avail - j, fs, &nx)) continue;
            confirmed = (nx.sample == fr.sample + fr.block);
            break;
        }
        if (!confirmed) continue;

        *out = fr;
        return (long)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

bool flac_seek_probe(FILE *f, flac_seek_t *fs)
{
    if (!f || !fs) return false;

    const long saved = ftell(f);
    memset(fs, 0, sizeof(*fs));
    bool ok = false;

    uint8_t magic[4];
    if (read_at(f, 0, magic, 4) && memcmp(magic, "fLaC", 4) == 0) {
        /*
         * Walk the metadata blocks to find where the audio starts.
         * STREAMINFO is mandatory and always first, so it is read from
         * a fixed place; everything after it is skipped by its own
         * length, which is what makes a 500 KB PICTURE block cost one
         * seek rather than a scan.
         */
        long pos = 4;
        bool have_si = false;
        for (int guard = 0; guard < 128; guard++) {
            uint8_t h[4];
            if (!read_at(f, pos, h, 4)) break;
            const uint8_t type = (uint8_t)(h[0] & 0x7F);
            const bool last = (h[0] & 0x80) != 0;
            const uint32_t len = be24(h + 1);

            if (type == 0 && len == 34 && !have_si) {
                if (!read_at(f, pos + 4, fs->streaminfo, 34)) break;
                have_si = true;
            }
            pos += 4 + (long)len;
            if (last) break;
        }

        if (have_si) {
            const uint8_t *si = fs->streaminfo;
            fs->min_block = (uint16_t)(((uint16_t)si[0] << 8) | si[1]);
            fs->max_frame = be24(si + 5);
            fs->sample_rate = ((uint32_t)si[10] << 12) |
                              ((uint32_t)si[11] << 4) | (si[12] >> 4);
            fs->channels = (uint16_t)(((si[12] >> 1) & 0x07) + 1);
            fs->bits = (uint16_t)((((si[12] & 0x01) << 4) |
                                   (si[13] >> 4)) + 1);
            fs->total_samples = ((uint64_t)(si[13] & 0x0F) << 32) |
                                ((uint64_t)si[14] << 24) |
                                ((uint64_t)si[15] << 16) |
                                ((uint64_t)si[16] << 8) | si[17];
            fs->first_frame = pos;
            fs->file_end = file_size(f);
            ok = fs->sample_rate && fs->channels && fs->bits &&
                 fs->file_end > fs->first_frame;
        }
    }

    if (saved >= 0) fseek(f, saved, SEEK_SET);

    if (ok) {
        fs->ok = true;
        ESP_LOGI(TAG, "flac: %" PRIu32 " Hz, %u ch, %u bit, audio at %ld, "
                      "%" PRIu64 " samples",
                 fs->sample_rate, (unsigned)fs->channels, (unsigned)fs->bits,
                 fs->first_frame, fs->total_samples);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Bisection                                                           */
/* ------------------------------------------------------------------ */

/*
 * How much to read at each probe.
 *
 * It has to be able to hold a whole frame plus the start of the next
 * one, or the confirmation above never fires and every candidate is
 * rejected. STREAMINFO's max frame size states the number when the
 * encoder filled it in -- most do -- and 16 KB is the fallback, which
 * covers a 4096-sample stereo 16-bit frame several times over.
 */
#define PROBE_MIN   (16 * 1024)
#define PROBE_MAX   (96 * 1024)

static size_t probe_window(const flac_seek_t *fs)
{
    size_t w = fs->max_frame ? (size_t)fs->max_frame * 2 + 1024 : PROBE_MIN;
    if (w < PROBE_MIN) w = PROBE_MIN;
    if (w > PROBE_MAX) w = PROBE_MAX;
    return w;
}

long flac_seek_find(FILE *f, const flac_seek_t *fs, uint32_t sec,
                    uint64_t *landed_sample)
{
    if (!f || !fs || !fs->ok || !fs->sample_rate) return -1;

    uint64_t target = (uint64_t)sec * fs->sample_rate;
    if (fs->total_samples && target >= fs->total_samples) {
        target = fs->total_samples - 1;
    }

    const size_t win = probe_window(fs);
    uint8_t *buf = malloc(win);
    if (!buf) return -1;

    long lo = fs->first_frame, hi = fs->file_end;
    long best = -1;
    uint64_t best_sample = 0;

    /*
     * Bounded rather than "until lo >= hi". The invariant that makes a
     * bisection terminate is that the interval shrinks every round, and
     * this one shrinks by landing on a frame header whose position is
     * decided by the data -- so a pathological file could in principle
     * stall it. Twenty-four rounds is far more than the log2 of any
     * file a FAT volume can hold, and a bounded loop on the decode loop
     * is worth more than a proof.
     */
    for (int round = 0; round < 24 && lo < hi; round++) {
        long mid = lo + (hi - lo) / 2;
        if (mid < fs->first_frame) mid = fs->first_frame;

        long want = fs->file_end - mid;
        if (want > (long)win) want = (long)win;
        if (want < 5) break;
        if (!read_at(f, mid, buf, (size_t)want)) break;

        flac_frame_t fr;
        const long at = find_frame(buf, (size_t)want, fs, &fr);
        if (at < 0) {
            /* No header in this window. The audio is not where we
             * looked -- most likely we are in the last partial frame --
             * so give up on the upper half. */
            hi = mid;
            continue;
        }

        const long off = mid + at;
        if (fr.sample <= target) {
            best = off;
            best_sample = fr.sample;
            if (off <= lo) break;                   /* no progress possible */
            lo = off + 1;
        } else {
            hi = mid;
        }
    }

    /*
     * Nothing at or before the target, which happens when the target is
     * inside the very first frame: fall back to the start of the audio,
     * which is the honest answer rather than a failure.
     */
    if (best < 0) {
        long want = fs->file_end - fs->first_frame;
        if (want > (long)win) want = (long)win;
        flac_frame_t fr;
        if (want >= 5 && read_at(f, fs->first_frame, buf, (size_t)want)) {
            const long at = find_frame(buf, (size_t)want, fs, &fr);
            if (at >= 0) {
                best = fs->first_frame + at;
                best_sample = fr.sample;
            }
        }
    }

    free(buf);
    if (best < 0) return -1;
    if (landed_sample) *landed_sample = best_sample;
    return best;
}

size_t flac_seek_preamble(const flac_seek_t *fs, uint8_t *buf, size_t cap)
{
    if (!fs || !fs->ok || !buf || cap < FLAC_PREAMBLE_BYTES) return 0;
    memcpy(buf, "fLaC", 4);
    /* Last metadata block, type 0, length 34. Last, so the decoder
     * expects a frame next -- which is what it is about to get. */
    buf[4] = 0x80;
    buf[5] = 0x00;
    buf[6] = 0x00;
    buf[7] = 34;
    memcpy(buf + 8, fs->streaminfo, 34);
    return FLAC_PREAMBLE_BYTES;
}
