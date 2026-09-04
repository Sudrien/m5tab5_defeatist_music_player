/*
 * covertag.c -- see covertag.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "covertag.h"
#include "storage_io.h"

static const char *TAG = "tab5_cover";

/* ------------------------------------------------------------------ */
/* Byte and file helpers                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static inline uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}

/*
 * Every read in this file goes through here, which is what makes the
 * arbiter a one-function change: the lease is taken around each read
 * inside the parser rather than around the parser.
 *
 * Wrapping covertag_extract_art() instead would have been one line in
 * player.c and would have put the starvation straight back -- a single
 * lease held across a 512 KB cover is the uninterruptible read this is
 * meant to break up. storage_io.c logs that mistake if it is ever made.
 *
 * The priority is a parameter and not a constant, which it used to be:
 * every read here was STORAGE_IO_PREFETCH. That is right for the next
 * track and wrong for the current one, and both go through this file.
 * load_tags() runs on the decode loop at the track change and do_art()
 * on media_task behind it -- both about the song being listened to --
 * while prefetch_next() and its neighbours are about a song nobody has
 * heard yet. Filing the first two as prefetch queued the playing track's
 * own tags and cover behind, and at equal standing with, work for a
 * track that might never be reached.
 *
 * Threaded through every parser rather than kept in a file-scope
 * variable, because those two callers are on different tasks and can be
 * in here at the same time. A shared "current priority" would be read by
 * whichever one happened to look after the other one set it.
 */
static bool read_at(FILE *f, storage_io_class_t prio,
                    long off, void *buf, size_t len)
{
    return storage_io_read_at(f, off, buf, len, prio);
}

static long file_size(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    return ftell(f);
}

/*
 * Copy UTF-8 in, stopping on a character boundary.
 *
 * Vorbis comments are UTF-8 by specification and MP4 text atoms are
 * UTF-8 by type code, so nothing is converted here -- but a 64 byte
 * field will cut a long title somewhere, and cutting it mid-sequence is
 * how the last visible character becomes a replacement box. Same
 * reasoning as albumart.c's utf8_put(), one layer up.
 */
static void tag_copy_utf8(char *dst, size_t dst_len, const uint8_t *src, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        const uint8_t b = src[i];
        if (b == 0) break;
        size_t n = (b < 0x80) ? 1 : ((b & 0xE0) == 0xC0) ? 2
                 : ((b & 0xF0) == 0xE0) ? 3 : ((b & 0xF8) == 0xF0) ? 4 : 1;
        if (i + n > len) break;
        if (o + n + 1 > dst_len) break;
        memcpy(&dst[o], &src[i], n);
        o += n;
        i += n;
    }
    dst[o] = 0;
}

/* Case-insensitive compare of a fixed-length key against a literal. */
static bool key_is(const uint8_t *p, size_t len, const char *name)
{
    const size_t n = strlen(name);
    if (len != n) return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (uint8_t)name[i]) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* base64                                                              */
/* ------------------------------------------------------------------ */

/*
 * Decoded in place, into a buffer the caller already owns.
 *
 * In place because METADATA_BLOCK_PICTURE is the one field here that is
 * base64, it can be a megabyte, and allocating a second megabyte to
 * decode a third of it away is the difference between working and not on
 * a device where the artwork task runs alongside a decoder. Output is
 * always shorter than input, so the write pointer never overtakes the
 * read pointer.
 *
 * Whitespace is skipped -- some taggers wrap the field -- and any other
 * stray byte ends the decode rather than being treated as zero, because
 * a silently truncated picture is easier to debug than a corrupt one.
 */
