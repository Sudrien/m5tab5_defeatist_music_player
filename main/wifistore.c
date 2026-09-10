/*
 * wifistore.c -- the saved-network list, in NVS.
 *
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "wifistore.h"

#ifdef WIFISTORE_HOST
#include "wifistore_host.h"
#else
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#endif

#ifndef WIFISTORE_HOST
static const char *TAG = "tab5_wifi";
#endif

#define NVS_NAMESPACE   "wifistore"
#define NVS_BLOB_KEY    "nets"

/*
 * One blob, not one key per record.
 *
 * The whole list is under a kilobyte and every mutation rewrites it, so
 * a blob is one erase where per-record keys are up to eight, plus the
 * problem of what to do when a partial rewrite leaves record 3 from the
 * old list beside records 1, 2 and 4 from the new. NVS gives atomicity
 * per key; taking it for the list rather than for a record means the
 * list is never half-written.
 */
static wifistore_cred_t s_net[WIFISTORE_MAX];
static int              s_count;
static bool             s_ready;

#ifndef WIFISTORE_HOST
static SemaphoreHandle_t s_lock;
#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)
#else
#define LOCK()   do { } while (0)
#define UNLOCK() do { } while (0)
#endif

/* ---- validation ---------------------------------------------------- */

/*
 * A PSK is exactly 64 hex characters; a passphrase is 8 to 63 of
 * anything. Checked here rather than trusted from the portal, because
 * the store is the thing that has to be readable next boot and a record
 * that cannot be presented to the radio is worse than a refusal now.
 */
static bool hex64(const char *s)
{
    for (int i = 0; i < 64; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return s[64] == '\0';
}

static bool cred_ok(const char *ssid, const char *secret, bool is_psk)
{
    if (!ssid || !secret) return false;

    const size_t sl = strlen(ssid);
    if (sl == 0 || sl > WIFISTORE_SSID_MAX) return false;

    if (is_psk) return hex64(secret);

    const size_t pl = strlen(secret);
    return pl >= 8 && pl <= 63;
}

/* ---- persistence --------------------------------------------------- */

/*
 * The blob is the array and the count, written as they sit in memory.
 *
 * Not a text format, unlike .defeatist.dat, and for the same reason that
 * file is text: nobody is going to hand-edit NVS, so the argument for
 * something readable does not apply and the argument for something with
 * no parser does.
 *
 * A length mismatch on load is treated as "no networks" rather than
 * partially applied. The struct's layout is the format, so a build that
 * changes WIFISTORE_MAX or the field sizes reads a blob of the wrong
 * size and starts empty -- which loses the saved networks and is the
 * right failure, because the alternative is presenting a misaligned
 * secret to the radio.
 */
static esp_err_t store_write(void)
{
#ifdef WIFISTORE_HOST
    return host_blob_write(s_net, sizeof(s_net), s_count);
#else
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* Count first in the blob so a short read cannot be mistaken for a
     * full one. */
    uint8_t buf[sizeof(uint32_t) + sizeof(s_net)];
    const uint32_t n = (uint32_t)s_count;
    memcpy(buf, &n, sizeof(n));
    memcpy(buf + sizeof(n), s_net, sizeof(s_net));

    err = nvs_set_blob(h, NVS_BLOB_KEY, buf, sizeof(buf));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    /* The buffer held every secret in the store. */
    memset(buf, 0, sizeof(buf));
    return err;
#endif
}

static void store_read(void)
{
#ifdef WIFISTORE_HOST
    s_count = host_blob_read(s_net, sizeof(s_net));
#else
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved networks");
        return;
    }

    uint8_t buf[sizeof(uint32_t) + sizeof(s_net)];
    size_t len = sizeof(buf);
    const esp_err_t err = nvs_get_blob(h, NVS_BLOB_KEY, buf, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(buf)) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "network list is %u bytes, expected %u -- "
                          "starting empty", (unsigned)len, (unsigned)sizeof(buf));
        }
        memset(buf, 0, sizeof(buf));
        return;
    }

    uint32_t n;
    memcpy(&n, buf, sizeof(n));
    if (n > WIFISTORE_MAX) n = WIFISTORE_MAX;
    memcpy(s_net, buf + sizeof(n), sizeof(s_net));
    s_count = (int)n;
    memset(buf, 0, sizeof(buf));

    /* A record whose SSID lost its terminator would be read past by
     * every strcmp below. Cheaper to enforce it once here than to trust
     * eight records that came off flash. */
    for (int i = 0; i < s_count; i++) {
        s_net[i].ssid[WIFISTORE_SSID_MAX]     = '\0';
        s_net[i].secret[WIFISTORE_SECRET_MAX] = '\0';
    }

    ESP_LOGI(TAG, "%d saved network%s", s_count, s_count == 1 ? "" : "s");
#endif
}

#ifdef WIFISTORE_HOST
/*
 * A power cycle, for the host test: drop everything held in RAM and read
 * the blob back. Not compiled into the firmware -- on the board the only
 * way to reach this state is a reset.
 */
