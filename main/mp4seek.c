/*
 * mp4seek.c -- see mp4seek.h for why this parses the tables itself.
 *
 * Every length here comes off a card, so the rules covertag.c already
 * states apply unchanged: cap before allocating, check each length
 * against what is left rather than against the total, and treat a
 * declared count as a claim rather than a fact.
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

#include "mp4seek.h"
#include "storage_io.h"

static const char *TAG = "tab5_mp4sk";

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

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void *big_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

static const uint32_t k_sf_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0,     0
};

/* ------------------------------------------------------------------ */
/* Box walking                                                         */
/* ------------------------------------------------------------------ */

/*
 * Find a child box by type within [start, end). Returns the offset of
 * its PAYLOAD and fills its length.
 *
 * `skip` is what `meta` needs and nothing else does: four bytes of
 * version and flags before its children. Walking it like a plain
 * container puts every child type four bytes out -- the trap covertag.c
 * already documents from the other side of the same file format.
 */
static long find_box(FILE *f, long start, long end, const char *type,
                     long *len_out, int skip)
{
    long pos = start + skip;
    while (pos + 8 <= end) {
        uint8_t h[16];
        if (!read_at(f, pos, h, 8)) return -1;
        uint64_t sz = be32(h);
        int hdr = 8;
        if (sz == 1) {
            if (!read_at(f, pos + 8, h + 8, 8)) return -1;
            sz = be64(h + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = (uint64_t)(end - pos);
        }
        if (sz < (uint64_t)hdr || pos + (long)sz > end) return -1;

        if (memcmp(h + 4, type, 4) == 0) {
            if (len_out) *len_out = (long)sz - hdr;
            return pos + hdr;
        }
        pos += (long)sz;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* esds                                                                */
/* ------------------------------------------------------------------ */

/*
 * The AudioSpecificConfig, from the descriptor chain inside esds:
 * ES_Descriptor (0x03) -> DecoderConfigDescriptor (0x04) ->
 * DecoderSpecificInfo (0x05). Descriptor lengths are the same
 * seven-bits-per-byte encoding ID3v2 uses, for the same reason.
 *
 * The config itself is five bits of object type, four of sampling
 * frequency index, four of channel configuration -- which is exactly
 * the three fields an ADTS header needs, which is why this remux is
 * fifteen lines rather than a transcode.
 */
static bool parse_asc(const uint8_t *p, size_t n, mp4_t *m)
{
    size_t at = 4;                              /* version and flags */
    int want = 0x03;

    while (at < n) {
        const uint8_t tag = p[at++];
        uint32_t len = 0;
        for (int i = 0; i < 4 && at < n; i++) {
            const uint8_t b = p[at++];
            len = (len << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        if (at + len > n) return false;

        if (tag == 0x03 && want == 0x03) {
            /* ES_ID, then flags whose bits say which optional fields
             * follow. Skipping them by the wrong count lands in the
             * middle of the next descriptor. */
            size_t skip = 2;
            if (at + 2 >= n) return false;
            const uint8_t flags = p[at + 2];
            skip = 3;
            if (flags & 0x80) skip += 2;        /* streamDependenceFlag */
            if (flags & 0x40) {                 /* URL_Flag */
                if (at + skip >= n) return false;
                skip += 1 + p[at + skip];
            }
            if (flags & 0x20) skip += 2;        /* OCRstreamFlag */
            at += skip;
            want = 0x04;
            continue;
        }
        if (tag == 0x04 && want == 0x04) {
            at += 13;                           /* object type, stream
                                                 * type, buffer size,
                                                 * bitrates */
            want = 0x05;
            continue;
        }
        if (tag == 0x05 && want == 0x05) {
            if (len < 2) return false;
            const uint8_t b0 = p[at], b1 = p[at + 1];
            uint8_t aot = (uint8_t)(b0 >> 3);
            uint8_t sf = (uint8_t)(((b0 & 0x07) << 1) | (b1 >> 7));
            uint8_t ch = (uint8_t)((b1 >> 3) & 0x0F);

            /* An escape value means the rate is stated explicitly in 24
             * bits, which ADTS has no way to carry. Refuse rather than
             * write a header that says something else. */
            if (sf >= 13) return false;
            if (aot == 0 || aot > 4) {
                /* HE-AAC (5) and PS (29) signal the BASE object type in
                 * ADTS and let the decoder detect the extension, which
                 * is what implicit signalling means. Anything else --
                 * scalable, ER, USAC -- this does not write a header
                 * for. */
                if (aot != 5 && aot != 29) return false;
                aot = 2;
            }
            if (ch == 0 || ch > 2) return false;    /* the player is stereo */

            m->profile = (uint8_t)(aot - 1);
            m->sf_index = sf;
            m->chan_cfg = ch;
            m->sample_rate = k_sf_rates[sf];
            m->channels = ch;
            return m->sample_rate != 0;
        }
        at += len;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Sample tables                                                       */
/* ------------------------------------------------------------------ */

/*
 * stsc maps chunks to samples in runs, stco says where each chunk is,
 * and stsz how big each sample is. Expanding the three into one offset
 * per sample is the whole of what makes a seek a lookup, and it is done
 * once at open.
 *
 * A file states its own counts, and the counts are what get allocated
 * against, so each is checked against what the box can actually hold
 * before anything is reserved.
 */
static bool build_tables(FILE *f, long stbl, long stbl_end, mp4_t *m)
{
    long len;
    uint8_t hdr[16];

    /* --- stsz --------------------------------------------------- */
    long stsz = find_box(f, stbl, stbl_end, "stsz", &len, 0);
    if (stsz < 0 || len < 12) return false;
    if (!read_at(f, stsz, hdr, 12)) return false;
    const uint32_t uniform = be32(hdr + 4);
    const uint32_t count = be32(hdr + 8);
    if (!count || count > MP4_MAX_SAMPLES) {
        ESP_LOGI(TAG, "%u samples is outside what is held here", (unsigned)count);
        return false;
    }
    if (!uniform && (long)(12 + 4 * (uint64_t)count) > len) return false;

    m->count = count;
    m->offset = big_alloc((size_t)count * sizeof(uint32_t));
    m->size = big_alloc((size_t)count * sizeof(uint32_t));
    if (!m->offset || !m->size) return false;

    if (uniform) {
        for (uint32_t i = 0; i < count; i++) m->size[i] = uniform;
    } else {
        /* In blocks, because one read per sample is twelve thousand
         * reads on a five-minute track. */
        uint8_t buf[1024];
        uint32_t done = 0;
        while (done < count) {
            const uint32_t n = (count - done > 256) ? 256 : (count - done);
            if (!read_at(f, stsz + 12 + 4 * (long)done, buf, (size_t)n * 4)) {
                return false;
            }
            for (uint32_t i = 0; i < n; i++) m->size[done + i] = be32(buf + 4 * i);
            done += n;
        }
    }

    /* --- stco / co64 -------------------------------------------- */
    bool co64 = false;
    long stco = find_box(f, stbl, stbl_end, "stco", &len, 0);
    if (stco < 0) {
        stco = find_box(f, stbl, stbl_end, "co64", &len, 0);
        co64 = true;
    }
    if (stco < 0 || len < 8) return false;
    if (!read_at(f, stco, hdr, 8)) return false;
    const uint32_t chunks = be32(hdr + 4);
    if (!chunks || (long)(8 + (co64 ? 8 : 4) * (uint64_t)chunks) > len) {
        return false;
    }

    uint32_t *chunk_off = big_alloc((size_t)chunks * sizeof(uint32_t));
    if (!chunk_off) return false;
    bool ok = true;
    {
        uint8_t buf[1024];
        uint32_t done = 0;
        const int w = co64 ? 8 : 4;
        while (done < chunks && ok) {
            const uint32_t n = (chunks - done > 128) ? 128 : (chunks - done);
            if (!read_at(f, stco + 8 + (long)done * w, buf, (size_t)n * w)) {
                ok = false;
                break;
            }
            for (uint32_t i = 0; i < n; i++) {
                const uint64_t v = co64 ? be64(buf + 8 * i) : be32(buf + 4 * i);
                /* A FAT volume cannot hold a file this large, and a
                 * truncated offset is a read of the wrong bytes rather
                 * than a failure. */
                if (v > UINT32_MAX) { ok = false; break; }
                chunk_off[done + i] = (uint32_t)v;
            }
            done += n;
        }
    }

    /* --- stsc, expanded ----------------------------------------- */
    long stsc = ok ? find_box(f, stbl, stbl_end, "stsc", &len, 0) : -1;
    if (stsc < 0 || len < 8) ok = false;
    uint32_t runs = 0;
    if (ok) {
        if (!read_at(f, stsc, hdr, 8)) ok = false;
        else {
            runs = be32(hdr + 4);
            if (!runs || (long)(8 + 12 * (uint64_t)runs) > len) ok = false;
        }
    }

    if (ok) {
        uint32_t sample = 0;
        uint32_t first = 0, per = 0;
        bool have = false;

        for (uint32_t r = 0; r < runs && ok; r++) {
            uint8_t e[12];
            if (!read_at(f, stsc + 8 + 12 * (long)r, e, 12)) { ok = false; break; }
            const uint32_t nfirst = be32(e);          /* 1-based chunk */
            const uint32_t nper = be32(e + 4);
            if (!nfirst || nfirst > chunks || !nper) { ok = false; break; }

            if (have) {
                for (uint32_t c = first; c < nfirst && sample < m->count; c++) {
                    uint32_t at = chunk_off[c - 1];
                    for (uint32_t k = 0; k < per && sample < m->count; k++) {
                        m->offset[sample] = at;
                        at += m->size[sample];
                        sample++;
                    }
                }
            }
            first = nfirst;
            per = nper;
            have = true;
        }
        /* The last run covers every remaining chunk, which is why it
         * cannot be written by the loop above. */
        if (ok && have) {
            for (uint32_t c = first; c <= chunks && sample < m->count; c++) {
                uint32_t at = chunk_off[c - 1];
                for (uint32_t k = 0; k < per && sample < m->count; k++) {
                    m->offset[sample] = at;
                    at += m->size[sample];
                    sample++;
                }
            }
        }
        if (sample != m->count) {
            ESP_LOGI(TAG, "sample tables disagree: %u placed of %u",
                     (unsigned)sample, (unsigned)m->count);
            ok = false;
        }
    }

    free(chunk_off);
    if (!ok) return false;

    /* --- stts --------------------------------------------------- */
    long stts = find_box(f, stbl, stbl_end, "stts", &len, 0);
    if (stts < 0 || len < 8) return false;
    if (!read_at(f, stts, hdr, 8)) return false;
    const uint32_t entries = be32(hdr + 4);
    if (!entries || entries > 4096 ||
        (long)(8 + 8 * (uint64_t)entries) > len) {
        return false;
    }

    m->stts_count = malloc((size_t)entries * sizeof(uint32_t));
    m->stts_delta = malloc((size_t)entries * sizeof(uint32_t));
    if (!m->stts_count || !m->stts_delta) return false;
    for (uint32_t i = 0; i < entries; i++) {
        uint8_t e[8];
        if (!read_at(f, stts + 8 + 8 * (long)i, e, 8)) return false;
        m->stts_count[i] = be32(e);
        m->stts_delta[i] = be32(e + 4);
    }
    m->stts_n = (int)entries;
    return true;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

bool mp4_probe(FILE *f, mp4_t *m)
{
    if (!f || !m) return false;

    const long saved = ftell(f);
    memset(m, 0, sizeof(*m));
    bool ok = false;

    const long end = file_size(f);
    if (end < 16) goto out;

    long len;
    const long moov = find_box(f, 0, end, "moov", &len, 0);
    if (moov < 0) goto out;
    const long moov_end = moov + len;

    /* One audio track, which is every .m4a. A file with several is a
     * video container and the fallback path can have it. */
    const long trak = find_box(f, moov, moov_end, "trak", &len, 0);
    if (trak < 0) goto out;
    const long trak_end = trak + len;

    const long mdia = find_box(f, trak, trak_end, "mdia", &len, 0);
    if (mdia < 0) goto out;
    const long mdia_end = mdia + len;

    const long mdhd = find_box(f, mdia, mdia_end, "mdhd", &len, 0);
    if (mdhd < 0 || len < 24) goto out;
    {
        uint8_t b[32];
        if (!read_at(f, mdhd, b, (len < 32 ? (size_t)len : 32))) goto out;
        if (b[0] == 1) {
            if (len < 36) goto out;
            uint8_t b1[36];
            if (!read_at(f, mdhd, b1, 36)) goto out;
            m->timescale = be32(b1 + 20);
            m->duration = be64(b1 + 24);
        } else {
            m->timescale = be32(b + 12);
            m->duration = be32(b + 16);
        }
    }
    if (!m->timescale) goto out;

    const long minf = find_box(f, mdia, mdia_end, "minf", &len, 0);
    if (minf < 0) goto out;
    const long minf_end = minf + len;
    const long stbl = find_box(f, minf, minf_end, "stbl", &len, 0);
    if (stbl < 0) goto out;
    const long stbl_end = stbl + len;

    /* stsd: four bytes of version/flags, four of entry count, then the
     * sample entries. The entry itself is a box, so the esds inside it
     * is found by walking from a fixed offset into it: 8 bytes of box
     * header plus 28 of audio sample entry. */
    const long stsd = find_box(f, stbl, stbl_end, "stsd", &len, 0);
    if (stsd < 0 || len < 16) goto out;
    {
        uint8_t b[16];
        if (!read_at(f, stsd + 8, b, 8)) goto out;
        if (memcmp(b + 4, "mp4a", 4) != 0) {
            ESP_LOGI(TAG, "sample entry is '%c%c%c%c', not mp4a; "
                          "leaving it to the M4A parser",
                     b[4], b[5], b[6], b[7]);
            goto out;
        }
        const long entry = stsd + 8;
        const long entry_end = entry + (long)be32(b);
        long esds_len;
        const long esds = find_box(f, entry + 8 + 28, entry_end, "esds",
                                   &esds_len, 0);
        if (esds < 0 || esds_len < 8 || esds_len > 512) goto out;

        uint8_t desc[512];
        if (!read_at(f, esds, desc, (size_t)esds_len)) goto out;
        if (!parse_asc(desc, (size_t)esds_len, m)) {
            ESP_LOGI(TAG, "no usable AudioSpecificConfig; "
                          "leaving it to the M4A parser");
            goto out;
        }
    }

    if (!build_tables(f, stbl, stbl_end, m)) goto out;
    ok = true;

out:
    if (saved >= 0) fseek(f, saved, SEEK_SET);
    if (ok) {
        m->ok = true;
        m->pos = -1;
        ESP_LOGI(TAG, "mp4: aac %" PRIu32 " Hz, %u ch, %" PRIu32 " samples, "
                      "%" PRIu32 " s, seekable",
                 m->sample_rate, (unsigned)m->channels, m->count,
                 mp4_duration_sec(m));
    } else {
        mp4_free(m);
    }
    return ok;
}

void mp4_free(mp4_t *m)
{
    if (!m) return;
    free(m->offset);
    free(m->size);
    free(m->stts_count);
    free(m->stts_delta);
    memset(m, 0, sizeof(*m));
}

uint32_t mp4_duration_sec(const mp4_t *m)
{
    if (!m || !m->timescale || !m->duration) return 0;
    return (uint32_t)(m->duration / m->timescale);
}

/* ------------------------------------------------------------------ */
/* Position                                                            */
/* ------------------------------------------------------------------ */

uint32_t mp4_seek_sec(mp4_t *m, uint32_t sec)
{
    if (!m || !m->ok) return 0;

    const uint64_t want = (uint64_t)sec * m->timescale;
    uint64_t t = 0;
    uint32_t idx = 0;

    for (int e = 0; e < m->stts_n && idx < m->count; e++) {
        const uint64_t span = (uint64_t)m->stts_count[e] * m->stts_delta[e];
        if (m->stts_delta[e] && t + span > want) {
            const uint64_t into = (want - t) / m->stts_delta[e];
            idx += (uint32_t)into;
            t += into * m->stts_delta[e];
            break;
        }
        t += span;
        idx += m->stts_count[e];
    }
    if (idx >= m->count) idx = m->count - 1;

    m->cur = idx;
    /* The reader is not where the table says any more, so the next read
     * seeks. Without this a seek backwards would be read as a
     * contiguous run and simply carry on from where it was. */
    m->pos = -1;
    return (uint32_t)(t / m->timescale);
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

#define ADTS_HDR    7

static void adts_header(const mp4_t *m, uint8_t *h, uint32_t payload)
{
    const uint32_t total = payload + ADTS_HDR;
    h[0] = 0xFF;
    h[1] = 0xF1;                                /* MPEG-4, no CRC */
    h[2] = (uint8_t)((m->profile << 6) | (m->sf_index << 2) |
                     ((m->chan_cfg >> 2) & 0x01));
    h[3] = (uint8_t)(((m->chan_cfg & 0x03) << 6) | ((total >> 11) & 0x03));
    h[4] = (uint8_t)((total >> 3) & 0xFF);
    /* Buffer fullness 0x7FF is "variable", which is the truth about a
     * remuxed stream and is also what cbrseek.c reads as a refusal to
     * treat it as CBR. Both are correct: this stream's byte rate is not
     * a line and nothing should pretend it is. */
    h[5] = (uint8_t)(((total & 0x07) << 5) | 0x1F);
    h[6] = 0xFC;
}

/*
 * ONE READ PER CONTIGUOUS RUN, NOT ONE PER SAMPLE.
 *
 * The first version read each sample separately, on the reasoning that
 * a normally-muxed file keeps them in order so the stdio buffer would
 * absorb it. The board says otherwise: a sixty-second track logged 2585
 * reads for 962 KB, held the arbiter for 1545 ms, and produced
 * `decoder_read blocked 959 ms` -- against 43 reads for the same audio
 * as Ogg. Two and a half thousand lease acquisitions is the cost, and
 * it is paid on the decode loop.
 *
 * So the run of samples that will fit in `cap` is measured first, read
 * in one call, and then spread out in place to make room for the
 * headers. The spread is safe in ascending order because each sample's
 * destination is at most 7*N bytes before where it was read to, and
 * every earlier sample moves further back than the one after it.
 */
size_t mp4_read(FILE *f, mp4_t *m, uint8_t *dst, size_t cap)
{
    if (!f || !m || !m->ok || !dst) return 0;

    /* Skip anything absurd before measuring, so the run below is made
     * of samples that will actually be emitted. */
    while (m->cur < m->count &&
           (!m->size[m->cur] || m->size[m->cur] > 0x20000)) {
        m->cur++;
    }
    if (m->cur >= m->count) return 0;

    /* How many contiguous samples fit, headers included. */
    uint32_t n = 0;
    size_t payload = 0;
    long expect = (long)m->offset[m->cur];
    while (m->cur + n < m->count) {
        const uint32_t sz = m->size[m->cur + n];
        if (!sz || sz > 0x20000) break;
        if ((long)m->offset[m->cur + n] != expect) break;   /* a gap */
        if (payload + sz + (size_t)(n + 1) * ADTS_HDR > cap) break;
        payload += sz;
        expect += sz;
        n++;
    }
    if (!n) return 0;

    const long at = (long)m->offset[m->cur];
    if (m->pos != at) {
        if (fseek(f, at, SEEK_SET) != 0) return 0;
        m->pos = at;
    }

    /* Read the whole run into the tail of the output, where the headers
     * are not yet in the way. */
    const size_t head = (size_t)n * ADTS_HDR;
    if (storage_io_fread(dst + head, payload, f, STORAGE_IO_PLAYBACK)
            != payload) {
        m->pos = -1;
        return 0;
    }
    m->pos += (long)payload;

    size_t src = head, out = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t sz = m->size[m->cur + i];
        adts_header(m, dst + out, sz);
        memmove(dst + out + ADTS_HDR, dst + src, sz);
        out += ADTS_HDR + sz;
        src += sz;
    }
    m->cur += n;
    return out;
}
