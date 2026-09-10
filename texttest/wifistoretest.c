/*
 * wifistoretest -- the saved-network list, on the host, under ASan.
 *
 * The same bargain mediacache and covertag already made: the logic that
 * decides which record is replaced, which is evicted and which network a
 * scan should join is arithmetic on an array, and arithmetic on an array
 * can be exercised ten thousand times in a second here and not at all on
 * the board. What this cannot see is NVS -- the blob round-trip is
 * against a memory stand-in, so a wear-out, a full partition or a failed
 * commit is not covered and has to be found on hardware.
 *
 * What it does cover is every path that writes a secret into a fixed
 * buffer, which is the part that would corrupt the heap rather than
 * merely misbehave.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifistore.h"

/* ---- the fake NVS -------------------------------------------------- */

static unsigned char *g_blob;
static size_t         g_blob_len;
static int            g_blob_count;
static int            g_writes;

esp_err_t host_blob_write(const void *data, size_t len, int count)
{
    free(g_blob);
    g_blob = malloc(len);
    if (!g_blob) return ESP_FAIL;
    memcpy(g_blob, data, len);
    g_blob_len = len;
    g_blob_count = count;
    g_writes++;
    return ESP_OK;
}

int host_blob_read(void *data, size_t len)
{
    if (!g_blob || g_blob_len != len) return 0;
    memcpy(data, g_blob, len);
    return g_blob_count;
}

static void blob_reset(void)
{
    free(g_blob);
    g_blob = NULL;
    g_blob_len = 0;
    g_blob_count = 0;
}

/* ---- helpers ------------------------------------------------------- */

static int g_checks, g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fails++;
        printf("  FAIL: %s\n", what);
    }
}

static const char *PSK =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void fresh(void)
{
    wifistore_clear();
}

/* ---- cases --------------------------------------------------------- */

static void t_validation(void)
{
    printf("what a record may contain\n");
    fresh();

    ck(wifistore_save("", PSK, true) == ESP_ERR_INVALID_ARG, "empty ssid");
    ck(wifistore_save(NULL, PSK, true) == ESP_ERR_INVALID_ARG, "null ssid");
    ck(wifistore_save("net", NULL, true) == ESP_ERR_INVALID_ARG, "null secret");

    /* 33 bytes: one past what an SSID may be. */
    char long_ssid[64];
    memset(long_ssid, 'a', 33);
    long_ssid[33] = '\0';
    ck(wifistore_save(long_ssid, PSK, true) == ESP_ERR_INVALID_ARG,
       "33-byte ssid refused");

    long_ssid[32] = '\0';
    ck(wifistore_save(long_ssid, PSK, true) == ESP_OK, "32-byte ssid accepted");

    /* A PSK is 64 hex characters, exactly, and hex. */
    ck(wifistore_save("n", "0123456789abcdef", true) == ESP_ERR_INVALID_ARG,
       "short psk");
    char bad[66];
    memcpy(bad, PSK, 65);
    bad[10] = 'z';
    ck(wifistore_save("n", bad, true) == ESP_ERR_INVALID_ARG, "non-hex psk");
    memcpy(bad, PSK, 65);
    bad[64] = 'a';
    bad[65] = '\0';
    ck(wifistore_save("n", bad, true) == ESP_ERR_INVALID_ARG, "65-char psk");

    /* A passphrase is 8..63 of anything, including the punctuation a
     * router's sticker is full of. */
    ck(wifistore_save("n", "1234567", false) == ESP_ERR_INVALID_ARG, "7-char pass");
    ck(wifistore_save("n", "12345678", false) == ESP_OK, "8-char pass");
    char p63[80];
    memset(p63, '!', 63);
    p63[63] = '\0';
    ck(wifistore_save("n2", p63, false) == ESP_OK, "63-char pass");
    memset(p63, '!', 64);
    p63[64] = '\0';
    ck(wifistore_save("n3", p63, false) == ESP_ERR_INVALID_ARG, "64-char pass");
}

