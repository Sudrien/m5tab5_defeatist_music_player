/*
 * oggseek.c -- see oggseek.h, and in particular the note on what the
 * precompiled parser was found to do.
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

#include "oggseek.h"
#include "storage_io.h"

static const char *TAG = "tab5_oggsk";

/* PLAYBACK: the open and the drag, both moments somebody is waiting. */
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

static inline uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static inline uint64_t le64(const uint8_t *p)
{
    return ((uint64_t)le32(p + 4) << 32) | le32(p);
}

static void *big_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

/*
 * An Ogg page is 27 bytes of header, a segment table of `segments`
 * bytes, and the sum of that table in payload. Everything this file
 * needs is in the header:
 *
 *   0   "OggS"
 *   4   version, always 0
 *   5   header type: bit 0 continued, bit 1 first, bit 2 last
 *   6   granule position, 64-bit little endian
 *   14  stream serial number
 *   18  page sequence number   -- not used here, and not by the parser
 *   22  CRC32                  -- likewise
 *   26  number of segments
 */
#define OGG_HDR      27
#define OGG_PAGE_MAX (27 + 255 + 255 * 255)     /* 65307 */

typedef struct {
    uint64_t granule;
    uint32_t serial;
    long     size;              /* whole page, header included */
    bool     continued;
} ogg_page_t;

static bool page_at(const uint8_t *p, size_t avail, ogg_page_t *pg)
{
    if (avail < OGG_HDR) return false;
    if (memcmp(p, "OggS", 4) != 0) return false;
    if (p[4] != 0) return false;

    const size_t segs = p[26];
    if (avail < OGG_HDR + segs) return false;

    long payload = 0;
    for (size_t i = 0; i < segs; i++) payload += p[OGG_HDR + i];

    pg->granule = le64(p + 6);
    pg->serial = le32(p + 14);
    pg->size = (long)(OGG_HDR + segs) + payload;
    pg->continued = (p[5] & 0x01) != 0;
    return true;
}

/*
 * First page at or after `at`.
 *
 * `serial` of 0 means "any", which is only used before the stream's own
 * serial is known. Otherwise a page from a different logical stream is
 * not a candidate -- a chained file is several streams and this seeks
 * within one.
 *
 * Confirmed by the following page where the window reaches it. `OggS`
 * is four bytes and appears in Vorbis payload rarely but not never, and
 * the version byte and serial number take most of the rest; the
 * confirmation is what makes the residual not worth thinking about.
 */
