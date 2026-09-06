/*
 * cachetest.c -- mediacache.c's shared cover buffers, on the host.
 *
 * The real file, compiled against stub headers. Under ASan+UBSan the
 * interesting failures are the ones the board would show as a wrong
 * picture or a crash three tracks later: a double free when two entries
 * share a blob and one is evicted, a use-after-free when the survivor is
 * then read, and a leak when the last reference goes.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mediacache.h"

char g_fake_mutex;

/* The real Murmur2, copied from albumart.c so the test links. */
uint32_t albumart_cover_hash(const void *key, size_t len)
{
    const uint32_t m = 0x5bd1e995u;
    const int r = 24;
    uint32_t h = 0x9747b28cu ^ (uint32_t)len;
    const unsigned char *d = (const unsigned char *)key;
    while (len >= 4) {
        uint32_t k;
        memcpy(&k, d, 4);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        d += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= (uint32_t)d[2] << 16; /* fall through */
        case 2: h ^= (uint32_t)d[1] << 8;  /* fall through */
        case 1: h ^= (uint32_t)d[0]; h *= m; break;
        default: break;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

#define COVER  (64u * 1024)

static uint8_t *mkcover(int seed)
{
    uint8_t *p = malloc(COVER);
    assert(p);
    for (size_t i = 0; i < COVER; i++) p[i] = (uint8_t)((i * 31 + seed) & 0xff);
    return p;
}

static int fails;
static void ck(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    int n; size_t bytes, saved;

    /* --- 1. an album: three paths, one picture --- */
    mediacache_init();
    mediacache_put_art("/a/01.mp3", mkcover(1), COVER);
    mediacache_put_art("/a/02.mp3", mkcover(1), COVER);
    mediacache_put_art("/a/03.mp3", mkcover(1), COVER);

    mediacache_stats(&n, &bytes, &saved);
    ck("album of three shares one buffer", n == 3 && bytes >= COVER && bytes < 2 * COVER);
    ck("  and reports two copies saved", saved == 2 * COVER);

    /* Every path still answers with the right bytes. */
    uint8_t *ref = mkcover(1);
    int same = 1;
    for (int i = 1; i <= 3; i++) {
        char p[32]; snprintf(p, sizeof p, "/a/%02d.mp3", i);
        size_t l = 0;
        const uint8_t *g = mediacache_art(p, &l);
        if (!g || l != COVER || memcmp(g, ref, COVER) != 0) same = 0;
    }
    ck("every path returns the full original bytes", same);

    uint32_t h1 = mediacache_art_hash("/a/01.mp3");
    ck("hash is exposed and agrees across sharers",
       h1 != 0 && h1 == mediacache_art_hash("/a/03.mp3") &&
       h1 == albumart_cover_hash(ref, COVER));

    /* --- 2. eviction of one sharer must not kill the others --- */
    mediacache_pin("/a/02.mp3");
    mediacache_put_art("/b/01.mp3", mkcover(2), COVER);   /* evicts an /a/ */

    size_t l2 = 0;
    const uint8_t *survivor = mediacache_art("/a/02.mp3", &l2);
    ck("pinned sharer survives eviction of its co-owner",
       survivor && l2 == COVER && memcmp(survivor, ref, COVER) == 0);

    size_t lb = 0;
    const uint8_t *other = mediacache_art("/b/01.mp3", &lb);
    ck("a different picture is not shared with it",
       other && lb == COVER && memcmp(other, ref, COVER) != 0);

    /* --- 3. distinct covers are not merged --- */
    mediacache_init();
    mediacache_put_art("/c/01.mp3", mkcover(1), COVER);
    mediacache_put_art("/c/02.mp3", mkcover(2), COVER);
    mediacache_stats(&n, &bytes, &saved);
    ck("two different covers share nothing", saved == 0 && bytes >= 2 * COVER);

    /* --- 4. re-storing the same cover for the same path --- */
    mediacache_init();
    mediacache_put_art("/d/01.mp3", mkcover(3), COVER);
    mediacache_put_art("/d/01.mp3", mkcover(3), COVER);   /* prefetch races play */
    mediacache_stats(&n, &bytes, &saved);
    ck("re-store of the same path is idempotent",
       n == 1 && saved == 0 && bytes >= COVER && bytes < 2 * COVER);

    /* --- 5. replacing a shared cover under one path --- */
    mediacache_init();
    mediacache_put_art("/e/01.mp3", mkcover(4), COVER);
    mediacache_put_art("/e/02.mp3", mkcover(4), COVER);
    mediacache_put_art("/e/01.mp3", mkcover(5), COVER);   /* 01 retagged */
    size_t le = 0;
    const uint8_t *e2 = mediacache_art("/e/02.mp3", &le);
    uint8_t *r4 = mkcover(4);
    ck("replacing one sharer leaves the other intact",
       e2 && le == COVER && memcmp(e2, r4, COVER) == 0);
    mediacache_stats(&n, &bytes, &saved);
    ck("  and sharing is dropped", saved == 0);
    free(r4);

    /* --- 6. every slot pinned: ownership still honoured (leak check) --- */
    mediacache_init();
    mediacache_put_art("/f/01.mp3", mkcover(6), COVER);
    mediacache_put_art("/f/02.mp3", mkcover(7), COVER);
    mediacache_put_art("/f/03.mp3", mkcover(8), COVER);
    mediacache_pin("/f/01.mp3");
    mediacache_pin("/f/02.mp3");
    mediacache_pin("/f/03.mp3");
    mediacache_put_art("/f/04.mp3", mkcover(9), COVER);   /* nowhere to go */
    mediacache_stats(&n, &bytes, &saved);
    ck("all-pinned store is refused without leaking", n == 3);

    /* --- 7. zero length --- */
    mediacache_put_art("/g/01.mp3", mkcover(1), 0);
    ck("zero-length store is refused", mediacache_art("/g/01.mp3", NULL) == NULL);

    /* --- 8. teardown frees everything exactly once --- */
    mediacache_init();
    mediacache_put_art("/h/01.mp3", mkcover(1), COVER);
    mediacache_put_art("/h/02.mp3", mkcover(1), COVER);
    mediacache_put_art("/h/03.mp3", mkcover(1), COVER);
    mediacache_clear();
    mediacache_stats(&n, &bytes, &saved);
    ck("clear() releases a shared blob once", n == 0 && bytes == 0 && saved == 0);

    mediacache_init();   /* init over a populated cache */
    mediacache_put_art("/i/01.mp3", mkcover(1), COVER);
    mediacache_put_art("/i/02.mp3", mkcover(1), COVER);
    mediacache_init();
    mediacache_stats(&n, &bytes, &saved);
    ck("init() over shared entries releases once", n == 0);

    free(ref);
    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
