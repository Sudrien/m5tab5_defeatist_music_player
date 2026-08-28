/*
 * framewalk.c
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "framewalk.h"
#include "storage_io.h"

static const char *TAG = "tab5_walk";

/* Frames held before resampling. A 4 minute MP3 is about 9200 frames and
 * AMR is 50/s, so 65536 covers roughly 22 minutes of the densest format.
 * Beyond that the walk still counts frames -- only the envelope stops
 * gaining detail, which at 548 columns is invisible. */
#define MAX_FRAMES      (65536)

/* Read buffer. Large enough that the walk is one big sequential read per
 * chunk rather than a syscall per frame; a 128k MP3 frame is 417 bytes,
 * so this is ~78 frames per refill. */
#define BUF_SIZE        (32 * 1024)

/* Two frames' worth of slack must always be available ahead of the parse
 * point, or a header straddling the end of the buffer is missed. The
 * largest frame here is an MP3 at 320k/32kHz, 1440 bytes. */
#define MAX_FRAME_LEN   (2048)

typedef struct {
    FILE *f;
    uint8_t *buf;
    size_t len;             /* valid bytes in buf */
    size_t pos;             /* parse point */
    bool eof;
} reader_t;

static bool fill(reader_t *r)
{
    if (r->eof && r->pos >= r->len) return false;

    /* Move the tail down and top up. memmove rather than a ring buffer:
     * the tail is at most MAX_FRAME_LEN, so this is a 2 KB copy per
     * 32 KB read. */
    if (r->pos > 0) {
        const size_t keep = r->len - r->pos;
        memmove(r->buf, r->buf + r->pos, keep);
        r->len = keep;
        r->pos = 0;
    }
    if (r->len >= BUF_SIZE) return true;

    /* Background class, and the reason the arbiter exists. This is the
     * 32 KB read that used to sit in front of the decoder's next refill
     * holding FatFs's volume lock; it is now two 16 KB leases that let
     * go in between, so the decoder waits for a chunk rather than for
     * the rest of the file. The buffer size is unchanged -- only the
     * number of times this lets go is. */
    const size_t got = storage_io_fread(r->buf + r->len, BUF_SIZE - r->len,
                                        r->f, STORAGE_IO_BACKGROUND);
    r->len += got;
    if (got == 0) r->eof = true;
    return r->len > 0;
}

/* Bytes available ahead of the parse point. */
static inline size_t avail(const reader_t *r) { return r->len - r->pos; }

static bool ensure(reader_t *r, size_t need)
{
    if (avail(r) >= need) return true;
    fill(r);
    return avail(r) >= need;
}

/* ------------------------------------------------------------------ */
/* MP3                                                                 */
/* ------------------------------------------------------------------ */

static const uint16_t k_mp3_bitrate_v1l3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const uint16_t k_mp3_bitrate_v2l3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};
static const uint32_t k_mp3_rate[4] = { 44100, 48000, 32000, 0 };

/* Big-endian bit reader over a frame, for the side info. */
static uint32_t bits(const uint8_t *p, int bit_off, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        const int b = bit_off + i;
        v = (v << 1) | ((p[b >> 3] >> (7 - (b & 7))) & 1);
    }
    return v;
}

/*
 * global_gain of the first granule, first channel.
 *
 * The side info layout is fixed, so this is a constant bit offset rather
 * than a parse. MPEG1: main_data_begin(9) + private(3 stereo / 5 mono) +
 * scfsi(4 per channel), then part2_3_length(12) + big_values(9) before
 * the gain. MPEG2/2.5 drops scfsi entirely and uses 8 + 1 or 2.
 *
 * It is a 8-bit log-scale quantiser gain -- a proxy for loudness, not a
 * peak. Which is the right thing for a waveform anyway: peak alone
 * flattens every mastered track into the same slab.
 */
