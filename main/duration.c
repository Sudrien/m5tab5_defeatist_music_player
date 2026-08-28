/*
 * duration.c
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include <stdlib.h>

#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "duration.h"
#include "storage_io.h"

static const char *TAG = "tab5_dur";

/* ------------------------------------------------------------------ */
/* Byte helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static inline uint64_t le64(const uint8_t *p)
{
    return ((uint64_t)le32(p + 4) << 32) | le32(p);
}

/* Same chokepoint as covertag.c's, for the same reason. These reads are
 * a few hundred bytes each except the Ogg path's 64 KB tail window,
 * which is the one that wanted breaking up. */
static bool read_at(FILE *f, long off, void *buf, size_t len)
{
    return storage_io_read_at(f, off, buf, len, STORAGE_IO_PREFETCH);
}

static long file_size(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    return ftell(f);
}

/* ------------------------------------------------------------------ */
/* FLAC                                                                */
/* ------------------------------------------------------------------ */

/*
 * STREAMINFO is mandatory and is always the first metadata block, so it
 * sits at a fixed offset: 4 bytes of "fLaC", 4 bytes of block header,
 * then the block itself.
 *
 * The two fields wanted are packed across byte boundaries, 18 bytes in:
 *
 *   20 bits  sample rate
 *    3 bits  channels - 1
 *    5 bits  bits per sample - 1
 *   36 bits  total samples
 *
 * Total samples is 0 for a stream of unknown length, which is legal and
 * is exactly the "does not say" case.
 */
static uint32_t probe_flac(FILE *f)
{
    uint8_t b[38];
    if (!read_at(f, 0, b, sizeof(b))) return 0;

    const uint8_t *si = b + 8;                  /* skip magic + block header */
    const uint32_t rate = ((uint32_t)si[10] << 12) |
                          ((uint32_t)si[11] << 4) |
                          (si[12] >> 4);
    const uint64_t samples = ((uint64_t)(si[13] & 0x0F) << 32) |
                             ((uint64_t)si[14] << 24) |
                             ((uint64_t)si[15] << 16) |
                             ((uint64_t)si[16] << 8) |
                             si[17];

    if (!rate || !samples) return 0;
    return (uint32_t)(samples / rate);
}

/* ------------------------------------------------------------------ */
/* WAV                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Walk the RIFF chunks for `fmt ` and `data`. Not assumed adjacent --
 * anything that writes LIST/INFO metadata puts it between them, and a
 * probe that assumed offset 36 would read the metadata size as the audio
 * size.
 *
 * Byte rate comes from `fmt `, so this is right for any PCM variant
 * without needing to know the bit depth.
 */
static uint32_t probe_wav(FILE *f)
{
    const long end = file_size(f);
    if (end < 12) return 0;

    long pos = 12;                              /* past "RIFF" size "WAVE" */
    uint32_t byte_rate = 0, data_len = 0;

    while (pos + 8 <= end) {
        uint8_t h[8];
        if (!read_at(f, pos, h, sizeof(h))) break;
        const uint32_t clen = le32(h + 4);

        if (memcmp(h, "fmt ", 4) == 0 && clen >= 16) {
            uint8_t fmt[16];
            if (read_at(f, pos + 8, fmt, sizeof(fmt))) byte_rate = le32(fmt + 8);
        } else if (memcmp(h, "data", 4) == 0) {
            data_len = clen;
            /* A streamed WAV can carry 0 or 0xFFFFFFFF here, meaning
             * "until the file ends". Fall back to what is actually
             * there. */
            if (data_len == 0 || data_len == 0xFFFFFFFFu) {
                data_len = (uint32_t)(end - (pos + 8));
            }
            break;
        }
        /* Chunks are word aligned; an odd size is followed by a pad
         * byte that is not counted in the size. */
        pos += 8 + clen + (clen & 1);
    }

    if (!byte_rate || !data_len) return 0;
    return data_len / byte_rate;
}

/* ------------------------------------------------------------------ */
/* Ogg (Vorbis, Opus, FLAC-in-Ogg)                                     */
/* ------------------------------------------------------------------ */

/*
 * The last page of the stream carries the final granule position, which
 * is the sample count. Finding it means scanning backwards for the last
 * "OggS" capture pattern.
 *
 * 64 KB is the window. The spec caps a page at about 64 KB (255 segments
 * of 255 bytes plus the header), so the last page's start is always
 * inside it -- and it is one sequential read rather than a walk of the
 * whole file.
 *
 * The rate is the awkward part:
 *
 *   - Opus granule is ALWAYS in 48 kHz units regardless of the original
 *     sample rate. Dividing by the stream rate is the classic way to get
 *     a duration that is wrong by a constant factor.
 *   - Vorbis granule is in stream-rate units, and the rate is in the
 *     identification header at a fixed offset in the first page.
 *
 * So the codec is identified from the first page and the divisor chosen
 * from that.
 */
#define OGG_WINDOW  (65536)

