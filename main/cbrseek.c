/*
 * cbrseek.c -- see cbrseek.h for why this is not `file size / bitrate`.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cbrseek.h"
#include "storage_io.h"

static const char *TAG = "tab5_cbr";

/* ------------------------------------------------------------------ */
/* Byte helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static inline uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

/*
 * PLAYBACK class, unlike duration.c's PREFETCH.
 *
 * Every caller of this file is decoder_open() on the decode loop, which
 * is the read nothing may be queued in front of -- it is the pause
 * before the first sample. duration.c's classification is the wart
 * CLAUDE.md already records against covertag.c; there is no reason to
 * copy it into a new file.
 */
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

/* ------------------------------------------------------------------ */
/* WAV                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The chunks are walked rather than assumed adjacent, for the reason
 * duration.c gives: anything that writes LIST/INFO metadata puts it
 * between `fmt ` and `data`, and a parser that assumed offset 36 would
 * take the metadata for the audio.
 *
 * Only uncompressed PCM is accepted. The block-aligned compressed WAV
 * variants (IMA/MS ADPCM and the rest) do have a constant byte rate, and
 * a byte offset into one of them is meaningless without decoding the
 * block that contains it -- and they do not route to a decoder here
 * anyway, because esp_audio_codec's ADPCM path is one of the
 * one-frame-per-call codecs this player cannot feed. Rejecting them by
 * format tag is the honest version of a restriction that already exists.
 */
static bool probe_wav(FILE *f, long base, cbr_map_t *m)
{
    const long end = file_size(f);
    if (end < base + 12) return false;

    long pos = base + 12;                       /* past "RIFF" size "WAVE" */
    uint32_t byte_rate = 0, block_align = 0, data_len = 0;
    long data_off = -1;
    bool pcm = false, have_fmt = false;

    while (pos + 8 <= end) {
        uint8_t h[8];
        if (!read_at(f, pos, h, sizeof(h))) break;
        const uint32_t clen = le32(h + 4);

        if (memcmp(h, "fmt ", 4) == 0 && clen >= 16) {
            uint8_t fmt[40];
            const size_t want = (clen >= 40) ? 40 : 16;
            if (!read_at(f, pos + 8, fmt, want)) break;
            uint16_t tag = le16(fmt);
            m->channels = le16(fmt + 2);
            m->sample_rate = le32(fmt + 4);
            m->bits = le16(fmt + 14);
            /*
             * Extensible carries its real format in the first two bytes
             * of the subformat GUID, and it is read rather than assumed:
             * the resume preamble below re-declares the stream as plain
             * PCM, so accepting a format that is not PCM would describe
             * it wrongly to the parser at every seek. Float is refused
             * for the same reason rather than because it is not linear,
             * which it is.
             */
            if (tag == 0xFFFE && want == 40) tag = le16(fmt + 24);
            pcm = (tag == 0x0001);
            byte_rate = le32(fmt + 8);
            block_align = le16(fmt + 12);
            have_fmt = true;
        } else if (memcmp(h, "data", 4) == 0) {
            data_off = pos + 8;
            data_len = clen;
            /* A streamed WAV can carry 0 or 0xFFFFFFFF here, meaning
             * "until the file ends". */
            if (data_len == 0 || data_len == 0xFFFFFFFFu ||
                (long)data_len > end - data_off) {
                data_len = (uint32_t)(end - data_off);
            }
            break;
        }
        /* Chunks are word aligned; an odd size is followed by a pad byte
         * that is not counted in the size. */
        pos += 8 + (long)clen + (clen & 1);
    }

    if (!have_fmt || data_off < 0 || !byte_rate || !block_align || !data_len) {
        return false;
    }
    if (!m->channels || !m->sample_rate || !m->bits) return false;
    if (!pcm) {
        ESP_LOGI(TAG, "wav is not PCM (compressed), not mapping it");
        return false;
    }

    m->data_start = data_off;
    m->data_end = data_off + (long)data_len;
    m->byte_rate = byte_rate;
    m->align = block_align;
    m->resync = false;
    m->what = "wav pcm";
    return true;
}

/* ------------------------------------------------------------------ */
/* ADTS AAC                                                            */
/* ------------------------------------------------------------------ */

static const uint32_t k_adts_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0,     0
};