static int mp3_global_gain(const uint8_t *fr, bool lsf, bool mono, bool crc)
{
    /* A CRC puts two bytes between the header and the side info. Shifted
     * past, not skipped: the previous version dropped the loudness for
     * every protected frame, and a file where every frame is protected
     * produced no envelope at all with nothing to say why. */
    const int hdr = crc ? 6 : 4;
    int off;
    if (!lsf) off = mono ? (9 + 5 + 4) : (9 + 3 + 8);
    else      off = mono ? (8 + 1)     : (8 + 2);

    /*
     * part2_3_length == 0 means the granule carries no main data at all:
     * no scalefactors, no Huffman data, nothing to apply a gain to. It is
     * digital silence, and global_gain is then whatever the encoder
     * happened to leave in the field -- LAME writes 210, and something in
     * the run writes 255.
     *
     * This is the spike at each end of every envelope. It was read as
     * metadata, and it is not: the ID3v2 tag at the front and the ID3v1
     * tag at the back are both already stepped over, and the file this
     * was chased on has exactly 51 of these frames, seven at the start
     * and the rest trailing. They are the encoder delay at the head and
     * the flush padding at the tail -- real MP3 frames, correctly parsed,
     * containing silence and reporting a gain of 210 for it. Read
     * literally, a track opens and closes at four fifths of full scale.
     *
     * Reported as 0 so the run of silence lands at the bottom of the
     * range, which is both what it sounds like and, because span() then
     * normalises from the minimum, what stops it dragging the scale.
     */
    if (bits(fr + hdr, off, 12) == 0) return 0;

    off += 12 + 9;          /* part2_3_length, big_values */
    return (int)bits(fr + hdr, off, 8);
}

/* Returns frame length in bytes, or 0 if this is not a valid header. */
static int mp3_frame_len(const uint8_t *h, uint32_t *rate, uint32_t *spf,
                         bool *lsf, bool *mono, bool *crc)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;

    const int ver = (h[1] >> 3) & 3;        /* 3=MPEG1 2=MPEG2 0=MPEG2.5 */
    const int layer = (h[1] >> 1) & 3;      /* 1 = Layer III */
    if (ver == 1 || layer != 1) return 0;

    const int br_i = (h[2] >> 4) & 0x0F;
    const int sr_i = (h[2] >> 2) & 3;
    if (br_i == 0 || br_i == 15 || sr_i == 3) return 0;

    const bool is_lsf = (ver != 3);
    uint32_t sr = k_mp3_rate[sr_i];
    if (ver == 2) sr /= 2;
    else if (ver == 0) sr /= 4;

    const uint32_t br = 1000u * (is_lsf ? k_mp3_bitrate_v2l3[br_i]
                                        : k_mp3_bitrate_v1l3[br_i]);
    const uint32_t samples = is_lsf ? 576 : 1152;
    const int pad = (h[2] >> 1) & 1;

    *rate = sr;
    *spf = samples;
    *lsf = is_lsf;
    *mono = (((h[3] >> 6) & 3) == 3);
    *crc = ((h[1] & 1) == 0);               /* bit clear means CRC present */

    return (int)((samples / 8) * br / sr) + pad;
}

/* ------------------------------------------------------------------ */
/* ADTS (raw AAC)                                                      */
/* ------------------------------------------------------------------ */

static const uint32_t k_adts_rate[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

static int adts_frame_len(const uint8_t *h, uint32_t *rate)
{
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) return 0;
    const int sr_i = (h[2] >> 2) & 0x0F;
    if (!k_adts_rate[sr_i]) return 0;
    *rate = k_adts_rate[sr_i];
    const int len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
    return (len >= 7) ? len : 0;
}

/* ------------------------------------------------------------------ */
/* AMR-NB                                                              */
/* ------------------------------------------------------------------ */

/* Bytes per frame including the mode byte, indexed by mode. 15 is NO_DATA
 * (1 byte), 9-14 are reserved. */
