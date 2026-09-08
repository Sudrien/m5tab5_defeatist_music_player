/*
 * pooltest.c -- does TJpgDec's scratch pool fit?
 *
 * The claim behind 1104 is arithmetic, so it is checked as arithmetic:
 * alloc_pool()'s rounding and jd_prepare()'s allocation sequence,
 * transcribed from esp_jpeg's tjpgd.c, run against both work buffer
 * sizes and both JD_FASTDECODE settings.
 *
 * What this does NOT check is that the board agrees. It checks that the
 * explanation is arithmetically sound and that 8 KB has real margin.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

/* alloc_pool(): rounds every block up to a word, first-fit from the front. */
typedef struct { size_t left; bool failed; } pool_t;
static void take(pool_t *p, size_t n)
{
    n = (n + 3) & ~(size_t)3;
    if (p->left >= n) p->left -= n; else p->failed = true;
}

/*
 * jd_prepare()'s sequence for a baseline JPEG.
 *   nqt   quantiser tables in the DQT segment
 *   ac_np codes in each AC Huffman table (162 is the standard set)
 *   n     Y blocks per MCU: 1 for 4:4:4, 2 for 4:2:2, 4 for 4:2:0
 *   yuv   sizeof(jd_yuv_t): 2 when JD_FASTDECODE >= 1, else 1
 */
static size_t used(size_t pool, int nqt, int ac_np, int n, int yuv, bool *ok)
{
    pool_t p = { pool, false };
    take(&p, 512);                          /* inbuf, JD_SZBUF */
    for (int i = 0; i < nqt; i++) take(&p, 64 * 4);        /* int32_t */
    for (int i = 0; i < 2; i++) {           /* DC tables, np ~12 */
        take(&p, 16); take(&p, 12 * 2); take(&p, 12);
    }
    for (int i = 0; i < 2; i++) {           /* AC tables */
        take(&p, 16); take(&p, (size_t)ac_np * 2); take(&p, ac_np);
    }
    take(&p, (size_t)n * 64 * 2 + 64);      /* workbuf */
    take(&p, (size_t)(n + 2) * 64 * yuv);   /* mcubuf */
    *ok = !p.failed;
    return pool - p.left;
}

static int fails;
static void ck(const char *what, bool got, bool want)
{
    printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}

int main(void)
{
    bool ok; size_t n;

    /* The board's build: CONFIG_JD_FASTDECODE=1, a 4:2:0 cover.
     *
     * The requirement is measured against a pool that cannot fail --
     * asking a pool that DID fail how much it wanted gives the total of
     * the allocations made before the first refusal, which is a smaller
     * number and not the answer. */
    bool spare;
    n = used(1u << 20, 2, 162, 4, 2, &spare);
    printf("4:2:0, FASTDECODE=1: needs %zu, pool is 3100\n", n);
    used(3100, 2, 162, 4, 2, &ok);
    ck("  the board's configuration does NOT fit 3100", ok, false);

    /* Same image with FASTDECODE=0 -- what 3100 was tuned for. */
    n = used(1u << 20, 2, 162, 4, 1, &spare);
    printf("4:2:0, FASTDECODE=0: needs %zu, pool is 3100\n", n);
    used(3100, 2, 162, 4, 1, &ok);
    ck("  FASTDECODE=0 fits, with almost nothing spare", ok, true);

    /* Which is why this was never seen on a 4:4:4 cover. */
    used(3100, 2, 162, 1, 2, &ok);
    ck("4:4:4 at FASTDECODE=1 fits 3100 (why it looked size-related)",
       ok, true);

    /* Independent of image dimensions -- a small cover fails identically. */
    used(3100, 2, 162, 4, 2, &ok);
    ck("failure does not depend on the image being large", ok, false);

    /* The fix, and its margin. */
    n = used(1u << 20, 2, 162, 4, 2, &spare);
    printf("4:2:0, FASTDECODE=1: needs %zu, new pool is 8192\n", n);
    used(8192, 2, 162, 4, 2, &ok);
    ck("8 KB fits the board's configuration", ok, true);

    n = used(1u << 20, 4, 256, 4, 2, &spare);
    printf("worst case (4 qt, 256-code AC): needs %zu\n", n);
    used(8192, 4, 256, 4, 2, &ok);
    ck("8 KB still fits four quantiser tables and oversized Huffman",
       ok, true);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