typedef struct {
    uint32_t len;           /* frame length in bytes, header included */
    uint8_t  sf_index;
    uint8_t  channels;      /* channel_configuration, 0 means "in the AOT" */
    uint16_t fullness;      /* 0x7FF is the stream saying it is VBR */
    uint16_t samples;       /* 1024 per raw data block */
} adts_hdr_t;

/* Parses a header at p, which must have at least 7 readable bytes. */
static bool adts_at(const uint8_t *p, size_t avail, adts_hdr_t *h)
{
    if (avail < 7) return false;
    /* Syncword is twelve bits; the two layer bits must be zero for AAC,
     * which is what rules out landing on an MPEG audio frame. */
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return false;

    h->sf_index = (uint8_t)((p[2] >> 2) & 0x0F);
    if (!k_adts_rates[h->sf_index]) return false;
    h->channels = (uint8_t)(((p[2] & 0x01) << 2) | (p[3] >> 6));
    h->len = ((uint32_t)(p[3] & 0x03) << 11) | ((uint32_t)p[4] << 3) |
             ((uint32_t)p[5] >> 5);
    if (h->len < 7) return false;
    h->fullness = (uint16_t)(((uint32_t)(p[5] & 0x1F) << 6) | (p[6] >> 2));
    h->samples = (uint16_t)(1024 * ((p[6] & 0x03) + 1));
    return true;
}

/*
 * A single valid-looking header is not a frame start. 0xFF 0xF1 appears
 * inside AAC payload often enough that resyncing on one byte pattern
 * locks onto noise -- the same trap ID3 tags full of PNG set for MP3
 * sync scanning. So a candidate is only accepted when the frame length
 * it declares lands on another valid header, twice over.
 */
static bool adts_chain_ok(const uint8_t *buf, size_t avail, size_t at)
{
    size_t p = at;
    for (int i = 0; i < 3; i++) {
        adts_hdr_t h;
        if (!adts_at(buf + p, avail - p, &h)) return false;
        if (p + h.len > avail) return (i > 0);  /* ran out of buffer, not out of frames */
        p += h.len;
    }
    return true;
}

/* First frame start at or after `at` within buf, or -1. */
static long adts_find(const uint8_t *buf, size_t avail, size_t at)
{
    for (size_t i = at; i + 7 <= avail; i++) {
        if (buf[i] != 0xFF) continue;
        if (adts_chain_ok(buf, avail, i)) return (long)i;
    }
    return -1;
}

#define ADTS_GROUP_BYTES  4096      /* one read per sample point */
#define ADTS_GROUPS       5
#define ADTS_TOLERANCE    15        /* thousandths: 1.5% spread allowed */

typedef struct {
    uint64_t sum;
    uint32_t frames;
    uint8_t  sf_index;
    uint8_t  channels;
    uint16_t spf;
} adts_group_t;

/* Walks the frames in one window. False if it could not find enough to
 * say anything. */
static bool adts_group(FILE *f, long off, long limit, uint8_t *buf,
                       adts_group_t *g)
{
    long want = limit - off;
    if (want <= 0) return false;
    if (want > ADTS_GROUP_BYTES) want = ADTS_GROUP_BYTES;
    if (!read_at(f, off, buf, (size_t)want)) return false;

    const long first = adts_find(buf, (size_t)want, 0);
    if (first < 0) return false;

    memset(g, 0, sizeof(*g));
    size_t p = (size_t)first;
    while (p + 7 <= (size_t)want) {
        adts_hdr_t h;
        if (!adts_at(buf + p, (size_t)want - p, &h)) return false;
        if (p + h.len > (size_t)want) break;    /* partial frame at the end */
        if (h.fullness == 0x7FF) {
            /* The stream is telling us it is VBR. Believe it. */
            ESP_LOGI(TAG, "adts declares VBR (buffer_fullness 0x7FF)");
            return false;
        }
        if (g->frames == 0) {
            g->sf_index = h.sf_index;
            g->channels = h.channels;
            g->spf = h.samples;
        } else if (h.sf_index != g->sf_index || h.channels != g->channels ||
                   h.samples != g->spf) {
            /* Concatenated streams. The mapping is not one line. */
            return false;
        }
        g->sum += h.len;
        g->frames++;
        p += h.len;
    }
    return g->frames >= 4;
}