static const uint8_t k_amr_len[16] = {
    13, 14, 16, 18, 20, 21, 27, 32, 6, 0, 0, 0, 0, 0, 0, 1
};

/* ------------------------------------------------------------------ */
/* Ogg (Vorbis, Opus)                                                  */
/* ------------------------------------------------------------------ */

/*
 * Packet size as the loudness proxy, with no decoding at all.
 *
 * Vorbis and Opus have nothing like MP3's global_gain -- there is no
 * loudness in the header, so an amplitude envelope means decoding one
 * packet in N, which is minutes of CPU on a long track and a second codec
 * instance live alongside the one making the sound.
 *
 * But both codecs are always VBR, and that is the way in: a VBR encoder
 * spends bits where there is something to encode. A silent passage is a
 * handful of bytes per packet and a dense one is several hundred. What
 * comes out is a bitrate envelope rather than an amplitude envelope --
 * not the same measurement, but it rises and falls in the same places,
 * which is all a waveform drawn 548 px wide can show anyway.
 *
 * The cost is one sequential read and no decode, so Ogg scans at the same
 * speed as MP3 instead of being the slow format.
 */
static bool ogg_walk(reader_t *r, volatile bool *abort_flag,
                     uint8_t *gains, uint32_t *out_stored,
                     uint32_t *out_packets, uint64_t *out_granule,
                     uint32_t *out_rate)
{
    uint32_t packets = 0, stored = 0, rate = 0;
    uint64_t granule = 0;
    uint32_t pkt_bytes = 0;
    uint32_t since_poll = 0;

    /*
     * Header packets are not audio and must not become columns.
     *
     * Opus has two (OpusHead, OpusTags) and Vorbis three (identification,
     * comment, setup). The big one is Vorbis's setup packet -- several KB
     * of codebooks -- and OpusTags carries the comment block and any
     * embedded picture. Both clamp to 255, which is why every Ogg drew a
     * full-height spike in its first column.
     *
     * -1 means the codec is not identified yet, which is the state for
     * exactly the first packet.
     */
    int skip_headers = -1;

    while (1) {
        if (!ensure(r, 27)) break;
        const uint8_t *h = r->buf + r->pos;
        if (memcmp(h, "OggS", 4) != 0) {
            /* Resync. Pages are the only structure here, so a byte at a
             * time is correct -- unlike MP3 there is no compressed image
             * data to get lost in. */
            r->pos++;
            continue;
        }

        const int nsegs = h[26];
        if (!ensure(r, (size_t)27 + nsegs)) break;
        const uint8_t *segs = h + 27;

        /* Identify the codec before walking this page's segments, so the
         * header count is known by the time the first packet ends. */
        if (!rate && 27 + nsegs + 16 <= (int)avail(r)) {
            const uint8_t *p = segs + nsegs;
            if (memcmp(p, "OpusHead", 8) == 0) {
                rate = 48000;           /* granule is always 48 kHz */
                skip_headers = 2;
            } else if (p[0] == 0x01 && memcmp(p + 1, "vorbis", 6) == 0) {
                rate = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                       ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
                skip_headers = 3;
            }
        }

        /* Little-endian 64-bit granule at +6. The last page's value is
         * the sample count, which is where the duration comes from. */
        uint64_t g = 0;
        for (int i = 7; i >= 0; i--) g = (g << 8) | h[6 + i];
        if (g != UINT64_MAX) granule = g;

        uint32_t payload = 0;
        for (int i = 0; i < nsegs; i++) {
            payload += segs[i];
            pkt_bytes += segs[i];
            /* A segment shorter than 255 ends a packet. */
            if (segs[i] < 255) {
                if (skip_headers > 0) {
                    skip_headers--;
                    pkt_bytes = 0;
                    continue;
                }
                if (stored < MAX_FRAMES) {
                    /* >>2 puts a typical 100-800 byte audio packet into
                     * 25-200, which span() then stretches. Clamped rather
                     * than scaled adaptively: a header packet carrying a
                     * comment block and cover art would otherwise set the
                     * ceiling for the whole track. */
                    uint32_t v = pkt_bytes >> 2;
                    if (v > 255) v = 255;
                    gains[stored++] = (uint8_t)v;
                }
                packets++;
                pkt_bytes = 0;
            }
        }

        if (!rate) {
            /* Identification header is the first packet of the stream. */
            const uint8_t *p = segs + nsegs;
            if (27 + nsegs + 16 <= (int)avail(r)) {
                if (memcmp(p, "OpusHead", 8) == 0) {
                    rate = 48000;   /* granule is always 48 kHz for Opus */
                } else if (p[0] == 0x01 && memcmp(p + 1, "vorbis", 6) == 0) {
                    rate = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                           ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
                }
            }
        }

        const size_t page = (size_t)27 + nsegs + payload;
        if (page > avail(r)) {
            /* Page runs past what is buffered. Refill and, if it still
             * does not fit, give up rather than spin. */
            if (!ensure(r, page)) break;
        }
        r->pos += page;

        if (abort_flag && ++since_poll >= 64) {
            since_poll = 0;
            if (*abort_flag) return false;
        }
    }

    /*
     * The last packet is the end of the stream, not the end of the music.
     * Opus pads the final packet and both codecs can finish with a short
     * or padded one; either way its size says nothing about the audio and
     * it landed in the final column as a spike. Held at the previous
     * value rather than dropped, so the envelope still spans the width.
     */
    if (stored >= 2) gains[stored - 1] = gains[stored - 2];

    *out_stored = stored;
    *out_packets = packets;
    *out_granule = granule;
    *out_rate = rate;
    return true;
}