static void t_replace(void)
{
    printf("one record per ssid\n");
    fresh();

    ck(wifistore_save("home", "oldpassword", false) == ESP_OK, "first save");
    ck(wifistore_count() == 1, "one record");

    ck(wifistore_save("home", "newpassword", false) == ESP_OK, "resave");
    ck(wifistore_count() == 1, "still one record");

    wifistore_cred_t c;
    ck(wifistore_get(0, &c), "get 0");
    ck(strcmp(c.secret, "newpassword") == 0, "newest password wins");

    /* The kind can change with it: a router upgraded to WPA3 stops
     * accepting the derived PSK and the portal stores a passphrase. */
    ck(wifistore_save("home", PSK, true) == ESP_OK, "resave as psk");
    ck(wifistore_get(0, &c), "get 0 again");
    ck(c.is_psk, "kind updated");
    ck(wifistore_count() == 1, "still one record");
}

static void t_eviction(void)
{
    printf("the oldest goes\n");
    fresh();

    char name[16];
    for (int i = 0; i < WIFISTORE_MAX; i++) {
        snprintf(name, sizeof(name), "net%d", i);
        ck(wifistore_save(name, PSK, true) == ESP_OK, "fill");
    }
    ck(wifistore_count() == WIFISTORE_MAX, "full");

    ck(wifistore_save("newcomer", PSK, true) == ESP_OK, "one too many");
    ck(wifistore_count() == WIFISTORE_MAX, "still full");

    wifistore_cred_t c;
    ck(wifistore_get(0, &c), "get 0");
    ck(strcmp(c.ssid, "net1") == 0, "net0 evicted, net1 is oldest");
    ck(wifistore_get(WIFISTORE_MAX - 1, &c), "get last");
    ck(strcmp(c.ssid, "newcomer") == 0, "newcomer is newest");

    /* Re-saving a held network must not evict: it is a replace, and a
     * full store that forgot a network every time a password was
     * corrected would be a bug nobody could describe. */
    ck(wifistore_save("net4", "adifferentone", false) == ESP_OK, "resave when full");
    ck(wifistore_count() == WIFISTORE_MAX, "no eviction on replace");
    ck(wifistore_get(0, &c) && strcmp(c.ssid, "net1") == 0, "oldest unchanged");
}

static void t_forget(void)
{
    printf("forgetting one, and all\n");
    fresh();

    wifistore_save("a", PSK, true);
    wifistore_save("b", PSK, true);
    wifistore_save("c", PSK, true);

    ck(wifistore_forget("nope") == ESP_ERR_NOT_FOUND, "forget absent");
    ck(wifistore_count() == 3, "unchanged");

    ck(wifistore_forget("b") == ESP_OK, "forget middle");
    ck(wifistore_count() == 2, "two left");

    wifistore_cred_t c;
    ck(wifistore_get(0, &c) && strcmp(c.ssid, "a") == 0, "a still first");
    ck(wifistore_get(1, &c) && strcmp(c.ssid, "c") == 0, "c shuffled down");
    ck(!wifistore_get(2, &c), "nothing at 2");
    ck(!wifistore_get(-1, &c), "nothing at -1");

    ck(wifistore_clear() == ESP_OK, "clear");
    ck(wifistore_count() == 0, "empty");
    ck(!wifistore_get(0, &c), "nothing at 0");
}

static void t_best(void)
{
    printf("which saved network a scan should join\n");
    fresh();

    wifistore_save("home", PSK, true);
    wifistore_save("cafe", PSK, true);

    /* Nothing saved is in the air. */
    {
        const char *seen[] = { "stranger", "neighbour" };
        const int8_t r[] = { -40, -50 };
        ck(wifistore_best(seen, r, 2) == -1, "none present");
    }

    /* The strong one wins, not the first saved. */
    {
        const char *seen[] = { "home", "cafe" };
        const int8_t r[] = { -80, -40 };
        wifistore_cred_t c;
        const int i = wifistore_best(seen, r, 2);
        ck(i >= 0 && wifistore_get(i, &c), "found one");
        ck(strcmp(c.ssid, "cafe") == 0, "strongest wins over store order");
    }

    /* Reversed strengths, reversed answer -- so the case above is not
     * passing because "cafe" happens to be second in the store. */
    {
        const char *seen[] = { "home", "cafe" };
        const int8_t r[] = { -40, -80 };
        wifistore_cred_t c;
        const int i = wifistore_best(seen, r, 2);
        ck(i >= 0 && wifistore_get(i, &c) && strcmp(c.ssid, "home") == 0,
           "strongest wins the other way");
    }

    /* One SSID on two APs, which is every office. The first entry wins
     * the tie, deterministically. */
    {
        const char *seen[] = { "home", "home" };
        const int8_t r[] = { -55, -55 };
        ck(wifistore_best(seen, r, 2) == wifistore_best(seen, r, 2),
           "tie is stable");
    }

    /* Degenerate arguments do not walk off anything. */
    {
        const char *seen[] = { NULL, "home" };
        const int8_t r[] = { -40, -60 };
        ck(wifistore_best(seen, r, 2) >= 0, "null entry skipped");
        ck(wifistore_best(NULL, r, 2) == -1, "null list");
        ck(wifistore_best(seen, NULL, 2) == -1, "null rssi");
        ck(wifistore_best(seen, r, 0) == -1, "empty scan");
        ck(wifistore_best(seen, r, -1) == -1, "negative count");
    }
}

