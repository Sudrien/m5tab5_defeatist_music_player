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
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "pngle.h"

#include "albumart.h"
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
esp_err_t albumart_extract(FILE *f, uint8_t **out, size_t *out_len)
{
    uint8_t hdr[10];

    *out = NULL;
    *out_len = 0;

    fseek(f, 0, SEEK_SET);
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

    const long tag_end = 10 + (long)syncsafe32(&hdr[6]);

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
        if (fread(frame, 1, fsz, f) != fsz) { free(frame); return ESP_ERR_INVALID_SIZE; }

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
        const bool is_jpeg = img_len > 3 && frame[i] == 0xFF && frame[i + 1] == 0xD8;
        const bool is_png  = img_len > 8 && memcmp(&frame[i], "\x89PNG\r\n\x1a\n", 8) == 0;
        if (!is_jpeg && !is_png) {
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

/*
 * Copy an ID3 text frame body into a plain ASCII buffer.
 *
 * The first byte is the encoding: 0 latin1, 1 UTF-16 with BOM, 2 UTF-16BE,
 * 3 UTF-8. The font is ASCII-only, so everything is flattened to it and
 * anything above 0x7F becomes '?' -- which is at least visibly wrong,
 * rather than a mojibake glyph that looks deliberate.
 */
static void id3_text_to_ascii(const uint8_t *body, size_t len, char *out, size_t out_len)
{
    if (len < 1 || out_len < 1) { if (out_len) out[0] = 0; return; }

    const uint8_t enc = body[0];
    size_t i = 1, o = 0;

    if (enc == 1 || enc == 2) {
        /* UTF-16. Skip a BOM if present and read the low byte of each
         * unit; anything with a nonzero high byte is outside ASCII. */
        bool le = (enc == 1);
        if (enc == 1 && i + 1 < len) {
            if (body[i] == 0xFF && body[i + 1] == 0xFE) { le = true;  i += 2; }
            else if (body[i] == 0xFE && body[i + 1] == 0xFF) { le = false; i += 2; }
        }
        for (; i + 1 < len && o + 1 < out_len; i += 2) {
            const uint8_t lo = le ? body[i] : body[i + 1];
            const uint8_t hi = le ? body[i + 1] : body[i];
            if (lo == 0 && hi == 0) break;
            out[o++] = (hi == 0 && lo >= 0x20 && lo < 0x7F) ? (char)lo : '?';
        }
    } else {
        for (; i < len && o + 1 < out_len; i++) {
            const uint8_t c = body[i];
            if (c == 0) break;
            out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[o] = 0;
}

esp_err_t id3_read_tags(FILE *f, id3_tags_t *out)
{
    uint8_t hdr[10];

    memset(out, 0, sizeof(*out));

    fseek(f, 0, SEEK_SET);
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

    const long tag_end = 10 + (long)syncsafe32(&hdr[6]);
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
        id3_text_to_ascii(body, fsz, dst, dst_len);
        found++;
    }

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Decode and draw                                                     */
/* ------------------------------------------------------------------ */

esp_err_t albumart_draw(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *jpeg, size_t jpeg_len)
{
    esp_err_t ret = ESP_OK;
    jpeg_decoder_handle_t dec = NULL;
    uint8_t *in = NULL;
    uint8_t *rgb = NULL;
    size_t in_size = 0, rgb_size = 0;

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

    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
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
     * Downscale by an integer factor, then centre what is left.
     *
     * Cropping alone is what this used to do, and on a 3000x3000 cover it
     * showed a 720 px square cut out of the middle -- under a quarter of
     * the picture, with the edges of the artwork missing and no
     * indication that anything had been left out. Taking every Nth pixel
     * costs one multiply per output pixel, needs no second buffer, and
     * shows the whole cover.
     *
     * Nearest neighbour, no filtering. At the factors that come up -- 4
     * for a 3000 px cover, 2 for a 1500 -- a box filter would be visibly
     * better on fine detail and would cost a read of every source pixel
     * rather than one in sixteen, during playback. Album art is not fine
     * detail.
     *
     * The step is the largest that still covers the panel, so the result
     * is never smaller than the area: 3000/4 = 750 for a 720 px square,
     * and the remaining 30 px is the crop it always did.
     */
    const int iw = (int)info.width, ih = (int)info.height;
    const int stride = (int)pad_w;

    int step = iw / screen_w;
    const int step_v = ih / screen_h;
    if (step_v < step) step = step_v;
    if (step < 1) step = 1;

    const int sw = iw / step, sh = ih / step;   /* scaled source size */
    const int cw = (sw < screen_w) ? sw : screen_w;
    const int ch = (sh < screen_h) ? sh : screen_h;
    const int sx = (sw - cw) / 2, sy = (sh - ch) / 2;
    const int dx = (screen_w - cw) / 2, dy = (screen_h - ch) / 2;

    if (step > 1) {
        ESP_LOGI(TAG, "cover scaled 1/%d to %dx%d", step, sw, sh);
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
        const uint16_t *row = &src[(size_t)(sy + y) * step * stride];
        uint16_t *dst = &fb[(dy + y) * screen_w + dx];
        if (step == 1) {
            memcpy(dst, &row[sx], (size_t)cw * 2);
        } else {
            for (int x = 0; x < cw; x++) dst[x] = row[(sx + x) * step];
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
    int dx, dy;             /* top-left of the image in screen space */
} png_ctx_t;

static void png_on_init(pngle_t *pngle, uint32_t w, uint32_t h)
{
    png_ctx_t *c = pngle_get_user_data(pngle);
    c->saw_init = true;
    ESP_LOGI(TAG, "cover is %"PRIu32"x%"PRIu32" (png)", w, h);
    c->dx = (c->screen_w - (int)w) / 2;
    c->dy = (c->screen_h - (int)h) / 2;
}

static void png_on_draw(pngle_t *pngle, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, const uint8_t rgba[4])
{
    png_ctx_t *c = pngle_get_user_data(pngle);

    if (rgba[3] == 0) return;                   /* fully transparent */
    const uint16_t px = (uint16_t)(((rgba[0] & 0xF8) << 8) |
                                   ((rgba[1] & 0xFC) << 3) |
                                    (rgba[2] >> 3));

    for (uint32_t yy = 0; yy < h; yy++) {
        const int sy = c->dy + (int)(y + yy);
        if (sy < 0 || sy >= c->screen_h) continue;
        for (uint32_t xx = 0; xx < w; xx++) {
            const int sx = c->dx + (int)(x + xx);
            if (sx < 0 || sx >= c->screen_w) continue;
            c->fb[sy * c->screen_w + sx] = px;
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