/*
 * Five windows spread across the file, and they have to agree.
 *
 * This is the whole proof. A VBR stream's mean frame length differs
 * between a quiet passage and a loud one by far more than 1.5%, so
 * disagreement is the signal and no decoding is needed to see it. What
 * it cannot catch is a VBR file that happens to average the same
 * everywhere -- which is a file that is, for the purpose of placing a
 * finger on a bar, CBR.
 */
static bool probe_adts(FILE *f, long base, cbr_map_t *m)
{
    const long end = file_size(f);
    if (end <= base + 64) return false;

    uint8_t *buf = malloc(ADTS_GROUP_BYTES);
    if (!buf) return false;

    adts_group_t first;
    bool ok = adts_group(f, base, end, buf, &first);
    uint64_t sum = 0;
    uint32_t frames = 0;
    uint32_t lo = 0, hi = 0;

    if (ok) {
        sum = first.sum;
        frames = first.frames;
        /* Fixed point so the comparison between groups is integer:
         * mean frame length in 1/256ths of a byte. */
        lo = hi = (uint32_t)((first.sum * 256) / first.frames);
    }

    const long span = end - base;
    for (int i = 1; ok && i < ADTS_GROUPS; i++) {
        const long at = base + (long)((uint64_t)span * i / ADTS_GROUPS);
        adts_group_t g;
        if (!adts_group(f, at, end, buf, &g)) {
            /* A window that produced nothing is not a disagreement --
             * the tail of the file can be shorter than a window -- but
             * a window with different parameters is. */
            if (at + 8 * ADTS_GROUP_BYTES < end) ok = false;
            continue;
        }
        if (g.sf_index != first.sf_index || g.channels != first.channels ||
            g.spf != first.spf) {
            ok = false;
            break;
        }
        const uint32_t mean = (uint32_t)((g.sum * 256) / g.frames);
        if (mean < lo) lo = mean;
        if (mean > hi) hi = mean;
        sum += g.sum;
        frames += g.frames;
    }

    free(buf);
    if (!ok || !frames) return false;

    if ((uint64_t)(hi - lo) * 1000 > (uint64_t)lo * ADTS_TOLERANCE) {
        ESP_LOGI(TAG, "adts frame length varies %u..%u (1/256 B): VBR, not mapping it",
                 (unsigned)lo, (unsigned)hi);
        return false;
    }

    const uint32_t rate = k_adts_rates[first.sf_index];
    const uint64_t byte_rate = (sum * rate) / ((uint64_t)frames * first.spf);
    if (!byte_rate) return false;

    m->data_start = base;
    m->data_end = end;
    m->byte_rate = (uint32_t)byte_rate;
    /* Nothing to align to: the frames are not at a fixed pitch to the
     * byte, which is what the resync is for. */
    m->align = 1;
    m->resync = true;
    m->what = "adts cbr";
    return true;
}

/* ------------------------------------------------------------------ */
/* AMR                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Frame size is a function of the mode bits in the one-byte frame
 * header, and every frame is 20 ms, so a file at a fixed mode is a file
 * at a fixed byte rate with frames at a fixed pitch. That makes AMR the
 * one format here that is both exactly seekable and cheap to verify:
 * the frames are at computable offsets, so checking is reading one byte
 * where a header must be.
 *
 * Sizes include the header byte. Index 15 is NO_DATA (1 byte); the SID
 * frames and the reserved modes are the entries that are not audio, and
 * a file that uses them is not at a fixed rate and gets rejected by the
 * walk below rather than by a special case here.
 */
static const uint8_t k_amr_nb[16] = {
    13, 14, 16, 18, 20, 21, 27, 32, 6, 0, 0, 0, 0, 0, 0, 1
};
static const uint8_t k_amr_wb[16] = {
    18, 24, 33, 37, 41, 47, 51, 59, 61, 6, 0, 0, 0, 0, 1, 1
};

#define AMR_CHECK_FRAMES  8

