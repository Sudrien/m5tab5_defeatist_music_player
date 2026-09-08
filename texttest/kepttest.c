/*
 * kepttest.c -- the one retained decoded cover.
 *
 * The risk is not that the cache misses; it is that it HITS wrongly, or
 * that it holds memory the next decode needs. Both are checked, along
 * with the ownership rule, since a retained frame that is also freed is
 * a use-after-free the board would show as a corrupted cover much later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define COVER_KEEP_MAX_BYTES (2u * 1024 * 1024)

static uint8_t *kept;
static size_t   kept_size;
static int      kept_w, kept_h, kept_stride;
static uint32_t kept_hash;

static void cover_drop(void)
{
    if (!kept) return;
    free(kept); kept = NULL; kept_size = 0; kept_hash = 0;
}

static bool cover_retain(uint8_t *rgb, size_t size, int w, int h,
                         int stride, uint32_t hash)
{
    if (!rgb || !size || size > COVER_KEEP_MAX_BYTES) return false;
    cover_drop();
    kept = rgb; kept_size = size; kept_w = w; kept_h = h;
    kept_stride = stride; kept_hash = hash;
    return true;
}

/* Model of albumart_draw's entry: returns 1 on a cache hit (no decode),
 * 0 on a decode. `dropped_before_decode` records the ordering. */
static bool dropped_before_decode;
static int draw(uint32_t hash, size_t decode_size, int w, int h, bool blit_ok)
{
    if (kept && kept_hash == hash) return 1;

    dropped_before_decode = (kept != NULL);
    cover_drop();                       /* miss: release first */

    uint8_t *rgb = malloc(decode_size);
    if (!rgb) return -1;
    memset(rgb, 0xAB, decode_size);
    if (blit_ok && cover_retain(rgb, decode_size, w, h, w, hash)) {
        rgb = NULL;                     /* ownership moved */
    }
    free(rgb);                          /* frees only what was refused */
    return 0;
}

static int fails;
static void ck(const char *what, bool ok)
{ printf("%-60s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    /* First draw decodes and keeps. */
    ck("first draw decodes", draw(0x04d36f37, 1098u*1024, 750, 750, true) == 0);
    ck("  and the frame is kept", kept != NULL && kept_hash == 0x04d36f37);

    /* The case the patch exists for: same picture, no decode. */
    ck("same hash draws from the kept frame",
       draw(0x04d36f37, 1098u*1024, 750, 750, true) == 1);
    ck("  three more times", draw(0x04d36f37, 0, 0, 0, true) == 1 &&
                             draw(0x04d36f37, 0, 0, 0, true) == 1 &&
                             draw(0x04d36f37, 0, 0, 0, true) == 1);

    /* A different picture must not hit. */
    ck("a different hash decodes", draw(0x8a181f76, 991u*1024, 700, 700, true) == 0);
    ck("  and replaces the kept frame", kept_hash == 0x8a181f76);
    ck("  releasing the old one BEFORE decoding", dropped_before_decode);

    /* Oversized frames are drawn and dropped -- the cap protects the
     * contiguous block the next large decode needs. */
    ck("an oversized frame decodes", draw(0xdeadbeef, 17u*1024*1024, 3000, 3000, true) == 0);
    ck("  and is not kept", kept == NULL);
    ck("  so the next draw of it decodes again",
       draw(0xdeadbeef, 17u*1024*1024, 3000, 3000, true) == 0 && kept == NULL);

    /* A frame that failed to blit must not be answered with later. */
    draw(0x11111111, 1024, 32, 32, true);
    ck("a blit failure is not kept",
       draw(0x22222222, 1024, 32, 32, false) == 0 && kept == NULL);

    /* Explicit release. */
    draw(0x33333333, 1024, 32, 32, true);
    ck("a frame is held before the release", kept != NULL);
    cover_drop();
    ck("forget releases it", kept == NULL);
    cover_drop();
    ck("  and is safe to repeat", kept == NULL);

    /* Hash 0 must not collide with "nothing kept". */
    ck("no kept frame never hits", draw(0, 1024, 8, 8, true) == 0);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
