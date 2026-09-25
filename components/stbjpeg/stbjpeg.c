/*
 * stbjpeg.c -- see stbjpeg.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "stbjpeg.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* PSRAM, all of it. A progressive decode is megabytes, and the internal
 * RAM this would otherwise reach for is what the Wi-Fi transport runs
 * out of first (see sdkconfig.defaults, 5054). */
static void *stbjpeg_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void *stbjpeg_realloc(void *p, size_t n)
{
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
#define STBI_MALLOC(n)      stbjpeg_malloc(n)
#define STBI_REALLOC(p, n)  stbjpeg_realloc((p), (n))
#define STBI_FREE(p)        heap_caps_free(p)
#define OUT_MALLOC(n)       stbjpeg_malloc(n)
#else
#define OUT_MALLOC(n)       malloc(n)
#endif

/* JPEG and nothing else: every other format here has its own decoder,
 * and each one compiled in is attack surface fed from the internet. */
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

stbjpeg_err_t stbjpeg_info(const uint8_t *in, size_t len, int *w, int *h)
{
    int comp = 0;
    *w = *h = 0;
    if (!in || len < 4 || len > (size_t)INT32_MAX) return STBJPEG_NOT_JPEG;
    if (!stbi_info_from_memory(in, (int)len, w, h, &comp)) return STBJPEG_NOT_JPEG;
    if (*w < 1 || *h < 1) return STBJPEG_NOT_JPEG;
    if ((uint64_t)*w * (uint64_t)*h > STBJPEG_MAX_PIXELS) return STBJPEG_TOO_BIG;
    return STBJPEG_OK;
}

size_t stbjpeg_peak_bytes(int w, int h)
{
    /* Coefficients for three full-size components at 2 bytes (the
     * progressive worst case, 4:4:4), the RGB888 result at 3, and room
     * for the RGB565 copy and stb's line buffers. */
    return (size_t)w * (size_t)h * 10u + 256u * 1024u;
}

stbjpeg_err_t stbjpeg_decode_rgb565(const uint8_t *in, size_t len,
                                    int box_w, int box_h,
                                    uint16_t **out, size_t *out_size,
                                    int *w_out, int *h_out,
                                    const char **last_error)
{
    *out = NULL;
    *out_size = 0;
    *w_out = *h_out = 0;
    if (last_error) *last_error = NULL;

    int w, h;
    const stbjpeg_err_t info = stbjpeg_info(in, len, &w, &h);
    if (info != STBJPEG_OK) {
        if (last_error) *last_error = stbi_failure_reason();
        return info;
    }

    int got_w = 0, got_h = 0, comp = 0;
    uint8_t *rgb = stbi_load_from_memory(in, (int)len, &got_w, &got_h, &comp, 3);
    if (!rgb) {
        const char *why = stbi_failure_reason();
        if (last_error) *last_error = why;
        return (why && strcmp(why, "outofmem") == 0) ? STBJPEG_NO_MEM
                                                      : STBJPEG_FAILED;
    }

    /* Whole-step thinning: the largest step that still covers the box.
     * Nearest neighbour, like blit_cover()'s own scaling after it. */
    int step = 1;
    if (box_w > 0 && box_h > 0) {
        while (got_w / (step + 1) >= box_w && got_h / (step + 1) >= box_h) step++;
    }
    const int ow = got_w / step, oh = got_h / step;

    const size_t need = (size_t)ow * (size_t)oh * 2u;
    uint16_t *px = OUT_MALLOC(need);
    if (!px) {
        stbi_image_free(rgb);
        return STBJPEG_NO_MEM;
    }
    for (int y = 0; y < oh; y++) {
        const uint8_t *src = rgb + ((size_t)y * step * (size_t)got_w) * 3u;
        uint16_t *dst = px + (size_t)y * ow;
        for (int x = 0; x < ow; x++, src += 3u * (size_t)step) {
            /* gfx.h RGB(): native RGB565, as TJpgDec's output is here. */
            dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) |
                                ((src[1] & 0xFC) << 3) |
                                (src[2] >> 3));
        }
    }
    stbi_image_free(rgb);

    *out = px;
    *out_size = need;
    *w_out = ow;
    *h_out = oh;
    return STBJPEG_OK;
}

const char *stbjpeg_err_name(stbjpeg_err_t e)
{
    switch (e) {
    case STBJPEG_OK:       return "ok";
    case STBJPEG_NOT_JPEG: return "not a JPEG stb_image can read";
    case STBJPEG_TOO_BIG:  return "too many pixels";
    case STBJPEG_NO_MEM:   return "out of memory";
    case STBJPEG_FAILED:   return "decode failed";
    }
    return "?";
}
