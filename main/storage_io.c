/*
 * storage_io.c -- see storage_io.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "storage_io.h"

static const char *TAG = "tab5_io";

/*
 * How long a waiter sleeps before re-testing whether it is the one who
 * should go next.
 *
 * The semaphore below wakes exactly one waiter on release, and FreeRTOS
 * picks that one by task priority -- which is the thing this file exists
 * to not decide by. So the wake is a fast path, not the mechanism: every
 * waiter also re-tests on a timer, and correctness comes from the re-test
 * rather than from who woke.
 *
 * 4 ms is the resulting worst-case grant latency when the wrong task is
 * woken. Against the PCM ring's tens of seconds of slack that is not a
 * number anybody can hear, and it buys an arbiter whose correctness does
 * not depend on FreeRTOS's choice of waiter.
 */
#define ARB_POLL_MS         (4)

/* Complain once per boot per class rather than per read: a nested
 * acquire in a hot path would otherwise be the only thing in the log. */
#define NEST_WARN_LIMIT     (1)

static SemaphoreHandle_t s_lock;    /* guards everything below; short holds */
static SemaphoreHandle_t s_free;    /* given on release; a wake, not a grant */

static bool                s_busy;
static storage_io_class_t  s_owner;
static TaskHandle_t        s_owner_task;
static int                 s_depth;
static int                 s_waiting[STORAGE_IO_CLASSES];

static uint32_t s_acquires[STORAGE_IO_CLASSES];
static uint64_t s_bytes[STORAGE_IO_CLASSES];
static uint32_t s_worst_ms[STORAGE_IO_CLASSES];
static int      s_nest_warned;

static const char *k_class_name[STORAGE_IO_CLASSES] = {
    "playback", "prefetch", "background",
};

void storage_io_init(void)
{
    if (s_lock) return;             /* idempotent; app_main is not the only caller */

    s_lock = xSemaphoreCreateMutex();
    s_free = xSemaphoreCreateBinary();

    if (!s_lock || !s_free) {
        /* Degrade to the old behaviour rather than refusing to boot. An
         * unarbitrated player is what this replaced and it did play. */
        if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
        if (s_free) { vSemaphoreDelete(s_free); s_free = NULL; }
        ESP_LOGE(TAG, "no memory for the arbiter; card access is unserialised");
        return;
    }

    s_busy = false;
    s_depth = 0;
    s_owner_task = NULL;
    memset(s_waiting, 0, sizeof(s_waiting));
    memset(s_acquires, 0, sizeof(s_acquires));
    memset(s_bytes, 0, sizeof(s_bytes));
    memset(s_worst_ms, 0, sizeof(s_worst_ms));

    ESP_LOGI(TAG, "storage arbiter up, %d KB chunks", STORAGE_IO_CHUNK / 1024);
}

/* Most urgent class with somebody waiting, or STORAGE_IO_CLASSES for
 * none. Called with s_lock held. */
static storage_io_class_t top_waiter(void)
{
    for (int i = 0; i < STORAGE_IO_CLASSES; i++) {
        if (s_waiting[i] > 0) return (storage_io_class_t)i;
    }
    return STORAGE_IO_CLASSES;
}

void storage_io_acquire(storage_io_class_t cls)
{
    if (!s_lock) return;            /* uninitialised: straight through */
    if (cls >= STORAGE_IO_CLASSES) cls = STORAGE_IO_BACKGROUND;

    TaskHandle_t me = xTaskGetCurrentTaskHandle();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /*
     * The mistake this catches is holding a lease around a parser rather
     * than around a read -- see the header. It is honoured rather than
     * refused because the alternative is a self-deadlock, and a hang on
     * the decode loop is worse than a wrong-but-working read. The log
     * line is the actual product of this branch.
     */
    if (s_busy && s_owner_task == me) {
        if (s_nest_warned < NEST_WARN_LIMIT) {
            s_nest_warned++;
            ESP_LOGE(TAG,
                     "nested acquire (%s inside %s) -- a lease is being held "
                     "across more than one read, which defeats the arbiter",
                     k_class_name[cls], k_class_name[s_owner]);
        }
        s_depth++;
        xSemaphoreGive(s_lock);
        return;
    }

    s_waiting[cls]++;
    xSemaphoreGive(s_lock);

    const int64_t t0 = esp_timer_get_time();

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_busy && top_waiter() == cls) {
            s_waiting[cls]--;
            s_busy = true;
            s_owner = cls;
            s_owner_task = me;
            s_depth = 1;

            const uint32_t waited =
                (uint32_t)((esp_timer_get_time() - t0) / 1000);
            s_acquires[cls]++;
            if (waited > s_worst_ms[cls]) s_worst_ms[cls] = waited;

            xSemaphoreGive(s_lock);
            return;
        }
        xSemaphoreGive(s_lock);

        /* Woken by a release, or by the timer. Either way the loop
         * re-tests; neither is a grant. */
        xSemaphoreTake(s_free, pdMS_TO_TICKS(ARB_POLL_MS));
    }
}

