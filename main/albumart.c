/*
 * ID3v2 APIC extraction and display on the Tab5's ST7121 panel.
 *
 * The ESP32-P4 has a hardware JPEG codec, so the art is decoded by the
 * jpeg_decode driver rather than in software -- a 1000x1000 cover takes
 * a few ms instead of most of a second, which matters because it runs
 * while audio is already streaming.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_md5.h"

#include "pngle.h"

#include "albumart.h"
#include "storage_io.h"
#include "gfx.h"

static const char *TAG = "tab5_art";

/* ------------------------------------------------------------------ */
/* ID3v2 APIC                                                          */
/* ------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* v2.4 frame sizes are syncsafe; v2.3 are plain big-endian. Getting
 * this backwards walks straight off the end of the first frame. */
static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}

/*
 * Find the first APIC frame holding a JPEG and return a malloc'd copy of
 * the image bytes. Caller frees.
 *
 * APIC payload: encoding byte, MIME string (NUL-terminated latin1),
 * picture type byte, description (NUL-terminated, two NULs for the
 * UTF-16 encodings), then the image.
 */
/*
 * Is this something albumart_show() can decode?
 *
 * Exported because every container's picture block ends in the same
 * question, and each of them answering it separately is how one of them
 * ends up accepting a BMP and failing further downstream, where the
 * error message is about the decoder rather than about the file.
 */