void wifistore_host_forget_ram(void)
{
    memset(s_net, 0, sizeof(s_net));
    s_count = 0;
    s_ready = false;
    wifistore_init();
}
#endif

/* ---- API ----------------------------------------------------------- */

void wifistore_init(void)
{
    if (s_ready) return;
#ifndef WIFISTORE_HOST
    s_lock = xSemaphoreCreateMutex();
#endif
    s_ready = true;
    LOCK();
    store_read();
    UNLOCK();
}

int wifistore_count(void)
{
    LOCK();
    const int n = s_count;
    UNLOCK();
    return n;
}

bool wifistore_get(int i, wifistore_cred_t *out)
{
    if (!out) return false;
    LOCK();
    const bool ok = (i >= 0 && i < s_count);
    if (ok) memcpy(out, &s_net[i], sizeof(*out));
    UNLOCK();
    return ok;
}

static int find_locked(const char *ssid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_net[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

esp_err_t wifistore_save(const char *ssid, const char *secret, bool is_psk)
{
    if (!cred_ok(ssid, secret, is_psk)) return ESP_ERR_INVALID_ARG;

    LOCK();

    int slot = find_locked(ssid);
    if (slot < 0) {
        if (s_count < WIFISTORE_MAX) {
            slot = s_count++;
        } else {
            /* Drop the oldest and shuffle down, so insertion order stays
             * the array order and there is no separate age field to keep
             * consistent with it. Eight records; the memmove is free. */
            memmove(&s_net[0], &s_net[1], (WIFISTORE_MAX - 1) * sizeof(s_net[0]));
            slot = WIFISTORE_MAX - 1;
        }
    }

    memset(&s_net[slot], 0, sizeof(s_net[slot]));
    snprintf(s_net[slot].ssid, sizeof(s_net[slot].ssid), "%s", ssid);
    snprintf(s_net[slot].secret, sizeof(s_net[slot].secret), "%s", secret);
    s_net[slot].is_psk = is_psk;

    const esp_err_t err = store_write();
    UNLOCK();
    return err;
}

esp_err_t wifistore_forget(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;

    LOCK();
    const int i = find_locked(ssid);
    if (i < 0) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    memmove(&s_net[i], &s_net[i + 1], (size_t)(s_count - i - 1) * sizeof(s_net[0]));
    s_count--;
    memset(&s_net[s_count], 0, sizeof(s_net[s_count]));

    const esp_err_t err = store_write();
    UNLOCK();
    return err;
}

esp_err_t wifistore_clear(void)
{
    LOCK();
    memset(s_net, 0, sizeof(s_net));
    s_count = 0;
    const esp_err_t err = store_write();
    UNLOCK();
    return err;
}

int wifistore_best(const char *const *seen, const int8_t *rssi, int n)
{
    if (!seen || !rssi || n <= 0) return -1;

    int best = -1;
    int8_t best_rssi = 0;

    LOCK();
    for (int i = 0; i < n; i++) {
        if (!seen[i]) continue;
        const int slot = find_locked(seen[i]);
        if (slot < 0) continue;

        /* Strictly greater, so the first scan entry wins a tie. A scan
         * can list one SSID twice -- two APs on one network is the
         * ordinary case in any building -- and picking either is
         * correct, but picking deterministically means a join that
         * fails once fails the same way twice and can be diagnosed. */
        if (best < 0 || rssi[i] > best_rssi) {
            best = slot;
            best_rssi = rssi[i];
        }
    }
    UNLOCK();

    return best;
}

int wifistore_rank(const char *const *seen, const int8_t *rssi, int n,
                   wifistore_cred_t *out, int max)
{
    if (!seen || !rssi || !out || n <= 0 || max <= 0) return 0;

    /* The strongest reading per saved slot; one SSID on several APs
     * counts once, at its best. INT8_MIN marks "not seen". */
    int8_t strongest[WIFISTORE_MAX];
    for (int k = 0; k < WIFISTORE_MAX; k++) strongest[k] = INT8_MIN;

    LOCK();
    int present = 0;
    for (int i = 0; i < n; i++) {
        if (!seen[i]) continue;
        const int slot = find_locked(seen[i]);
        if (slot < 0) continue;
        if (strongest[slot] == INT8_MIN) present++;
        /* Strictly greater, for the same deterministic-tie reason
         * wifistore_best() gives. */
        if (strongest[slot] == INT8_MIN || rssi[i] > strongest[slot]) {
            strongest[slot] = rssi[i] == INT8_MIN ? (int8_t)(INT8_MIN + 1) : rssi[i];
        }
    }

    /* Selection by strength, ties to the earlier slot. Eight entries;
     * nothing cleverer is worth its lines. */
    int got = 0;
    while (got < max && got < present) {
        int pick = -1;
        for (int k = 0; k < s_count; k++) {
            if (strongest[k] == INT8_MIN) continue;
            if (pick < 0 || strongest[k] > strongest[pick]) pick = k;
        }
        if (pick < 0) break;
        out[got++] = s_net[pick];
        strongest[pick] = INT8_MIN;
    }
    UNLOCK();

    return got;
}