void storage_io_release(void)
{
    if (!s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_busy) {                  /* release without acquire */
        xSemaphoreGive(s_lock);
        return;
    }
    if (--s_depth > 0) {            /* nested; the outer one still holds */
        xSemaphoreGive(s_lock);
        return;
    }

    s_busy = false;
    s_owner_task = NULL;
    const bool anyone = top_waiter() != STORAGE_IO_CLASSES;
    xSemaphoreGive(s_lock);

    if (anyone) xSemaphoreGive(s_free);
}

bool storage_io_should_yield(storage_io_class_t cls)
{
    if (!s_lock) return false;
    if (cls == STORAGE_IO_PLAYBACK) return false;

    bool yield = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < (int)cls; i++) {
        if (s_waiting[i] > 0) { yield = true; break; }
    }
    xSemaphoreGive(s_lock);
    return yield;
}

/* Counted here rather than in acquire(), because a lease is not a
 * quantity: the playback path takes one lease for a whole read and the
 * background path takes one per chunk, so acquires alone would make the
 * two incomparable. */
static void count_bytes(storage_io_class_t cls, size_t n)
{
    if (!s_lock || !n) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bytes[cls] += n;
    xSemaphoreGive(s_lock);
}

size_t storage_io_fread(void *dst, size_t len, FILE *f,
                        storage_io_class_t cls)
{
    if (!f || !dst) return 0;

    uint8_t *p = (uint8_t *)dst;
    size_t done = 0;

    /*
     * Playback takes one lease for the whole read. It is the class
     * nothing preempts, so chunking it would buy nothing and cost a pair
     * of semaphore operations per 16 KB.
     */
    if (cls == STORAGE_IO_PLAYBACK) {
        storage_io_acquire(cls);
        done = fread(p, 1, len, f);
        storage_io_release();
        count_bytes(cls, done);
        return done;
    }

    while (done < len) {
        size_t want = len - done;
        if (want > STORAGE_IO_CHUNK) want = STORAGE_IO_CHUNK;

        storage_io_acquire(cls);
        const size_t got = fread(p + done, 1, want, f);
        storage_io_release();

        count_bytes(cls, got);
        done += got;
        if (got < want) break;      /* EOF or error; same as fread */
    }
    return done;
}

bool storage_io_read_at(FILE *f, long off, void *dst, size_t len,
                        storage_io_class_t cls)
{
    if (!f || !dst) return false;

    /*
     * The seek goes inside a lease of its own rather than outside the
     * read, because the file position is per-FILE* and every caller here
     * owns its own handle -- so nothing else can move it, and holding a
     * lease across both would only widen the window for no reason.
     */
    storage_io_acquire(cls);
    const bool sought = (fseek(f, off, SEEK_SET) == 0);
    storage_io_release();

    if (!sought) return false;
    return storage_io_fread(dst, len, f, cls) == len;
}

void storage_io_stats(storage_io_class_t cls, uint32_t *acquires,
                      uint64_t *bytes, uint32_t *worst_wait_ms)
{
    if (cls >= STORAGE_IO_CLASSES) return;
    if (!s_lock) {
        if (acquires) *acquires = 0;
        if (bytes) *bytes = 0;
        if (worst_wait_ms) *worst_wait_ms = 0;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (acquires) *acquires = s_acquires[cls];
    if (bytes) *bytes = s_bytes[cls];
    if (worst_wait_ms) *worst_wait_ms = s_worst_ms[cls];
    s_acquires[cls] = 0;
    s_bytes[cls] = 0;
    s_worst_ms[cls] = 0;
    xSemaphoreGive(s_lock);
}

void storage_io_report(const char *phase)
{
    if (!s_lock) return;

    for (int i = 0; i < STORAGE_IO_CLASSES; i++) {
        uint32_t n = 0, worst = 0;
        uint64_t bytes = 0;
        storage_io_stats((storage_io_class_t)i, &n, &bytes, &worst);
        if (!n) continue;
        ESP_LOGI(TAG, "%s %s: %" PRIu32 " reads, %" PRIu32 " KB, "
                 "worst wait %" PRIu32 " ms",
                 phase ? phase : "?", k_class_name[i], n,
                 (uint32_t)(bytes / 1024), worst);
    }
}