bool albumart_is_supported_image(const uint8_t *p, size_t len)
{
    if (!p) return false;
    if (len > 3 && p[0] == 0xFF && p[1] == 0xD8) return true;               /* JPEG */
    if (len > 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return true;    /* PNG */
    return false;
}

esp_err_t albumart_extract_at(FILE *f, storage_io_class_t cls, long base,
                              uint8_t **out, size_t *out_len)
{
    uint8_t hdr[10];

    *out = NULL;
    *out_len = 0;

    fseek(f, base, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const int ver = hdr[3];
    if (ver < 3) {
        ESP_LOGD(TAG, "ID3v2.%d predates APIC frames", ver);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr[5] & 0x40) {
        /* Extended header: skip it. Its own size field is syncsafe in
         * v2.4 and plain in v2.3, same trap as frame sizes. */
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) return ESP_ERR_INVALID_SIZE;
        const uint32_t esz = (ver >= 4) ? syncsafe32(ext) : be32(ext);
        fseek(f, (long)esz - ((ver >= 4) ? 4 : 0), SEEK_CUR);
    }

    const long tag_end = base + 10 + (long)syncsafe32(&hdr[6]);

    while (ftell(f) + 10 <= tag_end) {
        uint8_t fh[10];
        if (fread(fh, 1, sizeof(fh), f) != sizeof(fh)) break;
        if (fh[0] == 0) break;                      /* padding */

        const uint32_t fsz = (ver >= 4) ? syncsafe32(&fh[4]) : be32(&fh[4]);
        if (fsz == 0 || ftell(f) + (long)fsz > tag_end) break;

        if (memcmp(fh, "APIC", 4) != 0) {
            fseek(f, (long)fsz, SEEK_CUR);
            continue;
        }

        uint8_t *frame = malloc(fsz);
        if (!frame) return ESP_ERR_NO_MEM;
        /* The APIC frame is the cover itself -- the one read in this file
         * big enough to matter, and the half-megabyte that used to sit in
         * front of the decoder's next refill. The ten-byte header reads
         * around it are left alone: a lease costs two semaphore
         * operations and those reads are shorter than that. */
        if (storage_io_fread(frame, fsz, f, cls) != fsz) {
            free(frame);
            return ESP_ERR_INVALID_SIZE;
        }

        size_t i = 0;
        const uint8_t enc = frame[i++];
        const char *mime = (const char *)&frame[i];
        while (i < fsz && frame[i]) i++;
        i++;                                        /* MIME NUL */
        if (i >= fsz) { free(frame); return ESP_ERR_INVALID_SIZE; }
        i++;                                        /* picture type */

        /* Description. Encodings 1 and 2 are UTF-16 and terminate with
         * two NUL bytes on an even boundary. */
        if (enc == 1 || enc == 2) {
            while (i + 1 < fsz && !(frame[i] == 0 && frame[i + 1] == 0)) i += 2;
            i += 2;
        } else {
            while (i < fsz && frame[i]) i++;
            i++;
        }
        if (i >= fsz) { free(frame); return ESP_ERR_INVALID_SIZE; }

        const size_t img_len = fsz - i;
        if (!albumart_is_supported_image(&frame[i], img_len)) {
            ESP_LOGW(TAG, "APIC is %s, which is neither JPEG nor PNG", mime);
            free(frame);
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint8_t *img = malloc(img_len);
        if (!img) { free(frame); return ESP_ERR_NO_MEM; }
        memcpy(img, &frame[i], img_len);

        /* Logged before the free, not after. `mime` points into frame[],
         * so the old order read freed memory to print it -- which usually
         * still showed the right string, because nothing had reused the
         * block yet, and would have started printing something else the
         * moment anything did. */
        ESP_LOGI(TAG, "cover art: %s, %u bytes", mime, (unsigned)img_len);
        free(frame);

        *out = img;
        *out_len = img_len;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

/* An ID3v2 tag at the front of the file, which is where an MP3 keeps it. */
esp_err_t albumart_extract(FILE *f, storage_io_class_t cls,
                           uint8_t **out, size_t *out_len)
{
    return albumart_extract_at(f, cls, 0, out, out_len);
}

/*
 * Append one codepoint to a UTF-8 buffer, if it fits whole.
 *
 * "If it fits whole" is the point: a 3 byte character with 2 bytes of
 * room left has to be dropped rather than half-written, or the tag ends
 * in a truncated sequence and gfx.c draws a replacement box for it. The
 * old code could not have this bug because every character was one byte.
 *
 * Returns the number of bytes written, 0 if it did not fit.
 */
static size_t utf8_put(char *out, size_t o, size_t out_len, uint32_t cp)
{
    /* Anything past the BMP is outside the font's subset by a wide
     * margin -- emoji in a TIT2 frame, usually -- so it becomes U+FFFD
     * here rather than costing four bytes to render as a box anyway. */
    if (cp > 0xFFFF) cp = 0xFFFD;

    size_t n = (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : 3;
    if (o + n + 1 > out_len) return 0;

    if (n == 1) {
        out[o] = (char)cp;
    } else if (n == 2) {
        out[o]     = (char)(0xC0 | (cp >> 6));
        out[o + 1] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[o]     = (char)(0xE0 | (cp >> 12));
        out[o + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o + 2] = (char)(0x80 | (cp & 0x3F));
    }
    return n;
}

/*
 * Copy an ID3 text frame body into a UTF-8 buffer.
 *
 * The first byte is the encoding: 0 latin1, 1 UTF-16 with BOM, 2 UTF-16BE,
 * 3 UTF-8. This used to flatten all four to ASCII and replace everything
 * above 0x7F with '?', because the font was ASCII. The font is now
 * ark12, which has Latin-1 Supplement and Latin Extended-A, so the
 * flattening is gone and each encoding is converted properly instead.
 *
 * Note that encoding 0 is Latin-1, not ASCII and not UTF-8: byte 0xE9 in
 * a v2.3 frame means 'é' and has to be widened to two bytes, not copied.
 * Getting this backwards is the classic ID3 mojibake, and it is silent --
 * the tag looks fine to anything that also gets it backwards.
 *
 * Surrogate pairs in UTF-16 are decoded rather than dropped so that a
 * tag containing an emoji yields one replacement box rather than two.
 */
static void id3_text_to_utf8(const uint8_t *body, size_t len, char *out, size_t out_len)
{
    if (len < 1 || out_len < 1) { if (out_len) out[0] = 0; return; }

    const uint8_t enc = body[0];
    size_t i = 1, o = 0;

    if (enc == 1 || enc == 2) {
        bool le = (enc == 1);
        if (enc == 1 && i + 1 < len) {
            if (body[i] == 0xFF && body[i + 1] == 0xFE) { le = true;  i += 2; }
            else if (body[i] == 0xFE && body[i + 1] == 0xFF) { le = false; i += 2; }
        }
        for (; i + 1 < len; i += 2) {
            uint32_t u = le ? ((uint32_t)body[i + 1] << 8 | body[i])
                            : ((uint32_t)body[i] << 8 | body[i + 1]);
            if (u == 0) break;

            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
                const uint32_t lo = le ? ((uint32_t)body[i + 3] << 8 | body[i + 2])
                                       : ((uint32_t)body[i + 2] << 8 | body[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;  /* unpaired */
            if (u < 0x20) continue;                      /* control codes */

            const size_t n = utf8_put(out, o, out_len, u);
            if (!n) break;
            o += n;
        }
    } else if (enc == 3) {
        /* Already UTF-8. Copied through rather than decoded and
         * re-encoded, but stopped at a character boundary: a byte-wise
         * memcpy into a 64 byte field is how the last character of a
         * long title becomes a box. */
        for (; i < len; ) {
            const uint8_t b = body[i];
            if (b == 0) break;
            size_t n = (b < 0x80) ? 1 : ((b & 0xE0) == 0xC0) ? 2
                     : ((b & 0xF0) == 0xE0) ? 3 : ((b & 0xF8) == 0xF0) ? 4 : 1;
            if (i + n > len) break;
            if (o + n + 1 > out_len) break;
            memcpy(&out[o], &body[i], n);
            o += n;
            i += n;
        }
    } else {
        /* Latin-1. Every byte is its own codepoint, by definition. */
        for (; i < len; i++) {
            const uint8_t c = body[i];
            if (c == 0) break;
            if (c < 0x20) continue;
            const size_t n = utf8_put(out, o, out_len, c);
            if (!n) break;
            o += n;
        }
    }
    out[o] = 0;
}

esp_err_t id3_read_tags_at(FILE *f, long base, id3_tags_t *out)
{
    uint8_t hdr[10];

    memset(out, 0, sizeof(*out));

    fseek(f, base, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "ID3", 3) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const int ver = hdr[3];
    if (ver < 3) return ESP_ERR_NOT_SUPPORTED;

    if (hdr[5] & 0x40) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) return ESP_ERR_INVALID_SIZE;
        const uint32_t esz = (ver >= 4) ? syncsafe32(ext) : be32(ext);
        fseek(f, (long)esz - ((ver >= 4) ? 4 : 0), SEEK_CUR);
    }

    const long tag_end = base + 10 + (long)syncsafe32(&hdr[6]);
    int found = 0;

    while (ftell(f) + 10 <= tag_end && found < 3) {
        uint8_t fh[10];
        if (fread(fh, 1, sizeof(fh), f) != sizeof(fh)) break;
        if (fh[0] == 0) break;                      /* padding */

        const uint32_t fsz = (ver >= 4) ? syncsafe32(&fh[4]) : be32(&fh[4]);
        if (fsz == 0 || ftell(f) + (long)fsz > tag_end) break;

        char *dst = NULL;
        size_t dst_len = 0;
        if      (!memcmp(fh, "TIT2", 4)) { dst = out->title;  dst_len = sizeof(out->title);  }
        else if (!memcmp(fh, "TPE1", 4)) { dst = out->artist; dst_len = sizeof(out->artist); }
        else if (!memcmp(fh, "TALB", 4)) { dst = out->album;  dst_len = sizeof(out->album);  }

        if (!dst) {
            fseek(f, (long)fsz, SEEK_CUR);
            continue;
        }

        /* Frames are small; anything absurd is a corrupt size field, and
         * malloc'ing it would be the corruption's idea rather than ours. */
        if (fsz > 512) { fseek(f, (long)fsz, SEEK_CUR); continue; }

        uint8_t body[512];
        if (fread(body, 1, fsz, f) != fsz) break;
        id3_text_to_utf8(body, fsz, dst, dst_len);
        found++;
    }

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t id3_read_tags(FILE *f, id3_tags_t *out)
{
    return id3_read_tags_at(f, 0, out);
}

/* ------------------------------------------------------------------ */
/* Decode and draw                                                     */
/* ------------------------------------------------------------------ */

/*
 * Is this a baseline JPEG?
 *
 * The P4's decoder handles SOF0 and nothing else. Handed a progressive
 * file it walks the markers, never finds SOF0, reaches the scan and
 * reports:
 *
 *   E jpeg.decoder: SOS encountered before SOF0
 *   E tab5_art: albumart_draw(261): jpeg info
 *   W tab5_mp3: cover art failed to decode (ESP_ERR_NOT_FOUND)
 *
 * Which is accurate, in the way a stack trace is accurate. It reads as a
 * corrupt tag or a bug in this file, and it is neither: the file is a
 * perfectly good JPEG that this silicon cannot decode. Saying so is a
 * fifteen line marker walk, and it is worth it because the answer tells
 * you what to do about it -- re-encode that cover as baseline -- while
 * the driver's version does not.
 *
 * Markers are two bytes, 0xFF then a code, and every one except the
 * standalone few carries a big-endian length that includes itself. SOS
 * ends the header, so the SOF has to appear before it or there is not
 * one.
 */
static bool jpeg_is_baseline(const uint8_t *p, size_t len, uint8_t *sof_out,
                             uint32_t *w_out, uint32_t *h_out)
{
    *sof_out = 0;
    *w_out = 0;
    *h_out = 0;
    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;   /* no SOI */

    size_t i = 2;
    while (i + 3 < len) {
        if (p[i] != 0xFF) { i++; continue; }             /* fill byte or junk */
        const uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }                /* fill */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (m == 0xDA) return false;                     /* SOS, no SOF seen */

        /* SOFn: C0-CF except C4 (DHT), C8 (JPG), CC (DAC). C0 is
         * baseline, C1 is extended sequential and the hardware takes it
         * too; C2 is progressive, which it does not. */
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *sof_out = m;
            /* Every SOF has the same first five payload bytes: precision,
             * then height and width, both big-endian 16-bit. Read them
             * even for the flavours that cannot be decoded -- the size is
             * what says whether a software fallback could afford the
             * picture, and bailing before jpeg_decoder_get_info() means
             * nothing else will report it. */
            if (i + 9 < len) {
                *h_out = ((uint32_t)p[i + 5] << 8) | p[i + 6];
                *w_out = ((uint32_t)p[i + 7] << 8) | p[i + 8];
            }
            return (m == 0xC0 || m == 0xC1);
        }

        const size_t seg = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (seg < 2) return false;
        i += 2 + seg;
    }
    return false;
}

static const char *sof_name(uint8_t m)
{
    switch (m) {
    case 0xC2: return "progressive";
    case 0xC3: return "lossless";
    case 0xC5: case 0xC6: case 0xC7: return "differential";
    case 0xC9: case 0xCA: case 0xCB: return "arithmetic-coded";
    case 0xCD: case 0xCE: case 0xCF: return "differential arithmetic-coded";
    case 0x00: return "no SOF marker";
    default:   return "unsupported";
    }
}

esp_err_t albumart_draw(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *jpeg, size_t jpeg_len)
{
    esp_err_t ret = ESP_OK;
    jpeg_decoder_handle_t dec = NULL;
    uint8_t *in = NULL;
    uint8_t *rgb = NULL;
    size_t in_size = 0, rgb_size = 0;

    uint8_t sof = 0;
    uint32_t sof_w = 0, sof_h = 0;
    if (!jpeg_is_baseline(jpeg, jpeg_len, &sof, &sof_w, &sof_h)) {
        ESP_LOGW(TAG, "cover is a %s JPEG (SOF marker 0x%02X), %"PRIu32"x%"PRIu32"; "
                      "this decoder is baseline-only", sof_name(sof), sof,
                 sof_w, sof_h);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const jpeg_decode_engine_cfg_t engine = { .timeout_ms = 5000 };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine, &dec), TAG, "jpeg engine");

    jpeg_decode_picture_info_t info;
    ESP_GOTO_ON_ERROR(jpeg_decoder_get_info(jpeg, jpeg_len, &info), cleanup, TAG, "jpeg info");
    ESP_LOGI(TAG, "cover is %"PRIu32"x%"PRIu32, info.width, info.height);

    /*
     * The decoder works in whole MCUs, so it writes a picture rounded up
     * to the MCU grid -- and it writes the padding too. Two consequences,
     * and this code had both wrong.
     *
     * The buffer has to hold the padded picture. A 3000x3000 cover is
     * 3008x3008 at 4:2:0, which is 18,096,128 bytes rather than the
     * 18,000,000 an unpadded calculation asks for, and the driver
     * refuses the whole decode over the 96 KB difference:
     *
     *   Given buffer size 18000000 is smaller than actual jpeg decode
     *   output size 18096128
     *
     * And the padded width is the row stride. Copying out at info.width
     * shifts every row by (padded - width) pixels relative to the one
     * above it, which is a picture sheared diagonally across the screen.
     * That was latent rather than absent: it needs a cover whose width is
     * not already a multiple of the MCU, and 500 and 1000 both are not --
     * so the bug was there all along and the log had nothing to say about
     * it, because the decode succeeded.
     *
     * MCU size follows the chroma subsampling, which is why it has to be
     * read from the header rather than assumed to be 16.
     */
    int mcu_w = 16, mcu_h = 16;
    switch (info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV420: mcu_w = 16; mcu_h = 16; break;
    case JPEG_DOWN_SAMPLING_YUV422: mcu_w = 16; mcu_h = 8;  break;
    case JPEG_DOWN_SAMPLING_YUV444:
    case JPEG_DOWN_SAMPLING_GRAY:   mcu_w = 8;  mcu_h = 8;  break;
    default: break;                 /* 16x16 is the largest, so it is safe */
    }
    const uint32_t pad_w = ((info.width  + mcu_w - 1) / mcu_w) * mcu_w;
    const uint32_t pad_h = ((info.height + mcu_h - 1) / mcu_h) * mcu_h;

    /* The decoder DMAs straight out of the input buffer, so the
     * bitstream has to live in memory it can reach and be padded to the
     * alignment it asks for -- a plain malloc'd copy is not enough. */
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    in = jpeg_alloc_decoder_mem(jpeg_len, &in_cfg, &in_size);
    ESP_GOTO_ON_FALSE(in, ESP_ERR_NO_MEM, cleanup, TAG, "jpeg in buf");
    memcpy(in, jpeg, jpeg_len);

    /*
     * What the decoder is about to be handed, hashed.
     *
     * The same 145926-byte cover decodes on some tracks and fails on
     * others with "marker parsed by the decoder is not supported by the
     * hardware", reproducibly, from files that are byte-identical off
     * the card -- confirmed by extracting both with ffmpeg and comparing
     * pixel signatures. So either the bytes arriving here are not the
     * bytes on the card, or they are perfect and the engine is the
     * problem. Nothing in the log distinguishes those, and they want
     * completely different fixes.
     *
     * Hashed from `in` rather than from `jpeg`, deliberately: `in` is
     * the DMA-reachable copy the hardware actually reads, so this covers
     * the ID3 extraction, the read off the card AND the memcpy above.
     * Hashing the source would leave the last of those untested.
     *
     * MD5 from ROM: no code size, no dependency, and collision
     * resistance is irrelevant when the question is "are these the same
     * bytes twice".
     */
    {
        md5_context_t md5;
        uint8_t digest[16];
        esp_rom_md5_init(&md5);
        esp_rom_md5_update(&md5, in, (uint32_t)jpeg_len);
        esp_rom_md5_final(digest, &md5);

        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        ESP_LOGI(TAG, "jpeg in: %u bytes, md5 %s", (unsigned)jpeg_len, hex);
    }

    const jpeg_decode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    const size_t want = (size_t)pad_w * pad_h * 2;              /* RGB565 */

    /*
     * Ask before allocating, so a cover that does not fit says so in one
     * line instead of failing somewhere inside the driver.
     *
     * There is no scaler in this hardware -- the decoder produces the
     * picture at full size or not at all -- so a 3000x3000 cover costs
     * 18 MB of PSRAM for a 720 px square, on a board that is also holding
     * a 1.8 MB shadow buffer, the bitstream, and a decode ring with audio
     * running through it. It usually fits. When it does not, the useful
     * thing to print is how much was wanted.
     */
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largest < want) {
        ESP_LOGW(TAG, "cover needs %u KB of PSRAM in one block, largest free is %u KB",
                 (unsigned)(want / 1024), (unsigned)(largest / 1024));
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    rgb = jpeg_alloc_decoder_mem(want, &out_cfg, &rgb_size);
    ESP_GOTO_ON_FALSE(rgb, ESP_ERR_NO_MEM, cleanup, TAG, "jpeg out buf");

    /*
     * BGR, on a panel that is RGB565 and a codebase that is RGB565
     * everywhere else.
     *
     * `rgb_order` does not name the output colour order. It selects a
     * DMA2D *byte* scramble applied after the colour conversion, and for
     * RGB565 the two settings are:
     *
     *   ..._ORDER_RGB -> DMA2D_SCRAMBLE_ORDER_BYTE2_0_1
     *   ..._ORDER_BGR -> DMA2D_SCRAMBLE_ORDER_BYTE2_1_0   (identity)
     *
     * A 16-bit RGB565 word in little-endian memory does not survive
     * having its bytes reordered: red and blue come out exchanged. Which
     * is what a gold cover on a red background rendered as silver on
     * blue -- (200,150,50) read back as (50,150,200), with the near-grey
     * highlights unchanged because swapping R and B does nothing to a
     * pixel where they are equal.
     *
     * So the setting that looks right is the one that corrupts, and the
     * name is describing the scramble rather than the result. The rest of
     * the file is the control: gfx.c's RGB() macro, the pngle path and
     * the DPI panel's own LCD_COLOR_FMT_RGB565 all agree with each other,
     * and only the JPEG path disagreed.
     *
     * conv_std is stated rather than left at 0. It happens to be BT.601
     * either way, which is the right answer for JPEG, but a colour
     * standard arrived at by zero-initialisation is not a decision.
     */
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded = 0;
    ESP_GOTO_ON_ERROR(jpeg_decoder_process(dec, &cfg, in, jpeg_len, rgb, rgb_size, &decoded),
                      cleanup, TAG, "jpeg decode");

    /*
     * The decoder reports what it actually wrote. Cross-check it, because
     * everything below indexes at a stride derived from the header and a
     * disagreement there is a sheared picture rather than an error.
     */
    if (decoded != (uint32_t)want) {
        ESP_LOGW(TAG, "decoder wrote %"PRIu32" bytes, expected %u for %"PRIu32"x%"PRIu32,
                 decoded, (unsigned)want, pad_w, pad_h);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    /*
     * Scale to fit, at a fractional ratio, and centre the result.
     *
     * Two revisions here and the second is the interesting one.
     *
     * Cropping alone was the original: a 720 px square cut out of the
     * middle of a 3000 px cover, which is under a quarter of the picture
     * with no indication that anything was missing. Then an integer
     * decimation, taking every Nth pixel, with N the largest that still
     * covered the panel -- which fixed the 3000 px case and left the
     * common ones badly served. 1920 over 720 is 2.67, so N was 2, the
     * cover came out 960 px, and a quarter of it was still cropped away.
     * An integer step can only ever land on the panel exactly when the
     * cover is a multiple of it.
     *
     * So the step is 16.16 fixed point instead. The arithmetic is the
     * same shape and the same cost -- one shift and one multiply per
     * output pixel -- and the picture lands on the panel exactly. 1920
     * becomes 720 whole rather than 960 cropped.
     *
     * Fit rather than fill, so nothing is lost. A cover that is not
     * square gets black at two edges instead of having its other two
     * trimmed; the cover is the thing being shown, and a player that
     * quietly crops the artwork it was given is deciding something it was
     * not asked to decide.
     *
     * Nearest neighbour, no filtering. A box filter would be visibly
     * better on fine detail and would read every source pixel rather than
     * one in seven, during playback. Album art is not fine detail.
     */
    const int iw = (int)info.width, ih = (int)info.height;
    const int stride = (int)pad_w;

    /*
     * Fitted to the box in BOTH directions -- enlarged as well as
     * reduced. There used to be an `if (iw > screen_w || ih > screen_h)`
     * around this, so a cover smaller than the panel was centred at its
     * native size with black all round it. A 300 px cover on a 720 px
     * panel occupied a sixth of the area it was given and looked like a
     * thumbnail somebody forgot to load properly.
     *
     * Nothing about the arithmetic below needed to change to enlarge:
     * the 16.16 step is simply less than 1.0 when cw > iw, and the same
     * loop reads each source pixel several times instead of skipping
     * some. That is the advantage of a fixed-point step over the integer
     * one this replaced.
     *
     * Nearest neighbour, so enlarging is blocky -- a 300 px cover on a
     * 720 px panel is 2.4x and the pixels show. That is the honest
     * result: the alternative is a bilinear pass that makes a small
     * image look soft instead of blocky, which is not obviously better
     * and costs four reads and three lerps per output pixel during
     * playback.
     */
    const int64_t fit_by_width = (int64_t)iw * screen_h;
    const int64_t fit_by_height = (int64_t)ih * screen_w;

    int cw, ch;
    if (fit_by_width >= fit_by_height) {
        /* Wider than the box's aspect: width is the binding dimension. */
        cw = screen_w;
        ch = (int)(((int64_t)ih * screen_w) / iw);
    } else {
        ch = screen_h;
        cw = (int)(((int64_t)iw * screen_h) / ih);
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    /* 16.16, rounded up so the last output pixel cannot index past the
     * last source row or column. */
    const uint32_t xstep = (uint32_t)(((uint64_t)iw << 16) / (uint32_t)cw);
    const uint32_t ystep = (uint32_t)(((uint64_t)ih << 16) / (uint32_t)ch);

    const int dx = (screen_w - cw) / 2, dy = (screen_h - ch) / 2;

    if (cw != iw || ch != ih) {
        ESP_LOGI(TAG, "cover %s to %dx%d",
                 (cw > iw) ? "enlarged" : "fitted", cw, ch);
    }

    /* The shadow, not the panel's buffer. Drawing straight into the
     * scanned-out buffer is what made the cover appear as a flash of
     * black followed by a slow fill. */
    uint16_t *fb = gfx_fb();
    ESP_GOTO_ON_ERROR(fb ? ESP_OK : ESP_ERR_INVALID_STATE,
                      cleanup, TAG, "no shadow buffer");

    /* Black out, then place the crop, then copy the band up once.
     *
     * screen_h is the height of the artwork area, not of the panel: the
     * caller passes the space above the transport bar. Clearing the full
     * panel height here would blank the bar in the shadow and blit that
     * over it, so the bar vanished on every track change until the next
     * ui_draw() put it back. */
    memset(fb, 0, (size_t)screen_w * screen_h * 2);
    const uint16_t *src = (const uint16_t *)rgb;
    for (int y = 0; y < ch; y++) {
        uint32_t syf = (uint32_t)y * ystep;
        int srow = (int)(syf >> 16);
        if (srow >= ih) srow = ih - 1;

        const uint16_t *row = &src[(size_t)srow * stride];
        uint16_t *dst = &fb[(dy + y) * screen_w + dx];

        if (xstep == (1u << 16)) {
            memcpy(dst, row, (size_t)cw * 2);
        } else {
            uint32_t sxf = 0;
            for (int x = 0; x < cw; x++, sxf += xstep) {
                int scol = (int)(sxf >> 16);
                if (scol >= iw) scol = iw - 1;
                dst[x] = row[scol];
            }
        }
    }
    ESP_GOTO_ON_ERROR(gfx_blit_err(0, screen_h),
                      cleanup, TAG, "draw");

cleanup:
    if (rgb) free(rgb);
    if (in) free(in);
    if (dec) jpeg_del_decoder_engine(dec);
    return ret;
}

/* ------------------------------------------------------------------ */
/* PNG                                                                 */
/* ------------------------------------------------------------------ */

/*
 * pngle streams: it calls back per pixel run rather than handing over a
 * bitmap, so there is never a full RGBA copy of the image in memory.
 * That matters here -- a 1400x1400 RGBA buffer is 7.8 MB, and we would
 * only be throwing most of it away to crop anyway.
 *
 * Pixels are written straight into the panel's scan buffer with the
 * same centre-and-crop arithmetic the JPEG path uses.
 */
typedef struct {
    bool saw_init;
    uint16_t *fb;
    int screen_w, screen_h;
    int dx, dy;             /* top-left of the scaled image in screen space */
    int iw, ih;             /* source size */
    int cw, ch;             /* size on screen after fitting */
} png_ctx_t;

static void png_on_init(pngle_t *pngle, uint32_t w, uint32_t h)
{
    png_ctx_t *c = pngle_get_user_data(pngle);
    c->saw_init = true;
    c->iw = (int)w;
    c->ih = (int)h;

    /*
     * Same fit-to-box as the JPEG path, up as well as down.
     *
     * Scaled forwards rather than backwards, and that is forced by the
     * streaming: pngle hands over source pixels as it finds them and
     * never offers a bitmap to sample, so the destination rectangle has
     * to be computed FROM each source pixel instead of the destination
     * loop pulling from a source. Which is why the mapping below is
     * expressed as edges -- pixel i covers [i*cw/iw, (i+1)*cw/iw) -- and
     * not as a step. Rounding each edge the same way is what stops
     * enlargement leaving unwritten seams between blocks.
     */
    if ((int64_t)w * c->screen_h >= (int64_t)h * c->screen_w) {
        c->cw = c->screen_w;
        c->ch = (int)(((int64_t)h * c->screen_w) / w);
    } else {
        c->ch = c->screen_h;
        c->cw = (int)(((int64_t)w * c->screen_h) / h);
    }
    if (c->cw < 1) c->cw = 1;
    if (c->ch < 1) c->ch = 1;

    c->dx = (c->screen_w - c->cw) / 2;
    c->dy = (c->screen_h - c->ch) / 2;

    ESP_LOGI(TAG, "cover is %"PRIu32"x%"PRIu32" (png)", w, h);
    if (c->cw != (int)w || c->ch != (int)h) {
        ESP_LOGI(TAG, "cover %s to %dx%d",
                 (c->cw > (int)w) ? "enlarged" : "fitted", c->cw, c->ch);
    }
}

static void png_on_draw(pngle_t *pngle, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, const uint8_t rgba[4])
{
    png_ctx_t *c = pngle_get_user_data(pngle);

    if (rgba[3] == 0) return;                   /* fully transparent */
    const uint16_t px = (uint16_t)(((rgba[0] & 0xF8) << 8) |
                                   ((rgba[1] & 0xFC) << 3) |
                                    (rgba[2] >> 3));

    /*
     * The source run [x, x+w) x [y, y+h) maps to the half-open
     * destination rectangle between the scaled edges of its first and
     * last pixels. Computing both edges with the same expression is what
     * makes adjacent runs abut exactly: run A's right edge and run B's
     * left edge are the same arithmetic on the same number.
     *
     * At 1:1 this reduces to the old behaviour. Below 1:1 a run can map
     * to zero pixels and is dropped, which is the correct way to shrink
     * -- the run that lands on that pixel wins.
     */
    const int px0 = c->dx + (int)(((int64_t)x * c->cw) / c->iw);
    const int px1 = c->dx + (int)(((int64_t)(x + w) * c->cw) / c->iw);
    const int py0 = c->dy + (int)(((int64_t)y * c->ch) / c->ih);
    const int py1 = c->dy + (int)(((int64_t)(y + h) * c->ch) / c->ih);

    for (int sy = py0; sy < py1; sy++) {
        if (sy < 0 || sy >= c->screen_h) continue;
        uint16_t *row = &c->fb[(size_t)sy * c->screen_w];
        for (int sx = px0; sx < px1; sx++) {
            if (sx < 0 || sx >= c->screen_w) continue;
            row[sx] = px;
        }
    }
}

static esp_err_t albumart_draw_png(esp_lcd_panel_handle_t panel,
                                   int screen_w, int screen_h,
                                   const uint8_t *png, size_t png_len)
{
    uint16_t *fb = gfx_fb();
    ESP_RETURN_ON_FALSE(fb, ESP_ERR_INVALID_STATE, TAG, "no shadow buffer");
    memset(fb, 0, (size_t)screen_w * screen_h * 2);

    pngle_t *p = pngle_new();
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "pngle_new");

    png_ctx_t ctx = { .fb = fb, .screen_w = screen_w, .screen_h = screen_h };
    pngle_set_user_data(p, &ctx);
    pngle_set_init_callback(p, png_on_init);
    pngle_set_draw_callback(p, png_on_draw);

    esp_err_t ret = ESP_OK;
    const int fed = pngle_feed(p, png, png_len);
    if (fed < 0) {
        ESP_LOGE(TAG, "png decode failed: %s", pngle_error(p));
        ret = ESP_FAIL;
    }
    /* pngle_feed() can consume the whole buffer, return a non-negative
     * count and never call a single callback -- a truncated or unsupported
     * stream is not an error to it. That reported success while drawing
     * nothing: no dimensions logged, no warning, and a black panel where
     * the cover should be, which is exactly what the first boot showed.
     *
     * The init callback firing is the only proof the decoder actually
     * looked at an image. */
    if (ret == ESP_OK && !ctx.saw_init) {
        ESP_LOGW(TAG, "png produced no image (%d of %u bytes consumed)",
                 fed, (unsigned)png_len);
        ret = ESP_ERR_INVALID_SIZE;
    }
    pngle_destroy(p);

    if (ret == ESP_OK) {
        /* One copy, once, when the whole image is decoded -- rather than
         * the panel showing the picture arrive scanline by scanline. */
        ret = gfx_blit_err(0, screen_h);
    }
    return ret;
}

/* ------------------------------------------------------------------ */

esp_err_t albumart_show(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *img, size_t img_len)
{
    if (img_len > 8 && memcmp(img, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return albumart_draw_png(panel, screen_w, screen_h, img, img_len);
    }
    return albumart_draw(panel, screen_w, screen_h, img, img_len);
}