static size_t base64_decode_inplace(uint8_t *buf, size_t len)
{
    static const int8_t T[256] = {
        ['A']= 0,['B']= 1,['C']= 2,['D']= 3,['E']= 4,['F']= 5,['G']= 6,['H']= 7,
        ['I']= 8,['J']= 9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
    };

    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < len; i++) {
        const uint8_t c = buf[i];
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c != 'A' && T[c] == 0) break;           /* not base64 */
        acc = (acc << 6) | (uint32_t)T[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* FLAC METADATA_BLOCK_PICTURE                                         */
/* ------------------------------------------------------------------ */

/*
 * The picture block body, whether it came from a FLAC metadata block or
 * from a base64 Ogg comment -- they are the same structure, which is the
 * only reason the Ogg path is affordable.
 *
 * Layout: BE32 picture type, BE32 MIME length + MIME, BE32 description
 * length + description, four BE32s of geometry nobody needs, BE32 data
 * length, data.
 *
 * Every length is read from the file, so every one of them is checked
 * against what is actually left. The four geometry fields are skipped
 * rather than validated: they are advisory, taggers get them wrong, and
 * nothing here uses them.
 */
static esp_err_t picture_block_parse(const uint8_t *b, size_t len,
                                     uint8_t **out, size_t *out_len,
                                     uint32_t *pic_type)
{
    size_t i = 0;
    if (len < 8) return ESP_ERR_INVALID_SIZE;

    const uint32_t type = be32(&b[i]); i += 4;

    const uint32_t mime_len = be32(&b[i]); i += 4;
    if (mime_len > len - i) return ESP_ERR_INVALID_SIZE;
    i += mime_len;

    if (len - i < 4) return ESP_ERR_INVALID_SIZE;
    const uint32_t desc_len = be32(&b[i]); i += 4;
    if (desc_len > len - i) return ESP_ERR_INVALID_SIZE;
    i += desc_len;

    if (len - i < 20) return ESP_ERR_INVALID_SIZE;
    i += 16;                                    /* w, h, depth, colours */

    const uint32_t data_len = be32(&b[i]); i += 4;
    if (data_len > len - i || data_len == 0) return ESP_ERR_INVALID_SIZE;
    if (data_len > COVERTAG_MAX_IMAGE) return ESP_ERR_INVALID_SIZE;

    if (!albumart_is_supported_image(&b[i], data_len)) return ESP_ERR_NOT_SUPPORTED;

    uint8_t *img = malloc(data_len);
    if (!img) return ESP_ERR_NO_MEM;
    memcpy(img, &b[i], data_len);

    *out = img;
    *out_len = data_len;
    if (pic_type) *pic_type = type;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* VorbisComment                                                       */
/* ------------------------------------------------------------------ */

/*
 * Walk a VorbisComment block, filling tags and/or picking up a
 * METADATA_BLOCK_PICTURE.
 *
 * One walk for both because in Ogg they arrive in the same packet and
 * splitting them would mean reassembling that packet twice. `tags` and
 * `out` are each optional; passing only one is how the FLAC path uses
 * this, where the picture lives in its own block.
 *
 * The buffer is mutable because the base64 decode happens in place.
 * Nothing else writes to it.
 */
static void vorbis_comment_walk(uint8_t *b, size_t len,
                                id3_tags_t *tags,
                                uint8_t **out, size_t *out_len)
{
    size_t i = 0;
    if (len < 8) return;

    const uint32_t vendor_len = le32(&b[i]); i += 4;
    if (vendor_len > len - i) return;
    i += vendor_len;

    if (len - i < 4) return;
    uint32_t count = le32(&b[i]); i += 4;

    /* A corrupt count would otherwise spin until the bounds check below
     * happens to fail; there cannot be more comments than there is room
     * for a 4 byte length each. */
    if (count > (len - i) / 4) count = (uint32_t)((len - i) / 4);

    for (uint32_t n = 0; n < count; n++) {
        if (len - i < 4) return;
        const uint32_t clen = le32(&b[i]); i += 4;
        if (clen > len - i) return;

        uint8_t *c = &b[i];
        i += clen;

        /* KEY=value. No '=' means a malformed comment, not a key with an
         * empty value -- skip it rather than guessing. */
        size_t eq = 0;
        while (eq < clen && c[eq] != '=') eq++;
        if (eq == clen) continue;

        const uint8_t *val = c + eq + 1;
        const size_t val_len = clen - eq - 1;

        if (tags) {
            if      (key_is(c, eq, "TITLE"))  tag_copy_utf8(tags->title,  sizeof(tags->title),  val, val_len);
            else if (key_is(c, eq, "ARTIST")) tag_copy_utf8(tags->artist, sizeof(tags->artist), val, val_len);
            else if (key_is(c, eq, "ALBUM"))  tag_copy_utf8(tags->album,  sizeof(tags->album),  val, val_len);
        }

        if (out && !*out && key_is(c, eq, "METADATA_BLOCK_PICTURE")) {
            /* Decoded over the comment's own bytes. The walk has already
             * taken its length, so consuming it is safe. */
            const size_t raw = base64_decode_inplace((uint8_t *)val, val_len);
            uint32_t pic_type = 0;
            if (picture_block_parse(val, raw, out, out_len, &pic_type) != ESP_OK) {
                *out = NULL;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* FLAC                                                                */
/* ------------------------------------------------------------------ */

/*
 * Metadata block header: one byte of (last-block flag << 7 | type), then
 * a 24 bit big-endian length. Type 4 is VORBIS_COMMENT, type 6 PICTURE.
 *
 * Both are wanted, and a file can hold several PICTUREs -- front cover,
 * back cover, a scan of the liner notes. Type 3 (front cover) is
 * preferred, and anything else is kept only as a fallback, because
 * showing the back of the sleeve when the front is right there is the
 * kind of wrong that looks like a bug.
 */
static esp_err_t flac_read(FILE *f, storage_io_class_t prio, long base, id3_tags_t *tags,
                           uint8_t **out, size_t *out_len)
{
    uint8_t magic[4];
    if (!read_at(f, prio, base, magic, 4) || memcmp(magic, "fLaC", 4) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const long end = file_size(f);
    long pos = base + 4;
    int found = 0;
    bool have_front = false;

    while (pos + 4 <= end) {
        uint8_t bh[4];
        if (!read_at(f, prio, pos, bh, 4)) break;

        const bool last = (bh[0] & 0x80) != 0;
        const uint8_t type = bh[0] & 0x7F;
        const uint32_t blen = be24(&bh[1]);
        pos += 4;

        if (pos + (long)blen > end) break;

        const bool want_pic = (out && type == 6 && !have_front);
        const bool want_cmt = (tags && type == 4);

        if ((want_pic || want_cmt) && blen > 0 && blen <= COVERTAG_MAX_IMAGE) {
            uint8_t *b = malloc(blen);
            if (!b) return ESP_ERR_NO_MEM;
            if (!read_at(f, prio, pos, b, blen)) { free(b); break; }

            if (want_cmt) {
                vorbis_comment_walk(b, blen, tags, NULL, NULL);
                /* Counted only if something landed. A VORBIS_COMMENT
                 * block with no TITLE/ARTIST/ALBUM in it is not a
                 * successful read -- returning OK for it would tell
                 * player.c the tags are good and suppress the filename
                 * fallback, leaving the title row blank. */
                if (tags->title[0] || tags->artist[0] || tags->album[0]) found++;
            } else {
                uint8_t *img = NULL;
                size_t img_len = 0;
                uint32_t pic_type = 0;
                if (picture_block_parse(b, blen, &img, &img_len, &pic_type) == ESP_OK) {
                    /* A later front cover replaces an earlier
                     * something-else; a later something-else does not
                     * replace anything. */
                    if (!*out || pic_type == 3) {
                        free(*out);
                        *out = img;
                        *out_len = img_len;
                        have_front = (pic_type == 3);
                        found++;
                    } else {
                        free(img);
                    }
                }
            }
            free(b);
        }

        pos += blen;
        if (last) break;
    }

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* MP4 / M4A                                                           */
/* ------------------------------------------------------------------ */

/*
 * Atoms are size (BE32) then a four character type. A size of 1 means
 * the real size is a 64 bit value after the type; a size of 0 means the
 * atom runs to the end of the file. Both appear in the wild and both
 * used to be the bug that made an M4A parser walk off a cliff.
 *
 * The path to the tags is moov -> udta -> meta -> ilst -> item -> data,
 * and `meta` is the trap: it is a full atom, so it carries four bytes of
 * version and flags before its children, which nothing else on this path
 * does. Walking it like a plain container puts you four bytes out and
 * every child type reads as garbage.
 */
static bool atom_next(FILE *f, storage_io_class_t prio, long pos, long end, char type[4], long *body, long *next)
{
    uint8_t h[8];
    if (pos + 8 > end || !read_at(f, prio, pos, h, 8)) return false;

    uint64_t sz = be32(h);
    long hdr = 8;

    if (sz == 1) {
        uint8_t ext[8];
        if (pos + 16 > end || !read_at(f, prio, pos + 8, ext, 8)) return false;
        sz = ((uint64_t)be32(ext) << 32) | be32(ext + 4);
        hdr = 16;
    } else if (sz == 0) {
        sz = (uint64_t)(end - pos);
    }

    if (sz < (uint64_t)hdr || pos + (long)sz > end) return false;

    memcpy(type, h + 4, 4);
    *body = pos + hdr;
    *next = pos + (long)sz;
    return true;
}

/* Find a direct child of [pos, end) by type. `skip` is the version and
 * flags word that `meta` has and the others do not. */
static bool atom_find(FILE *f, storage_io_class_t prio, long pos, long end, const char *want, long skip,
                      long *body, long *body_end)
{
    pos += skip;
    char type[4];
    long b, next;
    while (atom_next(f, prio, pos, end, type, &b, &next)) {
        if (memcmp(type, want, 4) == 0) {
            *body = b;
            *body_end = next;
            return true;
        }
        pos = next;
    }
    return false;
}

/*
 * The payload of an item atom: a 'data' child whose first byte-pair is a
 * type indicator -- 1 UTF-8, 13 JPEG, 14 PNG -- then four bytes of
 * locale, then the value.
 */
static uint8_t *ilst_data(FILE *f, storage_io_class_t prio, long pos, long end, size_t *len, uint32_t *kind)
{
    long b, be;
    if (!atom_find(f, prio, pos, end, "data", 0, &b, &be)) return NULL;
    if (be - b < 8) return NULL;

    uint8_t hdr[8];
    if (!read_at(f, prio, b, hdr, 8)) return NULL;

    const size_t n = (size_t)(be - b - 8);
    if (n == 0 || n > COVERTAG_MAX_IMAGE) return NULL;

    uint8_t *v = malloc(n);
    if (!v) return NULL;
    if (!read_at(f, prio, b + 8, v, n)) { free(v); return NULL; }

    *len = n;
    *kind = be32(hdr) & 0xFFFFFF;
    return v;
}

static esp_err_t mp4_read(FILE *f, storage_io_class_t prio, id3_tags_t *tags,
                          uint8_t **out, size_t *out_len)
{
    const long end = file_size(f);
    if (end < 8) return ESP_ERR_NOT_FOUND;

    long moov, moov_end;
    if (!atom_find(f, prio, 0, end, "moov", 0, &moov, &moov_end)) return ESP_ERR_NOT_FOUND;

    long udta, udta_end;
    if (!atom_find(f, prio, moov, moov_end, "udta", 0, &udta, &udta_end)) return ESP_ERR_NOT_FOUND;

    long meta, meta_end;
    if (!atom_find(f, prio, udta, udta_end, "meta", 0, &meta, &meta_end)) return ESP_ERR_NOT_FOUND;

    /* The four bytes that make `meta` different from everything above. */
    long ilst, ilst_end;
    if (!atom_find(f, prio, meta, meta_end, "ilst", 4, &ilst, &ilst_end)) return ESP_ERR_NOT_FOUND;

    int found = 0;
    long pos = ilst;
    char type[4];
    long b, next;

    while (atom_next(f, prio, pos, ilst_end, type, &b, &next)) {
        char *dst = NULL;
        size_t dst_len = 0;

        /*
         * The (c) prefix is the single byte 0xA9, not the two characters
         * -- and it is split out of the literal deliberately. Written as
         * "\xA9ART", C reads \xA9A as one hex escape, because 'A' is a
         * hex digit; the string is then three bytes, the memcmp compares
         * garbage, and no M4A ever reports an artist. 'a' in "alb" does
         * the same thing. Only "nam" is safe, which is the worst
         * possible outcome: it would have looked like it worked.
         */
        if      (!memcmp(type, "\xA9" "nam", 4) && tags) { dst = tags->title;  dst_len = sizeof(tags->title);  }
        else if (!memcmp(type, "\xA9" "ART", 4) && tags) { dst = tags->artist; dst_len = sizeof(tags->artist); }
        else if (!memcmp(type, "\xA9" "alb", 4) && tags) { dst = tags->album;  dst_len = sizeof(tags->album);  }

        if (dst) {
            size_t n = 0;
            uint32_t kind = 0;
            uint8_t *v = ilst_data(f, prio, b, next, &n, &kind);
            if (v) {
                tag_copy_utf8(dst, dst_len, v, n);
                free(v);
                found++;
            }
        } else if (!memcmp(type, "covr", 4) && out && !*out) {
            size_t n = 0;
            uint32_t kind = 0;
            uint8_t *v = ilst_data(f, prio, b, next, &n, &kind);
            if (v) {
                /* Trusting the bytes over the type indicator: taggers
                 * write 13 for a PNG often enough that the indicator is
                 * a hint, and the magic bytes are not. */
                if (albumart_is_supported_image(v, n)) {
                    *out = v;
                    *out_len = n;
                    found++;
                } else {
                    ESP_LOGW(TAG, "covr atom is neither JPEG nor PNG (kind %u)",
                             (unsigned)kind);
                    free(v);
                }
            }
        }

        pos = next;
    }

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Ogg                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The comment header is the second packet of the logical stream, and it
 * is the only packet here worth reassembling.
 *
 * Ogg pages carry a segment table of up to 255 lengths; a segment of
 * exactly 255 means the packet continues into the next segment, and a
 * packet that ends the page with a 255 continues onto the next page. A
 * cover of any size does exactly that -- which is why this cannot be
 * done by reading a fixed prefix of the file, the way the tags alone
 * could be.
 *
 * Two guards. Pages are only read while the packet is still open, so a
 * file with no comment header costs a few KB rather than a full scan.
 * And the reassembled packet is capped: base64 inflates by a third, so
 * the cap is COVERTAG_MAX_IMAGE plus room for the encoding and the rest
 * of the comments.
 */
#define OGG_MAX_PACKET  (COVERTAG_MAX_IMAGE + COVERTAG_MAX_IMAGE / 2)

static esp_err_t ogg_read(FILE *f, storage_io_class_t prio, id3_tags_t *tags,
                          uint8_t **out, size_t *out_len)
{
    const long end = file_size(f);
    long pos = 0;
    int packet = 0;                 /* packets completed on this stream */
    uint32_t serial = 0;
    bool have_serial = false;

    uint8_t *pkt = NULL;
    size_t pkt_len = 0;
    size_t pkt_cap = 0;
    bool collecting = false;
    esp_err_t rc = ESP_ERR_NOT_FOUND;

    while (pos + 27 <= end) {
        uint8_t ph[27];
        if (!read_at(f, prio, pos, ph, 27) || memcmp(ph, "OggS", 4) != 0) break;

        const uint32_t this_serial = le32(&ph[14]);
        const int nsegs = ph[26];

        uint8_t segs[255];
        if (!read_at(f, prio, pos + 27, segs, (size_t)nsegs)) break;

        const long body = pos + 27 + nsegs;
        long body_len = 0;
        for (int i = 0; i < nsegs; i++) body_len += segs[i];
        if (body + body_len > end) break;

        /* Chained or multiplexed streams: follow the first one seen and
         * ignore the rest. A video track in an Ogg would otherwise have
         * its own second packet counted as the comment header. */
        if (!have_serial) { serial = this_serial; have_serial = true; }
        if (this_serial != serial) { pos = body + body_len; continue; }

        long seg_off = body;
        for (int i = 0; i < nsegs; i++) {
            const size_t slen = segs[i];

            if (packet == 1 || collecting) {
                if (pkt_len + slen > OGG_MAX_PACKET) {
                    ESP_LOGW(TAG, "Ogg comment header past the cap; dropped");
                    free(pkt);
                    return ESP_ERR_INVALID_SIZE;
                }
                /*
                 * Doubling, not realloc-per-segment. Segments are at
                 * most 255 bytes, so a 1 MB cover arrives in four
                 * thousand pieces; growing by exactly what each one
                 * needs is four thousand reallocs, and on a heap this
                 * task shares with a running decoder that is both slow
                 * and a good way to fragment PSRAM.
                 */
                if (pkt_len + slen + 1 > pkt_cap) {
                    size_t want = pkt_cap ? pkt_cap * 2 : 4096;
                    while (want < pkt_len + slen + 1) want *= 2;
                    uint8_t *grown = realloc(pkt, want);
                    if (!grown) { free(pkt); return ESP_ERR_NO_MEM; }
                    pkt = grown;
                    pkt_cap = want;
                }
                if (slen && !read_at(f, prio, seg_off, pkt + pkt_len, slen)) {
                    free(pkt);
                    return ESP_ERR_INVALID_SIZE;
                }
                pkt_len += slen;
                collecting = true;
            }

            seg_off += slen;

            if (slen != 255) {
                /* Packet boundary. */
                if (collecting) goto complete;
                packet++;
            }
        }

        pos = body + body_len;
    }

    free(pkt);
    return rc;

complete:
    /*
     * Two codecs, two framings of the same VorbisComment block. Vorbis
     * prefixes packet type 3 and the string "vorbis"; Opus prefixes
     * "OpusTags". Anything else is a codec there is no reader for, which
     * is not an error worth logging loudly -- Ogg is a container and
     * carries plenty this player never opens.
     */
    {
        size_t off = 0;
        if (pkt_len > 7 && pkt[0] == 3 && memcmp(pkt + 1, "vorbis", 6) == 0) {
            off = 7;
        } else if (pkt_len > 8 && memcmp(pkt, "OpusTags", 8) == 0) {
            off = 8;
        } else {
            free(pkt);
            return ESP_ERR_NOT_SUPPORTED;
        }

        vorbis_comment_walk(pkt + off, pkt_len - off, tags, out, out_len);

        /* Same rule as the FLAC path: OK means something was found, not
         * that a comment block was present. */
        const bool got_tags = tags && (tags->title[0] || tags->artist[0] || tags->album[0]);
        const bool got_art  = out && *out;
        rc = (got_tags || got_art) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    free(pkt);
    return rc;
}

/* ------------------------------------------------------------------ */
/* WAV                                                                 */
/* ------------------------------------------------------------------ */

/*
 * RIFF chunks, looking for 'id3 ' -- which is where a tagger puts an
 * ID3v2 tag in a WAV, and is the only place a WAV keeps anything this
 * player can use. The tag itself is albumart.c's problem, which is what
 * the _at() variants are for.
 *
 * LIST/INFO is the other convention and carries no picture at all, so it
 * is not read: the three strings it could supply are the three the
 * filename already supplies.
 */
static bool wav_find_id3(FILE *f, storage_io_class_t prio, long *off)
{
    const long end = file_size(f);
    uint8_t hdr[12];
    if (!read_at(f, prio, 0, hdr, 12)) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    long pos = 12;
    while (pos + 8 <= end) {
        uint8_t ch[8];
        if (!read_at(f, prio, pos, ch, 8)) break;
        const uint32_t sz = le32(&ch[4]);
        if (pos + 8 + (long)sz > end) break;

        if (memcmp(ch, "id3 ", 4) == 0 || memcmp(ch, "ID3 ", 4) == 0) {
            *off = pos + 8;
            return true;
        }
        pos += 8 + sz + (sz & 1);               /* chunks are word aligned */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/*
 * An ID3v2 tag can sit in front of a FLAC or an Ogg. It is not legal in
 * either, and taggers do it anyway; skipping past it costs one read and
 * turns "unrecognised container" into a working file.
 */
static long skip_leading_id3(FILE *f, storage_io_class_t prio)
{
    uint8_t h[10];
    if (!read_at(f, prio, 0, h, 10) || memcmp(h, "ID3", 3) != 0) return 0;
    long off = 10 + (long)syncsafe32(&h[6]);
    if (h[5] & 0x10) off += 10;                 /* footer */
    return off;
}

typedef enum {
    FMT_UNKNOWN,
    FMT_ID3,
    FMT_FLAC,
    FMT_OGG,
    FMT_MP4,
    FMT_WAV,
} fmt_t;

static fmt_t sniff(FILE *f, storage_io_class_t prio, long *base)
{
    uint8_t m[12];

    *base = 0;
    if (!read_at(f, prio, 0, m, sizeof(m))) return FMT_UNKNOWN;

    if (memcmp(m, "fLaC", 4) == 0) return FMT_FLAC;
    if (memcmp(m, "OggS", 4) == 0) return FMT_OGG;
    if (memcmp(m, "RIFF", 4) == 0 && memcmp(m + 8, "WAVE", 4) == 0) return FMT_WAV;
    if (memcmp(m + 4, "ftyp", 4) == 0) return FMT_MP4;

    if (memcmp(m, "ID3", 3) == 0) {
        /* Could be an MP3, or could be a tagger's ID3 bolted onto a FLAC
         * or an Ogg. Look past it before deciding. */
        const long off = skip_leading_id3(f, prio);
        uint8_t n[4];
        if (read_at(f, prio, off, n, 4)) {
            if (memcmp(n, "fLaC", 4) == 0) { *base = off; return FMT_FLAC; }
            if (memcmp(n, "OggS", 4) == 0) { *base = off; return FMT_OGG;  }
        }
        return FMT_ID3;
    }

    return FMT_UNKNOWN;
}

esp_err_t covertag_extract_art(FILE *f, storage_io_class_t prio,
                               uint8_t **out, size_t *out_len)
{
    long base = 0;

    *out = NULL;
    *out_len = 0;

    switch (sniff(f, prio, &base)) {
    case FMT_ID3:
        return albumart_extract_at(f, prio, 0, out, out_len);

    case FMT_FLAC: {
        const esp_err_t err = flac_read(f, prio, base, NULL, out, out_len);
        return (err == ESP_OK && *out) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    case FMT_OGG: {
        /* base is ignored: ogg_read() scans for the capture pattern from
         * zero, and a leading ID3 has no "OggS" in it to trip on. */
        const esp_err_t err = ogg_read(f, prio, NULL, out, out_len);
        return (err == ESP_OK && *out) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    case FMT_MP4: {
        const esp_err_t err = mp4_read(f, prio, NULL, out, out_len);
        return (err == ESP_OK && *out) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    case FMT_WAV: {
        long off;
        if (!wav_find_id3(f, prio, &off)) return ESP_ERR_NOT_FOUND;
        return albumart_extract_at(f, prio, off, out, out_len);
    }

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t covertag_read_tags(FILE *f, storage_io_class_t prio, id3_tags_t *out)
{
    long base = 0;

    memset(out, 0, sizeof(*out));

    switch (sniff(f, prio, &base)) {
    case FMT_ID3:
        return id3_read_tags_at(f, 0, out);

    case FMT_FLAC:
        return flac_read(f, prio, base, out, NULL, NULL);

    case FMT_OGG:
        return ogg_read(f, prio, out, NULL, NULL);

    case FMT_MP4:
        return mp4_read(f, prio, out, NULL, NULL);

    case FMT_WAV: {
        long off;
        if (!wav_find_id3(f, prio, &off)) return ESP_ERR_NOT_FOUND;
        return id3_read_tags_at(f, off, out);
    }

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
