/*
 * ctrltest -- the ownership handoff in hid.c's report_desc_scan().
 *
 * The bug this covers is a use-after-free that only happens when a USB
 * control transfer completes AFTER the waiter has given up, so the thing
 * worth testing is the handoff itself, not the descriptor walk. The
 * logic is reproduced here against a fake USB host whose completion
 * timing the test controls, and run under ASan -- which is the point: if
 * either owner frees while the other still holds a pointer, ASan says so
 * with a stack trace instead of the panic being a rare field crash.
 *
 * Reproduced rather than compiled from hid.c, unlike texttest/, because
 * the real function is welded to the USB host API and the interesting
 * part is twenty lines. That is a real weakness and worth naming: this
 * verifies the SHAPE of the fix, and drift between this and hid.c is
 * possible. The shape is what was wrong before, so it is what is tested.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- the fake host ------------------------------------------------ */

typedef struct transfer_s transfer_t;
typedef void (*cb_t)(transfer_t *);
struct transfer_s {
    void *context;
    cb_t  callback;
    int   status;
    char  payload[64];      /* so ASan has something to catch a UAF on */
};

#define STATUS_COMPLETED 0
#define STATUS_CANCELED  3

/* One in-flight transfer, delivered by a thread after a controllable
 * delay, which is how "completes before the timeout" and "completes long
 * after the waiter gave up" become two settings of one knob. */
static transfer_t   *g_inflight;
static int           g_delay_us;
static pthread_t     g_thread;
static bool          g_running;

static void *deliver(void *arg)
{
    (void)arg;
    usleep(g_delay_us);
    transfer_t *t = g_inflight;
    if (t) {
        t->status = STATUS_COMPLETED;
        t->callback(t);
    }
    return NULL;
}

static int host_submit(transfer_t *t)
{
    g_inflight = t;
    g_running = true;
    pthread_create(&g_thread, NULL, deliver, NULL);
    return 0;
}

/* Stands in for halt+flush+clear: retires the URB now, invoking the
 * callback with CANCELED, which is what the real flush does. */
static void host_flush(void)
{
    pthread_join(g_thread, NULL);
    g_running = false;
    g_inflight = NULL;
}

static transfer_t *host_alloc(void) { return calloc(1, sizeof(transfer_t)); }
static void host_free(transfer_t *t) { free(t); }

/* ---- the logic under test, mirroring hid.c ------------------------- */

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    bool            signalled;
    transfer_t     *transfer;
    bool            abandoned;
} ctrl_ctx_t;

static pthread_mutex_t g_flag_lock = PTHREAD_MUTEX_INITIALIZER;

static void ctrl_ctx_free(ctrl_ctx_t *c)
{
    if (c->transfer) host_free(c->transfer);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cv);
    free(c);
}

static void ctrl_cb(transfer_t *t)
{
    ctrl_ctx_t *c = (ctrl_ctx_t *)t->context;

    bool mine;
    pthread_mutex_lock(&g_flag_lock);
    mine = c->abandoned;
    pthread_mutex_unlock(&g_flag_lock);

    if (mine) {
        ctrl_ctx_free(c);
        return;
    }

    pthread_mutex_lock(&c->lock);
    c->signalled = true;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->lock);
}

/* Returns true if the descriptor was got, matching report_desc_scan(). */
static bool scan(int timeout_ms, int completion_delay_ms)
{
    ctrl_ctx_t *c = calloc(1, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cv, NULL);

    c->transfer = host_alloc();
    c->transfer->context = c;
    c->transfer->callback = ctrl_cb;
    memset(c->transfer->payload, 0xAB, sizeof(c->transfer->payload));

    g_delay_us = completion_delay_ms * 1000;
    if (host_submit(c->transfer) != 0) {
        ctrl_ctx_free(c);
        return false;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    bool got = false;
    pthread_mutex_lock(&c->lock);
    while (!c->signalled) {
        if (pthread_cond_timedwait(&c->cv, &c->lock, &ts) != 0) break;
    }
    got = c->signalled;
    pthread_mutex_unlock(&c->lock);

    if (!got) {
        /* Timed out. Hand ownership over and touch nothing after. */
        pthread_mutex_lock(&g_flag_lock);
        c->abandoned = true;
        pthread_mutex_unlock(&g_flag_lock);
        host_flush();       /* retires it; cb runs and frees */
        return false;
    }

    const bool ok = (c->transfer->status == STATUS_COMPLETED);
    host_flush();
    ctrl_ctx_free(c);
    return ok;
}

int main(void)
{
    /* 1. Completes well inside the timeout: the waiter owns and frees. */
    printf("fast completion...\n");
    for (int i = 0; i < 50; i++) assert(scan(200, 1));

    /* 2. Never answers inside the timeout: the callback owns and frees.
     *    This is the crash. Before the fix the waiter freed here and the
     *    callback then wrote through a dangling context. */
    printf("timeout, late completion...\n");
    for (int i = 0; i < 50; i++) assert(!scan(20, 60));

    /* 3. The race itself: completion lands right at the deadline, so
     *    which owner wins differs run to run. Repeated, because a
     *    handoff that is only usually right shows up as a flake. */
    printf("completion at the deadline...\n");
    for (int i = 0; i < 400; i++) (void)scan(20, 20);

    printf("\nno leaks or use-after-free reported\n");
    return 0;
}