static long find_page(const uint8_t *buf, size_t avail, uint32_t serial,
                      ogg_page_t *out)
{
    for (size_t i = 0; i + OGG_HDR <= avail; i++) {
        if (buf[i] != 'O') continue;
        ogg_page_t pg;
        if (!page_at(buf + i, avail - i, &pg)) continue;
        if (serial && pg.serial != serial) continue;

        const size_t next = i + (size_t)pg.size;
        if (next + 4 <= avail && memcmp(buf + next, "OggS", 4) != 0) {
            continue;                   /* the size it declared is wrong */
        }
        *out = pg;
        return (long)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

/*
 * Header pages are the pages at the front of the file whose granule
 * position is zero; the first page with a nonzero granule is audio.
 * That is true of both codecs here -- Vorbis has three header packets
 * and Opus two, and neither can have produced any samples yet -- and it
 * is far more robust than counting packets, which for Vorbis means
 * walking a segment table across page boundaries to find where the
 * setup header ends.
 */
bool ogg_seek_probe(FILE *f, ogg_seek_t *os)
{
    if (!f || !os) return false;

    const long saved = ftell(f);
    memset(os, 0, sizeof(*os));
    bool ok = false;

    uint8_t *buf = big_alloc(OGG_HEADER_MAX);
    if (!buf) goto out;

    const long end = file_size(f);
    if (end < OGG_HDR) goto out;

    long want = end < OGG_HEADER_MAX ? end : OGG_HEADER_MAX;
    if (!read_at(f, 0, buf, (size_t)want)) goto out;

    ogg_page_t first;
    if (!page_at(buf, (size_t)want, &first)) goto out;
    os->serial = first.serial;

    /* The codec, from the first page's payload, which starts after the
     * header and its segment table. Both identify themselves in their
     * first packet, which is why this does not have to guess from the
     * file extension -- .ogg and .opus are both routed to the same
     * parser and either can hold either. */
    const size_t p0 = OGG_HDR + (size_t)buf[26];
    if (p0 + 19 > (size_t)want) goto out;
    const uint8_t *id = buf + p0;

    if (memcmp(id, "OpusHead", 8) == 0) {
        os->opus = true;
        os->rate = 48000;               /* granule is ALWAYS 48 kHz units,
                                         * whatever the input rate field
                                         * says -- the classic way to get
                                         * an Opus position wrong by a
                                         * constant factor */
        os->pre_skip = (uint32_t)id[10] | ((uint32_t)id[11] << 8);
    } else if (id[0] == 0x01 && memcmp(id + 1, "vorbis", 6) == 0) {
        os->opus = false;
        os->rate = le32(id + 12);
    } else {
        goto out;                       /* FLAC-in-Ogg, Speex, Theora... */
    }
    if (!os->rate) goto out;

    /* Walk pages until one carries audio. */
    long pos = 0;
    for (int guard = 0; guard < 64; guard++) {
        ogg_page_t pg;
        if (!page_at(buf + pos, (size_t)want - (size_t)pos, &pg)) break;
        if (pg.serial != os->serial) break;
        if (pg.granule != 0) break;             /* audio starts here */
        pos += pg.size;
        if (pos + OGG_HDR > want) break;
    }
    if (pos <= 0 || pos >= want) goto out;

    os->header = malloc((size_t)pos);
    if (!os->header) goto out;
    memcpy(os->header, buf, (size_t)pos);
    os->header_len = (size_t)pos;
    os->first_audio = pos;
    os->file_end = end;

    /*
     * The last granule, which is the length. duration.c reads the same
     * number for the same reason and by the same method -- the spec
     * caps a page at about 64 KB, so the last page starts inside a
     * 64 KB tail window. It is read here as well rather than shared,
     * because this needs it as a CLAMP: a bisection whose target is
     * past the end of the stream converges on the last page, which is
     * correct but takes every round to get there.
     */
    {
        long tw = end < OGG_HEADER_MAX ? end : OGG_HEADER_MAX;
        const long start = end - tw;
        if (read_at(f, start, buf, (size_t)tw)) {
            for (long i = tw - OGG_HDR; i >= 0; i--) {
                ogg_page_t pg;
                if (!page_at(buf + i, (size_t)(tw - i), &pg)) continue;
                if (pg.serial != os->serial) continue;
                if (pg.granule == UINT64_MAX) continue;   /* no packet ends */
                os->last_granule = pg.granule;
                break;
            }
        }
    }

    ok = true;

out:
    free(buf);
    if (saved >= 0) fseek(f, saved, SEEK_SET);
    if (ok) {
        os->ok = true;
        ESP_LOGI(TAG, "%s: %" PRIu32 " Hz granule, %u B of headers, "
                      "audio at %ld, %" PRIu32 " s",
                 os->opus ? "opus" : "vorbis", os->rate,
                 (unsigned)os->header_len, os->first_audio,
                 os->last_granule ? (uint32_t)((os->last_granule -
                                                (os->pre_skip < os->last_granule
                                                 ? os->pre_skip : 0)) / os->rate)
                                  : 0);
    } else {
        free(os->header);
        memset(os, 0, sizeof(*os));
    }
    return ok;
}

void ogg_seek_free(ogg_seek_t *os)
{
    if (!os) return;
    free(os->header);
    os->header = NULL;
    os->header_len = 0;
    os->ok = false;
}

/* ------------------------------------------------------------------ */
/* Bisection                                                           */
/* ------------------------------------------------------------------ */

/*
 * The window has to be able to hold a page start, and a page can be
 * 65307 bytes. Reading that much fifteen times per drag is a megabyte
 * off the card for one press, so the ordinary window is 24 KB -- five
 * or six typical audio pages -- and the full page size is the fallback
 * for the probe that finds nothing, which on real files is the last
 * round or two and not the common case.
 */
#define OGG_WIN      (24 * 1024)
#define OGG_WIN_MAX  (OGG_PAGE_MAX + 1024)

static uint64_t granule_of_sec(const ogg_seek_t *os, uint32_t sec)
{
    return (uint64_t)sec * os->rate + os->pre_skip;
}

static uint32_t sec_of_granule(const ogg_seek_t *os, uint64_t g)
{
    if (g <= os->pre_skip) return 0;
    return (uint32_t)((g - os->pre_skip) / os->rate);
}

long ogg_seek_find(FILE *f, const ogg_seek_t *os, uint32_t sec,
                   uint32_t *landed_sec)
{
    if (!f || !os || !os->ok) return -1;

    uint64_t target = granule_of_sec(os, sec);
    if (os->last_granule && target >= os->last_granule) {
        target = os->last_granule - 1;
    }

    uint8_t *buf = big_alloc(OGG_WIN_MAX);
    if (!buf) return -1;

    long lo = os->first_audio, hi = os->file_end;
    long best = os->first_audio;
    uint64_t best_granule = 0;
    bool have_best = false;

    /* Bounded, for the reason flacseek.c gives: the interval shrinks by
     * landing on a page whose position the data decides, and a bound on
     * the decode loop is worth more than a proof about the data. */
    for (int round = 0; round < 24 && lo < hi; round++) {
        long mid = lo + (hi - lo) / 2;
        if (mid < os->first_audio) mid = os->first_audio;

        ogg_page_t pg;
        long at = -1;
        for (int attempt = 0; attempt < 2 && at < 0; attempt++) {
            const long cap = attempt ? OGG_WIN_MAX : OGG_WIN;
            long want = os->file_end - mid;
            if (want > cap) want = cap;
            if (want < OGG_HDR) break;
            if (!read_at(f, mid, buf, (size_t)want)) break;
            at = find_page(buf, (size_t)want, os->serial, &pg);
        }
        if (at < 0) { hi = mid; continue; }

        const long off = mid + at;

        /*
         * A page that continues a packet is not a landing site: the
         * parser would be handed the tail of a packet whose head it has
         * never seen, and `append_packet` would splice it onto nothing.
         * It is still perfectly good evidence about WHERE we are, so it
         * still moves the interval -- it just cannot be `best`.
         *
         * Likewise a granule of -1, which means no packet finishes on
         * this page and says nothing about position at all.
         */
        const bool usable = !pg.continued && pg.granule != UINT64_MAX;

        if (pg.granule != UINT64_MAX && pg.granule <= target) {
            if (usable) {
                best = off;
                best_granule = pg.granule;
                have_best = true;
            }
            if (off <= lo) break;
            lo = off + 1;
        } else {
            hi = mid;
        }
    }

    /*
     * WHERE THE AUDIO ACTUALLY RESUMES IS THE PREVIOUS PAGE'S GRANULE.
     *
     * A page's granule position is the position of the END of the last
     * packet that finishes on it. So resuming from the page we settled
     * on produces audio starting where the page BEFORE it ended, and
     * reporting this page's own granule would put the clock up to one
     * page ahead of the sound -- 20 to 200 ms depending on codec and
     * encoder, permanently, for the rest of the track.
     *
     * One extra read to get it right: the window ending at the landing
     * page, scanned for the last page start in it. If the predecessor
     * is not in that window the page is enormous and this falls back to
     * the landing page's own granule, which is the old approximation
     * and is logged nowhere because it is bounded by the same page.
     */
    if (have_best && best > os->first_audio) {
        long from = best - OGG_WIN;
        if (from < os->first_audio) from = os->first_audio;
        const long want = best - from;
        if (want >= OGG_HDR && read_at(f, from, buf, (size_t)want)) {
            size_t at = 0;
            uint64_t prev = 0;
            bool found = false;
            while (at + OGG_HDR <= (size_t)want) {
                ogg_page_t pg;
                const long i = find_page(buf + at, (size_t)want - at,
                                         os->serial, &pg);
                if (i < 0) break;
                if (pg.granule != UINT64_MAX) { prev = pg.granule; found = true; }
                at += (size_t)i + (size_t)pg.size;
            }
            if (found) best_granule = prev;
        }
    }

    free(buf);

    /*
     * Falling back to the first audio page is the honest answer when
     * the target is inside it, and the only answer when every candidate
     * was a continuation. A seek that lands at the start of the track
     * is visibly wrong on the bar and is not a wrong POSITION -- the
     * clock is anchored to where it landed, so the two agree.
     */
    if (landed_sec) {
        *landed_sec = have_best ? sec_of_granule(os, best_granule) : 0;
    }
    return best;
}

const uint8_t *ogg_seek_preamble(const ogg_seek_t *os, size_t *len)
{
    if (!os || !os->ok || !os->header) { if (len) *len = 0; return NULL; }
    if (len) *len = os->header_len;
    return os->header;
}