static uint32_t ogg_rate_from_first_page(FILE *f, bool *is_opus)
{
    uint8_t b[64];
    if (!read_at(f, 0, b, sizeof(b))) return 0;

    /* Header size is 27 + the segment table, and the first page of a
     * logical stream holds exactly one segment table entry in practice
     * for both codecs. The payload starts right after it. */
    const int segs = b[26];
    const int payload = 27 + segs;
    if (payload + 16 > (int)sizeof(b)) return 0;
    const uint8_t *p = b + payload;

    if (memcmp(p, "OpusHead", 8) == 0) {
        *is_opus = true;
        /* Input sample rate sits at +12, but it is informational: the
         * granule is 48 kHz whatever it says. */
        return 48000;
    }
    if (p[0] == 0x01 && memcmp(p + 1, "vorbis", 6) == 0) {
        *is_opus = false;
        return le32(p + 12);                    /* audio_sample_rate */
    }
    return 0;
}

static uint32_t probe_ogg(FILE *f)
{
    bool is_opus = false;
    const uint32_t rate = ogg_rate_from_first_page(f, &is_opus);
    if (!rate) return 0;

    const long end = file_size(f);
    if (end <= 0) return 0;

    const long win = (end < OGG_WINDOW) ? end : OGG_WINDOW;
    const long start = end - win;

    /*
     * PSRAM, and freed on the way out. This was a static, which is 64 KB
     * of internal RAM held for the life of the program to answer one
     * question once per track -- on a chip with 384 KB of it, next to a
     * USB host stack and three tasks that all want DMA-capable memory.
     */
#ifdef ESP_PLATFORM
    uint8_t *buf = heap_caps_malloc(OGG_WINDOW, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint8_t *buf = malloc(OGG_WINDOW);
#endif
    if (!buf) return 0;

    if (!read_at(f, start, buf, (size_t)win)) { free(buf); return 0; }

    uint64_t granule = 0;
    bool found = false;
    for (long i = win - 27; i >= 0; i--) {
        if (memcmp(&buf[i], "OggS", 4) != 0) continue;
        granule = le64(&buf[i + 6]);
        found = true;
        break;
    }
    free(buf);

    /* -1 means "no packet finishes on this page", which a final page
     * should never say -- treat it as unknown rather than as a
     * seventeen-million-year track. */
    if (!found || granule == UINT64_MAX) return 0;

    /* Opus granule includes the encoder pre-skip, which is typically
     * 6.5 ms. Not corrected for: it is under one second, and the seek
     * bar reads in seconds. */
    return (uint32_t)(granule / rate);
}

/* ------------------------------------------------------------------ */
/* MP4 / M4A                                                           */
/* ------------------------------------------------------------------ */

/*
 * moov -> mvhd carries duration and timescale. Only the top level is
 * walked, then one level into moov, which is where mvhd always is.
 *
 * Version 1 uses 64-bit fields; version 0, which is nearly everything,
 * uses 32-bit. The layout differs by 12 bytes and reading the wrong one
 * gives a number that looks plausible.
 */
static uint32_t probe_mp4(FILE *f)
{
    const long end = file_size(f);
    long pos = 0;

    while (pos + 8 <= end) {
        uint8_t h[8];
        if (!read_at(f, pos, h, sizeof(h))) break;
        uint64_t sz = be32(h);
        if (sz == 1) {
            uint8_t big[8];
            if (!read_at(f, pos + 8, big, sizeof(big))) break;
            sz = ((uint64_t)be32(big) << 32) | be32(big + 4);
        }
        if (sz < 8) break;

        if (memcmp(h + 4, "moov", 4) == 0) {
            long in = pos + 8;
            const long moov_end = pos + (long)sz;
            while (in + 8 <= moov_end) {
                uint8_t ih[8];
                if (!read_at(f, in, ih, sizeof(ih))) return 0;
                const uint32_t isz = be32(ih);
                if (isz < 8) return 0;

                if (memcmp(ih + 4, "mvhd", 4) == 0) {
                    uint8_t m[32];
                    if (!read_at(f, in + 8, m, sizeof(m))) return 0;
                    const uint8_t ver = m[0];
                    uint32_t ts;
                    uint64_t dur;
                    if (ver == 1) {
                        ts = be32(m + 20);
                        dur = ((uint64_t)be32(m + 24) << 32) | be32(m + 28);
                    } else {
                        ts = be32(m + 12);
                        dur = be32(m + 16);
                    }
                    if (!ts || !dur || dur == 0xFFFFFFFFu) return 0;
                    return (uint32_t)(dur / ts);
                }
                in += isz;
            }
            return 0;
        }
        pos += (long)sz;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

uint32_t duration_probe(FILE *f)
{
    if (!f) return 0;

    const long saved = ftell(f);
    uint32_t sec = 0;

    uint8_t magic[12];
    if (read_at(f, 0, magic, sizeof(magic))) {
        if (memcmp(magic, "fLaC", 4) == 0) {
            sec = probe_flac(f);
        } else if (memcmp(magic, "OggS", 4) == 0) {
            sec = probe_ogg(f);
        } else if (memcmp(magic, "RIFF", 4) == 0 &&
                   memcmp(magic + 8, "WAVE", 4) == 0) {
            sec = probe_wav(f);
        } else if (memcmp(magic + 4, "ftyp", 4) == 0) {
            sec = probe_mp4(f);
        }
    }

    if (saved >= 0) fseek(f, saved, SEEK_SET);

    if (sec) ESP_LOGI(TAG, "container says %" PRIu32 "s", sec);
    return sec;
}
