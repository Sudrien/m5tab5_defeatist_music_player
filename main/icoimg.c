/*
 * icoimg -- see icoimg.h (5088).
 */
#include "icoimg.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ICO_ALLOC(n)  heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#else
#define ICO_ALLOC(n)  malloc(n)
#endif

static const uint8_t PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool icoimg_is_ico(const uint8_t *p, size_t len)
{
    return p && len >= 6 + 16 && rd16(p) == 0 && rd16(p + 2) == 1 &&
           rd16(p + 4) >= 1;
}

/* ------------------------------------------------------------------ */
/* The PNG writer: RGBA, 8-bit, filter 0, deflate as stored blocks.     */
/* ------------------------------------------------------------------ */

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* `raw` is h rows of (1 filter byte + w*4). Returns a new PNG or NULL. */
static uint8_t *png_stored(const uint8_t *raw, int w, int h, size_t *out_len)
{
    const size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    const size_t nblk = (raw_len + 65534) / 65535;
    const size_t idat = 2 + raw_len + 5 * nblk + 4;
    const size_t total = 8 + (12 + 13) + (12 + idat) + 12;

    uint8_t *png = ICO_ALLOC(total);
    if (!png) return NULL;
    uint8_t *o = png;

    memcpy(o, PNG_SIG, 8); o += 8;

    /* IHDR */
    wr32be(o, 13); memcpy(o + 4, "IHDR", 4);
    wr32be(o + 8, (uint32_t)w); wr32be(o + 12, (uint32_t)h);
    o[16] = 8; o[17] = 6; o[18] = 0; o[19] = 0; o[20] = 0;    /* RGBA8 */
    wr32be(o + 21, crc32_update(0, o + 4, 17));
    o += 25;

    /* IDAT: zlib header, stored blocks, Adler-32 */
    uint8_t *chunk = o;
    wr32be(o, (uint32_t)idat); memcpy(o + 4, "IDAT", 4); o += 8;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    size_t left = raw_len;
    const uint8_t *r = raw;
    while (left) {
        const uint16_t n = (uint16_t)(left > 65535 ? 65535 : left);
        *o++ = (left == n) ? 1 : 0;                          /* BFINAL, stored */
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, r, n);
        for (uint16_t i = 0; i < n; i++) {
            s1 += r[i]; if (s1 >= 65521) s1 -= 65521;
            s2 += s1;   if (s2 >= 65521) s2 -= 65521;
        }
        o += n; r += n; left -= n;
    }
    wr32be(o, (s2 << 16) | s1); o += 4;
    wr32be(o, crc32_update(0, chunk + 4, 4 + idat)); o += 4;

    /* IEND */
    wr32be(o, 0); memcpy(o + 4, "IEND", 4);
    wr32be(o + 8, crc32_update(0, o + 4, 4)); o += 12;

    *out_len = (size_t)(o - png);
    return png;
}

/* ------------------------------------------------------------------ */
/* The DIB reader.                                                      */
/* ------------------------------------------------------------------ */

