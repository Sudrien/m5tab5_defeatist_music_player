/*
 * tsseek.c -- see tsseek.h, including what the precompiled parser was
 * found to ignore.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "tsseek.h"
#include "storage_io.h"

static const char *TAG = "tab5_tssk";

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

#define TS_PKT      188
#define PTS_HZ      90000u

/* ------------------------------------------------------------------ */
/* Packets                                                             */
/* ------------------------------------------------------------------ */

static inline uint16_t pkt_pid(const uint8_t *p)
{
    return (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
}

static inline bool pkt_unit_start(const uint8_t *p)
{
    return (p[1] & 0x40) != 0;
}

/*
 * Payload offset within a packet, or -1 when there is none.
 *
 * The adaptation field's length byte is not included in its own length,
 * and a length of 183 is a packet that is all adaptation field -- both
 * are the ordinary ways to read one byte past the end here.
 */
static int pkt_payload(const uint8_t *p)
{
    if (p[0] != 0x47) return -1;
    if (p[1] & 0x80) return -1;                 /* transport error */
    const uint8_t afc = (uint8_t)((p[3] >> 4) & 0x03);
    if (!(afc & 0x01)) return -1;               /* no payload */
    if (!(afc & 0x02)) return 4;
    const int af = p[4];
    if (4 + 1 + af > TS_PKT) return -1;
    return 4 + 1 + af;
}

/*
 * PTS out of a PES header, if it has one.
 *
 * Only the first packet of a PES packet carries the header, which is
 * what payload_unit_start_indicator says, so this is only ever asked of
 * those.
 */
static bool pes_pts(const uint8_t *p, int len, uint64_t *pts)
{
    if (len < 14) return false;
    if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) return false;
    /* Audio stream_id is 110x xxxx. Anything else here is video or a
     * private stream and its timestamps are not this track's. */
    if ((p[3] & 0xE0) != 0xC0) return false;
    if ((p[6] & 0xC0) != 0x80) return false;    /* not an MPEG-2 PES header */
    if (!(p[7] & 0x80)) return false;           /* no PTS present */
    if (p[8] < 5) return false;

    const uint8_t *t = p + 9;
    *pts = ((uint64_t)(t[0] & 0x0E) << 29) |
           ((uint64_t)t[1] << 22) |
           ((uint64_t)(t[2] & 0xFE) << 14) |
           ((uint64_t)t[3] << 7) |
           ((uint64_t)t[4] >> 1);
    return true;
}

/* PTS of the packet at `off`, if it starts a PES packet on the audio
 * PID. Reads one packet. */
static bool pts_at(FILE *f, const ts_seek_t *ts, long off, uint64_t *pts)
{
    uint8_t p[TS_PKT];
    if (off + TS_PKT > ts->file_end) return false;
    if (!read_at(f, off, p, TS_PKT)) return false;
    if (p[0] != 0x47) return false;
    if (pkt_pid(p) != ts->audio_pid) return false;
    if (!pkt_unit_start(p)) return false;
    const int at = pkt_payload(p);
    if (at < 0) return false;
    return pes_pts(p + at, TS_PKT - at, pts);
}

/*
 * The next packet at or after index `n` that carries a PTS, searching
 * forward. Returns its index, or -1 past `limit`.
 *
 * One packet read per step. A stream interleaved with video can put a
 * few hundred packets between audio PES headers, which is a few tens of
 * KB -- bounded, and the bisection only does this a handful of times
 * per drag.
 */