/* ------------------------------------------------------------------ */

static void resample(const uint8_t *src, uint32_t n, framewalk_t *out)
{
    const int cols = out->columns;
    if (!n || cols <= 0) return;

    for (int c = 0; c < cols; c++) {
        const uint32_t a = (uint32_t)(((uint64_t)c * n) / cols);
        uint32_t b = (uint32_t)(((uint64_t)(c + 1) * n) / cols);
        if (b <= a) b = a + 1;
        if (b > n) b = n;

        /* Max within the bucket, not mean. A mean over 20 frames turns a
         * drum hit into a bump; the envelope should keep the transient,
         * which is the part that makes one track look unlike another. */
        uint8_t m = 0;
        for (uint32_t i = a; i < b; i++) if (src[i] > m) m = src[i];
        out->level[c] = m;
    }
}

bool framewalk_supports(FILE *f)
{
    if (!f) return false;
    const long saved = ftell(f);
    uint8_t b[4];
    const bool got = (fread(b, 1, sizeof(b), f) == sizeof(b));
    if (saved >= 0) fseek(f, saved, SEEK_SET);
    if (!got) return false;

    if (memcmp(b, "ID3", 3) == 0) return true;              /* MP3 */
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return true; /* MP3 or ADTS */
    if (memcmp(b, "#!AM", 4) == 0) return true;             /* AMR */
    if (memcmp(b, "OggS", 4) == 0) return true;             /* Vorbis/Opus */
    return false;
}