static uint8_t *dib_to_png(const uint8_t *d, size_t dlen, int dir_w, int dir_h,
                           size_t *out_len, icoimg_info_t *info)
{
    if (dlen < 40) return NULL;
    const uint32_t hs = rd32(d);
    if (hs < 40 || hs > dlen) return NULL;

    const int32_t bw = (int32_t)rd32(d + 4);
    int32_t bh = (int32_t)rd32(d + 8);
    const int bpp = rd16(d + 14);
    const uint32_t comp = rd32(d + 16);
    const uint32_t clr_used = rd32(d + 32);

    const bool top_down = bh < 0;
    if (top_down) bh = -bh;
    /* The stored height counts the AND mask: twice the picture. A file
     * that did not double it is believed only if the directory agrees. */
    const int w = bw;
    const int h = (bh == dir_h && bh != 2 * dir_h) ? bh : bh / 2;
    (void)dir_w;
    if (w < 1 || h < 1 || w > ICOIMG_MAX_DIM || h > ICOIMG_MAX_DIM) return NULL;

    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) return NULL;
    if (!(comp == 0 || (comp == 3 && bpp == 32))) return NULL;   /* BI_RGB, or BITFIELDS as BGRA */

    size_t pos = hs;
    if (comp == 3 && hs == 40) pos += 12;        /* the three masks after a v1 header */

    const uint8_t *pal = NULL;
    uint32_t npal = 0;
    if (bpp <= 8) {
        npal = clr_used ? clr_used : (1u << bpp);
        if (npal > 256) return NULL;
        if (pos + 4 * (size_t)npal > dlen) return NULL;
        pal = d + pos;
        pos += 4 * (size_t)npal;
    }

    const size_t xstride = (((size_t)w * bpp + 31) / 32) * 4;
    const size_t astride = (((size_t)w + 31) / 32) * 4;
    if (pos + xstride * h > dlen) return NULL;
    const uint8_t *xor_px = d + pos;
    const uint8_t *and_px = d + pos + xstride * h;
    const bool have_and = pos + xstride * h + astride * h <= dlen;

    /* A 32-bit icon whose alpha is zero everywhere was written before
     * alpha meant anything; its transparency is in the mask. */
    bool use_alpha = false;
    if (bpp == 32) {
        for (int y = 0; y < h && !use_alpha; y++) {
            const uint8_t *row = xor_px + xstride * y;
            for (int x = 0; x < w; x++) {
                if (row[x * 4 + 3]) { use_alpha = true; break; }
            }
        }
    }

    const size_t rowlen = 1 + (size_t)w * 4;
    uint8_t *raw = ICO_ALLOC(rowlen * h);
    if (!raw) return NULL;

    for (int y = 0; y < h; y++) {
        const int sy = top_down ? y : h - 1 - y;          /* bottom-up by default */
        const uint8_t *xr = xor_px + xstride * sy;
        const uint8_t *ar = and_px + astride * sy;
        uint8_t *o = raw + rowlen * y;
        *o++ = 0;                                          /* filter: none */
        for (int x = 0; x < w; x++, o += 4) {
            uint8_t b, g, r, a = 255;
            if (bpp == 32) {
                b = xr[x * 4]; g = xr[x * 4 + 1]; r = xr[x * 4 + 2];
                if (use_alpha) a = xr[x * 4 + 3];
            } else if (bpp == 24) {
                b = xr[x * 3]; g = xr[x * 3 + 1]; r = xr[x * 3 + 2];
            } else {
                const int bit = x * bpp;
                const int shift = 8 - bpp - (bit & 7);
                const uint32_t idx = (xr[bit >> 3] >> shift) & ((1u << bpp) - 1);
                if (idx < npal) {
                    b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2];
                } else {
                    b = g = r = 0;
                }
            }
            if (!use_alpha && have_and && ((ar[x >> 3] >> (7 - (x & 7))) & 1)) a = 0;
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }

    uint8_t *png = png_stored(raw, w, h, out_len);
    free(raw);
    if (png && info) {
        info->w = w; info->h = h; info->bpp = bpp; info->was_png = false;
    }
    return png;
}

/* ------------------------------------------------------------------ */

bool icoimg_to_png(const uint8_t *ico, size_t len,
                   uint8_t **out, size_t *out_len, icoimg_info_t *info)
{
    if (!out || !out_len) return false;
    *out = NULL;
    *out_len = 0;
    if (!icoimg_is_ico(ico, len)) return false;

    const int count = rd16(ico + 4);
    /* Best first: the most pixels, then the deepest colour. Entries that
     * point outside the file are passed over, not trusted. */
    int best = -1;
    long best_area = -1;
    int best_bpp = -1;
    for (int i = 0; i < count; i++) {
        const size_t e = 6 + 16 * (size_t)i;
        if (e + 16 > len) break;
        const uint8_t *d = ico + e;
        const int w = d[0] ? d[0] : 256;
        const int h = d[1] ? d[1] : 256;
        const int bpp = rd16(d + 6);
        const uint32_t size = rd32(d + 8), off = rd32(d + 12);
        if (off >= len || size > len - off || size < 8) continue;
        const long area = (long)w * h;
        if (area > best_area || (area == best_area && bpp > best_bpp)) {
            best = i; best_area = area; best_bpp = bpp;
        }
    }
    if (best < 0) return false;

    const uint8_t *e = ico + 6 + 16 * (size_t)best;
    const int dir_w = e[0] ? e[0] : 256, dir_h = e[1] ? e[1] : 256;
    const uint8_t *d = ico + rd32(e + 12);
    const size_t dlen = rd32(e + 8);

    if (dlen >= 8 && memcmp(d, PNG_SIG, 8) == 0) {
        uint8_t *png = ICO_ALLOC(dlen);
        if (!png) return false;
        memcpy(png, d, dlen);
        *out = png;
        *out_len = dlen;
        if (info) { info->w = dir_w; info->h = dir_h; info->bpp = 0; info->was_png = true; }
        return true;
    }

    uint8_t *png = dib_to_png(d, dlen, dir_w, dir_h, out_len, info);
    if (!png) return false;
    *out = png;
    return true;
}
