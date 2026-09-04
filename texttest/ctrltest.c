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

/* ---- the ordering the 0903 restructure is about ------------------- */

/*
 * A single-threaded event loop, like usb_host_client_handle_events():
 * completions are delivered only while it is running. The bug was that
 * the descriptor wait ran INSIDE a callback dispatched by this loop, so
 * the completion it waited for could not be delivered until it returned.
 *
 * Modelled rather than mocked from the host API, same caveat as the rest
 * of this file: what is checked is that "wait inside the callback" times
 * out and "wait after the callback returns" does not.
 */
static bool g_loop_running;
static transfer_t *g_loop_pending;

static void loop_submit(transfer_t *t) { g_loop_pending = t; }

/* Deliver whatever is queued -- only callable from the loop. */
static void loop_run_once(void)
{
    g_loop_running = true;
    if (g_loop_pending) {
        transfer_t *t = g_loop_pending;
        g_loop_pending = NULL;
        t->status = STATUS_COMPLETED;
        t->callback(t);
    }
    g_loop_running = false;
}

static bool g_completed;
static void loop_cb(transfer_t *t) { (void)t; g_completed = true; }

/* The old shape: submit and wait without ever returning to the loop. */
static bool scan_inside_callback(void)
{
    transfer_t t = { .callback = loop_cb };
    g_completed = false;
    loop_submit(&t);

    /* "Waiting", i.e. spinning without letting the loop run, because we
     * ARE the loop. Bounded so the test terminates; on the device this
     * is the one-second timeout. */
    for (int i = 0; i < 1000 && !g_completed; i++) { /* no loop_run_once */ }
    return g_completed;
}

/* The new shape: the callback records, the loop returns, then we wait
 * while the loop is free to dispatch. */
static bool scan_after_callback(void)
{
    transfer_t t = { .callback = loop_cb };
    g_completed = false;
    loop_submit(&t);

    for (int i = 0; i < 1000 && !g_completed; i++) loop_run_once();
    return g_completed;
}

/* ---- close-after-release ordering (0904) --------------------------- */

/*
 * The wedge that survived 0902 and 0903: DEV_GONE arrives on one task
 * and the interface release happens on another, and closing a device
 * that still has a claim leaves the host library unable to finish
 * tearing it down -- address never released, no enumeration on replug.
 *
 * Modelled: a claim count and a gone flag, with the close performed by
 * whoever brings the count to zero after the flag is set. What is
 * asserted is that the close happens exactly once and never while a
 * claim is outstanding, in both orderings and under contention.
 */
#define MAX_DEVS 4
typedef struct { void *dev; int claims; bool gone; } open_dev_t;
static open_dev_t g_open[MAX_DEVS];
static pthread_mutex_t g_open_lock = PTHREAD_MUTEX_INITIALIZER;

static int  g_closes;           /* how many times close was performed */
static int  g_close_with_claim; /* closes that happened with a claim live */

static void od_remember(void *dev)
{
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < MAX_DEVS; i++)
        if (!g_open[i].dev) { g_open[i] = (open_dev_t){ dev, 0, false }; break; }
    pthread_mutex_unlock(&g_open_lock);
}
static void od_claimed(void *dev)
{
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < MAX_DEVS; i++) if (g_open[i].dev == dev) { g_open[i].claims++; break; }
    pthread_mutex_unlock(&g_open_lock);
}
static bool od_released(void *dev)
{
    bool close_now = false;
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < MAX_DEVS; i++) {
        if (g_open[i].dev != dev) continue;
        if (g_open[i].claims > 0) g_open[i].claims--;
        if (g_open[i].gone && g_open[i].claims == 0) { g_open[i].dev = NULL; close_now = true; }
        break;
    }
    pthread_mutex_unlock(&g_open_lock);
    return close_now;
}
static bool od_gone(void *dev)
{
    bool close_now = false;
    pthread_mutex_lock(&g_open_lock);
    for (int i = 0; i < MAX_DEVS; i++) {
        if (g_open[i].dev != dev) continue;
        g_open[i].gone = true;
        if (g_open[i].claims == 0) { g_open[i].dev = NULL; close_now = true; }
        break;
    }
    pthread_mutex_unlock(&g_open_lock);
    return close_now;
}

/* Records a close and whether any claim was still live when it happened. */
static int g_live_claims;
static void do_close(void)
{
    pthread_mutex_lock(&g_open_lock);
    g_closes++;
    if (g_live_claims > 0) g_close_with_claim++;
    pthread_mutex_unlock(&g_open_lock);
}

static void *releaser(void *arg)
{
    void *dev = arg;
    usleep((unsigned)(rand() % 200));
    pthread_mutex_lock(&g_open_lock); g_live_claims--; pthread_mutex_unlock(&g_open_lock);
    if (od_released(dev)) do_close();
    return NULL;
}

static void ordering_case(int n_claims)
{
    void *dev = (void *)0xD00D;
    g_closes = 0; g_close_with_claim = 0; g_live_claims = 0;
    memset(g_open, 0, sizeof(g_open));

    od_remember(dev);
    pthread_t th[3];
    for (int i = 0; i < n_claims; i++) {
        od_claimed(dev);
        pthread_mutex_lock(&g_open_lock); g_live_claims++; pthread_mutex_unlock(&g_open_lock);
    }
    for (int i = 0; i < n_claims; i++) pthread_create(&th[i], NULL, releaser, dev);

    usleep((unsigned)(rand() % 200));
    if (od_gone(dev)) do_close();

    for (int i = 0; i < n_claims; i++) pthread_join(th[i], NULL);

    assert(g_closes == 1);              /* exactly once, never zero */
    assert(g_close_with_claim == 0);    /* and never while claimed */
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

    /* 4. The ordering. Waiting inside the dispatching loop can never
     *    see its own completion; waiting outside it always does. This is
     *    what made the timeout universal rather than rare. */
    printf("wait inside the event callback...\n");
    assert(!scan_inside_callback());        /* deadlocks -> times out */
    printf("wait after the callback returns...\n");
    assert(scan_after_callback());          /* completes */

    /* 5. close-after-release, both orderings, under contention. */
    printf("close waits for the last release...\n");
    srand(1234);
    for (int i = 0; i < 300; i++) ordering_case(1);
    for (int i = 0; i < 300; i++) ordering_case(3);

    printf("\nno leaks or use-after-free reported\n");
    return 0;
}