esp_err_t framewalk_scan(FILE *f, int columns, volatile bool *abort_flag,
                         framewalk_t *out)
{
    if (!f || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (columns < 1) columns = 1;
    if (columns > FRAMEWALK_MAX_COLUMNS) columns = FRAMEWALK_MAX_COLUMNS;
    out->columns = columns;

#ifdef ESP_PLATFORM
    uint8_t *buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *gains = heap_caps_malloc(MAX_FRAMES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint8_t *buf = malloc(BUF_SIZE);
    uint8_t *gains = malloc(MAX_FRAMES);
#endif
    if (!buf || !gains) { free(buf); free(gains); return ESP_ERR_NO_MEM; }

    reader_t r = { .f = f, .buf = buf };
    esp_err_t ret = ESP_OK;

    uint32_t frames = 0, rate = 0, spf = 0, stored = 0;
    int kind = 0;           /* 1 mp3, 2 adts, 3 amr */
    bool levels = false;

    /*
     * Skip an ID3v2 tag before scanning for sync.
     *
     * This is not an optimisation. The tag holds the album art -- 130 KB
     * of PNG on the file this was first tested against -- and PNG data is
     * full of bytes that look like an MP3 sync word. Resyncing a byte at
     * a time through it locks onto noise, parses nonsense frame lengths
     * and walks off into the middle of the image.
     *
     * The size field is syncsafe: four bytes, seven bits each, high bit
     * always clear, so the length itself can never contain a false sync.
     */
    if (ensure(&r, 10) && memcmp(r.buf + r.pos, "ID3", 3) == 0) {
        const uint8_t *h = r.buf + r.pos;
        uint32_t tag = ((uint32_t)(h[6] & 0x7F) << 21) |
                       ((uint32_t)(h[7] & 0x7F) << 14) |
                       ((uint32_t)(h[8] & 0x7F) << 7) |
                       (uint32_t)(h[9] & 0x7F);
        tag += 10;
        if (h[5] & 0x10) tag += 10;             /* footer present */

        /* Too big for the buffer, so seek rather than read through it. */
        r.pos = 0;
        r.len = 0;
        r.eof = false;
        if (fseek(f, (long)tag, SEEK_SET) != 0) { ret = ESP_ERR_INVALID_STATE; goto done; }
    }

    /* Ogg and AMR announce themselves; MP3 and ADTS are found by sync
     * scanning. */
    if (ensure(&r, 4) && memcmp(r.buf + r.pos, "OggS", 4) == 0) {
        uint32_t packets = 0;
        uint64_t granule = 0;
        if (!ogg_walk(&r, abort_flag, gains, &stored, &packets, &granule,
                      &rate)) {
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        out->frames = packets;
        out->rate = rate;
        out->has_levels = (stored > 0);
        if (rate && granule) out->sec = (uint32_t)(granule / rate);
        if (stored) resample(gains, stored, out);
        free(buf);
        free(gains);
        ESP_LOGI(TAG, "ogg walk: %" PRIu32 " packets, %" PRIu32 "s",
                 packets, out->sec);
        return ESP_OK;
    }

    /* AMR announces itself; the other two are found by sync scanning. */
    if (ensure(&r, 6) && memcmp(r.buf + r.pos, "#!AMR\n", 6) == 0) {
        r.pos += 6;
        kind = 3;
        rate = 8000;
    }

    uint32_t since_poll = 0;

    while (1) {
        if (!ensure(&r, MAX_FRAME_LEN) && avail(&r) < 8) break;

        if (abort_flag && ++since_poll >= 256) {
            since_poll = 0;
            if (*abort_flag) { ret = ESP_ERR_INVALID_STATE; goto done; }
        }

        const uint8_t *p = r.buf + r.pos;

        /*
         * Stop at a trailing tag rather than resyncing through it.
         *
         * ID3v1 is 128 bytes at EOF and APE tags are larger; both are
         * arbitrary bytes after the last frame, and the byte-at-a-time
         * resync happily parsed them into nonsense frames whose side info
         * became the spike at the end of the envelope. Same class of bug
         * as the ID3v2 tag at the front, at the other end of the file.
         */
        if (avail(&r) >= 8 &&
            (memcmp(p, "TAG", 3) == 0 || memcmp(p, "APETAGEX", 8) == 0 ||
             memcmp(p, "LYRICSBEGIN", 8) == 0)) {
            break;
        }

        if (kind == 3) {
            const int mode = (p[0] >> 3) & 0x0F;
            const int len = k_amr_len[mode];
            if (len == 0) break;                    /* reserved: give up */
            if (avail(&r) < (size_t)len) break;
            r.pos += (size_t)len;
            frames++;
            continue;
        }

        uint32_t fr_rate = 0, fr_spf = 0;
        bool lsf = false, mono = false, crc = false;
        int len = 0;

        if (kind == 0 || kind == 1) {
            len = mp3_frame_len(p, &fr_rate, &fr_spf, &lsf, &mono, &crc);
            /*
             * Before trusting the first sync, check that another header
             * sits exactly where this one says the next frame starts.
             *
             * One valid-looking header proves nothing -- 0xFF followed by
             * a plausible second byte turns up constantly in compressed
             * image data. Two in a row at the stated spacing does not.
             * Only the first frame is confirmed this way; once locked,
             * the stream is trusted until a header fails to parse.
             */
            if (len > 0 && kind == 0) {
                uint32_t r2 = 0, s2 = 0;
                bool l2 = false, m2 = false, c2 = false;
                if ((size_t)len + 4 > avail(&r) ||
                    mp3_frame_len(p + len, &r2, &s2, &l2, &m2, &c2) <= 0 ||
                    r2 != fr_rate) {
                    len = 0;
                }
            }
            if (len > 0) kind = 1;
        }
        if (len == 0 && (kind == 0 || kind == 2)) {
            len = adts_frame_len(p, &fr_rate);
            if (len > 0 && kind == 0) {
                uint32_t r2 = 0;
                if ((size_t)len + 7 > avail(&r) ||
                    adts_frame_len(p + len, &r2) <= 0 || r2 != fr_rate) {
                    len = 0;
                }
            }
            if (len > 0) { kind = 2; fr_spf = 1024; }
        }

        if (len <= 0 || (size_t)len > avail(&r)) {
            /* Not a frame here. Advance one byte and keep looking --
             * this is what walks past an ID3v2 tag, album art and any
             * junk between frames without needing to parse it. */
            r.pos++;
            if (avail(&r) < 8 && r.eof) break;
            continue;
        }

        if (!rate) { rate = fr_rate; spf = fr_spf; }

        /*
         * The Xing/Info/VBRI frame is a real MP3 frame carrying the
         * seek table, not audio. Its side info is whatever the encoder
         * left there, so it became an arbitrary first column -- the spike
         * at the start of every MP3 envelope.
         *
         * Only ever the first frame, so the test costs one comparison per
         * track rather than per frame.
         */
        if (kind == 1 && frames == 0 && (size_t)len <= avail(&r)) {
            const int probe = (len < 200) ? len : 200;
            for (int i = 4; i + 4 <= probe; i++) {
                if (memcmp(p + i, "Xing", 4) == 0 ||
                    memcmp(p + i, "Info", 4) == 0 ||
                    memcmp(p + i, "VBRI", 4) == 0) {
                    r.pos += (size_t)len;
                    goto next_frame;
                }
            }
        }

        if (kind == 1 && stored < MAX_FRAMES) {
            /* A CRC adds two bytes between the header and the side info,
             * shifting every offset. Skipped rather than handled: it is
             * rare, and a wrong gain is worse than a missing one. */
            gains[stored++] = (uint8_t)mp3_global_gain(p, lsf, mono, crc);
            levels = true;
        }

        r.pos += (size_t)len;
        frames++;
        continue;

next_frame:
        /* The Xing frame, skipped without counting: it is not a second of
         * audio and must not lengthen the duration. */
        continue;
    }

done:
    out->frames = frames;
    out->rate = rate;
    out->has_levels = levels;
    if (rate && spf) out->sec = (uint32_t)((uint64_t)frames * spf / rate);
    else if (kind == 3) out->sec = frames / 50;     /* AMR is 20 ms flat */

    if (levels) resample(gains, stored, out);

    free(buf);
    free(gains);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "walk: %" PRIu32 " frames, %" PRIu32 "s, levels=%d",
                 frames, out->sec, (int)levels);
    }
    return ret;
}