static void t_rank(void)
{
    printf("every saved network in range, in the order to try them\n");
    fresh();

    wifistore_save("home", PSK, true);
    wifistore_save("cafe", "correcthorse", false);
    wifistore_save("work", PSK, true);

    wifistore_cred_t out[WIFISTORE_MAX];

    {
        const char *seen[] = { "stranger", "work", "neighbour", "home" };
        const int8_t r[] = { -30, -70, -40, -50 };
        const int n = wifistore_rank(seen, r, 4, out, WIFISTORE_MAX);
        ck(n == 2, "two of three saved are in range");
        ck(n == 2 && strcmp(out[0].ssid, "home") == 0 &&
           strcmp(out[1].ssid, "work") == 0, "strongest first");
    }

    /* One SSID on two APs counts once, at its best, and can outrank a
     * network it would lose to on its weaker AP. */
    {
        const char *seen[] = { "cafe", "home", "cafe" };
        const int8_t r[] = { -80, -60, -45 };
        const int n = wifistore_rank(seen, r, 3, out, WIFISTORE_MAX);
        ck(n == 2, "a repeated SSID is one entry");
        ck(n == 2 && strcmp(out[0].ssid, "cafe") == 0, "at its strongest AP");
        ck(n == 2 && !out[0].is_psk && strcmp(out[0].secret, "correcthorse") == 0,
           "the copy carries the record");
    }

    /* `max` is honoured, and the ones kept are the strongest. */
    {
        const char *seen[] = { "home", "cafe", "work" };
        const int8_t r[] = { -70, -40, -55 };
        const int n = wifistore_rank(seen, r, 3, out, 2);
        ck(n == 2 && strcmp(out[0].ssid, "cafe") == 0 &&
           strcmp(out[1].ssid, "work") == 0, "top two of three");
    }

    /* Ties go to the earlier record, every time. */
    {
        const char *seen[] = { "work", "home" };
        const int8_t r[] = { -50, -50 };
        const int n = wifistore_rank(seen, r, 2, out, WIFISTORE_MAX);
        ck(n == 2 && strcmp(out[0].ssid, "home") == 0, "tie to the earlier record");
    }

    /* The very weakest reading is still a reading. */
    {
        const char *seen[] = { "home" };
        const int8_t r[] = { INT8_MIN };
        ck(wifistore_rank(seen, r, 1, out, WIFISTORE_MAX) == 1, "-128 dBm counts");
    }

    /* A copy is a copy: forgetting after ranking does not change it. */
    {
        const char *seen[] = { "work" };
        const int8_t r[] = { -50 };
        const int n = wifistore_rank(seen, r, 1, out, WIFISTORE_MAX);
        wifistore_forget("work");
        ck(n == 1 && strcmp(out[0].ssid, "work") == 0, "copy outlives the record");
    }

    /* All eight, in range at once. */
    {
        fresh();
        char names[WIFISTORE_MAX][8];
        const char *seen[WIFISTORE_MAX];
        int8_t r[WIFISTORE_MAX];
        for (int i = 0; i < WIFISTORE_MAX; i++) {
            snprintf(names[i], sizeof(names[i]), "net%d", i);
            wifistore_save(names[i], PSK, true);
            seen[i] = names[i];
            r[i] = (int8_t)(-90 + i * 5);           /* last saved is strongest */
        }
        const int n = wifistore_rank(seen, r, WIFISTORE_MAX, out, WIFISTORE_MAX);
        int ordered = 1;
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].ssid, names[WIFISTORE_MAX - 1 - i]) != 0) ordered = 0;
        }
        ck(n == WIFISTORE_MAX && ordered, "eight saved, eight ranked, in order");
    }

    /* Degenerate arguments. */
    {
        const char *seen[] = { NULL, "net0" };
        const int8_t r[] = { -40, -60 };
        ck(wifistore_rank(seen, r, 2, out, WIFISTORE_MAX) == 1, "null entry skipped");
        ck(wifistore_rank(NULL, r, 2, out, WIFISTORE_MAX) == 0, "null list");
        ck(wifistore_rank(seen, NULL, 2, out, WIFISTORE_MAX) == 0, "null rssi");
        ck(wifistore_rank(seen, r, 2, NULL, WIFISTORE_MAX) == 0, "null out");
        ck(wifistore_rank(seen, r, 0, out, WIFISTORE_MAX) == 0, "empty scan");
        ck(wifistore_rank(seen, r, 2, out, 0) == 0, "no room");
    }
}