static long scan_pts(FILE *f, const ts_seek_t *ts, long n, long limit,
                     uint64_t *pts)
{
    for (long i = n; i < limit; i++) {
        if (pts_at(f, ts, ts->base + i * ts->stride, pts)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* PSI                                                                 */
/* ------------------------------------------------------------------ */

/*
 * A PSI section begins after a pointer_field in the first payload byte.
 * Sections spanning packets are not followed: a PAT is 4 bytes of
 * payload and a PMT for a radio stream is a few dozen, so both fit in
 * one packet in every stream anybody will play here, and a
 * multi-packet-PMT reassembler is a parser for a case that does not
 * arise.
 */
static const uint8_t *psi_section(const uint8_t *p, int *len)
{
    const int at = pkt_payload(p);
    if (at < 0 || !pkt_unit_start(p)) return NULL;
    const int ptr = p[at];
    const int start = at + 1 + ptr;
    if (start + 3 > TS_PKT) return NULL;

    const uint8_t *sec = p + start;
    const int seclen = (int)(((sec[1] & 0x0F) << 8) | sec[2]);
    if (seclen < 9 || start + 3 + seclen > TS_PKT) return NULL;
    *len = seclen;
    return sec;
}

/* An audio stream type, by the numbers that actually appear. */
static bool is_audio_type(uint8_t t)
{
    return t == 0x03 || t == 0x04 ||        /* MPEG-1/2 audio */
           t == 0x0F ||                     /* AAC in ADTS */
           t == 0x11 ||                     /* AAC in LATM */
           t == 0x81 || t == 0x87;          /* AC-3, E-AC-3 */
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

/*
 * The lattice. 188 is the format; 192 is m2ts, which prefixes a
 * four-byte arrival timestamp; 204 is 188 with Reed-Solomon parity
 * appended. All three put a sync byte at a fixed stride, so finding the
 * stride is checking a handful of them rather than parsing anything.
 */
static bool find_lattice(FILE *f, long end, long *base, int *stride)
{
    static const int k_strides[] = { 188, 192, 204 };
    uint8_t win[1024];

    long want = end < (long)sizeof(win) ? end : (long)sizeof(win);
    if (want < TS_PKT || !read_at(f, 0, win, (size_t)want)) return false;

    for (size_t s = 0; s < sizeof(k_strides) / sizeof(k_strides[0]); s++) {
        const int st = k_strides[s];
        for (long off = 0; off < st && off + 4 * st < want; off++) {
            if (win[off] != 0x47) continue;
            bool all = true;
            for (int k = 1; k <= 4; k++) {
                if (win[off + (long)k * st] != 0x47) { all = false; break; }
            }
            if (all) { *base = off; *stride = st; return true; }
        }
    }
    return false;
}

bool ts_seek_probe(FILE *f, ts_seek_t *ts)
{
    if (!f || !ts) return false;

    const long saved = ftell(f);
    memset(ts, 0, sizeof(*ts));
    bool ok = false;

    const long end = file_size(f);
    if (end < TS_PKT) goto out;
    ts->file_end = end;
    ts->packet = TS_PKT;

    if (!find_lattice(f, end, &ts->base, &ts->stride)) goto out;

    const long packets = (end - ts->base) / ts->stride;
    if (packets < 4) goto out;

    /*
     * PAT then PMT, both captured verbatim for the replay. The search
     * is bounded rather than run to the end of the file: a stream that
     * has not announced itself in its first few hundred packets is not
     * one this player is going to make sense of, and the bound is what
     * stops a mislabelled file becoming a whole-file read at open.
     */
    long limit = packets < 512 ? packets : 512;
    uint16_t pmt_pid = 0xFFFF;
    uint8_t pkt[TS_PKT];

    for (long i = 0; i < limit && pmt_pid == 0xFFFF; i++) {
        if (!read_at(f, ts->base + i * ts->stride, pkt, TS_PKT)) goto out;
        if (pkt[0] != 0x47 || pkt_pid(pkt) != 0) continue;
        int len;
        const uint8_t *sec = psi_section(pkt, &len);
        if (!sec || sec[0] != 0x00) continue;

        /* 8 bytes of section header, then four per program, then a
         * four-byte CRC. The first program with a nonzero number is the
         * one; number zero is the network information table. */
        for (int at = 8; at + 4 <= 3 + len - 4; at += 4) {
            const uint16_t prog = (uint16_t)((sec[at] << 8) | sec[at + 1]);
            if (!prog) continue;
            pmt_pid = (uint16_t)(((sec[at + 2] & 0x1F) << 8) | sec[at + 3]);
            break;
        }
        if (pmt_pid != 0xFFFF) {
            memcpy(ts->preamble, pkt, TS_PKT);
            ts->preamble_len = TS_PKT;
        }
    }
    if (pmt_pid == 0xFFFF || !ts->preamble_len) goto out;

    for (long i = 0; i < limit && !ts->audio_pid; i++) {
        if (!read_at(f, ts->base + i * ts->stride, pkt, TS_PKT)) goto out;
        if (pkt[0] != 0x47 || pkt_pid(pkt) != pmt_pid) continue;
        int len;
        const uint8_t *sec = psi_section(pkt, &len);
        if (!sec || sec[0] != 0x02) continue;

        const int info_len = (int)(((sec[10] & 0x0F) << 8) | sec[11]);
        int at = 12 + info_len;
        const int stop = 3 + len - 4;
        uint16_t first_pid = 0;
        uint8_t first_type = 0;

        while (at + 5 <= stop) {
            const uint8_t type = sec[at];
            const uint16_t pid = (uint16_t)(((sec[at + 1] & 0x1F) << 8) |
                                            sec[at + 2]);
            const int es_len = (int)(((sec[at + 3] & 0x0F) << 8) | sec[at + 4]);
            if (!first_pid) { first_pid = pid; first_type = type; }
            if (is_audio_type(type)) {
                ts->audio_pid = pid;
                ts->stream_type = type;
                break;
            }
            at += 5 + es_len;
        }
        /* A stream type this table does not list is still worth trying
         * if it is the only elementary stream in the programme -- the
         * decoder will say so if it cannot read it, and refusing here
         * would cost a seek on a file that plays. */
        if (!ts->audio_pid && first_pid) {
            ts->audio_pid = first_pid;
            ts->stream_type = first_type;
        }
        if (ts->audio_pid) {
            memcpy(ts->preamble + ts->preamble_len, pkt, TS_PKT);
            ts->preamble_len += TS_PKT;
        }
    }
    if (!ts->audio_pid) goto out;

    /* The ends of the timestamp range. The first is near the front; the
     * last is found by walking back from the end, which is bounded
     * because an audio PES header appears every few frames. */
    if (scan_pts(f, ts, 0, limit, &ts->first_pts) < 0) goto out;

    for (long i = packets - 1; i >= 0 && i > packets - 4096; i--) {
        if (pts_at(f, ts, ts->base + i * ts->stride, &ts->last_pts)) break;
    }

    /*
     * A bisection needs a key that increases. PTS is 33 bits at 90 kHz,
     * so it wraps every 26.5 hours, and a spliced stream can restart it
     * part way. Either way the key is not monotonic and there is
     * nothing here to search -- refused rather than searched anyway,
     * because a bisection over a non-monotonic key does not fail, it
     * converges on the wrong packet.
     */
    if (ts->last_pts <= ts->first_pts) {
        ESP_LOGI(TAG, "ts timestamps do not increase; not seekable");
        goto out;
    }

    ok = true;

out:
    if (saved >= 0) fseek(f, saved, SEEK_SET);
    if (ok) {
        ts->ok = true;
        ESP_LOGI(TAG, "ts: stride %d, audio pid %u type 0x%02x, %" PRIu32 " s",
                 ts->stride, (unsigned)ts->audio_pid,
                 (unsigned)ts->stream_type, ts_seek_duration_sec(ts));
    } else {
        memset(ts, 0, sizeof(*ts));
    }
    return ok;
}

uint32_t ts_seek_duration_sec(const ts_seek_t *ts)
{
    if (!ts || ts->last_pts <= ts->first_pts) return 0;
    return (uint32_t)((ts->last_pts - ts->first_pts) / PTS_HZ);
}

/* ------------------------------------------------------------------ */
/* Bisection                                                           */
/* ------------------------------------------------------------------ */

/*
 * How far forward a probe may look for a packet carrying a timestamp
 * before giving up on that half of the interval.
 *
 * A stream with video in it puts hundreds of packets between audio PES
 * headers. This is generous enough for that and bounded enough that a
 * file with one stray audio packet cannot turn a drag into a whole-file
 * read.
 */
#define TS_SCAN_PACKETS  2048

long ts_seek_find(FILE *f, const ts_seek_t *ts, uint32_t sec,
                  uint32_t *landed_sec)
{
    if (!f || !ts || !ts->ok) return -1;

    const long packets = (ts->file_end - ts->base) / ts->stride;
    uint64_t target = ts->first_pts + (uint64_t)sec * PTS_HZ;
    if (target >= ts->last_pts) target = ts->last_pts;

    long lo = 0, hi = packets;
    long best = -1;
    uint64_t best_pts = ts->first_pts;

    /* Bounded, as flacseek.c and oggseek.c are, and for the same
     * reason: each round narrows by landing on a packet the data
     * chooses, and a bound on the decode loop beats a proof. */
    for (int round = 0; round < 32 && lo < hi; round++) {
        const long mid = lo + (hi - lo) / 2;

        uint64_t pts = 0;
        long scan_to = mid + TS_SCAN_PACKETS;
        if (scan_to > hi) scan_to = hi;
        const long at = scan_pts(f, ts, mid, scan_to, &pts);

        if (at < 0) {
            /* Nothing timestamped in the upper half within reach. */
            hi = mid;
            continue;
        }
        if (pts <= target) {
            best = at;
            best_pts = pts;
            if (at + 1 <= lo) break;
            lo = at + 1;
        } else {
            hi = mid;
        }
    }

    if (best < 0) {
        /* The target is inside the first PES packet, or every candidate
         * fell the other side. The start of the audio is the honest
         * answer and the clock is anchored to it, so the two agree. */
        uint64_t pts = 0;
        best = scan_pts(f, ts, 0, packets < TS_SCAN_PACKETS
                                  ? packets : TS_SCAN_PACKETS, &pts);
        if (best < 0) return -1;
        best_pts = pts;
    }

    if (landed_sec) {
        *landed_sec = (best_pts > ts->first_pts)
                    ? (uint32_t)((best_pts - ts->first_pts) / PTS_HZ) : 0;
    }
    return ts->base + best * ts->stride;
}

const uint8_t *ts_seek_preamble(const ts_seek_t *ts, size_t *len)
{
    if (!ts || !ts->ok || !ts->preamble_len) { if (len) *len = 0; return NULL; }
    if (len) *len = ts->preamble_len;
    return ts->preamble;
}
