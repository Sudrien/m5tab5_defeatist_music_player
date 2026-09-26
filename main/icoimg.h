/*
 * icoimg -- a favicon.ico, as a PNG the cover path can already draw (5088).
 *
 * A .ico is a directory of images of the same icon at several sizes.
 * Each entry is either a whole PNG (usual for 256 px, and common for
 * 48-64 px in newer files) or a BMP body without its file header -- a
 * DIB, 32-bit BGRA with alpha, or 24/8/4/1-bit with a 1-bit
 * transparency mask after the pixels, its stored height doubled to count
 * that mask.
 *
 * icoimg_to_png() picks the largest entry. A PNG entry is copied out as
 * it is. A DIB entry is converted to RGBA and written as a PNG whose
 * deflate stream is stored blocks, not compressed: the miniz that pngle
 * brings has its compressor compiled out (MINIZ_NO_COMPRESSION), and a
 * 256x256 icon stored is 257 KB of PSRAM for the moment it takes to draw.
 * pngle inflates stored blocks like any others, so albumart_show() gets
 * an ordinary PNG and nothing downstream knows an icon was involved.
 *
 * Untrusted input from the network: every offset and size in the
 * directory and the DIB header is checked against the bytes in hand,
 * and images over ICOIMG_MAX_DIM on a side are refused. Pure C, no ESP-IDF
 * outside the allocator, so it builds on a host for testing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ICOIMG_MAX_DIM  (256)

typedef struct {
    int  w, h;          /* the entry chosen */
    int  bpp;           /* its bit depth; 0 for an embedded PNG */
    bool was_png;       /* the entry was a PNG, copied out unchanged */
} icoimg_info_t;

/* A Windows icon directory (type 1), with at least one entry. */
bool icoimg_is_ico(const uint8_t *p, size_t len);

/*
 * The largest readable entry of `ico` as a PNG. On success *out is a
 * fresh allocation (PSRAM on the target) that the caller frees, and
 * *info describes the entry. On failure nothing is allocated.
 */
bool icoimg_to_png(const uint8_t *ico, size_t len,
                   uint8_t **out, size_t *out_len, icoimg_info_t *info);

#ifdef __cplusplus
}
#endif