static void t_persistence(void)
{
    printf("what survives a reboot\n");
    fresh();

    wifistore_save("home", "correcthorse", false);
    wifistore_save("cafe", PSK, true);

    /* A reboot is the module forgetting everything and reading the blob
     * back. wifistore_init() is idempotent by design, so the state is
     * reset here the way a power cycle would. */
    const int writes_before = g_writes;
    wifistore_cred_t before[WIFISTORE_MAX];
    const int n = wifistore_count();
    for (int i = 0; i < n; i++) assert(wifistore_get(i, &before[i]));

    extern void wifistore_host_forget_ram(void);
    wifistore_host_forget_ram();

    ck(wifistore_count() == n, "count survives");
    for (int i = 0; i < n; i++) {
        wifistore_cred_t c;
        ck(wifistore_get(i, &c), "get after reload");
        ck(strcmp(c.ssid, before[i].ssid) == 0, "ssid survives");
        ck(strcmp(c.secret, before[i].secret) == 0, "secret survives");
        ck(c.is_psk == before[i].is_psk, "kind survives");
    }
    ck(g_writes == writes_before, "reading writes nothing");

    /* A blob of the wrong size is a build that changed the layout. It
     * must start empty rather than read a misaligned secret. */
    g_blob_len = 4;
    wifistore_host_forget_ram();
    ck(wifistore_count() == 0, "short blob starts empty");
    blob_reset();
}

static void t_churn(void)
{
    printf("ten thousand saves\n");
    fresh();

    /* The point is ASan, not the assertions: every one of these writes a
     * secret and an SSID into fixed buffers and shuffles the array, and
     * an off-by-one in the eviction memmove is invisible until it is a
     * heap error. */
    char ssid[40], secret[80];
    for (int i = 0; i < 10000; i++) {
        const int len = 8 + (i % 56);           /* 8..63 */
        memset(secret, 'a' + (i % 26), (size_t)len);
        secret[len] = '\0';

        const int nlen = 1 + (i % WIFISTORE_SSID_MAX);
        memset(ssid, 'A' + (i % 7), (size_t)nlen);
        ssid[nlen] = '\0';

        if (i % 17 == 0) wifistore_forget(ssid);
        else if (i % 97 == 0) wifistore_clear();
        else assert(wifistore_save(ssid, secret, false) == ESP_OK);

        ck(wifistore_count() >= 0 && wifistore_count() <= WIFISTORE_MAX,
           "count in range");

        wifistore_cred_t c;
        for (int k = 0; k < wifistore_count(); k++) {
            assert(wifistore_get(k, &c));
            assert(strlen(c.ssid) <= WIFISTORE_SSID_MAX);
            assert(strlen(c.secret) <= WIFISTORE_SECRET_MAX);
        }
    }
}

int main(void)
{
    wifistore_init();

    t_validation();
    t_replace();
    t_eviction();
    t_forget();
    t_best();
    t_rank();
    t_persistence();
    t_churn();

    blob_reset();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