static bool probe_amr(FILE *f, long base, cbr_map_t *m)
{
    uint8_t magic[9];
    if (!read_at(f, base, magic, sizeof(magic))) return false;

    const uint8_t *sizes;
    long data;
    if (memcmp(magic, "#!AMR-WB\n", 9) == 0) {
        sizes = k_amr_wb;
        data = base + 9;
    } else if (memcmp(magic, "#!AMR\n", 6) == 0) {
        sizes = k_amr_nb;
        data = base + 6;
    } else {
        return false;                           /* multi-channel variants */
    }

    const long end = file_size(f);
    if (end <= data + 1) return false;

    uint8_t h;
    if (!read_at(f, data, &h, 1)) return false;
    const uint8_t mode = (uint8_t)((h >> 3) & 0x0F);
    const uint8_t sz = sizes[mode];
    if (sz < 6) return false;                   /* SID, no-data or reserved */

    const long frames = (end - data) / sz;
    if (frames < 2) return false;

    /* Read where the headers must be, at five points and then at the
     * end. Anything that changes mode part way puts every later frame at
     * a different offset, so a wrong mode bit here is a mis-framed file
     * rather than a tolerable variation.
     *
     * The last group is not one of the five and is not decoration: the
     * frame count is derived from the file size divided by the size the
     * FIRST frame declares, so a file that changes to a smaller mode
     * half way through has more frames than that arithmetic says and
     * every evenly-spaced sample point lands before the change. Checking
     * the end is what catches it, and it is one read. */
    uint8_t win[64 * 8];
    for (int g = 0; g <= 5; g++) {
        long n = (g == 5) ? frames - AMR_CHECK_FRAMES
                          : (long)((uint64_t)frames * g / 5);
        if (n + AMR_CHECK_FRAMES > frames) n = frames - AMR_CHECK_FRAMES;
        if (n < 0) n = 0;
        const long want = (frames - n < AMR_CHECK_FRAMES ? frames - n
                                                         : AMR_CHECK_FRAMES) * sz;
        if (want <= 0 || (size_t)want > sizeof(win)) return false;
        if (!read_at(f, data + n * sz, win, (size_t)want)) return false;
        for (long i = 0; i * sz < want; i++) {
            if (((win[i * sz] >> 3) & 0x0F) != mode) {
                ESP_LOGI(TAG, "amr mode changes; not a fixed frame size");
                return false;
            }
        }
    }

    memcpy(m->magic, magic, (size_t)(data - base));
    m->magic_len = (uint8_t)(data - base);
    m->data_start = data;
    m->data_end = data + frames * sz;
    m->byte_rate = (uint32_t)sz * 50;           /* 20 ms per frame */
    m->align = sz;
    m->resync = false;
    m->what = (sizes == k_amr_wb) ? "amr-wb" : "amr-nb";
    return true;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

/*
 * An ID3v2 tag on a raw ADTS or AMR file is not legal and taggers write
 * one anyway -- the same note covertag.c carries about FLAC and Ogg. The
 * size field is syncsafe, seven bits per byte, so the length can never
 * itself contain a false sync.
 */
static long skip_id3v2(FILE *f)
{
    uint8_t h[10];
    if (!read_at(f, 0, h, sizeof(h))) return 0;
    if (memcmp(h, "ID3", 3) != 0) return 0;
    const long size = ((long)(h[6] & 0x7F) << 21) | ((long)(h[7] & 0x7F) << 14) |
                      ((long)(h[8] & 0x7F) << 7) | (h[9] & 0x7F);
    /* Bit 4 of the flags is a footer, which is ten more bytes and is not
     * counted in the size. */
    return 10 + size + ((h[5] & 0x10) ? 10 : 0);
}

bool cbr_probe(FILE *f, cbr_map_t *m)
{
    if (!f || !m) return false;

    const long saved = ftell(f);
    memset(m, 0, sizeof(*m));

    const long base = skip_id3v2(f);
    bool ok = false;

    uint8_t magic[12];
    if (read_at(f, base, magic, sizeof(magic))) {
        if (memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0) {
            ok = probe_wav(f, base, m);
        } else if (memcmp(magic, "#!AMR", 5) == 0) {
            ok = probe_amr(f, base, m);
        } else if (magic[0] == 0xFF && (magic[1] & 0xF6) == 0xF0) {
            ok = probe_adts(f, base, m);
        }
    }

    if (saved >= 0) fseek(f, saved, SEEK_SET);

    if (ok) {
        ESP_LOGI(TAG, "%s: %" PRIu32 " B/s over %ld B, seekable, %" PRIu32 " s",
                 m->what, m->byte_rate, m->data_end - m->data_start,
                 cbr_duration_sec(m));
    } else {
        memset(m, 0, sizeof(*m));
    }
    return ok;
}

uint32_t cbr_duration_sec(const cbr_map_t *m)
{
    if (!m || !m->byte_rate || m->data_end <= m->data_start) return 0;
    return (uint32_t)((uint64_t)(m->data_end - m->data_start) / m->byte_rate);
}

long cbr_offset_for_sec(FILE *f, const cbr_map_t *m, uint32_t sec)
{
    if (!f || !m || !m->byte_rate) return -1;

    const long span = m->data_end - m->data_start;
    if (span <= 0) return -1;

    uint64_t rel = (uint64_t)sec * m->byte_rate;
    if (rel >= (uint64_t)span) {
        /* A drag to the far right lands on the last frame rather than
         * past it: seeking to EOF and calling that a position is how a
         * seek bar ends up asserting a track ended when it did not. */
        rel = (uint64_t)span - 1;
    }
    if (m->align > 1) rel -= rel % m->align;

    long off = m->data_start + (long)rel;
    if (!m->resync) return off;

    /* ADTS. The arithmetic gives a byte, the decoder needs a frame. Two
     * windows: one at the target, and -- because the tolerated drift can
     * put the target inside the last frame of the file -- a fallback
     * that gives up rather than returning a byte that is not a header.
     */
    uint8_t *buf = malloc(ADTS_GROUP_BYTES);
    if (!buf) return -1;

    long want = m->data_end - off;
    if (want > ADTS_GROUP_BYTES) want = ADTS_GROUP_BYTES;
    long found = -1;
    if (want >= 7 && read_at(f, off, buf, (size_t)want)) {
        found = adts_find(buf, (size_t)want, 0);
    }
    free(buf);

    if (found < 0) {
        ESP_LOGW(TAG, "no frame header within %d B of the seek target",
                 ADTS_GROUP_BYTES);
        return -1;
    }
    return off + found;
}

/* ------------------------------------------------------------------ */

static void w32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void w16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

size_t cbr_resume_preamble(const cbr_map_t *m, long off, uint8_t *buf, size_t cap)
{
    if (!m || !buf) return 0;

    if (m->magic_len) {                         /* AMR */
        if (cap < m->magic_len) return 0;
        memcpy(buf, m->magic, m->magic_len);
        return m->magic_len;
    }

    if (!m->channels || !m->bits) return 0;     /* ADTS: nothing needed */
    if (cap < 44) return 0;

    /*
     * Canonical 44-byte RIFF/WAVE. Not the file's own header re-read:
     * the file's `data` length describes the whole track and the parser
     * is being handed the middle of it, so the length written here is
     * what is actually left from `off`. A parser that clamps its output
     * to the declared length then stops at the real end of the audio
     * rather than a track's worth of bytes past it.
     */
    long left = m->data_end - off;
    if (left < 0) left = 0;

    memcpy(buf, "RIFF", 4);
    w32le(buf + 4, (uint32_t)(36 + left));
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    w32le(buf + 16, 16);
    w16le(buf + 20, 1);                         /* PCM; extensible is not
                                                 * re-declared, because what
                                                 * follows it here is plain
                                                 * PCM either way */
    w16le(buf + 22, m->channels);
    w32le(buf + 24, m->sample_rate);
    w32le(buf + 28, m->byte_rate);
    w16le(buf + 32, (uint16_t)m->align);
    w16le(buf + 34, m->bits);
    memcpy(buf + 36, "data", 4);
    w32le(buf + 40, (uint32_t)left);
    return 44;
}

/* ------------------------------------------------------------------ */

long cbr_adts_resync(FILE *f, long off, long end)
{
    if (!f || off < 0 || end <= off) return -1;

    uint8_t *buf = malloc(ADTS_GROUP_BYTES);
    if (!buf) return -1;

    long want = end - off;
    if (want > ADTS_GROUP_BYTES) want = ADTS_GROUP_BYTES;

    long found = -1;
    if (want >= 7 && read_at(f, off, buf, (size_t)want)) {
        found = adts_find(buf, (size_t)want, 0);
    }
    free(buf);
    return (found < 0) ? -1 : off + found;
}
